/*
 * perf_lod.c -- why the city feels like it is "still generating" while
 * driving, and the measured half-step back (Fase 62, paso 5).
 *
 * Evidence (all read from the real binary, nothing guessed):
 * - GS3DStuff::loadPerformanceProfile() (out_ghidra.c:27166-27288) hardcodes
 *   the world streaming radius and far plane per device id: Motorola 1 =
 *   6500/14000, Samsung 2 = 10000/30000, HTC 3 and Low End 4 = 6000/15000
 *   and 6000/13000 respectively (the 3/4 block shares radius 6000,
 *   out_ghidra.c:27254-27271), plus spawn caps and effects flags.
 * - Fase 60 made Method_GetDeviceType() return 4, so since then the game
 *   streams the world with radius 6000 (latched once into the function-local
 *   static of GS3DStuff::updateStreaming(), out_ghidra.c:25506-25514 --
 *   init-once guard, so it is the FIRST streaming update that counts) and
 *   culls past 13000. Before Fase 60 (device -1) the profile FILE's larger
 *   values stood, and nobody reported pop-in -- the pop-in report arrives
 *   exactly with the 6000 radius. Cause, not coincidence.
 * - The same symptom exists in the sibling port Asphalt-6-Vita, whose README
 *   documents it as open: it keeps the factory LOD factor (0.4) because
 *   forcing the per-track LOD (-1.0) cost ~50 ms/frame there (their Bugs
 *   #041/#048, source/patch.c). Lesson: buy view distance back in small
 *   measured steps, watching the GPU wells of our Fase 46/60, not all at
 *   once.
 *
 * What this file does: keeps device 4 (ALL of its savings -- spawn caps 1,
 * no shadows/dynamic lighting/far water/retro) and only widens the two
 * view-distance ints inside the engine's own gPhonePerf struct (dynamically
 * exported, resolved by name like every other engine symbol in this port):
 * radius 6000 -> 8000 (+33% streaming frontier), far 13000 -> 15000 (the
 * HTC 3 value, same flags family). Ghidra's field names confirm the byte
 * offsets (_12_4_ = radius, _16_4_ = far: 10 booleans + 2 pad bytes, then
 * ints). Reverting is one line per value; going to Samsung's 10000/30000
 * is the documented next step ONLY if the next console log shows no
 * `gpu_alloc...failed` regression AND pop-in persists.
 */

#include "utils/perf_lod.h"
#include "utils/logger.h"

#include <stdint.h>

#include <so_util/so_util.h>

extern so_module so_mod;

#define PERF_OFF_RADIUS  12  /* gPhonePerf._12_4_: streaming radius, latched by updateStreaming() */
#define PERF_OFF_FAR     16  /* gPhonePerf._16_4_: far plane */

#define LOD_STREAM_RADIUS 8000
#define LOD_FAR_PLANE     15000

static int32_t *s_perf = NULL;

void perf_lod_init(void) {
    s_perf = (int32_t *)so_symbol(&so_mod, "gPhonePerf");
    if (!s_perf)
        l_warn("[lod] gPhonePerf not resolved -- LOD override disabled");
}

void perf_lod_apply(void) {
    if (!s_perf)
        return;
    /* Fase 63: log every real transition instead of just the first apply.
     * Log 062 showed why: loadPerformanceProfile() runs during PostInit
     * (frame 2), AFTER Gangster2_nativeInit() -- so the init-time apply saw
     * a still-zero struct ("radius 0->8000"). The per-frame refresh is what
     * actually wins the race against the 6000/13000 write; when it does, this
     * logs "radius 6000->8000, far 13000->15000", which both confirms the
     * override landed and timestamps the load. Silent while values are ours
     * (the steady state -- no per-frame spam). */
    int32_t r = s_perf[PERF_OFF_RADIUS / 4];
    int32_t f = s_perf[PERF_OFF_FAR / 4];
    if (r == (int32_t)LOD_STREAM_RADIUS && f == (int32_t)LOD_FAR_PLANE)
        return;
    l_note("[lod] gPhonePerf radius %d->%d, far %d->%d",
           r, (int)LOD_STREAM_RADIUS, f, (int)LOD_FAR_PLANE);
    s_perf[PERF_OFF_RADIUS / 4] = (int32_t)LOD_STREAM_RADIUS;
    s_perf[PERF_OFF_FAR / 4] = (int32_t)LOD_FAR_PLANE;
}
