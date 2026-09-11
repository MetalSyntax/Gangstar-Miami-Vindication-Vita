# Plan de Port — Gangstar Miami Vindication (PS Vita)

> Generado por psvita-port-toolkit el 2026-08-23; **documento vivo, última revisión 2026-09-07.**
> Lo que dice "CONFIRMADO" se verificó a mano contra el `.so` real (objdump/Ghidra), el Java de jadx
> o un log de consola física. Lo que no lo dice sigue siendo detección automática -- no asumirlo.
> La bitácora cronológica, bug por bug, está en `port_progress.md`.

## 0. Contexto

- **Juego:** Gangstar Miami Vindication
- **Paquete Java:** com.gameloft.android.TBFV.GloftGMHP.ML
- **APK original:** `Gangstar-Miami-Vindication-HD.apk`
- **TITLEID asignado:** `PSVGMV002`

**Motor: CONFIRMADO 2026-09-05.** Es **glitch engine 0.1.0.2**, el motor propio de Gameloft (no
Unity/UE/cocos2d, y ningún port hermano lo comparte -- no hay código que reusar). Identificado por
la línea que el propio motor imprime al arrancar (`[ALOG][GameLoft Printer::log] Glitch Engine
version 0.1.0.2`, ver `logs/debug_local_019.log`) y por el namespace `glitch::` en todo el pseudo-C
de Ghidra (`glitch::os::Printer`, `glitch::io::createReadFile`, `glitch::IReferenceCounted`,
`glitch::ILogger`, `glitch::scene_node`).

Lo que eso implica y ya está confirmado:

- **C++ con STLport**, no libstdc++ (`std::_Node_Alloc_Lock::_S_lock` aparece en el arranque).
- **dlmalloc propio, linkeado estáticamente**: el juego no usa el malloc de newlib para su heap
  interno. Ver `port_progress.md` Fase 8 -- por eso un log por lock de mutex se traduce en dos
  round-trips a la memory card por cada `malloc()`/`free()` del juego.
- **Formatos de asset propios:** `.bdae` (modelos/escenas), `.bsprite` + `.bmp` (sprites/HUD),
  `.gmap`, `.array`, `.english` (textos). Los `.bdae` son contenedores que el motor recorre con
  miles de `fread()`/`fseek()` chicos.
- **Su logger manda la format string SIN expandir** a `__android_log_write()` (ver Fase 11) --
  cualquier `[ALOG]` con `%s` va a aparecer literal en el log. Es correcto, no es un bug nuestro.

## 1. Detección automática

- **ABI(s):** armeabi
- **ABI elegida:** armeabi
- **Nota de arquitectura:** Solo armeabi (ARMv6, soft-float) -- Vita lo ejecuta igual (ARMv7 es superset), sin NEON de v7a.
- **Versión de GLES (CONFIRMADO 2026-08-31 vía `objdump -T` sobre el .so real):** GLES 1.1, pipeline
  fijo (`glMatrixMode`/`glLoadMatrixf`/`glVertexPointer`/`glTexParameterx`), **más** VBOs
  (`glBindBuffer`/`glBufferData`/`glBufferSubData`) y la extensión OES de FBO
  (`glBindFramebufferOES`/`glCheckFramebufferStatusOES`/`glBlendEquationOES`) para render-to-texture.
  No hay símbolos de shaders/programa (`glCreateShader`, `glUseProgram`) -- confirmado que NO usa GLES2.
  El wrapper GL de Vita necesita exponer FBO OES y VBOs, no solo el pipeline fijo mínimo.

## 2. .so encontrados (ABI armeabi)

- `gangstarmiamivindication_extract/lib/armeabi/._libGangster2.so` (4 KB)
- `gangstarmiamivindication_extract/lib/armeabi/libGangster2.so` (9564 KB)
- `ux0_data/gangstarmiamivindication/libGangster2.so` -- copia usada para el análisis real (la
  detección automática de la sección 3 se había hecho sobre el .so equivocado o falló).

## 3. Exports JNI (convención `Java_*`) -- CONFIRMADO 2026-08-31

**24 exports `Java_*` confirmados con `objdump -T ux0_data/.../libGangster2.so`.** No hay
`JNI_OnLoad` ni `RegisterNatives` en la tabla de símbolos (`objdump -T | grep -i onload/registernatives`
no devuelve nada) -- el motor se registra 100% por convención de nombre estática, nunca por
`RegisterNatives`.

