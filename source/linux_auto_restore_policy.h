// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// linux_auto_restore_policy.h -- when the Linux daemon is allowed to write the
// GPU without a user asking it to.
//
// Windows answers this with the lifecycle worker in auto-restore-policy.md: an
// ordinary service start never writes, a confirmed driver recovery needs ten
// awake minutes of proof, and a sticky lockout dominates every automatic origin
// until an explicit Apply succeeds.  The Linux daemon deliberately keeps its
// `restore-last` boot apply -- it has no logon coordinator, so replaying the
// committed intent is how settings survive a reboot at all -- but that leaves a
// loop Windows does not have:
//
//     start -> replay a setting that hangs the driver -> crash ->
//     systemd Restart= -> start -> replay the same setting -> ...
//
// The guard below is the missing half.  It is pure so both hosts' regression
// harness can drive it, and it holds no clock: attempts are counted per boot,
// not per unit of time, because a crash loop is defined by repetition and a
// wall/monotonic deadline would be a timing assumption.
//
// Ownership of the persisted copy lives in linux_daemon_state.{h,cpp}; this
// header owns only the decisions.

#ifndef GREEN_CURVE_LINUX_AUTO_RESTORE_POLICY_H
#define GREEN_CURVE_LINUX_AUTO_RESTORE_POLICY_H

#include <string.h>
#include "gpu_core.h"

// /proc/sys/kernel/random/boot_id is a 36-character UUID.  One spare byte for
// the terminator, rounded up so the record stays naturally aligned.
enum {
    LINUX_BOOT_ID_MAX = 40,
    // Three automatic start-time writes per boot.  Chosen to match the Windows
    // SCM failure-action count (SC_ACTION_RESTART x3) rather than invented: the
    // fourth consecutive start in one boot is the point at which both platforms
    // stop trying by themselves.
    LINUX_AUTO_RESTORE_MAX_START_ATTEMPTS = 3,
};

enum LinuxAutoRestoreTrigger : gc_u32 {
    LINUX_AUTO_RESTORE_TRIGGER_NONE = 0,
    // The daemon process started and the policy is `restore-last`.
    LINUX_AUTO_RESTORE_TRIGGER_BOOT_RESTORE_LAST = 1,
    // The daemon process started and the policy names an exact profile.
    LINUX_AUTO_RESTORE_TRIGGER_BOOT_PROFILE = 2,
    // The machine came back from suspend/hibernate and the resume unit asked
    // for the in-memory intent to be written again.
    LINUX_AUTO_RESTORE_TRIGGER_RESUME = 3,
};

enum LinuxAutoRestoreVerdict : gc_u32 {
    LINUX_AUTO_RESTORE_ALLOW = 0,
    // A previous run already latched automatic restoration off.
    LINUX_AUTO_RESTORE_DENY_LOCKED_OUT = 1,
    // This boot has already spent its automatic start-time writes.
    LINUX_AUTO_RESTORE_DENY_ATTEMPTS_EXHAUSTED = 2,
    // The caller asked about nothing.
    LINUX_AUTO_RESTORE_DENY_NO_TRIGGER = 3,
};

// Persisted verbatim (see LinuxDaemonRestoreGuardRecord).  Keep POD.
//
// `lockoutReason` is a ServiceAutoRestoreLockoutReason and is part of the
// latched state, not a log detail: it is what populate_snapshot() publishes, so
// losing it across a restart would turn every surviving lockout into the
// generic "automatic recovery apply failed" -- a statement that is simply false
// for the crash-loop arm, where no apply was ever issued.  It is coherent with
// `lockedOut` by construction: locked out implies a non-NONE reason, and clear
// implies NONE.
struct LinuxAutoRestoreGuard {
    gc_bool8 lockedOut;
    gc_u32 startAttempts;
    gc_u32 lockoutReason;
    char bootId[LINUX_BOOT_ID_MAX];
};

// Wire reasons, spelled for a Linux log line.  The Windows service has its own
// copy of these strings (service_auto_restore_lockout_reason_name) phrased for
// its own origins; the two describe the same enum from opposite platforms.
static inline const char* linux_auto_restore_lockout_reason_name(
    gc_u32 reason) {
    switch (reason) {
        case SERVICE_AUTO_RESTORE_LOCKOUT_NONE: return "not locked out";
        case SERVICE_AUTO_RESTORE_LOCKOUT_UNSTABLE_APPLY:
            return "the stored settings did not survive repeated start-time replay";
        case SERVICE_AUTO_RESTORE_LOCKOUT_TDR_SPAM: return "driver restart spam";
        case SERVICE_AUTO_RESTORE_LOCKOUT_AUTOMATIC_APPLY_FAILED:
            return "an automatic write failed at the hardware";
        default: return "unknown safety reason";
    }
}

