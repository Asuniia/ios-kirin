/* sweetgl EAGL shim (eagl.c) and the generated GLI dispatch table (out/dispatch.c). */
#ifndef SWEETGL_EAGL_H
#define SWEETGL_EAGL_H

#define GL_GLEXT_PROTOTYPES 1
#include <GL/gl.h>
#include <GL/glext.h>
#include <stdint.h>

#define SGL_SLOTS 1036

/* rctx of the GLI calls: the shim's context; made current on the calling thread if it is not */
extern __thread void *sgl_current;
void sgl_bind(void *rctx);
#define SGL_CUR(r) do { if (__builtin_expect(sgl_current != (r), 0)) sgl_bind(r); } while (0)

/* SWEETGL_GLERR=1: glGetError after every call, logged (eats the error CA would see) */
extern int sgl_check_errors;
void sgl_post(int slot);
#define SGL_POST(i) do { if (__builtin_expect(sgl_check_errors, 0)) sgl_post(i); } while (0)

void sgl_unsupported(int slot, const char *name);

extern void *const sgl_dispatch[SGL_SLOTS];
extern const char *const sgl_slot_names[SGL_SLOTS];

/* hand-written slots (eagl.c) */
void sgl_DeleteObjectARB(void *rctx, GLhandleARB obj);
void sgl_GetObjectParameterivARB(void *rctx, GLhandleARB obj, GLenum pname, GLint *params);
void sgl_GetInfoLogARB(void *rctx, GLhandleARB obj, GLsizei max, GLsizei *length, GLcharARB *log);
void sgl_GetAttachedObjectsARB(void *rctx, GLhandleARB prog, GLsizei max, GLsizei *count, GLhandleARB *obj);
GLhandleARB sgl_GetHandleARB(void *rctx, GLenum pname);
void sgl_DiscardFramebufferEXT(void *rctx, GLenum target, GLsizei n, const GLenum *attachments);
void sgl_GetShaderPrecisionFormatOES(void *rctx, GLenum shader, GLenum precision, GLint *range, GLint *prec);
void sgl_ReleaseShaderCompilerOES(void *rctx);
void sgl_ShaderBinaryOES(void *rctx, GLsizei n, const GLuint *shaders, GLenum format, const void *binary,
    GLsizei length);
void sgl_GetIntegerIndexedv(void *rctx, GLenum pname, GLuint index, GLint *data);
void sgl_TexImage2D(void *rctx, GLenum target, GLint level, GLint internalformat, GLsizei width,
    GLsizei height, GLint border, GLenum format, GLenum type, const void *pixels);
void sgl_FinishObjectAPPLE(void *rctx, GLenum object, GLint name);
GLboolean sgl_TestObjectAPPLE(void *rctx, GLenum object, GLuint name);
void sgl_Flush(void *rctx);
const GLubyte *sgl_GetString(void *rctx, GLenum name);
void sgl_CompileShaderARB(void *rctx, GLhandleARB shader);
void sgl_LinkProgramARB(void *rctx, GLhandleARB program);

#endif
