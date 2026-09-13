// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT

#ifndef GREEN_CURVE_APPLY_CLOCK_CEILING_POLICY_H
#define GREEN_CURVE_APPLY_CLOCK_CEILING_POLICY_H

// F-APPLY-CEILING -- the transition clock ceiling an Apply must hold while it
// rewrites the VF curve.
//
// THE BUG THIS EXISTS FOR (measured, 2026-09-13 13:43, RTX 50-series/Blackwell,
// captured in greencurve_debug.txt and confirmed by nvlddmkm event 153):
//
//   Profile 3 was active with a FLATTEN lock at 2957 MHz.  Flatten holds the
//   ceiling with the VF curve itself (a uniform floor offset on every tail
//   point), so NO NVML locked-clock pin was armed -- the apply that established
//   profile 3 had explicitly called nvmlDeviceResetGpuLockedClocks().
//
//   Switching to profile 4 (HARD pin at the same 2957 MHz) while a game was
//   rendering then ran this sequence:
//     13:43:53.0  reset-to-stock: every curve offset -> 0.  The flatten floor,
//                 the only thing capping the tail, is gone.
//     13:43:54.2  1000 ms settle at the stock curve.
//     13:43:55.2  curve batch writes the new profile: +475 MHz selective offset
//                 on points 70..126.  Because the requested mode is HARD (not
//                 FLATTEN) the tail points are NOT floored -- they carry the
//                 full +475 MHz.  Post-write readback: ci76=2947 MHz rising
//                 monotonically to ci126=3637 MHz, every one of them with
//                 freqOffs=475000.
//     13:43:56.4  nvmlDeviceSetGpuLockedClocks(2957, 2957) -- the pin finally
//                 lands, 1.21 s after the curve went up.
//     13:43:57    nvlddmkm event 153.
//
//   For 1.21 s under game load the GPU ran a curve topping out at 3637 MHz with
//   NOTHING capping it, at 100% power and +3000 MHz memory.  2957 MHz is stable
//   on this board; 3637 MHz is not.  The clocks were never the problem -- the
//   ORDER of the writes was.
//
//   The natural control experiment is in the same log: the 13:42:42 apply was
//   HARD -> HARD, so the previous profile's pin was still armed while the same
//   curve batch ran, and it did not crash.  Only the unpinned transition did.
//
// THE RULE: an Apply may never raise the VF curve while the GPU is uncapped.
// When the incoming request declares a lock target, that target is a ceiling
// the user has already validated, so arm it as an NVML locked-clock clamp
// BEFORE the first clock-affecting write of the apply and hold it until the
// final lock state is established.  Arming it costs one NVML call and cannot
// violate the request's own intent: the apply is going to end at or below that
// ceiling by definition.
//
// This header is pure so both platform backends and the regression suite share
// one decision; the hardware calls live in the backends.

// The ceiling clamp shape.  A transition guard wants a CEILING, not a pin:
// nvmlDeviceSetGpuLockedClocks(0, ceiling) caps without also forcing the clock
// up at idle (the same thing `nvidia-smi --lock-gpu-clocks=0,N` asks for).  A
// driver that refuses a 0 minimum still accepts (ceiling, ceiling), which caps
// correctly and merely adds the floor -- strictly better than no clamp at all,
// so it is the documented fallback rather than a reason to give up.
struct ApplyClockCeilingPlan {
    // Arm a transition clamp before any clock-affecting write in this apply.
    bool arm;
    // The clamp value in MHz (0 when `arm` is false).
    unsigned int ceilingMHz;
    // The apply ends at a HARD pin at exactly this value, so the final lock
    // step re-asserts the same number and the guard needs no separate release.
    // When false the final step releases the clamp (the flatten tail, already
    // written by then, becomes the ceiling).
    bool finalPinIsCeiling;
};

// `requestOwnsClockDomain` is the caller's existing "this request replaces the
// VF/lock domain" answer (Windows: service_request_replaces_lock_domain()).  A
// sparse fan/memory/power request writes nothing that can raise a clock, so it
// neither needs nor may install a clamp on a domain it does not own.
static inline ApplyClockCeilingPlan apply_clock_ceiling_plan(
    bool requestOwnsClockDomain, bool requestHasLock, int lockMode,
    unsigned int lockMHz, bool nvmlLockedClocksAvailable) {
    ApplyClockCeilingPlan plan = {};
    if (!requestOwnsClockDomain) return plan;
    if (!requestHasLock) return plan;
    if (lockMode == LOCK_MODE_NONE) return plan;
    if (lockMHz == 0) return plan;
    if (!nvmlLockedClocksAvailable) return plan;
    plan.arm = true;
    plan.ceilingMHz = lockMHz;
    plan.finalPinIsCeiling = (lockMode == LOCK_MODE_HARD);
    return plan;
}

// ---------------------------------------------------------------------------
// The witness verdict: did the armed clamp actually hold?
//
// The first post-fix test run could only report "nothing crashed", and the
// original crash did not reproduce every time either, so that is not evidence.
// The backends sample the live GPU clock across the apply and feed the peak
// here; this is the rule behind the one log line that answers the question.
// ---------------------------------------------------------------------------

