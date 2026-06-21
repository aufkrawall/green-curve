// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// platform_win32.cpp — Windows implementations of the out-of-line platform
// shim entry points declared in platform.h.  Header-inline wrappers (dlopen,
// sleep, atomics, mutex, thread, bounded strings) need no .cpp body; only the
// subprocess capture lives here.

#include "platform.h"

#include <stdlib.h>

namespace {

// Append `arg` to a UTF-16 command line with CommandLineToArgvW-compatible
// quoting.  Conservative: always quotes and escapes embedded quotes/trailing
// backslashes so paths with spaces work.
void append_quoted_arg(WCHAR* cmd, size_t cmdCount, const WCHAR* arg) {
    size_t len = 0;
    while (len + 1 < cmdCount && cmd[len]) len++;
    auto put = [&](WCHAR c) {
        if (len + 1 < cmdCount) { cmd[len++] = c; cmd[len] = L'\0'; }
    };
    if (len > 0) put(L' ');
    put(L'"');
    size_t backslashes = 0;
    for (const WCHAR* p = arg; *p; p++) {
        if (*p == L'\\') {
            backslashes++;
        } else if (*p == L'"') {
            for (size_t i = 0; i < backslashes * 2 + 1; i++) put(L'\\');
            backslashes = 0;
            put(L'"');
            continue;
        } else {
            backslashes = 0;
        }
        put(*p);
    }
    for (size_t i = 0; i < backslashes; i++) put(L'\\');
    put(L'"');
}

bool utf8_to_wide(const char* in, WCHAR* out, int outCount) {
    if (!in || !out || outCount <= 0) return false;
    int n = MultiByteToWideChar(CP_UTF8, 0, in, -1, out, outCount);
    return n > 0;
}

} // namespace

bool pl_run_capture(const char* const* argv, char* out, size_t outSize,
                    unsigned int timeoutMs) {
    if (out && outSize > 0) out[0] = '\0';
    if (!argv || !argv[0] || !out || outSize == 0) return false;

    WCHAR appPath[MAX_PATH] = {};
    if (!utf8_to_wide(argv[0], appPath, (int)PL_ARRAY_COUNT(appPath))) return false;

    WCHAR cmd[1024] = {};
    for (int i = 0; argv[i]; i++) {
        WCHAR warg[MAX_PATH] = {};
        if (!utf8_to_wide(argv[i], warg, (int)PL_ARRAY_COUNT(warg))) return false;
        append_quoted_arg(cmd, PL_ARRAY_COUNT(cmd), warg);
    }

    SECURITY_ATTRIBUTES sa = { sizeof(sa), nullptr, TRUE };
    HANDLE hRead = nullptr, hWrite = nullptr;
    if (!CreatePipe(&hRead, &hWrite, &sa, 0)) return false;
    SetHandleInformation(hRead, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdOutput = hWrite;
    si.hStdError = hWrite;

    PROCESS_INFORMATION pi = {};
    if (!CreateProcessW(appPath, cmd, nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
                        nullptr, nullptr, &si, &pi)) {
        CloseHandle(hRead);
        CloseHandle(hWrite);
        return false;
    }
    CloseHandle(hWrite); // child owns the write end now

    size_t totalRead = 0;
    bool timedOut = false;
    ULONGLONG startTickMs = GetTickCount64();
    while (totalRead < outSize - 1) {
        DWORD available = 0;
        if (PeekNamedPipe(hRead, nullptr, 0, nullptr, &available, nullptr) && available > 0) {
            size_t room = outSize - 1 - totalRead;
            DWORD toRead = (available < room) ? available : (DWORD)room;
            DWORD n = 0;
            if (!ReadFile(hRead, out + totalRead, toRead, &n, nullptr) || n == 0) break;
            totalRead += n;
            continue;
        }
        DWORD waitResult = WaitForSingleObject(pi.hProcess, 25);
        if (waitResult == WAIT_OBJECT_0) {
            // Drain any final buffered output before exiting the loop.
            DWORD tail = 0;
            if (PeekNamedPipe(hRead, nullptr, 0, nullptr, &tail, nullptr) && tail > 0) continue;
            break;
        }
        if (GetTickCount64() - startTickMs >= timeoutMs) {
            timedOut = true;
            TerminateProcess(pi.hProcess, 1);
            break;
        }
    }
    out[totalRead] = '\0';

    WaitForSingleObject(pi.hProcess, 1000);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    CloseHandle(hRead);
    return !timedOut;
}
