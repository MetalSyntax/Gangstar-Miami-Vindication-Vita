/*
 * gamepad_actions.c -- see gamepad_actions.h for the full rationale (Fase 47).
 *
 * All engine facts below are cited from:
 * - decompiled/libGangster2_armeabi/ghidra/out_ghidra.c (CHudManager::load,
 *   CHudManager::update, VirtualButton::update/processTouch/
 *   processTouchRelease, OnFoot/Driving/Flying/Sniper handleVirtualButton)
 * - decompiled/disasm/full_libGangster2.so.md (`002bfb24
 *   <VirtualButton::processTouch(long)>`, `002bfa8c
 *   <VirtualButton::processTouchRelease()>`, `002b5ec4
 *   <CHudManager::update(double)>`)
 * - syms.txt (`nm -D`: every symbol resolved here is a real dynamic export)
 * - decompiled/apk_jadx/.../GameGLSurfaceView.java:64-88 (nativeOnTouch
 *   action codes 1=down / 2=move / 0=up and (action,x,y,pointerId,0,0) order)
 */

#include "utils/gamepad_actions.h"
#include "utils/logger.h"

#include <psp2/ctrl.h>
#include <stdint.h>

#include <so_util/so_util.h>

extern so_module so_mod;

/* Engine touch space. MUST match ENGINE_W/ENGINE_H in source/main.c: the
 * synthesized touches go through the same nativeOnTouch() entry point as
 * the real touchscreen relay, so they live in the same coordinate space. */
#define GA_ENGINE_W 960
#define GA_ENGINE_H 480

/* Pointer ids for synthesized touches. The real-finger relay in main.c
 * owns slots 0..4 (MAX_TOUCH_SLOTS); these start well clear of that range
 * so a physical button can never steal or corrupt a real finger's touch
 * point inside TouchScreenBase (which keys points by id). One id per
 * mapping entry -- Triangle fires two entries (enter-car + enter-shop) and
 * each needs its own simultaneous touch point. */
#define GA_SLOT_BASE 16

/* HudElement vtable slots, as used by CHudManager::update() itself when it
 * dispatches a real touch to a widget (see `002b5ec4`): +0x14 is the
 * interactable/visible predicate consulted before dispatch (draw2d() uses
 * the same slot, so it tracks visibility, including the nearCar/nearShop/
 * nearCover-gated buttons), +0x1c is getTouchRegion() filling the
 * +0x10..0x1c design-space rect. Calling through the vtable (not a fixed
 * symbol) also keeps subclass overrides working if any of these members
 * ever turn out to be a ToggleButton/AnimatedButton instead of a plain
 * VirtualButton. */
#define GA_VT_GATE   (0x14 / 4)
#define GA_VT_REGION (0x1c / 4)

typedef int (*gate_fn)(void *);
typedef void (*region_fn)(float *, void *);
typedef void *(*getinstance_fn)(void);
typedef void (*getscale_fn)(void *, float *, float *);

/* CHudManager::s_hudManager is a plain `CHudManager*` global (BSS) --
 * resolving its symbol gives the address of the pointer, not the instance. */
typedef void *CHudManagerPtr;

static gamepad_touch_fn s_touch = NULL;
static CHudManagerPtr *s_hudManagerAddr = NULL;
static getinstance_fn s_getInstance = NULL;
static getscale_fn s_getScale = NULL;
static int s_ready = 0;

/* One representative CHudManager member offset per VirtualButton instance,
 * grouped by EvVButton::ButtonType::Type id. Taken straight from
 * CHudManager::load() (out_ghidra.c, `CHudManager::load()`): each line is
 * `VirtualButton::VirtualButton(sprite, <frame>, <type>, true)` stored at
 * the cited member offset. Several ids own more than one instance (one per
 * HUD skin/mode) -- the +0x14 gate in pad_press() picks the currently
 * interactable one at press time, so all of them must be listed here:
 *
 *   off    frame  type   meaning
 *   0x30   0x71   2      enter/exit car (driving/flying skins)
 *   0x34   0x70   0      attack (driving/flying skins)
 *   0x38   0x72   4      vehicle special A (driving/flying skins)
 *   0x3c   0x73   5      vehicle special B (driving/flying skins)
 *   0x40   0xb6   4      vehicle special A (alt skin)
 *   0x44   0xb7   5      vehicle special B (alt skin)
 *   0x48   2      2      enter/exit car (alt skin)
 *   0x4c   0x77   3      enter shop (on-foot only)
 *   0x50   3      0      attack (alt skin)
 *   0x5c   0x6a   2      enter/exit car (alt skin)
 *   0x60   0x6b   0      attack (alt skin)
 *   0x64   0xb5   2      enter/exit car (alt skin)
 *   0x68   0xb4   0      attack (alt skin)
 *   0x6c   0xb9   0xe    take cover (on-foot + sniper)
 *   0x70   0xcc   0xf    sprint (on-foot only)
 */
