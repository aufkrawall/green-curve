// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// platform_win32.cpp — Windows implementations of the out-of-line platform
// shim entry points declared in platform.h.  Header-inline wrappers (dlopen,
// sleep, atomics, mutex, thread, bounded strings) need no .cpp body; only the
// command-line quoting lives here.  There is deliberately no Windows
// subprocess capture: nothing shipped needs one, and a SYSTEM service that
// spawns hidden children with redirected output reads as a remote shell.

#include "platform.h"

#include <stdlib.h>

bool pl_append_quoted_arg_w(WCHAR* cmd, size_t cmdCount, const WCHAR* arg) {
    if (!cmd || cmdCount == 0 || !arg) return false;
    size_t len = 0;
    while (len + 1 < cmdCount && cmd[len]) len++;
    if (len + 1 >= cmdCount && cmd[len]) return false;
    bool ok = true;
    auto put = [&](WCHAR c) {
        if (len + 1 < cmdCount) { cmd[len++] = c; cmd[len] = L'\0'; }
        else ok = false;
    };
    if (len > 0) put(L' ');
    put(L'"');
    size_t backslashes = 0;
    for (const WCHAR* p = arg; *p; p++) {
        if (*p == L'\\') {
            backslashes++;
            continue;
        } else if (*p == L'"') {
            for (size_t i = 0; i < backslashes * 2 + 1; i++) put(L'\\');
            backslashes = 0;
            put(L'"');
            continue;
        } else {
            for (size_t i = 0; i < backslashes; i++) put(L'\\');
            backslashes = 0;
        }
        put(*p);
    }
    for (size_t i = 0; i < backslashes * 2; i++) put(L'\\');
    put(L'"');
    return ok;
}
