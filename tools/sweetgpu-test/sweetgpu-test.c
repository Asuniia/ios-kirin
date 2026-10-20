/*
 * sweetgpu-test: exercise the SweetGPU user client (kernel 0138+, boot-arg
 * sweetgpu=8) from user space: map user memory, run quads, check them
 * against the CPU, time them. Output goes to stdout and /dev/null (the
 * sweetnull tee puts it in the kernel log as |sweetgpu-test| ...).
 *
 *   tools/sweetgpu-test/build.sh -> tools/sweetgpu-test/out/sweetgpu-test
 */
#include <IOKit/IOKitLib.h>
#include <fcntl.h>
#include <mach/mach_time.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* bsd/dev/sweet_gpu.h */
struct sweet_gpu_quad {
	uint64_t src_va, dst_va;
	uint32_t src_w, src_h, src_pitch;
	uint32_t dst_w, dst_h, dst_pitch;
	uint32_t x0, y0, w, h;
	uint32_t m[6];
	uint32_t reserved;
	uint32_t flags;
	uint32_t color[4];
	uint32_t src_fmt;
	uint32_t src_swiz;
	uint32_t nedges;
	uint32_t edge[4][3];
};

#define SWEET_GPU_FMT_RGBA8     0u
#define SWEET_GPU_FMT_R8        1u
#define SWEET_GPU_FMT_RG8       2u
#define SWIZ(x, y, z, w)        ((x) | (y) << 3 | (z) << 6 | (w) << 9)
#define SWIZ_XYZW               SWIZ(0, 1, 2, 3)
#define ONE4                    {0x3f800000u, 0x3f800000u, 0x3f800000u, 0x3f800000u}

#define SWEET_GPU_QUAD_NEAREST  1u
#define SWEET_GPU_QUAD_ADD      2u
#define SWEET_GPU_QUAD_COPY     4u

static int g_null = -1;

static void
say(const char *fmt, ...)
{
	char buf[256];
	va_list ap;
	va_start(ap, fmt);
	int n = vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
#ifndef SWEETGPU_LIB
	fputs(buf, stdout);
#endif
	if (g_null >= 0 && n > 0) {
		write(g_null, buf, (size_t)n < sizeof(buf) ? (size_t)n : sizeof(buf) - 1);
	}
}

static uint32_t
fbits(float f)
{
	uint32_t u;
	memcpy(&u, &f, 4);
	return u;
}

/* sweet_gpu.c src_px / blend_ref */
static uint32_t
src_px(uint32_t x, uint32_t y)
{
	uint32_t al = (x * 9 + y * 5 + 40) & 0xff, c = al ? (x * 7 + y * 3) % (al + 1) : 0;
	return (al << 24) | (c << 16) | ((c / 2) << 8) | (c / 3);
}

static uint32_t
blend_ref(uint32_t src, uint32_t dst)
{
	uint32_t ia = 255 - (src >> 24), out = 0;
	for (int sh = 0; sh < 32; sh += 8) {
		uint32_t t = ((dst >> sh) & 0xff) * ia;
		out |= (((t + 128 + (t >> 8)) >> 8) & 0xff) << sh;
	}
	return src + out;
}

static uint32_t
dst_px(uint32_t i)
{
	return 0xff000000u | ((i * 7) & 0xff) << 16 | ((i * 13) & 0xff) << 8 | (i & 0xff);
}

static double
ms(uint64_t abs)
{
	static mach_timebase_info_data_t tb;
	if (tb.denom == 0) {
		mach_timebase_info(&tb);
	}
	return (double)abs * tb.numer / tb.denom / 1e6;
}

static void *
pages(size_t len)
{
	void *p = NULL;
	return posix_memalign(&p, 16384, len) == 0 ? p : NULL;
}

static uint64_t
map(io_connect_t c, void *p, size_t len)
{
	uint64_t in[2] = {(uint64_t)p, len}, out[1] = {0};
	uint32_t n = 1;
	kern_return_t kr = IOConnectCallScalarMethod(c, 0, in, 2, out, &n);
	if (kr != KERN_SUCCESS) {
		say("map %p+%zu failed: 0x%x\n", p, len, kr);
		return 0;
	}
	return out[0];
}

static kern_return_t
quad(io_connect_t c, const struct sweet_gpu_quad *q)
{
	return IOConnectCallStructMethod(c, 3, q, sizeof(*q), NULL, NULL);
}

