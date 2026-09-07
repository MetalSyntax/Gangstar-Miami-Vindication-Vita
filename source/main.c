#include "utils/init.h"
#include "utils/glutil.h"
#include "utils/logger.h"
#include "utils/dialog.h"
#include "reimpl/gl.h"

#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/clib.h>
#include <psp2/touch.h>
#include <psp2/ctrl.h>

#include <falso_jni/FalsoJNI.h>
#include <so_util/so_util.h>

int _newlib_heap_size_user = 256 * 1024 * 1024;

#ifdef USE_SCELIBC_IO
int sceLibcHeapSize = 4 * 1024 * 1024;
#endif

so_module so_mod;

/*
 * This .so has no JNI_OnLoad and never calls RegisterNatives -- confirmed with
 * `objdump -T` (see PORTING_PLAN.md section 3). It relies entirely on the static
 * `Java_...` symbol naming convention, so every entry point the real Activity/
 * GLSurfaceView/Renderer would have called is resolved and invoked here by hand,
 * following the exact order confirmed by reading the decompiled Java lifecycle
 * (Gangster2.java / GameRenderer.java / GameGLSurfaceView.java).
 */

typedef void (*fn_void_int)(JNIEnv *, jclass, jint);
typedef void (*fn_void_void)(JNIEnv *, jclass);
typedef void (*fn_void_int_int)(JNIEnv *, jclass, jint, jint);
typedef void (*fn_void_touch)(JNIEnv *, jclass, jint, jint, jint, jlong, jint, jint);
typedef void (*fn_void_instance)(JNIEnv *, jobject);

static fn_void_int      Gangster2_nativeSetPhone;
static fn_void_instance GameRenderer_nativeGetJNIEnv;
static fn_void_int      GLResLoader_nativeInit;
static fn_void_int      GLMediaPlayer_nativeInit;
static fn_void_int      Gangster2_nativeInit;
static fn_void_int      GameRenderer_nativeInit;
static fn_void_int_int  GameRenderer_nativeResize;
static fn_void_void     GameRenderer_nativeRender;
static fn_void_touch    GameGLSurfaceView_nativeOnTouch;
static fn_void_int      Gangster2_nativeKeyDown;
static fn_void_int      Gangster2_nativeKeyUp;

#define RESOLVE(dst, sym) \
    dst = (void *) so_symbol(&so_mod, sym); \
    if (!dst) fatal_error("Could not resolve required symbol: %s", sym);

#define GAME_W 960
#define GAME_H 544
#define MAX_TOUCH_SLOTS 5

