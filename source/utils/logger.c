/*
 * Copyright (C) 2022-2024 Volodymyr Atamanenko
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#include "utils/logger.h"

#include <psp2/kernel/clib.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/net/net.h>
#include <psp2/net/netctl.h>
#include <psp2/sysmodule.h>
#include <psp2/io/fcntl.h>

#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdatomic.h>

#define COLOR_RED    "\x1B[38;5;196m"
#define COLOR_PINK   "\x1B[38;5;212m"
#define COLOR_ORANGE "\x1B[38;5;202m"
#define COLOR_BLUE   "\x1B[38;5;32m"
#define COLOR_GREEN  "\x1B[32m"
#define COLOR_CYAN   "\x1B[36m"

#define COLOR_END    "\033[0m"

static SceKernelLwMutexWork _log_mutex;
static atomic_bool _log_mutex_ready = ATOMIC_VAR_INIT(false);

// debugnet-style UDP log broadcaster, so `psvita-toolkit logs-live` can show
// this output on the dev machine without an FTP round-trip -- useful for a
// crash/freeze that never gets far enough to flush anything to a log file.
// Wire format expected by the toolkit's listener: one UTF-8 text line per UDP
// datagram, no framing (see debugnet_server.md in the toolkit repo). Attempted
// once, lazily, from inside the already-taken `_log_mutex` critical section so
// its setup can't race with itself; if the network stack or a socket can't be
// brought up (no Wi-Fi, etc.) logging silently falls back to sceClibPrintf-only,
// exactly like before this was added.
#define DEBUGNET_PORT 9999
static char _debugnet_net_buf[128 * 1024];
static int _debugnet_sock = -1;
static SceNetSockaddrIn _debugnet_addr;
static bool _debugnet_init_done = false;

static void _debugnet_init(void) {
    _debugnet_init_done = true;

    // sceNetInit/sceNetCtlInit are import stubs that only resolve to real
    // syscalls once the SceNet sysmodule is actually loaded -- without this,
    // the stub jumps through an unresolved NID and the thread takes a
    // Prefetch abort at PC=0x0 right after the `blx sceNetInit` (confirmed in
    // gangstarmiamivindication-psp2core-1788275718, the very first attempt to
    // add debugnet logging).
    if (sceSysmoduleLoadModule(SCE_SYSMODULE_NET) < 0) return;

    SceNetInitParam net_init_param;
    net_init_param.memory = _debugnet_net_buf;
    net_init_param.size = sizeof(_debugnet_net_buf);
    net_init_param.flags = 0;
    if (sceNetInit(&net_init_param) < 0) return;
    sceNetCtlInit();

    _debugnet_sock = sceNetSocket("debugnet", SCE_NET_AF_INET, SCE_NET_SOCK_DGRAM, 0);
    if (_debugnet_sock < 0) return;

    int broadcast = 1;
    sceNetSetsockopt(_debugnet_sock, SCE_NET_SOL_SOCKET, SCE_NET_SO_BROADCAST,
                      &broadcast, sizeof(broadcast));

    memset(&_debugnet_addr, 0, sizeof(_debugnet_addr));
    _debugnet_addr.sin_family = SCE_NET_AF_INET;
    _debugnet_addr.sin_port = sceNetHtons(DEBUGNET_PORT);
    _debugnet_addr.sin_addr.s_addr = SCE_NET_INADDR_BROADCAST;
}

static void _debugnet_send(const char *line) {
    if (_debugnet_sock < 0) return;
    sceNetSendto(_debugnet_sock, line, strlen(line), 0,
                 (SceNetSockaddr *) &_debugnet_addr, sizeof(SceNetSockaddrIn));
}

// UDP is inherently lossy, and the SINGLE most important line for triaging a
// crash -- the very last checkpoint before it -- is exactly the one most at
// risk of never reaching debugnet_server.py's listener (sent right before
// the process dies, no retransmit, no ack). This mirrors every line to a
// local file too via a synchronous open+write+close per call (slow, but this
// is temporary crash-triage instrumentation, not the steady-state hot path)
// so the last few lines survive even if their UDP datagrams were dropped or
// never sent in time.
//
// One file per session (debug_local_001.log, _002.log, ... wrapping back to
// _001.log after _999.log) instead of a single path everyone overwrites --
// a counter file (debug_local_counter.txt) tracks the last-used number
// across boots so consecutive sessions never collide and old sessions'
// captures stay comparable side by side.
#define LOCAL_LOG_DIR "ux0:data/gangstarmiamivindication/logs"
#define LOCAL_LOG_COUNTER_PATH LOCAL_LOG_DIR "/debug_local_counter.txt"

static char _localfile_path[128];
static bool _localfile_init_done = false;

static void _localfile_init(void) {
    _localfile_init_done = true;

    int session = 1;
    SceUID fd = sceIoOpen(LOCAL_LOG_COUNTER_PATH, SCE_O_RDONLY, 0);
    if (fd >= 0) {
        char buf[8];
        int n = sceIoRead(fd, buf, sizeof(buf) - 1);
        sceIoClose(fd);
        if (n > 0) {
            buf[n] = '\0';
            session = (int) strtoul(buf, NULL, 10) + 1;
        }
    }
    if (session < 1 || session > 999) session = 1;

    sceClibSnprintf(_localfile_path, sizeof(_localfile_path),
                     LOCAL_LOG_DIR "/debug_local_%03d.log", session);

    char counter_buf[8];
    int len = sceClibSnprintf(counter_buf, sizeof(counter_buf), "%d", session);
    fd = sceIoOpen(LOCAL_LOG_COUNTER_PATH, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
    if (fd >= 0) {
        sceIoWrite(fd, counter_buf, len);
        sceIoClose(fd);
    }
}

static void _localfile_send(const char *line) {
    if (!_localfile_init_done) {
        _localfile_init();
    }
    SceUID fd = sceIoOpen(_localfile_path, SCE_O_WRONLY | SCE_O_APPEND | SCE_O_CREAT, 0777);
    if (fd < 0) return;
    sceIoWrite(fd, line, strlen(line));
    sceIoClose(fd);
}

// Buffer A is used to adjust the format string.
static char buffer_a[2048];
// Buffer B is used to compile the final log using the updated format string.
static char buffer_b[2048];

void l_raw_line(const char *line) {
    if (!atomic_load_explicit(&_log_mutex_ready, memory_order_relaxed)) {
        int ret = sceKernelCreateLwMutex(&_log_mutex, "log_lock", 0, 0, NULL);
        if (ret < 0) return;
        atomic_store_explicit(&_log_mutex_ready, true, memory_order_relaxed);
    }
    sceKernelLockLwMutex(&_log_mutex, 1, NULL);

    if (!_debugnet_init_done) {
        _debugnet_init();
    }

    _debugnet_send(line);
    _localfile_send(line);

    sceKernelUnlockLwMutex(&_log_mutex, 1);
}

void _log_print(int t, const char* fmt, ...) {
    if (!atomic_load_explicit(&_log_mutex_ready, memory_order_relaxed)) {
        int ret = sceKernelCreateLwMutex(&_log_mutex, "log_lock", 0, 0, NULL);
        if (ret < 0) {
            sceClibPrintf("Error: failed to create log mutex: 0x%x\n", ret);
            return;
        }
        atomic_store_explicit(&_log_mutex_ready, true, memory_order_relaxed);
    }
    sceKernelLockLwMutex(&_log_mutex, 1, NULL);

    if (!_debugnet_init_done) {
        _debugnet_init();
    }

    switch (t) {
        case LT_DEBUG:
            sceClibSnprintf(buffer_a, sizeof(buffer_a), " %s• debug%s    %s\n",
                            COLOR_PINK, COLOR_END, fmt); break;
        case LT_INFO:
            sceClibSnprintf(buffer_a, sizeof(buffer_a), " %sℹ info%s     %s\n",
                            COLOR_BLUE, COLOR_END, fmt); break;
        case LT_WARN:
            sceClibSnprintf(buffer_a, sizeof(buffer_a), " %s⚠ warning%s  %s\n",
                            COLOR_ORANGE, COLOR_END, fmt); break;
        case LT_ERROR:
            sceClibSnprintf(buffer_a, sizeof(buffer_a), " %s⨯ error%s    %s\n",
                            COLOR_RED, COLOR_END, fmt); break;
        case LT_FATAL:
            sceClibSnprintf(buffer_a, sizeof(buffer_a), " %s! fatal%s    %s\n",
                            COLOR_RED, COLOR_END, fmt); break;
        case LT_SUCCESS:
            sceClibSnprintf(buffer_a, sizeof(buffer_a), " %s! success%s  %s\n",
                            COLOR_GREEN, COLOR_END, fmt); break;
        case LT_WAIT:
            sceClibSnprintf(buffer_a, sizeof(buffer_a), " %s… waiting%s  %s\n",
                            COLOR_CYAN, COLOR_END, fmt); break;
        default:
            if (atomic_load_explicit(&_log_mutex_ready, memory_order_relaxed)) {
                sceKernelUnlockLwMutex(&_log_mutex, 1);
            }
            return;
    }

    va_list list;
    va_start(list, fmt);
    sceClibVsnprintf(buffer_b, sizeof(buffer_b), buffer_a, list);
    va_end(list);
    sceClibPrintf(buffer_b);
    _debugnet_send(buffer_b);
    _localfile_send(buffer_b);

    if (atomic_load_explicit(&_log_mutex_ready, memory_order_relaxed)) {
        sceKernelUnlockLwMutex(&_log_mutex, 1);
    }
}
