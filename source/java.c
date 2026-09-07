#include <falso_jni/FalsoJNI_Impl.h>
#include <falso_jni/FalsoJNI_Logger.h>
#include <falso_jni/FalsoJNI.h>

#include <stdio.h>
#include <string.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>

#include <so_util/so_util.h>

#include "utils/logger.h"

extern so_module so_mod;

/*
 * Resource loading
 *
 * The engine asks for paths exactly as they appear in the .so's string table
 * ("./Achievements.gmap", "./Res.array", "./miami.bdae", ...). On Android
 * GLResLoader.getResourceFull()/getResourceBytes()/getResourceLength()
 * normalized those (stripping a leading "./" or ".//", then trimming) and
 * looked them up in three places, in order: a "res_<name>" drawable resource,
 * the APK's assets, and /sdcard/gameloft/games/gangster2/<path>.
 *
 * This APK ships no assets/ directory at all -- the toolkit extracted the game
 * data to DATA_PATH "data/" (3214 files: Achievements.gmap, miami.bdae, ...)
 * and the res_* drawables to DATA_PATH "res/drawable/". So the same lookup
 * order collapses to: DATA_PATH "data/<path>" first, then DATA_PATH "<path>"
 * for anything that already carries its own subdirectory.
 */

static void res_normalize_path(const char *in, char *out, size_t out_size) {
    if (!in) {
        out[0] = '\0';
        return;
    }

    // Strip the "./" or ".//" prefix the engine puts on every path.
    if (strncmp(in, ".//", 3) == 0) in += 3;
    else if (strncmp(in, "./", 2) == 0) in += 2;

    // Trim leading whitespace (Java's String.trim()).
    while (*in == ' ' || *in == '\t') in++;

    snprintf(out, out_size, "%s", in);

    // Trim trailing whitespace.
    size_t len = strlen(out);
    while (len > 0 && (out[len - 1] == ' ' || out[len - 1] == '\t' ||
                       out[len - 1] == '\r' || out[len - 1] == '\n')) {
        out[--len] = '\0';
    }
}

// Opens a normalized resource path, trying DATA_PATH "data/" first (where the
// bulk of the game data lives) and then DATA_PATH itself. Returns a valid fd
// and fills `size`, or a negative value if the resource does not exist.
static SceUID res_open(const char *raw_path, SceOff *size) {
    char rel[512];
    char full[640];

    res_normalize_path(raw_path, rel, sizeof(rel));
    if (rel[0] == '\0') return -1;

    static const char *prefixes[] = { DATA_PATH "data/", DATA_PATH };

    for (int i = 0; i < 2; i++) {
        snprintf(full, sizeof(full), "%s%s", prefixes[i], rel);
        SceUID fd = sceIoOpen(full, SCE_O_RDONLY, 0);
        if (fd >= 0) {
            *size = sceIoLseek(fd, 0, SCE_SEEK_END);
            sceIoLseek(fd, 0, SCE_SEEK_SET);
            return fd;
        }
    }

    return -1;
}

static jobject res_read_chunk(const char *raw_path, int offset, int length) {
    SceOff size = 0;
    SceUID fd = res_open(raw_path, &size);
    if (fd < 0) {
        fjni_logv_warn("[JNI] resource not found: \"%s\"", raw_path ? raw_path : "(null)");
        return NULL;
    }

    if (length < 0) length = 0;
    if (offset > 0) sceIoLseek(fd, offset, SCE_SEEK_SET);

    JavaDynArray *jda = jda_alloc(length, FIELD_TYPE_BYTE);
    if (!jda) {
        fjni_logv_err("[JNI] resource \"%s\": jda_alloc(%i) failed", raw_path, length);
        sceIoClose(fd);
        return NULL;
    }

    if (length > 0) sceIoRead(fd, jda->array, length);
    sceIoClose(fd);

    return jda;
}

