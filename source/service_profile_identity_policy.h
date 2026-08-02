// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// Which profile identity the SERVICE is allowed to record for a successful
// APPLY.  The GUI counterpart -- which slot the tray menu then ticks -- is
// applied_profile_indicator_policy.h; this file decides the input that one
// consumes, so the two are deliberately readable side by side.
//
// A client sends the slot it believes it is applying.  The service never takes
// that on trust: it compares the slot's STORED RECORD against the intent under
// discussion, and there are two moments at which that can succeed.
//
//   1. Against the request payload, before the write.  The tray, hotkey, and
//      app-start paths load a slot and send it whole, so the payload IS the
//      record.  Only this proof lets the caller replace active ownership
//      outright with the named profile.
//
//   2. Against the intent the service ends up holding, after the write.  The
//      GUI's Apply is deliberately a DELTA -- capture_gui_apply_settings()
//      drops every domain the editor is not changing, most often the fan,
//      because re-writing an unchanged fan policy would disturb a running
//      curve mid-game.  A delta can never equal a complete record field for
//      field, so proof 1 always failed for it and a genuine "apply profile N"
//      from the main window was recorded as ad-hoc.  The tray menu then showed
//      no checkmark and the tooltip said "Manual settings" until the same
//      profile was re-picked from the tray, which is the only path that had
//      ever sent a whole record.
//
// The honest question is "is the profile what is now in force", not "is the
// payload the profile", and after the write the service holds precisely that.
// Proof 1 still outranks proof 2: it is established before the hardware is
// touched and is what authorizes replacing ownership rather than merging into
// it.

#ifndef GREEN_CURVE_SERVICE_PROFILE_IDENTITY_POLICY_H
#define GREEN_CURVE_SERVICE_PROFILE_IDENTITY_POLICY_H

#include "service_protocol.h"

enum ServiceProfileIdentityOutcome {
    // No slot was claimed, the claim was out of range, or neither record
    // comparison held: the apply owns the GPU without belonging to a slot.
    SERVICE_PROFILE_IDENTITY_AD_HOC = 0,
    // The payload was the complete stored record.
    SERVICE_PROFILE_IDENTITY_FROM_REQUEST,
    // The payload was a delta, but the intent it established is the record.
    SERVICE_PROFILE_IDENTITY_FROM_ACTIVE_INTENT,
};

static inline const char* service_profile_identity_outcome_name(
    ServiceProfileIdentityOutcome outcome) {
    switch (outcome) {
        case SERVICE_PROFILE_IDENTITY_AD_HOC:      return "ad-hoc";
        case SERVICE_PROFILE_IDENTITY_FROM_REQUEST: return "request payload";
        case SERVICE_PROFILE_IDENTITY_FROM_ACTIVE_INTENT:
            return "resulting active intent";
    }
    return "unknown";
}

// Shared and machine banks are separate banks with separate contents, so a
// claim naming one of them is considered on its own terms; every other source
// (ad-hoc, none, out-of-range slots) can never name a stored record.
static inline bool service_profile_metadata_claims_slot(
    unsigned int source, int slot, int maxSlots) {
    if (slot < 1 || slot > maxSlots) return false;
    return source == (unsigned int)SERVICE_PROFILE_SOURCE_USER_SLOT ||
        source == (unsigned int)SERVICE_PROFILE_SOURCE_SHARED_SLOT;
}

// Pure: every input is supplied by the caller, so the whole decision table is
// asserted by the regression harness on either host.  `requestMatchesRecord`
// is only ever known before the write and `activeIntentMatchesRecord` only
// after it; passing false for the one that has not been evaluated yet is how
// each call site asks about its own moment.
static inline ServiceProfileIdentityOutcome service_profile_identity_outcome(
    unsigned int source, int slot, int maxSlots,
    bool requestMatchesRecord, bool activeIntentMatchesRecord) {
    if (!service_profile_metadata_claims_slot(source, slot, maxSlots))
        return SERVICE_PROFILE_IDENTITY_AD_HOC;
    if (requestMatchesRecord) return SERVICE_PROFILE_IDENTITY_FROM_REQUEST;
    if (activeIntentMatchesRecord)
        return SERVICE_PROFILE_IDENTITY_FROM_ACTIVE_INTENT;
    return SERVICE_PROFILE_IDENTITY_AD_HOC;
}

// Only a payload that was itself the complete record may replace active
// ownership outright.  A delta must keep merging into what the service already
// owns -- treating it as a complete declaration would return every domain it
// omits to defaults, which is precisely how an unchanged fan curve would get
// reset by an Apply that never mentioned the fan.
static inline bool service_profile_identity_replaces_active_intent(
    ServiceProfileIdentityOutcome outcome) {
    return outcome == SERVICE_PROFILE_IDENTITY_FROM_REQUEST;
}

#endif  // GREEN_CURVE_SERVICE_PROFILE_IDENTITY_POLICY_H
