/*
 * Copyright (C) 2023 Volodymyr Atamanenko
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

/**
 * @file  patch.c
 * @brief Patching some of the .so internal functions or bridging them to native
 *        for better compatibility.
 */

#include <kubridge.h>
#include <so_util/so_util.h>

#include "utils/gamepad_actions.h"
#include "utils/logger.h"

extern so_module so_mod;

/* Fase 63 (paso 4, el leak en presionado): hook HudElement::blink(bool,bool).
 *
 * WHY A HOOK, AND WHY THIS FUNCTION -- read from the real binary:
 * - The user reports buttons at 1% in standby but full-bright while pressing
 *   (log 062). The pre-render poke (alpha stamps + this+0xc bit4 clear) provably
 *   lands -- standby holds 1% on the same instances.
 * - Raw disasm of HudElement::setAlpha() (2ba98c-2bab52) leaves exactly ONE
 *   in-render path that can paint quads bright in normal HUDs: the blink
 *   branch (2baa00-2baa4e, forces 0x54 AND all quad alphas to 0xff), taken
 *   when this+0xc bit4 is set AT DRAW TIME. Pressed/idle/release branches
 *   either leave the quads alone or re-derive them from our 0x34 floor (the
 *   fade divisor this+0x3c is 0 forever -- grep-verified -- so it collapses
 *   to the 0x34 clamp, never above it).
 * - So bit4 is being (re-)set AFTER our poke, every time it matters:
 *   CHudManager::update() dispatches touches inside nativeRender (after the
 *   poke), and ScriptCommands::HudHighlight::update() (out_ghidra.c:150547)
 *   re-arms blink highlights PER FRAME while a mission/tutorial script step
 *   is active. A pre-render bit-clear can never win that race -- hence the
 *   hook, which runs INSTEAD of the setter at whatever time it fires.
 * - SAFE TO SWALLOW: HudElement::blink() (out_ghidra.c:73349) ONLY flips bit
 *   0x10 (orr/bic, gated on the +0x14 predicate) -- no timers, no state --
 *   and HudElement::isBlinking() has no logic callers in the binary (only
 *   its own definition). The bit is consumed solely by setAlpha()'s visual
 *   branch. Hint/tutorial TEXT and messages are untouched; only the button
 *   flash stays at our 1% poke. With L+R held we call through, so highlights
 *   still show when the user explicitly asked to see the controls.
 * - COST: blink() fires at events (tutorials, HudHighlight steps), never
 *   per-frame-per-widget like setAlpha() -- hooking it adds zero per-frame
 *   overhead, unlike hooking the draw path itself.
 */
static so_hook s_blinkHook;
static int s_blinkHooked = 0;

static void blink_hook(void *th, int b1, int b2) {
    if (gamepad_alpha_full() && s_blinkHooked) {
        uintptr_t a = s_blinkHook.addr;
        kuKernelCpuUnrestrictedMemcpy((void *)a, s_blinkHook.orig_instr, sizeof(s_blinkHook.orig_instr));
        kuKernelFlushCaches((void *)a, sizeof(s_blinkHook.orig_instr));
        /* blink() is Thumb: calling the even h.addr BLXes into ARM state and runs
         * garbage (crash dump 0x00076c3d7f, PC in glf::GetColor). Use thumb_addr. */
        uintptr_t fn = s_blinkHook.thumb_addr ? s_blinkHook.thumb_addr : a;
        ((void (*)(void *, int, int))fn)(th, b1, b2);
        kuKernelCpuUnrestrictedMemcpy((void *)a, s_blinkHook.patch_instr, sizeof(s_blinkHook.patch_instr));
        kuKernelFlushCaches((void *)a, sizeof(s_blinkHook.patch_instr));
        return;
    }
    /* Swallowed: the tutorial/script highlight never sets bit4, setAlpha()
     * never takes its 0xff branch, the 1% poke survives the whole frame. */
}

void so_patch(void) {
    // Sample hook
    //hook_addr((uintptr_t)so_symbol(&so_mod, "_ZN6glitch2os7Printer5printEPKcz"), (uintptr_t)&hookedFunction);

    uintptr_t blink = (uintptr_t)so_symbol(&so_mod, "_ZN10HudElement5blinkEbb");
    if (!blink) {
        l_warn("[patch] HudElement::blink not resolved -- highlight hook disabled");
        return;
    }
    s_blinkHook = hook_addr(blink, (uintptr_t)&blink_hook);
    so_flush_caches(&so_mod);
    s_blinkHooked = 1;
    l_note("[patch] HudElement::blink hooked -- tutorial highlights stay at 1% (L+R calls through)");
}

