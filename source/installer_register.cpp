// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// Shortcut removal and the uninstall itself.
//
// Linked into BOTH binaries.  The install-side record writing and shortcut
// creation live in installer_register_install.cpp, which only the setup stub
// links: greencurve-uninstall.exe must not statically contain the Add/Remove
// Programs writer or the .lnk creator.
//
// Shortcuts go to the all-users locations because the payload lands in a
// machine-wide directory under an elevated setup: writing to the invoking
// account's own Start menu would put the icon in the administrator's profile,
// which is frequently not the person who will use the program.

#include "installer_common.h"
#include "installer_move_cleanup.h"

#define GC_SHORTCUT_FILE_NAME L"Green Curve.lnk"

static bool gc_shortcut_directory(REFKNOWNFOLDERID folder, WCHAR* out, size_t outCount) {
    if (!out || outCount == 0) return false;
    out[0] = 0;
    PWSTR path = nullptr;
    if (FAILED(SHGetKnownFolderPath(folder, 0, nullptr, &path)) || !path) return false;
    bool ok = SUCCEEDED(StringCchCopyW(out, outCount, path));
    CoTaskMemFree(path);
    return ok;
}

// Remove a shortcut we may have created earlier.  Absence is success: the user
// is free to delete icons, and an upgrade that unticks the box must not fail
// because the icon was already gone.
static void gc_remove_shortcut(REFKNOWNFOLDERID folder, const WCHAR* subFolder) {
    WCHAR directory[MAX_PATH] = {};
    if (!gc_shortcut_directory(folder, directory, GC_ARRAY_COUNT(directory))) return;
    WCHAR linkPath[GC_INSTALLER_MAX_PATH_CHARS] = {};
    if (subFolder && subFolder[0]) {
        WCHAR nested[GC_INSTALLER_MAX_PATH_CHARS] = {};
        if (!gc_join_path(directory, subFolder, nested, GC_ARRAY_COUNT(nested))) return;
        if (!gc_join_path(nested, GC_SHORTCUT_FILE_NAME, linkPath, GC_ARRAY_COUNT(linkPath))) return;
        if (DeleteFileW(linkPath)) gc_log_step("shortcut: removed %ls", linkPath);
        // The program folder is ours, so removing it when empty is safe;
        // RemoveDirectory refuses a non-empty one on its own.
        RemoveDirectoryW(nested);
        return;
    }
    if (!gc_join_path(directory, GC_SHORTCUT_FILE_NAME, linkPath, GC_ARRAY_COUNT(linkPath))) return;
    if (DeleteFileW(linkPath)) gc_log_step("shortcut: removed %ls", linkPath);
}

// ---------------------------------------------------------------------------
// Removal
// ---------------------------------------------------------------------------

// Only files this installer places are removed.  Anything else the user put in
// the folder stays, and the folder itself is removed only when it ends up
// empty, so an uninstall can never take a directory of unrelated files with it.
// The service reports STOPPED to the SCM from inside its own process, so its
// binary stays locked for a short while afterwards.  `--service-remove` waits
// for the status, not for the process, which is why the handle has to be taken
// before the registration disappears and waited on after.
#define GC_UNINSTALL_SERVICE_EXIT_TIMEOUT_MS 20000

