// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// Service registration and install-location regressions (assertion codes
// 5720-5799).  Host-neutral policy first; the Windows half drives the real
// handle-bound ACL functions and the location gate against temp fixtures.
//
// The fixtures that harden a temp folder run unelevated, so the owner change
// is refused and the folder keeps the test user as owner: that owner's
// implicit WRITE_DAC is what lets release_service_hardening() give the folder
// back for cleanup.  Every exit path releases before deleting.

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <winioctl.h>
#include <aclapi.h>
#include <sddl.h>
#include <shlobj.h>
#include <strsafe.h>
#endif
#include <cstring>
#include <cwchar>

#include "service_acl.h"
#include "service_admin_reason_policy.h"
#include "service_install_location_policy.h"
#include "service_scm_wait_policy.h"
#include "installer_transaction_files.h"

namespace {

int run_scm_wait_policy_tests() {
    GcScmWaitTracker t = {};
    gc_scm_wait_begin(&t, 1000);
    if (gc_scm_wait_step(&t, 1000, GC_SCM_STATE_RUNNING, 0, 0, GC_SCM_STATE_RUNNING) !=
        GC_SCM_WAIT_REACHED) return 5720;
    // A start that failed comes back to STOPPED: finished, not "still waiting".
    gc_scm_wait_begin(&t, 0);
    if (gc_scm_wait_step(&t, 800, GC_SCM_STATE_STOPPED, 0, 20000, GC_SCM_STATE_RUNNING) !=
        GC_SCM_WAIT_SETTLED_ELSEWHERE) return 5721;
    // Progress resets the stall clock; the hint is the budget between reports.
    gc_scm_wait_begin(&t, 0);
    if (gc_scm_wait_step(&t, 0, GC_SCM_STATE_START_PENDING, 1, 20000, GC_SCM_STATE_RUNNING) !=
        GC_SCM_WAIT_CONTINUE) return 5722;
    if (gc_scm_wait_step(&t, 15000, GC_SCM_STATE_START_PENDING, 2, 20000, GC_SCM_STATE_RUNNING) !=
        GC_SCM_WAIT_CONTINUE) return 5723;
    // 35000 ms total is past the OLD fixed 30 s deadline, yet still healthy.
    if (gc_scm_wait_step(&t, 35000, GC_SCM_STATE_START_PENDING, 2, 20000, GC_SCM_STATE_RUNNING) !=
        GC_SCM_WAIT_CONTINUE) return 5724;
    if (gc_scm_wait_step(&t, 35001, GC_SCM_STATE_START_PENDING, 2, 20000, GC_SCM_STATE_RUNNING) !=
        GC_SCM_WAIT_STALLED) return 5725;
    // A service that keeps reporting progress is still bounded overall.
    gc_scm_wait_begin(&t, 0);
    int verdict = GC_SCM_WAIT_CONTINUE;
    unsigned long cp = 1;
    unsigned long long now = 0;
    for (; now <= GC_SCM_WAIT_MAX_TOTAL_MS && verdict == GC_SCM_WAIT_CONTINUE; now += 1000, cp++)
        verdict = gc_scm_wait_step(&t, now, GC_SCM_STATE_START_PENDING, cp, 20000, GC_SCM_STATE_RUNNING);
    if (verdict != GC_SCM_WAIT_OVERALL_TIMEOUT) return 5726;
    // Stall budgets: no hint = the SCM default; tiny hints are floored.
    if (gc_scm_wait_stall_budget_ms(0) != GC_SVC_SCM_STATE_WAIT_MS) return 5727;
    if (gc_scm_wait_stall_budget_ms(100) != GC_SCM_WAIT_MIN_STALL_MS) return 5728;
    if (gc_scm_wait_stall_budget_ms(600000) != GC_SCM_WAIT_MAX_TOTAL_MS) return 5729;
    if (gc_scm_wait_poll_interval_ms(0) != 250 || gc_scm_wait_poll_interval_ms(3000) != 300 ||
        gc_scm_wait_poll_interval_ms(20000) != 500) return 5730;
    // A state change is progress even when the checkpoint number repeats.
    gc_scm_wait_begin(&t, 0);
    gc_scm_wait_step(&t, 0, GC_SCM_STATE_STOP_PENDING, 1, 3000, GC_SCM_STATE_STOPPED);
    if (gc_scm_wait_step(&t, 2500, GC_SCM_STATE_START_PENDING, 1, 3000, GC_SCM_STATE_STOPPED) !=
        GC_SCM_WAIT_CONTINUE) return 5731;
    if (gc_scm_wait_step(&t, 5000, GC_SCM_STATE_START_PENDING, 1, 3000, GC_SCM_STATE_STOPPED) !=
        GC_SCM_WAIT_CONTINUE) return 5732;
    if (gc_scm_wait_step(&t, 5501, GC_SCM_STATE_START_PENDING, 1, 3000, GC_SCM_STATE_STOPPED) !=
        GC_SCM_WAIT_STALLED) return 5733;
    for (int v = GC_SCM_WAIT_CONTINUE; v <= GC_SCM_WAIT_OVERALL_TIMEOUT; v++)
        if (strcmp(gc_scm_wait_verdict_name(v), "unknown") == 0) return 5734;
    return 0;
}

int run_location_content_policy_tests() {
    auto ours = [](const wchar_t* name, bool isDirectory) {
        return gc_service_location_entry_is_ours(name, wcslen(name), isDirectory);
    };
    if (!ours(L"greencurve.exe", false) || !ours(L"GREENCURVE-SERVICE.EXE", false) ||
        !ours(L"README.md", false) || !ours(L"LICENSE", false) || !ours(L"uninstall.exe", false))
        return 5735;
    if (!ours(L"greencurve-service.exe.gcnew", false) || !ours(L"desktop.ini", false) ||
        !ours(L"machine.ini", false)) return 5736;
    // Anything else -- a prefix, a suffix, a user's file, ANY folder -- is foreign.
    if (ours(L"greencurve.ex", false) || ours(L"greencurve.exe.bak", false) ||
        ours(L"notes.txt", false) || ours(L"LICENSE", true) || ours(L"", false)) return 5737;
    if (!gc_service_location_is_within(L"C:\\Windows\\Temp", L"C:\\Windows") ||
        !gc_service_location_is_within(L"c:\\windows", L"C:\\Windows\\") ||
        !gc_service_location_is_within(L"C:/Windows/System32/x", L"C:\\Windows")) return 5738;
    if (gc_service_location_is_within(L"C:\\WindowsApps", L"C:\\Windows") ||
        gc_service_location_is_within(L"C:\\Win", L"C:\\Windows")) return 5739;
    for (int v = GC_SVC_LOCATION_OK; v < GC_SVC_LOCATION_VERDICT_COUNT; v++)
        if (strcmp(gc_service_location_verdict_name(v), "unknown") == 0) return 5740;
    return 0;
}

#if defined(_WIN32)

bool sd_from_sddl_is_ours(const wchar_t* sddl, GcServiceAclKind kind) {
    PSECURITY_DESCRIPTOR sd = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl, SDDL_REVISION_1, &sd, nullptr))
        return false;
    bool ours = service_security_descriptor_is_ours(sd, kind);
    LocalFree(sd);
    return ours;
}

