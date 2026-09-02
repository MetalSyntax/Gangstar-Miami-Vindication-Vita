#include "utils/init.h"
#include "utils/glutil.h"
#include "utils/logger.h"
#include "utils/dialog.h"

#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/processmgr.h>
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

    while (1) {
        SceUInt64 frame_start = sceKernelGetProcessTimeWide();

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
        // KEYCODE_BACK = 4 (SCE_CTRL_CIRCLE), the only key code the engine reads outside of touch.
        if ((pad.buttons & SCE_CTRL_CIRCLE) && !(oldButtons & SCE_CTRL_CIRCLE))
            Gangster2_nativeKeyDown(&jni, NULL, 4);
        if (!(pad.buttons & SCE_CTRL_CIRCLE) && (oldButtons & SCE_CTRL_CIRCLE))
            Gangster2_nativeKeyUp(&jni, NULL, 4);
        oldButtons = pad.buttons;

        GameRenderer_nativeRender(&jni, NULL);
        gl_swap();

        // Mirrors GameRenderer.onDrawFrame()'s own 30 FPS pacing (33ms/frame).
        SceUInt64 frame_time = sceKernelGetProcessTimeWide() - frame_start;
        if (frame_time < 33000)
            sceKernelDelayThread(33000 - frame_time);
    }

    sceKernelExitDeleteThread(0);
}
