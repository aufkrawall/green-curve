// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// The command line the updater hands to the setup program, as pure logic.
//
// ## Why this is a file and not four lines at the call site
//
// It was four lines at the call site, and it shipped broken. The updater built:
//
//     "<setup>" /S /D=C:\Program Files\Green Curve --no-launch
//
// `CommandLineToArgvW()` splits an unquoted argument on spaces, so the setup
// program received `/D=C:\Program`, `Files\Green`, `Curve`, `--no-launch`. It
// parsed the install directory as `C:\Program`, met `Files\Green` as an unknown
// switch, and refused the whole command line -- exactly as
// `installer_cli_policy.h` promises ("unknown switches are rejected, not
// ignored: an updater passing a typo must fail loudly"). Exit code 3, before a
// single install step ran, so there was not even a failure log to find.
//
// The default install directory contains a space, so this failed for **every**
// standard installation. No unit test caught it because nothing tested the
// string the updater actually produced against the parser that actually reads
// it. That round trip is now assertion 4260-4269.
//
// ## The form chosen, and why not `/D=`
//
// `/D=` keeps its NSIS spelling for humans and existing scripts, where the
// convention is that it comes last and is taken verbatim to end-of-line. That
// convention does not survive `CommandLineToArgvW`, which is what the setup
// program actually uses, so `/D=` is only safe unquoted when the path has no
// spaces -- i.e. exactly when nobody needs it.
//
// `--dir <value>` takes the following argv entry, so the value can be quoted
// like any other argument and needs no positional rule. That is the form
// designed for a machine to emit, so the machine emits it.

#ifndef GREEN_CURVE_UPDATE_INSTALL_POLICY_H
#define GREEN_CURVE_UPDATE_INSTALL_POLICY_H

#include <stddef.h>

// Setup path + install directory + switches + quotes + terminator.
#define GC_UPDATE_COMMAND_LINE_MAX_CHARS 1024

// A path that cannot be expressed as a single argv entry.
//
// Windows paths cannot contain `"`, so a quote here means the value did not
// come from the registry or the filesystem and the caller is confused; the same
// goes for control characters. Refused rather than escaped: this string becomes
// the command line of a process running as SYSTEM, and "sanitize it" is not a
// risk worth taking for an input that should never occur.
static inline bool gc_update_path_is_quotable(const char* path) {
    if (!path || !path[0]) return false;
    for (size_t i = 0; path[i]; ++i) {
        unsigned char c = (unsigned char)path[i];
        if (c == '"' || c < 0x20 || c == 0x7F) return false;
    }
    // A trailing backslash would escape the closing quote ("C:\dir\" -> the
    // quote becomes literal and the argument swallows the rest of the line).
    size_t length = 0;
    while (path[length]) length++;
    if (path[length - 1] == '\\') return false;
    return true;
}

static inline bool gc_update_session_id_is_quotable(const char* sessionId) {
    if (!sessionId || !sessionId[0]) return false;
    size_t digits = 0;
    while (sessionId[digits]) {
        if (sessionId[digits] < '0' || sessionId[digits] > '9') return false;
        ++digits;
        if (digits > 10) return false;
    }
    if (digits > 1 && sessionId[0] == '0') return false;
    // ULONG_MAX.  Session ids are well below this; the boundary keeps a hostile
    // or corrupt value from becoming an integer parser's wraparound problem.
    static const char* const max = "4294967295";
    if (digits < 10) return true;
    for (size_t i = 0; i < digits; ++i) {
        if (sessionId[i] != max[i]) return sessionId[i] < max[i];
    }
    return true;
}

static inline bool gc_update_command_append(char* out, size_t outSize, size_t* at,
                                            const char* text) {
    if (!out || !at || !text) return false;
    for (size_t i = 0; text[i]; ++i) {
        if (*at + 1 >= outSize) return false;
        out[(*at)++] = text[i];
    }
    out[*at] = 0;
    return true;
}

// Build `"<setup>" /S --no-launch --dir "<installDir>"`, or the relaunch form
// `"<setup>" /S --launch --launch-session <id> --dir "<installDir>"`.
//
//   /S            silent; the whole point of driving setup from a service.
//   --dir         the CURRENT install directory. Passing it explicitly matters:
//                 without it setup uses its own default and could relocate an
//                 installation the user deliberately put elsewhere.
//   --launch /
//   --no-launch   whether setup starts the GUI again when it finishes.
//
// `relaunchGui` is decided from processes actually closed, and
// `launchSessionId` is the authenticated pipe session that requested the
// update. Setup itself lives in session 0 and must not guess between console
// and RDP users.
//
// Silent mode defaults `launch` to OFF, so the flag is always passed explicitly
// rather than relying on that default meaning what we want.
static inline bool gc_update_build_installer_command_line(const char* setupPath,
                                                          const char* installDir,
                                                          bool relaunchGui,
                                                          const char* launchSessionId,
                                                          char* out, size_t outSize) {
    if (!out || outSize == 0) return false;
    out[0] = 0;
    if (!gc_update_path_is_quotable(setupPath)) return false;
    if (!gc_update_path_is_quotable(installDir)) return false;
    if (relaunchGui && !gc_update_session_id_is_quotable(launchSessionId)) return false;

    size_t at = 0;
    if (!gc_update_command_append(out, outSize, &at, "\"")) { out[0] = 0; return false; }
    if (!gc_update_command_append(out, outSize, &at, setupPath)) { out[0] = 0; return false; }
    if (!gc_update_command_append(out, outSize, &at,
                                  relaunchGui ? "\" /S --launch" : "\" /S --no-launch")) {
        out[0] = 0; return false;
    }
    if (relaunchGui) {
        if (!gc_update_command_append(out, outSize, &at, " --launch-session ")) {
            out[0] = 0; return false;
        }
        if (!gc_update_command_append(out, outSize, &at, launchSessionId)) {
            out[0] = 0; return false;
        }
    }
    if (!gc_update_command_append(out, outSize, &at, " --dir \"")) { out[0] = 0; return false; }
    if (!gc_update_command_append(out, outSize, &at, installDir)) { out[0] = 0; return false; }
    if (!gc_update_command_append(out, outSize, &at, "\"")) { out[0] = 0; return false; }
    return true;
}

#endif // GREEN_CURVE_UPDATE_INSTALL_POLICY_H
