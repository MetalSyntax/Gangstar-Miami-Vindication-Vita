/*
 * See audio.h for the why. Implementation notes:
 * - Decoding via libvorbisfile (libvorbisfile.a + libvorbis.a + libogg.a
 *   ship with vitasdk). Files are opened by Vita path directly
 *   (DATA_PATH "data/<name>") so no io.c translation is involved.
 * - Resampling to the 48 kHz output rate is linear interpolation; SFX are
 *   resampled once at load, streams per chunk in the mixer thread.
 * - The mixer thread is the ONLY thread touching voice decode state; the
 *   JNI thread only flips flags / volumes under a mutex. No per-frame
 *   allocation in the mix loop (scratch buffers are static).
 * - Float args arriving through FalsoJNI varargs are read as double
 *   (C promotion) with NaN/range guards so a misread can only affect
 *   loudness, never indexing or state.
 */

#include "utils/audio.h"
#include "utils/filecache.h"
#include "utils/logger.h"
#include "sound_files.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include <psp2/audioout.h>
#include <psp2/kernel/clib.h>
#include <psp2/kernel/threadmgr.h>

#include <vorbis/vorbisfile.h>

#ifndef DATA_PATH
#define DATA_PATH "ux0:data/gangstarmiamivindication/"
#endif

#define OUT_RATE 48000
#define OUT_FRAMES 2048
#define MAX_SFX_VOICES 8
#define MAX_BIG_VOICES 4
#define SFX_CACHE_MAX 128
#define SFX_DECODE_CAP (2 * 1024 * 1024)

static int audio_port = -1;
static volatile int audio_running = 0;
static SceUID audio_mutex = -1;
static SceUID audio_thread = -1;
static int audio_blocked = 0;
static float gain_music = 1.0f, gain_sfx = 1.0f, gain_vfx = 1.0f;

static float sane_vol(float v) {
    if (!(v >= 0.0f) || !(v <= 2.0f))
        return 1.0f;
    return v;
}

static float sane_pitch(float p) {
    if (!(p >= 0.25f) || !(p <= 4.0f))
        return 1.0f;
    return p;
}

static int snd_category(int index) {
    if (index < 0 || index >= GMV_SOUND_COUNT)
        return 1;
    const char *n = gmv_sound_files[index];
    if (n[0] == 'm' && n[1] == '_')
        return 0;
    if (!strncmp(n, "sfx_", 4))
        return 1;
    return 2;
}

static float cat_gain(int index) {
    switch (snd_category(index)) {
    case 0: return gain_music;
    case 1: return gain_sfx;
    default: return gain_vfx;
    }
}

static void snd_path(int index, char *out, size_t n) {
    if (index < 0 || index >= GMV_SOUND_COUNT) {
        out[0] = '\0';
        return;
    }
    snprintf(out, n, "%sdata/%s", DATA_PATH, gmv_sound_files[index]);
}

static int8_t sound_exists_cache[GMV_SOUND_COUNT];

static int snd_exists(int index) {
    if (index < 0 || index >= GMV_SOUND_COUNT)
        return 0;
    if (sound_exists_cache[index] == 0) {
        if (filecache_exists(gmv_sound_files[index])) {
            sound_exists_cache[index] = 1;
        } else {
            sound_exists_cache[index] = -1;
        }
    }
    return (sound_exists_cache[index] == 1);
}

/* ---------------- SFX cache (fully decoded) ---------------- */

typedef struct {
    int index;
    int16_t *pcm;      /* stereo, OUT_RATE */
    uint32_t frames;
    int used;
    uint32_t last_used;
} sfx_entry_t;

static sfx_entry_t sfx_cache[SFX_CACHE_MAX];
static uint32_t sfx_tick = 0;

typedef struct {
    int active;
    int paused;
    sfx_entry_t *e;
    uint32_t pos_fix;  /* 16.16 */
    uint32_t step_fix;
    float vol;
} sfx_voice_t;

static sfx_voice_t sfx_voices[MAX_SFX_VOICES];

static sfx_entry_t *sfx_find(int index) {
    for (int i = 0; i < SFX_CACHE_MAX; i++)
        if (sfx_cache[i].used && sfx_cache[i].index == index) {
            sfx_cache[i].last_used = ++sfx_tick;
            return &sfx_cache[i];
        }
    return NULL;
}

/* Decode whole ogg to stereo OUT_RATE PCM into a LOCAL buffer -- touches no
 * shared state (sfx_cache/sfx_voices), so unlike the old single-function
 * sfx_decode() this is safe to call WITHOUT audio_mutex held. This is the
 * expensive part (file open + full decode + resample, up to several hundred
 * ms for a long SFX on flash storage) and is only ever called from the
 * mixer thread now (see audio_mix_thread()'s pending-request drain below) --
 * never from the JNI/render thread. Returns 0 and fills *out_pcm/*out_frames
 * on success. */
