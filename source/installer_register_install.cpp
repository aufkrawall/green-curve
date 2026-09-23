// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// Shortcut creation and the Add/Remove Programs record.
//
// Setup-only: this shard is not linked into greencurve-uninstall.exe.  The
// registry writer and the .lnk creator are install-side surface an uninstaller
// image should not carry.
//
// The registry record is the installer's memory: it is what makes the next run
// an upgrade instead of a second parallel installation, and what makes it
// possible to move an installation without stranding the previous one.  It is
// therefore saved by the install transaction and written just before service
// registration. A failed helper restores the previous record; shortcuts are
// updated only after the service has been confirmed running.

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

bool gc_write_uninstall_registration(GcInstallContext* context) {
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

void gc_update_shortcuts(GcInstallContext* context) {
    WCHAR installDirectory[GC_INSTALLER_MAX_PATH_CHARS] = {};
    WCHAR guiPath[GC_INSTALLER_MAX_PATH_CHARS] = {};
    if (!gc_utf8_to_wide(context->plan.targetDirectory, installDirectory,
                         (int)GC_ARRAY_COUNT(installDirectory)) ||
        !gc_join_path(installDirectory, GC_SETUP_GUI_EXE_W, guiPath, GC_ARRAY_COUNT(guiPath))) {
        gc_log_fail("shortcut: the installation path could not be formed");
        return;
    }
    if (context->plan.directoryChanged || !context->plan.createStartMenuShortcut)
        gc_remove_shortcut(FOLDERID_CommonPrograms, GC_SETUP_PRODUCT_NAME_W);
    if (context->plan.directoryChanged || !context->plan.createDesktopShortcut)
        gc_remove_shortcut(FOLDERID_PublicDesktop, nullptr);
    if (context->plan.createStartMenuShortcut &&
        !gc_write_start_menu_shortcut(guiPath, installDirectory))
        gc_log_step("shortcut: the Start menu entry could not be created; continuing");
    if (context->plan.createDesktopShortcut &&
        !gc_write_desktop_shortcut(guiPath, installDirectory))
        gc_log_step("shortcut: the desktop icon could not be created; continuing");
}
