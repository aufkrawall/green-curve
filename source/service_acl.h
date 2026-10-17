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
#include "service_install_location_policy.h"

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
// Unconditional: callers that release a folder they did not just create use
// release_service_hardening() instead, which proves the DACL is ours first.
bool restore_inherited_dacl(const wchar_t* path, char* err, size_t errSize);

// ---------------------------------------------------------------------------
// Handle-bound service hardening.
//
// Every check before a DACL write and the write itself must name the SAME
// object.  SetNamedSecurityInfoW re-resolves the path and follows reparse
// points, so a folder a standard account can rename could be swapped for a
// junction between the elevated caller's checks and its write -- and the
// elevated caller would then rewrite the DACL of whatever the junction names.
// These functions take a handle the caller opened with
// FILE_FLAG_OPEN_REPARSE_POINT (and, to pin the object while it works,
// without FILE_SHARE_DELETE), and verify through that same handle.
// ---------------------------------------------------------------------------

enum GcServiceAclKind {
    GC_SERVICE_ACL_DIRECTORY = 0,   // D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICI;0x1200a9;;;BU)
    GC_SERVICE_ACL_BINARY = 1,      // D:P(A;;FA;;;SY)(A;;FA;;;BA)(A;;0x1200a9;;;BU)
    GC_SERVICE_ACL_CONFIG = 2,      // same file ACL, Users read-only (0x120089)
    // SYSTEM + Administrators only, nothing for Users: the directory a SYSTEM
    // process launches an executable from, and the executable inside it.
    GC_SERVICE_ACL_PRIVATE_DIRECTORY = 3,  // D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)
    GC_SERVICE_ACL_PRIVATE_FILE = 4        // D:P(A;;FA;;;SY)(A;;FA;;;BA)
};
bool service_acl_kind_is_directory(GcServiceAclKind kind);
// The protected DACL Green Curve writes for `kind`, as SDDL (no owner part).
const wchar_t* service_acl_kind_sddl(GcServiceAclKind kind);

// True when `sd` carries EXACTLY the protected DACL Green Curve writes for
// `kind`: inheritance disabled and precisely the three ACEs above, in any
// order.  This is the proof of ownership a release needs -- Program Files,
// System32 or a user's Downloads never carry exactly this DACL, so a folder
// that does was hardened by Green Curve (any build: the SDDL never changed).
// (`securityDescriptor` is a PSECURITY_DESCRIPTOR; void* keeps this header
// includable by the host-neutral regression harness.)
bool service_security_descriptor_is_ours(void* securityDescriptor, GcServiceAclKind kind);
bool service_handle_dacl_is_ours(void* handle, GcServiceAclKind kind);
// Owner is BUILTIN\Administrators.  A standard-user owner keeps implicit
// WRITE_DAC and could re-grant itself write to a hardened folder.
bool service_handle_owner_is_administrators(void* handle);
// Owner is BUILTIN\Administrators or LocalSystem: an owner a standard account
// cannot assign, so the object was created or claimed by a privileged caller.
bool service_handle_owner_is_privileged(void* handle);
// A startup bank may retain bytes only when its PREVIOUS ACL and owner prove
// admin control.  An unproven bank is never adopted: no byte of it is read,
// kept or written through.  Its NAME is set aside (service_set_aside_by_name)
// and the bank is absent afterwards (`*discarded` = true).  On that path the
// function closes `*handle` and sets it to INVALID_HANDLE_VALUE, because the
// move needs the name unpinned; `path` is the name the handle was opened by.
// A proven bank is re-hardened in place through the handle, left open.
bool service_prepare_shared_bank_handle(void** handle, const wchar_t* path,
    bool requireAdminOwner, bool* discarded, char* err, size_t errSize);

// Move whatever holds `path` out of the way without writing through it: a
// same-directory rename to "<path>.unproven-<suffix>" (a link is moved, never
// its target), then a best-effort delete (a non-empty directory stays renamed
// aside).  `expectedIdentity` (a BY_HANDLE_FILE_INFORMATION*, may be null) is
// the file id the caller inspected; a moved object with a different id fails
// the call and is left aside untouched.
bool service_set_aside_by_name(const wchar_t* path, const void* expectedIdentity,
                               char* err, size_t errSize);

