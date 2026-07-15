// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// platform_win32.cpp — Windows implementations of the out-of-line platform
// shim entry points declared in platform.h.  Header-inline wrappers (dlopen,
// sleep, atomics, mutex, thread, bounded strings) need no .cpp body; only the
// subprocess capture lives here.

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

namespace {

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
        if (!pl_append_quoted_arg_w(cmd, PL_ARRAY_COUNT(cmd), warg)) return false;
    }

    SECURITY_ATTRIBUTES sa = { sizeof(sa), nullptr, TRUE };
    HANDLE hRead = nullptr, hWrite = nullptr;
    if (!CreatePipe(&hRead, &hWrite, &sa, 0)) return false;
    if (!SetHandleInformation(hRead, HANDLE_FLAG_INHERIT, 0)) {
        CloseHandle(hRead);
        CloseHandle(hWrite);
        return false;
    }

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
    bool ioOk = true;
    bool processExited = false;
    bool pipeClosed = false;
    ULONGLONG startTickMs = GetTickCount64();
    for (;;) {
        DWORD available = 0;
        if (!pipeClosed && !PeekNamedPipe(hRead, nullptr, 0, nullptr,
                &available, nullptr)) {
            if (GetLastError() == ERROR_BROKEN_PIPE) pipeClosed = true;
            else { ioOk = false; break; }
        }
        if (!pipeClosed && available > 0) {
            size_t room = outSize - 1 - totalRead;
            char discard[4096];
            char* target = room > 0 ? out + totalRead : discard;
            size_t targetSize = room > 0 ? room : sizeof(discard);
            if (targetSize > MAXDWORD) targetSize = MAXDWORD;
            DWORD toRead = available < targetSize ? available : (DWORD)targetSize;
            DWORD n = 0;
            if (!ReadFile(hRead, target, toRead, &n, nullptr) || n == 0) {
                ioOk = false;
                break;
            }
            if (room > 0) totalRead += n;
            continue;
        }

        DWORD waitResult = WaitForSingleObject(pi.hProcess, 0);
        if (waitResult == WAIT_OBJECT_0) {
            processExited = true;
            if (!pipeClosed && PeekNamedPipe(hRead, nullptr, 0, nullptr,
                    &available, nullptr) && available > 0) continue;
            break;
        }
        if (waitResult == WAIT_FAILED) { ioOk = false; break; }

        ULONGLONG elapsedMs = GetTickCount64() - startTickMs;
        if (elapsedMs >= timeoutMs) {
            timedOut = true;
            break;
        }
        DWORD waitMs = (DWORD)(timeoutMs - elapsedMs);
        if (waitMs > 25) waitMs = 25;
        if (WaitForSingleObject(pi.hProcess, waitMs) == WAIT_FAILED) {
            ioOk = false;
            break;
        }
    }
    out[totalRead] = '\0';

    if (!processExited) {
        TerminateProcess(pi.hProcess, 1);
        processExited = WaitForSingleObject(pi.hProcess, INFINITE) == WAIT_OBJECT_0;
    }
    DWORD exitCode = STILL_ACTIVE;
    bool cleanExit = processExited && GetExitCodeProcess(pi.hProcess, &exitCode) &&
        exitCode == 0;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    CloseHandle(hRead);
    return !timedOut && ioOk && cleanExit;
}
