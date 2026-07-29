// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// linux_daemon_lifecycle.h — daemon run-state, monotonic fan wake condition,
// and async-signal-safe stop handling.  Included directly by linux_daemon.cpp
// (same shard convention as linux_fan_runtime.h / linux_operation_runtime.h).
//
// Why this exists as its own shard: the daemon previously had no signal
// handling at all, so `systemctl stop` killed it at default disposition and
// the entire teardown path -- including handing the fan back to the driver --
// was unreachable.

#ifndef GREEN_CURVE_LINUX_DAEMON_LIFECYCLE_H
#define GREEN_CURVE_LINUX_DAEMON_LIFECYCLE_H

static unsigned int g_fanFailureCount = 0;
static volatile sig_atomic_t g_running = 1;
static pthread_mutex_t g_fanWakeMutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_fanWakeCondition = PTHREAD_COND_INITIALIZER;
static unsigned long long g_fanWakeGeneration = 0;

// Written by the signal handler, read by the accept loop.  A self-pipe is the
// only async-signal-safe way to break the accept wait without polling or a
// timeout, so shutdown stays event-driven.
static int g_shutdownPipe[2] = {-1, -1};

// PTHREAD_COND_INITIALIZER implies CLOCK_REALTIME.  The fan runtime's poll
// deadline must not be affected by wall-clock steps -- a backwards NTP
// correction would otherwise stall re-assertion while a manual fan duty stays
// pinned -- so the condition variable is bound to CLOCK_MONOTONIC explicitly.
static bool init_fan_wake_condition() {
    pthread_condattr_t attr;
    if (pthread_condattr_init(&attr) != 0) return false;
    bool ok = pthread_condattr_setclock(&attr, CLOCK_MONOTONIC) == 0 &&
              pthread_cond_init(&g_fanWakeCondition, &attr) == 0;
    pthread_condattr_destroy(&attr);
    return ok;
}

static void wake_fan_runtime() {
    pthread_mutex_lock(&g_fanWakeMutex);
    ++g_fanWakeGeneration;
    pthread_cond_broadcast(&g_fanWakeCondition);
    pthread_mutex_unlock(&g_fanWakeMutex);
}

// Async-signal-safe: only a sig_atomic_t store and a one-byte write().
static void on_daemon_stop_signal(int) {
    g_running = 0;
    if (g_shutdownPipe[1] >= 0) {
        const char token = 1;
        ssize_t ignored = write(g_shutdownPipe[1], &token, 1);
        (void)ignored;
    }
}

static bool install_daemon_signal_handlers() {
    if (pipe(g_shutdownPipe) != 0) return false;
    for (int i = 0; i < 2; ++i) {
        int flags = fcntl(g_shutdownPipe[i], F_GETFD);
        if (flags >= 0) fcntl(g_shutdownPipe[i], F_SETFD, flags | FD_CLOEXEC);
    }
    // Never let a full pipe block inside the handler.
    int writeFlags = fcntl(g_shutdownPipe[1], F_GETFL);
    if (writeFlags >= 0)
        fcntl(g_shutdownPipe[1], F_SETFL, writeFlags | O_NONBLOCK);
    struct sigaction stopAction = {};
    stopAction.sa_handler = on_daemon_stop_signal;
    sigemptyset(&stopAction.sa_mask);
    bool ok = sigaction(SIGTERM, &stopAction, nullptr) == 0 &&
              sigaction(SIGINT, &stopAction, nullptr) == 0;
    // SIGHUP would otherwise terminate the daemon at default disposition.
    struct sigaction ignoreAction = {};
    ignoreAction.sa_handler = SIG_IGN;
    sigemptyset(&ignoreAction.sa_mask);
    sigaction(SIGHUP, &ignoreAction, nullptr);
    return ok;
}

static void close_daemon_shutdown_pipe() {
    if (g_shutdownPipe[0] >= 0) close(g_shutdownPipe[0]);
    if (g_shutdownPipe[1] >= 0) close(g_shutdownPipe[1]);
    g_shutdownPipe[0] = -1;
    g_shutdownPipe[1] = -1;
}

#endif // GREEN_CURVE_LINUX_DAEMON_LIFECYCLE_H
