/*
 * Copyright (C) 2021      Andy Nguyen
 * Copyright (C) 2022      Rinnegatamante
 * Copyright (C) 2022-2024 Volodymyr Atamanenko
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#include "reimpl/log.h"
#include "utils/logger.h"
#include <psp2/kernel/clib.h>
#include <stdlib.h>
#include <string.h>

/*
 * Load-spam filter (2026-09-06): these engine lines are per-allocation /
 * per-texture noise, not signal -- in logs/debug_local_021.log they are
 * ~500 of ~1100 lines (324x `---------------locale/basic_ios`, 95x
 * `createTextureImpl`, plus `Loaded texture`/`CTexture::mapImpl`). Every
 * l_note() costs a LwMutex + 2x snprintf + a blocking sceNetSendto + a
 * sceIoWrite (+ periodic sceIoSyncByFd), so this spam directly slows the
 * loading path it is supposed to observe (same mechanism as Fase 8).
 *
 * They go through l_debug(): visible in Debug builds, compiled out in
 * Release (smaller log + faster load). Everything else keeps its tier, so
 * version/driver/mismatch/unbound/duplicate lines still survive release.
 */
static int is_load_spam(const char *tag, const char *text) {
    if (!text) return 0;
    // NOTE: tags are "GameLoft", "GameLoft Printer::log/log2/logf", "Gameloft"
    // (census in debug_local_022.log) -- exact-match on "GameLoft" missed 69x
    // "Loaded texture" and most of the `---------------` noise, which ride on
    // the Printer::log* tags. Prefix-match instead; the text checks below are
    // still exact, so signal lines on the same tags are unaffected.
    if (tag && (strncmp(tag, "GameLoft", 8) == 0 || strncmp(tag, "Gameloft", 8) == 0)) {
        if (strncmp(text, "---------------", 15) == 0) return 1;
        if (strcmp(text, "createTextureImpl 1") == 0) return 1;
        if (strcmp(text, "Loaded texture") == 0) return 1;
        if (strcmp(text, "CTexture::mapImpl") == 0) return 1;
    }
    return 0;
}

/*
 * The engine's own log is the single best description of what it is doing --
 * "Driver informations:", "createTextureImpl", the glitch device/scene
 * messages -- and it is low volume (a few dozen lines for a whole boot). It
 * used to go through l_info()/l_warn(), which a Release build compiles out
 * entirely, so a release run that hung left no trace of how far it got. INFO
 * and WARN now use the always-compiled l_note()/l_error() tier instead; only
 * the engine's own DEBUG/VERBOSE chatter stays behind DEBUG_SOLOADER.
 */
#define print_common \
    switch (prio) { \
        case ANDROID_LOG_INFO: \
            if (is_load_spam(tag, text)) \
                l_debug("[ALOG][%s] %s", tag, text); \
            else \
                l_note("[ALOG][%s] %s", tag, text); \
            break; \
        case ANDROID_LOG_WARN: \
            if (is_load_spam(tag, text)) \
                l_debug("[ALOG][WARN][%s] %s", tag, text); \
            else \
                l_note("[ALOG][WARN][%s] %s", tag, text); \
            break; \
        case ANDROID_LOG_ERROR: \
        case ANDROID_LOG_FATAL: \
            l_error("[ALOG][%s] %s", tag, text); \
            break; \
        case ANDROID_LOG_UNKNOWN: \
        case ANDROID_LOG_DEFAULT: \
        case ANDROID_LOG_VERBOSE: \
        case ANDROID_LOG_DEBUG: \
        case ANDROID_LOG_SILENT: \
        default: \
            l_debug("[ALOG][%s] %s", tag, text); \
            break; \
    }

int __android_log_write(int prio, const char* tag, const char* text) {
    print_common
    return 0;
}

int __android_log_print(int prio, const char* tag, const char* fmt, ...) {
    va_list list;
    char text[1024];

    va_start(list, fmt);
    sceClibVsnprintf(text, sizeof(text), fmt, list);
    va_end(list);

    print_common

    return 0;
}

int __android_log_vprint(int prio, const char* tag, const char* fmt, va_list ap) {
    char text[1024];

    sceClibVsnprintf(text, sizeof(text), fmt, ap);

    print_common

    return 0;
}

void __android_log_assert(const char* cond, const char* tag, const char* fmt, ...) {
    if (fmt) {
        va_list list;
        char text[1024];

        va_start(list, fmt);
        sceClibVsnprintf(text, sizeof(text), fmt, list);
        va_end(list);

        l_fatal("[ALOG][ASSERT] %s", text);
    } else {
        if (cond) {
            l_fatal("[ALOG][ASSERT] Assertion failed: %s", cond);
        } else {
            l_fatal("[ALOG][ASSERT] Unspecified assertion failed");
        }
    }

    abort();
}
