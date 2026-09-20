#ifndef SOLOADER_VIDEO_H
#define SOLOADER_VIDEO_H

#ifdef __cplusplus
extern "C" {
#endif

// Prepares the software (FFmpeg) video/audio decoder and mutexes. Call once,
// after gl_init() -- video_play() creates a GL texture for frame output,
// which needs the GXM context vitaGL's init brings up. No Vita sysmodule is
// loaded here: cutscene decode is done entirely in software (see
// video.cpp's file header for why -- the Vita's hardware decoder only
// supports H.264, and this game's intro.m4v is MPEG-4 Part 2).
void video_init(void);

void video_shutdown(void);

// Plays a cutscene fullscreen, blocking the calling thread until the video
// ends naturally, the user skips it (Cross/Start), or it fails to open/init
// -- always returns, never hangs, so Method_loadMovie (java.c) can fire
// nativeSetOnVideoCompletion() unconditionally right after this call. `name`
// is the bare filename the engine passed to loadMovie() (e.g. "intro.m4v");
// video_play() resolves it against DATA_PATH itself.
void video_play(const char *name);

#ifdef __cplusplus
}
#endif

#endif // SOLOADER_VIDEO_H
