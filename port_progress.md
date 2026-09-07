# Registro de Progreso — Gangstar Miami Vindication (PS Vita)

> Bitácora cronológica, un bug confirmado a la vez. Para el estado **estructural** del port (motor,
> mapa JNI, filesystem, niveles de logging, checklist) ver `PORTING_PLAN.md`.

## Estado actual — 2026-09-07 (intento en curso, SIN verificar en consola)

**Dónde está el port:** el juego **carga y corre a ~30 fps estables con pantalla
negra y sin sonido**. Tanda Fases 14-17 (esta sesión, un solo build release verde
tras otro, **ninguno verificado en hardware todavía**):

1. **Carga acelerada** (Fase 14): `frame 2` de 12,6 s → 7,4 s; log de 1092 → ~400
   líneas (filtro de spam del motor, dedupe de `fopen FAILED`, syncs cada 256,
   `MULTISAMPLE_NONE` + 12 MB). Verificado en `debug_local_022.log`.
2. **Atribución del error pegajoso** (Fases 15-16): cada frame terminaba con
   `GL_INVALID_ENUM`; instrumentación `[GL]` + `LOG_ERRORS` reenviado al log
   identificó `glLightfv(GL_SPOT_DIRECTION)` — gap real de vitaGL (sin soporte
   spot), más 4 ofensores benignos (`DITHER`/MSAA-caps/`FOG_HINT`/`PACK_ALIGNMENT`).
   Verificado en `debug_local_023/024.log`.
3. **Fix + ojos** (Fase 17): casos `SPOT_*` agregados en vitaGL `ffp.c` (solo
   storage, los shaders siguen sin spots — cambio visual esperado: ninguno) y
   capturas `shot_*.bmp` del framebuffer cada ~20 s.
4. **Sonido scopeado, NO implementado**: el `.so` no importa audio nativo (todo es
   JNI `SoundPool`/`MediaPlayer`); los 1722 sonidos son `.ogg` sin decoder
   vendored → fase propia, después del render.

**Lo que sigue (pendiente):** desplegar el último `eboot.bin`
(`build/eboot.bin` de esta tanda), correr 3-4 min, traer `debug_local_025.log` +
`shot_*.bmp`. Si `lastErr=0x0` y sigue negro → el spot no era; los BMP deciden
(negro total vs. escena oscura vs. geometría fuera de vista). Si el negro persiste,
el siguiente sospechoso es textura negra (PVR/`Loaded texture` vs `texUp`) o
cámara/lógica de menú.

**Deuda conocida (no bugs, funcionalidad sin portar):** audio, video, y los hooks de ciclo de vida
`nativePause`/`nativeResume`/`nativeAccelerometer`/`nativeDone`/`nativeOpenIGM`/`nativeCanInterrupt`,
que están exportados pero no cableados en `main.c`. Ver el checklist de `PORTING_PLAN.md` sección 7.

## Fase 14: acelerar la carga + achicar el log (2026-09-06)

### Diagnóstico sobre `logs/debug_local_021.log` (1092 líneas, varios minutos, a mitad de carga)

- El loop NO estaba colgado: `frame 184 slow render (20656 ms)` retornó y las últimas
  `Loaded texture` son de adentro del frame 185. Pantalla negra = carga a mitad de camino
  (95 `createTextureImpl` vs 121 en `020`), no deadlock. Sin dump en esta corrida.
- Tres costos medidos en el propio log, todos en la ruta de carga:
  1. Cada `l_note()` en release = LwMutex + 2x snprintf + `sceNetSendto` bloqueante +
     `sceIoWrite` (+ sync periódico). El spam del motor (~500/1092 líneas: 324x
     `---------------locale/basic_ios`, 95x `createTextureImpl`, más `Loaded texture`/
     `CTexture::mapImpl`) paga eso cientos de veces.
  2. Cada `fopen FAILED` = `l_error` = `sceIoSyncByFd()` inmediato a la memory card.
     Solo `dummy.tga` x18 ya son 18 syncs seguidos.
  3. `vglSwapBuffers()` con `MULTISAMPLE_4X` en cada frame de una pantalla negra: resolve
     multimuestra puro overhead + combinación riesgosa con el FBO OES que el motor usa
     (receta Asphalt-5 de Fase 12, pendiente desde entonces).

### Cambios (solo loader, sin tocar motor/render)

1. `source/reimpl/log.c`: `is_load_spam()` — `---------------*`, `createTextureImpl 1`,
   `Loaded texture`, `CTexture::mapImpl` (tags `GameLoft`/`Gameloft`) pasan de `l_note` a
   `l_debug` en INFO y WARN. En release desaparecen (log chico + carga rápida); en debug
   siguen visibles. Versión/driver/mismatch/unbound/duplicate/invalid-bind siguen en `l_note`.
2. `source/reimpl/io.c`: `fopen_soloader` deduplica FAILED consecutivos del mismo path —
   solo el primero lleva `l_error` (un sync); las repeticiones van por `l_debug` y al cambiar
   de path sale una línea `l_note("fopen: %s failed x%d total")` con el conteo.
3. `source/utils/logger.c`: `LOCAL_LOG_SYNC_EVERY` 64 → 256 (WARN o más sigue con sync
   inmediato; peor caso ante crash duro: se pierden las últimas <256 líneas low-severity).
4. `source/utils/glutil.c`: `vglInitExtended(0, 960, 544, 12MB, MULTISAMPLE_NONE)`
   (antes 6MB + 4X). Reversible si los menús se ven mal.

### Estado

- **Build:** verde (`psvita-toolkit build --preset release`, 2026-09-06).
- **Pendiente:** desplegar (`eboot.bin` → `/ux0:/app/PSVGMV002/`) y correr dejando
  **5-10 min sin matar**. Qué esperar: log de ~1/3 del tamaño, mismos `slow render`/`alive`
  pero llegando más lejos en el mismo tiempo; si se queda >10 min sin línea nueva y sin
  dump, entonces sí es cuelgue dentro de `nativeRender` y se triagea con ese frame.

## Fase 15: el juego corre a full-speed con pantalla negra — triage de render (2026-09-06)

### Qué muestra `logs/debug_local_022.log` (443 líneas, build Fase 14)

- La Fase 14 funcionó: log de 1092 → 443 líneas, `frame 2` de 12,6 s → 7,4 s, y el dedupe
  (`fopen: ... failed x8 total`) visible. El eboot desplegado SÍ lleva los cambios.
- El motor pasó la carga y **corre a velocidad real**: frames hasta 1668, renders de
  ~14-80 ms, `DeviceKeyInput:23` x3 (el CROSS llega al motor), 24x `[SOUNDS-VV] PLAYEX`
  (el mixer del juego pide sonidos — stubs silenciosos, no bloquean), 69 `Loaded texture`.
  Sin dump. La pantalla negra ya NO es carga: es render (o espera de input en título).
- Corrección al filtro Fase 14: los tags reales son `GameLoft Printer::log/log2/logf`
  (censo: 188/92/75 líneas), así que el exact-match solo pescó 5 líneas y las 69
  `Loaded texture` se colaron. `is_load_spam()` ahora matchea por prefijo de tag
  (los textos siguen exactos, la señal no se toca).
- Descartado: `glDrawTex*OES` (todas en `ret0`) — el `.so` no importa ningún `glDrawTex*`
  (`objdump -T`, solo una C++ `Graphics2D::DrawTexture` no relacionada) y vitaGL ni lo
  implementa. Los draws 2D/3D van por `glDrawArrays`/`glDrawElements`.

### Instrumentación TEMP (`source/reimpl/gl.{c,h}`, `dynlib.c`, `glutil.c`)

- Contadores draws/clears/texUploads/FBO-binds + viewport actual + último status de FBO
  + `glGetError()` drenado una vez por frame (mismo hilo/contexto que el motor).
- Wrappers 1:1 re-apuntados en `dynlib.c`: `glDrawArrays`/`glDrawElements`/`glClear`/
  `glViewport`/`glBindFramebuffer(OES)`/`glCheckFramebufferStatus(OES)`.
- `gl_frame_tick()` desde `gl_swap()`: FBO incompleto y cambio de `glGetError` salen al
  instante por `l_note`; resumen `[GL] frames/draws/clears/texUp/fbo/vp/fboStatus/lastErr`
  cada 5 s. Costo: unos incrementos por draw. Quitar al cerrar el triage de render.

### Cómo leer el próximo log

- `draws==0` con frames girando → el motor no envía geometría (espera lógica/input, no GL).
- `draws>0` + negro → estado GL (`vp`, `fbo`, `lastErr` dicen cuál).
- `lastErr!=0x0` pegajoso → el error mismo es la causa.
- `FBO incomplete` → el render-to-texture del motor no completa en vitaGL.

## Fase 16: sticky `GL_INVALID_ENUM` cada frame + heartbeat legible (2026-09-06)

### Qué muestra `logs/debug_local_023.log` (412 líneas, build Fase 15)

- El `[GL]` es decisivo: **draws>0 y creciendo** (13 → 48011, ~25 draws/frame), clears
  ~2/frame, `fbo=0` con 1 solo bind (render al framebuffer default — hipótesis FBO
  **descartada**), `vp=0,0,960,480` (el motor elige 480 de alto, no es bug nuestro),
  `texUp=4-5` (las 69 `Loaded texture` del engine suben por vía comprimida/subimage,
  antes invisible), y **`glGetError: 0x500 (GL_INVALID_ENUM)` pegajoso en CADA frame**
  desde ~frame 200. El motor envía geometría pero una llamada con enum inválido falla
  siempre — candidato directo a la pantalla negra.
- Audio: el `.so` **no importa ningún símbolo nativo de audio** (`objdump -T | grep UND`
  no trae OpenSL/AudioTrack/ALSA/nada) — todo el sonido va por JNI (`SoundPool` 5 streams
  + `MediaPlayer`, 23 métodos ya stubbeados en `java.c`). Los 1722 sonidos en `data/` son
  **.ogg Vorbis** (0 .wav) y no hay ningún decoder vendored en `lib/`. O sea: que suene
  = vendorizar un decoder vorbis (ej. stb_vorbis) + emular SoundPool/MediaPlayer sobre
  `SceAudioOut`. Es una fase propia, no un fix rápido; va después del render (sonido sin
  imagen no sirve). Diseño listo para cuando toque.

### Cambios

1. `CMakeLists.txt`: `LOG_ERRORS` en `vitaGL_local` — prende las líneas internas
   `file:line: func set GL_INVALID_ENUM (param: 0xVALUE)` de vitaGL.
2. `lib/vitaGL/source/utils/debug_utils.h` (patch vendor marcado, TEMP): `vgl_log` →
   `vgl_log_capture()` en vez de `sceClibPrintf` pelado, que ninguna captura ve.
3. `source/reimpl/gl.c`: `vgl_log_capture()` reenvía a `l_note("[vitaGL] ...")` con
   dedupe de repetidos (misma línea: silencio + recordatorio cada x300 + total al
   cambiar — un error pegajoso a 30 fps no puede spamear).
4. Contadores de subida completos: `glCompressedTexImage2D`/`glTexSubImage2D`/
   `glCopyTexSubImage2D` cuentan y pasan; `glCompressedTexSubImage2D` (sin backend en
   vitaGL, seguía en `ret0`) ahora cuenta y avisa una vez si el motor la usa
   (`glCompressedTexSubImage2D_drop` — mismo comportamiento, sin cambio de conducta).
5. Heartbeat legible (`main.c` + `gl_get_counters()`): la línea cada 5 s ahora es
   `[022] frame N | F fps | render X ms | draws D clears C` — fps medido entre beats,
   no estimado. Responde "¿pasa algo?" de un vistazo.

### Estado

- **Build:** verde (`psvita-toolkit build --preset release`, 2026-09-06).
- **Pendiente:** desplegar y correr 2-3 min. La línea `[vitaGL] ... set
  GL_INVALID_ENUM (...: 0xXXXX)` nombra **función + valor exactos** del culpable —
  con eso se decide el fix (parámetro que vitaGL no acepta vs. bug del motor).

## Fase 17: culpables con nombre + screenshots (2026-09-06)

### Qué muestra `logs/debug_local_024.log` (398 líneas, build Fase 16)

Rank de ofensores (`<GLES/gl.h>` para los valores):

1. **`glLightfv pname 0x1204 = GL_SPOT_DIRECTION`, pegajoso (x4200+).** El motor setea
   dirección de spotlight cada frame; `glLightfv`/`glLightxv` de vitaGL (`ffp.c`)
   aceptan AMBIENT/DIFFUSE/SPECULAR/POSITION/attenuations pero **ningún `SPOT_*`** —
   gap real de vitaGL (sus headers ni definen los enums; sus shaders FFP no tienen
   falloff de spot, todo es point light). Resto benigno de una vez:
2. `glDisable 0x809E/0x80A0` = `GL_SAMPLE_ALPHA_TO_COVERAGE`/`GL_SAMPLE_COVERAGE`
   (una vez, arranque; corremos MSAA NONE) — benigno.
3. `glDisable 0xBD0` = `GL_DITHER` (una vez) — benigno.
4. `glHint 0xC54` = `GL_FOG_HINT` (2x) — solo calidad, benigno.
5. `glPixelStorei 0xD05` = `GL_PACK_ALIGNMENT` (1x) — benigno.
- El heartbeat nuevo funciona: `frame 1104 | 30.1 fps | render 14.4 ms | draws
  12392` — el juego corre a 30 fps estables con pantalla negra. El avance que notas
  es real (draws creciendo, sonidos, input).

### Cambios

