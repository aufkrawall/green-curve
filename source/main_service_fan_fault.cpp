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
//
// A handback between suspend and the standby restore's write also keeps the
// intent it replaced, so that restore can replay it once
// (service_standby_fan_stash_policy.h).

#include "fan_fault_notice_policy.h"
#include "service_scoped_apply_policy.h"
#include "service_standby_fan_stash_policy.h"

static bool service_ownership_marker_clear(const char* reason);
static void service_write_restart_reapply_snapshot();
static void service_clear_restart_reapply_snapshot();
// main_service_lifecycle_events.cpp: the reducer state is guarded by g_appLock,
// which this takes; call it WITHOUT g_appLock.
static bool service_lifecycle_standby_fan_handback_window(gc_u64* generationOut);

// Guarded by g_appLock.
static ServiceFanFault g_serviceFanFault = {};
static gc_u32 g_serviceFanFaultSerial = 0;
// Guarded by the service runtime lock, like the owned intent it copies.
static ServiceStandbyFanStash g_serviceStandbyFanStash = {};

static void service_standby_fan_stash_clear(const char* reason) {
    if (!g_serviceStandbyFanStash.valid) return;
    debug_log("standby fan stash: dropped (generation=%llu) by %s\n",
        (unsigned long long)g_serviceStandbyFanStash.suspendGeneration,
        reason ? reason : "unspecified");
    g_serviceStandbyFanStash = ServiceStandbyFanStash{};
}

// The standby restore of `resumedGeneration` asks what to do with the stash.
// A stale or superseded stash is dropped here; a replayable one stays until
// the restore actually writes (service_standby_fan_stash_consume), so a
// restore that is still waiting for the GPU keeps it.  Runtime lock held.
static ServiceStandbyFanStashUse service_standby_fan_stash_peek(
    gc_u64 resumedGeneration, ServiceStandbyFanStash* out) {
    if (out) *out = ServiceStandbyFanStash{};
    const ServiceStandbyFanStashUse use = service_standby_fan_stash_use(
        &g_serviceStandbyFanStash, resumedGeneration,
        g_serviceHasActiveDesired, &g_serviceActiveDesired);
    if (use == SERVICE_STANDBY_FAN_STASH_REPLAY) {
        if (out) *out = g_serviceStandbyFanStash;
    } else if (use != SERVICE_STANDBY_FAN_STASH_NONE) {
        debug_log("standby fan stash: not replayed (%s, stash generation=%llu resumed=%llu)\n",
            service_standby_fan_stash_use_name(use),
            (unsigned long long)g_serviceStandbyFanStash.suspendGeneration,
            (unsigned long long)resumedGeneration);
        service_standby_fan_stash_clear(service_standby_fan_stash_use_name(use));
    }
    return use;
}

