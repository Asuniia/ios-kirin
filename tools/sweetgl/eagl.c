/*
 * EAGL on sweetgl: QuartzCore's GLES renderer (CA::OGL::GLESContext) on
 * Mesa instead of Apple's Metal-backed GLEngine.
 *
 * CA uses EAGLContext only through initWithAPI:properties:, API,
 * setParameter:to:, getMacroContextPrivate, texImageIOSurface:...,
 * attachImage:toCoreSurface:invertedRender:, +setCurrentContext: and
 * +currentContext; every GL call goes through the GLI dispatch table it gets
 * from getMacroContextPrivate ({rctx, disp[1036]}, layout from GLEngine,
 * table generated into out/dispatch.c). Those methods are replaced here for
 * contexts we create; the original EAGL code still runs for everything else.
 *
 * IOSurfaces become texture storage the EGL way: imported as a GEM bo
 * (SweetGPU selector 12), wrapped in a pipe_resource, handed to
 * glEGLImageTargetTexture2DOES through the frontend's get_egl_image.
 */
#include <objc/NSObjCRuntime.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <IOSurface/IOSurfaceRef.h>
#include <mach/vm_page_size.h>

#include "eagl.h"

#include "pipe/p_screen.h"
#include "pipe/p_state.h"
#include "frontend/api.h"
#include "frontend/winsys_handle.h"
#include "util/u_inlines.h"
#include "drm-uapi/drm_fourcc.h"

#include "sweet_drm.h"
#include "sweetgl.h"

/* not in the GL bridge (ES-only): Mesa's implementation */
void _mesa_EGLImageTargetTexture2DOES(GLenum target, GLeglImageOES image);

struct sgl_context {
	void *rctx;                     /* = this; CA reads {rctx, disp[]} from macro */
	void *disp[SGL_SLOTS];
	id eagl;
	unsigned api;
	struct sweetgl_ctx *ctx;
};

struct sgl_image {
	struct pipe_resource *res;
	enum pipe_format format;
	unsigned internalformat;
};

__thread void *sgl_current;
int sgl_check_errors;
static int trace_left;          /* SWEETGL_TRACE=1: this many trace lines */
static bool dump_shaders;       /* SWEETGL_TRACE=1: every shader source, no limit */

#define TRACE(...) do { if (trace_left > 0) { trace_left--; sweet_drm_log("sweetgl: trace " __VA_ARGS__); } } while (0)

/* ponytail: linear table of contexts; backboardd makes one or two */
#define MAX_CTX 32
static struct sgl_context *ctxs[MAX_CTX];
static pthread_mutex_t ctx_lock = PTHREAD_MUTEX_INITIALIZER;

static IMP orig_init, orig_dealloc, orig_api, orig_set_param, orig_macro, orig_tex_iosurface, orig_attach,
    orig_set_current, orig_current;

static struct sgl_context *
lookup(id eagl)
{
	struct sgl_context *c = NULL;
	pthread_mutex_lock(&ctx_lock);
	for (int i = 0; i < MAX_CTX && c == NULL && eagl != NULL; i++) {
		if (ctxs[i] && ctxs[i]->eagl == eagl) {
			c = ctxs[i];
		}
	}
	pthread_mutex_unlock(&ctx_lock);
	return c;
}

void
sgl_bind(void *rctx)
{
	struct sgl_context *c = rctx;
	sweetgl_make_current(c ? c->ctx : NULL);
	sgl_current = c;
}

void
sgl_unsupported(int slot, const char *name)
{
	static unsigned char seen[SGL_SLOTS];
	if (slot >= 0 && slot < SGL_SLOTS && !seen[slot]) {
		seen[slot] = 1;
		sweet_drm_log("sweetgl: unsupported GLI slot %d (%s)", slot, name);
	}
}

void
sgl_post(int slot)
{
	static unsigned char logged[SGL_SLOTS][8];     /* per slot and error, the first 3 */
	static bool used[SGL_SLOTS];
	if (!used[slot]) {
		used[slot] = true;
		sweet_drm_log("sweetgl: first call of slot %d %s", slot, sgl_slot_names[slot]);
	}
	GLenum e = glGetError();
	unsigned k = e - GL_INVALID_ENUM;
	if (e != GL_NO_ERROR && (k >= 8 || logged[slot][k] < 3)) {
		if (k < 8) {
			logged[slot][k]++;
		}
		sweet_drm_log("sweetgl: GL error %#x after slot %d %s", e, slot, sgl_slot_names[slot]);
	}
}

/* ---- hand-written slots ---- */

#define H(o) ((GLuint)(uintptr_t)(o))

void
sgl_DeleteObjectARB(void *rctx, GLhandleARB obj)
{
	SGL_CUR(rctx);
	if (glIsProgram(H(obj))) {
		glDeleteProgram(H(obj));
	} else {
		glDeleteShader(H(obj));
	}
}

void
sgl_GetObjectParameterivARB(void *rctx, GLhandleARB obj, GLenum pname, GLint *params)
{
	SGL_CUR(rctx);
	if (pname == GL_OBJECT_TYPE_ARB) {
		*params = glIsProgram(H(obj)) ? GL_PROGRAM_OBJECT_ARB : GL_SHADER_OBJECT_ARB;
	} else if (glIsProgram(H(obj))) {
		glGetProgramiv(H(obj), pname, params);
	} else {
		glGetShaderiv(H(obj), pname == GL_OBJECT_SUBTYPE_ARB ? GL_SHADER_TYPE : pname, params);
	}
}

void
sgl_GetInfoLogARB(void *rctx, GLhandleARB obj, GLsizei max, GLsizei *length, GLcharARB *log)
{
	SGL_CUR(rctx);
	if (glIsProgram(H(obj))) {
		glGetProgramInfoLog(H(obj), max, length, log);
	} else {
		glGetShaderInfoLog(H(obj), max, length, log);
	}
	if (log && max > 0 && log[0]) {
		sweet_drm_log("sweetgl: info log %u: %.300s", H(obj), log);
	}
}

void
sgl_GetAttachedObjectsARB(void *rctx, GLhandleARB prog, GLsizei max, GLsizei *count, GLhandleARB *obj)
{
	SGL_CUR(rctx);
	GLuint s[16];
	GLsizei n = 0;
	glGetAttachedShaders(H(prog), max < 16 ? max : 16, &n, s);
	for (GLsizei i = 0; i < n; i++) {
		obj[i] = (GLhandleARB)(uintptr_t)s[i];
	}
	if (count) {
		*count = n;
	}
}

GLhandleARB
sgl_GetHandleARB(void *rctx, GLenum pname)
{
	(void)rctx;
	(void)pname;
	return 0;
}

void
sgl_DiscardFramebufferEXT(void *rctx, GLenum target, GLsizei n, const GLenum *attachments)
{
	SGL_CUR(rctx);
	GLint fb = 0;
	glGetIntegerv(GL_FRAMEBUFFER_BINDING, &fb);
	TRACE("DiscardFramebuffer fb %d n %d first %#x", fb, n, n > 0 ? attachments[0] : 0);
	glInvalidateFramebuffer(target, n, attachments);
}

