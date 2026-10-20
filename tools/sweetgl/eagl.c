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
	if (level != 0 || SGL_TGT(target) != GL_TEXTURE_2D) {
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

static void
rect_fixup(void)
{
	if (n_rect_programs == 0) {
		return;
	}
	GLint prog = 0;
	glGetIntegerv(GL_CURRENT_PROGRAM, &prog);
	for (int i = 0; i < n_rect_programs; i++) {
		if (rect_programs[i].program != (GLuint)prog || rect_programs[i].scale < 0) {
			continue;
		}
		GLint unit = 0, active = GL_TEXTURE0, w = 0, h = 0;
		if (rect_programs[i].sampler >= 0) {
			glGetUniformiv(prog, rect_programs[i].sampler, &unit);
		}
		GLint tex = 0;
		glGetIntegerv(GL_ACTIVE_TEXTURE, &active);
		glActiveTexture(GL_TEXTURE0 + unit);
		glGetIntegerv(GL_TEXTURE_BINDING_2D, &tex);
		glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &w);
		glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &h);
		GLenum qe = glGetError();
		glActiveTexture(active);
		if ((w <= 0 || h <= 0) && tex > 0 && tex < TEXSZ_MAX) {
			w = tex_w[tex];
			h = tex_h[tex];
		}
		glUniform2f(rect_programs[i].scale, w > 0 ? 1.0f / w : 1.0f, h > 0 ? 1.0f / h : 1.0f);
		TRACE("rect program %d unit %d tex %d %dx%d (query error %#x)", prog, unit, tex, w, h, qe);
		return;
	}
}

void
sgl_BindTexture(void *rctx, GLenum target, GLuint texture)
{
	SGL_CUR(rctx);
	if (trace_left > 0) {
		GLint active = 0;
		glGetIntegerv(GL_ACTIVE_TEXTURE, &active);
		if (target != GL_TEXTURE_2D || active != GL_TEXTURE0) {
			TRACE("BindTexture %#x %u on unit %d", target, texture, active - GL_TEXTURE0);
		}
	}
	glBindTexture(SGL_TGT(target), texture);
	SGL_POST(5);
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
	rect_fixup();
	glDrawArrays(mode, first, count);
	SGL_POST(65);
}

void
sgl_DrawElements(void *rctx, GLenum mode, GLsizei count, GLenum type, const void *indices)
{
	SGL_CUR(rctx);
	rect_fixup();
	glDrawElements(mode, count, type, indices);
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
	if (!ok) {
		static char log[1024];
		glGetProgramInfoLog(H(program), sizeof(log), NULL, log);
		sweet_drm_log("sweetgl: program %u link failed: %.400s", H(program), log);
	}
}

/* Apple's ES takes internalformat GL_RGBA with format GL_BGRA; Mesa wants GL_BGRA for both */
void
sgl_TexImage2D(void *rctx, GLenum target, GLint level, GLint internalformat, GLsizei width,
    GLsizei height, GLint border, GLenum format, GLenum type, const void *pixels)
{
	SGL_CUR(rctx);
	target = SGL_TGT(target);
	TRACE("TexImage2D target %#x level %d ifmt %#x %dx%d fmt %#x type %#x pixels %p", target, level,
	    internalformat, width, height, format, type, pixels);
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
	uint32_t handle = 0;
	uint64_t size = 0;
	const char *step = img.format == PIPE_FORMAT_NONE ? "format" : "kernel import";
	if (img.format != PIPE_FORMAT_NONE &&
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
	}
	if (img.res == NULL) {
		if (logged++ < 50) {
			sweet_drm_log("sweetgl: %s: surface %u %c%c%c%c %zux%zu pitch %zu plane %u: %s failed",
			    why, IOSurfaceGetID(s), FCC(fourcc), w, h, pitch, plane, step);
		}
		return false;
	}
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
	trace_left = t && t[0] == '1' ? 600 : 0;
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
