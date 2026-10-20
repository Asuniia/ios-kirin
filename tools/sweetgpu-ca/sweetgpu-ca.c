/*
 * sweetgpu-ca.dylib, v2: loaded into backboardd (kernel 0139+,
 * sweetenv=backboardd:DYLD_INSERT_LIBRARIES=/usr/lib/sweetgpu-ca.dylib).
 *
 * 1. Runs tools/sweetgpu-test once (3 s after launch, in its own thread).
 * 2. Hooks CA::OGL::SW::draw_polygon (QuartzCore's software rasterizer,
 *    one call per clipped polygon) and every 2 s logs how many polygons
 *    were drawn, their area and CPU time, by kind (see the stats key), plus
 *    one dump per new kind of large polygon, to decide what the GPU should
 *    take over.
 * 3. Verify mode (see verify_prepare): GPU vs CPU on real draws, logged
 *    every 2 s; the screen is still drawn by the CPU.
 *
 * Log lines go to /dev/null, which the kernel's sweetnull tee prints as
 * |backboardd| sweetgpu: ... . Env SWEETGPU_CA=0 disables everything,
 * SWEETGPU_CA_TEST=0 the test (and so verify
 * mode), SWEETGPU_CA_VERIFY=0 verify mode.
 */
#define SWEETGPU_LIB
#define main sweetgpu_test_main
#include "../sweetgpu-test/sweetgpu-test.c"
#undef main

#include <libkern/OSCacheControl.h>
#include <mach-o/dyld.h>
#include <mach/mach.h>
#include <pthread.h>
#include <math.h>
#include <stdatomic.h>
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

/*
 * Verify mode (v2). For eligible polygons the screen still comes from the
 * CPU; the same draw also runs on the GPU into a copy of the destination
 * rect, and the two are compared. Eligible: unit 0 only, ARGB8_Host texture,
 * shading fn 1 or 3, blend Sover, an axis-aligned affine quad with one
 * gray (opacity) color, a 32-bit destination, >= 4096 pixels. At most one
 * every 100 ms.
 */
#define ARGB8_HOST_SAMPLER0     0x18844ba44ull  /* image_sampler<ARGB8_Host, u32, 0, 0, 0, 0> */

static io_connect_t g_conn;
static _Atomic bool g_gpu_ok;
static pthread_mutex_t g_gpu_lock = PTHREAD_MUTEX_INITIALIZER;
static uint64_t g_last_verify;

struct vjob {
	uint32_t *t;                    /* copy of the destination rect */
	size_t tlen;
	uint32_t tpitch, w, h;
	uint8_t *dst;                   /* the destination rect's first pixel */
	int64_t drb;
	const uint8_t *tex;
	uint64_t texrb;
	uint32_t texw, texh;
	struct sweet_gpu_quad q;
	uint32_t vkey;
};

#define NVKEY 32
static struct {
	uint32_t key;
	uint64_t n, exact, bad, inner_bad, px, cpu, gpu;
} vst[NVKEY];
static int v_logged;
static uint64_t v_skip[16];

static float
half(const void *p)
{
	__fp16 h;
	memcpy(&h, p, 2);
	return h;
}

/* s = a x + c y + e through three vertices (Vertex stride 12 floats); false if degenerate */
static bool
plane(const float *f, int k, float *a, float *c, float *e)
{
	const float *p0 = f, *p1 = f + 12, *p2 = f + 36;
	float det = (p1[0] - p0[0]) * (p2[1] - p0[1]) - (p2[0] - p0[0]) * (p1[1] - p0[1]);
	if (det == 0) {
		return false;
	}
	*a = ((p1[k] - p0[k]) * (p2[1] - p0[1]) - (p2[k] - p0[k]) * (p1[1] - p0[1])) / det;
	*c = ((p1[0] - p0[0]) * (p2[k] - p0[k]) - (p2[0] - p0[0]) * (p1[k] - p0[k])) / det;
	*e = p0[k] - *a * p0[0] - *c * p0[1];
	const float *p3 = f + 24;       /* the fourth vertex must agree */
	float d = *a * p3[0] + *c * p3[1] + *e - p3[k];
	return d > -1e-3f && d < 1e-3f;
}