void
sgl_GetShaderPrecisionFormatOES(void *rctx, GLenum shader, GLenum precision, GLint *range, GLint *prec)
{
	SGL_CUR(rctx);
	glGetShaderPrecisionFormat(shader, precision, range, prec);
}

void
sgl_ReleaseShaderCompilerOES(void *rctx)
{
	(void)rctx;
}

void
sgl_ShaderBinaryOES(void *rctx, GLsizei n, const GLuint *shaders, GLenum format, const void *binary,
    GLsizei length)
{
	SGL_CUR(rctx);
	glShaderBinary(n, shaders, format, binary, length);
}

void
sgl_GetIntegerIndexedv(void *rctx, GLenum pname, GLuint index, GLint *data)
{
	SGL_CUR(rctx);
	glGetIntegeri_v(pname, index, data);
}

const GLubyte *
sgl_GetString(void *rctx, GLenum name)
{
	SGL_CUR(rctx);
	const GLubyte *s = glGetString(name);
	if (s == NULL) {
		sweet_drm_log("sweetgl: glGetString(%#x) = NULL (error %#x)", name, glGetError());
	}
	return s;
}

/*
 * GL_TEXTURE_RECTANGLE (Apple's ES has it, Mesa's ES does not): such
 * textures are 2D ones (SGL_TGT), and shaders sampling them as
 * sampler2DRect get texel coordinates scaled by sgl_rect_scale, set before
 * each draw to 1/size of the texture on that sampler's unit.
 * ponytail: one rect sampler per shader (CA's), small fixed tables.
 */
#define RECT_MAX 64
static struct { GLuint shader; char sampler[32]; } rect_shaders[RECT_MAX];
static struct { GLuint program; GLint scale, sampler; } rect_programs[RECT_MAX];
static int n_rect_shaders, n_rect_programs;

/* level-0 sizes of 2D textures as created through us (rect scales without GL queries) */
#define TEXSZ_MAX 8192
static uint16_t tex_w[TEXSZ_MAX], tex_h[TEXSZ_MAX];

static void
tex_size_bound(GLenum target, GLint level, GLsizei w, GLsizei h)
{
	GLint tex = 0;
	if (level != 0 || (target != GL_TEXTURE_2D && target != 0x84F5)) {
		return;
	}
	glGetIntegerv(GL_TEXTURE_BINDING_2D, &tex);
	if (tex > 0 && tex < TEXSZ_MAX) {
		tex_w[tex] = (uint16_t)w;
		tex_h[tex] = (uint16_t)h;
	}
}

static const char rect_prelude[] =
    "#define sampler2DRect sampler2D\n"
    "#define texture2DRect(s, c) texture2D(s, (c) * sgl_rect_scale)\n"
    "uniform highp vec2 sgl_rect_scale;\n";

void
sgl_ShaderSourceARB(void *rctx, GLhandleARB shader, GLsizei count, const GLcharARB **string, const GLint *length)
{
	SGL_CUR(rctx);
	size_t n = 0;
	for (GLsizei i = 0; i < count; i++) {
		n += length && length[i] >= 0 ? (size_t)length[i] : strlen(string[i]);
	}
	char *src = malloc(n + sizeof(rect_prelude) + 1), *p = src;
	for (GLsizei i = 0; i < count; i++) {
		size_t l = length && length[i] >= 0 ? (size_t)length[i] : strlen(string[i]);
		memcpy(p, string[i], l);
		p += l;
	}
	*p = 0;
	/*
	 * CA's inverted-mask shader uses coord_arg (clamp bounds) without
	 * declaring it; Apple's compiler takes that, Mesa's does not.
	 */
	const char *extra = strstr(src, "coord_arg") && !strstr(src, "vec4 coord_arg") ?
	    "uniform highp vec4 coord_arg;\n" : "";
	size_t extra_len = strlen(extra);
	if (extra_len) {
		src = realloc(src, n + sizeof(rect_prelude) + extra_len + 1);
		size_t at = strncmp(src, "#version", 8) == 0 && strchr(src, '\n') ? (size_t)(strchr(src, '\n') - src + 1) : 0;
		memmove(src + at + extra_len, src + at, n - at + 1);
		memcpy(src + at, extra, extra_len);
		n += extra_len;
	}
	const char *rect = strstr(src, "sampler2DRect");
	if (rect != NULL && n_rect_shaders < RECT_MAX) {
		const char *name = rect + strlen("sampler2DRect");
		while (*name == ' ' || *name == '\t') {
			name++;
		}
		size_t l = strspn(name, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_");
		rect_shaders[n_rect_shaders].shader = H(shader);
		snprintf(rect_shaders[n_rect_shaders].sampler, sizeof(rect_shaders[0].sampler), "%.*s", (int)l, name);
		n_rect_shaders++;
		/* after a leading #version line, if any */
		size_t at = strncmp(src, "#version", 8) == 0 && strchr(src, '\n') ? (size_t)(strchr(src, '\n') - src + 1) : 0;
		memmove(src + at + sizeof(rect_prelude) - 1, src + at, n - at + 1);
		memcpy(src + at, rect_prelude, sizeof(rect_prelude) - 1);
	}
	if (dump_shaders) {
		/* whole source, in 400-byte pieces (log lines are 512 bytes) */
		sweet_drm_log("sweetgl: shader %u source (%zu bytes):", H(shader), strlen(src));
		for (size_t i = 0, l = strlen(src); i < l; i += 400) {
			sweet_drm_log("sweetgl:   | %.400s", src + i);
		}
	}
	const GLchar *one = src;
	glShaderSource(H(shader), 1, &one, NULL);
	free(src);
}

static void
rect_link(GLuint program)
{
	GLuint attached[8];
	GLsizei na = 0;
	glGetAttachedShaders(program, 8, &na, attached);
	for (GLsizei a = 0; a < na; a++) {
		for (int i = 0; i < n_rect_shaders; i++) {
			if (rect_shaders[i].shader == attached[a] && n_rect_programs < RECT_MAX) {
				rect_programs[n_rect_programs].program = program;
				rect_programs[n_rect_programs].scale = glGetUniformLocation(program, "sgl_rect_scale");
				rect_programs[n_rect_programs].sampler = glGetUniformLocation(program, rect_shaders[i].sampler);
				n_rect_programs++;
				return;
			}
		}
	}
}

/* per unit: the 2D and rect bindings CA made, and the one GL holds */
#define UNITS 16
static GLuint unit_2d[UNITS], unit_rect[UNITS], unit_held[UNITS];

static int
active_unit(void)
{
	GLint active = GL_TEXTURE0;
	glGetIntegerv(GL_ACTIVE_TEXTURE, &active);
	return (active - GL_TEXTURE0) & (UNITS - 1);
}

GLenum
sgl_tgt(GLenum target)
{
	int u = active_unit();
	GLuint want = target == GL_TEXTURE_2D ? unit_2d[u] : unit_rect[u];
	if (unit_held[u] != want) {
		glBindTexture(GL_TEXTURE_2D, want);
		unit_held[u] = want;
	}
	return GL_TEXTURE_2D;
}

/*
 * Textures holding a copy of a surface (copy_surface): copied again before
 * a draw when the surface's seed moved (its writer unlocked it since).
 * ponytail: one copy per texture name below TEXSZ_MAX.
 */
struct copied {
	IOSurfaceRef surface;
	uint32_t seed;
	GLenum format, type;
};
static struct copied copies[TEXSZ_MAX];
static bool copy_surface(IOSurfaceRef s, GLenum target, GLenum format, GLenum type, size_t w, size_t h,
    size_t pitch, const char *why);
static void set_swizzle(GLenum target, bool argb);

static void
copy_forget(GLuint tex)
{
	if (tex < TEXSZ_MAX && copies[tex].surface != NULL) {
		CFRelease(copies[tex].surface);
		copies[tex].surface = NULL;
	}
}

static void
copy_refresh(int u, int *active)
{
	GLuint tex = unit_held[u];
	struct copied *c = tex < TEXSZ_MAX ? &copies[tex] : NULL;
	if (c == NULL || c->surface == NULL || IOSurfaceGetSeed(c->surface) == c->seed) {
		return;
	}
	if (*active < 0) {
		*active = active_unit();
	}
	glActiveTexture(GL_TEXTURE0 + u);
	IOSurfaceRef s = c->surface;
	copy_surface(s, GL_TEXTURE_2D, c->format, c->type, IOSurfaceGetWidth(s), IOSurfaceGetHeight(s),
	    IOSurfaceGetBytesPerRow(s), "refresh");
}

/*
 * SWEETGL_TRACE: samplers of the current program whose texture has no level
 * 0 (unbound or never filled), once per program/unit/texture.
 */
static void
trace_empty_samplers(GLint prog)
{
	static uint32_t seen[4096];
	GLint n = 0;
	glGetProgramiv(prog, GL_ACTIVE_UNIFORMS, &n);
	for (GLint i = 0; i < n; i++) {
		char name[64];
		GLint size = 0;
		GLenum type = 0;
		glGetActiveUniform(prog, i, sizeof(name), NULL, &size, &type, name);
		if (type != GL_SAMPLER_2D) {
			continue;
		}
		GLint unit = 0, tex = 0, w = 0, active = GL_TEXTURE0;
		glGetUniformiv(prog, glGetUniformLocation(prog, name), &unit);
		glGetIntegerv(GL_ACTIVE_TEXTURE, &active);
		glActiveTexture(GL_TEXTURE0 + unit);
		glGetIntegerv(GL_TEXTURE_BINDING_2D, &tex);
		glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &w);
		glActiveTexture(active);
		uint32_t key = (uint32_t)prog << 20 ^ (uint32_t)unit << 16 ^ (uint32_t)tex;
		if (w > 0 || seen[key % 4096] == key + 1) {
			continue;
		}
		seen[key % 4096] = key + 1;
		TRACE("empty sampler: program %d %s unit %d tex %d", prog, name, unit, tex);
	}
}

