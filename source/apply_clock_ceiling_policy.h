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

// Why a transition needs a temporary clamp.  Kept explicit because the two
// reasons have different ceiling values and different release rules, and
// because a log that only says "armed" cannot be used to tell an intentional
// no-clamp apply from one whose protection was never required in the first
// place.
enum ApplyClockCeilingReason {
    // Nothing this apply writes can put the GPU above what it is already
    // entitled to run.
    APPLY_CEILING_REASON_NONE = 0,
    // The request names a lock target.  That target is a ceiling the user has
    // already validated, so it bounds the transition too.
    APPLY_CEILING_REASON_REQUESTED_LOCK,
    // The apply resets to stock first, and the outgoing state was being held
    // DOWN by something the reset removes -- a FLATTEN tail floor, a negative
    // offset, an old pin.  Stock is above both endpoints for the whole window
    // between the reset and the new curve write.  This is the case the
    // lock-only predicate used to miss entirely (audit finding CT-05): an
    // unpinned undervolt switching to another unpinned undervolt got no clamp
    // at all, because neither profile named a lock.
    APPLY_CEILING_REASON_RESET_DROPS_CAP,
};

static inline const char* apply_clock_ceiling_reason_name(
    ApplyClockCeilingReason r) {
    switch (r) {
        case APPLY_CEILING_REASON_REQUESTED_LOCK: return "requested lock target";
        case APPLY_CEILING_REASON_RESET_DROPS_CAP:
            return "reset-to-stock removes the outgoing cap";
        default: return "none";
    }
}

// The ceiling clamp shape.  A transition guard wants a CEILING, not a pin:
// nvmlDeviceSetGpuLockedClocks(0, ceiling) caps without also forcing the clock
// up at idle (the same thing `nvidia-smi --lock-gpu-clocks=0,N` asks for).  A
// driver that refuses a 0 minimum still accepts (ceiling, ceiling), which caps
// correctly and merely adds the floor -- but that floor is only safe when the
// value is one BOTH endpoints already permit, so it is gated rather than
// unconditional (see `symmetricFallbackAllowed`).
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
    // This transition CANNOT be performed safely without a clamp.  Distinct
    // from `arm`, which additionally requires the clamp to be installable:
    // `required && !arm` is a transition that must be refused before it
    // mutates anything, not one that may quietly proceed unprotected.  The
    // pre-fix code had no such distinction -- a missing NVML entry point
    // turned "protection required" into "no plan", and the apply ran the
    // uncapped sequence the whole mechanism exists to prevent (CT-01).
    bool required;
    // Whether the (ceiling, ceiling) fallback form may be used when the driver
    // refuses the open-ended one.  The symmetric form adds a FLOOR, so it is
    // only admissible at a value both the outgoing and the incoming state
    // already permit -- true when the request names its own lock, false for a
    // clamp derived purely from the outgoing state, where forcing an unpinned
    // profile's idle clock up would be a new restriction the user never asked
    // for.
    bool symmetricFallbackAllowed;
    ApplyClockCeilingReason reason;
};

