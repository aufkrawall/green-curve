// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// Service-binary ACL hardening (F-SEC-1).  See service_acl.h for rationale.

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

void set_acl_err(char* err, size_t errSize, const char* msg, DWORD code) {
    if (!err || errSize == 0) return;
    StringCchPrintfA(err, errSize, "%s (error %lu)", msg, (unsigned long)code);
}

// Bits that let a principal replace or tamper with the binary.
const DWORD kDangerousWriteMask =
    FILE_WRITE_DATA | FILE_APPEND_DATA | FILE_WRITE_EA | FILE_WRITE_ATTRIBUTES |
    DELETE | WRITE_DAC | WRITE_OWNER | GENERIC_WRITE | GENERIC_ALL;

bool build_well_known_sid(WELL_KNOWN_SID_TYPE type, BYTE* buf, DWORD bufSize) {
    DWORD sz = bufSize;
    return CreateWellKnownSid(type, nullptr, buf, &sz) != FALSE;
}

bool sid_is_non_admin_principal(PSID sid) {
    if (!sid || !IsValidSid(sid)) return false;
    BYTE everyone[SECURITY_MAX_SID_SIZE] = {};
    BYTE users[SECURITY_MAX_SID_SIZE] = {};
    BYTE authUsers[SECURITY_MAX_SID_SIZE] = {};
    BYTE interactive[SECURITY_MAX_SID_SIZE] = {};
    if (build_well_known_sid(WinWorldSid, everyone, sizeof(everyone)) &&
        EqualSid(sid, everyone)) return true;
    if (build_well_known_sid(WinBuiltinUsersSid, users, sizeof(users)) &&
        EqualSid(sid, users)) return true;
    if (build_well_known_sid(WinAuthenticatedUserSid, authUsers, sizeof(authUsers)) &&
        EqualSid(sid, authUsers)) return true;
    if (build_well_known_sid(WinInteractiveSid, interactive, sizeof(interactive)) &&
        EqualSid(sid, interactive)) return true;
    return false;
}

} // namespace

bool apply_protected_service_binary_dacl(const wchar_t* path, char* err, size_t errSize) {
    if (err && errSize) err[0] = 0;
    if (!path || !path[0]) {
        set_acl_err(err, errSize, "Empty service binary path", 0);
        return false;
    }
    // SYSTEM: Full, Administrators: Full, BUILTIN\Users: Read & Execute (0x1200a9).
    // Administrators retain Full so an elevated admin (e.g. Explorer with a UAC
    // prompt) can still update the binary while the service is merely stopped.
    const wchar_t* sddl = L"D:P(A;;FA;;;SY)(A;;FA;;;BA)(A;;0x1200a9;;;BU)";
    PSECURITY_DESCRIPTOR psd = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl, SDDL_REVISION_1, &psd, nullptr)) {
        set_acl_err(err, errSize, "Failed building service binary DACL", GetLastError());
        return false;
    }
    BOOL daclPresent = FALSE, daclDefaulted = FALSE;
    PACL pDacl = nullptr;
    bool ok = false;
    if (GetSecurityDescriptorDacl(psd, &daclPresent, &pDacl, &daclDefaulted) && daclPresent) {
        // PROTECTED_DACL_SECURITY_INFORMATION disables inheritance so a writable
        // parent's ACEs cannot flow back in and re-grant non-admin write.
        DWORD rc = SetNamedSecurityInfoW((LPWSTR)path, SE_FILE_OBJECT,
            DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
            nullptr, nullptr, pDacl, nullptr);
        if (rc == ERROR_SUCCESS) {
            ok = true;
            // Best-effort: set the owner to BUILTIN\Administrators so a
            // standard-user owner cannot use implicit WRITE_DAC to rewrite the
            // DACL and re-grant themselves write.  This requires elevation (the
            // install path is elevated); in the non-elevated unit test it is a
            // no-op and does not affect the DACL protection asserted above.
            BYTE adminSid[SECURITY_MAX_SID_SIZE] = {};
            DWORD adminSidSize = sizeof(adminSid);
            if (CreateWellKnownSid(WinBuiltinAdministratorsSid, nullptr, adminSid, &adminSidSize)) {
                SetNamedSecurityInfoW((LPWSTR)path, SE_FILE_OBJECT,
                    OWNER_SECURITY_INFORMATION, adminSid, nullptr, nullptr, nullptr);
            }
        } else {
            set_acl_err(err, errSize, "Failed applying service binary DACL", rc);
        }
    } else {
        set_acl_err(err, errSize, "Service binary DACL missing after build", GetLastError());
    }
    LocalFree(psd);
    return ok;
}

