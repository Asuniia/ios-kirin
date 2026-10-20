/*
 * sweetgpu-ca.dylib, v8: loaded into backboardd (kernel 0139+,
 * sweetenv=backboardd:DYLD_INSERT_LIBRARIES=/usr/lib/sweetgpu-ca.dylib).
 *
 * 1. Runs tools/sweetgpu-test once (3 s after launch, in its own thread).
 * 2. Hooks CA::OGL::SW::draw_polygon (QuartzCore's software rasterizer,
 *    one call per clipped polygon) and every 2 s logs how many polygons
 *    were drawn, their area and CPU time, by kind (see the stats key), plus
 *    one dump per new kind of large polygon, to decide what the GPU should
 *    take over.
 * 3. GPU draws (see plan()): verify mode compares GPU and CPU on real
 *    draws (the screen is still drawn by the CPU); offload mode
 *    (SWEETGPU_CA_MODE=offload) lets the GPU draw instead.
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
 * GPU draws. plan() decides whether a polygon can go to the GPU: unit 0
 * only, ARGB8_Host texture with a pitch that is a multiple of 64 (the
 * texture unit rounds pitches up to 64), shading fn 1 (texture) or 3
 * (texture * vertex color, gray only), blend Copy, Sover or Add, an
 * axis-aligned affine quad that samples inside its texture, a 32-bit
 * destination, >= 4096 pixels.
 *
 * Verify mode (default): the screen still comes from the CPU; the same draw
 * also runs on the GPU into a copy of the destination rect, at most one
 * every 100 ms, and the two are compared.
 * Offload mode (SWEETGPU_CA_MODE=offload): those draws go to
 * the GPU instead of the CPU, straight into the destination; the CPU if the
 * GPU fails.
 */
#define ARGB8_HOST_SAMPLER0     0x18844ba44ull  /* image_sampler<ARGB8_Host, u32, 0, 0, 0, 0> */

static io_connect_t g_conn;
static _Atomic bool g_gpu_ok;
static bool g_offload;
static pthread_mutex_t g_gpu_lock = PTHREAD_MUTEX_INITIALIZER;
static uint64_t g_last_verify;

struct plan {
	int32_t X0, Y0, X1, Y1;         /* destination pixels */
	float A, B, C, D, E, F;         /* texel (u, v) = (A x + C y + E, B x + D y + F) at pixel center (x, y) */
	uint32_t color[4], flags, key, fmt, swiz, nedges, edge[4][3];
	const uint8_t *tex;
	uint64_t texrb;
	uint32_t texw, texh;
	uint8_t *dst;                   /* pixel (dx0, dy0) */
	int64_t drb;
	int32_t dx0, dy0, dx1, dy1;
};

#define NVKEY 32
static struct {
	uint32_t key;
	uint64_t n, exact, bad, inner_bad, px, cpu, gpu;
} vst[NVKEY];
static int v_logged;
static uint64_t v_skip[16];
static uint64_t off_n, off_px, off_ns, off_fail;
static uint64_t el_n, el_px, el_ns;     /* eligible draws the CPU did */

static float
half(const void *p)
{
	__fp16 h;
	memcpy(&h, p, 2);
	return h;
}

/* s = a x + c y + e through vertices 0, 1, n-1 (stride 12 floats), checked at the others */
static bool
plane(const float *f, uint32_t n, int k, float *a, float *c, float *e)
{
	const float *p0 = f, *p1 = f + 12, *p2 = f + 12 * (n - 1);
	float det = (p1[0] - p0[0]) * (p2[1] - p0[1]) - (p2[0] - p0[0]) * (p1[1] - p0[1]);
	if (det == 0) {
		return false;
	}
	*a = ((p1[k] - p0[k]) * (p2[1] - p0[1]) - (p2[k] - p0[k]) * (p1[1] - p0[1])) / det;
	*c = ((p1[0] - p0[0]) * (p2[k] - p0[k]) - (p2[0] - p0[0]) * (p1[k] - p0[k])) / det;
	*e = p0[k] - *a * p0[0] - *c * p0[1];
	for (uint32_t i = 2; i + 1 < n; i++) {
		const float *p3 = f + 12 * i;
		float d = *a * p3[0] + *c * p3[1] + *e - p3[k];
		if (!(d > -1e-3f && d < 1e-3f)) {
			return false;
		}
	}
	return true;
}

