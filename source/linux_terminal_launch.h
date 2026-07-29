// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// linux_terminal_launch.h — relaunch the TUI inside a terminal emulator when
// the binary was started from a graphical file manager.  Selection policy lives
// in linux_terminal_policy.h; this is the thin POSIX layer around it.

#ifndef GREEN_CURVE_LINUX_TERMINAL_LAUNCH_H
#define GREEN_CURVE_LINUX_TERMINAL_LAUNCH_H

#include <stdbool.h>
#include <stddef.h>

// True when `name` resolves to a regular executable on PATH.
bool linux_command_available(const char* name);

// Re-exec this binary inside a detected terminal emulator.
//
// Only returns on failure (nothing suitable found, or exec failed), with the
// reason in `err`; on success the process image has been replaced.  `argv` is
// forwarded unchanged plus the `--from-desktop` guard, which both prevents a
// second relaunch and tells the inner instance to keep the window open when it
// has an error to show.
bool linux_terminal_relaunch(int argc, char** argv, char* err, size_t errSize);

// True when this process has no controlling terminal but a display server is
// reachable — i.e. it was double-clicked rather than run from a shell.
bool linux_terminal_relaunch_wanted(bool alreadyRelaunched);

#endif
