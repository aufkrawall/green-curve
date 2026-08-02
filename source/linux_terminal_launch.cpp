// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// linux_terminal_launch.cpp — see linux_terminal_launch.h.

#include "linux_terminal_launch.h"

#include "linux_debug_log.h"
#include "linux_terminal_policy.h"
#include "platform.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace {

// Maximum argv we will build: terminal + 2 style words + exe + forwarded args +
// the guard + the NUL terminator.  A caller cannot exceed it in practice, and
// exceeding it is reported rather than truncated.
const int kMaxRelaunchArgs = 64;

bool path_is_executable_file(const char* path) {
    struct stat status = {};
    // flawfinder: ignore -- read-only probe of a fixed terminal command name.
    if (access(path, X_OK) != 0) return false;
    if (stat(path, &status) != 0) return false;
    return S_ISREG(status.st_mode);
}

bool probe_command(const char* command, void* context) {
    (void)context;
    return linux_command_available(command);
}

}  // namespace

bool linux_command_available(const char* name) {
    if (!name || !name[0]) return false;
    if (strchr(name, '/')) return path_is_executable_file(name);
    const char* pathList = getenv("PATH");
    if (!pathList || !pathList[0]) pathList = "/usr/local/bin:/usr/bin:/bin";
    const char* cursor = pathList;
    while (*cursor) {
        const char* end = cursor;
        while (*end && *end != ':') ++end;
        char candidate[4096] = {};
        size_t directoryLength = (size_t)(end - cursor);
        if (directoryLength == 0) {
            gc_strlcpy(candidate, sizeof(candidate), ".");
        } else if (directoryLength < sizeof(candidate)) {
            memcpy(candidate, cursor, directoryLength);
            candidate[directoryLength] = 0;
        } else {
            cursor = *end ? end + 1 : end;
            continue;
        }
        gc_strlcat(candidate, sizeof(candidate), "/");
        gc_strlcat(candidate, sizeof(candidate), name);
        if (path_is_executable_file(candidate)) return true;
        cursor = *end ? end + 1 : end;
    }
    return false;
}

bool linux_terminal_relaunch_wanted(bool alreadyRelaunched) {
    return linux_terminal_should_relaunch(
        isatty(STDIN_FILENO) != 0, isatty(STDOUT_FILENO) != 0,
        alreadyRelaunched, getenv("WAYLAND_DISPLAY"), getenv("DISPLAY"));
}

bool linux_terminal_relaunch(int argc, char** argv, char* err, size_t errSize) {
    if (err && errSize) err[0] = 0;

    char selfPath[4096] = {};
    // flawfinder: ignore -- kernel-owned /proc/self/exe, bounded and terminated.
    ssize_t linkLength = readlink("/proc/self/exe", selfPath, sizeof(selfPath) - 1);
    if (linkLength <= 0) {
        gc_strlcpy(err, errSize, "cannot resolve /proc/self/exe");
        return false;
    }
    selfPath[linkLength] = 0;

    const char* currentDesktop = getenv("XDG_CURRENT_DESKTOP");
    LinuxTerminalChoice choice =
        linux_terminal_select(currentDesktop, probe_command, nullptr);
    if (!choice.found) {
        gc_snprintf(err, errSize,
            "no supported terminal emulator found on PATH (desktop=%s); "
            "run '%s --tui' from a terminal",
            currentDesktop && currentDesktop[0] ? currentDesktop : "unknown",
            selfPath);
        return false;
    }

    const char* stylePrefix[2] = {nullptr, nullptr};
    unsigned int prefixCount = linux_terminal_style_prefix(choice.style, stylePrefix);

    char* relaunchArgs[kMaxRelaunchArgs] = {};
    int count = 0;
    relaunchArgs[count++] = (char*)choice.command;
    for (unsigned int i = 0; i < prefixCount; ++i)
        relaunchArgs[count++] = (char*)stylePrefix[i];
    relaunchArgs[count++] = selfPath;
    for (int i = 1; i < argc; ++i) {
        if (count >= kMaxRelaunchArgs - 2) {
            gc_strlcpy(err, errSize,
                "too many arguments to relaunch inside a terminal");
            return false;
        }
        relaunchArgs[count++] = argv[i];
    }
    relaunchArgs[count++] = (char*)"--from-desktop";
    relaunchArgs[count] = nullptr;

    linux_debug_logf("terminal relaunch: desktop=%s terminal=%s style=%d argc=%d",
                     currentDesktop && currentDesktop[0] ? currentDesktop : "unknown",
                     choice.command, choice.style, count);

    // execvp, not fork: the file manager already treats us as the launched
    // process, so replacing the image keeps exactly one process in the tree and
    // no orphan to reap.  A fixed argv is passed — never a shell string — so
    // paths with spaces or quotes cannot be re-parsed.
    // flawfinder: ignore -- argv is fixed; the command comes from the vetted
    // terminal table, not from user input.
    execvp(choice.command, relaunchArgs);

    gc_snprintf(err, errSize, "cannot start terminal %s: %s",
                choice.command, strerror(errno));
    linux_debug_logf("terminal relaunch failed: %s", err ? err : "");
    return false;
}