extern void _mesa_TextureBarrierNV(void);

/*
 * CA reads the destination by sampling the texture it is rendering into
 * (Apple's GPUs return what memory held when the pass began). A flush
 * first makes the earlier draws visible to the sampler; without it the
 * read came back empty (App Library pods, CC tiles, vibrant glyphs).
 * ponytail: sampler units per program read once (CA sets them at link).
 */
#define PROG_MAX 4096
static uint16_t prog_units[PROG_MAX];   /* bit u: a sampler reads unit u; bit 15: known */

static void
feedback_barrier(GLint prog)
{
	GLint fb = 0, att = 0, type = 0;
	if (prog <= 0 || prog >= PROG_MAX) {
		return;
	}
	if (!(prog_units[prog] & 0x8000)) {
		uint16_t m = 0x8000;
		GLint n = 0;
		glGetProgramiv(prog, GL_ACTIVE_UNIFORMS, &n);
		for (GLint i = 0; i < n; i++) {
			char name[64];
			GLint size = 0, unit = 0;
			GLenum t = 0;
			glGetActiveUniform(prog, i, sizeof(name), NULL, &size, &t, name);
			if (t == GL_SAMPLER_2D) {
				glGetUniformiv(prog, glGetUniformLocation(prog, name), &unit);
				m |= unit >= 0 && unit < 15 ? 1u << unit : 0;
			}
		}
		prog_units[prog] = m;
	}
	glGetIntegerv(GL_FRAMEBUFFER_BINDING, &fb);
	if (fb == 0) {
		return;
	}
	glGetFramebufferAttachmentParameteriv(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
	    GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE, &type);
	if (type != GL_TEXTURE) {
		return;
	}
	glGetFramebufferAttachmentParameteriv(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
	    GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME, &att);
	for (int u = 0; u < 15; u++) {
		if ((prog_units[prog] & 1u << u) && unit_held[u] == (GLuint)att) {
			TRACE("feedback: prog %d samples fb %d's texture %d on unit %d: barrier", prog, fb, att, u);
			_mesa_TextureBarrierNV();
			return;
		}
	}
}

/* SWEETGL_TRACE: each distinct draw state once (program, blend, stencil, depth, target) */
static bool
trace_draw_state(GLint prog, GLenum mode, GLsizei count)
{
	static uint32_t seen[8192];
	GLint v[16] = {0};
	GLboolean m[4] = {0};
	v[0] = glIsEnabled(GL_BLEND);
	glGetIntegerv(GL_BLEND_SRC_RGB, &v[1]);
	glGetIntegerv(GL_BLEND_DST_RGB, &v[2]);
	glGetIntegerv(GL_BLEND_SRC_ALPHA, &v[3]);
	glGetIntegerv(GL_BLEND_DST_ALPHA, &v[4]);
	glGetIntegerv(GL_BLEND_EQUATION_RGB, &v[5]);
	v[6] = glIsEnabled(GL_STENCIL_TEST);
	glGetIntegerv(GL_STENCIL_FUNC, &v[7]);
	glGetIntegerv(GL_STENCIL_PASS_DEPTH_PASS, &v[8]);
	v[9] = glIsEnabled(GL_DEPTH_TEST);
	v[10] = glIsEnabled(GL_SCISSOR_TEST);
	glGetBooleanv(GL_COLOR_WRITEMASK, m);
	glGetIntegerv(GL_FRAMEBUFFER_BINDING, &v[11]);
	GLint vp[4] = {0};
	glGetIntegerv(GL_VIEWPORT, vp);
	uint32_t key = 2166136261u;
	for (int i = 0; i < 12; i++) {
		key = (key ^ (uint32_t)v[i]) * 16777619u;
	}
	key = (key ^ (uint32_t)prog ^ (uint32_t)mode << 8 ^ (uint32_t)(m[0] | m[1] << 1 | m[2] << 2 | m[3] << 3) << 16) *
	    16777619u;
	key = (key ^ (uint32_t)vp[2] ^ (uint32_t)vp[3] << 16) * 16777619u;
	if (seen[key % 8192] == key) {
		return false;
	}
	seen[key % 8192] = key;
	TRACE("draw prog %d tex %u %u %u mode %#x n %d fb %d vp %dx%d blend %d %#x/%#x %#x/%#x eq %#x stencil %d func %#x pass %#x "
	    "depth %d scissor %d mask %d%d%d%d", prog, unit_held[0], unit_held[1], unit_held[2], mode, count, v[11], vp[2], vp[3], v[0], v[1], v[2], v[3], v[4],
	    v[5], v[6], v[7], v[8], v[9], v[10], m[0], m[1], m[2], m[3]);
	return true;
}

