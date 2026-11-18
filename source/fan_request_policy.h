// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// What a fan request means on a GPU whose driver reports no fan Green Curve
// can drive.  Pure: shared by the Windows GUI and service, the Linux backend
// and daemon, and the regression harness.
//
// WHY THIS EXISTS
// ---------------
// Notebook boards hand cooling to the platform embedded controller, and some
// desktop boards are passive.  Their driver answers the fan-count query with
// zero fans or refuses it, so Green Curve disables its fan controls and never
// writes a fan.  The fan domain still travels in every request, though: the
// GUI's fan selection defaults to Auto and every saved profile carries the
// mandatory fan keys.  Both platforms then treated that Auto as a real fan
// write.  Windows attempted "restore driver auto", which cannot work without a
// fan, failed, and rolled back the whole transaction - VF curve included - so
// no undervolt could ever be applied.  Linux refused the request up front
// because the fan domain was unavailable.
//
// THE RULE (the fan counterpart of the inert power request in
// `linux_power_request_is_inert()` and gpu-backend.md, F-POWER-SURFACE)
// ----------------------------------------------------------------------
// * Without a controllable fan, Auto is the only fan state that exists: the
//   platform's own.  An Auto request there is INERT.  It keeps fan ownership
//   for profile-equality and active-intent purposes, but writes nothing and
//   must not be counted as a missing or unavailable domain.
// * Fixed or Curve there is a real request for a surface that does not exist.
//   It is refused, by name, before any write, so the rest of the transaction is
//   never put at risk by a domain the board cannot have.
// * "No controllable fan" needs POSITIVE evidence: no fan was ever reported in
//   this session AND the capability probe recorded the fan domain as absent or
//   refused by the driver.  An unprobed domain is not evidence of anything and
//   keeps the ordinary hardware path with its existing refusals.

#ifndef GREEN_CURVE_FAN_REQUEST_POLICY_H
#define GREEN_CURVE_FAN_REQUEST_POLICY_H

#include "gpu_core.h"

enum FanRequestDisposition : gc_u32 {
    // The request carries no fan intent.
    FAN_REQUEST_NOT_OWNED = 0,
    // The ordinary path: the driver reports fans (or nothing proves it does
    // not), so the request is validated, written and verified as before.
    FAN_REQUEST_HARDWARE = 1,
    // Auto on a GPU without a controllable fan: owned, already satisfied,
    // nothing to write, no fan surface required.
    FAN_REQUEST_INERT_NO_FAN = 2,
    // Fixed or Curve on a GPU without a controllable fan: refuse before writes.
    FAN_REQUEST_REFUSED_NO_FAN = 3,
};

// The request's fan mode.  Older requests may carry only the legacy auto flag,
// which every apply path already interprets this way.
static inline int fan_request_effective_mode(int fanMode, bool fanAuto) {
    if (fanMode >= FAN_MODE_AUTO && fanMode <= FAN_MODE_CURVE) return fanMode;
    return fanAuto ? FAN_MODE_AUTO : FAN_MODE_FIXED;
}

// `fansReported`: the driver reported at least one fan during this session.
// `fanDomainMissing`: the capability probe recorded the fan domain as absent
// or refused (`gpu_capability_domain_is_missing()`).
static inline bool fan_control_known_absent(bool fansReported,
                                            bool fanDomainMissing) {
    return !fansReported && fanDomainMissing;
}

// Whether a fan selection already holds on a GPU that reports no fan.  Used by
// the GUI's "is this a fan change?" predicate: without a fan Green Curve can
// drive, Auto needs no write, and anything else can never hold.
static inline bool fan_selection_holds_without_fans(int fanMode) {
    return fanMode == FAN_MODE_AUTO;
}

static inline FanRequestDisposition fan_request_disposition(
    bool hasFan, int fanMode, bool fanAuto, bool fansReported,
    bool fanDomainMissing) {
    if (!hasFan) return FAN_REQUEST_NOT_OWNED;
    if (!fan_control_known_absent(fansReported, fanDomainMissing))
        return FAN_REQUEST_HARDWARE;
    return fan_request_effective_mode(fanMode, fanAuto) == FAN_MODE_AUTO
        ? FAN_REQUEST_INERT_NO_FAN
        : FAN_REQUEST_REFUSED_NO_FAN;
}

// Whether the request needs a fan control surface.  The mutation-domain gates
// (Linux daemon preconditions and backend preflight) drop the fan domain from
// their "unavailable" mask when this is false; an inert request owns the
// domain without writing it.
static inline bool fan_request_needs_fan_surface(
    FanRequestDisposition disposition) {
    return disposition == FAN_REQUEST_HARDWARE;
}

// The fan mode a GUI editor may adopt.  On a GPU without a controllable fan
// the fan controls are disabled, so a non-Auto mode loaded from a profile
// would be shown, captured and refused, with no control left to change it.
static inline int fan_editor_adoptable_mode(int requestedMode,
                                            bool fanControlAbsent) {
    return fanControlAbsent ? (int)FAN_MODE_AUTO : requestedMode;
}

static inline const char* fan_request_disposition_name(
    FanRequestDisposition disposition) {
    switch (disposition) {
        case FAN_REQUEST_NOT_OWNED: return "not-owned";
        case FAN_REQUEST_HARDWARE: return "hardware";
        case FAN_REQUEST_INERT_NO_FAN: return "inert-no-fan";
        case FAN_REQUEST_REFUSED_NO_FAN: return "refused-no-fan";
        default: return "unknown";
    }
}

static inline const char* fan_request_mode_name(int effectiveMode) {
    switch (effectiveMode) {
        case FAN_MODE_AUTO: return "Auto";
        case FAN_MODE_FIXED: return "Fixed";
        case FAN_MODE_CURVE: return "Curve";
        default: return "unknown";
    }
}

// The single user-facing refusal for a non-Auto request on a fanless GPU, so
// both platforms say the same thing.
static inline const char* fan_request_refusal_text() {
    return "this GPU reports no fan that Green Curve can control (its cooling"
           " is managed by the system), so only fan mode Auto can be applied."
           " Nothing was changed.";
}

#endif  // GREEN_CURVE_FAN_REQUEST_POLICY_H
