// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// Handle-bound service hardening and its exact-ownership proof (F-SEC-1).
// See service_acl.h for why every check and every DACL write here goes through
// one handle instead of a path.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#undef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#include <windows.h>
#include <sddl.h>
#include <aclapi.h>
#include <strsafe.h>

#include "service_acl.h"

namespace {

void set_handle_acl_err(char* err, size_t errSize, const char* msg, DWORD code) {
    if (!err || errSize == 0) return;
    StringCchPrintfA(err, errSize, "%s (error %lu)", msg, (unsigned long)code);
}

bool build_sid(WELL_KNOWN_SID_TYPE type, BYTE* buf, DWORD bufSize) {
    DWORD size = bufSize;
    return CreateWellKnownSid(type, nullptr, buf, &size) != FALSE;
}

struct GcExpectedServiceAce {
    WELL_KNOWN_SID_TYPE sid;
    DWORD mask;
};

// FILE_ALL_ACCESS is what SDDL "FA" maps to; 0x1200a9 is read & execute and
// 0x120089 is read.  The private kinds are the first two rows alone.
const GcExpectedServiceAce kExpectedServiceAces[3] = {
    {WinLocalSystemSid, FILE_ALL_ACCESS},
    {WinBuiltinAdministratorsSid, FILE_ALL_ACCESS},
    {WinBuiltinUsersSid, 0x1200a9},
};

int service_acl_expected_ace_count(GcServiceAclKind kind) {
    return kind == GC_SERVICE_ACL_PRIVATE_DIRECTORY || kind == GC_SERVICE_ACL_PRIVATE_FILE
        ? 2 : 3;
}

DWORD service_acl_expected_mask(GcServiceAclKind kind, int row) {
    if (kind == GC_SERVICE_ACL_CONFIG && row == 2) return 0x120089;
    return kExpectedServiceAces[row].mask;
}

const wchar_t* service_acl_sddl(GcServiceAclKind kind) {
    switch (kind) {
        case GC_SERVICE_ACL_CONFIG:
            return L"D:P(A;;FA;;;SY)(A;;FA;;;BA)(A;;0x120089;;;BU)";
        case GC_SERVICE_ACL_DIRECTORY:
            return L"D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICI;0x1200a9;;;BU)";
        case GC_SERVICE_ACL_PRIVATE_DIRECTORY:
            return L"D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)";
        case GC_SERVICE_ACL_PRIVATE_FILE:
            return L"D:P(A;;FA;;;SY)(A;;FA;;;BA)";
        case GC_SERVICE_ACL_BINARY:
        default:
            return L"D:P(A;;FA;;;SY)(A;;FA;;;BA)(A;;0x1200a9;;;BU)";
    }
}

BYTE service_acl_ace_flags(GcServiceAclKind kind) {
    return service_acl_kind_is_directory(kind)
        ? (BYTE)(OBJECT_INHERIT_ACE | CONTAINER_INHERIT_ACE) : (BYTE)0;
}

bool handle_owner_is_one_of(HANDLE handle, const WELL_KNOWN_SID_TYPE* types, int count) {
    if (!handle || handle == INVALID_HANDLE_VALUE) return false;
    PSECURITY_DESCRIPTOR sd = nullptr;
    PSID owner = nullptr;
    DWORD rc = GetSecurityInfo(handle, SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION,
                               &owner, nullptr, nullptr, nullptr, &sd);
    if (rc != ERROR_SUCCESS || !sd) return false;
    bool match = false;
    if (owner && IsValidSid(owner)) {
        for (int i = 0; i < count && !match; i++) {
            BYTE sid[SECURITY_MAX_SID_SIZE] = {};
            match = build_sid(types[i], sid, sizeof(sid)) && EqualSid(owner, (PSID)sid);
        }
    }
    LocalFree(sd);
    return match;
}

// A suffix that only has to be unlikely to collide (a collision fails the
// rename, which the caller retries or fails closed on), so no CSPRNG dependency
// is pulled into every binary that links this shard.
unsigned long long set_aside_suffix(unsigned attempt) {
    LARGE_INTEGER counter = {};
    QueryPerformanceCounter(&counter);
    unsigned long long value = (unsigned long long)counter.QuadPart;
    value ^= (unsigned long long)GetCurrentProcessId() << 32;
    value ^= GetTickCount64() * 0x9E3779B97F4A7C15ull;
    value ^= (unsigned long long)(attempt + 1) * 0xC2B2AE3D27D4EB4Full;
    return value;
}

// The reparse/directory facts of the object the handle names -- never of
// whatever a path happens to resolve to now.
bool handle_attribute_facts(HANDLE handle, bool* isDirectory, bool* isReparse) {
    FILE_ATTRIBUTE_TAG_INFO tag = {};
    if (!GetFileInformationByHandleEx(handle, FileAttributeTagInfo, &tag, sizeof(tag)))
        return false;
    *isDirectory = (tag.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    *isReparse = (tag.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
    return true;
}

bool handle_matches_kind(HANDLE handle, GcServiceAclKind kind) {
    bool isDirectory = false;
    bool isReparse = false;
    if (!handle_attribute_facts(handle, &isDirectory, &isReparse)) return false;
    if (isReparse) return false;
    return isDirectory == service_acl_kind_is_directory(kind);
}

}  // namespace

const wchar_t* service_acl_kind_sddl(GcServiceAclKind kind) {
    return service_acl_sddl(kind);
}

bool service_acl_kind_is_directory(GcServiceAclKind kind) {
    return kind == GC_SERVICE_ACL_DIRECTORY || kind == GC_SERVICE_ACL_PRIVATE_DIRECTORY;
}

bool service_security_descriptor_is_ours(void* securityDescriptor, GcServiceAclKind kind) {
    PSECURITY_DESCRIPTOR sd = (PSECURITY_DESCRIPTOR)securityDescriptor;
    if (!sd || !IsValidSecurityDescriptor(sd)) return false;
    SECURITY_DESCRIPTOR_CONTROL control = 0;
    DWORD revision = 0;
    if (!GetSecurityDescriptorControl(sd, &control, &revision) ||
        (control & SE_DACL_PROTECTED) == 0) return false;
    BOOL present = FALSE;
    BOOL defaulted = FALSE;
    PACL dacl = nullptr;
    // A null DACL is "Everyone: Full Control" -- the opposite of ours.
    if (!GetSecurityDescriptorDacl(sd, &present, &dacl, &defaulted) || !present || !dacl)
        return false;
    const int expectedCount = service_acl_expected_ace_count(kind);
    if (dacl->AceCount != (DWORD)expectedCount) return false;
    BYTE expectedSids[3][SECURITY_MAX_SID_SIZE] = {};
    for (int i = 0; i < expectedCount; i++) {
        if (!build_sid(kExpectedServiceAces[i].sid, expectedSids[i], sizeof(expectedSids[i])))
            return false;
    }
    bool seen[3] = {};
    const BYTE expectedFlags = service_acl_ace_flags(kind);
    for (DWORD i = 0; i < dacl->AceCount; i++) {
        void* aceRaw = nullptr;
        if (!GetAce(dacl, i, &aceRaw) || !aceRaw) return false;
        ACE_HEADER* header = (ACE_HEADER*)aceRaw;
        if (header->AceType != ACCESS_ALLOWED_ACE_TYPE) return false;
        if (header->AceFlags != expectedFlags) return false;
        ACCESS_ALLOWED_ACE* ace = (ACCESS_ALLOWED_ACE*)aceRaw;
        PSID sid = (PSID)&ace->SidStart;
        bool matched = false;
        for (int e = 0; e < expectedCount; e++) {
            if (seen[e] || !EqualSid(sid, (PSID)expectedSids[e])) continue;
            if (ace->Mask != service_acl_expected_mask(kind, e)) return false;
            seen[e] = true;
            matched = true;
            break;
        }
        if (!matched) return false;
    }
    for (int e = 0; e < expectedCount; e++) {
        if (!seen[e]) return false;
    }
    return true;
}

bool service_handle_dacl_is_ours(void* handle, GcServiceAclKind kind) {
    if (!handle || handle == INVALID_HANDLE_VALUE) return false;
    PSECURITY_DESCRIPTOR sd = nullptr;
    PACL dacl = nullptr;
    DWORD rc = GetSecurityInfo((HANDLE)handle, SE_FILE_OBJECT, DACL_SECURITY_INFORMATION,
                               nullptr, nullptr, &dacl, nullptr, &sd);
    if (rc != ERROR_SUCCESS || !sd) return false;
    bool ours = service_security_descriptor_is_ours(sd, kind);
    LocalFree(sd);
    return ours;
}

bool service_handle_owner_is_administrators(void* handle) {
    const WELL_KNOWN_SID_TYPE admins[] = {WinBuiltinAdministratorsSid};
    return handle_owner_is_one_of((HANDLE)handle, admins, 1);
}

bool service_handle_owner_is_privileged(void* handle) {
    const WELL_KNOWN_SID_TYPE privileged[] = {WinBuiltinAdministratorsSid, WinLocalSystemSid};
    return handle_owner_is_one_of((HANDLE)handle, privileged, 2);
}

bool service_set_aside_by_name(const wchar_t* path, const void* expectedIdentityRaw,
                               char* err, size_t errSize) {
    if (err && errSize) err[0] = 0;
    const BY_HANDLE_FILE_INFORMATION* expected =
        (const BY_HANDLE_FILE_INFORMATION*)expectedIdentityRaw;
    const size_t pathChars = path ? wcslen(path) : 0;
    if (pathChars == 0 || pathChars > MAX_PATH - 32 || path[pathChars - 1] == L'\\') {
        set_handle_acl_err(err, errSize, "The object to set aside has an unusable name",
                           ERROR_INVALID_NAME);
        return false;
    }
    DWORD lastError = ERROR_ALREADY_EXISTS;
    for (unsigned attempt = 0; attempt < 4; attempt++) {
        // "<path>.unproven-<suffix>": the same directory by construction, so
        // the move is a rename (no copy fallback) that never leaves it.
        WCHAR aside[MAX_PATH] = {};
        if (FAILED(StringCchPrintfW(aside, ARRAYSIZE(aside), L"%ls.unproven-%016llx", path,
                                    set_aside_suffix(attempt)))) {
            set_handle_acl_err(err, errSize, "The set-aside name is too long", ERROR_INVALID_NAME);
            return false;
        }
        // A rename moves a link itself, never its target, and never replaces
        // whatever might already hold the new name.
        if (!MoveFileExW(path, aside, 0)) {
            lastError = GetLastError();
            if (lastError == ERROR_ALREADY_EXISTS || lastError == ERROR_FILE_EXISTS) continue;
            break;
        }
        HANDLE moved = CreateFileW(aside, FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        BY_HANDLE_FILE_INFORMATION now = {};
        bool identityOk = moved != INVALID_HANDLE_VALUE && GetFileInformationByHandle(moved, &now);
        const bool isDirectory = identityOk && (now.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY);
        if (moved != INVALID_HANDLE_VALUE) CloseHandle(moved);
        if (expected) {
            identityOk = identityOk &&
                now.dwVolumeSerialNumber == expected->dwVolumeSerialNumber &&
                now.nFileIndexHigh == expected->nFileIndexHigh &&
                now.nFileIndexLow == expected->nFileIndexLow;
        }
        if (!identityOk) {
            // Something else took the name between the inspection and the
            // move.  It stays aside untouched; the caller looks again.
            set_handle_acl_err(err, errSize, "A different object was set aside than inspected",
                               ERROR_INVALID_DATA);
            return false;
        }
        // Best effort: a non-empty directory simply stays renamed aside.  The
        // original name is free either way, which is what the caller needs.
        if (isDirectory) RemoveDirectoryW(aside);
        else DeleteFileW(aside);
        return true;
    }
    set_handle_acl_err(err, errSize, "Cannot rename the unproven object aside", lastError);
    return false;
}

bool service_path_dacl_is_ours(const wchar_t* path, GcServiceAclKind kind) {
    if (!path || !path[0]) return false;
    HANDLE handle = CreateFileW(path, READ_CONTROL | FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return false;
    bool ours = handle_matches_kind(handle, kind) && service_handle_dacl_is_ours(handle, kind);
    CloseHandle(handle);
    return ours;
}

bool apply_protected_service_dacl_to_handle(void* handleRaw, GcServiceAclKind kind,
                                            bool requireAdminOwner,
                                            char* err, size_t errSize) {
    if (err && errSize) err[0] = 0;
    HANDLE handle = (HANDLE)handleRaw;
    if (!handle || handle == INVALID_HANDLE_VALUE) {
        set_handle_acl_err(err, errSize, "No handle to harden", ERROR_INVALID_HANDLE);
        return false;
    }
    // The object behind the handle, not the name: a reparse point or the
    // wrong kind of object is refused before anything is written.
    bool isDirectory = false;
    bool isReparse = false;
    if (!handle_attribute_facts(handle, &isDirectory, &isReparse)) {
        set_handle_acl_err(err, errSize, "Failed reading the object's attributes", GetLastError());
        return false;
    }
    if (isReparse || isDirectory != service_acl_kind_is_directory(kind)) {
        set_handle_acl_err(err, errSize,
                           "Refusing to harden a reparse point or the wrong object type",
                           ERROR_INVALID_PARAMETER);
        return false;
    }
    PSECURITY_DESCRIPTOR psd = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(service_acl_sddl(kind),
            SDDL_REVISION_1, &psd, nullptr)) {
        set_handle_acl_err(err, errSize, "Failed building the protected DACL", GetLastError());
        return false;
    }
    BOOL daclPresent = FALSE;
    BOOL daclDefaulted = FALSE;
    PACL dacl = nullptr;
    BYTE adminSid[SECURITY_MAX_SID_SIZE] = {};
    bool ok = false;
    if (!GetSecurityDescriptorDacl(psd, &daclPresent, &dacl, &daclDefaulted) ||
        !daclPresent || !dacl) {
        set_handle_acl_err(err, errSize, "Protected DACL missing after build", GetLastError());
    } else if (!build_sid(WinBuiltinAdministratorsSid, adminSid, sizeof(adminSid))) {
        set_handle_acl_err(err, errSize, "Failed building the Administrators SID", GetLastError());
    } else if (requireAdminOwner) {
        // One call: the owner is part of the protection, not a courtesy.  A
        // standard-user owner keeps implicit WRITE_DAC over the folder.
        DWORD rc = SetSecurityInfo(handle, SE_FILE_OBJECT,
            OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION |
                PROTECTED_DACL_SECURITY_INFORMATION,
            (PSID)adminSid, nullptr, dacl, nullptr);
        if (rc == ERROR_SUCCESS) ok = true;
        else set_handle_acl_err(err, errSize, "Failed applying the protected DACL and owner", rc);
    } else {
        DWORD rc = SetSecurityInfo(handle, SE_FILE_OBJECT,
            DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
            nullptr, nullptr, dacl, nullptr);
        if (rc == ERROR_SUCCESS) {
            ok = true;
            // Unelevated callers (the regression harness) may be refused.
            SetSecurityInfo(handle, SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION,
                            (PSID)adminSid, nullptr, nullptr, nullptr);
        } else {
            set_handle_acl_err(err, errSize, "Failed applying the protected DACL", rc);
        }
    }
    LocalFree(psd);
    if (!ok) return false;
    if (!service_handle_dacl_is_ours(handle, kind)) {
        set_handle_acl_err(err, errSize, "The protected DACL did not read back exactly",
                           ERROR_INVALID_ACL);
        return false;
    }
    if (requireAdminOwner && !service_handle_owner_is_administrators(handle)) {
        set_handle_acl_err(err, errSize, "The owner did not read back as Administrators",
                           ERROR_INVALID_OWNER);
        return false;
    }
    return true;
}

int release_service_hardening(const wchar_t* path, GcServiceAclKind kind,
                              char* err, size_t errSize) {
    if (err && errSize) err[0] = 0;
    if (!path || !path[0]) return GC_SERVICE_RELEASE_ABSENT;
    // No FILE_SHARE_DELETE: nobody may rename this object away while it is
    // being inspected and released.
    HANDLE inspected = CreateFileW(path, READ_CONTROL | FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (inspected == INVALID_HANDLE_VALUE) {
        DWORD openErr = GetLastError();
        if (openErr == ERROR_FILE_NOT_FOUND || openErr == ERROR_PATH_NOT_FOUND)
            return GC_SERVICE_RELEASE_ABSENT;
        set_handle_acl_err(err, errSize, "Failed inspecting the object to release", openErr);
        return GC_SERVICE_RELEASE_FAILED;
    }
    // Most candidate paths are not ours. Read them without requesting write
    // access, which a protected unrelated directory need not grant us.
    if (!handle_matches_kind(inspected, kind) || !service_handle_dacl_is_ours(inspected, kind)) {
        CloseHandle(inspected);
        return GC_SERVICE_RELEASE_NOT_OURS;
    }
    // Keep the inspection handle open without FILE_SHARE_DELETE while opening
    // the write handle; then recheck the DACL on that handle before changing it.
    HANDLE handle = CreateFileW(path, READ_CONTROL | WRITE_DAC | FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        DWORD openErr = GetLastError();
        CloseHandle(inspected);
        set_handle_acl_err(err, errSize, "Failed opening our object to release", openErr);
        return GC_SERVICE_RELEASE_FAILED;
    }
    CloseHandle(inspected);
    int result = GC_SERVICE_RELEASE_NOT_OURS;
    if (handle_matches_kind(handle, kind) && service_handle_dacl_is_ours(handle, kind)) {
        // An EMPTY explicit ACL plus UNPROTECTED: the object keeps nothing of
        // ours and takes its parent's inheritable ACEs again.  A null pDacl
        // here would instead leave no DACL at all = Everyone: Full Control.
        ACL emptyAcl = {};
        if (!InitializeAcl(&emptyAcl, sizeof(ACL), ACL_REVISION)) {
            set_handle_acl_err(err, errSize, "Failed initializing an empty DACL", GetLastError());
            result = GC_SERVICE_RELEASE_FAILED;
        } else {
            DWORD rc = SetSecurityInfo(handle, SE_FILE_OBJECT,
                DACL_SECURITY_INFORMATION | UNPROTECTED_DACL_SECURITY_INFORMATION,
                nullptr, nullptr, &emptyAcl, nullptr);
            if (rc != ERROR_SUCCESS) {
                set_handle_acl_err(err, errSize, "Failed restoring the inherited DACL", rc);
                result = GC_SERVICE_RELEASE_FAILED;
            } else {
                PSECURITY_DESCRIPTOR sd = nullptr;
                PACL dacl = nullptr;
                DWORD readRc = GetSecurityInfo(handle, SE_FILE_OBJECT, DACL_SECURITY_INFORMATION,
                                               nullptr, nullptr, &dacl, nullptr, &sd);
                SECURITY_DESCRIPTOR_CONTROL control = 0;
                DWORD revision = 0;
                bool sane = readRc == ERROR_SUCCESS && sd && dacl &&
                    GetSecurityDescriptorControl(sd, &control, &revision) &&
                    (control & SE_DACL_PROTECTED) == 0;
                if (sd) LocalFree(sd);
                if (sane) {
                    result = GC_SERVICE_RELEASE_RELEASED;
                } else {
                    set_handle_acl_err(err, errSize,
                        "The released DACL did not read back as inherited",
                        readRc != ERROR_SUCCESS ? readRc : (DWORD)ERROR_INVALID_ACL);
                    result = GC_SERVICE_RELEASE_FAILED;
                }
            }
        }
    }
    CloseHandle(handle);
    return result;
}

const char* service_release_result_name(int result) {
    switch (result) {
        case GC_SERVICE_RELEASE_RELEASED: return "released";
        case GC_SERVICE_RELEASE_NOT_OURS: return "not-ours";
        case GC_SERVICE_RELEASE_ABSENT: return "absent";
        case GC_SERVICE_RELEASE_FAILED: return "failed";
        default: return "unknown";
    }
}

bool service_prepare_shared_bank_handle(void** handleInOut, const wchar_t* path,
    bool requireAdminOwner, bool* discarded, char* err, size_t errSize) {
    if (discarded) *discarded = false;
    HANDLE handle = handleInOut ? (HANDLE)*handleInOut : INVALID_HANDLE_VALUE;
    if (!handle_matches_kind(handle, GC_SERVICE_ACL_CONFIG)) {
        set_handle_acl_err(err, errSize, "Shared bank is not a regular non-reparse file", ERROR_INVALID_DATA);
        return false;
    }
    BY_HANDLE_FILE_INFORMATION info = {};
    if (!GetFileInformationByHandle(handle, &info)) {
        set_handle_acl_err(err, errSize, "Cannot read the shared bank's link identity", GetLastError());
        return false;
    }
    const bool trusted = info.nNumberOfLinks == 1 &&
        service_handle_dacl_is_ours(handle, GC_SERVICE_ACL_CONFIG) &&
        service_handle_owner_is_administrators(handle);
    if (!trusted) {
        // Never adopted, never truncated in place: whoever created it may still
        // hold a WRITE_DAC handle that a re-ACL would not revoke, and a second
        // hard link would make a write through this name a write elsewhere.
        // Only this NAME is removed; the object's bytes are never touched.
        // The caller's handle denies FILE_SHARE_DELETE, so it is closed for
        // the move and the moved object is matched to its file id instead.
        CloseHandle(handle);
        *handleInOut = INVALID_HANDLE_VALUE;
        if (!service_set_aside_by_name(path, &info, err, errSize)) return false;
        if (discarded) *discarded = true;
        return true;
    }
    return apply_protected_service_dacl_to_handle(handle, GC_SERVICE_ACL_CONFIG,
        requireAdminOwner, err, errSize);
}
