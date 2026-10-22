/*
 * sweetgl: Mesa (gallium freedreno + GLES) as a library for iOS processes.
 *
 * A minimal gallium frontend after Mesa's Haiku one (frontends/hgl): one
 * pipe_screen over the msm emulation in sweet_drm.c, GLES contexts made
 * current without a drawable (surfaceless: CoreAnimation draws into FBOs
 * only). IOSurfaces will come in as EGL-style images (get_egl_image).
 *
 * SWEETGL_TEST=1 in the environment runs sweetgl_selftest() on a thread at
 * load: an offscreen triangle, read back and checked, result in the kernel
 * log as "sweetgl: ...".
 */
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <mach/mach_time.h>

#include "pipe/p_screen.h"
#include "pipe/p_context.h"
#include "frontend/api.h"
#include "state_tracker/st_context.h"
#include "util/driconf.h"
#include "util/xmlconfig.h"
#include "freedreno/drm/freedreno_drm_public.h"
#include "renderonly/renderonly.h"
#include <GLES3/gl3.h>

#include "sweet_drm.h"
#include "sweetgl.h"

static const driOptionDescription sweetgl_driconf[] = {
#include "pipe-loader/driinfo_gallium.h"
#include "freedreno/driinfo_freedreno.h"
};

/* freedreno calls these only with a renderonly device (KMS scanout); we pass none */
void
renderonly_scanout_destroy(struct renderonly_scanout *scanout, struct renderonly *ro)
{
	(void)scanout;
	(void)ro;
}

struct renderonly_scanout *
renderonly_create_gpu_import_for_resource(struct pipe_resource *rsc, struct renderonly *ro,
    struct winsys_handle *out_handle)
{
	(void)rsc;
	(void)ro;
	(void)out_handle;
	return NULL;
}

struct sweetgl_ctx {
	struct st_context *st;
	int api;
};

static pthread_mutex_t init_lock = PTHREAD_MUTEX_INITIALIZER;
static struct pipe_frontend_screen *fscreen;

/* st_api_create_context calls it unconditionally (ST_MANAGER_BROKEN_INVALIDATE): no. */
static int
sweetgl_get_param(struct pipe_frontend_screen *fs, enum st_manager_param param)
{
	(void)fs;
	(void)param;
	return 0;
}
static driOptionCache opt_info, opts;

void
sweet_drm_log(const char *fmt, ...)
{
	static int fd = -1;
	if (fd < 0) {
		fd = open("/dev/null", O_WRONLY | O_CLOEXEC);
	}
	char line[512];
	va_list ap;
	va_start(ap, fmt);
	int n = vsnprintf(line, sizeof(line) - 1, fmt, ap);
	va_end(ap);
	if (n < 0) {
		return;
	}
	if (n > (int)sizeof(line) - 2) {
		n = sizeof(line) - 2;
	}
	line[n++] = '\n';
	(void)write(fd, line, n);
}

bool
sweetgl_init(void)
{
	pthread_mutex_lock(&init_lock);
	if (fscreen == NULL) {
		int fd = sweet_drm_open();
		if (fd >= 0) {
			/*
			 * Render straight to memory, no GMEM tiles: tile restore/resolve
			 * covers whole 32-row blocks past the end of IOSurfaces that were
			 * not padded for it (GPU fault on the display surface, boot 0154a).
			 */
			setenv("FD_MESA_DEBUG", "sysmem,nofp16", 0);
			/*
			 * SWEETGL_FD=flag+flag+...: replaces FD_MESA_DEBUG, '+' standing for
			 * ',' since boot-args (sweetenv) cannot carry commas or spaces.
			 */
			const char *fdd = getenv("SWEETGL_FD");
			if (fdd != NULL && *fdd) {
				char buf[256];
				strlcpy(buf, fdd, sizeof(buf));
				for (char *c = buf; *c; c++) {
					if (*c == '+') {
						*c = ',';
					}
				}
				setenv("FD_MESA_DEBUG", buf, 1);
				sweet_drm_log("sweetgl: FD_MESA_DEBUG=%s", buf);
			}
			/*
			 * nofp16: CA's shaders count on Apple's mediump/lowp being enough for
			 * texture coordinates and corner shapes; fp16 is not (App
			 * Library pod corners/backdrops, the wallpaper "+").
			 */
			setenv("IR3_SHADER_DEBUG", "nofp16", 0);
			/* fd_screen_create fills opts from opt_info (no xmlconfig: defaults) */
			driParseOptionInfo(&opt_info, sweetgl_driconf, ARRAY_SIZE(sweetgl_driconf));
			struct pipe_screen_config cfg = {.options = &opts, .options_info = &opt_info};
			struct pipe_screen *screen = fd_drm_screen_create_renderonly(fd, NULL, &cfg);
			if (screen == NULL) {
				sweet_drm_log("sweetgl: freedreno screen creation failed");
				close(fd);
			} else {
				fscreen = calloc(1, sizeof(*fscreen));
				fscreen->screen = screen;
				fscreen->get_param = sweetgl_get_param;
				sweetgl_image_hooks(fscreen);  /* before any context: st checks them at creation */
				sweet_drm_log("sweetgl: screen %s (%s)", screen->get_name(screen),
				    screen->get_vendor(screen));
			}
		}
	}
	bool ok = fscreen != NULL;
	pthread_mutex_unlock(&init_lock);
	return ok;
}

