// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#ifndef GREEN_CURVE_LINUX_OUTPUT_DIRECTORY_H
#define GREEN_CURVE_LINUX_OUTPUT_DIRECTORY_H

#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <string>

// Each component is opened relative to the directory already held. Directory
// creation uses that same handle; no privileged path lookup follows a link.
// Ordinary users retain their redirected configuration-directory support.
static inline int linux_open_output_directory(const char* path, bool privileged) {
    if (!path || !*path) { errno = EINVAL; return -1; }
    int directory = open(path[0] == '/' ? "/" : ".",
                         O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (directory < 0) return -1;
    std::string remaining(path);
    size_t start = path[0] == '/' ? 1u : 0u;
    while (start < remaining.size()) {
        size_t end = remaining.find('/', start);
        std::string part = remaining.substr(start,
            end == std::string::npos ? std::string::npos : end - start);
        if (!part.empty() && part != ".") {
            if (privileged && part == "..") {
                close(directory); errno = EINVAL; return -1;
            }
            const int flags = O_RDONLY | O_DIRECTORY | O_CLOEXEC |
                              (privileged ? O_NOFOLLOW : 0);
            int next = openat(directory, part.c_str(), flags);
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
