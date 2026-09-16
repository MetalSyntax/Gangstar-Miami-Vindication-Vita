/**
 * @file video.cpp
 * @brief Cutscene video playback using the SceAvPlayer hardware decoder.
 * @details Ported from the Shadow Guardian port's video.cpp (same soloader
 *          lineage). Decodes NV12 (Y + interleaved UV) frames and converts
 *          them to RGB on the GPU via a small GLES2 shader pair -- vitaGL
 *          supports this fine even though the game's own renderer only ever
 *          sees the spoofed "OpenGL ES 1.1" fixed-function pipeline
 *          (source/reimpl/gl.c); this code talks to vitaGL directly and
 *          never goes through the engine's GOT-patched GL entry points.
 */

#include "video.h"
#include "utils/logger.h"
#include "utils/glutil.h"

#include <psp2/avplayer.h>
#include <psp2/sysmodule.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/ctrl.h>
#include <psp2/audioout.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/gxm.h>

#include <kubridge.h>
#include <malloc.h>
#include <pthread.h>
#include <string.h>
#include <stdio.h>

static bool gModuleLoaded = false;
static unsigned char *gYuvScratch = NULL;
static unsigned gYuvScratchCap = 0;

/**
 * @struct AvFileCtx
 * @brief Context structure for SceAvPlayer file replacement callbacks.
 */
struct AvFileCtx {
    SceUID fd;
    uint64_t total_read;
    unsigned read_calls;
};
static AvFileCtx gAvFileCtx = { -1, 0, 0 };

static int av_file_open(void *p, const char *filename) {
    AvFileCtx *ctx = (AvFileCtx *) p;
    ctx->fd = sceIoOpen(filename, SCE_O_RDONLY, 0);
    ctx->total_read = 0;
    ctx->read_calls = 0;
    l_info("video: file open(%s) -> fd=0x%08X", filename, (unsigned) ctx->fd);
    return ctx->fd < 0 ? -1 : 0;
}

static int av_file_close(void *p) {
    AvFileCtx *ctx = (AvFileCtx *) p;
    l_info("video: file close (reads=%u, total_bytes=%llu)", ctx->read_calls,
           (unsigned long long) ctx->total_read);
    if (ctx->fd >= 0) sceIoClose(ctx->fd);
    ctx->fd = -1;
    return 0;
}

static int av_file_read(void *p, uint8_t *buffer, uint64_t position, uint32_t length) {
    AvFileCtx *ctx = (AvFileCtx *) p;
    int n = sceIoPread(ctx->fd, buffer, length, (SceOff) position);
    ctx->read_calls++;
    if (ctx->read_calls <= 5 || n < 0)
        l_info("video: file read #%u pos=%llu len=%u -> %d", ctx->read_calls,
               (unsigned long long) position, length, n);
    if (n > 0) ctx->total_read += (uint64_t) n;
    return n;
}

static uint64_t av_file_size(void *p) {
    AvFileCtx *ctx = (AvFileCtx *) p;
    SceOff end = sceIoLseek(ctx->fd, 0, SCE_SEEK_END);
    l_info("video: file size -> %llu", (unsigned long long) end);
    return (uint64_t) end;
}

/**
 * @brief Event and diagnostic callbacks for SceAvPlayer.
 */
static const char *av_event_name(int32_t id) {
    switch (id) {
        case 0x01: return "STATE_STOP";
        case 0x02: return "STATE_READY";
        case 0x03: return "STATE_PLAY";
        case 0x04: return "STATE_PAUSE";
        case 0x05: return "STATE_BUFFERING";
        case 0x10: return "TIMED_TEXT_DELIVERY";
        case 0x20: return "WARNING_ID";
        default:   return "?";
    }
}

static void av_event_cb(void *p, int32_t eventId, int32_t sourceId, void *eventData) {
    (void) p;
    if (eventId == 0x20 && eventData) {
        l_error("video: event WARNING_ID source=%d code=0x%08X", sourceId,
                (unsigned) *(int32_t *) eventData);
    } else {
        l_info("video: event %s (0x%02X) source=%d data=%p", av_event_name(eventId),
               (unsigned) eventId, sourceId, eventData);
    }
}

