// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// The apply-transition clock witness (F-APPLY-CEILING diagnostics).
//
// WHY THIS EXISTS. The 2026-09-13 driver crash happened in a 1.21 s window
// where the apply had raised the VF curve to 3652 MHz and had not yet pinned
// the clock. The fix arms an NVML locked-clock ceiling before that window. But
// the first test run after the fix could only report "nothing crashed" -- and
// the original crash did not reproduce every time either, so an absence of
// crashes proves nothing on its own. What is missing is evidence that the clamp
// ACTUALLY HELD: the measured GPU clock during the window, next to the ceiling
// that was supposed to be capping it.
//
// It also records the LOAD. The first post-fix test ran at 34-36 C with no game
// running, which was only discoverable afterwards by reading fan-runtime
// temperature lines and inferring. A crash that reproduces only under 3D load
// cannot be investigated with a trace that does not say whether there was load.
//
// WHAT IT DOES NOT DO. It never samples from another thread. Sampling NVML
// concurrently with an in-flight NvAPI VF write would add exactly the kind of
// racy, timing-sensitive behaviour this project refuses -- and in the one code
// path whose stability is under investigation. Every sample is taken
// synchronously between writes, on the apply thread, and the polling hook folds
// into the existing settle loop without changing its timing by so much as one
// sleep.
#ifndef GREEN_CURVE_APPLY_CLOCK_WITNESS_H
#define GREEN_CURVE_APPLY_CLOCK_WITNESS_H

struct ApplyClockSample {
    bool clockValid;
    unsigned int gpcMHz;
    unsigned int smMHz;
    unsigned int memMHz;
    bool tempValid;
    unsigned int tempC;
    bool utilValid;
    unsigned int utilGpuPct;
    unsigned int utilMemPct;
    bool powerValid;
    unsigned int powerMw;
};

struct ApplyClockWitness {
    bool active;
    // 0 when no F-APPLY-CEILING clamp is armed for this apply; the verdict then
    // reports the peak without claiming anything was supposed to hold it.
    unsigned int ceilingMHz;
    bool clampArmed;
    int samples;
    // Judged peak: samples taken while the clamp was actually armed. Only this
    // one reaches the verdict (apply_clock_witness_counts_toward_verdict()).
    unsigned int peakGpcMHz;
    char peakStage[48];
    // What the outgoing profile was running before the clamp went on. Reported
    // for context, never judged -- the clamp was not yet in a position to cap it.
    unsigned int preArmPeakGpcMHz;
    // Peak load seen across the whole apply, so one verdict line answers
    // "was the GPU busy while this ran" without the reader scanning telemetry.
    bool utilKnown;
    unsigned int peakUtilGpuPct;
    unsigned int peakPowerMw;
    unsigned int peakTempC;
};

static ApplyClockWitness g_applyClockWitness = {};

static ApplyClockSample apply_clock_sample_now() {
    ApplyClockSample s = {};
    if (!nvml_ensure_ready()) return s;
    unsigned int mhz = 0;
    if (g_nvml_api.getClock &&
        g_nvml_api.getClock(g_app.nvmlDevice, NVML_CLOCK_GRAPHICS,
                            NVML_CLOCK_ID_CURRENT, &mhz) == NVML_SUCCESS) {
        s.clockValid = true;
        s.gpcMHz = mhz;
        if (g_nvml_api.getClock(g_app.nvmlDevice, NVML_CLOCK_SM,
                                NVML_CLOCK_ID_CURRENT, &mhz) == NVML_SUCCESS)
            s.smMHz = mhz;
        if (g_nvml_api.getClock(g_app.nvmlDevice, NVML_CLOCK_MEM,
                                NVML_CLOCK_ID_CURRENT, &mhz) == NVML_SUCCESS)
            s.memMHz = mhz;
    }
    unsigned int tempC = 0;
    if (g_nvml_api.getTemperature &&
        g_nvml_api.getTemperature(g_app.nvmlDevice, NVML_TEMPERATURE_GPU, &tempC) == NVML_SUCCESS) {
        s.tempValid = true;
        s.tempC = tempC;
    }
    nvmlUtilization_t util = {};
    if (g_nvml_api.getUtilization &&
        g_nvml_api.getUtilization(g_app.nvmlDevice, &util) == NVML_SUCCESS) {
        s.utilValid = true;
        s.utilGpuPct = util.gpu;
        s.utilMemPct = util.memory;
    }
    unsigned int powerMw = 0;
    if (g_nvml_api.getPowerUsage &&
        g_nvml_api.getPowerUsage(g_app.nvmlDevice, &powerMw) == NVML_SUCCESS) {
        s.powerValid = true;
        s.powerMw = powerMw;
    }
    return s;
}