// `requestOwnsClockDomain` is the caller's existing "this request replaces the
// VF/lock domain" answer (Windows: service_request_replaces_lock_domain()).  A
// sparse fan/memory/power request writes nothing that can raise a clock, so it
// neither needs nor may install a clamp on a domain it does not own.
//
// The three trailing arguments describe the state the apply is leaving, and
// default to "nothing known", which reproduces the original lock-only
// behaviour for callers that cannot supply them:
//   `resetsToStock`                 -- this apply runs reset-to-stock first.
//   `outgoingStateHoldsClocksDown`  -- the live state has a negative offset,
//                                      a flatten floor or a pin, i.e. the
//                                      reset RAISES the GPU on its way through
//                                      stock.
//   `outgoingCeilingMHz`            -- the highest clock the outgoing profile
//                                      was entitled to run: its pin if it had
//                                      one, otherwise its live curve peak.
//
// The bound is the LOWER of the two applicable ceilings.  A clamp at the
// minimum of old and new can violate neither endpoint's intent -- both were
// already running at or below it -- and it is what makes a low-pin -> high-pin
// switch safe: the old low pin keeps holding until the new curve exists, and
// only then does the final lock step raise the ceiling to what was asked for.
// Taking the incoming value alone would relax the old pin onto the old curve.
static inline ApplyClockCeilingPlan apply_clock_ceiling_plan(
    bool requestOwnsClockDomain, bool requestHasLock, int lockMode,
    unsigned int lockMHz, bool nvmlLockedClocksAvailable,
    bool resetsToStock = false, bool outgoingStateHoldsClocksDown = false,
    unsigned int outgoingCeilingMHz = 0) {
    ApplyClockCeilingPlan plan = {};
    if (!requestOwnsClockDomain) return plan;

    const bool incomingLock =
        requestHasLock && lockMode != LOCK_MODE_NONE && lockMHz > 0;
    const bool resetWillUncap = resetsToStock && outgoingStateHoldsClocksDown;
    if (!incomingLock && !resetWillUncap) return plan;

    plan.required = true;
    plan.reason = incomingLock ? APPLY_CEILING_REASON_REQUESTED_LOCK
                               : APPLY_CEILING_REASON_RESET_DROPS_CAP;

    unsigned int bound = incomingLock ? lockMHz : 0;
    if (outgoingCeilingMHz > 0 && (bound == 0 || outgoingCeilingMHz < bound))
        bound = outgoingCeilingMHz;
    // Required, but there is no number this code can defend.  Arming at a
    // guess would be worse than refusing: it would either fail to cap or
    // impose a limit nobody asked for, and in both cases the log would claim
    // the transition was protected.
    if (bound == 0) return plan;

    plan.ceilingMHz = bound;
    plan.finalPinIsCeiling =
        incomingLock && lockMode == LOCK_MODE_HARD && bound == lockMHz;
    plan.symmetricFallbackAllowed = incomingLock;
    plan.arm = nvmlLockedClocksAvailable;
    return plan;
}

// What actually happened at the arming call site.  `arm()` used to return
// void, so "the driver refused the clamp" and "no clamp was needed" reached
// the apply as the same non-event.
enum ApplyClockCeilingArmResult {
    APPLY_CEILING_ARM_NOT_NEEDED = 0,
    APPLY_CEILING_ARM_INSTALLED,
    // No NVML entry points, or no defensible ceiling value.  Nothing was
    // written to the driver.
    APPLY_CEILING_ARM_UNAVAILABLE,
    // Every permitted clamp form was rejected by the driver.  A write WAS
    // attempted, so the caller must still treat the operation as having
    // touched the hardware.
    APPLY_CEILING_ARM_REFUSED,
};