#define AV_FB_ALIGNMENT 0x40000
#define AV_ALIGN_MEM(x, a) (((x) + ((a) - 1)) & ~((a) - 1))

static void *av_alloc(void *arg, uint32_t alignment, uint32_t size) {
    (void) arg;
    void *p = memalign(alignment, size);
    if (!p)
        l_error("video: general alloc FAILED (align=%u size=%u)", alignment, size);
    return p;
}

static void av_free(void *arg, void *ptr) {
    (void) arg;
    free(ptr);
}

#define AV_TEX_MAX_BLOCKS 8
static struct { void *base; SceUID uid; } gAvTexBlocks[AV_TEX_MAX_BLOCKS];

/**
 * @brief Texture memory allocator with tiered fallback so the intro actually starts.
 * @details Tier 1 is CDRAM with custom alignment (what SceAvPlayer normally
 *          uses). Tier 2 is physically-contiguous main RAM, which the kernel
 *          always aligns to 1MB itself and rejects a custom alignment opt for
 *          (SCE_KERNEL_ERROR_INVALID_ARGUMENT) -- hence NULL opt. Tier 3 is
 *          USER_RW_UNCACHE: uncached, GXM-mappable, and not drawn from either
 *          contiguous pool, so it still succeeds when both CDRAM and PHYCONT
 *          report NO_FREE_PHYSICAL_PAGE -- a real possibility here since
 *          vitaGL's circular pool (gl_init(), 64 MB) and shader cache already
 *          compete for the same physically-contiguous memory. Our playback
 *          path only memcpys the decoded frame out and uploads it via
 *          glTexSubImage2D, so any CPU-writable mapping works; the
 *          sceGxmMapMemory call keeps the pointer valid for AvPlayer.
 */
static void *av_alloc_texture(void *arg, uint32_t alignment, uint32_t size) {
    (void) arg;
    uint32_t req_align = alignment, req_size = size;
    if (alignment < AV_FB_ALIGNMENT)
        alignment = AV_FB_ALIGNMENT;
    size = AV_ALIGN_MEM(size, alignment);

    SceKernelAllocMemBlockOpt opt;
    memset(&opt, 0, sizeof(opt));
    opt.size = sizeof(opt);
    opt.attr = 0x00000004U; // SCE_KERNEL_ALLOC_MEMBLOCK_ATTR_HAS_ALIGNMENT
    opt.alignment = alignment;
    SceUID blk = sceKernelAllocMemBlock("av_tex", SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW, size, &opt);
    const char *usedType = "CDRAM";
    if (blk < 0) {
        SceUID cdram_err = blk;
        // PHYCONT blocks are always naturally aligned to 1MB by the kernel and
        // reject a custom alignment attribute with SCE_KERNEL_ERROR_INVALID_ARGUMENT
        // (0x80020005) -- pass no opt at all for this fallback allocation.
        uint32_t phy_size = AV_ALIGN_MEM(size, 0x100000);
        SceUID blk2 = sceKernelAllocMemBlock("av_tex_phycont", SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_PHYCONT_RW, phy_size, NULL);
        if (blk2 < 0) {
            SceUID phy_err = blk2;
            // Last resort: uncached RAM (SDL Vita GXM pattern). Accepts an alignment
            // opt like CDRAM and is GXM-mappable, but comes from neither contiguous
            // pool, so it survives both being exhausted.
            SceUID blk3 = sceKernelAllocMemBlock("av_tex_uncache", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE, size, &opt);
            if (blk3 < 0) {
                l_error("video: texture memblock alloc FAILED on CDRAM (0x%08X), PHYCONT (0x%08X) and UNCACHE (0x%08X) (req align=%u size=%u -> size=%u)",
                        (unsigned) cdram_err, (unsigned) phy_err, (unsigned) blk3, req_align, req_size, size);
                return NULL;
            }
            l_warn("video: CDRAM (0x%08X) and PHYCONT (0x%08X) exhausted -- fell back to UNCACHE for this frame buffer",
                   (unsigned) cdram_err, (unsigned) phy_err);
            blk = blk3;
            usedType = "UNCACHE";
        } else {
            l_warn("video: CDRAM alloc failed (0x%08X) -- fell back to PHYCONT for this frame buffer", (unsigned) cdram_err);
            blk = blk2;
            usedType = "PHYCONT";
            size = phy_size;
        }
    }
    void *base = NULL;
    sceKernelGetMemBlockBase(blk, &base);
    int map = sceGxmMapMemory(base, size, (SceGxmMemoryAttribFlags)(SCE_GXM_MEMORY_ATTRIB_READ | SCE_GXM_MEMORY_ATTRIB_WRITE));

    int slot = -1;
    for (int i = 0; i < AV_TEX_MAX_BLOCKS; i++) {
        if (!gAvTexBlocks[i].base) { slot = i; break; }
    }
    if (slot < 0) {
        l_error("video: no free texture block slots (max %d) -- releasing uid=0x%08X", AV_TEX_MAX_BLOCKS, (unsigned) blk);
        sceGxmUnmapMemory(base);
        sceKernelFreeMemBlock(blk);
        return NULL;
    }
    gAvTexBlocks[slot].base = base;
    gAvTexBlocks[slot].uid = blk;
    l_info("video: texture memblock ok (%s) (req align=%u size=%u -> size=%u) base=%p uid=0x%08X gxm_map=0x%08X",
           usedType, req_align, req_size, size, base, (unsigned) blk, (unsigned) map);
    return base;
}

