// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#include "update_freshness_policy.h"
#include "service_pipe_transport_lease.h"
#include "record_read.h"
#include "service_acl.h"
#include "installer_cli_policy.h"
#include <string.h>
#include <initializer_list>
#if defined(_WIN32)
#include <windows.h>
#include <sddl.h>
#include <aclapi.h>

static int native_preheader_lease_test() {
    WCHAR name[96] = {};
    wsprintfW(name, L"\\\\.\\pipe\\greencurve-lease-test-%lu", GetCurrentProcessId());
    HANDLE servers[6] = {}, clients[6] = {};
    ServicePipeTransportLeases leases = {};
    int result = 0;
    for (int i = 0; i < 6; ++i) {
        servers[i] = CreateNamedPipeW(name, PIPE_ACCESS_DUPLEX,
            PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
            6, 4096, 4096, 0, nullptr);
        if (servers[i] == INVALID_HANDLE_VALUE) { result = 6229; break; }
        clients[i] = CreateFileW(name, GENERIC_READ | GENERIC_WRITE, 0, nullptr,
            OPEN_EXISTING, 0, nullptr);
        if (clients[i] == INVALID_HANDLE_VALUE) { result = 6230; break; }
        if (!ConnectNamedPipe(servers[i], nullptr) && GetLastError() != ERROR_PIPE_CONNECTED) {
            result = 6231; break;
        }
        ServiceIpcThrottleKey account;
        // No WriteFile or ReadFile precedes this query: all six peers are silent.
        if (!service_pipe_transport_account(servers[i], &account)) { result = 6232; break; }
        int slot = leases.acquire(account);
        if ((i < 2) != (slot >= 0)) { result = 6233; break; }
    }
    for (int i = 0; i < 6; ++i) {
        if (clients[i] && clients[i] != INVALID_HANDLE_VALUE) CloseHandle(clients[i]);
        if (servers[i] && servers[i] != INVALID_HANDLE_VALUE) {
            DisconnectNamedPipe(servers[i]); CloseHandle(servers[i]);
        }
    }
    return result;
}
#endif

