// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT

static bool configure_daemon_socket_permissions(int socketFd) {
    struct group* group = getgrnam("greencurve");
    gid_t expectedGroup = group ? group->gr_gid : 0;
    mode_t expectedMode = group ? 0660 : 0600;
    // Operate on the bound socket descriptor itself. Path-based chmod would
    // follow a replacement symlink and reopen a race between validation and
    // use, even though the runtime directory is intended to be root-owned.
    if (fchown(socketFd, 0, expectedGroup) != 0) {
        dlog("daemon: refusing socket after ownership update failed: %s\n",
            strerror(errno));
        return false;
    }
    if (fchmod(socketFd, expectedMode) != 0) {
        dlog("daemon: refusing socket after mode update failed: %s\n",
            strerror(errno));
        return false;
    }
    struct stat status = {};
    if (fstat(socketFd, &status) != 0 || !S_ISSOCK(status.st_mode) ||
        status.st_uid != 0 || status.st_gid != expectedGroup ||
        (status.st_mode & 0777) != expectedMode) {
        dlog("daemon: refusing socket because verified ownership/mode differs (uid=%lu gid=%lu mode=%04o expectedGid=%lu expectedMode=%04o)\n",
            (unsigned long)status.st_uid, (unsigned long)status.st_gid,
            (unsigned int)(status.st_mode & 0777),
            (unsigned long)expectedGroup, (unsigned int)expectedMode);
        return false;
    }
    dlog("daemon: socket authorization verified root:%s mode=%04o\n",
        group ? "greencurve" : "root", (unsigned int)expectedMode);
    return true;
}
