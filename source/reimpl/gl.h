/*
 * Wrapper for GL version reporting.
 *
 * The engine (glitch, GLES1.1-only) parses glGetString(GL_VERSION) with
 * sscanf and computes version = major*100 + minor. It then requires
 * version <= 199 in CFixedGLDriver<COpenGLESDriver>::doVersionCheck()
 * (0x4e8304: returns (199 >= version)). Our vendorized vitaGL reports
 * "OpenGL ES 2.0 VitaGL" (lib/vitaGL/source/get_info.c), i.e. version 200,
 * so doVersionCheck() returns 0, genericDriverInit() fails,
 * createOpenGLES1Driver() returns NULL, createDeviceEx() returns NULL and
 * Application::PostInit() (0x28ba68: ldr r0,[r0,#16]) dereferences NULL.
 * Log signature: "OpenGL|ES driver version is 1.1 or better." followed by
 * "Could not create OpenGL|ES 1.1 driver." + data abort at 0x28ba68 (R0=0).
 *
 * Fix: report "OpenGL ES 1.1 VitaGL" so version parses as 101, which passes
 * both the "> 100" gate and the "<= 199" check. All other names fall through
 * to the real vitaGL implementation.
 */

#ifndef SOLOADER_GL_H
#define SOLOADER_GL_H

#include <vitaGL.h>

#ifdef __cplusplus
extern "C" {
#endif

const GLubyte *glGetString_soloader(GLenum name);

// TEMP triage wrappers (see gl.c) -- remove with the logging there.
void glTexImage2D_soloader(GLenum target, GLint level, GLint internalformat,
                           GLsizei width, GLsizei height, GLint border,
                           GLenum format, GLenum type, const GLvoid *pixels);
void *memcpy_soloader(void *dst, const void *src, unsigned int len);

// TEMP triage (black screen with live loop, 2026-09-06): draw/clear/viewport/
// FBO counters + per-frame glGetError drain, summarized by gl_frame_tick().
// Signatures match the real entry points 1:1 so dynlib.c can repoint them.
void glDrawArrays_soloader(GLenum mode, GLint first, GLsizei count);
void glDrawElements_soloader(GLenum mode, GLsizei count, GLenum type,
                             const GLvoid *indices);
void glClear_soloader(GLbitfield mask);
void glViewport_soloader(GLint x, GLint y, GLsizei w, GLsizei h);
void glBindFramebufferOES_soloader(GLenum target, GLuint fb);
GLenum glCheckFramebufferStatusOES_soloader(GLenum target);
void gl_frame_tick(void);
void gl_get_counters(unsigned *draws, unsigned *clears);
void glCompressedTexImage2D_soloader(GLenum target, GLint level,
                                     GLenum internalformat, GLsizei width,
                                     GLsizei height, GLint border,
                                     GLsizei imageSize, const GLvoid *data);
void glTexSubImage2D_soloader(GLenum target, GLint level, GLint xoffset,
                              GLint yoffset, GLsizei width, GLsizei height,
                              GLenum format, GLenum type, const GLvoid *pixels);
void glCopyTexSubImage2D_soloader(GLenum target, GLint level, GLint xoffset,
                                  GLint yoffset, GLint x, GLint y,
                                  GLsizei width, GLsizei height);
void glCompressedTexSubImage2D_drop(GLenum target, GLint level, GLint xoffset,
                                    GLint yoffset, GLsizei width,
                                    GLsizei height, GLenum format,
                                    GLsizei imageSize, const GLvoid *data);

#ifdef __cplusplus
};
#endif

#endif // SOLOADER_GL_H
