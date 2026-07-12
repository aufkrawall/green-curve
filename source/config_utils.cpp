// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT

#include "app_shared.h"

void trim_ascii(char* s) {
    if (!s) return;

    int len = (int)strlen(s);
    int start = 0;
    while (start < len && (unsigned char)s[start] <= ' ') start++;
    int end = len;
    while (end > start && (unsigned char)s[end - 1] <= ' ') end--;

    if (start > 0 && end > start) {
        memmove(s, s + start, (size_t)(end - start));
    }

    if (end <= start) {
        s[0] = 0;
    } else {
        s[end - start] = 0;
    }
}

bool streqi_ascii(const char* a, const char* b) {
    if (!a || !b) return false;
    while (*a && *b) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return false;
        ++a;
        ++b;
    }
    return *a == 0 && *b == 0;
}

bool parse_int_strict(const char* s, int* out) {
    if (!s || !*s || !out) return false;

    char* end = nullptr;
    errno = 0;
    long v = strtol(s, &end, 10);
    if (errno == ERANGE) return false;
    if (!end || *end != 0) return false;
    if (v < -2147483647L - 1L || v > 2147483647L) return false;

    *out = (int)v;
    return true;
}

bool parse_cli_point_arg_w(const WCHAR* arg, int* pointIndexOut) {
    if (pointIndexOut) *pointIndexOut = -1;
    if (!arg || wcsncmp(arg, L"--point", 7) != 0) return false;

    const WCHAR* digits = arg + 7;
    if (!*digits) return false;

    int value = 0;
    for (const WCHAR* p = digits; *p; ++p) {
        if (*p < L'0' || *p > L'9') return false;
        value = value * 10 + (int)(*p - L'0');
        if (value >= VF_NUM_POINTS) return false;
    }

    if (pointIndexOut) *pointIndexOut = value;
    return true;
}

bool gpu_family_uses_best_guess_backend(GpuFamily family) {
    return family == GPU_FAMILY_UNKNOWN;
}

void set_message(char* dst, size_t dstSize, const char* fmt, ...) {
    if (!dst || dstSize == 0) return;

    va_list ap;
    va_start(ap, fmt);
    StringCchVPrintfA(dst, dstSize, fmt, ap);
    va_end(ap);
    dst[dstSize - 1] = 0;
}

bool parse_fan_value(const char* text, bool* isAuto, int* pct) {
    if (!isAuto || !pct) return false;

    char buf[64] = {};
    if (text) StringCchCopyA(buf, ARRAY_COUNT(buf), text);
    trim_ascii(buf);
    if (buf[0] == 0 || streqi_ascii(buf, "auto")) {
        *isAuto = true;
        *pct = 0;
        return true;
    }

    int value = 0;
    if (!parse_int_strict(buf, &value)) return false;
    if (value < 0 || value > 100) return false;

    *isAuto = false;
    *pct = value;
    return true;
}

static HANDLE g_configInterprocessMutex = nullptr;

// The GUI runs in an interactive WTS session while the service runs in session
// 0.  A Local\\ mutex silently creates one unrelated lock per session, so it
// cannot serialize their access to the same per-user INI file.  Use one Global
// object and grant every authenticated caller only the two mutex rights it
// needs: wait (SYNCHRONIZE) and release (MUTEX_MODIFY_STATE).  The explicit
// medium integrity label lets the unelevated GUI open an object first created by
// the SYSTEM service without widening the DACL.
static const char* const CONFIG_INTERPROCESS_MUTEX_NAME =
    "Global\\GreenCurveConfigMutex-v2";
static const DWORD CONFIG_INTERPROCESS_MUTEX_ACCESS =
    SYNCHRONIZE | MUTEX_MODIFY_STATE;
static const char* const CONFIG_INTERPROCESS_MUTEX_SDDL =
    "D:P"
    "(A;;0x00100001;;;SY)"
    "(A;;0x00100001;;;BA)"
    "(A;;0x00100001;;;AU)"
    "S:(ML;;NW;;;ME)";

