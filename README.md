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
  <img src="https://img.shields.io/badge/Status-Playable%20(WIP)-yellow.svg?style=flat-square" alt="Status: Playable, work in progress" />
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

### 🎮 Current Status: Playable (Work in Progress)

The game boots, renders, and reaches real third-person gameplay — driving, on-foot, vehicle entry —
at **20-30 fps**. It got there through a long bug-by-bug history (format-string crashes, a missing
`HAVE_SOFTFP_ABI` causing an all-black screen, a circular-pool GPU stall dragging it to ~9 fps,
among others); see [`port_progress.md`](port_progress.md) for the full diagnosis log, one confirmed
bug at a time, and [`PORTING_PLAN.md`](PORTING_PLAN.md) for the living engine/JNI map.

It is **not yet polished** — see "Known Issues" below.

### ✨ What Works

- **Native ARM Execution**: `libGangster2.so` (armeabi/ARMv6, soft-float) runs directly on the
  Vita's CPU via the soloader.
- **vitaGL Graphics Pipeline**: GLES 1.1 fixed-function rendering at the native 960x544 panel
  resolution (the engine's own internal resolution is 960x480; touch input and viewport are
  remapped to match).
- **Audio**: Real backend (`sceAudioOut` + `libvorbisfile`) — short SFX decoded and cached, up to
  8+4 concurrent streamed voices for music/radio/voice with looping, independent music/SFX/VFX
  gain control.
- **Intro cutscene**: Plays via `SceAvPlayer` (hardware H.264 decoder), letterboxed to the full
  panel, skippable with Cross/Start. Note: the original `intro.m4v` ships as MPEG-4 Part 2, which
  the Vita's hardware decoder cannot play — see "Known Issues" below. The original asset is never
  altered by this project.
- **Touch + physical input**: Front touchscreen mapped to the engine's multi-touch slots, D-Pad/
  Cross/Circle/Start mapped to the same Android keycodes the real device's keyboard would send.
- **Overclocked + tuned for the Cortex-A9**: CPU/Bus/GPU/GPU-Xbar clocks at their ceiling, NEON
  codegen, a tuned vitaGL speedhack set, and in-memory caches for path translation and sound
  existence checks to cut down on SD-card I/O during gameplay.

### ⚠️ Known Issues

- **Some characters/vehicles can render solid black.** Under investigation — a vitaGL vertex-data
  speedhack racing the GPU on large meshes was ruled out on real hardware (removing it only cost
  performance); the current suspect is a matrix-math speedhack that can silently drop the active
  matrix stack for `glOrtho`/`glFrustum` (see `port_progress.md`, latest phase).
- **Intro cutscene shows no video** with the original `intro.m4v` (MPEG-4 Part 2 — the Vita's
  hardware video decoder only supports H.264/AVC). This project does not transcode or otherwise
  alter original game assets; audio and the rest of the boot sequence are unaffected.
- **Multi-second freeze the first time a vehicle (or new area) loads.** Same class of one-time
  shader-compile/asset-load stall already seen at the title screen; the on-disk shader cache
  should make repeat loads of the same asset fast.
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
