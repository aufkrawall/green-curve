// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT

#include "linux_socket_path_permissions.h"

static bool configure_daemon_socket_permissions(int socketDirectoryFd) {
    struct group* group = getgrnam("greencurve");
    gid_t expectedGroup = group ? group->gr_gid : 0;
    mode_t expectedMode = group ? 0660 : 0600;
    char error[256] = {};
    if (!linux_configure_socket_path_permissions_at(
            socketDirectoryFd, GC_DAEMON_SOCKET_NAME, 0, expectedGroup,
            expectedMode, error, sizeof(error))) {
        dlog("daemon: refusing socket after pathname authorization failed: %s\n",
             error[0] ? error : "unknown socket pathname failure");
        return false;
    }
    dlog("daemon: socket pathname authorization verified root:%s mode=%04o\n",
        group ? "greencurve" : "root", (unsigned int)expectedMode);
    return true;
}
