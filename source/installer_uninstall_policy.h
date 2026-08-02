// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// What an uninstall is allowed to remove, decided without a registry, a task
// scheduler, or a running process.
//
// Removal is the one operation where a wrong match is unrecoverable, so every
// "is this ours?" question is answered here by a pure predicate that
// `build.py --test` can pin on either host.  Three of them exist because three
// things outlive a file deletion:
//
//   * the per-user logon task in Task Scheduler, named after the user who
//     enabled it, so the name is only recognizable by its prefix;
//   * the per-user HKCU Run value that starts the resident tray GUI, which
//     lives in a registry key shared with every other program on the machine;
//   * the uninstaller binary itself, which must delete itself when it is the
//     installed copy and must NOT when it is the setup stub run with
//     --uninstall from a downloads folder.

#ifndef GREEN_CURVE_INSTALLER_UNINSTALL_POLICY_H
#define GREEN_CURVE_INSTALLER_UNINSTALL_POLICY_H

#include "installer_plan_policy.h"

// Mirrors STARTUP_TASK_PREFIX in app_shared.h, which the installer cannot
// include (it drags in the GPU model and the service protocol).  The two
// spellings are pinned to each other by a source gate in
// tools/installer_build.py: drifting them apart would silently turn "uninstall"
// back into "uninstall, but the logon task stays behind".
#define GC_STARTUP_TASK_PREFIX "Green Curve Startup - "

// Mirrors main_tray_autostart.cpp.  The resident tray process is started from
// an ordinary per-user Run value, not from the scheduled task, so removing one
// never removes the other.  Same source-gate treatment as the task prefix.
#define GC_TRAY_AUTOSTART_RUN_SUBKEY "Software\\Microsoft\\Windows\\CurrentVersion\\Run"
#define GC_TRAY_AUTOSTART_VALUE_NAME "Green Curve"

static inline char gc_uninstall_lower_ascii(char c) {
    return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

// Every logon task this program registers is "<prefix><sanitized SAM name>".
// The trailing part belongs to whoever ticked the box, so the prefix carries
// the entire identity claim.
//
// It is deliberately an anchored prefix test with a non-empty remainder, not a
// substring search: a task somebody else named "Backup before Green Curve
// Startup - nightly" is not ours, and a bare "Green Curve Startup - " with no
// user was never written by this program either.
static inline bool gc_uninstall_task_name_is_ours(const char* name) {
    if (!name) return false;
    size_t i = 0;
    for (; GC_STARTUP_TASK_PREFIX[i]; i++) {
        if (!name[i]) return false;
        if (gc_uninstall_lower_ascii(name[i]) !=
            gc_uninstall_lower_ascii(GC_STARTUP_TASK_PREFIX[i])) return false;
    }
    return name[i] != 0;
}

// HKCU\...\Run is shared with every other program on the machine, and the value
// name is a plain product string.  Matching the name alone would let an
// uninstall delete a same-named entry that has nothing to do with this
// installation, so the command line's first executable token has to be our
// exact leaf name as well.  Arguments are not identity: another program may
// legitimately mention greencurve.exe in one of them.
//
// `leafName` is passed in rather than spelled here so the file name lives in
// exactly one place (GC_SETUP_GUI_EXE in installer_common.h).
static inline bool gc_uninstall_command_references(const char* command, const char* leafName) {
    if (!command || !command[0] || !leafName || !leafName[0]) return false;

    size_t tokenStart = 0;
    while (command[tokenStart] == ' ' || command[tokenStart] == '\t')
        ++tokenStart;
    bool quoted = command[tokenStart] == '"';
    if (quoted) ++tokenStart;
    size_t tokenEnd = tokenStart;
    if (quoted) {
        while (command[tokenEnd] && command[tokenEnd] != '"') ++tokenEnd;
        if (command[tokenEnd] != '"') return false;
        char after = command[tokenEnd + 1];
        if (after && after != ' ' && after != '\t') return false;
    } else {
        while (command[tokenEnd] &&
               command[tokenEnd] != ' ' && command[tokenEnd] != '\t') {
            ++tokenEnd;
        }
    }
    if (tokenEnd == tokenStart) return false;

    size_t leafStart = tokenStart;
    for (size_t i = tokenStart; i < tokenEnd; ++i) {
        if (command[i] == '\\' || command[i] == '/') leafStart = i + 1;
    }
    size_t commandLeafLength = tokenEnd - leafStart;
    size_t expectedLength = 0;
    while (leafName[expectedLength]) ++expectedLength;
    if (commandLeafLength != expectedLength) return false;
    for (size_t i = 0; i < expectedLength; ++i) {
        if (gc_uninstall_lower_ascii(command[leafStart + i]) !=
            gc_uninstall_lower_ascii(leafName[i])) return false;
    }
    return true;
}

// Directory part of a full file path, without a trailing separator.  Used to
// decide whether the running binary is the copy being removed; kept here rather
// than reusing the Win32 helper so the decision itself stays testable.
static inline bool gc_uninstall_directory_of(const char* filePath, char* out, size_t outCount) {
    if (!out || outCount == 0) return false;
    out[0] = 0;
    if (!filePath || !filePath[0]) return false;
    size_t cut = 0;
    bool found = false;
    for (size_t i = 0; filePath[i]; i++) {
        if (filePath[i] == '\\' || filePath[i] == '/') {
            cut = i;
            found = true;
        }
    }
    if (!found || cut == 0 || cut + 1 > outCount) return false;
    for (size_t i = 0; i < cut; i++) out[i] = filePath[i];
    out[cut] = 0;
    return true;
}

// The uninstaller removes itself only when it is running from the directory it
// is removing.
//
// The same gc_uninstall_execute() also runs inside the setup stub launched with
// --uninstall, and that binary is normally sitting in a downloads folder.
// Deleting *that* would take the user's setup file with it -- which is exactly
// what the unconditional self-delete did before this predicate existed.
static inline bool gc_uninstall_self_is_installed_copy(const char* modulePath,
                                                       const char* installDirectory) {
    char directory[GC_INSTALLER_MAX_PATH_CHARS] = {};
    if (!gc_uninstall_directory_of(modulePath, directory, sizeof(directory))) return false;
    return gc_install_paths_equal(directory, installDirectory);
}

#endif // GREEN_CURVE_INSTALLER_UNINSTALL_POLICY_H
