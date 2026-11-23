// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// Pure CLI output-path composition helpers.
//
// Windows file-write IPC commands (SERVICE_CMD_WRITE_PROBE_REPORT, etc.) are
// validated inside the background service against the caller's profile
// directory via GetFullPathNameW. A bare relative filename sent across IPC is
// resolved against the service's own working directory (System32) and always
// refused as outside the caller's profile directory. Building the default
// --probe report path inside userDataDir (matching --dump) and canonicalizing
// user-supplied relative paths in the client before IPC closes that gap.
#ifndef GREEN_CURVE_CLI_OUTPUT_PATH_POLICY_H
#define GREEN_CURVE_CLI_OUTPUT_PATH_POLICY_H

#include <stddef.h>
#include <stdio.h>
#include <string.h>

static inline bool cli_format_default_probe_path(
    const char* userDataDir,
    unsigned int year, unsigned int month, unsigned int day,
    unsigned int hour, unsigned int minute, unsigned int second,
    char* outPath, size_t outSize) {
    if (!outPath || outSize == 0) return false;
    outPath[0] = 0;
    const bool hasDir = userDataDir && userDataDir[0] != '\0';
    size_t dirLen = hasDir ? strlen(userDataDir) : 0;
    const bool needsSep = hasDir &&
        userDataDir[dirLen - 1] != '\\' && userDataDir[dirLen - 1] != '/';
    int written = snprintf(outPath, outSize,
        "%s%sgreencurve_probe_%04u%02u%02u_%02u%02u%02u.json",
        hasDir ? userDataDir : "",
        needsSep ? "\\" : "",
        year, month, day, hour, minute, second);
    if (written < 0 || (size_t)written >= outSize) {
        outPath[0] = 0;
        return false;
    }
    return true;
}

static inline bool cli_select_probe_output_path(
    bool hasExplicitPath, const char* explicitPath,
    const char* userDataDir,
    unsigned int year, unsigned int month, unsigned int day,
    unsigned int hour, unsigned int minute, unsigned int second,
    char* outPath, size_t outSize) {
    if (!outPath || outSize == 0) return false;
    outPath[0] = 0;
    if (hasExplicitPath) {
        if (!explicitPath || !explicitPath[0]) return false;
        int written = snprintf(outPath, outSize, "%s", explicitPath);
        if (written < 0 || (size_t)written >= outSize) {
            outPath[0] = 0;
            return false;
        }
        return true;
    }
    return cli_format_default_probe_path(
        userDataDir, year, month, day, hour, minute, second, outPath, outSize);
}

#endif  // GREEN_CURVE_CLI_OUTPUT_PATH_POLICY_H
