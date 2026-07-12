// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// Recovery proof clock. QueryUnbiasedInterruptTime advances only while Windows
// is awake, so standby/hibernate cannot satisfy the driver-recovery proof.

#include "service_recovery_policy.h"

#define SERVICE_OC_STABILIZATION_WINDOW_MS AUTO_RESTORE_STABILITY_WINDOW_MS

static bool service_auto_restore_is_locked_out(DWORD* reasonOut);
static void service_latch_auto_restore_lockout(DWORD reason, const char* context);
static volatile LONG g_serviceOcProofInvalidated = 0;

// SystemBootEnvironmentInformation is not declared by every user-mode SDK.
// Its BootIdentifier is generated once per Windows boot and remains stable
// across wall-clock corrections, standby, and driver restarts.
struct ServiceSystemBootEnvironmentInformation {
    GUID bootIdentifier;
    ULONG firmwareType;
    ULONG reserved;
    ULONGLONG bootFlags;
};

static_assert(sizeof(ServiceSystemBootEnvironmentInformation) == 32,
    "unexpected SystemBootEnvironmentInformation layout");

static bool service_query_unbiased_time_100ns(ULONGLONG* out) {
    if (!out) return false;
    *out = 0;
    HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
    if (!kernel32) return false;
    typedef BOOL (WINAPI *QueryUnbiasedInterruptTimeFn)(PULONGLONG);
    QueryUnbiasedInterruptTimeFn query = reinterpret_cast<QueryUnbiasedInterruptTimeFn>(
        GetProcAddress(kernel32, "QueryUnbiasedInterruptTime"));
    if (!query) return false;
    ULONGLONG value = 0;
    if (!query(&value) || value == 0) return false;
    *out = value;
    return true;
}

static bool service_query_boot_identity(ServiceBootIdentity* out) {
    if (!out) return false;
    memset(out, 0, sizeof(*out));
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (!ntdll) return false;
    typedef LONG (NTAPI *NtQuerySystemInformationFn)(ULONG, PVOID, ULONG, PULONG);
    NtQuerySystemInformationFn query = reinterpret_cast<NtQuerySystemInformationFn>(
        GetProcAddress(ntdll, "NtQuerySystemInformation"));
    if (!query) return false;
    ServiceSystemBootEnvironmentInformation info = {};
    ULONG returned = 0;
    // SYSTEM_INFORMATION_CLASS::SystemBootEnvironmentInformation == 90.
    LONG status = query(90u, &info, (ULONG)sizeof(info), &returned);
    if (status < 0) {
        debug_log("oc stabilization: BootIdentifier query failed (ntstatus=0x%08lx returned=%lu)\n",
            (unsigned long)status, (unsigned long)returned);
        return false;
    }
    memcpy(&out->high, &info.bootIdentifier, sizeof(out->high));
    memcpy(&out->low,
        reinterpret_cast<const BYTE*>(&info.bootIdentifier) + sizeof(out->high),
        sizeof(out->low));
    if (!service_boot_identity_valid(*out)) {
        debug_log("oc stabilization: BootIdentifier query returned an all-zero identity\n");
        memset(out, 0, sizeof(*out));
        return false;
    }
    return true;
}

static bool service_oc_apply_stamp_path(char* out, size_t outSize) {
    if (!out || outSize == 0) return false;
    char dir[MAX_PATH] = {};
    if (!resolve_service_machine_data_dir(dir, sizeof(dir))) return false;
    return SUCCEEDED(StringCchPrintfA(out, outSize, "%s\\service_oc_apply_stamp.bin", dir));
}

static bool service_clear_oc_apply_stamp() {
    InterlockedExchange(&g_serviceOcProofInvalidated, 1);
    char path[MAX_PATH] = {};
    if (!service_oc_apply_stamp_path(path, sizeof(path))) return false;
    if (DeleteFileA(path)) return true;
    DWORD error = GetLastError();
    return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
}

static bool service_invalidate_oc_apply_proof_before_write() {
    bool ok = service_clear_oc_apply_stamp();
    if (!ok) {
        debug_log("oc stabilization: old proof could not be invalidated before hardware write; automatic restore is locked out\n");
        service_latch_auto_restore_lockout(
            SERVICE_AUTO_RESTORE_LOCKOUT_AUTOMATIC_APPLY_FAILED,
            "old stability proof could not be invalidated before a hardware write");
    }
    return ok;
}

