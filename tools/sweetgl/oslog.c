/*
 * sweetgl in the processes of boot-arg sweetoslog: their os_log lines in the log. logd
 * keeps them out of reach here (and OS_ACTIVITY_DT_MODE does nothing for system binaries),
 * so _os_log_impl and its error and fault forms are hooked: the buffer is decoded as
 * libtrace lays it out (a summary byte, the argument count, then per argument a
 * descriptor byte -- its type in the high nibble: 0 scalar, 1 count, 2 string, 3 pointer,
 * 4 object -- a size byte and the data) against the format string, write the line through
 * sweet_drm_log (the process's /dev/null, teed to the console), then the original runs.
 */
#include <CoreFoundation/CoreFoundation.h>
#include <os/log.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "sweet_drm.h"


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

/*
 * The calls from inside the shared cache are not interposed (the HAL and VirtualAudio log
 * from there), so the entry points themselves are patched. In iOS 16.7 each is a relay:
 * stp fp, lr / mov fp, sp / mov x6, lr / bl _os_log / ldp / ret. Its first four words
 * become a jump to oslog_hook, which decodes the line, then does what the relay did
 * (x6 = the caller) and branches to the internal _os_log found from that bl, with lr
 * left as the caller's: nothing of the original is replayed.
 */
#include <dlfcn.h>
#include <libkern/OSCacheControl.h>
#include <mach/mach.h>

uint64_t sweet_os_log_target;
void sweet_oslog_decode(uint64_t type, const char *fmt, const uint8_t *buf, uint64_t size);

void
sweet_oslog_decode(uint64_t type, const char *fmt, const uint8_t *buf, uint64_t size)
{
	if (!in_log && type != OS_LOG_TYPE_DEBUG) {
		in_log = 1;
		decode((os_log_type_t)type, fmt, buf, (uint32_t)size);
		in_log = 0;
	}
}

__attribute__((naked)) static void
oslog_hook(void)
{
	__asm__ volatile(
	    "stp x29, x30, [sp, #-96]!\n"
	    "mov x29, sp\n"
	    "stp x0, x1, [sp, #16]\n"
	    "stp x2, x3, [sp, #32]\n"
	    "stp x4, x5, [sp, #48]\n"
	    "stp x6, x7, [sp, #64]\n"
	    "str x8, [sp, #80]\n"
	    "mov x0, x2\n"
	    "mov x1, x3\n"
	    "mov x2, x4\n"
	    "mov x3, x5\n"
	    "bl _sweet_oslog_decode\n"
	    "ldp x0, x1, [sp, #16]\n"
	    "ldp x2, x3, [sp, #32]\n"
	    "ldp x4, x5, [sp, #48]\n"
	    "ldp x6, x7, [sp, #64]\n"
	    "ldr x8, [sp, #80]\n"
	    "ldp x29, x30, [sp], #96\n"
	    "mov x6, x30\n"
	    "adrp x16, _sweet_os_log_target@PAGE\n"
	    "ldr x16, [x16, _sweet_os_log_target@PAGEOFF]\n"
	    "br x16\n");
}

static void
patch_relay(const char *name)
{
	uint32_t *t = dlsym(RTLD_DEFAULT, name);
	if (t == NULL) {
		sweet_drm_log("sweetgl: os_log hook: no %s", name);
		return;
	}
	if (t[0] != 0xa9bf7bfd || t[1] != 0x910003fd || t[2] != 0xaa1e03e6 || (t[3] & 0xfc000000) != 0x94000000) {
		sweet_drm_log("sweetgl: os_log hook: %s is not the relay expected (%08x %08x %08x %08x)", name, t[0], t[1], t[2], t[3]);
		return;
	}
	int64_t off = (int64_t)((uint64_t)(t[3] & 0x03ffffff) << 38) >> 36;     /* imm26 * 4, sign-extended */
	uint64_t target = (uint64_t)(uintptr_t)&t[3] + (uint64_t)off;
	if (sweet_os_log_target == 0) {
		sweet_os_log_target = target;
	} else if (target != sweet_os_log_target) {
		sweet_drm_log("sweetgl: os_log hook: %s relays elsewhere, not hooked", name);
		return;
	}
	vm_address_t page = (vm_address_t)t & ~(vm_address_t)vm_page_mask;
	if (vm_protect(mach_task_self(), page, vm_page_size * 2, FALSE, VM_PROT_READ | VM_PROT_WRITE | VM_PROT_COPY) !=
	    KERN_SUCCESS) {
		sweet_drm_log("sweetgl: os_log hook: %s not writable", name);
		return;
	}
	uint32_t patch[4] = { 0x58000050, 0xd61f0200, 0, 0 };  /* ldr x16, #8; br x16; .quad oslog_hook */
	uint64_t hook = (uint64_t)(uintptr_t)oslog_hook;
	memcpy(&patch[2], &hook, 8);
	memcpy(t, patch, sizeof(patch));
	vm_protect(mach_task_self(), page, vm_page_size * 2, FALSE, VM_PROT_READ | VM_PROT_EXECUTE);
	sys_icache_invalidate(t, sizeof(patch));
}

/* sweetgl's constructor: only in the processes of sweetoslog */
void sweetgl_oslog_install(void);

void
sweetgl_oslog_install(void)
{
	if (!wanted()) {
		return;
	}
	patch_relay("_os_log_impl");
	patch_relay("_os_log_error_impl");
	patch_relay("_os_log_fault_impl");
	sweet_drm_log("sweetgl: os_log hooked (_os_log at %#llx)", (unsigned long long)sweet_os_log_target);
}
