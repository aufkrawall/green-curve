// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// Shortcuts, the Add/Remove Programs record, and removal.
//
// The registry record is the installer's memory: it is what makes the next run
// an upgrade instead of a second parallel installation, and what makes it
// possible to move an installation without stranding the previous one.  It is
// therefore written last, after the files and the service registration have
// already succeeded, so a half-finished install never advertises itself as
// complete.
//
// Shortcuts go to the all-users locations because the payload lands in a
// machine-wide directory under an elevated setup: writing to the invoking
// account's own Start menu would put the icon in the administrator's profile,
// which is frequently not the person who will use the program.

#include "installer_common.h"

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

// Create (or replace) one .lnk pointing at the installed GUI.
static bool gc_write_shortcut(const WCHAR* linkPath, const WCHAR* targetExe,
                              const WCHAR* workingDirectory, const char* description) {
    IShellLinkW* link = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_IShellLinkW, (void**)&link);
    if (FAILED(hr) || !link) {
        gc_log_fail("shortcut: CoCreateInstance(ShellLink) failed (hr 0x%08lx)", (unsigned long)hr);
        return false;
    }
    bool ok = false;
    WCHAR descriptionW[128] = {};
    gc_utf8_to_wide(description ? description : "", descriptionW, (int)GC_ARRAY_COUNT(descriptionW));
    if (SUCCEEDED(link->SetPath(targetExe)) &&
        SUCCEEDED(link->SetWorkingDirectory(workingDirectory)) &&
        SUCCEEDED(link->SetDescription(descriptionW)) &&
        SUCCEEDED(link->SetIconLocation(targetExe, 0))) {
        IPersistFile* persist = nullptr;
        if (SUCCEEDED(link->QueryInterface(IID_IPersistFile, (void**)&persist)) && persist) {
            hr = persist->Save(linkPath, TRUE);
            ok = SUCCEEDED(hr);
            if (!ok) gc_log_fail("shortcut: saving %ls failed (hr 0x%08lx)", linkPath, (unsigned long)hr);
            persist->Release();
        }
    }
    link->Release();
    if (ok) gc_log_step("shortcut: wrote %ls", linkPath);
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

static bool gc_write_start_menu_shortcut(const WCHAR* targetExe, const WCHAR* installDirectory) {
    WCHAR programs[MAX_PATH] = {};
    if (!gc_shortcut_directory(FOLDERID_CommonPrograms, programs, GC_ARRAY_COUNT(programs))) {
        gc_log_fail("shortcut: the all-users Start menu folder could not be resolved");
        return false;
    }
    WCHAR groupDirectory[GC_INSTALLER_MAX_PATH_CHARS] = {};
    if (!gc_join_path(programs, GC_SETUP_PRODUCT_NAME_W, groupDirectory, GC_ARRAY_COUNT(groupDirectory)) ||
        !gc_create_directory_tree(groupDirectory)) {
        gc_log_fail("shortcut: the Start menu folder could not be created");
        return false;
    }
    WCHAR linkPath[GC_INSTALLER_MAX_PATH_CHARS] = {};
    if (!gc_join_path(groupDirectory, GC_SHORTCUT_FILE_NAME, linkPath, GC_ARRAY_COUNT(linkPath))) return false;
    return gc_write_shortcut(linkPath, targetExe, installDirectory, "NVIDIA GPU VF curve editor");
}

static bool gc_write_desktop_shortcut(const WCHAR* targetExe, const WCHAR* installDirectory) {
    WCHAR desktop[MAX_PATH] = {};
    if (!gc_shortcut_directory(FOLDERID_PublicDesktop, desktop, GC_ARRAY_COUNT(desktop))) {
        gc_log_fail("shortcut: the all-users desktop folder could not be resolved");
        return false;
    }
    WCHAR linkPath[GC_INSTALLER_MAX_PATH_CHARS] = {};
    if (!gc_join_path(desktop, GC_SHORTCUT_FILE_NAME, linkPath, GC_ARRAY_COUNT(linkPath))) return false;
    return gc_write_shortcut(linkPath, targetExe, installDirectory, "NVIDIA GPU VF curve editor");
}

static bool gc_set_registry_string(HKEY key, const WCHAR* name, const WCHAR* value) {
    DWORD bytes = (DWORD)((wcslen(value) + 1) * sizeof(WCHAR));
    LONG status = RegSetValueExW(key, name, 0, REG_SZ, (const BYTE*)value, bytes);
    if (status != ERROR_SUCCESS) {
        gc_log_fail("registry: could not write %ls (error %ld)", name, status);
        return false;
    }
    return true;
}