static void log_config_mutex_failure(const char* operation, DWORD error) {
    char warning[192] = {};
    StringCchPrintfA(warning, ARRAY_COUNT(warning),
        "[GreenCurve] ERROR: config mutex %s failed (error %lu)\n",
        operation && operation[0] ? operation : "operation",
        (unsigned long)error);
    OutputDebugStringA(warning);
}

static HANDLE ensure_config_interprocess_mutex() {
    if (g_configInterprocessMutex) return g_configInterprocessMutex;

    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorA(
            CONFIG_INTERPROCESS_MUTEX_SDDL, SDDL_REVISION_1,
            &descriptor, nullptr)) {
        log_config_mutex_failure("security descriptor creation", GetLastError());
        return nullptr;
    }

    SECURITY_ATTRIBUTES attributes = {};
    attributes.nLength = sizeof(attributes);
    attributes.lpSecurityDescriptor = descriptor;
    attributes.bInheritHandle = FALSE;
    HANDLE mutex = CreateMutexExA(&attributes,
        CONFIG_INTERPROCESS_MUTEX_NAME, 0,
        CONFIG_INTERPROCESS_MUTEX_ACCESS);
    DWORD createError = mutex ? ERROR_SUCCESS : GetLastError();
    LocalFree(descriptor);
    if (!mutex) {
        log_config_mutex_failure("creation/open", createError);
        return nullptr;
    }
    g_configInterprocessMutex = mutex;
    return g_configInterprocessMutex;
}

bool enter_config_storage_lock(HANDLE* acquiredMutex) {
    if (!acquiredMutex) return false;
    *acquiredMutex = nullptr;
    EnterCriticalSection(&g_configLock);

    HANDLE mutex = ensure_config_interprocess_mutex();
    if (!mutex) {
        LeaveCriticalSection(&g_configLock);
        return false;
    }

    const DWORD CONFIG_MUTEX_TIMEOUT_MS = 5000;
    DWORD waitResult = WaitForSingleObject(mutex, CONFIG_MUTEX_TIMEOUT_MS);
    if (waitResult == WAIT_TIMEOUT) {
        char warning[128] = {};
        StringCchPrintfA(warning, ARRAY_COUNT(warning),
            "[GreenCurve] ERROR: config mutex timed out after %lu ms\n", CONFIG_MUTEX_TIMEOUT_MS);
        OutputDebugStringA(warning);
    } else if (waitResult == WAIT_FAILED) {
        log_config_mutex_failure("wait", GetLastError());
    } else if (waitResult != WAIT_OBJECT_0 && waitResult != WAIT_ABANDONED) {
        log_config_mutex_failure("wait", waitResult);
    }
    if (waitResult == WAIT_OBJECT_0 || waitResult == WAIT_ABANDONED) {
        if (waitResult == WAIT_ABANDONED) {
            // Ownership IS acquired on WAIT_ABANDONED; the flag just means a
            // peer process died while holding the mutex (possible torn
            // config write) — log it for diagnosis.
            OutputDebugStringA("[GreenCurve] WARNING: config mutex was abandoned by a dying peer process; continuing with ownership\n");
        }
        *acquiredMutex = mutex;
        return true;
    }

    LeaveCriticalSection(&g_configLock);
    return false;
}

void leave_config_storage_lock(HANDLE acquiredMutex) {
    if (acquiredMutex) ReleaseMutex(acquiredMutex);
    LeaveCriticalSection(&g_configLock);
}

bool config_section_has_keys(const char* path, const char* section) {
    if (!path || !section) return false;

    char keys[256] = {};
    HANDLE configMutex = nullptr;
    if (!enter_config_storage_lock(&configMutex)) return false;
    DWORD n = GetPrivateProfileStringA(section, nullptr, "", keys, ARRAY_COUNT(keys), path);
    leave_config_storage_lock(configMutex);
    return n > 0 && keys[0] != 0;
}