// ---------------------------------------------------------------------------
// Never adopt what we did not create (service_acl_claim.cpp).
//
// A pre-existing object under a world-creatable parent (%ProgramData%) may
// have been created by a standard account that still holds a handle with
// WRITE_DAC or DELETE.  Re-ACLing such an object in place does not revoke
// those handles, so the service never converts an unproven object into a
// trusted one.  It either proves the object is already ours -- exact
// protected DACL and a privileged owner, read through a handle that does not
// follow reparse points -- or sets it aside and creates a fresh one whose
// security descriptor is part of the CreateDirectoryW / CreateFileW call.
// ---------------------------------------------------------------------------
enum GcServiceClaimResult {
    GC_SERVICE_CLAIM_FAILED = 0,
    GC_SERVICE_CLAIM_CREATED,    // nothing was there; created with our descriptor
    GC_SERVICE_CLAIM_ADOPTED,    // already provably ours; nothing rewritten
    GC_SERVICE_CLAIM_REPLACED    // something unproven was set aside, then created
};
// `kind` is GC_SERVICE_ACL_DIRECTORY or GC_SERVICE_ACL_PRIVATE_DIRECTORY.
// `requireAdminOwner` puts O:BA into the creation descriptor and requires a
// privileged owner to adopt (false only for the unelevated regression harness).
// `allowAdopt` false means fresh-only: any existing name fails the claim.
// `pinnedOut` (optional) receives a handle opened with
// FILE_FLAG_OPEN_REPARSE_POINT and without FILE_SHARE_DELETE, carrying
// READ_CONTROL | FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES.
int service_claim_protected_directory(const wchar_t* path, GcServiceAclKind kind,
                                      bool requireAdminOwner, bool allowAdopt,
                                      void** pinnedOut, char* err, size_t errSize);
const char* service_claim_result_name(int result);
// CREATE_NEW a file with the protected descriptor for `kind` in the call
// itself (GC_SERVICE_ACL_CONFIG / BINARY / PRIVATE_FILE).  Opened with
// FILE_FLAG_OPEN_REPARSE_POINT; `access` and `shareMode` are the caller's.
// Returns INVALID_HANDLE_VALUE (as void*) on failure.
void* service_create_protected_file(const wchar_t* path, GcServiceAclKind kind,
                                    bool requireAdminOwner, unsigned long access,
                                    unsigned long shareMode, char* err, size_t errSize);
// Re-prove a pinned directory immediately before its content is used: the
// exact DACL for `kind`, a privileged owner (when required), and that it holds
// exactly one entry, `expectedLeaf`, which is a regular non-reparse file.
bool service_directory_holds_only(void* directoryHandle, GcServiceAclKind kind,
                                  bool requireAdminOwner, const wchar_t* expectedLeaf,
                                  char* err, size_t errSize);

// Opens `path` without following a leaf reparse point and answers
// service_handle_dacl_is_ours.  False for anything it cannot open or read.
bool service_path_dacl_is_ours(const wchar_t* path, GcServiceAclKind kind);

// Apply the protected DACL through `handle` (opened with READ_CONTROL |
// WRITE_DAC | WRITE_OWNER).  With `requireAdminOwner` the owner change is part
// of the SAME call and its failure fails the hardening; without it (the
// unelevated regression harness) the owner change is attempted separately and
// may be refused.  Verified by reading the descriptor back from the handle.
bool apply_protected_service_dacl_to_handle(void* handle, GcServiceAclKind kind,
                                            bool requireAdminOwner,
                                            char* err, size_t errSize);

// Undo Green Curve's hardening of `path`, but only if the DACL there is
// provably ours (service_security_descriptor_is_ours).  Anything else -- a
// system folder, a folder someone re-ACLed since, a reparse point -- is left
// exactly as it is.  The inherited DACL is restored with an EMPTY explicit
// ACL, never a null one: a null DACL is "Everyone: Full Control".
enum GcServiceReleaseResult {
    GC_SERVICE_RELEASE_RELEASED = 0,
    GC_SERVICE_RELEASE_NOT_OURS,
    GC_SERVICE_RELEASE_ABSENT,
    GC_SERVICE_RELEASE_FAILED
};
int release_service_hardening(const wchar_t* path, GcServiceAclKind kind,
                              char* err, size_t errSize);
const char* service_release_result_name(int result);

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

// May Green Curve REWRITE this folder's permissions in order to register a
// LocalSystem service out of it?  See service_install_location_policy.h: this
// is not "how protected is it" but "is this folder plausibly Green Curve's
// own", because registering the service REPLACES the folder's DACL with an
// administrators-only-write one and propagates it to everything already inside.
// Returns a GcServiceLocationVerdict; GC_SVC_LOCATION_OK means it may proceed.
// Refuses a drive root, a UNC share root, any well-known shell folder (the
// user profile, Desktop, Downloads, Documents, AppData, ProgramData, Program
// Files, ...) by directory identity, anything inside the Windows directory,
// and an existing folder that holds files that are not Green Curve's -- unless
// Green Curve already hardened it.  A dedicated subfolder is fine, which keeps
// C:\Program Files\Green Curve and a deliberate portable folder working.
// Implemented in service_install_location.cpp.
int gc_service_install_location_verdict(const wchar_t* directory);
// Same verdict, with the existing directory's identity, reparse state and
// content read through `directoryHandle` -- the handle the caller is about to
// harden through -- instead of a fresh open by name.  The handle needs
// FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES | READ_CONTROL and must have
// been opened with FILE_FLAG_OPEN_REPARSE_POINT.
int gc_service_install_location_verdict_for_handle(const wchar_t* directory,
                                                   void* directoryHandle);
// Both of the above, plus what was seen (for the log line; never names).
// `directoryHandle` may be null; `detailOut` may be null.
int gc_service_install_location_verdict_detailed(const wchar_t* directory, void* directoryHandle,
                                                 GcServiceLocationDetail* detailOut);
