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

// Triage instrumentation for the 0x98673b88-signature crash in PSVGMV002
// (real pthread_mutex_unlock() jumping to a garbage PC, bit-identical across
// 6 real-hardware runs). Disassembling libsupc++.a's own __cxa_guard_acquire/
// __cxa_guard_release (guard.o) shows they don't touch any per-guard-variable
// mutex at all -- every C++ "magic static" in the whole program (both our own
// loader's and the .so's, e.g. its _Locale_true/_Init_timeinfo function-local
// statics) serializes through ONE process-wide global mutex+cond pair,
// `_ZN12_GLOBAL__N_1L12static_mutexE`/`..._1L11static_condE`, lazily
// pthread_once-initialized the first time __cxa_guard_acquire() is ever
// called anywhere. If THAT single mutex's calloc'd internal struct gets
// corrupted early (its first word ends up holding a .so-data-looking pointer
// instead of a semaphore UID), every later __cxa_guard_release() anywhere in
// the program would crash with this exact identical signature -- matching
// the total determinism observed.
//
// `static_mutex` has C++ internal (anonymous-namespace) linkage, so it can't
// be `extern`-declared from here -- the address below was read via
// `arm-vita-eabi-nm build/gangstarmiamivindication.elf | grep static_mutex`
// on THIS exact build. It WILL shift if the loader binary's .bss layout
// changes (e.g. adding more static locals) -- re-run that nm command after
// any further code change and update this if it moved, or the log will
// print the wrong address's value.
#define STATIC_MUTEX_ADDR ((void **) 0x811963d0)

static void log_guard_mutex_state(const char *when) {
    void *handle = *STATIC_MUTEX_ADDR;
    l_checkpoint(19, "cxa guard static_mutex %s: handle=%p *handle=%p",
                 when, handle, handle ? *(void **) handle : NULL);
}

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

    if (!file_exists(SO_PATH)) {
        fatal_error("Looks like you haven't installed the data files for this "
                    "port, or they are in an incorrect location. Please make "
                    "sure that you have %s file exactly at that path.", SO_PATH);
    }

    if (so_file_load(&so_mod, SO_PATH, LOAD_ADDRESS) < 0) {
        l_fatal("SO could not be loaded.");
        fatal_error("Error: could not load %s.", SO_PATH);
    }
    log_guard_mutex_state("after so_file_load");

    settings_load();
    l_success("Settings loaded.");
    log_guard_mutex_state("after settings_load");

    so_relocate(&so_mod);
    l_success("SO relocated.");
    log_guard_mutex_state("after so_relocate");

    resolve_imports(&so_mod);
    l_success("SO imports resolved.");
    log_guard_mutex_state("after resolve_imports");

    so_patch();
    l_success("SO patched.");
    log_guard_mutex_state("after so_patch");

    so_flush_caches(&so_mod);
    l_success("SO caches flushed.");
    log_guard_mutex_state("after so_flush_caches");

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
