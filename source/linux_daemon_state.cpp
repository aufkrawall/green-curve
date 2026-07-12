// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT

#include "linux_daemon_state.h"
#include "platform.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static bool write_all(int fd, const void* data, size_t size) {
    const unsigned char* bytes = (const unsigned char*)data;
    size_t written = 0;
    while (written < size) {
        ssize_t count = write(fd, bytes + written, size - written);
        if (count > 0) { written += (size_t)count; continue; }
        if (count < 0 && errno == EINTR) continue;
        return false;
    }
    return true;
}

static bool split_path(const char* path, char* directory, size_t directorySize,
                       char* name, size_t nameSize) {
    if (!path || !*path) return false;
    const char* slash = strrchr(path, '/');
    if (!slash || slash == path || !slash[1]) return false;
    size_t dirLength = (size_t)(slash - path);
    if (dirLength >= directorySize || strlen(slash + 1) >= nameSize) return false;
    memcpy(directory, path, dirLength);
    directory[dirLength] = 0;
    gc_strlcpy(name, nameSize, slash + 1);
    return true;
}

static int open_state_directory(const char* path, char* name, size_t nameSize,
                                char* err, size_t errSize) {
    char directory[4096] = {};
    if (!split_path(path, directory, sizeof(directory), name, nameSize)) {
        gc_snprintf(err, errSize, "invalid daemon state path: %s", path ? path : "<null>");
        return -1;
    }
    if (mkdir(directory, 0755) != 0 && errno != EEXIST) {
        gc_snprintf(err, errSize, "cannot create state directory %s: %s", directory, strerror(errno));
        return -1;
    }
    int dirfd = open(directory, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (dirfd < 0) {
        gc_snprintf(err, errSize, "cannot open state directory %s: %s", directory, strerror(errno));
        return -1;
    }
    struct stat st = {};
    if (fstat(dirfd, &st) != 0 || !S_ISDIR(st.st_mode) || st.st_uid != 0 ||
        (st.st_mode & 0022) != 0) {
        gc_snprintf(err, errSize, "state directory %s is not root-owned and protected", directory);
        close(dirfd);
        return -1;
    }
    return dirfd;
}

LinuxDaemonStateLoadResult linux_daemon_state_load(const char* path,
                                                   LinuxDaemonStateRecord* out,
                                                   char* err, size_t errSize) {
    if (err && errSize) err[0] = 0;
    if (out) memset(out, 0, sizeof(*out));
    char name[256] = {};
    int dirfd = open_state_directory(path, name, sizeof(name), err, errSize);
    if (dirfd < 0) return LINUX_DAEMON_STATE_IO_ERROR;
    int fd = openat(dirfd, name, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) {
        int saved = errno;
        close(dirfd);
        if (saved == ENOENT) return LINUX_DAEMON_STATE_MISSING;
        gc_snprintf(err, errSize, "cannot open daemon state: %s", strerror(saved));
        return LINUX_DAEMON_STATE_IO_ERROR;
    }
    struct stat st = {};
    bool regular = fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_uid == 0 &&
                   st.st_nlink == 1 && (st.st_mode & 0077) == 0 &&
                   st.st_size == (off_t)sizeof(LinuxDaemonStateRecord);
    LinuxDaemonStateRecord record = {};
    ssize_t count = regular ? read(fd, &record, sizeof(record)) : -1;
    close(fd);
    LinuxDaemonStateLoadResult result = LINUX_DAEMON_STATE_LOADED;
    if (!regular || count != (ssize_t)sizeof(record) || !linux_daemon_record_valid(&record)) {
        result = (S_ISREG(st.st_mode) && st.st_size == (off_t)sizeof(DesiredSettings))
            ? LINUX_DAEMON_STATE_LEGACY_REMOVED : LINUX_DAEMON_STATE_INVALID_REMOVED;
        if (unlinkat(dirfd, name, 0) != 0 && errno != ENOENT) {
            gc_snprintf(err, errSize, "cannot remove invalid daemon state: %s", strerror(errno));
            result = LINUX_DAEMON_STATE_IO_ERROR;
        } else {
            fsync(dirfd);
        }
    } else if (out) {
        *out = record;
    }
    close(dirfd);
    return result;
}

bool linux_daemon_state_store(const char* path, const LinuxDaemonStateRecord* record,
                              char* err, size_t errSize) {
    if (err && errSize) err[0] = 0;
    if (!linux_daemon_record_valid(record)) {
        gc_strlcpy(err, errSize, "refusing invalid daemon state record");
        return false;
    }
    char name[256] = {};
    int dirfd = open_state_directory(path, name, sizeof(name), err, errSize);
    if (dirfd < 0) return false;
    char temp[320] = {};
    bool stored = false;
    for (unsigned int attempt = 0; attempt < 32 && !stored; ++attempt) {
        gc_snprintf(temp, sizeof(temp), ".%s.tmp.%ld.%u", name, (long)getpid(), attempt);
        int fd = openat(dirfd, temp, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
        if (fd < 0) {
            if (errno == EEXIST) continue;
            gc_snprintf(err, errSize, "cannot create daemon state temp: %s", strerror(errno));
            break;
        }
        bool ok = fchmod(fd, 0600) == 0 && fchown(fd, 0, 0) == 0 &&
                  write_all(fd, record, sizeof(*record)) && fsync(fd) == 0;
        if (close(fd) != 0) ok = false;
        if (ok && renameat(dirfd, temp, dirfd, name) == 0 && fsync(dirfd) == 0) {
            stored = true;
        } else {
            if (err && errSize && !err[0])
                gc_snprintf(err, errSize, "cannot commit daemon state: %s", strerror(errno));
            unlinkat(dirfd, temp, 0);
        }
    }
    close(dirfd);
    return stored;
}

bool linux_daemon_state_remove(const char* path, char* err, size_t errSize) {
    if (err && errSize) err[0] = 0;
    char name[256] = {};
    int dirfd = open_state_directory(path, name, sizeof(name), err, errSize);
    if (dirfd < 0) return false;
    bool ok = unlinkat(dirfd, name, 0) == 0 || errno == ENOENT;
    if (ok) ok = fsync(dirfd) == 0;
    if (!ok) gc_snprintf(err, errSize, "cannot remove daemon state: %s", strerror(errno));
    close(dirfd);
    return ok;
}
