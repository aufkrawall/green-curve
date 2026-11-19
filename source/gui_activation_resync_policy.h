// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// Whether the GUI re-reads live GPU state because the user switched to it.
//
// Clock settings can change outside Green Curve -- another tool applies or
// resets a profile, a driver reset puts the GPU back at stock -- and the GUI
// used to keep showing its last snapshot until the user pressed Refresh.  The
// service learns nothing on its own: its fan worker polls only fan and
// temperature telemetry, and a full VF/offset read costs it tens of
// milliseconds of NvAPI/NVML work under the serialized hardware lock.
//
// So the re-read is EVENT-driven, never timed: it runs when the window becomes
// the foreground application (WM_ACTIVATEAPP).  Changing settings in another
// tool means leaving this window, so coming back is exactly the moment the
// snapshot may be stale, and it is a moment the user is looking at the GUI
// rather than at a game.  While Green Curve sits in the tray, behind another
// application or minimized there is no read at all, so nothing here can add
// periodic work to a running game or to the desktop.
//
// The read is the same presentation-preserving full sync the Refresh button
// queues (gui_service_resync_policy.h), minus its status line: it keeps the
// editor, overlay and tray theme until the answer arrives, and an attached
// unsaved draft is re-projected over the fresh baseline rather than discarded.

#ifndef GREEN_CURVE_GUI_ACTIVATION_RESYNC_POLICY_H
#define GREEN_CURVE_GUI_ACTIVATION_RESYNC_POLICY_H

// Coming back to the window twice within this interval reads once.  The read
// is user-paced already; this only stops fast app switching from queueing a
// full sync per switch.
static const unsigned long long GUI_ACTIVATION_RESYNC_MIN_INTERVAL_MS = 2000ULL;

enum GuiActivationResyncDecision {
    GUI_ACTIVATION_RESYNC_QUEUE = 0,
    // No background-service client (CLI, the service itself).
    GUI_ACTIVATION_RESYNC_SKIP_NO_SERVICE,
    // Hidden to the tray or minimized: nothing visible to bring up to date.
    GUI_ACTIVATION_RESYNC_SKIP_NOT_SHOWN,
    // A modal dialog owns the window (profile tools, a confirmation): state
    // must not be re-adopted underneath a dialog that is working from it.
    GUI_ACTIVATION_RESYNC_SKIP_MODAL,
    // No coherent READY authority yet; startup or the reconnect timer owns the
    // next read, and a re-read here would only flash the sync transition.
    GUI_ACTIVATION_RESYNC_SKIP_NOT_READY,
    // A write, full sync, capture or service change is already queued or in
    // flight; its completion carries fresh state, and a write must never
    // queue behind a read the user did not ask for.
    GUI_ACTIVATION_RESYNC_SKIP_BUSY,
    // A preserved draft for a different GPU or configuration: the manual
    // Refresh asks before discarding it, so an automatic read must not decide.
    GUI_ACTIVATION_RESYNC_SKIP_DETACHED_DRAFT,
    GUI_ACTIVATION_RESYNC_SKIP_RECENT,
};

struct GuiActivationResyncInput {
    bool serviceClient = false;
    bool windowShown = false;
    bool modalActive = false;
    bool modelReady = false;
    bool liveAuthority = false;
    bool serviceWorkOutstanding = false;
    bool draftDetached = false;
    unsigned long long nowMs = 0;
    // When the previous activation re-read was queued; 0 = never.
    unsigned long long lastQueuedMs = 0;
};

static inline GuiActivationResyncDecision gui_activation_resync_decide(
    const GuiActivationResyncInput& in) {
    if (!in.serviceClient) return GUI_ACTIVATION_RESYNC_SKIP_NO_SERVICE;
    if (!in.windowShown) return GUI_ACTIVATION_RESYNC_SKIP_NOT_SHOWN;
    if (in.modalActive) return GUI_ACTIVATION_RESYNC_SKIP_MODAL;
    if (!in.modelReady || !in.liveAuthority) return GUI_ACTIVATION_RESYNC_SKIP_NOT_READY;
    if (in.serviceWorkOutstanding) return GUI_ACTIVATION_RESYNC_SKIP_BUSY;
    if (in.draftDetached) return GUI_ACTIVATION_RESYNC_SKIP_DETACHED_DRAFT;
    // A tick that went backwards cannot prove recency; read.
    if (in.lastQueuedMs != 0 && in.nowMs >= in.lastQueuedMs &&
        in.nowMs - in.lastQueuedMs < GUI_ACTIVATION_RESYNC_MIN_INTERVAL_MS)
        return GUI_ACTIVATION_RESYNC_SKIP_RECENT;
    return GUI_ACTIVATION_RESYNC_QUEUE;
}

static inline const char* gui_activation_resync_decision_name(
    GuiActivationResyncDecision decision) {
    switch (decision) {
        case GUI_ACTIVATION_RESYNC_QUEUE: return "queue";
        case GUI_ACTIVATION_RESYNC_SKIP_NO_SERVICE: return "skip-no-service";
        case GUI_ACTIVATION_RESYNC_SKIP_NOT_SHOWN: return "skip-not-shown";
        case GUI_ACTIVATION_RESYNC_SKIP_MODAL: return "skip-modal";
        case GUI_ACTIVATION_RESYNC_SKIP_NOT_READY: return "skip-not-ready";
        case GUI_ACTIVATION_RESYNC_SKIP_BUSY: return "skip-busy";
        case GUI_ACTIVATION_RESYNC_SKIP_DETACHED_DRAFT: return "skip-detached-draft";
        case GUI_ACTIVATION_RESYNC_SKIP_RECENT: return "skip-recent";
    }
    return "unknown";
}

#endif  // GREEN_CURVE_GUI_ACTIVATION_RESYNC_POLICY_H