static void apply_clock_witness_fold(const ApplyClockSample* s, const char* stage,
                                     bool sampledAtArmingInstant) {
    ApplyClockWitness* w = &g_applyClockWitness;
    if (!w->active || !s) return;
    w->samples++;
    if (s->clockValid) {
        if (!apply_clock_witness_counts_toward_verdict(w->clampArmed,
                                                       sampledAtArmingInstant)) {
            // Pre-clamp reading: the outgoing profile's clock, kept for context.
            if (s->gpcMHz > w->preArmPeakGpcMHz) w->preArmPeakGpcMHz = s->gpcMHz;
        } else if (s->gpcMHz > w->peakGpcMHz) {
            w->peakGpcMHz = s->gpcMHz;
            StringCchCopyA(w->peakStage, ARRAY_COUNT(w->peakStage), stage ? stage : "?");
        }
    }
    if (s->utilValid) {
        w->utilKnown = true;
        if (s->utilGpuPct > w->peakUtilGpuPct) w->peakUtilGpuPct = s->utilGpuPct;
    }
    if (s->powerValid && s->powerMw > w->peakPowerMw) w->peakPowerMw = s->powerMw;
    if (s->tempValid && s->tempC > w->peakTempC) w->peakTempC = s->tempC;
}

static void apply_clock_witness_format(const ApplyClockSample* s, char* out, size_t outSize) {
    if (!out || !outSize) return;
    out[0] = 0;
    if (!s) return;
    char clocks[96] = {};
    if (s->clockValid) {
        StringCchPrintfA(clocks, ARRAY_COUNT(clocks), "gpc=%u MHz sm=%u mem=%u",
            s->gpcMHz, s->smMHz, s->memMHz);
    } else {
        StringCchCopyA(clocks, ARRAY_COUNT(clocks), "gpc=unknown");
    }
    char load[128] = {};
    StringCchPrintfA(load, ARRAY_COUNT(load), "util=%s%u%%/%u%% power=%s%u.%01u W temp=%s%u C",
        s->utilValid ? "" : "unknown:", s->utilGpuPct, s->utilMemPct,
        s->powerValid ? "" : "unknown:", s->powerMw / 1000, (s->powerMw % 1000) / 100,
        s->tempValid ? "" : "unknown:", s->tempC);
    StringCchPrintfA(out, outSize, "%s %s", clocks, load);
}

// Start a witness for one apply.  `ceilingMHz` is the F-APPLY-CEILING target
// (0 when none), `clampArmed` whether it was actually accepted by the driver.
static void apply_clock_witness_begin(unsigned int ceilingMHz, bool clampArmed) {
    ApplyClockWitness* w = &g_applyClockWitness;
    memset(w, 0, sizeof(*w));
    w->active = true;
    w->ceilingMHz = ceilingMHz;
    w->clampArmed = clampArmed;
}

// Update what the guard ended up doing; arming can fail after the witness
// started, and the verdict must not claim a clamp that never landed.
static void apply_clock_witness_set_clamp(unsigned int ceilingMHz, bool clampArmed) {
    g_applyClockWitness.ceilingMHz = ceilingMHz;
    g_applyClockWitness.clampArmed = clampArmed;
}