**Impacto crítico para el bootstrap (`source/main.c`):** el boilerplate actual busca el símbolo
`JNI_OnLoad` con `so_symbol` y lo llama a través de un puntero de función -- ese símbolo no existe en
este .so, así que esa llamada crashea con un puntero NULL apenas se pruebe. Hay que **resolver cada
`Java_*` directamente con `so_symbol`** (o registrarlos a mano en FalsoJNI si el runtime lo requiere)
en vez de depender de `JNI_OnLoad`.

Exports confirmados (clase → métodos):

- `Gangster2`: `nativeInit(int)`, `nativeResume(int,int)`, `nativePause(int,int)`,
  `nativeCanInterrupt()`, `nativeSetPhone(int)`, `nativeGetInfo(String,String,String,String)`,
  `nativeAccelerometer(float,float,float)`, `nativeKeyDown(int)`, `nativeKeyUp(int)`,
  `nativeonTrackballEvent(int)`, `nativeOpenIGM()`
- `GameRenderer`: `nativeInit(int)`, `nativeResize(int,int)`, `nativeRender()`, `nativeDone()`,
  `nativeGetJNIEnv()` (método de instancia)
- `GameGLSurfaceView`: `nativeOnTouch(int,int,int,long,int,int)`
- `GLResLoader`: `nativeInit(int)`
- `GLMediaPlayer`: `nativeInit(int)`, `nativeGetTotalSounds()`, `nativeGetTotalSoundsOfSameInstance()`,
  `nativeGetSoundFileName()`, `nativeSetStopOnMusic()`
- `MyVideoView`: `nativeSetOnVideoCompletion()`

### Ciclo de vida nativo real (confirmado leyendo `decompiled/apk_jadx/sources/.../Gangster2.java`,
`GameRenderer.java`, `GameGLSurfaceView.java`, `GLResLoader.java`)

1. `Activity.onCreate()` → `Gangster2.nativeSetPhone(dm.widthPixels)` (ancho de pantalla, antes de
   crear el contexto GL) → crea `GameGLSurfaceView` (esto dispara el hilo de render).
2. `GLSurfaceView.Renderer.onSurfaceCreated()` (hilo GL, contexto EGL ya creado), **en este orden**:
   `GameRenderer.nativeGetJNIEnv()` → `GLResLoader.nativeInit(0)` → `GLMediaPlayer.nativeInit(...)` →
   `Gangster2.nativeInit(isDemo ? 1 : 0)` (la función más grande, 0x25c bytes -- init real del juego)
   → `GameRenderer.nativeInit(1)`.
3. `onSurfaceChanged(w,h)` → `GameRenderer.nativeResize(w,h)`.
4. Loop de render (objetivo 30 FPS, `Thread.sleep` para completar 33 ms por frame):
   procesa flags de pausa/resume (`Gangster2.nativePause`/`nativeResume`) → procesa
   `s_keyDownCode`/`s_keyUpCode` (`Gangster2.nativeKeyDown`/`nativeKeyUp`) → `GameRenderer.nativeRender()`.
5. Touch → `GameGLSurfaceView.nativeOnTouch(action, x, y, pointerId, 0, 0)` (action: 1=down, 2=move,
   0=up).
6. Acelerómetro → `Gangster2.nativeAccelerometer(x, y, z)`.
7. Pausa real de Activity → `Gangster2.nativeCanInterrupt()` luego `Gangster2.nativePause(1, onPauseCount)`;
   resume → `Gangster2.nativeResume(1 o 2, onPauseCount)` (el "2" es el resume "interno"/focus, el "1"
   es resume real desde `onResume()`).


## 4. Mapa de despacho JNI -- AUDITADO COMPLETO 2026-09-05

El `.so` no llama a `RegisterNatives`: pide cada método por nombre con `GetStaticMethodID` y después
lo invoca por el offset de la variante correspondiente en `JNINativeInterface`. Contando los sitios
sobre el pseudo-C de Ghidra, el binario entero usa **exactamente cuatro** entradas de esa tabla:

| Offset  | Índice | Función                  | Sitios | Cubierto por          |
|---------|--------|--------------------------|--------|-----------------------|
| `0x1c4` | 113    | `GetStaticMethodID`      | 57     | `nameToMethodId[]` (57 entradas, coincide 1:1) |
| `0x1c8` | 114    | `CallStaticObjectMethod` | 5      | `methodsObject[]`     |
| `0x204` | 129    | `CallStaticIntMethod`    | 18     | `methodsInt[]`        |
| `0x234` | 141    | `CallStaticVoidMethod`   | 34     | `methodsVoid[]`       |