static bool gc_set_registry_dword(HKEY key, const WCHAR* name, DWORD value) {
    LONG status = RegSetValueExW(key, name, 0, REG_DWORD, (const BYTE*)&value, sizeof(value));
    if (status != ERROR_SUCCESS) {
        gc_log_fail("registry: could not write %ls (error %ld)", name, status);
        return false;
    }
    return true;
}

// Sum the payload so Add/Remove Programs can show a size instead of a blank.
static DWORD gc_estimated_size_kb(const GcPayload* payload) {
    uint64_t total = 0;
    for (uint32_t i = 0; i < payload->fileCount; i++) total += payload->files[i].size;
    uint64_t kb = (total + 1023) / 1024;
    return (DWORD)(kb > 0xFFFFFFFFull ? 0xFFFFFFFFull : kb);
}

bool gc_write_shortcuts_and_registration(GcInstallContext* context) {
    WCHAR installDirectory[GC_INSTALLER_MAX_PATH_CHARS] = {};
    if (!gc_utf8_to_wide(context->plan.targetDirectory, installDirectory,
                         (int)GC_ARRAY_COUNT(installDirectory))) {
        return false;
    }
    WCHAR guiPath[GC_INSTALLER_MAX_PATH_CHARS] = {};
    WCHAR uninstallPath[GC_INSTALLER_MAX_PATH_CHARS] = {};
    if (!gc_join_path(installDirectory, GC_SETUP_GUI_EXE_W, guiPath, GC_ARRAY_COUNT(guiPath)) ||
        !gc_join_path(installDirectory, GC_SETUP_UNINSTALL_EXE_W, uninstallPath,
                      GC_ARRAY_COUNT(uninstallPath))) {
        StringCchCopyA(context->error, GC_ARRAY_COUNT(context->error),
                       "The installation path is too long for the program files.");
        gc_log_fail("%s", context->error);
        return false;
    }

    // An upgrade that moved must not leave the old install's icons behind
    // pointing at a directory that is no longer maintained.
    if (context->plan.directoryChanged || !context->plan.createStartMenuShortcut) {
        gc_remove_shortcut(FOLDERID_CommonPrograms, GC_SETUP_PRODUCT_NAME_W);
    }
    if (context->plan.directoryChanged || !context->plan.createDesktopShortcut) {
        gc_remove_shortcut(FOLDERID_PublicDesktop, nullptr);
    }
    // Shortcut failures are recorded but never fail the installation: the
    // program is installed and working at this point, and refusing to finish
    // over a missing icon would be worse than the missing icon.
    if (context->plan.createStartMenuShortcut) {
        if (!gc_write_start_menu_shortcut(guiPath, installDirectory)) {
            gc_log_step("shortcut: the Start menu entry could not be created; continuing");
        }
    }
    if (context->plan.createDesktopShortcut) {
        if (!gc_write_desktop_shortcut(guiPath, installDirectory)) {
            gc_log_step("shortcut: the desktop icon could not be created; continuing");
        }
    }

    HKEY key = nullptr;
    LONG status = RegCreateKeyExW(HKEY_LOCAL_MACHINE, GC_SETUP_UNINSTALL_KEY, 0, nullptr,
                                  REG_OPTION_NON_VOLATILE, KEY_READ | KEY_WRITE, nullptr, &key, nullptr);
    if (status != ERROR_SUCCESS) {
        snprintf(context->error, sizeof(context->error),
                 "Could not write the Add/Remove Programs entry (error %ld). "
                 "Run setup as an administrator.", status);
        gc_log_fail("%s", context->error);
        return false;
    }
    WCHAR quotedUninstall[GC_INSTALLER_MAX_PATH_CHARS + 8] = {};
    WCHAR quietUninstall[GC_INSTALLER_MAX_PATH_CHARS + 24] = {};
    StringCchPrintfW(quotedUninstall, GC_ARRAY_COUNT(quotedUninstall), L"\"%ls\"", uninstallPath);
    StringCchPrintfW(quietUninstall, GC_ARRAY_COUNT(quietUninstall), L"\"%ls\" /S", uninstallPath);
    WCHAR version[64] = {};
    gc_utf8_to_wide(APP_VERSION, version, (int)GC_ARRAY_COUNT(version));

    bool ok = gc_set_registry_string(key, L"DisplayName", GC_SETUP_PRODUCT_NAME_W) &&
              gc_set_registry_string(key, L"DisplayVersion", version) &&
              gc_set_registry_string(key, L"Publisher", GC_SETUP_PUBLISHER_W) &&
              gc_set_registry_string(key, L"InstallLocation", installDirectory) &&
              gc_set_registry_string(key, L"UninstallString", quotedUninstall) &&
              gc_set_registry_string(key, L"QuietUninstallString", quietUninstall) &&
              gc_set_registry_string(key, L"DisplayIcon", guiPath) &&
              gc_set_registry_dword(key, L"NoModify", 1) &&
              gc_set_registry_dword(key, L"NoRepair", 1) &&
              gc_set_registry_dword(key, L"EstimatedSize", gc_estimated_size_kb(&context->payload)) &&
              // Remembered so the next upgrade offers the same boxes instead of
              // silently re-creating icons the user removed.
              gc_set_registry_dword(key, L"GreenCurveStartMenuShortcut",
                                    context->plan.createStartMenuShortcut ? 1 : 0) &&
              gc_set_registry_dword(key, L"GreenCurveDesktopShortcut",
                                    context->plan.createDesktopShortcut ? 1 : 0) &&
              // The binary just written is from this payload, so it understands
              // --export-active-settings by construction.  Recording that is
              // what lets the NEXT upgrade ask it directly instead of inferring
              // the capability from a version string.
              gc_set_registry_dword(key, GC_SETUP_SETTINGS_EXPORT_VALUE, 1);
    RegCloseKey(key);
    if (!ok) {
        StringCchCopyA(context->error, GC_ARRAY_COUNT(context->error),
                       "The Add/Remove Programs entry could not be completed.");
        return false;
    }
    gc_log_step("registry: Add/Remove Programs entry written for %ls", installDirectory);
    return true;
}

