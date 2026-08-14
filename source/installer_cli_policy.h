// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// Setup command line parsing, as pure logic.
//
// The silent switch is the contract a future in-app updater will drive, so its
// parsing is unit-tested rather than discovered in the field: a typo that makes
// `/S` open a window would hang an unattended update forever behind an
// invisible dialog.
//
// Accepted forms:
//
//   /S               --silent            install/upgrade with no window
//   /D=<path>        --dir <path>        install directory (the "Green Curve"
//                                        folder itself, not its parent)
//   /uninstall       --uninstall         remove an existing installation
//   /log=<name>      --log <name>        override the failure log filename
//                    --no-start-menu     suppress the Start menu shortcut
//                    --no-desktop        suppress the desktop shortcut
//                    --desktop           create the desktop shortcut
//                    --no-launch         do not start Green Curve afterwards
//                    --launch            start Green Curve afterwards
//                    --launch-session id the authenticated session to relaunch
//                                        into (used only by the updater)
//   /?  -h           --help              usage
//
// `/S` and `/D=` keep their NSIS spelling on purpose: the previous releases
// shipped an NSIS setup, so scripts and notes that already exist keep working
// against the replacement.

#ifndef GREEN_CURVE_INSTALLER_CLI_POLICY_H
#define GREEN_CURVE_INSTALLER_CLI_POLICY_H

#include <stddef.h>

#define GC_INSTALLER_MAX_PATH_CHARS 520

// A tri-state, because "the user did not mention shortcuts" and "the user asked
// for no shortcuts" are different answers on an upgrade: the first keeps what
// the previous install chose, the second overrides it.
enum GcInstallerToggle {
    GC_TOGGLE_UNSET = 0,
    GC_TOGGLE_OFF = 1,
    GC_TOGGLE_ON = 2,
};

enum GcInstallerMode {
    GC_INSTALLER_MODE_INSTALL = 0,
    GC_INSTALLER_MODE_UNINSTALL = 1,
    GC_INSTALLER_MODE_HELP = 2,
};

struct GcInstallerOptions {
    GcInstallerMode mode;
    bool silent;
    bool hasDirectory;
    char directory[GC_INSTALLER_MAX_PATH_CHARS];
    bool hasLogPath;
    char logPath[GC_INSTALLER_MAX_PATH_CHARS];
    GcInstallerToggle startMenuShortcut;
    GcInstallerToggle desktopShortcut;
    GcInstallerToggle launchAfterInstall;
    bool hasLaunchSession;
    unsigned int launchSessionId;
    bool valid;
    char error[192];
};

static inline void gc_installer_options_defaults(GcInstallerOptions* options) {
    if (!options) return;
    GcInstallerOptions blank = {};
    *options = blank;
    options->mode = GC_INSTALLER_MODE_INSTALL;
    options->valid = true;
}

static inline bool gc_installer_copy_arg(char* dst, size_t dstCount, const char* src) {
    if (!dst || dstCount == 0) return false;
    dst[0] = 0;
    if (!src) return false;
    size_t i = 0;
    while (src[i]) {
        if (i + 1 >= dstCount) return false;
        dst[i] = src[i];
        i++;
    }
    dst[i] = 0;
    return i > 0;
}

static inline bool gc_installer_arg_equals(const char* arg, const char* candidate) {
    if (!arg || !candidate) return false;
    size_t i = 0;
    while (arg[i] && candidate[i]) {
        char a = arg[i], b = candidate[i];
        if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
        if (a != b) return false;
        i++;
    }
    return arg[i] == 0 && candidate[i] == 0;
}

// True when `arg` starts with `prefix` (case-insensitively); `*restOut` then
// points at the remainder, which is how `/D=C:\...` yields its path.
static inline bool gc_installer_arg_prefix(const char* arg, const char* prefix, const char** restOut) {
    if (restOut) *restOut = nullptr;
    if (!arg || !prefix) return false;
    size_t i = 0;
    while (prefix[i]) {
        char a = arg[i], b = prefix[i];
        if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
        if (a != b) return false;
        i++;
    }
    if (restOut) *restOut = arg + i;
    return true;
}

static inline bool gc_installer_log_name_is_acceptable(const char* name) {
    if (!name || !name[0]) return false;
    if ((name[0] == '.' && name[1] == 0) ||
        (name[0] == '.' && name[1] == '.' && name[2] == 0)) return false;
    for (size_t i = 0; name[i]; ++i) {
        unsigned char c = (unsigned char)name[i];
        if (c < 0x20 || c == '/' || c == '\\' || c == ':' ||
            c == '*' || c == '?' || c == '"' || c == '<' ||
            c == '>' || c == '|') return false;
    }
    return true;
}

static inline void gc_installer_reject(GcInstallerOptions* options, const char* message, const char* detail) {
    if (!options || !options->valid) return;
    options->valid = false;
    size_t i = 0;
    while (message && message[i] && i + 1 < sizeof(options->error)) {
        options->error[i] = message[i];
        i++;
    }
    size_t d = 0;
    while (detail && detail[d] && i + 1 < sizeof(options->error)) {
        options->error[i++] = detail[d++];
    }
    options->error[i] = 0;
}