int get_config_int(const char* path, const char* section, const char* key, int defaultVal) {
    if (!path || !section || !key) return defaultVal;

    char buf[32] = {};
    HANDLE configMutex = nullptr;
    if (!enter_config_storage_lock(&configMutex)) return defaultVal;
    DWORD n = GetPrivateProfileStringA(section, key, "", buf, sizeof(buf), path);
    leave_config_storage_lock(configMutex);
    if (n >= sizeof(buf) - 1) return defaultVal;
    trim_ascii(buf);
    if (!buf[0]) return defaultVal;

    int value = 0;
    if (!parse_int_strict(buf, &value)) return defaultVal;
    return value;
}

bool set_config_int(const char* path, const char* section, const char* key, int value) {
    if (!path || !section || !key) return false;

    char buf[32] = {};
    StringCchPrintfA(buf, ARRAY_COUNT(buf), "%d", value);
    HANDLE configMutex = nullptr;
    if (!enter_config_storage_lock(&configMutex)) return false;
    bool ok = WritePrivateProfileStringA(section, key, buf, path) != FALSE;
    if (ok) {
        // Force the Win32 profile cache to the backing file before claiming the
        // preference was saved, then verify the exact value while the
        // cross-session lock still excludes competing writers.
        (void)WritePrivateProfileStringA(nullptr, nullptr, nullptr, path);
        char readback[32] = {};
        DWORD length = GetPrivateProfileStringA(
            section, key, "", readback, ARRAY_COUNT(readback), path);
        int parsed = 0;
        ok = length > 0 && length < ARRAY_COUNT(readback) - 1 &&
            parse_int_strict(readback, &parsed) && parsed == value;
    }
    leave_config_storage_lock(configMutex);
    if (ok) invalidate_tray_profile_cache();
    return ok;
}

bool enter_config_storage_lock_interruptible(HANDLE cancelEvent,
    HANDLE* acquiredMutex) {
    if (!acquiredMutex) return false;
    *acquiredMutex = nullptr;
    EnterCriticalSection(&g_configLock);
    HANDLE mutex = ensure_config_interprocess_mutex();
    if (!mutex) {
        LeaveCriticalSection(&g_configLock);
        return false;
    }
    HANDLE waits[2] = { mutex, cancelEvent };
    DWORD waitCount = cancelEvent ? 2u : 1u;
    DWORD wait = WaitForMultipleObjects(waitCount, waits, FALSE, INFINITE);
    if (wait == WAIT_OBJECT_0 || wait == WAIT_ABANDONED_0) {
        if (wait == WAIT_ABANDONED_0) {
            OutputDebugStringA("[GreenCurve] WARNING: config mutex was abandoned; caller must validate the complete transaction\n");
        }
        *acquiredMutex = mutex;
        return true;
    }
    LeaveCriticalSection(&g_configLock);
    return false;
}

static bool profiles_section_entry_has_key(const char* entry, const char* key) {
    if (!entry || !key) return false;
    const char* equals = strchr(entry, '=');
    if (!equals) return false;
    const char* keyEnd = equals;
    while (keyEnd > entry && (keyEnd[-1] == ' ' || keyEnd[-1] == '\t')) --keyEnd;
    const char* keyBegin = entry;
    while (keyBegin < keyEnd && (*keyBegin == ' ' || *keyBegin == '\t')) ++keyBegin;
    size_t actualLength = (size_t)(keyEnd - keyBegin);
    size_t expectedLength = strlen(key);
    return actualLength == expectedLength && _strnicmp(keyBegin, key, expectedLength) == 0;
}