static void av_free_texture(void *arg, void *ptr) {
    (void) arg;
    if (!ptr) return;
    glFinish();
    for (int i = 0; i < AV_TEX_MAX_BLOCKS; i++) {
        if (gAvTexBlocks[i].base == ptr) {
            l_info("video: texture memblock free %p uid=0x%08X", ptr, (unsigned) gAvTexBlocks[i].uid);
            sceGxmUnmapMemory(ptr);
            sceKernelFreeMemBlock(gAvTexBlocks[i].uid);
            gAvTexBlocks[i].base = NULL;
            gAvTexBlocks[i].uid = -1;
            return;
        }
    }
    l_warn("video: texture free for unknown ptr %p (leaking it)", ptr);
}

/**
 * @brief GLES2 program that converts NV12 (Y plane + interleaved UV plane) to
 *        RGB on the GPU, sampling both planes as plain luminance/luminance-alpha
 *        textures -- no CPU-side color conversion needed.
 */
static GLuint gVideoProgram = 0;
static GLint gVideoPosLoc = -1;
static GLint gVideoUvLoc = -1;
static GLint gVideoYTexLoc = -1;
static GLint gVideoUVTexLoc = -1;
static GLuint gVideoYTex = 0;
static GLuint gVideoUVTex = 0;
static unsigned gVideoTexW = 0;
static unsigned gVideoTexH = 0;

static void ensure_video_program() {
    if (gVideoProgram) return;

    static const char *kVideoVS =
        "attribute vec2 aPos;\n"
        "attribute vec2 aUV;\n"
        "varying vec2 vUV;\n"
        "void main() {\n"
        "    gl_Position = vec4(aPos, 0.0, 1.0);\n"
        "    vUV = aUV;\n"
        "}\n";
    static const char *kVideoFS =
        "precision mediump float;\n"
        "varying vec2 vUV;\n"
        "uniform sampler2D uYTex;\n"
        "uniform sampler2D uUVTex;\n"
        "void main() {\n"
        "    float y = texture2D(uYTex, vUV).r;\n"
        "    vec2 uv = texture2D(uUVTex, vUV).ra - vec2(0.5, 0.5);\n"
        "    float r = y + 1.401993 * uv.y;\n"
        "    float g = y - 0.344136 * uv.x - 0.714136 * uv.y;\n"
        "    float b = y + 1.772000 * uv.x;\n"
        "    gl_FragColor = vec4(r, g, b, 1.0);\n"
        "}\n";

    GLuint vs = glCreateShader(GL_VERTEX_SHADER);
    glShaderSource_soloader(vs, 1, &kVideoVS, NULL);
    glCompileShader_soloader(vs);

    GLuint fs = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource_soloader(fs, 1, &kVideoFS, NULL);
    glCompileShader_soloader(fs);

    gVideoProgram = glCreateProgram();
    glAttachShader(gVideoProgram, vs);
    glAttachShader(gVideoProgram, fs);
    glBindAttribLocation(gVideoProgram, 0, "aPos");
    glBindAttribLocation(gVideoProgram, 1, "aUV");
    glLinkProgram_soloader(gVideoProgram);

    glDeleteShader(vs);
    glDeleteShader(fs);

    gVideoPosLoc = 0;
    gVideoUvLoc = 1;
    gVideoYTexLoc = glGetUniformLocation(gVideoProgram, "uYTex");
    gVideoUVTexLoc = glGetUniformLocation(gVideoProgram, "uUVTex");
    l_info("video: GLES2 YUV program ready (program=%u posLoc=%d uvLoc=%d yTexLoc=%d uvTexLoc=%d)",
           gVideoProgram, gVideoPosLoc, gVideoUvLoc, gVideoYTexLoc, gVideoUVTexLoc);
}