static inline bool linux_auto_restore_trigger_is_start(
    LinuxAutoRestoreTrigger trigger) {
    return trigger == LINUX_AUTO_RESTORE_TRIGGER_BOOT_RESTORE_LAST ||
           trigger == LINUX_AUTO_RESTORE_TRIGGER_BOOT_PROFILE;
}

static inline const char* linux_auto_restore_trigger_name(
    LinuxAutoRestoreTrigger trigger) {
    switch (trigger) {
        case LINUX_AUTO_RESTORE_TRIGGER_BOOT_RESTORE_LAST: return "boot restore-last";
        case LINUX_AUTO_RESTORE_TRIGGER_BOOT_PROFILE:      return "boot profile";
        case LINUX_AUTO_RESTORE_TRIGGER_RESUME:            return "standby resume";
        default:                                           return "none";
    }
}

static inline const char* linux_auto_restore_verdict_name(
    LinuxAutoRestoreVerdict verdict) {
    switch (verdict) {
        case LINUX_AUTO_RESTORE_ALLOW: return "allowed";
        case LINUX_AUTO_RESTORE_DENY_LOCKED_OUT:
            return "refused: automatic restoration is locked out until an explicit Apply or Reset succeeds";
        case LINUX_AUTO_RESTORE_DENY_ATTEMPTS_EXHAUSTED:
            return "refused: this boot already spent its automatic start-time writes";
        default:
            return "refused: no automatic trigger";
    }
}

// A new boot resets the per-boot attempt count but NOT the lockout.  The
// lockout is sticky across boots exactly as on Windows, because "the settings
// that are on disk kill this driver" does not stop being true because the
// machine rebooted -- that is the loop it exists to break.
static inline bool linux_auto_restore_guard_adopt_boot(
    LinuxAutoRestoreGuard* guard, const char* currentBootId) {
    if (!guard || !currentBootId || !currentBootId[0]) return false;
    bool sameBoot = guard->bootId[0] != '\0' &&
        strncmp(guard->bootId, currentBootId, sizeof(guard->bootId) - 1) == 0;
    if (!sameBoot) {
        guard->startAttempts = 0;
        memset(guard->bootId, 0, sizeof(guard->bootId));
        size_t length = strlen(currentBootId);
        if (length >= sizeof(guard->bootId)) length = sizeof(guard->bootId) - 1;
        memcpy(guard->bootId, currentBootId, length);
        guard->bootId[length] = '\0';
    }
    guard->lockedOut = guard->lockedOut ? 1 : 0;
    return sameBoot;
}

static inline LinuxAutoRestoreVerdict linux_auto_restore_decide(
    const LinuxAutoRestoreGuard* guard, LinuxAutoRestoreTrigger trigger) {
    if (!guard || trigger == LINUX_AUTO_RESTORE_TRIGGER_NONE)
        return LINUX_AUTO_RESTORE_DENY_NO_TRIGGER;
    if (guard->lockedOut) return LINUX_AUTO_RESTORE_DENY_LOCKED_OUT;
    // A resume is a user-visible machine event, not a restart: it cannot repeat
    // by itself, so counting it would only make a laptop stop restoring its
    // curve after the third lid open.  Windows treats standby the same way --
    // it is the one restore that bypasses the stability proof entirely.
    if (!linux_auto_restore_trigger_is_start(trigger))
        return LINUX_AUTO_RESTORE_ALLOW;
    if (guard->startAttempts >= (gc_u32)LINUX_AUTO_RESTORE_MAX_START_ATTEMPTS)
        return LINUX_AUTO_RESTORE_DENY_ATTEMPTS_EXHAUSTED;
    return LINUX_AUTO_RESTORE_ALLOW;
}

// Recorded BEFORE the hardware write and persisted before it is attempted.  A
// crash mid-write must leave the attempt counted; an attempt that is only
// remembered after a successful return counts nothing at all in exactly the
// case this guard exists for.
static inline void linux_auto_restore_note_start_attempt(
    LinuxAutoRestoreGuard* guard) {
    if (!guard) return;
    if (guard->startAttempts < 0xFFFFFFFFu) guard->startAttempts++;
}

