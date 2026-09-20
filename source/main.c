#include "utils/init.h"
#include "utils/glutil.h"
#include "utils/logger.h"
#include "utils/dialog.h"
#include "utils/audio.h"
#include "utils/gamepad_actions.h"
#include "reimpl/gl.h"
#include "video.h"

#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/clib.h>
#include <psp2/touch.h>
#include <psp2/ctrl.h>

#include <falso_jni/FalsoJNI.h>
#include <so_util/so_util.h>

int _newlib_heap_size_user = 256 * 1024 * 1024;

#ifdef USE_SCELIBC_IO
int sceLibcHeapSize = 16 * 1024 * 1024;
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

#define SCREEN_W 960
#define SCREEN_H 544

// The game engine operates internally at 960x480 (hardcoded height 480 in
// Application::Init and GameRenderer_nativeResize). glViewport_soloader (gl.c)
// scales the default framebuffer vertically from 480 to 544 to fill the Vita
// panel. Touch inputs from the Vita panel (960x544) must therefore be mapped to
// the engine's internal space (960x480) so touch hitboxes match visual buttons.
#define ENGINE_W 960
#define ENGINE_H 480
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

    // Fase 47: maps physical buttons onto the engine's own on-screen virtual
    // HUD buttons (attack, enter car/shop, take cover, sprint, ...) by
    // synthesizing real touches at the active button's position -- see
    // gamepad_actions.h. Gets the same nativeOnTouch() entry point the
    // touchscreen relay below uses (dedicated pointer ids 16+, real fingers
    // own slots 0-4). Soft-fails (touchscreen relay below still works)
    // if the engine's internal symbols ever move.
    gamepad_actions_init(GameGLSurfaceView_nativeOnTouch);

    gl_init();
    l_checkpoint(3, "main: gl_init() done");

    // Loads SceAvPlayer so Method_loadMovie (java.c) can actually play
    // intro.m4v instead of skipping straight to completion. Must come after
    // gl_init(): video_play()'s texture allocator maps memory via
    // sceGxmMapMemory, which needs the GXM context vitaGL's init brings up.
    video_init();

    // onCreate(): nativeSetPhone(dm.widthPixels), before any GL/surface work.
    Gangster2_nativeSetPhone(&jni, NULL, ENGINE_W);
    l_checkpoint(4, "main: Gangster2_nativeSetPhone() done");

    // onSurfaceCreated(), in the exact order the real Renderer calls them.
    GameRenderer_nativeGetJNIEnv(&jni, NULL);
    l_checkpoint(5, "main: GameRenderer_nativeGetJNIEnv() done");
    GLResLoader_nativeInit(&jni, NULL, 0);
    l_checkpoint(6, "main: GLResLoader_nativeInit() done");
    GLMediaPlayer_nativeInit(&jni, NULL, 0);
    l_checkpoint(7, "main: GLMediaPlayer_nativeInit() done");
    // Fase 31: no hay VM Java que ejecute GLMediaPlayer.init(), asi que el
    // SoundPool/MediaPlayer nunca se crearia solo. Levantar el backend real
    // (SceAudioOut + vorbis) aqui; es barato y sin el los queries de estado
    // harian que el motor reintentara la radio cada frame (ver audio.c).
    audio_init();
    Gangster2_nativeInit(&jni, NULL, 1); // 1 == demo mode, matches a fresh install's default state
    l_checkpoint(8, "main: Gangster2_nativeInit() done -- most likely place the engine spawns its worker thread(s)");
    GameRenderer_nativeInit(&jni, NULL, 1);
    l_checkpoint(9, "main: GameRenderer_nativeInit() done");

    // onSurfaceChanged(w, h)
    GameRenderer_nativeResize(&jni, NULL, ENGINE_W, ENGINE_H);
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
            int x = (int)((touch.report[r].x * ENGINE_W) / 1920);
            int y = (int)((touch.report[r].y * ENGINE_H) / 1088);
            if (x < 0) x = 0;
            else if (x >= ENGINE_W) x = ENGINE_W - 1;
            if (y < 0) y = 0;
            else if (y >= ENGINE_H) y = ENGINE_H - 1;

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
        // path the real Activity uses (GameRenderer.onDrawFrame consumes them).
        // Decompiled reality check (Fase 47): Application::DeviceKeyPress
        // only reacts to KEYCODE_BACK = 4 and KEYCODE_MENU = 82 -- every
        // other code (including DPAD 19-22 and DPAD_CENTER 23) hits an
        // immediate `return` and is a confirmed no-op, in menus and in game.
        // Kept for parity with a real keyboard-equipped device; gameplay
        // input for the mapped buttons goes through gamepad_actions_update()
        // below, not through here.
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
        // Fase 47: Cross/Triangle/Square/Circle/L/R synthesize real taps on
        // the engine's own on-screen action buttons (attack, enter car/shop,
        // take cover, sprint, vehicle-only extras) via gamepad_actions.c --
        // down on press, up on release, at the currently visible skin's
        // position. Independent of the keycode loop above (CROSS/CIRCLE also
        // send keycodes 23/4 there, which the engine ignores outside menus).
        gamepad_actions_update(pad.buttons, oldButtons);
        // Fase 51: physical movement input (D-pad + left analog stick) was
        // never wired to anything -- the keycode loop above only reaches
        // KEYCODE_BACK/MENU (Fase 47), and nothing else in this file drove
        // the engine's on-screen movement AnalogStick. See
        // gamepad_actions.c's GA_OFF_ANALOGSTICK comment for why a synthetic
        // touch-drag on that HUD widget is the only path that works, and
        // why there is no equivalent camera-look control to wire the right
        // stick to (this engine's on-foot/driving camera is fully
        // automatic -- confirmed in the decompiled sources, not a gap in
        // this port).
        gamepad_stick_update(pad.buttons & (SCE_CTRL_UP | SCE_CTRL_DOWN | SCE_CTRL_LEFT | SCE_CTRL_RIGHT),
                              pad.lx, pad.ly);
        oldButtons = pad.buttons;

        GameRenderer_nativeRender(&jni, NULL);
        // Fase 31: evitar que el governor baje relojes / suspenda durante
        // cargas largas (el frame 2 y la rafaga de shaders tardan minutos).
        sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DEFAULT);
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
                float fps = (float)(frame_no - last_beat_frame) * 1000000.0f /
                            (float)(now - last_beat);
                l_note("[022] frame %d | %.1f fps | render %.1f ms",
                       frame_no, fps, (float)render_us / 1000.0f);
                last_beat = now;
                last_beat_frame = frame_no;
            }
        }
        gl_swap();

        // TEMP triage (black screen with live draws, 2026-09-06): one BMP of
        // the displayed framebuffer -- fetch
        // ux0:data/gangstarmiamivindication/logs/shot_*.bmp over FTP and look
        // at them instead of guessing from counters.
        // Fase 25: frame 150 cae en la ventana del splash (tras la carga del
        // frame 2, antes de la rafaga de shaders ~185); luego cada 300.
        //
        // Fase 30 (2026-09-10): apagado por default. debug_local_035.log
        // muestra que CADA captura (gl_shot: 2 MB, ~522240 px escritos fila
        // por fila con sceIoWrite bloqueante en source/utils/glutil.c) coincide
        // 1:1 con un pozo de fps en la ventana de 5 s siguiente (0.6-2.7 fps,
        // ver frames 151/1201/1801/2101/2401/2701/3001/4201 -- todos justo
        // después de un "screenshot shot_*.bmp" log, con el render normal
        // (7-10 ms) intacto: el costo no está en dibujar, está en el I/O
        // síncrono de la captura). Esa misma pausa de 1-2 s cada ~10 s
        // (300 frames a 30 fps) es la causa más probable de que el táctil
        // "deje de responder a veces" -- si el toque llega durante el I/O
        // bloqueante, no hay vuelta de main loop para leerlo hasta que
        // termina. Ya cumplió su misión (Fase 26 confirmó la imagen real);
        // reactivar con GMV_SHOT_TRIAGE=1 solo si hace falta ver el
        // framebuffer de nuevo para triage.
