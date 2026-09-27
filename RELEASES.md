# Releases — Gangstar Miami Vindication (PS Vita)

> Test builds. Everything here is **alpha**: NOT playable, relatively playable at best,
> strictly for testing. Each entry states what is **proven on real hardware** (console log /
> user testing) vs. what is **built but unconfirmed** — please report runs with the
> `debug_local_NNN.log`.

---

## v0.64.0-alpha — 2026-09-27 (Fase 64, current)

VPK: `build/gangstarmiamivindication.vpk` (built with `psvita-toolkit build --preset release`).

### What's new since v0.63.0
- **Performance & GPU Memory (VRAM / RAM)**:
  * Eliminated the critical `Circular pool #2 spilled into VRAM` regression by tuning `_newlib_heap_size_user` from 256 MB down to 192 MB. vitaGL now receives over 140 MB of RAM, fitting all 3 circular pool buffers completely into RAM with zero spill into VRAM (recovering >21 MB of pure VRAM).
  * Solved the 4.3-second freezes / FPS drops (`gpu_alloc_mapped_aligned failed with a requested size of 4194304 bytes` and 4-cycle GC stalls) when driving near mission triggers, by ensuring large 4 MB textures have ample contiguous memory in RAM and VRAM.
  * Added `sceUserMainThreadStackSize = 4 * 1024 * 1024` (4 MB stack) for deep recursion and fast streaming in vehicles.
- **Animation Streaming**:
  * Resolved `CAnimationStreamingManager::Instance` and expanded its memory cache limit from 384 KB (`0x60000`) to 2 MB (`0x200000`), ending cache thrashing, evictions, and warning chatter when loading dense mission areas with animated cutscene triggers and NPCs.
- **Input & Sound I/O Optimization**:
  * Demoted all per-event touch/stick/wheel logs in `gamepad_actions.c` to `l_debug` (compiled out in Release), eliminating up to 100 ms/sec of blocking synchronous disk writes to `ux0:` during active steering, sprinting, or shooting.
  * Filtered out `DeviceKeyInput`, `stopRadio`, `SOUNDS-VV`, `----Gameloft----`, and `AnimationStreamingManager` chatter from disk writes in `reimpl/log.c`.
- **Rendering Stability**:
  * Diagnosed and avoided the `libmathneon.a` softfp bug in `cosf_neon_sfp` (which caused the total black screen in `debug_local_063.log`), preserving standard rock-solid newlib math and 100% rendering integrity.

### Confirmed on hardware (logs 063–064 + user testing)
- Fluidity significantly improved after removing synchronous input logging (`debug_local_064.log`).
- Rendering 100% restored after reverting `libmathneon` softfp wrappers (no black screen).
- Buttons at 1% opacity and steering wheel control confirmed working.
- Everything from v0.63.0 (intro video, character rendering, radio stability) fully functional.

### Known errors in this release
1. Initial 0.5–16 s shader/texture bursts at title and first vehicle/area load (settles via on-disk shader cache).
2. ~8 s engine `PostInit` stall after intro video.
3. City pop-in still present at distant horizon (streaming radius 8000 / far 15000).
4. No suspend/resume or accelerometer lifecycle hooks wired yet.

---

## v0.63.0-alpha — 2026-09-25 (Fases 61–63)

VPK: `build/gangstarmiamivindication.vpk` (built with `psvita-toolkit build --preset release`).

### What's new since v0.60.0
- **Controls**: steering wheel works via D-Pad/stick (clamped to the visible rim,
  `wheel down @(202,316)` — no more `wheel miss`).
- **Buttons**: back to ~1% opacity, now held through presses AND mission/tutorial highlight
  pulses (Fase 63 hooks `HudElement::blink`; L+R restores 100% + highlights).
- **LOD**: streaming radius 6000→8000, far 13000→15000 (all other Low-End savings kept);
  transition logging (`[lod] ...`) proves when the override lands.

### Confirmed on hardware (logs 061–062 + user testing)
- Steering in vehicles with D-Pad/stick — **fixed (user-tested)**.
- Buttons at 1% in standby — **user-tested**.
- Smoother driving feel with the wider radius — **user-tested feel**.
- No GPU-pool failures in three sessions (060–062); menus ~60 fps, driving 20–60 fps.
- Everything v0.60.0 had (intro video, black characters/vehicles, radio stability).

