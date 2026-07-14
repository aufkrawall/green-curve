// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT

// Protected shared-profile bank paths, migration, and policy accessors.

static bool resolve_machine_config_dir_w(WCHAR* outW, size_t outCount, char* err, size_t errSize) {
    if (outW && outCount > 0) outW[0] = 0;
    if (!outW || outCount == 0) {
        set_message(err, errSize, "Invalid machine config dir buffer");
        return false;
    }
    PWSTR programData = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_ProgramData, KF_FLAG_DEFAULT, nullptr, &programData)) && programData) {
        HRESULT hr = StringCchPrintfW(outW, outCount, L"%ls\\Green Curve", programData);
        CoTaskMemFree(programData);
        if (FAILED(hr)) {
            set_message(err, errSize, "Machine config directory path is too long");
            return false;
        }
        return true;
    }
    // Fallback to the %ProgramData% environment variable (then the canonical
    // default) if the known folder cannot be resolved.
    WCHAR base[MAX_PATH] = {};
    DWORD n = GetEnvironmentVariableW(L"ProgramData", base, ARRAY_COUNT(base));
    if (n == 0 || n >= ARRAY_COUNT(base)) {
        StringCchCopyW(base, ARRAY_COUNT(base), L"C:\\ProgramData");
    }
    if (FAILED(StringCchPrintfW(outW, outCount, L"%ls\\Green Curve", base))) {
        set_message(err, errSize, "Machine config directory path is too long");
        return false;
    }
    return true;
}
// Resolve the path to the machine-wide config file
// (%ProgramData%\Green Curve\shared-profiles.ini).
static bool resolve_machine_config_path_internal(WCHAR* outW, size_t outCount, char* err, size_t errSize) {
    if (outW && outCount > 0) outW[0] = 0;
    if (!outW || outCount == 0) {
        set_message(err, errSize, "Invalid machine config path buffer");
        return false;
    }
    WCHAR dirW[MAX_PATH] = {};
    if (!resolve_machine_config_dir_w(dirW, ARRAY_COUNT(dirW), err, errSize)) return false;
    if (FAILED(StringCchPrintfW(outW, outCount, L"%ls\\%hs", dirW, MACHINE_CONFIG_FILE_NAME))) {
        set_message(err, errSize, "Machine config path is too long");
        return false;
    }
    return true;
}

bool resolve_machine_config_path(char* out, size_t outSize) {
    if (out && outSize > 0) out[0] = 0;
    if (!out || outSize == 0) return false;
    WCHAR pathW[MAX_PATH] = {};
    if (!resolve_machine_config_path_internal(pathW, ARRAY_COUNT(pathW), out, outSize)) return false;
    return copy_wide_to_utf8(pathW, out, (int)outSize);
}