static const int offs_attack[]    = { 0x34, 0x50, 0x60, 0x68 };
static const int offs_entercar[]  = { 0x30, 0x48, 0x5c, 0x64 };
static const int offs_veh_a[]     = { 0x38, 0x40 };
static const int offs_veh_b[]     = { 0x3c, 0x44 };
static const int offs_shop[]      = { 0x4c };
static const int offs_cover[]     = { 0x6c };
static const int offs_sprint[]    = { 0x70 };

struct pad_action {
    uint32_t mask;
    const char *name;
    const int *offs;
    int noffs;
    int slot;
    int active;
    int x;
    int y;
};

/* Physical mapping.
 * - On foot: Cross=attack, Triangle=enter-car/shop, Square=cover, Circle=sprint.
 * - In vehicle: Cross=accelerate, Circle=brake, Triangle=exit-car.
 *   (L/R triggers also act as brake/accelerate for driving convenience).
 * Each entry gates on its button's visibility (e.g. attack is visible on foot
 * and hidden in cars; accelerate is visible in cars and hidden on foot). */
static struct pad_action s_actions[] = {
    { SCE_CTRL_CROSS,    "attack",     offs_attack,   4, GA_SLOT_BASE + 0, 0, 0, 0 },
    { SCE_CTRL_CROSS,    "accelerate", offs_veh_a,    2, GA_SLOT_BASE + 1, 0, 0, 0 },
    { SCE_CTRL_CIRCLE,   "sprint",     offs_sprint,   1, GA_SLOT_BASE + 2, 0, 0, 0 },
    { SCE_CTRL_CIRCLE,   "brake",      offs_veh_b,    2, GA_SLOT_BASE + 3, 0, 0, 0 },
    { SCE_CTRL_TRIANGLE, "enter-car",  offs_entercar, 4, GA_SLOT_BASE + 4, 0, 0, 0 },
    { SCE_CTRL_TRIANGLE, "enter-shop", offs_shop,     1, GA_SLOT_BASE + 5, 0, 0, 0 },
    { SCE_CTRL_SQUARE,   "take-cover", offs_cover,    1, GA_SLOT_BASE + 6, 0, 0, 0 },
    { SCE_CTRL_RTRIGGER, "accelerate", offs_veh_a,    2, GA_SLOT_BASE + 7, 0, 0, 0 },
    { SCE_CTRL_LTRIGGER, "brake",      offs_veh_b,    2, GA_SLOT_BASE + 8, 0, 0, 0 },
};

#define GA_NACTIONS (sizeof(s_actions) / sizeof(s_actions[0]))

/*
 * Fase 51: the on-foot/sniper movement widget. CHudManager::load()
 * (out_ghidra.c) allocates exactly ONE `AnalogStick` (0x8c bytes) and stores
 * it at this offset -- `AnalogStick::AnalogStick(this_00, sprite, 0, 1)`,
 * i.e. frame 0 for the fixed base graphic and frame 1 for the moving knob.
 * It derives from the same `HudElement` base as VirtualButton (its ctor
 * calls `HudElement::HudElement(...)` first), so the same vtable gate
 * (+0x14) and getTouchRegion (+0x1c) mechanism `pad_press()` already uses
 * applies here unchanged -- confirmed by reading the ctor itself, which
 * fills this object's own +0x10..0x1c with a design-space rect computed
 * from the base sprite frame's module dimensions, the exact same field
 * layout VirtualButton uses for its touch region.
 *
 * `AnalogStick::processTouch()` (out_ghidra.c) does NOT hit-test the touch
 * position against the base's region on every call -- it looks up the
 * ALREADY-CAPTURED pointer by id in `TouchScreenBase::s_touchScreenBase` and
 * accumulates the delta between consecutive samples. That is standard touch
 * capture: once a DOWN lands inside the widget's region, subsequent MOVEs
 * for the same pointer id keep going to it regardless of how far the touch
 * point drifts -- exactly like a real thumb dragging a joystick knob past
 * its drawn base circle. So the down position only needs to land inside the
 * region once; everything from there is a plain drag.
 *
 * No separate camera-look control exists in this engine for on-foot or
 * driving play -- confirmed by reading every mode's onEvent()/*ControlHandler
 * in out_ghidra.c: OnFootControlHandler only subscribes to EvStickMove(7)/
 * StickReleased(8)/EvVirtualButton(0xd); FollowCamera::updateAngles() takes
 * no touch input at all (auto-follow behind the player). The two
 * `SlideControl` widgets at CHudManager+0x54/+0x58 (EvLeftRightStickMove/
 * EvUpDownStickMove) are an ALTERNATE STEERING+THROTTLE input while driving
 * (DrivingControlHandler::handleLeftRightStickMove calls the identical
 * Player vtable+0xe8 slot handleWheelTurn() does -- same effect as turning
 * the wheel, not a camera), and AnalogStick itself is reused verbatim by
 * SniperControlHandler::handleStickMove for aim, not a second stick. A
 * physical right-stick "look" therefore has nothing in the original game to
 * map onto outside of a car; see port_progress.md Fase 51 for the full
 * trail. Not wired up here -- there is no HUD element it could drive.
 */
