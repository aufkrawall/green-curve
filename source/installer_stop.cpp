// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// Stopping the running Green Curve GUI and background service.
//
// Included by installer_apply.cpp where the stop step used to sit, so the
// amalgamated ordering is unchanged: the static helpers it relies on
// (gc_set_error, gc_report, gc_log_step) are defined above the include, and
// gc_stop_gui_processes() stays a normal function visible to the uninstall
// shard through installer_common.h.  Split out because the stop step grew
// fail-closed handle accounting and installer_apply.cpp reached the ~800-line
// guideline; the source-size ratchet forbids raising the ceiling instead.

// Bounded waits.  These are not race workarounds: each one waits on a real
// kernel object (process exit) or on an external state machine (the SCM) that
// has no other completion signal, and the bound exists only so a wedged
// third-party state cannot hang an unattended silent update forever.
#define GC_GUI_CLOSE_TIMEOUT_MS 15000
#define GC_SERVICE_STOP_TIMEOUT_MS 20000

struct GcCloseWindowsContext {
    DWORD processIds[64];
    int count;
};

static BOOL CALLBACK gc_collect_app_window(HWND hwnd, LPARAM param) {
    WCHAR className[64] = {};
    if (GetClassNameW(hwnd, className, GC_ARRAY_COUNT(className)) == 0) return TRUE;
    if (lstrcmpW(className, GC_APP_WINDOW_CLASS) != 0) return TRUE;
    auto* context = (GcCloseWindowsContext*)param;
    DWORD processId = 0;
    GetWindowThreadProcessId(hwnd, &processId);
    if (processId == 0) return TRUE;
    for (int i = 0; i < context->count; i++) {
        if (context->processIds[i] == processId) {
            // Already recorded; still ask this extra top-level window to close.
            PostMessageW(hwnd, WM_CLOSE, 0, 0);
            return TRUE;
        }
    }
    if (context->count < (int)GC_ARRAY_COUNT(context->processIds)) {
        context->processIds[context->count++] = processId;
    }
    // WM_CLOSE reaches the application's normal shutdown path (DestroyWindow ->
    // WM_DESTROY -> PostQuitMessage), which releases the tray icon, the
    // single-instance mutex, and the service connection.  Killing the process
    // outright would leave all three behind.
    PostMessageW(hwnd, WM_CLOSE, 0, 0);
    return TRUE;
}

