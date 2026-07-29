// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// Everything the setup program actually does to the machine.
//
// Ordering is the whole design here.  An upgrade must ask the running build for
// its live settings BEFORE anything is stopped (afterwards there is nothing
// left to ask), stop the GUI and the service BEFORE their files are replaced,
// and re-point the service registration BEFORE the new service is started, so
// the SCM never launches a binary from a directory the user just moved away
// from.  Each step reports into the failure log, so a support log shows exactly
// which of those boundaries was crossed.

#include "installer_common.h"

// Bounded waits.  These are not race workarounds: each one waits on a real
// kernel object (process exit) or on an external state machine (the SCM) that
// has no other completion signal, and the bound exists only so a wedged
// third-party state cannot hang an unattended silent update forever.
#define GC_GUI_CLOSE_TIMEOUT_MS 15000
#define GC_SERVICE_STOP_TIMEOUT_MS 20000
#define GC_APP_CLI_TIMEOUT_MS 60000
// The settings export is a read: it talks to a service that is already running
// and writes one small file.  It gets a tighter bound than a general CLI call
// because it is also the one place setup runs a binary whose vocabulary it had
// to infer -- and a build that does not know the verb opens its window instead
// of exiting.  Terminating that after seconds rather than a minute is the
// difference between a hiccup and an unattended update that looks hung.
#define GC_APP_EXPORT_TIMEOUT_MS 20000
// The restore additionally waits for the just-started service to bring the GPU
// up, so it needs more headroom than a plain CLI call.
#define GC_APP_REAPPLY_TIMEOUT_MS 180000

// Defined below with the extraction step; the settings capture unpacks the new
// greencurve.exe through the same verified path before anything is stopped.
static bool gc_write_payload_file(const WCHAR* directory, const GcPayloadFile* file,
                                  GcInstallContext* context);

static void gc_report(GcInstallContext* context, int percent, const char* status) {
    gc_log_step("[%3d%%] %s", percent, status ? status : "");
    if (context && context->progress) context->progress(context->progressContext, percent, status);
}

[[gnu::format(printf, 2, 3)]]
static void gc_set_error(GcInstallContext* context, const char* fmt, ...) {
    if (!context) return;
    va_list args;
    va_start(args, fmt);
    int written = vsnprintf(context->error, sizeof(context->error), fmt, args);
    va_end(args);
    if (written < 0) context->error[0] = 0;
    context->error[sizeof(context->error) - 1] = 0;
    gc_log_fail("%s", context->error);
}

// ---------------------------------------------------------------------------
// Discovering what is already installed
// ---------------------------------------------------------------------------

static bool gc_read_registry_string(HKEY key, const WCHAR* name, char* out, size_t outCount) {
    if (!out || outCount == 0) return false;
    out[0] = 0;
    WCHAR value[GC_INSTALLER_MAX_PATH_CHARS] = {};
    DWORD bytes = sizeof(value) - sizeof(WCHAR);
    DWORD type = 0;
    if (RegQueryValueExW(key, name, nullptr, &type, (LPBYTE)value, &bytes) != ERROR_SUCCESS) return false;
    if (type != REG_SZ && type != REG_EXPAND_SZ) return false;
    value[bytes / sizeof(WCHAR)] = 0;
    return gc_wide_to_utf8(value, out, (int)outCount);
}

static GcInstallerToggle gc_read_registry_toggle(HKEY key, const WCHAR* name) {
    DWORD value = 0;
    DWORD bytes = sizeof(value);
    DWORD type = 0;
    if (RegQueryValueExW(key, name, nullptr, &type, (LPBYTE)&value, &bytes) != ERROR_SUCCESS) {
        return GC_TOGGLE_UNSET;
    }
    if (type != REG_DWORD) return GC_TOGGLE_UNSET;
    return value ? GC_TOGGLE_ON : GC_TOGGLE_OFF;
}

