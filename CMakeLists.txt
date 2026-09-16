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
    SKIP_ERROR_HANDLING
    HAVE_WVP_ON_GPU
    HAVE_SHADER_CACHE
    TEXTURES_SPEEDHACK
    DISABLE_TILE_CLIPPER
    DISABLE_TEXTURE_COMBINER
    SAMPLERS_SPEEDHACK
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
                      SceAvPlayer_stub
                      SceSysmodule_stub
                      ScePower_stub
                      SceTouch_stub
                      SceVshBridge_stub
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
