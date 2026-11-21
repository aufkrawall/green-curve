// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// Pure path rules for the Linux CLI/TUI atomic text writer
// (write_text_file_atomic in linux_port.cpp).  Host-neutral so the regression
// harness asserts them on every build host, not only in Linux CI.
#pragma once

#include <errno.h>
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

// Whether root's output walk (linux_open_output_directory) may follow a
// symlinked directory component.  Only a link root itself placed: owned by
// root, inside a directory only root can modify, so nobody else can create or
// swap it between the check and the open.  Distribution layouts such as
// /home -> var/home and /root -> var/roothome keep working, while a link
// another account controls (or one in a shared, writable directory such as
// /tmp) is still refused.
static inline bool linux_output_directory_may_follow_link(
    unsigned int linkUid, unsigned int parentUid, unsigned int parentMode) {
    return linkUid == 0 && parentUid == 0 && (parentMode & 0022u) == 0;
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

// How a create-only publish settled.  Logged, never branched on by callers.
enum LinuxAtomicPublishMethod {
    LINUX_ATOMIC_PUBLISH_NONE = 0,
    LINUX_ATOMIC_PUBLISH_RENAME_NOREPLACE,   // renameat2(RENAME_NOREPLACE)
    LINUX_ATOMIC_PUBLISH_LINK,               // linkat: the temporary still exists
};

// Whether a failed renameat2(RENAME_NOREPLACE) means "this filesystem or
// kernel cannot do it" rather than "the destination exists" or a real I/O
// error.  NFS, some FUSE and FAT-family mounts answer EINVAL (flag unknown);
// an old kernel or libc answers ENOSYS.
static inline bool linux_atomic_write_noreplace_unsupported(int error) {
    if (error == EINVAL || error == ENOSYS) return true;
#ifdef EOPNOTSUPP
    if (error == EOPNOTSUPP) return true;
#endif
#ifdef ENOTSUP
    if (error == ENOTSUP) return true;
#endif
    return false;
}

// Publish a complete temporary without replacing any destination. If both
// atomic primitives are unavailable, leave the destination alone.
template <class RenameNoReplace, class LinkNoReplace>
static inline int linux_atomic_write_publish_new(RenameNoReplace renameNoReplace,
    LinkNoReplace linkNoReplace, LinuxAtomicPublishMethod* method) {
    if (method) *method = LINUX_ATOMIC_PUBLISH_NONE;
    const int renameError = renameNoReplace();
    if (renameError == 0) {
        if (method) *method = LINUX_ATOMIC_PUBLISH_RENAME_NOREPLACE;
        return 0;
    }
    if (!linux_atomic_write_noreplace_unsupported(renameError)) return renameError;
    const int linkError = linkNoReplace();
    if (linkError == 0 && method) *method = LINUX_ATOMIC_PUBLISH_LINK;
    return linkError;
}
