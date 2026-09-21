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
#include <psp2/kernel/processmgr.h>
#include <stdint.h>
#include <stdio.h>

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
static int s_alphaFull = 0;

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
#define GA_OFF_SLIDECONTROL2 0x58

/*
 * Touch-control opacity. `HudElement::HudElement(ASprite*, int, bool)`
 * (out_ghidra.c: `*(undefined4 *)(this + 0x34) = 100;`) sets a per-instance
 * "floor" alpha at this+0x34. `HudElement::setAlpha()` (raw disasm:
 * decompiled/disasm/full_libGangster2.so.md, `002ba98c
 * <HudElement::setAlpha() const>`; called from `draw2d()`, 2bab68, right
 * before `ASprite::PaintFrame()`) is re-run every frame and re-derives the
 * actual drawn RGBA quad bytes (this+0x44/0x48/0x4c/0x50, alpha at the +3 of
 * each: 0x47/0x4b/0x4f/0x53) and their int mirror at this+0x54.
 *
 * Re-derived from the RAW disassembly (not the Ghidra pseudo-C, which
 * mis-decompiles part of this -- see below), setAlpha()'s branches on the
 * flags word at this+0xc are:
 *   - bit 5 (mask 0x20): forces the quads to solid white, alpha=0xA0 (160)
 *     fixed. Grep-verified (out_ghidra.c, every HudElement/VirtualButton/
 *     AnalogStick/Wheel/SlideControl function): NOTHING in this binary ever
 *     sets bit 5 on a touch-control HudElement. Dead branch for our widgets.
 *   - bit 4 (mask 0x10, confirmed = "blink" via `HudElement::isBlinking()`
 *     `(flags<<0x1b)>>0x1f` and `HudElement::blink(bool,bool)` orr/bic #0x10,
 *     out_ghidra.c): only toggled by `CHudManager::blink(int)` for specific
 *     tutorial/hint pulses (out_ghidra.c lines ~66952-66991), not during
 *     ordinary idle rendering. When set, sub-branches: 2ba9fe/2baa00 test
 *     BIT 8 (mask 0x100) not of this+0xc but of R0, the return value of a
 *     virtual call made earlier in setAlpha() (2ba99c-2ba9a2, through
 *     `(*(Application::GetInstance()+0x179bc))->vtable[4]()`) -- this is
 *     what Ghidra mis-decompiles as a standalone `iVar1`; it is NOT a flags
 *     bit at all. Also dead for our widgets in their normal idle state.
 *   - bit 1 (mask 0x2, "used"/pressed, `HudElement::isUsed()`/`setUsed()`)
 *     combined with bit 2 (mask 0x4, set to 1 at construction for every
 *     interactive HudElement, out_ghidra.c ctor, never cleared elsewhere):
 *     tested as `flags & 6`. While NOT pressed, flags&6==4 -> falls into a
 *     fade/"drag" sub-branch (2baa5e) that decrements this+0x54 by
 *     `255.0f / this+0x3c` per frame. Grep-verified: this+0x3c is set to 0
 *     in the constructor and NEVER written anywhere else in the whole
 *     binary for HudElement/VirtualButton/AnalogStick/Wheel/SlideControl,
 *     so this division is a 255/0 divide-by-zero EVERY frame, degenerates
 *     to -infinity, and the result always clamps to this+0x34, deterministically
 *     writing this+0x34's value into this+0x54 AND the RGBA quads (2bab00-
 *     2bab0e) every single frame the widget is idle. While pressed (bit1
 *     set, flags&6==6), setAlpha() instead hardcodes this+0x54=0xff (2baa58)
 *     and RETURNS WITHOUT touching the RGBA quad bytes for that frame.
 *
 * Net result: for a touch control at rest (not pressed, not blinking, not
 * sniper/camera hud), this+0x34 really is the field that ends up in the
 * drawn alpha, every frame -- confirmed end to end, not just at the ctor.
 * We poke it in addition to writing the quad alpha bytes and this+0x54
 * directly (belt and suspenders for the one branch above that skips
 * re-deriving them from this+0x34, i.e. while actively pressed).
 *
 * Call order (verified in source/main.c): gamepad_actions_update() runs at
 * line ~255, GameRenderer_nativeRender() (which pumps CHudManager::update()
 * -> draw2d() -> setAlpha() for that same frame) at line ~270, in the same
 * `while(1)` iteration -- our poke always lands before that frame's
 * setAlpha() call, never after.
 *
 * VirtualButton, AnalogStick and Wheel/SlideControl all derive from
 * HudElement without overriding this field, so every touch control CHudManager
 * owns shares the same this+0x34 byte (and the same quad-byte layout) at the
 * same offsets -- one poke, applied every frame, covers all of them.
 */
