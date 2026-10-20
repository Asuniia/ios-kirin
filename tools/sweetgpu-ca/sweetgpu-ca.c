/*
 * sweetgpu-ca.dylib, v1: loaded into backboardd (kernel 0139+,
 * sweetenv=backboardd:DYLD_INSERT_LIBRARIES=/usr/lib/sweetgpu-ca.dylib).
 *
 * 1. Runs tools/sweetgpu-test once (3 s after launch, in its own thread).
 * 2. Hooks CA::OGL::SW::draw_polygon (QuartzCore's software rasterizer,
 *    one call per clipped polygon) and every 2 s logs how many polygons
 *    were drawn, their area and CPU time, by kind (see the stats key), plus
 *    one dump per new kind of large polygon, to decide what the GPU should
 *    take over.
 *
 * Log lines go to /dev/null, which the kernel's sweetnull tee prints as
 * |backboardd| sweetgpu: ... . Env SWEETGPU_CA=0 disables everything,
 * SWEETGPU_CA_TEST=0 only the test.
 */
#define SWEETGPU_LIB
#define main sweetgpu_test_main
#include "../sweetgpu-test/sweetgpu-test.c"
#undef main

#include <libkern/OSCacheControl.h>
#include <mach-o/dyld.h>
#include <mach/mach.h>
#include <pthread.h>
#include <os/lock.h>

/* iOS 16.7.x (20H) dyld shared cache, unslid */
#define DRAW_POLYGON    0x188441a58ull

static const uint32_t dp_prologue[4] = {
	0xa9bb67fa,     /* stp x26, x25, [sp, #-0x50]! */
	0xa9015ff8,     /* stp x24, x23, [sp, #0x10] */
	0xa90257f6,     /* stp x22, x21, [sp, #0x20] */
	0xa9034ff4,     /* stp x20, x19, [sp, #0x30] */
};

/* The four replaced instructions, then on to draw_polygon + 16. */
uint64_t sweetgpu_dp_resume;
void sweetgpu_dp_tramp(void *v, uint32_t n, const void *rs);
__asm__(
	".text\n"
	".globl _sweetgpu_dp_tramp\n"
	".p2align 2\n"
	"_sweetgpu_dp_tramp:\n"
	"	stp x26, x25, [sp, #-0x50]!\n"
	"	stp x24, x23, [sp, #0x10]\n"
	"	stp x22, x21, [sp, #0x20]\n"
	"	stp x20, x19, [sp, #0x30]\n"
	"	adrp x16, _sweetgpu_dp_resume@PAGE\n"
	"	ldr x16, [x16, _sweetgpu_dp_resume@PAGEOFF]\n"
	"	br x16\n");

static void
log_line(const char *fmt, ...)
{
	char buf[256] = "sweetgpu: ";
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf + 10, sizeof(buf) - 10, fmt, ap);
	va_end(ap);
	if (g_null >= 0) {
		write(g_null, buf, strlen(buf));
	}
}

/*
 * Stats per kind of polygon. Key: texture-unit mask (rs + 0x124), shading
 * function (state + 0x10), blend op (state + 0x1b0: 0 Clear, 1 Copy,
 * 2 Dest, 3 Sover, 4 Sin, 5 Sout, ...), shape bits (1 quad, 2 axis-aligned,
 * 4 all w == 1, 8 one color) and unit 0's sampler table (Texture + 0x30 =
 * rs + 0x38, unslid, low 24 bits: the texture format).
 *
 * RenderState (built by SWContext::draw_elements): +0 SWContext*,
 * +8 unit 0 SW::Texture {+0 data, +8 ?, +0x10 rowbytes, +0x18 w, +0x1c h,
 * +0x20/+0x24 16.16 max, +0x28 1/w 1/h, +0x30 samplers, +0x40/+0x41
 * filters}, +0x110 clip x0 y0 x1 y1, +0x124 unit mask, +0x128 blend fn,
 * +0x130/+0x134 colors. SWContext + 0x710: destination (data, data2,
 * rowbytes, rowbytes2, bpp, Bpp, x, x1, y, y1).
 */