struct pipe_screen *
sweetgl_screen(void)
{
	return fscreen ? fscreen->screen : NULL;
}

struct sweetgl_ctx *
sweetgl_create(int api, struct sweetgl_ctx *share)
{
	if (!sweetgl_init()) {
		return NULL;
	}
	struct st_context_attribs attribs = {
		.profile = API_OPENGLES2,
		.major = api >= 3 ? 3 : 2,
		.minor = api >= 3 ? 1 : 0,      /* 3.1: glGetTexLevelParameteriv (eagl.c rect textures) */
	};
	enum st_context_error err = ST_CONTEXT_SUCCESS;
	struct st_context *st = st_api_create_context(fscreen, &attribs, &err, share ? share->st : NULL);
	if (st == NULL) {
		sweet_drm_log("sweetgl: GLES %d context failed: %d", attribs.major, err);
		return NULL;
	}
	struct sweetgl_ctx *c = calloc(1, sizeof(*c));
	c->st = st;
	c->api = attribs.major;
	st->frontend_context = c;
	return c;
}

bool
sweetgl_make_current(struct sweetgl_ctx *c)
{
	return st_api_make_current(c ? c->st : NULL, NULL, NULL);
}

void
sweetgl_destroy(struct sweetgl_ctx *c)
{
	if (c == NULL) {
		return;
	}
	if (st_api_get_current() == c->st) {
		st_api_make_current(NULL, NULL, NULL);
	}
	st_destroy_context(c->st);
	free(c);
}

static double
ms_since(uint64_t t0)
{
	mach_timebase_info_data_t tb;
	mach_timebase_info(&tb);
	return (double)(mach_absolute_time() - t0) * tb.numer / tb.denom / 1e6;
}

static GLuint
shader(GLenum type, const char *src)
{
	GLuint s = glCreateShader(type);
	glShaderSource(s, 1, &src, NULL);
	glCompileShader(s);
	GLint ok = 0;
	glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
	if (!ok) {
		char log[256] = "";
		glGetShaderInfoLog(s, sizeof(log), NULL, log);
		sweet_drm_log("sweetgl: shader compile failed: %s", log);
	}
	return s;
}

/*
 * 256x256 RGBA8 FBO: clear to blue, a red triangle over the lower-left half
 * (vertices (-1,-1) (1,-1) (-1,1)), read back: pixel (32, 32) must be red,
 * (224, 224) blue. Returns true if both are.
 */
/*
 * Where do pixels land? A 256x256 pattern whose pixel (x, y) is
 * (R, G, B) = (x, y, 7x + 13y): every pixel read back names the pixel it
 * came from. Three paths, each read back with glReadPixels:
 *   C  rendered from gl_FragCoord (render target write + readback)
 *   A  uploaded with glTexImage2D, read through an FBO (upload + readback)
 *   B  A's texture drawn with texelFetch into the first FBO (sampling)
 */
static void
pattern_check(const char *what, const uint8_t *px)
{
	unsigned bad = 0;
	for (unsigned y = 0; y < 256; y++) {
		for (unsigned x = 0; x < 256; x++) {
			const uint8_t *q = &px[(y * 256 + x) * 4];
			bad += q[0] != x || q[1] != y || q[2] != (uint8_t)(7 * x + 13 * y);
		}
	}
	static const uint8_t pt[][2] = {{0, 0}, {1, 0}, {2, 0}, {4, 0}, {8, 0}, {16, 0}, {0, 1}, {0, 2},
	    {0, 4}, {0, 8}, {5, 3}, {31, 7}, {64, 0}, {0, 64}, {100, 37}, {255, 255}};
	char line[600];
	int m = snprintf(line, sizeof(line), "sweetgl: pattern %s: %u/65536 wrong;", what, bad);
	for (unsigned i = 0; i < sizeof(pt) / sizeof(pt[0]) && m < (int)sizeof(line) - 24; i++) {
		const uint8_t *q = &px[(pt[i][1] * 256 + pt[i][0]) * 4];
		m += snprintf(line + m, sizeof(line) - m, " %u,%u<-%u,%u", pt[i][0], pt[i][1], q[0], q[1]);
	}
	sweet_drm_log("%s", line);
}

