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

static void gl_probe_dump_state(GLenum mode, GLsizei count);

void glDrawArrays_soloader(GLenum mode, GLint first, GLsizei count) {
    gl_stat_draws++;
    gl_probe_dump_state(mode, count);
    glDrawArrays(mode, first, count);
}

void glDrawElements_soloader(GLenum mode, GLsizei count, GLenum type,
                             const GLvoid *indices) {
    gl_stat_draws++;
    gl_probe_dump_state(mode, count);
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
    // Fase 26: el motor dibuja en 960x480 pero el panel es 960x544 (franja
    // negra de 64px arriba, ver foto 2026-09-10-013317). Todo rect en espacio
    // del motor sobre el framebuffer default se estira al alto completo;
    // los FBOs offscreen se dejan intactos (ahi las medidas son reales).
    // 480*544/480 = 544 exacto; el clamp de abajo absorbe el resto.
    if (gl_stat_cur_fbo == 0) {
        y = (y * 544) / 480;
        h = (h * 544) / 480;
    }
    // Fase 24: clamp al panel 960x544. Un rect fuera de rango (o NaN
    // convertido a int gigante) colgaria la GPU (ver Asphalt-5-Vita). El
    // area minima 1x1 evita viewports degenerados sin anular el estado.
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > 960) w = 960 - x;
    if (y + h > 544) h = 544 - y;
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    glViewport(x, y, w, h);
}

void glScissor_soloader(GLint x, GLint y, GLsizei w, GLsizei h) {
    // Fase 26: mismo estirado que el viewport para que el recorte de UI
    // coincida con la geometria estirada (solo framebuffer default).
    if (gl_stat_cur_fbo == 0) {
        y = (y * 544) / 480;
        h = (h * 544) / 480;
    }
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > 960) w = 960 - x;
    if (y + h > 544) h = 544 - y;
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    glScissor(x, y, w, h);
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

// Fase 20: el motor importa glLightf/glLightModelf/glLightx/
// glMultiTexCoord4f (objdump -T UND) pero vitaGL solo expone las variantes
// vectoriales (glLightfv/glLightModelfv/glLightxv) y glMultiTexCoord2f.
// Antes iban a ret0: las atenuaciones/cutoff de cada luz y las UV del
// segundo set de textura se perdian en silencio. Reenvio sin logging
// (MultiTexCoord va por vertice: cero costo permitido).
void glLightf_soloader(GLenum light, GLenum pname, GLfloat param) {
    GLfloat v[1] = { param };
    glLightfv(light, pname, v);
}

void glLightModelf_soloader(GLenum pname, GLfloat param) {
    // 0xB52 = GL_LIGHT_MODEL_COLOR_CONTROL (SEPARATE_SPECULAR_COLOR): la FFP
    // de vitaGL no tiene path de especular separado y levanta
    // GL_INVALID_ENUM (visto en debug_local_027.log, ffp.c:3467). Es solo
    // calidad (single vs separate specular); el default SINGLE_COLOR queda.
    if (pname == 0x0B52)
        return;
    GLfloat v[1] = { param };
    glLightModelfv(pname, v);
}

void glLightx_soloader(GLenum light, GLenum pname, GLfixed param) {
    GLfloat v[1] = { (GLfloat)param / 65536.0f };
    glLightfv(light, pname, v);
}

void glMultiTexCoord4f_soloader(GLenum target, GLfloat s, GLfloat t,
                                GLfloat r, GLfloat q) {
    (void)r; (void)q;
    glMultiTexCoord2f(target, s, t);
}

// Fase 22: log-on-change de tipos de array y clear color. Solo se loguea
// cuando la combinacion cambia (setup, unas pocas veces por pantalla), jamas
// por vertice ni por frame.
//
// Bug real (Fase 28, 2026-09-10): las 4 llamadas (Vertex/Color/TexCoord/
// NormalPointer) comparten un unico array de 8 slots. Un modelo 3D real usa
// muchas mas de 8 combinaciones distintas (solo glVertexPointer ya tiene 7
// strides distintos -- 12/16/20/24/28/32/36 -- vistos en debug_local_033.log),
// asi que el array se llena enseguida. Una vez lleno, el loop de arriba no
// encuentra la tupla (nunca se guardo) y el codigo caia al `l_note()` de
// abajo INCONDICIONALMENTE -- volviendo a loguear en cada llamada para
// cualquier combinacion nueva no trackeada, para siempre. Como el motor
// llama a estas funciones por-mesh antes de cada draw, eso significaba pagar
// el costo completo de `l_note()` (LwMutex + 2 snprintf + sceNetSendto
// bloqueante + sceIoWrite) varias veces por draw durante toda la carga real
// de gameplay -- coincide exactamente con los ~40-55 draws/frame y los
// frames sostenidos a 700-1200 ms vistos desde el frame ~271 en adelante en
// debug_local_033.log (no es un pico de compilacion de shaders como el de
// los frames ~176-187: ahi SI hay huecos entre picos, aca es CADA frame por
// igual). Fix: una vez el array esta lleno, no volver a loguear esa tupla
// (silencio, no crash) en vez de caer al `l_note()` de todos modos.
static void gl_note_once(const char *name, GLint size, GLenum type,
                         GLsizei stride) {
    static struct { const char *name; GLint size; GLenum type; GLsizei stride; } seen[8];
    static unsigned n;
    for (unsigned i = 0; i < n; i++) {
        if (seen[i].name == name && seen[i].size == size &&
            seen[i].type == type && seen[i].stride == stride)
            return;
    }
    if (n >= 8)
        return;
    seen[n].name = name; seen[n].size = size;
    seen[n].type = type; seen[n].stride = stride;
    n++;
    // 0x1401=UBYTE 0x1403=USHORT 0x1406=FLOAT 0x140C=FIXED — en crudo para no
    // depender de strings GL en este shim.
    l_note("[GL] %s: size=%d type=0x%x stride=%d", name, size, type, stride);
}