#define GA_ALPHA_OFFSET      0x34
#define GA_ALPHA_INT_OFFSET  0x54  /* this+0x54: int mirror setAlpha() itself writes/reads */

/* Draw-visibility flag word (this+0x8, bit31). Fase 59: this is the REAL
 * hide switch -- every touch-widget draw2d() in the binary gates its
 * whole PaintFrame on it (HudElement::draw2d, AnalogStick::draw2d,
 * SlideControl::draw2d AND Wheel::draw2d all start with
 * `if (*(this+8) << 31 < 0)`, verified in out_ghidra.c), while every
 * touch/input path gates on this+0xc instead (VirtualButton::update and
 * AnalogStick::update check in_r0[3]<<31, isVisible() returns
 * *(this+0xc)&1, processTouch needs bit0). So clearing this bit hides
 * the widget from the renderer every frame -- immune to the pressed
 * branch (setAlpha writes 0x54=0xff for ANY flags&6 != 4, i.e. no flag
 * combination can hide a pressed widget through alpha) and to the
 * processTouchRelease direct 0x54=0xff write -- while synthesized and
 * real touches keep working untouched. L+R sets it back (fully visible
 * + touchable, the pre-Fase-54 look). */
#define GA_DRAWVIS_OFFSET    0x8
#define GA_DRAWVIS_BIT       0x80000000u
#define GA_ALPHA_HIDDEN      3     /* Fase 58: ~1% of the 0..255 range (1% = 2.55) -- virtual
                                    * buttons hidden by request, physical controls drive */
#define GA_ALPHA_FULL        255
#define GA_ALPHA_COMBO_MASK  (SCE_CTRL_LTRIGGER | SCE_CTRL_RTRIGGER)

/* Vertex-color alpha byte of each of the 4 RGBA quads HudElement::setAlpha()
 * writes (this+0x44/0x48/0x4c/0x50, alpha at the +3 of each -- raw disasm,
 * e.g. 2ba9b0/2baa0e/2baac4/2ba9c2 all `strb <alpha>, [r4, #0x47/0x4b/0x4f/0x53]`). */
static const int s_alpha_quad_offsets[] = { 0x47, 0x4b, 0x4f, 0x53 };
#define GA_NALPHA_QUAD_OFFSETS (sizeof(s_alpha_quad_offsets) / sizeof(s_alpha_quad_offsets[0]))

static const int s_alpha_offsets[] = {
    GA_OFF_ANALOGSTICK, GA_OFF_WHEEL, GA_OFF_SLIDECONTROL, GA_OFF_SLIDECONTROL2,
    0x30, 0x34, 0x38, 0x3c, 0x40, 0x44, 0x48, 0x4c, 0x50, 0x5c, 0x60, 0x64, 0x68, 0x6c, 0x70,
};
#define GA_NALPHA_OFFSETS (sizeof(s_alpha_offsets) / sizeof(s_alpha_offsets[0]))

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

