// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// What is already installed, read from the Add/Remove Programs record and the
// SCM.
//
// Setup-only: this shard is not linked into greencurve-uninstall.exe.  The
// uninstaller resolves its target from its own module directory; a registry
// read of the ARP record would pull the whole "what would setup do next" model
// into a binary whose only job is removal.

#include "installer_common.h"

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
            prior->installerRegistered = true;
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
