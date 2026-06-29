// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// Service install / SCM lifecycle split out of main_service_server.cpp
// (F-MAINT-1): SCM state waits, binary-update stop, SCM failure-action config +
// verification, and service_install_or_remove. Compiled as a shard included after
// main_service_ipc.cpp and before main_service_server.cpp (no behavior change).

static bool wait_for_service_state(SC_HANDLE svc, DWORD desiredState, DWORD timeoutMs) {
    if (!svc) return false;
    ULONGLONG startTick = GetTickCount64();
    SERVICE_STATUS_PROCESS ssp = {};
    DWORD needed = 0;
    while ((GetTickCount64() - startTick) < timeoutMs) {
        ZeroMemory(&ssp, sizeof(ssp));
        if (!QueryServiceStatusEx(svc, SC_STATUS_PROCESS_INFO, (LPBYTE)&ssp, sizeof(ssp), &needed)) return false;
        if (ssp.dwCurrentState == desiredState) return true;
        Sleep(200);
    }
    ZeroMemory(&ssp, sizeof(ssp));
    return QueryServiceStatusEx(svc, SC_STATUS_PROCESS_INFO, (LPBYTE)&ssp, sizeof(ssp), &needed) &&
        ssp.dwCurrentState == desiredState;
}

// GUID for display adapter device interface (GUID_DEVINTERFACE_DISPLAY_ADAPTER)
// Defined here to avoid include issues with initguid.h in headers.
// CLSID {4D36E978-E325-11CE-BFC1-08002BE10318}
static const GUID GUID_DISPLAY_ADAPTER_DEVINTERFACE = {
    0x4d36e978, 0xe325, 0x11ce,
    { 0xbf, 0xc1, 0x08, 0x00, 0x2b, 0xe1, 0x03, 0x18 }
};

// SERVICE_ACCEPT_DEVICE_EVENTS — llvm-mingw's winsvc.h does not define this
// (Windows 8+ only). See MSDN SERVICE_ACCEPT_DEVICE_EVENTS.
#ifndef SERVICE_ACCEPT_DEVICE_EVENTS
#define SERVICE_ACCEPT_DEVICE_EVENTS  0x00001000
#endif

// Device notification constants — llvm-mingw may not expose these in setupapi.h
#ifndef DBT_DEVNODES_CHANGED
#define DBT_DEVNODES_CHANGED         0x0007
#endif
#ifndef DBT_DEVICEREMOVEPENDING
#define DBT_DEVICEREMOVEPENDING      0x0003
#endif
#ifndef DBT_DEVICEREMOVECOMPLETE
#define DBT_DEVICEREMOVECOMPLETE     0x0004
#endif
#ifndef DBT_DEVICEARRIVAL
#define DBT_DEVICEARRIVAL            0x0008
#endif
#ifndef DBT_DEVTYP_DEVICEINTERFACE
#define DBT_DEVTYP_DEVICEINTERFACE   0x0005
#endif

// Device broadcast struct types — llvm-mingw may not expose these
typedef struct {
    DWORD dbcc_size;
    DWORD dbcc_devicetype;
    DWORD dbcc_reserved;
    GUID dbcc_classGuid;
    WCHAR dbcc_name[1];
} DEV_BROADCAST_DEVICEINTERFACEW, *PDEV_BROADCAST_DEVICEINTERFACEW;

static bool stop_service_for_binary_update(SC_HANDLE svc, char* err, size_t errSize) {
    if (!svc) return true;
    SERVICE_STATUS_PROCESS ssp = {};
    DWORD needed = 0;
    if (!QueryServiceStatusEx(svc, SC_STATUS_PROCESS_INFO, (LPBYTE)&ssp, sizeof(ssp), &needed)) {
        set_message(err, errSize, "Failed querying service state before repair (error %lu)", GetLastError());
        return false;
    }
    if (ssp.dwCurrentState == SERVICE_STOPPED) return true;
    debug_log("service repair: stopping existing service before binary update (state=%lu pid=%lu)\n",
        (unsigned long)ssp.dwCurrentState,
        (unsigned long)ssp.dwProcessId);
    if (ssp.dwCurrentState != SERVICE_STOP_PENDING) {
        SERVICE_STATUS status = {};
        if (!ControlService(svc, SERVICE_CONTROL_STOP, &status)) {
            DWORD stopErr = GetLastError();
            if (stopErr != ERROR_SERVICE_NOT_ACTIVE) {
                set_message(err, errSize, "Failed stopping service for binary update (error %lu)", stopErr);
                return false;
            }
            return true;
        }
    }
    if (!wait_for_service_state(svc, SERVICE_STOPPED, 10000)) {
        set_message(err, errSize, "Timed out stopping service for binary update");
        return false;
    }
    debug_log("service repair: existing service stopped for binary update\n");
    return true;
}