static int g_why;                       /* last skip reason, for skip_ns */
static uint64_t skip_ns[16], skip_px[16];      /* 13 kind, 14 small, 15 colorprog */

static bool
skip(int why)
{
	v_skip[why]++;
	g_why = why;
	return false;
}

/* Texture formats by their sampler table's first entry (unslid; 0143b log). */
static const struct {
	uint64_t sampler0;
	uint32_t fmt, swiz;
	bool tint_only;                 /* alpha masks: only with fn 3 (texture * color) */
	const char *name;
} formats[] = {
	{0x18844ba44ull, SWEET_GPU_FMT_RGBA8, SWIZ(0, 1, 2, 3), false, "ARGB8_Host"},  /* bytes B G R A */
	{0x18844cbc4ull, SWEET_GPU_FMT_RGBA8, SWIZ(0, 1, 2, 5), false, "XRGB8_Host"},  /* alpha 1 */
	{0x188451f74ull, SWEET_GPU_FMT_RGBA8, SWIZ(2, 1, 0, 3), false, "RGBA8"},       /* bytes R G B A */
	{0x1884547f4ull, SWEET_GPU_FMT_RGBA8, SWIZ(1, 2, 3, 0), false, "ABGR8"},       /* bytes A B G R */
	{0x188450950ull, SWEET_GPU_FMT_R8, SWIZ(0, 0, 0, 0), true, "A8"},
};

/*
 * The 3 or 4 vertices in convex order around their centroid (12-float
 * stride kept): some quads come in strip order (TL TR BL BR), which made
 * the edges a bow-tie that covered nothing (0149a).
 */
static void
hull_order(const float *f, uint32_t n, float *out)
{
	float cx = 0, cy = 0, ang[4];
	uint32_t idx[4];
	for (uint32_t i = 0; i < n; i++) {
		cx += f[i * 12] / n, cy += f[i * 12 + 1] / n;
	}
	for (uint32_t i = 0; i < n; i++) {
		ang[i] = atan2f(f[i * 12 + 1] - cy, f[i * 12] - cx);
		idx[i] = i;
	}
	for (uint32_t i = 1; i < n; i++) {
		for (uint32_t k = i; k > 0 && ang[idx[k]] < ang[idx[k - 1]]; k--) {
			uint32_t t = idx[k];
			idx[k] = idx[k - 1], idx[k - 1] = t;
		}
	}
	for (uint32_t i = 0; i < n; i++) {
		memcpy(out + i * 12, f + idx[i] * 12, 12 * sizeof(float));
	}
}

