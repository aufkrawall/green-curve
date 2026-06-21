// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// platform_posix.cpp — Linux implementations of the out-of-line platform shim
// entry points declared in platform.h.  Only subprocess capture needs an
// out-of-line body; the rest of the shim is header-inline.

#include "platform.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <time.h>

extern char** environ;

static unsigned long long monotonic_ms() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long long)ts.tv_sec * 1000ull + (unsigned long long)(ts.tv_nsec / 1000000l);
}

bool pl_run_capture(const char* const* argv, char* out, size_t outSize,
                    unsigned int timeoutMs) {
    if (out && outSize > 0) out[0] = '\0';
    if (!argv || !argv[0] || !out || outSize == 0) return false;

    int pipefd[2];
    if (pipe(pipefd) != 0) return false;

    // Child writes stdout/stderr to pipefd[1]; redirect both via spawn actions.
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, pipefd[1], STDOUT_FILENO);
    posix_spawn_file_actions_adddup2(&fa, pipefd[1], STDERR_FILENO);
    posix_spawn_file_actions_addclose(&fa, pipefd[0]);
    posix_spawn_file_actions_addclose(&fa, pipefd[1]);

    pid_t pid = 0;
    // posix_spawnp searches PATH for argv[0]; callers pass an absolute path for
    // trusted execution, but PATH search is convenient for plain "nvidia-smi".
    int rc = posix_spawnp(&pid, argv[0], &fa, nullptr,
                          const_cast<char* const*>(argv), environ);
    posix_spawn_file_actions_destroy(&fa);
    close(pipefd[1]); // parent only reads

    if (rc != 0) {
        close(pipefd[0]);
        return false;
    }

    int flags = fcntl(pipefd[0], F_GETFL, 0);
    fcntl(pipefd[0], F_SETFL, flags | O_NONBLOCK);

    size_t totalRead = 0;
    bool timedOut = false;
    unsigned long long startMs = monotonic_ms();
    while (totalRead < outSize - 1) {
        unsigned long long elapsed = monotonic_ms() - startMs;
        if (elapsed >= timeoutMs) { timedOut = true; break; }

        struct pollfd pfd;
        pfd.fd = pipefd[0];
        pfd.events = POLLIN;
        int waitMs = (int)(timeoutMs - elapsed);
        if (waitMs > 100) waitMs = 100; // re-check the deadline at least every 100ms
        int pr = poll(&pfd, 1, waitMs);
        if (pr < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (pr == 0) continue; // timeout slice; loop re-checks the deadline
        if (pfd.revents & (POLLIN | POLLHUP)) {
            ssize_t n = read(pipefd[0], out + totalRead, outSize - 1 - totalRead);
            if (n > 0) {
                totalRead += (size_t)n;
                continue;
            }
            if (n == 0) break; // EOF: child closed stdout
            if (errno == EAGAIN || errno == EINTR) continue;
            break;
        }
    }
    out[totalRead] = '\0';
    close(pipefd[0]);

    if (timedOut) {
        kill(pid, SIGKILL);
    }
    // Reap the child so it does not linger as a zombie.
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    return !timedOut;
}