// Win32 INI section names are case-insensitive.  The direct whole-file
// rewriter must use the same comparison rule or an existing [Profiles] section
// will survive beside a newly appended [profiles] section; the profile APIs
// then continue reading the stale first copy.
bool config_section_header_matches_ascii(const char* line, const char* section) {
    if (!line || !section || line[0] != '[') return false;
    size_t length = strlen(section);
    if (_strnicmp(line + 1, section, length) != 0) return false;
    return line[1 + length] == ']';
}

bool update_logon_profile_selection_transaction(const char* path,
    int perUserSlot, int sharedSlot, ConfigProfilesSectionCommitFn commit,
    void* context, char* err, size_t errSize) {
    if (err && errSize) err[0] = 0;
    if (!path || !path[0] || !commit ||
        perUserSlot < 0 || perUserSlot > CONFIG_NUM_SLOTS ||
        sharedSlot < 0 || sharedSlot > CONFIG_NUM_SLOTS ||
        (perUserSlot > 0 && sharedSlot > 0)) {
        set_message(err, errSize, "Invalid logon profile selection transaction");
        return false;
    }

    HANDLE configMutex = nullptr;
    if (!enter_config_storage_lock(&configMutex)) {
        set_message(err, errSize, "Failed to acquire the config lock for logon profile selection");
        return false;
    }

    const DWORD sectionCapacity = 32768;
    char* existing = (char*)calloc(sectionCapacity, 1);
    if (!existing) {
        leave_config_storage_lock(configMutex);
        set_message(err, errSize, "Out of memory reading the profiles section");
        return false;
    }
    DWORD sectionLength = GetPrivateProfileSectionA("profiles", existing, sectionCapacity, path);
    if (sectionLength >= sectionCapacity - 2) {
        free(existing);
        leave_config_storage_lock(configMutex);
        set_message(err, errSize, "The profiles section is too large to update safely");
        return false;
    }

    // Multi-string NUL separators become CRLF pairs, so allow up to twice the
    // section size plus the replacement keys.
    size_t outputCapacity = (size_t)sectionLength * 2 + 256;
    char* output = (char*)calloc(outputCapacity, 1);
    if (!output) {
        free(existing);
        leave_config_storage_lock(configMutex);
        set_message(err, errSize, "Out of memory building the profiles transaction");
        return false;
    }
    size_t outputUsed = 0;
    auto append_text = [&](const char* text) -> bool {
        size_t length = text ? strlen(text) : 0;
        if (!text || outputUsed + length + 1 > outputCapacity) return false;
        memcpy(output + outputUsed, text, length);
        outputUsed += length;
        output[outputUsed] = 0;
        return true;
    };

    bool built = append_text("[profiles]\r\n");
    for (const char* entry = existing; built && *entry; entry += strlen(entry) + 1) {
        if (profiles_section_entry_has_key(entry, "logon_slot") ||
            profiles_section_entry_has_key(entry, "logon_shared_slot")) {
            continue;
        }
        built = append_text(entry) && append_text("\r\n");
    }
    char selection[96] = {};
    StringCchPrintfA(selection, ARRAY_COUNT(selection),
        "logon_slot=%d\r\nlogon_shared_slot=%d\r\n\r\n", perUserSlot, sharedSlot);
    built = built && append_text(selection);
    free(existing);

    bool committed = false;
    if (built) {
        committed = commit(path, output, context, err, errSize);
    } else {
        set_message(err, errSize, "The profiles transaction buffer was unexpectedly too small");
    }
    free(output);

    bool verified = false;
    if (committed) {
        // write_config_sections_atomic replaces the file directly instead of
        // going through the profile APIs.  Flush their per-process cache while
        // the cross-session lock is still held so locked readback cannot observe
        // the section image from before the atomic rename.
        // This special all-null call returns zero when it *successfully flushes*
        // the cache as well as on failure, so its return value is intentionally
        // not interpreted.  The exact two-key readback below is the fail-closed
        // success criterion.
        (void)WritePrivateProfileStringA(nullptr, nullptr, nullptr, path);
        char perUserText[32] = {};
        char sharedText[32] = {};
        DWORD perUserLength = GetPrivateProfileStringA(
            "profiles", "logon_slot", "", perUserText, ARRAY_COUNT(perUserText), path);
        DWORD sharedLength = GetPrivateProfileStringA(
            "profiles", "logon_shared_slot", "", sharedText, ARRAY_COUNT(sharedText), path);
        int readPerUser = -1;
        int readShared = -1;
        verified = perUserLength > 0 && perUserLength < ARRAY_COUNT(perUserText) - 1 &&
            sharedLength > 0 && sharedLength < ARRAY_COUNT(sharedText) - 1 &&
            parse_int_strict(perUserText, &readPerUser) &&
            parse_int_strict(sharedText, &readShared) &&
            readPerUser == perUserSlot && readShared == sharedSlot;
        if (!verified) {
            set_message(err, errSize,
                "Logon profile selection was committed but failed locked readback verification");
        }
    }
    leave_config_storage_lock(configMutex);
    return verified;
}