// getResourceFull(String path) -> byte[]
jobject Method_getResourceFull(jmethodID id, va_list args) {
    JavaString *js = va_arg(args, JavaString *);
    const char *path = (js && js->utf8) ? (const char *) js->utf8->array : NULL;

    SceOff size = 0;
    SceUID fd = res_open(path, &size);
    if (fd < 0) {
        fjni_logv_warn("[JNI] getResourceFull(\"%s\"): not found", path ? path : "(null)");
        return NULL;
    }
    sceIoClose(fd);

    fjni_logv_dbg("[JNI] getResourceFull(\"%s\"): %i bytes", path, (int) size);
    return res_read_chunk(path, 0, (int) size);
}

// getResourceBytes(String path, int offset, int length) -> byte[]
jobject Method_getResourceBytes(jmethodID id, va_list args) {
    JavaString *js = va_arg(args, JavaString *);
    int offset = va_arg(args, int);
    int length = va_arg(args, int);
    const char *path = (js && js->utf8) ? (const char *) js->utf8->array : NULL;

    fjni_logv_dbg("[JNI] getResourceBytes(\"%s\", %i, %i)", path ? path : "(null)", offset, length);
    return res_read_chunk(path, offset, length);
}

// getResourceLength(String path) -> int
jint Method_getResourceLength(jmethodID id, va_list args) {
    JavaString *js = va_arg(args, JavaString *);
    const char *path = (js && js->utf8) ? (const char *) js->utf8->array : NULL;

    SceOff size = 0;
    SceUID fd = res_open(path, &size);
    if (fd < 0) {
        fjni_logv_warn("[JNI] getResourceLength(\"%s\"): not found", path ? path : "(null)");
        return 0;
    }
    sceIoClose(fd);

    fjni_logv_dbg("[JNI] getResourceLength(\"%s\"): %i", path, (int) size);
    return (jint) size;
}

/*
 * Audio
 *
 * The engine drives Android's SoundPool/MediaPlayer through these. Audio isn't
 * wired up yet (see PORTING_PLAN.md section 4), so they're accepted and
 * ignored -- the engine only ever checks the two isSoundLoaded* queries, and
 * answering "not loaded" keeps it from waiting on a sound that will never
 * arrive.
 */
void Method_soundVoidStub(jmethodID id, va_list args) {}
jint Method_soundNotLoaded(jmethodID id, va_list args) { return 0; }

// setMusicGain(float) / setSfxGain(float) / setVfxGain(float).
//
// These are `private static void` in GLMediaPlayer.java, but the .so does NOT
// invoke them through CallStaticVoidMethod: nativeSetMusicGain/SfxGain/VfxGain
// all dispatch through JNIEnv offset 0x204, i.e. CallStaticIntMethod (see the
// Ghidra pseudo-C -- `(**(code **)(iVar4 + 0x204))(piVar1, uVar2, setMusicGain,
// ..., uVar5)`). Registering them only as METHOD_TYPE_VOID left methodIntCall()
// with nothing to find, hence the "[WARN] method ID 27/28/29 not found!" lines
// that were the ONLY output of the 2026-09-05 release run. The native wrappers
// return void and discard the result, so the -1 was harmless -- but the noise
// was the last thing standing between us and a clean log.
//
// Auditing every JNI dispatch site in the binary (0x1c4 GetStaticMethodID x57,
// 0x1c8 CallStaticObjectMethod x5, 0x204 CallStaticIntMethod x18, 0x234
// CallStaticVoidMethod x34) confirms these three were the only gap: every other
// method is invoked through the variant its table entry already covers.
jint Method_soundGainStub(jmethodID id, va_list args) { return 0; }

/*
 * Device / system queries
 */

// getDeviceWidth() -> int. GameGLSurfaceView.mDevice_W on Android; the Vita's
// screen width, matching GAME_W in main.c.
jint Method_getDeviceWidth(jmethodID id, va_list args) { return 960; }

// GetDeviceType() -> int. Android branched on Build.MANUFACTURER (motorola=1,
// samsung=2, htc=3, motorola Droid/Milestone=4) and returned -1 for anything
// else -- which is what the Vita is.
jint Method_GetDeviceType(jmethodID id, va_list args) { return -1; }

// GetDeviceSoundType() -> int. AudioManager.getRingerMode();
// RINGER_MODE_NORMAL == 2, i.e. sound enabled.
jint Method_GetDeviceSoundType(jmethodID id, va_list args) { return 2; }