static bool
plan(struct plan *pl, const float *fin, uint32_t n, const uint8_t *r)
{
	float f[4 * 12];
	const uint8_t *ctx = *(const uint8_t *const *)r;
	const uint8_t *state = *(const uint8_t *const *)(ctx + 0x10);
	const uint8_t *tex = r + 8;
	uint32_t fn = state[0x10];
	uint32_t blend = state[0x1b0];
	g_why = 0;
	if ((*(const uint32_t *)(r + 0x124) & 7) != 1 || (blend != 1 && blend != 3 && blend != 12) ||
	    (fn != 1 && fn != 3) || n < 3 || n > 4) {
		return skip(13);
	}
	/*
	 * a color program (state + 0x168, e.g. the dock material's color matrix)
	 * or the special unit mode (state[0x11] == 0x11) changes the sampled
	 * colors (sample_texture): not modelled (0149b: dock gone, black strips)
	 */
	if (*(const uint64_t *)(state + 0x168) != 0 || state[0x11] == 0x11) {
		return skip(15);
	}
	hull_order(fin, n, f);
	const uint64_t *table = *(const uint64_t *const *)(tex + 0x30);
	int fi = -1;
	for (int i = 0; table != NULL && i < (int)(sizeof(formats) / sizeof(formats[0])); i++) {
		if (table[0] - g_slide == formats[i].sampler0) {
			fi = i;
		}
	}
	if (fi < 0 || (formats[fi].tint_only && fn != 3)) {
		/* name the other formats: the table's first sampler, once each */
		static const uint64_t *seen[8];
		static int nseen;
		int i;
		for (i = 0; i < nseen && seen[i] != table; i++) {
		}
		if (fi < 0 && i == nseen && nseen < 8 && table != NULL) {
			seen[nseen++] = table;
			log_line("format: sampler table %llx, first sampler %llx (unslid)\n",
			    (uint64_t)table - g_slide, table[0] - g_slide);
		}
		return skip(1);
	}
	bool axis = true;
	for (uint32_t i = 0; i < n; i++) {
		const float *p = f + i * 12, *q = f + ((i + 1) % n) * 12;
		if (p[3] != 1.0f || (fn == 3 && memcmp(p + 8, f + 8, 8) != 0)) {
			return skip(2);
		}
		axis &= p[0] == q[0] || p[1] == q[1];
	}
	axis &= n == 4;
	/* vertex color half4 (r g b a?) -> memory order of a BGRA destination */
	float cr = half(f + 8), cg = half((const uint8_t *)(f + 8) + 2), cb = half(f + 9),
	    ca = half((const uint8_t *)(f + 9) + 2);
	if (fn == 1) {
		cr = cg = cb = ca = 1;  /* fn 1 ignores the vertex color (0141a) */
	} else if (ca <= 0 || ca > 1 || cr > ca || cg > ca || cb > ca || cr < 0 || cg < 0 || cb < 0) {
		return skip(3);
	}
	uint8_t *dst = *(uint8_t *const *)(ctx + 0x710), *dst2 = *(uint8_t *const *)(ctx + 0x718);
	int64_t drb = *(const int64_t *)(ctx + 0x720);
	if (dst == NULL || (dst2 != NULL && dst2 != dst) || drb <= 0 ||
	    *(const uint32_t *)(ctx + 0x730) != 32 || *(const uint32_t *)(ctx + 0x734) != 4) {
		return skip(4);
	}
	const uint8_t *data = *(const uint8_t *const *)tex;
	uint64_t texrb = *(const uint64_t *)(tex + 0x10);
	uint32_t texw = *(const uint32_t *)(tex + 0x18), texh = *(const uint32_t *)(tex + 0x1c);
	static const uint32_t bpp[3] = {4, 1, 2};
	if (data == NULL || texw == 0 || texh == 0 || texrb < (uint64_t)texw * bpp[formats[fi].fmt]) {
		return skip(5);
	}
	if (((uintptr_t)data & 63) || texrb % 64) {
		return skip(9);
	}
	int32_t dx0 = *(const int32_t *)(ctx + 0x738), dx1 = *(const int32_t *)(ctx + 0x73c);
	int32_t dy0 = *(const int32_t *)(ctx + 0x740), dy1 = *(const int32_t *)(ctx + 0x744);
	/* bounding box pixels whose centers are inside, clipped (rs + 0x110) and inside the destination */
	float x0 = f[0], x1 = f[0], y0 = f[1], y1 = f[1];
	for (uint32_t i = 1; i < n; i++) {
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
		return skip(14);
	}
	/* unit 0 coordinates: s' = s * state[0x60] + state[0x68] (t: 0x64, 0x6c), texels = s' * w */
	const float *tm = (const float *)(state + 0x60);
	float as, cs, es, at, ct, et;
	if (!plane(f, n, 4, &as, &cs, &es) || !plane(f, n, 5, &at, &ct, &et)) {
		return skip(6);
	}
	pl->A = texw * tm[0] * as, pl->C = texw * tm[0] * cs, pl->E = texw * (tm[0] * es + tm[2]);
	pl->B = texh * tm[1] * at, pl->D = texh * tm[1] * ct, pl->F = texh * (tm[1] * et + tm[3]);
	/* sample inside the texture (what happens outside depends on the edge mode): the
	 * polygon's uvs are convex combinations of the vertices' */
	for (uint32_t i = 0; i < n; i++) {
		float x = f[i * 12], y = f[i * 12 + 1];
		float u = pl->A * x + pl->C * y + pl->E, v = pl->B * x + pl->D * y + pl->F;
		if (u < -0.01f || u > texw + 0.01f || v < -0.01f || v > texh + 0.01f) {
			return skip(10);
		}
	}
	/* other polygons: an edge function per side, positive inside */
	pl->nedges = 0;
	if (!axis) {
		float area = 0;
		for (uint32_t i = 0; i < n; i++) {
			const float *p = f + i * 12, *q = f + ((i + 1) % n) * 12;
			area += p[0] * q[1] - q[0] * p[1];
		}
		float sgn = area > 0 ? 1 : -1;
		for (uint32_t i = 0; i < n; i++) {
			const float *p = f + i * 12, *q = f + ((i + 1) % n) * 12;
			float ea = -(q[1] - p[1]) * sgn, eb = (q[0] - p[0]) * sgn;
			pl->edge[i][0] = fbits(ea), pl->edge[i][1] = fbits(eb), pl->edge[i][2] = fbits(-(ea * p[0] + eb * p[1]));
		}
		pl->nedges = n;
	}
	uint8_t f0 = tex[0x40], f1 = tex[0x41];
	pl->X0 = X0, pl->Y0 = Y0, pl->X1 = X1, pl->Y1 = Y1;
	pl->color[0] = fbits(cb), pl->color[1] = fbits(cg), pl->color[2] = fbits(cr), pl->color[3] = fbits(ca);
	pl->fmt = formats[fi].fmt, pl->swiz = formats[fi].swiz;
	pl->flags = (f0 == 0 && f1 == 0 ? SWEET_GPU_QUAD_NEAREST : 0) |
	    (blend == 12 ? SWEET_GPU_QUAD_ADD : blend == 1 ? SWEET_GPU_QUAD_COPY : 0);
	pl->tex = data, pl->texrb = texrb, pl->texw = texw, pl->texh = texh;
	pl->dst = dst, pl->drb = drb, pl->dx0 = dx0, pl->dy0 = dy0, pl->dx1 = dx1, pl->dy1 = dy1;
	bool ident = pl->A == 1 && pl->D == 1 && pl->B == 0 && pl->C == 0;
	/* key: fn 0-7, filters 8-11 / 12-15, format 16-18, ident 19, alpha < 1 20, blend 21-28, edges 29 */
	pl->key = fn | (uint32_t)f0 << 8 | (uint32_t)f1 << 12 | (uint32_t)fi << 16 | (uint32_t)ident << 19 |
	    (uint32_t)(ca < 1) << 20 | blend << 21 | (uint32_t)(pl->nedges != 0) << 29;
	return true;
}

