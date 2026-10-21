// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// What the GUI says when the service's automatic restore is locked out.
//
// A failed automatic logon/standby/driver-recovery apply latches the sticky
// AUTOMATIC_APPLY_FAILED lockout, and until 2026-10 the GUI said nothing about
// it: the only announced reason was HANDBACK_INCOMPLETE.  On the 0.28 field
// machine a cold-boot logon apply failed verification, the apply's recovery
// returned the clocks to stock, automation stayed off -- and the user saw a
// GPU silently running stock until they happened to open the log.
//
// The rule now: the edge INTO a lockout is announced once, with the cause the
// service recorded for it (protocol v29 ServiceAutoApplyFailure), and with an
// action the user can take.  "Returned to stock" is claimed only when the
// apply's own result says its core recovery verified; otherwise the text asks
// for Reset as well.  A Reset's downgrade of HANDBACK_INCOMPLETE to
// AUTOMATIC_APPLY_FAILED is not news and stays silent.
#ifndef GREEN_CURVE_AUTO_APPLY_FAILURE_NOTICE_POLICY_H
#define GREEN_CURVE_AUTO_APPLY_FAILURE_NOTICE_POLICY_H

#include <stddef.h>
#include <string.h>

#include "service_protocol.h"

// The apply's recovery sentence.  The service matches it in the full result
// before truncating the detail for the wire.
#include "apply_recovery_text.h"

enum GcAutoRestoreNotice {
    GC_AUTO_RESTORE_NOTICE_NONE = 0,
    GC_AUTO_RESTORE_NOTICE_HANDBACK_INCOMPLETE = 1,
    GC_AUTO_RESTORE_NOTICE_AUTO_APPLY_FAILED = 2,
    // A driver restart inside the stability window, or too many of them: the
    // profile was deliberately not restored.  Silent until 2026-10-21, so a
    // GPU could run stock with automation off and nothing on screen said so.
    GC_AUTO_RESTORE_NOTICE_UNSTABLE_APPLY = 3,
    GC_AUTO_RESTORE_NOTICE_RECOVERY_SPAM = 4,
};

static inline bool gc_apply_result_reports_core_recovery(const char* result) {
    return result && strstr(result, GC_APPLY_RECOVERY_VERIFIED_TEXT) != nullptr;
}

static inline GcAutoRestoreNotice gc_auto_restore_notice_for_transition(
    gc_u32 previous, gc_u32 current) {
    if (current == previous) return GC_AUTO_RESTORE_NOTICE_NONE;
    if (current == SERVICE_AUTO_RESTORE_LOCKOUT_HANDBACK_INCOMPLETE)
        return GC_AUTO_RESTORE_NOTICE_HANDBACK_INCOMPLETE;
    if (current == SERVICE_AUTO_RESTORE_LOCKOUT_UNSTABLE_APPLY)
        return GC_AUTO_RESTORE_NOTICE_UNSTABLE_APPLY;
    if (current == SERVICE_AUTO_RESTORE_LOCKOUT_TDR_SPAM)
        return GC_AUTO_RESTORE_NOTICE_RECOVERY_SPAM;
    // Only from "not locked out": HANDBACK_INCOMPLETE -> AUTOMATIC_APPLY_FAILED
    // is the user's own successful Reset answering the previous notice.
    if (current == SERVICE_AUTO_RESTORE_LOCKOUT_AUTOMATIC_APPLY_FAILED &&
        previous == SERVICE_AUTO_RESTORE_LOCKOUT_NONE)
        return GC_AUTO_RESTORE_NOTICE_AUTO_APPLY_FAILED;
    return GC_AUTO_RESTORE_NOTICE_NONE;
}

// The notice decision for one adopted snapshot, against the reason THIS client
// last observed (stored in `*observed`, updated here).  Only answered
// snapshots are observed: a disconnect is not a transition to "not locked
// out", so reconnecting to a daemon/service still under the same lockout
// announces nothing new.  GUI and TUI share this rule.
static inline GcAutoRestoreNotice gc_auto_restore_notice_observe(gc_u32* observed,
                                                                 gc_u32 current) {
    if (!observed) return GC_AUTO_RESTORE_NOTICE_NONE;
    const gc_u32 previous = *observed;
    *observed = current;
    return gc_auto_restore_notice_for_transition(previous, current);
}

static inline const char* gc_auto_apply_trigger_phrase(gc_u32 trigger) {
    switch (trigger) {
        case SERVICE_LIFECYCLE_TRIGGER_WTS_LOGON:
        case SERVICE_LIFECYCLE_TRIGGER_TASK_HANDOFF: return "at logon";
        case SERVICE_LIFECYCLE_TRIGGER_STANDBY_RESUME: return "after standby";
        case SERVICE_LIFECYCLE_TRIGGER_DRIVER_RECOVERY: return "after a driver restart";
        case SERVICE_LIFECYCLE_TRIGGER_DAEMON_START: return "at daemon start";
        default: return nullptr;
    }
}