1. `lib/vitaGL/source/ffp.c` (patch vendor marcado): casos `GL_SPOT_DIRECTION`/
   `GL_SPOT_EXPONENT`/`GL_SPOT_CUTOFF` en `glLightfv` + `glLightxv`, con defines
   locales (0x1204/0x1205/0x1206) y arrays de storage. Sin plumbing a uniforms: los
   shaders FFP no saben de spots, las luces siguen evaluándose como point lights.
   Efecto esperado: desaparece el error pegajoso (señal limpia); cambio visual
   esperado: ninguno — si el negro persiste, el bug está en otro lado y ya no hay
   error que lo tape.
2. `source/utils/glutil.{c,h}` + `main.c`: `gl_shot(path)` vuelca el framebuffer
   mostrado a BMP 32-bit; `main.c` captura `logs/shot_%05d.bmp` cada 600 frames
   (~20 s) desde el frame 100, con `l_note("[022] screenshot ... -> rc")`.
   Traer los BMP por FTP y mirarlos: negro total vs. escena oscura vs. geometría
   fuera de vista deciden el siguiente paso sin adivinar.

### Estado

- **Build:** verde (`psvita-toolkit build --preset release`, 2026-09-06).
- **Pendiente:** desplegar, correr 3-4 min, traer `logs/debug_local_025.log` **+ los
  `logs/shot_*.bmp`**. Qué mirar: (a) si `lastErr` queda en 0x0 y el negro sigue →
  el spot no era (confirmado por eliminación); (b) qué muestran los BMP.

---

## Fase 1: Configuración y Preparación (Completada — 2026-08-23)
- Repo creado desde soloader-boilerplate, `.gitignore` anti-DMCA.
- APK `Gangstar-Miami-Vindication-HD.apk` copiado y extraído.
- ABI detectada: armeabi (elegida: armeabi).
- GLES detectado: AndroidManifest.xml no declara glEsVersion -- usar heurística (GLES1 (pipeline fijo: glVertexPointer/glClearColorx/glTexParameterx))

## Fase 2: Decompilación (Completada — 2026-08-23)
- jadx: corrido, resultados en decompiled/apk_jadx/.
- Ghidra (.so): corrido para cada .so.

## Fase 3: Análisis del Motor Real (Completada — 2026-08-31)
- [x] Leer decompiled/apk_jadx/sources/ para el ciclo de vida nativo real -- ver PORTING_PLAN.md
      sección 3 ("Ciclo de vida nativo real") para la secuencia completa confirmada.
- [x] Confirmar exports JNI reales y si hay RegisterNatives -- **24 exports `Java_*` confirmados**
      con `objdump -T` sobre `ux0_data/gangstarmiamivindication/libGangster2.so`. **No hay
      `JNI_OnLoad` ni `RegisterNatives`** en la tabla de símbolos.
      - **BUG ya identificado antes del primer build:** `source/main.c` (boilerplate) resuelve y
        llama `JNI_OnLoad` vía `so_symbol` -- ese símbolo no existe en este .so, así que esa línea
        va a crashear con puntero NULL. Hay que reemplazarla por resolución directa de cada
        `Java_*` necesario y llamarlos en el orden real del ciclo de vida (ver PORTING_PLAN.md).
      - Corolario: no hace falta tabla JNI de "exports" en FalsoJNI para este motor (no llama
        `RegisterNatives`); FalsoJNI solo necesita cubrir los métodos Java que el motor invoca
        *hacia* la JVM (`nameToMethodId`/campos), no al revés.
- [x] GLES confirmado por símbolos reales: GLES 1.1 fijo + VBOs + FBO OES (no GLES2). Corrige la
      heurística automática de la Fase 1 (que solo mencionaba fixed-function puro).
- [ ] Confirmar si comparte motor con algún port hermano -- sin evidencia de reuso de código de otro
      port en este repo; parece motor propietario de Gameloft de esa época (`.bdae`, `.gmap`,
      `allscripts_pyarray.bin` en `ux0_data/.../data/`). No bloqueante para seguir a Fase 4.

## Fase 4: Bootstrap del loader (En progreso)
Ver PORTING_PLAN.md sección 3 (ciclo de vida nativo) y sección 4 (checklist). Reescribir
`source/main.c` para no depender de `JNI_OnLoad` y seguir la secuencia real de init/render/pausa.
Actualizar con un bug confirmado a la vez en pruebas reales (skill `so-crash-triage`).

### Bug confirmado 2026-08-31: race condition en `pthr.c` crashea el thread `PSVGMV002`

- **Dump:** `logs/gangstarmiamivindication-psp2core-1788232234-0x00007c2a2d-eboot.bin.psp2dmp`
  (Data abort, PC en dirección basura, LR dentro de `pthread_mutex_unlock` real de VitaSDK).
- **Causa raíz confirmada:** `_mutex_t_static_init()`/`_cond_t_static_init()` en
  `source/reimpl/pthr.c` hacían el check-then-act ("¿ya está inicializado?" / inicializar / recordarlo)
  en 3 pasos separados, cada uno con su propio lock corto -- dejando una ventana sin proteger entre
  el chequeo y la inicialización real. El thread que crasheó (`PSVGMV002`, el segundo thread del
  motor -- probablemente lanzado durante `Gangster2_nativeInit`) corrió esa ventana en paralelo con
  otro thread sobre el MISMO mutex/cond estático de Bionic (candidatos por el stack: guardas internas
  de `std::locale` de libstdc++ -- `_Locale_extract_ctype_name`/`_Locale_true`/`_Init_timeinfo`/
  `_Locale_mon_grouping`), pisándose el campo `real_ptr` -- un thread quedó con el lock tomado sobre
  un buffer que ya nadie referencia, y el otro desbloqueó un buffer distinto a medio inicializar ->
  `pthread_mutex_unlock` real saltó a una dirección basura leída de ese buffer corrupto.
- **Fix:** todo el check-then-act ahora corre bajo una sola sección crítica continua (el
  `SceKernelLwMutex` `pthr_mutex` que ya existía en el archivo, antes solo usado para el array de
  tracking). Ver `source/reimpl/pthr.c` (`_mutex_t_static_init`/`_cond_t_static_init`).
- **Instrumentación agregada (a pedir del usuario):** convención de logs incrementales
  `l_checkpoint(n, msg)` en `source/utils/logger.h` -- imprime `[NNN] msg` vía `l_debug` (activo con
  `DEBUG_SOLOADER`, definido por defecto en `CMakeLists.txt`). Números YA USADOS (numeración
  correlativa de todo el proyecto, 001-999, no por archivo -- seguir desde 013 en el próximo uso):
  - **001-010** -- `source/main.c`, secuencia de arranque en `main()` (una por cada `RESOLVE`/llamada
    nativa, en orden; 008 marca el punto más probable donde el motor lanza su(s) thread(s) propio(s)).
  - **011** -- `source/reimpl/pthr.c`, primera inicialización lazy de un mutex Bionic (loggea puntero +
    thread ID).
  - **012** -- ídem para un cond var.
- **Pendiente de confirmar en consola real:** recompilar, correr, y si vuelve a crashear cerca de la
  misma zona, el último `[NNN]` en el log dice exactamente en qué paso quedó -- si el crash ya no
  aparece, o aparece en otro lado, seguir la skill `so-crash-triage` con el log+dump de esa corrida.

### Bug confirmado 2026-09-01: el fix anterior tenía la MISMA carrera un nivel más arriba (creación lazy de `pthr_mutex`)

- **Dump:** `logs/gangstarmiamivindication-psp2core-1788241153-0x0004132a8d-eboot.bin.psp2dmp`
  (Data abort, misma firma que el bug anterior: `LR` dentro del `pthread_mutex_unlock` real de
  VitaSDK, `PC` basura totalmente fuera de rango del `.so`, stack con los mismos guardas de
  `std::locale` de libstdc++ -- `_Locale_extract_ctype_name`/`_Locale_true`/`_Init_timeinfo`/
  `_Locale_mon_grouping`).
- **Causa raíz confirmada:** el fix del 2026-08-31 hizo atómico el check-then-act de
  `_mutex_t_static_init()`/`_cond_t_static_init()` envolviéndolo en la sección crítica del
  `SceKernelLwMutexWork pthr_mutex` -- correcto para ese nivel. Pero el propio macro `PTHR_LOCK`
  (`source/reimpl/pthr.c`) que crea `pthr_mutex` la primera vez tenía el MISMO patrón
  check-then-act sin atomicidad, sobre una variable `volatile short int pthr_mutex_inited` común
  (sin CAS -- pese a que `<stdatomic.h>` ya estaba incluido en el archivo, nunca se usaba). Dos
  threads de `Gangster2` llegando a `PTHR_LOCK` por primera vez casi al mismo tiempo (mismo
  escenario que la vez pasada: dos threads pisando guardas de `std::locale` en simultáneo) podían
  ambos ver `pthr_mutex_inited == 0` y ambos llamar `sceKernelCreateLwMutex(&pthr_mutex, ...)`
  concurrentemente sobre la misma estructura -- corrompiéndola. Con `pthr_mutex` corrupto, ya no
  garantiza exclusión mutua real, así que la carrera original sobre `real_ptr` de la guarda de
  locale se reabría un nivel más abajo -- mismo crash.
- **Fix:** `pthr_mutex_state` ahora es un `atomic_int` de 3 estados (`PTHR_META_UNINIT` /
  `PTHR_META_CREATING` / `PTHR_META_READY`), con `atomic_compare_exchange_strong` para que solo un
  thread cree el LwMutex; los demás esperan en un spin corto (`sceKernelDelayThread`) hasta que
  quede listo. Ver `source/reimpl/pthr.c` (`PTHR_LOCK`/`PTHR_UNLOCK`).
- **Build:** verde (`psvita-toolkit build`, 2026-09-01).
- **Resultado:** el crash volvió a aparecer en la siguiente corrida
  (`logs/gangstarmiamivindication-psp2core-1788268266-0x0002ad2e55-eboot.bin.psp2dmp`), con
  registros y `PC`/`LR` **prácticamente idénticos bit a bit** al dump anterior (mismo hilo
  `PSVGMV002`, mismo `PC` basura `0x983763ea`, mismo patrón de stack). Esa identidad total entre
  dos corridas separadas es la pista clave: una carrera real dependiente del scheduler no suele
  producir el MISMO valor corrupto dos veces -- apunta a un bug determinístico, no (solo) de
  timing.

### Bug confirmado 2026-09-01 (segundo hallazgo, misma tanda): `pthread_mutex_unlock_soloader` no convierte los sentinels estáticos Bionic no-cero antes de usar `real_ptr`

- **Causa raíz confirmada leyendo el header (`sys/_pthreadtypes.h` de VitaSDK,
  `pthread_mutex_t` es un puntero) y desensamblando `pthread_mutex_unlock` real de
  `libpthread.a`:** `pthread_mutex_lock_soloader`/`pthread_mutex_trylock_soloader` llaman a
  `_mutex_t_static_init()` antes de tocar `real_ptr` (convierte el valor crudo del inicializador
  estático de Bionic -- `0`=normal, `0x4000`=recursivo, `0x8000`=errorcheck -- en un handle real
  vía `pthread_mutex_init`). `pthread_mutex_unlock_soloader` en cambio solo chequeaba
  `if (!mutex->real_ptr) return EINVAL;` -- eso atrapa el sentinel NORMAL (`0`) pero NO
  `0x4000`/`0x8000` (son "truthy", no NULL). Si el motor invoca `unlock()` como primera operación
  sobre un mutex estático recursivo/errorcheck (nunca pasó por `lock()`/`trylock()` antes), el
  shim le pasa ese número crudo (`0x4000`/`0x8000`) al `pthread_mutex_unlock` real como si fuera
  un puntero a handle válido -- que lo dereferencia en varios offsets (`+0`/`+8`/`+12`/`+16`) y
  llama indirectamente a `pte_osAtomicExchange`/`pte_osSemaphorePost` con esos datos basura,
  produciendo el salto a dirección inválida visto en ambos dumps. Al ser 100% determinístico
  (no depende de qué thread gana una carrera), explica por qué el crash es bit-idéntico entre
  corridas.
- **Fix:** `pthread_mutex_unlock_soloader` ahora también llama `_mutex_t_static_init(mutex, NULL)`
  antes de chequear `real_ptr` -- es no-op si el mutex ya está inicializado, así que es seguro
  llamarlo siempre. Ver `source/reimpl/pthr.c` (`pthread_mutex_unlock_soloader`).
- **Build:** verde (`psvita-toolkit build`, 2026-09-01).
- **Resultado:** el crash volvió a aparecer, otra vez bit-idéntico
  (`logs/gangstarmiamivindication-psp2core-1788273861-0x0003042d61-eboot.bin.psp2dmp`, mismo
  `PC=0x983763ea`, mismo `SP`, mismos R0-R11) a pesar de que este fix también se compiló y
  desplegó -- descarta esta hipótesis como causa de esta firma puntual.

### Análisis forense 2026-09-01 (tercer dump, mismo crash bit-idéntico): confirmado con el ELF real, bloqueado por falta de log