static int sfx_decode_raw(int index, int16_t **out_pcm, uint32_t *out_frames_p) {
    char path[512];
    snd_path(index, path, sizeof(path));
    if (!path[0])
        return -1;

    FILE *f = fopen(path, "rb");
    if (!f)
        return -1;
    OggVorbis_File vf;
    if (ov_open(f, &vf, NULL, 0) < 0) {
        fclose(f);
        return -1;
    }
    vorbis_info *vi = ov_info(&vf, -1);
    long src_rate = vi ? vi->rate : 44100;
    int src_ch = vi ? vi->channels : 1;
    double total = ov_time_total(&vf, -1);
    if (!(total > 0.0) || total > 30.0) {
        ov_clear(&vf);
        return -1;
    }
    uint32_t out_frames = (uint32_t)(total * OUT_RATE) + 1;
    if ((uint64_t)out_frames * 4 > SFX_DECODE_CAP) {
        ov_clear(&vf);
        return -1;
    }
    int16_t *pcm = calloc(out_frames, 4);
    if (!pcm) {
        ov_clear(&vf);
        return -1;
    }

    /* Decode at source rate into a stereo temp buffer, then
     * linear-resample to OUT_RATE. Chunked: mono files are upmixed to
     * stereo here, so stride bugs can't leak heap garbage into the mix. */
    uint32_t src_cap = (uint32_t)(total * src_rate) + 64;
    int16_t *src = malloc((size_t)src_cap * 2 * 2);
    uint32_t src_frames = 0;
    if (src) {
        static uint8_t raw[8192];
        int bitstream = 0;
        while (src_frames < src_cap) {
            long got = ov_read(&vf, (char *)raw, sizeof(raw), 0, 2, 1, &bitstream);
            if (got <= 0)
                break;
            long fr = got / (2L * (long)src_ch);
            int16_t *s = (int16_t *)raw;
            for (long i = 0; i < fr && src_frames < src_cap; i++) {
                if (src_ch == 1) {
                    src[(size_t)src_frames * 2 + 0] = s[0];
                    src[(size_t)src_frames * 2 + 1] = s[0];
                    s += 1;
                } else {
                    src[(size_t)src_frames * 2 + 0] = s[0];
                    src[(size_t)src_frames * 2 + 1] = s[1];
                    s += src_ch;
                }
                src_frames++;
            }
        }
    }
    ov_clear(&vf);
    if (!src || !src_frames) {
        free(src);
        free(pcm);
        return -1;
    }

    for (uint32_t i = 0; i < out_frames; i++) {
        double sp = (double)i * (double)src_rate / (double)OUT_RATE;
        uint32_t s0 = (uint32_t)sp;
        double fr = sp - s0;
        if (s0 >= src_frames - 1) {
            s0 = src_frames - 1;
            fr = 0.0;
        }
        for (int c = 0; c < 2; c++) {
            int a = src[(size_t)s0 * 2 + c];
            int b = src[(size_t)(s0 + (fr > 0 ? 1 : 0)) * 2 + c];
            pcm[(size_t)i * 2 + c] = (int16_t)(a + (b - a) * fr);
        }
    }
    free(src);

    *out_pcm = pcm;
    *out_frames_p = out_frames;
    return 0;
}

/* Installs already-decoded PCM into the cache. Mutates sfx_cache/sfx_voices,
 * so the caller MUST hold audio_mutex. Cheap (no I/O), unlike the decode
 * above. */
static sfx_entry_t *sfx_cache_install(int index, int16_t *pcm, uint32_t frames) {
    int slot = -1;
    for (int i = 0; i < SFX_CACHE_MAX; i++)
        if (!sfx_cache[i].used) {
            slot = i;
            break;
        }
    if (slot < 0) {
        /* Evict least recently used slot; voices referencing it are
         * stopped first to avoid a dangling pointer. */
        uint32_t min_tick = 0xFFFFFFFF;
        int lru_slot = 0;
        for (int i = 0; i < SFX_CACHE_MAX; i++) {
            if (sfx_cache[i].last_used < min_tick) {
                min_tick = sfx_cache[i].last_used;
                lru_slot = i;
            }
        }
        slot = lru_slot;
        for (int v = 0; v < MAX_SFX_VOICES; v++)
            if (sfx_voices[v].active && sfx_voices[v].e == &sfx_cache[slot])
                sfx_voices[v].active = 0;
        free(sfx_cache[slot].pcm);
        sfx_cache[slot].used = 0;
    }
    sfx_cache[slot].used = 1;
    sfx_cache[slot].last_used = ++sfx_tick;
    sfx_cache[slot].index = index;
    sfx_cache[slot].pcm = pcm;
    sfx_cache[slot].frames = frames;
    return &sfx_cache[slot];
}


/* ---------------- Big (streamed) voices ---------------- */

/* Decode buffer per voice, in source frames. Must comfortably cover one
 * mixer tick's worth of source audio (OUT_FRAMES * step) with slack for
 * a source rate close to OUT_RATE; see the fill loop below. */
#define BIG_BUF_CAP (OUT_FRAMES + 64)

typedef struct {
    int active;
    int paused;
    int index;
    int loop;
    float vol;
    OggVorbis_File vf;
    FILE *f;
    int opened;
    long src_rate;
    int src_ch;
    int16_t buf[BIG_BUF_CAP * 2]; /* stereo, source rate, compacted each tick */
    int buf_frames;               /* valid frames held in buf[0..buf_frames) */
    double frac_pos;              /* fractional read position within buf */
    int eof;                      /* stream exhausted, non-looping: drain then stop */
    int pending_open;             /* reserved by audio_play_big(), fopen()/ov_open()
                                     * not done yet -- see the request queue below. */
    unsigned open_gen;            /* Fase 59: bumped on every reserve AND every
                                     * stop, so the mixer thread can tell a
                                     * stale in-flight open (stop arrived
                                     * mid-ov_open) from the request it was
                                     * opened for -- see pending_big_req_t.gen
                                     * and the mixer drain loop. */
} big_voice_t;

static big_voice_t big_voices[MAX_BIG_VOICES];

static void big_close(big_voice_t *v) {
    if (v->opened) {
        ov_clear(&v->vf);
        v->opened = 0;
    }
    v->f = NULL;
    v->active = 0;
    v->paused = 0;
    v->buf_frames = 0;
    v->frac_pos = 0.0;
    v->eof = 0;
    v->pending_open = 0;
}

