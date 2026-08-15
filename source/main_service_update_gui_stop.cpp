// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// Closing every Green Curve window before an update installs, across sessions.
//
// Split out of main_service_update_worker.cpp for the same reason
// main_service_update_worker_thread.cpp was: that file sits against the
// source-size ratchet, and this is the part of it with a self-contained job.
// It is included BEFORE the worker in the amalgamation, so the orchestration
// there can call straight into it.
//
// ## Why the service does this at all, rather than setup
//
// The updater launches setup from a LocalSystem service, so setup lives in
// session 0 -- and window enumeration is session-scoped. Setup therefore cannot
// see, close, or even detect a GUI running in the user's session: left to
// itself it logs "no running Green Curve window found" and then fails replacing
// greencurve.exe with ERROR_ACCESS_DENIED, half way through copying files.
//
// So the service closes them first, over the one channel that already crosses
// sessions: the ServiceUpdateState every GUI polls. It then proves they are
// gone, because "probably closed" is not a state you start overwriting binaries
// from.

// ---------------------------------------------------------------------------
// Stopping the GUI, across sessions
// ---------------------------------------------------------------------------

// How long a GUI gets to close itself after seeing guiShutdownRequested, and
// how long it then gets to die after being terminated.  Both are bounded
// because this runs before an install that will fail at file replacement if a
// GUI is still holding greencurve.exe.
#define GC_UPDATE_GUI_EXIT_TIMEOUT_MS 10000
#define GC_UPDATE_GUI_KILL_TIMEOUT_MS 5000
#define GC_UPDATE_MAX_GUI_PROCESSES 32

// Open every greencurve.exe running out of `installDir`, in ANY session.
//
// Matched by full image path, not by name: another process called
// greencurve.exe somewhere else on the machine is not ours to terminate, and
// this code runs as SYSTEM where that mistake is unrecoverable.
static bool service_update_collect_gui_processes(const char* installDir,
                                                 HANDLE* handles, int maxCount,
                                                 int* countOut) {
    if (countOut) *countOut = 0;
    // Owned by the lambda below so that every refusal path releases it.  It
    // used to be closed by hand at each `return`, and the OpenProcess arm was
    // missed -- a leaked kernel handle on an error path in a service that runs
    // for the life of the machine.
    HANDLE snapshot = INVALID_HANDLE_VALUE;
    auto fail_closed = [&](DWORD error, const char* reason) {
        if (snapshot != INVALID_HANDLE_VALUE) {
            CloseHandle(snapshot);
            snapshot = INVALID_HANDLE_VALUE;
        }
        // `handles` is checked here, not only by the caller-argument test
        // below: this runs before that test on the argument-validation path.
        for (int i = 0; handles && i < maxCount; ++i) {
            if (handles[i]) CloseHandle(handles[i]);
            handles[i] = nullptr;
        }
        if (countOut) *countOut = 0;
        debug_log("update install: GUI enumeration FAILED at %s (error %lu); "
                  "refusing to proceed\n", reason, error);
        return false;
    };

    if (!handles || maxCount <= 0 || !installDir || !installDir[0])
        return fail_closed(ERROR_INVALID_PARAMETER, "argument validation");

    char wanted[MAX_PATH] = {};
    if (FAILED(StringCchPrintfA(wanted, sizeof(wanted), "%s\\%s",
                                installDir, APP_EXE_NAME))) {
        return fail_closed(GetLastError(), "target path construction");
    }

    snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE)
        return fail_closed(GetLastError(), "process snapshot");

    int found = 0;
    PROCESSENTRY32W entry = {};
    entry.dwSize = sizeof(entry);
    if (Process32FirstW(snapshot, &entry)) {
        do {
            if (entry.th32ProcessID == GetCurrentProcessId()) continue;
            if (_wcsicmp(entry.szExeFile, APP_EXE_NAME_W) != 0) continue;

            HANDLE process = OpenProcess(
                SYNCHRONIZE | PROCESS_TERMINATE | PROCESS_QUERY_LIMITED_INFORMATION,
                FALSE, entry.th32ProcessID);
            if (!process) {
                DWORD openError = GetLastError();
                // The snapshot is a point-in-time list, so a process that
                // exited between it and here is simply GONE -- which is the
                // state this routine is trying to reach, not a failure to
                // measure it.  That race is reachable in ordinary use, because
                // greencurve.exe is the CLI binary too: the previous update's
                // own `--apply-settings-file` restore helper runs under exactly
                // this name and is short-lived.  Treating it as an enumeration
                // failure aborted the whole install.
                //
                // ERROR_ACCESS_DENIED and everything else still fail closed: a
                // process we are refused a handle to is one we cannot prove is
                // not holding greencurve.exe open.
                if (openError == ERROR_INVALID_PARAMETER) continue;
                return fail_closed(openError, "opening candidate GUI");
            }

            WCHAR imageW[MAX_PATH] = {};
            DWORD imageChars = ARRAY_COUNT(imageW);
            char image[MAX_PATH] = {};
            bool identified =
                QueryFullProcessImageNameW(process, 0, imageW, &imageChars) &&
                copy_wide_to_utf8(imageW, image, (int)sizeof(image));
            DWORD identifyError = identified ? 0 : GetLastError();
            if (!identified) {
                // Same split as above, and it used to be neither: an
                // unidentifiable process was silently skipped, so a live
                // greencurve.exe we could not name was quietly treated as "not
                // ours" in the one routine whose entire job is proving that.
                bool alreadyGone = WaitForSingleObject(process, 0) == WAIT_OBJECT_0;
                CloseHandle(process);
                if (alreadyGone) continue;
                return fail_closed(identifyError, "identifying a candidate GUI");
            }
            if (_stricmp(image, wanted) != 0) {
                CloseHandle(process);
                continue;
            }
            if (found >= maxCount) {
                CloseHandle(process);
                return fail_closed(ERROR_TOO_MANY_CMDS,
                                   "matching GUI process count");
            }
            handles[found++] = process;
            debug_log("update install: found GUI process pid=%lu session-agnostic path=%s\n",
                      (unsigned long)entry.th32ProcessID, image);
        } while (Process32NextW(snapshot, &entry));
    } else {
        DWORD enumError = GetLastError();
        CloseHandle(snapshot);
        return fail_closed(enumError, "first process enumeration");
    }
    CloseHandle(snapshot);
    if (countOut) *countOut = found;
    return true;
}

