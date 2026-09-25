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
        ((void (*)(void *, int, int))a)(th, b1, b2);
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
