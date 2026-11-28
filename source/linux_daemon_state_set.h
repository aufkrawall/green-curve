// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
// Included by linux_daemon_state.h after LinuxDaemonStateLoadResult.
#ifndef GREEN_CURVE_LINUX_DAEMON_STATE_SET_H
#define GREEN_CURVE_LINUX_DAEMON_STATE_SET_H

// A daemon that runs several GPUs keeps one current-layout record per GPU in
// active.bin, k records back to back. This daemon still writes exactly one
// record (it runs one GPU), but it recognizes such a file by its size and
// keeps it intact: reported as LINUX_DAEMON_STATE_MULTIPLE, never "matches no
// known layout" (which would delete every card's journal), never overwritten.
static inline unsigned int linux_daemon_state_set_entry_count(long long size) {
    const long long one = (long long)sizeof(LinuxDaemonStateRecord);
    if (size <= 0 || size % one) return 0;
    const long long count = size / one;
    return count <= MAX_GPU_ADAPTERS ? (unsigned int)count : 0;
}

#endif
