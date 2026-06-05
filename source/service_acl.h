// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// Service-binary ACL hardening (F-SEC-1).
//
// The LocalSystem service is registered with the SCM by absolute path.  If that
// binary (or its containing directory) is writable by a standard user, the user
// can replace it and obtain SYSTEM code execution the next time the SCM starts
// the service — and the driver-recovery design auto-restarts it.  These helpers
// apply a protected DACL (SYSTEM + Administrators: Full, Users: Read & Execute,
// inheritance disabled) to the installed service binary so a non-admin cannot
// overwrite it in place, and revert to inherited ACLs on uninstall so the user
// can freely delete/replace the binary again.
//
// Declared in a standalone translation unit so the security-critical ACL logic
// is unit-testable by the regression harness.

#pragma once

#include <cstddef>

// Apply the protected service-binary DACL to `path`.  Returns false (with a
// human-readable reason in err) on failure.
bool apply_protected_service_binary_dacl(const wchar_t* path, char* err, size_t errSize);

// Re-enable inheritance and drop the explicit protected DACL from `path`, so the
// object inherits its parent directory's ACLs again (used on uninstall).
bool restore_inherited_dacl(const wchar_t* path, char* err, size_t errSize);

// True if `path` resolves under an admin-only system root (%ProgramFiles%,
// %ProgramFiles(x86)%, %ProgramW6432%, %SystemRoot%).  When false, the install
// directory is assumed user-writable and the binary cannot be fully protected
// in place (the parent can still grant delete/create), so the caller should warn.
bool service_path_is_under_secure_root(const wchar_t* path);

// True if `path` currently carries a hardened DACL: inheritance disabled
// (SE_DACL_PROTECTED) AND no non-admin principal (Everyone / BUILTIN\Users /
// Authenticated Users / INTERACTIVE) is granted any write/delete/own access.
bool service_binary_dacl_is_hardened(const wchar_t* path);