static int big_open(big_voice_t *v, int index) {
    char path[512];
    snd_path(index, path, sizeof(path));
    if (!path[0])
        return -1;
    FILE *f = fopen(path, "rb");
    if (!f)
        return -1;
    if (ov_open(f, &v->vf, NULL, 0) < 0) {
        fclose(f);
        return -1;
    }
    vorbis_info *vi = ov_info(&v->vf, -1);
    v->src_rate = vi ? vi->rate : 44100;
    v->src_ch = vi ? vi->channels : 1;
    if (v->src_rate <= 0)
        v->src_rate = 44100;
    if (v->src_ch <= 0)
        v->src_ch = 1;
    v->f = f;
    v->opened = 1;
    v->buf_frames = 0;
    v->frac_pos = 0.0;
    v->eof = 0;
    v->index = index;
    return 0;
}

/* ---------------- Deferred load/open request queue ----------------
 *
 * Fase 56 (2026-09-20): real hardware logs (logs/debug_local_053/054/056.log)
 * show "[022] main loop: frame N slow render" spikes of 500 ms to 4+ s
 * landing exactly on frames right after engine ALOG lines for a brand-new
 * SOUNDS-VV PLAYEX, "pad enter-car", or stopRadio/playRadio -- i.e. right
 * when the engine calls Method_playSound()/Method_playSoundBig() (java.c)
 * for an index that was not decoded/opened yet. Those methods run
 * synchronously on whatever thread the engine calls them from, which for
 * this port is the SAME thread that drives GameRenderer_nativeRender() in
 * the main loop (java.c's Method_* handlers are invoked inline from the
 * engine's own JNI call, not from a separate engine thread) -- so the old
 * sfx_decode()/big_open() calls inside audio_play()/audio_play_big(),
 * which do a blocking fopen() + full ogg decode (or ov_open()'s
 * end-of-file bisection for the streamed path) while holding audio_mutex,
 * stalled the render loop itself for their entire duration. This matches
 * the "tirones fuertes" (periodic hitches, not a flat low fps) reported by
 * the user -- a flat fps ceiling was already addressed in Fases 33-53.
 *
 * Fix: audio_play()/audio_load()/audio_play_big() below only take the fast,
 * already-cached/already-opened path synchronously (cheap, no I/O). A
 * cache miss instead pushes a small request here and returns immediately;
 * the actual fopen()/decode happens on the dedicated mixer thread
 * (audio_mix_thread(), core 2 per audio_init()) at the top of its own
 * loop, never blocking the render thread. Worst case, a brand-new sound
 * starts one mixer tick (~OUT_FRAMES/OUT_RATE, ~43 ms) later than before,
 * or drops one mixer output buffer while decoding -- an inaudible-to-minor
 * audio hiccup instead of a multi-second visual freeze. */
#define MAX_PENDING_SFX 8
#define MAX_PENDING_BIG 4

typedef struct {
    int index;
    float volume;
    float pitch;
    int activate; /* 1 = audio_play() (decode + start a voice), 0 =
                   * audio_load() (decode-only preload, no voice) */
} pending_sfx_req_t;

typedef struct {
    int slot;   /* big_voices[] slot already reserved (pending_open=1) by the caller */
    float volume;
    int loop;
    unsigned gen; /* big_voices[slot].open_gen at reserve time -- the mixer
                   * only activates the opened handle if this still matches
                   * (a stop in between bumps open_gen and voids it). */
} pending_big_req_t;

static pending_sfx_req_t pending_sfx[MAX_PENDING_SFX];
static int pending_sfx_count = 0;
static pending_big_req_t pending_big[MAX_PENDING_BIG];
static int pending_big_count = 0;

/* Called with audio_mutex held. Cheap (struct copy only, no I/O): coalesces
 * a repeated request for the same index instead of growing the queue. A
 * later audio_play() (activate=1) upgrades an already-queued audio_load()
 * preload (activate=0) for the same index; a later audio_load() never
 * downgrades one that is already set to play. */
static void enqueue_play_sfx_locked(int index, float volume, float pitch, int activate) {
    for (int i = 0; i < pending_sfx_count; i++) {
        if (pending_sfx[i].index == index) {
            if (activate) {
                pending_sfx[i].volume = volume;
                pending_sfx[i].pitch = pitch;
                pending_sfx[i].activate = 1;
            }
            return;
        }
    }
    if (pending_sfx_count < MAX_PENDING_SFX) {
        pending_sfx[pending_sfx_count].index = index;
        pending_sfx[pending_sfx_count].volume = volume;
        pending_sfx[pending_sfx_count].pitch = pitch;
        pending_sfx[pending_sfx_count].activate = activate;
        pending_sfx_count++;
    }
    /* Queue full: drop. A missed one-shot SFX trigger is far less bad than
     * blocking the render thread; a steady/looping sound will simply be
     * requested again on its next occurrence. */
}

/* Called with audio_mutex held: removes slot's queued open request, if any
 * (used when a slot with a still-pending open gets forcibly reused). */
static void cancel_pending_big_locked(int slot) {
    for (int q = 0; q < pending_big_count; q++) {
        if (pending_big[q].slot == slot) {
            pending_big[q] = pending_big[--pending_big_count];
            return;
        }
    }
}

/* ---------------- Mixer thread ---------------- */

static int16_t mix_buf[OUT_FRAMES * 2];

/* Soft-knee limiter: passes normal levels through untouched and only
 * compresses (never hard-clips) once |s| goes past KNEE, so overlapping
 * voices saturate smoothly instead of producing the harsh square-wave
 * distortion a hard clamp gives. */