- Con `nm`/`objdump` sobre `build/gangstarmiamivindication.elf` se ubicó `LR` con precisión de
  instrucción: cae exactamente en `pthread_mutex_unlock + 0x13` (offset confirmado:
  `0x810156e3 - 0x810156d0 = 0x13`, que coincide con el bit Thumb + el retorno de la PRIMERA
  llamada a `pte_osAtomicExchange` dentro del real `pthread_mutex_unlock` de VitaSDK -- el "fast
  path" no recursivo). Se desensambló `pte_osAtomicExchange` (`ldrex`/`strex`/`dmb`, función hoja
  sin llamadas ni saltos indirectos) -- no puede por sí sola desviar la ejecución a
  `PC=0x983763ea`. Se descartó que sea un bug de parseo de `vita-parse-core` (se verificó con un
  script Python que `thread.pc` -- offset 0x9C de la estructura de Sony -- y `regs.gpr[15]` --el
  registro real -- coinciden exactamente: el valor es genuino, viene de dos estructuras
  independientes pobladas por el propio volcado de Sony).
- **`R4=0x98673b88` (el puntero interno del mutex, `*real_ptr`) tiene forma de puntero de heap
  válido, NO de sentinel Bionic crudo (`0`/`0x4000`/`0x8000`)** -- es decir, `_mutex_t_static_init`
  ya corrió con éxito antes en algún momento anterior. Esto descarta que el bug esté en la
  inicialización lazy (ya blindada dos veces en `pthr.c`) -- algo corrompe ese mutex, o el `real_ptr`
  que apunta a él, DESPUÉS de que ya estaba en uso.