/* Fase 67: buttons still flashed at 100% while PRESSED (screenshot
 * 2026-11-22-213124, the attack crosshair). Raw disasm of
 * HudElement::setAlpha() (2ba98c): every visual branch ends at 2ba9f2,
 * which publishes `this+0x44` (the 4 RGBA quads we stamp to alpha 0) into the
 * global colour pointer that ASprite::PaintFrame() uses; resetAlpha() nulls
 * it again after each draw. The PRESSED branch (flags&6 != 4, 2baa58) instead
 * does `str 0xff,[r4,#0x54]; b 2ba9fa` -- it returns WITHOUT publishing, so
 * the pointer stays NULL and the sprite paints with the default opaque
 * colour, whatever alpha we stamped. (Fase 62's "pressed leaves the quads
 * untouched, so 3 survives" was half right: the quads survive, but nobody
 * reads them.) Fix: retarget that one `b` (2baa5c) to 2ba9f2 while the
 * controls are hidden, so a pressed widget draws with its stamped quads
 * (r4 = this and r5 = GOT base are both still live there). Restored to the
 * original branch whenever the controls are shown (L+R / menu SHOWN), so
 * pressed feedback is stock then. Side effect while hidden: other pressable
 * HUD widgets we don't stamp draw pressed at their idle alpha instead of 255. */
/* Fase 68: the steering wheel. Wheel::draw2d (2c0e18) tests flags bit1
 * (pressed = being steered) at 2c0e34/2c0e36: NOT pressed -> `b 2c100a` =
 * HudElement::draw2d (setAlpha path, honours our stamps); pressed -> its own
 * rotated PaintFrame that never calls setAlpha, i.e. always opaque (log 068,
 * "the wheel shows while driving"). While hidden, the `bmi` at 2c0e36 becomes
 * a NOP so a steered wheel also takes the stamped HudElement::draw2d path
 * (drawn unrotated at alpha 0 -- invisible either way). AnalogStick/
 * SlideControl draw2d both call setAlpha, covered by the branch above. */
static const struct {
    const char *sym;
    uint32_t off;      /* from the function start */
    uint16_t stock, hidden;
    const char *what;
} s_hudPatches[] = {
    { "_ZNK10HudElement8setAlphaEv", 0x2baa5c - 0x2ba98c, 0xe7cd /* b 2ba9fa */, 0xe7c9 /* b 2ba9f2 */, "setAlpha pressed branch" },
    { "_ZNK5Wheel6draw2dEv",         0x2c0e36 - 0x2c0e18, 0xd400 /* bmi 2c0e3a */, 0x46c0 /* nop */,   "Wheel pressed draw" },
    /* Fase 78: the HUD radio (CHudManager+0x78) and its on/off toggle (+0x74)
     * PaintFrame directly, no setAlpha, and their draw gate is the same +0xc
     * bit their touch logic needs. While hidden, their first instruction
     * (push) becomes `bx lr`: nothing drawn, radio logic untouched (the
     * Square "Radio station" action calls SoundManager directly). */
    { "_ZNK5Radio6draw2dEv",         0, 0xb510 /* push {r4, lr} */, 0x4770 /* bx lr */, "Radio draw" },
    { "_ZNK12ToggleButton6draw2dEv", 0, 0xb500 /* push {lr} */,     0x4770 /* bx lr */, "Radio toggle draw" },
};
#define N_HUD_PATCHES (sizeof(s_hudPatches) / sizeof(s_hudPatches[0]))
static uint16_t *s_hudPatchAddr[N_HUD_PATCHES];

void patch_hud_pressed_alpha(int hidden) {
    static int resolved = 0, state = -1;
    if (!resolved) {
        resolved = 1;
        for (unsigned i = 0; i < N_HUD_PATCHES; i++) {
            uintptr_t f = (uintptr_t)so_symbol(&so_mod, s_hudPatches[i].sym);
            uint16_t *p = f ? (uint16_t *)((f & ~1u) + s_hudPatches[i].off) : NULL;
            if (p && (*p == s_hudPatches[i].stock || *p == s_hudPatches[i].hidden))
                s_hudPatchAddr[i] = p;
            else
                l_warn("[patch] %s not found (got 0x%04x) -- stays visible when pressed",
                       s_hudPatches[i].what, p ? *p : 0);
        }
    }
    if (state == hidden)
        return;
    state = hidden;
    for (unsigned i = 0; i < N_HUD_PATCHES; i++) {
        if (!s_hudPatchAddr[i])
            continue;
        uint16_t insn = hidden ? s_hudPatches[i].hidden : s_hudPatches[i].stock;
        kuKernelCpuUnrestrictedMemcpy(s_hudPatchAddr[i], &insn, sizeof(insn));
        kuKernelFlushCaches(s_hudPatchAddr[i], sizeof(insn));
    }
    l_note("[patch] HUD draws (setAlpha pressed, Wheel, Radio) -> %s", hidden ? "hidden (stamped alpha)" : "stock");
}
