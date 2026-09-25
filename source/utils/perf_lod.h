/*
 * perf_lod.h -- runtime view-distance override for the Low-End device profile
 * (Fase 62, paso 5). See perf_lod.c for the full evidence trail.
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* Resolves the engine's gPhonePerf struct. Safe to call once at startup;
 * logs a warning and disables itself if the symbol is missing, rather than
 * aborting the port over an unconfirmed extra. */
void perf_lod_init(void);

/* Re-applies the tuned radius/far over whatever loadPerformanceProfile()
 * last wrote. Idempotent and cheap (two int loads, two stores when needed):
 * call once right after Gangster2_nativeInit() and once per frame (both
 * almost always silent -- it only logs on a real transition, e.g. when the
 * PostInit profile load lands) as insurance against the settings-driven
 * profile-reload path. */
void perf_lod_apply(void);

#ifdef __cplusplus
}
#endif
