// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// Fan stall detection, shared by the Windows fan runtime and the Linux daemon
// fan worker.
//
// The failure ladder in fan_runtime_policy.h reacts when the driver refuses a
// fan write or a temperature read.  It cannot see a fan that accepts every
// write and still does not turn: a seized bearing, an unplugged fan, or a
// board whose manual duty never reaches the motor.  Under a manual duty the
// driver's own fan control is out of the loop, so such a GPU would run with
// no airflow while Green Curve reports the duty as held.  This policy decides,
// from the readings the runtime already takes, when a commanded fan has
// demonstrably stopped, so the runtime can hand the fan back to the driver.
//
// It must never fire on a healthy fan.  Each of these is a normal condition:
//
//   * A native zero-RPM stop.  The curve hands the fan to driver automatic
//     control below its start temperature; the fan stands still by design.
//     Only a fan the driver reports under MANUAL policy is ever judged.
//   * Spin-up.  Measured duty and tach read 0 for a few seconds after a duty is
//     first applied to a stopped fan.  A stall needs consecutive stopped
//     readings spanning FAN_STALL_CONFIRM_MS.
//   * A sensor that does not exist.  Many boards answer the tach or measured
//     duty getter with 0 forever, or not at all.  A sensor only counts once it
//     has reported a nonzero value for that fan on this GPU in this process.
//   * A duty too low to turn the motor.  Only duties at or above the driver's
//     advertised minimum, and never below FAN_STALL_MIN_JUDGED_PERCENT, are
//     judged.
//   * A mode or Apply change.  A new command starts every window over.
//
// Pure: inputs are readings and a monotonic clock value; no NVML, no globals.

#ifndef GREEN_CURVE_FAN_STALL_POLICY_H
#define GREEN_CURVE_FAN_STALL_POLICY_H

#include "gpu_core.h"

#include <stdio.h>
#include <string.h>

// 20 s of consecutive stopped readings.  A fan spinning up from a stop reports
// tach within a few seconds (the measured-duty lag documented in
// fan_runtime_policy.h); 20 s is several times that, and spans at least four
// samples on the slowest (5 s fixed-duty) cadence.  It is still short against
// the minutes a GPU takes to heat-soak without airflow, and the GPU's own
// thermal throttling protects it meanwhile.
static const unsigned long long FAN_STALL_CONFIRM_MS = 20000ull;
// A window also needs this many stopped readings, so two readings far apart
// can never confirm on elapsed time alone.
static const unsigned int FAN_STALL_MIN_OBSERVATIONS = 3u;
// Readings further apart than this are not consecutive: the window restarts at
// the later one.  Covers a worker that was blocked behind a long Apply, or a
// system that slept between two readings (the tick clock keeps counting).
// Above the 5 s fixed cadence and the 5 s maximum curve poll interval.
static const unsigned long long FAN_STALL_MAX_OBSERVATION_GAP_MS = 12000ull;
// The runtime may poll every 250 ms; fan readings are taken at most this often.
static const unsigned long long FAN_STALL_SAMPLE_INTERVAL_MS = 2000ull;
// Duties below this are never judged, whatever minimum the driver advertises
// (an older board may advertise 0 and still not turn its fan at 15%).
static const int FAN_STALL_MIN_JUDGED_PERCENT = 30;

static_assert(MAX_GPU_FANS <= 32, "fan stall masks hold one bit per fan");

// One fan's readings from one sample.  `*Known` is whether that getter
// answered in this sample, independent of the value (0 is a valid reading).
struct FanStallFanSample {
    bool policyKnown;
    bool policyManual;
    bool intentKnown;
    int intentPercent;
    bool rpmKnown;
    unsigned int rpm;
    bool measuredKnown;
    int measuredPercent;
};

// What the runtime commands right now.  `manualCommanded` is false while a
// zero-RPM curve has handed the fan to driver automatic control.
struct FanStallCommand {
    bool manualCommanded;
    int commandedPercent;   // after the driver-range clamp the write uses
    bool rangeKnown;
    int minPercent;
};

struct FanStallFanTrack {
    bool rpmProven;
    bool measuredProven;
    unsigned int stalledObservations;
    unsigned long long windowStartMs;
    unsigned long long lastObservationMs;
};

