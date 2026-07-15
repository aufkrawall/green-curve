// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT

#include "app_shared.h"

int g_dpi = 96;
float g_scale = 1.0f;
CRITICAL_SECTION g_configLock = {};
CRITICAL_SECTION g_appLock = {};

AppData g_app = {};
NvmlApi g_nvml_api = {};
HMODULE g_nvml = nullptr;
bool g_debug_logging = false;
bool g_bestGuessWarningShownThisSession = false;
bool g_guiForceFullRefresh = false;

static HMODULE load_system_library_local_a(const char* name) {
    if (!name || !name[0] || strchr(name, '\\') || strchr(name, '/')) return nullptr;
    char systemDir[MAX_PATH] = {};
    UINT systemLen = gc_GetSystemDirectoryUtf8(systemDir, ARRAY_COUNT(systemDir));
    if (systemLen == 0 || systemLen >= ARRAY_COUNT(systemDir)) return nullptr;
    char path[MAX_PATH] = {};
    if (FAILED(StringCchPrintfA(path, ARRAY_COUNT(path), "%s\\%s", systemDir, name))) return nullptr;
    return LoadLibraryA(path);
}

int nvmin(int a, int b) {
    return a < b ? a : b;
}

int nvmax(int a, int b) {
    return a > b ? a : b;
}

int dp(int px) {
    return (int)((float)px * g_scale);
}

void enable_best_process_dpi_awareness() {
    HMODULE user32 = GetModuleHandleA("user32.dll");
    if (user32) {
        typedef BOOL (WINAPI *SetProcessDpiAwarenessContext_t)(HANDLE);
        auto setContext = (SetProcessDpiAwarenessContext_t)GetProcAddress(
            user32, "SetProcessDpiAwarenessContext");
        // DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 is intentionally expressed
        // as its documented pseudo-handle so older SDK/import libraries remain
        // usable. The manifest requests the same mode; this call also covers
        // unusual launchers that discard or override manifest awareness.
        if (setContext && setContext((HANDLE)(INT_PTR)-4)) return;
    }
    SetProcessDPIAware();
}

void init_dpi() {
    InitializeCriticalSection(&g_configLock);
    InitializeCriticalSection(&g_appLock);

    HMODULE user32 = GetModuleHandleA("user32.dll");
    if (user32) {
        typedef UINT (WINAPI *GetDpiForSystem_t)();
        auto pGetDpiForSystem = (GetDpiForSystem_t)GetProcAddress(user32, "GetDpiForSystem");
        if (pGetDpiForSystem) {
            UINT dpi = pGetDpiForSystem();
            if (dpi > 0) {
                g_dpi = (int)dpi;
                g_scale = (float)g_dpi / 96.0f;
                return;
            }
        }
    }

    HMODULE shcore = load_system_library_local_a("shcore.dll");
    if (shcore) {
        typedef HRESULT (WINAPI *GetDpiForMonitor_t)(HANDLE, int, UINT*, UINT*);
        auto pGetDpiForMonitor = (GetDpiForMonitor_t)GetProcAddress(shcore, "GetDpiForMonitor");
        if (pGetDpiForMonitor) {
            UINT dpiX = 96;
            UINT dpiY = 96;
            if (pGetDpiForMonitor(nullptr, 0, &dpiX, &dpiY) == 0 && dpiX > 0) {
                g_dpi = (int)dpiX;
            }
        }
        FreeLibrary(shcore);
    }

    if (g_dpi == 96) {
        HDC hdc = GetDC(nullptr);
        if (hdc) {
            int dpi = GetDeviceCaps(hdc, LOGPIXELSX);
            if (dpi > 0) g_dpi = dpi;
            ReleaseDC(nullptr, hdc);
        }
    }

    if (g_dpi <= 0) g_dpi = 96;
    g_scale = (float)g_dpi / 96.0f;
}

// Pure policy decision shared by every logon auto-apply path (client + service).
// See app_shared.h for the contract.  Kept free of any I/O so it is exhaustively
// unit-testable by build.py's regression harness.
LogonProfileSource resolve_logon_profile_source(bool policyActive, bool isAdmin,
    int logonSharedSlot, bool bankSlotSaved, int logonUserSlot,
    bool hasPerUserSlot, bool hasMachineDefault) {
    // 1) An explicit, currently-published user choice of a shared bank profile
    //    always wins: it is the admin's authoritative copy, so it is valid for
    //    everyone and safe under the shared-only policy.
    if (logonSharedSlot > 0) {
        return bankSlotSaved
            ? LOGON_PROFILE_SOURCE_SHARED_BANK
            : LOGON_PROFILE_SOURCE_PENDING;
    }

    // 2) A restricted user (policy on AND not a machine admin) may NEVER have
    //    their own per-user custom OC applied at logon.  They get only the
    //    machine-wide shared default (authoritative bank copy), if any.  This is
    //    what closes the service-router bypass.
    if (policyActive && !isAdmin) {
        if (hasMachineDefault) return LOGON_PROFILE_SOURCE_MACHINE_DEFAULT;
        return LOGON_PROFILE_SOURCE_NONE;
    }

    // 3) Admins / unrestricted machines: per-user logon slot first, then the
    //    machine-wide default as a fallback (unchanged legacy behavior).
    if (logonUserSlot > 0) {
        return hasPerUserSlot
            ? LOGON_PROFILE_SOURCE_PER_USER
            : LOGON_PROFILE_SOURCE_PENDING;
    }
    if (hasMachineDefault) return LOGON_PROFILE_SOURCE_MACHINE_DEFAULT;
    return LOGON_PROFILE_SOURCE_NONE;
}

bool should_auto_apply_logon_profile(bool configuredLogonProfile, bool autoRestoreLockedOut) {
    return configuredLogonProfile && !autoRestoreLockedOut;
}

bool should_auto_restore_after_standby_resume(bool hasActiveDesiredSettings, bool autoRestoreLockedOut) {
    return hasActiveDesiredSettings && !autoRestoreLockedOut;
}

bool should_auto_restore_after_driver_event(bool hasSuccessfulApplyStamp,
    unsigned long long successfulApplyAgeMs, bool autoRestoreLockedOut) {
    return !autoRestoreLockedOut && hasSuccessfulApplyStamp &&
        successfulApplyAgeMs >= AUTO_RESTORE_STABILITY_WINDOW_MS;
}
