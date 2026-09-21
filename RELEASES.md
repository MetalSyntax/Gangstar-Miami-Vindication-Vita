# Releases — Gangstar Miami Vindication (PS Vita)

> Test builds. Everything here is **alpha**: NOT playable, relatively playable at best,
> strictly for testing. Each entry states what is **proven on real hardware** (console log /
> user testing) vs. what is **built but unconfirmed** — please report runs with the
> `debug_local_NNN.log`.

---

## v0.60.0-alpha — 2026-09-21 (Fases 58–60, current)

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