/* after a new draw state (trace): what the viewport now holds */
static void
trace_draw_result(GLint prog)
{
	GLint vp[4] = {0}, fb = 0;
	glGetIntegerv(GL_VIEWPORT, vp);
	glGetIntegerv(GL_FRAMEBUFFER_BINDING, &fb);
	size_t n = (size_t)vp[2] * (size_t)vp[3];
	uint8_t *px = n ? malloc(n * 4) : NULL;
	if (px == NULL) {
		return;
	}
	glReadPixels(vp[0], vp[1], vp[2], vp[3], GL_RGBA, GL_UNSIGNED_BYTE, px);
	GLenum e = glGetError();
	size_t lit = 0, first = n;
	uint8_t mx[4] = {0};
	for (size_t i = 0; i < n; i++) {
		if (px[i * 4 + 3] != 0) {
			lit++;
			first = first < n ? first : i;
		}
		for (int c = 0; c < 4; c++) {
			mx[c] = px[i * 4 + c] > mx[c] ? px[i * 4 + c] : mx[c];
		}
	}
	const uint8_t *f = first < n ? px + first * 4 : mx;
	TRACE("  -> prog %d fb %d: %zu of %zu pixels with alpha, max %02x%02x%02x%02x, first lit %02x%02x%02x%02x at %zu,%zu "
	    "(err %#x)", prog, fb, lit, n, mx[0], mx[1], mx[2], mx[3], f[0], f[1], f[2], f[3],
	    first < n ? first % (size_t)vp[2] : 0, first < n ? first / (size_t)vp[2] : 0, e);
	free(px);
}

/* before a draw: each unit holds what the program samples there, rect scale set */
static bool
draw_prep(GLenum mode, GLsizei count)
{
	GLint prog = 0, unit = -1, w = 0, h = 0, scale = -1;
	bool fresh = false;
	glGetIntegerv(GL_CURRENT_PROGRAM, &prog);
	for (int i = 0; i < n_rect_programs; i++) {
		if (rect_programs[i].program == (GLuint)prog) {
			scale = rect_programs[i].scale;
			unit = 0;
			if (rect_programs[i].sampler >= 0) {
				glGetUniformiv(prog, rect_programs[i].sampler, &unit);
			}
			break;
		}
	}
	int active = -1;
	for (int u = 0; u < UNITS; u++) {
		GLuint want = u == unit ? unit_rect[u] : unit_2d[u];
		if (unit_held[u] != want) {
			if (active < 0) {
				active = active_unit();
			}
			glActiveTexture(GL_TEXTURE0 + u);
			glBindTexture(GL_TEXTURE_2D, want);
			unit_held[u] = want;
		}
		if (want != 0) {
			copy_refresh(u, &active);
		}
	}
	if (unit >= 0 && unit < UNITS && scale >= 0) {
		GLuint tex = unit_rect[unit];
		if (tex < TEXSZ_MAX) {
			w = tex_w[tex];
			h = tex_h[tex];
		}
		if (w <= 0 || h <= 0) {
			if (active < 0) {
				active = active_unit();
			}
			glActiveTexture(GL_TEXTURE0 + unit);
			glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &w);
			glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &h);
		}
		glUniform2f(scale, w > 0 ? 1.0f / w : 1.0f, h > 0 ? 1.0f / h : 1.0f);
	}
	if (active >= 0) {
		glActiveTexture(GL_TEXTURE0 + active);
	}
	feedback_barrier(prog);
	if (trace_left > 0 && prog != 0) {
		trace_empty_samplers(prog);
		fresh = trace_draw_state(prog, mode, count);
	}
	return fresh;
}

void
sgl_BindTexture(void *rctx, GLenum target, GLuint texture)
{
	SGL_CUR(rctx);
	if (target == GL_TEXTURE_2D || target == 0x84F5) {
		int u = active_unit();
		*(target == GL_TEXTURE_2D ? &unit_2d[u] : &unit_rect[u]) = texture;
		unit_held[u] = texture;
		glBindTexture(GL_TEXTURE_2D, texture);
	} else {
		glBindTexture(target, texture);
	}
	SGL_POST(5);
}

/* GL unbinds deleted textures everywhere: so do the shadows */
void
sgl_DeleteTextures(void *rctx, GLsizei n, const GLuint *textures)
{
	SGL_CUR(rctx);
	for (GLsizei i = 0; i < n; i++) {
		copy_forget(textures[i]);
		for (int u = 0; u < UNITS; u++) {
			if (unit_2d[u] == textures[i]) {
				unit_2d[u] = 0;
			}
			if (unit_rect[u] == textures[i]) {
				unit_rect[u] = 0;
			}
			if (unit_held[u] == textures[i]) {
				unit_held[u] = 0;
			}
		}
	}
	glDeleteTextures(n, textures);
	SGL_POST(59);
}

void
sgl_PixelStorei(void *rctx, GLenum pname, GLint param)
{
	SGL_CUR(rctx);
	(void)0;
	glPixelStorei(pname, param);
	SGL_POST(195);
}

void
sgl_DrawArrays(void *rctx, GLenum mode, GLint first, GLsizei count)
{
	SGL_CUR(rctx);
	bool fresh = draw_prep(mode, count);
	glDrawArrays(mode, first, count);
	if (fresh) {
		GLint prog = 0;
		glGetIntegerv(GL_CURRENT_PROGRAM, &prog);
		trace_draw_result(prog);
	}
	SGL_POST(65);
}

void
sgl_DrawElements(void *rctx, GLenum mode, GLsizei count, GLenum type, const void *indices)
{
	SGL_CUR(rctx);
	bool fresh = draw_prep(mode, count);
	glDrawElements(mode, count, type, indices);
	if (fresh) {
		GLint prog = 0;
		glGetIntegerv(GL_CURRENT_PROGRAM, &prog);
		trace_draw_result(prog);
	}
	SGL_POST(67);
}

/* failures logged with the info log and source: CA does not ask for them */
void
sgl_CompileShaderARB(void *rctx, GLhandleARB shader)
{
	SGL_CUR(rctx);
	glCompileShader(H(shader));
	GLint ok = 0;
	glGetShaderiv(H(shader), GL_COMPILE_STATUS, &ok);
	if (!ok) {
		static char log[1024], src[4096];
		glGetShaderInfoLog(H(shader), sizeof(log), NULL, log);
		glGetShaderSource(H(shader), sizeof(src), NULL, src);
		sweet_drm_log("sweetgl: shader %u compile failed: %.400s", H(shader), log);
		for (size_t i = 0, n = strlen(src); i < n; i += 400) {
			sweet_drm_log("sweetgl:   src %.400s", src + i);
		}
	}
}

void
sgl_LinkProgramARB(void *rctx, GLhandleARB program)
{
	SGL_CUR(rctx);
	glLinkProgram(H(program));
	GLint ok = 0;
	glGetProgramiv(H(program), GL_LINK_STATUS, &ok);
	if (ok) {
		rect_link(H(program));
	}
	if (dump_shaders) {
		GLuint sh[4] = {0};
		GLsizei n = 0;
		glGetAttachedShaders(H(program), 4, &n, sh);
		sweet_drm_log("sweetgl: program %u = shaders %u %u", H(program), sh[0], sh[1]);
	}
	if (!ok) {
		static char log[1024];
		glGetProgramInfoLog(H(program), sizeof(log), NULL, log);
		sweet_drm_log("sweetgl: program %u link failed: %.400s", H(program), log);
	}
}