#define REAL_SCREEN_W 960
#define REAL_SCREEN_H 544

static bool gFirstDrawLogged = false;
#define FIRST_DRAW_LOG(...) do { if (!gFirstDrawLogged) l_info(__VA_ARGS__); } while (0)

/**
 * @brief Uploads one decoded NV12 frame and draws it letterboxed to fill the
 *        screen, saving/restoring every bit of GL state the engine cares
 *        about around it (this runs mid-frame, borrowed from the engine's own
 *        render loop timing -- see video_play()'s caller in java.c).
 */
static void draw_video_frame(const unsigned char *yuvData, unsigned w, unsigned h) {
    FIRST_DRAW_LOG("video: draw_video_frame ENTER (%ux%u)", w, h);
    ensure_video_program();

    const unsigned char *yPlane = yuvData;
    const unsigned char *uvPlane = yuvData + (size_t) w * h;
    unsigned uvW = w / 2, uvH = h / 2;

    if (!gVideoYTex || gVideoTexW != w || gVideoTexH != h) {
        FIRST_DRAW_LOG("video: creating Y/UV texture storage...");
        if (!gVideoYTex) glGenTextures(1, &gVideoYTex);
        if (!gVideoUVTex) glGenTextures(1, &gVideoUVTex);

        glBindTexture(GL_TEXTURE_2D, gVideoYTex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE, (GLsizei) w, (GLsizei) h, 0, GL_LUMINANCE, GL_UNSIGNED_BYTE, NULL);

        glBindTexture(GL_TEXTURE_2D, gVideoUVTex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE_ALPHA, (GLsizei) uvW, (GLsizei) uvH, 0, GL_LUMINANCE_ALPHA, GL_UNSIGNED_BYTE, NULL);

        gVideoTexW = w;
        gVideoTexH = h;
    }

    glBindTexture(GL_TEXTURE_2D, gVideoYTex);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, (GLsizei) w, (GLsizei) h, GL_LUMINANCE, GL_UNSIGNED_BYTE, yPlane);
    glBindTexture(GL_TEXTURE_2D, gVideoUVTex);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, (GLsizei) uvW, (GLsizei) uvH, GL_LUMINANCE_ALPHA, GL_UNSIGNED_BYTE, uvPlane);

    float srcAspect = (float) w / (float) h;
    float dstAspect = (float) REAL_SCREEN_W / (float) REAL_SCREEN_H;
    float qx0 = 0, qy0 = 0, qx1 = REAL_SCREEN_W, qy1 = REAL_SCREEN_H;
    if (srcAspect > dstAspect) {
        float qh = REAL_SCREEN_W / srcAspect;
        qy0 = (REAL_SCREEN_H - qh) / 2.0f;
        qy1 = qy0 + qh;
    } else {
        float qw = REAL_SCREEN_H * srcAspect;
        qx0 = (REAL_SCREEN_W - qw) / 2.0f;
        qx1 = qx0 + qw;
    }

    float nx0 = qx0 / REAL_SCREEN_W * 2.0f - 1.0f;
    float nx1 = qx1 / REAL_SCREEN_W * 2.0f - 1.0f;
    float ny0 = 1.0f - qy0 / REAL_SCREEN_H * 2.0f;
    float ny1 = 1.0f - qy1 / REAL_SCREEN_H * 2.0f;

    const GLfloat verts[8] = {
        nx0, ny0,  nx1, ny0,  nx0, ny1,  nx1, ny1,
    };
    const GLfloat uvs[8] = {
        0.0f, 0.0f,  1.0f, 0.0f,  0.0f, 1.0f,  1.0f, 1.0f,
    };

    GLboolean savedBlend = glIsEnabled(GL_BLEND);
    GLboolean savedDepthTest = glIsEnabled(GL_DEPTH_TEST);
    GLboolean savedScissor = glIsEnabled(GL_SCISSOR_TEST);
    GLboolean savedCull = glIsEnabled(GL_CULL_FACE);
    GLint savedViewport[4];
    glGetIntegerv(GL_VIEWPORT, savedViewport);
    GLint savedProgram = 0;
    glGetIntegerv(GL_CURRENT_PROGRAM, &savedProgram);
    GLint savedActiveTexture = 0;
    glGetIntegerv(GL_ACTIVE_TEXTURE, &savedActiveTexture);
    glActiveTexture(GL_TEXTURE0);
    GLint savedTex0 = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &savedTex0);
    glActiveTexture(GL_TEXTURE1);
    GLint savedTex1 = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &savedTex1);
    glActiveTexture((GLenum) savedActiveTexture);
    GLint savedArrayBuffer = 0;
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &savedArrayBuffer);
    glBindBuffer(GL_ARRAY_BUFFER, 0);

    // Native resolution, matching gl_init()'s vglInitExtended(0, 960, 544, ...)
    // -- no intermediate downsample FBO in this port, so the quad's clip-space
    // coordinates (computed above from REAL_SCREEN_W/H) map straight to the
    // real framebuffer.
    glViewport(0, 0, REAL_SCREEN_W, REAL_SCREEN_H);
    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_CULL_FACE);

    glUseProgram(gVideoProgram);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, gVideoYTex);
    glUniform1i(gVideoYTexLoc, 0);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, gVideoUVTex);
    glUniform1i(gVideoUVTexLoc, 1);

    glVertexAttribPointer(gVideoPosLoc, 2, GL_FLOAT, GL_FALSE, 0, verts);
    glVertexAttribPointer(gVideoUvLoc, 2, GL_FLOAT, GL_FALSE, 0, uvs);
    glEnableVertexAttribArray(gVideoPosLoc);
    glEnableVertexAttribArray(gVideoUvLoc);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glDisableVertexAttribArray(gVideoPosLoc);
    glDisableVertexAttribArray(gVideoUvLoc);

    glBindBuffer(GL_ARRAY_BUFFER, (GLuint) savedArrayBuffer);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, (GLuint) savedTex1);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, (GLuint) savedTex0);
    glActiveTexture((GLenum) savedActiveTexture);
    glUseProgram((GLuint) savedProgram);
    if (savedBlend) glEnable(GL_BLEND); else glDisable(GL_BLEND);
    if (savedDepthTest) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
    if (savedScissor) glEnable(GL_SCISSOR_TEST); else glDisable(GL_SCISSOR_TEST);
    if (savedCull) glEnable(GL_CULL_FACE); else glDisable(GL_CULL_FACE);
    glViewport(savedViewport[0], savedViewport[1], savedViewport[2], savedViewport[3]);

    gl_swap();
    FIRST_DRAW_LOG("video: gl_swap() returned -- first frame fully presented");
    gFirstDrawLogged = true;
}

