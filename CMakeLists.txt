cmake_minimum_required(VERSION 3.14)

if (NOT DEFINED ENV{VITASDK} AND NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  message(FATAL_ERROR "Please define VITASDK env var to point to your SDK path or set CMAKE_TOOLCHAIN_FILE.")
endif ()

set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake" CACHE PATH "Toolchain file")

project(gangstarmiamivindication C CXX)

include("${VITASDK}/share/vita.cmake" REQUIRED)

# Vita application configuration
set(VITA_APP_NAME "Gangstar Miami Vindication")
set(VITA_TITLEID "PSVGMV002")
set(VITA_VPKNAME "gangstarmiamivindication")
set(VITA_VERSION "01.00")
set(VITA_MKSFOEX_FLAGS "-d ATTRIBUTE2=12")

# Optional: used for "send" and "dump" targets
set(PSVITAIP "192.168.3.15" CACHE STRING "PSVita IP (for FTP access)")
set(PARSECOREPATH "$ENV{PARSECOREPATH}" CACHE STRING "vita-parse-core main.py")

# Game-specific definitions
set(DATA_PATH "ux0:data/gangstarmiamivindication/" CACHE STRING "Path to data (with trailing /)")
set(SO_PATH "${DATA_PATH}libGangster2.so" CACHE STRING "Path to .so")
add_definitions(-DDATA_PATH="${DATA_PATH}" -DSO_PATH="${SO_PATH}")

option(USE_SCELIBC_IO "Use SceLibcBridge for IO and some other std functions" ON)
if (USE_SCELIBC_IO)
  add_definitions(-DUSE_SCELIBC_IO)
endif()

set(SHADER_FORMAT "GLSL" CACHE STRING "Preferred shader format (one of 'GLSL', 'CG', 'GXP')")
if (${SHADER_FORMAT} STREQUAL "GLSL")
  add_definitions(-DUSE_GLSL_SHADERS)
elseif (${SHADER_FORMAT} STREQUAL "CG")
  add_definitions(-DUSE_CG_SHADERS)
elseif (${SHADER_FORMAT} STREQUAL "GXP")
  add_definitions(-DUSE_GXP_SHADERS)
else()
  message(FATAL_ERROR "SHADER_FORMAT option can only be one of: 'GLSL', 'CG', 'GXP'")
endif()

set(valid_dump_formats GLSL CG)
option(DUMP_COMPILED_SHADERS "When using GLSL or CG shader format, cache compiled shaders on disk" ON)
if (DUMP_COMPILED_SHADERS)
  if (SHADER_FORMAT IN_LIST valid_dump_formats)
    add_definitions(-DDUMP_COMPILED_SHADERS)
  else()
    message(WARNING "DUMP_COMPILED_SHADERS has no effect for the chosen shader format ${SHADER_FORMAT}")
  endif()
endif()

# Unique debug definition for loader's logging to not interfere with anything
if (CMAKE_BUILD_TYPE STREQUAL "Debug")
  add_definitions(-DDEBUG_SOLOADER)
  # Verbose per-call FalsoJNI logging (every JNI call logged with its args --
  # see port_progress.md, matches the level of detail other ports' debugnet
  # captures show and this one was missing).
  add_definitions(-DFALSOJNI_DEBUGLEVEL=0)
endif()

# Per-mutex-operation tracing in source/reimpl/pthr.c ([011]..[015], [021]).
# OFF by default and deliberately so: the game statically links its own
# dlmalloc, so those hooks fire once per malloc() and once per free(), and one
# log line per allocation makes the engine's loading path take effectively
# forever (that was the "infinite black-screen loop", see port_progress.md
# Fase 8). Turn on only while chasing a mutex bug.
option(PTHR_TRACE_LOCKS "Log every pthread mutex lock/unlock/init/destroy" OFF)
if (PTHR_TRACE_LOCKS)
  add_definitions(-DPTHR_TRACE_LOCKS)
endif()

# Per-call fread()/fseek() tracing in source/reimpl/io.c. OFF by default for the
# same reason as PTHR_TRACE_LOCKS: the engine reads its .bdae/.bsprite archives
# with thousands of tiny reads, and logging each one slows the loading path
# enough to change the behaviour under test. Turn on only to chase a bad read.
option(IO_TRACE_STREAMS "Log every fread()/fseek() call" OFF)
if (IO_TRACE_STREAMS)
  add_definitions(-DIO_TRACE_STREAMS)
endif()

# makes sincos, sincosf, etc. visible
add_definitions(-D_GNU_SOURCE -D__POSIX_VISIBLE=999999)

set(COMMON_C_FLAGS "-Wl,-q -mfloat-abi=softfp -mcpu=cortex-a9 -mfpu=neon -fno-strict-aliasing -Wno-deprecated")
if (CMAKE_BUILD_TYPE STREQUAL "Debug")
  set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${COMMON_C_FLAGS} -O1 -g3")
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${COMMON_C_FLAGS} -O1 -g3 -std=gnu++20 -Wno-write-strings -Wno-psabi")
else()
  set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${COMMON_C_FLAGS} -O3 -g0 -fomit-frame-pointer -ffast-math")
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${COMMON_C_FLAGS} -O3 -g0 -fomit-frame-pointer -ffast-math -std=gnu++20 -Wno-write-strings -Wno-psabi")
endif()

add_executable(${CMAKE_PROJECT_NAME}
               source/main.c
               source/dynlib.c
               source/java.c
               source/patch.c
               source/video.cpp
               source/reimpl/bits/_ctype.c
               source/reimpl/errno.c
               source/reimpl/gl.c
               source/reimpl/io.c
               source/reimpl/log.c
               source/reimpl/sysconf.c
               source/reimpl/mem.c
               source/reimpl/pthr.c
               source/reimpl/sys.c
               source/reimpl/time64.c
               source/reimpl/asset_manager.cpp
               source/utils/dialog.c
               source/utils/glutil.c
               source/utils/audio.c
               source/utils/init.c
               source/utils/logger.c
               source/utils/settings.c
               source/utils/utils.c
               source/utils/filecache.c
               source/utils/gamepad_actions.c
               lib/falso_jni/FalsoJNI.c
               lib/falso_jni/FalsoJNI_ImplBridge.c
               lib/falso_jni/FalsoJNI_Logger.c
               lib/falso_jni/converter.c
               lib/sha1/sha1.c
               lib/fios/fios.c
               lib/so_util/so_util.c)

add_subdirectory(lib/libc_bridge)
add_dependencies(${CMAKE_PROJECT_NAME} SceLibcBridge)

target_include_directories(${CMAKE_PROJECT_NAME} SYSTEM
                           PUBLIC ${CMAKE_SOURCE_DIR}/lib
                           )

target_include_directories(${CMAKE_PROJECT_NAME}
                           PUBLIC ${CMAKE_SOURCE_DIR}/source
                           )

file(GLOB VITAGL_SRC
    lib/vitaGL/source/*.c
    lib/vitaGL/source/*.cpp
    lib/vitaGL/source/utils/*.c
    lib/vitaGL/source/utils/*.cpp
    lib/vitaGL/source/utils/preprocessor/*.c
    lib/vitaGL/source/utils/preprocessor/*.cpp
)
list(FILTER VITAGL_SRC EXCLUDE REGEX ".*/\\._.*")
add_library(vitaGL_local STATIC ${VITAGL_SRC})
target_include_directories(vitaGL_local PUBLIC lib/vitaGL/source)
target_compile_definitions(vitaGL_local PRIVATE VGL_GIT_HASH="vendorized")
target_compile_options(vitaGL_local PRIVATE -Wno-incompatible-pointer-types -Wno-stringop-overflow -mfp16-format=ieee)
# Speed and optimization flags for vitaGL
target_compile_definitions(vitaGL_local PRIVATE
    HAVE_SOFTFP_ABI
    SKIP_SPLASHSCREEN
    HAVE_WVP_ON_GPU
    HAVE_SHADER_CACHE
    TEXTURES_SPEEDHACK
    DISABLE_TILE_CLIPPER
    SAMPLERS_SPEEDHACK
    # HAVE_TEX_CACHE (Fase 46, 2026-09-18): logs/debug_local_048.log muestra, a
    # partir del frame 2499 (mundo abierto real, no carga), una racha de
    # `gpu_alloc_mapped_aligned_for_gpu failed with a requested size of
    # 4194304 bytes` -- los cuatro pools que gpu_alloc_mapped_aligned_for_gpu_inner
    # intenta en orden (VRAM, RAM, PHYCONT, BUDGET; utils/gpu_utils.c:103-113)
    # estan agotados. Sin HAVE_TEX_CACHE, vitaGL no tiene forma de liberar
    # memoria por su cuenta: cae directo a
    # gpu_alloc_mapped_aligned_unsafe_for_gpu (utils/gpu_utils.c:181-205), que
    # por cada intento hace sceGxmFinish() + sceKernelDelayThread(1000000) (1
    # SEGUNDO fijo) y reintenta hasta FRAME_PURGE_FREQ=4 veces
    # (lib/vitaGL/source/shared.h:49) -- coincide exacto con los "frame N slow
    # render (4130-4368 ms)" del log (4 ciclos x ~1s + overhead). Peor: de 8
    # fallos de gpu_alloc_mapped_aligned_for_gpu en ese log, solo 1 termino en
    # "successfully allocated" -- los otros 7 agotan los 4 ciclos SIN liberar
    # memoria, dejando la textura sin subir (riesgo real de una textura rota o
    # un NULL-deref mas adelante, no solo de framerate).
    #
    # HAVE_TEX_CACHE es la respuesta de este mismo vitaGL (Rinnegatamante) para
    # exactamente este escenario -- mundo abierto con mas texturas que VRAM
    # disponible (la misma tecnica que usan sus ports de la familia GTA):
    # gpu_alloc_mapped_aligned_for_gpu/for_cpu, antes de forzar el GC "unsafe",
    # prueban vgl_cache_old_textures() (utils/gpu_utils.c:116-165), que vuelca a
    # disco (`ux0:data/vgl_cache/<titleid>`, creado solo por vitaGL en
    # vglInitWithCustomSizes) cualquier textura no usada en el frame actual y
    # libera su copia en VRAM -- evita por completo la espera fija de 1s/ciclo
    # cuando alcanza con reciclar texturas frias, y si no alcanza, el codigo cae
    # igual al mismo camino "unsafe" de antes (no empeora el peor caso). Ver
    # `port_progress.md` Fase 46. Pendiente de confirmar en consola real: los
    # `gpu_alloc_mapped_aligned_for_gpu failed` deberian bajar en frecuencia y
    # duracion, sin nuevos stalls de I/O de disco visibles en el framerate.
    HAVE_TEX_CACHE
    # SAFER_DRAW_SPEEDHACK (Fase 37): para streams de color/normal/vertice de un
    # draw grande (>0x8000 bytes -- mallas de vehiculos/edificios/personajes),
    # manda el puntero del vertex array del motor directo a
    # sceGxmSetVertexStream() en vez de copiarlo al circular pool. Se habia
    # sacado en la Fase 39 sospechando una carrera CPU/GPU como causa del negro
    # en personajes/vehiculos, pero en consola real (Fase 41, debug_local_044.log)
    # sacarlo NO arreglo el negro y en cambio devolvio el patron exacto de la
    # Fase 29 (overrun del circular pool: fps cayendo en degradacion progresiva
    # de 30 a 3-5 fps a medida que se acumulan mallas grandes en escena, no un
    # piso parejo). Confirmado: esto es lo que daba la velocidad, se restaura.
    SAFER_DRAW_SPEEDHACK
)
# MATH_SPEEDHACK (adoptado de Asphalt-6-Vita en la Fase 38) SACADO en la Fase 41:
# revisando lib/vitaGL/source/matrices.c (glOrthof/glFrustumf/glMatrixOrtho/
# glMatrixFrustum), la variante rapida REEMPLAZA la matriz activa por la nueva
# proyeccion (`matrix4x4_init_orthographic(*matrix, ...)`) en vez de componerla
# con la que ya estaba cargada (`matrix4x4_multiply(res, *matrix, ortho_matrix)`).
# Eso es distinto del comportamiento real de glOrtho/glFrustum salvo que el
# motor siempre llame glLoadIdentity() justo antes -- no verificado para este
# motor. Sacado porque en consola real (Fase 41) las texturas de
# personajes/vehiculos seguian en negro incluso con SAFER_DRAW_SPEEDHACK
# restaurado, así que el sospechoso pasa a ser este flag (el otro cambio nuevo
# de la misma tanda). Reactivar solo si se confirma que no es la causa.
# target_compile_definitions(vitaGL_local PRIVATE MATH_SPEEDHACK)

# SKIP_ERROR_HANDLING (Fase 39) SACADO en la Fase 43: con MATH_SPEEDHACK Y
# SAFER_DRAW_SPEEDHACK ya descartados en consola real (Fase 41,
# debug_local_044/045.log) y el negro de personajes/vehiculos todavia presente
# tras el trabajo de Fase 42 (debug_local_045.log), es el ultimo flag nuevo de
# la tanda Fase 33-39 sin verificar. Revisado a mano en lib/vitaGL/source/*.c:
# la mayoria de sus ~90 sitios son chequeos de parametro puros (SET_GL_ERROR +
# return temprano, sin efecto de estado real si se saltean) -- salvo
# glPopAttrib (misc.c), donde el chequeo de stack-underflow tambien hace
# `return`; sin SKIP_ERROR_HANDLING ese guard desaparece y un pop de mas leeria
# basura de attrib_stack[255] (contador uint8_t desbordado). Descartado como
# causa puntual: el motor real (objdump -T sobre libGangster2.so) NO importa
# glPushAttrib/glPopAttrib. No se encontro un mecanismo concreto que explique
# por que el negro es selectivo a personajes/vehiculos (geometria dinamicamente
# iluminada, GL_LIGHTING con luces reales) vs. mundo estatico (lightmap
# horneado, sin GL_LIGHTING) -- glMaterialfv/glLightfv/light_global_ambient se
# revisaron y son correctos. Se saca este flag igual, junto con LOG_ERRORS
# (abajo), para que la PROXIMA corrida en consola real muestre en el log
# cualquier `[vitaGL] file:line: func set GL_INVALID_ENUM/VALUE (...)` que el
# motor dispare al armar el material de un personaje/vehiculo -- evidencia
# concreta en vez de otra hipotesis. Si el log sale limpio (lastErr=0x0) y el
# negro persiste, el ultimo sospechoso de esta tanda es HAVE_WVP_ON_GPU (mueve
# la composicion World-View-Projection a la GPU) -- aunque el codigo revisado
# (lib/vitaGL/source/shaders/ffp_v.h `Jwvp = mul(Jwvp, Imodelview)`) se ejecuta
# igual para geometria estatica que YA renderiza bien, asi que es un
# sospechoso mas debil que SKIP_ERROR_HANDLING.
# target_compile_definitions(vitaGL_local PRIVATE SKIP_ERROR_HANDLING)

# LOG_ERRORS (reactivado en la Fase 43, mismo mecanismo que la Fase 24): sin
# esto, SET_GL_ERROR() en vitaGL llama a vgl_log() pero vgl_log es un no-op
# cuando LOG_ERRORS no esta definido (lib/vitaGL/source/utils/debug_utils.h) --
# o sea que sacar SKIP_ERROR_HANDLING por si solo no alcanza para VER ningun
# error nuevo en el log. `source/reimpl/gl.c` ya tiene `vgl_log_capture()`
# (Fase 24) que reenvia estas lineas a `l_note("[vitaGL] ...")` con dedupe, asi
# que reactivarlo es gratis en términos de nuevo código.
target_compile_definitions(vitaGL_local PRIVATE LOG_ERRORS)

# DISABLE_TEXTURE_COMBINER (Fase 37) SACADO en la Fase 44 -- CAUSA RAIZ REAL
# del negro en personajes/vehiculos, confirmada con hardware real gracias a la
# instrumentacion de la Fase 43 (`logs/debug_local_046.log`, lineas 62-93 y
# 132-134): decenas de `glTexEnvi set GL_INVALID_ENUM` en
# `lib/vitaGL/source/ffp.c:3021`, con pnames 0x8571/0x8572 (GL_COMBINE_RGB/
# ALPHA), 0x8580-0x858A (GL_SOURCE0-2_RGB/ALPHA), 0x8590-0x859A
# (GL_OPERAND0-2_RGB/ALPHA) y 0xD1C (GL_ALPHA_SCALE) -- exactamente el bloque
# de parametros de `GL_COMBINE` que existe en ese `switch` SOLO cuando
# DISABLE_TEXTURE_COMBINER NO esta definido (ver `#ifndef DISABLE_TEXTURE_
# COMBINER` alrededor de esos `case` en ffp.c). El motor SI usa GL_COMBINE de
# verdad para el material de personajes/vehiculos -- la premisa de la Fase 37
# ("Gangstar nunca utiliza GL_COMBINE, confirmado en Ghidra") era incorrecta o
# incompleta. Con el flag puesto, cada llamada a glTexEnvi(GL_COMBINE_RGB/...)
# caia al `default: SET_GL_ERROR_WITH_VALUE(GL_INVALID_ENUM, pname)` y se
# descartaba en silencio (con SKIP_ERROR_HANDLING todavia puesto, ni siquiera
# quedaba loggeado) -- el combiner del personaje/vehiculo nunca se armaba,
# quedando en un estado sin fuente de color valida (negro). El mundo estatico
# no usa GL_COMBINE (solo GL_MODULATE/REPLACE, confirmado por seguir
# renderizando bien todo este tiempo) -- coincide exactamente con "negro
# selectivo a personajes/vehiculos, no a todos los objetos" documentado desde
# la Fase 39.
# Pendiente de confirmar en consola real: correr esta build y mandar un log
# fresco -- deberian desaparecer tanto las lineas `[vitaGL] ... GL_INVALID_
# ENUM (pname: 0x85xx/0xD1C)` como el negro en personajes/vehiculos.

add_library(vitashark_local STATIC lib/vitashark/source/vitashark.c)
target_include_directories(vitashark_local PUBLIC lib/vitashark/source)
target_compile_options(vitashark_local PRIVATE -Wno-incompatible-pointer-types)

target_link_libraries(vitaGL_local PUBLIC vitashark_local)

target_link_libraries(${CMAKE_PROJECT_NAME}
                      -Wl,--whole-archive pthread -Wl,--no-whole-archive
                      stdc++
                      vorbisfile
                      vorbis
                      ogg
                      vitaGL_local
                      vitashark_local
                      SceShaccCgExt
                      taihen_stub
                      SceShaccCg_stub
                      z
                      m
                      ScePgf_stub
                      mathneon
                      kubridge_stub
                      ${CMAKE_BINARY_DIR}/lib/libc_bridge/stubs/libSceLibcBridge_stub.a
                      SceHid_stub
                      SceTouch_stub
                      SceMotion_stub
                      SceAppMgr_stub
                      SceAppUtil_stub
                      SceAudio_stub
                      SceAudioIn_stub
                      SceCommonDialog_stub
                      SceDisplay_stub
                      SceKernelDmacMgr_stub
                      SceFios2_stub
                      SceNet_stub
                      SceNetCtl_stub
                      SceCtrl_stub
                      SceGxm_stub
                      SceSysmodule_stub
                      ScePower_stub
                      SceTouch_stub
                      SceVshBridge_stub
                      # Fase 50 (2026-09-19): cutscene video/audio decode moved
                      # entirely to software FFmpeg -- see source/video.cpp's
                      # file header for why (the Vita's hardware decoder only
                      # supports H.264, and intro.m4v is MPEG-4 Part 2).
                      # SceAvPlayer_stub is intentionally NOT linked anymore --
                      # nothing in this port calls into it. Same technique and
                      # same vita-portlibs packages (`vdpm ffmpeg` + `vdpm lame`)
                      # as this workspace's Asphalt-5-Vita port, which hit and
                      # fixed the exact same missing-mov-demuxer bug first.
                      avformat
                      avcodec
                      avutil
                      swresample
                      # vita-portlibs' avcodec.a is built with the MP3 (LAME)
                      # ENCODER compiled in, so it's a hard link-time
                      # dependency even though this port only ever decodes.
                      mp3lame
                      )

vita_create_self(eboot.bin ${CMAKE_PROJECT_NAME} UNSAFE NOASLR)
vita_create_vpk(${VITA_VPKNAME}.vpk ${VITA_TITLEID} eboot.bin
                VERSION ${VITA_VERSION}
                NAME "${VITA_APP_NAME}"
                FILE "${CMAKE_SOURCE_DIR}/extras/cpuinfo" cpuinfo
                "${CMAKE_SOURCE_DIR}/extras/meminfo" meminfo
                "${CMAKE_SOURCE_DIR}/extras/livearea/icon0.png" sce_sys/icon0.png
                "${CMAKE_SOURCE_DIR}/extras/livearea/pic0.png" sce_sys/pic0.png
                "${CMAKE_SOURCE_DIR}/extras/livearea/startup.png" sce_sys/livearea/contents/startup.png
                "${CMAKE_SOURCE_DIR}/extras/livearea/bg0.png" sce_sys/livearea/contents/bg0.png
                "${CMAKE_SOURCE_DIR}/extras/livearea/template.xml" sce_sys/livearea/contents/template.xml
                )

add_custom_target(send
                  COMMAND echo destroy | nc ${PSVITAIP} 1338
                  COMMAND echo destroy | nc ${PSVITAIP} 1338
                  COMMAND curl -T eboot.bin ftp://${PSVITAIP}:1337/ux0:/app/${VITA_TITLEID}/
                  COMMAND echo launch ${VITA_TITLEID} | nc ${PSVITAIP} 1338
                  DEPENDS ${VITA_VPKNAME}.vpk-vpk
                  )

add_custom_target(send_kvdb
                  COMMAND echo destroy | nc ${PSVITAIP} 1338
                  COMMAND curl -T eboot.bin ftp://${PSVITAIP}:1337/ux0:/app/${VITA_TITLEID}/
                  COMMAND echo launch ux0:/app/${VITA_TITLEID}/eboot.bin | nc ${PSVITAIP} 1338
                  DEPENDS ${VITA_VPKNAME}.vpk-vpk
                  )

add_custom_target(dump
                  COMMAND ${CMAKE_SOURCE_DIR}/extras/scripts/get_dump.sh ${PSVITAIP}
                  COMMAND python3 ${PARSECOREPATH} coredump ${CMAKE_PROJECT_NAME}
                  )

add_custom_target(reboot
                  COMMAND echo reboot | nc ${PSVITAIP} 1338
                  )