// detectPhoneLang() -> int. GLMediaPlayer maps ISO3 language codes to indices,
// with English == 0.
jint Method_detectPhoneLang(jmethodID id, va_list args) { return 0; }

// isWifiAlive() -> int. No connectivity is wired up, so report "no Wi-Fi"
// rather than claiming a connection the engine could then try to use.
jint Method_isWifiAlive(jmethodID id, va_list args) { return 0; }

// unlockDemo() -> int. Android set playMode=1 and returned 0 regardless.
jint Method_unlockDemo(jmethodID id, va_list args) { return 0; }

// DisableLaunchGame() -> int. Returns 1 only after 5 launches of the demo
// nag counter; with no persisted counter there's nothing to disable.
jint Method_DisableLaunchGame(jmethodID id, va_list args) { return 0; }

// isMediaPlaying(int) -> int. No MediaPlayer, so nothing is ever playing.
jint Method_isMediaPlaying(jmethodID id, va_list args) { return 0; }

// loadMovie(String) -> int. Video playback isn't ported (no real Android
// Activity/VideoView here) -- but the real Java side always returns 1 (see
// GLMediaPlayer.java) and, whether the clip plays to completion or the user
// hits skip, MyVideoView eventually calls the native
// Java_..._MyVideoView_nativeSetOnVideoCompletion() export exactly once to
// hand control back to the engine (that's what it was waiting for while the
// screen stayed black -- confirmed against the log, the engine got stuck
// polling GetDeviceSoundType right after this call with no completion ever
// arriving). Standing in for the whole video system: return 1 and fire that
// same completion callback immediately, as if every clip finishes instantly.
jint Method_loadMovie(jmethodID id, va_list args) {
    JavaString *js = va_arg(args, JavaString *);
    const char *name = (js && js->utf8) ? (const char *) js->utf8->array : "(null)";
    fjni_logv_info("[JNI] loadMovie(\"%s\"): not implemented, skipping straight to completion", name);

    static void (*nativeSetOnVideoCompletion)(JNIEnv *, jclass) = NULL;
    static int resolved = 0;
    if (!resolved) {
        resolved = 1;
        nativeSetOnVideoCompletion = (void (*)(JNIEnv *, jclass))
                so_symbol(&so_mod, "Java_com_gameloft_android_TBFV_GloftGMHP_ML_MyVideoView_nativeSetOnVideoCompletion");
        if (!nativeSetOnVideoCompletion) {
            fjni_log_err("[JNI] loadMovie: could not resolve nativeSetOnVideoCompletion");
        }
    }
    if (nativeSetOnVideoCompletion) {
        nativeSetOnVideoCompletion(&jni, NULL);
    }

    return 1;
}

/*
 * Lifecycle / UI actions with no Vita equivalent
 */
void Method_voidStub(jmethodID id, va_list args) {}

/*
 * Verizon in-app-purchase / carrier-network SDK
 *
 * Dead code on this build -- there's no carrier billing to talk to. The
 * queries report "idle, no error, not ready" so the engine's state machine
 * settles instead of polling forever, and the three ()[B getters hand back an
 * empty (but non-NULL) byte array, since the native side reads the result
 * without a NULL check.
 */
jint Method_VZIsErrorOcurred(jmethodID id, va_list args) { return 0; }
jint Method_VZIsInProgress(jmethodID id, va_list args) { return 0; }
jint Method_VZIsMobileNetworkReady(jmethodID id, va_list args) { return 0; }

jobject Method_VZEmptyString(jmethodID id, va_list args) {
    JavaDynArray *jda = jda_alloc(1, FIELD_TYPE_BYTE);
    if (!jda) return NULL;
    ((char *) jda->array)[0] = '\0';
    return jda;
}

/*
 * JNI Methods
*/