// Ask every running Green Curve GUI to exit, then wait for the processes.
// Returns false whenever shutdown cannot be positively proved, including an
// enumeration/wait API failure or a process still alive after escalation.
bool gc_stop_gui_processes(GcInstallContext* context) {
    GcCloseWindowsContext windows = {};
    SetLastError(ERROR_SUCCESS);
    if (!EnumWindows(gc_collect_app_window, (LPARAM)&windows)) {
        // F-STOP-GUI-ENUMFAIL: enumeration failure is not evidence that no
        // Green Curve window exists.  Proceeding would make the next file
        // replacement race an unobserved running process.
        gc_log_fail("stop: could not enumerate Green Curve windows (error %lu)",
                    GetLastError());
        return false;
    }
    if (windows.count == 0) {
        gc_log_step("stop: no running Green Curve window found");
        return true;
    }
    gc_report(context, 10, "Closing Green Curve...");
    gc_log_step("stop: asked %d Green Curve process(es) to close", windows.count);

    HANDLE handles[64] = {};
    DWORD handleCount = 0;
    int openFailures = 0;
    for (int i = 0; i < windows.count; i++) {
        HANDLE process = OpenProcess(SYNCHRONIZE | PROCESS_TERMINATE, FALSE, windows.processIds[i]);
        if (process) {
            handles[handleCount++] = process;
        } else {
            // F-STOP-GUI-OPENFAIL: a process we cannot open cannot be waited
            // on or terminated, so "all exited" would be an assumption rather
            // than a fact -- and a still-running GUI holds the very files the
            // next step replaces.  Fail the stop step and let the user close
            // it, instead of surfacing the same failure later as a file error.
            openFailures++;
            gc_log_step("stop: could not open Green Curve process %lu (error %lu); "
                        "failing the stop step so the user can close it",
                        (unsigned long)windows.processIds[i], GetLastError());
        }
    }
    if (openFailures > 0) {
        for (DWORD i = 0; i < handleCount; i++) CloseHandle(handles[i]);
        return false;
    }
    bool allExited = true;
    if (handleCount > 0) {
        DWORD wait = WaitForMultipleObjects(handleCount, handles, TRUE, GC_GUI_CLOSE_TIMEOUT_MS);
        if (wait == WAIT_FAILED) {
            // F-STOP-GUI-WAITFAIL: only a positively signaled wait proves all
            // process handles exited.  A wait API failure must not retain the
            // optimistic allExited initializer.
            gc_log_fail("stop: waiting for Green Curve processes failed (error %lu)",
                        GetLastError());
            allExited = false;
        } else if (wait != WAIT_OBJECT_0 && wait != WAIT_TIMEOUT) {
            gc_log_fail("stop: waiting for Green Curve processes returned "
                        "unexpected result 0x%08lx",
                        (unsigned long)wait);
            allExited = false;
        } else if (wait == WAIT_TIMEOUT) {
            // A GUI stuck in a modal dialog would otherwise block the whole
            // update.  The files are about to be replaced anyway, so the
            // process cannot be left running; it is terminated and the fact is
            // recorded rather than hidden.
            gc_log_step("stop: a Green Curve process did not exit within %d ms; terminating it",
                        GC_GUI_CLOSE_TIMEOUT_MS);
            for (DWORD i = 0; i < handleCount; i++) {
                DWORD probe = WaitForSingleObject(handles[i], 0);
                if (probe == WAIT_FAILED) {
                    // F-STOP-GUI-PROBEFAIL: inability to determine whether an
                    // individual process exited is a failed stop proof, not a
                    // reason to skip it.
                    gc_log_fail("stop: probing a Green Curve process failed "
                                "(error %lu)", GetLastError());
                    allExited = false;
                } else if (probe != WAIT_OBJECT_0 && probe != WAIT_TIMEOUT) {
                    gc_log_fail("stop: probing a Green Curve process returned "
                                "unexpected result 0x%08lx",
                                (unsigned long)probe);
                    allExited = false;
                } else if (probe == WAIT_TIMEOUT) {
                    if (!TerminateProcess(handles[i], 0)) {
                        gc_log_fail("stop: could not terminate a Green Curve process (error %lu)",
                                    GetLastError());
                        allExited = false;
                    } else {
                        // F-STOP-TERM-WAIT: termination was requested, not
                        // proven.  A process that still holds its image after
                        // the wait must fail the stop step like any other.
                        DWORD terminatedWait = WaitForSingleObject(handles[i], 5000);
                        if (terminatedWait == WAIT_TIMEOUT) {
                            gc_log_fail("stop: a terminated Green Curve process "
                                        "did not exit within 5 s");
                            allExited = false;
                        } else if (terminatedWait == WAIT_FAILED) {
                            gc_log_fail("stop: waiting for a terminated Green Curve "
                                        "process failed (error %lu)", GetLastError());
                            allExited = false;
                        } else if (terminatedWait != WAIT_OBJECT_0) {
                            gc_log_fail("stop: waiting for a terminated Green Curve "
                                        "process returned unexpected result 0x%08lx",
                                        (unsigned long)terminatedWait);
                            allExited = false;
                        }
                    }
                }
            }
        }
    }
    for (DWORD i = 0; i < handleCount; i++) CloseHandle(handles[i]);
    return allExited;
}

