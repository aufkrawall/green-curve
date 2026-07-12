// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT

// ============================================================================
// Per-user resident tray autostart
// ============================================================================
//
// The Task Scheduler job is deliberately bounded because it can wait for the
// service and must report a failed authenticated handoff.  A GUI launched by
// that task remains in the task's job and can therefore be terminated when the
// task reaches its ExecutionTimeLimit.  Keep tray residency independent: the
// scheduled task runs only --logon-start, while this HKCU Run value launches a
// distinct --tray-start process that is not owned by the scheduled task.

#ifndef GREEN_CURVE_SERVICE_BINARY

static const WCHAR* TRAY_AUTOSTART_RUN_KEY =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
static const WCHAR* TRAY_AUTOSTART_VALUE_NAME = L"Green Curve";

static bool build_tray_autostart_command(const char* configPath,
    WCHAR* commandOut, size_t commandOutCount, char* err, size_t errSize) {
    if (!configPath || !configPath[0] || !commandOut || commandOutCount == 0) {
        set_message(err, errSize, "Invalid tray autostart configuration");
        return false;
    }

    WCHAR exePath[MAX_PATH] = {};
    DWORD exeLength = GetModuleFileNameW(nullptr, exePath, ARRAY_COUNT(exePath));
    if (exeLength == 0 || exeLength >= ARRAY_COUNT(exePath)) {
        set_message(err, errSize, "Could not resolve the Green Curve executable path (error %lu)",
            GetLastError());
        return false;
    }

    WCHAR configPathW[MAX_PATH] = {};
    if (!utf8_to_wide(configPath, configPathW, ARRAY_COUNT(configPathW))) {
        set_message(err, errSize, "Could not convert the tray autostart config path");
        return false;
    }
    if (wcschr(exePath, L'"') || wcschr(configPathW, L'"')) {
        set_message(err, errSize, "Tray autostart paths contain an unsupported quote character");
        return false;
    }

    HRESULT hr = StringCchPrintfW(commandOut, commandOutCount,
        L"\"%ls\" --tray-start --config \"%ls\"", exePath, configPathW);
    if (FAILED(hr)) {
        set_message(err, errSize, "Tray autostart command is too long");
        return false;
    }
    return true;
}

static bool query_tray_autostart_value(WCHAR* valueOut, size_t valueOutCount,
    bool* existsOut, char* err, size_t errSize) {
    if (existsOut) *existsOut = false;
    if (valueOut && valueOutCount) valueOut[0] = 0;
    if (!valueOut || valueOutCount == 0 || !existsOut) {
        set_message(err, errSize, "Invalid tray autostart readback arguments");
        return false;
    }

    HKEY key = nullptr;
    LONG status = RegOpenKeyExW(HKEY_CURRENT_USER, TRAY_AUTOSTART_RUN_KEY, 0,
        KEY_QUERY_VALUE, &key);
    if (status == ERROR_FILE_NOT_FOUND) return true;
    if (status != ERROR_SUCCESS) {
        set_message(err, errSize, "Could not open the per-user startup registry key (error %ld)",
            status);
        return false;
    }

    DWORD type = 0;
    DWORD bytes = (DWORD)(valueOutCount * sizeof(WCHAR));
    status = RegQueryValueExW(key, TRAY_AUTOSTART_VALUE_NAME, nullptr, &type,
        reinterpret_cast<BYTE*>(valueOut), &bytes);
    RegCloseKey(key);
    if (status == ERROR_FILE_NOT_FOUND) return true;
    if (status != ERROR_SUCCESS) {
        set_message(err, errSize, "Could not read the per-user tray startup entry (error %ld)",
            status);
        return false;
    }
    if (type != REG_SZ || bytes < sizeof(WCHAR) ||
        valueOut[(bytes / sizeof(WCHAR)) - 1] != 0) {
        set_message(err, errSize, "The per-user tray startup entry has an invalid type or value");
        return false;
    }
    *existsOut = true;
    return true;
}

static bool set_tray_autostart_enabled_for_config(const char* configPath,
    bool enabled, char* err, size_t errSize) {
    if (err && errSize) err[0] = 0;

    WCHAR expected[2 * MAX_PATH + 64] = {};
    if (enabled && !build_tray_autostart_command(configPath, expected,
            ARRAY_COUNT(expected), err, errSize)) {
        return false;
    }

    HKEY key = nullptr;
    LONG status = enabled
        ? RegCreateKeyExW(HKEY_CURRENT_USER, TRAY_AUTOSTART_RUN_KEY, 0, nullptr,
              REG_OPTION_NON_VOLATILE, KEY_QUERY_VALUE | KEY_SET_VALUE, nullptr,
              &key, nullptr)
        : RegOpenKeyExW(HKEY_CURRENT_USER, TRAY_AUTOSTART_RUN_KEY, 0,
              KEY_QUERY_VALUE | KEY_SET_VALUE, &key);
    if (!enabled && status == ERROR_FILE_NOT_FOUND) {
        debug_log("tray autostart: already absent\n");
        return true;
    }
    if (status != ERROR_SUCCESS) {
        set_message(err, errSize, "Could not open the per-user startup registry key (error %ld)",
            status);
        return false;
    }

    if (enabled) {
        const DWORD bytes = (DWORD)((wcslen(expected) + 1) * sizeof(WCHAR));
        status = RegSetValueExW(key, TRAY_AUTOSTART_VALUE_NAME, 0, REG_SZ,
            reinterpret_cast<const BYTE*>(expected), bytes);
    } else {
        status = RegDeleteValueW(key, TRAY_AUTOSTART_VALUE_NAME);
        if (status == ERROR_FILE_NOT_FOUND) status = ERROR_SUCCESS;
    }
    RegCloseKey(key);
    if (status != ERROR_SUCCESS) {
        set_message(err, errSize, "%s the per-user tray startup entry failed (error %ld)",
            enabled ? "Writing" : "Removing", status);
        return false;
    }

    WCHAR actual[2 * MAX_PATH + 64] = {};
    bool exists = false;
    if (!query_tray_autostart_value(actual, ARRAY_COUNT(actual), &exists,
            err, errSize)) {
        return false;
    }
    if (enabled ? (!exists || wcscmp(actual, expected) != 0) : exists) {
        set_message(err, errSize,
            "The per-user tray startup entry did not match after %s",
            enabled ? "write" : "removal");
        return false;
    }

    debug_log("tray autostart: %s verified (separate --tray-start process)\n",
        enabled ? "enabled" : "disabled");
    return true;
}

static bool sync_tray_autostart_from_config(const char* configPath,
    char* err, size_t errSize) {
    ConfigEnablementState state = tray_autostart_config_state(configPath);
    if (state == CONFIG_ENABLEMENT_INDETERMINATE) {
        debug_log("tray autostart: config read is indeterminate; preserving existing HKCU Run value\n");
        return true;
    }
    return set_tray_autostart_enabled_for_config(configPath,
        state == CONFIG_ENABLEMENT_ENABLED, err, errSize);
}

#endif
