// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// The update install reservation, shared by every GPU-write path.
//
// The updater sets this flag while holding the service runtime lock and then
// keeps it set for the remainder of the service process once setup starts.
// Every other write path checks it immediately after acquiring the same lock,
// so a queued APPLY cannot begin between the updater's check and CreateProcess.

static volatile LONG g_serviceUpdateInstallReserved = 0;

static bool service_update_install_reserved() {
    return InterlockedExchangeAdd(&g_serviceUpdateInstallReserved, 0) != 0;
}

static void service_update_set_install_reserved(bool reserved) {
    InterlockedExchange(&g_serviceUpdateInstallReserved, reserved ? 1 : 0);
}

// Called by APPLY/RESET while their runtime lock is held.  Returning true means
// the caller already filled the response and must not perform the write.
static bool service_update_install_reject_mutation(
    ServiceResponse* response, const char* commandName) {
    if (!service_update_install_reserved()) return false;
    if (!response) return true;
    response->status = SERVICE_STATUS_ERROR;
    StringCchCopyA(response->message, ARRAY_COUNT(response->message),
                   "An update installation is starting; GPU changes are disabled");
    debug_log("service %s rejected: update install reservation is active\n",
              commandName ? commandName : "mutation");
    return true;
}

// Variant for callers whose only legal response to an active reservation is
// "return immediately".  They must already hold the runtime lock; this helper
// releases it before reporting.
static bool service_update_install_blocks_locked() {
    if (!service_update_install_reserved()) return false;
    unlock_service_runtime();
    debug_log("update install reservation suppressed a runtime write\n");
    return true;
}