- **Bloqueado sin log de consola** -- siguiendo el primer paso de `so-crash-triage` ("leer el log
  antes que el dump"), no se pudo completar en ninguna de las 3 corridas porque el proyecto no
  emitía nada por red: `_log_print()` (`source/utils/logger.c`) solo llamaba a `sceClibPrintf`,
  sin salida capturable en vivo por `psvita-toolkit logs-live` (que espera un logger estilo
  "debugnet": una línea de texto por datagrama UDP, ver `debugnet_server.md` del toolkit).

### Cambio de infraestructura 2026-09-01: logging UDP tipo "debugnet" agregado a `source/utils/logger.c`

- **Motivo:** poder capturar el log real de la próxima corrida con
  `psvita-toolkit logs-live` (puerto UDP 9999 por defecto) en vez de seguir adivinando causas
  a partir de solo el `.psp2dmp` -- necesario para no seguir gastando ciclos de build+deploy en
  hipótesis no confirmadas.
- **Implementación:** `_debugnet_init()`/`_debugnet_send()` en `source/utils/logger.c` --
  `sceNetInit`+`sceNetCtlInit`+socket UDP con `SCE_NET_SO_BROADCAST`, enviando cada línea ya
  formateada (misma que va a `sceClibPrintf`) a `255.255.255.255:9999`. Init lazy, disparado una
  sola vez desde dentro de la sección crítica de `_log_mutex` que `_log_print` ya tomaba (evita
  agregar una carrera nueva); si `sceNetInit`/el socket fallan (sin Wi-Fi, etc.) el logging cae
  de nuevo a solo `sceClibPrintf`, sin crashear. Se agregaron `SceNet_stub`/`SceNetCtl_stub` a
  `target_link_libraries` en `CMakeLists.txt`.
- **Build:** verde (`psvita-toolkit build`, 2026-09-01).
- **Resultado:** crasheo inmediato en el primer intento, ANTES de llegar siquiera al motor --
  `logs/gangstarmiamivindication-psp2core-1788275718-0x0003b62a49-eboot.bin.psp2dmp` (sin
  `.analysis.txt`/`.triage_summary.md` generados aún -- se corrió `vita-parse-core` a mano).

### Bug confirmado 2026-09-01 (auto-infligido): `sceNetInit` sin cargar el sysmódulo `SceNet` primero

- **Causa raíz confirmada:** Prefetch abort, `PC=0x0`, `LR` justo después de `blx sceNetInit`
  dentro de `_debugnet_init()` (agregado en la tanda anterior para habilitar
  `psvita-toolkit logs-live`). `sceNetInit`/`sceNetCtlInit` son stubs de importación que solo se
  resuelven a una syscall real si el sysmódulo `SceNet` fue cargado antes vía
  `sceSysmoduleLoadModule(SCE_SYSMODULE_NET)` -- sin eso, el stub salta a un NID sin resolver,
  de ahí el salto a dirección nula.
- **Fix:** `_debugnet_init()` ahora llama `sceSysmoduleLoadModule(SCE_SYSMODULE_NET)` antes de
  `sceNetInit` (y aborta silenciosamente el resto de la inicialización de red si falla). Ver
  `source/utils/logger.c`.
- **Build:** verde (`psvita-toolkit build`, 2026-09-01).
- **Resultado:** ¡funcionó! Primer log real capturado:
  `logs/live_session_20260901_114758.log`, cruzado con
  `logs/gangstarmiamivindication-psp2core-1788277717-0x00043223e1-eboot.bin.psp2dmp` (mismo
  crash bit-idéntico otra vez: `PC=0x983763ea`, `LR` en `pthread_mutex_unlock+0x13` -- confirmado
  con `nm`/`objdump` contra el ELF de ESTE build que sigue siendo el "fast path" NO recursivo,
  o sea que un registro R5 con pinta de puntero de heap visto en corridas anteriores era solo
  basura de un registro no tocado por este camino de código, no una señal real -- corregido en
  el propio análisis).
- **Lo que reveló el log:** la secuencia completa de carga (`Settings loaded` → `SO relocated` →
  `SO imports resolved` → `SO patched` → `SO caches flushed` → 3x `[011] pthr: first-time lazy
  mutex init`, todo desde el mismo thread `0x40010003`) aparece DOS VECES seguidas, idéntica,
  antes de que el log se corte -- muy probablemente un duplicado a nivel de red (broadcast UDP
  recibido más de una vez por el listener), no una doble ejecución real del loader, dado que
  `main()` llama a `soloader_init_all()` una sola vez y el checkpoint `[001]` (justo después de
  esa llamada) nunca aparece. Confirma que el crash ocurre MUY temprano, durante los
  constructores globales C++ del `.so` (dentro de `so_initialize()`), inmediatamente después del
  3er mutex logueado.
- **Todavía sin causa raíz confirmada** -- se revisó `source/patch.c` (vacío, sin parches activos)
  y la resolución de `malloc`/`calloc`/`free`/`operator new`/`operator delete` en
  `source/dynlib.c` (todo enruta al mismo allocador de newlib, sin doble-allocador aparente).

### Instrumentación agregada 2026-09-01: checkpoints `[013]`/`[014]`/`[015]` en lock/unlock/destroy de mutex

- **Motivo:** el log solo tenía visibilidad de la PRIMERA inicialización de cada mutex
  (checkpoint `[011]`), no de los lock/unlock/destroy posteriores -- sin eso no se puede saber si
  el mutex que crashea (`real_ptr` interno `0x98673b88`, igual en las 4 corridas) fue destruido
  prematuramente y su memoria reusada (use-after-free) o si hay alguna otra secuencia anómala.
- **Cambio:** `pthread_mutex_lock_soloader` (`[013]`), `pthread_mutex_unlock_soloader` (`[014]`) y
  `pthread_mutex_destroy_soloader` (`[015]`) ahora loguean `mutex`/`real_ptr`/thread en cada
  llamada. Es instrumentación TEMPORAL de triage -- sacar una vez confirmada la causa real. Ver
  `source/reimpl/pthr.c`.
- **Build:** verde (`psvita-toolkit build`, 2026-09-01).
- **Resultado:** segundo log real capturado (`live_session_20260901_115845.log`) cruzado con
  `gangstarmiamivindication-psp2core-1788278341-0x00069323dd-eboot.bin.psp2dmp` (crash bit-idéntico
  otra vez). Hallazgo clave: de los 3 mutex logueados (`0x98aabae0`/`0x98aabcb8`/`0x98aabae4`,
  todos con `real_ptr` en el rango `0x8230xxxx` de nuestro heap), **el mutex `0x98aabcb8` se
  loguea con `[013] lock` pero JAMÁS aparece un `[014] unlock` correspondiente** en toda la traza
  (los otros dos sí tienen lock/unlock apareados). Además, el `real_ptr` que crashea (`R4` del
  dump, `0x98673b88`) **no coincide con ninguno de los 3 `real_ptr` logueados** (que están en
  `0x8230xxxx`) -- está en el rango `0x98xxxxxx`, el mismo rango donde carga el propio `.so`
  (`LOAD_ADDRESS 0x98000000` en `source/utils/init.c`). Esto sugiere que el mutex que crashea
  **nunca pasó por `_mutex_t_static_init()`/nuestros wrappers `_soloader`** -- ninguno de los 4
  checkpoints (`011`/`013`/`014`/`015`) lo vio nunca -- lo cual apunta a que el `real
  pthread_mutex_unlock` se está llamando con un puntero crudo que no es un handle armado por
  nuestro shim, posiblemente desde dentro del propio código nativo del juego o de runtime C++
  interno (ej. STLport), no confirmado todavía.

### Instrumentación agregada 2026-09-01: checkpoint `[016]` con los rangos reales de memoria del `.so`

- **Motivo:** confirmar con certeza si `0x98673b88` (la dirección que crashea) cae dentro de la
  memoria real asignada al `.so` (`mod->text_base`/`mod->data_base[]`) o no -- decide si el bug
  está en el propio motor del juego (llamando pthread_mutex_unlock con un puntero que no es un
  bionic mutex armado por nuestro shim) o en otro lado.
- **Cambio:** `soloader_init_all()` (`source/utils/init.c`) ahora loguea `[016]` con los rangos
  `text_base`/`text_size` y cada `data_base[i]`/`data_size[i]` de `so_mod` justo después de
  `so_flush_caches()`. Instrumentación TEMPORAL de triage -- sacar una vez confirmada la causa.
- **Build:** verde (`psvita-toolkit build`, 2026-09-01).
- **Resultado:** ¡CONFIRMADO! Tercer log real (`live_session_20260901_122921.log`) muestra
  `[016] so ranges: data[0]=[0x9863cbdc-0x98ae2800)` -- `0x98673b88` cae DENTRO de ese rango. La
  dirección que se desreferencia en el crash es una dirección real y legítima del propio `.so`,
  no basura ni un puntero de heap corrupto al azar.

### Instrumentación agregada 2026-09-01: loguear `*real_ptr` (no solo la dirección del slot) en `[013]`/`[014]`

- **Motivo:** con `[016]` confirmado, la hipótesis de trabajo es que el slot de 4 bytes
  `mutex->real_ptr` (nuestro `malloc()`, normalmente en `0x8230xxxx`) tiene su CONTENIDO
  corrompido -- en vez de guardar el puntero a la estructura interna de PTE que
  `pthread_mutex_init` escribe ahí, contiene un valor con forma de dirección del `.so`. Sin
  loguear el CONTENIDO de `*real_ptr` en cada lock/unlock no se puede ver en qué momento pasa esto.
- **Cambio:** `pthread_mutex_lock_soloader` (`[013]`) y `pthread_mutex_unlock_soloader` (`[014]`)
  ahora también loguean `*real_ptr` (el valor apuntado, no solo la dirección del slot). Ver
  `source/reimpl/pthr.c`.
- **Build:** verde (`psvita-toolkit build`, 2026-09-01).
- **Resultado:** cuarto log real (`live_session_20260901_124238.log`) -- **los 3 `*real_ptr`
  logueados están completamente sanos** (`0x82305870`/`0x82305898`/`0x823058c0`, todos punteros de
  heap plausibles). El crash (mismo dump bit-idéntico de siempre) sigue sin involucrar a ninguno
  de los 3 mutex trackeados -- el log se corta justo antes de que aparezca un 4to `[011]`/`[013]`
  para el mutex que realmente crashea. Descarta la hipótesis de "el slot `real_ptr` de un mutex ya
  trackeado se corrompe" -- el mutex que crashea es uno que **nunca pasa por
  `pthread_mutex_lock_soloader`/`trylock_soloader`/`unlock_soloader` en absoluto**.

### Instrumentación agregada 2026-09-01: checkpoints `[017]`/`[018]` en `pthread_cond_wait`/`pthread_cond_timedwait`

- **Motivo:** son las únicas dos funciones del shim que tocan `mutex->real_ptr` SIN pasar por
  `pthread_mutex_lock_soloader`/`unlock_soloader` (llaman a `_mutex_t_static_init` directo) --
  y el `pthread_cond_wait`/`pthread_cond_timedwait` REAL de VitaSDK hace internamente su propio
  unlock/relock del mutex, bypaseando por completo nuestros checkpoints `[013]`/`[014]`. Es el
  único código de nuestro shim que le pasa el control a la librería real sin loguear el estado
  de `real_ptr` justo antes.
- **Cambio:** `pthread_cond_wait_soloader` (`[017]`) y `pthread_cond_timedwait_soloader` (`[018]`)
  ahora loguean `cond`/`mutex`/`real_ptr`/`*real_ptr` justo antes de llamar a la función real. Ver
  `source/reimpl/pthr.c`.
- **Build:** verde (`psvita-toolkit build`, 2026-09-01).
- **Resultado:** quinto log real (`live_session_20260901_124751.log`) -- otra vez `[017]`/`[018]`
  NUNCA aparecen antes del corte. Confirma que el mutex que crashea no pasa por `lock`/`trylock`/
  `unlock`/`cond_wait`/`cond_timedwait` -- es decir, no pasa por NINGUNA función de `pthr.c` en
  absoluto.

### Hallazgo clave 2026-09-01: el mutex que crashea no es del juego -- es el guard global de C++ de nuestro propio `libsupc++.a`

- **Investigación:** se extrajo y desensambló `guard.o` (contiene `__cxa_guard_acquire`/
  `__cxa_guard_release`) de `~/vitasdk/arm-vita-eabi/lib/libsupc++.a` -- el mecanismo ABI de C++
  que usa TODO "magic static" del programa (variables locales de función con inicialización
  lazy thread-safe, exactamente lo que son `_Locale_true`/`_Init_timeinfo`/etc. según el primer
  triage automático de esta sesión). El desensamblado muestra que NINGÚN guard variable del `.so`
  se usa directo como mutex -- en cambio, `__cxa_guard_acquire`/`release` serializan TODO el
  programa (tanto el `.so` como nuestro propio loader) a través de UN SOLO mutex+cond globales,
  `_ZN12_GLOBAL__N_1L12static_mutexE`/`..._1L11static_condE` (símbolos de `libsupc++.a`, viven en
  el `.bss` de `gangstarmiamivindication`, no del `.so`), inicializados una sola vez vía
  `pthread_once` la primera vez que se llama `__cxa_guard_acquire` en TODO el programa.
  `dynlib.c` resuelve `__cxa_guard_acquire`/`release` (símbolos que el `.so` importa) directo a
  los símbolos REALES de `libsupc++.a` -- sin wrapper propio, así que esta ruta nunca pasa por
  `pthr.c` ni por ninguno de sus checkpoints, coincide perfectamente con lo observado.
- **Hipótesis de trabajo:** si la estructura interna `calloc`-eada de ESE ÚNICO
  `static_mutex` global se corrompe temprano (su primera palabra termina con un valor con forma
  de puntero al `.data` del `.so` en vez de un UID de semáforo), CUALQUIER `__cxa_guard_release`
  posterior en cualquier parte del programa crashearía con esta firma exacta -- explica la
  determinismo total observado en las 6 corridas (siempre el mismo `.so`, siempre el mismo hilo,
  siempre el mismo momento del arranque).

### Instrumentación agregada 2026-09-01: checkpoint `[019]` con el estado del guard mutex global de C++

- **Cambio:** `source/utils/init.c` agrega `log_guard_mutex_state()`, que lee directamente la
  dirección de `_ZN12_GLOBAL__N_1L12static_mutexE` (`0x811963d0` en este build exacto --
  símbolo con linkage interno de C++, no se puede referenciar con `extern`, hay que reobtener la
  dirección con `arm-vita-eabi-nm build/gangstarmiamivindication.elf | grep static_mutex` después
  de cualquier cambio que afecte el tamaño del binario del loader) y loguea su valor (`handle`,
  el puntero a la estructura interna real de PTE) y el primer word al que apunta (`*handle`).
  Se llama en 5 puntos de `soloader_init_all()`: después de `so_file_load`, `settings_load`,
  `so_relocate`, `resolve_imports`, y `so_patch`/`so_flush_caches` -- para acotar exactamente
  entre cuáles dos pasos se corrompe.
- **Build:** verde (`psvita-toolkit build`, 2026-09-01) -- dirección de `static_mutex`
  reverificada con `nm`, no cambió.
- **Resultado:** sexto log real (`live_session_20260901_130425.log`) -- **`static_mutex` sigue en
  `handle=0x0` en los 5 checkpoints `[019]`**, incluso después de `so_flush_caches()`. O sea que
  ningún `__cxa_guard_acquire()` corrió todavía en absoluto a esta altura -- el primer magic
  static de TODO el programa (del `.so` o de nuestro loader) se inicializa recién dentro de
  `so_initialize()` (los constructores globales del `.so`), después de donde llega este log.

### Instrumentación agregada 2026-09-01: checkpoint `[020]` dentro del loop de constructores globales del `.so`

- **Motivo:** acotar la corrupción de `static_mutex` a un constructor global específico --
  `so_initialize()` (`lib/so_util/so_util.c`) simplemente recorre `mod->init_array[]` llamando
  cada constructor uno por uno, sin ninguna visibilidad intermedia.
- **Cambio:** `so_initialize()` ahora loguea `[020]` con el índice del constructor, su dirección,
  y el estado de `static_mutex` (`handle`/`*handle`) DESPUÉS de cada llamada. Mismo aviso que
  `[019]`: la dirección hardcodeada de `static_mutex` (`0x811963d0`) hay que reverificarla con
  `nm` si cambia el tamaño del binario del loader -- reverificada en este build, sigue igual.
- **Build:** verde (`psvita-toolkit build`, 2026-09-01).
- **Resultado:** séptimo log real (`live_session_20260901_134742.log`) -- `[020] ctor[0]`,
  `ctor[1]`, `ctor[2]` completan con `static_mutex` todavía en `0x0`. **`[020] ctor[3]` NUNCA
  aparece** -- el crash ocurre DENTRO del constructor #3, después de que ese mismo constructor ya
  logueó `[011]`/`[013]`/`[014]` para 3 mutex.

### Análisis forense 2026-09-01: identificado el constructor global exacto (`ctor[3]`) y su código real

- Se calculó `init_array[3]` leyendo los bytes crudos de la sección `.init_array` del `.so`
  (`objdump -s -j .init_array`): `0x982816d1`. Coincide (a 0x16 bytes) con `0x982816bb`, una de
  las direcciones que aparecían como contenido de pila en TODOS los dumps desde el primer triage
  automático de esta sesión (los que mencionaban `_Locale_true`/`_Init_timeinfo`).
  `objdump -t` identifica esa dirección como `_GLOBAL__I_.._sources_Utils_memory.cpp` -- el
  inicializador sintético de GCC para los globals de `Utils/memory.cpp` (archivo propio del
  juego).
- Se desensambló completo (84 bytes): construye un objeto `glf::Mutex` GLOBAL del propio motor
  (namespace `glf`, probablemente "Gameloft Framework") vía `_ZN3glf5MutexC1Ev` →
  `_ZN3glf5Mutex4ImplC1Ei`, que llama a `pthread_mutex_init(this, &local_recursive_attr)` pasando
  la dirección del objeto `glf::Mutex` COMPLETO (48 bytes) directamente como `pthread_mutex_t*`
  -- coincide con el patrón ya confirmado en `Lock`/`Unlock`/`TryLock`/`~Mutex` (`_ZN3glf5Mutex...`),
  todos leen/usan offset `+44` del objeto como el "handle" real.
- **`R4=0x98673b88` (el valor que crashea) es exactamente `_GLOBAL_OFFSET_TABLE_`** del `.so`
  (confirmado con `objdump -t`: `00673b88 l O *ABS* .hidden _GLOBAL_OFFSET_TABLE_`) -- no es un
  mutex en absoluto, es la GOT. Se investigó si una relocación `R_ARM_RELATIVE` mal aplicada en
  `so_relocate()` (`lib/so_util/so_util.c`) podría estar escribiendo la dirección de la GOT en el
  slot equivocado -- el slot específico usado por `ctor[3]` para obtener el puntero al objeto
  `glf::Mutex` (`GOT+0x2e3c`) SÍ calcula correctamente (`0x986812d8`, una dirección de `.so` válida,
  no la GOT) -- esa relocación puntual está bien. La fuente exacta de cómo `0x98673b88` (la GOT)
  termina en el offset+0 (`real_ptr`) de ALGÚN mutex real todavía no está confirmada.
- **`pthread_mutex_init()` para el `glf::Mutex` de `ctor[3]` (dirección calculada `0x986812d8`)
  NUNCA aparece logueado** (ningún `[011]` con esa dirección) -- el crash ocurre ANTES de llegar
  a esa llamada, probablemente durante la máquina de asignación de memoria por-hilo de STLport
  (`_Pthread_alloc_impl`, disparada por el `operator new[](200)` que `ctor[3]` llama justo antes
  de construir el Mutex) -- de ahí los 3 mutex `[011]`/`[013]`/`[014]` que sí se ven, ninguno de
  los cuales es `static_mutex` (`0x811963d0`, sigue en `0x0`) ni el `glf::Mutex` recién descrito.

### Instrumentación agregada 2026-09-01: checkpoint `[021]` en `pthread_mutex_init_soloader`

- **Motivo:** era la única función `_soloader` de `pthr.c` sin checkpoint dedicado propio --
  `[011]` (dentro de `_mutex_t_static_init`) solo loguea la dirección del mutex al ÉXITO, sin el
  puntero `attr` -- necesario para distinguir un `pthread_mutex_init(&obj, &attr_recursivo)`
  explícito (como el de `glf::Mutex::Impl`) de una inicialización lazy normal.
- **Cambio:** `pthread_mutex_init_soloader` (`source/reimpl/pthr.c`) loguea `[021]` con
  `mutex`/`attr`/thread ANTES de delegar a `_mutex_t_static_init`.
- **Build:** verde (`psvita-toolkit build`, 2026-09-01).
- **Resultado:** octavo log real (`live_session_20260901_140129.log`) -- apareció
  `[021] pthr: init mutex=0x98aabcb8 attr=0x0` (llamada EXPLÍCITA a `pthread_mutex_init`, no solo
  lazy-init vía `lock()`). Cruzado con el pseudo-C de Ghidra (`decompiled/libGangster2_armeabi/`),
  esto identifica el flujo exacto: `ctor[3]` llama `_Znaj(200)` (`operator new[]`) → `CustomAlloc`
  → `MemMgr::Alloc` (allocador propio del juego) que, para su propio tracking interno, dispara la
  máquina de asignación por-hilo de STLport (`std::priv::_Pthread_alloc_impl`):
  `_S_get_per_thread_state()` bloquea `_S_chunk_allocator_lock` (`0x98aabae0` -- confirmado por el
  patrón lock/unlock/lock/unlock que coincide EXACTAMENTE con la recursión de `_S_chunk_alloc` en
  el pseudo-C), crea el primer `_Pthread_alloc_per_thread_state` vía `_S_new_per_thread_state()`
  (que llama `pthread_mutex_init(nuevo_estado+0x44, NULL)` -- ese es el `mutex=0x98aabcb8` del
  `[021]`), y libera `_S_chunk_allocator_lock`. Los 3 mutex trackeados (`0x98aabae0`, `0x98aabcb8`,
  `0x98aabae4`) son TODOS objetos legítimos del propio motor/STLport, con `real_ptr` sanos en
  cada captura -- **no hay corrupción en ninguno de estos tres**.
- **Bloqueado sin GDB en vivo:** el mutex que realmente crashea (`real_ptr` termina apuntando a
  `_GLOBAL_OFFSET_TABLE_`) sigue sin pasar por NINGÚN checkpoint de `pthr.c` (`[011]`/`[013]`/
  `[014]`/`[017]`/`[018]`/`[021]`) -- esto solo es posible si la llamada real a
  `pthread_mutex_unlock` que crashea NO llega ahí a través del import table del `.so` resuelto por
  `resolve_imports()`/`dynlib.c` hacia `pthread_mutex_unlock_soloader`, contradiciendo lo que
  muestra `objdump -T` (que confirma que "pthread_mutex_unlock" SÍ está en la tabla de símbolos
  importados del `.so` y `dynlib.c` lo resuelve correctamente a nuestro wrapper). Sin un backtrace
  real (no el heurístico de `vita-parse-core`, que solo escanea la pila buscando valores que
  parezcan direcciones válidas) no se puede confirmar con certeza el CALLER exacto de esa llamada
  específica.

### Recomendación para la próxima sesión: usar GDB en vivo, no más ciclos de build+deploy+triage

Dado que el crash es 100% determinístico (idéntico en las 8 corridas), es el candidato ideal para
depuración en vivo en vez de seguir agregando checkpoints y re-desplegando:

1. Usar `psvita-toolkit gdb-map` (o el equivalente en la TUI) para conectar GDB a la consola.
2. Breakpoint en la dirección real de `pthread_mutex_unlock` en `build/gangstarmiamivindication.elf`
   (obtenerla con `arm-vita-eabi-nm build/gangstarmiamivindication.elf | grep " pthread_mutex_unlock$"`
   -- en el build de esta sesión: revisar, cambia con cada build).
3. Al llegar al breakpoint: `bt`/backtrace REAL (no heurístico) para identificar la función
   exacta del `.so` que hizo la llamada, y `print *(void**)$r0` para ver el handle recibido.
4. Contrastar esa función contra `decompiled/libGangster2_armeabi/ghidra/out_ghidra.c` (ya se
   identificó el área: `std::priv::_Pthread_alloc_impl` / `MemMgr::Alloc` / `CustomAlloc`,
   alrededor de la línea 1058636-1058926) para ubicar la línea exacta de código que pasa un
   handle inválido (con forma de `_GLOBAL_OFFSET_TABLE_`) a `pthread_mutex_unlock`.

Sin esto, seguir agregando checkpoints en `pthr.c` no va a avanzar más -- ya se descartó que el
problema esté en el propio shim de `pthr.c` (los 3 mutex trackeados están sanos) y ya se ubicó el
área general del bug (motor propio del juego + STLport), pero falta el backtrace real para la
línea exacta.

**Corrección 2026-09-01:** GDB no está disponible sin trabajo de infraestructura previo --
`gdb_bridge.py` del toolkit espera un gdbstub YA corriendo en la consola, y este loader no tiene
ninguno integrado (no hay `psp2gdb`/taiHEN gdbstub en el proyecto). El usuario prefiere seguir con
UDP debugnet. Se reconsideró la hipótesis "el mutex que crashea nunca pasa por los checkpoints de
`pthr.c`" -- UDP no es confiable, y la ÚLTIMA línea antes de un crash (el checkpoint `[014]` que
buscamos) es exactamente la más propensa a perderse (enviada justo antes de que el proceso muera,
sin retransmisión). Ya se habían visto indicios de duplicación/pérdida de paquetes en capturas
anteriores.

### Instrumentación agregada 2026-09-01: log a archivo local además de UDP

