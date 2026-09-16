#ifndef SOLOADER_VIDEO_H
#define SOLOADER_VIDEO_H

#ifdef __cplusplus
extern "C" {
#endif

// Loads the AVPLAYER sysmodule. Call once, after gl_init() (video_play()'s
// texture allocator maps memory via sceGxmMapMemory, which needs the GXM
// context vitaGL's init brings up).
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