struct FanStallTracker {
    bool initialized;
    bool sampled;
    unsigned long long deviceKey;    // which GPU: sensor proof belongs to it
    unsigned long long commandKey;   // which command: windows belong to it
    unsigned long long lastSampleMs;
    FanStallFanTrack fans[MAX_GPU_FANS];
};

enum FanStallFanState {
    FAN_STALL_FAN_NOT_JUDGED = 0,   // not under a judged manual duty
    FAN_STALL_FAN_NO_EVIDENCE,      // no proven sensor answered
    FAN_STALL_FAN_SPINNING,
    FAN_STALL_FAN_STOPPED,
};

enum FanStallVerdict {
    FAN_STALL_VERDICT_NONE = 0,
    FAN_STALL_VERDICT_CONFIRMED,
};

// Why windows were cleared before this sample was judged.
enum FanStallReset {
    FAN_STALL_RESET_NONE = 0,
    FAN_STALL_RESET_DEVICE,     // other GPU (or first sample): proof dropped too
    FAN_STALL_RESET_COMMAND,    // new Apply or mode: proof kept
};

struct FanStallReport {
    FanStallVerdict verdict;
    FanStallReset reset;
    // The confirmed fan (the lowest index when several confirm together).
    unsigned int fanIndex;
    int judgedPercent;
    unsigned long long stalledForMs;
    unsigned int observations;
    bool rpmEvidence;
    bool measuredEvidence;
    // Per-fan transitions for one-line diagnostics.
    unsigned int proofGainedMask;     // a sensor of this fan was proven now
    unsigned int windowStartedMask;   // first stopped reading of a window
    unsigned int windowRestartedMask; // stopped again after a gap
    unsigned int windowClearedMask;   // an open window ended without a stall
    FanStallFanState states[MAX_GPU_FANS];
};

static inline void fan_stall_tracker_clear_windows(FanStallTracker* t) {
    if (!t) return;
    for (unsigned int i = 0; i < MAX_GPU_FANS; ++i) {
        t->fans[i].stalledObservations = 0;
        t->fans[i].windowStartMs = 0;
        t->fans[i].lastObservationMs = 0;
    }
}

static inline bool fan_stall_tracker_has_open_window(const FanStallTracker* t) {
    if (!t) return false;
    for (unsigned int i = 0; i < MAX_GPU_FANS; ++i)
        if (t->fans[i].stalledObservations) return true;
    return false;
}

// Whether the runtime should take fan readings for this tracker now.  A clock
// that went backwards is treated as due rather than waited out.
static inline bool fan_stall_sample_due(const FanStallTracker* t,
                                        unsigned long long nowMs) {
    if (!t || !t->sampled) return true;
    if (nowMs < t->lastSampleMs) return true;
    return nowMs - t->lastSampleMs >= FAN_STALL_SAMPLE_INTERVAL_MS;
}

static inline int fan_stall_judged_floor(bool rangeKnown, int minPercent) {
    int floor = FAN_STALL_MIN_JUDGED_PERCENT;
    if (rangeKnown && minPercent > floor) floor = minPercent;
    return floor;
}

// The duty this fan should be turning at: the driver's own intent when it
// answers (that is what it drives), the runtime's command otherwise.
static inline int fan_stall_judged_percent(const FanStallCommand* cmd,
                                           const FanStallFanSample* s) {
    if (s && s->intentKnown) return s->intentPercent;
    return cmd ? cmd->commandedPercent : 0;
}

static inline FanStallFanState fan_stall_classify(const FanStallCommand* cmd,
                                                  const FanStallFanSample* s,
                                                  bool rpmProven,
                                                  bool measuredProven) {
    if (!cmd || !s || !cmd->manualCommanded) return FAN_STALL_FAN_NOT_JUDGED;
    // The driver holds this fan (or will not say): not ours to judge.
    if (!s->policyKnown || !s->policyManual) return FAN_STALL_FAN_NOT_JUDGED;
    if (fan_stall_judged_percent(cmd, s) <
            fan_stall_judged_floor(cmd->rangeKnown, cmd->minPercent))
        return FAN_STALL_FAN_NOT_JUDGED;
    const bool rpmUsable = s->rpmKnown && rpmProven;
    const bool measuredUsable = s->measuredKnown && measuredProven;
    if (!rpmUsable && !measuredUsable) return FAN_STALL_FAN_NO_EVIDENCE;
    // Any proven sensor that sees motion wins: stopping a working fan is the
    // one mistake this policy must not make.
    if ((rpmUsable && s->rpm > 0) || (measuredUsable && s->measuredPercent > 0))
        return FAN_STALL_FAN_SPINNING;
    return FAN_STALL_FAN_STOPPED;
}