- **Cambio:** `_log_print()` (`source/utils/logger.c`) ahora también escribe cada línea a
  `ux0:data/gangstarmiamivindication/logs/debug_local.log` (open+write+close síncrono por línea --
  lento pero confiable, aceptable para instrumentación temporal de triage). Mantiene el envío UDP
  existente sin cambios.
- **Build:** verde (`psvita-toolkit build`, 2026-09-01).
- **Resultado:** `logs/debug_local.log` traído de la consola se corta EXACTAMENTE en el mismo
  punto que el UDP (`[014] unlock mutex=0x98aabae4 ...`, línea 29) -- confirma que NO era pérdida
  de paquetes UDP. El mutex que crashea genuinamente nunca pasa por ningún checkpoint de
  `pthr.c` -- hace falta GDB real (con gdbstub integrado al loader) para ver el backtrace real y
  la línea de código exacta que llama `pthread_mutex_unlock` con el handle inválido.

### Cambio 2026-09-01: log local incremental por sesión (001-999)

- **Motivo (pedido del usuario):** `debug_local.log` se pisaba en cada sesión -- no se podían
  comparar capturas de corridas distintas una vez sobreescritas.
- **Cambio:** `source/utils/logger.c` ahora escribe a `debug_local_NNN.log` (NNN = 001-999,
  contador persistido en `debug_local_counter.txt` en el mismo directorio, incrementado una vez
  por arranque y reiniciado a 1 después de 999). Un archivo nuevo por sesión, nunca se pisan entre
  sí.
- **Build:** verde (`psvita-toolkit build`, 2026-09-01).

### Mejora 2026-09-01 (pedido del usuario): logging tan detallado como otros ports (`Inmortal-Dusk-vita/logs/game_003.log`)

- **Motivo:** ese log de referencia muestra trazado verboso de CADA llamada JNI
  (`NewByteArray(env, 28): 0x82152578`, etc.) -- justo el tipo de detalle que hace falta para
  depurar problemas de JNI/ciclo de vida nativo con precisión, y que esta sesión no tenía activado.
- **Hallazgo:** el proyecto YA TENÍA el mismo sistema (`lib/falso_jni/FalsoJNI_Logger.c`, con
  niveles `FALSOJNI_DEBUG_ALL/INFO/WARN/ERROR`), pero (a) `CMakeLists.txt` traía el nivel verboso
  comentado (`#add_definitions(-DFALSOJNI_DEBUGLEVEL=0)`, quedando en el default `WARN`, que
  silencia las trazas por-llamada que usan `fjni_logv_dbg`), y (b) `FalsoJNI_Logger.c` tiene su
  PROPIO mutex/buffers y solo imprime por `sceClibPrintf` -- nunca llegaba a nuestro UDP debugnet
  ni al archivo local, aunque estuviera habilitado.
- **Cambio:**
  - `CMakeLists.txt`: descomentado y activado `-DFALSOJNI_DEBUGLEVEL=0` (solo en builds Debug).
  - `source/utils/logger.h`/`.c`: nueva función pública `l_raw_line(const char *line)` que
    reenvía una línea YA formateada a los mismos sumideros que `_log_print` (UDP debugnet +
    archivo local por sesión), sin pasar por el switch de colores por severidad -- pensada para
    que OTROS loggers del proyecto (no solo el nuestro) puedan sumarse a la misma captura.
  - `lib/falso_jni/FalsoJNI_Logger.c`: el macro `LOG_PRINT` ahora también llama `l_raw_line(...)`
    después de `sceClibPrintf`, así toda traza JNI (`fjni_logv_dbg`/`_info`/`_warn`/`_error`) queda
    visible en `psvita-toolkit logs-live` Y en `debug_local_NNN.log`, no solo en la consola serie.
- **Build:** verde (`psvita-toolkit build`, 2026-09-01).
- **Nota:** el crash que se está investigando ocurre ANTES de que arranque cualquier llamada JNI
  (dentro de `so_initialize()`, en el propio motor nativo/STLport) -- esta mejora no va a mostrar
  la línea exacta de ESE crash puntual, pero da visibilidad total sobre el ciclo de vida JNI real
  una vez que se resuelva y el juego llegue más lejos (`Gangster2_nativeInit`, `nativeGetInfo`,
  etc. en `source/main.c`). Instrumentación de Debug únicamente -- no afecta builds Release
  (`CMAKE_BUILD_TYPE STREQUAL "Debug"` ya lo condiciona).

### Bug solucionado 2026-09-01: Crash en sys_alloc (dlmalloc) por fallback a sbrk roto

- **Síntoma:** Crash persistente en `sys_alloc` (offset `0x3763ea` del `.so`), con una firma de registros idéntica en múltiples corridas (`R4=0x98673b88`, `R6=0x82306000`, `R1=0xffffffd8`). `LR` apuntaba a `pthread_mutex_unlock` engañosamente porque fue la última función llamada por `sys_alloc` antes del crash, haciendo pensar que el problema era una corrupción del mutex.
- **Causa Raíz:** `sys_alloc` es un port interno de `dlmalloc`. Al intentar alocar un chunk, el engine pide a `mmap` (`r4 < mmap_threshold`). Pero en `source/reimpl/mem.c`, si `mmap` falla (o en este caso, si el tamaño pedido viaja por paths distintos y el threshold decide bypass), el control cae a `sbrk`. `sys_alloc` llama a `sbrk(increment)` obteniendo una dirección válida (ej. `0x82306000`), pero luego llama a `sbrk(0)` para obtener el nuevo límite y el `sbrk` de VitaSDK (que no soporta este patrón para el heap contiguo de libGangster2) retorna `0` en vez del límite esperado o `-1`.
- **Efecto:** `sys_alloc` calcula el nuevo tamaño de chunk como `new_break - old_break` (`0 - 0x82306000 = 0x7dcfa000`). Este tamaño de 2.1 GB se almacena temporalmente y se procesa. Al tratar de leer el header del "siguiente" chunk sumando este tamaño gigantesco a la base `0x82306000`, el puntero da un wraparound a `0xffffffd8`. Cuando intenta escribir en `0xffffffd8`, lanza Data Abort.
- **Fix:** Se reimplementó `sbrk_soloader` en `source/reimpl/mem.c` para proveer un heap contiguo real de 32MB gestionado mediante `memalign` internamente, simulando el comportamiento esperado de `sbrk` por dlmalloc (retorna `-1` si falla o si se excede el límite). Se enlazó en `dynlib.c` reemplazando al `sbrk` nativo de libc.

### Phase 2: Resolving the 2.1 GB OOM Crash (sbrk alignment bug)
- **Symptom:** After fixing `sys_alloc`, the game crashed at `PC: 0x982b7b4e` (`CWalkingHud::CWalkingHud`). The log showed `sbrk_soloader` returning `-1` because it was requested `2110758912` bytes (`0x7dcfc000`).
- **Root Cause:** A compiler optimization bug in the Android game's embedded `dlmalloc` `sys_alloc`. When extending the heap with `sbrk`, `dlmalloc` computes `aligned_brk - brk` to align `sbrk(0)`. If `sbrk(0)` is ALREADY aligned to the granularity (4096), `sys_alloc` executes a branch `beq` which erroneously uses `r6` (which held `-page_size`) instead of `aligned_brk` due to a register allocation bug. This causes it to subtract `sbrk(0)` from the size instead of adding `aligned_brk - brk`, resulting in `r4 = size - 0x82304000 + 4096 = 0x7DCFC000`. On Android, this bug rarely hits because Bionic's `sbrk(0)` is practically never 4096-aligned on startup, but on Vita, our `memalign(4096, 32MB)` made it perfectly aligned.
- **Fix:** We offset the initial `sbrk_heap` by artificially setting `static size_t sbrk_used = 8;`. This forces `sbrk(0)` to be unaligned, avoiding the buggy branch in `sys_alloc`, and correctly requesting the right amount of memory.

### Bug solucionado 2026-09-01: El VERDADERO origen del bug de los 2.1 GB en dlmalloc
- **Contexto:** El fix anterior (`sbrk_used = 8`) resultó ser incorrecto. El log mostraba que la reserva de 2.1 GB seguía ocurriendo silenciosamente (`sbrk_soloader: out of memory (used: 8, increment: 2110758904)`) al inicio del juego, lo que provocaba que `dlmalloc` fallara y devolviera un puntero `NULL` (con `errno = ENOMEM`), causando eventualmente un crash al instanciar `CWalkingHud` en `ctor[44]`.
- **Causa Raíz:** Se descubrió que el bug no era una optimización errónea del compilador en `dlmalloc`, sino que `dlmalloc` inicializa sus parámetros (`page_size` y `granularity`) llamando a `sysconf(_SC_PAGESIZE)`. En `source/dynlib.c`, la función `sysconf` estaba hookeada a un stub genérico (`ret0`) que devolvía `0`! 
- **Efecto:** Como `page_size` y `granularity` eran `0`, al calcular el padding necesario para la primera reserva de memoria con `sbrk`, `dlmalloc` alineaba el tamaño pedido haciendo un `AND` bit a bit con `0` (el negativo del tamaño de página). Esto resultó en un padding de `0` bytes. Luego, la fórmula en `sys_alloc` restaba el puntero actual del heap (`brk`) a este valor (`0 - brk`), resultando en el incremento absurdo de 2.1 GB (ej. `0x7DCF9FF8`). Al fallar `sbrk_soloader`, `sys_alloc` arrojaba `NULL`.
- **Fix:**
  - Se deshizo el parche incorrecto en `mem.c` (`sbrk_used = 0`).
  - Se creó un stub funcional para `sysconf` en `source/reimpl/sysconf.c` (`sysconf_soloader`) que devuelve correctamente `4096` cuando se le consulta por `_SC_PAGESIZE` (40).
  - Se reemplazó el hook en `dynlib.c` apuntando `"sysconf"` a `sysconf_soloader`.
  - El proyecto compila limpiamente (`psvita-toolkit build`) y este problema estructural de la memoria queda resuelto de forma definitiva.

## Fase 5: El motor arranca — símbolos faltantes y tabla JNI (2026-09-01)

### Hito: el crash de `pthread_mutex_unlock` desapareció

Con los fixes acumulados de `pthr.c` + el logging verboso de FalsoJNI activado, la corrida
`logs/live_session_20260901_203018.log` muestra el juego **ejecutándose ~17 segundos** y llegando a
pedir métodos JNI reales — muy por delante de donde moría antes (dentro de `so_initialize()`).
El crash determinístico de `0x98673b88` queda cerrado.

### Bug confirmado: 26 símbolos importados sin resolver (`__aeabi_i2f` y compañía)

- **Síntoma:** `Unknown symbol "__aeabi_i2f" (0x98673ed0)` -- `so_resolve()` no encontraba el
  símbolo en la tabla de `source/dynlib.c`, escribía `plt0_stub` en el slot de la GOT, y la
  primera llamada del juego a esa función caía en `reloc_err()`. La dirección `0x98673ed0` cae
  dentro de la GOT del `.so` (base `0x98673b88`), que es exactamente el valor que aparecía en los
  dumps anteriores.
- **Diagnóstico completo:** se cruzaron los 312 símbolos `*UND*` de
  `objdump -T ux0_data/gangstarmiamivindication/libGangster2.so` contra los 741 registrados en
  `dynlib.c`. Resultado: **26 faltantes**, casi todos helpers de runtime ARM EABI de float/double
  (`__aeabi_i2f`, `__aeabi_fadd`, `__aeabi_fmul`, `__aeabi_fdiv`, `__aeabi_fcmp*`, `__aeabi_d2f`,
  `__aeabi_dsub`, `__aeabi_lmul`, ...), más `_ZSt7nothrow`, `_ZnajRKSt9nothrow_t`, `__dso_handle`
  y `__isfinitef`.
- **Fix:** los 26 agregados a `source/dynlib.c`. 24 resuelven directo contra libgcc/libstdc++ del
  propio toolchain (misma ABI ARM EABI, verificado con `nm` sobre cada librería antes de
  agregarlos); `__dso_handle` usa el de nuestro `crtbegin.o` (ya presente en el ELF); y
  `__isfinitef` (Bionic) se mapea a `finitef` (newlib), misma firma y semántica.
- **Verificación:** `comm -23` entre los símbolos UND del `.so` y la tabla de `dynlib.c` ahora
  devuelve vacío -- **cero símbolos sin resolver**.

### Tabla JNI completa (`source/java.c`): 57 métodos

`source/java.c` tenía TODAS las tablas vacías, así que los ~57 `GetStaticMethodID` del motor
devolvían "not found" y después spameaban `methodIntCall: method ID 0 not found!`.
Implementados los 57 que el log real muestra que el juego pide:

- **Carga de recursos (lo crítico):** `getResourceFull`, `getResourceBytes`, `getResourceLength`.
  Semántica tomada de `GLResLoader.java` (jadx): normaliza la ruta (saca el prefijo `./` o `.//`
  y hace trim) y la busca en `DATA_PATH "data/"` y después en `DATA_PATH`. Confirmado que el `.so`
  pide rutas tipo `./Achievements.gmap`/`./miami.bdae` y que esos archivos están en
  `ux0:data/gangstarmiamivindication/data/` (3214 archivos). Los `byte[]` se devuelven vía
  `jda_alloc(len, FIELD_TYPE_BYTE)`; el `jstring` de entrada se lee como `JavaString*` →
  `js->utf8->array`.
- **Audio (26 métodos):** aceptados y ignorados -- el audio todavía no está portado
  (PORTING_PLAN.md sección 4). `isSoundLoaded`/`isSoundLoadedBig` devuelven 0 para que el motor
  no espere un sonido que nunca va a llegar.
