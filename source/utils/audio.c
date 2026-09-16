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
        char path[512];
        snd_path(index, path, sizeof(path));
        FILE *f = path[0] ? fopen(path, "rb") : NULL;
        if (f) {
            fclose(f);
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

/* Decode whole ogg to stereo OUT_RATE PCM. Returns entry or NULL. */
static sfx_entry_t *sfx_decode(int index) {
    sfx_entry_t *hit = sfx_find(index);
    if (hit)
        return hit;

    char path[512];
    snd_path(index, path, sizeof(path));
    if (!path[0])
        return NULL;

    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    OggVorbis_File vf;
    if (ov_open(f, &vf, NULL, 0) < 0) {
        fclose(f);
        return NULL;
    }
    vorbis_info *vi = ov_info(&vf, -1);
    long src_rate = vi ? vi->rate : 44100;
    int src_ch = vi ? vi->channels : 1;
    double total = ov_time_total(&vf, -1);
    if (!(total > 0.0) || total > 30.0) {
        ov_clear(&vf);
        return NULL;
    }
    uint32_t out_frames = (uint32_t)(total * OUT_RATE) + 1;
    if ((uint64_t)out_frames * 4 > SFX_DECODE_CAP) {
        ov_clear(&vf);
        return NULL;
    }
    int16_t *pcm = calloc(out_frames, 4);
    if (!pcm) {
        ov_clear(&vf);
        return NULL;
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
        return NULL;
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
    sfx_cache[slot].frames = out_frames;
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
    sfx_decode(index);
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
    sfx_entry_t *e = sfx_decode(index);
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
    }
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
    int slot = -1;
    for (int i = 0; i < MAX_BIG_VOICES; i++)
        if (!big_voices[i].active) {
            slot = i;
            break;
        }
    if (slot < 0) {
        big_close(&big_voices[0]);
        slot = 0;
    }
    big_voice_t *v = &big_voices[slot];
    if (v->opened && v->index != index)
        big_close(v);
    if (!v->opened && big_open(v, index) < 0) {
        sceKernelUnlockMutex(audio_mutex, 1);
        return;
    }
    if (v->index == index && v->opened && v->active && v->paused) {
        /* Resume same stream. */
        v->paused = 0;
        v->vol = volume;
        v->loop = loop ? 1 : 0;
    } else {
        if (v->opened)
            ov_time_seek(&v->vf, 0.0);
        v->buf_frames = 0;
        v->frac_pos = 0.0;
        v->eof = 0;
        v->vol = volume;
        v->loop = loop ? 1 : 0;
        v->paused = 0;
        v->active = 1;
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
    sceKernelLockMutex(audio_mutex, 1, NULL);
    for (int i = 0; i < MAX_BIG_VOICES; i++)
        if (big_voices[i].active &&
            (index < 0 || big_voices[i].index == index))
            big_close(&big_voices[i]);
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
    int playing = 0;
    sceKernelLockMutex(audio_mutex, 1, NULL);
    for (int i = 0; i < MAX_BIG_VOICES; i++)
        if (big_voices[i].active && !big_voices[i].paused &&
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
    for (int i = 0; i < MAX_BIG_VOICES; i++)
        if (big_voices[i].active)
            big_close(&big_voices[i]);
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
