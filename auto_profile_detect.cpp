// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// auto_profile_detect.cpp — Win32 foreground/process/fullscreen probing for the
// auto-profile subsystem.  Strictly read-only observation that NEVER touches
// another process: GetForegroundWindow + GetWindowText/GetClassName for the
// focused window, and a CreateToolhelp32Snapshot of the global process list to
// map the foreground PID -> exe base name and to check focus-optional rule
// presence.  It does NOT open a handle to another process, does NOT read/write
// their memory or threads, and installs NO in-process hook (the foreground
// WinEvent hook is OUTOFCONTEXT — notification only).  Plain passive observation
// with standard APIs, no stealth/evasion — so it cannot trip anti-cheat.  Locked
// by the F-NO-INJECT build guard.

#include "app_shared.h"
#include "auto_profile.h"
#include <tlhelp32.h>

static void ap_wide_to_acp(const WCHAR* w, char* out, size_t outSize) {
    if (!out || outSize == 0) return;
    out[0] = 0;
    if (!w) return;
    WideCharToMultiByte(CP_ACP, 0, w, -1, out, (int)outSize, nullptr, nullptr);
    out[outSize - 1] = 0;
}

static void ap_base_name_acp(const WCHAR* fullPath, char* out, size_t outSize) {
    const WCHAR* base = fullPath;
    for (const WCHAR* p = fullPath; *p; ++p) {
        if (*p == L'\\' || *p == L'/') base = p + 1;
    }
    ap_wide_to_acp(base, out, outSize);
}

// Resolve a PID's exe base name from the global process-list snapshot.  This
// deliberately avoids opening a handle to another process (e.g. a game guarded
// by anti-cheat); it also works for elevated processes that an unelevated GUI
// could not have opened a handle to.
static void ap_exe_name_for_pid(DWORD pid, char* out, size_t outSize) {
    if (out && outSize) out[0] = 0;
    if (!pid) return;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    PROCESSENTRY32W pe;
    pe.dwSize = sizeof(pe);
    if (Process32FirstW(snap, &pe)) {
        do {
            if (pe.th32ProcessID == pid) {
                ap_base_name_acp(pe.szExeFile, out, outSize);   // szExeFile is already a base name
                break;
            }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
}

// Borderless / exclusive fullscreen: the window rect covers the whole monitor.
static bool ap_window_is_fullscreen(HWND hwnd) {
    RECT wr;
    if (!GetWindowRect(hwnd, &wr)) return false;
    HMONITOR mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi;
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfo(mon, &mi)) return false;
    return wr.left <= mi.rcMonitor.left && wr.top <= mi.rcMonitor.top &&
           wr.right >= mi.rcMonitor.right && wr.bottom >= mi.rcMonitor.bottom;
}

bool auto_profile_get_foreground_info(HWND selfWnd, ForegroundInfo* out) {
    if (!out) return false;
    memset(out, 0, sizeof(*out));

    HWND fg = GetForegroundWindow();
    if (!fg || fg == selfWnd) return false;

    WCHAR clsW[128] = {};
    GetClassNameW(fg, clsW, (int)ARRAY_COUNT(clsW) - 1);
    char cls[AUTO_PROFILE_CLASS_MAX] = {};
    ap_wide_to_acp(clsW, cls, sizeof(cls));
    // The desktop/shell counts as "no app foreground" — never a match target.
    if (streqi_ascii(cls, "Progman") || streqi_ascii(cls, "WorkerW") ||
        streqi_ascii(cls, "Shell_TrayWnd")) {
        return false;
    }

    out->valid = true;
    StringCchCopyA(out->className, sizeof(out->className), cls);

    WCHAR titleW[256] = {};
    GetWindowTextW(fg, titleW, (int)ARRAY_COUNT(titleW) - 1);
    ap_wide_to_acp(titleW, out->title, sizeof(out->title));

    // Resolve the exe base name from the process-list snapshot — never by opening
    // a handle to the game process.  exeName may stay empty if the process
    // vanished between the foreground query and the snapshot; title/class/
    // fullscreen rules still match in that case.
    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    if (pid) ap_exe_name_for_pid(pid, out->exeName, sizeof(out->exeName));

    out->isFullscreen = ap_window_is_fullscreen(fg);
    return true;
}

void auto_profile_compute_presence(const AutoProfileConfig* cfg, ProcessPresence* out) {
    if (!out) return;
    memset(out, 0, sizeof(*out));
    if (!cfg) return;

    // Only enumerate processes when at least one focus-optional exe rule needs
    // presence — the common (focus-required) case pays nothing.
    bool needScan = false;
    int n = cfg->ruleCount;
    if (n > AUTO_PROFILE_MAX_RULES) n = AUTO_PROFILE_MAX_RULES;
    for (int i = 0; i < n; i++) {
        const AutoProfileRule* r = &cfg->rules[i];
        if (r->matchType == AUTO_MATCH_EXE && !r->requireFocus && r->pattern[0]) needScan = true;
    }
    if (!needScan) return;

    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    PROCESSENTRY32W pe;
    pe.dwSize = sizeof(pe);
    if (Process32FirstW(snap, &pe)) {
        do {
            char exe[AUTO_PROFILE_PATTERN_MAX] = {};
            ap_base_name_acp(pe.szExeFile, exe, sizeof(exe));
            for (int i = 0; i < n; i++) {
                const AutoProfileRule* r = &cfg->rules[i];
                if (r->matchType == AUTO_MATCH_EXE && !r->requireFocus && r->pattern[0] &&
                    streqi_ascii(exe, r->pattern)) {
                    out->rulePresent[i] = true;
                }
            }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
}
