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
#include "reimpl/io.h"

#include <string.h>

#include <psp2/appmgr.h>
#include <psp2/apputil.h>
#include <psp2/kernel/clib.h>
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
