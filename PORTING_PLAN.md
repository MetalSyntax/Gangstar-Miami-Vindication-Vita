# Plan de Port — Gangstar Miami Vindication (PS Vita)

> Generado por psvita-port-toolkit el 2026-08-23. Punto de partida con lo detectado automáticamente --
confirmar todo con objdump/Ghidra/jadx a mano antes de asumirlo como cierto.

## 0. Contexto

- **Juego:** Gangstar Miami Vindication
- **Paquete Java:** com.gameloft.android.TBFV.GloftGMHP.ML
- **APK original:** `Gangstar-Miami-Vindication-HD.apk`
- **TITLEID asignado:** `PSVGMV002`

**¿Motor conocido?** Revisar si algún port hermano (bajo la misma BASE_DIR) comparte motor antes de
reusar su código -- confirmar con símbolos JNI reales, no por analogía superficial.

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


## 4. Checklist

- [x] Repo creado desde soloader-boilerplate, git init, .gitignore anti-DMCA.
- [x] APK decompilado (jadx) y .so decompilado(s) (Ghidra) -- ver sección 2/3.
- [x] Análisis del motor real (ciclo de vida nativo, reuso de otro port o boilerplate genérico) --
      ver sección 3 y `port_progress.md` Fase 3 (2026-08-31).
- [x] Bootstrap del loader: so_file_load/so_relocate/so_resolve, primer build -- `source/main.c`
      reescrito (ya no depende de `JNI_OnLoad`, inexistente en este .so), build verde el 2026-08-31
      (se arreglaron además: `lib/falso_jni` vacío -- submódulo nunca clonado -- y colisión de
      símbolos EGL entre `source/reimpl/egl.c` y el vitaGL instalado, que ahora trae su propio EGL).
- [x] Tabla JNI (FalsoJNI): completada el 2026-09-01 con los **57 métodos** que el log real de
      consola muestra que el motor pide (`source/java.c`) -- carga de recursos
      (`getResourceFull`/`getResourceBytes`/`getResourceLength`, semántica sacada de
      `GLResLoader.java`), audio (aceptado e ignorado hasta portar audio), dispositivo/sistema, y
      el SDK muerto de Verizon. Ver `port_progress.md` Fase 5.
- [x] Símbolos importados: los 26 que faltaban en `source/dynlib.c` (`__aeabi_i2f` y demás helpers
      ARM EABI de float/double, `__dso_handle`, `__isfinitef`, `_ZSt7nothrow`,
      `_ZnajRKSt9nothrow_t`) agregados el 2026-09-01. Verificado con `objdump -T`: cero símbolos
      sin resolver.
- [x] Primer arranque en consola real: el motor arranca y corre (~17 s en la corrida del
      2026-09-01), ya pasando el crash de `pthread_mutex_unlock` que bloqueaba el boot.
- [ ] Gráficos (wrappers GL según versión detectada).
- [ ] Input, Audio, Assets, LiveArea/VPK.
- [ ] Pruebas en hardware real.

## 5. Herramientas

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