NameToMethodID nameToMethodId[] = {
        // Resources
        { 1, "getResourceFull", METHOD_TYPE_OBJECT },
        { 2, "getResourceBytes", METHOD_TYPE_OBJECT },
        { 3, "getResourceLength", METHOD_TYPE_INT },

        // Audio (accepted, ignored)
        { 4, "loadSound", METHOD_TYPE_VOID },
        { 5, "loadSoundBig", METHOD_TYPE_VOID },
        { 6, "unloadSound", METHOD_TYPE_VOID },
        { 7, "unloadSoundBig", METHOD_TYPE_VOID },
        { 8, "playSound", METHOD_TYPE_VOID },
        { 9, "playSoundBig", METHOD_TYPE_VOID },
        { 10, "pauseSound", METHOD_TYPE_VOID },
        { 11, "pauseSoundBig", METHOD_TYPE_VOID },
        { 12, "resumeSound", METHOD_TYPE_VOID },
        { 13, "resumeSoundBig", METHOD_TYPE_VOID },
        { 14, "stopSound", METHOD_TYPE_VOID },
        { 15, "stopSoundBig", METHOD_TYPE_VOID },
        { 16, "setVolume", METHOD_TYPE_VOID },
        { 17, "setVolumeBig", METHOD_TYPE_VOID },
        { 18, "resetSound", METHOD_TYPE_VOID },
        { 19, "setPitch", METHOD_TYPE_VOID },
        { 20, "stopAllSounds", METHOD_TYPE_VOID },
        { 21, "pauseAllSounds", METHOD_TYPE_VOID },
        { 22, "resumeAllSounds", METHOD_TYPE_VOID },
        { 23, "stopAllPool", METHOD_TYPE_VOID },
        { 24, "stopAllBig", METHOD_TYPE_VOID },
        { 25, "destroySoundPool", METHOD_TYPE_VOID },
        { 26, "initSoundPoolArray", METHOD_TYPE_VOID },
        // Declared `void` in Java, but the .so calls them via CallStaticIntMethod
        // -- see Method_soundGainStub(). They are listed in BOTH methodsInt and
        // methodsVoid so either dispatch path resolves.
        { 27, "setMusicGain", METHOD_TYPE_INT },
        { 28, "setSfxGain", METHOD_TYPE_INT },
        { 29, "setVfxGain", METHOD_TYPE_INT },
        { 30, "isSoundLoaded", METHOD_TYPE_INT },
        { 31, "isSoundLoadedBig", METHOD_TYPE_INT },
        { 32, "isMediaPlaying", METHOD_TYPE_INT },

        // Device / system
        { 33, "getDeviceWidth", METHOD_TYPE_INT },
        { 34, "GetDeviceType", METHOD_TYPE_INT },
        { 35, "GetDeviceSoundType", METHOD_TYPE_INT },
        { 36, "detectPhoneLang", METHOD_TYPE_INT },
        { 37, "isWifiAlive", METHOD_TYPE_INT },
        { 38, "unlockDemo", METHOD_TYPE_INT },
        { 39, "DisableLaunchGame", METHOD_TYPE_INT },

        // Lifecycle / UI
        { 40, "lockDemo", METHOD_TYPE_VOID },
        { 41, "IncreaseLaunchTimes", METHOD_TYPE_VOID },
        { 42, "Exit", METHOD_TYPE_VOID },
        { 43, "openBrowser", METHOD_TYPE_VOID },
        { 44, "sendAppToBackground", METHOD_TYPE_VOID },
        { 45, "NotifyTrophy", METHOD_TYPE_VOID },
        { 46, "OpenGLive", METHOD_TYPE_VOID },
        { 47, "loadMovie", METHOD_TYPE_INT },

        // Verizon carrier SDK
        { 48, "VZInitMobileNetwork", METHOD_TYPE_VOID },
        { 49, "VZRequestLogin", METHOD_TYPE_VOID },
        { 50, "VZRequestPurchaseGame", METHOD_TYPE_VOID },
        { 51, "VZRestoreNetworkState", METHOD_TYPE_VOID },
        { 52, "VZIsErrorOcurred", METHOD_TYPE_INT },
        { 53, "VZIsInProgress", METHOD_TYPE_INT },
        { 54, "VZIsMobileNetworkReady", METHOD_TYPE_INT },
        { 55, "VZGetGameName", METHOD_TYPE_OBJECT },
        { 56, "VZGetGamePrice", METHOD_TYPE_OBJECT },
        { 57, "VZGetLastServerMsg", METHOD_TYPE_OBJECT },
};