void
sgl_FramebufferTexture2DEXT(void *rctx, GLenum target, GLenum attachment, GLenum textarget, GLuint texture, GLint level)
{
	SGL_CUR(rctx);
	GLint fb = 0;
	glGetIntegerv(GL_FRAMEBUFFER_BINDING, &fb);
	TRACE("FramebufferTexture2D fb %d attachment %#x tex %u", fb, attachment, texture);
	glFramebufferTexture2D(target, attachment, textarget == 0x84F5 ? GL_TEXTURE_2D : textarget, texture, level);
	SGL_POST(677);
}

void
sgl_TexParameteri(void *rctx, GLenum target, GLenum pname, GLint param)
{
	SGL_CUR(rctx);
	target = SGL_TGT(target);
	if (pname >= 0x8E42 && pname <= 0x8E46) {
		TRACE("TexParameteri tex %u swizzle %#x = %#x", unit_held[active_unit()], pname, param);
	}
	glTexParameteri(target, pname, param);
	SGL_POST(304);
}

void
sgl_Clear(void *rctx, GLbitfield mask)
{
	SGL_CUR(rctx);
	GLint fb = 0;
	glGetIntegerv(GL_FRAMEBUFFER_BINDING, &fb);
	TRACE("Clear fb %d mask %#x", fb, mask);
	glClear(mask);
	SGL_POST(10);
}

void
sgl_BindFramebufferEXT(void *rctx, GLenum target, GLuint framebuffer)
{
	SGL_CUR(rctx);
	TRACE("BindFramebuffer %u", framebuffer);
	glBindFramebuffer(target, framebuffer);
	SGL_POST(672);
}

static GLint
unpack_row(void)
{
	GLint row = 0;
	glGetIntegerv(GL_UNPACK_ROW_LENGTH, &row);
	return row;
}

void
sgl_TexSubImage2D(void *rctx, GLenum target, GLint level, GLint x, GLint y, GLsizei width, GLsizei height,
    GLenum format, GLenum type, const void *pixels)
{
	SGL_CUR(rctx);
	target = SGL_TGT(target);
	TRACE("TexSubImage2D tex %u level %d at %d,%d %dx%d fmt %#x type %#x row %d", unit_held[active_unit()],
	    level, x, y, width, height, format, type, unpack_row());
	glTexSubImage2D(target, level, x, y, width, height, format, type, pixels);
	SGL_POST(307);
}

/* Apple's ES takes internalformat GL_RGBA with format GL_BGRA; Mesa wants GL_BGRA for both */
void
sgl_TexImage2D(void *rctx, GLenum target, GLint level, GLint internalformat, GLsizei width,
    GLsizei height, GLint border, GLenum format, GLenum type, const void *pixels)
{
	SGL_CUR(rctx);
	GLenum asked = target;
	target = SGL_TGT(target);
	TRACE("TexImage2D target %#x unit %d tex %u level %d ifmt %#x %dx%d fmt %#x type %#x row %d", asked,
	    active_unit(), unit_held[active_unit()], level, internalformat, width, height, format, type, unpack_row());
	copy_forget(unit_held[active_unit()]);
	set_swizzle(target, false);
	if (format == GL_BGRA_EXT && (internalformat == GL_RGBA || internalformat == GL_RGBA8)) {
		internalformat = GL_BGRA_EXT;
	}
	glTexImage2D(target, level, internalformat, width, height, border, format, type, pixels);
	SGL_POST(301);
	tex_size_bound(target, level, width, height);
}

/*
 * ponytail: flush = finish. CA hands rendered IOSurfaces to the display
 * (IOMFB swap, copied by the kernel at once) without waiting for the GPU;
 * per-surface fences in the swap path would let the GPU run ahead.
 */
void
sgl_Flush(void *rctx)
{
	SGL_CUR(rctx);
	glFinish();
}

void
sgl_FinishObjectAPPLE(void *rctx, GLenum object, GLint name)
{
	(void)object;
	(void)name;
	SGL_CUR(rctx);
	glFinish();
}

GLboolean
sgl_TestObjectAPPLE(void *rctx, GLenum object, GLuint name)
{
	(void)object;
	(void)name;
	SGL_CUR(rctx);
	glFinish();
	return GL_TRUE;
}

/* ---- IOSurfaces as textures ---- */

static bool
get_egl_image(struct pipe_frontend_screen *fscreen, void *image, struct st_egl_image *out)
{
	(void)fscreen;
	struct sgl_image *img = image;
	pipe_resource_reference(&out->texture, img->res);
	out->format = img->format;
	out->level = 0;
	out->layer = 0;
	out->internalformat = img->internalformat;
	return true;
}

static bool
validate_egl_image(struct pipe_frontend_screen *fscreen, void *image)
{
	(void)fscreen;
	return image != NULL;
}

void
sweetgl_image_hooks(struct pipe_frontend_screen *fscreen)
{
	fscreen->get_egl_image = get_egl_image;
	fscreen->validate_egl_image = validate_egl_image;
}

static enum pipe_format
gl_format(GLenum format, GLenum type, unsigned *internalformat)
{
	bool half = type == GL_HALF_FLOAT || type == 0x8D61 /* GL_HALF_FLOAT_OES */;
	switch (format) {
	case GL_BGRA_EXT:
		*internalformat = GL_RGBA8;
		return PIPE_FORMAT_B8G8R8A8_UNORM;
	case GL_RGBA:
		*internalformat = half ? GL_RGBA16F : GL_RGBA8;
		return half ? PIPE_FORMAT_R16G16B16A16_FLOAT : PIPE_FORMAT_R8G8B8A8_UNORM;
	case GL_LUMINANCE_ALPHA:
		*internalformat = GL_LUMINANCE_ALPHA;
		return PIPE_FORMAT_L8A8_UNORM;
	case GL_LUMINANCE:
		*internalformat = GL_LUMINANCE;
		return PIPE_FORMAT_L8_UNORM;
	case GL_ALPHA:
		*internalformat = GL_ALPHA;
		return PIPE_FORMAT_A8_UNORM;
	case GL_RED:
		*internalformat = half ? GL_R16F : GL_R8;
		return half ? PIPE_FORMAT_R16_FLOAT : PIPE_FORMAT_R8_UNORM;
	case GL_RG:
		*internalformat = half ? GL_RG16F : GL_RG8;
		return half ? PIPE_FORMAT_R16G16_FLOAT : PIPE_FORMAT_R8G8_UNORM;
	default:
		return PIPE_FORMAT_NONE;
	}
}