static bool ensure_machine_config_directory(char* err, size_t errSize) {
    WCHAR pathW[MAX_PATH] = {};
    if (!resolve_machine_config_path_internal(pathW, ARRAY_COUNT(pathW), err, errSize)) return false;
    WCHAR dirW[MAX_PATH] = {};
    StringCchCopyW(dirW, ARRAY_COUNT(dirW), pathW);
    WCHAR* slash = wcsrchr(dirW, L'\\');
    if (!slash) slash = wcsrchr(dirW, L'/');
    if (!slash) {
        set_message(err, errSize, "Machine config path has no directory");
        return false;
    }
    *slash = 0;
    if (!CreateDirectoryW(dirW, nullptr)) {
        DWORD createErr = GetLastError();
        if (createErr != ERROR_ALREADY_EXISTS) {
            set_message(err, errSize, "Failed creating machine config directory (error %lu)", createErr);
            return false;
        }
    }
    DWORD attrs = GetFileAttributesW(dirW);
    if (attrs == INVALID_FILE_ATTRIBUTES || (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0 ||
        (attrs & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        set_message(err, errSize, "Machine config directory is unavailable or unsafe");
        return false;
    }
    // Harden the directory so a standard user cannot plant or delete files in
    // the shared bank (the default %ProgramData% ACL grants users create-file
    // rights).  This is a required postcondition for all machine-bank writers.
    char aclErr[256] = {};
    if (!apply_protected_machine_config_dir_dacl(dirW, aclErr, sizeof(aclErr))) {
        set_message(err, errSize, "Machine config directory DACL hardening failed: %s", aclErr[0] ? aclErr : "unknown");
        return false;
    }
    if (!machine_config_dacl_is_hardened(dirW)) {
        set_message(err, errSize, "Machine config directory DACL verification failed");
        return false;
    }
    return true;
}

static bool harden_machine_config_file_required(const WCHAR* pathW, const char* pathForLog, char* err, size_t errSize) {
    char aclErr[256] = {};
    if (!apply_protected_machine_config_dacl(pathW, aclErr, sizeof(aclErr))) {
        set_message(err, errSize, "Machine config file DACL hardening failed: %s", aclErr[0] ? aclErr : "unknown");
        return false;
    }
    if (!machine_config_dacl_is_hardened(pathW)) {
        set_message(err, errSize, "Machine config file DACL verification failed");
        return false;
    }
    debug_log("machine config: verified protected DACL on %s\n", pathForLog ? pathForLog : "<wide path>");
    return true;
}

// One-time migration from the legacy machine.ini location (next to the
// installed service binary) to the %ProgramData%\Green Curve\shared-profiles.ini
// location.  Runs service-side as LocalSystem so it can write %ProgramData% and
// set DACLs.  No-op once the new file exists or when there is no legacy file.
static void migrate_legacy_machine_config() {
    char newPath[MAX_PATH] = {};
    if (!resolve_machine_config_path(newPath, sizeof(newPath))) return;
    if (gc_GetFileAttributesUtf8(newPath) != INVALID_FILE_ATTRIBUTES) {
        return; // already migrated (or freshly created at the new location)
    }
    WCHAR installDir[MAX_PATH] = {};
    if (!get_service_binary_directory_from_scm(installDir, ARRAY_COUNT(installDir))) {
        return; // service binary dir unknown; nothing legacy to migrate
    }
    WCHAR legacyW[MAX_PATH] = {};
    if (FAILED(StringCchPrintfW(legacyW, ARRAY_COUNT(legacyW), L"%ls\\%hs",
            installDir, LEGACY_MACHINE_CONFIG_FILE_NAME))) {
        return;
    }
    if (GetFileAttributesW(legacyW) == INVALID_FILE_ATTRIBUTES) {
        return; // no legacy file to migrate
    }
    char dirErr[256] = {};
    if (!ensure_machine_config_directory(dirErr, sizeof(dirErr))) {
        debug_log("machine config migration: cannot ensure target directory: %s\n",
            dirErr[0] ? dirErr : "unknown");
        return;
    }
    WCHAR newW[MAX_PATH] = {};
    char pathErr[256] = {};
    if (!resolve_machine_config_path_internal(newW, ARRAY_COUNT(newW), pathErr, sizeof(pathErr))) {
        debug_log("machine config migration: cannot resolve target path: %s\n",
            pathErr[0] ? pathErr : "unknown");
        return;
    }
    if (!CopyFileW(legacyW, newW, TRUE)) {
        debug_log("machine config migration: CopyFile failed (error %lu) from %ls\n",
            GetLastError(), legacyW);
        return;
    }
    char aclErr[256] = {};
    if (!apply_protected_machine_config_dacl(newW, aclErr, sizeof(aclErr))) {
        debug_log("machine config migration: DACL hardening failed: %s\n", aclErr[0] ? aclErr : "unknown");
        DeleteFileW(newW);
        return;
    }
    if (!machine_config_dacl_is_hardened(newW)) {
        debug_log("machine config migration: DACL verification failed after copy\n");
        DeleteFileW(newW);
        return;
    }
    if (!DeleteFileW(legacyW)) {
        debug_log("machine config migration: copied to %s but failed deleting legacy file (error %lu)\n",
            newPath, GetLastError());
    } else {
        debug_log("machine config migration: moved legacy %ls -> %s\n", legacyW, newPath);
    }
}

// Harden the %ProgramData% shared bank at service start (SYSTEM, before any
// interactive login).  The default %ProgramData% ACL lets standard users create
// subfolders, so without this a user could pre-create %ProgramData%\Green Curve
// (owning it with full control) and plant a malicious shared-profiles.ini before
// any admin initializes it — which the service would then trust (e.g. apply a
// hostile "shared default" to other users on logon).  Creating + hardening the
// directory at boot wins the race; if a squatted directory/file already exists,
// SYSTEM reclaims the protected DACL (and the file's owner) so the bank is
// admin-controlled from then on.
static void secure_shared_bank_at_startup() {
    char err[256] = {};
    if (!ensure_machine_config_directory(err, sizeof(err))) {
        debug_log("shared bank: startup hardening could not ensure directory: %s\n", err[0] ? err : "unknown");
        return;
    }
    WCHAR fileW[MAX_PATH] = {};
    char perr[256] = {};
    if (resolve_machine_config_path_internal(fileW, ARRAY_COUNT(fileW), perr, sizeof(perr)) &&
        GetFileAttributesW(fileW) != INVALID_FILE_ATTRIBUTES) {
        char aclErr[256] = {};
        if (!apply_protected_machine_config_dacl(fileW, aclErr, sizeof(aclErr))) {
            debug_log("shared bank: startup file DACL reclaim failed: %s\n", aclErr[0] ? aclErr : "unknown");
        } else if (!machine_config_dacl_is_hardened(fileW)) {
            debug_log("shared bank: startup file DACL verification failed after reclaim\n");
        } else {
            debug_log("shared bank: startup hardening verified/reclaimed %ls\n", fileW);
        }
    }
}

bool get_machine_logon_slot(int* slotOut) {
    if (slotOut) *slotOut = 0;
    char path[MAX_PATH] = {};
    if (!resolve_machine_config_path(path, sizeof(path))) return false;
    int slot = get_config_int(path, "profiles", "logon_slot", 0);
    if (slot < 0 || slot > CONFIG_NUM_SLOTS) slot = 0;
    if (slotOut) *slotOut = slot;
    return true;
}

bool set_machine_logon_slot(int slot, char* err, size_t errSize) {
    if (err && errSize) err[0] = 0;
    if (slot < 1 || slot > CONFIG_NUM_SLOTS) {
        set_message(err, errSize, "Invalid machine logon slot %d", slot);
        return false;
    }
    if (!is_elevated()) {
        set_message(err, errSize, "Setting a machine-wide default profile requires administrator rights");
        return false;
    }
    if (!ensure_machine_config_directory(err, errSize)) return false;
    char path[MAX_PATH] = {};
    if (!resolve_machine_config_path(path, sizeof(path))) {
        set_message(err, errSize, "Cannot resolve machine config path");
        return false;
    }
    if (!set_config_int(path, "profiles", "logon_slot", slot)) {
        set_message(err, errSize, "Failed writing machine config");
        return false;
    }
    WCHAR pathW[MAX_PATH] = {};
    if (!resolve_machine_config_path_internal(pathW, ARRAY_COUNT(pathW), err, errSize)) return false;
    if (!harden_machine_config_file_required(pathW, path, err, errSize)) return false;
    g_app.machineLogonSlotCache = slot;
    debug_log("machine config: set machine-wide logon slot to %d in %s\n", slot, path);
    return true;
}

bool clear_machine_logon_slot(char* err, size_t errSize) {
    if (err && errSize) err[0] = 0;
    if (!is_elevated()) {
        set_message(err, errSize, "Clearing the machine-wide default profile requires administrator rights");
        return false;
    }
    if (!ensure_machine_config_directory(err, errSize)) return false;
    char path[MAX_PATH] = {};
    if (!resolve_machine_config_path(path, sizeof(path))) {
        set_message(err, errSize, "Cannot resolve machine config path");
        return false;
    }
    if (!set_config_int(path, "profiles", "logon_slot", 0)) {
        set_message(err, errSize, "Failed clearing machine config");
        return false;
    }
    WCHAR pathW[MAX_PATH] = {};
    if (!resolve_machine_config_path_internal(pathW, ARRAY_COUNT(pathW), err, errSize)) return false;
    if (!harden_machine_config_file_required(pathW, path, err, errSize)) return false;
    g_app.machineLogonSlotCache = 0;
    debug_log("machine config: cleared machine-wide logon slot in %s\n", path);
    return true;
}

bool get_machine_restrict_policy(bool* enabledOut) {
    if (enabledOut) *enabledOut = false;
    char path[MAX_PATH] = {};
    if (!resolve_machine_config_path(path, sizeof(path))) return false;
    HANDLE configMutex = nullptr;
    if (!enter_config_storage_lock(&configMutex)) return false;
    HANDLE file = gc_CreateFileUtf8(path, GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        DWORD error = GetLastError();
        leave_config_storage_lock(configMutex);
        // A genuinely absent protected bank means no machine policy has been
        // configured. Any other failure is indeterminate and authorization
        // callers must fail closed rather than silently treating policy as OFF.
        return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
    }
    CloseHandle(file);
    int value = 0;
    bool valid = read_config_int_strict_locked(path, "policy",
        "restrict_non_admin_to_shared", 0, &value) &&
        (value == 0 || value == 1);
    leave_config_storage_lock(configMutex);
    if (!valid) return false;
    if (enabledOut) *enabledOut = value != 0;
    return true;
}

bool set_machine_restrict_policy(bool enable, char* err, size_t errSize) {
    if (err && errSize) err[0] = 0;
    if (!is_elevated()) {
        set_message(err, errSize, "Changing the shared-only policy requires administrator rights");
        return false;
    }
    if (!ensure_machine_config_directory(err, errSize)) return false;
    char path[MAX_PATH] = {};
    if (!resolve_machine_config_path(path, sizeof(path))) {
        set_message(err, errSize, "Cannot resolve machine config path");
        return false;
    }
    if (!set_config_int(path, "policy", "restrict_non_admin_to_shared", enable ? 1 : 0)) {
        set_message(err, errSize, "Failed writing machine config");
        return false;
    }
    WCHAR pathW[MAX_PATH] = {};
    if (!resolve_machine_config_path_internal(pathW, ARRAY_COUNT(pathW), err, errSize)) return false;
    if (!harden_machine_config_file_required(pathW, path, err, errSize)) return false;
    debug_log("machine config: set restrict_non_admin_to_shared=%d in %s\n", enable ? 1 : 0, path);
    return true;
}
