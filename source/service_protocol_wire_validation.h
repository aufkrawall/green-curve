// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#ifndef GREEN_CURVE_SERVICE_PROTOCOL_WIRE_VALIDATION_H
#define GREEN_CURVE_SERVICE_PROTOCOL_WIRE_VALIDATION_H

// STRICT wire checks: what a well-formed request looks like BEFORE anything
// canonicalizes it.  Split out of service_protocol.h, which sits on its size
// ratchet, and included from the point these functions used to occupy so every
// consumer still gets them by including service_protocol.h.
//
// These reject; validate_desired_settings_for_ipc() then clamps.  The order
// matters: a boolean the strict pass lets through is folded to true downstream,
// so an omission here is invisible in behaviour and only shows up as a request
// that should have been refused being quietly accepted.

static inline bool service_wire_string_is_terminated(
    const char* value, unsigned int count) {
    if (!value || count == 0) return false;
    for (unsigned int i = 0; i < count; ++i)
        if (value[i] == '\0') return true;
    return false;
}

// A client-supplied string that we echo into a log line, a crash breadcrumb, or
// a diagnostic must not be able to CARRY structure.  CR and LF would let any
// authenticated local user -- the pipe ACL admits all of them -- forge whole
// lines in the SYSTEM service's debug log and, worse, in the crash breadcrumb
// that is the primary artifact for telling "the driver died under us" apart
// from "we handed the driver an invalidated handle".  ESC would let a caller
// drive the reader's terminal.  TAB is allowed: it is whitespace, not framing.
//
// Bytes >= 0x80 stay allowed so a legitimate non-ASCII profile path is not
// refused; this is a framing check, not an encoding check, and the paths that
// are actually used for file I/O get their own much stricter validation in
// service_validate_file_write_path().
//
// The length bound is the same NUL-terminated scan as above, so a caller cannot
// hide a control byte past the terminator.
static inline bool service_wire_string_is_log_safe(
    const char* value, unsigned int count) {
    if (!value || count == 0) return false;
    for (unsigned int i = 0; i < count; ++i) {
        unsigned char c = (unsigned char)value[i];
        // An empty string is safe: it is terminated, and it carries nothing.
        // The dispatch site already handles it explicitly
        // (`request->source[0] ? request->source : "service request"`).
        if (c == '\0') return true;
        if (c < 0x20 && c != '\t') return false;
        if (c == 0x7F) return false;
    }
    return false;  // ran off the end without a terminator
}

static inline bool service_desired_bool_fields_valid(
    const DesiredSettings* desired) {
    if (!desired) return false;
    for (int i = 0; i < VF_NUM_POINTS; ++i) {
        if (desired->hasCurvePoint[i] > 1) return false;
        // Provenance decides whether a point is written as an offset or held to
        // an absolute MHz, so it is request-carried state like every flag below
        // and belongs on this side of the trust boundary.  Canonicalization
        // downstream folds 2 into true, which is why the omission was not
        // exploitable -- and also why it would have stayed invisible.
        if (desired->curvePointFromGpuOffset[i] > 1) return false;
    }
    const gc_bool8* flags[] = {
        &desired->hasLock, &desired->lockTracksAnchor,
        &desired->hasGpuOffset, &desired->hasMemOffset,
        &desired->hasPowerLimit, &desired->hasFan, &desired->fanAuto,
        &desired->resetOcBeforeApply,
        &desired->hasXbarOffsetKhz, &desired->hasXbarMsvddOffsetUv,
        &desired->hasSysClkOffsetKhz,
        &desired->hasVideoClkOffsetKhz,
    };
    for (const gc_bool8* flag : flags)
        if (*flag > 1) return false;
    return fan_curve_wire_flags_valid(&desired->fanCurve);
}

static inline bool service_gpu_bool_fields_valid(const GpuAdapterInfo* gpu) {
    return gpu && gpu->valid <= 1 && gpu->pciInfoValid <= 1 &&
        gpu->vfReadSupported <= 1 && gpu->vfWriteSupported <= 1 &&
        gpu->vfBestGuess <= 1;
}

#endif