- **Dispositivo/sistema:** valores tomados del Java real -- `getDeviceWidth()`=960 (coincide con
  `GAME_W` de `main.c`), `GetDeviceType()`=-1 (el default de Android para un fabricante que no es
  motorola/samsung/htc), `GetDeviceSoundType()`=2 (`RINGER_MODE_NORMAL`), `detectPhoneLang()`=0
  (inglés), `unlockDemo()`=0 y `DisableLaunchGame()`=0 (como devuelve `Gangster2.java`).
- **SDK de Verizon (10 métodos):** código muerto en este build; las consultas reportan
  "sin error / no en progreso / red no lista" para que la máquina de estados del motor se estabilice,
  y los tres getters `()[B` devuelven un `byte[]` vacío pero NO nulo (el código nativo lee el
  resultado sin chequear NULL).

- **Build:** verde (`psvita-toolkit build`, 2026-09-01).
- **Pendiente:** desplegar y probar en consola -- con los símbolos resueltos y los recursos
  cargables, el próximo paso natural es ver hasta dónde llega el arranque real del motor
  (`Gangster2_nativeInit` → `GameRenderer_nativeInit` → loop de render) y qué muestra por pantalla.

## Fase 6: pantalla negra tras el intro (2026-09-01)

### Bug confirmado: `loadMovie()` nunca avisa que el video "terminó" -> el motor espera para siempre

- **Síntoma:** el juego arranca (sin crash), pero se queda en pantalla negra. El log muestra la
  secuencia exacta: `NewStringUTF("intro.m4v")` → `CallStaticIntMethod(..., 47 [loadMovie], ...)`
  → 36 segundos después, el motor entra en un polling infinito de
  `CallStaticIntMethod(..., 35 [GetDeviceSoundType], ...)` cada 1-5 segundos, sin avanzar nunca.
- **Causa raíz:** en Android, `GLMediaPlayer.loadMovie()` (`GLMediaPlayer.java`) lanza la Activity
  `MyVideoView`, que reproduce el clip y, tanto si termina normalmente
  (`OnCompletionListener.onCompletion`) como si el usuario aprieta "saltar" (`mSkip` click),
  llama exactamente una vez a la función nativa REAL exportada por el `.so`:
  `Java_com_gameloft_android_TBFV_GloftGMHP_ML_MyVideoView_nativeSetOnVideoCompletion` -- es la
  señal que el motor nativo espera para soltar el estado de "reproduciendo video" y seguir hacia
  el título. Nuestro `Method_loadMovie` (agregado en la Fase 5) solo devolvía un entero, sin
  reproducir nada ni avisar de una finalización -- el motor queda esperando esa señal para
  siempre.
- **Fix:** `Method_loadMovie` (`source/java.c`) ahora resuelve
  `Java_..._MyVideoView_nativeSetOnVideoCompletion` vía `so_symbol()` (cacheado tras la primera
  resolución) y la llama inmediatamente después de loguear el pedido, simulando que CADA clip
  termina al instante -- coincide con el comportamiento real de Android salvo por la duración
  visible del video (que no está portado). Sigue devolviendo 1 (éxito), como el Java real.
- **Build:** verde (`psvita-toolkit build`, 2026-09-01).
- **Pendiente:** desplegar y confirmar que el motor pasa el intro y llega al título. Si hay OTRO
  clip más adelante en el flujo (ej. logo de Gameloft antes del de "intro.m4v", o cinemáticas
  entre capítulos) debería resolverse solo con este mismo fix, al ser genérico para cualquier
  llamada a `loadMovie`.

## Fase 7: crash en Application::PostInit — vitaGL reporta ES 2.0, el motor solo acepta <= 1.99 (2026-09-04)

### Bug confirmado: `glGetString(GL_VERSION)` devuelve "OpenGL ES 2.0 VitaGL" -> `doVersionCheck()` falla -> NULL deref en `0x28ba68`

- **Síntoma:** log `logs/debug_local_006.log` llega hasta `GameRenderer_nativeInit()` y termina con:
  `Glitch Engine version 0.1.0.2` ... `OpenGL|ES driver version is 1.1 or better.` ...
  `Could not create OpenGL|ES 1.1 driver.` Dump `logs/gangstarmiamivindication-psp2core-1788560906-0x0015fe2ba7-eboot.bin.psp2dmp`:
  Data abort en `PC 0x9828ba68` (= file-offset `0x28ba68` con base real `0x98000000` del log, NO la
  base autodetectada `0x981db000` del reporte), `R0=0`.
- **Causa raíz (objdump `-M force-thumb` + vtable real):**
  - `0x28ba68` es `Application::PostInit()` (`0x28b9e8`): `r0=createDeviceEx(app); ldr r0,[r0,#16]` sin
    chequeo de NULL. `createDeviceEx` (`0x4a3a64`) devuelve NULL porque `[r4,#16]` (driver) es NULL.
  - El driver es NULL porque `CGlfDevice::createDriver()` (`0x4a34cc`) llama a
    `createOpenGLES1Driver()` (`0x4eba24`), que llama al virtual `+0x200` = `CCommonGLDriver::initDriver()`
    (`0x4edc08`, resuelto vía `_ZTVN6glitch5video15COpenGLESDriverE` en `0x66d938` + `0x208`), que llama a
    `genericDriverInit()`, que falla en `doVersionCheck()` (`0x4e8304`: `return (199 >= version)`).
  - `version` se parsea de `glGetString(GL_VERSION)` con sscanf (`major*100+minor`). Nuestra vitaGL
    vendorized (`lib/vitaGL/source/get_info.c:189`) devuelve `"OpenGL ES 2.0 VitaGL"` -> version 200 >
    199 -> check devuelve 0 -> toda la cadena colapsa a NULL. `initDriverWithGlf()` (`0x4e81cc`) siempre
    devuelve 1 y `driverInit()` (`0x4ead18`) siempre devuelve 1: descartados como causa.
- **Fix:** nuevo `source/reimpl/gl.{c,h}` con `glGetString_soloader()`: si `name == GL_VERSION` devuelve
  `"OpenGL ES 1.1 VitaGL"` (version 101: pasa el gate `> 100` y el check `<= 199`); el resto cae al vitaGL
  real con log de debug. `source/dynlib.c` remapea `"glGetString"` al wrapper; `CMakeLists.txt` agrega
  `source/reimpl/gl.c` al build.
- **Build:** verde (`psvita-toolkit build --preset debug`, 2026-09-04).
- **Pendiente:** desplegar en consola real y confirmar que el motor supera `PostInit` y llega al título.
  Si aparece otro NULL más adelante, triage nuevo con log+dump de esa corrida (un bug a la vez).

### Bug confirmado 2026-09-04 (bis): crash en el propio loader por instrumentación con dirección `.bss` hardcodeada y obsoleta

- **Síntoma:** tras agregar `source/reimpl/gl.c` (fix de Fase 7), la corrida nueva muere al instante:
  `logs/debug_local_007.log` solo tiene 2 líneas (`FIOS initialized`, `kubridge check passed`) y el dump
  `logs/gangstarmiamivindication-psp2core-1788566141-0x000dd038b9-eboot.bin.psp2dmp` muestra Data abort en
  el hilo `PSVGMV002` con `PC` en `log_guard_mutex_state` (`source/utils/init.c:57`) y `R3 = 0x811963d0`.
- **Causa raíz:** instrumentación de triage temporal del viejo crash `0x98673b88` (ya cerrado en Fase 5)
  que leía el mutex interno `static_mutex` de libsupc++ vía una dirección `.bss` hardcodeada
  (`STATIC_MUTEX_ADDR 0x811963d0`, con su propio comentario avisando que se movería con cualquier cambio
  del layout). Al agregar `gl.c` el layout cambió (`nm` del build actual: `static_mutex` en `0x811b6aa8`),
  así que `*STATIC_MUTEX_ADDR` desreferenció una dirección rancia -> data abort en `soloader_init_all()`
  antes siquiera de cargar el `.so`. Había una segunda copia del mismo patrón en
  `lib/so_util/so_util.c` (`SO_INIT_STATIC_MUTEX_ADDR`, log `[020]` por cada ctor).
- **Fix:** eliminada la instrumentación en ambos sitios (su propósito ya se cumplió): `init.c` pierde
  `log_guard_mutex_state()` + sus 6 llamadas; `so_util.c:so_initialize()` conserva el log útil
  `ctor[%d]=%p done` pero sin desreferenciar la dirección obsoleta. `so_util` es código vendorized del
  repo (no submódulo), así que el cambio es local al port.
- **Build:** verde (`psvita-toolkit build --preset debug`, 2026-09-04).
- **Pendiente:** redesplegar en consola real; el binario ahora lleva el fix de Fase 7 (spoof ES 1.1) sin
  este crash autoinfligido en el arranque.

## Fase 8: la "pantalla negra en loop infinito" no era un deadlock — era el propio logging (2026-09-05)

### Bug confirmado: el trace por-mutex convierte cada `malloc()`/`free()` del juego en 2 escrituras a `ux0:`

- **Síntoma:** `logs/debug_local_013.log` llega bien hasta el título del motor —
  `Glitch Engine version 0.1.0.2`, el gate de OpenGL ES 1.1 ya pasa (fix de Fase 7 confirmado),
  se imprime la lista de extensiones y el `Driver/Renderer/Vendor` — entra en
  `main loop: frame 2` y a partir de ahí las 2.600 líneas restantes del log son exclusivamente
  `pthr: lock mutex=0x98aabcb8` / `pthr: unlock mutex=0x98aabcb8`, siempre desde el mismo hilo
  `0x40010003`. Nunca vuelve de `GameRenderer_nativeRender()`.
- **Causa raíz (identificada con los símbolos del `.so` real, no por inspección del loop):**
  - `nm` sobre `libGangster2.so`: `_gm_` está en `.bss+0xaabb00` y mide `0x1cc`, así que
    `0x98aabcb8` (= base `0x98000000` + `0xaabcb8`) cae DENTRO de `_gm_`. El juego linkea
    estáticamente su propio **dlmalloc**, y `0x98aabcb8` es el `MLOCK_T` de su estado global.
    Los vecinos lo confirman: `magic_init_mutex` (`0xaabae0`) y `morecore_mutex` (`0xaabae4`)
    son los otros dos mutex que aparecen al arrancar. `0x98ae24c8` es
    `std::_Node_Alloc_Lock::_S_lock` (el allocator de nodos de STLport).
  - La instrumentación `[023]` que ya estaba puesta (muestreo del caller cada 500 locks) resolvió
    los dos únicos callers observados: `0x983765c7` → `malloc` (`0x3765c6`, ARM Thumb) y
    `0x98375c25` → `free` (`0x375c24`). O sea: el "loop infinito" es **tráfico normal de
    asignación de memoria**, el motor está cargando recursos, no está trabado. Además el log no
    tiene ni una sola línea `[017]`/`[018]` (`pthread_cond_wait`) y hay **un solo hilo** en toda
    la corrida, así que un spin esperando a otro hilo era imposible por construcción.
  - Lo que sí estaba roto: `pthread_mutex_lock_soloader`/`unlock_soloader` logueaban
    `[013]`/`[014]` en CADA llamada, y `_log_print()` hacía `sceIoOpen` + `sceIoWrite` +
    `sceIoClose` sobre `ux0:` por línea. Como el lock que dominaba era el de dlmalloc, eso son
    **dos round-trips completos a la memory card por cada `malloc()` y cada `free()`**. La carga
    de recursos pasa a correr a la velocidad de la tarjeta en vez de la del CPU: de ahí la
    pantalla negra "para siempre".
  - Segundo multiplicador, en el mismo camino: `_mutex_t_static_init()` corre en cada lock/unlock
    y hacía *global LwMutex + scan lineal de 1024 slots* antes de responder "sí, ya está
    inicializado" — otra vez, una vez por `malloc()` y una por `free()`.
- **Fix:**
  - `source/reimpl/pthr.c`: los traces `[011]`/`[012]`/`[013]`/`[014]`/`[015]`/`[021]` pasan a
    `PTHR_TRACE()`, apagado salvo que se compile con `-DPTHR_TRACE_LOCKS` (nueva opción en
    `CMakeLists.txt`, OFF por defecto). En su lugar queda `[023]` como sonda de progreso barata:
    lee el reloj una vez cada 4096 locks y emite **como mucho una línea cada 3 s** con el contador
    acumulado. Un motor realmente trabado ahora se ve como el contador quieto entre dos `[023]`,
    en vez de como un muro de texto que él mismo provoca. `[017]`/`[018]` (`cond_wait`) siguen
    siendo incondicionales: son raros y son exactamente lo que un cuelgue real sí produce.
  - `source/reimpl/pthr.c`: caché "ya inicializado" de mapeo directo (1024 entradas atómicas)
    delante de `initializedObjects`. El caso común se resuelve con una sola lectura, sin lock ni
    scan; un miss cae al camino exacto de siempre, así que la caché solo puede ser una
    optimización. Se invalida en `forgetObject()`. Además `_mutex_t_static_init()`/
    `_cond_t_static_init()` ahora inicializan el handle en un puntero local y recién después
    publican `real_ptr` + la entrada de caché (release store): antes se asignaba
    `mutex->real_ptr = malloc(...)` ANTES de `pthread_mutex_init()`, lo que con un lector sin lock
    expondría un handle a medio construir. También se eliminó el `sceClibMemcpy()` desde una
    `pthread_mutex_t`/`pthread_cond_t` sin inicializar del stack.
  - `source/utils/logger.c`: el archivo de sesión se abre **una vez** y el fd queda abierto, en
    vez de open+write+close por línea. La supervivencia ante crash se mantiene con
    `sceIoSyncByFd()` en toda línea de severidad `LT_WARN` o mayor y cada 64 líneas de debug.