int
main(void)
{
	g_null = open("/dev/null", O_WRONLY);
	io_service_t svc = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("SweetGPU"));
	io_connect_t c = 0;
	kern_return_t kr = svc ? IOServiceOpen(svc, mach_task_self(), 0, &c) : KERN_FAILURE;
	if (kr != KERN_SUCCESS) {
		say("no SweetGPU (service %u, open 0x%x)\n", svc, kr);
		return 1;
	}

	/* the kernel brings the GPU up in its own thread: wait for it (up to 2 min) */
	uint64_t probe = 0;
	void *pp = pages(16384);
	for (int i = 0; i < 120; i++) {
		uint64_t in[2] = {(uint64_t)pp, 16384};
		uint32_t n1 = 1;
		kr = IOConnectCallScalarMethod(c, 0, in, 2, &probe, &n1);
		if (kr != kIOReturnNotReady) {
			break;
		}
		sleep(1);
	}
	if (kr != KERN_SUCCESS) {
		say("GPU not ready: 0x%x\n", kr);
		return 1;
	}
	IOConnectCallScalarMethod(c, 2, &probe, 1, NULL, NULL);

	/* 1. correctness: 64x64 image at (16, 16) of a 128x128 dst (kernel api_selftest) */
	uint32_t *s = pages(64 * 64 * 4), *d = pages(128 * 128 * 4);
	for (uint32_t i = 0; i < 64 * 64; i++) {
		s[i] = src_px(i % 64, i / 64);
	}
	for (uint32_t i = 0; i < 128 * 128; i++) {
		d[i] = dst_px(i);
	}
	uint64_t sg = map(c, s, 64 * 64 * 4), dg = map(c, d, 128 * 128 * 4);
	struct sweet_gpu_quad q = {
		.src_va = sg, .dst_va = dg,
		.src_w = 64, .src_h = 64, .src_pitch = 256,
		.dst_w = 128, .dst_h = 128, .dst_pitch = 512,
		.x0 = 16, .y0 = 16, .w = 64, .h = 64,
		.m = {fbits(1), 0, 0, fbits(1), fbits(-16), fbits(-16)},
		.color = ONE4, .src_swiz = SWIZ_XYZW,
	};
	kr = quad(c, &q);
	uint32_t bad = 0;
	for (uint32_t i = 0; i < 128 * 128; i++) {
		uint32_t px = i % 128, py = i / 128;
		bool in = px >= 16 && px < 80 && py >= 16 && py < 80;
		uint32_t want = in ? blend_ref(src_px(px - 16, py - 16), dst_px(i)) : dst_px(i);
		for (int sh = 0; sh < 32; sh += 8) {
			int df = (int)((d[i] >> sh) & 0xff) - (int)((want >> sh) & 0xff);
			if (df > 1 || df < -1) {
				bad++;
				break;
			}
		}
	}
	/* 2. rejections: an address outside this client's mappings, unmap twice */
	struct sweet_gpu_quad e = q;
	e.src_va = dg + 128 * 512;
	kern_return_t r1 = quad(c, &e);
	uint64_t um = dg;
	kern_return_t r2 = IOConnectCallScalarMethod(c, 2, &um, 1, NULL, NULL);
	kern_return_t r3 = IOConnectCallScalarMethod(c, 2, &um, 1, NULL, NULL);
	kern_return_t r4 = quad(c, &q);
	say("quad 0x%x, %u bad pixels; foreign src 0x%x, unmap 0x%x then 0x%x, quad after unmap 0x%x -> %s\n",
	    kr, bad, r1, r2, r3, r4,
	    kr == 0 && bad == 0 && r1 != 0 && r2 == 0 && r3 != 0 && r4 != 0 ? "USER CLIENT OK" : "USER CLIENT WRONG");

	/* 3. timing: small quads, then a full screen (750x1334) source-over at opacity 1 */
	dg = map(c, d, 128 * 128 * 4);
	q.dst_va = dg;
	uint64_t t0 = mach_absolute_time();
	for (int i = 0; i < 100; i++) {
		quad(c, &q);
	}
	uint64_t t1 = mach_absolute_time();
	say("64x64 quad: %.3f ms each (100 calls)\n", ms(t1 - t0) / 100);

	const uint32_t W = 750, H = 1334, P = 3008;     /* pitch: multiple of 64 bytes */
	size_t len = ((size_t)P * H + 16383) & ~(size_t)16383;
	uint32_t *fs = pages(len), *fd = pages(len), *fc = pages(len);
	for (uint32_t y = 0; y < H; y++) {
		for (uint32_t x = 0; x < W; x++) {
			fs[y * P / 4 + x] = src_px(x, y);
			fd[y * P / 4 + x] = fc[y * P / 4 + x] = dst_px(y * W + x);
		}
	}
	uint64_t fsg = map(c, fs, len), fdg = map(c, fd, len);
	struct sweet_gpu_quad f = {
		.src_va = fsg, .dst_va = fdg,
		.src_w = W, .src_h = H, .src_pitch = P,
		.dst_w = W, .dst_h = H, .dst_pitch = P,
		.x0 = 0, .y0 = 0, .w = W, .h = H,
		.m = {fbits(1), 0, 0, fbits(1), 0, 0},
		.color = ONE4, .src_swiz = SWIZ_XYZW,
	};
	t0 = mach_absolute_time();
	kr = quad(c, &f);
	t1 = mach_absolute_time();
	for (int i = 0; i < 9; i++) {
		quad(c, &f);
	}
	uint64_t t2 = mach_absolute_time();
	/* the same source-over on one CPU core (no sampling: a lower bound for CA's path) */
	for (uint32_t y = 0; y < H; y++) {
		for (uint32_t x = 0; x < W; x++) {
			fc[y * P / 4 + x] = blend_ref(fs[y * P / 4 + x], fc[y * P / 4 + x]);
		}
	}
	uint64_t t3 = mach_absolute_time();
	say("750x1334 quad 0x%x: first %.2f ms, then %.2f ms each; CPU source-over %.2f ms\n",
	    kr, ms(t1 - t0), ms(t2 - t1) / 9, ms(t3 - t2));
	/* the whole per-draw cost if nothing stays mapped: wire + map both, quad, unmap both */
	IOConnectCallScalarMethod(c, 2, &fsg, 1, NULL, NULL);
	IOConnectCallScalarMethod(c, 2, &fdg, 1, NULL, NULL);
	t0 = mach_absolute_time();
	for (int i = 0; i < 10; i++) {
		f.src_va = map(c, fs, len);
		f.dst_va = map(c, fd, len);
		kr = quad(c, &f);
		IOConnectCallScalarMethod(c, 2, &f.src_va, 1, NULL, NULL);
		IOConnectCallScalarMethod(c, 2, &f.dst_va, 1, NULL, NULL);
	}
	t1 = mach_absolute_time();
	say("750x1334 map + map + quad + unmap + unmap: 0x%x, %.2f ms each\n", kr, ms(t1 - t0) / 10);
	IOServiceClose(c);
	return 0;
}