// Configure SCM auto-restart failure actions (SC_ACTION_RESTART 2s/5s/10s, reset
// daily).  GPU driver-recovery restarts the process with a non-zero exit code;
// these actions make the SCM relaunch us.  Applied both at install and at every
// service start (so older installs and the failure-count reset window cannot
// leave a service that exits but never restarts).  Requires SERVICE_CHANGE_CONFIG.
static bool service_configure_failure_actions(SC_HANDLE svc) {
    if (!svc) return false;
    SC_ACTION failureActions[3] = {};
    failureActions[0].Type = SC_ACTION_RESTART;
    failureActions[0].Delay = 2000;  // 2 s
    failureActions[1].Type = SC_ACTION_RESTART;
    failureActions[1].Delay = 5000;  // 5 s
    failureActions[2].Type = SC_ACTION_RESTART;
    failureActions[2].Delay = 10000; // 10 s
    SERVICE_FAILURE_ACTIONS sfa = {};
    // Short reset window so the SCM's failure COUNT resets quickly between
    // unrelated driver events.  With only 3 RESTART actions a long reset period
    // (e.g. 1 day) would stop auto-restarting after the 3rd driver-recovery
    // restart of the day; 10 min lets each spaced-out reconnect get a fresh 2 s
    // restart, while the persisted 5-in-5-min loop protection still breaks tight
    // crash loops.
    sfa.dwResetPeriod = 600; // 10 min
    sfa.lpRebootMsg = nullptr;
    sfa.lpCommand = nullptr;
    sfa.cActions = 3;
    sfa.lpsaActions = failureActions;
    if (!ChangeServiceConfig2W(svc, SERVICE_CONFIG_FAILURE_ACTIONS, &sfa)) {
        DWORD gle = GetLastError();
        if (gle == ERROR_ACCESS_DENIED) {
            // Expected when called from the running service: the LocalSystem
            // token lacks SERVICE_CHANGE_CONFIG on its own service object (only
            // Administrators have DC in the default service DACL).  The failure
            // actions are set authoritatively at INSTALL time (elevated/admin).
            debug_log("service: ChangeServiceConfig2(FAILURE_ACTIONS) access denied (error 5) — expected for the LocalSystem service token; failure actions are set at install\n");
        } else {
            debug_log("service: ChangeServiceConfig2(FAILURE_ACTIONS) failed (error %lu)\n", gle);
        }
        return false;
    }
    // Secondary / defense-in-depth: opt into non-crash failure actions so that a
    // *reported* SERVICE_STOPPED carrying a non-zero dwWin32ExitCode would also
    // trigger SC_ACTION_RESTART.  The PRIMARY driver-recovery mechanism does NOT
    // rely on this — service_main's recovery exit terminates WITHOUT reporting
    // SERVICE_STOPPED, which fires the failure action via the SCM's default path
    // (no flag needed).  This flag only ever applies on a fresh install (admin);
    // the LocalSystem service cannot set it at runtime.  A normal stop is exempt
    // because the graceful-shutdown path reports dwWin32ExitCode == NO_ERROR.
    SERVICE_FAILURE_ACTIONS_FLAG faf = {};
    faf.fFailureActionsOnNonCrashFailures = TRUE;
    if (!ChangeServiceConfig2W(svc, SERVICE_CONFIG_FAILURE_ACTIONS_FLAG, &faf)) {
        debug_log("service: ChangeServiceConfig2(FAILURE_ACTIONS_FLAG) failed (error %lu)\n", GetLastError());
        return false;
    }
    return true;
}

// Open our own service and (re)apply the SCM failure actions.  Called from
// service_main() at startup so the auto-restart actions are guaranteed present
// even on installs predating this code (the driver-recovery restart depends on
// them).
static void service_ensure_failure_actions_configured() {
    ScopedServiceHandle scm(OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT));
    if (!scm.valid()) {
        debug_log("service: OpenSCManager (for failure actions) failed (error %lu)\n", GetLastError());
        return;
    }
    ScopedServiceHandle svc(OpenServiceW(scm.get(), L"GreenCurveService", SERVICE_CHANGE_CONFIG));
    if (!svc.valid()) {
        debug_log("service: OpenService (for failure actions) failed (error %lu)\n", GetLastError());
        return;
    }
    if (service_configure_failure_actions(svc.get())) {
        debug_log("service: SCM auto-restart failure actions + non-crash-failure flag ensured at startup\n");
    }
}