static bool service_commit_oc_apply_stamp(
    const ServiceOcApplyProofStamp& stamp, const char* successKind) {
    if (stamp.magic != SERVICE_OC_APPLY_STAMP_MAGIC ||
        stamp.version != SERVICE_OC_APPLY_STAMP_VERSION ||
        stamp.size != sizeof(stamp) || stamp.reserved != 0 ||
        !service_boot_identity_valid(stamp.bootIdentity) ||
        stamp.awakeTime100ns == 0) {
        service_clear_oc_apply_stamp();
        service_latch_auto_restore_lockout(
            SERVICE_AUTO_RESTORE_LOCKOUT_AUTOMATIC_APPLY_FAILED,
            "invalid stability proof could not be committed");
        return false;
    }
    char path[MAX_PATH] = {};
    if (!service_oc_apply_stamp_path(path, sizeof(path))) {
        service_latch_auto_restore_lockout(
            SERVICE_AUTO_RESTORE_LOCKOUT_AUTOMATIC_APPLY_FAILED,
            "new stability proof path could not be resolved");
        return false;
    }
    char pathErr[256] = {};
    if (!ensure_parent_directory_for_file(path, pathErr, sizeof(pathErr))) {
        debug_log("oc stabilization: cannot create proof directory: %s\n",
            pathErr[0] ? pathErr : "unknown error");
        service_clear_oc_apply_stamp();
        service_latch_auto_restore_lockout(
            SERVICE_AUTO_RESTORE_LOCKOUT_AUTOMATIC_APPLY_FAILED,
            "new stability proof directory could not be created");
        return false;
    }

    char tempPath[MAX_PATH] = {};
    if (FAILED(StringCchPrintfA(tempPath, ARRAY_COUNT(tempPath), "%s.tmp.%lu.%llu", path,
            (unsigned long)GetCurrentProcessId(),
            (unsigned long long)GetTickCount64()))) {
        service_clear_oc_apply_stamp();
        service_latch_auto_restore_lockout(
            SERVICE_AUTO_RESTORE_LOCKOUT_AUTOMATIC_APPLY_FAILED,
            "new stability proof temporary path could not be formed");
        return false;
    }
    HANDLE h = CreateFileA(tempPath, GENERIC_WRITE, 0, nullptr, CREATE_NEW,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        debug_log("oc stabilization: proof temp create failed (error=%lu)\n", GetLastError());
        service_clear_oc_apply_stamp();
        service_latch_auto_restore_lockout(
            SERVICE_AUTO_RESTORE_LOCKOUT_AUTOMATIC_APPLY_FAILED,
            "new stability proof file could not be created");
        return false;
    }
    DWORD written = 0;
    bool ok = WriteFile(h, &stamp, sizeof(stamp), &written, nullptr) &&
        written == sizeof(stamp) && FlushFileBuffers(h) != FALSE;
    CloseHandle(h);
    if (ok) {
        ok = MoveFileExA(tempPath, path,
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
    }
    if (!ok) {
        DWORD error = GetLastError();
        DeleteFileA(tempPath);
        service_clear_oc_apply_stamp();
        debug_log("oc stabilization: proof commit failed (error=%lu); no proof retained\n", error);
        service_latch_auto_restore_lockout(
            SERVICE_AUTO_RESTORE_LOCKOUT_AUTOMATIC_APPLY_FAILED,
            "new stability proof commit failed");
        return false;
    }
    debug_log("oc stabilization: %s v%u proof boot=%016llx-%016llx awake100ns=%llu (window=%llu ms)\n",
        successKind && successKind[0] ? successKind : "committed",
        (unsigned)SERVICE_OC_APPLY_STAMP_VERSION,
        (unsigned long long)stamp.bootIdentity.high,
        (unsigned long long)stamp.bootIdentity.low,
        (unsigned long long)stamp.awakeTime100ns,
        (unsigned long long)SERVICE_OC_STABILIZATION_WINDOW_MS);
    InterlockedExchange(&g_serviceOcProofInvalidated, 0);
    return true;
}

static bool service_record_oc_apply_stamp() {
    ServiceOcApplyProofStamp stamp = {};
    stamp.magic = SERVICE_OC_APPLY_STAMP_MAGIC;
    stamp.version = SERVICE_OC_APPLY_STAMP_VERSION;
    stamp.size = sizeof(stamp);
    if (!service_query_boot_identity(&stamp.bootIdentity) ||
        !service_query_unbiased_time_100ns(&stamp.awakeTime100ns)) {
        debug_log("oc stabilization: proof clock unavailable; deleting prior proof (fail closed)\n");
        service_clear_oc_apply_stamp();
        service_latch_auto_restore_lockout(
            SERVICE_AUTO_RESTORE_LOCKOUT_AUTOMATIC_APPLY_FAILED,
            "new stability proof clock was unavailable");
        return false;
    }
    return service_commit_oc_apply_stamp(stamp, "recorded fresh");
}

static bool service_read_oc_apply_proof(ServiceOcApplyProofStamp* out) {
    if (!out) return false;
    memset(out, 0, sizeof(*out));
    char path[MAX_PATH] = {};
    if (!service_oc_apply_stamp_path(path, sizeof(path))) return false;
    HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        DWORD error = GetLastError();
        if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND) {
            debug_log("oc stabilization: proof unreadable (error=%lu); failing closed\n", error);
        }
        return false;
    }
    DWORD read = 0;
    BYTE trailing = 0;
    bool ok = ReadFile(h, out, sizeof(*out), &read, nullptr) && read == sizeof(*out);
    DWORD trailingRead = 0;
    if (ok) ok = ReadFile(h, &trailing, sizeof(trailing), &trailingRead, nullptr) && trailingRead == 0;
    CloseHandle(h);
    if (!ok || out->magic != SERVICE_OC_APPLY_STAMP_MAGIC ||
        out->version != SERVICE_OC_APPLY_STAMP_VERSION ||
        out->size != sizeof(*out) || out->reserved != 0 ||
        !service_boot_identity_valid(out->bootIdentity) ||
        out->awakeTime100ns == 0) {
        debug_log("oc stabilization: rejected legacy/corrupt proof stamp; a new successful apply is required\n");
        DeleteFileA(path);
        memset(out, 0, sizeof(*out));
        return false;
    }
    return true;
}

