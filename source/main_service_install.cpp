// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// Service install / SCM lifecycle split out of main_service_server.cpp
// (F-MAINT-1): SCM state waits, binary-update stop, SCM failure-action config +
// verification, and service_install_or_remove. Compiled as a shard included after
// main_service_ipc.cpp and before main_service_server.cpp (no behavior change).

// The result of following one SCM state transition to its end.
struct ServiceStateWait {
    int verdict = GC_SCM_WAIT_STALLED;   // GcScmWaitVerdict
    bool readable = false;               // the last sample could be read at all
    SERVICE_STATUS_PROCESS last = {};
    ULONGLONG elapsedMs = 0;
};

// Follow a pending transition the way the SCM protocol defines it
// (service_scm_wait_policy.h): alive while the checkpoint advances within the
// service's own wait hint, finished the moment the state settles -- including
// settling somewhere ELSE, such as STOPPED while waiting for RUNNING, which
// means the start failed and waiting longer cannot help.  A checkpoint change
// raises no notification, so the status is sampled; the interval is derived
// from the hint as the protocol prescribes.
static ServiceStateWait wait_for_service_transition(SC_HANDLE svc, DWORD desiredState) {
    ServiceStateWait result;
    if (!svc) return result;
    GcScmWaitTracker tracker = {};
    ULONGLONG started = GetTickCount64();
    gc_scm_wait_begin(&tracker, started);
    for (;;) {
        DWORD needed = 0;
        ZeroMemory(&result.last, sizeof(result.last));
        result.readable = QueryServiceStatusEx(svc, SC_STATUS_PROCESS_INFO,
            (LPBYTE)&result.last, sizeof(result.last), &needed) != FALSE;
        ULONGLONG now = GetTickCount64();
        result.elapsedMs = now - started;
        if (!result.readable) {
            result.verdict = GC_SCM_WAIT_STALLED;
            break;
        }
        result.verdict = gc_scm_wait_step(&tracker, now, result.last.dwCurrentState,
            result.last.dwCheckPoint, result.last.dwWaitHint, desiredState);
        if (result.verdict != GC_SCM_WAIT_CONTINUE) break;
        Sleep(gc_scm_wait_poll_interval_ms(result.last.dwWaitHint));
    }
    debug_log("service state wait: desired=%lu verdict=%s after %llu ms state=%lu "
              "checkPoint=%lu waitHint=%lu win32Exit=%lu specificExit=%lu pid=%lu readable=%d\n",
              (unsigned long)desiredState, gc_scm_wait_verdict_name(result.verdict),
              (unsigned long long)result.elapsedMs,
              (unsigned long)result.last.dwCurrentState,
              (unsigned long)result.last.dwCheckPoint,
              (unsigned long)result.last.dwWaitHint,
              (unsigned long)result.last.dwWin32ExitCode,
              (unsigned long)result.last.dwServiceSpecificExitCode,
              (unsigned long)result.last.dwProcessId, result.readable ? 1 : 0);
    return result;
}