/*
 * Offload where it pays. GPU path cost ~0.6 ms + 4.3 ms/Mpx (0149a: map +
 * quad + unmap); the CPU's cost per pixel is measured per kind of draw
 * (plan key) whenever the CPU draws one, and offloaded kinds go back to the
 * CPU every 64th draw to keep that current. Unknown kinds: CPU first.
 */
#define NCOST 256
static struct {
	uint32_t key, n;
	float cpu_ns_px;                /* EMA */
	uint32_t ok, bad, vn;           /* verifications: matched, mismatched; offload counter */
} cost[NCOST];

static uint32_t
cost_slot(uint32_t key)
{
	uint32_t h = (key * 2654435761u) >> 24;
	for (uint32_t i = 0; i < NCOST; i++, h = (h + 1) % NCOST) {
		if (cost[h].key == key || cost[h].n == 0) {
			cost[h].key = key;
			return h;
		}
	}
	return NCOST;
}

static bool
gpu_wins(const struct plan *pl)
{
	uint32_t c = cost_slot(pl->key);
	if (c == NCOST || cost[c].n == 0 || cost[c].bad || ++cost[c].n % 64 == 0) {
		return false;
	}
	float px = (float)(pl->X1 - pl->X0) * (pl->Y1 - pl->Y0);
	return cost[c].cpu_ns_px * px > 1.1f * (600000.0f + 4.3f * px);
}

/*
 * Offload mode only draws kinds the GPU has matched the CPU on (3 verified
 * draws, none bad); one in 32 of their draws is still drawn by the CPU and
 * verified. A mismatch bans the kind for good.
 */
static bool
trusted(const struct plan *pl)
{
	uint32_t c = cost_slot(pl->key);
	return c < NCOST && cost[c].ok >= 3 && cost[c].bad == 0 && ++cost[c].vn % 32 != 0;
}

static void
cost_verified(const struct plan *pl, bool good)
{
	uint32_t c = cost_slot(pl->key);
	if (c == NCOST) {
		return;
	}
	if (good) {
		cost[c].ok++;
	} else if (cost[c].bad++ == 0) {
		log_line("kind %08x banned (fmt %s fn %u blend %u)\n", pl->key, formats[(pl->key >> 16) & 7].name,
		    pl->key & 0xff, (pl->key >> 21) & 0xff);
	}
}