#define GA_OFF_ANALOGSTICK 0x28
#define GA_OFF_WHEEL       0x2c
#define GA_OFF_SLIDECONTROL 0x54

/* Deadzone as a fraction of the raw analog range (SceCtrlData.lx/ly are
 * 0..255, 128 = centered) -- keeps a resting stick (which never sits at
 * exactly 128 on real hardware) from dribbling a tiny constant drag. */
#define GA_STICK_DEADZONE 24

struct stick_state {
    int slot;
    int active;
    int cx, cy;      /* base center, engine touch pixels (fixed while active) */
    float rx, ry;    /* max knob travel per axis, engine touch pixels */
    int last_x, last_y;
};

static struct stick_state s_moveStick  = { GA_SLOT_BASE + 9, 0, 0, 0, 0.0f, 0.0f, 0, 0 };
static struct stick_state s_wheelStick = { GA_SLOT_BASE + 10, 0, 0, 0, 0.0f, 0.0f, 0, 0 };

/*
 * Fase 52 (crash confirmed on hardware, debug_local_052.log +
 * gangstarmiamivindication-psp2core-1789879707-...psp2dmp): pressing Cross
 * to skip the intro video, then having ANY physical button/stick register a
 * fresh edge on the very next frame, data-aborts inside the ENGINE's own
 * `Application::GetScreenDimensions(int&, int&) const`
 * (`_ZNK11Application19GetScreenDimensionsERiS0_`) -- `ldr r0, [r3, #16]`
 * with r3 == 0, called from `Application::GetScreenScaleFactors()`, called
 * from this file's own `pad_press()`/`stick_region()`. The crash PC/disasm
 * pin the field exactly: `r3 = *(Application* + 0x179bc)`, NULL until some
 * point during the engine's own heavy `Application::PostInit()` work
 * (historically observed running inside the SECOND call to nativeRender --
 * port_progress.md Fase 13/48). `CHudManager::s_hudManager` (the `hud` gate
 * below) is already non-NULL by then -- CHudManager is constructed earlier
 * in `Application::Init()` -- so that check alone does not protect this
 * call. Every physical-input path in this file goes through
 * `Application::GetScreenScaleFactors()` to convert a widget's design-space
 * rect to touch pixels, so this one guard, called first, protects all of
 * them (button taps AND the movement stick).
 */
#define GA_SCREENDIM_FIELD_OFF 0x179bc

static int screen_scale_ready(void) {
    void *app = s_getInstance();
    if (!app)
        return 0;
    return *(void **)((char *)app + GA_SCREENDIM_FIELD_OFF) != NULL;
}

