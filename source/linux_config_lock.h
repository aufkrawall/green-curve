// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#ifndef GREEN_CURVE_LINUX_CONFIG_LOCK_H
#define GREEN_CURVE_LINUX_CONFIG_LOCK_H
#include "linux_port_internal.h"
#include <errno.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

// Stable side lock survives atomic replacement. All local config writers hold
// it across their entire read-modify-write transaction, including bank import.
class LinuxConfigLock {
    int fd_ = -1;
public:
    LinuxConfigLock(const char* path, char* why, size_t whySize) {
        const auto fail = [&] { set_message(why, whySize, "Could not acquire profile storage lock"); };
        if (!path || !*path || !ensure_directory_recursive(path_dirname(path).c_str(), why, whySize)) { fail(); return; }
        const std::string lockPath = std::string(path) + ".lock";
        const int fd = open(lockPath.c_str(), O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK, 0600);
        if (fd < 0) { fail(); return; }
        struct stat status{};
        if (fstat(fd, &status) != 0 || !S_ISREG(status.st_mode) || status.st_nlink != 1 ||
            (geteuid() != 0 && status.st_uid != geteuid())) {
            close(fd); fail(); return;
        }
        int result;
        do { result = flock(fd, LOCK_EX); } while (result != 0 && errno == EINTR);
        if (result != 0) { close(fd); fail(); return; }
        fd_ = fd;
    }
    ~LinuxConfigLock() { if (fd_ >= 0) { (void)flock(fd_, LOCK_UN); close(fd_); } }
    bool locked() const { return fd_ >= 0; }
    LinuxConfigLock(const LinuxConfigLock&) = delete;
    LinuxConfigLock& operator=(const LinuxConfigLock&) = delete;
};
#endif