static GLuint shader(GLenum type, const char *src);

static void
pattern_tests(GLuint fbo, GLuint tex)
{
	static uint8_t pat[256 * 256 * 4], px[256 * 256 * 4];
	for (unsigned y = 0; y < 256; y++) {
		for (unsigned x = 0; x < 256; x++) {
			uint8_t *q = &pat[(y * 256 + x) * 4];
			q[0] = (uint8_t)x; q[1] = (uint8_t)y; q[2] = (uint8_t)(7 * x + 13 * y); q[3] = 255;
		}
	}
	static const float quad[] = {-1, -1, 1, -1, -1, 1, -1, 1, 1, -1, 1, 1};
	glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
	glBindFramebuffer(GL_FRAMEBUFFER, fbo);
	glViewport(0, 0, 256, 256);

	GLuint pc = glCreateProgram();
	glAttachShader(pc, shader(GL_VERTEX_SHADER,
	    "#version 300 es\nlayout(location = 0) in vec2 p;\nvoid main() { gl_Position = vec4(p, 0.0, 1.0); }\n"));
	glAttachShader(pc, shader(GL_FRAGMENT_SHADER,
	    "#version 300 es\nprecision highp float;\nout vec4 c;\nvoid main() {\n"
	    "  uvec2 q = uvec2(gl_FragCoord.xy);\n"
	    "  c = vec4(float(q.x), float(q.y), float((7u * q.x + 13u * q.y) & 255u), 255.0) / 255.0;\n}\n"));
	glLinkProgram(pc);
	glUseProgram(pc);
	glDrawArrays(GL_TRIANGLES, 0, 6);
	glReadPixels(0, 0, 256, 256, GL_RGBA, GL_UNSIGNED_BYTE, px);
	pattern_check("C render", px);

	GLuint t2, f2;
	glGenTextures(1, &t2);
	glBindTexture(GL_TEXTURE_2D, t2);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 256, 256, 0, GL_RGBA, GL_UNSIGNED_BYTE, pat);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glGenFramebuffers(1, &f2);
	glBindFramebuffer(GL_FRAMEBUFFER, f2);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t2, 0);
	glReadPixels(0, 0, 256, 256, GL_RGBA, GL_UNSIGNED_BYTE, px);
	pattern_check("A upload", px);

	GLuint ps = glCreateProgram();
	glAttachShader(ps, shader(GL_VERTEX_SHADER,
	    "#version 300 es\nlayout(location = 0) in vec2 p;\nvoid main() { gl_Position = vec4(p, 0.0, 1.0); }\n"));
	glAttachShader(ps, shader(GL_FRAGMENT_SHADER,
	    "#version 300 es\nprecision highp float;\nuniform highp sampler2D t;\nout vec4 c;\n"
	    "void main() { c = texelFetch(t, ivec2(gl_FragCoord.xy), 0); }\n"));
	glLinkProgram(ps);
	glUseProgram(ps);
	glBindFramebuffer(GL_FRAMEBUFFER, fbo);
	glBindTexture(GL_TEXTURE_2D, t2);
	glDrawArrays(GL_TRIANGLES, 0, 6);
	glReadPixels(0, 0, 256, 256, GL_RGBA, GL_UNSIGNED_BYTE, px);
	pattern_check("B sample", px);
	(void)tex;
	sweet_drm_log("sweetgl: pattern tests done, GL error %x", glGetError());
}