static bool gc_inspect_service_before_uninstall(HANDLE* processOut, bool* registeredOut,
                                                 char* error, size_t errorSize) {
    if (processOut) *processOut = nullptr;
    if (registeredOut) *registeredOut = false;
    GcScopedServiceHandle scm(OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT));
    if (!scm.valid()) {
        if (error && errorSize) StringCchCopyA(error, errorSize,
            "Could not open the service manager before uninstall.");
        gc_log_step("uninstall: OpenSCManager failed (error %lu)", GetLastError());
        return false;
    }
    GcScopedServiceHandle service(OpenServiceW(scm.get(), GC_SETUP_SERVICE_NAME, SERVICE_QUERY_STATUS));
    if (!service.valid()) {
        DWORD openErr = GetLastError();
        if (gc_service_admin_open_proves_absence(openErr)) return true;
        if (error && errorSize) StringCchCopyA(error, errorSize,
            "Could not check the background service before uninstall.");
        gc_log_step("uninstall: OpenService failed (error %lu)", (unsigned long)openErr);
        return false;
    }
    if (registeredOut) *registeredOut = true;
    SERVICE_STATUS_PROCESS status = {};
    DWORD needed = 0;
    if (!QueryServiceStatusEx(service.get(), SC_STATUS_PROCESS_INFO, (LPBYTE)&status,
                              sizeof(status), &needed)) {
        if (error && errorSize) StringCchCopyA(error, errorSize,
            "Could not read the background service state before uninstall.");
        gc_log_step("uninstall: QueryServiceStatusEx failed (error %lu)", GetLastError());
        return false;
    }
    if (status.dwProcessId == 0) return true;
    HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, status.dwProcessId);
    if (!process) {
        DWORD openErr = GetLastError();
        // A process that already exited between the query and the open leaves
        // nothing to wait for; every other failure is a failed proof.
        if (openErr != ERROR_INVALID_PARAMETER) {
            if (error && errorSize) StringCchCopyA(error, errorSize,
                "Could not open the background service process before uninstall.");
            gc_log_step("uninstall: OpenProcess(%lu) failed (error %lu)",
                        (unsigned long)status.dwProcessId, (unsigned long)openErr);
            return false;
        }
    }
    if (processOut) *processOut = process;
    else CloseHandle(process);
    return true;
}

// Nothing may be holding the install directory open when it is removed, and a
// process's own working directory holds it as surely as an open file does.  An
// uninstaller started by double-clicking it in Explorer inherits exactly that.
// Hand the (by then empty) install folder to the session manager.  Reported
// back so the finish page can say the folder goes away at the next restart
// instead of claiming it is already gone.
static void gc_schedule_directory_for_restart(const WCHAR* installDirectory, bool* folderLeftForRestart) {
    if (MoveFileExW(installDirectory, nullptr, MOVEFILE_DELAY_UNTIL_REBOOT)) {
        if (folderLeftForRestart) *folderLeftForRestart = true;
        return;
    }
    gc_log_step("uninstall: %ls could not be scheduled for removal at the next restart (error %lu)",
                installDirectory, GetLastError());
}

static void gc_leave_install_directory() {
    WCHAR system[MAX_PATH] = {};
    UINT length = GetSystemDirectoryW(system, GC_ARRAY_COUNT(system));
    if (length == 0 || length >= GC_ARRAY_COUNT(system)) return;
    if (!SetCurrentDirectoryW(system)) {
        gc_log_step("uninstall: the working directory could not be moved out of the installation (error %lu)",
                    GetLastError());
    }
}

