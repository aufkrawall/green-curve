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
// A drive root, a share root, any well-known shell folder and anything inside
// the Windows directory are refused, and so is an existing folder holding
// files that are not Green Curve's (the content rule below) -- unless Green
// Curve hardened it already.  The refusal names the fix.  Refusing is safe in
// both directions: nothing is registered, no service is stopped, and no
// permissions were touched before the check (it runs before the first DACL
// write and before any running service is disturbed).
//
// The shape and content rules are host-neutral and pinned by the regression
// harness; the Win32 half (known folders by identity, the Windows subtree, the
// directory enumeration) lives in service_install_location.cpp.

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
    GC_SVC_LOCATION_KNOWN_FOLDER,
    GC_SVC_LOCATION_UNREADABLE,
    GC_SVC_LOCATION_REPARSE,
    // Anywhere inside the Windows directory: every folder there belongs to
    // the OS or to software the OS installed, at any depth.
    GC_SVC_LOCATION_SYSTEM_SUBTREE,
    // An existing folder holding something that is not Green Curve's (see
    // gc_service_location_entry_is_ours) that Green Curve has not already
    // hardened.  Hardening it would take write access to those files away.
    GC_SVC_LOCATION_FOREIGN_CONTENT,
    GC_SVC_LOCATION_VERDICT_COUNT
};

