// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
// Filesystem-path authorization for a bound Unix-domain socket.

#ifndef GREEN_CURVE_LINUX_SOCKET_PATH_PERMISSIONS_H
#define GREEN_CURVE_LINUX_SOCKET_PATH_PERMISSIONS_H

#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

static inline bool linux_verify_socket_path_permissions_at(
    int directoryFd, const char* socketName, uid_t expectedOwner,
    gid_t expectedGroup, mode_t expectedMode,
    char* error, size_t errorSize) {
    if (error && errorSize) error[0] = 0;
    if (directoryFd < 0 || !socketName || !socketName[0]) {
        if (error && errorSize)
            snprintf(error, errorSize, "socket pathname verification arguments are invalid");
        return false;
    }

    struct stat status = {};
    if (fstatat(directoryFd, socketName, &status,
                AT_SYMLINK_NOFOLLOW) != 0) {
        if (error && errorSize)
            snprintf(error, errorSize, "cannot inspect socket pathname %s: %s",
                     socketName, strerror(errno));
        return false;
    }
    if (!S_ISSOCK(status.st_mode)) {
        if (error && errorSize)
            snprintf(error, errorSize,
                     "socket pathname %s is not a Unix socket (type=%06o)",
                     socketName, (unsigned int)(status.st_mode & S_IFMT));
        return false;
    }
    mode_t actualMode = status.st_mode & 0777;
    expectedMode &= 0777;
    if (status.st_uid != expectedOwner || status.st_gid != expectedGroup ||
        actualMode != expectedMode) {
        if (error && errorSize)
            snprintf(error, errorSize,
                     "socket pathname %s has uid=%lu gid=%lu mode=%04o; "
                     "expected uid=%lu gid=%lu mode=%04o",
                     socketName, (unsigned long)status.st_uid,
                     (unsigned long)status.st_gid, (unsigned int)actualMode,
                     (unsigned long)expectedOwner,
                     (unsigned long)expectedGroup,
                     (unsigned int)expectedMode);
        return false;
    }
    return true;
}

static inline bool linux_configure_socket_path_permissions_at(
    int directoryFd, const char* socketName, uid_t expectedOwner,
    gid_t expectedGroup, mode_t expectedMode,
    char* error, size_t errorSize) {
    if (error && errorSize) error[0] = 0;

    // The caller first proves that directoryFd names a root-owned 0755
    // directory. Unprivileged processes therefore cannot replace this entry
    // between the no-follow type check and fchmodat(). The descriptor returned
    // by socket() is a different sockfs inode and cannot authorize clients of
    // the filesystem pathname created by bind().
    struct stat before = {};
    if (directoryFd < 0 || !socketName || !socketName[0]) {
        if (error && errorSize)
            snprintf(error, errorSize,
                     "socket pathname configuration arguments are invalid");
        return false;
    }
    if (fstatat(directoryFd, socketName, &before,
                AT_SYMLINK_NOFOLLOW) != 0) {
        if (error && errorSize)
            snprintf(error, errorSize, "cannot inspect bound socket pathname %s: %s",
                     socketName, strerror(errno));
        return false;
    }
    if (!S_ISSOCK(before.st_mode)) {
        if (error && errorSize)
            snprintf(error, errorSize,
                     "refusing ownership change because %s is not a Unix socket",
                     socketName);
        return false;
    }
    if (fchownat(directoryFd, socketName, expectedOwner, expectedGroup,
                 AT_SYMLINK_NOFOLLOW) != 0) {
        if (error && errorSize)
            snprintf(error, errorSize,
                     "cannot set socket pathname %s ownership: %s",
                     socketName, strerror(errno));
        return false;
    }
    if (fchmodat(directoryFd, socketName, expectedMode & 0777, 0) != 0) {
        if (error && errorSize)
            snprintf(error, errorSize,
                     "cannot set socket pathname %s mode: %s",
                     socketName, strerror(errno));
        return false;
    }
    return linux_verify_socket_path_permissions_at(
        directoryFd, socketName, expectedOwner, expectedGroup, expectedMode,
        error, errorSize);
}

#endif // GREEN_CURVE_LINUX_SOCKET_PATH_PERMISSIONS_H
