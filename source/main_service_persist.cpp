// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// Service restart/recovery persistence split out of main_service_runtime.cpp.
// The awake-time proof clock and consolidated spam ledger live in focused
// shards included immediately before this file.

// ---- Driver-restart recovery persistence ----
// Recovery after a GPU device reconnect, TDR, or driver upgrade is performed by
// restarting the service PROCESS (see launch_recovery_thread / request_service_
// restart).  Before exiting, the service snapshots the active desired OC/fan
// profile to disk; the freshly relaunched process loads clean driver DLLs and
// service_startup_coordinator_thread_proc() may re-apply the snapshot only when
// the new process presents the nonce issued to the protected restart helper.
// Ordinary service startup is always non-mutating.

#define SERVICE_ACTIVE_DESIRED_MAGIC   0x47434144u /* 'GCAD' */
#define SERVICE_ACTIVE_DESIRED_VERSION 5u
#define SERVICE_ACTIVE_DESIRED_LEGACY_VERSION 4u
#define SERVICE_CONTROLLED_RECOVERY_MAGIC   0x47434352u /* 'GCCR' */
#define SERVICE_CONTROLLED_RECOVERY_VERSION 3u
#define SERVICE_CONTROLLED_RECOVERY_MAX_AGE_MS 300000ULL
#define SERVICE_CONTROLLED_RECOVERY_NONCE_BYTES 32u
#define SERVICE_CONTROLLED_RECOVERY_NONCE_HEX_CHARS (SERVICE_CONTROLLED_RECOVERY_NONCE_BYTES * 2u)
#define SERVICE_CONTROLLED_RECOVERY_HELPER_VALIDATION_MAGIC 0x47434856u /* 'GCHV' */

struct ServiceRestartReapplySnapshot {
    DesiredSettings desired;
    GpuAdapterInfo targetGpu;
    DWORD activeProfileSource;
    DWORD activeProfileSlot;
    DWORD reserved[2];
};

struct ServiceControlledRecoveryAuthorization {
    DWORD magic;
    DWORD version;
    DWORD size;
    DWORD reserved;
    BYTE nonce[SERVICE_CONTROLLED_RECOVERY_NONCE_BYTES];
    ServiceRecoveryEvidenceKey evidenceKey;
    DWORD previousProcessId;
    DWORD reserved2;
    ULONGLONG previousProcessCreationTime100ns;
    ServiceBootIdentity bootIdentity;
    ULONGLONG createdUptimeMs;
    ULONGLONG snapshotFingerprint;
    ULONGLONG snapshotLastWriteTime100ns;
    DWORD snapshotSize;
    DWORD reserved3;
    DWORD helperValidationMagic;
    DWORD helperProcessId;
    ULONGLONG helperProcessCreationTime100ns;
};

static bool service_active_desired_persist_path(char* out, size_t outSize) {
    if (!out || outSize == 0) return false;
    char dir[MAX_PATH] = {};
    if (!resolve_service_machine_data_dir(dir, sizeof(dir))) return false;
    return SUCCEEDED(StringCchPrintfA(out, outSize, "%s\\service_restart_reapply.bin", dir));
}

// A restart snapshot alone is deliberately not permission to replay settings.
// The authorization is bound to a cryptographically random nonce, the exact
// previous process identity, the current boot, and a short awake-time window.
static bool service_controlled_recovery_authorization_path(char* out, size_t outSize) {
    if (!out || outSize == 0) return false;
    char dir[MAX_PATH] = {};
    if (!resolve_service_machine_data_dir(dir, sizeof(dir))) return false;
    return SUCCEEDED(StringCchPrintfA(out, outSize,
        "%s\\service_controlled_recovery_authorization.bin", dir));
}