static void
cost_cpu(const struct plan *pl, uint64_t abs)
{
	uint32_t c = cost_slot(pl->key);
	if (c == NCOST) {
		return;
	}
	float ns = (float)(ms(abs) * 1e6), px = (float)(pl->X1 - pl->X0) * (pl->Y1 - pl->Y0);
	cost[c].cpu_ns_px = cost[c].n ? 0.75f * cost[c].cpu_ns_px + 0.25f * ns / px : ns / px;
	cost[c].n += cost[c].n == 0;
}

static uint64_t
map_ro(io_connect_t c, const void *p, size_t len)
{
	uint64_t in[3] = {(uint64_t)p, len, 1}, out[1] = {0};
	uint32_t n = 1;
	kern_return_t kr = IOConnectCallScalarMethod(c, 0, in, 3, out, &n);
	if (kr != KERN_SUCCESS && v_logged++ < 16) {
		log_line("map ro %p+%zu failed: 0x%x\n", p, len, kr);
	}
	return kr == KERN_SUCCESS ? out[0] : 0;
}

static void
unmap(io_connect_t c, uint64_t gva)
{
	if (gva) {
		IOConnectCallScalarMethod(c, 2, &gva, 1, NULL, NULL);
	}
}

/*
 * The quad with the destination image at GPU VA dva whose pixel (0, 0) is
 * destination pixel (ox, oy), w x h pixels, pitch dpitch.
 */
static kern_return_t
gpu_draw(const struct plan *pl, uint64_t dva, int32_t ox, int32_t oy, uint32_t w, uint32_t h, uint32_t dpitch)
{
	uint64_t sva = map_ro(g_conn, pl->tex, pl->texrb * pl->texh);
	if (sva == 0) {
		return KERN_FAILURE;
	}
	struct sweet_gpu_quad q = {
		.src_va = sva, .dst_va = dva,
		.src_w = pl->texw, .src_h = pl->texh, .src_pitch = (uint32_t)pl->texrb,
		.dst_w = w, .dst_h = h, .dst_pitch = dpitch,
		.x0 = pl->X0 - ox, .y0 = pl->Y0 - oy, .w = pl->X1 - pl->X0, .h = pl->Y1 - pl->Y0,
		.m = {fbits(pl->A), fbits(pl->B), fbits(pl->C), fbits(pl->D),
		      fbits(pl->A * ox + pl->C * oy + pl->E), fbits(pl->B * ox + pl->D * oy + pl->F)},
		.flags = pl->flags,
		.color = {pl->color[0], pl->color[1], pl->color[2], pl->color[3]},
		.src_fmt = pl->fmt, .src_swiz = pl->swiz, .nedges = pl->nedges,
	};
	memcpy(q.edge, pl->edge, sizeof(q.edge));
	kern_return_t kr = quad(g_conn, &q);
	unmap(g_conn, sva);
	return kr;
}

/* Offload: the GPU draws into the destination rows Y0..Y1. False: let the CPU draw. */
static bool
offload(const struct plan *pl)
{
	uint8_t *rows = pl->dst + (int64_t)(pl->Y0 - pl->dy0) * pl->drb;
	uint32_t h = pl->Y1 - pl->Y0, w = pl->dx1 - pl->dx0;
	if (((uintptr_t)rows & 63) || pl->drb % 64) {
		return skip(11);
	}
	pthread_mutex_lock(&g_gpu_lock);
	uint64_t t0 = mach_absolute_time();
	uint64_t dva = map(g_conn, rows, (size_t)pl->drb * h);
	kern_return_t kr = dva ? gpu_draw(pl, dva, pl->dx0, pl->Y0, w, h, (uint32_t)pl->drb) : KERN_FAILURE;
	unmap(g_conn, dva);
	uint64_t dt = mach_absolute_time() - t0;
	pthread_mutex_unlock(&g_gpu_lock);
	if (kr != KERN_SUCCESS) {
		off_fail++;
		if (v_logged++ < 16) {
			log_line("offload: 0x%x (tex %ux%u rb %llu, rect %ux%u)\n", kr, pl->texw, pl->texh, pl->texrb,
			    pl->X1 - pl->X0, h);
		}
		return false;
	}
	off_n++, off_px += (uint64_t)(pl->X1 - pl->X0) * h, off_ns += dt;
	return true;
}

