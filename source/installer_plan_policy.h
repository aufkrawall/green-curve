// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// What the installer is going to do, decided before it does any of it.
//
// Every branch that matters on an upgrade — which directory wins, whether the
// service registration has to be re-pointed, whether the previous settings are
// worth capturing, which shortcut boxes start ticked — is resolved here from
// plain inputs.  The runtime then executes a plan it cannot argue with, and
// `build.py --test` can pin the interesting combinations (silent upgrade into a
// moved directory, fresh install, re-run over the same path) without a
// registry, a service, or a window.

#ifndef GREEN_CURVE_INSTALLER_PLAN_POLICY_H
#define GREEN_CURVE_INSTALLER_PLAN_POLICY_H

#include "installer_cli_policy.h"

// The folder name the payload always lands in.  The user picks where this
// folder goes, never what it is called, so an install is always recognizable
// and the uninstaller never removes a directory it did not create.
#define GC_INSTALL_FOLDER_NAME "Green Curve"

// Where the previous install was found, and what it recorded about itself.
struct GcPriorInstall {
    bool present;
    // Install directory recorded by a previous run of this installer.
    char directory[GC_INSTALLER_MAX_PATH_CHARS];
    char version[32];
    // The service is registered and its binary lives at this directory.  On an
    // upgrade that moves the installation, the SCM registration must follow.
    bool serviceRegistered;
    char serviceDirectory[GC_INSTALLER_MAX_PATH_CHARS];
    // Shortcut choices the previous run made, so an upgrade does not silently
    // add or remove icons the user already decided about.
    GcInstallerToggle startMenuShortcut;
    GcInstallerToggle desktopShortcut;
    // Recorded by the installer that placed this build: does its greencurve.exe
    // understand --export-active-settings?  A capability the previous install
    // wrote down is exact; a version number is a proxy that was already wrong
    // once (see gc_prior_supports_settings_export).  UNSET means the install
    // predates the marker and only the version can answer.
    GcInstallerToggle settingsExport;
};

struct GcInstallPlan {
    char targetDirectory[GC_INSTALLER_MAX_PATH_CHARS];
    bool isUpgrade;
    // The installation is moving: the old directory keeps its files (removing
    // them is the user's call) but every machine-wide pointer must be re-aimed.
    bool directoryChanged;
    char previousDirectory[GC_INSTALLER_MAX_PATH_CHARS];
    // Re-point the SCM registration.  True whenever a service is registered and
    // its binary directory is not the new target.
    bool repointService;
    // Ask the still-running old build for its live settings before anything is
    // stopped, then re-apply them explicitly once the new build is running.
    bool captureActiveSettings;
    // Ask the INSTALLED binary rather than the payload's.  The export talks to
    // the service that is running right now, and across a protocol bump the new
    // payload binary cannot: it refuses the old service's responses and reports
    // "no active settings", so the upgrade silently restores nothing.  The
    // installed binary always matches the running service by construction; it is
    // only used when the recorded version is known to understand the verb, since
    // an older build treats it as an unknown argument and opens its window.
    bool captureFromInstalledBinary;
    char captureBinaryDirectory[GC_INSTALLER_MAX_PATH_CHARS];
    bool createStartMenuShortcut;
    bool createDesktopShortcut;
    bool launchAfterInstall;
    bool valid;
    char error[192];
};

