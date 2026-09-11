/*
 * Minimal audio backend for Gangstar Miami Vindication (PS Vita).
 *
 * Why this exists (confirmed 2026-09-11, see port_progress.md Fase 31):
 * the engine drives ALL audio through the GLMediaPlayer Java methods and
 * then polls `isMediaPlaying(index)` every frame from SoundManager::update().
 * The old stubs accepted every call and always reported "not playing", so
 * the engine re-issued playRadio/playSound on nearly every frame -- each
 * retry paying operator new[] + sprintf + appDebugLog + a JNI round-trip.
 * That churn is the dominant cost behind the reported 10-13 fps AND the
 * reason for total silence. A backend that really plays and reports state
 * settles the loop and produces sound with one change.
 *
 * Design (kept small on purpose):
 * - SceAudioOut MAIN port, 48000 Hz stereo, mixer thread waking per buffer.
 * - Short SFX (SoundPool path, playSound): fully decoded to 48 kHz stereo
 *   PCM on first use, cached (LRU-ish, capped), mixed on up to 8 voices
 *   with volume + pitch (resample step).
 * - Music/voice/radio (MediaPlayer path, playSoundBig): streamed with
 *   libvorbisfile straight from DATA_PATH "data/", up to 4 concurrent
 *   streams, with loop support (radio tracks loop).
 * - All state queries (isLoaded / isPlaying) are answered from this
 *   backend, so the engine stops retrying.
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

void audio_init(void);

/* SoundPool path (short SFX). */
void audio_load(int index);
void audio_play(int index, float volume, float pitch);
void audio_pause_sfx(int index, int instance);
void audio_resume_sfx(int index, int instance);
void audio_stop_sfx(int index, int instance);
void audio_unload(int index);
void audio_set_volume_sfx(int index, int instance, float vol);
void audio_set_pitch(int index, int instance, float pitch);
int audio_is_loaded(int index);

/* MediaPlayer path (music / voice / radio). */
void audio_load_big(int index);
void audio_play_big(int index, float volume, int loop);
void audio_pause_big(int index);
void audio_resume_big(int index);
void audio_stop_big(int index);
void audio_unload_big(int index);
void audio_set_volume_big(int index, float vol);
int audio_is_loaded_big(int index);
int audio_is_media_playing(int index);

/* Global controls. */
void audio_stop_all(void);
void audio_pause_all(void);
void audio_resume_all(void);
void audio_set_gains(float music, float sfx, float vfx);
void audio_block(int blocked);

#ifdef __cplusplus
}
#endif