/**
 * @brief Dedicated cutscene audio output thread using sceAudioOut (VOICE port).
 */
static pthread_mutex_t gCutAudioLock = PTHREAD_MUTEX_INITIALIZER;
static unsigned char *gCutAudioBuf[2] = { NULL, NULL };
static unsigned gCutAudioBufCap = 0;
static unsigned gCutAudioLen[2] = { 0, 0 };
static int gCutAudioWriteSlot = 0;
static int gCutAudioPort = -1;
static volatile bool gCutAudioQuit = false;

static int cutscene_audio_thread(SceSize args, void *argp) {
    (void) args; (void) argp;
    int slot = 0;
    for (;;) {
        pthread_mutex_lock(&gCutAudioLock);
        unsigned len = gCutAudioLen[slot];
        bool quit = gCutAudioQuit;
        pthread_mutex_unlock(&gCutAudioLock);

        if (len == 0) {
            if (quit)
                break;
            sceKernelDelayThread(500);
            continue;
        }

        if (gCutAudioPort >= 0)
            sceAudioOutOutput(gCutAudioPort, gCutAudioBuf[slot]);
        pthread_mutex_lock(&gCutAudioLock);
        gCutAudioLen[slot] = 0;
        pthread_mutex_unlock(&gCutAudioLock);
        slot ^= 1;
    }
    return 0;
}

