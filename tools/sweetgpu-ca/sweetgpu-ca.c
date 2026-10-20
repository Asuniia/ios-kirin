/*
 * sweetgpu-ca.dylib, v0: loaded into backboardd (kernel 0139+,
 * sweetenv=backboardd:DYLD_INSERT_LIBRARIES=/usr/lib/sweetgpu-ca.dylib).
 *
 * 1. Runs tools/sweetgpu-test once (3 s after launch, in its own thread).
 * 2. Hooks CA::OGL::SW::draw_polygon (QuartzCore's software rasterizer,
 *    one call per clipped polygon) and every 2 s logs how many polygons
 *    were drawn, their area and CPU time, by texture-unit mask and blend
 *    mode, plus one dump of a large textured polygon's RenderState, to
 *    decide what the GPU should take over.
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
#include <stdatomic.h>

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

/* Stats by texture-unit mask (RenderState + 0x124, 3 bits) x blend mode byte. */
#define NBLEND 64
static _Atomic uint64_t st_n[8][NBLEND], st_ns[8][NBLEND], st_area[8][NBLEND];
static _Atomic int st_dumped;

static void
dump_words(const char *what, const void *p, size_t bytes)
{
	const uint32_t *w = p;
	for (size_t i = 0; i < bytes / 4; i += 8) {
		log_line("%s+%03zx: %08x %08x %08x %08x %08x %08x %08x %08x\n", what, i * 4,
		    w[i], w[i + 1], w[i + 2], w[i + 3], w[i + 4], w[i + 5], w[i + 6], w[i + 7]);
	}
}

static void
dp_hook(void *v, uint32_t n, const void *rs)
{
	uint64_t t0 = mach_absolute_time();
	sweetgpu_dp_tramp(v, n, rs);
	uint64_t dt = mach_absolute_time() - t0;

	const uint8_t *r = rs;
	const uint8_t *state = *(const uint8_t *const *)(*(const uint8_t *const *)r + 0x10);
	uint32_t mask = *(const uint32_t *)(r + 0x124) & 7, blend = state[0x10] % NBLEND;
	/* Vertex: 0x30 bytes, x, y floats first */
	const float *f = v;
	float a = 0;
	for (uint32_t i = 0; i < n; i++) {
		const float *p = f + i * 12, *q = f + ((i + 1) % n) * 12;
		a += p[0] * q[1] - q[0] * p[1];
	}
	uint64_t area = (uint64_t)(a < 0 ? -a / 2 : a / 2);
	atomic_fetch_add_explicit(&st_n[mask][blend], 1, memory_order_relaxed);
	atomic_fetch_add_explicit(&st_ns[mask][blend], dt, memory_order_relaxed);
	atomic_fetch_add_explicit(&st_area[mask][blend], area, memory_order_relaxed);

	if (mask == 1 && area > 200000 && atomic_exchange(&st_dumped, 1) == 0) {
		log_line("dump: n %u area %llu mask %u blend %u ms %.3f\n", n, area, mask, blend, ms(dt));
		dump_words("vtx", v, n * 0x30);
		dump_words("rs", rs, 0x130);
		dump_words("rs0", *(const void *const *)r, 0x40);
		dump_words("state", state, 0x1c0);
	}
}

static void *
reporter(void *arg)
{
	(void)arg;
	for (;;) {
		sleep(2);
		uint64_t n = 0, ns = 0, area = 0;
		struct { uint32_t m, b; uint64_t n, ns, area; } top[6] = {{0}};
		for (uint32_t m = 0; m < 8; m++) {
			for (uint32_t b = 0; b < NBLEND; b++) {
				uint64_t cn = atomic_exchange(&st_n[m][b], 0);
				uint64_t cns = atomic_exchange(&st_ns[m][b], 0);
				uint64_t ca = atomic_exchange(&st_area[m][b], 0);
				if (cn == 0) {
					continue;
				}
				n += cn, ns += cns, area += ca;
				for (int k = 0; k < 6; k++) {
					if (cns > top[k].ns) {
						memmove(&top[k + 1], &top[k], (5 - k) * sizeof(top[0]));
						top[k].m = m, top[k].b = b, top[k].n = cn, top[k].ns = cns, top[k].area = ca;
						break;
					}
				}
			}
		}
		if (n == 0) {
			continue;
		}
		log_line("2 s: %llu polys, %.1f Mpx, %.1f ms in draw_polygon\n", n, area / 1e6, ms(ns));
		for (int k = 0; k < 6 && top[k].n; k++) {
			log_line("  tex %u blend %2u: %6llu polys %7.2f Mpx %7.1f ms\n",
			    top[k].m, top[k].b, top[k].n, top[k].area / 1e6, ms(top[k].ns));
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
			return (uint8_t *)(unslid + _dyld_get_image_vmaddr_slide(i));
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