int run_security_audit_tests() {
    for (const char* device : {"CONIN$", "CONOUT$.txt", "COM\xC2\xB9.log", "LPT\xC2\xB3"}) {
        if (gc_archive_name_is_safe(device) || gc_installer_log_name_is_acceptable(device)) return 6235;
    }
    const long long now = 2000000000;
    const char envelope[] = "freshness=1\nissued=2000000000\nexpires=2000000100\nformat=1\n";
    GcUpdateFreshness fresh = {};
    if (!gc_update_fresh_parse(envelope, sizeof(envelope)-1, now, &fresh)) return 6201;
    if (strcmp(envelope + fresh.manifestOffset, "format=1\n")) return 6202;
    if (gc_update_fresh_parse(envelope, sizeof(envelope)-1, now+100, &fresh)) return 6203;
    if (gc_update_fresh_parse(envelope, sizeof(envelope)-1, now-301, &fresh)) return 6204;
    if (!gc_update_fresh_parse(envelope, sizeof(envelope)-1, now-300, &fresh)) return 6205;
    const char legacy[] = "format=1\nversion=1.2.3\n";
    if (gc_update_fresh_parse(legacy, sizeof(legacy)-1, now, &fresh)) return 6206;
    fresh = {now, now + GC_UPDATE_FRESH_MAX_LIFETIME + 1, 0};
    if (gc_update_fresh_time_valid(&fresh, now)) return 6207;
    const char overflow[] = "freshness=1\nissued=99999999999999999999\nexpires=2000000100\nformat=1\n";
    if (gc_update_fresh_parse(overflow, sizeof(overflow)-1, now, &fresh)) return 6208;
    for (size_t size = 0; size < sizeof(envelope)-1; ++size) {
        // A prefix with no manifest must never be accepted. Manifest validity
        // itself is independently enforced by the frozen parser.
        if (size <= 47 && gc_update_fresh_parse(envelope, size, now, &fresh)) return 6209;
    }

    ServicePipeTransportLeases leases = {};
    ServiceIpcThrottleKey attacker, honest, unknown;
    attacker.fill(0, 0, "S-1-5-21-100-200-300-1000");
    honest.fill(0, 0, "S-1-5-21-100-200-300-1001");
    unknown.clear();
    int a = leases.acquire(attacker), b = leases.acquire(attacker);
    if (a < 0 || b < 0 || a == b) return 6210;
    // All remaining hostile connects are refused immediately; an independent
    // account still gets transport capacity without a clock or cooldown.
    for (int i = 0; i < 6; ++i) if (leases.acquire(attacker) >= 0) return 6211;
    if (leases.acquire(honest) < 0 || leases.acquire(unknown) >= 0) return 6212;
    leases.release(a);
    if (leases.acquire(attacker) < 0) return 6213;

    ServiceIpcAdmissionTable table;
    table.reset(0);
    service_ipc_charge(&table, attacker, SERVICE_IPC_CLASS_NORMAL, 79, 0);
    service_ipc_charge(&table, attacker, SERVICE_IPC_CLASS_NORMAL, 10, 0);
    if (service_ipc_decide_admission(&table, attacker, SERVICE_IPC_CLASS_NORMAL, 0) !=
        SERVICE_IPC_REJECTED_RATE) return 6214;
    if (service_ipc_decide_admission(&table, attacker, SERVICE_IPC_CLASS_NORMAL, 50) !=
        SERVICE_IPC_ADMITTED) return 6215;

    // An expired colliding entry must not reset a later resident's quota.
    table.reset(0);
    table.acquire(honest, 7, 0);
    ServiceIpcIdentitySlot* resident = table.acquire(attacker, 7, 0);
    resident->normalBucket.tokensMilli = 0;
    resident->lastSeenMs = SERVICE_IPC_IDLE_EXPIRY_MS + 1;
    if (table.acquire(attacker, 7, SERVICE_IPC_IDLE_EXPIRY_MS + 2) != resident ||
        resident->normalBucket.tokensMilli != 0) return 6236;

    unsigned char output[4] = {};
    int calls = 0;
    auto interrupted = [&](int, void* data, size_t) -> ptrdiff_t {
        ++calls;
        if (calls == 1 || calls == 3) { errno = EINTR; return -1; }
        *(unsigned char*)data = (unsigned char)calls;
        return 1;
    };
    if (gc_read_record(interrupted, 0, output, sizeof(output)) != 4 || calls != 6 ||
        output[0] != 2 || output[1] != 4 || output[3] != 6) return 6216;
    auto error = [](int, void*, size_t) -> ptrdiff_t { errno = EIO; return -1; };
    if (gc_read_record(error, 0, output, sizeof(output)) != -1) return 6217;
    auto eof = [](int, void*, size_t) -> ptrdiff_t { return 0; };
    if (gc_read_record(eof, 0, output, sizeof(output)) != 0) return 6218;

#if defined(_WIN32)
    if (int failure = native_preheader_lease_test()) return failure;
    WCHAR directory[MAX_PATH] = {}, path[MAX_PATH] = {};
    if (!GetTempPathW(MAX_PATH, directory) ||
        !GetTempFileNameW(directory, L"gca", 0, path)) return 6219;
    HANDLE file = CreateFileW(path, GENERIC_READ | GENERIC_WRITE | READ_CONTROL | WRITE_DAC,
        0, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (file == INVALID_HANDLE_VALUE) { DeleteFileW(path); return 6220; }
    DWORD written = 0;
    const char planted[] = "[profiles]\nlogon_slot=1\n";
    bool ok = WriteFile(file, planted, sizeof(planted)-1, &written, nullptr) &&
        written == sizeof(planted)-1;
    bool discarded = false;
    char err[256] = {};
    WCHAR linkedPath[MAX_PATH] = {};
    wsprintfW(linkedPath, L"%ls.link", path);
    CloseHandle(file);
    bool linked = CreateHardLinkW(linkedPath, path, nullptr) != FALSE;
    file = CreateFileW(path, GENERIC_READ | GENERIC_WRITE | READ_CONTROL | WRITE_DAC,
        0, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    LARGE_INTEGER preserved = {};
    ok = ok && linked && !service_prepare_shared_bank_handle(file, false,
        &discarded, err, sizeof(err)) && !discarded &&
        GetFileSizeEx(file, &preserved) && preserved.QuadPart == sizeof(planted)-1;
    CloseHandle(file);
    if (linked) DeleteFileW(linkedPath);
    file = CreateFileW(path, GENERIC_READ | GENERIC_WRITE | READ_CONTROL | WRITE_DAC,
        0, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    err[0] = 0;
    ok = ok && service_prepare_shared_bank_handle(file, false, &discarded, err, sizeof(err));
    LARGE_INTEGER size = {};
    ok = ok && discarded && GetFileSizeEx(file, &size) && size.QuadPart == 0 &&
        service_handle_dacl_is_ours(file, GC_SERVICE_ACL_CONFIG);
    CloseHandle(file);
    // A named account's delete-child right and a null protected DACL must
    // both fail the diagnostics; neither was recognized by the old detector.
    PSECURITY_DESCRIPTOR sd = nullptr;
    const WCHAR* sddl = L"D:P(A;;FA;;;SY)(A;;FA;;;BA)(A;;0x40;;;S-1-5-21-100-200-300-1001)";
    bool diagnosticOk = ConvertStringSecurityDescriptorToSecurityDescriptorW(
        sddl, SDDL_REVISION_1, &sd, nullptr) != FALSE;
    PACL dacl = nullptr;
    BOOL present = FALSE, defaulted = FALSE;
    diagnosticOk = diagnosticOk && GetSecurityDescriptorDacl(sd, &present, &dacl, &defaulted) &&
        SetNamedSecurityInfoW(path, SE_FILE_OBJECT,
            DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
            nullptr, nullptr, dacl, nullptr) == ERROR_SUCCESS;
    if (sd) LocalFree(sd);
    diagnosticOk = diagnosticOk && !machine_config_dacl_is_hardened(path) &&
        !service_binary_dacl_is_hardened(path);
    diagnosticOk = diagnosticOk && SetNamedSecurityInfoW(path, SE_FILE_OBJECT,
        DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
        nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS &&
        !machine_config_dacl_is_hardened(path) && !service_binary_dacl_is_hardened(path);
    restore_inherited_dacl(path, err, sizeof(err));
    DeleteFileW(path);
    if (!ok) return 6221;
    if (!diagnosticOk) return 6234;
#endif
    return 0;
}