bool restore_inherited_dacl(const wchar_t* path, char* err, size_t errSize) {
    if (err && errSize) err[0] = 0;
    if (!path || !path[0]) {
        set_acl_err(err, errSize, "Empty path for DACL restore", 0);
        return false;
    }
    // An empty, UNPROTECTED DACL removes our explicit ACEs and re-enables
    // inheritance, so the object picks up its parent's ACLs again (restoring the
    // user's ability to delete/replace the binary after uninstall).
    ACL emptyAcl = {};
    if (!InitializeAcl(&emptyAcl, sizeof(ACL), ACL_REVISION)) {
        set_acl_err(err, errSize, "Failed initializing empty DACL", GetLastError());
        return false;
    }
    DWORD rc = SetNamedSecurityInfoW((LPWSTR)path, SE_FILE_OBJECT,
        DACL_SECURITY_INFORMATION | UNPROTECTED_DACL_SECURITY_INFORMATION,
        nullptr, nullptr, &emptyAcl, nullptr);
    if (rc != ERROR_SUCCESS) {
        set_acl_err(err, errSize, "Failed restoring inherited DACL", rc);
        return false;
    }
    return true;
}

bool service_path_is_under_secure_root(const wchar_t* path) {
    if (!path || !path[0]) return false;
    wchar_t full[MAX_PATH] = {};
    if (GetFullPathNameW(path, MAX_PATH, full, nullptr) == 0) return false;
    CharLowerW(full);

    const char* vars[] = { "ProgramFiles", "ProgramFiles(x86)", "ProgramW6432", "SystemRoot" };
    for (const char* var : vars) {
        wchar_t root[MAX_PATH] = {};
        // GetEnvironmentVariableW needs a wide name.
        wchar_t wvar[64] = {};
        MultiByteToWideChar(CP_ACP, 0, var, -1, wvar, ARRAYSIZE(wvar));
        DWORD n = GetEnvironmentVariableW(wvar, root, ARRAYSIZE(root));
        if (n == 0 || n >= ARRAYSIZE(root)) continue;
        CharLowerW(root);
        size_t rootLen = wcslen(root);
        if (rootLen == 0) continue;
        if (wcsncmp(full, root, rootLen) == 0) {
            // Require a path separator (or exact match) after the root so that
            // e.g. "C:\Program Files Evil" does not match "C:\Program Files".
            wchar_t next = full[rootLen];
            if (next == 0 || next == L'\\' || next == L'/') return true;
        }
    }
    return false;
}

bool machine_config_dacl_is_hardened(const wchar_t* path) {
    if (!path || !path[0]) return false;
    PSECURITY_DESCRIPTOR psd = nullptr;
    PACL pDacl = nullptr;
    DWORD rc = GetNamedSecurityInfoW((LPWSTR)path, SE_FILE_OBJECT,
        DACL_SECURITY_INFORMATION, nullptr, nullptr, &pDacl, nullptr, &psd);
    if (rc != ERROR_SUCCESS || !psd) return false;

    bool hardened = false;
    SECURITY_DESCRIPTOR_CONTROL control = 0;
    DWORD revision = 0;
    if (GetSecurityDescriptorControl(psd, &control, &revision) &&
        (control & SE_DACL_PROTECTED)) {
        bool nonAdminWrite = false;
        if (pDacl) {
            for (DWORD i = 0; i < pDacl->AceCount; i++) {
                void* aceRaw = nullptr;
                if (!GetAce(pDacl, i, &aceRaw) || !aceRaw) continue;
                ACE_HEADER* hdr = (ACE_HEADER*)aceRaw;
                if (hdr->AceType != ACCESS_ALLOWED_ACE_TYPE) continue;
                ACCESS_ALLOWED_ACE* ace = (ACCESS_ALLOWED_ACE*)aceRaw;
                PSID sid = (PSID)&ace->SidStart;
                if (sid_is_non_admin_principal(sid) && (ace->Mask & kDangerousWriteMask)) {
                    nonAdminWrite = true;
                    break;
                }
            }
        }
        hardened = !nonAdminWrite;
    }
    LocalFree(psd);
    return hardened;
}