static enum pipe_format
surface_format(uint32_t fourcc, size_t bpe, unsigned *internalformat)
{
	switch (fourcc) {
	case 0:                 /* CA's own render surfaces carry no format */
		return bpe == 4 ? gl_format(GL_BGRA_EXT, GL_UNSIGNED_BYTE, internalformat) :
		       bpe == 8 ? gl_format(GL_RGBA, GL_HALF_FLOAT, internalformat) :
		       bpe == 1 ? gl_format(GL_RED, GL_UNSIGNED_BYTE, internalformat) : PIPE_FORMAT_NONE;
	case 'LA08':
		return gl_format(GL_LUMINANCE_ALPHA, GL_UNSIGNED_BYTE, internalformat);
	case 'A008':
		return gl_format(GL_ALPHA, GL_UNSIGNED_BYTE, internalformat);
	case 'BGRA':
		*internalformat = GL_RGBA8;
		return PIPE_FORMAT_B8G8R8A8_UNORM;
	case 'RGBA':
		*internalformat = GL_RGBA8;
		return PIPE_FORMAT_R8G8B8A8_UNORM;
	case 'RGhA':
		*internalformat = GL_RGBA16F;
		return PIPE_FORMAT_R16G16B16A16_FLOAT;
	case 'L008':
		*internalformat = GL_R8;
		return PIPE_FORMAT_R8_UNORM;
	case 'L016':
		*internalformat = GL_RG8;
		return PIPE_FORMAT_R8G8_UNORM;
	default:
		return PIPE_FORMAT_NONE;
	}
}

/* fourcc as 4 printable characters ('.' for the rest) */
#define FCC_C(c) ((c) >= 0x20 && (c) < 0x7f ? (c) : '.')
#define FCC(f) FCC_C((f) >> 24), FCC_C(((f) >> 16) & 0xff), FCC_C(((f) >> 8) & 0xff), FCC_C((f) & 0xff)

/*
 * One Mesa resource per surface (and format): CA renders into a surface
 * through one texture (attachImage) and samples it through another
 * (texImageIOSurface); as separate resources Mesa could run the reading
 * batch before the writing one (App Library pods, CC tiles, vibrant glyphs).
 * ponytail: 32 entries, least recently used dropped; a dropped entry keeps
 * nothing alive, a freed surface's pages stay imported until then.
 */
struct surf_res {
	uint32_t id, w, h, pitch;
	enum pipe_format format;
	uintptr_t base;
	struct pipe_resource *res;
	uint64_t used;
};
#define SURF_RES_MAX 32
static struct surf_res surf_res[SURF_RES_MAX];
static uint64_t surf_res_clock;

static bool
surf_res_same(const struct surf_res *a, const struct surf_res *b)
{
	return a->id == b->id && a->format == b->format && a->w == b->w && a->h == b->h && a->pitch == b->pitch &&
	       a->base == b->base;
}

/* a new reference to the cached resource, or NULL */
static struct pipe_resource *
surf_res_get(const struct surf_res *key)
{
	for (int i = 0; i < SURF_RES_MAX; i++) {
		if (surf_res[i].res != NULL && surf_res_same(&surf_res[i], key)) {
			struct pipe_resource *res = NULL;
			surf_res[i].used = ++surf_res_clock;
			pipe_resource_reference(&res, surf_res[i].res);
			return res;
		}
	}
	return NULL;
}

static void
surf_res_put(const struct surf_res *key, struct pipe_resource *res)
{
	if (res == NULL) {
		return;
	}
	int victim = 0;
	for (int i = 1; i < SURF_RES_MAX; i++) {
		if (surf_res[i].used < surf_res[victim].used) {
			victim = i;
		}
	}
	pipe_resource_reference(&surf_res[victim].res, NULL);
	surf_res[victim] = *key;
	surf_res[victim].used = ++surf_res_clock;
	pipe_resource_reference(&surf_res[victim].res, res);
}

/* texture swizzle of the bound texture: identity, or A,R,G,B bytes read as RGBA */
static uint8_t tex_argb[TEXSZ_MAX];

static void
set_swizzle(GLenum target, bool argb)
{
	GLuint tex = unit_held[active_unit()];
	if (tex >= TEXSZ_MAX || tex_argb[tex] == argb) {
		return;
	}
	tex_argb[tex] = argb;
	glTexParameteri(target, GL_TEXTURE_SWIZZLE_R, argb ? GL_GREEN : GL_RED);
	glTexParameteri(target, GL_TEXTURE_SWIZZLE_G, argb ? GL_BLUE : GL_GREEN);
	glTexParameteri(target, GL_TEXTURE_SWIZZLE_B, argb ? GL_ALPHA : GL_BLUE);
	glTexParameteri(target, GL_TEXTURE_SWIZZLE_A, argb ? GL_RED : GL_ALPHA);
}

/* GL upload format/type of a surface CA gave none for (as surface_format) */
static void
upload_format(uint32_t fourcc, size_t bpe, GLenum *format, GLenum *type)
{
	*type = GL_UNSIGNED_BYTE;
	switch (fourcc) {
	case 0:
		*format = bpe == 4 ? GL_BGRA_EXT : bpe == 8 ? GL_RGBA : bpe == 1 ? GL_RED : 0;
		*type = bpe == 8 ? GL_HALF_FLOAT : GL_UNSIGNED_BYTE;
		break;
	case 'LA08': *format = GL_LUMINANCE_ALPHA; break;
	case 'A008': *format = GL_ALPHA; break;
	case 'BGRA': *format = GL_BGRA_EXT; break;
	case 'RGBA': *format = GL_RGBA; break;
	case 'RGhA': *format = GL_RGBA; *type = GL_HALF_FLOAT; break;
	case 'L008': *format = GL_RED; break;
	case 'L016': *format = GL_RG; break;
	default: *format = 0;
	}
}

/*
 * Rows the GPU cannot sample in place (A5xx pitch is 64-byte aligned; CA's
 * own bitmaps are not): copied into the texture instead.
 * ponytail: a snapshot, later CPU writes to the surface are not seen (CA's
 * images are immutable).
 */