Cruzados uno por uno contra `source/java.c`, el único hueco que había eran `setMusicGain`/
`setSfxGain`/`setVfxGain`: son `void` en el Java pero el motor los llama por `CallStaticIntMethod`,
así que estaban solo en `methodsVoid[]` y `methodIntCall()` no los encontraba (Fase 10). Ya
corregido. **No queda ningún otro método sin cubrir.**

Regla para el futuro: no alcanza con mirar la firma Java para decidir en qué tabla va un método --
hay que mirar por qué offset lo llama el `.so`.

## 5. Filesystem: traducción Android → Vita

El `.so` tiene su raíz de contenido compilada como literal y **no hay ningún JNI para cambiarla**,
así que la corrección va en el camino al filesystem (`source/reimpl/io.c`, Fase 9):

| Ruta Android                                          | Vita                    |
|-------------------------------------------------------|-------------------------|
| `/sdcard/gameloft/games/Gangstar2`                     | `DATA_PATH "data"`      |
| `/data/data/com.gameloft.android.TBFV.GloftGMHP.ML`    | `DATA_PATH "saves"`     |
| `/sdcard` (cualquier otra cosa: IGP/ads)               | `DATA_PATH "data"`      |

Además normaliza los `//` y los segmentos `.` (el motor concatena su raíz con nombres que ya
empiezan con `./`, así que lo que llega a `fopen()` es literalmente
`./sdcard/gameloft/games/Gangstar2//./about.english`, que sceLibcBridge no resuelve). Se aplica en
`fopen`, `freopen`, `open`, `stat`, `lstat`, `opendir`, `access`, `chdir`, `chmod`, `mkdir`,
`remove`, `rename`, `rmdir`, `unlink` y `realpath`.

**Datos:** 3214 archivos en `ux0:data/gangstarmiamivindication/data/`, verificados contra el
`zipinfo` del zip original -- no falta ninguno.

## 6. Logging: tres niveles, y por qué

Aprendido a los golpes (Fases 8, 10 y 11). El logging de este port **cambia el comportamiento bajo
prueba** si se pasa de volumen, así que está estratificado a propósito:

| Nivel | Macro | ¿Compila en Release? | Para qué |
|-------|-------|----------------------|----------|
| Siempre | `l_error` / `l_fatal` / **`l_note`** | Sí | El `[ALOG]` del motor (INFO/WARN), el latido de frames `[022]`, `fopen()` fallidos. Bajo volumen, alta señal. |
| Debug | `l_debug` / `l_info` / `l_warn` | No (`DEBUG_SOLOADER`) | Todo `io.c`, los checkpoints numerados, FalsoJNI verboso (`FALSOJNI_DEBUGLEVEL=0`). |
| Opt-in | `PTHR_TRACE_LOCKS`, `IO_TRACE_STREAMS` | No (opciones CMake, OFF) | Un lock o un `fread()` por línea. Solo para cazar un bug puntual: a este volumen el juego deja de reproducir lo que hace sin el trace. |

**Nunca** pasar una cadena de runtime como format string (Fase 11): `sceClibPrintf("%s", buf)`,
jamás `sceClibPrintf(buf)`. El motor manda mensajes con `%s` adentro.

## 7. Checklist

- [x] Repo creado desde soloader-boilerplate, git init, .gitignore anti-DMCA.
- [x] APK decompilado (jadx) y .so decompilado(s) (Ghidra) -- ver sección 2/3.
- [x] Análisis del motor real (ciclo de vida nativo, motor identificado) -- secciones 0 y 3,
      `port_progress.md` Fase 3.
- [x] Bootstrap del loader: `source/main.c` reescrito (no depende de `JNI_OnLoad`, inexistente en
      este .so). Build verde desde el 2026-08-31.
- [x] Tabla JNI (FalsoJNI): 57 métodos en `source/java.c`, **auditada completa** contra los sitios
      de despacho reales del binario el 2026-09-05 (sección 4).
- [x] Símbolos importados: cero sin resolver (`objdump -T`).
- [x] Primer arranque en consola real (2026-09-01).
- [x] **Assets**: capa de traducción de rutas Android → Vita, los 3214 archivos en su lugar
      (sección 5, Fase 9).