static bool service_capture_mature_oc_apply_proof(
    ServiceOcApplyProofStamp* out, ULONGLONG* proofAgeMsOut = nullptr) {
    if (out) memset(out, 0, sizeof(*out));
    if (proofAgeMsOut) *proofAgeMsOut = 0;
    if (!out || InterlockedExchangeAdd(&g_serviceOcProofInvalidated, 0) != 0 ||
        !service_read_oc_apply_proof(out)) return false;
    ServiceBootIdentity currentBoot = {};
    ULONGLONG currentAwake = 0;
    ULONGLONG ageMs = 0;
    bool valid = service_query_boot_identity(&currentBoot) &&
        service_query_unbiased_time_100ns(&currentAwake) &&
        service_compute_proof_age_ms(out, currentBoot, currentAwake, &ageMs);
    if (proofAgeMsOut) *proofAgeMsOut = ageMs;
    if (!service_should_preserve_proof_after_standby(valid, ageMs,
            SERVICE_OC_STABILIZATION_WINDOW_MS)) {
        memset(out, 0, sizeof(*out));
        return false;
    }
    debug_log("oc stabilization: standby captured mature proof age=%llu ms for preservation\n",
        (unsigned long long)ageMs);
    return true;
}

static bool service_restore_mature_oc_apply_proof(
    const ServiceOcApplyProofStamp& stamp) {
    ServiceBootIdentity currentBoot = {};
    ULONGLONG currentAwake = 0;
    ULONGLONG ageMs = 0;
    bool valid = service_query_boot_identity(&currentBoot) &&
        service_query_unbiased_time_100ns(&currentAwake) &&
        service_compute_proof_age_ms(&stamp, currentBoot, currentAwake, &ageMs);
    if (!service_should_preserve_proof_after_standby(valid, ageMs,
            SERVICE_OC_STABILIZATION_WINDOW_MS)) {
        service_clear_oc_apply_stamp();
        service_latch_auto_restore_lockout(
            SERVICE_AUTO_RESTORE_LOCKOUT_AUTOMATIC_APPLY_FAILED,
            "mature standby proof could not be revalidated after restore");
        return false;
    }
    return service_commit_oc_apply_stamp(stamp, "preserved mature standby");
}

static bool service_auto_restore_allowed_after_driver_event(DWORD* lockoutReasonOut = nullptr,
    ULONGLONG* successfulApplyAgeMsOut = nullptr) {
    if (lockoutReasonOut) *lockoutReasonOut = SERVICE_AUTO_RESTORE_LOCKOUT_NONE;
    if (successfulApplyAgeMsOut) *successfulApplyAgeMsOut = 0;
    if (InterlockedExchangeAdd(&g_serviceOcProofInvalidated, 0) != 0) return false;
    DWORD lockoutReason = SERVICE_AUTO_RESTORE_LOCKOUT_NONE;
    if (service_auto_restore_is_locked_out(&lockoutReason)) {
        if (lockoutReasonOut) *lockoutReasonOut = lockoutReason;
        return false;
    }
    ServiceOcApplyProofStamp stamp = {};
    if (!service_read_oc_apply_proof(&stamp)) return false;
    ServiceBootIdentity bootIdentity = {};
    ULONGLONG awakeTime100ns = 0;
    if (!service_query_boot_identity(&bootIdentity) ||
        !service_query_unbiased_time_100ns(&awakeTime100ns)) {
        debug_log("oc stabilization: proof clock unavailable during eligibility check (fail closed)\n");
        return false;
    }
    ULONGLONG ageMs = 0;
    if (!service_compute_proof_age_ms(&stamp, bootIdentity, awakeTime100ns, &ageMs)) {
        debug_log("oc stabilization: proof boot/clock mismatch (proofBoot=%016llx-%016llx currentBoot=%016llx-%016llx proofAwake=%llu currentAwake=%llu; fail closed)\n",
            (unsigned long long)stamp.bootIdentity.high,
            (unsigned long long)stamp.bootIdentity.low,
            (unsigned long long)bootIdentity.high,
            (unsigned long long)bootIdentity.low,
            (unsigned long long)stamp.awakeTime100ns,
            (unsigned long long)awakeTime100ns);
        return false;
    }
    if (successfulApplyAgeMsOut) *successfulApplyAgeMsOut = ageMs;
    return should_auto_restore_after_driver_event(true, ageMs, false);
}