static int16_t soft_clip16(int32_t s) {
    const int32_t KNEE = 24000;
    const int32_t HEAD = 32767 - KNEE;
    int32_t a = s < 0 ? -s : s;
    if (a <= KNEE)
        return (int16_t)s;
    int32_t over = a - KNEE;
    int32_t comp = KNEE + (int32_t)((float)HEAD * (float)over / (float)(over + HEAD));
    return (int16_t)(s < 0 ? -comp : comp);
}

static int audio_mix_thread(SceSize argc, void *argv) {
    (void)argc;
    (void)argv;
    while (audio_running) {
        /* Drain deferred load/open requests queued by audio_play()/
         * audio_load()/audio_play_big() (see the request-queue comment
         * above enqueue_play_sfx_locked()). The blocking part -- fopen()
         * plus a full ogg decode, or ov_open()'s end-of-file bisection --
         * runs right here, on this thread/core, UNLOCKED, so it never
         * holds up the render thread or blocks the mixing section below
         * for longer than the cheap dequeue/install copies do. */
        pending_sfx_req_t sfx_reqs[MAX_PENDING_SFX];
        int sfx_req_n;
        int big_slots[MAX_PENDING_BIG];
        float big_vols[MAX_PENDING_BIG];
        int big_loops[MAX_PENDING_BIG];
        unsigned big_gens[MAX_PENDING_BIG];
        int big_req_n;

        sceKernelLockMutex(audio_mutex, 1, NULL);
        sfx_req_n = pending_sfx_count;
        memcpy(sfx_reqs, pending_sfx, sizeof(pending_sfx_req_t) * (size_t)sfx_req_n);
        pending_sfx_count = 0;
        big_req_n = pending_big_count;
        for (int i = 0; i < big_req_n; i++) {
            big_slots[i] = pending_big[i].slot;
            big_vols[i] = pending_big[i].volume;
            big_loops[i] = pending_big[i].loop;
            big_gens[i] = pending_big[i].gen;
        }
        pending_big_count = 0;
        sceKernelUnlockMutex(audio_mutex, 1);

        for (int i = 0; i < sfx_req_n; i++) {
            int16_t *pcm;
            uint32_t frames;
            if (sfx_decode_raw(sfx_reqs[i].index, &pcm, &frames) != 0)
                continue;
            sceKernelLockMutex(audio_mutex, 1, NULL);
            sfx_entry_t *e = sfx_find(sfx_reqs[i].index);
            if (e) {
                /* A concurrent request for the same index was coalesced
                 * (enqueue_play_sfx_locked) or raced in via the fast path
                 * -- someone else's copy already won, drop ours. */
                free(pcm);
            } else {
                e = sfx_cache_install(sfx_reqs[i].index, pcm, frames);
            }
            if (e && sfx_reqs[i].activate) {
                /* audio_play(): actually start a voice. A plain
                 * audio_load() preload (activate=0) only needed the decode
                 * + cache install above -- no voice to start. */
                int slot = -1;
                for (int s = 0; s < MAX_SFX_VOICES; s++)
                    if (!sfx_voices[s].active) { slot = s; break; }
                if (slot < 0) slot = 0;
                sfx_voices[slot].active = 1;
                sfx_voices[slot].paused = 0;
                sfx_voices[slot].e = e;
                sfx_voices[slot].pos_fix = 0;
                sfx_voices[slot].step_fix = (uint32_t)((double)sfx_reqs[i].pitch * 65536.0);
                sfx_voices[slot].vol = sfx_reqs[i].volume;
            }
            sceKernelUnlockMutex(audio_mutex, 1);
        }

        for (int i = 0; i < big_req_n; i++) {
            big_voice_t *v = &big_voices[big_slots[i]];
            /* Fase 59: pre-flight revalidate -- a stop (or a newer play
             * that stole the slot) may have voided this request while it
             * sat queued. Skipping the open entirely then is free. */
            int current = 0;
            sceKernelLockMutex(audio_mutex, 1, NULL);
            current = v->pending_open && v->open_gen == big_gens[i];
            sceKernelUnlockMutex(audio_mutex, 1);
            if (!current)
                continue;
            int ok = big_open(v, v->index) == 0;
            sceKernelLockMutex(audio_mutex, 1, NULL);
            if (v->pending_open && v->open_gen == big_gens[i]) {
                /* Still ours: activate (or drop a failed open). */
                v->pending_open = 0;
                if (ok) {
                    v->vol = big_vols[i];
                    v->loop = big_loops[i] ? 1 : 0;
                    v->paused = 0;
                    v->active = 1;
                }
            } else if (ok) {
                /* A stop (or a newer reserve) landed mid-open: release
                 * the fresh handle WITHOUT touching the slot's current
                 * state -- big_close() would also clear a newer
                 * reservation's pending_open. */
                ov_clear(&v->vf);
                v->opened = 0;
                v->f = NULL;
                if (!v->pending_open) {
                    v->buf_frames = 0;
                    v->frac_pos = 0.0;
                    v->eof = 0;
                }
            }
            sceKernelUnlockMutex(audio_mutex, 1);
        }

        int32_t acc[OUT_FRAMES * 2];
        memset(acc, 0, sizeof(acc));

        sceKernelLockMutex(audio_mutex, 1, NULL);

        if (!audio_blocked) {
            for (int i = 0; i < MAX_SFX_VOICES; i++) {
                sfx_voice_t *v = &sfx_voices[i];
                if (!v->active || v->paused || !v->e || !v->e->pcm)
                    continue;
                float g = v->vol * cat_gain(v->e->index);
                if (g > 1.5f) g = 1.5f;
                for (int n = 0; n < OUT_FRAMES; n++) {
                    uint32_t p = v->pos_fix >> 16;
                    if (p >= v->e->frames) {
                        v->active = 0;
                        break;
                    }
                    acc[n * 2 + 0] += (int32_t)(v->e->pcm[p * 2 + 0] * g);
                    acc[n * 2 + 1] += (int32_t)(v->e->pcm[p * 2 + 1] * g);
                    v->pos_fix += v->step_fix;
                }
            }

            for (int i = 0; i < MAX_BIG_VOICES; i++) {
                big_voice_t *v = &big_voices[i];
                if (!v->active || v->paused || !v->opened)
                    continue;
                float g = v->vol * cat_gain(v->index);
                if (g > 1.5f) g = 1.5f;
                double step = (double)v->src_rate / (double)OUT_RATE;

                /* Top off the per-voice decode buffer, appending after
                 * whatever is already held. Earlier this window was
                 * rebuilt from scratch every tick and any source frames
                 * decoded past the mixed position were thrown away --
                 * ~7-8 frames per 2048-sample buffer, a periodic skip at
                 * the ~23 Hz buffer rate that read back as a harsh buzz
                 * riding on top of the music. Carrying the tail forward
                 * (compacted below) makes the position sample-accurate
                 * across ticks: nothing decoded is ever discarded. */
                uint32_t target = (uint32_t)(v->frac_pos + (double)OUT_FRAMES * step) + 4;
                if (target > BIG_BUF_CAP)
                    target = BIG_BUF_CAP;
                int bitstream = 0;
                int loop_retries = 0;
                static uint8_t braw[8192];
                while (!v->eof && (uint32_t)v->buf_frames < target) {
                    long want = (long)(target - v->buf_frames) * 2L * (long)v->src_ch;
                    if (want > (long)sizeof(braw))
                        want = sizeof(braw);
                    long got = ov_read(&v->vf, (char *)braw, (int)want,
                                       0, 2, 1, &bitstream);
                    if (got <= 0) {
                        if (v->loop && ++loop_retries <= 4) {
                            ov_time_seek(&v->vf, 0.0);
                            continue;
                        }
                        v->eof = 1;
                        break;
                    }
                    long fr = got / (2L * (long)v->src_ch);
                    int16_t *s = (int16_t *)braw;
                    for (long f = 0; f < fr && (uint32_t)v->buf_frames < BIG_BUF_CAP; f++) {
                        if (v->src_ch == 1) {
                            v->buf[(size_t)v->buf_frames * 2 + 0] = s[0];
                            v->buf[(size_t)v->buf_frames * 2 + 1] = s[0];
                            s += 1;
                        } else {
                            v->buf[(size_t)v->buf_frames * 2 + 0] = s[0];
                            v->buf[(size_t)v->buf_frames * 2 + 1] = s[1];
                            s += v->src_ch;
                        }
                        v->buf_frames++;
                    }
                }

                if (v->buf_frames < 2) {
                    if (v->eof && !v->loop)
                        big_close(v);
                    continue;
                }

                double sp = v->frac_pos;
                for (int n = 0; n < OUT_FRAMES; n++) {
                    uint32_t s0 = (uint32_t)sp;
                    if (s0 + 1 >= (uint32_t)v->buf_frames)
                        break; /* caught up with the decoder; resume next tick */
                    double fr = sp - s0;
                    for (int c = 0; c < 2; c++) {
                        int a = v->buf[(size_t)s0 * 2 + c];
                        int b = v->buf[(size_t)(s0 + 1) * 2 + c];
                        int s = (int)(a + (b - a) * fr);
                        acc[n * 2 + c] += (int32_t)(s * g);
                    }
                    sp += step;
                }

                /* Compact: drop whole frames actually consumed, keep the
                 * fractional remainder plus any decoded-but-unconsumed
                 * tail for next tick. */
                int consumed = (int)sp;
                if (consumed > v->buf_frames)
                    consumed = v->buf_frames;
                if (consumed > 0) {
                    memmove(v->buf, v->buf + (size_t)consumed * 2,
                            (size_t)(v->buf_frames - consumed) * 2 * sizeof(int16_t));
                    v->buf_frames -= consumed;
                    v->frac_pos = sp - consumed;
                } else {
                    v->frac_pos = sp;
                }

                if (v->buf_frames == 0 && v->eof && !v->loop)
                    big_close(v);
            }
        }

        sceKernelUnlockMutex(audio_mutex, 1);

        for (int n = 0; n < OUT_FRAMES * 2; n++)
            mix_buf[n] = soft_clip16(acc[n]);
        if (audio_port >= 0)
            sceAudioOutOutput(audio_port, mix_buf);
    }
    return 0;
}