static int
skip(int why)
{
	v_skip[why]++;
	return 0;
}

/* Before the CPU draw: decide, copy the destination rect. */
static int
verify_prepare(struct vjob *j, const float *f, uint32_t n, const uint8_t *r)
{
	const uint8_t *ctx = *(const uint8_t *const *)r;
	const uint8_t *state = *(const uint8_t *const *)(ctx + 0x10);
	const uint8_t *tex = r + 8;
	uint32_t fn = state[0x10];
	if ((*(const uint32_t *)(r + 0x124) & 7) != 1 || state[0x1b0] != 3 || (fn != 1 && fn != 3) || n != 4) {
		return 0;
	}
	const uint64_t *table = *(const uint64_t *const *)(tex + 0x30);
	if (table == NULL || table[0] - g_slide != ARGB8_HOST_SAMPLER0) {
		return skip(1);
	}
	for (int i = 0; i < 4; i++) {
		const float *p = f + i * 12, *q = f + ((i + 1) % 4) * 12;
		if (p[3] != 1.0f || !(p[0] == q[0] || p[1] == q[1]) || memcmp(p + 8, f + 8, 8) != 0) {
			return skip(2);
		}
	}
	float cr = half(f + 8), cg = half((const uint8_t *)(f + 8) + 2), cb = half(f + 9),
	    ca = half((const uint8_t *)(f + 9) + 2);
	if (cr != ca || cg != ca || cb != ca || ca <= 0 || ca > 1) {
		return skip(3);
	}
	const uint8_t *dst = *(uint8_t *const *)(ctx + 0x710), *dst2 = *(uint8_t *const *)(ctx + 0x718);
	int64_t drb = *(const int64_t *)(ctx + 0x720);
	int32_t dx0 = *(const int32_t *)(ctx + 0x738), dx1 = *(const int32_t *)(ctx + 0x73c);
	int32_t dy0 = *(const int32_t *)(ctx + 0x740), dy1 = *(const int32_t *)(ctx + 0x744);
	if (dst == NULL || (dst2 != NULL && dst2 != dst) || drb <= 0 ||
	    *(const uint32_t *)(ctx + 0x730) != 32 || *(const uint32_t *)(ctx + 0x734) != 4) {
		return skip(4);
	}
	const uint8_t *data = *(const uint8_t *const *)tex;
	uint64_t texrb = *(const uint64_t *)(tex + 0x10);
	uint32_t texw = *(const uint32_t *)(tex + 0x18), texh = *(const uint32_t *)(tex + 0x1c);
	if (data == NULL || ((uintptr_t)data & 63) || texrb % 4 || texrb < texw * 4ull || texw == 0 || texh == 0) {
		return skip(5);
	}
	/* pixels whose centers are inside, clipped (rs + 0x110) and inside the destination */
	float x0 = f[0], x1 = f[0], y0 = f[1], y1 = f[1];
	for (int i = 1; i < 4; i++) {
		x0 = fminf(x0, f[i * 12]), x1 = fmaxf(x1, f[i * 12]);
		y0 = fminf(y0, f[i * 12 + 1]), y1 = fmaxf(y1, f[i * 12 + 1]);
	}
	const int32_t *clip = (const int32_t *)(r + 0x110);
	int32_t X0 = (int32_t)ceilf(x0 - 0.5f), X1 = (int32_t)ceilf(x1 - 0.5f);
	int32_t Y0 = (int32_t)ceilf(y0 - 0.5f), Y1 = (int32_t)ceilf(y1 - 0.5f);
	X0 = X0 > clip[0] ? X0 : clip[0], Y0 = Y0 > clip[1] ? Y0 : clip[1];
	X1 = X1 < clip[2] ? X1 : clip[2], Y1 = Y1 < clip[3] ? Y1 : clip[3];
	X0 = X0 > dx0 ? X0 : dx0, Y0 = Y0 > dy0 ? Y0 : dy0;
	X1 = X1 < dx1 ? X1 : dx1, Y1 = Y1 < dy1 ? Y1 : dy1;
	if (X1 <= X0 || Y1 <= Y0 || (int64_t)(X1 - X0) * (Y1 - Y0) < 4096) {
		return 0;
	}
	/* unit 0 coordinates: s' = s * state[0x60] + state[0x68] (t: 0x64, 0x6c), texels = s' * w */
	const float *tm = (const float *)(state + 0x60);
	float as, cs, es, at, ct, et;
	if (!plane(f, 4, &as, &cs, &es) || !plane(f, 5, &at, &ct, &et)) {
		return skip(6);
	}
	float A = texw * tm[0] * as, C = texw * tm[0] * cs, E = texw * (tm[0] * es + tm[2]);
	float B = texh * tm[1] * at, D = texh * tm[1] * ct, F = texh * (tm[1] * et + tm[3]);
	uint64_t now = mach_absolute_time();
	if (ms(now - g_last_verify) < 100) {
		return 0;
	}
	g_last_verify = now;

	j->w = X1 - X0, j->h = Y1 - Y0;
	j->tpitch = (j->w * 4 + 63) & ~63u;
	j->tlen = ((size_t)j->tpitch * j->h + 16383) & ~(size_t)16383;
	j->t = pages(j->tlen);
	if (j->t == NULL) {
		return skip(7);
	}
	j->dst = (uint8_t *)dst + (int64_t)(Y0 - dy0) * drb + (int64_t)(X0 - dx0) * 4;
	j->drb = drb;
	for (uint32_t y = 0; y < j->h; y++) {
		memcpy((uint8_t *)j->t + (size_t)y * j->tpitch, j->dst + y * drb, j->w * 4);
	}
	j->tex = data, j->texrb = texrb, j->texw = texw, j->texh = texh;
	uint8_t f0 = tex[0x40], f1 = tex[0x41];
	struct sweet_gpu_quad q = {
		.src_w = texw, .src_h = texh, .src_pitch = (uint32_t)texrb,
		.dst_w = j->w, .dst_h = j->h, .dst_pitch = j->tpitch,
		.x0 = 0, .y0 = 0, .w = j->w, .h = j->h,
		/* T pixel (X, Y) is destination pixel (X + X0, Y + Y0) */
		.m = {fbits(A), fbits(B), fbits(C), fbits(D), fbits(A * X0 + C * Y0 + E), fbits(B * X0 + D * Y0 + F)},
		.opacity = fbits(ca),
		.flags = f0 == 0 && f1 == 0 ? SWEET_GPU_QUAD_NEAREST : 0,
	};
	j->q = q;
	j->vkey = fn | (uint32_t)f0 << 8 | (uint32_t)f1 << 12 | (texrb % 64 == 0) << 16 | (ca < 1) << 17 |
	    (A == 1 && D == 1 && B == 0 && C == 0) << 18;
	return 1;
}