// An exhausted boot latches the sticky lockout, so the next boot does not get
// three fresh attempts at the same crash.
//
// The reason is required rather than defaulted: the two arms that latch mean
// materially different things to whoever reads the status (settings that
// crash-loop the daemon versus a driver that refused a write), and the first
// caller to pass "whatever" would collapse them again.  A caller that passes
// NONE is latching a lockout it cannot explain, which is recorded as the
// generic failed-write reason rather than left incoherent.
static inline void linux_auto_restore_note_lockout(
    LinuxAutoRestoreGuard* guard, gc_u32 reason) {
    if (!guard) return;
    guard->lockedOut = 1;
    if (reason == SERVICE_AUTO_RESTORE_LOCKOUT_NONE ||
        reason > SERVICE_AUTO_RESTORE_LOCKOUT_AUTOMATIC_APPLY_FAILED)
        reason = SERVICE_AUTO_RESTORE_LOCKOUT_AUTOMATIC_APPLY_FAILED;
    // First cause wins.  A lockout is cleared only by an explicit Apply/Reset,
    // so a later automatic refusal is a consequence of the original latch and
    // must not overwrite the reason that actually explains it.
    if (guard->lockoutReason == SERVICE_AUTO_RESTORE_LOCKOUT_NONE)
        guard->lockoutReason = reason;
}

// What the snapshot publishes in ServiceSnapshot::autoRestoreLockoutReason.
//
// This exists because the Linux daemon used to derive that field from
// g_stateUncertain alone.  That is a proxy for one of the two latching arms:
// a failed hardware write sets both, but the crash-loop arm
// (LINUX_AUTO_RESTORE_DENY_ATTEMPTS_EXHAUSTED) latches the guard without ever
// reaching a write, so the daemon published LOCKOUT_NONE while having
// permanently stopped restoring settings at boot.  Windows has always fed the
// same field from its real lockout state; this makes Linux answer the question
// it is actually asked.
//
// An uncertain state still reports a lockout even when the guard is clear: the
// rollback or the record did not settle, so no unattended write may proceed
// until a user resolves it, which is exactly what the field means.
static inline gc_u32 linux_auto_restore_published_lockout_reason(
    const LinuxAutoRestoreGuard* guard, bool stateUncertain) {
    if (guard && guard->lockedOut) {
        // A locked-out guard must never publish NONE; a record that says
        // otherwise is incoherent and is reported as the generic reason.
        if (guard->lockoutReason == SERVICE_AUTO_RESTORE_LOCKOUT_NONE ||
            guard->lockoutReason >
                SERVICE_AUTO_RESTORE_LOCKOUT_AUTOMATIC_APPLY_FAILED)
            return SERVICE_AUTO_RESTORE_LOCKOUT_AUTOMATIC_APPLY_FAILED;
        return guard->lockoutReason;
    }
    if (stateUncertain)
        return SERVICE_AUTO_RESTORE_LOCKOUT_AUTOMATIC_APPLY_FAILED;
    return SERVICE_AUTO_RESTORE_LOCKOUT_NONE;
}

// Reaching the ordinary teardown path proves the daemon ran long enough to be
// asked to stop, so the starts that led here were not a loop.
static inline bool linux_auto_restore_note_clean_stop(
    LinuxAutoRestoreGuard* guard) {
    if (!guard || guard->startAttempts == 0) return false;
    guard->startAttempts = 0;
    return true;
}

// Only an explicit, user-originated Apply or Reset re-arms automatic
// restoration.  Automatic success deliberately does not: on Windows that rule
// is what keeps a lockout from being cleared by the very replay that caused it.
static inline bool linux_auto_restore_note_explicit_success(
    LinuxAutoRestoreGuard* guard) {
    if (!guard) return false;
    if (!guard->lockedOut && guard->startAttempts == 0 &&
        guard->lockoutReason == SERVICE_AUTO_RESTORE_LOCKOUT_NONE) return false;
    guard->lockedOut = 0;
    guard->startAttempts = 0;
    // Cleared with the flag, so the record cannot persist a reason for a
    // lockout that is no longer in force.
    guard->lockoutReason = SERVICE_AUTO_RESTORE_LOCKOUT_NONE;
    return true;
}

#endif // GREEN_CURVE_LINUX_AUTO_RESTORE_POLICY_H