static bool wait_for_service_state(SC_HANDLE svc, DWORD desiredState) {
    return wait_for_service_transition(svc, desiredState).verdict == GC_SCM_WAIT_REACHED;
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

static bool stop_service_for_binary_update(SC_HANDLE svc, char* err, size_t errSize,
                                           int* reasonOut) {
    if (!svc) return true;
    SERVICE_STATUS_PROCESS ssp = {};
    DWORD needed = 0;
    if (!QueryServiceStatusEx(svc, SC_STATUS_PROCESS_INFO, (LPBYTE)&ssp, sizeof(ssp), &needed)) {
        DWORD queryErr = GetLastError();
        if (reasonOut) *reasonOut = gc_service_admin_classify_win32(GC_SVC_STAGE_STOP, queryErr);
        set_message(err, errSize, "Failed querying service state before repair (error %lu)", queryErr);
        return false;
    }
    if (ssp.dwCurrentState == SERVICE_STOPPED) return true;
    debug_log("service repair: stopping existing service before re-registration (state=%lu pid=%lu)\n",
        (unsigned long)ssp.dwCurrentState,
        (unsigned long)ssp.dwProcessId);
    if (ssp.dwCurrentState != SERVICE_STOP_PENDING) {
        SERVICE_STATUS status = {};
        if (!ControlService(svc, SERVICE_CONTROL_STOP, &status)) {
            DWORD stopErr = GetLastError();
            if (stopErr != ERROR_SERVICE_NOT_ACTIVE) {
                if (reasonOut) *reasonOut = gc_service_admin_classify_win32(GC_SVC_STAGE_STOP, stopErr);
                set_message(err, errSize, "Failed stopping the service for re-registration (error %lu)", stopErr);
                return false;
            }
            return true;
        }
    }
    ServiceStateWait stopped = wait_for_service_transition(svc, SERVICE_STOPPED);
    if (stopped.verdict != GC_SCM_WAIT_REACHED) {
        // Name the state it actually reached and why the wait ended: a service
        // wedged in STOP_PENDING, one that stopped reporting progress, and one
        // whose status could no longer be read want different answers.
        if (reasonOut) *reasonOut = GC_SVC_ADMIN_STOP_TIMED_OUT;
        set_message(err, errSize,
            "The service did not stop after %llu ms (%s, last state %lu%s)",
            (unsigned long long)stopped.elapsedMs, gc_scm_wait_verdict_name(stopped.verdict),
            stopped.readable ? (unsigned long)stopped.last.dwCurrentState : 0ul,
            stopped.readable ? "" : ", unreadable");
        return false;
    }
    debug_log("service repair: existing service stopped for re-registration\n");
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
    // Unexpected-crash availability net only. Controlled driver recovery reports
    // a clean stop and is restarted exclusively by the nonce-bound helper; it
    // never relies on SCM failure actions. Any failure-action restart has no
    // nonce and is therefore strictly non-restoring.
    SERVICE_FAILURE_ACTIONS_FLAG faf = {};
    faf.fFailureActionsOnNonCrashFailures = TRUE;
    if (!ChangeServiceConfig2W(svc, SERVICE_CONFIG_FAILURE_ACTIONS_FLAG, &faf)) {
        debug_log("service: ChangeServiceConfig2(FAILURE_ACTIONS_FLAG) failed (error %lu)\n", GetLastError());
        return false;
    }
    return true;
}

// Open our own service and (re)apply the SCM failure actions.  Called from
// service_main() at startup so the unexpected-crash availability net remains
// present even on installs predating this code. It never authorizes restoration.
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


// Shown in Services.msc and `sc qdescription`; a blank entry reads like
// something nobody installed on purpose.
#define GC_SERVICE_DESCRIPTION_W \
    L"Applies the GPU clock, voltage-curve, power-limit and fan settings requested " \
    L"by the Green Curve app. Removing it disables live GPU control in Green Curve."

static void service_configure_description(SC_HANDLE svc) {
    SERVICE_DESCRIPTIONW description = {};
    description.lpDescription = (LPWSTR)GC_SERVICE_DESCRIPTION_W;
    if (ChangeServiceConfig2W(svc, SERVICE_CONFIG_DESCRIPTION, &description)) {
        debug_log("service install: service description set\n");
    } else {
        debug_log("service install: ChangeServiceConfig2(DESCRIPTION) failed (error %lu)\n",
            (unsigned long)GetLastError());
    }
}

// The registration that existed before this install, so a failure AFTER the
// running service was stopped can put back what the user had.  Everything
// that can refuse without side effects (location, permissions, the binary)
// is checked before the stop; this covers what can only fail after it.
struct PreviousServiceRegistration {
    bool present = false;
    bool wasRunning = false;
    WCHAR commandLine[1024] = {};
    WCHAR binaryPath[MAX_PATH] = {};
};

static void read_previous_service_registration(SC_HANDLE svc, PreviousServiceRegistration* out) {
    if (!svc || !out) return;
    out->present = true;
    SERVICE_STATUS_PROCESS ssp = {};
    DWORD needed = 0;
    if (QueryServiceStatusEx(svc, SC_STATUS_PROCESS_INFO, (LPBYTE)&ssp, sizeof(ssp), &needed)) {
        out->wasRunning = ssp.dwCurrentState != SERVICE_STOPPED;
    }
    needed = 0;
    QueryServiceConfigW(svc, nullptr, 0, &needed);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || needed == 0) return;
    HeapBuffer buf(needed);
    if (!buf) return;
    QUERY_SERVICE_CONFIGW* config = (QUERY_SERVICE_CONFIGW*)buf.ptr;
    if (!QueryServiceConfigW(svc, config, needed, &needed) || !config->lpBinaryPathName) return;
    if (FAILED(StringCchCopyW(out->commandLine, ARRAY_COUNT(out->commandLine),
                              config->lpBinaryPathName))) {
        out->commandLine[0] = 0;
        return;
    }
    parse_service_binary_path_from_command_line(out->commandLine, out->binaryPath,
                                                ARRAY_COUNT(out->binaryPath));
}