int run_exact_dacl_tests() {
    const wchar_t* dir = L"D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICI;0x1200a9;;;BU)";
    const wchar_t* bin = L"D:P(A;;FA;;;SY)(A;;FA;;;BA)(A;;0x1200a9;;;BU)";
    if (!sd_from_sddl_is_ours(dir, GC_SERVICE_ACL_DIRECTORY)) return 5741;
    if (!sd_from_sddl_is_ours(bin, GC_SERVICE_ACL_BINARY)) return 5742;
    if (sd_from_sddl_is_ours(dir, GC_SERVICE_ACL_BINARY) ||
        sd_from_sddl_is_ours(bin, GC_SERVICE_ACL_DIRECTORY)) return 5743;
    // Order does not matter; anything else does.
    if (!sd_from_sddl_is_ours(L"D:P(A;OICI;0x1200a9;;;BU)(A;OICI;FA;;;BA)(A;OICI;FA;;;SY)",
                              GC_SERVICE_ACL_DIRECTORY)) return 5744;
    if (sd_from_sddl_is_ours(L"D:(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICI;0x1200a9;;;BU)",
                             GC_SERVICE_ACL_DIRECTORY)) return 5745;  // not protected
    if (sd_from_sddl_is_ours(L"D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICI;0x1200a9;;;BU)(A;OICI;FA;;;WD)",
                             GC_SERVICE_ACL_DIRECTORY)) return 5746;  // extra grant
    if (sd_from_sddl_is_ours(L"D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICI;0x1301bf;;;BU)",
                             GC_SERVICE_ACL_DIRECTORY)) return 5747;  // Users: Modify
    if (sd_from_sddl_is_ours(L"D:P(A;OICIID;FA;;;SY)(A;OICI;FA;;;BA)(A;OICI;0x1200a9;;;BU)",
                             GC_SERVICE_ACL_DIRECTORY)) return 5748;  // inherited ACE
    // A null DACL (Everyone: Full Control) is the opposite of ours.
    if (sd_from_sddl_is_ours(L"D:PNO_ACCESS_CONTROL", GC_SERVICE_ACL_DIRECTORY)) return 5749;
    if (service_security_descriptor_is_ours(nullptr, GC_SERVICE_ACL_DIRECTORY)) return 5750;
    return 0;
}