/* After the CPU draw: the same on the GPU into the copy, then compare. */
static void
verify_run(struct vjob *j, uint64_t cpu)
{
	pthread_mutex_lock(&g_gpu_lock);
	uint64_t t0 = mach_absolute_time();
	uint64_t sg = map(g_conn, (void *)j->tex, j->texrb * j->texh), tg = map(g_conn, j->t, j->tlen);
	kern_return_t kr = KERN_FAILURE;
	if (sg && tg) {
		j->q.src_va = sg, j->q.dst_va = tg;
		kr = quad(g_conn, &j->q);
	}
	if (sg) {
		IOConnectCallScalarMethod(g_conn, 2, &sg, 1, NULL, NULL);
	}
	if (tg) {
		IOConnectCallScalarMethod(g_conn, 2, &tg, 1, NULL, NULL);
	}
	uint64_t gpu = mach_absolute_time() - t0;
	pthread_mutex_unlock(&g_gpu_lock);
	if (kr != KERN_SUCCESS) {
		v_skip[8]++;
		if (v_logged++ < 16) {
			log_line("verify: quad 0x%x (tex %ux%u rb %llu, dst %ux%u)\n", kr, j->texw, j->texh, j->texrb, j->w, j->h);
		}
		free(j->t);
		return;
	}
	uint64_t bad = 0, inner = 0;
	int first = -1;
	for (uint32_t y = 0; y < j->h; y++) {
		const uint32_t *c = (const uint32_t *)(j->dst + y * j->drb), *g = j->t + (size_t)y * j->tpitch / 4;
		for (uint32_t x = 0; x < j->w; x++) {
			uint32_t a = c[x], b = g[x];
			for (int sh = 0; sh < 32; sh += 8) {
				int d = (int)((a >> sh) & 0xff) - (int)((b >> sh) & 0xff);
				if (d > 2 || d < -2) {
					bad++;
					inner += x > 0 && y > 0 && x + 1 < j->w && y + 1 < j->h;
					if (first < 0) {
						first = (int)(y * j->w + x);
					}
					break;
				}
			}
		}
	}
	uint32_t k;
	for (k = 0; k < NVKEY && vst[k].n && vst[k].key != j->vkey; k++) {
	}
	if (k < NVKEY) {
		vst[k].key = j->vkey;
		vst[k].n++, vst[k].exact += bad == 0, vst[k].bad += bad, vst[k].inner_bad += inner;
		vst[k].px += (uint64_t)j->w * j->h, vst[k].cpu += cpu, vst[k].gpu += gpu;
	}
	if (bad && v_logged++ < 16) {
		uint32_t x = first % j->w, y = first / j->w;
		const struct sweet_gpu_quad *q = &j->q;
		float m[6];
		memcpy(m, q->m, sizeof(m));
		log_line("verify %05x: %ux%u %llu bad (%llu inner); first (%u,%u) cpu %08x gpu %08x\n", j->vkey,
		    j->w, j->h, bad, inner, x, y, ((const uint32_t *)(j->dst + y * j->drb))[x], j->t[y * j->tpitch / 4 + x]);
		log_line("  m %g %g %g %g %g %g op %g tex %ux%u rb %llu flags %u\n", m[0], m[1], m[2], m[3], m[4], m[5],
		    *(const float *)&q->opacity, j->texw, j->texh, j->texrb, q->flags);
	}
	free(j->t);
}