void glVertexPointer_soloader(GLint size, GLenum type, GLsizei stride,
                              const GLvoid *pointer) {
    gl_note_once("glVertexPointer", size, type, stride);
    glVertexPointer(size, type, stride, pointer);
}

void glColorPointer_soloader(GLint size, GLenum type, GLsizei stride,
                             const GLvoid *pointer) {
    gl_note_once("glColorPointer", size, type, stride);
    glColorPointer(size, type, stride, pointer);
}

void glTexCoordPointer_soloader(GLint size, GLenum type, GLsizei stride,
                                const GLvoid *pointer) {
    gl_note_once("glTexCoordPointer", size, type, stride);
    glTexCoordPointer(size, type, stride, pointer);
}

void glNormalPointer_soloader(GLenum type, GLsizei stride,
                              const GLvoid *pointer) {
    gl_note_once("glNormalPointer", 3, type, stride);
    glNormalPointer(type, stride, pointer);
}

void glClearColor_soloader(GLclampf r, GLclampf g, GLclampf b, GLclampf a) {
    static GLfloat lr, lg, lb, la;
    static int first = 1;
    if (first || r != lr || g != lg || b != lb || a != la) {
        first = 0;
        lr = r; lg = g; lb = b; la = a;
        l_note("[GL] glClearColor: %.2f %.2f %.2f %.2f", r, g, b, a);
    }
    glClearColor(r, g, b, a);
}

/*
 * TEMP triage (2026-09-10): "does anything WE draw reach the screen?"
 *
 * Where the triage stands: the engine runs at a steady 30 fps, submits draws
 * every frame, leaves glGetError() at 0x0, uploads 205 textures -- and the
 * displayed framebuffer is verified 100% zero (shot_00600.bmp: 522240 px,
 * avg 0.00, zero px above brightness 8). Every hypothesis tested from Fase 15
 * to Fase 22 lives *inside* the engine's own GL state. Nothing has tested the
 * layer underneath it.
 *
 * This probe does: our own geometry, our own known-good state, through the
 * same glDrawArrays path the engine uses, right before vglSwapBuffers().
 *
 * How to read the next shot_*.bmp:
 *   - bars visible          -> vitaGL, the present path and vertex arrays are
 *                              all fine; the black screen is something in the
 *                              state the ENGINE sets (the one-shot state dump
 *                              below then says which knob).
 *   - still 100% black      -> the problem is below the engine entirely
 *                              (surface / present / vglInit), and every
 *                              engine-state hypothesis so far is a red herring.
 *   - top bars only, bottom -> the panel is 544 tall but only the engine's
 *     strip missing            960x480 region reaches the display.
 *
 * Bounded and non-invasive: stops after GMV_PROBE_LAST_FRAME, and wraps
 * everything in glPushAttrib/glPopAttrib + matrix push/pop. Caveat: vertex
 * ARRAY POINTERS are client state, which glPushAttrib does not cover -- the
 * engine re-specifies them per batch (Fase 22's gl*Pointer log confirms), and
 * the screen is already black, so there is nothing to lose either way.
 */
#define GMV_PROBE_SELFTEST 0 // Fase 25: cumplio su mision (barras visibles
// en 031 = viewport real). Apagado para ver la imagen del juego; el dump de
// estado del frame 400 sigue activo.
#define GMV_PROBE_LAST_FRAME 1200u

