/*
 * gamepad_actions.h -- maps Vita physical buttons onto the engine's own
 * logical HUD virtual-button actions (Fase 45, reworked in Fase 47, see
 * port_progress.md).
 *
 * Confirmed via decompiled/libGangster2_armeabi/ghidra/out_ghidra.c: the
 * engine's on-screen touch controls (VirtualButton widgets, permanently
 * allocated as CHudManager members by CHudManager::load()) each raise an
 * EvVirtualButton event carrying a small mode-independent
 * EvVButton::ButtonType::Type id (0=attack, 2=enter/exit car, 3=enter shop,
 * 4/5=vehicle-only special actions, 0xe=take cover, 0xf=sprint, ...).
 * Whichever IControlHandler is currently subscribed (OnFoot/Driving/Flying/
 * Sniper -- selected by the engine itself based on gameplay mode) reacts to
 * the type ids it understands and silently ignores the rest -- so firing a
 * button's real action does not need us to track which mode is active: the
 * engine already does that.
 *
 * Fase 47 rework -- WHY TOUCH SYNTHESIS instead of calling
 * VirtualButton::processTouch() directly (what Fase 45 did):
 *
 * 1. The Fase 45 premise was wrong. Its comment claimed processTouch() "only
 *    ever dereferences the fake touch-point struct we pass it". The real
 *    disassembly (decompiled/disasm/full_libGangster2.so.md,
 *    `002bfb24 <VirtualButton::processTouch(long)>`) proves the opposite:
 *    `ldr r2, [r0, #12]` reads the BUTTON's own flags word (this+0xc) and
 *    `tst r2, #1; beq return` silently ignores the call unless the button's
 *    own bit0 is set. The second argument (r1) is never read at all -- the
 *    hand-built `{0,0,0,1}` buffer Fase 45 passed was dead weight, and the
 *    Ghidra listing it relied on (`param_1+0xc`) is the same field seen
 *    through the secondary vtable (the _ZThn8_ thunk reads `this+4`, which
 *    IS `this+0xc` of the real object). Net effect: every Fase-45 fire hit
 *    an inactive skin instance with bit0 clear and did nothing -- matches
 *    the user report (log 049: game runs, `gamepad_actions: ready`, but no
 *    physical button does anything).
 * 2. Fase 45 fired a single CHudManager member per type id, but
 *    CHudManager::load() builds SEVERAL instances per id (one per HUD
 *    skin/mode: e.g. type 0/attack lives at +0x34, +0x50, +0x60 AND +0x68).
 *    Only the currently visible skin's instance can react; the rest ignore
 *    the call. Picking one offset by hand is picking the wrong skin in most
 *    modes.
 * 3. Fase 45 only ever called processTouch (the DOWN half of a tap). The
 *    on-foot handlers prove a tap needs both halves: handleTakeCoverButton,
 *    handleEnterShopButton and handleMiniSaveButton only act on the RELEASE
 *    event (EvVirtualButton+8 == 2, raised by processTouchRelease), and
 *    handleSprintButton is a down(1)/up(0) latch. Attack/enter-car act on
 *    the down event (+8 == 0). A press without a release can never trigger
 *    cover/shop, and leaves sprint latched on.
 *
 * So instead of poking widget flags by hand, this file SYNTHESIZES REAL
 * TOUCHES at the active virtual button's screen position through the exact
 * same GameGLSurfaceView_nativeOnTouch() path the touchscreen relay in
 * main.c already uses (proven to work): down on the physical press, up on
 * the physical release. The engine then runs its own hit-testing,
 * VirtualButton::update/processTouch/processTouchRelease dispatch and
 * EvVirtualButton (+8 == 0 down / 2-or-3 up) construction -- flag handling,
 * skin selection and press/release pairing included, with zero guessing
 * about private flag bits.
 *
 * Two engine queries make this mode-independent (cited from the decompiled
 * sources, no guessing):
 * - Which skin instance is currently interactable: the same vtable slot
 *   CHudManager::update() itself consults before dispatching a touch to a
 *   widget (slot +0x14 -- also the predicate draw2d() uses, so it tracks
 *   visibility). First passing instance per type wins, so a press can never
 *   multi-fire across skins.
 * - Where the button is on screen: vtable slot +0x1c (HudElement::
 *   getTouchRegion, returns the +0x10..0x1c design-space rect), scaled by
 *   Application::GetScreenScaleFactors() (both symbols dynamically
 *   exported, resolved by name like every other engine symbol in this
 *   port). Near/far gating (nearCar/nearShop/nearCover) is then automatic:
 *   a hidden button fails the +0x14 gate and is simply skipped.
 */

#pragma once

#include <stdint.h>

#include <falso_jni/FalsoJNI.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Same shape as the GameGLSurfaceView_nativeOnTouch resolved in main.c. */
typedef void (*gamepad_touch_fn)(JNIEnv *, jclass, jint, jint, jint, jlong, jint, jint);

/* Resolves the required engine symbols. Safe to call once at startup;
 * logs a warning and disables itself (no-op afterwards) if anything is
 * missing, rather than aborting the whole port over an unconfirmed extra. */
void gamepad_actions_init(gamepad_touch_fn touch);

/* Called once per frame with the current and previous SceCtrlData.buttons
 * bitmasks; injects a touch-down at the active virtual button's position on
 * each mapped button's rising edge and the matching touch-up on its falling
 * edge -- i.e. a complete tap per physical press, down to up. */
void gamepad_actions_update(uint32_t buttons, uint32_t old_buttons);

/* Called once per frame with the D-pad bitmask (from the same `buttons` as
 * above -- pass the masked SCE_CTRL_UP/DOWN/LEFT/RIGHT bits or 0) and the
 * left analog stick's raw SceCtrlData.lx/ly (0..255, 128 = centered).
 * Synthesizes a touch-DOWN/MOVE/UP drag on the engine's on-screen movement
 * AnalogStick widget (CHudManager+0x28) -- the D-pad acts as a full-deflection
 * override per axis, the analog stick as continuous deflection with a
 * deadzone. In a vehicle the same inputs steer the Wheel widget
 * (CHudManager+0x2c, SlideControl +0x54/+0x58 as alternate) with the real
 * finger gesture: grab near the top of the rim, curve down-left/down-right
 * (Fase 58). Virtual buttons are hidden at ~1% opacity every frame
 * (L+R restores 100%) -- physical controls drive, no touch needed. This is what makes physical movement input work at all: the
 * engine has no keycode-based movement path (Fase 47), only this HUD widget's
 * own drag tracking (AnalogStick::processTouch, via TouchScreenBase pointer-id
 * capture -- same touch relay `gamepad_actions_update()` already uses for
 * buttons). Fase 51, see port_progress.md. */
void gamepad_stick_update(uint32_t dpad_buttons, uint8_t lx, uint8_t ly);

#ifdef __cplusplus
}
#endif
