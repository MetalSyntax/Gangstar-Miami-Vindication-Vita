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
// Fase 24: clamp al panel real (leccion de Asphalt-5-Vita: un rect fuera de
// rango para GXM = GPU crash). El motor usa 960x480 sobre panel 960x544.
void glScissor_soloader(GLint x, GLint y, GLsizei w, GLsizei h);
void glBindFramebufferOES_soloader(GLenum target, GLuint fb);
GLenum glCheckFramebufferStatusOES_soloader(GLenum target);
void gl_frame_tick(void);
// TEMP triage (2026-09-10): draws our own known-good bars right before the
// swap -- see the long comment in gl.c for how to read the resulting BMP.
void gl_probe_selftest(void);
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
// Fase 20 (pantalla negra con draws>0, BMP 100% negro): el .so importa estos
// 4 entry points y dynlib.c los mandaba a ret0. vitaGL no los implementa,
// asi que se reenvian al equivalente real mas cercano, sin logging (algunos
// van por vertice). Firmas 1:1 para re-apuntar en dynlib.c.
void glLightf_soloader(GLenum light, GLenum pname, GLfloat param);
void glLightModelf_soloader(GLenum pname, GLfloat param);
void glLightx_soloader(GLenum light, GLenum pname, GLfixed param);
void glMultiTexCoord4f_soloader(GLenum target, GLfloat s, GLfloat t,
                                GLfloat r, GLfloat q);
// Fase 22 (pantalla negra con draws>0 y lastErr limpio): el motor es GLES 1.1
// de la era fixed-point (usa glOrthox/glTexParameterx) y vitaGL podria comer
// arrays GL_FIXED sin quejarse. Estos wrappers solo anotan size/type/stride
// cuando CAMBIAN (llamadas de setup, no por vertice: costo cero en steady
// state) para decidir si la geometria llega en fixed o float. Igual para el
// clear color: dice de que color pinta el fondo cada pantalla.
void glVertexPointer_soloader(GLint size, GLenum type, GLsizei stride,
                              const GLvoid *pointer);
void glColorPointer_soloader(GLint size, GLenum type, GLsizei stride,
                             const GLvoid *pointer);
void glTexCoordPointer_soloader(GLint size, GLenum type, GLsizei stride,
                                const GLvoid *pointer);
void glNormalPointer_soloader(GLenum type, GLsizei stride,
                              const GLvoid *pointer);
void glClearColor_soloader(GLclampf r, GLclampf g, GLclampf b, GLclampf a);

#ifdef __cplusplus
};
#endif

#endif // SOLOADER_GL_H
