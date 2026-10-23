/*
 * sweetgl in the processes of boot-arg sweetoslog: their os_log lines in the log. logd
 * keeps them out of reach here (and OS_ACTIVITY_DT_MODE does nothing for system binaries),
 * so the calls are interposed: _os_log_impl and its error/fault/debug forms decode the
 * buffer as libtrace lays it out (a summary byte, the argument count, then per argument a
 * descriptor byte -- its type in the high nibble: 0 scalar, 1 count, 2 string, 3 pointer,
 * 4 object -- a size byte and the data) against the format string, write the line through
 * sweet_drm_log (the process's /dev/null, teed to the console), then call the original.
 */
#include <CoreFoundation/CoreFoundation.h>
#include <os/log.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "sweet_drm.h"

void _os_log_impl(void *dso, os_log_t log, os_log_type_t type, const char *format, uint8_t *buf, uint32_t size);
void _os_log_error_impl(void *dso, os_log_t log, os_log_type_t type, const char *format, uint8_t *buf, uint32_t size);
void _os_log_fault_impl(void *dso, os_log_t log, os_log_type_t type, const char *format, uint8_t *buf, uint32_t size);
void _os_log_debug_impl(void *dso, os_log_t log, os_log_type_t type, const char *format, uint8_t *buf, uint32_t size);

static __thread int in_log;

/* only in the processes of sweetoslog: the kernel gives them CFLOG_FORCE_STDERR=YES */
static int
wanted(void)
{
	static int w = -1;
	if (w < 0) {
		const char *e = getenv("CFLOG_FORCE_STDERR");
		w = e && e[0] == 'Y';
	}
	return w;
}

struct item {
	uint8_t type, size;
	const uint8_t *data;
};

static void
put(char *out, size_t *n, size_t room, const char *s, size_t len)
{
	if (*n + len >= room) {
		len = room - 1 - *n;
	}
	memcpy(out + *n, s, len);
	*n += len;
}

static void
decode(os_log_type_t type, const char *fmt, const uint8_t *buf, uint32_t size)
{
	struct item it[48];
	unsigned count = 0;
	if (buf && size >= 2) {
		unsigned want = buf[1];
		uint32_t off = 2;
		for (unsigned i = 0; i < want && count < 48 && off + 2 <= size; i++) {
			uint8_t d = buf[off], s = buf[off + 1];
			if (off + 2 + s > size) {
				break;
			}
			it[count++] = (struct item){ (uint8_t)(d >> 4), s, buf + off + 2 };
			off += 2u + s;
		}
	}
	char out[512], tmp[256];
	size_t n = 0;
	const char *lvl = type == OS_LOG_TYPE_ERROR ? "E " : type == OS_LOG_TYPE_FAULT ? "F " : "";
	put(out, &n, sizeof(out), "oslog: ", 7);
	put(out, &n, sizeof(out), lvl, strlen(lvl));
	unsigned a = 0;
	for (const char *p = fmt; p && *p && n < sizeof(out) - 1; p++) {
		if (*p != '%') {
			put(out, &n, sizeof(out), p, 1);
			continue;
		}
		const char *start = p++;
		if (*p == '%') {
			put(out, &n, sizeof(out), "%", 1);
			continue;
		}
		if (*p == '{') {                        /* {public}, {private}, {bool}... */
			while (*p && *p != '}') {
				p++;
			}
			if (*p) {
				p++;
			}
		}
		int precision_arg = 0;
		while (*p && strchr("-+ #0123456789.*hlqjztL", *p)) {
			if (*p == '*') {
				precision_arg = 1;
			}
			p++;
		}
		char conv = *p;
		if (conv == 0) {
			break;
		}
		if (precision_arg && a < count && it[a].type == 1) {
			a++;                            /* the precision of a %.*s */
		}
		if (conv == 'm') {
			put(out, &n, sizeof(out), "<errno>", 7);
			continue;
		}
		if (a >= count) {
			put(out, &n, sizeof(out), start, (size_t)(p - start + 1));
			continue;
		}
		struct item x = it[a++];
		tmp[0] = 0;
		if (x.type == 2 && x.size == 8) {               /* C string */
			const char *str;
			memcpy(&str, x.data, 8);
			snprintf(tmp, sizeof(tmp), "%s", str ? str : "(null)");
		} else if (x.type == 4 && x.size == 8) {        /* object */
			CFTypeRef o;
			memcpy(&o, x.data, 8);
			CFStringRef d = o ? CFCopyDescription(o) : NULL;
			if (d == NULL || !CFStringGetCString(d, tmp, sizeof(tmp), kCFStringEncodingUTF8)) {
				snprintf(tmp, sizeof(tmp), o ? "<object>" : "(null)");
			}
			if (d) {
				CFRelease(d);
			}
		} else if (x.type == 0 || x.type == 3) {        /* scalar or pointer */
			uint64_t v = 0;
			memcpy(&v, x.data, x.size < 8 ? x.size : 8);
			if (strchr("fgeFGEaA", conv) && x.size == 8) {
				double dv;
				memcpy(&dv, &v, 8);
				snprintf(tmp, sizeof(tmp), "%g", dv);
			} else if (conv == 'd' || conv == 'i') {
				int64_t sv = x.size == 1 ? (int8_t)v : x.size == 2 ? (int16_t)v : x.size == 4 ? (int32_t)v : (int64_t)v;
				snprintf(tmp, sizeof(tmp), "%lld", (long long)sv);
			} else if (conv == 'x' || conv == 'X' || conv == 'p') {
				snprintf(tmp, sizeof(tmp), "0x%llx", (unsigned long long)v);
			} else if (conv == 'c') {
				snprintf(tmp, sizeof(tmp), "%c", (char)v);
			} else {
				snprintf(tmp, sizeof(tmp), "%llu", (unsigned long long)v);
			}
		} else {
			snprintf(tmp, sizeof(tmp), "<%u bytes>", x.size);
		}
		put(out, &n, sizeof(out), tmp, strlen(tmp));
	}
	out[n] = 0;
	sweet_drm_log("%s", out);
}

#define WRAP(name)                                                                              \
	static void                                                                             \
	sweet_##name(void *dso, os_log_t log, os_log_type_t type, const char *format, uint8_t *buf, \
	    uint32_t size)                                                                      \
	{                                                                                       \
		if (!in_log && type != OS_LOG_TYPE_DEBUG && wanted()) {                         \
			in_log = 1;                                                             \
			decode(type, format, buf, size);                                        \
			in_log = 0;                                                             \
		}                                                                               \
		name(dso, log, type, format, buf, size);                                        \
	}

WRAP(_os_log_impl)
WRAP(_os_log_error_impl)
WRAP(_os_log_fault_impl)
WRAP(_os_log_debug_impl)

/* dyld interposes these for every image, the shared cache's included, while sweetgl is loaded */
__attribute__((used, section("__DATA,__interpose"))) static const struct {
	const void *replacement, *replacee;
} interposers[] = {
	{ (const void *)sweet__os_log_impl, (const void *)_os_log_impl },
	{ (const void *)sweet__os_log_error_impl, (const void *)_os_log_error_impl },
	{ (const void *)sweet__os_log_fault_impl, (const void *)_os_log_fault_impl },
	{ (const void *)sweet__os_log_debug_impl, (const void *)_os_log_debug_impl },
};
