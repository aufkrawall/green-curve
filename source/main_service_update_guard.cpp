// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// The update install reservation, shared by every GPU-write path.
//
// The updater sets this flag while holding the service runtime lock and then
// keeps it set for the remainder of the service process once setup starts.
// Every other write path checks it immediately after acquiring the same lock,
// so a queued APPLY cannot begin between the updater's check and CreateProcess.

#include "update_install_fan_policy.h"

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
// "return immediately".  They must already hold the runtime lock.
//
// NAMED FOR ITS SIDE EFFECT (F-05-002).  This is not a predicate: when it
// returns true it has ALREADY RELEASED the caller's runtime lock, and the
// caller must return or `continue` without unlocking again.  It was previously
// spelled service_update_install_blocks_locked(), which reads as a question and
// hid that -- every one of the five call sites happened to be correct, but the
// next caller to add cleanup after the check would have double-unlocked or
// deadlocked, and nothing in the name would have warned them.
static bool service_update_install_release_and_block() {
    if (!service_update_install_reserved()) return false;
    unlock_service_runtime();
    debug_log("update install reservation suppressed a runtime write "
              "(runtime lock released by the reservation check)\n");
    return true;
}

// ---------------------------------------------------------------------------
// The fan during an install (update_install_fan_policy.h)
// ---------------------------------------------------------------------------

// Both guarded by the runtime lock.
static UpdateInstallFanHandback g_updateInstallFanHandback = {};
static FanCurveConfig g_updateInstallFanCurve = {};

// Called with the runtime lock held, immediately after the reservation is set:
// from here on the fan pulse is blocked, so a manual duty must not be left
// behind for it.
static void service_update_hand_fans_to_driver_for_install() {
    g_updateInstallFanHandback = update_install_fan_handback_capture(
        g_app.fanCurveRuntimeActive, g_app.fanFixedRuntimeActive,
        g_app.activeFanFixedPercent);
    if (!update_install_fan_handback_needed(&g_updateInstallFanHandback)) {
        debug_log("update install: no manual fan runtime active; fan stays with the driver\n");
        return;
    }
    g_updateInstallFanCurve = g_app.activeFanCurve;
    // Restores driver auto and joins the fan worker; the runtime lock is kept
    // for the whole join (stop_service_fan_runtime_thread).
    stop_fan_curve_runtime(true);
    char fanDetail[128] = {};
    bool readOk = nvml_read_fans(fanDetail, sizeof(fanDetail));
    populate_control_state(&g_serviceControlState);
    g_serviceControlStateValid = true;
    debug_log("update install: handed the %s fan runtime (fixed=%d%%) to driver auto for the"
              " install; readback=%d driverAuto=%d%s%s\n",
        update_install_fan_runtime_name(g_updateInstallFanHandback.runtime),
        g_updateInstallFanHandback.fixedPercent, readOk ? 1 : 0,
        g_app.fanIsAuto ? 1 : 0, fanDetail[0] ? " detail=" : "", fanDetail);
    if (!g_app.fanIsAuto) {
        debug_log("update install: WARNING driver auto fan could not be confirmed; the last"
                  " manual duty may persist until setup stops the service\n");
    }
}

// Runtime lock held.  Puts back what the handback stopped, once the
// reservation has been released on a path where setup will NOT stop us.
static void service_update_restore_fans_after_install(const char* why) {
    UpdateInstallFanHandback handback = g_updateInstallFanHandback;
    g_updateInstallFanHandback = {};
    UpdateInstallFanRuntime plan = update_install_fan_restore_plan(&handback,
        !service_update_install_reserved(),
        g_app.fanCurveRuntimeActive || g_app.fanFixedRuntimeActive);
    if (plan == UPDATE_INSTALL_FAN_RUNTIME_CURVE) {
        g_app.activeFanCurve = g_updateInstallFanCurve;
        start_fan_curve_runtime();
    } else if (plan == UPDATE_INSTALL_FAN_RUNTIME_FIXED) {
        g_app.activeFanFixedPercent = handback.fixedPercent;
        start_fixed_fan_runtime();
    }
    debug_log("update install: reservation released (%s); fan restore plan=%s handedBack=%s"
              " curveActive=%d fixedActive=%d\n",
        why && why[0] ? why : "unspecified", update_install_fan_runtime_name(plan),
        update_install_fan_runtime_name(handback.runtime),
        g_app.fanCurveRuntimeActive ? 1 : 0, g_app.fanFixedRuntimeActive ? 1 : 0);
}

// Every non-success exit of the install ends the reservation through here, so
// GPU writes and the fan runtime come back together.  Takes the runtime lock.
static void service_update_release_install_reservation(const char* why) {
    lock_service_runtime();
    service_update_set_install_reserved(false);
    service_update_restore_fans_after_install(why);
    unlock_service_runtime();
}