// Feeds one sample.  `fanCount` is clamped to MAX_GPU_FANS; fans beyond it
// have their windows cleared.  On a confirmed stall every window is cleared,
// so the caller's handback is requested exactly once.
static inline FanStallReport fan_stall_observe(FanStallTracker* t,
    unsigned long long deviceKey, unsigned long long commandKey,
    unsigned long long nowMs, const FanStallCommand* cmd,
    const FanStallFanSample* samples, unsigned int fanCount) {
    FanStallReport report;
    memset(&report, 0, sizeof(report));
    if (!t || !cmd) return report;
    if (!t->initialized || t->deviceKey != deviceKey) {
        const bool first = !t->initialized;
        memset(t, 0, sizeof(*t));
        t->initialized = true;
        t->deviceKey = deviceKey;
        t->commandKey = commandKey;
        report.reset = first ? FAN_STALL_RESET_NONE : FAN_STALL_RESET_DEVICE;
    } else if (t->commandKey != commandKey) {
        if (fan_stall_tracker_has_open_window(t)) {
            for (unsigned int i = 0; i < MAX_GPU_FANS; ++i)
                if (t->fans[i].stalledObservations) report.windowClearedMask |= 1u << i;
        }
        fan_stall_tracker_clear_windows(t);
        t->commandKey = commandKey;
        report.reset = FAN_STALL_RESET_COMMAND;
    }
    t->sampled = true;
    t->lastSampleMs = nowMs;
    if (fanCount > MAX_GPU_FANS) fanCount = MAX_GPU_FANS;

    bool confirmed = false;
    for (unsigned int i = 0; i < MAX_GPU_FANS; ++i) {
        FanStallFanTrack* f = &t->fans[i];
        const unsigned int bit = 1u << i;
        if (i >= fanCount || !samples) {
            if (f->stalledObservations) report.windowClearedMask |= bit;
            f->stalledObservations = 0;
            continue;
        }
        const FanStallFanSample* s = &samples[i];
        // Proof first: a reading that proves the sensor also shows motion.
        if (s->rpmKnown && s->rpm > 0 && !f->rpmProven) {
            f->rpmProven = true;
            report.proofGainedMask |= bit;
        }
        if (s->measuredKnown && s->measuredPercent > 0 && !f->measuredProven) {
            f->measuredProven = true;
            report.proofGainedMask |= bit;
        }
        const FanStallFanState state = fan_stall_classify(cmd, s,
            f->rpmProven, f->measuredProven);
        report.states[i] = state;
        if (state != FAN_STALL_FAN_STOPPED) {
            if (f->stalledObservations) report.windowClearedMask |= bit;
            f->stalledObservations = 0;
            f->windowStartMs = 0;
            f->lastObservationMs = 0;
            continue;
        }
        const bool gap = f->stalledObservations &&
            (nowMs < f->lastObservationMs ||
             nowMs - f->lastObservationMs > FAN_STALL_MAX_OBSERVATION_GAP_MS);
        if (!f->stalledObservations || gap) {
            report.windowStartedMask |= f->stalledObservations ? 0u : bit;
            report.windowRestartedMask |= gap ? bit : 0u;
            f->stalledObservations = 1;
            f->windowStartMs = nowMs;
        } else if (f->stalledObservations < 0xFFFFFFFFu) {
            f->stalledObservations++;
        }
        f->lastObservationMs = nowMs;
        if (!confirmed && f->stalledObservations >= FAN_STALL_MIN_OBSERVATIONS &&
            nowMs - f->windowStartMs >= FAN_STALL_CONFIRM_MS) {
            confirmed = true;
            report.verdict = FAN_STALL_VERDICT_CONFIRMED;
            report.fanIndex = i;
            report.judgedPercent = fan_stall_judged_percent(cmd, s);
            report.stalledForMs = nowMs - f->windowStartMs;
            report.observations = f->stalledObservations;
            report.rpmEvidence = s->rpmKnown && f->rpmProven;
            report.measuredEvidence = s->measuredKnown && f->measuredProven;
        }
    }
    if (confirmed) fan_stall_tracker_clear_windows(t);
    return report;
}