static inline const char* gc_service_location_verdict_name(int verdict) {
    switch (verdict) {
        case GC_SVC_LOCATION_OK: return "ok";
        case GC_SVC_LOCATION_EMPTY: return "empty";
        case GC_SVC_LOCATION_NOT_ABSOLUTE: return "not-absolute";
        case GC_SVC_LOCATION_DRIVE_ROOT: return "drive-root";
        case GC_SVC_LOCATION_SHARE_ROOT: return "share-root";
        case GC_SVC_LOCATION_KNOWN_FOLDER: return "well-known-folder";
        case GC_SVC_LOCATION_UNREADABLE: return "unreadable";
        case GC_SVC_LOCATION_REPARSE: return "reparse";
        case GC_SVC_LOCATION_SYSTEM_SUBTREE: return "windows-subtree";
        case GC_SVC_LOCATION_FOREIGN_CONTENT: return "foreign-content";
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

// THE CONTENT RULE ("is this folder plausibly ours?").  A blocklist of known
// folders can never be complete: %LOCALAPPDATA%\Programs, C:\Program
// Files\<another vendor>, or a D:\Tools shared with other programs are
// nobody's known folder and every one of them is destroyed by an
// administrators-only DACL.  So the Win32 half also enumerates an existing
// folder and accepts it only if every entry is a file Green Curve itself
// ships, leaves behind, or stages -- or if Green Curve already hardened it,
// in which case re-hardening changes nothing.
//
// Case-insensitive, exact names; no subdirectories.  Transient names are the
// ones setup (".gcnew") and older builds' staging (".tmp") write beside the
// payload; the rest are payload, legacy side files, and shell metadata.
static inline bool gc_service_location_entry_is_ours(const wchar_t* name, size_t length,
                                                     bool isDirectory) {
    if (!name || length == 0) return false;
    if (isDirectory) return false;
    static const wchar_t* const kOurs[] = {
        L"greencurve.exe", L"greencurve-service.exe", L"README.md", L"LICENSE",
        L"uninstall.exe",
        L"greencurve.exe.gcnew", L"greencurve-service.exe.gcnew", L"README.md.gcnew",
        L"LICENSE.gcnew", L"uninstall.exe.gcnew", L"greencurve-service.exe.tmp",
        // Written beside the binary by older builds.
        L"config.ini", L"machine.ini", L"greencurve_log.txt", L"greencurve_cli_log.txt",
        L"greencurve_debug.txt", L"greencurve_curve.json",
        // Explorer's own per-folder metadata.
        L"desktop.ini", L"Thumbs.db",
    };
    for (const wchar_t* ours : kOurs) {
        size_t i = 0;
        for (; i < length && ours[i]; i++) {
            wchar_t a = name[i];
            wchar_t b = ours[i];
            if (a >= L'A' && a <= L'Z') a += L'a' - L'A';
            if (b >= L'A' && b <= L'Z') b += L'a' - L'A';
            if (a != b) break;
        }
        if (i == length && ours[i] == 0) return true;
    }
    return false;
}

// What the Win32 half saw, for the log line that accompanies a verdict.
// Deliberately no names: a foreign entry is the user's own file, and its name
// is nobody's business in a support log.  Counts and kinds say enough.
struct GcServiceLocationDetail {
    bool exists;
    bool alreadyHardened;       // carried exactly Green Curve's DACL already
    unsigned int entriesScanned;
    bool foreignIsDirectory;    // the first foreign entry was a folder
    bool foreignIsReparse;      // ... or a reparse point
};

// `path` is `root` or lies below it (case-insensitive, separator-bounded).
// Used for the Windows-directory subtree rule on already-canonical paths.
static inline bool gc_service_location_is_within(const wchar_t* path, const wchar_t* root) {
    if (!path || !root) return false;
    size_t rootLength = 0;
    while (root[rootLength]) rootLength++;
    while (rootLength > 0 && (root[rootLength - 1] == L'\\' || root[rootLength - 1] == L'/'))
        rootLength--;
    if (rootLength == 0) return false;
    for (size_t i = 0; i < rootLength; i++) {
        wchar_t a = path[i];
        wchar_t b = root[i];
        if (!a) return false;
        if (a >= L'A' && a <= L'Z') a += L'a' - L'A';
        if (b >= L'A' && b <= L'Z') b += L'a' - L'A';
        if (a == L'/') a = L'\\';
        if (b == L'/') b = L'\\';
        if (a != b) return false;
    }
    return path[rootLength] == 0 || path[rootLength] == L'\\' || path[rootLength] == L'/';
}

// True when the shape half already refuses.  The Win32 half calls this first
// and only then pays for the known-folder lookups.
static inline bool gc_service_location_shape_is_acceptable(const wchar_t* path) {
    return gc_service_location_shape_verdict(path) == GC_SVC_LOCATION_OK;
}

// A different administrator account approving UAC has different per-user
// SHGetKnownFolderPath answers. Recognize the standard shell folders below
// *any* profile in the machine's Profiles directory after the Win32 gatherer
// has resolved the candidate's real path (including junctions and 8.3 names).
// This is deliberately exact: a child such as Downloads\Green Curve is still
// a dedicated installation directory.
static inline bool gc_service_location_ascii_component_eq(const wchar_t* begin,
                                                            size_t length,
                                                            const wchar_t* word) {
    size_t i = 0;
    for (; i < length && word[i]; i++) {
        wchar_t a = begin[i];
        wchar_t b = word[i];
        if (a >= L'A' && a <= L'Z') a += L'a' - L'A';
        if (b >= L'A' && b <= L'Z') b += L'a' - L'A';
        if (a != b) return false;
    }
    return i == length && word[i] == 0;
}

static inline bool gc_service_location_is_profile_shell_folder(
    const wchar_t* candidate, const wchar_t* profilesRoot) {
    if (!candidate || !profilesRoot) return false;
    size_t rootLength = 0;
    while (profilesRoot[rootLength]) rootLength++;
    while (rootLength > 0 && (profilesRoot[rootLength - 1] == L'\\' ||
                              profilesRoot[rootLength - 1] == L'/')) rootLength--;
    if (rootLength == 0) return false;
    for (size_t i = 0; i < rootLength; i++) {
        wchar_t a = candidate[i];
        wchar_t b = profilesRoot[i];
        if (!a) return false;
        if (a >= L'A' && a <= L'Z') a += L'a' - L'A';
        if (b >= L'A' && b <= L'Z') b += L'a' - L'A';
        if (a != b) return false;
    }
    if (candidate[rootLength] != L'\\' && candidate[rootLength] != L'/') return false;
    const wchar_t* profile = candidate + rootLength + 1;
    const wchar_t* end = profile;
    while (*end && *end != L'\\' && *end != L'/') end++;
    if (end == profile) return false;
    if (!*end) return true;  // another account's profile root

    const wchar_t* child = end + 1;
    end = child;
    while (*end && *end != L'\\' && *end != L'/') end++;
    size_t childLength = (size_t)(end - child);
    const wchar_t* const shells[] = {
        L"Desktop", L"Downloads", L"Documents", L"Music", L"Pictures",
        L"Videos", L"Favorites", L"Links", L"Searches", L"Contacts",
        L"Saved Games", L"AppData",
    };
    bool oneDriveRoot = childLength >= 8 &&
        gc_service_location_ascii_component_eq(child, 8, L"OneDrive") &&
        (childLength == 8 ||
         (childLength > 10 && child[8] == L' ' && child[9] == L'-' && child[10] == L' '));
    if (!*end) {
        if (oneDriveRoot) return true;
        for (const wchar_t* shell : shells) {
            if (gc_service_location_ascii_component_eq(child, childLength, shell)) return true;
        }
        return false;
    }
    if (oneDriveRoot) {
        const wchar_t* syncedChild = end + 1;
        end = syncedChild;
        while (*end && *end != L'\\' && *end != L'/') end++;
        if (*end) return false;
        size_t syncedLength = (size_t)(end - syncedChild);
        for (const wchar_t* shell : shells) {
            if (gc_service_location_ascii_component_eq(syncedChild, syncedLength, shell))
                return true;
        }
        return false;
    }
    if (!gc_service_location_ascii_component_eq(child, childLength, L"AppData")) return false;
    const wchar_t* appDataChild = end + 1;
    end = appDataChild;
    while (*end && *end != L'\\' && *end != L'/') end++;
    if (*end) return false;
    size_t appDataLength = (size_t)(end - appDataChild);
    return gc_service_location_ascii_component_eq(appDataChild, appDataLength, L"Local") ||
           gc_service_location_ascii_component_eq(appDataChild, appDataLength, L"Roaming") ||
           gc_service_location_ascii_component_eq(appDataChild, appDataLength, L"LocalLow");
}

#endif // GREEN_CURVE_SERVICE_INSTALL_LOCATION_POLICY_H
