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
};

static inline bool gc_apply_result_reports_core_recovery(const char* result) {
    return result && strstr(result, GC_APPLY_RECOVERY_VERIFIED_TEXT) != nullptr;
}

static inline GcAutoRestoreNotice gc_auto_restore_notice_for_transition(
    gc_u32 previous, gc_u32 current) {
    if (current == previous) return GC_AUTO_RESTORE_NOTICE_NONE;
    if (current == SERVICE_AUTO_RESTORE_LOCKOUT_HANDBACK_INCOMPLETE)
        return GC_AUTO_RESTORE_NOTICE_HANDBACK_INCOMPLETE;
    // Only from "not locked out": HANDBACK_INCOMPLETE -> AUTOMATIC_APPLY_FAILED
    // is the user's own successful Reset answering the previous notice.
    if (current == SERVICE_AUTO_RESTORE_LOCKOUT_AUTOMATIC_APPLY_FAILED &&
        previous == SERVICE_AUTO_RESTORE_LOCKOUT_NONE)
        return GC_AUTO_RESTORE_NOTICE_AUTO_APPLY_FAILED;
    return GC_AUTO_RESTORE_NOTICE_NONE;
}

static inline const char* gc_auto_apply_trigger_phrase(gc_u32 trigger) {
    switch (trigger) {
        case SERVICE_LIFECYCLE_TRIGGER_WTS_LOGON:
        case SERVICE_LIFECYCLE_TRIGGER_TASK_HANDOFF: return "at logon";
        case SERVICE_LIFECYCLE_TRIGGER_STANDBY_RESUME: return "after standby";
        case SERVICE_LIFECYCLE_TRIGGER_DRIVER_RECOVERY: return "after a driver restart";
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
// `failure` may be all-zero (the lockout predates this service instance or has
// another cause): the text then says only what still holds -- automation is
// paused until Apply.
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
    gc_notice_append(title, titleSize, "Green Curve: automatic apply failed");
    if (!when) {
        gc_notice_append(status, statusSize,
            "Automatic profile restore is paused after a failed automatic apply. "
            "Press Apply to re-enable it.");
        gc_notice_append(body, bodySize,
            "Automatic profile restore is paused after a failed automatic apply. "
            "Open Green Curve and press Apply to re-enable it.");
        return true;
    }
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

#endif // GREEN_CURVE_AUTO_APPLY_FAILURE_NOTICE_POLICY_H