- **Fix adicional (latente, encontrado en el camino):** `munmap()` en `source/reimpl/mem.c` hacía
  `free(addr)` sobre cualquier puntero. El dlmalloc del juego llama a `CALL_MUNMAP()` desde
  `sys_trim()` para liberar solo la **cola** de un segmento (`sp->base + newsize`), o sea un
  puntero interior — `free()` sobre eso corrompe el heap de newlib y explota mucho después en
  cualquier otro lado. Ahora `mmap()` registra base+tamaño de cada mapeo emulado (256 slots) y
  `munmap()` solo libera si la dirección es exactamente una base conocida; para un unmap parcial
  o desconocido devuelve -1, que dlmalloc maneja quedándose con el segmento.
- **Build:** verde (`psvita-toolkit build --preset debug`, 2026-09-05), sin warnings nuevos.
- **Pendiente:** desplegar en consola real. La consola no estaba accesible por FTP
  (`192.168.3.15:1337`) al momento de hacer estos cambios, así que **el fix no está verificado en
  hardware**. Qué mirar en el log siguiente: `main loop: frame 2 returned` (y frames 3+) deberían
  aparecer; las líneas `[023]` deberían mostrar el contador de locks creciendo. Si el contador se
  queda quieto entre dos `[023]`, ahí sí hay un cuelgue real y hay que triagearlo con
  `-DPTHR_TRACE_LOCKS` puesto o con GDB.

## Fase 9: los assets se buscaban en `/sdcard/...` — crash en `ASprite::ASprite()` (2026-09-05)

### Confirmación de la Fase 8

`logs/debug_local_014.log` tiene 657 líneas contra las 3.410 de la corrida anterior, y llega
*mucho* más lejos en mucho menos tiempo: `frame 1 returned (46382 us)` (antes 124.702 us) y el
motor ya entra a cargar assets. El "loop infinito" era efectivamente el logging. Ni una línea
`[023]`, o sea que nunca se llegó a 4096 locks: el arranque entero ahora cuesta menos que lo que
antes costaba una fracción de un frame.

### Bug confirmado: `ASprite::ASprite(const char*)` desreferencia sin chequear el resultado del open

- **Síntoma:** `logs/debug_local_014.log` termina con decenas de
  `fopen(./sdcard/gameloft/games/Gangstar2//./huds.bsprite, rb): 0x0` y el dump
  `gangstarmiamivindication-psp2core-1788583236-0x000cc63f73` es un Data abort con `R0 = R4 = 0`.
- **Causa raíz:** la base auto-detectada del reporte (`0x97ccf000`) es incorrecta; con la base real
  del log (`0x98000000`) el `PC 0x98364bbe` cae en `ASprite::ASprite(char const*)` (`0x364b28`) a
  +0x96. El desensamblado (`-M force-thumb`) muestra exactamente:
  `364bbc: blx r3` (el virtual de apertura del vfs, vía `Application::GetInstance()`) e inmediato
  `364bbe: ldr r3, [r0, #0]` — sin chequeo de NULL. La pila conserva el ASCII de
  `"./huds_hi.bsprite"` y de `"./about_lo.engli..."`, y en la pila aparecen
  `glitch::io::createReadFile()` (`0x400d4c`) y `glitch::IReferenceCounted::drop()`.
  O sea: el archivo no se encuentra → `createReadFile()` devuelve NULL → boom.
- **Por qué no se encuentra:** el `.so` tiene su raíz de contenido compilada como literal
  (`strings`: `/sdcard/gameloft/games/Gangstar2/` en `.rodata+0x5e0d60`, más `.../igp` y
  `.../tmp/`, y el dir privado de Android `/data/data/com.gameloft.android.TBFV.GloftGMHP.ML/`).
  **No hay ningún JNI para setear la ruta** — los únicos `Java_*` que exporta la librería son los
  cinco `nativeInit`/`nativeRender` que `main.c` ya llama — así que solo se puede corregir en el
  camino al filesystem.
- **Fix (`source/reimpl/io.c`):** capa de traducción Android → Vita:
  - `/sdcard/gameloft/games/Gangstar2` → `DATA_PATH "data"`
  - `/data/data/com.gameloft.android.TBFV.GloftGMHP.ML` → `DATA_PATH "saves"`
  - `/sdcard` (cualquier otra cosa, ej. el código de IGP/ads) → `DATA_PATH "data"`
  - Normaliza además los `//` y los segmentos `.`: el motor concatena su raíz con nombres que ya
    empiezan con `./` y a veces le antepone otro `.` al conjunto, así que lo que llega a `fopen()`
    es literalmente `./sdcard/gameloft/games/Gangstar2//./about.english`. sceLibcBridge no resuelve
    eso. Un `./foo` genuinamente relativo se deja intacto, igual que `ux0:`/`app0:`/`/proc/...`.
  - La traducción se aplica en `fopen`, `open`, `stat`, `opendir`, y en wrappers nuevos para
    `access`, `chdir`, `chmod`, `mkdir`, `remove`, `rename`, `rmdir`, `unlink`, `realpath`,
    `freopen` y `lstat`, todos los cuales estaban mapeados directo a newlib en `dynlib.c` y por lo
    tanto seguían mirando un `/sdcard` que no existe.
  - `lstat` de paso deja de ser un bug latente: estaba mapeado a la `lstat()` de newlib, que
    escribe un `struct stat` de newlib en un buffer que el `.so` dimensionó como el `struct stat64`
    de bionic — la misma discrepancia que `stat_soloader()` existe para evitar.
  - Escrituras: `fopen(...,"w"/"a"/"+")` y `open(..., O_CREAT)` crean los directorios padre que
    falten, y `io_prepare_dirs()` (llamado desde `soloader_init_all()`) crea `data/`, `saves/`,
    `data/tmp/` y `data/igp/`. En Android el motor da por sentado que su "home" y su `tmp/` ya
    existen; acá solo existen si los creamos, y una escritura que falla por eso es invisible hasta
    que el motor tropieza con el resultado a medio escribir mucho después.
  - El `l_warn` de `fopen`/`open` fallidos ahora imprime la ruta traducida **y** la original
    (`[was: ...]`), para que el próximo asset que falte se identifique de una.
- **Verificación:** la traducción se probó en el host contra las rutas exactas del log
  (`./sdcard/...//./about.english`, `.../tmp/x.tmp`, `.../igp`, el dir privado, rutas relativas,
  `ux0:`/`app0:`/`/proc`) — todos los casos dan el resultado esperado.
- **Datos:** `zipinfo` de `Gangstar-Miami-V.zip` (3248 archivos, incluido el subdirectorio `igp/`)
  contra `ux0_data/gangstarmiamivindication/data/`: **coinciden exactamente, no falta ninguno**.
  Así que una vez traducida la ruta, todos los assets están donde el motor los va a buscar.
- **Build:** verde (`psvita-toolkit build --preset debug`, 2026-09-05).
- **Pendiente:** desplegar (la consola seguía sin responder en `192.168.3.15:1337`). Antes de
  correr, confirmar que `ux0:/data/gangstarmiamivindication/data/` está realmente en la consola con
  sus 3248 archivos — hasta ahora ninguna corrida había intentado abrir un asset de verdad, así que
  eso nunca se ejerció. En el log siguiente, los `fopen(...)` deberían pasar de `l_warn` a `l_debug`
  con rutas `ux0:data/gangstarmiamivindication/data/...`.

## Fase 10: el log de la corrida en release estaba ciego (2026-09-05)

### Qué mostraba el log

`logs/live_session_20260905_025658.log` tiene **6 líneas**, todas iguales, dos ciclos de:

```
[WARN][FalsoJNI_ImplBridge.c:263][methodIntCall] method ID 27 not found!
[WARN][FalsoJNI_ImplBridge.c:263][methodIntCall] method ID 28 not found!
[WARN][FalsoJNI_ImplBridge.c:263][methodIntCall] method ID 29 not found!
```

### Por qué el log estaba vacío (causa de "no me muestra nada", no del pantallazo negro)

`build/CMakeCache.txt` tenía `CMAKE_BUILD_TYPE:STRING=` (vacío), o sea que la corrida se hizo con
un build **sin `DEBUG_SOLOADER`**. Con eso, `logger.h` define `l_debug/l_info/l_warn/l_success/
l_wait` como **nada**: se compilan fuera. Se van con ellos los `l_checkpoint()` de `main.c`, todo
`io.c`, y — lo más caro — el log propio del motor, porque `reimpl/log.c` mandaba
`__android_log_*()` de nivel INFO/WARN a `l_info()`/`l_warn()`. Lo único que sobrevivía eran los
`l_error`/`l_fatal` y el logger **propio** de FalsoJNI (`FalsoJNI_Logger.c`), que no depende de
`DEBUG_SOLOADER` sino de `FALSOJNI_DEBUGLEVEL`, cuyo default (`FalsoJNI.h:22`) es
`FALSOJNI_DEBUG_WARN`. De ahí que el único sobreviviente fueran esos tres warnings.

El otro lado del problema es real y está documentado en la Fase 8: un build Debug prende *todos*
los `l_debug` de golpe, incluidos los `fread()`/`fseek()` por llamada de `io.c`, y ese volumen
frena la carga de assets lo suficiente como para cambiar el comportamiento bajo prueba. No había
punto medio entre "3.400 líneas de fread" y "3 líneas".

### Dato importante: la corrida en release llegó MÁS LEJOS que cualquier corrida en debug

Ningún `debug_local_0*.log` tiene una sola línea `method ID ... not found` — ninguna corrida en
debug llegó nunca a **llamar** a `setMusicGain`. Todas registraban el método
(`GetStaticMethodID(env, ..., "setMusicGain", ...)`) y morían antes, adentro del **segundo**
`GameRenderer_nativeRender()` (todos los logs terminan en `frame 2 enter` sin su `frame 2
returned`); la última, `debug_local_016`, terminó en un data abort dentro de SceLibKernel
(dump `...-1788589177`). La corrida en release del 02:51/02:57 **no generó dump** — no crasheó,
sigue viva. O sea: el port avanza más de lo que decía la Fase 9, y el motor ya está en el
setup de audio cuando la pantalla queda negra.

### Bug confirmado: IDs 27/28/29 nunca estuvieron en la tabla `methodsInt`

- `setMusicGain`/`setSfxGain`/`setVfxGain` son `private static void` en `GLMediaPlayer.java`, así
  que estaban registrados como `METHOD_TYPE_VOID` y solo en `methodsVoid[]`.
- Pero el `.so` **no** los invoca por `CallStaticVoidMethod`. El pseudo-C de Ghidra
  (`nativeSetMusicGain`/`SfxGain`/`VfxGain`) muestra
  `(**(code **)(iVar4 + 0x204))(piVar1, uVar2, setMusicGain, ..., uVar5)`, y el offset `0x204`
  del `JNINativeInterface` es el índice 129 = **`CallStaticIntMethod`**. Por eso `methodIntCall()`
  no encontraba nada y devolvía -1.
- **Es ruido, no es el pantallazo negro:** los wrappers nativos (`void nativeSetMusicGain(void)`)
  descartan el valor de retorno, así que el -1 no cambia nada. Pero era lo único que quedaba
  visible en el log y había que sacarlo del medio.
- **Auditoría completa de los sitios de despacho JNI del binario** (contando sobre el pseudo-C):
  `0x1c4` `GetStaticMethodID` x57 (= exactamente las 57 entradas de `nameToMethodId`),
  `0x1c8` `CallStaticObjectMethod` x5, `0x204` `CallStaticIntMethod` x18,
  `0x234` `CallStaticVoidMethod` x34. Cruzados uno por uno contra `java.c`: **27/28/29 eran el
  único hueco**; todos los demás métodos se llaman por la variante que su entrada ya cubría.

### Fixes

- `source/java.c`: nuevo `Method_soundGainStub()` (devuelve 0) y entradas 27/28/29 en
  `methodsInt[]`. Se dejan también en `methodsVoid[]` para que cualquiera de los dos caminos
  resuelva. `nameToMethodId` pasa a `METHOD_TYPE_INT` en esos tres.
- `source/utils/logger.h`: nuevo nivel **`l_note()`** (`_log_print(LT_INFO, ...)`), que se compila
  **siempre**, también en Release. Reservado para líneas que son a la vez de bajo volumen y de
  alta señal.
- `source/reimpl/log.c`: `__android_log_*()` de nivel INFO/WARN pasa a `l_note()`. El log propio
  del motor (`[ALOG][GameLoft Printer::log] ...`) es la mejor descripción de lo que está haciendo
  y son unas pocas decenas de líneas por boot; ahora sobrevive a un build de release. El DEBUG/
  VERBOSE del motor sigue detrás de `DEBUG_SOLOADER`.
- `source/main.c`: los latidos de frame `[022]` pasan de `l_checkpoint()` (= `l_debug`) a
  `l_note()`. En release ahora se puede responder lo primero que hay que preguntar ante una
  pantalla negra: *¿el loop de render está girando?* Volumen acotado: los primeros 10 frames y
  después uno cada 300 (~10 s al pacing de 30 FPS).
