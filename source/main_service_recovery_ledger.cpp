// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// One protected, process-independent ledger owns driver-recovery spam counting.
// Entries carry evidence keys so corroborating observations of one recovery
// (for example removal/rearrival followed by the controlled restart request)
// are counted once.

#define RECOVERY_LOOP_WINDOW_MS           300000ULL
#define MAX_RECOVERIES_BEFORE_BACKOFF     3u
#define SERVICE_RESTART_LOOP_WINDOW_MS    RECOVERY_LOOP_WINDOW_MS
#define SERVICE_RESTART_LOOP_THRESHOLD    MAX_RECOVERIES_BEFORE_BACKOFF
#define SERVICE_RECOVERY_LEDGER_MAX       16u
#define SERVICE_RECOVERY_LEDGER_MAGIC     0x4743524Cu /* 'GCRL' */
#define SERVICE_RECOVERY_LEDGER_VERSION   2u
#define SERVICE_OLD_RESTART_HISTORY_MAGIC 0x47435248u /* 'GCRH' */

static void service_latch_auto_restore_lockout(DWORD reason, const char* context);

enum ServiceRecoveryEvidenceKind : DWORD {
    SERVICE_RECOVERY_EVIDENCE_DRIVER = 1,
};

struct ServiceRecoveryLedgerEntry {
    ServiceRecoveryEvidenceKey key;
    ServiceBootIdentity bootIdentity;
    ULONGLONG awakeTime100ns;
    DWORD kind;
    DWORD reserved;
};

struct ServiceRecoveryLedger {
    DWORD magic;
    DWORD version;
    DWORD size;
    DWORD count;
    ServiceRecoveryLedgerEntry entries[SERVICE_RECOVERY_LEDGER_MAX];
};

static ServiceRecoveryEvidenceKey g_serviceCurrentRecoveryEvidence = {};
static volatile LONG g_serviceRecoveryLedgerWriteFailed = 0;

static HANDLE service_acquire_recovery_ledger_mutex() {
    HANDLE mutex = CreateMutexW(nullptr, FALSE,
        L"Global\\GreenCurve-RecoveryLedger-v1");
    if (!mutex) return nullptr;
    DWORD wait = WaitForSingleObject(mutex, INFINITE);
    if (wait != WAIT_OBJECT_0 && wait != WAIT_ABANDONED) {
        CloseHandle(mutex);
        return nullptr;
    }
    return mutex;
}

static void service_release_recovery_ledger_mutex(HANDLE mutex) {
    if (!mutex) return;
    ReleaseMutex(mutex);
    CloseHandle(mutex);
}

static bool service_generate_random_bytes(void* buffer, ULONG size) {
    if (!buffer || size == 0) return false;
    HMODULE advapi = GetModuleHandleW(L"advapi32.dll");
    if (!advapi) advapi = LoadLibraryW(L"advapi32.dll");
    if (!advapi) return false;
    typedef BOOLEAN (WINAPI *RtlGenRandomFn)(PVOID, ULONG);
    RtlGenRandomFn random = reinterpret_cast<RtlGenRandomFn>(
        GetProcAddress(advapi, "SystemFunction036"));
    return random && random(buffer, size) != FALSE;
}

static bool service_new_recovery_evidence_key(ServiceRecoveryEvidenceKey* out) {
    if (!out) return false;
    memset(out, 0, sizeof(*out));
    if (!service_generate_random_bytes(out, (ULONG)sizeof(*out)) ||
        !service_recovery_evidence_key_valid(*out)) {
        memset(out, 0, sizeof(*out));
        return false;
    }
    return true;
}

static bool service_recovery_ledger_path(char* out, size_t outSize) {
    if (!out || outSize == 0) return false;
    char dir[MAX_PATH] = {};
    if (!resolve_service_machine_data_dir(dir, sizeof(dir))) return false;
    return SUCCEEDED(StringCchPrintfA(out, outSize, "%s\\service_recovery_ledger.bin", dir));
}