static void pad_press(struct pad_action *a) {
    if (!screen_scale_ready())
        return;

    CHudManagerPtr hud = *s_hudManagerAddr;
    if (!hud)
        return;

    float fx = 0.0f, fy = 0.0f;
    s_getScale(s_getInstance(), &fx, &fy);
    if (!(fx > 0.0f && fy > 0.0f))
        return; /* never inject touches computed with a broken scale */

    for (int i = 0; i < a->noffs; i++) {
        int off = a->offs[i];
        void *button = *(void **)((char *)hud + off);
        if (!button)
            continue;
        void **vt = *(void ***)button;

        /* Same interactable check the engine runs before dispatching a
         * real touch to this widget -- hidden/inapplicable skins (wrong
         * mode, not near car/shop/cover) fail here and are skipped. */
        gate_fn gate = (gate_fn)vt[GA_VT_GATE];
        if (!gate || !gate(button))
            continue;

        float rect[4];
        region_fn region = (region_fn)vt[GA_VT_REGION];
        if (!region)
            continue;
        region(rect, button);
        if (!(rect[2] > rect[0] && rect[3] > rect[1]))
            continue;

        /* Design-space rect center -> engine touch pixels, exactly like
         * VirtualButton::update() scales the rect before hit-testing
         * (`__aeabi_fmul(scale, rect)` on each edge). */
        int x = (int)((rect[0] + rect[2]) * 0.5f * fx);
        int y = (int)((rect[1] + rect[3]) * 0.5f * fy);
        if (x < 0 || x >= GA_ENGINE_W || y < 0 || y >= GA_ENGINE_H)
            continue;

        s_touch(&jni, NULL, 1, x, y, (jlong)a->slot, 0, 0);
        a->active = 1;
        a->x = x;
        a->y = y;
        l_note("[input] pad %s down -> vbutton hud+0x%x @(%d,%d) slot %d",
               a->name, off, x, y, a->slot);
        return; /* first interactable skin wins: one tap, never multi-fire */
    }

    l_debug("[input] pad %s down: no interactable vbutton right now", a->name);
}

static void pad_release(struct pad_action *a) {
    if (!a->active)
        return;
    s_touch(&jni, NULL, 0, a->x, a->y, (jlong)a->slot, 0, 0);
    a->active = 0;
    l_note("[input] pad %s up (slot %d)", a->name, a->slot);
}

/* Locates CHudManager+GA_OFF_ANALOGSTICK, runs the same interactable gate
 * VirtualButton uses, and fills *cx/*cy (region center) and *rx/*ry (half
 * the region's width/height -- the knob's usable travel per axis) in engine
 * touch pixels. Returns 0 (out params untouched) if the widget is currently
 * hidden/not interactable (e.g. no AnalogStick in Driving/Flying mode). */
static int stick_region(int *cx, int *cy, float *rx, float *ry) {
    if (!screen_scale_ready())
        return 0;

    CHudManagerPtr hud = *s_hudManagerAddr;
    if (!hud)
        return 0;

    float fx = 0.0f, fy = 0.0f;
    s_getScale(s_getInstance(), &fx, &fy);
    if (!(fx > 0.0f && fy > 0.0f))
        return 0;

    void *stick = *(void **)((char *)hud + GA_OFF_ANALOGSTICK);
    if (!stick)
        return 0;
    void **vt = *(void ***)stick;

    gate_fn gate = (gate_fn)vt[GA_VT_GATE];
    if (!gate || !gate(stick))
        return 0;

    float rect[4];
    region_fn region = (region_fn)vt[GA_VT_REGION];
    if (!region)
        return 0;
    region(rect, stick);
    if (!(rect[2] > rect[0] && rect[3] > rect[1]))
        return 0;

    *cx = (int)((rect[0] + rect[2]) * 0.5f * fx);
    *cy = (int)((rect[1] + rect[3]) * 0.5f * fy);
    *rx = (rect[2] - rect[0]) * 0.5f * fx;
    *ry = (rect[3] - rect[1]) * 0.5f * fy;
    return (*cx >= 0 && *cx < GA_ENGINE_W && *cy >= 0 && *cy < GA_ENGINE_H);
}

/* Locates CHudManager+GA_OFF_WHEEL (or GA_OFF_SLIDECONTROL if using slider steering),
 * runs the interactable gate (+0x14), and fills *cx/*cy (region center) and
 * *rx/*ry in engine touch pixels. Returns 1 if interactable, 0 if hidden (e.g. on foot). */