// NVIDIA rounds a locked-clock request to a supported clock bin, so a clamp
// asked for at 2957 MHz can legitimately settle a bin above it. Blackwell's VF
// table steps in 15 MHz increments (visible in any curve readback: 2947, 2977,
// 2992, 3022...), so one bin is the honest tolerance between "the driver
// rounded" and "the clamp is not holding". A wider fudge factor here would hide
// exactly the failure this witness exists to catch.
enum { APPLY_CLOCK_BIN_TOLERANCE_MHZ = 15 };

// A GPU this idle tells you nothing about a crash that needs 3D load. The
// threshold only decides whether the log says so out loud; it gates no
// behaviour.
enum { APPLY_CLOCK_WITNESS_LOAD_PCT = 20 };

// Which high-water mark a sample belongs to.
//
// The witness takes its first sample at `apply entry`, BEFORE the clamp is
// armed -- deliberately, because the outgoing profile's clock is worth knowing.
// But that reading says nothing about whether the clamp holds, and folding it
// into the judged peak makes the verdict lie: switching away from an unpinned
// profile that was boosting to 3600 MHz would report EXCEEDED against the
// incoming ceiling for a clock the clamp was never in a position to cap.
// Observed as a live near-miss on 2026-09-13: two of four under-load runs
// peaked at `apply entry` (2932 MHz), just under a 2957 MHz ceiling. A slightly
// higher outgoing profile would have produced a false alarm.
static inline bool apply_clock_witness_counts_toward_verdict(bool clampArmed) {
    return clampArmed;
}

enum ApplyClockWitnessVerdict {
    APPLY_CLOCK_WITNESS_NO_CLAMP = 0,   // none requested; nothing to hold
    APPLY_CLOCK_WITNESS_ARM_FAILED,     // requested, driver refused it
    APPLY_CLOCK_WITNESS_UNKNOWN,        // armed, but no clock reading answered
    APPLY_CLOCK_WITNESS_HELD,           // peak at or below the ceiling
    APPLY_CLOCK_WITNESS_HELD_ROUNDED,   // one bin above: the driver rounded
    APPLY_CLOCK_WITNESS_EXCEEDED,       // the clamp did not hold
};

static inline ApplyClockWitnessVerdict apply_clock_witness_verdict(
    bool clampRequested, bool clampArmed, unsigned int ceilingMHz,
    unsigned int peakGpcMHz) {
    if (!clampRequested || ceilingMHz == 0) return APPLY_CLOCK_WITNESS_NO_CLAMP;
    if (!clampArmed) return APPLY_CLOCK_WITNESS_ARM_FAILED;
    if (peakGpcMHz == 0) return APPLY_CLOCK_WITNESS_UNKNOWN;
    if (peakGpcMHz <= ceilingMHz) return APPLY_CLOCK_WITNESS_HELD;
    if (peakGpcMHz <= ceilingMHz + (unsigned int)APPLY_CLOCK_BIN_TOLERANCE_MHZ)
        return APPLY_CLOCK_WITNESS_HELD_ROUNDED;
    return APPLY_CLOCK_WITNESS_EXCEEDED;
}

static inline const char* apply_clock_witness_verdict_name(
    ApplyClockWitnessVerdict v) {
    switch (v) {
        case APPLY_CLOCK_WITNESS_NO_CLAMP: return "no clamp requested";
        case APPLY_CLOCK_WITNESS_ARM_FAILED: return "NO CLAMP (arming failed)";
        case APPLY_CLOCK_WITNESS_UNKNOWN: return "UNKNOWN (no clock reading)";
        case APPLY_CLOCK_WITNESS_HELD: return "HELD";
        case APPLY_CLOCK_WITNESS_HELD_ROUNDED: return "HELD (driver rounded up one bin)";
        default: return "EXCEEDED";
    }
}

// Whether the run carried enough 3D load for a clean verdict to mean anything.
static inline bool apply_clock_witness_load_is_meaningful(bool utilKnown,
                                                          unsigned int peakUtilPct) {
    return utilKnown && peakUtilPct >= (unsigned int)APPLY_CLOCK_WITNESS_LOAD_PCT;
}

// Whether a guard that was armed but never adopted by the final lock step must
// be released before the apply returns.
//
// Every abandoning EXIT in the Windows apply happens before the first
// clock-RAISING write -- the reset-to-stock write and the pre-write validations
// are the only things ahead of it -- so at that point the live curve is stock or
// lower, and a clamp nobody asked for would silently cap a GPU with no UI
// showing why.  It goes.
//
// The apply is responsible for calling retain() on the one fall-through that
// does NOT satisfy that: a lock point the visible map could not resolve clears
// `hasLock` after the plan was made, while a selective offset still reaches the
// curve batch.  retain() marks the guard adopted, which is what keeps this
// predicate honest rather than merely optimistic.
static inline bool apply_clock_ceiling_release_on_abandon(bool armed,
                                                          bool adopted) {
    return armed && !adopted;
}

#endif
