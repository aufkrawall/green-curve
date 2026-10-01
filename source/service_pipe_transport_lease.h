// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#pragma once
#include "service_ipc_throttle_policy.h"

// Bound concurrent transport occupancy per account before any blocking read.
// No eviction of active leases, no clock, and no wait: release is scope-owned.
struct ServicePipeTransportLeases {
    enum { kSlots = 6, kPerAccount = 2 };
    ServiceIpcThrottleKey owners[kSlots];
    int acquire(const ServiceIpcThrottleKey& key) {
        if (!key.valid) return -1;
        int count = 0, freeSlot = -1;
        for (int i = 0; i < kSlots; ++i) {
            if (owners[i].equals(key)) ++count;
            if (!owners[i].valid) freeSlot = i;
        }
        if (count >= kPerAccount || freeSlot < 0) return -1;
        owners[freeSlot] = key;
        return freeSlot;
    }
    void release(int slot) {
        if (slot >= 0 && slot < kSlots) owners[slot].clear();
    }
};

#if defined(_WIN32)
#include <windows.h>
#include <sddl.h>

// A primary-process SID is an availability key only. Authorization still uses
// the connection's impersonation token AFTER its mandatory first read. Ignore
// logon/session IDs here so spawning processes or logons cannot buy capacity.
static inline bool service_pipe_transport_account(HANDLE pipe, ServiceIpcThrottleKey* key) {
    key->clear();
    ULONG pid = 0;
    if (!GetNamedPipeClientProcessId(pipe, &pid) || !pid) return false;
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) return false;
    HANDLE token = nullptr;
    bool opened = OpenProcessToken(process, TOKEN_QUERY, &token) != FALSE;
    CloseHandle(process);
    if (!opened) return false;
    alignas(TOKEN_USER) BYTE storage[sizeof(TOKEN_USER) + SECURITY_MAX_SID_SIZE] = {};
    DWORD returned = 0;
    bool ok = GetTokenInformation(token, TokenUser, storage, sizeof(storage), &returned) != FALSE;
    CloseHandle(token);
    LPSTR sid = nullptr;
    if (ok) ok = ConvertSidToStringSidA(((TOKEN_USER*)storage)->User.Sid, &sid) != FALSE;
    if (ok && strlen(sid) < sizeof(key->sid)) key->fill(0, 0, sid);
    if (sid) LocalFree(sid);
    return key->valid;
}
#endif
