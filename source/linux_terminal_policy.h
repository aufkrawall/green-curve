// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// linux_terminal_policy.h — pure terminal-emulator selection.
//
// Double-clicking the binary in a graphical file manager runs it with no
// controlling terminal, so the TUI used to print "Linux TUI requires an
// interactive terminal" to a stderr nobody sees and exit 1.  This header owns
// the decision of *which* terminal to relaunch into and *how* to pass argv to
// it, with no POSIX dependency, so the whole table is covered by the pure
// regression harness on any host.
//
// Only terminals with a documented argv-passing flag are listed: the launcher
// execv()s a fixed argument vector and never builds a shell command string, so
// a path or profile name containing spaces or quotes cannot be reinterpreted.

#ifndef GREEN_CURVE_LINUX_TERMINAL_POLICY_H
#define GREEN_CURVE_LINUX_TERMINAL_POLICY_H

#include <stdbool.h>
#include <stddef.h>

enum LinuxTerminalArgStyle {
    // <term> -e <exe> <args...>
    LINUX_TERMINAL_ARG_DASH_E = 0,
    // <term> -x <exe> <args...>   (xfce4-terminal/mate-terminal/terminator:
    // their -e takes one string, -x takes the remainder as a real argv)
    LINUX_TERMINAL_ARG_DASH_X = 1,
    // <term> -- <exe> <args...>
    LINUX_TERMINAL_ARG_DOUBLE_DASH = 2,
    // <term> <exe> <args...>
    LINUX_TERMINAL_ARG_DIRECT = 3,
    // <term> start -- <exe> <args...>
    LINUX_TERMINAL_ARG_START_DOUBLE_DASH = 4,
};

struct LinuxTerminalCandidate {
    const char* command;
    int style;  // LinuxTerminalArgStyle
};

struct LinuxTerminalChoice {
    bool found;
    const char* command;
    int style;
};

// Probe callback: returns true when `command` is an executable on PATH.
typedef bool (*LinuxTerminalProbeFn)(const char* command, void* context);

// Generic fallback order, tried when the desktop-specific preference is absent.
static inline const LinuxTerminalCandidate* linux_terminal_fallback_table(
    unsigned int* count) {
    static const LinuxTerminalCandidate table[] = {
        // Debian/Ubuntu alternatives symlink first: it is whatever the admin
        // chose, and every implementation behind it accepts -e <argv>.
        {"x-terminal-emulator", LINUX_TERMINAL_ARG_DASH_E},
        {"konsole",             LINUX_TERMINAL_ARG_DASH_E},
        {"ptyxis",              LINUX_TERMINAL_ARG_DOUBLE_DASH},
        {"kgx",                 LINUX_TERMINAL_ARG_DOUBLE_DASH},
        {"gnome-terminal",      LINUX_TERMINAL_ARG_DOUBLE_DASH},
        {"xfce4-terminal",      LINUX_TERMINAL_ARG_DASH_X},
        {"mate-terminal",       LINUX_TERMINAL_ARG_DASH_X},
        {"qterminal",           LINUX_TERMINAL_ARG_DASH_E},
        {"lxterminal",          LINUX_TERMINAL_ARG_DASH_E},
        {"deepin-terminal",     LINUX_TERMINAL_ARG_DASH_E},
        {"cosmic-term",         LINUX_TERMINAL_ARG_DASH_E},
        {"terminator",          LINUX_TERMINAL_ARG_DASH_X},
        {"alacritty",           LINUX_TERMINAL_ARG_DASH_E},
        {"kitty",               LINUX_TERMINAL_ARG_DIRECT},
        {"foot",                LINUX_TERMINAL_ARG_DIRECT},
        {"wezterm",             LINUX_TERMINAL_ARG_START_DOUBLE_DASH},
        {"urxvt",               LINUX_TERMINAL_ARG_DASH_E},
        {"xterm",               LINUX_TERMINAL_ARG_DASH_E},
    };
    if (count) *count = (unsigned int)(sizeof(table) / sizeof(table[0]));
    return table;
}