static void
verify_report(void)
{
	for (uint32_t k = 0; k < NVKEY && vst[k].n; k++) {
		log_line("verify fn %u filt %u/%u pitch64 %u op<1 %u ident %u: %llu draws, %llu exact, bad px %.2f%% (inner %.2f%%), "
		    "cpu %.2f gpu %.2f ms/draw, %.0f px/draw\n",
		    vst[k].key & 0xff, (vst[k].key >> 8) & 0xf, (vst[k].key >> 12) & 0xf, (vst[k].key >> 16) & 1,
		    (vst[k].key >> 17) & 1, (vst[k].key >> 18) & 1, vst[k].n, vst[k].exact,
		    100.0 * vst[k].bad / vst[k].px, 100.0 * vst[k].inner_bad / vst[k].px,
		    ms(vst[k].cpu) / vst[k].n, ms(vst[k].gpu) / vst[k].n, (double)vst[k].px / vst[k].n);
	}
	log_line("verify skips: fmt %llu shape %llu color %llu dst %llu tex %llu plane %llu alloc %llu quad %llu\n",
	    v_skip[1], v_skip[2], v_skip[3], v_skip[4], v_skip[5], v_skip[6], v_skip[7], v_skip[8]);
}

static void
dp_hook(void *v, uint32_t n, const void *rs)
{
	struct vjob j;
	bool vj = atomic_load_explicit(&g_gpu_ok, memory_order_relaxed) && verify_prepare(&j, v, n, rs);
	uint64_t t0 = mach_absolute_time();
	sweetgpu_dp_tramp(v, n, rs);
	uint64_t dt = mach_absolute_time() - t0;
	if (vj) {
		verify_run(&j, dt);
	}

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
	/* one dump per new kind of large polygon (the table is cleared every 2 s: limit 4) */
	bool dump = fresh && area > 50000 && st_dumps < 4;
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
		if (atomic_load(&g_gpu_ok)) {
			verify_report();
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
	if (sweetgpu_test_main() != 0) {
		return NULL;
	}
	io_service_t svc = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("SweetGPU"));
	if (svc && IOServiceOpen(svc, mach_task_self(), 0, &g_conn) == KERN_SUCCESS) {
		const char *e = getenv("SWEETGPU_CA_VERIFY");
		if (e == NULL || strcmp(e, "0") != 0) {
			atomic_store(&g_gpu_ok, true);
			log_line("verify mode on\n");
		}
	}
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
