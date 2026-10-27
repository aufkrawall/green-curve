// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#ifndef GREEN_CURVE_LINUX_OUTPUT_DIRECTORY_H
#define GREEN_CURVE_LINUX_OUTPUT_DIRECTORY_H

#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <string>

#include "linux_atomic_write_policy.h"

// Read only a link protected by its root-owned, non-writable parent. Resolve
// the returned target component by component: following it with openat would
// let a second, untrusted link bypass this check.
static inline bool linux_read_trusted_directory_link(int directory,
                                                      const char* part,
                                                      std::string* target) {
    struct stat link = {}, parent = {};
    if (fstatat(directory, part, &link, AT_SYMLINK_NOFOLLOW) != 0 ||
        !S_ISLNK(link.st_mode) || fstat(directory, &parent) != 0 ||
        !linux_output_directory_may_follow_link((unsigned int)link.st_uid,
            (unsigned int)parent.st_uid, (unsigned int)parent.st_mode)) return false;
    char buffer[4097];
    const ssize_t size = readlinkat(directory, part, buffer, sizeof(buffer));
    if (size <= 0 || (size_t)size >= sizeof(buffer)) { errno = ELOOP; return false; }
    target->assign(buffer, (size_t)size);
    return true;
}

// Each component is opened relative to the directory already held. Directory
// creation uses that same handle; no privileged path lookup follows a link
// anyone but root could have placed. Ordinary users retain their redirected
// configuration-directory support.
static inline int linux_open_output_directory(const char* path, bool privileged) {
    if (!path || !*path) { errno = EINVAL; return -1; }
    int directory = open(path[0] == '/' ? "/" : ".",
                         O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (directory < 0) return -1;
    std::string remaining(path);
    // Reject user-supplied parent traversal; trusted link targets may contain
    // it and are walked under the same no-follow rules as every other part.
    if (privileged) {
        size_t pos = 0;
        while (pos < remaining.size()) {
            const size_t end = remaining.find('/', pos);
            if (remaining.substr(pos, end - pos) == "..") {
                close(directory); errno = EINVAL; return -1;
            }
            if (end == std::string::npos) break;
            pos = end + 1;
        }
    }
    size_t start = path[0] == '/' ? 1u : 0u;
    unsigned int links = 0;
    while (start < remaining.size()) {
        size_t end = remaining.find('/', start);
        std::string part = remaining.substr(start,
            end == std::string::npos ? std::string::npos : end - start);
        if (!part.empty() && part != ".") {
            const int flags = O_RDONLY | O_DIRECTORY | O_CLOEXEC |
                              (privileged ? O_NOFOLLOW : 0);
            int next = openat(directory, part.c_str(), flags);
            if (next < 0 && privileged && (errno == ELOOP || errno == ENOTDIR)) {
                const int refused = errno;
                std::string target;
                if (linux_read_trusted_directory_link(directory, part.c_str(), &target)) {
                    if (++links > 40) { close(directory); errno = ELOOP; return -1; }
                    if (target[0] == '/') {
                        const int root = open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
                        if (root < 0) { const int error = errno; close(directory); errno = error; return -1; }
                        close(directory);
                        directory = root;
                    }
                    remaining = target + (end == std::string::npos ? "" : remaining.substr(end));
                    start = 0;
                    continue;
                }
                errno = refused;
            }
            if (next < 0 && errno == ENOENT) {
                if (mkdirat(directory, part.c_str(), 0700) == 0 || errno == EEXIST)
                    next = openat(directory, part.c_str(), flags);
            }
            if (next < 0) {
                const int error = errno;
                close(directory); errno = error; return -1;
            }
            close(directory);
            directory = next;
        }
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return directory;
}

#endif