// Case-insensitive directory comparison that ignores one trailing separator.
// "C:\Program Files\Green Curve" and "C:\Program Files\Green Curve\" are the
// same install, and treating them as different would re-point a service that
// never moved.
static inline bool gc_install_paths_equal(const char* a, const char* b) {
    if (!a || !b) return false;
    size_t lenA = 0, lenB = 0;
    while (a[lenA]) lenA++;
    while (b[lenB]) lenB++;
    while (lenA > 0 && (a[lenA - 1] == '\\' || a[lenA - 1] == '/')) lenA--;
    while (lenB > 0 && (b[lenB - 1] == '\\' || b[lenB - 1] == '/')) lenB--;
    if (lenA != lenB) return false;
    for (size_t i = 0; i < lenA; i++) {
        char ca = a[i], cb = b[i];
        if (ca == '/') ca = '\\';
        if (cb == '/') cb = '\\';
        if (ca >= 'A' && ca <= 'Z') ca = (char)(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z') cb = (char)(cb - 'A' + 'a');
        if (ca != cb) return false;
    }
    return lenA > 0;
}

// Reject a destination before anything is extracted into it.
//
// The rules are about damage the installer could otherwise do: a drive root
// would scatter binaries across the volume and make the uninstaller's directory
// removal catastrophic, and a relative or device path would resolve against
// whatever working directory the caller happened to have.
static inline bool gc_install_directory_is_acceptable(const char* path, const char** reasonOut) {
    if (reasonOut) *reasonOut = nullptr;
    if (!path || !path[0]) {
        if (reasonOut) *reasonOut = "Choose an installation folder.";
        return false;
    }
    size_t length = 0;
    while (path[length]) length++;
    if (length + 1 >= GC_INSTALLER_MAX_PATH_CHARS) {
        if (reasonOut) *reasonOut = "That path is too long.";
        return false;
    }
    bool driveAbsolute = (length >= 3) &&
        ((path[0] >= 'A' && path[0] <= 'Z') || (path[0] >= 'a' && path[0] <= 'z')) &&
        path[1] == ':' && (path[2] == '\\' || path[2] == '/');
    bool uncAbsolute = (length >= 5) && (path[0] == '\\' || path[0] == '/') &&
        (path[1] == '\\' || path[1] == '/');
    if (!driveAbsolute && !uncAbsolute) {
        if (reasonOut) *reasonOut = "Enter a full path, for example C:\\Program Files\\Green Curve.";
        return false;
    }
    // A bare drive root ("C:\") has nothing after the separator.
    if (driveAbsolute) {
        size_t after = 3;
        while (after < length && (path[after] == '\\' || path[after] == '/')) after++;
        if (after >= length) {
            if (reasonOut) *reasonOut = "Choose a folder, not the root of a drive.";
            return false;
        }
    }
    for (size_t i = 0; i < length; i++) {
        char c = path[i];
        if ((unsigned char)c < 0x20 || c == '*' || c == '?' || c == '"' || c == '<' ||
            c == '>' || c == '|') {
            if (reasonOut) *reasonOut = "That path contains characters Windows does not allow.";
            return false;
        }
        // ":" is legal only as the drive separator.
        if (c == ':' && i != 1) {
            if (reasonOut) *reasonOut = "That path contains characters Windows does not allow.";
            return false;
        }
    }
    // Relative components would be resolved later, against a different base
    // than the one shown to the user.
    for (size_t i = 0; i + 1 < length; i++) {
        bool atComponentStart = (i == 0) || path[i - 1] == '\\' || path[i - 1] == '/';
        if (!atComponentStart || path[i] != '.') continue;
        char next = path[i + 1];
        if (next == '\\' || next == '/' || next == 0) {
            if (reasonOut) *reasonOut = "Enter a path without \".\" or \"..\" components.";
            return false;
        }
        if (next == '.') {
            char after = (i + 2 < length) ? path[i + 2] : 0;
            if (after == '\\' || after == '/' || after == 0) {
                if (reasonOut) *reasonOut = "Enter a path without \".\" or \"..\" components.";
                return false;
            }
        }
    }
    return true;
}

// Join a parent directory with the fixed install folder name.
static inline bool gc_install_default_directory(const char* programFiles, char* out, size_t outCount) {
    if (!out || outCount == 0) return false;
    out[0] = 0;
    if (!programFiles || !programFiles[0]) return false;
    size_t length = 0;
    while (programFiles[length]) length++;
    while (length > 0 && (programFiles[length - 1] == '\\' || programFiles[length - 1] == '/')) length--;
    if (length == 0) return false;
    size_t nameLength = 0;
    while (GC_INSTALL_FOLDER_NAME[nameLength]) nameLength++;
    if (length + 1 + nameLength + 1 > outCount) return false;
    for (size_t i = 0; i < length; i++) out[i] = programFiles[i];
    out[length] = '\\';
    for (size_t i = 0; i < nameLength; i++) out[length + 1 + i] = GC_INSTALL_FOLDER_NAME[i];
    out[length + 1 + nameLength] = 0;
    return true;
}

// The first release whose greencurve.exe understands
// --export-active-settings.  Anything older treats it as an unknown argument
// and falls through to opening the GUI, so it must never be run with it.
//
// This is a fallback, not the authority.  A version number answers a
// capability question only at release granularity, and it was already wrong
// once inside 0.21: the verb arrived with the custom installer, twelve commits
// after VERSION had moved to 0.21, so development builds exist that pass this
// test and do not understand the argument.  Every install written by an
// installer that carries the verb records it explicitly instead
// (GC_SETUP_SETTINGS_EXPORT_VALUE), and that marker wins.  This constant is
// consulted only for installs predating the marker.
#define GC_SETTINGS_EXPORT_MIN_MAJOR 0
#define GC_SETTINGS_EXPORT_MIN_MINOR 21

// Parse "0.21", "0.21.4", "1.0" — leading digits of the first two components,
// which is all the ARP DisplayVersion ever holds.  Returns false for anything
// it cannot read as two numbers, including an absent version: an unknown build
// is treated as too old rather than probed by running it.
static inline bool gc_version_at_least(const char* version,
                                       unsigned int major, unsigned int minor) {
    if (!version || !version[0]) return false;
    unsigned int parsed[2] = {0, 0};
    size_t index = 0;
    size_t component = 0;
    for (; component < 2; component++) {
        if (version[index] < '0' || version[index] > '9') return false;
        unsigned int value = 0;
        while (version[index] >= '0' && version[index] <= '9') {
            if (value > 100000u) return false;
            value = value * 10u + (unsigned int)(version[index] - '0');
            index++;
        }
        parsed[component] = value;
        if (component == 0) {
            if (version[index] != '.') return false;
            index++;
        }
    }
    if (parsed[0] != major) return parsed[0] > major;
    return parsed[1] >= minor;
}

static inline bool gc_prior_supports_settings_export(const GcPriorInstall* prior) {
    // The recorded capability and version both describe the ARP install.  When
    // the SCM is running a service from somewhere else, neither says anything
    // about the binary that would be asked, so the payload's binary is used.
    if (!prior || !prior->present || !prior->serviceRegistered ||
        !prior->serviceDirectory[0] ||
        !gc_install_paths_equal(prior->directory, prior->serviceDirectory))
        return false;
    // An explicit answer from the installer that placed the binary settles it
    // in both directions: OFF means "this build does not have the verb", which
    // a version comparison must not then talk us back out of.
    if (prior->settingsExport == GC_TOGGLE_ON) return true;
    if (prior->settingsExport == GC_TOGGLE_OFF) return false;
    return gc_version_at_least(prior->version, GC_SETTINGS_EXPORT_MIN_MAJOR,
                               GC_SETTINGS_EXPORT_MIN_MINOR);
}

static inline void gc_install_plan_reject(GcInstallPlan* plan, const char* message) {
    if (!plan) return;
    plan->valid = false;
    size_t i = 0;
    while (message && message[i] && i + 1 < sizeof(plan->error)) {
        plan->error[i] = message[i];
        i++;
    }
    plan->error[i] = 0;
}

static inline bool gc_install_toggle_value(GcInstallerToggle requested,
                                           GcInstallerToggle previous,
                                           bool freshDefault) {
    // An explicit command-line answer always wins.  Otherwise an upgrade
    // repeats what the previous install did, and a fresh install takes the
    // documented default.
    if (requested == GC_TOGGLE_ON) return true;
    if (requested == GC_TOGGLE_OFF) return false;
    if (previous == GC_TOGGLE_ON) return true;
    if (previous == GC_TOGGLE_OFF) return false;
    return freshDefault;
}

// Resolve the complete plan.  `defaultDirectory` is the fresh-install fallback
// (normally %ProgramFiles%\Green Curve).
static inline void gc_install_build_plan(const GcInstallerOptions* options,
                                         const GcPriorInstall* prior,
                                         const char* defaultDirectory,
                                         GcInstallPlan* plan) {
    if (!plan) return;
    GcInstallPlan blank = {};
    *plan = blank;
    plan->valid = true;
    if (!options || !options->valid) {
        gc_install_plan_reject(plan, "The installer command line was not understood.");
        return;
    }

    const char* chosen = nullptr;
    if (options->hasDirectory) {
        chosen = options->directory;
    } else if (prior && prior->present && prior->directory[0]) {
        // An upgrade defaults to where the program already is, so a silent
        // update never relocates an installation behind the user's back.
        chosen = prior->directory;
    } else {
        chosen = defaultDirectory;
    }
    const char* reason = nullptr;
    if (!gc_install_directory_is_acceptable(chosen, &reason)) {
        gc_install_plan_reject(plan, reason ? reason : "That installation folder cannot be used.");
        return;
    }
    size_t i = 0;
    while (chosen[i] && i + 1 < sizeof(plan->targetDirectory)) {
        plan->targetDirectory[i] = chosen[i];
        i++;
    }
    plan->targetDirectory[i] = 0;
    // Strip a trailing separator so every later comparison and join sees one
    // canonical spelling.
    while (i > 0 && (plan->targetDirectory[i - 1] == '\\' || plan->targetDirectory[i - 1] == '/')) {
        plan->targetDirectory[--i] = 0;
    }

    plan->isUpgrade = prior && prior->present;
    if (plan->isUpgrade && prior->directory[0]) {
        size_t p = 0;
        while (prior->directory[p] && p + 1 < sizeof(plan->previousDirectory)) {
            plan->previousDirectory[p] = prior->directory[p];
            p++;
        }
        plan->previousDirectory[p] = 0;
        plan->directoryChanged = !gc_install_paths_equal(plan->previousDirectory, plan->targetDirectory);
    }

    // The SCM registration is re-pointed whenever a registered service is not
    // already running out of the new directory.  This deliberately also covers
    // an install the registry does not know about (a hand-placed portable copy
    // that registered the service itself).
    if (prior && prior->serviceRegistered) {
        plan->repointService = !gc_install_paths_equal(prior->serviceDirectory, plan->targetDirectory);
    }

    // Capturing live settings only makes sense when something is already
    // running that can be asked for them.
    plan->captureActiveSettings = (prior && prior->present && prior->serviceRegistered);
    plan->captureFromInstalledBinary = plan->captureActiveSettings &&
        gc_prior_supports_settings_export(prior);
    if (plan->captureFromInstalledBinary) {
        size_t s = 0;
        while (prior->serviceDirectory[s] &&
               s + 1 < sizeof(plan->captureBinaryDirectory)) {
            plan->captureBinaryDirectory[s] = prior->serviceDirectory[s];
            s++;
        }
        plan->captureBinaryDirectory[s] = 0;
    }

    plan->createStartMenuShortcut = gc_install_toggle_value(
        options->startMenuShortcut, prior ? prior->startMenuShortcut : GC_TOGGLE_UNSET, true);
    plan->createDesktopShortcut = gc_install_toggle_value(
        options->desktopShortcut, prior ? prior->desktopShortcut : GC_TOGGLE_UNSET, false);
    // Silent runs exist to update a machine without interrupting it; a window
    // appearing at the end would defeat the point, so the default flips.
    plan->launchAfterInstall = gc_install_toggle_value(
        options->launchAfterInstall, GC_TOGGLE_UNSET, !options->silent);
}

#endif // GREEN_CURVE_INSTALLER_PLAN_POLICY_H