// ASCII case-insensitive compare of a bounded token against a literal.
static inline bool linux_terminal_token_equals(const char* token, size_t length,
                                               const char* literal) {
    if (!token || !literal) return false;
    size_t i = 0;
    for (; i < length && literal[i]; ++i) {
        char a = token[i];
        char b = literal[i];
        if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
        if (a != b) return false;
    }
    return i == length && literal[i] == '\0';
}

// Desktop-preferred terminals for one XDG_CURRENT_DESKTOP token.  Returns the
// number written into `out` (0 when the token is unknown).  `out` must have
// room for LINUX_TERMINAL_MAX_PREFERRED entries.
#define LINUX_TERMINAL_MAX_PREFERRED 3

static inline unsigned int linux_terminal_preferred_for_token(
    const char* token, size_t length,
    LinuxTerminalCandidate* out, unsigned int outCapacity) {
    if (!out || outCapacity == 0) return 0;
    const LinuxTerminalCandidate kde[] = {
        {"konsole", LINUX_TERMINAL_ARG_DASH_E},
    };
    const LinuxTerminalCandidate gnome[] = {
        {"ptyxis",         LINUX_TERMINAL_ARG_DOUBLE_DASH},
        {"kgx",            LINUX_TERMINAL_ARG_DOUBLE_DASH},
        {"gnome-terminal", LINUX_TERMINAL_ARG_DOUBLE_DASH},
    };
    const LinuxTerminalCandidate xfce[] = {
        {"xfce4-terminal", LINUX_TERMINAL_ARG_DASH_X},
    };
    const LinuxTerminalCandidate mate[] = {
        {"mate-terminal", LINUX_TERMINAL_ARG_DASH_X},
    };
    const LinuxTerminalCandidate lxqt[] = {
        {"qterminal", LINUX_TERMINAL_ARG_DASH_E},
    };
    const LinuxTerminalCandidate lxde[] = {
        {"lxterminal", LINUX_TERMINAL_ARG_DASH_E},
    };
    const LinuxTerminalCandidate deepin[] = {
        {"deepin-terminal", LINUX_TERMINAL_ARG_DASH_E},
    };
    const LinuxTerminalCandidate cosmic[] = {
        {"cosmic-term", LINUX_TERMINAL_ARG_DASH_E},
    };
    const LinuxTerminalCandidate wlroots[] = {
        {"foot",      LINUX_TERMINAL_ARG_DIRECT},
        {"alacritty", LINUX_TERMINAL_ARG_DASH_E},
        {"kitty",     LINUX_TERMINAL_ARG_DIRECT},
    };

    const LinuxTerminalCandidate* chosen = nullptr;
    unsigned int chosenCount = 0;
    if (linux_terminal_token_equals(token, length, "kde") ||
        linux_terminal_token_equals(token, length, "plasma")) {
        chosen = kde; chosenCount = 1;
    } else if (linux_terminal_token_equals(token, length, "gnome") ||
               linux_terminal_token_equals(token, length, "unity") ||
               linux_terminal_token_equals(token, length, "budgie") ||
               linux_terminal_token_equals(token, length, "x-cinnamon") ||
               linux_terminal_token_equals(token, length, "cinnamon") ||
               linux_terminal_token_equals(token, length, "pantheon")) {
        chosen = gnome; chosenCount = 3;
    } else if (linux_terminal_token_equals(token, length, "xfce")) {
        chosen = xfce; chosenCount = 1;
    } else if (linux_terminal_token_equals(token, length, "mate")) {
        chosen = mate; chosenCount = 1;
    } else if (linux_terminal_token_equals(token, length, "lxqt")) {
        chosen = lxqt; chosenCount = 1;
    } else if (linux_terminal_token_equals(token, length, "lxde")) {
        chosen = lxde; chosenCount = 1;
    } else if (linux_terminal_token_equals(token, length, "deepin") ||
               linux_terminal_token_equals(token, length, "dde")) {
        chosen = deepin; chosenCount = 1;
    } else if (linux_terminal_token_equals(token, length, "cosmic")) {
        chosen = cosmic; chosenCount = 1;
    } else if (linux_terminal_token_equals(token, length, "sway") ||
               linux_terminal_token_equals(token, length, "hyprland") ||
               linux_terminal_token_equals(token, length, "wlroots") ||
               linux_terminal_token_equals(token, length, "river")) {
        chosen = wlroots; chosenCount = 3;
    }
    if (!chosen) return 0;
    if (chosenCount > outCapacity) chosenCount = outCapacity;
    for (unsigned int i = 0; i < chosenCount; ++i) out[i] = chosen[i];
    return chosenCount;
}