// Every leaf this installer may have placed.  Includes the pre-rename
// uninstaller so an upgrade from a 0.26.0-era install does not strand it;
// GC_SETUP_UNINSTALL_EXE itself may be the running image and is then scheduled
// for the next restart instead of deleted in place.
static void gc_delete_installed_files(const WCHAR* installDirectory, bool* ownFilesLeftForRestart) {
    const wchar_t* const names[] = {
        GC_SETUP_PAYLOAD_FILE_NAMES[0],
        GC_SETUP_PAYLOAD_FILE_NAMES[1],
        GC_SETUP_PAYLOAD_FILE_NAMES[2],
        GC_SETUP_PAYLOAD_FILE_NAMES[3],
        GC_SETUP_UNINSTALL_EXE_W,
        GC_SETUP_UNINSTALL_EXE_LEGACY_W,
    };
    for (size_t i = 0; i < GC_ARRAY_COUNT(names); i++) {
        WCHAR path[GC_INSTALLER_MAX_PATH_CHARS] = {};
        if (!gc_join_path(installDirectory, names[i], path, GC_ARRAY_COUNT(path))) continue;
        if (!gc_file_exists(path)) continue;
        if (DeleteFileW(path)) {
            gc_log_step("uninstall: deleted %ls", path);
        } else {
            gc_log_step("uninstall: could not delete %ls (error %lu); scheduling it for the next restart",
                        path, GetLastError());
            MoveFileExW(path, nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
            if (ownFilesLeftForRestart) *ownFilesLeftForRestart = true;
        }
    }
}

bool gc_uninstall_execute(const WCHAR* installDirectory, bool* folderLeftForRestart,
                          char* error, size_t errorSize) {
    if (error && errorSize) error[0] = 0;
    if (folderLeftForRestart) *folderLeftForRestart = false;
    if (!installDirectory || !installDirectory[0]) {
        if (error && errorSize) StringCchCopyA(error, errorSize, "No installation folder was given.");
        return false;
    }
    gc_log_step("uninstall: removing the installation at %ls", installDirectory);
    gc_leave_install_directory();

    if (!gc_stop_gui_processes(nullptr)) {
        if (error && errorSize) {
            StringCchCopyA(error, errorSize,
                           "A running Green Curve program could not be closed. Close it and try again.");
        }
        return false;
    }

    // Let the installed binary unregister its own service: it also resets the
    // GPU and reverts the hardened permissions it applied, neither of which the
    // uninstaller should reimplement.
    GcScopedHandle serviceProcess;
    bool serviceRegistered = false;
    HANDLE runningProcess = nullptr;
    if (!gc_inspect_service_before_uninstall(&runningProcess, &serviceRegistered,
                                             error, errorSize)) return false;
    serviceProcess.reset(runningProcess);
    WCHAR guiPath[GC_INSTALLER_MAX_PATH_CHARS] = {};
    if (gc_join_path(installDirectory, GC_SETUP_GUI_EXE_W, guiPath, GC_ARRAY_COUNT(guiPath)) &&
        gc_file_exists(guiPath)) {
        WCHAR commandLine[2048] = {};
        StringCchPrintfW(commandLine, GC_ARRAY_COUNT(commandLine), L"\"%ls\" --service-remove", guiPath);
        DWORD exitCode = (DWORD)-1;
        // The shared admin-helper budget, not a private one: --service-remove
        // stops the service before deleting it and the stop wait alone may
        // take GC_SVC_SCM_STATE_WAIT_MS (service_admin_reason_policy.h).
        if (!gc_run_and_wait(guiPath, commandLine, GC_SVC_ADMIN_HELPER_TIMEOUT_MS,
                             &exitCode) || exitCode != 0) {
            gc_log_step("uninstall: --service-remove reported exit %lu (%s); "
                        "preserving program files",
                        exitCode,
                        gc_service_admin_uninstall_reason_text(
                            gc_service_admin_reason_from_exit_code(exitCode)));

            if (error && errorSize) StringCchCopyA(error, errorSize,
                gc_service_admin_uninstall_reason_text(
                    gc_service_admin_reason_from_exit_code(exitCode)));
            return false;
        }
        gc_log_step("uninstall: background service removed");
        // Wait on the real event.  Deleting the service binary while its
        // process is still exiting is what turns a clean uninstall into a
        // folder that survives until the next restart.
        if (serviceProcess.valid()) {
            DWORD wait = WaitForSingleObject(serviceProcess.get(), GC_UNINSTALL_SERVICE_EXIT_TIMEOUT_MS);
            if (wait != WAIT_OBJECT_0) {
                gc_log_step("uninstall: the service process was still running %d ms after removal (wait %lu)",
                            GC_UNINSTALL_SERVICE_EXIT_TIMEOUT_MS, wait);
                if (error && errorSize) StringCchCopyA(error, errorSize,
                    "The background service has not exited. Restart Windows and run uninstall again.");
                return false;
            } else {
                gc_log_step("uninstall: the service process has exited");
            }
        }
    } else if (serviceRegistered) {
        gc_log_step("uninstall: program missing while background service is still registered; preserving files");
        if (error && errorSize) StringCchCopyA(error, errorSize,
            "greencurve.exe is missing while the background service is registered. "
            "Restore the program files and run uninstall again.");
        return false;
    }

    gc_remove_shortcut(FOLDERID_CommonPrograms, GC_SETUP_PRODUCT_NAME_W);
    gc_remove_shortcut(FOLDERID_PublicDesktop, nullptr);

    // Autostart lives outside the install directory and outside the uninstall
    // key, so it survives everything else this function does unless it is
    // removed by name.  Both registrations are per-user and the uninstaller is
    // the only elevated component that sees all of them.
    gc_remove_startup_tasks();
    gc_remove_tray_autostart_values();

    // Set when one of our own files had to be left for the session manager.
    // It is the only case in which scheduling the *directory* for restart
    // removal makes sense: the folder will genuinely be empty by then.
    bool ownFilesLeftForRestart = false;
    gc_delete_installed_files(installDirectory, &ownFilesLeftForRestart);

    LONG status = RegDeleteKeyExW(HKEY_LOCAL_MACHINE, GC_SETUP_UNINSTALL_KEY, KEY_WOW64_64KEY, 0);
    if (status != ERROR_SUCCESS && status != ERROR_FILE_NOT_FOUND) {
        gc_log_step("uninstall: could not delete the Add/Remove Programs entry (error %ld)", status);
    }

    // The uninstaller is normally running from the directory it is deleting.
    // Windows refuses to delete a mapped image, so its own file -- and with it
    // the now otherwise empty folder -- is handed to the session manager for
    // the next restart, the conventional installer behavior.
    //
    // Deliberately NOT unlinked in place.  The only way to delete a running
    // image immediately (rename its default data stream into an alternate
    // stream, then a POSIX-semantics delete) is a published malware
    // self-deletion technique that antivirus behavior monitors flag; a false
    // detection in the middle of an uninstall costs the user far more than a
    // folder that lingers until the next restart.  tools/installer_build.py
    // forbids the pattern from returning.
    //
    // Only when it IS the installed copy: the same code path also runs inside
    // the setup stub launched with --uninstall, and that binary is sitting in
    // whatever folder the user downloaded it to.  Deleting that one used to
    // happen unconditionally, which quietly took the user's setup file with it.
    WCHAR selfPath[GC_INSTALLER_MAX_PATH_CHARS] = {};
    char selfPathUtf8[GC_INSTALLER_MAX_PATH_CHARS] = {};
    char installDirectoryUtf8[GC_INSTALLER_MAX_PATH_CHARS] = {};
    if (gc_module_path(selfPath, GC_ARRAY_COUNT(selfPath)) &&
        gc_wide_to_utf8(selfPath, selfPathUtf8, (int)sizeof(selfPathUtf8)) &&
        gc_wide_to_utf8(installDirectory, installDirectoryUtf8, (int)sizeof(installDirectoryUtf8)) &&
        gc_uninstall_self_is_installed_copy(selfPathUtf8, installDirectoryUtf8)) {
        if (MoveFileExW(selfPath, nullptr, MOVEFILE_DELAY_UNTIL_REBOOT)) {
            gc_log_step("uninstall: %ls is running; scheduled for removal at the next restart", selfPath);
            ownFilesLeftForRestart = true;
        } else {
            gc_log_fail("uninstall: %ls could not be scheduled for removal (error %lu)",
                        selfPath, GetLastError());
        }
    } else if (selfPath[0]) {
        gc_log_step("uninstall: %ls is not the installed copy; leaving it in place", selfPath);
    }

    if (!RemoveDirectoryW(installDirectory)) {
        DWORD removeError = GetLastError();
        if (removeError == ERROR_DIR_NOT_EMPTY && ownFilesLeftForRestart) {
            gc_log_step("uninstall: %ls still holds Green Curve files that are locked; "
                        "scheduling the folder for the next restart", installDirectory);
            gc_schedule_directory_for_restart(installDirectory, folderLeftForRestart);
        } else if (removeError == ERROR_DIR_NOT_EMPTY) {
            // Files the user put there are the user's; taking the folder with
            // them at the next restart would delete those too.
            gc_log_step("uninstall: %ls still holds files that were not part of this installation; leaving it in place",
                        installDirectory);
        } else {
            gc_log_step("uninstall: could not remove %ls (error %lu); scheduling it for the next restart",
                        installDirectory, removeError);
            gc_schedule_directory_for_restart(installDirectory, folderLeftForRestart);
        }
    } else {
        gc_log_step("uninstall: removed %ls", installDirectory);
    }
    return true;
}