// Put the previous registration back after a failure that happened once the
// running service had been stopped.  `restoreCommandLine` is set when the
// registration had already been re-pointed at the new folder.
static void restore_previous_service_after_failure(SC_HANDLE svc,
                                                   const PreviousServiceRegistration* previous,
                                                   bool restoreCommandLine, const char* why) {
    if (!svc || !previous || !previous->present) return;
    if (restoreCommandLine) {
        if (!previous->commandLine[0] ||
            !ChangeServiceConfigW(svc, SERVICE_NO_CHANGE, SERVICE_NO_CHANGE, SERVICE_NO_CHANGE,
                                  previous->commandLine, nullptr, nullptr, nullptr, nullptr,
                                  nullptr, nullptr)) {
            debug_log("service install: could not restore the previous registration after %s "
                      "(error %lu)\n", why, (unsigned long)GetLastError());
            return;
        }
        debug_log("service install: previous registration restored after %s\n", why);
    }
    if (!previous->wasRunning) {
        debug_log("service install: previous service was not running; leaving it stopped after %s\n",
                  why);
        return;
    }
    LPCWSTR startArgs[] = { L"--manual" };
    if (StartServiceW(svc, 1, startArgs) || GetLastError() == ERROR_SERVICE_ALREADY_RUNNING) {
        debug_log("service install: previous service restarted after %s\n", why);
    } else {
        debug_log("service install: previous service could not be restarted after %s (error %lu)\n",
                  why, (unsigned long)GetLastError());
    }
}