- [x] **Input**: touch frontal (5 slots, mapeo a `nativeOnTouch` down/move/up) +
      botones como keycodes Android por el camino `s_keyDownCode/s_keyUpCode`
      (dpad → 19-22, CROSS → 23, CIRCLE → 4/BACK, START → 82/MENU) + heartbeat
      por tiempo y slow-frames en release (`source/main.c`, Fase 13).
- [x] **Gráficos (parcial)**: vitaGL vendorizado (`lib/vitaGL` + `lib/vitashark`), shaders GLSL con
  `DUMP_COMPILED_SHADERS`, y el spoof de `glGetString(GL_VERSION)` a "OpenGL ES 1.1" que el
  motor exige (`source/reimpl/gl.h`, Fase 7). **Todavía no se vio un solo frame dibujado
  (2026-09-07: el juego corre a 30 fps con pantalla negra; instrumentación `[GL]` +
  capturas BMP cada ~20 s en vuelo, gap `SPOT_*` de vitaGL parchado en `ffp.c` —
  ver `port_progress.md` Fases 15-17, pendiente verificar en consola).**
- [ ] **Gráficos (real)**: que el motor dibuje. Pendiente el warning
  `parameter type mismatch when setting "%s/%s"` del camino de material/shader (Fase 11),
  más la pista nueva: 69 `Loaded texture` del engine vs un puñado de uploads GL
  (posible textura negra vía PVR) o cámara fuera de vista (los BMP lo dirán).
- [x] **Audio (Fase 31, 2026-09-11)**: backend real en `source/utils/audio.c`
  (SceAudioOut 48 kHz stereo + libvorbisfile: SFX cortos decodificados y
  cacheados, música/radio/voz en streaming con loop, hasta 8+4 voces,
  ganancias music/sfx/vfx). Los 26 métodos de `GLMediaPlayer` en
  `source/java.c` pasaron de stubs a manejar estado real; `isSoundLoaded*`
  (0/-1) e `isMediaPlaying` (1/0) responden desde el backend. Causa raíz
  del doble síntoma 10-13 fps + silencio: el motor reemitía
  playRadio/playSound cada frame al leer siempre "not playing".
- [x] **Salida limpia**: `Gangster2.Exit()` (`java.c`) termina el proceso
  en vez de colgar en el loop `Native Exit Triggered` del log 036.
- [ ] **Video**: sin portar. `loadMovie()` devuelve 1 y dispara
      `nativeSetOnVideoCompletion()` al instante, como si el clip terminara solo (Fase 6).
- [ ] **Ciclo de vida incompleto**: `nativePause`/`nativeResume`/`nativeAccelerometer`/`nativeDone`/
      `nativeOpenIGM`/`nativeCanInterrupt` están exportados pero **no cableados** en `main.c`.
      Hacen falta para suspender/reanudar la consola y para el menú in-game.
- [ ] LiveArea/VPK definitivos.
- [ ] Pruebas de juego en hardware real.

## 8. Herramientas

Este port se gestiona con **psvita-port-toolkit** (standalone, fuera de este repo). Desde el
toolkit: `Continuar con un port existente` → elegí esta carpeta (ya tiene `.psvita-toolkit.json`).

## Auto-detected lifecycle methods (psvita-toolkit)

Native methods whose name matches a well-known Android/GL app lifecycle hook --
these are the ones `main.c`/the loader most likely needs to call directly to
drive the game (there's no real Android `Activity`/`GLSurfaceView` calling them
for you).

- `com.gameloft.android.TBFV.GloftGMHP.ML.GLBluetooth.nativeInit(void)`
- `com.gameloft.android.TBFV.GloftGMHP.ML.GLMediaPlayer.nativeInit(int)`
- `com.gameloft.android.TBFV.GloftGMHP.ML.GLResLoader.nativeInit(int)`
- `com.gameloft.android.TBFV.GloftGMHP.ML.GameRenderer.nativeInit(int)`
- `com.gameloft.android.TBFV.GloftGMHP.ML.GameRenderer.nativeRender(void)`
- `com.gameloft.android.TBFV.GloftGMHP.ML.GameRenderer.nativeResize(int, int)`
- `com.gameloft.android.TBFV.GloftGMHP.ML.Gangster2.nativeInit(int)`