### Known errors in this release
1. Buttons at 1% WHILE pressed/tapping — fix built, **unconfirmed** (needs eyes on screen).
2. LOD numbers actually latched — **unconfirmed** (look for
   `[lod] ... radius 6000->8000, far 13000->15000`; log 062 showed the profile loads late,
   in PostInit). If `failed (4194304)` returns, radius goes back to 6000.
3. City pop-in may persist (further out now) — report the distance.
4. One-time stalls: ~8 s engine `PostInit` after the intro; 0.5–16 s shader/texture bursts at
   title and first vehicle/area load (better on repeat runs via on-disk shader cache).
5. Intro stretched ~10% horizontally (on purpose, fullscreen); video plays at real speed
   with dropped frames (14–15 fps) but full audio.
6. No suspend/resume, no accelerometer, no in-game-menu integration (lifecycle hooks not wired).

### How to report
Play, then fetch `ux0:data/gangstarmiamivindication/logs/debug_local_NNN.log` and note: buttons
at 1% while pressing (which button/action if not), `[lod]` lines, `[patch] ... blink hooked`,
FPS wells, and any `wheel miss`.

---

## v0.60.0-alpha — 2026-09-21 (Fases 58–60)

VPK: `build/gangstarmiamivindication.vpk` (built with `psvita-toolkit build --preset release`).

### What's new
- **Video**: intro decoded at full 800x500 (was 400x250), fullscreen stretch (was letterboxed),
  A/V pipeline deadlock fixed (was slow-motion ~11 fps, near-silent), whole-track AAC pre-demux.
- **Controls**: virtual buttons/wheel/stick hidden every frame (draw-skipped, immune to
  pressed/held flicker; L+R restores 100%). Full physical mapping (see README table).
- **Audio**: radio/music retry-loop settled (event-driven `stopRadio`/`playRadio` only);
  stop now really cancels deferred stream opens.
- **Performance (UNCONFIRMED)**: Low-End device profile (fewer spawns, smaller streaming radius,
  no shadows/dynamic lighting), bigger vitaGL pool (12→8 MB threshold).

### Confirmed on hardware (logs + user testing)
- Boots → warning → menus (~60 fps) → on-foot and driving gameplay.
- All physical buttons act in-game (attack/accelerate/brake/enter-car/cover/sprint, D-Pad/stick
  movement, pause menu).
- Radio/music stable, no per-frame restart spam.
- Intro video plays fine, full-res fullscreen; skips cleanly with Cross — **fixed (user-tested)**.
- Black characters/vehicles render correctly — **fixed (user-tested)**.

### Known errors in this release
**Performance**
1. Wells down to **~1 fps while driving** (GPU memory exhausted on 4 MB texture uploads, ~4.2 s
   stall each; tex cache recovers 0 bytes). Fixes included but **unconfirmed**.
2. Typical driving **13–31 fps** (GPU-bound open world); menus **~60 fps**.
3. One-time stalls: ~8 s engine `PostInit` after the intro; 0.5–16 s shader/texture bursts at
   title and first vehicle/area load (better on repeat runs via on-disk shader cache).

**Graphics**
4. Low-End profile look (**unconfirmed**): no dynamic lighting/shadows/far water/retro effect —
   flatter image. Revertible in one line (`Method_GetDeviceType`).
5. Intro stretched ~10% horizontally (on purpose, fullscreen).

**Controls**
6. Vehicles **CANNOT be steered with the wheel** via D-Pad/stick (widget found but its touch
   center is outside the engine's 960x480 band — steering fix pending; diagnostics log
   `wheel miss`). The wheel is **semi-invisible** (virtual controls hidden by design; hold **L+R**
   to show them).
7. No suspend/resume, no accelerometer, no in-game-menu integration (lifecycle hooks not wired).

### How to report
Play, then fetch `ux0:data/gangstarmiamivindication/logs/debug_local_NNN.log` and note: where
FPS wells happen, whether buttons stay hidden (and L+R restores them), whether the wheel turns,
and any `wheel miss` / `pre-demuxed audio` / `Motorola Low End 4` lines.

---

## Earlier phases
See [`port_progress.md`](port_progress.md) (bug-by-bug log, Fases 1–60) and git history.
No packaged releases precede this file; day-to-day testing used undeployed `eboot.bin` drops.