static bool service_install(SC_HANDLE scm, char* err, size_t errSize, int* reasonOut) {
    // 1. Everything that can refuse without touching anything: whether this
    //    folder may be hardened, how well it is protected, whether the binary
    //    is a plain single-link file.  The folder and the binary stay pinned
    //    (no FILE_SHARE_DELETE) until registration is over.
    ServiceInstallTarget target;
    if (!service_install_prepare_target(&target, err, errSize, reasonOut)) return false;
    // 2. Harden through the pinned handles.  Changing a DACL does not need the
    //    running service stopped, so a failure here still leaves it running.
    if (!service_install_harden_target(&target, err, errSize, reasonOut)) return false;

    WCHAR binPath[1024] = {};
    if (FAILED(StringCchPrintfW(binPath, ARRAY_COUNT(binPath), L"\"%ls\" --service-run",
                                target.binaryPath))) {
        if (reasonOut) *reasonOut = GC_SVC_ADMIN_REGISTRATION_FAILED;
        set_message(err, errSize, "Service command line is too long");
        return false;
    }

    // 3. Only now is the running service disturbed.
    ScopedServiceHandle svc(OpenServiceW(scm, L"GreenCurveService",
        SERVICE_CHANGE_CONFIG | SERVICE_START | SERVICE_STOP | SERVICE_QUERY_STATUS | SERVICE_QUERY_CONFIG));
    PreviousServiceRegistration previous;
    bool repointed = false;
    if (svc.valid()) {
        read_previous_service_registration(svc.get(), &previous);
        repointed = !previous.commandLine[0] || _wcsicmp(previous.commandLine, binPath) != 0;
        debug_log("service install: existing registration wasRunning=%d repoint=%d\n",
                  previous.wasRunning ? 1 : 0, repointed ? 1 : 0);
        // Recovery configuration does not require a stop. If policy blocks it,
        // keep the working service running instead of reporting a fragile
        // registration as successfully repaired.
        if (!service_configure_failure_actions(svc.get())) {
            if (reasonOut) *reasonOut = GC_SVC_ADMIN_RECOVERY_CONFIG_FAILED;
            set_message(err, errSize, "Failed configuring automatic service recovery");
            return false;
        }
        int stopReason = GC_SVC_ADMIN_UNKNOWN;
        if (!stop_service_for_binary_update(svc.get(), err, errSize, &stopReason)) {
            if (reasonOut) *reasonOut = stopReason;
            return false;
        }
        if (!ChangeServiceConfigW(svc.get(), SERVICE_NO_CHANGE, SERVICE_AUTO_START, SERVICE_NO_CHANGE,
                                  binPath, nullptr, nullptr, nullptr, nullptr, nullptr,
                                  L"Green Curve Background Service")) {
            // ERROR_SERVICE_MARKED_FOR_DELETE (1072) lands here, not on
            // CreateService: OpenService still succeeds for a service that is
            // only marked, so the reconfigure path is what refuses. It is also
            // the one failure here a user cannot fix by retrying.
            DWORD configErr = GetLastError();
            int reason = gc_service_admin_classify_win32(GC_SVC_STAGE_REGISTER, configErr);
            if (reasonOut) *reasonOut = reason;
            set_message(err, errSize, "Failed updating service configuration (error %lu)", configErr);
            restore_previous_service_after_failure(svc.get(), &previous, false,
                                                   "a refused reconfiguration");
            return false;
        }
    } else {
        svc.reset(CreateServiceW(
            scm,
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
        if (!svc.valid()) {
            DWORD createErr = GetLastError();
            int reason = gc_service_admin_classify_win32(GC_SVC_STAGE_REGISTER, createErr);
            if (reasonOut) *reasonOut = reason;
            set_message(err, errSize, "Failed installing service (error %lu)", createErr);
            return false;
        }
        if (!service_configure_failure_actions(svc.get())) {
            if (reasonOut) *reasonOut = GC_SVC_ADMIN_RECOVERY_CONFIG_FAILED;
            set_message(err, errSize, "Failed configuring automatic service recovery");
            if (!DeleteService(svc.get())) {
                debug_log("service install: failed removing unstarted registration after recovery configuration failure (error %lu)\n",
                          (unsigned long)GetLastError());
            }
            return false;
        }
    }

    // An unavailable unexpected-crash recovery net fails registration instead
    // of silently leaving a service that will not restart after a crash.
    debug_log("service install: configured SCM auto-restart failure actions + non-crash-failure flag\n");
    service_configure_description(svc.get());

    // 4. Start it and follow the start the way the SCM protocol defines it.
    //    The legacy --manual argument is kept for installed-version
    //    compatibility; every ordinary start is non-mutating.
    LPCWSTR startArgs[] = { L"--manual" };
    if (!StartServiceW(svc.get(), 1, startArgs)) {
        DWORD startErr = GetLastError();
        if (startErr != ERROR_SERVICE_ALREADY_RUNNING) {
            int reason = gc_service_admin_classify_win32(GC_SVC_STAGE_START, startErr);
            if (reasonOut) *reasonOut = reason;
            set_message(err, errSize, "Failed starting service (error %lu)", startErr);
            debug_log("service install: StartService failed error=%lu reason=%d\n",
                (unsigned long)startErr, reason);
            restore_previous_service_after_failure(svc.get(), &previous, repointed,
                                                   "a refused start");
            return false;
        }
    }
    ServiceStateWait started = wait_for_service_transition(svc.get(), SERVICE_RUNNING);
    if (started.verdict != GC_SCM_WAIT_REACHED) {
        // Settled elsewhere means the start FAILED (the service is back to
        // STOPPED, usually within a second); the exit codes say why.  Stalled
        // or out of time means it may still come up, so nothing is rolled back
        // underneath it.
        bool failed = started.verdict == GC_SCM_WAIT_SETTLED_ELSEWHERE;
        int reason = failed ? GC_SVC_ADMIN_START_FAILED : GC_SVC_ADMIN_START_TIMED_OUT;
        if (reasonOut) *reasonOut = reason;
        set_message(err, errSize,
            "The service was registered but did not reach RUNNING (%s after %llu ms, state %lu, "
            "exit %lu/%lu)",
            gc_scm_wait_verdict_name(started.verdict), (unsigned long long)started.elapsedMs,
            (unsigned long)started.last.dwCurrentState,
            (unsigned long)started.last.dwWin32ExitCode,
            (unsigned long)started.last.dwServiceSpecificExitCode);
        if (failed) {
            restore_previous_service_after_failure(svc.get(), &previous, repointed,
                                                   "a failed start");
        }
        return false;
    }

    // 5. The folder the service used to run from is not a service folder any
    //    more.  Release its hardening -- only if provably ours, and never when
    //    it is (or contains) the new one.
    if (repointed && previous.binaryPath[0]) {
        release_previous_service_location(previous.binaryPath, &target);
    }
    return true;
}

static bool service_remove(SC_HANDLE scm, char* err, size_t errSize, int* reasonOut) {
    ScopedServiceHandle svc(OpenServiceW(scm, L"GreenCurveService", SERVICE_STOP | DELETE | SERVICE_QUERY_STATUS));
    DWORD openErr = svc.valid() ? ERROR_SUCCESS : GetLastError();
    WCHAR installedServicePath[MAX_PATH] = {};
    get_service_binary_path_from_scm(installedServicePath, ARRAY_COUNT(installedServicePath));
    if (!svc.valid()) {
        if (!gc_service_admin_open_proves_absence(openErr)) {
            if (reasonOut) *reasonOut = gc_service_admin_classify_win32(GC_SVC_STAGE_REMOVE, openErr);
            set_message(err, errSize, "Failed opening service for removal (error %lu)", openErr);
            debug_log("service remove: OpenService failed error=%lu; registration state unknown, preserving permissions\n",
                      (unsigned long)openErr);
            return false;
        }
        debug_log("service remove: no registration to remove; releasing folder hardening only\n");
        cleanup_secure_service_binary_after_remove(installedServicePath[0] ? installedServicePath : nullptr);
        return true;
    }
    char probeError[128] = {};
    if (service_client_ping(probeError, sizeof(probeError))) {
        char resetResult[256] = {};
        service_client_reset(resetResult, sizeof(resetResult), nullptr);
    }
    SERVICE_STATUS status = {};
    if (!ControlService(svc.get(), SERVICE_CONTROL_STOP, &status)) {
        DWORD stopErr = GetLastError();
        if (stopErr != ERROR_SERVICE_NOT_ACTIVE) {
            debug_log("service remove: stop request refused (error %lu); checking the settled state before deletion\n",
                      (unsigned long)stopErr);
        }
    }
    ServiceStateWait stopped = wait_for_service_transition(svc.get(), SERVICE_STOPPED);
    if (stopped.verdict != GC_SCM_WAIT_REACHED) {
        if (reasonOut) *reasonOut = GC_SVC_ADMIN_REMOVE_STOP_TIMED_OUT;
        set_message(err, errSize, "Service removal stopped before deletion: stop did not complete "
                    "(%s after %llu ms, state %lu)",
                    gc_scm_wait_verdict_name(stopped.verdict),
                    (unsigned long long)stopped.elapsedMs,
                    (unsigned long)stopped.last.dwCurrentState);
        debug_log("service remove: stop did not complete (%s); preserving registration and permissions\n",
                  gc_scm_wait_verdict_name(stopped.verdict));
        return false;
    }
    if (!DeleteService(svc.get())) {
        DWORD deleteErr = GetLastError();
        if (reasonOut) *reasonOut = gc_service_admin_classify_win32(GC_SVC_STAGE_REMOVE, deleteErr);
        set_message(err, errSize, "Failed removing service (error %lu)", deleteErr);
        return false;
    }
    cleanup_secure_service_binary_after_remove(installedServicePath[0] ? installedServicePath : nullptr);
    return true;
}

static bool service_install_or_remove(bool enable, char* err, size_t errSize,
                                      int* reasonOut) {
    // Every exit from here carries a reason. UNKNOWN is the fail-safe default,
    // not a value any success path can leave behind.
    int reason = GC_SVC_ADMIN_UNKNOWN;
    if (reasonOut) *reasonOut = reason;

    // Elevation is checked BEFORE anything else, because "Failed opening
    // service manager (error 5)" was the single most common way this failed and
    // the least self-explanatory: README documents `greencurve.exe
    // --service-install` as the archive install step without saying it needs an
    // elevated shell, so an ordinary PowerShell window produced a bare error
    // number for a mistake with a one-line fix.
    if (!is_elevated()) {
        if (reasonOut) *reasonOut = GC_SVC_ADMIN_NOT_ELEVATED;
        set_message(err, errSize, "%s", gc_service_admin_reason_text(GC_SVC_ADMIN_NOT_ELEVATED));
        debug_log("service %s: refused, the process is not elevated\n",
            enable ? "install" : "remove");
        return false;
    }

    ScopedServiceHandle scm(OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CREATE_SERVICE | SC_MANAGER_CONNECT));
    if (!scm.valid()) {
        DWORD scmErr = GetLastError();
        reason = gc_service_admin_classify_win32(GC_SVC_STAGE_OPEN_SCM, scmErr);
        if (reasonOut) *reasonOut = reason;
        set_message(err, errSize, "Failed opening service manager (error %lu)", scmErr);
        return false;
    }

    bool ok = enable ? service_install(scm.get(), err, errSize, &reason)
                     : service_remove(scm.get(), err, errSize, &reason);
    if (ok) reason = GC_SVC_ADMIN_OK;
    if (reasonOut) *reasonOut = reason;
    debug_log("service %s: finished ok=%d reason=%d exitCode=%d\n",
        enable ? "install" : "remove", ok ? 1 : 0, reason,
        gc_service_admin_reason_exit_code(reason));
    return ok;
}