bool make_temp_dir(const wchar_t* prefix, wchar_t* out, size_t outCount) {
    wchar_t tempDir[MAX_PATH] = {};
    if (GetTempPathW(MAX_PATH, tempDir) == 0) return false;
    for (unsigned int attempt = 0; attempt < 64; ++attempt) {
        LARGE_INTEGER counter = {};
        QueryPerformanceCounter(&counter);
        if (FAILED(StringCchPrintfW(out, outCount, L"%ls%ls_%lu_%llx_%u", tempDir, prefix,
                                    (unsigned long)GetCurrentProcessId(),
                                    (unsigned long long)counter.QuadPart, attempt)))
            return false;
        if (CreateDirectoryW(out, nullptr)) return true;
        if (GetLastError() != ERROR_ALREADY_EXISTS) return false;
    }
    return false;
}

bool touch(const wchar_t* path) {
    HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    CloseHandle(file);
    return true;
}

HANDLE open_for_hardening(const wchar_t* path, bool directory) {
    // These fixtures pass changeOwner=false. Requiring WRITE_OWNER here tests
    // the temp directory's inherited ACL instead of handle-bound DACL writes.
    return CreateFileW(path,
        READ_CONTROL | WRITE_DAC | FILE_READ_ATTRIBUTES |
            (directory ? FILE_LIST_DIRECTORY : 0),
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
        (directory ? FILE_FLAG_BACKUP_SEMANTICS : 0) | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
}

// Every ACE on `path` names SYSTEM, Administrators or Users: nothing the test
// user held through inheritance before the hardening survived it.
bool only_service_principals(const wchar_t* path) {
    PSECURITY_DESCRIPTOR sd = nullptr;
    PACL dacl = nullptr;
    if (GetNamedSecurityInfoW(path, SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr,
                              &dacl, nullptr, &sd) != ERROR_SUCCESS || !sd) return false;
    bool ok = dacl != nullptr && dacl->AceCount > 0;
    BYTE sids[3][SECURITY_MAX_SID_SIZE] = {};
    const WELL_KNOWN_SID_TYPE types[3] = {WinLocalSystemSid, WinBuiltinAdministratorsSid, WinBuiltinUsersSid};
    for (int i = 0; i < 3 && ok; i++) {
        DWORD size = SECURITY_MAX_SID_SIZE;
        ok = CreateWellKnownSid(types[i], nullptr, sids[i], &size) != FALSE;
    }
    for (DWORD i = 0; ok && i < dacl->AceCount; i++) {
        void* raw = nullptr;
        if (!GetAce(dacl, i, &raw)) { ok = false; break; }
        PSID sid = (PSID)&((ACCESS_ALLOWED_ACE*)raw)->SidStart;
        ok = EqualSid(sid, sids[0]) || EqualSid(sid, sids[1]) || EqualSid(sid, sids[2]);
    }
    LocalFree(sd);
    return ok;
}

// The released DACL is a real, unprotected, inherited one -- never "no DACL".
bool released_dacl_is_inherited(const wchar_t* path) {
    PSECURITY_DESCRIPTOR sd = nullptr;
    PACL dacl = nullptr;
    if (GetNamedSecurityInfoW(path, SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr,
                              &dacl, nullptr, &sd) != ERROR_SUCCESS || !sd) return false;
    SECURITY_DESCRIPTOR_CONTROL control = 0;
    DWORD revision = 0;
    bool ok = dacl != nullptr && GetSecurityDescriptorControl(sd, &control, &revision) &&
              (control & SE_DACL_PROTECTED) == 0 && dacl->AceCount > 0;
    for (DWORD i = 0; ok && i < dacl->AceCount; i++) {
        void* raw = nullptr;
        ok = GetAce(dacl, i, &raw) && (((ACE_HEADER*)raw)->AceFlags & INHERITED_ACE) != 0;
    }
    LocalFree(sd);
    return ok;
}

int run_handle_hardening_tests() {
    wchar_t dir[MAX_PATH] = {}, child[MAX_PATH] = {}, binary[MAX_PATH] = {};
    if (!make_temp_dir(L"gc_svc_harden", dir, MAX_PATH)) return 5751;
    StringCchPrintfW(child, MAX_PATH, L"%ls\\greencurve.exe", dir);
    StringCchPrintfW(binary, MAX_PATH, L"%ls\\greencurve-service.exe", dir);
    auto cleanupAnd = [&](int code) -> int {
        char ignored[160] = {};
        release_service_hardening(binary, GC_SERVICE_ACL_BINARY, ignored, sizeof(ignored));
        release_service_hardening(dir, GC_SERVICE_ACL_DIRECTORY, ignored, sizeof(ignored));
        DeleteFileW(child);
        DeleteFileW(binary);
        RemoveDirectoryW(dir);
        return code;
    };
    if (!touch(child) || !touch(binary)) return cleanupAnd(5752);

    char err[160] = {};
    // Pin both handles before changing the folder ACL, as the installer does.
    HANDLE binaryHandle = open_for_hardening(binary, false);
    if (binaryHandle == INVALID_HANDLE_VALUE) return cleanupAnd(5758);
    HANDLE dirHandle = open_for_hardening(dir, true);
    if (dirHandle == INVALID_HANDLE_VALUE) {
        CloseHandle(binaryHandle);
        return cleanupAnd(5753);
    }
    // Wrong kind through the right handle is refused before any write.
    bool wrongKind = apply_protected_service_dacl_to_handle(dirHandle, GC_SERVICE_ACL_BINARY,
                                                            false, err, sizeof(err));
    bool applied = apply_protected_service_dacl_to_handle(dirHandle, GC_SERVICE_ACL_DIRECTORY,
                                                          false, err, sizeof(err));
    bool handleOurs = service_handle_dacl_is_ours(dirHandle, GC_SERVICE_ACL_DIRECTORY);
    CloseHandle(dirHandle);
    auto closeBinaryAnd = [&](int code) -> int {
        CloseHandle(binaryHandle);
        return cleanupAnd(code);
    };
    if (wrongKind) return closeBinaryAnd(5754);
    if (!applied || !handleOurs) return closeBinaryAnd(5755);
    if (!service_path_dacl_is_ours(dir, GC_SERVICE_ACL_DIRECTORY) ||
        service_path_dacl_is_ours(dir, GC_SERVICE_ACL_BINARY)) return closeBinaryAnd(5756);
    // Handle-bound SetSecurityInfo must still propagate to what is already in
    // the folder: the GUI binary beside the service loses the user's write.
    if (!only_service_principals(child)) return closeBinaryAnd(5757);

    applied = apply_protected_service_dacl_to_handle(binaryHandle, GC_SERVICE_ACL_BINARY,
                                                     false, err, sizeof(err));
    CloseHandle(binaryHandle);
    if (!applied || !service_path_dacl_is_ours(binary, GC_SERVICE_ACL_BINARY)) return cleanupAnd(5759);

    // Release: ours -> inherited (NEVER a null DACL); a second release finds
    // nothing of ours; a missing path is absent.
    if (release_service_hardening(binary, GC_SERVICE_ACL_BINARY, err, sizeof(err)) !=
        GC_SERVICE_RELEASE_RELEASED) return cleanupAnd(5760);
    if (release_service_hardening(dir, GC_SERVICE_ACL_DIRECTORY, err, sizeof(err)) !=
        GC_SERVICE_RELEASE_RELEASED) return cleanupAnd(5761);
    if (!released_dacl_is_inherited(dir) || !released_dacl_is_inherited(binary))
        return cleanupAnd(5762);
    if (release_service_hardening(dir, GC_SERVICE_ACL_DIRECTORY, err, sizeof(err)) !=
        GC_SERVICE_RELEASE_NOT_OURS) return cleanupAnd(5763);
    wchar_t missing[MAX_PATH] = {};
    StringCchPrintfW(missing, MAX_PATH, L"%ls\\absent", dir);
    if (release_service_hardening(missing, GC_SERVICE_ACL_DIRECTORY, err, sizeof(err)) !=
        GC_SERVICE_RELEASE_ABSENT) return cleanupAnd(5764);
    // A folder that is NOT ours -- here the plain temp folder with its
    // inherited DACL -- is never touched by a release.
    wchar_t tempRoot[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, tempRoot);
    if (release_service_hardening(tempRoot, GC_SERVICE_ACL_DIRECTORY, err, sizeof(err)) !=
        GC_SERVICE_RELEASE_NOT_OURS) return cleanupAnd(5765);
    return cleanupAnd(0);
}

int run_install_file_rollback_tests() {
    wchar_t directory[MAX_PATH] = {};
    if (!make_temp_dir(L"gc_install_rollback", directory, MAX_PATH)) return 6120;
    wchar_t destination[MAX_PATH] = {}, staged[MAX_PATH] = {}, backup[MAX_PATH] = {};
    StringCchPrintfW(destination, MAX_PATH, L"%ls\\greencurve.exe", directory);
    StringCchPrintfW(staged, MAX_PATH, L"%ls\\staged.exe", directory);
    StringCchPrintfW(backup, MAX_PATH, L"%ls\\old.exe", directory);
    auto finish = [&](int code) {
        char ignored[160] = {};
        release_service_hardening(staged, GC_SERVICE_ACL_BINARY, ignored, sizeof(ignored));
        DeleteFileW(destination);
        DeleteFileW(staged);
        DeleteFileW(backup);
        RemoveDirectoryW(directory);
        return code;
    };
    auto writeOne = [](const wchar_t* path, char value) {
        HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                                  FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) return false;
        DWORD written = 0;
        bool ok = WriteFile(file, &value, 1, &written, nullptr) && written == 1;
        CloseHandle(file);
        return ok;
    };
    auto readOne = [](const wchar_t* path, char expected) {
        HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr,
                                  OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) return false;
        char value = 0;
        DWORD read = 0;
        bool ok = ReadFile(file, &value, 1, &read, nullptr) && read == 1 &&
                  value == expected;
        CloseHandle(file);
        return ok;
    };
    if (!writeOne(destination, 'O') || !writeOne(staged, 'N') ||
        !CopyFileW(destination, backup, TRUE)) return finish(6121);
    HANDLE stagedHandle = open_for_hardening(staged, false);
    if (stagedHandle == INVALID_HANDLE_VALUE) return finish(6127);
    char aclError[160] = {};
    bool hardened = apply_protected_service_dacl_to_handle(stagedHandle,
        GC_SERVICE_ACL_BINARY, false, aclError, sizeof(aclError));
    CloseHandle(stagedHandle);
    if (!hardened || !service_path_dacl_is_ours(staged, GC_SERVICE_ACL_BINARY))
        return finish(6128);
    if (FAILED(gc_replace_staged_install_file(staged, destination)) ||
        !readOne(destination, 'N') || !released_dacl_is_inherited(destination))
        return finish(6122);
    if (FAILED(gc_restore_previous_install_file(backup, destination)) ||
        !readOne(destination, 'O')) return finish(6123);
    // An extraction failure must leave the last complete file untouched.
    char ignored[160] = {};
    if (release_service_hardening(staged, GC_SERVICE_ACL_BINARY, ignored,
                                  sizeof(ignored)) != GC_SERVICE_RELEASE_RELEASED ||
        !DeleteFileW(staged)) return finish(6129);
    if (SUCCEEDED(gc_replace_staged_install_file(staged, destination)) ||
        !readOne(destination, 'O')) return finish(6124);
    if (!writeOne(staged, 'N') ||
        FAILED(gc_replace_staged_install_file(staged, destination))) return finish(6125);
    // If the backup is lost, restoration must fail without truncating the
    // complete new file; the enclosing transaction reports recovery failure.
    DeleteFileW(backup);
    if (SUCCEEDED(gc_restore_previous_install_file(backup, destination)) ||
        !readOne(destination, 'N')) return finish(6126);
    return finish(0);
}