int logon_profile_selection_item_data(int perUserSlot, int sharedSlot) {
    if (sharedSlot >= 1 && sharedSlot <= CONFIG_NUM_SLOTS) {
        return LOGON_COMBO_SHARED_FLAG | sharedSlot;
    }
    if (perUserSlot >= 1 && perUserSlot <= CONFIG_NUM_SLOTS) return perUserSlot;
    return 0;
}

int applied_user_slot_from_service_profile(ServiceProfileSource source,
    unsigned int slot) {
    if (source != SERVICE_PROFILE_SOURCE_USER_SLOT ||
        slot < 1 || slot > CONFIG_NUM_SLOTS) {
        return 0;
    }
    return (int)slot;
}

// Read a free-form string value.  Returns true when a non-truncated value was
// found (out holds the trimmed value); false leaves out set to defaultVal (or
// empty when defaultVal is null).  Unlike get_config_int this can carry exe
// names, window-title/class patterns, and hotkey strings for the auto-profile
// feature, which do not fit the 32-byte integer buffer.
bool get_config_string(const char* path, const char* section, const char* key,
                       const char* defaultVal, char* out, size_t outSize) {
    if (!out || outSize == 0) return false;
    out[0] = 0;
    if (defaultVal) StringCchCopyA(out, outSize, defaultVal);
    if (!path || !section || !key) return false;

    // GetPrivateProfileString truncates silently; read into a generous scratch
    // buffer and report truncation rather than persisting a half value.
    char buf[512] = {};
    HANDLE configMutex = nullptr;
    if (!enter_config_storage_lock(&configMutex)) return false;
    DWORD n = GetPrivateProfileStringA(section, key, "", buf, sizeof(buf), path);
    leave_config_storage_lock(configMutex);
    if (n == 0) return false;                 // key absent → keep defaultVal
    if (n >= sizeof(buf) - 1) return false;   // truncated in scratch → treat as absent
    trim_ascii(buf);
    if (!buf[0]) return false;
    if (strlen(buf) >= outSize) return false; // would truncate in caller buffer
    StringCchCopyA(out, outSize, buf);
    return true;
}

bool set_config_string(const char* path, const char* section, const char* key, const char* value) {
    if (!path || !section || !key) return false;
    HANDLE configMutex = nullptr;
    if (!enter_config_storage_lock(&configMutex)) return false;
    // A null/empty value deletes the key (WritePrivateProfileString with a null
    // value string removes the entry) so cleared patterns do not linger.
    const char* persistedValue = (value && value[0]) ? value : nullptr;
    bool ok = WritePrivateProfileStringA(section, key, persistedValue, path) != FALSE;
    if (ok) {
        (void)WritePrivateProfileStringA(nullptr, nullptr, nullptr, path);
        char readback[512] = {};
        DWORD length = GetPrivateProfileStringA(
            section, key, "", readback, ARRAY_COUNT(readback), path);
        ok = persistedValue
            ? (length < ARRAY_COUNT(readback) - 1 && strcmp(readback, persistedValue) == 0)
            : (length == 0);
    }
    leave_config_storage_lock(configMutex);
    if (ok) invalidate_tray_profile_cache();
    return ok;
}

