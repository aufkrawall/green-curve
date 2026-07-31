// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// What the window does with the result of a MANUAL Apply / Reset: whether it
// interrupts the user with a modal box, and what the profile status line says
// afterwards.
//
// Until now every manual mutation ended in an OK-box, including the ordinary
// case where the service did exactly what was asked.  A confirmation the user
// dismisses without reading is not information -- it is a click, and it trains
// the user to dismiss the box that DOES matter.  So the modal is now reserved
// for an outcome the user has to know about, and the ordinary case is reported
// where the operation was already being narrated: the status line under the
// profile controls.
//
// The severity that decision keys off is NOT derived here.  It comes from the
// service, on the wire (ServiceOutcomeSeverity), because only the service can
// tell a fully verified apply from one that committed while the driver quietly
// declined some points -- both of which answer SERVICE_STATUS_OK.  Inferring it
// on this side would mean parsing `message`, which is prose.
//
// Do not confuse this with F-INFLIGHT (gui_apply_in_flight_policy.h), which is
// what every surface shows WHILE the write runs.  This file is the moment after
// it lands.

#ifndef GREEN_CURVE_GUI_MUTATION_RESULT_POLICY_H
#define GREEN_CURVE_GUI_MUTATION_RESULT_POLICY_H

#include "service_protocol.h"
#include "gui_mutation_queue_policy.h"
#include "gui_tray_callback_policy.h"

#include <stddef.h>

// The severity this completion should be presented at.
//
// `successForUi` is the window's own verdict: the transport succeeded, the
// service answered OK, and the answer still belongs to the current service
// generation and selected GPU.  A result that fails any of that is an ERROR
// here no matter what the envelope says, because the envelope is then either
// absent or describing an operation this window can no longer attribute.
//
// Only a result the window fully adopted may lower the severity to what the
// service reported, which is what keeps "no dialog" from ever being reached by
// a completion that was not understood.
static inline gc_u32 gui_mutation_result_severity(bool successForUi,
    gc_u32 envelopeSeverity) {
    if (!successForUi) return (gc_u32)SERVICE_OUTCOME_SEVERITY_ERROR;
    return service_response_resolve_outcome_severity(
        (gc_u32)SERVICE_STATUS_OK, envelopeSeverity);
}

// A modal box is for something the user must acknowledge.  A clean success is
// not: it is confirmed on the status line instead.
static inline bool gui_mutation_result_needs_prompt(gc_u32 severity) {
    return severity != (gc_u32)SERVICE_OUTCOME_SEVERITY_SUCCESS;
}

// Name the profile the way every other surface names it, from the request the
// user actually made rather than from `[profiles] applied_slot`: the config
// cache is refreshed by the same completion and reports what the SERVICE now
// owns, which for an ad-hoc Apply is deliberately not a slot at all.
//
// Shared and machine banks stay distinguishable from the same-numbered personal
// slot for the same reason as in the tray tooltip: they are separate banks with
// separate contents.
static inline void gui_mutation_result_profile_label(char* out, size_t outSize,
    unsigned int profileSource, int profileSlot, int maxSlots) {
    if (!out || outSize == 0) return;
    out[0] = '\0';
    GuiTrayProfileKind kind = GUI_TRAY_PROFILE_NONE;
    switch (profileSource) {
        case SERVICE_PROFILE_SOURCE_USER_SLOT:
            kind = GUI_TRAY_PROFILE_USER_SLOT; break;
        case SERVICE_PROFILE_SOURCE_SHARED_SLOT:
            kind = GUI_TRAY_PROFILE_SHARED_SLOT; break;
        case SERVICE_PROFILE_SOURCE_MACHINE_SLOT:
            kind = GUI_TRAY_PROFILE_MACHINE_SLOT; break;
        case SERVICE_PROFILE_SOURCE_AD_HOC:
            kind = GUI_TRAY_PROFILE_AD_HOC; break;
        default: kind = GUI_TRAY_PROFILE_NONE; break;
    }
    // A personal slot is the only kind that names itself "Profile N"; every
    // other kind is formatted from its own source slot.
    int userSlot = kind == GUI_TRAY_PROFILE_USER_SLOT ? profileSlot : 0;
    gui_tray_format_active_profile(out, outSize, userSlot, kind, profileSlot,
        maxSlots);
}

// The status line under the profile controls, which is where the operation was
// already being narrated ("Applying Profile 3 to the GPU...").  It is one
// clipped line, so the part that has to survive truncation comes first: what
// happened to what.  The service's own wording follows as detail, and for
// anything but a clean success the modal box carries it in full anyway.
static inline void gui_mutation_result_status_text(char* out, size_t outSize,
    GuiMutationKind kind, gc_u32 severity, const char* profileLabel,
    const char* message) {
    if (!out || outSize == 0) return;
    out[0] = '\0';
    const char* label = profileLabel && profileLabel[0] ? profileLabel
        : "Settings";
    size_t pos = 0;
    if (kind == GUI_MUTATION_RESET) {
        if (severity == (gc_u32)SERVICE_OUTCOME_SEVERITY_SUCCESS) {
            gc_appendf(out, outSize, 0, "GPU settings reset to defaults.");
            return;
        }
        pos = gc_appendf(out, outSize, 0,
            severity == (gc_u32)SERVICE_OUTCOME_SEVERITY_WARNING
                ? "GPU reset completed with warnings"
                : "GPU reset failed");
    } else if (severity == (gc_u32)SERVICE_OUTCOME_SEVERITY_SUCCESS) {
        gc_appendf(out, outSize, 0, "%s applied.", label);
        return;
    } else {
        pos = gc_appendf(out, outSize, pos,
            severity == (gc_u32)SERVICE_OUTCOME_SEVERITY_WARNING
                ? "%s applied with warnings" : "%s was not applied", label);
    }
    if (message && message[0]) {
        gc_appendf(out, outSize, pos, ": %s", message);
    } else {
        gc_appendf(out, outSize, pos, ".");
    }
}

// The status line WHILE the operation runs.  Same vocabulary as the completion
// above, so the user reads one continuing sentence rather than two unrelated
// ones; the old text ("GPU operation started in the background") named neither
// the profile nor the fact that a result was still coming.
static inline void gui_mutation_queued_status_text(char* out, size_t outSize,
    GuiMutationKind kind, const char* profileLabel) {
    if (!out || outSize == 0) return;
    out[0] = '\0';
    if (kind == GUI_MUTATION_RESET) {
        gc_appendf(out, outSize, 0, "Resetting the GPU to its defaults...");
        return;
    }
    const char* label = profileLabel && profileLabel[0] ? profileLabel
        : "Settings";
    gc_appendf(out, outSize, 0, "Applying %s to the GPU...", label);
}

#endif  // GREEN_CURVE_GUI_MUTATION_RESULT_POLICY_H