void gl_probe_selftest(void) {
#if GMV_PROBE_SELFTEST
    if (gl_stat_frames > GMV_PROBE_LAST_FRAME)
        return;

    glPushAttrib(GL_ENABLE_BIT | GL_VIEWPORT_BIT | GL_TRANSFORM_BIT |
                 GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_SCISSOR_BIT);

    glDisable(GL_LIGHTING);
    glDisable(GL_TEXTURE_2D);
    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_ALPHA_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_SCISSOR_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glDepthMask(GL_TRUE);

    // Full panel on purpose -- the engine renders into 960x480 (see the [GL]
    // vp= line), so a missing bottom bar localizes the difference.
    glViewport(0, 0, 960, 544);

    glMatrixMode(GL_PROJECTION);
    glPushMatrix();
    glLoadIdentity();
    glOrthof(0.0f, 960.0f, 544.0f, 0.0f, -1.0f, 1.0f);
    glMatrixMode(GL_MODELVIEW);
    glPushMatrix();
    glLoadIdentity();

    glEnableClientState(GL_VERTEX_ARRAY);
    glDisableClientState(GL_COLOR_ARRAY);
    glDisableClientState(GL_TEXTURE_COORD_ARRAY);
    glDisableClientState(GL_NORMAL_ARRAY);

    // 4 bars across the top (x = 0..960, y = 8..72) + 1 across the very bottom
    // (y = 500..536, inside the 480..544 strip the engine never touches).
    static const GLfloat bars[] = {
          0.f,   8.f,  240.f,   8.f,    0.f,  72.f,  240.f,  72.f,
        240.f,   8.f,  480.f,   8.f,  240.f,  72.f,  480.f,  72.f,
        480.f,   8.f,  720.f,   8.f,  480.f,  72.f,  720.f,  72.f,
        720.f,   8.f,  960.f,   8.f,  720.f,  72.f,  960.f,  72.f,
          0.f, 500.f,  960.f, 500.f,    0.f, 536.f,  960.f, 536.f,
    };
    static const GLfloat cols[5][4] = {
        { 1.f, 1.f, 1.f, 1.f },  // white
        { 1.f, 0.f, 0.f, 1.f },  // red
        { 0.f, 1.f, 0.f, 1.f },  // green
        { 0.f, 0.f, 1.f, 1.f },  // blue
        { 1.f, 0.f, 1.f, 1.f },  // magenta, bottom strip
    };

    glVertexPointer(2, GL_FLOAT, 0, bars);
    for (int i = 0; i < 5; i++) {
        glColor4f(cols[i][0], cols[i][1], cols[i][2], cols[i][3]);
        glDrawArrays(GL_TRIANGLE_STRIP, i * 4, 4);
    }

    glMatrixMode(GL_MODELVIEW);
    glPopMatrix();
    glMatrixMode(GL_PROJECTION);
    glPopMatrix();
    glMatrixMode(GL_MODELVIEW);
    glPopAttrib();

    static int announced;
    if (!announced) {
        announced = 1;
        GLenum e = glGetError();
        l_note("[GL] selftest bars armed (frames 1-%u), post-draw err=0x%x",
               GMV_PROBE_LAST_FRAME, e);
    }
#endif
}

/*
 * TEMP triage (2026-09-10): one-shot snapshot of the fixed-function state the
 * ENGINE has set at one of its own draw calls. Answers, in a single line each,
 * the questions the counters cannot: is the current color black (everything
 * modulates to zero)? is lighting on with no usable light? is a texture even
 * bound? do the matrices map geometry onto the screen or off it?
 *
 * Fires once, at the first draw of frame GMV_PROBE_DUMP_FRAME -- chosen well
 * past the shader-link burst so it samples the steady state, not loading.
 */
#define GMV_PROBE_DUMP_FRAME 400u

static void gl_probe_dump_state(GLenum mode, GLsizei count) {
    static int done;
    if (done || gl_stat_frames < GMV_PROBE_DUMP_FRAME)
        return;
    done = 1;

    GLfloat cur[4] = { -9.f, -9.f, -9.f, -9.f };
    GLfloat proj[16], mv[16];
    glGetFloatv(GL_CURRENT_COLOR, cur);
    glGetFloatv(GL_PROJECTION_MATRIX, proj);
    glGetFloatv(GL_MODELVIEW_MATRIX, mv);

    GLint tex = -1, bsrc = -1, bdst = -1, vp[4] = { -1, -1, -1, -1 };
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &tex);
    glGetIntegerv(GL_BLEND_SRC, &bsrc);
    glGetIntegerv(GL_BLEND_DST, &bdst);
    glGetIntegerv(GL_VIEWPORT, vp);

    l_note("[GL] draw-state @frame %u: mode=0x%x count=%d color=%.2f,%.2f,%.2f,%.2f "
           "tex=%d light=%d tex2d=%d blend=%d(%x/%x) alpha=%d depth=%d cull=%d "
           "vp=%d,%d,%d,%d",
           gl_stat_frames, mode, count, cur[0], cur[1], cur[2], cur[3], tex,
           glIsEnabled(GL_LIGHTING), glIsEnabled(GL_TEXTURE_2D),
           glIsEnabled(GL_BLEND), bsrc, bdst, glIsEnabled(GL_ALPHA_TEST),
           glIsEnabled(GL_DEPTH_TEST), glIsEnabled(GL_CULL_FACE),
           vp[0], vp[1], vp[2], vp[3]);
    l_note("[GL] proj = [%.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f | "
           "%.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f]",
           proj[0], proj[1], proj[2], proj[3], proj[4], proj[5], proj[6], proj[7],
           proj[8], proj[9], proj[10], proj[11], proj[12], proj[13], proj[14], proj[15]);
    l_note("[GL] mview = [%.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f | "
           "%.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f]",
           mv[0], mv[1], mv[2], mv[3], mv[4], mv[5], mv[6], mv[7],
           mv[8], mv[9], mv[10], mv[11], mv[12], mv[13], mv[14], mv[15]);
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