// No usable reading this sample (the fan read itself failed): nothing can be
// judged, so every window closes, and the sample still counts toward the
// cadence so a failing read is not retried on every 250 ms tick.  Returns
// whether a window was open.
static inline bool fan_stall_note_sample_unavailable(FanStallTracker* t,
                                                     unsigned long long nowMs) {
    if (!t) return false;
    const bool hadWindow = fan_stall_tracker_has_open_window(t);
    fan_stall_tracker_clear_windows(t);
    t->sampled = true;
    t->lastSampleMs = nowMs;
    return hadWindow;
}

// Window transitions can repeat every sample on a fan that flickers between
// readings; each fan logs at most once per interval and counts the rest.
static const unsigned long long FAN_STALL_LOG_INTERVAL_MS = 30000ull;
struct FanStallLogLimiter {
    unsigned long long lastMs[MAX_GPU_FANS];
    unsigned int suppressed[MAX_GPU_FANS];
    bool logged[MAX_GPU_FANS];
};

// True when this fan may log now; `suppressedOut` receives how many lines
// were withheld since its last one.
static inline bool fan_stall_log_allow(FanStallLogLimiter* l, unsigned int fan,
                                       unsigned long long nowMs,
                                       unsigned int* suppressedOut) {
    if (suppressedOut) *suppressedOut = 0;
    if (!l || fan >= MAX_GPU_FANS) return false;
    if (l->logged[fan] && nowMs >= l->lastMs[fan] &&
        nowMs - l->lastMs[fan] < FAN_STALL_LOG_INTERVAL_MS) {
        if (l->suppressed[fan] < 0xFFFFFFFFu) l->suppressed[fan]++;
        return false;
    }
    if (suppressedOut) *suppressedOut = l->suppressed[fan];
    l->suppressed[fan] = 0;
    l->lastMs[fan] = nowMs;
    l->logged[fan] = true;
    return true;
}

static inline const char* fan_stall_state_name(FanStallFanState state) {
    switch (state) {
        case FAN_STALL_FAN_NOT_JUDGED: return "not-judged";
        case FAN_STALL_FAN_NO_EVIDENCE: return "no-evidence";
        case FAN_STALL_FAN_SPINNING: return "spinning";
        case FAN_STALL_FAN_STOPPED: return "stopped";
    }
    return "unknown";
}

static inline const char* fan_stall_reset_name(FanStallReset reset) {
    switch (reset) {
        case FAN_STALL_RESET_NONE: return "none";
        case FAN_STALL_RESET_DEVICE: return "gpu-changed";
        case FAN_STALL_RESET_COMMAND: return "command-changed";
    }
    return "unknown";
}

// One line describing fan `fan`'s window transition in `r`, for the platform
// log; false when it had none.  Proof lines are separate (they are one-shot).
static inline bool fan_stall_format_window_event(const FanStallReport* r,
                                                 unsigned int fan,
                                                 const FanStallFanSample* s,
                                                 int commandedPercent,
                                                 unsigned int suppressed,
                                                 char* out, size_t outSize) {
    if (!r || fan >= MAX_GPU_FANS || !out || !outSize) return false;
    out[0] = '\0';
    const unsigned int bit = 1u << fan;
    const unsigned int rpm = s && s->rpmKnown ? s->rpm : 0u;
    const int measured = s && s->measuredKnown ? s->measuredPercent : -1;
    if (r->windowStartedMask & bit) {
        snprintf(out, outSize,
                 "fan %u reads stopped under a manual %d%% duty (rpm=%u measured=%d%%); "
                 "confirming over %llu s (suppressed=%u)",
                 fan, commandedPercent, rpm, measured,
                 FAN_STALL_CONFIRM_MS / 1000ull, suppressed);
        return true;
    }
    if (r->windowRestartedMask & bit) {
        snprintf(out, outSize,
                 "fan %u readings were not consecutive; stall window restarted (suppressed=%u)",
                 fan, suppressed);
        return true;
    }
    if (r->windowClearedMask & bit) {
        snprintf(out, outSize, "fan %u stall window closed: %s (suppressed=%u)", fan,
                 r->reset == FAN_STALL_RESET_COMMAND ? fan_stall_reset_name(r->reset)
                                                     : fan_stall_state_name(r->states[fan]),
                 suppressed);
        return true;
    }
    return false;
}

#endif  // GREEN_CURVE_FAN_STALL_POLICY_H