static void cutscene_audio_submit(const void *pData, unsigned bytes) {
    if (bytes > gCutAudioBufCap) {
        free(gCutAudioBuf[0]);
        free(gCutAudioBuf[1]);
        gCutAudioBuf[0] = (unsigned char *) malloc(bytes);
        gCutAudioBuf[1] = (unsigned char *) malloc(bytes);
        gCutAudioBufCap = (gCutAudioBuf[0] && gCutAudioBuf[1]) ? bytes : 0;
    }
    if (!gCutAudioBuf[0] || !gCutAudioBuf[1] || gCutAudioBufCap < bytes)
        return;

    for (;;) {
        pthread_mutex_lock(&gCutAudioLock);
        bool free_slot = gCutAudioLen[gCutAudioWriteSlot] == 0;
        if (free_slot) {
            memcpy(gCutAudioBuf[gCutAudioWriteSlot], pData, bytes);
            gCutAudioLen[gCutAudioWriteSlot] = bytes;
        }
        pthread_mutex_unlock(&gCutAudioLock);
        if (free_slot)
            break;
        sceKernelDelayThread(500);
    }
    gCutAudioWriteSlot ^= 1;
}

void video_init() {
    int ret = sceSysmoduleLoadModule(SCE_SYSMODULE_AVPLAYER);
    if (ret < 0) {
        l_error("video: sceSysmoduleLoadModule(AVPLAYER) failed (0x%08X) -- cutscenes will be skipped", (unsigned) ret);
        gModuleLoaded = false;
        return;
    }
    gModuleLoaded = true;
    l_success("video: SceAvPlayer module loaded.");
}

void video_shutdown() {
    if (gVideoYTex) {
        glDeleteTextures(1, &gVideoYTex);
        gVideoYTex = 0;
    }
    if (gVideoUVTex) {
        glDeleteTextures(1, &gVideoUVTex);
        gVideoUVTex = 0;
    }
    gVideoTexW = 0;
    gVideoTexH = 0;
    if (gVideoProgram) {
        glDeleteProgram(gVideoProgram);
        gVideoProgram = 0;
    }
    free(gYuvScratch);
    gYuvScratch = NULL;
    gYuvScratchCap = 0;
    if (gModuleLoaded) {
        sceSysmoduleUnloadModule(SCE_SYSMODULE_AVPLAYER);
        gModuleLoaded = false;
    }
}

/**
 * @brief Plays a video file using SceAvPlayer.
 * @param name Bare filename of the video cutscene to play (as passed to
 *             GLMediaPlayer.loadMovie() on Android, e.g. "intro.m4v").
 */