#define NKEY 256
static struct {
	uint64_t key, n, ns, area;
} st[NKEY];
static os_unfair_lock st_lock = OS_UNFAIR_LOCK_INIT;
static uint64_t g_slide;
static int st_dumps;

static void
dump_words(const char *what, const void *p, size_t off, size_t bytes)
{
	const uint32_t *w = (const uint32_t *)((const uint8_t *)p + off);
	for (size_t i = 0; i < bytes / 4; i += 8) {
		log_line("%s+%03zx: %08x %08x %08x %08x %08x %08x %08x %08x\n", what, off + i * 4,
		    w[i], w[i + 1], w[i + 2], w[i + 3], w[i + 4], w[i + 5], w[i + 6], w[i + 7]);
	}
}

static void
key_str(char *buf, size_t len, uint64_t k)
{
	snprintf(buf, len, "tex %llu fn %3llu blend %2llu shape %llx fmt %06llx",
	    k >> 56, (k >> 48) & 0xff, (k >> 40) & 0xff, (k >> 32) & 0xf, k & 0xffffff);
}

static void
dp_hook(void *v, uint32_t n, const void *rs)
{
	uint64_t t0 = mach_absolute_time();
	sweetgpu_dp_tramp(v, n, rs);
	uint64_t dt = mach_absolute_time() - t0;

	const uint8_t *r = rs, *ctx = *(const uint8_t *const *)r;
	const uint8_t *state = *(const uint8_t *const *)(ctx + 0x10);
	uint32_t mask = *(const uint32_t *)(r + 0x124) & 7;
	/* Vertex: 0x30 bytes: x y z w, unit 0/1 coords, color half4 at +0x20 */
	const float *f = v;
	float a = 0;
	bool axis = true, affine = true, color = true;
	for (uint32_t i = 0; i < n; i++) {
		const float *p = f + i * 12, *q = f + ((i + 1) % n) * 12;
		a += p[0] * q[1] - q[0] * p[1];
		axis &= p[0] == q[0] || p[1] == q[1];
		affine &= p[3] == 1.0f;
		color &= memcmp(p + 8, f + 8, 8) == 0;
	}
	uint64_t area = (uint64_t)(a < 0 ? -a / 2 : a / 2);
	uint64_t fmt = mask & 1 ? (*(const uint64_t *)(r + 0x38) - g_slide) & 0xffffff : 0;
	uint64_t shape = (n == 4) | (axis << 1) | (affine << 2) | (color << 3);
	uint64_t key = (uint64_t)mask << 56 | (uint64_t)state[0x10] << 48 | (uint64_t)state[0x1b0] << 40 |
	    shape << 32 | fmt;

	os_unfair_lock_lock(&st_lock);
	uint32_t h = (uint32_t)((key * 0x9e3779b97f4a7c15ull) >> 56);
	bool fresh = false;
	for (uint32_t i = 0; i < NKEY; i++, h = (h + 1) % NKEY) {
		if (st[h].key == key || st[h].n == 0) {
			fresh = st[h].key != key;
			st[h].key = key;
			st[h].n++, st[h].ns += dt, st[h].area += area;
			break;
		}
	}
	/* one dump per new kind of large polygon (the table is cleared every 2 s: limit 24) */
	bool dump = fresh && area > 50000 && st_dumps < 24;
	st_dumps += dump;
	if (dump) {
		char ks[96];
		key_str(ks, sizeof(ks), key);
		log_line("dump %d: %s n %u area %llu ms %.3f\n", st_dumps, ks, n, area, ms(dt));
		dump_words("vtx", v, 0, (n < 4 ? n : 4) * 0x30);
		dump_words("rs", rs, 0, 0x140);
		dump_words("state", state, 0, 0x20);
		dump_words("state", state, 0x60, 0x40);
		dump_words("state", state, 0x120, 0x20);
		dump_words("state", state, 0x1a0, 0x20);
		dump_words("ctx", ctx, 0x700, 0x80);
	}
	os_unfair_lock_unlock(&st_lock);
}

