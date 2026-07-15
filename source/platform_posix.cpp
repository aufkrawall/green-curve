// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// platform_posix.cpp — Linux implementations of the out-of-line platform shim
// entry points declared in platform.h.  Only subprocess capture needs an
// out-of-line body; the rest of the shim is header-inline.

#include "platform.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
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
    bool ioOk = true;
    unsigned long long startMs = monotonic_ms();
    for (;;) {
        unsigned long long elapsed = monotonic_ms() - startMs;
        if (elapsed >= timeoutMs) { timedOut = true; break; }

        struct pollfd pfd = {};
        pfd.fd = pipefd[0];
        pfd.events = POLLIN;
        unsigned long long remainingMs = timeoutMs - elapsed;
        int waitMs = remainingMs > INT_MAX ? INT_MAX : (int)remainingMs;
        int pr = poll(&pfd, 1, waitMs);
        if (pr < 0) {
            if (errno == EINTR) continue;
            ioOk = false;
            break;
        }
        if (pr == 0) { timedOut = true; break; }
        if (pfd.revents & (POLLIN | POLLHUP)) {
            size_t room = outSize - 1 - totalRead;
            char discard[4096];
            char* target = room > 0 ? out + totalRead : discard;
            size_t targetSize = room > 0 && room < sizeof(discard)
                ? room : sizeof(discard);
            ssize_t n = read(pipefd[0], target, targetSize);
            if (n > 0) {
                if (room > 0) totalRead += (size_t)n;
                continue;
            }
            if (n == 0) break; // EOF: child closed stdout
            if (errno == EAGAIN || errno == EINTR) continue;
            ioOk = false;
            break;
        }
        if (pfd.revents & (POLLERR | POLLNVAL)) { ioOk = false; break; }
    }
    out[totalRead] = '\0';
    close(pipefd[0]);

    if (timedOut) {
        kill(pid, SIGKILL);
    }
    // Reap the child so it does not linger as a zombie.
    int status = 0;
    pid_t waited = 0;
    do { waited = waitpid(pid, &status, 0); } while (waited < 0 && errno == EINTR);
    return !timedOut && ioOk && waited == pid && WIFEXITED(status) &&
        WEXITSTATUS(status) == 0;
}