bool gc_read_prior_install(GcPriorInstall* prior) {
    if (!prior) return false;
    GcPriorInstall blank = {};
    *prior = blank;

    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, GC_SETUP_UNINSTALL_KEY, 0, KEY_READ, &key) == ERROR_SUCCESS) {
        if (gc_read_registry_string(key, L"InstallLocation", prior->directory, sizeof(prior->directory)) &&
            prior->directory[0]) {
            prior->present = true;
        }
        gc_read_registry_string(key, L"DisplayVersion", prior->version, sizeof(prior->version));
        prior->startMenuShortcut = gc_read_registry_toggle(key, L"GreenCurveStartMenuShortcut");
        prior->desktopShortcut = gc_read_registry_toggle(key, L"GreenCurveDesktopShortcut");
        prior->settingsExport = gc_read_registry_toggle(key, GC_SETUP_SETTINGS_EXPORT_VALUE);
        RegCloseKey(key);
    }

    // The SCM is the authority on where the *service* runs from, and it can
    // disagree with the registry: a user may have registered the service by
    // hand from an unpacked archive, with no Add/Remove Programs entry at all.
    // Treating that as a prior install is what makes an upgrade over a portable
    // copy re-point the service instead of leaving two registrations behind.
    WCHAR serviceDirectory[GC_INSTALLER_MAX_PATH_CHARS] = {};
    if (gc_service_image_directory(serviceDirectory, GC_ARRAY_COUNT(serviceDirectory))) {
        prior->serviceRegistered = true;
        gc_wide_to_utf8(serviceDirectory, prior->serviceDirectory, (int)sizeof(prior->serviceDirectory));
        if (!prior->present && prior->serviceDirectory[0]) {
            prior->present = true;
            StringCchCopyA(prior->directory, GC_ARRAY_COUNT(prior->directory), prior->serviceDirectory);
            gc_log_step("prior install: no registry entry; adopting the registered service directory %ls",
                        serviceDirectory);
        }
    }
    if (prior->present) {
        gc_log_step("prior install: directory=%s version=%s serviceRegistered=%d serviceDirectory=%s settingsExport=%s",
                    prior->directory, prior->version[0] ? prior->version : "<unknown>",
                    prior->serviceRegistered ? 1 : 0,
                    prior->serviceDirectory[0] ? prior->serviceDirectory : "<none>",
                    prior->settingsExport == GC_TOGGLE_ON ? "recorded" :
                    prior->settingsExport == GC_TOGGLE_OFF ? "recorded-absent" :
                    "unrecorded (falling back to the version)");
    } else {
        gc_log_step("prior install: none detected");
    }
    return prior->present;
}

bool gc_default_install_directory(char* out, size_t outCount) {
    if (!out || outCount == 0) return false;
    out[0] = 0;
    PWSTR programFiles = nullptr;
    // FOLDERID_ProgramFiles resolves to the native directory for this process's
    // architecture, so the arm64 setup lands in the arm64 Program Files rather
    // than the x86 one.
    if (FAILED(SHGetKnownFolderPath(FOLDERID_ProgramFiles, 0, nullptr, &programFiles)) || !programFiles) {
        return false;
    }
    char parent[GC_INSTALLER_MAX_PATH_CHARS] = {};
    bool ok = gc_wide_to_utf8(programFiles, parent, (int)sizeof(parent));
    CoTaskMemFree(programFiles);
    if (!ok) return false;
    return gc_install_default_directory(parent, out, outCount);
}