// F-REL-1: verify the SCM auto-restart safety net is actually armed.  The whole
// driver-recovery design depends on the SCM relaunching the process after its
// non-zero exit (SC_ACTION_RESTART).  LocalSystem cannot SET the failure actions
// at runtime (it lacks SERVICE_CHANGE_CONFIG — that is why ensure_* above no-ops),
// so if the actions were never installed (or were cleared) the service would
// silently never come back after a GPU driver event (the build 229/230 failure
// class).  LocalSystem CAN query config (SERVICE_QUERY_CONFIG), so read the
// actual failure actions and log loudly whether the net is ARMED.  Surfacing this
// to the GUI would need a protocol field/bump; for now it is a prominent log line
// that immediately explains a "service never restarted" field report.
static void service_verify_restart_safety_net() {
    ScopedServiceHandle scm(OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT));
    if (!scm.valid()) {
        debug_log("service: restart-safety verify: OpenSCManager failed (error %lu)\n", GetLastError());
        return;
    }
    ScopedServiceHandle svc(OpenServiceW(scm.get(), L"GreenCurveService", SERVICE_QUERY_CONFIG));
    if (!svc.valid()) {
        debug_log("service: restart-safety verify: OpenService(QUERY_CONFIG) failed (error %lu)\n", GetLastError());
        return;
    }
    DWORD needed = 0;
    QueryServiceConfig2W(svc.get(), SERVICE_CONFIG_FAILURE_ACTIONS, nullptr, 0, &needed);
    if (needed == 0) {
        debug_log("service: restart-safety verify: cannot size failure-actions config (error %lu)\n", GetLastError());
        return;
    }
    BYTE* buf = (BYTE*)malloc(needed);
    if (!buf) return;
    int restartCount = 0;
    DWORD resetPeriod = 0;
    if (QueryServiceConfig2W(svc.get(), SERVICE_CONFIG_FAILURE_ACTIONS, buf, needed, &needed)) {
        SERVICE_FAILURE_ACTIONSW* sfa = (SERVICE_FAILURE_ACTIONSW*)buf;
        resetPeriod = sfa->dwResetPeriod;
        for (DWORD i = 0; sfa->lpsaActions && i < sfa->cActions; i++) {
            if (sfa->lpsaActions[i].Type == SC_ACTION_RESTART) restartCount++;
        }
        debug_log("service: restart-safety verify: SC_ACTION_RESTART actions=%d resetPeriod=%lus -> auto-restart net is %s\n",
            restartCount, (unsigned long)resetPeriod, restartCount > 0 ? "ARMED" : "NOT ARMED");
    } else {
        debug_log("service: restart-safety verify: QueryServiceConfig2(FAILURE_ACTIONS) failed (error %lu)\n", GetLastError());
    }
    free(buf);
    if (restartCount == 0) {
        debug_log("service: WARNING restart-safety net NOT ARMED — after a GPU driver event the service may not auto-restart. Reinstall the service (elevated) to repair the SCM failure actions.\n");
    }
}

