/*
 * Copyright (C) 2021      Andy Nguyen
 * Copyright (C) 2021-2022 Rinnegatamante
 * Copyright (C) 2022-2024 Volodymyr Atamanenko
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#include "utils/init.h"

#include "utils/dialog.h"
#include "utils/glutil.h"
#include "utils/logger.h"
#include "utils/utils.h"
#include "utils/settings.h"
#include "utils/filecache.h"
#include "reimpl/io.h"

#include <string.h>

#include <psp2/appmgr.h>
#include <psp2/apputil.h>
#include <psp2/kernel/clib.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/power.h>

#include <falso_jni/FalsoJNI.h>
#include <so_util/so_util.h>
#include <fios/fios.h>

// Base address for the Android .so to be loaded at
#define LOAD_ADDRESS 0x98000000

extern so_module so_mod;

void soloader_init_all() {
	// Launch `app0:configurator.bin` on `-config` init param
    sceAppUtilInit(&(SceAppUtilInitParam){}, &(SceAppUtilBootParam){});
    SceAppUtilAppEventParam eventParam;
    sceClibMemset(&eventParam, 0, sizeof(SceAppUtilAppEventParam));
    sceAppUtilReceiveAppEvent(&eventParam);
    if (eventParam.type == 0x05) {
        char buffer[2048];
        sceAppUtilAppEventParseLiveArea(&eventParam, buffer);
        if (strstr(buffer, "-config"))
            sceAppMgrLoadExec("app0:/configurator.bin", NULL, NULL);
    }

    // Set default overclock values
    scePowerSetArmClockFrequency(444);
    scePowerSetBusClockFrequency(222);
    scePowerSetGpuClockFrequency(222);
    scePowerSetGpuXbarClockFrequency(166);

    // Fase 57: pin the main/render thread to core 0. Right after the
    // overclock, before anything else -- same spot and same order as
    // Shadow-Guardian-vita and Sacred-Odyssey-vita (MetalSyntax, same
    // Gameloft "Glitch" engine as this port, same soloader+vitaGL+FalsoJNI
    // stack), both commented "Dedicate main thread to core 0"
    // (source/main.c in both repos, called from int main() immediately
    // after the four scePowerSet*() calls). Those two are also the only
    // two of the four MetalSyntax Glitch-engine ports checked (the other
    // two being Asphalt-5-Vita and Asphalt-6-Vita, neither of which pins
    // any thread) that are documented as reaching a stable smooth
    // framerate rather than "early port, open bugs" (Asphalt-6's own
    // README wording).
    //
    // This port's own source/video.cpp already assumes this: the cutscene
    // decode thread comment says "keep the render thread's core to itself"
    // and explicitly avoids CPU_MASK_USER_0 for the video/audio threads
    // (video_decode_thread uses USER_1|USER_2, cutscene_audio_thread uses
    // USER_2 only) -- but nothing ever pinned the render thread itself, so
    // the OS scheduler was free to migrate it onto core 1 or 2 and collide
    // with those decode threads, adding scheduling jitter/cache-thrashing
    // on top of whatever the frame itself costs. Pinning it here closes
    // that gap. Low risk: this only affects which physical core runs the
    // existing main-thread code, not what that code does -- unlike the
    // vitaGL *_SPEEDHACK flags this project has already tried and reverted
    // (see CLAUDE.md rule 2 / Fase 39-41), it changes no rendering
    // semantics. Not yet hardware-verified for this specific title.
    sceKernelChangeThreadCpuAffinityMask(sceKernelGetThreadId(), SCE_KERNEL_CPU_MASK_USER_0);

#ifdef USE_SCELIBC_IO
    if (fios_init(DATA_PATH) == 0)
        l_success("FIOS initialized.");
#endif

    if (!module_loaded("kubridge")) {
        l_fatal("kubridge is not loaded.");
        fatal_error("Error: kubridge.skprx is not installed.");
    }
    l_success("kubridge check passed.");

    // The .so writes into subdirectories of its (translated) content root that
    // nothing else creates -- see io_prepare_dirs() in reimpl/io.c.
    io_prepare_dirs();

    // Index all files in data directory into RAM to eliminate expensive FAT32 scans
    filecache_init();

    if (!file_exists(SO_PATH)) {
        fatal_error("Looks like you haven't installed the data files for this "
                    "port, or they are in an incorrect location. Please make "
                    "sure that you have %s file exactly at that path.", SO_PATH);
    }

    if (so_file_load(&so_mod, SO_PATH, LOAD_ADDRESS) < 0) {
        l_fatal("SO could not be loaded.");
        fatal_error("Error: could not load %s.", SO_PATH);
    }

    settings_load();
    l_success("Settings loaded.");

    so_relocate(&so_mod);
    l_success("SO relocated.");

    resolve_imports(&so_mod);
    l_success("SO imports resolved.");

    so_patch();
    l_success("SO patched.");

    so_flush_caches(&so_mod);
    l_success("SO caches flushed.");

    // Triage instrumentation for the 0x98673b88-signature crash in PSVGMV002
    // (pthread_mutex_unlock jumping to a garbage PC, confirmed bit-identical
    // across 5 real-hardware runs) -- the crashing mutex's real_ptr never
    // shows up in any [011]/[013]/[014]/[015] checkpoint, meaning whatever
    // calls the real pthread_mutex_unlock() on it never went through
    // _mutex_t_static_init()/our _soloader shims at all. Logging the .so's
    // own actual memory ranges here lets the next capture confirm whether
    // that address falls inside the .so's own text/data (game's own runtime
    // calling pthread_mutex_unlock with a raw non-bionic pointer) or not.
    // Remove once the real cause is confirmed -- see port_progress.md.
    l_checkpoint(16, "so ranges: text=[%p-%p)", (void*) so_mod.text_base,
                 (void*) (so_mod.text_base + so_mod.text_size));
    for (int i = 0; i < so_mod.n_data; i++) {
        l_checkpoint(16, "so ranges: data[%d]=[%p-%p)", i,
                     (void*) so_mod.data_base[i],
                     (void*) (so_mod.data_base[i] + so_mod.data_size[i]));
    }

    so_initialize(&so_mod);
    l_success("SO initialized.");

    gl_preload();
    l_success("OpenGL preloaded.");

    jni_init();
    l_success("FalsoJNI initialized.");
}