// Parse an argv-style vector that EXCLUDES argv[0].
//
// Unknown switches are rejected rather than ignored.  A silent updater that
// passes a misspelled flag must fail loudly at the command line instead of
// quietly installing with defaults the caller did not ask for.
static inline void gc_installer_parse_options(int argc, const char* const* argv,
                                              GcInstallerOptions* options) {
    if (!options) return;
    gc_installer_options_defaults(options);
    for (int i = 0; i < argc; i++) {
        const char* arg = argv ? argv[i] : nullptr;
        if (!arg || !arg[0]) continue;
        const char* rest = nullptr;

        if (gc_installer_arg_equals(arg, "/S") || gc_installer_arg_equals(arg, "--silent") ||
            gc_installer_arg_equals(arg, "/silent")) {
            options->silent = true;
        } else if (gc_installer_arg_equals(arg, "/?") || gc_installer_arg_equals(arg, "-h") ||
                   gc_installer_arg_equals(arg, "--help") || gc_installer_arg_equals(arg, "/help")) {
            options->mode = GC_INSTALLER_MODE_HELP;
        } else if (gc_installer_arg_equals(arg, "--uninstall") || gc_installer_arg_equals(arg, "/uninstall")) {
            options->mode = GC_INSTALLER_MODE_UNINSTALL;
        } else if (gc_installer_arg_prefix(arg, "/D=", &rest)) {
            // NSIS spelling: everything after "=" is the path, unquoted and
            // taken verbatim so spaces need no escaping.
            if (!gc_installer_copy_arg(options->directory, sizeof(options->directory), rest)) {
                gc_installer_reject(options, "Missing or overlong path after ", "/D=");
                return;
            }
            options->hasDirectory = true;
        } else if (gc_installer_arg_prefix(arg, "/log=", &rest)) {
            if (!gc_installer_copy_arg(options->logPath, sizeof(options->logPath), rest)) {
                gc_installer_reject(options, "Missing or overlong path after ", "/log=");
                return;
            }
            if (!gc_installer_log_name_is_acceptable(options->logPath)) {
                gc_installer_reject(options,
                    "The failure log must be a filename without a path: ", rest);
                return;
            }
            options->hasLogPath = true;
        } else if (gc_installer_arg_equals(arg, "--dir")) {
            if (i + 1 >= argc ||
                !gc_installer_copy_arg(options->directory, sizeof(options->directory), argv[i + 1])) {
                gc_installer_reject(options, "Missing or overlong path after ", "--dir");
                return;
            }
            options->hasDirectory = true;
            i++;
        } else if (gc_installer_arg_equals(arg, "--log")) {
            if (i + 1 >= argc ||
                !gc_installer_copy_arg(options->logPath, sizeof(options->logPath), argv[i + 1])) {
                gc_installer_reject(options, "Missing or overlong path after ", "--log");
                return;
            }
            if (!gc_installer_log_name_is_acceptable(options->logPath)) {
                gc_installer_reject(options,
                    "The failure log must be a filename without a path: ",
                    options->logPath);
                return;
            }
            options->hasLogPath = true;
            i++;
        } else if (gc_installer_arg_equals(arg, "--no-start-menu")) {
            options->startMenuShortcut = GC_TOGGLE_OFF;
        } else if (gc_installer_arg_equals(arg, "--start-menu")) {
            options->startMenuShortcut = GC_TOGGLE_ON;
        } else if (gc_installer_arg_equals(arg, "--no-desktop")) {
            options->desktopShortcut = GC_TOGGLE_OFF;
        } else if (gc_installer_arg_equals(arg, "--desktop")) {
            options->desktopShortcut = GC_TOGGLE_ON;
        } else if (gc_installer_arg_equals(arg, "--no-launch")) {
            options->launchAfterInstall = GC_TOGGLE_OFF;
        } else if (gc_installer_arg_equals(arg, "--launch")) {
            options->launchAfterInstall = GC_TOGGLE_ON;
        } else if (gc_installer_arg_equals(arg, "--launch-session")) {
            if (i + 1 >= argc) {
                gc_installer_reject(options, "Missing session id after ", "--launch-session");
                return;
            }
            const char* value = argv[++i];
            size_t digits = 0;
            while (value[digits] >= '0' && value[digits] <= '9') {
                digits++;
                if (digits > 10) break;
            }
            if (digits == 0 || value[digits] != 0 ||
                (digits > 1 && value[0] == '0')) {
                gc_installer_reject(options, "Invalid session id after ", "--launch-session");
                return;
            }
            unsigned long long parsed = 0;
            for (size_t j = 0; j < digits; ++j) {
                parsed = parsed * 10ULL + (unsigned long long)(value[j] - '0');
            }
            if (parsed > 0xFFFFFFFFULL) {
                gc_installer_reject(options, "Session id is out of range after ",
                                    "--launch-session");
                return;
            }
            options->hasLaunchSession = true;
            options->launchSessionId = (unsigned int)parsed;
        } else {
            gc_installer_reject(options, "Unrecognized option: ", arg);
            return;
        }
    }
    if (options->mode == GC_INSTALLER_MODE_UNINSTALL && options->hasDirectory) {
        gc_installer_reject(options, "--dir cannot be combined with ", "--uninstall");
    }
    if (options->hasLaunchSession && options->launchAfterInstall != GC_TOGGLE_ON) {
        gc_installer_reject(options, "--launch-session requires ", "--launch");
    }
}

#endif // GREEN_CURVE_INSTALLER_CLI_POLICY_H