static bool service_install_or_remove(bool enable, char* err, size_t errSize) {
    WCHAR exePath[MAX_PATH] = {};
    if (!enable && !get_adjacent_service_binary_path(exePath, ARRAY_COUNT(exePath), err, errSize)) {
        // Removal does not need the adjacent binary to exist; keep going with the secure target path for cleanup.
        err[0] = 0;
        WCHAR installDir[MAX_PATH] = {};
        char ignored[64] = {};
        if (get_secure_service_install_dir_w(installDir, ARRAY_COUNT(installDir), ignored, sizeof(ignored))) {
            StringCchPrintfW(exePath, ARRAY_COUNT(exePath), L"%ls\\%ls", installDir, APP_SERVICE_EXE_NAME_W);
        }
    }
    ScopedServiceHandle scm(OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CREATE_SERVICE | SC_MANAGER_CONNECT));
    if (!scm.valid()) {
        set_message(err, errSize, "Failed opening service manager (error %lu)", GetLastError());
        return false;
    }

    bool ok = false;
    if (enable) {
        ScopedServiceHandle svc(OpenServiceW(scm.get(), L"GreenCurveService", SERVICE_CHANGE_CONFIG | SERVICE_START | SERVICE_STOP | SERVICE_QUERY_STATUS));
        if (svc.valid() && !stop_service_for_binary_update(svc.get(), err, errSize)) return false;
        if (!ensure_secure_service_binary_path(exePath, ARRAY_COUNT(exePath), err, errSize)) return false;
        WCHAR binPath[1024] = {};
        if (FAILED(StringCchPrintfW(binPath, ARRAY_COUNT(binPath), L"\"%ls\" --service-run", exePath))) {
            set_message(err, errSize, "Service command line is too long");
            return false;
        }
        if (!svc.valid()) {
            svc.reset(CreateServiceW(
                scm.get(),
                L"GreenCurveService",
                L"Green Curve Background Service",
                SERVICE_CHANGE_CONFIG | SERVICE_START | SERVICE_STOP | DELETE | SERVICE_QUERY_STATUS,
                SERVICE_WIN32_OWN_PROCESS,
                SERVICE_AUTO_START,
                SERVICE_ERROR_NORMAL,
                binPath,
                nullptr,
                nullptr,
                nullptr,
                L"LocalSystem",
                nullptr));
        } else {
            if (!ChangeServiceConfigW(svc.get(), SERVICE_NO_CHANGE, SERVICE_AUTO_START, SERVICE_NO_CHANGE, binPath, nullptr, nullptr, nullptr, nullptr, nullptr, L"Green Curve Background Service")) {
                set_message(err, errSize, "Failed updating service configuration (error %lu)", GetLastError());
                return false;
            }
        }
        if (!svc.valid()) {
            set_message(err, errSize, "Failed installing service (error %lu)", GetLastError());
        } else {
            // Configure SCM auto-restart failure actions so a non-zero exit
            // (our driver-recovery restart) relaunches the service.  Also
            // re-applied at every service start by service_ensure_failure_
            // actions_configured().
            if (service_configure_failure_actions(svc.get())) {
                debug_log("service install: configured SCM auto-restart failure actions + non-crash-failure flag\n");
            }
            SERVICE_STATUS_PROCESS ssp = {};
            DWORD needed = 0;
            if (!QueryServiceStatusEx(svc.get(), SC_STATUS_PROCESS_INFO, (LPBYTE)&ssp, sizeof(ssp), &needed)) {
                set_message(err, errSize, "Failed querying installed service state (error %lu)", GetLastError());
                return false;
            }
            if (ssp.dwCurrentState != SERVICE_RUNNING) {
                if (!StartServiceW(svc.get(), 0, nullptr)) {
                    DWORD startErr = GetLastError();
                    if (startErr != ERROR_SERVICE_ALREADY_RUNNING) {
                        set_message(err, errSize, "Failed starting service (error %lu)", startErr);
                        return false;
                    }
                }
                wait_for_service_state(svc.get(), SERVICE_RUNNING, 10000);
                ZeroMemory(&ssp, sizeof(ssp));
                QueryServiceStatusEx(svc.get(), SC_STATUS_PROCESS_INFO, (LPBYTE)&ssp, sizeof(ssp), &needed);
            }
            if (ssp.dwCurrentState != SERVICE_RUNNING) {
                set_message(err, errSize, "Service install succeeded but the service did not reach RUNNING state");
            } else {
                ok = true;
            }
        }
    } else {
        ScopedServiceHandle svc(OpenServiceW(scm.get(), L"GreenCurveService", SERVICE_STOP | DELETE | SERVICE_QUERY_STATUS));
        WCHAR installedServicePath[MAX_PATH] = {};
        get_service_binary_path_from_scm(installedServicePath, ARRAY_COUNT(installedServicePath));
        if (!svc.valid()) {
            ok = true;
            cleanup_secure_service_binary_after_remove(installedServicePath[0] ? installedServicePath : nullptr);
        } else {
            if (g_app.backgroundServiceAvailable) {
                char resetResult[256] = {};
                service_client_reset(resetResult, sizeof(resetResult), nullptr);
            }
            SERVICE_STATUS status = {};
            ControlService(svc.get(), SERVICE_CONTROL_STOP, &status);
            wait_for_service_state(svc.get(), SERVICE_STOPPED, 10000);
            if (!DeleteService(svc.get())) {
                set_message(err, errSize, "Failed removing service (error %lu)", GetLastError());
            } else {
                ok = true;
                cleanup_secure_service_binary_after_remove(installedServicePath[0] ? installedServicePath : nullptr);
            }
        }
    }

    if (ok) refresh_background_service_state();
    return ok;
}

