// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// Escaping for the files `--write-assets` generates (F-03-003).
//
// THE DEFECT THIS EXISTS FOR (audit 2026-09-15, 0.25.2):
//
//   linux_main.cpp had exactly ONE escaping routine -- shell_quote_single(),
//   which wraps a value in single quotes and rewrites an embedded ' as '"'"'.
//   That is correct for the POSIX shell, and the generated files put its output
//   inside TWO OTHER grammars that it knows nothing about:
//
//     Exec=sh -lc "exec '<path>' --tui --from-desktop --config '<cfg>'"
//     ExecStart=/bin/sh -lc "exec '<path>' --apply-config --config '<cfg>'"
//     ConditionPathExists=<cfg>            <- not escaped at all
//
//   The Desktop Entry Specification says that inside a quoted argument the
//   characters " ` $ and \ must each be preceded by a backslash, and that a
//   literal % must be written %% (otherwise it is a field code such as %f/%U).
//   systemd unit files say that $ must be written $$ and % must be written %%,
//   and -- the serious one -- that a NEWLINE ENDS THE DIRECTIVE.  So a --config
//   path containing a newline appended attacker-chosen directives to a unit
//   file that the generated README then instructs the user to install with
//   `sudo install -Dm644 ... /etc/systemd/system/`.
//
//   --config is copied straight from argv with snprintf and never validated, so
//   nothing upstream stopped any of it.
//
// THE RULE, in two halves:
//
//   1. REFUSE what no grammar here can represent.  A newline or a control
//      character in a path cannot be escaped into a .desktop Exec value or a
//      unit directive in any way that round-trips, and "cleaned" output is a
//      much harder thing to reason about than a refusal -- the same argument
//      gc_archive_name_is_safe() makes for payload names.
//   2. ESCAPE the rest PER FORMAT.  Three grammars, three functions.  The shell
//      quoter stays exactly as it was and keeps its own job.
//
// Pure and header-only so the regression harness pins every case on both hosts;
// the callers live in linux_main.cpp.

#ifndef GREEN_CURVE_LINUX_ASSET_ESCAPING_POLICY_H
#define GREEN_CURVE_LINUX_ASSET_ESCAPING_POLICY_H

#include <stddef.h>

// Why a path cannot appear in a generated asset file, or nullptr when it can.
//
// The reason string is the message the user sees, so it names the character
// class rather than saying "invalid": the whole point is that the caller can
// fix their path.
static inline const char* linux_asset_path_reject_reason(const char* path) {
    if (!path || !path[0]) return "path is empty";
    for (const char* p = path; *p; ++p) {
        unsigned char c = (unsigned char)*p;
        if (c == '\n' || c == '\r') {
            // The one that matters most: in a systemd unit file a newline ends
            // the directive, so everything after it becomes a directive of its
            // own in a file the user is told to install as root.
            return "path contains a line break, which would split a generated "
                   "systemd unit directive";
        }
        if (c < 0x20 || c == 0x7f) {
            return "path contains a control character";
        }
    }
    return nullptr;
}

static inline bool linux_asset_path_is_representable(const char* path) {
    return linux_asset_path_reject_reason(path) == nullptr;
}

// Append one character, NUL-terminating, and report whether it fit.  Every
// escaper below fails closed on overflow rather than emitting a truncated value
// that would still parse as something.
static inline bool gc_asset_put(char* out, size_t outSize, size_t* at, char c) {
    if (!out || !at || *at + 1 >= outSize) return false;
    out[*at] = c;
    ++(*at);
    out[*at] = '\0';
    return true;
}