static bool
copy_surface(IOSurfaceRef s, GLenum target, GLenum format, GLenum type, size_t w, size_t h, size_t pitch,
    const char *why)
{
	size_t bpe = IOSurfaceGetBytesPerElement(s);
	const void *base = IOSurfaceGetBaseAddress(s);
	if (base == NULL || bpe == 0 || pitch % bpe != 0) {
		sweet_drm_log("sweetgl: %s: surface %u %zux%zu pitch %zu: cannot copy", why, IOSurfaceGetID(s), w, h, pitch);
		return false;
	}
	GLint row = 0, align = 4, skip_rows = 0, skip_pixels = 0;
	uint32_t seed = IOSurfaceGetSeed(s);
	glGetIntegerv(GL_UNPACK_ROW_LENGTH, &row);
	glGetIntegerv(GL_UNPACK_ALIGNMENT, &align);
	glGetIntegerv(GL_UNPACK_SKIP_ROWS, &skip_rows);
	glGetIntegerv(GL_UNPACK_SKIP_PIXELS, &skip_pixels);
	glPixelStorei(GL_UNPACK_ROW_LENGTH, (GLint)(pitch / bpe));
	glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
	glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);
	glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
	GLint ifmt = format == GL_RED ? GL_R8 : format == GL_RG ? GL_RG8 :
	    type == GL_HALF_FLOAT ? GL_RGBA16F : (GLint)format;
	glTexImage2D(target, 0, ifmt, (GLsizei)w, (GLsizei)h, 0, format, type, base);
	GLenum e = glGetError();
	glPixelStorei(GL_UNPACK_ROW_LENGTH, row);
	glPixelStorei(GL_UNPACK_ALIGNMENT, align);
	glPixelStorei(GL_UNPACK_SKIP_ROWS, skip_rows);
	glPixelStorei(GL_UNPACK_SKIP_PIXELS, skip_pixels);
	tex_size_bound(target, 0, (GLsizei)w, (GLsizei)h);
	GLuint tex = unit_held[active_unit()];
	if (tex < TEXSZ_MAX) {
		struct copied *c = &copies[tex];
		if (c->surface != s) {
			CFRetain(s);
			copy_forget(tex);
			c->surface = s;
		}
		c->seed = seed;
		c->format = format;
		c->type = type;
	}
	size_t lit = 0, any = 0;
	const uint8_t *b8 = base, *mid = b8 + (h / 2) * pitch;
	for (size_t i = 0; trace_left > 0 && i < pitch * h; i++) {
		lit += i % bpe == bpe - 1 && b8[i] != 0;
		any += b8[i] != 0;
	}
	TRACE("%s surf %u tex %u %zux%zu pitch %zu fmt %#x/%#x copied (seed %u, %zu of %zu last bytes set, %zu bytes set, "
	    "mid row %02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x), err %#x",
	    why, IOSurfaceGetID(s), tex, w, h, pitch, format, type, seed, lit, pitch * h / bpe, any,
	    mid[0], mid[1], mid[2], mid[3], mid[4], mid[5], mid[6], mid[7], mid[8], mid[9], mid[10], mid[11],
	    mid[12], mid[13], mid[14], mid[15], e);
	if (e != GL_NO_ERROR) {
		sweet_drm_log("sweetgl: %s: surface %u %zux%zu pitch %zu fmt %#x/%#x: copy error %#x", why,
		    IOSurfaceGetID(s), w, h, pitch, format, type, e);
	}
	return e == GL_NO_ERROR;
}

/* The surface's pages as the storage of the texture bound to target. */
static bool
bind_surface(IOSurfaceRef s, GLenum target, GLenum format, GLenum type, uint32_t plane, const char *why)
{
	static int logged;
	uint32_t fourcc = IOSurfaceGetPixelFormat(s);
	size_t w = IOSurfaceGetWidthOfPlane(s, plane), h = IOSurfaceGetHeightOfPlane(s, plane);
	size_t pitch = IOSurfaceGetBytesPerRowOfPlane(s, plane);
	struct sgl_image img = {0};
	bool multi = IOSurfaceGetPlaneCount(s) > 1 || plane != 0;   /* ponytail: YUV surfaces not yet */
	img.format = format && !multi ? gl_format(format, type, &img.internalformat) : PIPE_FORMAT_NONE;
	if (img.format == PIPE_FORMAT_NONE && !multi) {
		img.format = surface_format(fourcc, IOSurfaceGetBytesPerElementOfPlane(s, plane), &img.internalformat);
	}
	/*
	 * CA's own 32-bit images without an IOSurface format (attached, no GL
	 * format) are A,R,G,B in memory and sampled as RGBA by its shaders.
	 */
	bool argb = format == 0 && fourcc == 0 && !multi && IOSurfaceGetBytesPerElement(s) == 4;
	if (argb) {
		format = GL_RGBA;
		type = GL_UNSIGNED_BYTE;
		img.format = gl_format(format, type, &img.internalformat);
	}
	set_swizzle(target, argb);
	if (!multi && pitch % 64 != 0) {
		if (format == 0) {
			upload_format(fourcc, IOSurfaceGetBytesPerElement(s), &format, &type);
		}
		if (format != 0) {
			return copy_surface(s, target, format, type, w, h, pitch, why);
		}
	}
	uint32_t handle = 0;
	uint64_t size = 0;
	const char *step = img.format == PIPE_FORMAT_NONE ? "format" : "kernel import";
	struct surf_res key = {
		.id = IOSurfaceGetID(s),
		.format = img.format,
		.w = (uint32_t)w,
		.h = (uint32_t)h,
		.pitch = (uint32_t)pitch,
		.base = (uintptr_t)IOSurfaceGetBaseAddressOfPlane(s, plane),
	};
	img.res = img.format != PIPE_FORMAT_NONE ? surf_res_get(&key) : NULL;
	if (img.res == NULL && img.format != PIPE_FORMAT_NONE &&
	    sweet_drm_import_iosurface(IOSurfaceGetID(s), (uint64_t)(uintptr_t)IOSurfaceGetBaseAddress(s),
	    &handle, &size) == 0) {
		struct pipe_screen *screen = sweetgl_screen();
		struct pipe_resource templ = {
			.target = PIPE_TEXTURE_2D,
			.format = img.format,
			.width0 = (uint32_t)w,
			.height0 = (uint16_t)h,
			.depth0 = 1,
			.array_size = 1,
			.bind = PIPE_BIND_SAMPLER_VIEW | PIPE_BIND_RENDER_TARGET | PIPE_BIND_SHARED,
		};
		struct winsys_handle wh = {
			.type = WINSYS_HANDLE_TYPE_KMS,
			.handle = handle,
			.stride = (unsigned)pitch,
			/* client-memory surfaces start inside their first page (kernel 0157) */
			.offset = (unsigned)((uintptr_t)IOSurfaceGetBaseAddressOfPlane(s, plane) & vm_page_mask),
			.format = img.format,
			.modifier = DRM_FORMAT_MOD_LINEAR,
		};
		/* takes over the handle: closed with the resource, or now on failure */
		step = "mesa layout";
		img.res = screen->resource_from_handle(screen, &templ, &wh, PIPE_HANDLE_USAGE_FRAMEBUFFER_WRITE);
		surf_res_put(&key, img.res);
	}
	if (img.res == NULL) {
		if (logged++ < 50) {
			sweet_drm_log("sweetgl: %s: surface %u %c%c%c%c %zux%zu pitch %zu plane %u: %s failed",
			    why, IOSurfaceGetID(s), FCC(fourcc), w, h, pitch, plane, step);
		}
		return false;
	}
	copy_forget(unit_held[active_unit()]);
	_mesa_EGLImageTargetTexture2DOES(target, (GLeglImageOES)&img);
	GLenum e = glGetError();
	tex_size_bound(target, 0, (GLsizei)w, (GLsizei)h);
	pipe_resource_reference(&img.res, NULL);
	if (trace_left > 0) {
		const uint8_t *base = IOSurfaceGetBaseAddress(s);
		const uint8_t *c = base ? base + (h / 2) * pitch + (w / 2) * IOSurfaceGetBytesPerElement(s) : NULL;
		GLint tex = 0;
		glGetIntegerv(GL_TEXTURE_BINDING_2D, &tex);
		TRACE("%s surf %u %c%c%c%c %zux%zu pitch %zu fmt %#x/%#x -> pipe %d tex %d, center %02x%02x%02x%02x, err %#x",
		    why, IOSurfaceGetID(s), FCC(fourcc),
		    w, h, pitch, format, type, img.format, tex, c ? c[0] : 0, c ? c[1] : 0, c ? c[2] : 0, c ? c[3] : 0, e);
	}
	if (e != GL_NO_ERROR && logged++ < 50) {
		sweet_drm_log("sweetgl: %s: surface %u %zux%zu pitch %zu: EGLImageTargetTexture2D error %#x", why,
		    IOSurfaceGetID(s), w, h, pitch, e);
	}
	return e == GL_NO_ERROR;
}

