# GANGSTAR MIAMI VINDICATION — PS Vita Port

<p align="center">
  <img src="extras/livearea/pic0.png" width="700" alt="Gangstar Miami Vindication PS Vita Banner" />
</p>

<p align="center">
  <b>Native port of Gangstar Miami Vindication HD (Gameloft) for PlayStation Vita.</b>
</p>

<p align="center">
  <img src="https://img.shields.io/badge/Platform-PS%20Vita-003791.svg?style=flat-square&logo=playstation" alt="Platform PS Vita" />
  <img src="https://img.shields.io/badge/Title%20ID-PSVGMV002-ff69b4.svg?style=flat-square" alt="Title ID PSVGMV002" />
  <img src="https://img.shields.io/badge/Engine-Glitch%20Engine%200.1.0.2-brightgreen.svg?style=flat-square" alt="Engine" />
  <img src="https://img.shields.io/badge/Renderer-vitaGL%20%28GLES%201.1%29-orange.svg?style=flat-square" alt="Renderer" />
  <img src="https://img.shields.io/badge/Status-Alpha%20(test%20build)-red.svg?style=flat-square" alt="Status: Alpha, test build" />
</p>

---

## 📖 Description

**Gangstar Miami Vindication** is Gameloft's open-world action game, originally released for
Android as `Gangstar-Miami-Vindication-HD.apk`. This port runs the compiled native library
(`libGangster2.so`) from the Android release directly on the PS Vita's ARM Cortex-A9 processor,
using a dynamic loader (*soloader*) and an Android environment emulation layer (*FalsoJNI*), with
[vitaGL](https://github.com/Rinnegatamante/vitaGL) providing the GLES 1.1 fixed-function
rendering backend. The `.so` has no `JNI_OnLoad`/`RegisterNatives` — every JNI entry point is
resolved by symbol name and invoked by hand, following the exact lifecycle order of the real
Android Activity/Renderer.

### 🎮 Current Status: Alpha test build — NOT playable, for testing only

The game boots, renders, and reaches third-person gameplay — driving, on-foot, vehicle
entry — but **it is not playable**: at best it is **relatively playable**, and this build
exists **strictly for testing**. Expect FPS wells down to ~1 fps, visual glitches, and fixes
that are built but unconfirmed (see "Known Issues" below and
[`RELEASES.md`](RELEASES.md) for the per-release breakdown of what is proven on hardware vs.
what still needs confirmation). Do not treat this alpha as a finished or enjoyable release.

It got here through a long bug-by-bug history (format-string crashes, a missing `HAVE_SOFTFP_ABI`
causing an all-black screen, a circular-pool GPU stall dragging it to ~9 fps, among others); see
[`port_progress.md`](port_progress.md) for the full diagnosis log, one confirmed bug at a time,
and [`PORTING_PLAN.md`](PORTING_PLAN.md) for the living engine/JNI map.

#### ✅ Confirmed on real hardware (logs + user testing)
- Boots to warning screen, menus (~60 fps), and open gameplay on foot and driving.
- Physical controls drive the game (attack/accelerate/brake/enter-car/cover/sprint + D-Pad/stick
  movement all log their synthesized touches and act in-game).
- Steering wheel via D-Pad/stick in vehicles — **fixed (user-tested, log 061)**.
- Virtual buttons/wheel/stick at ~1% in standby — **user-tested (log 062)**.
- Radio/music retry-loop settled: `stopRadio`/`playRadio` now fire on events only, not per frame.
- Intro cutscene plays from the original `intro.m4v` at full 800x500 resolution, fullscreen —
  **video fixed (user-tested)**.
- **Black characters/vehicles fixed (user-tested)** — they render correctly now.
- Smoother driving feel with the widened streaming radius — **user-tested feel (log 062)**,
  numbers still unconfirmed (see below).

#### 🧪 NOT yet confirmed on hardware (latest changes, need testing)
- Buttons at 1% WHILE pressed/tapping and during mission highlight pulses (Fase 63 blink
  hook; standby 1% already user-tested) — needs eyes on screen, L+R restores them + highlights.
- Streaming radius 8000 / far 15000 actually latched (look for
  `[lod] ... radius 6000->8000, far 13000->15000` in the log) + pop-in distance + no
  `gpu_alloc...failed` regression.

### ✨ What Works

- **Native ARM Execution**: `libGangster2.so` (armeabi/ARMv6, soft-float) runs directly on the
  Vita's CPU via the soloader.
- **vitaGL Graphics Pipeline**: GLES 1.1 fixed-function rendering at the native 960x544 panel
  resolution (the engine's own internal resolution is 960x480; touch input and viewport are
  remapped to match).
- **Audio**: Real backend (`sceAudioOut` + `libvorbisfile`) — short SFX decoded and cached, up to
  8+4 concurrent streamed voices for music/radio/voice with looping, independent music/SFX/VFX
  gain control.
- **Intro cutscene**: Plays the original `intro.m4v` **as shipped** (MPEG-4 Part 2, 800x500)
  via a software FFmpeg decoder (`libavcodec`/`libswresample`, no transcodes — the Vita's hardware
  decoder only handles H.264 and this project never alters original assets). Decoded at full
  resolution and stretched to the full 960x544 panel, with its AAC audio in sync, skippable with
  Cross/Start.
- **Touch + physical input**: Front touchscreen mapped to the engine's multi-touch slots. Physical
  controls drive the game by synthesizing real touches on the engine's own HUD widgets — no touch
  needed:
  | Vita control | On foot | In vehicle |
  |---|---|---|
  | Cross | Attack | Accelerate |
  | Circle | Sprint | Brake |
  | Triangle | Enter car / Enter shop | Exit car |
  | Square | Take cover | — |
  | L / R triggers | — | Brake / Accelerate |
  | D-Pad / Left stick | Move (analog drag) | Steer the wheel (rim-grab gesture) |
  | Start | Pause menu | Pause menu |
  | L+R held | Show the hidden touch buttons (100%) | Same |
- **Hidden touch buttons**: The on-screen virtual buttons/wheel/stick are hidden every frame
  (draw-skipped, ~1% alpha fallback) so they never flash or flicker when pressed, held, or when
  the HUD changes state. Hold **L+R** to bring them back at full opacity at any time.
- **Overclocked + tuned for the Cortex-A9**: CPU/Bus/GPU/GPU-Xbar clocks at their ceiling, NEON
  codegen, a tuned vitaGL speedhack set, and in-memory caches for path translation and sound
  existence checks to cut down on SD-card I/O during gameplay.

### ⚠️ Known Issues (alpha — full list in [`RELEASES.md`](RELEASES.md))

**Performance / FPS drops**
- **Wells down to ~1 fps while driving in the open world.** No recurrence in logs 060–062
  (20–60 fps driving, zero `gpu_alloc...failed`), but sessions were shorter than the one that
  broke in 059 — not closed yet. Fase 62 widens the streaming radius 6000→8000 / far
  13000→15000 to push pop-in farther out (user reports smoother driving) — **numbers
  unconfirmed**, watch the next log for the `[lod] ... 6000->8000` line, pop-in distance,
  and any `failed (4194304)` return.
- **City pop-in while driving.** The Low-End profile streams at radius 6000 (same open symptom
  as sibling Asphalt-6-Vita, which keeps factory LOD for perf reasons). Fase 62 buys back
  distance in a measured step, keeping all Low-End savings — **unconfirmed**, report how far
  out the city still "generates".
- **One-time stalls:** ~8 s on the second engine frame after the intro (`Application::PostInit`),
  shader-compile bursts at the title screen / first vehicle load (frames of 0.5-16 s). Engine-side
  init on the render thread — not skippable from the loader; the on-disk shader cache makes repeat
  runs faster.

**Graphics**
- **Flatter look, no shadows (since Fase 60, unconfirmed):** the Low-End profile disables dynamic
  lighting, shadows, far water and the retro effect to save GPU. Reversible in one line if it
  looks unacceptable once the FPS wells are confirmed gone.
- ~~Characters/vehicles rendering solid black~~ — **fixed (user-tested)**.
- ~~Intro video issues~~ — **fixed (user-tested)**; plays full-res, fullscreen (stretched ~10%
  horizontally on purpose), skippable with Cross/Start.

**Controls**
- ~~Vehicles CANNOT be steered with the wheel~~ — **fixed (user-tested, log 061)**:
  D-Pad/stick steers via the clamped wheel grab (`wheel down @(202,316)`, no more `wheel miss`).
- Virtual buttons/wheel/stick render at ~1% opacity **always, including while pressed** —
  Fase 63 hooks the tutorial/script highlight path that flashed them bright (hint text still
  shows). Hold **L+R** to show them at full opacity. Pressed-state 1% is **unconfirmed** —
  needs eyes on screen. On foot, D-Pad/stick movement works.
- **Lifecycle hooks not wired**: `nativePause`/`nativeResume`/`nativeAccelerometer`/`nativeDone`/
  `nativeOpenIGM`/`nativeCanInterrupt` are exported by the `.so` but not yet called from `main.c`
  — no suspend/resume or in-game menu integration yet.

---

## 📋 Prerequisites

To run this port on your PS Vita, you will need:

1. A PS Vita running Custom Firmware (**HENkaku** or **Enso**), firmware 3.60/3.65 or later
   recommended.
2. [**kubridge**](https://github.com/TheOfficialFloW/kubridge/releases) installed as a kernel
   plugin (`ur0:tai/config.txt` under `*KERNEL`).
3. [**libshacccg.suprx**](https://github.com/Rinnegatamante/ShaRKBR33D/releases/latest) installed
   in `ur0:data/`.
4. A legally obtained copy of **Gangstar Miami Vindication HD**
   (`Gangstar-Miami-Vindication-HD.apk`, package `com.gameloft.android.TBFV.GloftGMHP.ML`).

---

## 📦 Installation Instructions

1. Install the `gangstarmiamivindication.vpk` file on your console using **VitaShell**.
2. On your PC, place `Gangstar-Miami-Vindication-HD.apk` in the project root (or extract it into
   `gangstarmiamivindication_extract/`).
3. Use **psvita-port-toolkit** (the standalone tool this port is managed with) to prepare the
   asset files — open the toolkit and select "Continuar con un port existente" pointing at this
   folder.
4. Transfer the game data to `ux0:data/gangstarmiamivindication/` via FTP or USB using VitaShell,
   as-is — this project does not modify original game assets (see "Known Issues" for what that
   means for `intro.m4v` specifically).

### Final File Structure in `ux0:data/gangstarmiamivindication/`

```text
ux0:data/gangstarmiamivindication/
├── libGangster2.so     <- Native library extracted from lib/armeabi/
├── data/               <- Game data files (.bdae, .bsprite, .gmap, .bmp, intro.m4v, ...)
├── res/                <- drawable/layout/raw resources extracted from the APK
├── saves/               <- Save slots (created at runtime)
└── logs/                <- Incremental debug logs (debug_local_NNN.log)
```

---

## 🛠️ Building from Source

This port does **not** keep a local copy of `porting_tools/` — all build, deploy, log, LiveArea,
and crash-dump workflows are handled by **psvita-port-toolkit**, a standalone tool kept outside
this repository.

### Build Prerequisites

- **VitaSDK**, with `kubridge`, `vitaGL`, `vitashark`, `mathneon` available.
- CMake and Make.

### Build Steps

```bash
cmake -Bbuild .
cmake --build build
```

This produces `build/gangstarmiamivindication.vpk`. For day-to-day development (build + deploy +
crash-dump parsing), use **psvita-port-toolkit** instead of raw `cmake`/`make`.

---

## 🏗️ Project Structure

- `source/`: Native C/C++ loader (lifecycle, GLES rendering, audio, video, input, JNI resource
  loader).
- `lib/`: Auxiliary libraries (`so_util`, `falso_jni`, `libc_bridge`, `fios`, `vitaGL`,
  `vitashark`, `sha1`).
- `extras/`: LiveArea assets (`icon0.png`, `bg0.png`, `pic0.png`, `startup.png`, `template.xml`),
  plus `cpuinfo`/`meminfo` and debug scripts.
- `PORTING_PLAN.md`: Living plan — confirmed engine findings, JNI export table, checklist.
- `port_progress.md`: Bug-by-bug diagnosis log, one confirmed bug at a time.
- `RELEASES.md`: Test-build release notes — what each alpha proves on hardware, all known
  performance/graphics/FPS errors, and what is still unconfirmed.

---

## ⚖️ Disclaimer

**Gangstar Miami Vindication** is a registered trademark of Gameloft. The work presented in this
repository is not "official" or produced or sanctioned by Gameloft or any other registered
trademark mentioned in this repository.

This software does not contain the original code, executables, assets, or other
non-redistributable parts of the original game product. The authors of this work do not promote
or condone piracy in any way. To launch and play the game on their PS Vita device, users must
possess their own legally obtained copy of the game in the form of an `.apk` file.

---

## 👥 Credits and Acknowledgements

- **Gameloft**: Original developers of Gangstar Miami Vindication.
- **TheFloW**: For `so_util`, `kubridge`, and foundational techniques for loading Android
  executables on PS Vita.
- **Rinnegatamante**: For `vitaGL` and continued support to the PS Vita porting scene.
- **v-atamanenko**: For `FalsoJNI` and the `soloader-boilerplate` base template.
- **Vita Community**: To all developers and enthusiasts in the PS Vita homebrew community.

---

## License

This software may be modified and distributed under the terms of the MIT license. See the
[LICENSE](LICENSE) file for details.
