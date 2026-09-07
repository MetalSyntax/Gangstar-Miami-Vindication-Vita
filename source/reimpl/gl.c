/*
 * See gl.h for why this wrapper exists (vitaGL reports ES 2.0, the engine
 * only accepts <= 1.99).
 */

#include "reimpl/gl.h"

#include "utils/logger.h"

#include <psp2/kernel/clib.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>

#include <stdarg.h>
#include <string.h>

// TEMP triage (black screen + sticky GL_INVALID_ENUM, 2026-09-06): vitaGL is
// built with LOG_ERRORS (CMakeLists.txt), so every internal GL error lands
// here as "file:line: func set GL_INVALID_ENUM (param: 0xVALUE)". Forwarded
// to the session log with repeat-dedup: a sticky per-frame error would
// otherwise print 30-60 lines/s and re-create the Fase-8 logging slowdown.
void vgl_log_capture(const char *fmt, ...) {
    char buf[384];
    va_list ap;
    va_start(ap, fmt);
    sceClibVsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    // Our logger adds its own newline; vitaGL's lines already end with one.
    size_t n = strlen(buf);
    if (n > 0 && buf[n - 1] == '\n')
        buf[n - 1] = '\0';

    static char last[384];
    static unsigned repeats;
    if (strcmp(buf, last) == 0) {
        repeats++;
        if (repeats % 300 == 0)
            l_note("[vitaGL] same error x%u (latest: %s)", repeats + 1, buf);
        return;
    }
    if (repeats > 0)
        l_note("[vitaGL] previous error repeated x%u total", repeats + 1);
    strncpy(last, buf, sizeof(last) - 1);
    last[sizeof(last) - 1] = '\0';
    repeats = 0;
    l_note("[vitaGL] %s", buf);
}

const GLubyte *glGetString_soloader(GLenum name) {
    if (name == GL_VERSION) {
        l_debug("glGetString(GL_VERSION): spoofed as OpenGL ES 1.1 (vitaGL reports 2.0)");
        return (const GLubyte *)"OpenGL ES 1.1 VitaGL";
    }
    const GLubyte *res = glGetString(name);
    l_debug("glGetString(0x%x): \"%s\"", name, res ? (const char *)res : "(null)");
    return res;
}

// TEMP triage (black screen with a LIVE loop, 2026-09-06): per-frame GL stats.
//
// logs/debug_local_022.log shows the engine past asset loading and running at
// full speed (frames to 1668, ~14 ms renders, key input received, sounds
// requested) with the screen still black. The next question is whether the
// engine submits ANY geometry, and whether GL is in an error state: draws==0
// means it never submits (logic/waiting problem), draws>0 + black means GL
// state (viewport/FBO/textures), and a sticky glGetError explains black on
// its own. One l_note() every 5 s from gl_frame_tick() (called by gl_swap()),
// so zero steady-state cost beyond a handful of counter increments --
// same discipline as the [023] mutex probe (Fase 8).
static unsigned gl_stat_frames;
static unsigned gl_stat_draws;   // glDrawArrays + glDrawElements
static unsigned gl_stat_clears;
static unsigned gl_stat_texup;   // glTexImage2D uploads
static unsigned gl_stat_fbo_binds;
static GLuint gl_stat_cur_fbo;
static GLint gl_stat_vp[4];
static unsigned gl_stat_vp_changes;
static GLenum gl_stat_fbo_status;
static GLenum gl_stat_last_err = GL_NO_ERROR;
static unsigned gl_stat_err_repeats;
static SceUInt64 gl_stat_last_beat;

void glDrawArrays_soloader(GLenum mode, GLint first, GLsizei count) {
    gl_stat_draws++;
    glDrawArrays(mode, first, count);
}

void glDrawElements_soloader(GLenum mode, GLsizei count, GLenum type,
                             const GLvoid *indices) {
    gl_stat_draws++;
    glDrawElements(mode, count, type, indices);
}

void glClear_soloader(GLbitfield mask) {
    gl_stat_clears++;
    glClear(mask);
}

void glViewport_soloader(GLint x, GLint y, GLsizei w, GLsizei h) {
    if (x != gl_stat_vp[0] || y != gl_stat_vp[1] ||
        w != gl_stat_vp[2] || h != gl_stat_vp[3]) {
        gl_stat_vp[0] = x; gl_stat_vp[1] = y;
        gl_stat_vp[2] = w; gl_stat_vp[3] = h;
        gl_stat_vp_changes++;
    }
    glViewport(x, y, w, h);
}

void glBindFramebufferOES_soloader(GLenum target, GLuint fb) {
    gl_stat_fbo_binds++;
    gl_stat_cur_fbo = fb;
    glBindFramebuffer(target, fb);
}

GLenum glCheckFramebufferStatusOES_soloader(GLenum target) {
    GLenum st = glCheckFramebufferStatus(target);
    gl_stat_fbo_status = st;
    if (st != GL_FRAMEBUFFER_COMPLETE)
        l_note("[GL] FBO incomplete: status=0x%x target=0x%x fbo=%u", st, target,
               gl_stat_cur_fbo);
    return st;
}

void gl_get_counters(unsigned *draws, unsigned *clears) {
    if (draws) *draws = gl_stat_draws;
    if (clears) *clears = gl_stat_clears;
}