MethodsBoolean methodsBoolean[] = {};
MethodsByte methodsByte[] = {};
MethodsChar methodsChar[] = {};
MethodsDouble methodsDouble[] = {};
MethodsFloat methodsFloat[] = {};

MethodsInt methodsInt[] = {
        { 3, Method_getResourceLength },
        { 27, Method_soundGainStub },
        { 28, Method_soundGainStub },
        { 29, Method_soundGainStub },
        { 30, Method_soundNotLoaded },
        { 31, Method_soundNotLoaded },
        { 32, Method_isMediaPlaying },
        { 33, Method_getDeviceWidth },
        { 34, Method_GetDeviceType },
        { 35, Method_GetDeviceSoundType },
        { 36, Method_detectPhoneLang },
        { 37, Method_isWifiAlive },
        { 38, Method_unlockDemo },
        { 39, Method_DisableLaunchGame },
        { 47, Method_loadMovie },
        { 52, Method_VZIsErrorOcurred },
        { 53, Method_VZIsInProgress },
        { 54, Method_VZIsMobileNetworkReady },
};

MethodsLong methodsLong[] = {};

MethodsObject methodsObject[] = {
        { 1, Method_getResourceFull },
        { 2, Method_getResourceBytes },
        { 55, Method_VZEmptyString },
        { 56, Method_VZEmptyString },
        { 57, Method_VZEmptyString },
};

MethodsShort methodsShort[] = {};

MethodsVoid methodsVoid[] = {
        { 4, Method_soundVoidStub },
        { 5, Method_soundVoidStub },
        { 6, Method_soundVoidStub },
        { 7, Method_soundVoidStub },
        { 8, Method_soundVoidStub },
        { 9, Method_soundVoidStub },
        { 10, Method_soundVoidStub },
        { 11, Method_soundVoidStub },
        { 12, Method_soundVoidStub },
        { 13, Method_soundVoidStub },
        { 14, Method_soundVoidStub },
        { 15, Method_soundVoidStub },
        { 16, Method_soundVoidStub },
        { 17, Method_soundVoidStub },
        { 18, Method_soundVoidStub },
        { 19, Method_soundVoidStub },
        { 20, Method_soundVoidStub },
        { 21, Method_soundVoidStub },
        { 22, Method_soundVoidStub },
        { 23, Method_soundVoidStub },
        { 24, Method_soundVoidStub },
        { 25, Method_soundVoidStub },
        { 26, Method_soundVoidStub },
        { 27, Method_soundVoidStub },
        { 28, Method_soundVoidStub },
        { 29, Method_soundVoidStub },
        { 40, Method_voidStub },
        { 41, Method_voidStub },
        { 42, Method_voidStub },
        { 43, Method_voidStub },
        { 44, Method_voidStub },
        { 45, Method_voidStub },
        { 46, Method_voidStub },
        { 48, Method_voidStub },
        { 49, Method_voidStub },
        { 50, Method_voidStub },
        { 51, Method_voidStub },
};

/*
 * JNI Fields
*/

// System-wide constant that applications sometimes request
// https://developer.android.com/reference/android/content/Context.html#WINDOW_SERVICE
char WINDOW_SERVICE[] = "window";

// System-wide constant that's often used to determine Android version
// https://developer.android.com/reference/android/os/Build.VERSION.html#SDK_INT
// Possible values: https://developer.android.com/reference/android/os/Build.VERSION_CODES
const int SDK_INT = 19; // Android 4.4 / KitKat

NameToFieldID nameToFieldId[] = {
		{ 0, "WINDOW_SERVICE", FIELD_TYPE_OBJECT },
		{ 1, "SDK_INT", FIELD_TYPE_INT },
};

FieldsBoolean fieldsBoolean[] = {};
FieldsByte fieldsByte[] = {};
FieldsChar fieldsChar[] = {};
FieldsDouble fieldsDouble[] = {};
FieldsFloat fieldsFloat[] = {};
FieldsInt fieldsInt[] = {
		{ 1, SDK_INT },
};
FieldsObject fieldsObject[] = {
		{ 0, WINDOW_SERVICE },
};
FieldsLong fieldsLong[] = {};
FieldsShort fieldsShort[] = {};

__FALSOJNI_IMPL_CONTAINER_SIZES