// Take one sample, fold it into the high-water marks, and log it.  Used at the
// named transitions; the critical one is immediately after the curve batch.
// `judged=` in the line says whether this reading reaches the verdict, so a
// reader never has to guess why a high sample did not trip it.
static void apply_clock_witness_record_ex(const char* stage,
                                          bool sampledAtArmingInstant) {
    if (!g_applyClockWitness.active) return;
    ApplyClockSample s = apply_clock_sample_now();
    apply_clock_witness_fold(&s, stage, sampledAtArmingInstant);
    char detail[256] = {};
    apply_clock_witness_format(&s, detail, sizeof(detail));
    debug_log("apply clock witness [%s]: %s (ceiling=%u MHz armed=%d judged=%d)\n",
        stage ? stage : "?", detail, g_applyClockWitness.ceilingMHz,
        g_applyClockWitness.clampArmed ? 1 : 0,
        apply_clock_witness_counts_toward_verdict(g_applyClockWitness.clampArmed,
                                                  sampledAtArmingInstant) ? 1 : 0);
}

// A sample at a point where the clamp, if armed, has been in force for the
// whole interval since the previous write.
static void apply_clock_witness_record(const char* stage) {
    apply_clock_witness_record_ex(stage, false);
}

// The sample taken in the same instant the arming call returns: on record for
// context, never judged.  See apply_clock_witness_counts_toward_verdict().
static void apply_clock_witness_record_at_arming(const char* stage) {
    apply_clock_witness_record_ex(stage, true);
}

// Silent sample, for the existing settle loop: the uncapped window is only
// ~175 ms wide, and one sample at each end of it can miss a spike in between.
// Adds an NVML read per iteration the loop was already making; it adds no
// sleep, no attempt, and no branch that can change what the loop returns.
static void apply_clock_witness_poll(const char* stage) {
    if (!g_applyClockWitness.active) return;
    ApplyClockSample s = apply_clock_sample_now();
    apply_clock_witness_fold(&s, stage, false);
}

// The verdict line: the single fact the next under-load test needs to produce.
static void apply_clock_witness_end() {
    ApplyClockWitness* w = &g_applyClockWitness;
    if (!w->active) return;
    ApplyClockWitnessVerdict verdict = apply_clock_witness_verdict(
        w->ceilingMHz != 0, w->clampArmed, w->ceilingMHz, w->peakGpcMHz);
    bool loadMeaningful =
        apply_clock_witness_load_is_meaningful(w->utilKnown, w->peakUtilGpuPct);
    debug_log("apply clock witness verdict: peak gpc=%u MHz (at %s) vs ceiling=%u MHz"
              " armed=%d -> %s; preArmPeak=%u MHz; load peak util=%s%u%%"
              " power=%u.%01u W temp=%u C loadMeaningful=%d samples=%d\n",
        w->peakGpcMHz, w->peakStage[0] ? w->peakStage : "-", w->ceilingMHz,
        w->clampArmed ? 1 : 0, apply_clock_witness_verdict_name(verdict),
        w->preArmPeakGpcMHz,
        w->utilKnown ? "" : "unknown:", w->peakUtilGpuPct,
        w->peakPowerMw / 1000, (w->peakPowerMw % 1000) / 100,
        w->peakTempC, loadMeaningful ? 1 : 0, w->samples);
    if (verdict == APPLY_CLOCK_WITNESS_EXCEEDED) {
        debug_log("apply clock witness: WARNING the GPU reached %u MHz while a %u MHz"
                  " clamp was armed (+%u MHz, beyond the %d MHz clock-bin tolerance)."
                  " F-APPLY-CEILING does not hold on this driver and the transition"
                  " is still uncapped\n",
            w->peakGpcMHz, w->ceilingMHz, w->peakGpcMHz - w->ceilingMHz,
            (int)APPLY_CLOCK_BIN_TOLERANCE_MHZ);
    }
    if (w->clampArmed && !loadMeaningful) {
        debug_log("apply clock witness: NOTE peak GPU utilisation was %s%u%% — this"
                  " apply ran with little or no 3D load, so a clean result here does"
                  " NOT clear the under-load case\n",
            w->utilKnown ? "" : "unknown:", w->peakUtilGpuPct);
    }
    w->active = false;
}

// Scope guard so no apply exit path can leave the witness armed for the next
// one; a stale `active` would make the telemetry settle loop sample forever.
struct ApplyClockWitnessScope {
    ApplyClockWitnessScope(unsigned int ceilingMHz, bool clampArmed) {
        apply_clock_witness_begin(ceilingMHz, clampArmed);
    }
    ~ApplyClockWitnessScope() { apply_clock_witness_end(); }
};

#endif
