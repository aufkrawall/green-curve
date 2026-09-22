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

#include "service_path_chain_policy.h"

// Apply the protected service-binary DACL to `path`.  Returns false (with a
// human-readable reason in err) on failure.
bool apply_protected_service_binary_dacl(const wchar_t* path, char* err, size_t errSize);

// Apply the protected service-install directory DACL: SYSTEM + Administrators:
// Full, BUILTIN\Users: Read & Execute.  Used before staging the LocalSystem
// service binary so a writable existing directory cannot keep delete/create
// rights around the protected file.
bool apply_protected_service_dir_dacl(const wchar_t* path, char* err, size_t errSize);

// Re-enable inheritance and drop the explicit protected DACL from `path`, so the
// object inherits its parent directory's ACLs again (used on uninstall).
bool restore_inherited_dacl(const wchar_t* path, char* err, size_t errSize);

// Gather Win32 facts about `path` and classify them into `out` (see
// service_path_chain_policy.h for the property and the fail-safe rule).  Never
// fails: an unprobeable path classifies as not protected.  Supersedes the old
// "is it under Program Files" check, which was a proxy for this proof.
// `preflightMode` should be set true only during setup preflight where the leaf
// directory DACL will be replaced before files are installed.
void classify_path_protection(const wchar_t* path, GcPathProtectionReport* out,
                              bool preflightMode = false);

// True when `path` resolves under %USERPROFILE% or under the user-profiles
// root (C:\Users and its localized/redirected spellings).  The single
// implementation behind both the path classifier's `under_user_profile` fact
// and the GUI's "other accounts cannot launch this copy" warning - they warn
// about the same directory in the same status line, so they must not be able
// to disagree about what a user profile is.
bool gc_path_is_under_user_profile(const wchar_t* path);

// True if `path` currently carries a hardened DACL: inheritance disabled
// (SE_DACL_PROTECTED) AND no non-admin principal (Everyone / BUILTIN\Users /
// Authenticated Users / INTERACTIVE) is granted any write/delete/own access.
bool service_binary_dacl_is_hardened(const wchar_t* path);

// Apply a protected DACL suitable for the machine-wide config file: SYSTEM +
// Administrators: Full, BUILTIN\Users: Read.  This lets unelevated GUIs read
// the current machine default while preventing non-admins from changing it.
bool apply_protected_machine_config_dacl(const wchar_t* path, char* err, size_t errSize);

// Apply a protected DACL to the machine-wide config DIRECTORY (e.g.
// %ProgramData%\Green Curve): SYSTEM + Administrators: Full, BUILTIN\Users:
// Read & Execute (list), inheritable to children.  Inheritance is disabled at
// the directory itself (PROTECTED) so the default %ProgramData% ACL — which
// grants ordinary users create-file rights — cannot flow in and let a non-admin
// plant or delete files in the shared bank directory.
bool apply_protected_machine_config_dir_dacl(const wchar_t* path, char* err, size_t errSize);

// True if `path` carries the machine-config protected DACL: inheritance
// disabled and no non-admin principal is granted write/delete/own access.
bool machine_config_dacl_is_hardened(const wchar_t* path);
