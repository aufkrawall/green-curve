// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT

#ifndef GREEN_CURVE_GUI_SERVICE_ACTIONABILITY_POLICY_H
#define GREEN_CURVE_GUI_SERVICE_ACTIONABILITY_POLICY_H

// Which groups of GUI controls are actionable for a given service/draft state.
//
// The window used to present two very different halves: with no background
// service installed the editor was correctly dead, while the entire profile row
// still read as live.  Two of those controls did not merely look wrong -- Save
// would persist an all-zero control state as a profile, and Load wrote an editor
// that is disabled and blank, so the only evidence it did anything was a status
// line.
//
// Two rules, in order of precedence:
//
//   1. An escape hatch is never blocked.  Refresh, the service-install checkbox
//      and License are the only ways out of an unavailable-service state.
//   2. Everything whose purpose depends on the service is greyed -- but a
//      control that already HOLDS a service-dependent setting stays enabled, so
//      that setting can always be undone.
//
// Rule 2's exemption is not politeness, it prevents a genuinely unreachable
// state.  `startup_task_config_state()` keeps a Task Scheduler entry registered
// for any `logon_slot`/`logon_shared_slot` assignment, and the logon combo is the
// only GUI path that can clear one; `clear_profile_from_config()` deliberately
// preserves `logon_shared_slot`, so deleting the profile is not an escape either.
// The same shape applies to a published shared profile and to the GUI's own
// autostart.  So: you cannot create a new service-dependent assignment while the
// service is unreachable, but you can always remove one you already have.
//
// Kept free of Win32 so the regression harness can pin every cell of the
// (state x capability) table without creating a window.

enum GuiServiceCapability {
    // Refresh, the service-install checkbox, License.  Never blocked: these are
    // the only ways out of an unavailable-service state.
    GUI_SERVICE_CAP_RECOVERY = 0,
    // Load, Save, Clear, the profile slot combo, and the shared-profile picker.
    // Save needs a coherent live snapshot to capture; Load and the picker need an
    // editor to write into; Clear and the slot combo only exist to target those.
    GUI_SERVICE_CAP_PROFILE_EDIT,
    // The auto-profile popup.  Its rules can only ever take effect by queueing
    // a mutation through the service.
    GUI_SERVICE_CAP_AUTOMATION,
    // The VF editor and the per-point controls.
    GUI_SERVICE_CAP_EDITOR,
    // Apply / Reset: the editor plus a populated snapshot.
    GUI_SERVICE_CAP_HARDWARE_MUTATION,
};

// installed/available/toggleInFlight describe the service itself;
// ready/draftAttached/draftDetached/loaded describe live GPU authority.
struct GuiServiceActionability {
    bool installed;
    bool available;
    bool toggleInFlight;
    bool ready;
    bool draftAttached;
    bool draftDetached;
    bool loaded;
};

// Registered with the SCM AND answering on the pipe, and not mid-install or
// mid-uninstall.  Collapses not-installed, installed-but-stopped and
// installed-but-not-responding into one "cannot act" state.
//
// Deliberately NOT keyed on the protocol phase: SYNCING / RECOVERING /
// DEVICE_MISSING must stay usable, because a profile loaded during those is
// preserved as a pre-READY overlay (GuiDraft::pendingDesired) and rebased onto
// the first coherent snapshot.  That is a documented feature with its own
// confirmation strings, and gating on readiness here would delete it.
static inline bool gui_service_reachable(const GuiServiceActionability* in) {
    return in && in->installed && in->available && !in->toggleInFlight;
}

// A coherent live snapshot plus a draft that belongs to THIS GPU and topology.
static inline bool gui_service_editor_actionable(
    const GuiServiceActionability* in) {
    return in && in->ready && in->draftAttached && !in->draftDetached;
}

// Rule 2's exemption, for the controls that persist a service-dependent setting:
// share-with-all-users, apply-on-GUI-start, apply-on-logon, and start-to-tray.
// Greyed while the setting is at its default, enabled while it is not, so an
// existing assignment can always be cleared.
static inline bool gui_service_config_control_actionable(
    const GuiServiceActionability* in, bool assignmentPresent) {
    return assignmentPresent || gui_service_reachable(in);
}

static inline bool gui_service_capability_enabled(
    const GuiServiceActionability* in, GuiServiceCapability capability) {
    if (!in) return false;
    switch (capability) {
        case GUI_SERVICE_CAP_RECOVERY:
            return true;
        case GUI_SERVICE_CAP_PROFILE_EDIT:
        case GUI_SERVICE_CAP_AUTOMATION:
            return gui_service_reachable(in);
        case GUI_SERVICE_CAP_EDITOR:
            return gui_service_editor_actionable(in);
        case GUI_SERVICE_CAP_HARDWARE_MUTATION:
            return gui_service_editor_actionable(in) && in->loaded;
        default:
            return false;
    }
}

#endif // GREEN_CURVE_GUI_SERVICE_ACTIONABILITY_POLICY_H