/* Verify, before the CPU draw: copy the destination rect. */
struct vjob {
	struct plan pl;
	uint32_t *t;
	size_t tlen;
	uint32_t tpitch, w, h;
	uint8_t *rect;                  /* the destination rect's first pixel */
};

static bool
verify_prepare(struct vjob *j)
{
	uint64_t now = mach_absolute_time();
	if (ms(now - g_last_verify) < (g_offload ? 20 : 100)) {
		return false;
	}
	g_last_verify = now;
	j->w = j->pl.X1 - j->pl.X0, j->h = j->pl.Y1 - j->pl.Y0;
	j->tpitch = (j->w * 4 + 63) & ~63u;
	j->tlen = ((size_t)j->tpitch * j->h + 16383) & ~(size_t)16383;
	j->t = pages(j->tlen);
	if (j->t == NULL) {
		return skip(7);
	}
	j->rect = j->pl.dst + (int64_t)(j->pl.Y0 - j->pl.dy0) * j->pl.drb + (int64_t)(j->pl.X0 - j->pl.dx0) * 4;
	for (uint32_t y = 0; y < j->h; y++) {
		memcpy((uint8_t *)j->t + (size_t)y * j->tpitch, j->rect + y * j->pl.drb, j->w * 4);
	}
	return true;
}