void audio_init(void) {
    if (audio_port >= 0)
        return;
    audio_mutex = sceKernelCreateMutex("gmv_audio", 0, 0, NULL);
    audio_port = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_MAIN, OUT_FRAMES,
                                     OUT_RATE, SCE_AUDIO_OUT_MODE_STEREO);
    if (audio_port < 0) {
        l_error("audio: sceAudioOutOpenPort failed (%d), sound disabled", audio_port);
        audio_port = -1;
        return;
    }
    audio_running = 1;
    audio_thread = sceKernelCreateThread("gmv_audio_mix", audio_mix_thread,
                                         0x10000100, 0x10000, 0, SCE_KERNEL_CPU_MASK_USER_2, NULL);
    if (audio_thread < 0) {
        l_error("audio: mixer thread create failed, sound disabled");
        audio_running = 0;
        sceAudioOutReleasePort(audio_port);
        audio_port = -1;
        return;
    }
    sceKernelStartThread(audio_thread, 0, NULL);
    l_note("[AUDIO] backend up: %d sounds, port %d @%dHz", GMV_SOUND_COUNT, audio_port, OUT_RATE);
}

/* ---------------- Public API ---------------- */

void audio_load(int index) {
    if (audio_port < 0 || index < 0 || index >= GMV_SOUND_COUNT)
        return;
    sceKernelLockMutex(audio_mutex, 1, NULL);
    sfx_entry_t *hit = sfx_find(index);
    if (!hit)
        enqueue_play_sfx_locked(index, 0.0f, 1.0f, 0 /* decode-only, no voice */);
    sceKernelUnlockMutex(audio_mutex, 1);
}