static void *
reporter(void *arg)
{
	(void)arg;
	for (;;) {
		sleep(2);
		os_unfair_lock_lock(&st_lock);
		uint64_t n = 0, ns = 0, area = 0;
		struct { uint64_t key, n, ns, area; } top[10] = {{0}};
		for (uint32_t i = 0; i < NKEY; i++) {
			if (st[i].n == 0) {
				continue;
			}
			n += st[i].n, ns += st[i].ns, area += st[i].area;
			for (int k = 0; k < 10; k++) {
				if (st[i].ns > top[k].ns) {
					memmove(&top[k + 1], &top[k], (9 - k) * sizeof(top[0]));
					top[k].key = st[i].key, top[k].n = st[i].n, top[k].ns = st[i].ns, top[k].area = st[i].area;
					break;
				}
			}
		}
		memset(st, 0, sizeof(st));
		os_unfair_lock_unlock(&st_lock);
		if (n == 0) {
			continue;
		}
		log_line("2 s: %llu polys, %.1f Mpx, %.1f ms in draw_polygon\n", n, area / 1e6, ms(ns));
		for (int k = 0; k < 10 && top[k].n; k++) {
			char ks[96];
			key_str(ks, sizeof(ks), top[k].key);
			log_line("  %s: %6llu polys %7.2f Mpx %7.1f ms\n", ks, top[k].n, top[k].area / 1e6, ms(top[k].ns));
		}
	}
	return NULL;
}

static void *
tester(void *arg)
{
	(void)arg;
	sleep(3);
	sweetgpu_test_main();
	return NULL;
}

static uint8_t *
quartzcore_addr(uint64_t unslid)
{
	for (uint32_t i = 0; i < _dyld_image_count(); i++) {
		const char *name = _dyld_get_image_name(i);
		size_t l = strlen(name);
		if (l >= 11 && strcmp(name + l - 11, "/QuartzCore") == 0) {
			g_slide = _dyld_get_image_vmaddr_slide(i);
			return (uint8_t *)(unslid + g_slide);
		}
	}
	return NULL;
}

/* Overwrite 16 bytes of code: ldr x16, #8; br x16; .quad target. */
static bool
patch_jump(uint8_t *at, void *target)
{
	uint32_t code[4] = {0x58000050, 0xd61f0200};
	uint64_t t = (uint64_t)target;
	memcpy(&code[2], &t, 8);
	vm_address_t page = (vm_address_t)at & ~(vm_address_t)PAGE_MASK;
	vm_size_t size = ((vm_address_t)at + 16 - page + PAGE_MASK) & ~(vm_size_t)PAGE_MASK;
	kern_return_t kr = vm_protect(mach_task_self(), page, size, FALSE,
	    VM_PROT_READ | VM_PROT_WRITE | VM_PROT_COPY);
	if (kr != KERN_SUCCESS) {
		log_line("protect rw: 0x%x\n", kr);
		return false;
	}
	memcpy(at, code, sizeof(code));
	kr = vm_protect(mach_task_self(), page, size, FALSE, VM_PROT_READ | VM_PROT_EXECUTE);
	sys_icache_invalidate(at, sizeof(code));
	if (kr != KERN_SUCCESS) {
		log_line("protect rx: 0x%x\n", kr);
		return false;
	}
	return true;
}

__attribute__((constructor))
static void
sweetgpu_ca_init(void)
{
	const char *e = getenv("SWEETGPU_CA");
	if (e != NULL && strcmp(e, "0") == 0) {
		return;
	}
	g_null = open("/dev/null", O_WRONLY);
	pthread_t th;
	uint8_t *dp = quartzcore_addr(DRAW_POLYGON);
	if (dp == NULL || memcmp(dp, dp_prologue, sizeof(dp_prologue)) != 0) {
		log_line("draw_polygon not found or unexpected (%p)\n", dp);
	} else {
		sweetgpu_dp_resume = (uint64_t)(dp + 16);
		if (patch_jump(dp, (void *)dp_hook)) {
			log_line("draw_polygon %p hooked\n", dp);
			pthread_create(&th, NULL, reporter, NULL);
		}
	}
	e = getenv("SWEETGPU_CA_TEST");
	if (e == NULL || strcmp(e, "0") != 0) {
		pthread_create(&th, NULL, tester, NULL);
	}
}