// ---------------------------------------------------------------------------
// Removal
// ---------------------------------------------------------------------------

// Only files this installer places are removed.  Anything else the user put in
// the folder stays, and the folder itself is removed only when it ends up
// empty, so an uninstall can never take a directory of unrelated files with it.
static const WCHAR* const GC_INSTALLED_FILE_NAMES[] = {
    GC_SETUP_GUI_EXE_W,
    GC_SETUP_SERVICE_EXE_W,
    L"README.md",
    L"LICENSE",
};

// The service reports STOPPED to the SCM from inside its own process, so its
// binary stays locked for a short while afterwards.  `--service-remove` waits
// for the status, not for the process, which is why the handle has to be taken
// before the registration disappears and waited on after.
#define GC_UNINSTALL_SERVICE_EXIT_TIMEOUT_MS 20000

static HANDLE gc_open_running_service_process() {
    GcScopedServiceHandle scm(OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT));
    if (!scm.valid()) return nullptr;
    GcScopedServiceHandle service(OpenServiceW(scm.get(), GC_SETUP_SERVICE_NAME, SERVICE_QUERY_STATUS));
    if (!service.valid()) return nullptr;
    SERVICE_STATUS_PROCESS status = {};
    DWORD needed = 0;
    if (!QueryServiceStatusEx(service.get(), SC_STATUS_PROCESS_INFO, (LPBYTE)&status,
                              sizeof(status), &needed)) return nullptr;
    if (status.dwProcessId == 0) return nullptr;
    return OpenProcess(SYNCHRONIZE, FALSE, status.dwProcessId);
}

// Nothing may be holding the install directory open when it is removed, and a
// process's own working directory holds it as surely as an open file does.  An
// uninstaller started by double-clicking it in Explorer inherits exactly that.
static void gc_leave_install_directory() {
    WCHAR system[MAX_PATH] = {};
    UINT length = GetSystemDirectoryW(system, GC_ARRAY_COUNT(system));
    if (length == 0 || length >= GC_ARRAY_COUNT(system)) return;
    if (!SetCurrentDirectoryW(system)) {
        gc_log_step("uninstall: the working directory could not be moved out of the installation (error %lu)",
                    GetLastError());
    }
}