void audio_play(int index, float volume, float pitch) {
    if (audio_port < 0 || index < 0 || index >= GMV_SOUND_COUNT)
        return;
    volume = sane_vol(volume);
    pitch = sane_pitch(pitch);
    static int logged = 0;
    if (!logged) {
        logged = 1;
        l_note("[AUDIO] first play: idx=%d vol=%.2f pitch=%.2f", index, volume, pitch);
    }
    sceKernelLockMutex(audio_mutex, 1, NULL);
    /* Fast path: already decoded and cached -- cheap, no I/O, stays
     * synchronous exactly like before. */
    sfx_entry_t *e = sfx_find(index);
    if (e) {
        /* Steal the first free (or oldest = slot 0) voice. */
        int slot = -1;
        for (int i = 0; i < MAX_SFX_VOICES; i++)
            if (!sfx_voices[i].active) {
                slot = i;
                break;
            }
        if (slot < 0)
            slot = 0;
        sfx_voices[slot].active = 1;
        sfx_voices[slot].paused = 0;
        sfx_voices[slot].e = e;
        sfx_voices[slot].pos_fix = 0;
        sfx_voices[slot].step_fix = (uint32_t)((double)pitch * 65536.0);
        sfx_voices[slot].vol = volume;
        sceKernelUnlockMutex(audio_mutex, 1);
        return;
    }
    /* Cold path (Fase 56): first play of this index -- fopen() + a full
     * ogg decode would otherwise block whatever thread called us (the
     * engine's own JNI call from inside GameRenderer_nativeRender(), see
     * the comment above enqueue_play_sfx_locked()). Defer it to the mixer
     * thread and return immediately; the voice starts up to one mixer
     * tick (~43 ms) later instead of freezing the frame. */
    enqueue_play_sfx_locked(index, volume, pitch, 1 /* start a voice once decoded */);
    sceKernelUnlockMutex(audio_mutex, 1);
}

static void sfx_set_paused(int index, int paused) {
    sceKernelLockMutex(audio_mutex, 1, NULL);
    for (int i = 0; i < MAX_SFX_VOICES; i++)
        if (sfx_voices[i].active && sfx_voices[i].e &&
            sfx_voices[i].e->index == index)
            sfx_voices[i].paused = paused;
    sceKernelUnlockMutex(audio_mutex, 1);
}

static void sfx_stop(int index) {
    sceKernelLockMutex(audio_mutex, 1, NULL);
    for (int i = 0; i < MAX_SFX_VOICES; i++)
        if (sfx_voices[i].active && sfx_voices[i].e &&
            sfx_voices[i].e->index == index)
            sfx_voices[i].active = 0;
    sceKernelUnlockMutex(audio_mutex, 1);
}

void audio_pause_sfx(int index, int instance) {
    (void)instance;
    sfx_set_paused(index, 1);
}

void audio_resume_sfx(int index, int instance) {
    (void)instance;
    sfx_set_paused(index, 0);
}

void audio_stop_sfx(int index, int instance) {
    (void)instance;
    sfx_stop(index);
}

void audio_unload(int index) {
    sceKernelLockMutex(audio_mutex, 1, NULL);
    sfx_stop(index);
    sfx_entry_t *e = sfx_find(index);
    if (e) {
        free(e->pcm);
        e->pcm = NULL;
        e->used = 0;
    }
    sceKernelUnlockMutex(audio_mutex, 1);
}

void audio_set_volume_sfx(int index, int instance, float vol) {
    (void)instance;
    vol = sane_vol(vol);
    sceKernelLockMutex(audio_mutex, 1, NULL);
    for (int i = 0; i < MAX_SFX_VOICES; i++)
        if (sfx_voices[i].active && sfx_voices[i].e &&
            sfx_voices[i].e->index == index)
            sfx_voices[i].vol = vol;
    sceKernelUnlockMutex(audio_mutex, 1);
}

void audio_set_pitch(int index, int instance, float pitch) {
    (void)instance;
    pitch = sane_pitch(pitch);
    sceKernelLockMutex(audio_mutex, 1, NULL);
    for (int i = 0; i < MAX_SFX_VOICES; i++)
        if (sfx_voices[i].active && sfx_voices[i].e &&
            sfx_voices[i].e->index == index)
            sfx_voices[i].step_fix = (uint32_t)((double)pitch * 65536.0);
    sceKernelUnlockMutex(audio_mutex, 1);
}

int audio_is_loaded(int index) {
    if (index < 0 || index >= GMV_SOUND_COUNT)
        return 0;
    sceKernelLockMutex(audio_mutex, 1, NULL);
    int ok = sfx_find(index) ? 1 : 0;
    sceKernelUnlockMutex(audio_mutex, 1);
    if (!ok) {
        ok = snd_exists(index);
    }
    return ok;
}

void audio_load_big(int index) {
    /* Streaming needs no preload; validated lazily on play. */
    (void)index;
}