void video_play(const char *name) {
    if (!gModuleLoaded) {
        l_warn("video: AVPLAYER module not loaded, skipping cutscene request \"%s\"", name ? name : "(null)");
        return;
    }
    if (!name) {
        l_warn("video: video_play() called with a null name, skipping");
        return;
    }

    char path[512];
    bool found = false;
    SceIoStat st;

    // GLMediaPlayer.loadMovie() on Android built its path from
    // "/sdcard/gameloft/games/Gangstar2//" + movieName, i.e. the same
    // external-storage asset root that the toolkit extracted to
    // DATA_PATH "data/" (see java.c's res_open()) -- try that first.
    snprintf(path, sizeof(path), DATA_PATH "data/%s", name);
    if (sceIoGetstat(path, &st) >= 0) {
        found = true;
    } else {
        snprintf(path, sizeof(path), DATA_PATH "%s", name);
        if (sceIoGetstat(path, &st) >= 0) {
            found = true;
        } else {
            snprintf(path, sizeof(path), DATA_PATH "files/%s", name);
            if (sceIoGetstat(path, &st) >= 0) {
                found = true;
            }
        }
    }

    if (!found) {
        l_error("video: file not found for \"%s\" (tried %sdata/%s, %s%s and %sfiles/%s)",
                name, DATA_PATH, name, DATA_PATH, name, DATA_PATH, name);
        return;
    }

    SceAvPlayerInitData init;
    memset(&init, 0, sizeof(init));
    init.memoryReplacement.allocate = av_alloc;
    init.memoryReplacement.deallocate = av_free;
    init.memoryReplacement.allocateTexture = av_alloc_texture;
    init.memoryReplacement.deallocateTexture = av_free_texture;
    init.fileReplacement.objectPointer = &gAvFileCtx;
    init.fileReplacement.open = av_file_open;
    init.fileReplacement.close = av_file_close;
    init.fileReplacement.readOffset = av_file_read;
    init.fileReplacement.size = av_file_size;
    init.eventReplacement.objectPointer = NULL;
    init.eventReplacement.eventCallback = av_event_cb;
    init.basePriority = 0xA0;
    init.numOutputVideoFrameBuffers = 2;
    init.autoStart = SCE_TRUE;
    init.debugLevel = 0;

    SceAvPlayerHandle handle = sceAvPlayerInit(&init);
    if ((unsigned)handle == 0 || (unsigned)handle == 0xFFFFFFFF || ((unsigned)handle & 0xFF000000) == 0x80000000) {
        l_error("video: sceAvPlayerInit failed (0x%08X) for %s", (unsigned) handle, path);
        return;
    }

    if (sceAvPlayerAddSource(handle, path) < 0) {
        l_error("video: sceAvPlayerAddSource failed for %s", path);
        sceAvPlayerClose(handle);
        return;
    }

    l_success("video: playing %s", path);

    int audioPort = -1;
    int audioChannels = 0;
    unsigned audioFrameLen = 0;
    SceUID cutAudioThreadUid = -1;

    bool skipped = false;

    int wait_count = 0;
    while (!sceAvPlayerIsActive(handle) && wait_count < 500) {
        sceKernelDelayThread(10000);
        wait_count++;
    }

    SceCtrlData pad_start;
    sceCtrlPeekBufferPositive(0, &pad_start, 1);
    uint32_t old_pad_buttons = pad_start.buttons;

    l_info("video: loop starting. active=%d, wait_count=%d, initial_pad_buttons=0x%08X",
           sceAvPlayerIsActive(handle), wait_count, (unsigned) old_pad_buttons);

    int frame_count = 0;
    int video_frames = 0, audio_frames = 0;
    bool audioOpenAttempted = false;

    if (!sceAvPlayerIsActive(handle)) {
        l_warn("video: timed out waiting for video decoder to become active (%s)", path);
    }

    while (sceAvPlayerIsActive(handle)) {
        SceCtrlData pad;
        sceCtrlPeekBufferPositive(0, &pad, 1);
        uint32_t pressed = pad.buttons & ~old_pad_buttons;

        if (pressed & (SCE_CTRL_CROSS | SCE_CTRL_START)) {
            l_info("video: skipped by user button press! (pad=0x%08X old_pad=0x%08X pressed=0x%08X, %d loop iteration(s) in)",
                   (unsigned) pad.buttons, (unsigned) old_pad_buttons, (unsigned) pressed, frame_count);
            skipped = true;
            break;
        }
        old_pad_buttons = pad.buttons;

        SceAvPlayerFrameInfo video;
        if (sceAvPlayerGetVideoData(handle, &video)) {
            unsigned w = video.details.video.width;
            unsigned h = video.details.video.height;
            if (++video_frames == 1)
                l_info("video: first video frame decoded (%ux%u, pData=%p)", w, h, video.pData);
            unsigned yuvNeed = w * h + w * h / 2;
            if (yuvNeed > gYuvScratchCap) {
                free(gYuvScratch);
                gYuvScratch = (unsigned char *) malloc(yuvNeed);
                gYuvScratchCap = gYuvScratch ? yuvNeed : 0;
            }
            if (gYuvScratch && gYuvScratchCap >= yuvNeed) {
                kuKernelFlushCaches((void *) video.pData, yuvNeed);
                memcpy(gYuvScratch, video.pData, yuvNeed);
                draw_video_frame(gYuvScratch, w, h);
            }
        }

        SceAvPlayerFrameInfo audio;
        if (sceAvPlayerGetAudioData(handle, &audio)) {
            if (++audio_frames == 1)
                l_info("video: first audio frame decoded (ch=%u rate=%u)",
                       (unsigned) audio.details.audio.channelCount,
                       (unsigned) audio.details.audio.sampleRate);
            if (audioPort < 0 && !audioOpenAttempted) {
                audioOpenAttempted = true;
                audioChannels = audio.details.audio.channelCount;
                SceAudioOutMode mode = (audioChannels >= 2) ? SCE_AUDIO_OUT_MODE_STEREO : SCE_AUDIO_OUT_MODE_MONO;
                audioFrameLen = audio.details.audio.size / (audioChannels * sizeof(int16_t));
                l_info("video: cutscene audio port: %u frames/channel (size=%u bytes, ch=%u)",
                       audioFrameLen, (unsigned) audio.details.audio.size, audioChannels);
                audioPort = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_VOICE, audioFrameLen,
                                                (int) audio.details.audio.sampleRate, mode);
                if (audioPort < 0) {
                    l_warn("video: sceAudioOutOpenPort for cutscene audio failed (0x%08X) -- cutscene audio disabled",
                           (unsigned) audioPort);
                } else {
                    gCutAudioPort = audioPort;
                    gCutAudioWriteSlot = 0;
                    gCutAudioLen[0] = 0;
                    gCutAudioLen[1] = 0;
                    gCutAudioQuit = false;
                    cutAudioThreadUid = sceKernelCreateThread("cutscene audio out", cutscene_audio_thread,
                                                               0x10000100, 0x4000, 0, 0, NULL);
                    if (cutAudioThreadUid >= 0) {
                        sceKernelStartThread(cutAudioThreadUid, 0, NULL);
                    } else {
                        l_warn("video: cutscene audio thread creation failed (0x%08X) -- cutscene audio disabled",
                               (unsigned) cutAudioThreadUid);
                        sceAudioOutReleasePort(audioPort);
                        audioPort = -1;
                        gCutAudioPort = -1;
                    }
                }
            }
            if (audioPort >= 0) {
                cutscene_audio_submit(audio.pData, (unsigned) audio.details.audio.size);
            }
        }

        frame_count++;
        sceKernelDelayThread(1000);
    }

    l_info("video: loop exited! active=%d, iterations=%d, video_frames=%d, audio_frames=%d",
           sceAvPlayerIsActive(handle), frame_count, video_frames, audio_frames);

    if (cutAudioThreadUid >= 0) {
        pthread_mutex_lock(&gCutAudioLock);
        gCutAudioQuit = true;
        pthread_mutex_unlock(&gCutAudioLock);
        sceKernelWaitThreadEnd(cutAudioThreadUid, NULL, NULL);
        sceKernelDeleteThread(cutAudioThreadUid);
    }
    gCutAudioPort = -1;

    if (audioPort >= 0)
        sceAudioOutReleasePort(audioPort);

    sceAvPlayerStop(handle);
    sceAvPlayerClose(handle);

    l_success("video: %s (%s)", skipped ? "skipped" : "finished", path);
}