// Stop the background service and wait for its process to actually exit.
//
// The wait is on the service PROCESS handle rather than on repeated SCM status
// queries: process exit is a real, signalable event, and it is also the
// condition that matters here, because a still-running process keeps a lock on
// the binary that is about to be overwritten.
static bool gc_stop_service(GcInstallContext* context, bool* wasRunningOut) {
    if (wasRunningOut) *wasRunningOut = false;
    GcScopedServiceHandle scm(OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT));
    if (!scm.valid()) {
        gc_set_error(context, "Could not open the service manager (error %lu). Run setup as an administrator.",
                     GetLastError());
        return false;
    }
    GcScopedServiceHandle service(OpenServiceW(scm.get(), GC_SETUP_SERVICE_NAME,
                                               SERVICE_STOP | SERVICE_QUERY_STATUS));
    if (!service.valid()) {
        DWORD error = GetLastError();
        if (error == ERROR_SERVICE_DOES_NOT_EXIST) {
            gc_log_step("stop: no background service is registered");
            return true;
        }
        gc_set_error(context, "Could not open the Green Curve service (error %lu).", error);
        return false;
    }

    SERVICE_STATUS_PROCESS status = {};
    DWORD needed = 0;
    if (!QueryServiceStatusEx(service.get(), SC_STATUS_PROCESS_INFO, (LPBYTE)&status,
                              sizeof(status), &needed)) {
        gc_set_error(context, "Could not query the Green Curve service state (error %lu).", GetLastError());
        return false;
    }
    if (status.dwCurrentState == SERVICE_STOPPED) {
        gc_log_step("stop: background service was already stopped");
        return true;
    }
    if (wasRunningOut) *wasRunningOut = true;
    gc_report(context, 15, "Stopping the background service...");

    GcScopedHandle serviceProcess;
    if (status.dwProcessId != 0) {
        serviceProcess.reset(OpenProcess(SYNCHRONIZE, FALSE, status.dwProcessId));
        if (!serviceProcess.valid()) {
            DWORD error = GetLastError();
            // F-STOP-SVC-OPENFAIL: without the process handle the exit wait
            // below cannot run, so "stopped" would be the SCM's word while the
            // binary may still be mapped.  A PID that is already gone is the
            // one legitimate exception: there is nothing left to wait for.
            if (error != ERROR_INVALID_PARAMETER) {
                gc_set_error(context,
                             "Could not open the Green Curve service process "
                             "(error %lu) to wait for it to stop.",
                             error);
                return false;
            }
            gc_log_step("stop: service process %lu exited before its handle "
                        "was taken; nothing left to wait for",
                        (unsigned long)status.dwProcessId);
        }
    }

    SERVICE_STATUS control = {};
    if (!ControlService(service.get(), SERVICE_CONTROL_STOP, &control)) {
        DWORD error = GetLastError();
        if (error != ERROR_SERVICE_NOT_ACTIVE) {
            gc_set_error(context, "Could not stop the Green Curve service (error %lu).", error);
            return false;
        }
    }

    if (serviceProcess.valid()) {
        DWORD wait = WaitForSingleObject(serviceProcess.get(), GC_SERVICE_STOP_TIMEOUT_MS);
        if (wait == WAIT_TIMEOUT) {
            gc_set_error(context,
                         "The Green Curve service did not stop within %d seconds. "
                         "Stop it manually and run setup again.",
                         GC_SERVICE_STOP_TIMEOUT_MS / 1000);
            return false;
        }
        if (wait == WAIT_FAILED) {
            gc_set_error(context,
                         "Could not wait for the Green Curve service process "
                         "to stop (error %lu).",
                         GetLastError());
            return false;
        }
        if (wait != WAIT_OBJECT_0) {
            gc_set_error(context,
                         "Waiting for the Green Curve service process returned "
                         "unexpected result 0x%08lx.",
                         (unsigned long)wait);
            return false;
        }
    }
    ZeroMemory(&status, sizeof(status));
    if (QueryServiceStatusEx(service.get(), SC_STATUS_PROCESS_INFO, (LPBYTE)&status,
                             sizeof(status), &needed) &&
        status.dwCurrentState != SERVICE_STOPPED) {
        // The process is gone but the SCM has not settled; the binary is
        // already unlocked, so this is logged rather than treated as fatal.
        gc_log_step("stop: service process exited; SCM still reports state %lu",
                    (unsigned long)status.dwCurrentState);
    }
    gc_log_step("stop: background service stopped");
    return true;
}
