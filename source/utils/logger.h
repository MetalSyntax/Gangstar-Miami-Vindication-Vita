/*
 * Copyright (C) 2022-2024 Volodymyr Atamanenko
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

/**
 * @file  logger.h
 * @brief Logging utilities.
 */

#ifndef SOLOADER_LOGGER_H
#define SOLOADER_LOGGER_H

#ifdef __cplusplus
extern "C" {
#endif

#define LT_DEBUG   0
#define LT_INFO    1
#define LT_WARN    2
#define LT_ERROR   3
#define LT_FATAL   4
#define LT_SUCCESS 5
#define LT_WAIT    6

#ifdef DEBUG_SOLOADER
#define l_debug(...)   _log_print(LT_DEBUG,   __VA_ARGS__)
#define l_info(...)    _log_print(LT_INFO,    __VA_ARGS__)
#define l_warn(...)    _log_print(LT_WARN,    __VA_ARGS__)
#define l_success(...) _log_print(LT_SUCCESS, __VA_ARGS__)
#define l_wait(...)    _log_print(LT_WAIT,    __VA_ARGS__)
#else
#define l_debug(...)
#define l_info(...)
#define l_warn(...)
#define l_success(...)
#define l_wait(...)
#endif

#define l_error(...)   _log_print(LT_ERROR,   __VA_ARGS__)
#define l_fatal(...)   _log_print(LT_FATAL,   __VA_ARGS__)

/**
 * Always-compiled informational line -- survives a Release build, unlike
 * l_info()/l_debug().
 *
 * Why this tier exists: a Debug build turns on EVERY l_debug() in the project
 * at once, including the per-fread()/fseek() traces in reimpl/io.c, and that
 * volume slows the engine's asset loading down enough to change its behaviour
 * (see port_progress.md Fase 8 -- logging itself was the "infinite black
 * screen"). A Release build has the opposite problem: it compiles out
 * l_debug/l_info/l_warn wholesale, so a run that hangs leaves nothing behind
 * but FalsoJNI's own warnings. `l_note()` is the middle ground, reserved for
 * lines that are BOTH low-volume and high-signal: the engine's own
 * __android_log_*() output, and the main loop's frame heartbeat.
 */
#define l_note(...)    _log_print(LT_INFO,    __VA_ARGS__)

/**
 * Numbered breadcrumb for crash triage: prints "[NNN] <msg>" via l_debug so
 * the last line in the log before a crash pins down exactly which checkpoint
 * was reached. Numbers are a project-wide sequence (001-999, zero-padded),
 * not per-file -- see port_progress.md for which numbers are already taken
 * before adding a new one.
 */
#define l_checkpoint(n, fmt, ...) l_debug("[%03d] " fmt, (n), ##__VA_ARGS__)

void _log_print(int t, const char* fmt, ...)
                __attribute__ ((format (printf, 2, 3)));

/**
 * Forwards an already-formatted line (no color codes needed) to the same
 * UDP debugnet broadcast + per-session local file sinks `_log_print` uses,
 * without going through its color-coded severity switch. Lets OTHER loggers
 * in the project (e.g. FalsoJNI's own `FalsoJNI_Logger.c`, which used to
 * only reach `sceClibPrintf`) show up in `psvita-toolkit logs-live` and the
 * local `debug_local_NNN.log` capture too, instead of being invisible to
 * both.
 */
void l_raw_line(const char *line);

#ifdef __cplusplus
};
#endif

#endif // SOLOADER_LOGGER_H
