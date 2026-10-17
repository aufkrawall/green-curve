// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#include "update_freshness_policy.h"
#include "service_pipe_transport_lease.h"
#include "record_read.h"
#include "service_acl.h"
#include "installer_cli_policy.h"
#include <stdio.h>
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

#if defined(_WIN32)
bool gc_test_make_junction(const wchar_t* junctionPath, const wchar_t* targetPath);

// Undo the fixture's protected DACLs (the harness is the owner, so it may)
// and remove one level of entries plus the directory itself.
static void claim_test_remove_tree(const WCHAR* dir) {
    char ignored[160] = {};
    restore_inherited_dacl(dir, ignored, sizeof(ignored));
    WCHAR pattern[MAX_PATH] = {};
    wsprintfW(pattern, L"%ls\\*", dir);
    WIN32_FIND_DATAW data = {};
    HANDLE find = FindFirstFileW(pattern, &data);
    if (find != INVALID_HANDLE_VALUE) {
        do {
            if (!wcscmp(data.cFileName, L".") || !wcscmp(data.cFileName, L"..")) continue;
            WCHAR child[MAX_PATH] = {};
            wsprintfW(child, L"%ls\\%ls", dir, data.cFileName);
            restore_inherited_dacl(child, ignored, sizeof(ignored));
            if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) RemoveDirectoryW(child);
            else DeleteFileW(child);
        } while (FindNextFileW(find, &data));
        FindClose(find);
    }
    RemoveDirectoryW(dir);
}

static bool claim_test_is_elevated() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
    TOKEN_ELEVATION elevation = {};
    DWORD size = 0;
    bool elevated = GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation),
                                        &size) && elevation.TokenIsElevated;
    CloseHandle(token);
    return elevated;
}

// Every "<path>.unproven-*" sibling: counted, then removed.
static int claim_test_sweep_aside(const WCHAR* path, bool remove) {
    WCHAR pattern[MAX_PATH] = {};
    wsprintfW(pattern, L"%ls.unproven-*", path);
    WCHAR parent[MAX_PATH] = {};
    lstrcpynW(parent, path, MAX_PATH);
    WCHAR* slash = wcsrchr(parent, L'\\');
    if (slash) *slash = 0;
    WIN32_FIND_DATAW data = {};
    HANDLE find = FindFirstFileW(pattern, &data);
    int count = 0;
    if (find == INVALID_HANDLE_VALUE) return 0;
    do {
        count++;
        if (!remove) continue;
        WCHAR aside[MAX_PATH] = {};
        wsprintfW(aside, L"%ls\\%ls", parent, data.cFileName);
        if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) claim_test_remove_tree(aside);
        else DeleteFileW(aside);
    } while (FindNextFileW(find, &data));
    FindClose(find);
    return count;
}

static bool claim_test_is_protected(const WCHAR* path) {
    PSECURITY_DESCRIPTOR sd = nullptr;
    PACL dacl = nullptr;
    if (GetNamedSecurityInfoW(path, SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr,
                              nullptr, &dacl, nullptr, &sd) != ERROR_SUCCESS || !sd)
        return false;
    SECURITY_DESCRIPTOR_CONTROL control = 0;
    DWORD revision = 0;
    bool isProtected = GetSecurityDescriptorControl(sd, &control, &revision) &&
        (control & SE_DACL_PROTECTED) != 0;
    LocalFree(sd);
    return isProtected;
}

