// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// Pure path rules for the Linux CLI/TUI atomic text writer
// (write_text_file_atomic in linux_port.cpp).  Host-neutral so the regression
// harness asserts them on every build host, not only in Linux CI.
#pragma once

#include <stdio.h>
#include <string>

// Split an output path into the directory that is pinned with a dirfd and the
// leaf that is atomically replaced inside it.
//
// A bare file name is valid and lives in the current directory: that is how
// `greencurve --probe-output report.md` and `--config my.ini` are spelled, and
// the writer used to accept it.  A path whose leaf is empty, "." or ".." names
// a directory, not a file, and is refused.
static inline bool linux_atomic_write_split(const std::string& path,
                                            std::string* dir, std::string* name) {
    if (!dir || !name || path.empty()) return false;
    size_t slash = path.find_last_of('/');
    if (slash == std::string::npos) {
        *dir = ".";
        *name = path;
    } else {
        *dir = slash == 0 ? std::string("/") : path.substr(0, slash);
        *name = path.substr(slash + 1);
    }
    return !name->empty() && *name != "." && *name != "..";
}

// Hidden temporary beside the leaf.  The leaf may be up to NAME_MAX (255)
// bytes, so only a bounded prefix of it is reused: ".", 200 bytes, ".tmp." and
// 16 hex digits stay inside NAME_MAX for every valid leaf.
enum { LINUX_ATOMIC_WRITE_TEMP_PREFIX_MAX = 200 };

static inline bool linux_atomic_write_temp_name(const std::string& name,
    unsigned long long suffix, char* out, size_t outSize) {
    if (!out || outSize == 0) return false;
    int prefix = (int)(name.size() < (size_t)LINUX_ATOMIC_WRITE_TEMP_PREFIX_MAX
        ? name.size() : (size_t)LINUX_ATOMIC_WRITE_TEMP_PREFIX_MAX);
    int written = snprintf(out, outSize, ".%.*s.tmp.%016llx", prefix,
                           name.c_str(), suffix);
    return written > 0 && (size_t)written < outSize;
}