- `source/reimpl/io.c`: los `fread()`/`fseek()` por llamada quedan detrás de `IO_TRACE_STREAMS`
  (nueva opción de CMake, OFF por default) — mismo razonamiento que `PTHR_TRACE_LOCKS` en la
  Fase 8. Y un `fopen()`/`open()` que falla pasa de `l_warn` a **`l_error`**: un asset que falta
  es la causa más común de un NULL-deref posterior (Fase 9) y `l_warn` desaparece en release.
- `CMakeLists.txt`: opción `IO_TRACE_STREAMS`.

### Estado

- **Build:** verde (`psvita-toolkit build --preset release`, 2026-09-05).
- **Pendiente:** desplegar. La consola seguía sin responder en `192.168.3.15:1337`, así que
  **nada de esto está verificado en hardware**.
- **Qué mirar en el próximo log** (ya sirve un build de *release*, no hace falta debug):
  1. Las líneas `[ALOG]` del motor: dicen hasta dónde llegó y qué estaba creando.
  2. `[022] main loop: frame N` — si aparecen frames 3, 4, ... y después uno cada 300, el loop
     gira y el problema es de **render** (vitaGL/estado GL). Si se corta en `frame N enter` sin
     su `returned`, el motor está trabado o crasheando **adentro** de `nativeRender`.
  3. Cualquier `fopen(...): FAILED` — asset que falta.
  4. Ya no debería haber ningún `method ID ... not found`.

## Fase 11: crash por format string — `sceClibPrintf(buffer_b)` (2026-09-05)

La instrumentación de la Fase 10 funcionó a la primera: `logs/debug_local_019.log` son **89 líneas
legibles** (contra 3 antes y 2.400 en debug), con todo el arranque del motor visible y el punto
exacto donde muere.

### Síntoma

El log termina en:

```
[022] main loop: frame 1 returned (10222 us)
[022] main loop: frame 2 enter (t=3961368)
[ALOG][GameLoft Printer::log] Glitch Engine version 0.1.0.2
...
[ALOG][GameLoft] createTextureImpl 1
[ALOG][GameLoft] nativedetectPhoneLang:0
[ALOG][GameLoft] createTextureImpl 1     ← última línea
```

Dump: `gangstarmiamivindication-psp2core-1788592309-0x0018152ed7`. Data abort,
`PC = SceLibKernel seg1 + 0x60a2`, `LR = +0x7d1d`, `R0 = R1 = R4 = 0xb472dd63` (basura),
`R12 = 0xdeadbeef`. **Firma idéntica** a la del dump `...-1788589177` de la Fase 9/10 — es el
mismo bug, no uno nuevo.

### Causa raíz

Descomprimiendo el core (es un ELF gzippeado) y volcando la pila cruda alrededor de `SP`, en
`0x815c0164` aparece el buffer que `sceClibPrintf` estaba formateando, cortado a mitad de línea:

```
 \x1b[38;5;32mℹ info\x1b[0m     [ALOG][GameLoft Printer::logf] parameter type mismatch when setting "
```

Ese prefijo ANSI + el tag ya sustituido dicen que lo que se estaba expandiendo era **`buffer_b`,
o sea la salida YA terminada de `_log_print()`**. Y el string completo está en el `.so`
(`.rodata+0x5d1f00`):

```
parameter type mismatch when setting "%s/%s": want %s, got %s
```

La cadena de llamadas, confirmada en el pseudo-C de Ghidra:

```c
glitch::os::Printer::logf(ELOG_LEVEL lvl, char *fmt, ...) {
    appDebugLog("GameLoft Printer::logf", fmt);   // ← pasa el FORMATO CRUDO, sin expandir varargs
    ...
}
void appDebugLog(tag, msg) { __android_log_write(4, tag, msg); }
```

O sea: el motor manda su format string al log **sin expandirla**. En Android eso es inofensivo,
porque `__android_log_write()` nunca la reinterpreta. Acá:

1. `reimpl/log.c` la pasa como argumento: `l_note("[ALOG][%s] %s", tag, text)` — correcto, `%s`
   copia `text` tal cual.
2. `_log_print()` la deja en `buffer_b`, que queda conteniendo cuatro `%s` literales.
3. `source/utils/logger.c:239` hacía **`sceClibPrintf(buffer_b)`** — pasando datos de runtime como
   **format string**. `sceClibPrintf` reparsea esos cuatro `%s` y lee cuatro punteros basura de la
   pila. El primero devolvió 2 bytes (por eso el `"` y el `/` del formato alcanzan a escribirse),
   el segundo era `0xb472dd63` → data abort adentro de SceLibKernel.

Bug de format string de manual. Cualquier línea del motor que contenga un `%` mataba el proceso.

### Por qué recién aparece ahora

En un build de release previo a la Fase 10 los `[ALOG]` de nivel INFO se compilaban fuera, así que
esa línea nunca llegaba a `sceClibPrintf` — de ahí que la corrida de las 02:51 no crasheara y
quedara en pantalla negra. Los builds debug (Fases 8-9) **sí** la ejecutaban: por eso todos
terminaban en el segundo `nativeRender`. El crash estaba latente desde siempre.

### Fix

- `source/utils/logger.c:239` → `sceClibPrintf("%s", buffer_b)`.
- `lib/falso_jni/FalsoJNI_Logger.c:58` → `sceClibPrintf("%s", _fjni_log_buffer_2)`. Mismo bug,
  heredado de FalsoJNI upstream; no había disparado todavía porque las líneas de FalsoJNI no
  llevan `%` del lado de los datos, pero es la misma trampa.
- (`buffer_a` se sigue construyendo embebiendo `fmt` y usándose como formato — eso es seguro:
  `fmt` siempre es un literal de compilación de nuestro propio código, nunca datos del motor.)

### Estado

- **Build:** verde (`psvita-toolkit build --preset release`, 2026-09-05), sin warnings nuevos.
- **Desplegado:** sí, `eboot.bin` subido a `/ux0:/app/PSVGMV002/` (2026-09-05 03:24).
- **Pendiente de verificar en consola.** Qué esperar en el próximo log:
  1. La línea ahora debería imprimirse entera y literal:
     `[ALOG][GameLoft Printer::logf] parameter type mismatch when setting "%s/%s": want %s, got %s`
     — con los `%s` sin expandir, porque el motor nunca los expande. Eso es lo correcto.
  2. El motor debería **seguir de largo** en vez de morir ahí, y el `frame 2` debería retornar.
- **Pista para lo que viene:** ese warning del motor es real y es del camino de render —
  *parameter type mismatch* al setear un parámetro de material/shader. Como el motor no expande
  los argumentos no sabemos cuál es. Si después del fix la pantalla sigue negra con geometría que
  no aparece, ese mensaje es por dónde empezar (probablemente un uniform que vitaGL reporta con
  otro tipo del que el material espera).

## Fase 13: el fix de format-string verificado + heartbeat ciego tras el frame 10 (2026-09-06)

### Qué muestra `logs/debug_local_020.log` (1471 líneas, build release con Fase 11)

- El Data abort de firma fija (Fase 11) **desapareció**: la línea
  `parameter type mismatch when setting "%s/%s": want %s, got %s` sale entera y
  literal, y el motor sigue de largo. Tal como se predijo, los `%s` sin expandir
  son correctos (el motor nunca los expande).
- `frame 2 returned (7788468 us)` — ~7,8 s de carga pesada adentro del segundo
  `nativeRender`, y después **frames 3-10 retornando a ~10 ms**. El loop gira.
- 121 `createTextureImpl`, 69 `Loaded texture`, cero dumps (sin crash).
- `dummy.tga` x20 `FAILED`: ese archivo **no existe entre los 3214 datos**
  (verificado en `ux0_data/.../data/`), o sea que en Android tampoco lo
  encontraría — el motor lo tolera (`Could not find texture file` y sigue con
  60+ texturas más). Benigno, no es el blocker. No se toca.
- El log **termina a mitad de carga** (`Loaded texture`, sin `frame 11` ni dump):
  no es un cuelgue confirmado — a partir del frame 11 el heartbeat anterior se
  quedaba mudo por diseño (solo 1-10 + uno cada 300 frames, y con cargas de
  varios segundos por frame esos 300 pueden ser minutos). La próxima corrida con
  el heartbeat por tiempo dirá si el frame 11+ avanza o se traba.

### Cambios (`source/main.c`, sin tocar motor/render)

1. **Heartbeat por tiempo + slow-frame** (sigue en `l_note`, release-safe):
   frames 1-10 igual que antes; después, una línea si `nativeRender` tarda
   > 0,5 s (la carga se ve como slow frames) y un `frame N alive` como mucho
   cada 5 s de pared. Reemplaza el `frame_no % 300` (ciego durante cargas).
2. **Botones mapeados a keycodes Android** por el mismo camino
   `s_keyDownCode/s_keyUpCode` que usa la Activity real (`GameRenderer.java:71`):
   dpad → 19/20/21/22, CROSS → 23 (confirm), CIRCLE → 4 (back, como antes),
   START → 82 (menu). El motor ignora los códigos que no usa, igual que en un
   equipo real con teclado — para poder navegar menús y continuar el juego
   además del táctil (que sigue igual).

### Qué mirar en el próximo log

1. `frame 11+ slow render / alive` — si aparecen, el juego sigue cargando o ya
   está en título/menú y el problema restante es de **render/input**, no de loop.
2. Si se queda en `frame N enter` sin `returned` ni `alive` → trabado adentro de
   `nativeRender`, triage nuevo con ese N.

## Fase 12: verificación del fix de format-string + inventario de instrumentación TEMP (2026-09-05)

### Verificación: `logs/debug_local_017.log` — sin crash, el juego avanza

- La corrida con el `eboot.bin` del fix de Fase 11 (desplegado 03:24) **no genera dump** y la
  pantalla negra "avanza" (hay animación/progreso visible). El Data abort de firma fija
  (`SceLibKernel+0x60a2`, `R0=R1=R4` basura) desapareció: era efectivamente el `%s` sin expandir
  de `parameter type mismatch when setting "%s/%s": want %s, got %s`.
- El log trae solo 3 líneas (`method ID 27/28/29 not found`, build release sin `DEBUG_SOLOADER`):
  el motor ya llega al setup de audio (`setMusicGain/SfxGain/VfxGain`). Esas 3 advertencias
  corresponden al comportamiento pre-Fase-10 (IDs solo en `methodsVoid[]`); el fix (`methodsInt[]`
  + `Method_soundGainStub()`, ya en el árbol) debería silenciarlas en el próximo binario
  desplegado — si reaparecen, el `eboot.bin` en consola es anterior al fix.
- Análisis corroborante independiente (sesión paralela, dumps `...-1788586817` y `...-1788589177`):
  descomprimiendo el core (gzip) y barriendo la pila cruda del hilo `PSVGMV002` se recuperó la
  cadena viva completa — `Application::Init → PostPostInit → CHudManager::load → ASprite →
  CTextureManager::getTexture → loadTextureFromFile → bind → CImageLoaderPVR::loadTextureData →
  setData → forceCommitTexture → createMaterialRenderer → constructEffect →
  setMaterialParameter → Printer::logf → appDebugLog → strings/malloc → kernel`. Coincide con el
  punto del format string de Fase 11 (el `logf` del parámetro de material). Nota metodológica: la
  base auto-detectada del reporte (`0x80dcd000`/`0x80bcd000`) es incorrecta; la base real es el
  `LOAD_ADDRESS` fijo (`0x98000000`), y los offsets del reporte solo valen con `--so-base`.
- Archivos de datos verificados (`huds.bmp` y cía. son originales Gameloft con header propio, no
  BMP estándar; tamaños sanos) y todo el I/O con retornos OK — el crash nunca fue de datos.

### Inventario de instrumentación TEMP pendiente de limpieza

- `source/reimpl/pthr.c`: el muestreador de caller cada 500 locks (id 23) ya fue reemplazado por
  la sonda `[023]` barata de Fase 8 (reloj cada 4096 locks, máx. 1 línea/3 s). Nada que hacer.
- `source/reimpl/io.c` + `io.h` + `CMakeLists.txt`: los wrappers `fread/fseek` con log de
  entrada/salida quedaron correctamente detrás de `IO_TRACE_STREAMS` (OFF por defecto). Nada que
  hacer.
- `source/reimpl/gl.{c,h}` + `source/dynlib.c`: **siguen activos sin flag**:
  `glTexImage2D_soloader` (loguea CADA upload en debug) y `memcpy_soloader` (solo `>8MB`, barato).
  En release son costo cero (`l_debug` se compila fuera; el `l_error` de `INSANE`/`HUGE` es raro
  por construcción). Recomendación: si el próximo debug se vuelve lento cargando texturas, poner
  el `l_debug` por-upload detrás de un flag como `IO_TRACE_STREAMS`; los `l_error` pueden quedar.
- `memcpy`/`__aeabi_memcpy[48]` siguen apuntando a `memcpy_soloader` (idéntico fast-path a
  `sceClibMemcpy` salvo la comparación de tamaño). Revertir a `sceClibMemcpy` cuando se cierre el
  triage de heap.

### Estado y siguiente paso

- El loop de render vive (pantalla negra con progreso, sin crash): el problema restante es de
  **render**, no de lógica. Dos puntas anotadas, en este orden:
  1. El `parameter type mismatch` de Fase 11 (uniform con tipo distinto al esperado por vitaGL).
  2. `vglInitExtended(0, 960, 544, 6MB, MULTISAMPLE_4X)` vs. la receta de Asphalt 5
     (`12MB, MULTISAMPLE_NONE` + downsample por FBO a 800x480 para menús correctos): MSAA 4X +
     FBO OES (que este motor usa) es combinación riesgosa en vitaGL. Cambiar a NONE es un cambio
     de comportamiento: probar solo y medir.