// Escape a value for the INSIDE of a double-quoted Desktop Entry Exec argument.
//
// Per the Desktop Entry Specification: backslash-escape " ` $ and \, and double
// a literal % so it is not read as a field code.  The caller supplies the
// surrounding quotes.
static inline bool linux_desktop_exec_escape(const char* value, char* out,
                                             size_t outSize) {
    if (!out || outSize == 0) return false;
    out[0] = '\0';
    if (!value) return true;
    size_t at = 0;
    for (const char* p = value; *p; ++p) {
        char c = *p;
        if (c == '"' || c == '`' || c == '$' || c == '\\') {
            if (!gc_asset_put(out, outSize, &at, '\\')) return false;
        } else if (c == '%') {
            // Doubling, not backslash-escaping: % is a field code, not a
            // quoting metacharacter, and the spec spells its literal as %%.
            if (!gc_asset_put(out, outSize, &at, '%')) return false;
        }
        if (!gc_asset_put(out, outSize, &at, c)) return false;
    }
    return true;
}

// Escape a value for a systemd unit-file directive.
//
// systemd expands %-specifiers (%h, %i, ...) and $-variables in unit values;
// the literals are %% and $$.  Callers must have already refused a newline via
// linux_asset_path_reject_reason() -- there is no escape for one.
static inline bool linux_systemd_value_escape(const char* value, char* out,
                                              size_t outSize) {
    if (!out || outSize == 0) return false;
    out[0] = '\0';
    if (!value) return true;
    size_t at = 0;
    for (const char* p = value; *p; ++p) {
        char c = *p;
        if (c == '%' || c == '$') {
            if (!gc_asset_put(out, outSize, &at, c)) return false;
        }
        if (!gc_asset_put(out, outSize, &at, c)) return false;
    }
    return true;
}

// Everything write_linux_assets() needs, escaped once, in one call.
//
// Bundled rather than left as five calls at the call site because the point of
// this header is that each value reaches exactly ONE grammar's escaper: spread
// across the caller, the next person to add a field picks whichever escaper is
// nearest.  `shellQuotedExec`/`shellQuotedConfig` are the already
// single-quoted forms (the inner sh layer); `rawConfig` is the unquoted path,
// which is what ConditionPathExists= takes.
struct LinuxAssetEscapedPaths {
    char desktopExec[1024];
    char desktopConfig[1024];
    char systemdExec[1024];
    char systemdConfig[1024];
    char systemdConditionPath[1024];
};

static inline bool linux_asset_escape_all(const char* shellQuotedExec,
                                          const char* shellQuotedConfig,
                                          const char* rawConfig,
                                          LinuxAssetEscapedPaths* out) {
    if (!out) return false;
    return linux_desktop_exec_escape(shellQuotedExec, out->desktopExec,
                                     sizeof(out->desktopExec)) &&
        linux_desktop_exec_escape(shellQuotedConfig, out->desktopConfig,
                                  sizeof(out->desktopConfig)) &&
        linux_systemd_value_escape(shellQuotedExec, out->systemdExec,
                                   sizeof(out->systemdExec)) &&
        linux_systemd_value_escape(shellQuotedConfig, out->systemdConfig,
                                   sizeof(out->systemdConfig)) &&
        linux_systemd_value_escape(rawConfig, out->systemdConditionPath,
                                   sizeof(out->systemdConditionPath));
}

// The whole gate: refuse what no grammar can represent, then escape the rest
// per format.  Returns the user-facing reason, or nullptr when the caller may
// proceed and `out` is filled.
//
// One entry point because the two halves must not be separable -- escaping a
// path that should have been refused is precisely the bug, and a caller that
// can reach the escaper without the check is a caller that can reintroduce it.
static inline const char* linux_asset_prepare_paths(
    const char* rawExec, const char* shellQuotedExec,
    const char* rawConfig, const char* shellQuotedConfig,
    LinuxAssetEscapedPaths* out) {
    const char* why = linux_asset_path_reject_reason(rawExec);
    if (why) return why;
    why = linux_asset_path_reject_reason(rawConfig);
    if (why) return why;
    if (!linux_asset_escape_all(shellQuotedExec, shellQuotedConfig, rawConfig, out))
        return "path is too long once escaped for the generated file formats";
    return nullptr;
}

#endif // GREEN_CURVE_LINUX_ASSET_ESCAPING_POLICY_H