// The restore wrote (or failed to): the stash is used up either way.
static void service_standby_fan_stash_consume() {
    g_serviceStandbyFanStash = ServiceStandbyFanStash{};
}

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
    // A deliberate fan write or a Reset also supersedes a pending standby replay.
    service_standby_fan_stash_clear(reason);
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
    const DesiredSettings previousDesired = g_serviceActiveDesired;
    const GpuAdapterInfo previousGpu = g_serviceActiveDesiredGpu;
    gc_u64 standbyGeneration = 0;
    const bool standbyWindow =
        service_lifecycle_standby_fan_handback_window(&standbyGeneration);
    EnterCriticalSection(&g_appLock);
    const ServiceProfileSource previousProfileSource = g_serviceActiveProfileSource;
    const int previousProfileSlot = g_serviceActiveProfileSlot;
    LeaveCriticalSection(&g_appLock);
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
        if (!hadIntent) {
            g_serviceActiveDesired = {};
            g_serviceActiveDesired.lockCi = -1;
            g_serviceActiveDesired.powerLimitPct = POWER_LIMIT_DEFAULT_PCT;
        }
        g_serviceActiveDesired.hasFan = 1;
        g_serviceActiveDesired.fanAuto = 0;
        g_serviceActiveDesired.fanMode = FAN_MODE_FIXED;
        g_serviceActiveDesired.fanPercent = FAN_RUNTIME_EMERGENCY_PERCENT;
    }
    g_serviceHasActiveDesired = service_scoped_has_owned_intent(g_serviceActiveDesired);
    bool markerCleared = false;
    if (!g_serviceHasActiveDesired) {
        memset(&g_serviceActiveDesiredGpu, 0, sizeof(g_serviceActiveDesiredGpu));
        if (hadIntent)
            markerCleared = service_ownership_marker_clear("fan runtime handback released the last owned domain");
    } else if (!g_serviceActiveDesiredGpu.valid) {
        if (g_app.selectedGpu.valid) {
            g_serviceActiveDesiredGpu = g_app.selectedGpu;
        } else if (g_app.selectedGpuIndex < g_app.adapterCount &&
                   g_app.selectedGpuIndex < MAX_GPU_ADAPTERS &&
                   g_app.adapters[g_app.selectedGpuIndex].valid) {
            g_serviceActiveDesiredGpu = g_app.adapters[g_app.selectedGpuIndex];
        }
    }

    ServiceProfileSource publishedProfileSource = SERVICE_PROFILE_SOURCE_NONE;
    int publishedProfileSlot = 0;
    EnterCriticalSection(&g_appLock);
    if (!g_serviceHasActiveDesired) {
        g_serviceActiveProfileSource = SERVICE_PROFILE_SOURCE_NONE;
        g_serviceActiveProfileSlot = 0;
    } else {
        // The owned fan domain just diverged from any saved profile slot; drop
        // slot attribution (and promote NONE on fresh emergency ownership) so
        // snapshots and the restart snapshot stay canonical.
        g_serviceActiveProfileSource = SERVICE_PROFILE_SOURCE_AD_HOC;
        g_serviceActiveProfileSlot = 0;
    }
    publishedProfileSource = g_serviceActiveProfileSource;
    publishedProfileSlot = g_serviceActiveProfileSlot;
    ServiceFanFault record = *fault;
    if (g_serviceFanFaultSerial == 0xFFFFFFFFu) g_serviceFanFaultSerial = 0;
    record.serial = ++g_serviceFanFaultSerial;
    record.detail[sizeof(record.detail) - 1] = '\0';
    g_serviceFanFault = record;
    LeaveCriticalSection(&g_appLock);

    // Inside a standby window keep what the handback replaced, so the restore
    // of this wake can replay it.  A second handback in the same window (the
    // intent no longer owns a manual fan) does not overwrite the first copy.
    const bool stashCaptured = service_standby_fan_stash_should_capture(
        standbyWindow, hadIntent, &previousDesired);
    if (stashCaptured) {
        ServiceStandbyFanStash stash = {};
        stash.valid = true;
        stash.suspendGeneration = standbyGeneration;
        stash.replaced = previousDesired;
        stash.replacedGpu = previousGpu;
        stash.replacedProfileSource = previousProfileSource;
        stash.replacedProfileSlot = previousProfileSlot;
        stash.handbackHasIntent = g_serviceHasActiveDesired;
        stash.handbackIntent = g_serviceActiveDesired;
        g_serviceStandbyFanStash = stash;
    } else if (standbyWindow && g_serviceStandbyFanStash.valid &&
               g_serviceStandbyFanStash.suspendGeneration == standbyGeneration) {
        // Keep the first copy, but follow the intent this handback left so the
        // restore still recognizes it as unchanged.
        g_serviceStandbyFanStash.handbackHasIntent = g_serviceHasActiveDesired;
        g_serviceStandbyFanStash.handbackIntent = g_serviceActiveDesired;
    } else {
        service_standby_fan_stash_clear("a fan handback outside a standby window");
    }
    debug_log("standby fan stash: window=%d generation=%llu captured=%d kept=%d previousMode=%s\n",
        standbyWindow ? 1 : 0, (unsigned long long)standbyGeneration,
        stashCaptured ? 1 : 0, g_serviceStandbyFanStash.valid ? 1 : 0,
        fan_mode_label(previousMode));

    // Keep the on-disk restart reapply snapshot aligned with the post-handback
    // intent so a later controlled restart/recovery cannot replay the faulted
    // fan curve.
    if (!g_serviceHasActiveDesired) {
        service_clear_restart_reapply_snapshot();
    } else {
        service_write_restart_reapply_snapshot();
    }

    debug_log("fan fault: owned fan intent %s -> %s after the %s handback "
              "(owned=%d markerCleared=%d ownerGpuValid=%d profileSource=%u slot=%d restartSnapshot=%s)\n",
        fan_mode_label(previousMode),
        fan_mode_label(fan_owned_intent_mode(g_serviceActiveDesired.hasFan != 0,
            g_serviceActiveDesired.fanMode, g_serviceActiveDesired.fanAuto != 0)),
        gc_fan_fault_handback_name(fault->handback),
        g_serviceHasActiveDesired ? 1 : 0, markerCleared ? 1 : 0,
        g_serviceActiveDesiredGpu.valid ? 1 : 0,
        (unsigned int)publishedProfileSource, publishedProfileSlot,
        g_serviceHasActiveDesired ? "updated" : "cleared");
    debug_log("fan fault: recorded for clients kind=%s serial=%u fan=%u pct=%u stalledMs=%u handback=%s detail=%s\n",
        gc_fan_fault_kind_name(record.kind), (unsigned int)record.serial,
        (unsigned int)record.fanIndex, (unsigned int)record.commandedPercent,
        (unsigned int)record.stalledForMs, gc_fan_fault_handback_name(record.handback),
        record.detail[0] ? record.detail : "<none>");
}