/* Verify, after the CPU draw: the same on the GPU into the copy, then compare. */
static void
verify_run(struct vjob *j, uint64_t cpu)
{
	pthread_mutex_lock(&g_gpu_lock);
	uint64_t t0 = mach_absolute_time();
	uint64_t tg = map(g_conn, j->t, j->tlen);
	kern_return_t kr = tg ? gpu_draw(&j->pl, tg, j->pl.X0, j->pl.Y0, j->w, j->h, j->tpitch) : KERN_FAILURE;
	unmap(g_conn, tg);
	uint64_t gpu = mach_absolute_time() - t0;
	pthread_mutex_unlock(&g_gpu_lock);
	if (kr != KERN_SUCCESS) {
		v_skip[8]++;
		if (v_logged++ < 16) {
			log_line("verify: quad 0x%x (tex %ux%u rb %llu, dst %ux%u)\n", kr, j->pl.texw, j->pl.texh,
			    j->pl.texrb, j->w, j->h);
		}
		free(j->t);
		return;
	}
	uint64_t bad = 0, inner = 0;
	int first = -1;
	for (uint32_t y = 0; y < j->h; y++) {
		const uint32_t *c = (const uint32_t *)(j->rect + y * j->pl.drb), *g = j->t + (size_t)y * j->tpitch / 4;
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
	cost_verified(&j->pl, bad * 200 <= (uint64_t)j->w * j->h);     /* <= 0.5% off */
	uint32_t k;
	for (k = 0; k < NVKEY && vst[k].n && vst[k].key != j->pl.key; k++) {
	}
	if (k < NVKEY) {
		vst[k].key = j->pl.key;
		vst[k].n++, vst[k].exact += bad == 0, vst[k].bad += bad, vst[k].inner_bad += inner;
		vst[k].px += (uint64_t)j->w * j->h, vst[k].cpu += cpu, vst[k].gpu += gpu;
	}
	if (bad && v_logged++ < 16) {
		uint32_t x = first % j->w, y = first / j->w;
		log_line("verify %05x: %ux%u %llu bad (%llu inner); first (%u,%u) cpu %08x gpu %08x\n", j->pl.key,
		    j->w, j->h, bad, inner, x, y, ((const uint32_t *)(j->rect + y * j->pl.drb))[x],
		    j->t[y * j->tpitch / 4 + x]);
		log_line("  uv %g %g %g %g %g %g color %g %g %g %g tex %ux%u rb %llu fmt %u flags %u edges %u at (%d,%d)\n",
		    j->pl.A, j->pl.B, j->pl.C, j->pl.D, j->pl.E, j->pl.F, *(const float *)&j->pl.color[0],
		    *(const float *)&j->pl.color[1], *(const float *)&j->pl.color[2], *(const float *)&j->pl.color[3],
		    j->pl.texw, j->pl.texh, j->pl.texrb, j->pl.fmt, j->pl.flags, j->pl.nedges, j->pl.X0, j->pl.Y0);
	}
	free(j->t);
}

static void
gpu_report(void)
{
	for (uint32_t k = 0; k < NVKEY && vst[k].n; k++) {
		log_line("verify %s fn %u blend %u filt %u/%u a<1 %u ident %u edges %u: %llu draws, %llu exact, bad px %.2f%% (inner %.2f%%), "
		    "cpu %.2f gpu %.2f ms/draw, %.0f px/draw\n",
		    formats[(vst[k].key >> 16) & 7].name, vst[k].key & 0xff, (vst[k].key >> 21) & 0xff,
		    (vst[k].key >> 8) & 0xf, (vst[k].key >> 12) & 0xf,
		    (vst[k].key >> 20) & 1, (vst[k].key >> 19) & 1, vst[k].key >> 29, vst[k].n, vst[k].exact,
		    100.0 * vst[k].bad / vst[k].px, 100.0 * vst[k].inner_bad / vst[k].px,
		    ms(vst[k].cpu) / vst[k].n, ms(vst[k].gpu) / vst[k].n, (double)vst[k].px / vst[k].n);
	}
	if (el_n) {
		log_line("eligible (CPU drew): %llu draws, %.1f Mpx, %.1f ms\n", el_n, el_px / 1e6, ms(el_ns));
		el_n = el_px = el_ns = 0;
	}
	if (off_n || off_fail) {
		log_line("offload: %llu draws, %.1f Mpx, %.1f ms on the GPU path (%.2f ms/draw), %llu failed\n",
		    off_n, off_px / 1e6, ms(off_ns), off_n ? ms(off_ns) / off_n : 0.0, off_fail);
		off_n = off_px = off_ns = 0;
	}
	log_line("skips: fmt %llu shape %llu color %llu dst %llu tex %llu plane %llu alloc %llu quad %llu "
	    "pitch %llu bounds %llu rows %llu cpu-faster %llu\n", v_skip[1], v_skip[2], v_skip[3], v_skip[4], v_skip[5],
	    v_skip[6], v_skip[7], v_skip[8], v_skip[9], v_skip[10], v_skip[11], v_skip[12]);
	/* CPU time of the skipped draws by reason (2 s) */
	static const char *why_name[16] = {"", "fmt", "shape", "color", "dst", "tex", "plane", "alloc", "quad",
		                            "pitch", "bounds", "rows", "cpu-faster", "kind", "small", "colorprog"};
	char line[256];
	int m = snprintf(line, sizeof(line), "skipped CPU ms:");
	for (int w = 1; w < 16; w++) {
		if (skip_ns[w]) {
			m += snprintf(line + m, sizeof(line) - m, " %s %.0f (%.1f Mpx)", why_name[w], ms(skip_ns[w]), skip_px[w] / 1e6);
		}
		skip_ns[w] = skip_px[w] = 0;
	}
	log_line("%s\n", line);
}

static void
dp_hook(void *v, uint32_t n, const void *rs)
{
	struct vjob j;
	bool gpu = atomic_load_explicit(&g_gpu_ok, memory_order_relaxed) && plan(&j.pl, v, n, rs);
	bool planned = gpu;
	if (gpu && !gpu_wins(&j.pl)) {
		gpu = false;
		skip(12);               /* the CPU is faster for this one (or measuring it) */
	}
	int why = gpu ? 0 : g_why;
	bool off = gpu && g_offload && trusted(&j.pl);
	bool vj = gpu && !off && verify_prepare(&j);
	uint64_t t0 = mach_absolute_time();
	if (!(off && offload(&j.pl))) {
		off = false;
		sweetgpu_dp_tramp(v, n, rs);
	}
	uint64_t dt = mach_absolute_time() - t0;
	if (planned && !off) {
		cost_cpu(&j.pl, dt);    /* the CPU drew a GPU-capable draw */
	}
	if (vj) {
		verify_run(&j, dt);
	}
	if (gpu && !g_offload) {
		el_n++, el_px += (uint64_t)(j.pl.X1 - j.pl.X0) * (j.pl.Y1 - j.pl.Y0), el_ns += dt;
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
	if (why) {
		skip_ns[why] += dt, skip_px[why] += area;
	}
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
			gpu_report();
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
			e = getenv("SWEETGPU_CA_MODE");
			g_offload = e != NULL && strcmp(e, "offload") == 0;
			atomic_store(&g_gpu_ok, true);
			log_line("%s mode on\n", g_offload ? "offload" : "verify");
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