// Whether the transition must be refused before it mutates anything.
//
// This is the executable form of the rule the audit's CT-01 is about: a
// required protection that could not be established is a reason to stop, not
// a reason to continue and log about it.  Note what it does NOT say -- an
// apply whose protection was never required proceeds exactly as before, which
// is what keeps default read/write support for unprobeable and unsupported
// GPUs intact.  Only the specific transition that cannot be made safe fails.
static inline bool apply_clock_ceiling_transition_must_refuse(
    bool required, ApplyClockCeilingArmResult result) {
    if (!required) return false;
    return result != APPLY_CEILING_ARM_INSTALLED;
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
// A sample may be judged against the ceiling only once the clamp is armed AND
// the driver has actually been in a position to act on it. Two sample sites
// fail that, and both were caught by real runs rather than by reasoning:
//
//  - `apply entry` is taken before the clamp exists at all, deliberately, so the
//    outgoing profile's clock is on record. Two of four under-load runs on
//    2026-09-13 peaked there (2932 MHz against a 2957 MHz ceiling) -- HELD, but
//    a slightly higher outgoing profile would have reported EXCEEDED for a clock
//    the clamp was never in a position to cap.
//  - `ceiling armed` is taken in the same instant the arming call returns. The
//    clock it reads is still the pre-clamp one; no clamp takes effect in the
//    microseconds between an NVML write returning and the next statement. Three
//    of seven full-load runs later the same day were judged on this sample, and
//    all three read exactly their pre-arm value (2932, 2917, 2902 MHz). They
//    passed only because the outgoing clocks happened to sit under the incoming
//    ceiling; a profile switch that LOWERS the ceiling would have cried wolf.
//
// The rule is structural, not temporal: it names which sample SITE is
// simultaneous with the write. A "wait a moment before judging" rule would be a
// timing assumption, which is exactly what this project refuses -- and it would
// be wrong anyway, because the honest statement is that the sample is
// contemporaneous with the arming write, not that it is merely early.
static inline bool apply_clock_witness_counts_toward_verdict(
    bool clampArmed, bool sampledAtArmingInstant, bool transitionFinished = false) {
    return clampArmed && !sampledAtArmingInstant && !transitionFinished;
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

// An abandoned guard has no proof that release is safe. Finalization and
// recovery explicitly release the restriction after verification instead.
static inline bool apply_clock_ceiling_release_on_abandon(bool armed, bool adopted) {
    (void)armed;
    (void)adopted;
    return false;
}

// ---------------------------------------------------------------------------
// The release-on-recovery rule.
//
// Separate from the rule above because it answers a different question.  The
// abandon rule asks "did the final lock step take ownership".  This one asks
// "is the hardware actually back in a state that does not need the clamp".
//
// THE BUG THIS EXISTS FOR (source-confirmed 2026-09-16, CT-04/CT-07):
// rollback_to_safe_defaults() discarded the return value of every reset it
// performed and then called nvmlDeviceResetGpuLockedClocks() unconditionally.
// service_reset_all() did the same and logged a failed unlock as "may be
// benign".  Linux's rollback restored the previous (possibly raised, possibly
// pinned) curve and then unlocked unconditionally too.  In each case a failed
// curve or offset reset produced exactly the shape the transition clamp exists
// to prevent: a raised VF curve with nothing capping it.  Worse, on Windows
// the rollback runs AFTER the guard has already decided to retain() the clamp,
// so the rollback silently undid the guard's decision.
//
// A restriction may only be released once the state it was protecting against
// is provably gone.  If it is not, keeping an unrequested cap is the strictly
// better failure: it is visible, it is reportable, and it does not crash.
// ---------------------------------------------------------------------------

// Per-domain outcome of a recovery attempt.  `attempted` matters independently
// of `verified`: a write that was issued and failed may have changed hardware,
// which is precisely the case the old success/fail counters could not express.
struct ApplyRecoveryDomain {
    bool attempted;
    bool verified;
};

static inline bool apply_recovery_domain_is_uncertain(
    const ApplyRecoveryDomain& d) {
    return d.attempted && !d.verified;
}

// The domains whose state decides whether a clock restriction may be lifted.
// Memory, power and fan are deliberately absent: none of them can leave the
// GPU running a higher CORE clock than intended, which is the only thing the
// clamp bounds.  Including them would make a failed fan write strand a cap.
struct ApplyRecoveryResult {
    ApplyRecoveryDomain curve;
    ApplyRecoveryDomain gpuOffset;
    // True once the caller has proved the restriction itself is gone.
    bool restrictionReleased;
};

// Whether recovery reached a state in which the clock restriction may go.
//
// Fresh verified state is required, including when a write was unnecessary.
static inline bool apply_recovery_permits_release(
    const ApplyRecoveryResult& r) {
    return r.curve.verified && r.gpuOffset.verified;
}

// Whether the user must be told that a restriction they did not ask for is
// still in force.  A silent cap is indistinguishable from a broken GPU.
static inline bool apply_recovery_must_report_retained_cap(
    const ApplyRecoveryResult& r, bool restrictionWasActive) {
    return restrictionWasActive && !r.restrictionReleased;
}

#endif
