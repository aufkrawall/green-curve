// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT

// Fan stall detection for the Windows fan runtime (fan_stall_policy.h).
// Included by main_fan_runtime.cpp after the failure escalation.
//
// Runs after every runtime tick, on the thread that ran it: the service fan
// worker, the runtime start inside an Apply, or the GUI fan timer.  The service
// callers hold the runtime mutex, which serializes the tracker; the GUI runtime
// lives on the window thread.  Like the tick, it calls NVML WITHOUT g_appLock.
//
// A confirmed stall goes through the same handback as repeated failures:
// driver automatic control, 100% if the driver refuses, the runtime stopped,
// and the user told (main_fan_runtime_failure.cpp).

#include "fan_stall_policy.h"

static FanStallTracker g_fanStallTracker = {};
static FanStallLogLimiter g_fanStallLogLimiter = {};
static unsigned long long g_fanStallLastReadFailureLogMs = 0;

// Sensor proof belongs to one GPU; another device handle starts it over.
static unsigned long long fan_stall_device_key() {
    return (unsigned long long)(uintptr_t)g_app.nvmlDevice;
}

struct FanStallRuntimeView {
    bool active;
    FanStallCommand command;
    unsigned long long commandKey;
};

// Caller holds g_appLock.  A zero-RPM curve in its driver-automatic phase is
// active but commands no manual duty.
static FanStallRuntimeView fan_stall_runtime_view_locked() {
    FanStallRuntimeView view = {};
    const bool fixed = g_app.fanFixedRuntimeActive;
    const bool curve = g_app.fanCurveRuntimeActive;
    view.active = fixed || curve;
    view.command.manualCommanded = fixed ||
        (curve && !g_app.fanCurveDriverAutoActive && g_app.fanCurveHasLastAppliedTemp);
    const int requested = fixed ? clamp_percent(g_app.activeFanFixedPercent)
                                : g_app.fanCurveLastAppliedPercent;
    view.command.rangeKnown = g_app.fanRangeKnown;
    view.command.minPercent = (int)g_app.fanMinPct;
    view.command.commandedPercent = fan_manual_effective_percent(requested,
        (int)g_app.fanMinPct, (int)g_app.fanMaxPct, g_app.fanRangeKnown);
    view.commandKey = (g_app.fanRuntimeGeneration << 1) | (fixed ? 1ull : 0ull);
    return view;
}

// Caller holds g_appLock; the values are the last nvml_read_fans().
static unsigned int fan_stall_collect_locked(FanStallFanSample* samples) {
    const unsigned int count = g_app.fanCount > MAX_GPU_FANS ? MAX_GPU_FANS : g_app.fanCount;
    for (unsigned int fan = 0; fan < count; ++fan) {
        FanStallFanSample& s = samples[fan];
        s.policyKnown = g_app.readback.fan.policy[fan];
        s.policyManual = s.policyKnown && g_app.fanPolicy[fan] == NVML_FAN_POLICY_MANUAL;
        s.intentKnown = g_app.readback.fan.target[fan];
        s.intentPercent = (int)g_app.fanTargetPercent[fan];
        s.rpmKnown = g_app.readback.fan.rpm[fan];
        s.rpm = g_app.fanRpm[fan];
        s.measuredKnown = g_app.readback.fan.measured[fan];
        s.measuredPercent = (int)g_app.fanPercent[fan];
    }
    return count;
}

static void fan_stall_log_report(const FanStallReport& r, const FanStallFanSample* samples,
                                 unsigned int fanCount, const FanStallRuntimeView& view,
                                 unsigned long long now) {
    if (r.reset == FAN_STALL_RESET_DEVICE)
        debug_log("fan stall: tracking restarted for another GPU; sensor proof dropped\n");
    for (unsigned int fan = 0; fan < MAX_GPU_FANS; ++fan) {
        const unsigned int bit = 1u << fan;
        const FanStallFanSample* s = fan < fanCount ? &samples[fan] : nullptr;
        if ((r.proofGainedMask & bit) && s)
            debug_log("fan stall: fan %u sensors proven (rpm=%u%s measured=%d%%%s); stall detection armed\n",
                fan, s->rpm, s->rpmKnown ? "" : "?", s->measuredPercent,
                s->measuredKnown ? "" : "?");
        if (r.verdict == FAN_STALL_VERDICT_CONFIRMED && r.fanIndex == fan) continue;
        if (!((r.windowStartedMask | r.windowRestartedMask | r.windowClearedMask) & bit)) continue;
        unsigned int suppressed = 0;
        if (!fan_stall_log_allow(&g_fanStallLogLimiter, fan, now, &suppressed)) continue;
        char line[192] = {};
        const int judged = s ? fan_stall_judged_percent(&view.command, s) : view.command.commandedPercent;
        if (fan_stall_format_window_event(&r, fan, s, judged, suppressed, line, sizeof(line)))
            debug_log("fan stall: %s\n", line);
    }
}

