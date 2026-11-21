// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#pragma once

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <string>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

// Staged siblings and backups stay on the destination filesystem. Every
// live replacement and restoration is one rename.
class LinuxInstallFile {
public:
    explicit LinuxInstallFile(const char* path) : path_(path) {}
    LinuxInstallFile(const LinuxInstallFile&) = delete;
    LinuxInstallFile& operator=(const LinuxInstallFile&) = delete;
    ~LinuxInstallFile() {
        if (!staged_.empty()) unlink(staged_.c_str());
        // A failed restoration leaves its backup available for recovery.
        if (!installed_) discard_backup();
    }

    int stage(mode_t mode) {
        std::string name = path_ + ".stage.XXXXXX";
        int fd = mkostemp(name.data(), O_CLOEXEC);
        if (fd < 0) return -1;
        staged_ = name;
        if (fchmod(fd, mode) == 0) return fd;
        const int error = errno;
        close(fd);
        errno = error;
        return -1;
    }

    bool save_previous() {
        struct stat file = {};
        if (lstat(path_.c_str(), &file) != 0) return errno == ENOENT;
        if (!S_ISREG(file.st_mode) || file.st_uid != geteuid() || (file.st_mode & 0022)) {
            errno = EPERM;
            return false;
        }
        std::string backup = staged_ + ".previous";
        if (link(path_.c_str(), backup.c_str()) != 0) return false;
        backup_ = backup;
        return sync_parent();
    }

    bool publish() {
        if (rename(staged_.c_str(), path_.c_str()) != 0) return false;
        staged_.clear();
        installed_ = true;
        return sync_parent();
    }

    bool restore() {
        if (!installed_) return true;
        const bool restored = backup_.empty()
            ? unlink(path_.c_str()) == 0
            : rename(backup_.c_str(), path_.c_str()) == 0;
        if (!restored) return false;
        backup_.clear();
        installed_ = false;
        return sync_parent();
    }

    bool commit() { return discard_backup(); }
    const char* path() const { return path_.c_str(); }
    const char* backup() const { return backup_.c_str(); }

private:
    bool sync_parent() const {
        const std::string directory = path_.substr(0, path_.find_last_of('/'));
        int fd = open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        if (fd < 0) return false;
        const int synced = fsync(fd);
        const int error = errno;
        close(fd);
        errno = error;
        return synced == 0;
    }

    bool discard_backup() {
        if (backup_.empty()) return true;
        if (unlink(backup_.c_str()) != 0) return false;
        backup_.clear();
        return sync_parent();
    }

    std::string path_;
    std::string staged_;
    std::string backup_;
    bool installed_ = false;
};

class LinuxServiceInstallLock {
public:
    explicit LinuxServiceInstallLock(const char* directory)
        : fd_(open(directory, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW)) {
        if (fd_ < 0 || flock(fd_, LOCK_EX | LOCK_NB) == 0) return;
        const int error = errno;
        close(fd_);
        fd_ = -1;
        errno = error;
    }
    LinuxServiceInstallLock(const LinuxServiceInstallLock&) = delete;
    LinuxServiceInstallLock& operator=(const LinuxServiceInstallLock&) = delete;
    ~LinuxServiceInstallLock() { if (fd_ >= 0) close(fd_); }
    bool locked() const { return fd_ >= 0; }

private:
    int fd_;
};