// Pick a terminal for `currentDesktop` (the raw XDG_CURRENT_DESKTOP value, which
// is a colon-separated list such as "ubuntu:GNOME").  Desktop-specific
// preferences are tried in order for every token, then the generic table.
static inline LinuxTerminalChoice linux_terminal_select(
    const char* currentDesktop, LinuxTerminalProbeFn probe, void* context) {
    LinuxTerminalChoice choice = {false, nullptr, LINUX_TERMINAL_ARG_DASH_E};
    if (!probe) return choice;

    if (currentDesktop && currentDesktop[0]) {
        const char* cursor = currentDesktop;
        while (*cursor) {
            const char* end = cursor;
            while (*end && *end != ':') ++end;
            LinuxTerminalCandidate preferred[LINUX_TERMINAL_MAX_PREFERRED] = {};
            unsigned int count = linux_terminal_preferred_for_token(
                cursor, (size_t)(end - cursor), preferred,
                LINUX_TERMINAL_MAX_PREFERRED);
            for (unsigned int i = 0; i < count; ++i) {
                if (probe(preferred[i].command, context)) {
                    choice.found = true;
                    choice.command = preferred[i].command;
                    choice.style = preferred[i].style;
                    return choice;
                }
            }
            cursor = *end ? end + 1 : end;
        }
    }

    unsigned int fallbackCount = 0;
    const LinuxTerminalCandidate* fallback =
        linux_terminal_fallback_table(&fallbackCount);
    for (unsigned int i = 0; i < fallbackCount; ++i) {
        if (probe(fallback[i].command, context)) {
            choice.found = true;
            choice.command = fallback[i].command;
            choice.style = fallback[i].style;
            return choice;
        }
    }
    return choice;
}

// Fixed argv prefix a style inserts between the terminal command and our
// executable.  Returns the number of entries written (0..2).
static inline unsigned int linux_terminal_style_prefix(
    int style, const char* out[2]) {
    if (!out) return 0;
    switch (style) {
        case LINUX_TERMINAL_ARG_DASH_E:      out[0] = "-e"; return 1;
        case LINUX_TERMINAL_ARG_DASH_X:      out[0] = "-x"; return 1;
        case LINUX_TERMINAL_ARG_DOUBLE_DASH: out[0] = "--"; return 1;
        case LINUX_TERMINAL_ARG_DIRECT:      return 0;
        case LINUX_TERMINAL_ARG_START_DOUBLE_DASH:
            out[0] = "start"; out[1] = "--"; return 2;
        default: out[0] = "-e"; return 1;
    }
}

// A graphical launch is one with no controlling terminal but a live display
// server.  Without a display there is nothing to open a window on, so failing
// with the existing message on stderr stays correct (cron, ssh, systemd).
static inline bool linux_terminal_should_relaunch(bool stdinIsTty,
                                                  bool stdoutIsTty,
                                                  bool alreadyRelaunched,
                                                  const char* waylandDisplay,
                                                  const char* x11Display) {
    if (alreadyRelaunched) return false;
    if (stdinIsTty && stdoutIsTty) return false;
    bool hasDisplay = (waylandDisplay && waylandDisplay[0]) ||
                      (x11Display && x11Display[0]);
    return hasDisplay;
}

#endif