// Bounded concatenation; a cut-off tail ends in "..." so a truncated cause is
// never mistaken for the whole one.  Returns false only on bad arguments.
static inline bool gc_notice_append(char* out, size_t outSize, const char* text) {
    if (!out || outSize == 0) return false;
    if (!text) return true;
    size_t at = strlen(out);
    size_t len = strlen(text);
    if (at + len + 1 <= outSize) {
        memcpy(out + at, text, len + 1);
        return true;
    }
    if (outSize < 4 || at + 4 > outSize) return true;
    size_t room = outSize - 1 - at - 3;
    memcpy(out + at, text, room);
    memcpy(out + at + room, "...", 4);
    return true;
}

// status: the main window's one-line profile status (<= 256).
// title/body: the tray balloon (szInfoTitle 64 / szInfo 256).
// `failure` may be all-zero: that means no cause was recorded.  It can be a
// lockout persisted by an earlier service instance, an explicit Apply failure,
// or one of the service's other safety lockouts.  In that case the text says
// only what is true for all of them -- automation is paused until Apply -- and
// never attributes the lockout to an automatic profile apply.
static inline bool gc_auto_apply_failure_compose(
    const ServiceAutoApplyFailure* failure,
    char* status, size_t statusSize,
    char* title, size_t titleSize,
    char* body, size_t bodySize) {
    if (!status || statusSize == 0 || !title || titleSize == 0 ||
        !body || bodySize == 0) return false;
    status[0] = title[0] = body[0] = '\0';
    const char* when = failure ? gc_auto_apply_trigger_phrase(failure->trigger)
                               : nullptr;
    char detail[sizeof(failure->detail) + 1] = {};
    if (when && failure) {
        memcpy(detail, failure->detail, sizeof(failure->detail));
        detail[sizeof(failure->detail)] = '\0';
    }
    if (!when) {
        gc_notice_append(title, titleSize, "Green Curve: automatic restore disabled");
        gc_notice_append(status, statusSize,
            "Automatic profile restore is paused for safety. "
            "Press Apply to re-enable it.");
        gc_notice_append(body, bodySize,
            "Automatic profile restore is paused for safety. "
            "Open Green Curve and press Apply to re-enable it.");
        return true;
    }
    gc_notice_append(title, titleSize, "Green Curve: automatic apply failed");
    const bool recovered = failure->coreRecovered != 0;
    char lead[128] = {};
    gc_notice_append(lead, sizeof(lead), "Automatic profile apply ");
    gc_notice_append(lead, sizeof(lead), when);
    gc_notice_append(lead, sizeof(lead), recovered
        ? " failed; GPU clocks were returned to stock. Press Apply to retry."
        : " failed. Press Apply to retry, or Reset to return to stock.");
    gc_notice_append(status, statusSize, lead);
    gc_notice_append(body, bodySize, lead);
    if (detail[0]) {
        gc_notice_append(status, statusSize, " Cause: ");
        gc_notice_append(status, statusSize, detail);
        gc_notice_append(body, bodySize, " Cause: ");
        gc_notice_append(body, bodySize, detail);
    }
    return true;
}

// The two driver-restart lockouts carry no recorded cause: the reason itself
// is the whole story.  Same sinks and limits as gc_auto_apply_failure_compose.
// Returns false for any other notice.
static inline bool gc_auto_restore_lockout_compose(
    GcAutoRestoreNotice notice,
    char* status, size_t statusSize,
    char* title, size_t titleSize,
    char* body, size_t bodySize) {
    if (!status || statusSize == 0 || !title || titleSize == 0 ||
        !body || bodySize == 0) return false;
    status[0] = title[0] = body[0] = '\0';
    const char* text = nullptr;
    if (notice == GC_AUTO_RESTORE_NOTICE_UNSTABLE_APPLY) {
        gc_notice_append(title, titleSize, "Green Curve: profile not restored");
        text = "The graphics driver restarted within 10 minutes of applying your "
               "profile, so it was not restored automatically. Press Apply to "
               "re-apply it.";
    } else if (notice == GC_AUTO_RESTORE_NOTICE_RECOVERY_SPAM) {
        gc_notice_append(title, titleSize, "Green Curve: automatic restore paused");
        text = "The graphics driver restarted repeatedly, so automatic profile "
               "restore is paused for safety. Press Apply to re-enable it.";
    } else {
        return false;
    }
    gc_notice_append(status, statusSize, text);
    gc_notice_append(body, bodySize, text);
    return true;
}

#endif // GREEN_CURVE_AUTO_APPLY_FAILURE_NOTICE_POLICY_H