static int wheel_region(int *cx, int *cy, float *rx, float *ry) {
    if (!screen_scale_ready())
        return 0;

    CHudManagerPtr hud = *s_hudManagerAddr;
    if (!hud)
        return 0;

    float fx = 0.0f, fy = 0.0f;
    s_getScale(s_getInstance(), &fx, &fy);
    if (!(fx > 0.0f && fy > 0.0f))
        return 0;

    const int offs[] = { GA_OFF_WHEEL, GA_OFF_SLIDECONTROL };
    for (int i = 0; i < 2; i++) {
        void *wheel = *(void **)((char *)hud + offs[i]);
        if (!wheel)
            continue;
        void **vt = *(void ***)wheel;

        gate_fn gate = (gate_fn)vt[GA_VT_GATE];
        if (!gate || !gate(wheel))
            continue;

        float rect[4];
        region_fn region = (region_fn)vt[GA_VT_REGION];
        if (!region)
            continue;
        region(rect, wheel);
        if (!(rect[2] > rect[0] && rect[3] > rect[1]))
            continue;

        *cx = (int)((rect[0] + rect[2]) * 0.5f * fx);
        *cy = (int)((rect[1] + rect[3]) * 0.5f * fy);
        *rx = (rect[2] - rect[0]) * 0.5f * fx;
        *ry = (rect[3] - rect[1]) * 0.5f * fy;
        return (*cx >= 0 && *cx < GA_ENGINE_W && *cy >= 0 && *cy < GA_ENGINE_H);
    }
    return 0;
}

