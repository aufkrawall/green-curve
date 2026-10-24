// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// What the POSIX fixtures need from the machine, decided in one place so a
// restricted runner (a CI container or an agent sandbox) gets either a working
// fixture or a precise refusal, never an opaque failure code.
#ifndef GREEN_CURVE_POSIX_TEST_ENVIRONMENT_H
#define GREEN_CURVE_POSIX_TEST_ENVIRONMENT_H

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

// Exit status of a fixture whose environment refuses something the fixture
// cannot run without. tools/security_gates.py reports it as blocked, keeps
// running the other stages, and still fails the run: it is never a pass.
#define GC_TEST_EXIT_ENVIRONMENT_BLOCKED 120

// Scratch files go under $TMPDIR when it names an absolute directory:
// sandboxed and CI runners point it at their only writable temp area, while
// /tmp itself may be read-only there. Without it, /tmp.
static inline const char* gc_test_temp_base() {
    const char* base = getenv("TMPDIR");
    return base && base[0] == '/' ? base : "/tmp";
}

// mkdtemp() under the temp base as `<base>/<prefix>-XXXXXX`, leaving at least
// `reserve` bytes of `out` free for names joined later (a Unix socket path
// must fit sockaddr_un.sun_path). A $TMPDIR too long for that falls back to
// /tmp rather than failing the fixture on path length. Returns `out` or null.
static inline char* gc_test_make_temp_dir(char* out, size_t outSize,
                                          const char* prefix, size_t reserve) {
    if (!out || outSize == 0 || !prefix) return nullptr;
    const char* bases[] = {gc_test_temp_base(), "/tmp"};
    for (const char* base : bases) {
        int n = snprintf(out, outSize, "%s/%s-XXXXXX", base, prefix);
        if (n > 0 && (size_t)n + reserve < outSize) return mkdtemp(out);
    }
    out[0] = '\0';
    return nullptr;
}

// Whether this process may create a Unix-domain socket at all. A seccomp
// sandbox can refuse socket(AF_UNIX) with EPERM/EACCES while still allowing
// socketpair(); a named listener cannot be tested there. Any other error is
// left to the fixture, which reports it as a real failure.
static inline bool gc_test_unix_sockets_refused(int* errnoOut) {
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd >= 0) {
        close(fd);
        return false;
    }
    if (errnoOut) *errnoOut = errno;
    return errno == EPERM || errno == EACCES;
}

#endif
