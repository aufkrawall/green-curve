// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// Fan stall policy table (fan_stall_policy.h), cases 12800-12829.  Executed
// worker fixtures for the Windows runtime and the Linux daemon live with their
// platform tests; this file proves the shared decisions, above all that a
// healthy fan is never judged stalled.
#include "fan_stall_policy.h"

#include <cstdio>
#include <cstring>

namespace fan_stall_table {

static FanStallFanSample spinning(unsigned int rpm = 1200, int measured = 35) {
    FanStallFanSample s = {};
    s.policyKnown = s.policyManual = true;
    s.intentKnown = true;
    s.intentPercent = 40;
    s.rpmKnown = true;
    s.rpm = rpm;
    s.measuredKnown = true;
    s.measuredPercent = measured;
    return s;
}

static FanStallFanSample stopped() { return spinning(0, 0); }

static FanStallCommand manual(int pct = 40) {
    FanStallCommand c = {};
    c.manualCommanded = true;
    c.commandedPercent = pct;
    c.rangeKnown = true;
    c.minPercent = 30;
    return c;
}

// Feeds `sample` every `stepMs` from `startMs` up to and including `endMs` and
// returns the first confirming time, or 0 when none confirmed.
static unsigned long long feed(FanStallTracker* t, unsigned long long deviceKey,
                               unsigned long long commandKey,
                               const FanStallCommand& cmd,
                               const FanStallFanSample& sample,
                               unsigned long long startMs, unsigned long long endMs,
                               unsigned long long stepMs,
                               FanStallReport* last = nullptr) {
    for (unsigned long long now = startMs; now <= endMs; now += stepMs) {
        FanStallFanSample samples[1] = {sample};
        FanStallReport r = fan_stall_observe(t, deviceKey, commandKey, now, &cmd, samples, 1);
        if (last) *last = r;
        if (r.verdict == FAN_STALL_VERDICT_CONFIRMED) return now;
    }
    return 0;
}

static int run() {
    const FanStallCommand cmd = manual();
    // A board without a tach or measured-duty getter is never judged.
    {
        FanStallTracker t = {};
        FanStallFanSample s = stopped();
        s.rpmKnown = s.measuredKnown = false;
        FanStallReport r = {};
        if (feed(&t, 1, 1, cmd, s, 1000, 120000, 2000, &r) ||
            r.states[0] != FAN_STALL_FAN_NO_EVIDENCE) return 12800;
    }
    // A getter that answers 0 forever proves nothing.
    {
        FanStallTracker t = {};
        FanStallReport r = {};
        if (feed(&t, 1, 1, cmd, stopped(), 1000, 120000, 2000, &r) ||
            r.states[0] != FAN_STALL_FAN_NO_EVIDENCE || t.fans[0].rpmProven ||
            t.fans[0].measuredProven) return 12801;
    }
    // Proven tach, then stopped: confirmed once 20 s of readings span the
    // window, not one sample earlier.
    {
        FanStallTracker t = {};
        FanStallReport r = {};
        feed(&t, 1, 1, cmd, spinning(), 0, 4000, 2000, &r);
        if (!t.fans[0].rpmProven || !t.fans[0].measuredProven ||
            r.states[0] != FAN_STALL_FAN_SPINNING) return 12802;
        // First stopped reading at 6000; 24000 is 18 s in, 26000 is 20 s in.
        if (feed(&t, 1, 1, cmd, stopped(), 6000, 24000, 2000)) return 12803;
        unsigned long long at = feed(&t, 1, 1, cmd, stopped(), 26000, 26000, 2000, &r);
        if (at != 26000 || r.fanIndex != 0 || r.judgedPercent != 40 ||
            r.stalledForMs != 20000 || r.observations != 11 ||
            !r.rpmEvidence || !r.measuredEvidence) return 12804;
        // Confirmation clears every window: no second request on the next sample.
        if (fan_stall_tracker_has_open_window(&t) ||
            feed(&t, 1, 1, cmd, stopped(), 28000, 28000, 2000)) return 12805;
    }
    // Spin-up after a new Apply: stopped readings for a few seconds, then
    // motion.  The window closes and nothing fires.
    {
        FanStallTracker t = {};
        feed(&t, 1, 1, cmd, spinning(), 0, 2000, 2000);
        FanStallReport r = {};
        if (feed(&t, 1, 2, cmd, stopped(), 4000, 10000, 2000, &r) ||
            r.reset != FAN_STALL_RESET_NONE) return 12806;
        if (feed(&t, 1, 2, cmd, spinning(), 12000, 12000, 2000, &r) ||
            r.windowClearedMask != 1u || fan_stall_tracker_has_open_window(&t)) return 12807;
    }
    // A zero-RPM curve in its driver-automatic phase: the fan stands still by
    // design and is never judged, however long.
    {
        FanStallTracker t = {};
        feed(&t, 1, 1, cmd, spinning(), 0, 2000, 2000);
        FanStallCommand zeroRpm = cmd;
        zeroRpm.manualCommanded = false;
        FanStallReport r = {};
        if (feed(&t, 1, 1, zeroRpm, stopped(), 4000, 300000, 2000, &r) ||
            r.states[0] != FAN_STALL_FAN_NOT_JUDGED) return 12808;
    }
    // The driver reports automatic policy (or will not say): not judged.
    {
        FanStallTracker t = {};
        feed(&t, 1, 1, cmd, spinning(), 0, 2000, 2000);
        FanStallFanSample s = stopped();
        s.policyManual = false;
        if (feed(&t, 1, 1, cmd, s, 4000, 120000, 2000)) return 12809;
        s.policyManual = true;
        s.policyKnown = false;
        if (feed(&t, 1, 1, cmd, s, 122000, 240000, 2000)) return 12810;
    }
    // Duty floors: never below 30%, and never below the advertised minimum.
    {
        FanStallTracker t = {};
        feed(&t, 1, 1, cmd, spinning(), 0, 2000, 2000);
        FanStallFanSample s = stopped();
        s.intentPercent = 25;
        FanStallCommand unknownRange = manual(25);
        unknownRange.rangeKnown = false;
        unknownRange.minPercent = 0;
        FanStallReport r = {};
        if (feed(&t, 1, 1, unknownRange, s, 4000, 120000, 2000, &r) ||
            r.states[0] != FAN_STALL_FAN_NOT_JUDGED) return 12811;
        FanStallCommand highFloor = manual(40);
        highFloor.minPercent = 45;
        s.intentPercent = 40;
        if (feed(&t, 1, 1, highFloor, s, 122000, 240000, 2000)) return 12812;
        // The driver's intent is what it drives: a 60% command it holds at
        // 20% is judged at 20% and therefore not at all.
        FanStallFanSample lowIntent = stopped();
        lowIntent.intentPercent = 20;
        if (feed(&t, 1, 1, manual(60), lowIntent, 242000, 360000, 2000)) return 12813;
        // Without an intent getter the command is judged.
        FanStallFanSample noIntent = stopped();
        noIntent.intentKnown = false;
        if (feed(&t, 1, 1, manual(60), noIntent, 362000, 400000, 2000, &r) != 382000 ||
            r.judgedPercent != 60) return 12814;
    }
    // Readings more than 12 s apart are not consecutive.
    {
        FanStallTracker t = {};
        feed(&t, 1, 1, cmd, spinning(), 0, 0, 2000);
        FanStallReport r = {};
        if (feed(&t, 1, 1, cmd, stopped(), 2000, 14000, 6000)) return 12815;
        // 13 s later: the window restarts there.
        feed(&t, 1, 1, cmd, stopped(), 27000, 27000, 2000, &r);
        if (r.windowRestartedMask != 1u || t.fans[0].windowStartMs != 27000) return 12816;
        if (feed(&t, 1, 1, cmd, stopped(), 33000, 45000, 6000) ||
            feed(&t, 1, 1, cmd, stopped(), 47000, 47000, 2000) != 47000) return 12817;
    }
    // A new command clears windows but keeps sensor proof.
    {
        FanStallTracker t = {};
        feed(&t, 1, 1, cmd, spinning(), 0, 0, 2000);
        if (feed(&t, 1, 1, cmd, stopped(), 2000, 16000, 2000)) return 12818;
        FanStallReport r = {};
        if (feed(&t, 1, 2, cmd, stopped(), 18000, 36000, 2000, &r)) return 12819;
        if (!t.fans[0].rpmProven ||
            feed(&t, 1, 2, cmd, stopped(), 38000, 38000, 2000, &r) != 38000) return 12820;
    }
    // Another GPU drops the proof: its sensors have proven nothing yet.
    {
        FanStallTracker t = {};
        feed(&t, 1, 1, cmd, spinning(), 0, 0, 2000);
        FanStallReport r = {};
        if (feed(&t, 2, 1, cmd, stopped(), 2000, 120000, 2000, &r) ||
            t.fans[0].rpmProven || r.states[0] != FAN_STALL_FAN_NO_EVIDENCE) return 12821;
    }
    // Multi-fan board: one stopped fan is enough, and it is the one named.
    {
        FanStallTracker t = {};
        FanStallFanSample both[2] = {spinning(), spinning()};
        fan_stall_observe(&t, 1, 1, 0, &cmd, both, 2);
        FanStallFanSample oneStopped[2] = {spinning(), stopped()};
        FanStallReport r = {};
        unsigned long long confirmedAt = 0;
        for (unsigned long long now = 2000; now <= 40000 && !confirmedAt; now += 2000) {
            r = fan_stall_observe(&t, 1, 1, now, &cmd, oneStopped, 2);
            if (r.verdict == FAN_STALL_VERDICT_CONFIRMED) confirmedAt = now;
        }
        if (confirmedAt != 22000 || r.fanIndex != 1 ||
            r.states[0] != FAN_STALL_FAN_SPINNING) return 12822;
        // A fan that disappears from the count closes its window.
        fan_stall_observe(&t, 1, 1, 42000, &cmd, oneStopped, 2);
        r = fan_stall_observe(&t, 1, 1, 44000, &cmd, oneStopped, 1);
        if (r.windowClearedMask != 2u || fan_stall_tracker_has_open_window(&t)) return 12823;
    }
    // Measured duty alone can prove and judge; one proven sensor seeing motion
    // outvotes another reading 0.
    {
        FanStallTracker t = {};
        FanStallFanSample s = spinning();
        s.rpmKnown = false;
        feed(&t, 1, 1, cmd, s, 0, 0, 2000);
        FanStallFanSample measuredStopped = stopped();
        measuredStopped.rpmKnown = false;
        FanStallReport r = {};
        if (feed(&t, 1, 1, cmd, measuredStopped, 2000, 22000, 2000, &r) != 22000 ||
            r.rpmEvidence || !r.measuredEvidence) return 12824;
        FanStallTracker both = {};
        feed(&both, 1, 1, cmd, spinning(), 0, 0, 2000);
        FanStallFanSample tachZero = stopped();
        tachZero.measuredPercent = 31;
        if (feed(&both, 1, 1, cmd, tachZero, 2000, 120000, 2000, &r) ||
            r.states[0] != FAN_STALL_FAN_SPINNING) return 12825;
    }
    // A proven sensor that stops answering mid-window ends the window.
    {
        FanStallTracker t = {};
        feed(&t, 1, 1, cmd, spinning(), 0, 0, 2000);
        feed(&t, 1, 1, cmd, stopped(), 2000, 14000, 2000);
        FanStallFanSample silent = stopped();
        silent.rpmKnown = silent.measuredKnown = false;
        FanStallReport r = {};
        feed(&t, 1, 1, cmd, silent, 16000, 16000, 2000, &r);
        if (r.windowClearedMask != 1u || r.states[0] != FAN_STALL_FAN_NO_EVIDENCE ||
            feed(&t, 1, 1, cmd, stopped(), 18000, 36000, 2000)) return 12826;
    }
    // A clock that runs backwards restarts the window instead of confirming.
    {
        FanStallTracker t = {};
        feed(&t, 1, 1, cmd, spinning(), 100000, 100000, 2000);
        feed(&t, 1, 1, cmd, stopped(), 102000, 118000, 2000);
        FanStallReport r = {};
        if (feed(&t, 1, 1, cmd, stopped(), 50000, 50000, 2000, &r) ||
            r.windowRestartedMask != 1u || t.fans[0].windowStartMs != 50000) return 12827;
    }
    // Sampling cadence.
    {
        FanStallTracker t = {};
        if (!fan_stall_sample_due(&t, 0)) return 12828;
        FanStallFanSample one[1] = {spinning()};
        fan_stall_observe(&t, 1, 1, 10000, &cmd, one, 1);
        if (fan_stall_sample_due(&t, 11999) || !fan_stall_sample_due(&t, 12000) ||
            !fan_stall_sample_due(&t, 9000)) return 12829;
    }
    return 0;
}

} // namespace fan_stall_table

int run_fan_stall_tests() {
    int result = fan_stall_table::run();
    if (result) std::fprintf(stderr, "regression assertion failed: %d\n", result);
    return result;
}