bool gc_uninstall_execute(const WCHAR* installDirectory, char* error, size_t errorSize) {
    if (error && errorSize) error[0] = 0;
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
    WCHAR guiPath[GC_INSTALLER_MAX_PATH_CHARS] = {};
    if (gc_join_path(installDirectory, GC_SETUP_GUI_EXE_W, guiPath, GC_ARRAY_COUNT(guiPath)) &&
        gc_file_exists(guiPath)) {
        GcScopedHandle serviceProcess(gc_open_running_service_process());
        WCHAR commandLine[2048] = {};
        StringCchPrintfW(commandLine, GC_ARRAY_COUNT(commandLine), L"\"%ls\" --service-remove", guiPath);
        DWORD exitCode = (DWORD)-1;
        if (!gc_run_and_wait(guiPath, commandLine, 60000, &exitCode) || exitCode != 0) {
            gc_log_step("uninstall: --service-remove reported exit %lu; continuing with file removal", exitCode);
        } else {
            gc_log_step("uninstall: background service removed");
        }
        // Wait on the real event.  Deleting the service binary while its
        // process is still exiting is what turns a clean uninstall into a
        // folder that survives until the next restart.
        if (serviceProcess.valid()) {
            DWORD wait = WaitForSingleObject(serviceProcess.get(), GC_UNINSTALL_SERVICE_EXIT_TIMEOUT_MS);
            if (wait != WAIT_OBJECT_0) {
                gc_log_step("uninstall: the service process was still running %d ms after removal (wait %lu)",
                            GC_UNINSTALL_SERVICE_EXIT_TIMEOUT_MS, wait);
            } else {
                gc_log_step("uninstall: the service process has exited");
            }
        }
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
    for (size_t i = 0; i < GC_ARRAY_COUNT(GC_INSTALLED_FILE_NAMES); i++) {
        WCHAR path[GC_INSTALLER_MAX_PATH_CHARS] = {};
        if (!gc_join_path(installDirectory, GC_INSTALLED_FILE_NAMES[i], path, GC_ARRAY_COUNT(path))) continue;
        if (!gc_file_exists(path)) continue;
        if (DeleteFileW(path)) {
            gc_log_step("uninstall: deleted %ls", path);
        } else {
            gc_log_step("uninstall: could not delete %ls (error %lu); scheduling it for the next restart",
                        path, GetLastError());
            MoveFileExW(path, nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
            ownFilesLeftForRestart = true;
        }
    }

    LONG status = RegDeleteKeyExW(HKEY_LOCAL_MACHINE, GC_SETUP_UNINSTALL_KEY, KEY_WOW64_64KEY, 0);
    if (status != ERROR_SUCCESS && status != ERROR_FILE_NOT_FOUND) {
        gc_log_step("uninstall: could not delete the Add/Remove Programs entry (error %ld)", status);
    }

    // The uninstaller is normally running from the directory it is deleting, so
    // it has to remove its own running image before the folder can go.
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
        // Immediately, so "uninstalled" and "the folder is gone" are the same
        // moment.  The reboot path stays as the fallback for a volume that
        // cannot do it (the rename needs alternate data streams).
        if (!gc_delete_running_module(selfPath)) {
            if (MoveFileExW(selfPath, nullptr, MOVEFILE_DELAY_UNTIL_REBOOT)) {
                ownFilesLeftForRestart = true;
            } else {
                gc_log_fail("uninstall: %ls could not be removed or scheduled for removal (error %lu)",
                            selfPath, GetLastError());
            }
        }
    } else if (selfPath[0]) {
        gc_log_step("uninstall: %ls is not the installed copy; leaving it in place", selfPath);
    }

    if (!RemoveDirectoryW(installDirectory)) {
        DWORD removeError = GetLastError();
        if (removeError == ERROR_DIR_NOT_EMPTY && ownFilesLeftForRestart) {
            gc_log_step("uninstall: %ls still holds Green Curve files that are locked; "
                        "scheduling the folder for the next restart", installDirectory);
            MoveFileExW(installDirectory, nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
        } else if (removeError == ERROR_DIR_NOT_EMPTY) {
            // Files the user put there are the user's; taking the folder with
            // them at the next restart would delete those too.
            gc_log_step("uninstall: %ls still holds files that were not installed by setup; leaving it in place",
                        installDirectory);
        } else {
            gc_log_step("uninstall: could not remove %ls (error %lu); scheduling it for the next restart",
                        installDirectory, removeError);
            MoveFileExW(installDirectory, nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
        }
    } else {
        gc_log_step("uninstall: removed %ls", installDirectory);
    }
    return true;
}
