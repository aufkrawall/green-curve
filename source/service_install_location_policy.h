// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// Pure policy: may Green Curve REWRITE this folder's permissions in order to
// register a LocalSystem service out of it?
//
// This is a different question from service_path_chain_policy.h, which asks how
// well a folder IS protected.  Registering the service does not only measure
// the folder, it replaces the folder's DACL with a protected
// SYSTEM/Administrators-Full + Users-Read&Execute one
// (apply_protected_service_dir_dacl), inheritable, with inheritance disabled.
// That is exactly right for a folder that holds nothing but Green Curve, and
// destructive for a folder that holds anything else.
//
// THE DEFECT THIS EXISTS FOR (audit 2026-09-22):
//
//   setup validates its destination (gc_install_directory_is_acceptable refuses
//   a drive root) but the PORTABLE path validated nothing.  README says
//   "extract the .7z archive anywhere and run greencurve.exe
//   --service-install", and 7-Zip's Extract Here into the Downloads folder is
//   the obvious way to follow that.  ensure_secure_service_binary_path then
//   hardened whatever directory greencurve.exe happened to sit in - so ticking
//   the GUI's service checkbox turned the user's own Downloads folder into
//   Users: Read & Execute, no create, no write, propagated to everything
//   already in it.  On a drive root it was worse than destructive, it was
//   permanent: cleanup_secure_service_binary_after_remove deliberately skips
//   reverting a root, so uninstalling never gave the volume back.
//
// THE RULE: we only ever harden a folder that is plausibly Green Curve's OWN.
// A drive root, a share root and any well-known shell folder are refused by
// name, and the refusal names the fix.  Refusing is safe in both directions:
// the service is not registered, so nothing is half-done, and no permissions
// were touched before the check (it runs before the first DACL write).
//
// The shape rules are host-neutral and pinned by the regression harness; the
// known-folder comparison needs SHGetKnownFolderPath and lives in
// service_path_chain.cpp beside the other Win32 gathering.

#ifndef GREEN_CURVE_SERVICE_INSTALL_LOCATION_POLICY_H
#define GREEN_CURVE_SERVICE_INSTALL_LOCATION_POLICY_H

// Why a location was refused.  Separate from GcServiceAdminReason because the
// caller maps all of these onto GC_SVC_ADMIN_LOCATION_REFUSED and only the
// LOG needs to distinguish them.
enum GcServiceLocationVerdict {
    GC_SVC_LOCATION_OK = 0,
    GC_SVC_LOCATION_EMPTY,
    GC_SVC_LOCATION_NOT_ABSOLUTE,
    GC_SVC_LOCATION_DRIVE_ROOT,
    GC_SVC_LOCATION_SHARE_ROOT,
    GC_SVC_LOCATION_KNOWN_FOLDER
};

static inline const char* gc_service_location_verdict_name(int verdict) {
    switch (verdict) {
        case GC_SVC_LOCATION_OK: return "ok";
        case GC_SVC_LOCATION_EMPTY: return "empty";
        case GC_SVC_LOCATION_NOT_ABSOLUTE: return "not-absolute";
        case GC_SVC_LOCATION_DRIVE_ROOT: return "drive-root";
        case GC_SVC_LOCATION_SHARE_ROOT: return "share-root";
        case GC_SVC_LOCATION_KNOWN_FOLDER: return "well-known-folder";
        default: return "unknown";
    }
}

// Path-SHAPE half of the rule, on a wide path that GetFullPathNameW has
// already canonicalized.  Answers only what the characters can prove:
//
//   * a drive-absolute path needs at least one component below "X:\";
//   * a UNC path needs at least one component below "\\server\share";
//   * anything not drive-absolute or UNC is refused outright, because a
//     relative or device path would be resolved against something other than
//     what the caller believes it named.
//
// Trailing separators are ignored, so "D:\" and "D:\\" and "D:" all refuse.
static inline int gc_service_location_shape_verdict(const wchar_t* path) {
    if (!path || !path[0]) return GC_SVC_LOCATION_EMPTY;

    size_t length = 0;
    while (path[length]) length++;
    while (length > 0 && (path[length - 1] == L'\\' || path[length - 1] == L'/')) length--;
    if (length == 0) return GC_SVC_LOCATION_EMPTY;

    bool driveAbsolute = length >= 2 && path[1] == L':' &&
        ((path[0] >= L'A' && path[0] <= L'Z') || (path[0] >= L'a' && path[0] <= L'z'));
    bool unc = length >= 2 && (path[0] == L'\\' || path[0] == L'/') &&
        (path[1] == L'\\' || path[1] == L'/');

    if (driveAbsolute) {
        // "C:" and "C:\" alike: nothing below the root.
        if (length <= 2) return GC_SVC_LOCATION_DRIVE_ROOT;
        if (path[2] != L'\\' && path[2] != L'/') return GC_SVC_LOCATION_NOT_ABSOLUTE;
        size_t after = 3;
        while (after < length && (path[after] == L'\\' || path[after] == L'/')) after++;
        if (after >= length) return GC_SVC_LOCATION_DRIVE_ROOT;
        return GC_SVC_LOCATION_OK;
    }

    if (unc) {
        // Count the components after the leading "\\": server, share, then the
        // first real directory.  Fewer than three is a share root or worse.
        unsigned int components = 0;
        bool inComponent = false;
        for (size_t i = 2; i < length; i++) {
            bool separator = path[i] == L'\\' || path[i] == L'/';
            if (separator) {
                inComponent = false;
            } else if (!inComponent) {
                inComponent = true;
                components++;
            }
        }
        if (components < 3) return GC_SVC_LOCATION_SHARE_ROOT;
        return GC_SVC_LOCATION_OK;
    }

    return GC_SVC_LOCATION_NOT_ABSOLUTE;
}

// True when the shape half already refuses.  The Win32 half calls this first
// and only then pays for the known-folder lookups.
static inline bool gc_service_location_shape_is_acceptable(const wchar_t* path) {
    return gc_service_location_shape_verdict(path) == GC_SVC_LOCATION_OK;
}

#endif // GREEN_CURVE_SERVICE_INSTALL_LOCATION_POLICY_H