bool apply_protected_machine_config_dacl(const wchar_t* path, char* err, size_t errSize) {
    if (err && errSize) err[0] = 0;
    if (!path || !path[0]) {
        set_acl_err(err, errSize, "Empty machine config path", 0);
        return false;
    }
    // SYSTEM: Full, Administrators: Full, BUILTIN\Users: Read (0x120089).
    // No execute permission is needed for an .ini file; read is enough so the
    // unelevated GUI can display the current machine default.
    const wchar_t* sddl = L"D:P(A;;FA;;;SY)(A;;FA;;;BA)(A;;0x120089;;;BU)";
    PSECURITY_DESCRIPTOR psd = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl, SDDL_REVISION_1, &psd, nullptr)) {
        set_acl_err(err, errSize, "Failed building machine config DACL", GetLastError());
        return false;
    }
    BOOL daclPresent = FALSE, daclDefaulted = FALSE;
    PACL pDacl = nullptr;
    bool ok = false;
    if (GetSecurityDescriptorDacl(psd, &daclPresent, &pDacl, &daclDefaulted) && daclPresent) {
        DWORD rc = SetNamedSecurityInfoW((LPWSTR)path, SE_FILE_OBJECT,
            DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
            nullptr, nullptr, pDacl, nullptr);
        if (rc == ERROR_SUCCESS) {
            ok = true;
            BYTE adminSid[SECURITY_MAX_SID_SIZE] = {};
            DWORD adminSidSize = sizeof(adminSid);
            if (CreateWellKnownSid(WinBuiltinAdministratorsSid, nullptr, adminSid, &adminSidSize)) {
                SetNamedSecurityInfoW((LPWSTR)path, SE_FILE_OBJECT,
                    OWNER_SECURITY_INFORMATION, adminSid, nullptr, nullptr, nullptr);
            }
        } else {
            set_acl_err(err, errSize, "Failed applying machine config DACL", rc);
        }
    } else {
        set_acl_err(err, errSize, "Machine config DACL missing after build", GetLastError());
    }
    LocalFree(psd);
    return ok;
}

bool apply_protected_machine_config_dir_dacl(const wchar_t* path, char* err, size_t errSize) {
    if (err && errSize) err[0] = 0;
    if (!path || !path[0]) {
        set_acl_err(err, errSize, "Empty machine config directory path", 0);
        return false;
    }
    // SYSTEM: Full, Administrators: Full, BUILTIN\Users: Read & Execute (0x1200a9,
    // = list directory + traverse + read), all object+container inheritable
    // (OICI) so files created in the directory inherit the same read-only-for-
    // users protection.  0x1200a9 carries no write/create/delete bits, so a
    // standard user cannot plant or remove files in the shared bank directory.
    const wchar_t* sddl = L"D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICI;0x1200a9;;;BU)";
    PSECURITY_DESCRIPTOR psd = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl, SDDL_REVISION_1, &psd, nullptr)) {
        set_acl_err(err, errSize, "Failed building machine config directory DACL", GetLastError());
        return false;
    }
    BOOL daclPresent = FALSE, daclDefaulted = FALSE;
    PACL pDacl = nullptr;
    bool ok = false;
    if (GetSecurityDescriptorDacl(psd, &daclPresent, &pDacl, &daclDefaulted) && daclPresent) {
        DWORD rc = SetNamedSecurityInfoW((LPWSTR)path, SE_FILE_OBJECT,
            DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
            nullptr, nullptr, pDacl, nullptr);
        if (rc == ERROR_SUCCESS) {
            ok = true;
            BYTE adminSid[SECURITY_MAX_SID_SIZE] = {};
            DWORD adminSidSize = sizeof(adminSid);
            if (CreateWellKnownSid(WinBuiltinAdministratorsSid, nullptr, adminSid, &adminSidSize)) {
                SetNamedSecurityInfoW((LPWSTR)path, SE_FILE_OBJECT,
                    OWNER_SECURITY_INFORMATION, adminSid, nullptr, nullptr, nullptr);
            }
        } else {
            set_acl_err(err, errSize, "Failed applying machine config directory DACL", rc);
        }
    } else {
        set_acl_err(err, errSize, "Machine config directory DACL missing after build", GetLastError());
    }
    LocalFree(psd);
    return ok;
}

bool service_binary_dacl_is_hardened(const wchar_t* path) {
    if (!path || !path[0]) return false;
    PSECURITY_DESCRIPTOR psd = nullptr;
    PACL pDacl = nullptr;
    DWORD rc = GetNamedSecurityInfoW((LPWSTR)path, SE_FILE_OBJECT,
        DACL_SECURITY_INFORMATION, nullptr, nullptr, &pDacl, nullptr, &psd);
    if (rc != ERROR_SUCCESS || !psd) return false;

    bool hardened = false;
    SECURITY_DESCRIPTOR_CONTROL control = 0;
    DWORD revision = 0;
    if (GetSecurityDescriptorControl(psd, &control, &revision) &&
        (control & SE_DACL_PROTECTED)) {
        bool nonAdminWrite = false;
        if (pDacl) {
            for (DWORD i = 0; i < pDacl->AceCount; i++) {
                void* aceRaw = nullptr;
                if (!GetAce(pDacl, i, &aceRaw) || !aceRaw) continue;
                ACE_HEADER* hdr = (ACE_HEADER*)aceRaw;
                if (hdr->AceType != ACCESS_ALLOWED_ACE_TYPE) continue;
                ACCESS_ALLOWED_ACE* ace = (ACCESS_ALLOWED_ACE*)aceRaw;
                PSID sid = (PSID)&ace->SidStart;
                if (sid_is_non_admin_principal(sid) && (ace->Mask & kDangerousWriteMask)) {
                    nonAdminWrite = true;
                    break;
                }
            }
        }
        hardened = !nonAdminWrite;
    }
    LocalFree(psd);
    return hardened;
}