void audio_play_big(int index, float volume, int loop) {
    if (audio_port < 0 || index < 0 || index >= GMV_SOUND_COUNT)
        return;
    volume = sane_vol(volume);
    static int logged_big = 0;
    if (!logged_big) {
        logged_big = 1;
        l_note("[AUDIO] first play_big: idx=%d vol=%.2f loop=%d", index, volume, loop);
    }
    sceKernelLockMutex(audio_mutex, 1, NULL);
    /* Same index replayed (the per-frame radio re-issue while already
     * playing) is a no-op: this single dedup is what settles the
     * playRadio/stopRadio churn once state reporting is correct. */
    for (int i = 0; i < MAX_BIG_VOICES; i++)
        if (big_voices[i].active && big_voices[i].index == index) {
            big_voices[i].vol = volume;
            big_voices[i].loop = loop ? 1 : 0;
            big_voices[i].paused = 0;
            sceKernelUnlockMutex(audio_mutex, 1);
            return;
        }

    /* Fase 56: an already-open handle for this exact index sitting idle
     * (e.g. previously stopped) can be restarted with just
     * ov_time_seek(0.0), which -- unlike ov_open() -- needs no
     * end-of-file bisection, so this stays cheap/synchronous. */
    for (int i = 0; i < MAX_BIG_VOICES; i++) {
        big_voice_t *v = &big_voices[i];
        if (v->opened && !v->pending_open && v->index == index && !v->active) {
            ov_time_seek(&v->vf, 0.0);
            v->buf_frames = 0;
            v->frac_pos = 0.0;
            v->eof = 0;
            v->vol = volume;
            v->loop = loop ? 1 : 0;
            v->paused = 0;
            v->active = 1;
            sceKernelUnlockMutex(audio_mutex, 1);
            return;
        }
    }

    /* An open request for this exact index is already queued (e.g. two
     * rapid playRadio() calls before the mixer thread caught up) --
     * refresh the params it will start with instead of queuing a second
     * request for the same slot. */
    for (int i = 0; i < MAX_BIG_VOICES; i++) {
        if (big_voices[i].pending_open && big_voices[i].index == index) {
            for (int q = 0; q < pending_big_count; q++)
                if (pending_big[q].slot == i) {
                    pending_big[q].volume = volume;
                    pending_big[q].loop = loop;
                    sceKernelUnlockMutex(audio_mutex, 1);
                    return;
                }
            break; /* reservation without a queue entry shouldn't happen -- fall through and re-reserve */
        }
    }

    /* Cold path (Fase 56, see real hardware logs logs/debug_local_053/
     * 054/056.log, e.g. frame 3145 "playRadio" -> 906 ms slow render):
     * fopen() + ov_open() -- the latter does an end-of-file bisection to
     * find the stream's duration/bitrate -- blocks whatever thread calls
     * us, which for this port is the same thread driving
     * GameRenderer_nativeRender(). Reserve a slot now (cheap, no I/O) and
     * defer the actual open to the mixer thread; see the request-queue
     * comment above enqueue_play_sfx_locked(). */
    int slot = -1;
    for (int i = 0; i < MAX_BIG_VOICES; i++)
        if (!big_voices[i].active && !big_voices[i].pending_open) {
            slot = i;
            break;
        }
    if (slot < 0) {
        /* No idle slot: evict one that is merely active/playing (safe --
         * a big_voice_t is only ever mutated under audio_mutex, except
         * for the mixer thread's big_open() below, which runs UNLOCKED
         * on a slot it marked pending_open). NEVER force-evict a
         * pending_open slot here: the mixer thread may be mid-flight in
         * an unlocked fopen()+ov_open() for that exact slot right now,
         * and closing it out from under that call would race on the same
         * OggVorbis_File/FILE* from two threads. If every slot happens to
         * be pending_open at once (all voices mid-open in the same tick),
         * drop this request instead -- the caller's next attempt (e.g.
         * the next playRadio retry) gets a slot once one finishes. */
        for (int i = 0; i < MAX_BIG_VOICES; i++)
            if (!big_voices[i].pending_open) {
                slot = i;
                break;
            }
        if (slot < 0) {
            sceKernelUnlockMutex(audio_mutex, 1);
            return;
        }
        cancel_pending_big_locked(slot);
        big_close(&big_voices[slot]);
    }
    big_voice_t *v = &big_voices[slot];
    if (v->opened)
        big_close(v); /* release the old ov handle before handing this slot to the mixer thread */
    v->pending_open = 1;
    v->index = index;
    v->open_gen++; /* new reservation epoch -- voids any in-flight open */
    if (pending_big_count < MAX_PENDING_BIG) {
        pending_big[pending_big_count].slot = slot;
        pending_big[pending_big_count].volume = volume;
        pending_big[pending_big_count].loop = loop;
        pending_big[pending_big_count].gen = v->open_gen;
        pending_big_count++;
    } else {
        /* Can't happen in practice (MAX_PENDING_BIG == MAX_BIG_VOICES, at
         * most one request per slot) -- don't leave the slot stuck
         * pending forever if it ever did. */
        v->pending_open = 0;
    }
    sceKernelUnlockMutex(audio_mutex, 1);
}

void audio_pause_big(int index) {
    sceKernelLockMutex(audio_mutex, 1, NULL);
    for (int i = 0; i < MAX_BIG_VOICES; i++)
        if (big_voices[i].active && big_voices[i].index == index)
            big_voices[i].paused = 1;
    sceKernelUnlockMutex(audio_mutex, 1);
}