// Ask every GUI to close, then make sure it did.
//
// Fails closed: if a GUI is still alive afterwards the install must NOT
// proceed, because setup would get to the file-replacement step and stop there
// with ERROR_ACCESS_DENIED -- which is precisely the failure this exists to
// prevent, and it leaves a half-copied install directory behind.
static bool service_update_stop_gui_processes(const char* installDir,
                                              int* closedCountOut,
                                              char* err, size_t errSize) {
    if (closedCountOut) *closedCountOut = 0;
    HANDLE handles[GC_UPDATE_MAX_GUI_PROCESSES] = {};
    int count = 0;
    if (!service_update_collect_gui_processes(
            installDir, handles, GC_UPDATE_MAX_GUI_PROCESSES, &count)) {
        set_message(err, errSize,
                    "Could not prove every Green Curve window was closed; "
                    "the update was not installed");
        return false;
    }
    if (count == 0) {
        debug_log("update install: no GUI processes are running\n");
        return true;
    }

    // Publish the request and let each GUI run its own Exit path.  Graceful
    // matters: that path releases the tray icon, the single-instance mutex and
    // the service connection, none of which TerminateProcess would.
    {
        GcUpdateStateLock guard;
        g_updateState.guiShutdownRequested = true;
    }
    debug_log("update install: asked %d GUI process(es) to close\n", count);

    DWORD waited = WaitForMultipleObjects((DWORD)count, handles, TRUE,
                                          GC_UPDATE_GUI_EXIT_TIMEOUT_MS);
    bool allGone = waited >= WAIT_OBJECT_0 && waited < WAIT_OBJECT_0 + (DWORD)count;

    if (!allGone) {
        // A GUI that ignored the request (wedged, or not polling) is terminated.
        // The installer's own stop step does the same escalation for the same
        // reason; a hung window must not block an update forever.
        debug_log("update install: GUI did not exit within %d ms; terminating\n",
                  GC_UPDATE_GUI_EXIT_TIMEOUT_MS);
        for (int i = 0; i < count; ++i) {
            if (WaitForSingleObject(handles[i], 0) == WAIT_TIMEOUT) {
                TerminateProcess(handles[i], 0);
            }
        }
        waited = WaitForMultipleObjects((DWORD)count, handles, TRUE,
                                        GC_UPDATE_GUI_KILL_TIMEOUT_MS);
        allGone = waited >= WAIT_OBJECT_0 && waited < WAIT_OBJECT_0 + (DWORD)count;
    }

    for (int i = 0; i < count; ++i) CloseHandle(handles[i]);
    {
        GcUpdateStateLock guard;
        g_updateState.guiShutdownRequested = false;
    }

    if (!allGone) {
        set_message(err, errSize,
                    "A Green Curve window is still running and could not be "
                    "closed; the update was not installed");
        debug_log("update install: ABORTED, GUI processes still alive\n");
        return false;
    }
    // Report how many were closed.  The caller turns this into --launch vs
    // --no-launch, so leaving it at the zero set on entry silently means "no
    // GUI was running" and the user's window never comes back -- which is
    // exactly what shipped in 0.27.
    if (closedCountOut) *closedCountOut = count;
    debug_log("update install: all %d GUI process(es) exited\n", count);
    return true;
}