bool make_junction(const wchar_t* junctionPath, const wchar_t* targetPath) {
    struct MountPointReparse {
        DWORD tag;
        WORD dataLength;
        WORD reserved;
        WORD substituteOffset;
        WORD substituteLength;
        WORD printOffset;
        WORD printLength;
        wchar_t pathBuffer[600];
    } reparse = {};
    wchar_t substitute[600] = {};
    if (FAILED(StringCchPrintfW(substitute, 600, L"\\??\\%ls", targetPath))) return false;
    size_t substituteChars = wcslen(substitute);
    size_t printChars = wcslen(targetPath);
    if (substituteChars + printChars + 2 > 600) return false;
    if (!CreateDirectoryW(junctionPath, nullptr)) return false;
    HANDLE handle = CreateFileW(junctionPath, GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                               FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return false;
    reparse.tag = IO_REPARSE_TAG_MOUNT_POINT;
    reparse.substituteLength = (WORD)(substituteChars * sizeof(wchar_t));
    reparse.printOffset = (WORD)(reparse.substituteLength + sizeof(wchar_t));
    reparse.printLength = (WORD)(printChars * sizeof(wchar_t));
    memcpy(reparse.pathBuffer, substitute, reparse.substituteLength);
    memcpy(reparse.pathBuffer + substituteChars + 1, targetPath, reparse.printLength);
    reparse.dataLength = (WORD)(8 + reparse.substituteLength + 2 + reparse.printLength + 2);
    DWORD returned = 0;
    bool ok = DeviceIoControl(handle, FSCTL_SET_REPARSE_POINT, &reparse,
                              (DWORD)(8 + reparse.dataLength), nullptr, 0, &returned, nullptr) != FALSE;
    CloseHandle(handle);
    return ok;
}

int run_location_verdict_tests() {
    wchar_t dir[MAX_PATH] = {}, file[MAX_PATH] = {}, foreign[MAX_PATH] = {}, sub[MAX_PATH] = {};
    wchar_t junction[MAX_PATH] = {};
    if (!make_temp_dir(L"gc_svc_location", dir, MAX_PATH)) return 5766;
    StringCchPrintfW(file, MAX_PATH, L"%ls\\greencurve.exe", dir);
    StringCchPrintfW(foreign, MAX_PATH, L"%ls\\notes.txt", dir);
    StringCchPrintfW(sub, MAX_PATH, L"%ls\\tools", dir);
    StringCchPrintfW(junction, MAX_PATH, L"%ls_link", dir);
    auto cleanupAnd = [&](int code) -> int {
        char ignored[160] = {};
        release_service_hardening(dir, GC_SERVICE_ACL_DIRECTORY, ignored, sizeof(ignored));
        RemoveDirectoryW(junction);
        RemoveDirectoryW(sub);
        DeleteFileW(foreign);
        DeleteFileW(file);
        RemoveDirectoryW(dir);
        return code;
    };
    GcServiceLocationDetail detail = {};
    // An empty dedicated folder, and one holding only our own files, are ours.
    if (gc_service_install_location_verdict_detailed(dir, nullptr, &detail) != GC_SVC_LOCATION_OK ||
        !detail.exists || detail.entriesScanned != 0) return cleanupAnd(5767);
    if (!touch(file)) return cleanupAnd(5768);
    if (gc_service_install_location_verdict(dir) != GC_SVC_LOCATION_OK) return cleanupAnd(5769);
    // A user's file makes it somebody else's folder...
    if (!touch(foreign)) return cleanupAnd(5770);
    if (gc_service_install_location_verdict_detailed(dir, nullptr, &detail) !=
        GC_SVC_LOCATION_FOREIGN_CONTENT || detail.foreignIsDirectory) return cleanupAnd(5771);
    DeleteFileW(foreign);
    // ...and so does any subfolder.
    if (!CreateDirectoryW(sub, nullptr)) return cleanupAnd(5772);
    if (gc_service_install_location_verdict_detailed(dir, nullptr, &detail) !=
        GC_SVC_LOCATION_FOREIGN_CONTENT || !detail.foreignIsDirectory) return cleanupAnd(5773);
    RemoveDirectoryW(sub);
    // The handle-bound verdict judges the pinned object identically.
    HANDLE handle = CreateFileW(dir, FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES | READ_CONTROL |
                                WRITE_DAC, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
                                nullptr);
    if (handle == INVALID_HANDLE_VALUE) return cleanupAnd(5774);
    int pinned = gc_service_install_location_verdict_for_handle(dir, handle);
    // A folder Green Curve already hardened is ours whatever it holds.
    char err[160] = {};
    bool hardened = apply_protected_service_dacl_to_handle(handle, GC_SERVICE_ACL_DIRECTORY,
                                                           false, err, sizeof(err));
    CloseHandle(handle);
    if (pinned != GC_SVC_LOCATION_OK) return cleanupAnd(5775);
    if (!hardened) return cleanupAnd(5776);
    char ignored[160] = {};
    // Put a foreign file in while it is released, then harden again.
    release_service_hardening(dir, GC_SERVICE_ACL_DIRECTORY, ignored, sizeof(ignored));
    if (!touch(foreign)) return cleanupAnd(5777);
    handle = CreateFileW(dir, FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES | READ_CONTROL |
                         WRITE_DAC, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                         OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return cleanupAnd(5778);
    hardened = apply_protected_service_dacl_to_handle(handle, GC_SERVICE_ACL_DIRECTORY,
                                                      false, err, sizeof(err));
    CloseHandle(handle);
    if (!hardened) return cleanupAnd(5779);
    if (gc_service_install_location_verdict_detailed(dir, nullptr, &detail) != GC_SVC_LOCATION_OK ||
        !detail.alreadyHardened) return cleanupAnd(5780);
    release_service_hardening(dir, GC_SERVICE_ACL_DIRECTORY, ignored, sizeof(ignored));
    // A leaf junction is seen, never followed.
    if (!make_junction(junction, dir)) return cleanupAnd(5781);
    if (gc_service_install_location_verdict(junction) != GC_SVC_LOCATION_REPARSE)
        return cleanupAnd(5782);
    // Anything inside the Windows directory, at any depth.
    wchar_t windows[MAX_PATH] = {}, deep[MAX_PATH] = {};
    if (!GetWindowsDirectoryW(windows, MAX_PATH)) return cleanupAnd(5783);
    StringCchPrintfW(deep, MAX_PATH, L"%ls\\Temp", windows);
    if (gc_service_install_location_verdict(deep) != GC_SVC_LOCATION_SYSTEM_SUBTREE)
        return cleanupAnd(5784);
    StringCchPrintfW(deep, MAX_PATH, L"%ls\\System32\\drivers\\etc\\Green Curve", windows);
    if (gc_service_install_location_verdict(deep) != GC_SVC_LOCATION_SYSTEM_SUBTREE)
        return cleanupAnd(5785);
    // Per-user program installs are a known folder of their own.
    PWSTR programs = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_UserProgramFiles, 0, nullptr, &programs)) && programs) {
        int verdict = gc_service_install_location_verdict(programs);
        CoTaskMemFree(programs);
        if (verdict != GC_SVC_LOCATION_KNOWN_FOLDER) return cleanupAnd(5786);
    } else if (programs) {
        CoTaskMemFree(programs);
    }
    return cleanupAnd(0);
}

#endif  // _WIN32

}  // namespace

int run_service_install_tests() {
    if (int failure = run_scm_wait_policy_tests()) return failure;
    if (int failure = run_location_content_policy_tests()) return failure;
#if defined(_WIN32)
    if (int failure = run_exact_dacl_tests()) return failure;
    if (int failure = run_handle_hardening_tests()) return failure;
    if (int failure = run_install_file_rollback_tests()) return failure;
    if (int failure = run_location_verdict_tests()) return failure;
#endif
    return 0;
}