void audio_resume_big(int index) {
    /* Unpause the matching stream; if it was fully stopped, restart it. */
    int restarted = 0;
    sceKernelLockMutex(audio_mutex, 1, NULL);
    for (int i = 0; i < MAX_BIG_VOICES; i++)
        if (big_voices[i].active && big_voices[i].index == index) {
            big_voices[i].paused = 0;
            restarted = 1;
        }
    sceKernelUnlockMutex(audio_mutex, 1);
    if (!restarted)
        audio_play_big(index, 1.0f, 0);
}

void audio_stop_big(int index) {
    /* Fase 59: a stop also cancels a still-pending open (and voids an
     * in-flight ov_open via the generation bump) -- previously a stop
     * arriving during the deferred-open window was a silent no-op and
     * the stream started anyway right after, which is exactly the
     * stopRadio/playRadio churn the engine falls into when it polls
     * state per frame (log 058 while driving). */
    sceKernelLockMutex(audio_mutex, 1, NULL);
    for (int i = 0; i < MAX_BIG_VOICES; i++) {
        big_voice_t *v = &big_voices[i];
        if (index >= 0 && v->index != index)
            continue;
        if (v->pending_open) {
            cancel_pending_big_locked(i);
            v->pending_open = 0;
            v->open_gen++;
        }
        if (v->active)
            big_close(v);
    }
    sceKernelUnlockMutex(audio_mutex, 1);
}

void audio_unload_big(int index) {
    audio_stop_big(index);
}

void audio_set_volume_big(int index, float vol) {
    vol = sane_vol(vol);
    sceKernelLockMutex(audio_mutex, 1, NULL);
    for (int i = 0; i < MAX_BIG_VOICES; i++)
        if (big_voices[i].active && big_voices[i].index == index)
            big_voices[i].vol = vol;
    sceKernelUnlockMutex(audio_mutex, 1);
}

int audio_is_loaded_big(int index) {
    return snd_exists(index);
}

int audio_is_media_playing(int index) {
    /* Fase 59: a slot with a still-pending open counts as playing.
     * SoundManager::update() polls this every frame and re-issues
     * playRadio/playSound on "not playing" -- reporting 0 during the
     * deferred-open window (~one mixer tick) is what kept the engine
     * re-firing stop+play pairs back-to-back (log 058: dozens of
     * consecutive stopRadio/playRadio lines while driving). The open
     * WILL complete (or be cancelled by a real stop), so answering 1
     * here settles the retry loop instead of feeding it. */
    int playing = 0;
    sceKernelLockMutex(audio_mutex, 1, NULL);
    for (int i = 0; i < MAX_BIG_VOICES; i++)
        if ((big_voices[i].active || big_voices[i].pending_open) && !big_voices[i].paused &&
            (index < 0 || big_voices[i].index == index)) {
            playing = 1;
            break;
        }
    if (!playing) {
        for (int i = 0; i < MAX_SFX_VOICES; i++)
            if (sfx_voices[i].active && !sfx_voices[i].paused &&
                (index < 0 || (sfx_voices[i].e && sfx_voices[i].e->index == index))) {
                playing = 1;
                break;
            }
    }
    sceKernelUnlockMutex(audio_mutex, 1);
    return playing;
}

void audio_stop_all(void) {
    sceKernelLockMutex(audio_mutex, 1, NULL);
    for (int i = 0; i < MAX_SFX_VOICES; i++)
        sfx_voices[i].active = 0;
    for (int i = 0; i < MAX_BIG_VOICES; i++) {
        /* Fase 59: same pending-cancel as audio_stop_big -- a queued or
         * in-flight open must not start playing after a stop-all. */
        if (big_voices[i].pending_open) {
            cancel_pending_big_locked(i);
            big_voices[i].pending_open = 0;
            big_voices[i].open_gen++;
        }
        if (big_voices[i].active)
            big_close(&big_voices[i]);
    }
    sceKernelUnlockMutex(audio_mutex, 1);
}

void audio_pause_all(void) {
    sceKernelLockMutex(audio_mutex, 1, NULL);
    for (int i = 0; i < MAX_SFX_VOICES; i++)
        sfx_voices[i].paused = 1;
    for (int i = 0; i < MAX_BIG_VOICES; i++)
        big_voices[i].paused = 1;
    sceKernelUnlockMutex(audio_mutex, 1);
}

void audio_resume_all(void) {
    sceKernelLockMutex(audio_mutex, 1, NULL);
    for (int i = 0; i < MAX_SFX_VOICES; i++)
        sfx_voices[i].paused = 0;
    for (int i = 0; i < MAX_BIG_VOICES; i++)
        big_voices[i].paused = 0;
    sceKernelUnlockMutex(audio_mutex, 1);
}

void audio_set_gains(float music, float sfx, float vfx) {
    if (music >= 0.0f && music <= 8.0f && music != gain_music) {
        gain_music = music;
        l_note("[AUDIO] gains: music=%.2f sfx=%.2f vfx=%.2f", gain_music, gain_sfx, gain_vfx);
    }
    if (sfx >= 0.0f && sfx <= 8.0f && sfx != gain_sfx) {
        gain_sfx = sfx;
        l_note("[AUDIO] gains: music=%.2f sfx=%.2f vfx=%.2f", gain_music, gain_sfx, gain_vfx);
    }
    if (vfx >= 0.0f && vfx <= 8.0f && vfx != gain_vfx) {
        gain_vfx = vfx;
        l_note("[AUDIO] gains: music=%.2f sfx=%.2f vfx=%.2f", gain_music, gain_sfx, gain_vfx);
    }
}

void audio_block(int blocked) {
    audio_blocked = blocked ? 1 : 0;
}