// Never adopt what we did not create: claim/replace/fresh-only semantics,
// handle-only DACL proof, junction and file squatters, the private kinds, and
// the pre-launch directory content proof.  Unelevated (requireAdminOwner
// false) except for the owner case, which must never adopt a user-owned
// folder even when its DACL is exactly ours.
static int claim_protected_directory_tests() {
    WCHAR base[MAX_PATH] = {}, dir[MAX_PATH] = {}, planted[MAX_PATH] = {};
    WCHAR target[MAX_PATH] = {}, targetFile[MAX_PATH] = {};
    if (!GetTempPathW(MAX_PATH, base)) return 6600;
    wsprintfW(dir, L"%lsgc_claim_%lu", base, GetCurrentProcessId());
    wsprintfW(planted, L"%ls\\planted.dll", dir);
    wsprintfW(target, L"%lsgc_claim_target_%lu", base, GetCurrentProcessId());
    wsprintfW(targetFile, L"%ls\\keep.txt", target);
    char err[256] = {};
    auto cleanupAnd = [&](int code) -> int {
        // The helper's own reason, so a failure here is diagnosable from the log.
        if (code) fprintf(stderr, "claim suite: %d failed (%s)\n", code, err[0] ? err : "-");
        claim_test_sweep_aside(dir, true);
        claim_test_remove_tree(dir);
        DeleteFileW(dir);
        claim_test_remove_tree(target);
        return code;
    };
    cleanupAnd(0);
    void* pinned = nullptr;

    // Absent: created with the descriptor in the call, pinned against rename.
    int r = service_claim_protected_directory(dir, GC_SERVICE_ACL_DIRECTORY, false, true,
                                              &pinned, err, sizeof(err));
    if (r != GC_SERVICE_CLAIM_CREATED || pinned == INVALID_HANDLE_VALUE) return cleanupAnd(6601);
    bool pinOk = service_handle_dacl_is_ours(pinned, GC_SERVICE_ACL_DIRECTORY);
    WCHAR moved[MAX_PATH] = {};
    wsprintfW(moved, L"%ls.moved", dir);
    bool renamed = MoveFileW(dir, moved) != FALSE;
    if (renamed) MoveFileW(moved, dir);
    CloseHandle((HANDLE)pinned);
    if (!pinOk || renamed) return cleanupAnd(6602);
    // Already provably ours: adopted, nothing set aside.
    r = service_claim_protected_directory(dir, GC_SERVICE_ACL_DIRECTORY, false, true,
                                          nullptr, err, sizeof(err));
    if (r != GC_SERVICE_CLAIM_ADOPTED || claim_test_sweep_aside(dir, false) != 0)
        return cleanupAnd(6603);
    // Fresh-only never accepts an existing name, even one that is ours.
    if (service_claim_protected_directory(dir, GC_SERVICE_ACL_DIRECTORY, false, false,
                                          nullptr, err, sizeof(err)) != GC_SERVICE_CLAIM_FAILED)
        return cleanupAnd(6604);
    // The private kind is a different, stricter DACL: not interchangeable.
    // Only a privileged caller can open a SYSTEM/Administrators-only folder at
    // all (CreateFileW always asks for SYNCHRONIZE), so unelevated the fresh
    // private folder cannot be proven and the claim fails closed.
    const bool elevated = claim_test_is_elevated();
    r = service_claim_protected_directory(dir, GC_SERVICE_ACL_PRIVATE_DIRECTORY, false, true,
                                          nullptr, err, sizeof(err));
    if (r != (elevated ? GC_SERVICE_CLAIM_REPLACED : GC_SERVICE_CLAIM_FAILED))
        return cleanupAnd(6605);
    if (service_path_dacl_is_ours(dir, GC_SERVICE_ACL_DIRECTORY) ||
        (elevated && !service_path_dacl_is_ours(dir, GC_SERVICE_ACL_PRIVATE_DIRECTORY)))
        return cleanupAnd(6606);
    cleanupAnd(0);

    // A squatted folder with content: set aside with its content intact; the
    // claimed folder is new and empty.
    if (!CreateDirectoryW(dir, nullptr)) return cleanupAnd(6607);
    HANDLE plant = CreateFileW(planted, GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                               FILE_ATTRIBUTE_NORMAL, nullptr);
    if (plant == INVALID_HANDLE_VALUE) return cleanupAnd(6608);
    CloseHandle(plant);
    r = service_claim_protected_directory(dir, GC_SERVICE_ACL_DIRECTORY, false, true,
                                          nullptr, err, sizeof(err));
    if (r != GC_SERVICE_CLAIM_REPLACED || GetFileAttributesW(planted) != INVALID_FILE_ATTRIBUTES ||
        !service_path_dacl_is_ours(dir, GC_SERVICE_ACL_DIRECTORY) ||
        claim_test_sweep_aside(dir, false) != 1) return cleanupAnd(6609);
    // Set aside BESIDE the original, never relative to the current directory
    // (kernel32's rename would have moved it there).
    WCHAR cwdPattern[MAX_PATH] = {};
    wsprintfW(cwdPattern, L"gc_claim_%lu.unproven-*", GetCurrentProcessId());
    WIN32_FIND_DATAW cwdData = {};
    HANDLE cwdFind = FindFirstFileW(cwdPattern, &cwdData);
    if (cwdFind != INVALID_HANDLE_VALUE) { FindClose(cwdFind); return cleanupAnd(6632); }
    cleanupAnd(0);

    // A junction squatting the name: the LINK is moved; its target keeps its
    // inherited DACL and its content.
    if (!CreateDirectoryW(target, nullptr)) return cleanupAnd(6610);
    plant = CreateFileW(targetFile, GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (plant == INVALID_HANDLE_VALUE) return cleanupAnd(6611);
    CloseHandle(plant);
    if (!gc_test_make_junction(dir, target)) return cleanupAnd(6612);
    r = service_claim_protected_directory(dir, GC_SERVICE_ACL_DIRECTORY, false, true,
                                          nullptr, err, sizeof(err));
    DWORD dirAttrs = GetFileAttributesW(dir);
    if (r != GC_SERVICE_CLAIM_REPLACED || dirAttrs == INVALID_FILE_ATTRIBUTES ||
        (dirAttrs & FILE_ATTRIBUTE_REPARSE_POINT) != 0 ||
        claim_test_is_protected(target) || claim_test_is_protected(targetFile) ||
        GetFileAttributesW(targetFile) == INVALID_FILE_ATTRIBUTES) return cleanupAnd(6613);
    cleanupAnd(0);

    // A file squatting the name is set aside and deleted.
    plant = CreateFileW(dir, GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (plant == INVALID_HANDLE_VALUE) return cleanupAnd(6614);
    CloseHandle(plant);
    r = service_claim_protected_directory(dir, GC_SERVICE_ACL_DIRECTORY, false, true,
                                          nullptr, err, sizeof(err));
    dirAttrs = GetFileAttributesW(dir);
    if (r != GC_SERVICE_CLAIM_REPLACED || dirAttrs == INVALID_FILE_ATTRIBUTES ||
        !(dirAttrs & FILE_ATTRIBUTE_DIRECTORY) || claim_test_sweep_aside(dir, false) != 0)
        return cleanupAnd(6615);
    cleanupAnd(0);

    // Exactly our DACL but a standard-user owner (this harness): an owner
    // keeps implicit WRITE_DAC, so with the owner required it is never adopted.
    if (service_claim_protected_directory(dir, GC_SERVICE_ACL_DIRECTORY, false, true,
                                          nullptr, err, sizeof(err)) != GC_SERVICE_CLAIM_CREATED)
        return cleanupAnd(6616);
    BY_HANDLE_FILE_INFORMATION before = {}, after = {};
    HANDLE probe = CreateFileW(dir, FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE |
        FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    bool haveBefore = probe != INVALID_HANDLE_VALUE && GetFileInformationByHandle(probe, &before);
    if (probe != INVALID_HANDLE_VALUE) CloseHandle(probe);
    r = service_claim_protected_directory(dir, GC_SERVICE_ACL_DIRECTORY, true, true,
                                          nullptr, err, sizeof(err));
    probe = CreateFileW(dir, FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE |
        FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    bool sameObject = probe != INVALID_HANDLE_VALUE && GetFileInformationByHandle(probe, &after) &&
        after.nFileIndexHigh == before.nFileIndexHigh && after.nFileIndexLow == before.nFileIndexLow;
    if (probe != INVALID_HANDLE_VALUE) CloseHandle(probe);
    // Unelevated the fresh O:BA directory cannot be created (FAILED); elevated
    // it is REPLACED.  Either way the user-owned folder no longer holds the name.
    if (!haveBefore || r == GC_SERVICE_CLAIM_ADOPTED || sameObject) return cleanupAnd(6617);
    cleanupAnd(0);

    // Fresh-only private claim, pinned.  An empty folder never passes the
    // content proof.
    if (elevated) {
        r = service_claim_protected_directory(dir, GC_SERVICE_ACL_PRIVATE_DIRECTORY, true, false,
                                              &pinned, err, sizeof(err));
        if (r != GC_SERVICE_CLAIM_CREATED || pinned == INVALID_HANDLE_VALUE)
            return cleanupAnd(6618);
        bool emptyRefused = !service_directory_holds_only(pinned,
            GC_SERVICE_ACL_PRIVATE_DIRECTORY, true, L"setup.exe", err, sizeof(err));
        bool ownerProven = service_handle_owner_is_administrators(pinned);
        CloseHandle((HANDLE)pinned);
        if (!emptyRefused || !ownerProven) return cleanupAnd(6619);
        cleanupAnd(0);
    }

    // The launch shape's content proof, on a Users-listable folder the
    // unelevated harness can enumerate: exactly one regular file by name.
    if (!CreateDirectoryW(dir, nullptr)) return cleanupAnd(6620);
    WCHAR setup[MAX_PATH] = {}, extra[MAX_PATH] = {};
    wsprintfW(setup, L"%ls\\setup.exe", dir);
    wsprintfW(extra, L"%ls\\neighbour.dll", dir);
    HANDLE created = (HANDLE)service_create_protected_file(setup, GC_SERVICE_ACL_PRIVATE_FILE,
        false, GENERIC_WRITE | READ_CONTROL, 0, err, sizeof(err));
    if (created == INVALID_HANDLE_VALUE) return cleanupAnd(6621);
    bool fileOurs = service_handle_dacl_is_ours(created, GC_SERVICE_ACL_PRIVATE_FILE) &&
        !service_handle_dacl_is_ours(created, GC_SERVICE_ACL_BINARY);
    CloseHandle(created);
    if (!fileOurs) return cleanupAnd(6622);
    // CREATE_NEW: an existing name is never reused or truncated.
    created = (HANDLE)service_create_protected_file(setup, GC_SERVICE_ACL_PRIVATE_FILE, false,
        GENERIC_WRITE, 0, err, sizeof(err));
    if (created != INVALID_HANDLE_VALUE) { CloseHandle(created); return cleanupAnd(6623); }
    HANDLE plantedDll = CreateFileW(extra, GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                                    FILE_ATTRIBUTE_NORMAL, nullptr);
    if (plantedDll == INVALID_HANDLE_VALUE) return cleanupAnd(6624);
    CloseHandle(plantedDll);
    HANDLE listed = CreateFileW(dir, READ_CONTROL | WRITE_DAC | FILE_LIST_DIRECTORY |
        FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (listed == INVALID_HANDLE_VALUE) return cleanupAnd(6625);
    auto closeListedAnd = [&](int code) -> int {
        CloseHandle(listed);
        return cleanupAnd(code);
    };
    // Not yet our DACL: refused before any listing.
    if (service_directory_holds_only(listed, GC_SERVICE_ACL_DIRECTORY, false, L"setup.exe",
                                     err, sizeof(err))) return closeListedAnd(6626);
    char aclErr[160] = {};
    if (!apply_protected_service_dacl_to_handle(listed, GC_SERVICE_ACL_DIRECTORY, false,
                                                aclErr, sizeof(aclErr))) return closeListedAnd(6627);
    // A planted neighbour fails the proof.
    if (service_directory_holds_only(listed, GC_SERVICE_ACL_DIRECTORY, false, L"setup.exe",
                                     err, sizeof(err))) return closeListedAnd(6628);
    restore_inherited_dacl(dir, aclErr, sizeof(aclErr));
    restore_inherited_dacl(extra, aclErr, sizeof(aclErr));
    if (!DeleteFileW(extra) ||
        !apply_protected_service_dacl_to_handle(listed, GC_SERVICE_ACL_DIRECTORY, false,
                                                aclErr, sizeof(aclErr))) return closeListedAnd(6629);
    // Exactly the expected file (name compared case-insensitively) passes;
    // any other expected name does not.
    if (!service_directory_holds_only(listed, GC_SERVICE_ACL_DIRECTORY, false, L"SETUP.EXE",
                                      err, sizeof(err)) ||
        service_directory_holds_only(listed, GC_SERVICE_ACL_DIRECTORY, false, L"other.exe",
                                     err, sizeof(err))) return closeListedAnd(6630);
    // A widened DACL on the pinned directory fails the pre-launch proof.
    restore_inherited_dacl(dir, aclErr, sizeof(aclErr));
    if (service_directory_holds_only(listed, GC_SERVICE_ACL_DIRECTORY, false, L"setup.exe",
                                     err, sizeof(err))) return closeListedAnd(6631);
    return closeListedAnd(0);
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
    bool invalidDiscarded = true;
    char invalidBankError[128] = {};
    if (service_prepare_shared_bank_handle(nullptr, nullptr, false,
            &invalidDiscarded, invalidBankError, sizeof(invalidBankError)) ||
        invalidDiscarded || !invalidBankError[0]) return 6925;
    if (service_prepare_shared_bank_handle(nullptr, nullptr, false,
            nullptr, nullptr, 0)) return 6926;
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
    // An unproven bank -- here also hard-linked elsewhere -- is never adopted
    // or written through: only its name leaves the bank directory, and the
    // other link keeps every byte.
    file = CreateFileW(path, GENERIC_READ | GENERIC_WRITE | READ_CONTROL | WRITE_DAC,
        FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    void* bank = file;
    ok = ok && linked && file != INVALID_HANDLE_VALUE &&
        service_prepare_shared_bank_handle(&bank, path, false, &discarded, err, sizeof(err)) &&
        discarded && bank == INVALID_HANDLE_VALUE &&
        GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES;
    if (bank != INVALID_HANDLE_VALUE) CloseHandle((HANDLE)bank);
    WIN32_FILE_ATTRIBUTE_DATA linkedData = {};
    ok = ok && GetFileAttributesExW(linkedPath, GetFileExInfoStandard, &linkedData) &&
        linkedData.nFileSizeLow == sizeof(planted)-1;
    // The set-aside name was marked for deletion and is gone once closed.
    WCHAR asidePattern[MAX_PATH] = {};
    wsprintfW(asidePattern, L"%ls.unproven-*", path);
    WIN32_FIND_DATAW asideData = {};
    HANDLE aside = FindFirstFileW(asidePattern, &asideData);
    if (aside != INVALID_HANDLE_VALUE) { FindClose(aside); ok = false; }
    // While another opener denies delete sharing the name cannot move, so
    // the bank is refused rather than adopted, and keeps every byte.
    if (linked) ok = ok && MoveFileW(linkedPath, path);
    HANDLE holder = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
        OPEN_EXISTING, 0, nullptr);
    file = CreateFileW(path, GENERIC_READ | GENERIC_WRITE | READ_CONTROL | WRITE_DAC,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT,
        nullptr);
    discarded = false;
    bank = file;
    ok = ok && holder != INVALID_HANDLE_VALUE && file != INVALID_HANDLE_VALUE &&
        !service_prepare_shared_bank_handle(&bank, path, false, &discarded, err, sizeof(err)) &&
        !discarded && GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES;
    if (bank != INVALID_HANDLE_VALUE) CloseHandle((HANDLE)bank);
    if (holder != INVALID_HANDLE_VALUE) CloseHandle(holder);
    WIN32_FILE_ATTRIBUTE_DATA keptData = {};
    ok = ok && GetFileAttributesExW(path, GetFileExInfoStandard, &keptData) &&
        keptData.nFileSizeLow == sizeof(planted)-1;
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
    DeleteFileW(linkedPath);
    if (!ok) return 6221;
    if (!diagnosticOk) return 6234;
    if (int failure = claim_protected_directory_tests()) return failure;
#endif
    return 0;
}