/* ---- EAGLContext ---- */

static id
m_init(id self, SEL cmd, NSUInteger api, id props)
{
	struct sweetgl_ctx *ctx = api >= 2 && sweetgl_init() ? sweetgl_create((int)api, NULL) : NULL;
	struct sgl_context *c = ctx ? calloc(1, sizeof(*c)) : NULL;
	int slot = -1;
	pthread_mutex_lock(&ctx_lock);
	for (int i = 0; i < MAX_CTX && slot < 0 && c; i++) {
		if (ctxs[i] == NULL) {
			slot = i;
			ctxs[i] = c;
		}
	}
	if (slot >= 0) {
		c->rctx = c;
		memcpy(c->disp, sgl_dispatch, sizeof(c->disp));
		c->eagl = self;
		c->api = (unsigned)api;
		c->ctx = ctx;
	}
	pthread_mutex_unlock(&ctx_lock);
	if (slot < 0) {
		free(c);
		sweetgl_destroy(ctx);
		sweet_drm_log("sweetgl: EAGLContext API %lu: no Mesa context, Apple's EAGL instead", (unsigned long)api);
		return ((id(*)(id, SEL, NSUInteger, id))orig_init)(self, cmd, api, props);
	}
	sweet_drm_log("sweetgl: EAGLContext %p API %lu on Mesa (GLI %p)", self, (unsigned long)api, c);
	return self;
}

static void
m_dealloc(id self, SEL cmd)
{
	struct sgl_context *c = lookup(self);
	if (c == NULL) {
		((void (*)(id, SEL))orig_dealloc)(self, cmd);
		return;
	}
	pthread_mutex_lock(&ctx_lock);
	for (int i = 0; i < MAX_CTX; i++) {
		if (ctxs[i] == c) {
			ctxs[i] = NULL;
		}
	}
	pthread_mutex_unlock(&ctx_lock);
	if (sgl_current == c) {
		sgl_bind(NULL);
	}
	sweetgl_destroy(c->ctx);
	free(c);
	/* EAGLContext's own init never ran: skip its dealloc */
	struct objc_super sup = {self, class_getSuperclass(objc_getClass("EAGLContext"))};
	((void (*)(struct objc_super *, SEL))objc_msgSendSuper)(&sup, cmd);
}

static NSUInteger
m_api(id self, SEL cmd)
{
	struct sgl_context *c = lookup(self);
	return c ? c->api : ((NSUInteger(*)(id, SEL))orig_api)(self, cmd);
}

static BOOL
m_set_param(id self, SEL cmd, uint32_t pname, const int32_t *value)
{
	if (lookup(self) == NULL) {
		return ((BOOL(*)(id, SEL, uint32_t, const int32_t *))orig_set_param)(self, cmd, pname, value);
	}
	sweet_drm_log("sweetgl: setParameter %#x to %d (ignored)", pname, value ? *value : 0);
	return YES;
}

static void *
m_macro(id self, SEL cmd)
{
	struct sgl_context *c = lookup(self);
	return c ? (void *)c : ((void *(*)(id, SEL))orig_macro)(self, cmd);
}

static BOOL
m_tex_iosurface(id self, SEL cmd, IOSurfaceRef s, NSUInteger target, NSUInteger internalformat, uint32_t width,
    uint32_t height, NSUInteger format, NSUInteger type, uint32_t plane, BOOL invert)
{
	struct sgl_context *c = lookup(self);
	if (c == NULL) {
		return ((BOOL(*)(id, SEL, IOSurfaceRef, NSUInteger, NSUInteger, uint32_t, uint32_t, NSUInteger,
		    NSUInteger, uint32_t, BOOL))orig_tex_iosurface)(self, cmd, s, target, internalformat, width,
		    height, format, type, plane, invert);
	}
	SGL_CUR(c);
	return bind_surface(s, SGL_TGT((GLenum)target), (GLenum)format, (GLenum)type, plane, "texImageIOSurface");
}

static BOOL
m_attach(id self, SEL cmd, NSUInteger target, IOSurfaceRef s, BOOL inverted)
{
	struct sgl_context *c = lookup(self);
	if (c == NULL) {
		return ((BOOL(*)(id, SEL, NSUInteger, IOSurfaceRef, BOOL))orig_attach)(self, cmd, target, s, inverted);
	}
	SGL_CUR(c);
	return bind_surface(s, SGL_TGT((GLenum)target), 0, 0, 0, "attachImage");
}

static __thread id current_eagl;

static BOOL
m_set_current(id cls, SEL cmd, id eagl)
{
	struct sgl_context *c = lookup(eagl);
	if (c) {
		sgl_bind(c);
		current_eagl = eagl;
		return YES;
	}
	if (sgl_current) {
		sgl_bind(NULL);
	}
	current_eagl = nil;
	return ((BOOL(*)(id, SEL, id))orig_set_current)(cls, cmd, eagl);
}

static id
m_current(id cls, SEL cmd)
{
	return current_eagl ? current_eagl : ((id(*)(id, SEL))orig_current)(cls, cmd);
}

static IMP
swap(Class cls, const char *sel, void *imp)
{
	Method m = class_getInstanceMethod(cls, sel_registerName(sel));
	if (m == NULL) {
		sweet_drm_log("sweetgl: EAGLContext has no %s", sel);
		return NULL;
	}
	return method_setImplementation(m, (IMP)imp);
}

/* Called at load. CA picks its GLES renderer by itself once initWithAPI:3 succeeds. */
void
sweetgl_eagl_install(void)
{
	const char *e = getenv("SWEETGL_GLERR");
	sgl_check_errors = e && e[0] == '1';
	const char *t = getenv("SWEETGL_TRACE");
	trace_left = t && t[0] == '1' ? 20000 : 0;
	dump_shaders = trace_left > 0;
	Class cls = objc_getClass("EAGLContext");
	if (cls == Nil) {
		sweet_drm_log("sweetgl: no EAGLContext class");
		return;
	}
	Class meta = object_getClass((id)cls);
	orig_init = swap(cls, "initWithAPI:properties:", m_init);
	orig_dealloc = swap(cls, "dealloc", m_dealloc);
	orig_api = swap(cls, "API", m_api);
	orig_set_param = swap(cls, "setParameter:to:", m_set_param);
	orig_macro = swap(cls, "getMacroContextPrivate", m_macro);
	orig_tex_iosurface = swap(cls, "texImageIOSurface:target:internalFormat:width:height:format:type:plane:invert:",
	    m_tex_iosurface);
	orig_attach = swap(cls, "attachImage:toCoreSurface:invertedRender:", m_attach);
	orig_set_current = swap(meta, "setCurrentContext:", m_set_current);
	orig_current = swap(meta, "currentContext", m_current);
	sweet_drm_log("sweetgl: EAGL shim installed%s", sgl_check_errors ? " (GL error checks on)" : "");
}
