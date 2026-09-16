# Gangstar Miami Vindication — Port a PS Vita

Port de `Gangstar-Miami-Vindication-HD.apk` (Android) a PS Vita vía soloader. Generado con **psvita-port-toolkit**.

## Estructura

- `gangstarmiamivindication_extract/` — APK extraído (gitignored).
- `decompiled/` — Java (jadx) y pseudo-C (Ghidra) del/los .so (gitignored, regenerable).
- `source/`, `lib/so_util`, `lib/falso_jni` — scaffold del boilerplate (SoLoader + FalsoJNI).
- `PORTING_PLAN.md` — plan vivo, actualizar a medida que se confirman cosas del motor real.
- `port_progress.md` — bitácora, un bug confirmado a la vez.
- `.psvita-toolkit.json` — config para el toolkit standalone (build/deploy/logs/LiveArea/crash dumps).

Este port **no** tiene una copia local de `porting_tools/` -- todo el build/deploy/debug se maneja
desde **psvita-port-toolkit**, la herramienta standalone (fuera de este repo). Abrí el toolkit y
elegí "Continuar con un port existente" apuntando a esta carpeta.

## Hallazgos de motor (CONFIRMADOS -- ver PORTING_PLAN.md secciones 0-6)

- **Motor: glitch engine 0.1.0.2**, propio de Gameloft. C++ con STLport, dlmalloc propio linkeado
  estático, assets `.bdae`/`.bsprite`/`.bmp`/`.gmap`. Ningún port hermano lo comparte.
- **ABI:** armeabi (ARMv6, soft-float).
- **GLES 1.1**, pipeline fijo + VBOs + FBO OES. NO usa GLES2 (confirmado con `objdump -T`: no hay
  `glCreateShader`/`glUseProgram`). El motor rechaza cualquier `GL_VERSION` > 1.99, así que
  `source/reimpl/gl.c` spoofea "OpenGL ES 1.1".
- **Paquete Java:** com.gameloft.android.TBFV.GloftGMHP.ML
- **JNI:** sin `JNI_OnLoad` ni `RegisterNatives` -- 24 exports `Java_*` por convención de nombre,
  resueltos a mano en `source/main.c`. La tabla de 57 métodos de `source/java.c` está **auditada
  completa** contra los cuatro offsets de `JNINativeInterface` que el binario realmente usa.

## Reglas aprendidas a los golpes (no repetir)

1. **El logging cambia el comportamiento bajo prueba.** Un log por `malloc()` o por `fread()` hace
   que la carga de assets tarde una eternidad y parezca un deadlock. `PTHR_TRACE_LOCKS` y
   `IO_TRACE_STREAMS` están OFF por default por eso.
2. **Nunca pasar una cadena de runtime como format string.** `sceClibPrintf("%s", buf)`, jamás
   `sceClibPrintf(buf)` -- el motor manda mensajes con `%s` sin expandir.
3. **Un build de release no es ciego:** `l_note()` (siempre compilado) lleva el `[ALOG]` del motor
   y el latido de frames. Preferir release para reproducir; debug solo para cazar un bug puntual.
4. **La firma Java no dice en qué tabla JNI va un método** -- hay que mirar por qué offset de
   `JNINativeInterface` lo llama el `.so`.
5. **`SceAvPlayer` solo decodifica H.264/AVC por hardware.** `intro.m4v` (extraído del APK, 2011)
   es MPEG-4 Part 2 (Simple Profile) -- confirmado con `ffprobe`, se abre bien (contenedor MP4
   válido) pero no produce ni un frame de video por esa vía. **No transcodificar/alterar el
   asset original para "arreglar" esto** -- el asset se deja tal cual sale del APK. `video.cpp`
   sigue el método de Shadow Guardian-vita (SceAvPlayer) sin cambios; con este asset puntual el
   intro no se ve, y eso es un límite conocido, no un bug del loader (ver Fase 38/40 en
   `port_progress.md`).

## Flujo de trabajo esperado

1. Análisis de símbolos antes de tocar loader/source -- skill `psvita-port-init` cubrió la Fase 0-2.
2. Bootstrap del loader guiado por la skill `psvita-porting`.
3. Build/deploy con el toolkit standalone → probar en consola real.
4. Un bug a la vez, guiado por el log real -- skill `so-crash-triage`.
5. Actualizar `port_progress.md` con cada bug confirmado.
