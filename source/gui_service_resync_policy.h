// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// Whether re-reading the service's state is a PRESENTATION TRANSITION or just a
// re-read.
//
// gui_service_begin_full_sync() is a transition: it forces the model to
// SYNCING, throws away live authority (`loaded`, the control state, telemetry)
// and repaints the phase-only presentation -- the "Synchronizing GPU state"
// overlay, a disabled editor, and a tray icon that has to fall back to the
// neutral theme because with no live state there is truthfully no OC or fan in
// effect.  That is exactly right when the answer really did become unknown: a
// GPU switch, a service install/removal, a lost transport.
//
// It is wrong for a re-read of the SAME service and the SAME GPU that the
// current presentation already describes coherently -- the manual Refresh
// button being the obvious one.  Nothing about the hardware changed; the GUI
// merely asked for fresh numbers.  Tearing the presentation down first made
// Refresh flash the sync overlay and drop the tray icon from its OC/fan theme
// to the neutral one and back (reported 2026-07-31), while claiming for those
// few hundred milliseconds that Green Curve's settings were not in effect.
//
// Deferring the decision to the completion is also strictly more truthful: a
// transport failure or a non-READY envelope still transitions through the
// normal completion paths, so the presentation only changes once there is an
// actual answer.  Nothing is weakened by waiting -- an Apply issued during the
// re-read still stamps the state identity it was computed against, and the
// service refuses it fail-closed if the fresh envelope moved that identity.

#ifndef GREEN_CURVE_GUI_SERVICE_RESYNC_POLICY_H
#define GREEN_CURVE_GUI_SERVICE_RESYNC_POLICY_H

enum GuiServiceResyncMode {
    // Queue the read and leave every surface alone until it answers.
    GUI_SERVICE_RESYNC_PRESERVE_PRESENTATION = 0,
    // Drop live authority now and show the synchronizing presentation.
    GUI_SERVICE_RESYNC_TRANSITION = 1,
};

// `identityMayChange` is the caller's statement that the read is aimed at a
// different GPU/service than the one on screen (GPU selector, service
// install/removal).  Such a read invalidates the current presentation by
// construction, so it can never be preserved.
static inline GuiServiceResyncMode gui_service_resync_decide(
    bool modelReady, bool liveAuthorityValid, bool identityMayChange) {
    if (identityMayChange) return GUI_SERVICE_RESYNC_TRANSITION;
    // Without a coherent READY model and live authority there is no truthful
    // presentation to preserve; the transition is the honest state.
    if (!modelReady || !liveAuthorityValid) return GUI_SERVICE_RESYNC_TRANSITION;
    return GUI_SERVICE_RESYNC_PRESERVE_PRESENTATION;
}

#endif // GREEN_CURVE_GUI_SERVICE_RESYNC_POLICY_H
