// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// The service's v39 fan fault record (service_fan_fault.h) and what a fan
// runtime handback does to the owned intent.  Included before the snapshot
// shard, which publishes the record.
//
// When the runtime gives a fan up -- repeated failures, or a fan that did not
// turn -- the escalation (main_fan_runtime_failure.cpp) hands it to the driver
// and stops the runtime.  The owned intent used to keep claiming the curve, so
// the service went on reporting it, the tray went on marking its profile
// active, and a later Apply's fan invariant would even have restarted it on
// the fan that had just failed.  The intent now says what Green Curve holds:
// nothing once the driver has the fan back, the 100% emergency duty when the
// driver refused it.  Re-engaging a custom fan setting takes a deliberate
// Apply, which also clears the record.
//
// In memory only, like the v29 record: a restarted service did not see the
// fault and does not claim to know of it.

#include "fan_fault_notice_policy.h"
#include "service_scoped_apply_policy.h"

static bool service_ownership_marker_clear(const char* reason);

// Guarded by g_appLock.
static ServiceFanFault g_serviceFanFault = {};
static gc_u32 g_serviceFanFaultSerial = 0;

// Caller holds g_appLock.
static ServiceFanFault service_fan_fault_for_snapshot_locked() {
    return g_serviceFanFault;
}

// A successful Apply that sets the fan, or a Reset: the handback is history.
static void service_fan_fault_clear(const char* reason) {
    EnterCriticalSection(&g_appLock);
    const ServiceFanFault previous = g_serviceFanFault;
    g_serviceFanFault = ServiceFanFault{};
    LeaveCriticalSection(&g_appLock);
    if (previous.kind != SERVICE_FAN_FAULT_NONE)
        debug_log("fan fault: record kind=%s serial=%u cleared by %s\n",
            gc_fan_fault_kind_name(previous.kind), (unsigned int)previous.serial,
            reason ? reason : "unspecified");
}

// Called by the runtime handback in the service process, WITHOUT g_appLock and
// with the service runtime lock held (the fan worker's pulse, or the runtime
// start inside an Apply), which is what serializes the owned intent.
static void service_fan_runtime_handback_publish(const ServiceFanFault* fault) {
    if (!fault || fault->kind == SERVICE_FAN_FAULT_NONE) return;
    if (!service_runtime_lock_held_by_current_thread())
        debug_log("fan fault: handback published WITHOUT the runtime lock; the owned"
                  " intent update is unserialized here\n");
    const bool hadIntent = g_serviceHasActiveDesired;
    const int previousMode = fan_owned_intent_mode(
        hadIntent && g_serviceActiveDesired.hasFan, g_serviceActiveDesired.fanMode,
        g_serviceActiveDesired.fanAuto != 0);
    if (fault->handback == SERVICE_FAN_HANDBACK_DRIVER_AUTO) {
        // The driver has the fan: Green Curve no longer owns it.
        g_serviceActiveDesired.hasFan = 0;
        g_serviceActiveDesired.fanAuto = 1;
        g_serviceActiveDesired.fanMode = FAN_MODE_AUTO;
        g_serviceActiveDesired.fanPercent = 0;
    } else {
        // Green Curve still holds the fan manually (forced to 100%), or could
        // not confirm either write: own the emergency duty, so every later
        // repair aims at maximum cooling rather than at the failed curve.
        g_serviceActiveDesired.hasFan = 1;
        g_serviceActiveDesired.fanAuto = 0;
        g_serviceActiveDesired.fanMode = FAN_MODE_FIXED;
        g_serviceActiveDesired.fanPercent = FAN_RUNTIME_EMERGENCY_PERCENT;
    }
    g_serviceHasActiveDesired = service_scoped_has_owned_intent(g_serviceActiveDesired);
    bool markerCleared = false;
    if (hadIntent && !g_serviceHasActiveDesired)
        markerCleared = service_ownership_marker_clear("fan runtime handback released the last owned domain");
    debug_log("fan fault: owned fan intent %s -> %s after the %s handback (owned=%d markerCleared=%d)\n",
        fan_mode_label(previousMode),
        fan_mode_label(fan_owned_intent_mode(g_serviceActiveDesired.hasFan != 0,
            g_serviceActiveDesired.fanMode, g_serviceActiveDesired.fanAuto != 0)),
        gc_fan_fault_handback_name(fault->handback),
        g_serviceHasActiveDesired ? 1 : 0, markerCleared ? 1 : 0);

    EnterCriticalSection(&g_appLock);
    ServiceFanFault record = *fault;
    if (g_serviceFanFaultSerial == 0xFFFFFFFFu) g_serviceFanFaultSerial = 0;
    record.serial = ++g_serviceFanFaultSerial;
    record.detail[sizeof(record.detail) - 1] = '\0';
    g_serviceFanFault = record;
    LeaveCriticalSection(&g_appLock);
    debug_log("fan fault: recorded for clients kind=%s serial=%u fan=%u pct=%u stalledMs=%u handback=%s detail=%s\n",
        gc_fan_fault_kind_name(record.kind), (unsigned int)record.serial,
        (unsigned int)record.fanIndex, (unsigned int)record.commandedPercent,
        (unsigned int)record.stalledForMs, gc_fan_fault_handback_name(record.handback),
        record.detail[0] ? record.detail : "<none>");
}