#define GMV_SHOT_TRIAGE 0
#if GMV_SHOT_TRIAGE
        if (frame_no == 150 || (frame_no > 150 && frame_no % 300 == 0)) {
            char shot[128];
            sceClibSnprintf(shot, sizeof(shot), DATA_PATH "logs/shot_%05d.bmp", frame_no);
            int sr = gl_shot(shot);
            l_note("[022] screenshot %s -> %d", shot, sr);
        }
#endif

        // Fase 49 (2026-09-19): the Android-side `Thread.sleep(33 - elapsed)`
        // this used to mirror (GameRenderer.onDrawFrame(), decompiled Java)
        // is a battery/thermal throttle for phone hardware, not a simulation
        // requirement -- confirmed in the decompiled source: onDrawFrame()
        // measures `System.currentTimeMillis()` itself and only sleeps the
        // *leftover* time, never enforcing a fixed step when a frame runs
        // long. The engine is delta-time driven, not tick-locked.
        //
        // On Vita this cap was pure waste stacked on top of a real limiter
        // that was already there: gl_swap() -> vglSwapBuffers() ->
        // scene_end() calls sceDisplayWaitVblankStartMulti(vsync_interval)
        // (lib/vitaGL/source/gxm.c, vsync_interval=1 by default) every frame,
        // which already blocks this thread until the next 60 Hz vblank. That
        // is the correct place to pace frames -- unlike the old
        // sceKernelDelayThread(33000 - elapsed) here, it can never leave the
        // CPU idle-spinning past the display's actual refresh, and it does
        // not clamp a fast frame down to 30 fps. Removing the extra sleep
        // lets any scene that renders in under 16.6 ms present at a full
        // 60 fps instead of being held to 30; scenes that are already
        // GPU/CPU-bound past 33 ms are unaffected either way.
        //
        // sceKernelPowerTick() above still runs every iteration regardless,
        // so this does not reintroduce the governor/suspend risk from Fase 31.
    }

    sceKernelExitDeleteThread(0);
}
