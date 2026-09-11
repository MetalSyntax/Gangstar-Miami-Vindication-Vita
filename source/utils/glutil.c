/*
 * Copyright (C) 2021      Andy Nguyen
 * Copyright (C) 2021      Rinnegatamante
 * Copyright (C) 2022-2023 Volodymyr Atamanenko
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#include "utils/glutil.h"

#include "reimpl/gl.h"
#include "utils/utils.h"
#include "utils/dialog.h"
#include "utils/logger.h"

#include <stdio.h>
#include <malloc.h>
#include <string.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/display.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>

// Helpers for our handling of shaders
GLboolean skip_next_compile = GL_FALSE;
char next_shader_fname[256];
void load_shader(GLuint shader, const char * string, size_t length);

void gl_preload() {
    if (!file_exists("ur0:/data/libshacccg.suprx")
        && !file_exists("ur0:/data/external/libshacccg.suprx")) {
        fatal_error("Error: libshacccg.suprx is not installed. "
                    "Google \"ShaRKBR33D\" for quick installation.");
    }

#ifdef USE_GLSL_SHADERS
    vglSetSemanticBindingMode(VGL_MODE_POSTPONED);
#endif
}

void gl_init() {
    // Fase 27 (2026-09-10): vitaGL's fixed-function pipeline (lib/vitaGL/source/ffp.c)
    // persists every compiled shader variant to disk at
    // ux0:data/shader_cache/v<FFP_SHADER_CACHE_MAGIC>/{v,f}/*.gxp (shared.h:300,
    // magic=28 in our pinned vitaGL) so a later run can sceIoOpen() the .gxp
    // instead of paying a real vitaShaRK compile again. But sceIoOpen(..., O_CREAT)
    // does NOT create missing parent directories -- with nothing creating this tree,
    // every save silently failed and EVERY run recompiled EVERY FFP shader variant
    // from scratch. That's the real cost behind the repeated 15-34s "shader link
    // burst" stalls at frames ~172-190 seen since Fase 17 (title screen's first
    // unique lighting/texture-combiner masks): not log spam (already fixed, Fase 19)
    // nor engine-side material diagnostics (ruled out, Fase 18) but vitaGL
    // recompiling from zero every single boot. Creating the tree once here lets the
    // cache actually persist: first run still compiles (unavoidable), every run
    // after reuses the .gxp files and should reach the title/menu much faster.
    if (!file_mkpath("ux0:data/shader_cache/v28/v/x", 0777) ||
        !file_mkpath("ux0:data/shader_cache/v28/f/x", 0777)) {
        l_warn("gl_init: could not create ux0:data/shader_cache/v28/{v,f} -- "
               "FFP shader cache will keep recompiling every run.");
    }

    // Fase 29 (2026-09-10): debug_local_034.log shows "Circular pool overrun on
    // frame N" (lib/vitaGL/source/gxm.c:788) on ~37% of frames in real gameplay
    // (299 hits between frames 705-1500, peaks up to 956592 bytes over budget),
    // exactly overlapping the region where the user-reported ~9 fps sits (this
    // engine's FFP draws copy client-side vertex/color/texcoord arrays into
    // vitaGL's circular pool per draw call, and real 3D meshes -- unlike the
    // simple loading-screen quads seen through Fase 26-28 -- need far more of
    // it). Once a frame's circular pool slot (default 32 MB / 3 buffers =
    // ~10.7 MB each, lib/vitaGL/source/vgl.c:111,364) is exhausted,
    // vgl_reserve_data_pool() (vgl.c:127-149) stops doing cheap bump-pointer
    // allocation and falls back to a real gpu_alloc_mapped_for_cpu() kernel
    // allocation *per over-budget reservation* -- which is exactly the kind of
    // per-draw cost that produces the wildly uneven frame times in the log
    // (588 ms-21 s "slow render" spikes mixed with normal ones), not a steady
    // per-frame cost like Fase 28's logging bug. Doubling the pool to 64 MB
    // (~21.3 MB/buffer) gives ~2x headroom over the worst peak seen so far.
    // Must run before vglInitExtended(): the buffers are sized from
    // circular_data_pool_size at init time (vgl.c:364), not resizable after.
    vglSetCircularPoolSize(64 * 1024 * 1024);

    // MULTISAMPLE_NONE + 12 MB (2026-09-06): the Asphalt-5 recipe from
    // port_progress.md Fase 12. The old 4X MSAA made every vglSwapBuffers()
    // pay a multisample resolve -- pure overhead during the minutes-long
    // black-screen asset load -- and MSAA + the FBO OES path this engine
    // uses is a risky combination in vitaGL. Reversible if menus look off.
    vglInitExtended(0, 960, 544, 12 * 1024 * 1024, SCE_GXM_MULTISAMPLE_NONE);
}

void gl_swap() {
    gl_frame_tick();
    // TEMP triage (2026-09-10): our own bars, drawn last so nothing the engine
    // does can paint over them. See gl_probe_selftest() in reimpl/gl.c.
    gl_probe_selftest();
    vglSwapBuffers(GL_FALSE);
}

// TEMP triage (black screen with live draws, 2026-09-06): dump the displayed
// framebuffer to a 32-bit BMP so a human can see whether the backbuffer is
// truly all-black, a very dark scene, or something rendered outside the
// visible area. Called every few hundred frames from main.c -- a ~2 MB write
// once per ~20 s, negligible next to asset loading. Remove with the triage.
int gl_shot(const char *path) {
    SceDisplayFrameBuf fb;
    memset(&fb, 0, sizeof(fb));
    fb.size = sizeof(fb);
    if (sceDisplayGetFrameBuf(&fb, SCE_DISPLAY_SETBUF_NEXTFRAME) < 0 || !fb.base)
        return -1;
    if (fb.pixelformat != SCE_DISPLAY_PIXELFORMAT_A8B8G8R8)
        return -2;
    if (fb.width == 0 || fb.height == 0 || fb.width > 1024 || fb.height > 1024)
        return -3;

    // BMP headers, 32-bit BI_RGB top-down (negative height).
    uint8_t hdr[54];
    memset(hdr, 0, sizeof(hdr));
    uint32_t row_bytes = fb.width * 4;
    uint32_t img_bytes = row_bytes * fb.height;
    uint32_t file_bytes = sizeof(hdr) + img_bytes;
    hdr[0] = 'B'; hdr[1] = 'M';
    hdr[2] = file_bytes & 0xFF; hdr[3] = (file_bytes >> 8) & 0xFF;
    hdr[4] = (file_bytes >> 16) & 0xFF; hdr[5] = (file_bytes >> 24) & 0xFF;
    hdr[10] = sizeof(hdr);
    hdr[14] = 40;
    hdr[18] = fb.width & 0xFF; hdr[19] = (fb.width >> 8) & 0xFF;
    int32_t neg_h = -(int32_t)fb.height;
    hdr[22] = neg_h & 0xFF; hdr[23] = (neg_h >> 8) & 0xFF;
    hdr[24] = (neg_h >> 16) & 0xFF; hdr[25] = (neg_h >> 24) & 0xFF;
    hdr[26] = 1; hdr[28] = 32;

    SceUID fd = sceIoOpen(path, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
    if (fd < 0) return -4;
    if (sceIoWrite(fd, hdr, sizeof(hdr)) != (SceSSize)sizeof(hdr)) {
        sceIoClose(fd);
        return -5;
    }
    // Display memory is R,G,B,A byte order; BMP 32-bit wants B,G,R,A.
    static uint8_t row[1024 * 4];
    const uint8_t *base = (const uint8_t *)fb.base;
    for (unsigned y = 0; y < fb.height; y++) {
        const uint8_t *src = base + y * fb.pitch * 4;
        for (unsigned x = 0; x < fb.width; x++) {
            row[x * 4 + 0] = src[x * 4 + 2];
            row[x * 4 + 1] = src[x * 4 + 1];
            row[x * 4 + 2] = src[x * 4 + 0];
            row[x * 4 + 3] = src[x * 4 + 3];
        }
        if (sceIoWrite(fd, row, row_bytes) != (SceSSize)row_bytes) {
            sceIoClose(fd);
            return -6;
        }
    }
    sceIoClose(fd);
    return 0;
}

void glShaderSource_soloader(GLuint shader, GLsizei count,
                             const GLchar **string, const GLint *_length) {
#ifdef DEBUG_OPENGL
    sceClibPrintf("[gl_dbg] glShaderSource<%p>(shader: %i, count: %i, string: %p, length: %p)\n", __builtin_return_address(0), shader, count, string, _length);
#endif
    if (!string) {
        l_error("<%p> Shader source string is NULL, count: %i",
                   __builtin_return_address(0), count);
        skip_next_compile = GL_TRUE;
        return;
    } else if (!*string) {
        l_error("<%p> Shader source *string is NULL, count: %i",
                   __builtin_return_address(0), count);
        skip_next_compile = GL_TRUE;
        return;
    }

    size_t total_length = 0;

    for (int i = 0; i < count; ++i) {
        if (!_length) {
            total_length += strlen(string[i]);
        } else {
            total_length += _length[i];
        }
    }

    char * str = malloc(total_length+1);
    size_t l = 0;

    for (int i = 0; i < count; ++i) {
        if (!_length) {
            memcpy(str + l, string[i], strlen(string[i]));
            l += strlen(string[i]);
        } else {
            memcpy(str + l, string[i], _length[i]);
            l += _length[i];
        }
    }
    str[total_length] = '\0';

    load_shader(shader, str, total_length);

    free(str);
}

void glCompileShader_soloader(GLuint shader) {
#ifdef DEBUG_OPENGL
    sceClibPrintf("[gl_dbg] glCompileShader<%p>(shader: %i)\n", __builtin_return_address(0), shader);
#endif

#ifndef USE_GXP_SHADERS
    if (!skip_next_compile) {
        glCompileShader(shader);
#ifdef DUMP_COMPILED_SHADERS
        void *bin = vglMalloc(32 * 1024);
        GLsizei len;
        vglGetShaderBinary(shader, 32 * 1024, &len, bin);
        file_save(next_shader_fname, bin, len);
        vglFree(bin);
#endif
    }
    skip_next_compile = GL_FALSE;
#endif
}

#if defined(USE_GLSL_SHADERS) && defined(DUMP_COMPILED_SHADERS)
void load_shader(GLuint shader, const char * string, size_t length) {
    char* sha_name = str_sha1sum(string, length);

    char gxp_path[256];
    snprintf(gxp_path, sizeof(gxp_path), DATA_PATH"gxp/%s.gxp", sha_name);

    if (file_exists(gxp_path)) {
        uint8_t *buffer;
        size_t size;

        file_load(gxp_path, &buffer, &size);

        glShaderBinary(1, &shader, 0, buffer, (int32_t) size);

        free(buffer);
        skip_next_compile = GL_TRUE;
    } else {
        glShaderSource(shader, 1, &string, &length);
        strcpy(next_shader_fname, gxp_path);
    }

    free(sha_name);
}
#elif defined(USE_GLSL_SHADERS)
void load_shader(GLuint shader, const char * string, size_t length) {
    glShaderSource(shader, 1, &string, &length);
}
#elif defined(USE_CG_SHADERS) && defined(DUMP_COMPILED_SHADERS)
void load_shader(GLuint shader, const char * string, size_t length) {
    char* sha_name = str_sha1sum(string, length);

    char gxp_path[256];
    char cg_path[256];
    snprintf(gxp_path, sizeof(gxp_path), DATA_PATH"gxp/%s.gxp", sha_name);
    snprintf(cg_path, sizeof(cg_path), DATA_PATH"cg/%s.cg", sha_name);

    if (file_exists(gxp_path)) {
        uint8_t *buffer;
        size_t size;

        file_load(gxp_path, &buffer, &size);

        glShaderBinary(1, &shader, 0, buffer, (int32_t) size);

        free(buffer);
        skip_next_compile = GL_TRUE;
    } else if (file_exists(cg_path)) {
        char *buffer;
        size_t size;

        file_load(cg_path, (uint8_t **) &buffer, &size);

        glShaderSource(shader, 1, &string, &size);
        strcpy(next_shader_fname, gxp_path);

        free(buffer);
        skip_next_compile = GL_FALSE;
    } else {
        l_warn("Encountered an untranslated shader %s, saving GLSL "
               "and using a dummy shader.", sha_name);

        char glsl_path[256];
        snprintf(glsl_path, sizeof(glsl_path), DATA_PATH"glsl/%s.glsl", sha_name);
        file_mkpath(glsl_path, 0777);
        file_save(glsl_path, (const uint8_t *) string, length);

        if (strstr(string, "gl_FragColor")) {
            const char *dummy_shader = "float4 main() { return float4(1.0,1.0,1.0,1.0); }";
            int32_t dummy_shader_len = (int32_t) strlen(dummy_shader);
            glShaderSource(shader, 1, &dummy_shader, &dummy_shader_len);
        } else {
            const char *dummy_shader = "void main(float4 out gl_Position : POSITION ) { gl_Position = float4(1.0,1.0,1.0,1.0); }";
            int32_t dummy_shader_len = (int32_t) strlen(dummy_shader);
            glShaderSource(shader, 1, &dummy_shader, &dummy_shader_len);
        }

        skip_next_compile = GL_FALSE;
    }

    free(sha_name);
}
#elif defined(USE_CG_SHADERS) || defined(USE_GXP_SHADERS)
void load_shader(GLuint shader, const char * string, size_t length) {
    char* sha_name = str_sha1sum(string, length);

    char path[256];
#ifdef USE_CG_SHADERS
    snprintf(path, sizeof(path), DATA_PATH"cg/%s.cg", sha_name);
#else
    snprintf(path, sizeof(path), DATA_PATH"gxp/%s.gxp", sha_name);
#endif

    if (file_exists(path)) {
#ifdef USE_CG_SHADERS
        char *buffer;
        size_t size;

        file_load(path, (uint8_t **) &buffer, &size);

        glShaderSource(shader, 1, &string, &size);

        free(buffer);
#else
        uint8_t *buffer;
        size_t size;

        file_load(path, &buffer, &size);

        glShaderBinary(1, &shader, 0, buffer, (int32_t) size);

        free(buffer);
#endif
    } else {
        l_warn("Encountered an untranslated shader %s, saving GLSL "
               "and using a dummy shader.", sha_name);

        char glsl_path[256];
        snprintf(glsl_path, sizeof(glsl_path), DATA_PATH"glsl/%s.glsl", sha_name);
        file_mkpath(glsl_path, 0777);
        file_save(glsl_path, (const uint8_t *) string, length);

        if (strstr(string, "gl_FragColor")) {
            const char *dummy_shader = "float4 main() { return float4(1.0,1.0,1.0,1.0); }";
            int32_t dummy_shader_len = (int32_t) strlen(dummy_shader);
            glShaderSource(shader, 1, &dummy_shader, &dummy_shader_len);
        } else {
            const char *dummy_shader = "void main(float4 out gl_Position : POSITION ) { gl_Position = float4(1.0,1.0,1.0,1.0); }";
            int32_t dummy_shader_len = (int32_t) strlen(dummy_shader);
            glShaderSource(shader, 1, &dummy_shader, &dummy_shader_len);
        }
    }

    free(sha_name);
}
#else
#error "Define one of (USE_GLSL_SHADERS, USE_CG_SHADERS, USE_GXP_SHADERS)"
#endif