static int clampi(int v, int lo, int hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

void gamepad_stick_update(uint32_t dpad_buttons, uint8_t lx, uint8_t ly) {
    if (!s_ready)
        return;

    /* Normalize to [-1, 1], deadzone around the 128 center; D-pad overrides
     * a given axis to full deflection when pressed (checked after the
     * analog read so a digital press always wins ties). Screen Y grows
     * downward, so "up" (ny > 0) must SUBTRACT from the touch Y target. */
    int dxRaw = (int)lx - 128, dyRaw = (int)ly - 128;
    float nx = (dxRaw > GA_STICK_DEADZONE || dxRaw < -GA_STICK_DEADZONE) ? (float)dxRaw / 128.0f : 0.0f;
    float ny = (dyRaw > GA_STICK_DEADZONE || dyRaw < -GA_STICK_DEADZONE) ? -(float)dyRaw / 128.0f : 0.0f;
    if (dpad_buttons & SCE_CTRL_LEFT)  nx = -1.0f;
    if (dpad_buttons & SCE_CTRL_RIGHT) nx = 1.0f;
    if (dpad_buttons & SCE_CTRL_UP)    ny = 1.0f;
    if (dpad_buttons & SCE_CTRL_DOWN)  ny = -1.0f;
    if (nx > 1.0f) nx = 1.0f; else if (nx < -1.0f) nx = -1.0f;
    if (ny > 1.0f) ny = 1.0f; else if (ny < -1.0f) ny = -1.0f;

    int want_active = (nx != 0.0f || ny != 0.0f);

    /* 1. On-foot / sniper: AnalogStick movement */
    int cx, cy; float rx, ry;
    if (stick_region(&cx, &cy, &rx, &ry)) {
        if (s_wheelStick.active) {
            s_touch(&jni, NULL, 0, s_wheelStick.last_x, s_wheelStick.last_y, (jlong)s_wheelStick.slot, 0, 0);
            l_note("[input] wheel up (slot %d)", s_wheelStick.slot);
            s_wheelStick.active = 0;
        }

        if (!want_active) {
            if (s_moveStick.active) {
                s_touch(&jni, NULL, 0, s_moveStick.last_x, s_moveStick.last_y, (jlong)s_moveStick.slot, 0, 0);
                l_note("[input] move-stick up (slot %d)", s_moveStick.slot);
                s_moveStick.active = 0;
            }
            return;
        }

        if (!s_moveStick.active) {
            s_moveStick.cx = cx;
            s_moveStick.cy = cy;
            s_moveStick.rx = rx;
            s_moveStick.ry = ry;
            s_moveStick.last_x = cx;
            s_moveStick.last_y = cy;
            s_touch(&jni, NULL, 1, cx, cy, (jlong)s_moveStick.slot, 0, 0);
            s_moveStick.active = 1;
            l_note("[input] move-stick down @(%d,%d) r=(%.0f,%.0f) slot %d",
                   cx, cy, rx, ry, s_moveStick.slot);
        }

        /* A bit past the widget's own drawn radius (1.25x) so a fully-deflected
         * physical stick reliably reaches whatever internal max-drag clamp
         * AnalogStick::processTouch applies, same margin a real thumb dragging
         * past the base graphic would give it. */
        int tx = clampi((int)(s_moveStick.cx + nx * s_moveStick.rx * 1.25f), 0, GA_ENGINE_W - 1);
        int ty = clampi((int)(s_moveStick.cy - ny * s_moveStick.ry * 1.25f), 0, GA_ENGINE_H - 1);
        if (tx != s_moveStick.last_x || ty != s_moveStick.last_y) {
            s_touch(&jni, NULL, 2, tx, ty, (jlong)s_moveStick.slot, 0, 0);
            s_moveStick.last_x = tx;
            s_moveStick.last_y = ty;
            l_debug("[input] move-stick move @(%d,%d)", tx, ty);
        }
        return;
    }

    /* Transitioning away from on-foot: release move-stick if active */
    if (s_moveStick.active) {
        s_touch(&jni, NULL, 0, s_moveStick.last_x, s_moveStick.last_y, (jlong)s_moveStick.slot, 0, 0);
        l_note("[input] move-stick up (slot %d)", s_moveStick.slot);
        s_moveStick.active = 0;
    }

    /* 2. In vehicle: steering wheel / slide control */
    if (wheel_region(&cx, &cy, &rx, &ry)) {
        int want_steer = (nx != 0.0f);
        if (!want_steer) {
            if (s_wheelStick.active) {
                s_touch(&jni, NULL, 0, s_wheelStick.last_x, s_wheelStick.last_y, (jlong)s_wheelStick.slot, 0, 0);
                l_note("[input] wheel up (slot %d)", s_wheelStick.slot);
                s_wheelStick.active = 0;
            }
            return;
        }

        if (!s_wheelStick.active) {
            s_wheelStick.cx = cx;
            s_wheelStick.cy = cy;
            s_wheelStick.rx = rx;
            s_wheelStick.ry = ry;
            s_wheelStick.last_x = cx;
            s_wheelStick.last_y = cy;
            s_touch(&jni, NULL, 1, cx, cy, (jlong)s_wheelStick.slot, 0, 0);
            s_wheelStick.active = 1;
            l_note("[input] wheel down @(%d,%d) slot %d", cx, cy, s_wheelStick.slot);
        }

        /* 85px deflection reaches full lock (engine threshold is 70px in Wheel+0x68).
         * nx > 0 (right) -> tx > cx -> first.x - current.x < 0 -> dir=0 (right)
         * nx < 0 (left)  -> tx < cx -> first.x - current.x > 0 -> dir=1 (left) */
        int tx = clampi((int)(s_wheelStick.cx + nx * 85.0f), 0, GA_ENGINE_W - 1);
        int ty = s_wheelStick.cy;
        if (tx != s_wheelStick.last_x || ty != s_wheelStick.last_y) {
            s_touch(&jni, NULL, 2, tx, ty, (jlong)s_wheelStick.slot, 0, 0);
            s_wheelStick.last_x = tx;
            s_wheelStick.last_y = ty;
            l_debug("[input] wheel move @(%d,%d) nx=%.2f", tx, ty, nx);
        }
        return;
    }

    /* 3. Neither stick nor wheel is interactable */
    if (s_wheelStick.active) {
        s_touch(&jni, NULL, 0, s_wheelStick.last_x, s_wheelStick.last_y, (jlong)s_wheelStick.slot, 0, 0);
        l_note("[input] wheel up (slot %d)", s_wheelStick.slot);
        s_wheelStick.active = 0;
    }
}

void gamepad_actions_init(gamepad_touch_fn touch) {
    s_touch = touch;
    s_hudManagerAddr = (CHudManagerPtr *) so_symbol(&so_mod, "_ZN11CHudManager12s_hudManagerE");
    s_getInstance = (getinstance_fn) so_symbol(&so_mod, "_ZN11Application11GetInstanceEv");
    s_getScale = (getscale_fn) so_symbol(&so_mod, "_ZNK11Application21GetScreenScaleFactorsERfS0_");

    if (!s_touch || !s_hudManagerAddr || !s_getInstance || !s_getScale) {
        l_warn("gamepad_actions: could not resolve required symbols -- "
               "physical-button gameplay mapping disabled, touchscreen relay still works");
        s_ready = 0;
        return;
    }
    s_ready = 1;
    l_note("gamepad_actions: ready (touch-synthesis onto HUD vbuttons)");
}

void gamepad_actions_update(uint32_t buttons, uint32_t old_buttons) {
    if (!s_ready)
        return;

    for (unsigned i = 0; i < GA_NACTIONS; i++) {
        struct pad_action *a = &s_actions[i];
        int down = (buttons & a->mask) != 0;
        int was = (old_buttons & a->mask) != 0;
        if (down && !was)
            pad_press(a);
        else if (!down && was)
            pad_release(a);
    }
}