/* Locates CHudManager+GA_OFF_WHEEL (or either SlideControl alternate at
 * +0x54/+0x58 if the wheel skin itself is hidden), runs the interactable
 * gate (+0x14), and fills *cx/*cy (region center) and *rx/*ry in engine
 * touch pixels. Returns 1 if interactable, 0 if hidden (e.g. on foot).
 * Fase 58: the second SlideControl at +0x58 is now a candidate too -- the
 * old code only tried +0x54, so a driving skin steering through +0x58 was
 * never found and the wheel "didn't move". */
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

    const int offs[] = { GA_OFF_WHEEL, GA_OFF_SLIDECONTROL, GA_OFF_SLIDECONTROL2 };
    for (int i = 0; i < 3; i++) {
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

/* Fase 59: steering input is present but NO wheel/slide candidate passed
 * the lookup -- log WHY, throttled to one line per 5s of wall time (never
 * per frame: logging changes timing under test). Log 058 proved the
 * symptom (DeviceKeyInput:21/22 while driving with zero "wheel down"
 * lines) but not which lookup step fails per skin (null widget? +0x14
 * gate? degenerate rect? out-of-range center?), so this line carries all
 * three per candidate: ptr=0/1 gate=0/1 + the raw design rect. One line
 * per 5s is enough to catch every driving skin the user tries. */
static void wheel_miss_diag(float nx) {
    static uint64_t last_us = 0;
    uint64_t now = sceKernelGetProcessTimeWide();
    if (last_us != 0 && now - last_us < 5000000)
        return;
    last_us = now;

    CHudManagerPtr hud = s_hudManagerAddr ? *s_hudManagerAddr : NULL;
    if (!hud) {
        l_note("[input] wheel miss (nx=%.2f): hud=NULL", nx);
        return;
    }
    float fx = 0.0f, fy = 0.0f;
    if (screen_scale_ready())
        s_getScale(s_getInstance(), &fx, &fy);

    static const int offs[] = { GA_OFF_WHEEL, GA_OFF_SLIDECONTROL, GA_OFF_SLIDECONTROL2 };
    char detail[192];
    int pos = 0;
    for (int i = 0; i < 3 && pos < (int)sizeof(detail) - 32; i++) {
        void *w = *(void **)((char *)hud + offs[i]);
        int has = (w != NULL);
        int gate = 0;
        float rect[4] = { 0, 0, 0, 0 };
        if (has) {
            void **vt = *(void ***)w;
            gate_fn g = (gate_fn)vt[GA_VT_GATE];
            gate = (g && g(w)) ? 1 : 0;
            if (gate) {
                region_fn r = (region_fn)vt[GA_VT_REGION];
                if (r) {
                    r(rect, w);
                    if (!(rect[2] > rect[0] && rect[3] > rect[1]))
                        gate = 2;  /* gate ok, rect degenerate */
                } else {
                    gate = 3;  /* gate ok, no region fn */
                }
            }
        }
        pos += snprintf(detail + pos, sizeof(detail) - (size_t)pos,
                        "%s+0x%x:ptr=%d gate=%d rect=(%.0f,%.0f,%.0f,%.0f)",
                        i ? " " : "", offs[i], has, gate,
                        rect[0], rect[1], rect[2], rect[3]);
    }
    l_note("[input] wheel miss (nx=%.2f, scale=%.2fx%.2f): %s", nx, fx, fy, detail);
}

/* Hides (or, with L+R held, restores) every touch control, every frame.
 * Fase 59: two layers. (1) The draw-visibility bit (this+8 bit31, see
 * GA_DRAWVIS_OFFSET): cleared = draw2d() skips the widget entirely, no
 * PaintFrame at all -- this is what keeps buttons hidden "aunque cambie
 * de estado", because NO alpha value can do that: setAlpha()'s pressed
 * branch (raw disasm 2baa50-2baa5c: any flags&6 != 4) forces 0x54=0xff
 * every held frame, and VirtualButton::processTouchRelease() writes
 * 0x54=0xff directly on every release, both AFTER our poke runs. Touch
 * dispatch is unaffected: it gates on this+0xc (update/isVisible/
 * processTouch), a different word we never touch. (2) The alpha stamps
 * (this+0x34 floor, this+0x54 mirror, 4 quad bytes) stay as
 * belt-and-suspenders for any draw path that doesn't go through the
 * draw2d gate. No screen-scale gate needed here (unlike
 * pad_press/stick_region/wheel_region) -- this only touches each
 * widget's own fields, not Application::GetScreenScaleFactors(). */
static void apply_touch_alpha(void) {
    CHudManagerPtr hud = *s_hudManagerAddr;
    if (!hud)
        return;

    uint32_t alpha = s_alphaFull ? GA_ALPHA_FULL : GA_ALPHA_HIDDEN;
    uint8_t alphaByte = (uint8_t)alpha;

    for (unsigned i = 0; i < GA_NALPHA_OFFSETS; i++) {
        void *widget = *(void **)((char *)hud + s_alpha_offsets[i]);
        if (!widget)
            continue;

        uint32_t *drawvis = (uint32_t *)((char *)widget + GA_DRAWVIS_OFFSET);
        if (s_alphaFull)
            *drawvis |= GA_DRAWVIS_BIT;   /* L+R: visible again */
        else
            *drawvis &= ~GA_DRAWVIS_BIT;  /* hidden: draw2d() skips PaintFrame */

        *(uint32_t *)((char *)widget + GA_ALPHA_OFFSET) = alpha;
        *(uint32_t *)((char *)widget + GA_ALPHA_INT_OFFSET) = alpha;
        for (unsigned q = 0; q < GA_NALPHA_QUAD_OFFSETS; q++)
            *((uint8_t *)widget + s_alpha_quad_offsets[q]) = alphaByte;
    }
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

    /* Fase 58: query BOTH widgets up front. On foot the movement AnalogStick
     * is interactable and the Wheel is hidden, in a vehicle it is the other
     * way around -- but during HUD transitions both gates can briefly report
     * interactable, and the old code checked the stick first, so a stale
     * stick gate starved the wheel (user report: cruceta never turned the
     * wheel). Steering (nx) therefore goes to the wheel whenever the wheel
     * is interactable, regardless of what the stick gate says. */
    int scx, scy; float srx, sry;
    int stick_ok = stick_region(&scx, &scy, &srx, &sry);
    int wcx, wcy; float wrx, wry;
    int wheel_ok = wheel_region(&wcx, &wcy, &wrx, &wry);

    /* Fase 59: steering deflection with neither widget interactable --
     * diagnose (throttled) instead of silently dropping it. On foot this
     * stays quiet (the move-stick takes nx); in a vehicle with a hidden
     * stick it pinpoints which wheel-lookup step fails per driving skin. */
    if (nx != 0.0f && !wheel_ok && !stick_ok)
        wheel_miss_diag(nx);

    /* 1. In vehicle: steering wheel / slide control, driven with the real
     * finger gesture -- grab near the top of the wheel rim and curve
     * down-left / down-right like a real steering wheel.
     *
     * Wheel::processTouch() (out_ghidra.c) only reads deltaX =
     * first.x - current.x (dir = sign, amount = |dx| / *(this+0x68), with
     * 0x68 = 0x46 = 70px for full lock -- the Wheel ctor hardcodes it), so
     * the X component below is what actually steers: DOWN lands near the
     * top of the wheel region (cx, cy - 0.7*ry, clamped inside so the DOWN
     * hit-tests onto the widget), then MOVE goes to
     * (down_x + nx*85, down_y + 50*|nx|). +-85px in X clears the 70px full-
     * lock threshold on digital input and stays proportional on the analog
     * stick; the +50px downward curve replicates the real finger path and
     * also exercises the Y axis for the SlideControl steering alternate
     * (its processTouch reads both axes depending on the mode at this+0x68).
     * Pointer-id capture (same as the AnalogStick, Fase 51) keeps routing
     * MOVEs to the wheel once the DOWN lands, so drifting outside the drawn
     * region is fine -- exactly what a real thumb does past the rim. */
    if (wheel_ok && nx != 0.0f) {
        if (s_moveStick.active) {
            s_touch(&jni, NULL, 0, s_moveStick.last_x, s_moveStick.last_y, (jlong)s_moveStick.slot, 0, 0);
            l_note("[input] move-stick up (slot %d)", s_moveStick.slot);
            s_moveStick.active = 0;
        }

        if (!s_wheelStick.active) {
            /* DOWN = grab point near the top of the rim. Stored in cx/cy
             * (fixed for the whole drag); tx/ty below derive from it. */
            s_wheelStick.cx = wcx;
            s_wheelStick.cy = clampi((int)(wcy - wry * 0.7f), 0, GA_ENGINE_H - 1);
            s_wheelStick.rx = wrx;
            s_wheelStick.ry = wry;
            s_wheelStick.last_x = s_wheelStick.cx;
            s_wheelStick.last_y = s_wheelStick.cy;
            s_touch(&jni, NULL, 1, s_wheelStick.cx, s_wheelStick.cy, (jlong)s_wheelStick.slot, 0, 0);
            s_wheelStick.active = 1;
            l_note("[input] wheel down @(%d,%d) slot %d", s_wheelStick.cx, s_wheelStick.cy, s_wheelStick.slot);
        }

        /* nx > 0 (right) -> tx > down_x -> first.x - current.x < 0 -> dir=0 (right)
         * nx < 0 (left)  -> tx < down_x -> first.x - current.x > 0 -> dir=1 (left)
         * ty always grows downward, curving like a real wheel turn. */
        float ax = nx < 0.0f ? -nx : nx;
        int tx = clampi((int)(s_wheelStick.cx + nx * 85.0f), 0, GA_ENGINE_W - 1);
        int ty = clampi((int)(s_wheelStick.cy + ax * 50.0f), 0, GA_ENGINE_H - 1);
        if (tx != s_wheelStick.last_x || ty != s_wheelStick.last_y) {
            s_touch(&jni, NULL, 2, tx, ty, (jlong)s_wheelStick.slot, 0, 0);
            s_wheelStick.last_x = tx;
            s_wheelStick.last_y = ty;
            l_debug("[input] wheel move @(%d,%d) nx=%.2f", tx, ty, nx);
        }
        return;
    }

    /* No steering input (or wheel hidden now): center the wheel if held. */
    if (s_wheelStick.active) {
        s_touch(&jni, NULL, 0, s_wheelStick.last_x, s_wheelStick.last_y, (jlong)s_wheelStick.slot, 0, 0);
        l_note("[input] wheel up (slot %d)", s_wheelStick.slot);
        s_wheelStick.active = 0;
    }

    /* 2. On-foot / sniper: AnalogStick movement (unchanged, Fase 51). */
    if (stick_ok) {
        if (!want_active) {
            if (s_moveStick.active) {
                s_touch(&jni, NULL, 0, s_moveStick.last_x, s_moveStick.last_y, (jlong)s_moveStick.slot, 0, 0);
                l_note("[input] move-stick up (slot %d)", s_moveStick.slot);
                s_moveStick.active = 0;
            }
            return;
        }

        if (!s_moveStick.active) {
            s_moveStick.cx = scx;
            s_moveStick.cy = scy;
            s_moveStick.rx = srx;
            s_moveStick.ry = sry;
            s_moveStick.last_x = scx;
            s_moveStick.last_y = scy;
            s_touch(&jni, NULL, 1, scx, scy, (jlong)s_moveStick.slot, 0, 0);
            s_moveStick.active = 1;
            l_note("[input] move-stick down @(%d,%d) r=(%.0f,%.0f) slot %d",
                   scx, scy, srx, sry, s_moveStick.slot);
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

    /* 3. Neither stick nor wheel is interactable: release move-stick if held. */
    if (s_moveStick.active) {
        s_touch(&jni, NULL, 0, s_moveStick.last_x, s_moveStick.last_y, (jlong)s_moveStick.slot, 0, 0);
        l_note("[input] move-stick up (slot %d)", s_moveStick.slot);
        s_moveStick.active = 0;
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

    int wantFull = (buttons & GA_ALPHA_COMBO_MASK) == GA_ALPHA_COMBO_MASK;
    if (wantFull != s_alphaFull) {
        s_alphaFull = wantFull;
        l_note("[input] touch controls opacity -> %s (L+R %s)",
               s_alphaFull ? "100%" : "hidden ~1%", s_alphaFull ? "held" : "released");
    }
    apply_touch_alpha();

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
