// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// Which personal profile slot the tray menu ticks, and the read that decision
// is allowed to be made from.
//
// `[profiles] applied_slot` is a GUI cache of service-owned profile identity.
// The service names the active `ServiceProfileSource` plus slot; the GUI then
// confirms that the slot's stored record still describes what is running, so a
// slot the user has since edited stops claiming to be applied.
//
// The confirming read MUST be drift-free.  This is the second time the applied
// indicator has been broken by live hardware readback leaking into it:
//
//   1. The original defect compared the saved absolute VF MHz against live
//      readback.  NVIDIA moves the reported curve by several MHz with
//      temperature and boost state, so the indicator cleared itself at idle.
//      That comparison was removed (see config-profiles.md, "Ownership versus
//      live drift").
//   2. The same dependency then came back *indirectly*.
//      `load_profile_from_config()` projects a stored profile onto the current
//      GPU before returning it -- it drops points the live curve says are not
//      visible, and re-derives `lockTracksAnchor` from
//      `curve_base_khz_for_point()`, which is
//      `g_app.curve[i].freq_kHz - g_app.freqOffsets[i]`, i.e. live readback.
//      Feeding that into the ownership comparison made the verdict flip with
//      ordinary boost drift, and because none of it is part of the sync's
//      cache key the flip was not noticed when it happened: the next event
//      that *did* invalidate the cache -- most often a plain profile Load,
//      which writes `selected_slot` -- applied the stale verdict and wrote
//      `applied_slot = 0`.  The tick then stayed gone until the next Apply.
//
// Hence `PROFILE_READ_FOR_OWNERSHIP`.  Projection onto the live curve is right
// for the editor and for anything on its way to the hardware; it is never right
// for deciding whether a stored record and a stored intent describe the same
// thing.  Both sides of that comparison are records, and records do not drift.

#ifndef GREEN_CURVE_APPLIED_PROFILE_INDICATOR_POLICY_H
#define GREEN_CURVE_APPLIED_PROFILE_INDICATOR_POLICY_H

#include "service_protocol.h"

// How a stored profile is being read back.
enum ProfileReadMode {
    // Historical behaviour, and the default: the profile is going into the
    // editor or to the hardware, so points the current GPU cannot show are
    // dropped and the lock anchor is re-derived from the live curve.
    PROFILE_READ_FOR_EDITOR = 0,
    // The profile is only being compared with the service's active intent.
    // Every live-readback-dependent projection is skipped so the answer depends
    // on the config file and the GPU's VF topology alone -- never on what the
    // curve happens to read this second.
    PROFILE_READ_FOR_OWNERSHIP = 1,
};

// Why the indicator ended up where it did.  Carried into the debug log so a
// support report answers "which branch cleared my tick" without a rebuild.
enum AppliedProfileIndicatorReason {
    APPLIED_PROFILE_REASON_NO_AUTHORITY = 0,   // no accepted service snapshot
    APPLIED_PROFILE_REASON_NOT_A_USER_SLOT,    // ad-hoc, shared, machine, none
    APPLIED_PROFILE_REASON_NO_ACTIVE_INTENT,   // service owns nothing readable
    APPLIED_PROFILE_REASON_PROFILE_UNREADABLE, // slot gone or malformed
    APPLIED_PROFILE_REASON_PROFILE_EDITED,     // stored record no longer matches
    APPLIED_PROFILE_REASON_APPLIED,            // the tick is legitimate
};

struct AppliedProfileIndicatorInputs {
    // An accepted, coherent service snapshot is the only thing that may assert
    // ownership.  Without one the indicator is cleared rather than guessed.
    bool serviceAuthoritative;
    // The service published a readable active intent alongside its identity.
    bool activeDesiredValid;
    // ServiceProfileSource + slot exactly as the service published them.
    unsigned int profileSource;
    unsigned int profileSlot;
    int maxSlots;
    // Results of the DRIFT-FREE ownership read (PROFILE_READ_FOR_OWNERSHIP).
    // Both are false when the read was not reached.
    bool profileReadable;
    bool intentMatchesProfile;
};

static inline const char* applied_profile_indicator_reason_name(
    AppliedProfileIndicatorReason reason) {
    switch (reason) {
        case APPLIED_PROFILE_REASON_NO_AUTHORITY:      return "no authoritative service snapshot";
        case APPLIED_PROFILE_REASON_NOT_A_USER_SLOT:   return "active profile is not a personal slot";
        case APPLIED_PROFILE_REASON_NO_ACTIVE_INTENT:  return "service published no active intent";
        case APPLIED_PROFILE_REASON_PROFILE_UNREADABLE:return "stored profile could not be read";
        case APPLIED_PROFILE_REASON_PROFILE_EDITED:    return "stored profile no longer matches active intent";
        case APPLIED_PROFILE_REASON_APPLIED:           return "applied";
    }
    return "unknown";
}

// Returns the personal slot to tick, or 0 for "no personal profile is active".
// Pure: every input is supplied by the caller, so the whole decision table is
// asserted by the regression harness on either host.
static inline int applied_profile_indicator_slot(
    const AppliedProfileIndicatorInputs& inputs,
    AppliedProfileIndicatorReason* reasonOut) {
    AppliedProfileIndicatorReason reason = APPLIED_PROFILE_REASON_NO_AUTHORITY;
    int slot = 0;
    if (!inputs.serviceAuthoritative) {
        reason = APPLIED_PROFILE_REASON_NO_AUTHORITY;
    } else if (inputs.profileSource != (unsigned int)SERVICE_PROFILE_SOURCE_USER_SLOT ||
               inputs.profileSlot < 1 ||
               (int)inputs.profileSlot > inputs.maxSlots) {
        // Shared and machine banks are separate banks with separate contents;
        // they must never masquerade as the same-numbered personal slot.
        reason = APPLIED_PROFILE_REASON_NOT_A_USER_SLOT;
    } else if (!inputs.activeDesiredValid) {
        reason = APPLIED_PROFILE_REASON_NO_ACTIVE_INTENT;
    } else if (!inputs.profileReadable) {
        reason = APPLIED_PROFILE_REASON_PROFILE_UNREADABLE;
    } else if (!inputs.intentMatchesProfile) {
        reason = APPLIED_PROFILE_REASON_PROFILE_EDITED;
    } else {
        reason = APPLIED_PROFILE_REASON_APPLIED;
        slot = (int)inputs.profileSlot;
    }
    if (reasonOut) *reasonOut = reason;
    return slot;
}

#endif  // GREEN_CURVE_APPLIED_PROFILE_INDICATOR_POLICY_H
