// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
// Pure path resolution policy for default Linux client configuration paths.

#ifndef GREEN_CURVE_LINUX_CONFIG_PATH_POLICY_H
#define GREEN_CURVE_LINUX_CONFIG_PATH_POLICY_H

#include <stddef.h>
#include <string.h>

#ifndef CONFIG_FILE_NAME
#define CONFIG_FILE_NAME "config.ini"
#endif

// Decides whether a binary directory is a root-owned system installation tree
// where ordinary users cannot write configuration files or logs.
static inline bool linux_is_system_binary_dir(const char* dir) {
    if (!dir || !dir[0]) return false;
    if (strncmp(dir, "/usr", 4) == 0 && (dir[4] == '/' || dir[4] == '\0')) return true;
    if (strncmp(dir, "/bin", 4) == 0 && (dir[4] == '/' || dir[4] == '\0')) return true;
    if (strncmp(dir, "/sbin", 5) == 0 && (dir[5] == '/' || dir[5] == '\0')) return true;
    if (strncmp(dir, "/opt", 4) == 0 && (dir[4] == '/' || dir[4] == '\0')) return true;
    return false;
}

// Pure decision for the default client config.ini path on Linux.
// Returns true when a bounded, NUL-terminated path was placed into dst.
static inline bool linux_resolve_default_config_path(
    const char* exeDir,
    bool exeDirHasConfig,
    const char* xdgConfigHome,
    const char* homeDir,
    char* dst,
    size_t dstSize) {
    if (!dst || dstSize == 0) return false;
    dst[0] = '\0';

    // 1. If an existing config.ini is present in a non-system directory beside
    //    the executable, retain portable mode.
    if (exeDir && exeDir[0] && exeDirHasConfig && !linux_is_system_binary_dir(exeDir)) {
        size_t exeLen = strlen(exeDir);
        bool needsSlash = (exeLen > 0 && exeDir[exeLen - 1] != '/');
        size_t needed = exeLen + (needsSlash ? 1 : 0) + strlen(CONFIG_FILE_NAME) + 1;
        if (needed <= dstSize) {
            size_t pos = 0;
            memcpy(dst + pos, exeDir, exeLen); pos += exeLen;
            if (needsSlash) dst[pos++] = '/';
            memcpy(dst + pos, CONFIG_FILE_NAME, strlen(CONFIG_FILE_NAME)); pos += strlen(CONFIG_FILE_NAME);
            dst[pos] = '\0';
            return true;
        }
    }

    // 2. Standard XDG user configuration: $XDG_CONFIG_HOME/greencurve/config.ini
    if (xdgConfigHome && xdgConfigHome[0] == '/') {
        size_t xdgLen = strlen(xdgConfigHome);
        bool needsSlash = (xdgLen > 0 && xdgConfigHome[xdgLen - 1] != '/');
        const char* suffix = "greencurve/" CONFIG_FILE_NAME;
        size_t needed = xdgLen + (needsSlash ? 1 : 0) + strlen(suffix) + 1;
        if (needed <= dstSize) {
            size_t pos = 0;
            memcpy(dst + pos, xdgConfigHome, xdgLen); pos += xdgLen;
            if (needsSlash) dst[pos++] = '/';
            memcpy(dst + pos, suffix, strlen(suffix)); pos += strlen(suffix);
            dst[pos] = '\0';
            return true;
        }
    }

    // 3. Fallback XDG user configuration: $HOME/.config/greencurve/config.ini
    if (homeDir && homeDir[0] == '/') {
        size_t homeLen = strlen(homeDir);
        bool needsSlash = (homeLen > 0 && homeDir[homeLen - 1] != '/');
        const char* suffix = ".config/greencurve/" CONFIG_FILE_NAME;
        size_t needed = homeLen + (needsSlash ? 1 : 0) + strlen(suffix) + 1;
        if (needed <= dstSize) {
            size_t pos = 0;
            memcpy(dst + pos, homeDir, homeLen); pos += homeLen;
            if (needsSlash) dst[pos++] = '/';
            memcpy(dst + pos, suffix, strlen(suffix)); pos += strlen(suffix);
            dst[pos] = '\0';
            return true;
        }
    }

    // 4. Last resort: beside the executable.
    if (exeDir && exeDir[0]) {
        size_t exeLen = strlen(exeDir);
        bool needsSlash = (exeLen > 0 && exeDir[exeLen - 1] != '/');
        size_t needed = exeLen + (needsSlash ? 1 : 0) + strlen(CONFIG_FILE_NAME) + 1;
        if (needed <= dstSize) {
            size_t pos = 0;
            memcpy(dst + pos, exeDir, exeLen); pos += exeLen;
            if (needsSlash) dst[pos++] = '/';
            memcpy(dst + pos, CONFIG_FILE_NAME, strlen(CONFIG_FILE_NAME)); pos += strlen(CONFIG_FILE_NAME);
            dst[pos] = '\0';
            return true;
        }
    }

    return false;
}

#endif // GREEN_CURVE_LINUX_CONFIG_PATH_POLICY_H