// ---------------------------------------------------------------------------
// Stopping what is running
// ---------------------------------------------------------------------------

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
// Returns false only when a process is still alive after the escalation.
bool gc_stop_gui_processes(GcInstallContext* context) {
    GcCloseWindowsContext windows = {};
    EnumWindows(gc_collect_app_window, (LPARAM)&windows);
    if (windows.count == 0) {
        gc_log_step("stop: no running Green Curve window found");
        return true;
    }
    gc_report(context, 10, "Closing Green Curve...");
    gc_log_step("stop: asked %d Green Curve process(es) to close", windows.count);

    HANDLE handles[64] = {};
    DWORD handleCount = 0;
    for (int i = 0; i < windows.count; i++) {
        HANDLE process = OpenProcess(SYNCHRONIZE | PROCESS_TERMINATE, FALSE, windows.processIds[i]);
        if (process) handles[handleCount++] = process;
    }
    bool allExited = true;
    if (handleCount > 0) {
        DWORD wait = WaitForMultipleObjects(handleCount, handles, TRUE, GC_GUI_CLOSE_TIMEOUT_MS);
        if (wait == WAIT_TIMEOUT) {
            // A GUI stuck in a modal dialog would otherwise block the whole
            // update.  The files are about to be replaced anyway, so the
            // process cannot be left running; it is terminated and the fact is
            // recorded rather than hidden.
            gc_log_step("stop: a Green Curve process did not exit within %d ms; terminating it",
                        GC_GUI_CLOSE_TIMEOUT_MS);
            for (DWORD i = 0; i < handleCount; i++) {
                if (WaitForSingleObject(handles[i], 0) == WAIT_TIMEOUT) {
                    if (!TerminateProcess(handles[i], 0)) {
                        gc_log_fail("stop: could not terminate a Green Curve process (error %lu)",
                                    GetLastError());
                        allExited = false;
                    } else {
                        WaitForSingleObject(handles[i], 5000);
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
        if (wait != WAIT_OBJECT_0) {
            gc_set_error(context,
                         "The Green Curve service did not stop within %d seconds. "
                         "Stop it manually and run setup again.",
                         GC_SERVICE_STOP_TIMEOUT_MS / 1000);
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

// ---------------------------------------------------------------------------
// Settings capture and explicit re-apply
// ---------------------------------------------------------------------------

// Run one `--export-active-settings` attempt and report whether it produced a
// snapshot.  The exit code alone is not the answer: only a readable file is.
static bool gc_try_export_active_settings(const WCHAR* exePath, const WCHAR* snapshot,
                                          const char* which) {
    if (!gc_file_exists(exePath)) {
        gc_log_step("capture: no %hs binary at the expected path", which);
        return false;
    }
    WCHAR commandLine[2048] = {};
    if (FAILED(StringCchPrintfW(commandLine, GC_ARRAY_COUNT(commandLine),
                                L"\"%ls\" --export-active-settings \"%ls\"", exePath, snapshot))) {
        return false;
    }
    DWORD exitCode = (DWORD)-1;
    bool ran = gc_run_and_wait(exePath, commandLine, GC_APP_EXPORT_TIMEOUT_MS, &exitCode);
    bool produced = ran && exitCode == 0 && gc_file_exists(snapshot);
    gc_log_step("capture: %hs binary export ran=%d exit=%lu snapshot=%d",
                which, ran ? 1 : 0, exitCode, produced ? 1 : 0);
    if (!produced) DeleteFileW(snapshot);
    return produced;
}

// Ask the running service for its live settings before anything is stopped.
//
// Two binaries can ask, and neither is right on its own:
//
//   - The INSTALLED one is the only client guaranteed to speak the protocol the
//     running service speaks, because they were built together.  It is asked
//     first, but only when the recorded version is new enough to understand the
//     verb -- an older build treats it as an unknown argument and falls through
//     to opening its window, which is what once made setup sit in front of an
//     unwanted GUI until the timeout expired.
//   - The PAYLOAD's one always understands the verb regardless of what is
//     installed, and is the fallback.  It cannot be the only attempt: across a
//     protocol bump it refuses the old service's responses, reports that no
//     settings are active, and the upgrade then restores nothing at all while
//     looking like a clean run.
//
// Both attempts are bounded and the helper is terminated on timeout, so neither
// can leave a process behind holding the files about to be replaced.
static void gc_capture_active_settings(GcInstallContext* context) {
    if (!context || !context->plan.captureActiveSettings) return;
    const GcPayloadFile* gui = gc_payload_find(&context->payload, GC_SETUP_GUI_EXE);
    if (!gui) {
        gc_log_step("capture: the payload carries no %s; skipping the settings snapshot", GC_SETUP_GUI_EXE);
        return;
    }

    WCHAR scratch[GC_INSTALLER_MAX_PATH_CHARS] = {};
    if (!gc_create_private_temp_directory(
            scratch, GC_ARRAY_COUNT(scratch))) {
        gc_log_step("capture: could not create an administrator-only scratch folder; skipping the settings snapshot");
        return;
    }
    WCHAR snapshot[GC_INSTALLER_MAX_PATH_CHARS] = {};
    WCHAR exePath[GC_INSTALLER_MAX_PATH_CHARS] = {};
    bool ok = gc_join_path(scratch, L"greencurve-upgrade-settings.ini", snapshot, GC_ARRAY_COUNT(snapshot)) &&
              gc_join_path(scratch, GC_SETUP_GUI_EXE_W, exePath, GC_ARRAY_COUNT(exePath));
    if (ok) {
        gc_report(context, 5, "Reading the current settings...");
        // Written through the same verified-payload path as a real install, so a
        // damaged download cannot be executed here either.
        ok = gc_write_payload_file(scratch, gui, nullptr);
    }
    bool captured = false;
    if (ok && context->plan.captureFromInstalledBinary) {
        WCHAR installedDirectory[GC_INSTALLER_MAX_PATH_CHARS] = {};
        WCHAR installedExe[GC_INSTALLER_MAX_PATH_CHARS] = {};
        if (gc_utf8_to_wide(context->plan.captureBinaryDirectory, installedDirectory,
                            (int)GC_ARRAY_COUNT(installedDirectory)) &&
            gc_join_path(installedDirectory, GC_SETUP_GUI_EXE_W, installedExe,
                         GC_ARRAY_COUNT(installedExe))) {
            captured = gc_try_export_active_settings(installedExe, snapshot, "installed");
        }
    }
    if (ok && !captured) captured = gc_try_export_active_settings(exePath, snapshot, "payload");
    if (captured) {
        // Keep the settings in the same administrator-only directory until the
        // new build consumes them. Moving them back to the ordinary user TEMP
        // root would re-open a replacement window after the executable race was
        // closed.
        StringCchCopyW(context->capturedSettingsPath,
            GC_ARRAY_COUNT(context->capturedSettingsPath), snapshot);
        context->haveCapturedSettings = true;
        gc_log_step("capture: active settings snapshot written to protected path");
    } else {
        // Not fatal: most often nothing is applied right now, which is a real
        // answer rather than an error.  The upgrade proceeds and simply does not
        // restore anything afterwards.
        gc_log_step("capture: no settings snapshot from either binary. "
                    "The upgrade will not re-apply settings.");
    }
    DeleteFileW(exePath);
    if (!context->haveCapturedSettings) {
        DeleteFileW(snapshot);
        RemoveDirectoryW(scratch);
    }
}

static void gc_discard_captured_settings(GcInstallContext* context) {
    if (!context || !context->haveCapturedSettings) return;
    WCHAR scratch[GC_INSTALLER_MAX_PATH_CHARS] = {};
    gc_directory_of(context->capturedSettingsPath, scratch,
                    GC_ARRAY_COUNT(scratch));
    DeleteFileW(context->capturedSettingsPath);
    context->capturedSettingsPath[0] = 0;
    context->haveCapturedSettings = false;
    if (scratch[0]) RemoveDirectoryW(scratch);
}

struct GcCapturedSettingsGuard {
    GcInstallContext* context;
    ~GcCapturedSettingsGuard() { gc_discard_captured_settings(context); }
};

// Re-apply the captured settings through the application's explicit CLI apply
// path.  This is a normal, user-typed-equivalent Apply — not a silent replay of
// a persisted snapshot — which is why it is allowed to write to the GPU at all
// under the service's event-only restore policy.
static void gc_reapply_captured_settings(GcInstallContext* context) {
    if (!context || !context->haveCapturedSettings) return;
    WCHAR targetDirectory[GC_INSTALLER_MAX_PATH_CHARS] = {};
    WCHAR exePath[GC_INSTALLER_MAX_PATH_CHARS] = {};
    if (!gc_utf8_to_wide(context->plan.targetDirectory, targetDirectory,
                         (int)GC_ARRAY_COUNT(targetDirectory)) ||
        !gc_join_path(targetDirectory, GC_SETUP_GUI_EXE_W, exePath, GC_ARRAY_COUNT(exePath))) {
        return;
    }
    WCHAR commandLine[2048] = {};
    if (FAILED(StringCchPrintfW(commandLine, GC_ARRAY_COUNT(commandLine),
                                L"\"%ls\" --apply-settings-file \"%ls\"",
                                exePath, context->capturedSettingsPath))) {
        return;
    }
    gc_report(context, 90, "Restoring your previous settings...");
    context->settingsRestoreAttempted = true;
    DWORD exitCode = (DWORD)-1;
    bool ran = gc_run_and_wait(exePath, commandLine, GC_APP_REAPPLY_TIMEOUT_MS, &exitCode);
    context->settingsRestored = ran && exitCode == 0;
    if (context->settingsRestored) {
        gc_log_step("re-apply: previous settings restored");
    } else {
        // The install itself succeeded; only the convenience restore did not.
        // It is reported on the final page as well as here, because a user who
        // is not told will simply find their GPU back at stock.
        gc_log_step("re-apply: previous settings were NOT restored (ran=%d exit=%lu); "
                    "see greencurve_cli_log.txt in %%LOCALAPPDATA%%\\Green Curve",
                    ran ? 1 : 0, exitCode);
    }
    gc_discard_captured_settings(context);
}

// ---------------------------------------------------------------------------
// Writing the payload
// ---------------------------------------------------------------------------

static bool gc_write_payload_file(const WCHAR* directory, const GcPayloadFile* file,
                                  GcInstallContext* context) {
    WCHAR name[GC_ARCHIVE_MAX_NAME + 1] = {};
    if (!gc_utf8_to_wide(file->name, name, (int)GC_ARRAY_COUNT(name))) {
        gc_set_error(context, "Payload file name \"%hs\" could not be decoded.", file->name);
        return false;
    }
    WCHAR finalPath[GC_INSTALLER_MAX_PATH_CHARS] = {};
    WCHAR tempPath[GC_INSTALLER_MAX_PATH_CHARS] = {};
    if (!gc_join_path(directory, name, finalPath, GC_ARRAY_COUNT(finalPath)) ||
        FAILED(StringCchPrintfW(tempPath, GC_ARRAY_COUNT(tempPath), L"%ls.gcnew", finalPath))) {
        gc_set_error(context, "The installation path for %hs is too long.", file->name);
        return false;
    }

    DeleteFileW(tempPath);
    GcScopedHandle handle(CreateFileW(tempPath, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                      FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!handle.valid()) {
        gc_set_error(context, "Could not create %ls (error %lu).", tempPath, GetLastError());
        return false;
    }
    const uint8_t* data = file->data;
    uint64_t remaining = file->size;
    while (remaining > 0) {
        DWORD chunk = (DWORD)(remaining > 0x10000000ull ? 0x10000000ull : remaining);
        DWORD written = 0;
        if (!WriteFile(handle.get(), data, chunk, &written, nullptr) || written == 0) {
            gc_set_error(context, "Could not write %ls (error %lu).", tempPath, GetLastError());
            handle.reset();
            DeleteFileW(tempPath);
            return false;
        }
        data += written;
        remaining -= written;
    }
    if (!FlushFileBuffers(handle.get())) {
        gc_log_step("extract: FlushFileBuffers failed for %ls (error %lu)", tempPath, GetLastError());
    }
    handle.reset();

    // Replace atomically so a failure part-way through never leaves a truncated
    // executable where a working one used to be.
    if (!MoveFileExW(tempPath, finalPath, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DWORD error = GetLastError();
        DeleteFileW(tempPath);
        gc_set_error(context,
                     "Could not replace %ls (error %lu). Close any running Green Curve program and retry.",
                     finalPath, error);
        return false;
    }
    gc_log_step("extract: wrote %ls (%llu bytes)", finalPath, (unsigned long long)file->size);
    return true;
}

// ---------------------------------------------------------------------------
// Service registration
// ---------------------------------------------------------------------------

// Let the newly installed binary register itself.  It owns the SCM
// registration, the failure-action policy, and the hardened DACLs on its own
// directory; duplicating any of that here would be a second implementation to
// keep in sync with the program's security model.
static bool gc_register_service(GcInstallContext* context) {
    WCHAR targetDirectory[GC_INSTALLER_MAX_PATH_CHARS] = {};
    WCHAR exePath[GC_INSTALLER_MAX_PATH_CHARS] = {};
    if (!gc_utf8_to_wide(context->plan.targetDirectory, targetDirectory,
                         (int)GC_ARRAY_COUNT(targetDirectory)) ||
        !gc_join_path(targetDirectory, GC_SETUP_GUI_EXE_W, exePath, GC_ARRAY_COUNT(exePath))) {
        gc_set_error(context, "The installation path is not usable.");
        return false;
    }
    WCHAR commandLine[2048] = {};
    if (FAILED(StringCchPrintfW(commandLine, GC_ARRAY_COUNT(commandLine),
                                L"\"%ls\" --service-install", exePath))) {
        gc_set_error(context, "The service install command line is too long.");
        return false;
    }
    gc_report(context, 70, context->plan.repointService
                               ? "Updating the background service registration..."
                               : "Installing the background service...");
    DWORD exitCode = (DWORD)-1;
    if (!gc_run_and_wait(exePath, commandLine, GC_APP_CLI_TIMEOUT_MS, &exitCode) || exitCode != 0) {
        gc_set_error(context,
                     "The background service could not be installed (exit code %lu). "
                     "See greencurve_cli_log.txt in %%LOCALAPPDATA%%\\Green Curve for the reason.",
                     exitCode);
        return false;
    }
    // Confirm the SCM now points at the new directory.  Without this an upgrade
    // that moved the installation could silently keep launching the old binary.
    WCHAR registered[GC_INSTALLER_MAX_PATH_CHARS] = {};
    if (gc_service_image_directory(registered, GC_ARRAY_COUNT(registered))) {
        char registeredUtf8[GC_INSTALLER_MAX_PATH_CHARS] = {};
        gc_wide_to_utf8(registered, registeredUtf8, (int)sizeof(registeredUtf8));
        if (!gc_install_paths_equal(registeredUtf8, context->plan.targetDirectory)) {
            gc_set_error(context,
                         "The background service is still registered at %ls instead of %hs.",
                         registered, context->plan.targetDirectory);
            return false;
        }
        gc_log_step("service: registration verified at %ls", registered);
    } else {
        gc_set_error(context, "The background service registration could not be read back.");
        return false;
    }
    return true;
}

// After the installation moves, the old directory keeps the protected DACL the
// previous service install applied to it.  Files there are deliberately left
// alone — deleting a user's old copy is not setup's decision — but the ACL is
// reverted to inherited so the user can actually delete them if they want to.
static void gc_release_previous_directory(const GcInstallContext* context) {
    if (!context->plan.directoryChanged || !context->plan.previousDirectory[0]) return;
    WCHAR previous[GC_INSTALLER_MAX_PATH_CHARS] = {};
    if (!gc_utf8_to_wide(context->plan.previousDirectory, previous, (int)GC_ARRAY_COUNT(previous))) return;
    if (!gc_directory_exists(previous)) return;
    DWORD result = SetNamedSecurityInfoW(previous, SE_FILE_OBJECT,
                                         DACL_SECURITY_INFORMATION | UNPROTECTED_DACL_SECURITY_INFORMATION,
                                         nullptr, nullptr, nullptr, nullptr);
    if (result == ERROR_SUCCESS) {
        gc_log_step("previous directory: %ls reverted to inherited permissions; "
                    "its files were left in place for you to remove", previous);
    } else {
        gc_log_step("previous directory: could not revert permissions on %ls (error %lu)", previous, result);
    }
    WCHAR previousService[GC_INSTALLER_MAX_PATH_CHARS] = {};
    if (gc_join_path(previous, GC_SETUP_SERVICE_EXE_W, previousService,
                     GC_ARRAY_COUNT(previousService)) && gc_file_exists(previousService)) {
        SetNamedSecurityInfoW(previousService, SE_FILE_OBJECT,
                              DACL_SECURITY_INFORMATION | UNPROTECTED_DACL_SECURITY_INFORMATION,
                              nullptr, nullptr, nullptr, nullptr);
    }
}

// ---------------------------------------------------------------------------
// Launching the installed program as the interactive user
// ---------------------------------------------------------------------------

bool gc_launch_installed_gui(const WCHAR* installDirectory) {
    WCHAR exePath[GC_INSTALLER_MAX_PATH_CHARS] = {};
    if (!gc_join_path(installDirectory, GC_SETUP_GUI_EXE_W, exePath, GC_ARRAY_COUNT(exePath))) return false;
    if (!gc_file_exists(exePath)) return false;

    // Setup runs elevated; starting the GUI directly from here would hand it an
    // administrator token it is not designed to hold (its manifest asks for
    // asInvoker).  Borrowing the desktop shell's token starts it at the
    // interactive user's normal integrity level instead.
    HWND shell = GetShellWindow();
    if (shell) {
        DWORD shellProcessId = 0;
        GetWindowThreadProcessId(shell, &shellProcessId);
        if (shellProcessId != 0) {
            GcScopedHandle shellProcess(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, shellProcessId));
            if (shellProcess.valid()) {
                HANDLE shellToken = nullptr;
                if (OpenProcessToken(shellProcess.get(), TOKEN_DUPLICATE, &shellToken)) {
                    GcScopedHandle scopedShellToken(shellToken);
                    HANDLE primaryToken = nullptr;
                    if (DuplicateTokenEx(shellToken,
                                         TOKEN_QUERY | TOKEN_DUPLICATE | TOKEN_ASSIGN_PRIMARY |
                                             TOKEN_ADJUST_DEFAULT | TOKEN_ADJUST_SESSIONID,
                                         nullptr, SecurityImpersonation, TokenPrimary, &primaryToken)) {
                        GcScopedHandle scopedPrimary(primaryToken);
                        WCHAR commandLine[GC_INSTALLER_MAX_PATH_CHARS + 8] = {};
                        StringCchPrintfW(commandLine, GC_ARRAY_COUNT(commandLine), L"\"%ls\"", exePath);
                        STARTUPINFOW startup = {};
                        startup.cb = sizeof(startup);
                        PROCESS_INFORMATION process = {};
                        if (CreateProcessWithTokenW(primaryToken, 0, exePath, commandLine, 0, nullptr,
                                                    installDirectory, &startup, &process)) {
                            CloseHandle(process.hProcess);
                            CloseHandle(process.hThread);
                            gc_log_step("launch: started %ls as the interactive user", exePath);
                            return true;
                        }
                        gc_log_step("launch: CreateProcessWithTokenW failed (error %lu)", GetLastError());
                    }
                }
            }
        }
    }

    // Fallback: ShellExecute from the elevated process.  Less ideal (the GUI
    // inherits elevation), but far better than not starting at all.
    SHELLEXECUTEINFOW info = {};
    info.cbSize = sizeof(info);
    info.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_FLAG_NO_UI;
    info.lpVerb = L"open";
    info.lpFile = exePath;
    info.lpDirectory = installDirectory;
    info.nShow = SW_SHOWNORMAL;
    if (ShellExecuteExW(&info)) {
        if (info.hProcess) CloseHandle(info.hProcess);
        gc_log_step("launch: started %ls through the shell (elevated fallback)", exePath);
        return true;
    }
    gc_log_step("launch: could not start %ls (error %lu)", exePath, GetLastError());
    return false;
}

// ---------------------------------------------------------------------------
// The install itself
// ---------------------------------------------------------------------------

bool gc_install_execute(GcInstallContext* context) {
    if (!context) return false;
    context->error[0] = 0;
    if (!context->plan.valid) {
        gc_set_error(context, "%s", context->plan.error[0] ? context->plan.error : "The install plan is invalid.");
        return false;
    }
    if (context->payload.fileCount == 0) {
        gc_set_error(context, "This setup file carries no program files.");
        return false;
    }
    gc_log_step("install: target=%s upgrade=%d moved=%d repointService=%d startMenu=%d desktop=%d launch=%d",
                context->plan.targetDirectory, context->plan.isUpgrade ? 1 : 0,
                context->plan.directoryChanged ? 1 : 0, context->plan.repointService ? 1 : 0,
                context->plan.createStartMenuShortcut ? 1 : 0, context->plan.createDesktopShortcut ? 1 : 0,
                context->plan.launchAfterInstall ? 1 : 0);

    // Reject an unsafe service location before capturing settings, closing the
    // GUI, or stopping the existing service.  A preflight failure must leave a
    // working installation completely untouched.
    WCHAR targetDirectory[GC_INSTALLER_MAX_PATH_CHARS] = {};
    if (!gc_utf8_to_wide(context->plan.targetDirectory, targetDirectory,
                         (int)GC_ARRAY_COUNT(targetDirectory))) {
        gc_set_error(context, "The installation folder could not be decoded.");
        return false;
    }
    if (!gc_install_directory_is_secure_rooted(targetDirectory)) {
        gc_set_error(context,
            "Green Curve must be installed in a direct child folder of Program Files because its background service runs as LocalSystem.");
        return false;
    }

    GcCapturedSettingsGuard capturedSettingsGuard = {context};

    // 1. Capture first: after the next step there is nothing left to ask.
    gc_capture_active_settings(context);

    // 2. Nothing may hold the files open once extraction starts.
    if (!gc_stop_gui_processes(context)) {
        gc_set_error(context, "A running Green Curve program could not be closed. Close it and run setup again.");
        return false;
    }
    bool serviceWasRunning = false;
    if (!gc_stop_service(context, &serviceWasRunning)) return false;

    // 3. Files.
    gc_report(context, 25, "Creating the installation folder...");
    if (!gc_create_directory_tree(targetDirectory)) {
        gc_set_error(context, "Could not create %ls. Choose a different folder or run setup as an administrator.",
                     targetDirectory);
        return false;
    }
    DWORD targetAttributes = GetFileAttributesW(targetDirectory);
    if (targetAttributes == INVALID_FILE_ATTRIBUTES ||
        (targetAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0 ||
        (targetAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        gc_set_error(context,
            "The installation folder is unavailable or is a reparse point.");
        return false;
    }
    char directoryAclError[256] = {};
    if (!apply_protected_service_dir_dacl(
            targetDirectory, directoryAclError, sizeof(directoryAclError)) ||
        !machine_config_dacl_is_hardened(targetDirectory)) {
        gc_set_error(context, "The installation folder could not be secured: %s",
            directoryAclError[0] ? directoryAclError :
            "DACL verification failed");
        return false;
    }
    gc_report(context, 30, "Copying program files...");
    for (uint32_t i = 0; i < context->payload.fileCount; i++) {
        if (!gc_write_payload_file(targetDirectory, &context->payload.files[i], context)) return false;
        int percent = 30 + (int)((40 * (uint64_t)(i + 1)) / context->payload.fileCount);
        gc_report(context, percent, "Copying program files...");
    }

    // 4. Service registration, which also re-hardens the new directory.
    if (!gc_register_service(context)) return false;
    gc_release_previous_directory(context);

    // 5. Shortcuts and the Add/Remove Programs entry.
    gc_report(context, 80, "Creating shortcuts...");
    if (!gc_write_shortcuts_and_registration(context)) return false;

    // 6. Put the user's settings back exactly as an explicit Apply would.
    gc_reapply_captured_settings(context);

    gc_report(context, 100, "Installation complete.");
    return true;
}