// Upload-path counters (TEMP, same triage): the engine reports 69 "Loaded
// texture" while glTexImage2D only fires a handful of times -- PVR textures
// likely go through the compressed/subimage entry points, which were
// invisible to the [GL] stats line until now.
void glCompressedTexImage2D_soloader(GLenum target, GLint level,
                                     GLenum internalformat, GLsizei width,
                                     GLsizei height, GLint border,
                                     GLsizei imageSize, const GLvoid *data) {
    gl_stat_texup++;
    glCompressedTexImage2D(target, level, internalformat, width, height,
                           border, imageSize, data);
}

void glTexSubImage2D_soloader(GLenum target, GLint level, GLint xoffset,
                              GLint yoffset, GLsizei width, GLsizei height,
                              GLenum format, GLenum type, const GLvoid *pixels) {
    gl_stat_texup++;
    glTexSubImage2D(target, level, xoffset, yoffset, width, height, format,
                    type, pixels);
}

void glCopyTexSubImage2D_soloader(GLenum target, GLint level, GLint xoffset,
                                  GLint yoffset, GLint x, GLint y,
                                  GLsizei width, GLsizei height) {
    gl_stat_texup++;
    glCopyTexSubImage2D(target, level, xoffset, yoffset, x, y, width, height);
}

// Kept as a drop (vitaGL has no glCompressedTexSubImage2D to pass through
// to) but counted: >0 means the engine uploads texture data we discard.
void glCompressedTexSubImage2D_drop(GLenum target, GLint level, GLint xoffset,
                                    GLint yoffset, GLsizei width,
                                    GLsizei height, GLenum format,
                                    GLsizei imageSize, const GLvoid *data) {
    (void)target; (void)level; (void)xoffset; (void)yoffset; (void)width;
    (void)height; (void)format; (void)imageSize; (void)data;
    static int logged;
    if (!logged) {
        logged = 1;
        l_note("[GL] glCompressedTexSubImage2D called (dropped, no vitaGL "
               "backend): target=0x%x %dx%d fmt=0x%x size=%d", target, width,
               height, format, imageSize);
    }
    gl_stat_texup++;
}

// Called once per frame from gl_swap(), same thread/context the engine just
// rendered on, so glGetError() observes the engine's own GL state.
void gl_frame_tick(void) {
    gl_stat_frames++;

    GLenum err = glGetError();
    if (err != GL_NO_ERROR) {
        if (err != gl_stat_last_err) {
            l_note("[GL] glGetError: 0x%x (prev 0x%x x%u frames)", err,
                   gl_stat_last_err, gl_stat_err_repeats);
            gl_stat_last_err = err;
            gl_stat_err_repeats = 0;
        } else {
            gl_stat_err_repeats++;
        }
    }

    SceUInt64 now = sceKernelGetProcessTimeWide();
    if (gl_stat_last_beat == 0)
        gl_stat_last_beat = now;
    if (now - gl_stat_last_beat > 5000000) {
        l_note("[GL] frames=%u draws=%u clears=%u texUp=%u fbo=%u binds=%u "
               "vp=%d,%d,%d,%d(chg %u) fboStatus=0x%x lastErr=0x%x(x%u)",
               gl_stat_frames, gl_stat_draws, gl_stat_clears, gl_stat_texup,
               gl_stat_cur_fbo, gl_stat_fbo_binds,
               gl_stat_vp[0], gl_stat_vp[1], gl_stat_vp[2], gl_stat_vp[3],
               gl_stat_vp_changes, gl_stat_fbo_status,
               gl_stat_last_err, gl_stat_err_repeats);
        gl_stat_last_beat = now;
    }
}

// TEMP triage (kernel data-abort during HUD texture/material setup,
// 2026-09-05): validate texture-upload dimensions -- see gl.h. Also counts
// uploads for the [GL] stats line above.
void glTexImage2D_soloader(GLenum target, GLint level, GLint internalformat,
                           GLsizei width, GLsizei height, GLint border,
                           GLenum format, GLenum type, const GLvoid *pixels) {
    gl_stat_texup++;
    if (width <= 0 || height <= 0 || width > 4096 || height > 4096) {
        l_error("glTexImage2D INSANE dims: target=0x%x level=%d w=%d h=%d border=%d fmt=0x%x type=0x%x pixels=%p",
                target, level, width, height, border, format, type, pixels);
    } else {
        l_debug("glTexImage2D: target=0x%x level=%d w=%d h=%d fmt=0x%x type=0x%x pixels=%p",
                target, level, width, height, format, type, pixels);
    }
    glTexImage2D(target, level, internalformat, width, height, border, format, type, pixels);
}

// TEMP triage, same crash: catch heap-smashing copies. Legit texture/file
// traffic here is <= ~2-3 MB (huds.bmp is 2 MB); anything far beyond that is
// a corrupt length trashing the dlmalloc heap (the secondary kernel crash
// surfaces much later, somewhere unrelated).
void *memcpy_soloader(void *dst, const void *src, unsigned int len) {
    if (len > 8 * 1024 * 1024) {
        l_error("memcpy HUGE: dst=%p src=%p len=%u (0x%x)", dst, src, len, len);
    }
    return sceClibMemcpy(dst, src, len);
}
