// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#pragma once
#include <stddef.h>
#include <errno.h>

// Read a bounded record through an injectable system-call adapter. EOF is a
// short record; a real I/O error stays an error rather than proving corruption.
template <typename Read>
static ptrdiff_t gc_read_record(Read readCall, int fd, void* data, size_t size) {
    size_t used = 0;
    while (used < size) {
        ptrdiff_t count = readCall(fd, (unsigned char*)data + used, size - used);
        if (count > 0) { used += (size_t)count; continue; }
        if (count < 0 && errno == EINTR) continue;
        if (count < 0) return -1;
        break;
    }
    return (ptrdiff_t)used;
}
