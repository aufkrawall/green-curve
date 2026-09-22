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

// FILE_ALL_ACCESS is what SDDL "FA" maps to; 0x1200a9 is read & execute.
const GcExpectedServiceAce kExpectedServiceAces[3] = {
    {WinLocalSystemSid, FILE_ALL_ACCESS},
    {WinBuiltinAdministratorsSid, FILE_ALL_ACCESS},
    {WinBuiltinUsersSid, 0x1200a9},
};

const wchar_t* service_acl_sddl(GcServiceAclKind kind) {
    return kind == GC_SERVICE_ACL_DIRECTORY
        ? L"D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICI;0x1200a9;;;BU)"
        : L"D:P(A;;FA;;;SY)(A;;FA;;;BA)(A;;0x1200a9;;;BU)";
}

BYTE service_acl_ace_flags(GcServiceAclKind kind) {
    return kind == GC_SERVICE_ACL_DIRECTORY
        ? (BYTE)(OBJECT_INHERIT_ACE | CONTAINER_INHERIT_ACE) : (BYTE)0;
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
    return isDirectory == (kind == GC_SERVICE_ACL_DIRECTORY);
}

}  // namespace

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
    if (dacl->AceCount != 3) return false;
    BYTE expectedSids[3][SECURITY_MAX_SID_SIZE] = {};
    for (int i = 0; i < 3; i++) {
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
        for (int e = 0; e < 3; e++) {
            if (seen[e] || !EqualSid(sid, (PSID)expectedSids[e])) continue;
            if (ace->Mask != kExpectedServiceAces[e].mask) return false;
            seen[e] = true;
            matched = true;
            break;
        }
        if (!matched) return false;
    }
    return seen[0] && seen[1] && seen[2];
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
    if (!handle || handle == INVALID_HANDLE_VALUE) return false;
    PSECURITY_DESCRIPTOR sd = nullptr;
    PSID owner = nullptr;
    DWORD rc = GetSecurityInfo((HANDLE)handle, SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION,
                               &owner, nullptr, nullptr, nullptr, &sd);
    if (rc != ERROR_SUCCESS || !sd) return false;
    BYTE admins[SECURITY_MAX_SID_SIZE] = {};
    bool isAdmins = owner && IsValidSid(owner) &&
        build_sid(WinBuiltinAdministratorsSid, admins, sizeof(admins)) &&
        EqualSid(owner, (PSID)admins);
    LocalFree(sd);
    return isAdmins;
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
    if (isReparse || isDirectory != (kind == GC_SERVICE_ACL_DIRECTORY)) {
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
    HANDLE handle = CreateFileW(path, READ_CONTROL | WRITE_DAC | FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        DWORD openErr = GetLastError();
        if (openErr == ERROR_FILE_NOT_FOUND || openErr == ERROR_PATH_NOT_FOUND)
            return GC_SERVICE_RELEASE_ABSENT;
        set_handle_acl_err(err, errSize, "Failed opening the object to release", openErr);
        return GC_SERVICE_RELEASE_FAILED;
    }
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