static bool parse_config_uint_hex_locked(const char* path, const char* section,
    const char* key, unsigned int* valueOut) {
    if (!path || !section || !key || !valueOut) return false;
    char text[32] = {};
    DWORD length = GetPrivateProfileStringA(
        section, key, "", text, ARRAY_COUNT(text), path);
    if (length == 0 || length >= ARRAY_COUNT(text) - 1) return false;
    trim_ascii(text);
    if (!text[0]) return false;
    size_t digitCount = strlen(text);
    if (digitCount == 0 || digitCount > 8) return false;
    for (size_t i = 0; i < digitCount; ++i) {
        if (!isxdigit((unsigned char)text[i])) return false;
    }
    char* end = nullptr;
    errno = 0;
    unsigned long value = strtoul(text, &end, 16);
    if (errno == ERANGE || !end || *end != 0) return false;
    *valueOut = (unsigned int)value;
    return true;
}

static bool read_config_int_value_locked(const char* path, const char* section,
    const char* key, int defaultValue, int* valueOut) {
    if (!path || !section || !key || !valueOut) return false;
    char text[32] = {};
    DWORD length = GetPrivateProfileStringA(
        section, key, "", text, ARRAY_COUNT(text), path);
    if (length >= ARRAY_COUNT(text) - 1) return false;
    trim_ascii(text);
    if (!text[0]) {
        *valueOut = defaultValue;
        return true;
    }
    return parse_int_strict(text, valueOut);
}

bool load_configured_gpu_selection_from_section(const char* path,
    const char* section, ConfiguredGpuSelection* selectionOut,
    char* err, size_t errSize) {
    if (err && errSize) err[0] = 0;
    if (!path || !path[0] || !section || !section[0] || !selectionOut) {
        set_message(err, errSize, "Invalid configured GPU selection arguments");
        return false;
    }
    memset(selectionOut, 0, sizeof(*selectionOut));
    HANDLE configMutex = nullptr;
    if (!enter_config_storage_lock(&configMutex)) {
        set_message(err, errSize, "Failed to acquire the config lock for GPU selection");
        return false;
    }

    int selectedIndex = 0;
    int identityVersion = 0;
    bool ok = read_config_int_value_locked(path, section, "selected_index", 0,
            &selectedIndex) && selectedIndex >= 0 &&
        selectedIndex < MAX_GPU_ADAPTERS &&
        read_config_int_value_locked(path, section, "selected_identity_version", 0,
            &identityVersion) && identityVersion >= 0 && identityVersion <= 1;
    selectionOut->legacyIndex = ok ? (unsigned int)selectedIndex : 0;

    if (ok && identityVersion == 1) {
        int bdfValid = 0;
        int domain = 0, bus = 0, device = 0, function = 0;
        GpuAdapterInfo identity = {};
        ok = parse_config_uint_hex_locked(path, section, "selected_device_id", &identity.deviceId) &&
            parse_config_uint_hex_locked(path, section, "selected_subsystem_id", &identity.subSystemId) &&
            parse_config_uint_hex_locked(path, section, "selected_revision_id", &identity.pciRevisionId) &&
            parse_config_uint_hex_locked(path, section, "selected_ext_device_id", &identity.extDeviceId) &&
            read_config_int_value_locked(path, section, "selected_bdf_valid", 0, &bdfValid) &&
            (bdfValid == 0 || bdfValid == 1) && identity.deviceId != 0;
        if (ok && bdfValid) {
            ok = read_config_int_value_locked(path, section, "selected_pci_domain", 0, &domain) &&
                read_config_int_value_locked(path, section, "selected_pci_bus", 0, &bus) &&
                read_config_int_value_locked(path, section, "selected_pci_device", 0, &device) &&
                read_config_int_value_locked(path, section, "selected_pci_function", 0, &function) &&
                domain >= 0 && domain <= 0xFFFF && bus >= 0 && bus <= 255 &&
                device >= 0 && device <= 31 && function >= 0 && function <= 7 &&
                (domain != 0 || bus != 0 || device != 0 || function != 0);
            if (ok) {
                identity.pciDomain = (unsigned int)domain;
                identity.pciBus = (unsigned int)bus;
                identity.pciDevice = (unsigned int)device;
                identity.pciFunction = (unsigned int)function;
            }
        }
        if (ok) {
            identity.valid = true;
            identity.pciInfoValid = true;
            selectionOut->stableIdentityPresent = true;
            selectionOut->identity = identity;
        }
    }
    leave_config_storage_lock(configMutex);
    if (!ok) {
        memset(selectionOut, 0, sizeof(*selectionOut));
        set_message(err, errSize,
            "Configured GPU selection is malformed or outside supported bounds");
    }
    return ok;
}