static bool service_commit_controlled_recovery_authorization(
    const ServiceControlledRecoveryAuthorization& authorization) {
    char path[MAX_PATH] = {};
    if (!service_controlled_recovery_authorization_path(path, sizeof(path))) return false;
    char pathErr[256] = {};
    if (!ensure_parent_directory_for_file(path, pathErr, sizeof(pathErr))) return false;
    char tempPath[MAX_PATH] = {};
    if (FAILED(StringCchPrintfA(tempPath, ARRAY_COUNT(tempPath), "%s.tmp.%lu.%llu", path,
            (unsigned long)GetCurrentProcessId(),
            (unsigned long long)GetTickCount64()))) return false;
    HANDLE h = gc_CreateFileUtf8(tempPath, GENERIC_WRITE, 0, nullptr, CREATE_NEW,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    bool ok = WriteFile(h, &authorization, sizeof(authorization), &written, nullptr) &&
        written == sizeof(authorization) && FlushFileBuffers(h) != FALSE;
    CloseHandle(h);
    if (ok) ok = gc_MoveFileExUtf8(tempPath, path,
        MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
    if (!ok) gc_DeleteFileUtf8(tempPath);
    return ok;
}

static bool service_format_controlled_recovery_nonce(const BYTE* nonce,
    char* out, size_t outSize) {
    static const char digits[] = "0123456789abcdef";
    if (!nonce || !out || outSize <= SERVICE_CONTROLLED_RECOVERY_NONCE_HEX_CHARS) return false;
    for (size_t i = 0; i < SERVICE_CONTROLLED_RECOVERY_NONCE_BYTES; ++i) {
        out[i * 2] = digits[(nonce[i] >> 4) & 0x0f];
        out[i * 2 + 1] = digits[nonce[i] & 0x0f];
    }
    out[SERVICE_CONTROLLED_RECOVERY_NONCE_HEX_CHARS] = 0;
    return true;
}

static bool service_parse_controlled_recovery_nonce(const char* text, BYTE* nonceOut) {
    if (!text || !nonceOut || strlen(text) != SERVICE_CONTROLLED_RECOVERY_NONCE_HEX_CHARS) return false;
    memset(nonceOut, 0, SERVICE_CONTROLLED_RECOVERY_NONCE_BYTES);
    for (size_t i = 0; i < SERVICE_CONTROLLED_RECOVERY_NONCE_BYTES; ++i) {
        int hi = text[i * 2];
        int lo = text[i * 2 + 1];
        hi = (hi >= '0' && hi <= '9') ? hi - '0' :
            (hi >= 'a' && hi <= 'f') ? hi - 'a' + 10 :
            (hi >= 'A' && hi <= 'F') ? hi - 'A' + 10 : -1;
        lo = (lo >= '0' && lo <= '9') ? lo - '0' :
            (lo >= 'a' && lo <= 'f') ? lo - 'a' + 10 :
            (lo >= 'A' && lo <= 'F') ? lo - 'A' + 10 : -1;
        if (hi < 0 || lo < 0) {
            memset(nonceOut, 0, SERVICE_CONTROLLED_RECOVERY_NONCE_BYTES);
            return false;
        }
        nonceOut[i] = (BYTE)((hi << 4) | lo);
    }
    return true;
}

static bool service_controlled_recovery_nonce_equal(const BYTE* a, const BYTE* b) {
    if (!a || !b) return false;
    unsigned int difference = 0;
    for (size_t i = 0; i < SERVICE_CONTROLLED_RECOVERY_NONCE_BYTES; ++i) {
        difference |= (unsigned int)(a[i] ^ b[i]);
    }
    return difference == 0;
}

static bool service_query_process_creation_time_100ns(HANDLE process, ULONGLONG* out) {
    if (!process || !out) return false;
    *out = 0;
    FILETIME creation = {}, exit = {}, kernel = {}, user = {};
    if (!GetProcessTimes(process, &creation, &exit, &kernel, &user)) return false;
    ULARGE_INTEGER value = {};
    value.LowPart = creation.dwLowDateTime;
    value.HighPart = creation.dwHighDateTime;
    if (value.QuadPart == 0) return false;
    *out = value.QuadPart;
    return true;
}

static bool service_fingerprint_restart_snapshot(ULONGLONG* fingerprintOut,
    ULONGLONG* lastWriteTimeOut, DWORD* sizeOut) {
    if (!fingerprintOut || !lastWriteTimeOut || !sizeOut) return false;
    *fingerprintOut = 0;
    *lastWriteTimeOut = 0;
    *sizeOut = 0;
    char path[MAX_PATH] = {};
    if (!service_active_desired_persist_path(path, sizeof(path))) return false;
    HANDLE h = gc_CreateFileUtf8(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size = {};
    FILETIME lastWrite = {};
    bool ok = GetFileSizeEx(h, &size) != FALSE && size.QuadPart > 0 &&
        size.QuadPart <= 1024 * 1024 &&
        GetFileTime(h, nullptr, nullptr, &lastWrite) != FALSE;
    ULONGLONG fingerprint = 1469598103934665603ULL; // FNV-1a 64
    BYTE buffer[4096] = {};
    ULONGLONG total = 0;
    while (ok) {
        DWORD read = 0;
        if (!ReadFile(h, buffer, sizeof(buffer), &read, nullptr)) {
            ok = false;
            break;
        }
        if (read == 0) break;
        total += read;
        for (DWORD i = 0; i < read; ++i) {
            fingerprint ^= buffer[i];
            fingerprint *= 1099511628211ULL;
        }
    }
    CloseHandle(h);
    ULARGE_INTEGER writeValue = {};
    writeValue.LowPart = lastWrite.dwLowDateTime;
    writeValue.HighPart = lastWrite.dwHighDateTime;
    if (!ok || total != (ULONGLONG)size.QuadPart || total > MAXDWORD ||
        fingerprint == 0 || writeValue.QuadPart == 0) {
        return false;
    }
    *fingerprintOut = fingerprint;
    *lastWriteTimeOut = writeValue.QuadPart;
    *sizeOut = (DWORD)total;
    return true;
}

static bool service_create_controlled_recovery_authorization(const char* reason,
    char* nonceHexOut, size_t nonceHexOutSize) {
    if (!nonceHexOut || nonceHexOutSize <= SERVICE_CONTROLLED_RECOVERY_NONCE_HEX_CHARS) return false;
    nonceHexOut[0] = 0;
    char path[MAX_PATH] = {};
    if (!service_controlled_recovery_authorization_path(path, sizeof(path))) return false;
    // Never let a failed new restart attempt inherit an older authorization.
    gc_DeleteFileUtf8(path);
    ServiceControlledRecoveryAuthorization authorization = {};
    authorization.magic = SERVICE_CONTROLLED_RECOVERY_MAGIC;
    authorization.version = SERVICE_CONTROLLED_RECOVERY_VERSION;
    authorization.size = sizeof(authorization);
    authorization.previousProcessId = GetCurrentProcessId();
    if (!service_generate_random_bytes(authorization.nonce, sizeof(authorization.nonce)) ||
        !service_query_process_creation_time_100ns(GetCurrentProcess(),
            &authorization.previousProcessCreationTime100ns) ||
        !service_query_boot_identity(&authorization.bootIdentity) ||
        !service_fingerprint_restart_snapshot(&authorization.snapshotFingerprint,
            &authorization.snapshotLastWriteTime100ns, &authorization.snapshotSize) ||
        !service_format_controlled_recovery_nonce(authorization.nonce,
            nonceHexOut, nonceHexOutSize)) {
        SecureZeroMemory(&authorization, sizeof(authorization));
        nonceHexOut[0] = 0;
        debug_log("controlled recovery authorization: could not establish protected identity/nonce\n");
        return false;
    }
    authorization.createdUptimeMs = GetTickCount64();
    if (authorization.createdUptimeMs == 0) {
        SecureZeroMemory(&authorization, sizeof(authorization));
        nonceHexOut[0] = 0;
        return false;
    }

    // Bind the authorization to the same unique ledger evidence key used by
    // corroborating removal/VEH/generation observations. This makes the
    // recovery count durable before the helper is launched and prevents the
    // later restart request from counting the same recovery twice.
    ServiceRecoveryEvidenceKey evidence = {};
    if (!service_record_current_recovery_evidence(
            SERVICE_RECOVERY_EVIDENCE_DRIVER, &evidence)) {
        SecureZeroMemory(&authorization, sizeof(authorization));
        SecureZeroMemory(nonceHexOut, nonceHexOutSize);
        debug_log("controlled recovery authorization: ledger commit failed; helper launch forbidden\n");
        return false;
    }
    authorization.evidenceKey = evidence;

    bool ok = service_commit_controlled_recovery_authorization(authorization);
    SecureZeroMemory(&authorization, sizeof(authorization));
    if (!ok) {
        gc_DeleteFileUtf8(path);
        SecureZeroMemory(nonceHexOut, nonceHexOutSize);
        debug_log("controlled recovery authorization: commit failed; recovery restore will be suppressed\n");
        return false;
    }
    debug_log("controlled recovery authorization: nonce bound to pid=%lu%s%s\n",
        (unsigned long)GetCurrentProcessId(),
        reason && reason[0] ? " for " : "",
        reason && reason[0] ? reason : "");
    return true;
}

static void service_clear_controlled_recovery_authorization() {
    char path[MAX_PATH] = {};
    if (service_controlled_recovery_authorization_path(path, sizeof(path))) gc_DeleteFileUtf8(path);
}

static bool service_read_controlled_recovery_authorization(const char* nonceHex,
    DWORD expectedPreviousProcessId, ULONGLONG expectedPreviousCreationTime100ns,
    bool consume, ServiceControlledRecoveryAuthorization* out = nullptr) {
    char path[MAX_PATH] = {};
    if (!service_controlled_recovery_authorization_path(path, sizeof(path))) return false;
    HANDLE h = gc_CreateFileUtf8(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        DWORD error = GetLastError();
        if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND) {
            debug_log("controlled recovery authorization: state unreadable (error=%lu)\n", error);
        }
        return false;
    }
    ServiceControlledRecoveryAuthorization authorization = {};
    DWORD read = 0;
    BYTE trailing = 0;
    bool ok = ReadFile(h, &authorization, sizeof(authorization), &read, nullptr) &&
        read == sizeof(authorization);
    DWORD trailingRead = 0;
    if (ok) ok = ReadFile(h, &trailing, sizeof(trailing), &trailingRead, nullptr) && trailingRead == 0;
    CloseHandle(h);
    if (consume) gc_DeleteFileUtf8(path);

    BYTE suppliedNonce[SERVICE_CONTROLLED_RECOVERY_NONCE_BYTES] = {};
    ServiceBootIdentity boot = {};
    ULONGLONG uptimeMs = GetTickCount64();
    bool nonceOk = service_parse_controlled_recovery_nonce(nonceHex, suppliedNonce);
    bool headerOk = ok && authorization.magic == SERVICE_CONTROLLED_RECOVERY_MAGIC &&
        authorization.version == SERVICE_CONTROLLED_RECOVERY_VERSION &&
        authorization.size == sizeof(authorization) && authorization.reserved == 0 &&
        authorization.reserved2 == 0 && authorization.reserved3 == 0 &&
        service_recovery_evidence_key_valid(authorization.evidenceKey) &&
        ((authorization.helperValidationMagic == 0 && authorization.helperProcessId == 0 &&
          authorization.helperProcessCreationTime100ns == 0) ||
         (authorization.helperValidationMagic == SERVICE_CONTROLLED_RECOVERY_HELPER_VALIDATION_MAGIC &&
          authorization.helperProcessId != 0 && authorization.helperProcessCreationTime100ns != 0));
    bool clockOk = service_query_boot_identity(&boot) && uptimeMs != 0;
    bool fresh = headerOk && clockOk && service_boot_identity_equal(
        authorization.bootIdentity, boot) &&
        authorization.createdUptimeMs != 0 &&
        authorization.createdUptimeMs <= uptimeMs &&
        (uptimeMs - authorization.createdUptimeMs) <= SERVICE_CONTROLLED_RECOVERY_MAX_AGE_MS;
    bool identityOk = headerOk && authorization.previousProcessId != 0 &&
        authorization.previousProcessCreationTime100ns != 0 &&
        (expectedPreviousProcessId == 0 ||
            authorization.previousProcessId == expectedPreviousProcessId) &&
        (expectedPreviousCreationTime100ns == 0 ||
            authorization.previousProcessCreationTime100ns == expectedPreviousCreationTime100ns);
    ULONGLONG snapshotFingerprint = 0, snapshotLastWrite = 0;
    DWORD snapshotSize = 0;
    bool snapshotOk = headerOk && authorization.snapshotFingerprint != 0 &&
        authorization.snapshotLastWriteTime100ns != 0 && authorization.snapshotSize != 0 &&
        service_fingerprint_restart_snapshot(&snapshotFingerprint, &snapshotLastWrite, &snapshotSize) &&
        snapshotFingerprint == authorization.snapshotFingerprint &&
        snapshotLastWrite == authorization.snapshotLastWriteTime100ns &&
        snapshotSize == authorization.snapshotSize;
    bool match = nonceOk && headerOk && fresh && identityOk && snapshotOk &&
        service_controlled_recovery_nonce_equal(suppliedNonce, authorization.nonce);
    SecureZeroMemory(suppliedNonce, sizeof(suppliedNonce));
    if (!match) {
        debug_log("controlled recovery authorization: rejected (header=%d nonce=%d identity=%d fresh=%d snapshot=%d); no automatic restore\n",
            headerOk ? 1 : 0, nonceOk ? 1 : 0, identityOk ? 1 : 0,
            fresh ? 1 : 0, snapshotOk ? 1 : 0);
        SecureZeroMemory(&authorization, sizeof(authorization));
        return false;
    }
    debug_log("controlled recovery authorization: validated pid=%lu after %llu uptime ms%s\n",
        (unsigned long)authorization.previousProcessId,
        (unsigned long long)(uptimeMs - authorization.createdUptimeMs),
        consume ? " and consumed" : "");
    if (out) *out = authorization;
    SecureZeroMemory(&authorization, sizeof(authorization));
    return true;
}

static bool service_mark_controlled_recovery_helper_validated(const char* nonceHex,
    DWORD expectedPreviousProcessId, ULONGLONG expectedPreviousCreationTime100ns) {
    ServiceControlledRecoveryAuthorization authorization = {};
    if (!service_read_controlled_recovery_authorization(nonceHex,
            expectedPreviousProcessId, expectedPreviousCreationTime100ns,
            false, &authorization)) return false;
    authorization.helperValidationMagic =
        SERVICE_CONTROLLED_RECOVERY_HELPER_VALIDATION_MAGIC;
    authorization.helperProcessId = GetCurrentProcessId();
    if (!service_query_process_creation_time_100ns(GetCurrentProcess(),
            &authorization.helperProcessCreationTime100ns)) {
        SecureZeroMemory(&authorization, sizeof(authorization));
        return false;
    }
    bool ok = service_commit_controlled_recovery_authorization(authorization);
    SecureZeroMemory(&authorization, sizeof(authorization));
    return ok;
}

static bool service_controlled_recovery_parent_matches(HANDLE parentProcess,
    const ServiceControlledRecoveryAuthorization& authorization) {
    if (!parentProcess || parentProcess == INVALID_HANDLE_VALUE) return false;
    DWORD pid = GetProcessId(parentProcess);
    ULONGLONG creation = 0;
    return pid != 0 && pid == authorization.previousProcessId &&
        service_query_process_creation_time_100ns(parentProcess, &creation) &&
        creation == authorization.previousProcessCreationTime100ns;
}

static bool service_current_target_gpu_for_snapshot(GpuAdapterInfo* out) {
    if (!out) return false;
    memset(out, 0, sizeof(*out));
    if (g_serviceActiveDesiredGpu.valid) {
        *out = g_serviceActiveDesiredGpu;
        return true;
    }
    // Never infer persisted ownership from the mutable current selection. A
    // controlled restore requires the GPU identity captured with our intent.
    return false;
}

static bool service_select_restart_reapply_gpu(const GpuAdapterInfo* target, char* err, size_t errSize) {
    if (!target || !target->valid) {
        set_message(err, errSize, "Restart snapshot has no GPU identity");
        return false;
    }
    if (g_app.adapterCount == 0) {
        set_message(err, errSize, "No GPU adapters are available");
        return false;
    }

    GpuAdapterInfo live = {};
    bool found = false;
    if (target->pciInfoValid) {
        unsigned int matches = 0;
        for (unsigned int i = 0; i < g_app.adapterCount && i < MAX_GPU_ADAPTERS; i++) {
            if (g_app.adapters[i].valid && gpu_adapter_has_same_pci_identity(&g_app.adapters[i], target)) {
                live = g_app.adapters[i];
                ++matches;
            }
        }
        found = matches == 1;
        if (matches > 1) {
            set_message(err, errSize,
                "Restart snapshot GPU identity is ambiguous across %u adapters",
                matches);
            debug_log("restart reapply target: rejected ambiguous PCI identity (%u matches)\n",
                matches);
            return false;
        }
    } else if (g_app.adapterCount == 1 && g_app.adapters[0].valid) {
        live = g_app.adapters[0];
        found = true;
    }

    if (!found) {
        set_message(err, errSize, "Restart snapshot GPU identity is not present");
        debug_log("restart reapply target: no live adapter matched snapshot nvapi=%u pciValid=%d name=%s\n",
            target->nvapiIndex,
            target->pciInfoValid ? 1 : 0,
            target->name[0] ? target->name : "<unnamed>");
        return false;
    }

    bool haveStrongIdentity = target->pciInfoValid && live.pciInfoValid;
    bool change = !g_app.selectedGpuIdentityValid ||
        g_app.selectedGpuIndex != live.nvapiIndex ||
        (haveStrongIdentity && !gpu_adapter_has_same_pci_identity(&g_app.selectedGpu, &live));
    if (change) {
        debug_log("restart reapply target: selecting matched nvapi=%u nvml=%u identity=%s name=%s\n",
            live.nvapiIndex,
            live.nvmlIndex,
            haveStrongIdentity ? "pci" : "single-adapter-ordinal",
            live.name[0] ? live.name : "<unnamed>");
        reset_gpu_runtime_selection();
        g_app.selectedGpuIndex = live.nvapiIndex;
        g_app.selectedNvmlIndex = live.nvmlIndex;
        g_app.selectedGpuExplicit = true;
        g_app.selectedGpu = live;
        g_app.selectedGpuIdentityValid = live.valid;
        g_app.selectedGpuOrdinalFallback = !haveStrongIdentity;
    }
    g_serviceActiveDesiredGpu = live;
    return true;
}

// True if the user-applied OC is still within its stabilization window (i.e. it
// has NOT yet proven stable for SERVICE_OC_STABILIZATION_WINDOW_MS).  Used by the
// restart-based reapply to decide whether a crash-restart should be treated as an
// unstable-OC failure (suppress + drop) rather than a normal recovery (reapply).
// Sticky automatic-restore lockout.  A crash/TDR before the proving period, a
// TDR/restart loop, or a failed automatic recovery means the service must stop
// making autonomous GPU writes.  The lockout persists across service and OS
// restarts and is cleared only by a later explicit successful GUI/CLI APPLY.
#define SERVICE_AUTO_RESTORE_LOCKOUT_MAGIC   0x4743414Cu /* 'GCAL' */
#define SERVICE_AUTO_RESTORE_LOCKOUT_VERSION 1u
static volatile LONG g_serviceAutoRestoreCachedLockoutReason =
    SERVICE_AUTO_RESTORE_LOCKOUT_NONE;

static HANDLE service_acquire_auto_restore_lockout_mutex() {
    HANDLE mutex = CreateMutexW(nullptr, FALSE,
        L"Global\\GreenCurve-AutoRestoreLockout-v1");
    if (!mutex) return nullptr;
    DWORD wait = WaitForSingleObject(mutex, INFINITE);
    if (wait != WAIT_OBJECT_0 && wait != WAIT_ABANDONED) {
        CloseHandle(mutex);
        return nullptr;
    }
    return mutex;
}

static void service_release_auto_restore_lockout_mutex(HANDLE mutex) {
    if (!mutex) return;
    ReleaseMutex(mutex);
    CloseHandle(mutex);
}

static const char* service_auto_restore_lockout_reason_name(DWORD reason) {
    switch (reason) {
        case SERVICE_AUTO_RESTORE_LOCKOUT_UNSTABLE_APPLY: return "apply did not survive the 10-minute proving period";
        case SERVICE_AUTO_RESTORE_LOCKOUT_TDR_SPAM: return "TDR/restart spam detected";
        case SERVICE_AUTO_RESTORE_LOCKOUT_AUTOMATIC_APPLY_FAILED: return "automatic recovery apply failed";
        default: return "unknown safety reason";
    }
}

static bool service_auto_restore_lockout_path(char* out, size_t outSize) {
    if (!out || outSize == 0) return false;
    char dir[MAX_PATH] = {};
    if (!resolve_service_machine_data_dir(dir, sizeof(dir))) return false;
    return SUCCEEDED(StringCchPrintfA(out, outSize, "%s\\service_auto_restore_lockout.bin", dir));
}

static const WCHAR* const SERVICE_AUTO_RESTORE_REGISTRY_KEY =
    L"SOFTWARE\\GreenCurve";
static const WCHAR* const SERVICE_AUTO_RESTORE_REGISTRY_VALUE =
    L"AutoRestoreLockoutReason";

static bool service_read_auto_restore_registry_lockout(
    DWORD* reasonOut, bool* presentOut) {
    if (reasonOut) *reasonOut = SERVICE_AUTO_RESTORE_LOCKOUT_NONE;
    if (presentOut) *presentOut = false;
    HKEY key = nullptr;
    LSTATUS status = RegOpenKeyExW(HKEY_LOCAL_MACHINE,
        SERVICE_AUTO_RESTORE_REGISTRY_KEY, 0,
        KEY_QUERY_VALUE | KEY_WOW64_64KEY, &key);
    if (status == ERROR_FILE_NOT_FOUND || status == ERROR_PATH_NOT_FOUND) {
        return true;
    }
    if (status != ERROR_SUCCESS) return false;
    DWORD type = 0;
    DWORD reason = 0;
    DWORD size = sizeof(reason);
    status = RegQueryValueExW(key, SERVICE_AUTO_RESTORE_REGISTRY_VALUE,
        nullptr, &type, reinterpret_cast<BYTE*>(&reason), &size);
    RegCloseKey(key);
    if (status == ERROR_FILE_NOT_FOUND) return true;
    if (status != ERROR_SUCCESS || type != REG_DWORD || size != sizeof(reason) ||
        reason == SERVICE_AUTO_RESTORE_LOCKOUT_NONE ||
        reason > SERVICE_AUTO_RESTORE_LOCKOUT_AUTOMATIC_APPLY_FAILED) {
        return false;
    }
    if (reasonOut) *reasonOut = reason;
    if (presentOut) *presentOut = true;
    return true;
}

static bool service_write_auto_restore_registry_lockout(DWORD reason) {
    HKEY key = nullptr;
    DWORD disposition = 0;
    LSTATUS status = RegCreateKeyExW(HKEY_LOCAL_MACHINE,
        SERVICE_AUTO_RESTORE_REGISTRY_KEY, 0, nullptr,
        REG_OPTION_NON_VOLATILE,
        KEY_SET_VALUE | KEY_QUERY_VALUE | KEY_WOW64_64KEY,
        nullptr, &key, &disposition);
    if (status != ERROR_SUCCESS) return false;
    status = RegSetValueExW(key, SERVICE_AUTO_RESTORE_REGISTRY_VALUE,
        0, REG_DWORD, reinterpret_cast<const BYTE*>(&reason), sizeof(reason));
    if (status == ERROR_SUCCESS) status = RegFlushKey(key);
    RegCloseKey(key);
    return status == ERROR_SUCCESS;
}

static bool service_clear_auto_restore_registry_lockout() {
    HKEY key = nullptr;
    LSTATUS status = RegOpenKeyExW(HKEY_LOCAL_MACHINE,
        SERVICE_AUTO_RESTORE_REGISTRY_KEY, 0,
        KEY_SET_VALUE | KEY_QUERY_VALUE | KEY_WOW64_64KEY, &key);
    if (status == ERROR_FILE_NOT_FOUND || status == ERROR_PATH_NOT_FOUND) {
        return true;
    }
    if (status != ERROR_SUCCESS) return false;
    status = RegDeleteValueW(key, SERVICE_AUTO_RESTORE_REGISTRY_VALUE);
    if (status == ERROR_FILE_NOT_FOUND) status = ERROR_SUCCESS;
    if (status == ERROR_SUCCESS) status = RegFlushKey(key);
    RegCloseKey(key);
    return status == ERROR_SUCCESS;
}

static bool service_auto_restore_is_locked_out(DWORD* reasonOut = nullptr) {
    if (reasonOut) *reasonOut = SERVICE_AUTO_RESTORE_LOCKOUT_NONE;
    DWORD cachedReason = (DWORD)InterlockedExchangeAdd(
        &g_serviceAutoRestoreCachedLockoutReason, 0);
    if (cachedReason != SERVICE_AUTO_RESTORE_LOCKOUT_NONE) {
        if (reasonOut) *reasonOut = cachedReason;
        return true;
    }
    HANDLE mutex = service_acquire_auto_restore_lockout_mutex();
    if (!mutex) {
        InterlockedExchange(&g_serviceAutoRestoreCachedLockoutReason,
            SERVICE_AUTO_RESTORE_LOCKOUT_AUTOMATIC_APPLY_FAILED);
        if (reasonOut) *reasonOut =
            SERVICE_AUTO_RESTORE_LOCKOUT_AUTOMATIC_APPLY_FAILED;
        return true;
    }
    cachedReason = (DWORD)InterlockedExchangeAdd(
        &g_serviceAutoRestoreCachedLockoutReason, 0);
    if (cachedReason != SERVICE_AUTO_RESTORE_LOCKOUT_NONE) {
        service_release_auto_restore_lockout_mutex(mutex);
        if (reasonOut) *reasonOut = cachedReason;
        return true;
    }
    char path[MAX_PATH] = {};
    if (!service_auto_restore_lockout_path(path, sizeof(path))) {
        InterlockedExchange(&g_serviceAutoRestoreCachedLockoutReason,
            SERVICE_AUTO_RESTORE_LOCKOUT_AUTOMATIC_APPLY_FAILED);
        if (reasonOut) *reasonOut =
            SERVICE_AUTO_RESTORE_LOCKOUT_AUTOMATIC_APPLY_FAILED;
        service_release_auto_restore_lockout_mutex(mutex);
        return true;
    }
    HANDLE h = gc_CreateFileUtf8(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        DWORD error = GetLastError();
        if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) {
            DWORD registryReason = SERVICE_AUTO_RESTORE_LOCKOUT_NONE;
            bool registryPresent = false;
            if (!service_read_auto_restore_registry_lockout(
                    &registryReason, &registryPresent)) {
                InterlockedExchange(&g_serviceAutoRestoreCachedLockoutReason,
                    SERVICE_AUTO_RESTORE_LOCKOUT_AUTOMATIC_APPLY_FAILED);
                if (reasonOut) *reasonOut =
                    SERVICE_AUTO_RESTORE_LOCKOUT_AUTOMATIC_APPLY_FAILED;
                debug_log("auto-restore lockout: file absent but registry fallback is unreadable; failing closed\n");
                service_release_auto_restore_lockout_mutex(mutex);
                return true;
            }
            if (registryPresent) {
                InterlockedExchange(&g_serviceAutoRestoreCachedLockoutReason,
                    (LONG)registryReason);
                if (reasonOut) *reasonOut = registryReason;
                debug_log("auto-restore lockout: restored durable registry fallback reason=%lu\n",
                    (unsigned long)registryReason);
                service_release_auto_restore_lockout_mutex(mutex);
                return true;
            }
            service_release_auto_restore_lockout_mutex(mutex);
            return false;
        }
        InterlockedExchange(&g_serviceAutoRestoreCachedLockoutReason,
            SERVICE_AUTO_RESTORE_LOCKOUT_AUTOMATIC_APPLY_FAILED);
        if (reasonOut) *reasonOut = SERVICE_AUTO_RESTORE_LOCKOUT_AUTOMATIC_APPLY_FAILED;
        debug_log("auto-restore lockout: state unreadable (error=%lu); failing closed\n", error);
        service_release_auto_restore_lockout_mutex(mutex);
        return true;
    }
    DWORD magic = 0, version = 0, reason = 0, read = 0;
    bool ok = ReadFile(h, &magic, sizeof(magic), &read, nullptr) && read == sizeof(magic);
    ok = ok && ReadFile(h, &version, sizeof(version), &read, nullptr) && read == sizeof(version);
    ok = ok && ReadFile(h, &reason, sizeof(reason), &read, nullptr) && read == sizeof(reason);
    BYTE trailing = 0;
    DWORD trailingRead = 0;
    if (ok) ok = ReadFile(h, &trailing, sizeof(trailing), &trailingRead, nullptr) && trailingRead == 0;
    CloseHandle(h);
    if (!ok || magic != SERVICE_AUTO_RESTORE_LOCKOUT_MAGIC ||
        version != SERVICE_AUTO_RESTORE_LOCKOUT_VERSION ||
        reason == SERVICE_AUTO_RESTORE_LOCKOUT_NONE ||
        reason > SERVICE_AUTO_RESTORE_LOCKOUT_AUTOMATIC_APPLY_FAILED) {
        InterlockedExchange(&g_serviceAutoRestoreCachedLockoutReason,
            SERVICE_AUTO_RESTORE_LOCKOUT_AUTOMATIC_APPLY_FAILED);
        if (reasonOut) *reasonOut = SERVICE_AUTO_RESTORE_LOCKOUT_AUTOMATIC_APPLY_FAILED;
        debug_log("auto-restore lockout: malformed/unknown persisted state; failing closed\n");
        service_release_auto_restore_lockout_mutex(mutex);
        return true;
    }
    InterlockedExchange(&g_serviceAutoRestoreCachedLockoutReason, (LONG)reason);
    if (reasonOut) *reasonOut = reason;
    service_release_auto_restore_lockout_mutex(mutex);
    return true;
}

static void service_latch_auto_restore_lockout(DWORD reason, const char* context) {
    if (reason == SERVICE_AUTO_RESTORE_LOCKOUT_NONE) return;
    InterlockedExchange(&g_serviceAutoRestoreCachedLockoutReason, (LONG)reason);
    HANDLE mutex = service_acquire_auto_restore_lockout_mutex();
    bool fileOk = false;
    if (mutex) {
        do {
            char path[MAX_PATH] = {};
            if (!service_auto_restore_lockout_path(path, sizeof(path))) break;
            char pathErr[256] = {};
            if (!ensure_parent_directory_for_file(path, pathErr, sizeof(pathErr))) {
                debug_log("auto-restore lockout: could not create protected state directory: %s\n",
                    pathErr[0] ? pathErr : "unknown error");
                break;
            }
            char tempPath[MAX_PATH] = {};
            if (FAILED(StringCchPrintfA(tempPath, ARRAY_COUNT(tempPath),
                    "%s.tmp.%lu.%llu", path,
                    (unsigned long)GetCurrentProcessId(),
                    (unsigned long long)GetTickCount64()))) break;
            HANDLE h = gc_CreateFileUtf8(tempPath, GENERIC_WRITE, 0, nullptr,
                CREATE_NEW, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH,
                nullptr);
            if (h == INVALID_HANDLE_VALUE) {
                debug_log("auto-restore lockout: could not persist %s (error=%lu)\n",
                    service_auto_restore_lockout_reason_name(reason), GetLastError());
                break;
            }
            DWORD magic = SERVICE_AUTO_RESTORE_LOCKOUT_MAGIC;
            DWORD version = SERVICE_AUTO_RESTORE_LOCKOUT_VERSION;
            DWORD written = 0;
            fileOk = WriteFile(h, &magic, sizeof(magic), &written, nullptr) &&
                written == sizeof(magic);
            fileOk = fileOk && WriteFile(h, &version, sizeof(version), &written,
                nullptr) && written == sizeof(version);
            fileOk = fileOk && WriteFile(h, &reason, sizeof(reason), &written,
                nullptr) && written == sizeof(reason);
            if (fileOk) fileOk = FlushFileBuffers(h) != FALSE;
            CloseHandle(h);
            if (fileOk) fileOk = gc_MoveFileExUtf8(tempPath, path,
                MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
            if (!fileOk) gc_DeleteFileUtf8(tempPath);
        } while (false);
    }
    bool registryOk = service_write_auto_restore_registry_lockout(reason);
    bool durable = fileOk || registryOk;
    debug_log("auto-restore lockout: %s%s%s (file=%d registry=%d durable=%d)\n",
        service_auto_restore_lockout_reason_name(reason),
        context && context[0] ? " after " : "",
        context && context[0] ? context : "",
        fileOk ? 1 : 0, registryOk ? 1 : 0, durable ? 1 : 0);
    if (!durable) {
        debug_log("auto-restore lockout: CRITICAL persistence failure; current process remains fail-closed and no automatic write is authorized\n");
    }
    if (mutex) service_release_auto_restore_lockout_mutex(mutex);
}

static bool service_clear_auto_restore_lockout() {
    HANDLE mutex = service_acquire_auto_restore_lockout_mutex();
    if (!mutex) return false;
    char path[MAX_PATH] = {};
    if (!service_auto_restore_lockout_path(path, sizeof(path))) {
        service_release_auto_restore_lockout_mutex(mutex);
        return false;
    }
    bool cleared = gc_DeleteFileUtf8(path) != FALSE;
    if (!cleared) {
        DWORD error = GetLastError();
        cleared = error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
    }
    DWORD attributes = gc_GetFileAttributesUtf8(path);
    DWORD attributesError = attributes == INVALID_FILE_ATTRIBUTES
        ? GetLastError() : ERROR_SUCCESS;
    bool registryCleared = service_clear_auto_restore_registry_lockout();
    if (cleared && registryCleared && attributes == INVALID_FILE_ATTRIBUTES &&
        (attributesError == ERROR_FILE_NOT_FOUND ||
         attributesError == ERROR_PATH_NOT_FOUND)) {
        InterlockedExchange(&g_serviceAutoRestoreCachedLockoutReason,
            SERVICE_AUTO_RESTORE_LOCKOUT_NONE);
        debug_log("auto-restore lockout: cleared by explicit successful apply\n");
        service_release_auto_restore_lockout_mutex(mutex);
        return true;
    }
    debug_log("auto-restore lockout: explicit clear failed; cached lockout remains active\n");
    service_release_auto_restore_lockout_mutex(mutex);
    return false;
}

// Suspend/resume is not a driver-crash signal and must not impose the
// driver/TDR proving period.  It still honors the persistent lockout, which is
// the explicit safety stop after a real unstable apply, TDR spam, or failed
// automatic recovery.
static bool service_auto_restore_allowed_after_standby_resume(DWORD* lockoutReasonOut = nullptr) {
    if (lockoutReasonOut) *lockoutReasonOut = SERVICE_AUTO_RESTORE_LOCKOUT_NONE;
    DWORD lockoutReason = SERVICE_AUTO_RESTORE_LOCKOUT_NONE;
    if (service_auto_restore_is_locked_out(&lockoutReason)) {
        if (lockoutReasonOut) *lockoutReasonOut = lockoutReason;
        return false;
    }
    return should_auto_restore_after_standby_resume(g_serviceHasActiveDesired, false);
}

// Snapshot the current active desired settings to disk so the post-restart
// process can re-apply them.  Writes nothing (and removes any stale file) when
// there is no active desired — a restart with nothing applied should not
// auto-apply on the next boot.
static void service_write_restart_reapply_snapshot() {
    if (!g_app.isServiceProcess) return;
    char path[MAX_PATH] = {};
    if (!service_active_desired_persist_path(path, sizeof(path))) return;
    if (!g_serviceHasActiveDesired) {
        gc_DeleteFileUtf8(path);
        return;
    }
    ServiceRestartReapplySnapshot payload = {};
    payload.desired = g_serviceActiveDesired;
    payload.activeProfileSource = (DWORD)g_serviceActiveProfileSource;
    payload.activeProfileSlot = g_serviceActiveProfileSlot;
    bool slottedProfile = g_serviceActiveProfileSource == SERVICE_PROFILE_SOURCE_USER_SLOT ||
        g_serviceActiveProfileSource == SERVICE_PROFILE_SOURCE_SHARED_SLOT ||
        g_serviceActiveProfileSource == SERVICE_PROFILE_SOURCE_MACHINE_SLOT;
    if ((DWORD)g_serviceActiveProfileSource > SERVICE_PROFILE_SOURCE_AD_HOC ||
        (slottedProfile && (g_serviceActiveProfileSlot < 1 || g_serviceActiveProfileSlot > 5)) ||
        (!slottedProfile && g_serviceActiveProfileSlot != 0)) {
        debug_log("restart reapply snapshot: active profile metadata is not canonical; clearing stale snapshot\n");
        gc_DeleteFileUtf8(path);
        return;
    }
    if (!service_current_target_gpu_for_snapshot(&payload.targetGpu) || !payload.targetGpu.valid) {
        debug_log("restart reapply snapshot: no validated target GPU identity; clearing stale snapshot\n");
        gc_DeleteFileUtf8(path);
        return;
    }
    validate_desired_settings_for_ipc(&payload.desired);
    validate_gpu_adapter_info_for_ipc(&payload.targetGpu);
    char pathErr[256] = {};
    ensure_parent_directory_for_file(path, pathErr, sizeof(pathErr));
    char tempPath[MAX_PATH] = {};
    if (FAILED(StringCchPrintfA(tempPath, ARRAY_COUNT(tempPath), "%s.tmp.%lu.%llu", path,
            (unsigned long)GetCurrentProcessId(),
            (unsigned long long)GetTickCount64()))) return;
    HANDLE h = gc_CreateFileUtf8(tempPath, GENERIC_WRITE, 0, nullptr, CREATE_NEW,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        debug_log("restart reapply snapshot: CreateFile failed (error %lu)\n", GetLastError());
        return;
    }
    DWORD magic = SERVICE_ACTIVE_DESIRED_MAGIC;
    DWORD version = SERVICE_ACTIVE_DESIRED_VERSION;
    DWORD size = (DWORD)sizeof(payload);
    DWORD written = 0;
    bool ok = WriteFile(h, &magic, sizeof(magic), &written, nullptr) && written == sizeof(magic);
    ok = ok && WriteFile(h, &version, sizeof(version), &written, nullptr) && written == sizeof(version);
    ok = ok && WriteFile(h, &size, sizeof(size), &written, nullptr) && written == sizeof(size);
    ok = ok && WriteFile(h, &payload, size, &written, nullptr) && written == size;
    if (ok) ok = FlushFileBuffers(h) != FALSE;
    CloseHandle(h);
    if (ok) ok = gc_MoveFileExUtf8(tempPath, path,
        MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
    if (!ok) {
        gc_DeleteFileUtf8(tempPath);
        gc_DeleteFileUtf8(path);
    }
    debug_log("restart reapply snapshot: wrote %s (ok=%d targetNvapi=%u pciValid=%d)\n",
        path,
        ok ? 1 : 0,
        payload.targetGpu.nvapiIndex,
        payload.targetGpu.pciInfoValid ? 1 : 0);
}

static bool service_load_restart_reapply_snapshot(DesiredSettings* out, GpuAdapterInfo* targetOut,
    ServiceProfileSource* profileSourceOut = nullptr, unsigned int* profileSlotOut = nullptr) {
    if (!out) return false;
    if (targetOut) memset(targetOut, 0, sizeof(*targetOut));
    if (profileSourceOut) *profileSourceOut = SERVICE_PROFILE_SOURCE_NONE;
    if (profileSlotOut) *profileSlotOut = 0;
    char path[MAX_PATH] = {};
    if (!service_active_desired_persist_path(path, sizeof(path))) return false;
    HANDLE h = gc_CreateFileUtf8(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD magic = 0, version = 0, size = 0, read = 0;
    bool ok = ReadFile(h, &magic, sizeof(magic), &read, nullptr) && read == sizeof(magic);
    ok = ok && ReadFile(h, &version, sizeof(version), &read, nullptr) && read == sizeof(version);
    ok = ok && ReadFile(h, &size, sizeof(size), &read, nullptr) && read == sizeof(size);
    if (ok && (magic != SERVICE_ACTIVE_DESIRED_MAGIC ||
               (version != SERVICE_ACTIVE_DESIRED_VERSION &&
                version != SERVICE_ACTIVE_DESIRED_LEGACY_VERSION) ||
               size != (DWORD)sizeof(ServiceRestartReapplySnapshot))) {
        debug_log("restart reapply load: header mismatch magic=%08lX ver=%lu size=%lu (expected version=%u size=%lu); clearing old snapshot\n",
            (unsigned long)magic,
            (unsigned long)version,
            (unsigned long)size,
            (unsigned)SERVICE_ACTIVE_DESIRED_VERSION,
            (unsigned long)sizeof(ServiceRestartReapplySnapshot));
        ok = false;
    }
    if (ok && version == SERVICE_ACTIVE_DESIRED_LEGACY_VERSION) {
        debug_log("restart reapply load: accepted backward-compatible v4 snapshot\n");
    }
    ServiceRestartReapplySnapshot payload = {};
    if (ok) ok = ReadFile(h, &payload, size, &read, nullptr) && read == size;
    BYTE trailing = 0;
    DWORD trailingRead = 0;
    if (ok) ok = ReadFile(h, &trailing, sizeof(trailing), &trailingRead, nullptr) && trailingRead == 0;
    CloseHandle(h);
    if (!ok) {
        debug_log("restart reapply load: read failed\n");
        gc_DeleteFileUtf8(path);
        return false;
    }
    // The file lives in the admin-only SYSTEM-profile dir, so this is not a
    // user trust boundary — but a torn/corrupt write would feed raw bytes
    // straight into the apply path.  Clamp every field with the same
    // validator used at the IPC boundary (also keeps lockMode in enum range).
    DesiredSettings originalDesired = payload.desired;
    GpuAdapterInfo originalTarget = payload.targetGpu;
    validate_desired_settings_for_ipc(&payload.desired);
    validate_gpu_adapter_info_for_ipc(&payload.targetGpu);
    if (memcmp(&originalDesired, &payload.desired, sizeof(payload.desired)) != 0 ||
        memcmp(&originalTarget, &payload.targetGpu, sizeof(payload.targetGpu)) != 0) {
        debug_log("restart reapply load: payload fields required sanitization; rejecting corrupt snapshot\n");
        gc_DeleteFileUtf8(path);
        return false;
    }
    if (!payload.targetGpu.valid) {
        debug_log("restart reapply load: missing target GPU identity; clearing snapshot\n");
        gc_DeleteFileUtf8(path);
        return false;
    }
    ServiceProfileSource profileSource = (ServiceProfileSource)payload.activeProfileSource;
    bool slotted = profileSource == SERVICE_PROFILE_SOURCE_USER_SLOT ||
        profileSource == SERVICE_PROFILE_SOURCE_SHARED_SLOT ||
        profileSource == SERVICE_PROFILE_SOURCE_MACHINE_SLOT;
    if (payload.activeProfileSource > SERVICE_PROFILE_SOURCE_AD_HOC ||
        (slotted && (payload.activeProfileSlot < 1 || payload.activeProfileSlot > 5)) ||
        (!slotted && payload.activeProfileSlot != 0) ||
        payload.reserved[0] != 0 || payload.reserved[1] != 0) {
        debug_log("restart reapply load: invalid active profile metadata; rejecting snapshot\n");
        gc_DeleteFileUtf8(path);
        return false;
    }
    *out = payload.desired;
    if (targetOut) *targetOut = payload.targetGpu;
    if (profileSourceOut) *profileSourceOut = profileSource;
    if (profileSlotOut) *profileSlotOut = payload.activeProfileSlot;
    return true;
}

static void service_clear_restart_reapply_snapshot() {
    char path[MAX_PATH] = {};
    if (service_active_desired_persist_path(path, sizeof(path))) gc_DeleteFileUtf8(path);
}

static void service_cleanup_obsolete_recovery_artifacts();
#include "main_service_obsolete_artifacts.cpp"