static bool service_old_restart_history_path(char* out, size_t outSize) {
    if (!out || outSize == 0) return false;
    char dir[MAX_PATH] = {};
    if (!resolve_service_machine_data_dir(dir, sizeof(dir))) return false;
    return SUCCEEDED(StringCchPrintfA(out, outSize, "%s\\service_restart_history.bin", dir));
}

static bool service_commit_recovery_ledger(const ServiceRecoveryLedger& ledger) {
    char path[MAX_PATH] = {};
    if (!service_recovery_ledger_path(path, sizeof(path))) return false;
    char pathErr[256] = {};
    if (!ensure_parent_directory_for_file(path, pathErr, sizeof(pathErr))) return false;
    char tempPath[MAX_PATH] = {};
    if (FAILED(StringCchPrintfA(tempPath, ARRAY_COUNT(tempPath), "%s.tmp.%lu", path,
            (unsigned long)GetCurrentProcessId()))) return false;
    HANDLE h = gc_CreateFileUtf8(tempPath, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    bool ok = WriteFile(h, &ledger, sizeof(ledger), &written, nullptr) &&
        written == sizeof(ledger) && FlushFileBuffers(h) != FALSE;
    CloseHandle(h);
    if (ok) ok = gc_MoveFileExUtf8(tempPath, path,
        MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
    if (!ok) gc_DeleteFileUtf8(tempPath);
    return ok;
}

static bool service_read_recovery_ledger_file(ServiceRecoveryLedger* out) {
    if (!out) return false;
    memset(out, 0, sizeof(*out));
    char path[MAX_PATH] = {};
    if (!service_recovery_ledger_path(path, sizeof(path))) return false;
    HANDLE h = gc_CreateFileUtf8(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD read = 0;
    BYTE trailing = 0;
    bool ok = ReadFile(h, out, sizeof(*out), &read, nullptr) && read == sizeof(*out);
    DWORD trailingRead = 0;
    if (ok) ok = ReadFile(h, &trailing, sizeof(trailing), &trailingRead, nullptr) && trailingRead == 0;
    CloseHandle(h);
    if (!ok || out->magic != SERVICE_RECOVERY_LEDGER_MAGIC ||
        out->version != SERVICE_RECOVERY_LEDGER_VERSION ||
        out->size != sizeof(*out) || out->count > SERVICE_RECOVERY_LEDGER_MAX) {
        debug_log("recovery ledger: corrupt/unsupported file rejected; automatic recovery fails closed\n");
        memset(out, 0, sizeof(*out));
        return false;
    }
    for (DWORD i = 0; i < out->count; ++i) {
        const ServiceRecoveryLedgerEntry& entry = out->entries[i];
        if (!service_recovery_evidence_key_valid(entry.key) ||
            !service_boot_identity_valid(entry.bootIdentity) ||
            entry.awakeTime100ns == 0 || entry.reserved != 0 ||
            entry.kind != SERVICE_RECOVERY_EVIDENCE_DRIVER) {
            debug_log("recovery ledger: malformed entry rejected; automatic recovery fails closed\n");
            memset(out, 0, sizeof(*out));
            return false;
        }
    }
    return true;
}

static void service_initialize_empty_recovery_ledger(ServiceRecoveryLedger* ledger) {
    if (!ledger) return;
    memset(ledger, 0, sizeof(*ledger));
    ledger->magic = SERVICE_RECOVERY_LEDGER_MAGIC;
    ledger->version = SERVICE_RECOVERY_LEDGER_VERSION;
    ledger->size = sizeof(*ledger);
}

// Empty obsolete history is removable. Non-empty legacy ticks have no stable
// boot identity, so only an explicit successful Apply may acknowledge them.
static bool service_resolve_old_restart_history(ServiceRecoveryLedger* ledger) {
    if (!ledger) return false;
    char oldPath[MAX_PATH] = {};
    if (!service_old_restart_history_path(oldPath, sizeof(oldPath))) return false;
    HANDLE h = gc_CreateFileUtf8(oldPath, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        DWORD error = GetLastError();
        return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
    }
    DWORD magic = 0, count = 0, read = 0;
    bool ok = ReadFile(h, &magic, sizeof(magic), &read, nullptr) && read == sizeof(magic);
    ok = ok && ReadFile(h, &count, sizeof(count), &read, nullptr) && read == sizeof(count);
    if (!ok || magic != SERVICE_OLD_RESTART_HISTORY_MAGIC || count > 8u) {
        CloseHandle(h);
        debug_log("recovery ledger: rejected obsolete/corrupt restart history; failing closed until explicit Apply\n");
        return false;
    }
    ULONGLONG oldTicks[8] = {};
    for (DWORD i = 0; ok && i < count; ++i) {
        ok = ReadFile(h, &oldTicks[i], sizeof(oldTicks[i]), &read, nullptr) &&
            read == sizeof(oldTicks[i]);
    }
    BYTE trailing = 0;
    DWORD trailingRead = 0;
    if (ok) ok = ReadFile(h, &trailing, sizeof(trailing), &trailingRead, nullptr) && trailingRead == 0;
    CloseHandle(h);
    if (!ok) {
        debug_log("recovery ledger: obsolete restart history is corrupt; failing closed until explicit Apply\n");
        return false;
    }
    if (count != 0) {
        // Legacy ticks and last-write time cannot be bound to the stable
        // BootIdentifier. Guessing could silently discard real recovery spam.
        debug_log("recovery ledger: non-empty legacy history has no stable BootIdentifier; failing closed until explicit Apply\n");
        return false;
    }
    gc_DeleteFileUtf8(oldPath);
    debug_log("recovery ledger: discarded empty obsolete restart history\n");
    return true;
}

static bool service_load_recovery_ledger(ServiceRecoveryLedger* ledger) {
    if (!ledger) return false;
    char path[MAX_PATH] = {};
    if (!service_recovery_ledger_path(path, sizeof(path))) return false;
    DWORD attrs = gc_GetFileAttributesUtf8(path);
    if (attrs != INVALID_FILE_ATTRIBUTES) return service_read_recovery_ledger_file(ledger);
    DWORD error = GetLastError();
    if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND) {
        debug_log("recovery ledger: protected state attributes unavailable (error=%lu)\n", error);
        return false;
    }
    service_initialize_empty_recovery_ledger(ledger);
    return service_resolve_old_restart_history(ledger);
}

static unsigned int service_count_recent_recovery_entries(const ServiceRecoveryLedger& ledger,
    const ServiceBootIdentity& bootIdentity, ULONGLONG awakeTime100ns) {
    ServiceRecoveryClockEntry clocks[SERVICE_RECOVERY_LEDGER_MAX] = {};
    for (DWORD i = 0; i < ledger.count; ++i) {
        clocks[i].bootIdentity = ledger.entries[i].bootIdentity;
        clocks[i].awakeTime100ns = ledger.entries[i].awakeTime100ns;
    }
    return service_count_recent_recovery_clock_entries(clocks, ledger.count,
        bootIdentity, awakeTime100ns, RECOVERY_LOOP_WINDOW_MS);
}

static unsigned int service_count_recent_restarts() {
    if (InterlockedExchangeAdd(&g_serviceRecoveryLedgerWriteFailed, 0) != 0) {
        return SERVICE_RESTART_LOOP_THRESHOLD;
    }
    HANDLE mutex = service_acquire_recovery_ledger_mutex();
    if (!mutex) {
        service_latch_auto_restore_lockout(SERVICE_AUTO_RESTORE_LOCKOUT_TDR_SPAM,
            "recovery ledger serialization failed");
        return SERVICE_RESTART_LOOP_THRESHOLD;
    }
    ServiceRecoveryLedger ledger = {};
    ServiceBootIdentity boot = {};
    ULONGLONG awake = 0;
    if (!service_load_recovery_ledger(&ledger) ||
        !service_query_boot_identity(&boot) || !service_query_unbiased_time_100ns(&awake)) {
        service_release_recovery_ledger_mutex(mutex);
        debug_log("recovery ledger: unavailable during spam check; failing closed\n");
        service_latch_auto_restore_lockout(SERVICE_AUTO_RESTORE_LOCKOUT_TDR_SPAM,
            "recovery ledger was unavailable or ambiguous");
        return SERVICE_RESTART_LOOP_THRESHOLD;
    }
    unsigned int count = service_count_recent_recovery_entries(ledger, boot, awake);
    service_release_recovery_ledger_mutex(mutex);
    return count;
}

static bool service_record_recovery_evidence(const ServiceRecoveryEvidenceKey& key,
    ServiceRecoveryEvidenceKind kind) {
    if (!service_recovery_evidence_key_valid(key)) return false;
    if (InterlockedExchangeAdd(&g_serviceRecoveryLedgerWriteFailed, 0) != 0) return false;
    HANDLE mutex = service_acquire_recovery_ledger_mutex();
    if (!mutex) {
        service_latch_auto_restore_lockout(SERVICE_AUTO_RESTORE_LOCKOUT_TDR_SPAM,
            "recovery ledger serialization failed");
        return false;
    }
    ServiceRecoveryLedger ledger = {};
    ServiceBootIdentity boot = {};
    ULONGLONG awake = 0;
    if (!service_load_recovery_ledger(&ledger) ||
        !service_query_boot_identity(&boot) || !service_query_unbiased_time_100ns(&awake)) {
        service_release_recovery_ledger_mutex(mutex);
        debug_log("recovery ledger: cannot record evidence; automatic recovery must fail closed\n");
        service_latch_auto_restore_lockout(SERVICE_AUTO_RESTORE_LOCKOUT_TDR_SPAM,
            "recovery evidence could not be recorded");
        return false;
    }
    ServiceRecoveryEvidenceKey recordedKeys[SERVICE_RECOVERY_LEDGER_MAX] = {};
    for (DWORD i = 0; i < ledger.count; ++i) recordedKeys[i] = ledger.entries[i].key;
    if (service_recovery_evidence_already_recorded(recordedKeys, ledger.count, key)) {
        debug_log("recovery ledger: corroborating evidence coalesced (kind=%lu)\n",
            (unsigned long)kind);
        service_release_recovery_ledger_mutex(mutex);
        return true;
    }

    ServiceRecoveryLedger compact = {};
    service_initialize_empty_recovery_ledger(&compact);
    for (DWORD i = 0; i < ledger.count && compact.count < SERVICE_RECOVERY_LEDGER_MAX - 1; ++i) {
        const ServiceRecoveryLedgerEntry& entry = ledger.entries[i];
        if (service_boot_identity_equal(entry.bootIdentity, boot) &&
            entry.awakeTime100ns <= awake &&
            ((awake - entry.awakeTime100ns) / 10000ULL) <= RECOVERY_LOOP_WINDOW_MS) {
            compact.entries[compact.count++] = entry;
        }
    }
    ServiceRecoveryLedgerEntry& entry = compact.entries[compact.count++];
    entry.key = key;
    entry.bootIdentity = boot;
    entry.awakeTime100ns = awake;
    entry.kind = kind;
    if (!service_commit_recovery_ledger(compact)) {
        service_release_recovery_ledger_mutex(mutex);
        debug_log("recovery ledger: durable commit failed; automatic recovery must fail closed\n");
        service_latch_auto_restore_lockout(SERVICE_AUTO_RESTORE_LOCKOUT_TDR_SPAM,
            "recovery evidence commit failed");
        return false;
    }
    debug_log("recovery ledger: recorded unique recovery evidence (recent=%u/%u)\n",
        service_count_recent_recovery_entries(compact, boot, awake),
        (unsigned)SERVICE_RESTART_LOOP_THRESHOLD);
    service_release_recovery_ledger_mutex(mutex);
    return true;
}

static bool service_record_current_recovery_evidence(
    ServiceRecoveryEvidenceKind kind, ServiceRecoveryEvidenceKey* keyOut = nullptr) {
    if (keyOut) memset(keyOut, 0, sizeof(*keyOut));
    HANDLE mutex = service_acquire_recovery_ledger_mutex();
    if (!mutex) return false;
    if (!service_recovery_evidence_key_valid(g_serviceCurrentRecoveryEvidence) &&
        !service_new_recovery_evidence_key(&g_serviceCurrentRecoveryEvidence)) {
        debug_log("recovery ledger: random evidence-key generation failed\n");
        InterlockedExchange(&g_serviceRecoveryLedgerWriteFailed, 1);
        service_latch_auto_restore_lockout(SERVICE_AUTO_RESTORE_LOCKOUT_TDR_SPAM,
            "recovery evidence identity generation failed");
        service_release_recovery_ledger_mutex(mutex);
        return false;
    }
    ServiceRecoveryEvidenceKey key = g_serviceCurrentRecoveryEvidence;
    bool ok = service_record_recovery_evidence(key, kind);
    if (!ok) {
        InterlockedExchange(&g_serviceRecoveryLedgerWriteFailed, 1);
    }
    if (ok && keyOut) *keyOut = key;
    service_release_recovery_ledger_mutex(mutex);
    return ok;
}

// Compatibility wrappers for current recovery call sites. The first
// corroborating observation creates an evidence key; the later restart request
// reuses it and is therefore not counted twice.
static void record_driver_recovery() {
    service_record_current_recovery_evidence(
        SERVICE_RECOVERY_EVIDENCE_DRIVER, nullptr);
}

static void service_record_restart_event() {
    service_record_current_recovery_evidence(
        SERVICE_RECOVERY_EVIDENCE_DRIVER, nullptr);
}

static unsigned int count_recent_driver_recoveries() {
    return service_count_recent_restarts();
}

static void service_abandon_current_recovery_evidence() {
    HANDLE mutex = service_acquire_recovery_ledger_mutex();
    if (!mutex) {
        InterlockedExchange(&g_serviceRecoveryLedgerWriteFailed, 1);
        return;
    }
    memset(&g_serviceCurrentRecoveryEvidence, 0,
        sizeof(g_serviceCurrentRecoveryEvidence));
    service_release_recovery_ledger_mutex(mutex);
}

// This acknowledgement is permitted only after an explicit successful Apply;
// automatic recovery paths must never call it.
static bool service_clear_restart_history() {
    HANDLE mutex = service_acquire_recovery_ledger_mutex();
    if (!mutex) {
        InterlockedExchange(&g_serviceRecoveryLedgerWriteFailed, 1);
        service_latch_auto_restore_lockout(SERVICE_AUTO_RESTORE_LOCKOUT_TDR_SPAM,
            "explicit recovery ledger clear could not be serialized");
        return false;
    }
    char path[MAX_PATH] = {};
    bool clearOk = true;
    bool ledgerPathReady = service_recovery_ledger_path(path, sizeof(path));
    if (!ledgerPathReady) {
        clearOk = false;
    } else if (!gc_DeleteFileUtf8(path)) {
        DWORD error = GetLastError();
        if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND) {
            clearOk = false;
        }
    }
    memset(path, 0, sizeof(path));
    bool legacyPathReady = service_old_restart_history_path(path, sizeof(path));
    if (!legacyPathReady) {
        clearOk = false;
    } else if (!gc_DeleteFileUtf8(path)) {
        DWORD error = GetLastError();
        if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND) {
            clearOk = false;
        }
    }
    memset(&g_serviceCurrentRecoveryEvidence, 0, sizeof(g_serviceCurrentRecoveryEvidence));
    InterlockedExchange(&g_serviceRecoveryLedgerWriteFailed, clearOk ? 0 : 1);
    service_release_recovery_ledger_mutex(mutex);
    debug_log("recovery ledger: explicit successful Apply clear %s\n",
        clearOk ? "succeeded" : "FAILED (automatic restoration remains locked out)");
    return clearOk;
}