bool load_configured_gpu_selection(const char* path,
    ConfiguredGpuSelection* selectionOut, char* err, size_t errSize) {
    return load_configured_gpu_selection_from_section(
        path, "gpu", selectionOut, err, errSize);
}

bool format_configured_gpu_selection_section(const char* section,
    const ConfiguredGpuSelection* selection, char* out, size_t outSize,
    char* err, size_t errSize) {
    if (err && errSize) err[0] = 0;
    if (!section || !section[0] || !selection || !out || outSize == 0 ||
        strchr(section, '[') || strchr(section, ']') ||
        selection->legacyIndex >= MAX_GPU_ADAPTERS) {
        set_message(err, errSize, "Invalid configured GPU section arguments");
        return false;
    }
    const GpuAdapterInfo& identity = selection->identity;
    bool stable = selection->stableIdentityPresent;
    bool bdf = stable && identity.pciInfoValid &&
        identity.pciDomain <= 0xFFFFu && identity.pciBus <= 255u &&
        identity.pciDevice <= 31u && identity.pciFunction <= 7u &&
        (identity.pciDomain != 0 || identity.pciBus != 0 ||
         identity.pciDevice != 0 || identity.pciFunction != 0);
    if (stable && (!identity.valid || !identity.pciInfoValid ||
        identity.deviceId == 0)) {
        set_message(err, errSize, "Configured GPU stable identity is incomplete");
        return false;
    }
    HRESULT result = StringCchPrintfA(out, outSize,
        "[%s]\r\n"
        "selected_index=%u\r\n"
        "selected_identity_version=%d\r\n"
        "selected_device_id=%08X\r\n"
        "selected_subsystem_id=%08X\r\n"
        "selected_revision_id=%08X\r\n"
        "selected_ext_device_id=%08X\r\n"
        "selected_bdf_valid=%d\r\n"
        "selected_pci_domain=%u\r\n"
        "selected_pci_bus=%u\r\n"
        "selected_pci_device=%u\r\n"
        "selected_pci_function=%u\r\n\r\n",
        section, selection->legacyIndex, stable ? 1 : 0,
        stable ? identity.deviceId : 0u,
        stable ? identity.subSystemId : 0u,
        stable ? identity.pciRevisionId : 0u,
        stable ? identity.extDeviceId : 0u,
        bdf ? 1 : 0,
        bdf ? identity.pciDomain : 0u,
        bdf ? identity.pciBus : 0u,
        bdf ? identity.pciDevice : 0u,
        bdf ? identity.pciFunction : 0u);
    if (FAILED(result)) {
        if (outSize) out[0] = 0;
        set_message(err, errSize, "Configured GPU section serialization overflow");
        return false;
    }
    return true;
}