int main() {
    soloader_init_all();
    l_checkpoint(1, "main: soloader_init_all() done");

    RESOLVE(Gangster2_nativeSetPhone, "Java_com_gameloft_android_TBFV_GloftGMHP_ML_Gangster2_nativeSetPhone")
    RESOLVE(GameRenderer_nativeGetJNIEnv, "Java_com_gameloft_android_TBFV_GloftGMHP_ML_GameRenderer_nativeGetJNIEnv")
    RESOLVE(GLResLoader_nativeInit, "Java_com_gameloft_android_TBFV_GloftGMHP_ML_GLResLoader_nativeInit")
    RESOLVE(GLMediaPlayer_nativeInit, "Java_com_gameloft_android_TBFV_GloftGMHP_ML_GLMediaPlayer_nativeInit")
    RESOLVE(Gangster2_nativeInit, "Java_com_gameloft_android_TBFV_GloftGMHP_ML_Gangster2_nativeInit")
    RESOLVE(GameRenderer_nativeInit, "Java_com_gameloft_android_TBFV_GloftGMHP_ML_GameRenderer_nativeInit")
    RESOLVE(GameRenderer_nativeResize, "Java_com_gameloft_android_TBFV_GloftGMHP_ML_GameRenderer_nativeResize")
    RESOLVE(GameRenderer_nativeRender, "Java_com_gameloft_android_TBFV_GloftGMHP_ML_GameRenderer_nativeRender")
    RESOLVE(GameGLSurfaceView_nativeOnTouch, "Java_com_gameloft_android_TBFV_GloftGMHP_ML_GameGLSurfaceView_nativeOnTouch")
    RESOLVE(Gangster2_nativeKeyDown, "Java_com_gameloft_android_TBFV_GloftGMHP_ML_Gangster2_nativeKeyDown")
    RESOLVE(Gangster2_nativeKeyUp, "Java_com_gameloft_android_TBFV_GloftGMHP_ML_Gangster2_nativeKeyUp")
    l_checkpoint(2, "main: all Java_* symbols resolved");

    gl_init();
    l_checkpoint(3, "main: gl_init() done");

    // onCreate(): nativeSetPhone(dm.widthPixels), before any GL/surface work.
    Gangster2_nativeSetPhone(&jni, NULL, GAME_W);
    l_checkpoint(4, "main: Gangster2_nativeSetPhone() done");

    // onSurfaceCreated(), in the exact order the real Renderer calls them.
    GameRenderer_nativeGetJNIEnv(&jni, NULL);
    l_checkpoint(5, "main: GameRenderer_nativeGetJNIEnv() done");
    GLResLoader_nativeInit(&jni, NULL, 0);
    l_checkpoint(6, "main: GLResLoader_nativeInit() done");
    GLMediaPlayer_nativeInit(&jni, NULL, 0);
    l_checkpoint(7, "main: GLMediaPlayer_nativeInit() done");
    Gangster2_nativeInit(&jni, NULL, 1); // 1 == demo mode, matches a fresh install's default state
    l_checkpoint(8, "main: Gangster2_nativeInit() done -- most likely place the engine spawns its worker thread(s)");
    GameRenderer_nativeInit(&jni, NULL, 1);
    l_checkpoint(9, "main: GameRenderer_nativeInit() done");

    // onSurfaceChanged(w, h)
    GameRenderer_nativeResize(&jni, NULL, GAME_W, GAME_H);
    l_checkpoint(10, "main: GameRenderer_nativeResize() done -- entering main loop");

    sceTouchSetSamplingState(SCE_TOUCH_PORT_FRONT, SCE_TOUCH_SAMPLING_STATE_START);

    int touchSlotHwId[MAX_TOUCH_SLOTS] = {-1, -1, -1, -1, -1};
    int touchLastX[MAX_TOUCH_SLOTS] = {-1, -1, -1, -1, -1};
    int touchLastY[MAX_TOUCH_SLOTS] = {-1, -1, -1, -1, -1};

    SceCtrlData pad;
    uint32_t oldButtons = 0;

    // Frame-progress instrumentation (id 22, otherwise unused): distinguishes
    // "stuck inside the Nth nativeRender" (only the 'enter' line appears) from
    // "loop alive but rendering black" (frame N lines keep appearing).
    //
    // This uses l_note(), NOT l_checkpoint()/l_debug(), on purpose: a Release
    // build compiles l_debug out, and a release run is exactly the situation
    // where this question ("is the render loop even turning?") is the first
    // thing worth answering. Volume is bounded -- the first 10 frames in
    // detail, then a time-based heartbeat (at most one line every 5 s) plus a
    // line for any frame whose nativeRender takes > 0.5 s (asset loading shows
    // up as slow frames -- frame 2 took ~7.8 s in logs/debug_local_020.log).
    // A frame-count modulo would go silent for minutes while loading, because
    // a single nativeRender can take seconds; wall-clock time does not.
    int frame_no = 0;
    SceUInt64 last_beat = 0;
    int last_beat_frame = 0;

    while (1) {
        frame_no++;
        SceUInt64 frame_start = sceKernelGetProcessTimeWide();
        if (frame_no <= 10)
            l_note("[022] main loop: frame %d enter (t=%llu)", frame_no, frame_start);

        SceTouchData touch;
        sceTouchPeek(SCE_TOUCH_PORT_FRONT, &touch, 1);

        int seen[MAX_TOUCH_SLOTS] = {0, 0, 0, 0, 0};
        for (int r = 0; r < touch.reportNum && r < MAX_TOUCH_SLOTS; r++) {
            int hwId = touch.report[r].id;
            int x = touch.report[r].x * GAME_W / 1920;
            int y = touch.report[r].y * GAME_H / 1088;

            int slot = -1;
            for (int s = 0; s < MAX_TOUCH_SLOTS; s++) {
                if (touchSlotHwId[s] == hwId) { slot = s; break; }
            }
            if (slot == -1) {
                for (int s = 0; s < MAX_TOUCH_SLOTS; s++) {
                    if (touchSlotHwId[s] == -1) { slot = s; break; }
                }
                if (slot == -1) continue;
                touchSlotHwId[slot] = hwId;
                touchLastX[slot] = -1;
                touchLastY[slot] = -1;
            }
            seen[slot] = 1;

            if (touchLastX[slot] == -1) {
                GameGLSurfaceView_nativeOnTouch(&jni, NULL, 1, x, y, slot, 0, 0);
            } else if (touchLastX[slot] != x || touchLastY[slot] != y) {
                GameGLSurfaceView_nativeOnTouch(&jni, NULL, 2, x, y, slot, 0, 0);
            }
            touchLastX[slot] = x;
            touchLastY[slot] = y;
        }
        for (int s = 0; s < MAX_TOUCH_SLOTS; s++) {
            if (touchSlotHwId[s] != -1 && !seen[s]) {
                GameGLSurfaceView_nativeOnTouch(&jni, NULL, 0, touchLastX[s], touchLastY[s], s, 0, 0);
                touchLastX[s] = -1;
                touchLastY[s] = -1;
                touchSlotHwId[s] = -1;
            }
        }

        sceCtrlPeekBufferPositive(0, &pad, 1);
        // Android keycodes forwarded through the same s_keyDownCode/s_keyUpCode
        // path the real Activity uses (GameRenderer.onDrawFrame consumes them):
        // KEYCODE_BACK = 4, KEYCODE_DPAD_UP/DOWN/LEFT/RIGHT = 19/20/21/22,
        // KEYCODE_DPAD_CENTER = 23 ("confirm"), KEYCODE_MENU = 82. The engine
        // ignores codes it does not bind, so unmapped buttons are harmless --
        // same as pressing them on a real keyboard-equipped device.
        static const struct { uint32_t btn; int code; } keymap[] = {
            { SCE_CTRL_UP,    19 },
            { SCE_CTRL_DOWN,  20 },
            { SCE_CTRL_LEFT,  21 },
            { SCE_CTRL_RIGHT, 22 },
            { SCE_CTRL_CROSS, 23 },
            { SCE_CTRL_CIRCLE, 4 },
            { SCE_CTRL_START, 82 },
        };
        for (unsigned k = 0; k < sizeof(keymap) / sizeof(keymap[0]); k++) {
            if ((pad.buttons & keymap[k].btn) && !(oldButtons & keymap[k].btn))
                Gangster2_nativeKeyDown(&jni, NULL, keymap[k].code);
            if (!(pad.buttons & keymap[k].btn) && (oldButtons & keymap[k].btn))
                Gangster2_nativeKeyUp(&jni, NULL, keymap[k].code);
        }
        oldButtons = pad.buttons;

        GameRenderer_nativeRender(&jni, NULL);
        SceUInt64 render_us = sceKernelGetProcessTimeWide() - frame_start;
        if (frame_no <= 10) {
            l_note("[022] main loop: frame %d returned (%llu us)", frame_no, render_us);
        } else {
            if (render_us > 500000)
                l_note("[022] main loop: frame %d slow render (%llu ms)", frame_no, render_us / 1000);
            SceUInt64 now = sceKernelGetProcessTimeWide();
            if (last_beat == 0) {
                last_beat = now;
                last_beat_frame = frame_no;
            }
            if (now - last_beat > 5000000) {
                // Human-readable heartbeat (2026-09-06): fps measured between
                // beats (not estimated), render cost of this frame, and total
                // GL draws/clears -- one line every 5 s answers "is anything
                // happening?" without reading tea leaves from microsecond
                // timings.
                unsigned draws = 0, clears = 0;
                gl_get_counters(&draws, &clears);
                float fps = (float)(frame_no - last_beat_frame) * 1000000.0f /
                            (float)(now - last_beat);
                l_note("[022] frame %d | %.1f fps | render %.1f ms | draws %u clears %u",
                       frame_no, fps, (float)render_us / 1000.0f, draws, clears);
                last_beat = now;
                last_beat_frame = frame_no;
            }
        }
        gl_swap();

        // TEMP triage (black screen with live draws, 2026-09-06): one BMP of
        // the displayed framebuffer every 600 frames (~20 s at 30 fps) so the
        // log run also produces pictures -- fetch
        // ux0:data/gangstarmiamivindication/logs/shot_*.bmp over FTP and look
        // at them instead of guessing from counters. Remove with the triage.
        if (frame_no > 100 && frame_no % 600 == 0) {
            char shot[128];
            sceClibSnprintf(shot, sizeof(shot), DATA_PATH "logs/shot_%05d.bmp", frame_no);
            int sr = gl_shot(shot);
            l_note("[022] screenshot %s -> %d", shot, sr);
        }

        // Mirrors GameRenderer.onDrawFrame()'s own 30 FPS pacing (33ms/frame).
        SceUInt64 frame_time = sceKernelGetProcessTimeWide() - frame_start;
        if (frame_time < 33000)
            sceKernelDelayThread(33000 - frame_time);
    }

    sceKernelExitDeleteThread(0);
}