bool
sweetgl_selftest(void)
{
	uint64_t t0 = mach_absolute_time();
	struct sweetgl_ctx *c = sweetgl_create(3, NULL);
	if (c == NULL || !sweetgl_make_current(c)) {
		sweet_drm_log("sweetgl: selftest: no context");
		return false;
	}
	double t_ctx = ms_since(t0);
	sweet_drm_log("sweetgl: GL_VERSION %s / GL_RENDERER %s / GLSL %s", glGetString(GL_VERSION),
	    glGetString(GL_RENDERER), glGetString(GL_SHADING_LANGUAGE_VERSION));

	GLuint tex, fbo;
	glGenTextures(1, &tex);
	glBindTexture(GL_TEXTURE_2D, tex);
	glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, 256, 256);
	glGenFramebuffers(1, &fbo);
	glBindFramebuffer(GL_FRAMEBUFFER, fbo);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
	GLenum fbs = glCheckFramebufferStatus(GL_FRAMEBUFFER);

	GLuint prog = glCreateProgram();
	glAttachShader(prog, shader(GL_VERTEX_SHADER,
	    "#version 300 es\nlayout(location = 0) in vec2 p;\nvoid main() { gl_Position = vec4(p, 0.0, 1.0); }\n"));
	glAttachShader(prog, shader(GL_FRAGMENT_SHADER,
	    "#version 300 es\nprecision mediump float;\nout vec4 c;\nvoid main() { c = vec4(1.0, 0.0, 0.0, 1.0); }\n"));
	glLinkProgram(prog);
	GLint linked = 0;
	glGetProgramiv(prog, GL_LINK_STATUS, &linked);
	glUseProgram(prog);

	static const float tri[] = {-1, -1, 1, -1, -1, 1};
	GLuint vbo;
	glGenBuffers(1, &vbo);
	glBindBuffer(GL_ARRAY_BUFFER, vbo);
	glBufferData(GL_ARRAY_BUFFER, sizeof(tri), tri, GL_STATIC_DRAW);
	glEnableVertexAttribArray(0);
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, 0);

	uint64_t t1 = mach_absolute_time();
	glViewport(0, 0, 256, 256);
	glClearColor(0, 0, 1, 1);
	glClear(GL_COLOR_BUFFER_BIT);
	glDrawArrays(GL_TRIANGLES, 0, 3);
	static uint8_t px[256 * 256 * 4];
	glReadPixels(0, 0, 256, 256, GL_RGBA, GL_UNSIGNED_BYTE, px);
	double t_draw = ms_since(t1);
	GLenum err = glGetError();

	const uint8_t *a = &px[(32 * 256 + 32) * 4], *b = &px[(224 * 256 + 224) * 4];
	bool ok = fbs == GL_FRAMEBUFFER_COMPLETE && linked && err == GL_NO_ERROR &&
	    a[0] == 255 && a[1] == 0 && a[2] == 0 && a[3] == 255 &&
	    b[0] == 0 && b[1] == 0 && b[2] == 255 && b[3] == 255;
	sweet_drm_log("sweetgl: selftest fbo %x linked %d err %x px(32,32) %02x%02x%02x%02x px(224,224) %02x%02x%02x%02x "
	    "context %.1f ms draw+readback %.2f ms -> %s",
	    fbs, linked, err, a[0], a[1], a[2], a[3], b[0], b[1], b[2], b[3], t_ctx, t_draw,
	    ok ? "MESA GLES ON THE ADRENO WORKS" : "WRONG");

	/* second frame timing, without compile and allocation */
	uint64_t t2 = mach_absolute_time();
	glClear(GL_COLOR_BUFFER_BIT);
	glDrawArrays(GL_TRIANGLES, 0, 3);
	glFinish();
	sweet_drm_log("sweetgl: second frame clear+draw+finish %.2f ms", ms_since(t2));

	pattern_tests(fbo, tex);

	sweetgl_make_current(NULL);
	sweetgl_destroy(c);
	return ok;
}

static void *
selftest_thread(void *arg)
{
	(void)arg;
	/* the GPU service comes up during boot: retry for 2 minutes */
	for (int i = 0; i < 120 && !sweetgl_init(); i++) {
		sleep(1);
	}
	if (!sweetgl_init()) {
		sweet_drm_log("sweetgl: selftest: no GPU after 2 minutes");
		return NULL;
	}
	sweetgl_selftest();
	return NULL;
}

void sweetgl_layerdump_start(void);
void sweetgl_vibrant_fill_install(void);

__attribute__((constructor)) static void
sweetgl_load(void)
{
	if (strcmp(getprogname(), "SpringBoard") == 0) {
		sweetgl_vibrant_fill_install();
		const char *d = getenv("SWEETGL_LAYERDUMP");
		if (d && d[0] == '1') {
			sweetgl_layerdump_start();
		}
		return;
	}
	sweetgl_eagl_install();
	const char *t = getenv("SWEETGL_TEST");
	if (t && t[0] == '1') {
		pthread_t th;
		pthread_create(&th, NULL, selftest_thread, NULL);
		pthread_detach(th);
	}
}