static void fan_stall_hand_back(const FanStallReport& r, const FanStallFanSample* samples) {
    const FanStallFanSample& s = samples[r.fanIndex];
    char rpmText[24] = "n/a", measuredText[24] = "n/a";
    if (r.rpmEvidence) StringCchPrintfA(rpmText, ARRAY_COUNT(rpmText), "%u RPM", s.rpm);
    if (r.measuredEvidence)
        StringCchPrintfA(measuredText, ARRAY_COUNT(measuredText), "%d%%", s.measuredPercent);
    char summary[SERVICE_FAN_FAULT_DETAIL_CHARS] = {};
    StringCchPrintfA(summary, ARRAY_COUNT(summary),
        "Fan %u did not turn at %d%% for %llu s (tach %s, measured %s)",
        r.fanIndex + 1, r.judgedPercent, (r.stalledForMs + 500ull) / 1000ull,
        rpmText, measuredText);
    debug_log("FAN STALL: %s over %u readings; handing the fan back to the driver\n",
        summary, r.observations);
    FanRuntimeHandbackCause cause = {};
    cause.kind = SERVICE_FAN_FAULT_STALL;
    cause.summary = summary;
    cause.fanIndex = r.fanIndex;
    cause.commandedPercent = r.judgedPercent;
    cause.stalledForMs = r.stalledForMs;
    escalate_fan_runtime_handback(&cause);
}

// `readSerialBefore` is g_app.fanReadSerial before the tick: a tick that read
// the fans already supplies this sample's readings.
static void fan_stall_check_after_tick(unsigned long long readSerialBefore) {
    EnterCriticalSection(&g_appLock);
    FanStallRuntimeView view = fan_stall_runtime_view_locked();
    LeaveCriticalSection(&g_appLock);
    if (!view.active) {
        if (fan_stall_tracker_has_open_window(&g_fanStallTracker)) {
            fan_stall_tracker_clear_windows(&g_fanStallTracker);
            debug_log("fan stall: runtime stopped; open stall windows discarded\n");
        }
        return;
    }
    const unsigned long long now = GetTickCount64();
    if (!fan_stall_sample_due(&g_fanStallTracker, now)) return;
    if (g_app.fanReadSerial == readSerialBefore) {
        char detail[128] = {};
        // nvml_read_fans can fault in nvml.dll -- NO g_appLock held.
        if (!nvml_read_fans(detail, sizeof(detail))) {
            const bool hadWindow = fan_stall_note_sample_unavailable(&g_fanStallTracker, now);
            if (hadWindow || g_fanStallLastReadFailureLogMs == 0 ||
                now - g_fanStallLastReadFailureLogMs >= FAN_STALL_LOG_INTERVAL_MS) {
                g_fanStallLastReadFailureLogMs = now;
                debug_log("fan stall: fan read failed (%s); nothing judged%s\n",
                    detail[0] ? detail : "unknown", hadWindow ? ", open stall windows closed" : "");
            }
            return;
        }
    }
    FanStallFanSample samples[MAX_GPU_FANS] = {};
    EnterCriticalSection(&g_appLock);
    const unsigned int fanCount = fan_stall_collect_locked(samples);
    view = fan_stall_runtime_view_locked();  // the read refreshes the duty range
    LeaveCriticalSection(&g_appLock);
    const FanStallReport report = fan_stall_observe(&g_fanStallTracker,
        fan_stall_device_key(), view.commandKey, now, &view.command, samples, fanCount);
    fan_stall_log_report(report, samples, fanCount, view, now);
    if (report.verdict == FAN_STALL_VERDICT_CONFIRMED) fan_stall_hand_back(report, samples);
}
