// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// The cached ServiceUpdateState, and the two questions the tray asks of it.
//
// ## One cache, fed from one place
//
// The service stamps ServiceUpdateState onto EVERY response, so this cache is
// refreshed from `gui_update_note_response()`, called from the single place the
// client reads a response back (`service_send_request`).  The tray and the main
// window therefore learn about an available update through polling that already
// happens -- no extra round trip, and no second code path that could go stale
// while the first one keeps working.
//
// ## Why this is separate from the sending half
//
// The tray menu is compiled into both binaries, so the queries below must be
// too.  The commands that ASK the service to do something live in
// gui_update_dialog.cpp, which is GUI-only: putting them here would leave the
// service binary carrying unused senders, and -Werror is on.
//
// ## The GUI decides nothing
//
// Everything on this side is presentation and triggering.  Nothing here
// resolves a URL, sees a digest, names a version or touches the staged file --
// all of that is the service's, because the service is the only participant
// running as SYSTEM and the only one that can be trusted to gate an installer
// launch.  If this code were rewritten by an attacker holding the user's
// account, the worst it could do is ask the service to check, to install
// something the service itself already verified, or to change a preference the
// service re-validates.  That is the point of the split.

struct GuiUpdateCache {
    ServiceUpdateState state;
    bool valid;
};

static GuiUpdateCache g_guiUpdate = {};
static SRWLOCK g_guiUpdateLock = SRWLOCK_INIT;

// Called for every successful service response.  Deliberately does not filter
// by command: the state is stamped on all of them, and filtering would mean the
// tray only refreshed when something happened to ask about updates.
static void gui_update_note_response(const ServiceResponse* response) {
    if (!response) return;
    ServiceUpdateState received = response->update;
    // Wire strings are terminated defensively before anything renders them; a
    // response that lost its terminator must not walk off the end of a tooltip.
    received.availableVersion[SERVICE_UPDATE_VERSION_CHARS - 1] = '\0';
    received.installedVersion[SERVICE_UPDATE_VERSION_CHARS - 1] = '\0';
    received.detail[ARRAY_COUNT(received.detail) - 1] = '\0';
    {
        AcquireSRWLockExclusive(&g_guiUpdateLock);
        g_guiUpdate.state = received;
        g_guiUpdate.valid = true;
        ReleaseSRWLockExclusive(&g_guiUpdateLock);
    }

#ifndef GREEN_CURVE_SERVICE_BINARY
    // The service is about to run setup and cannot reach across sessions to
    // close us, so it asks here instead.  Run the app's own Exit path rather
    // than exiting abruptly: it releases the tray icon, the single-instance
    // mutex and the service connection, none of which a TerminateProcess from
    // the service would.  Posted, not sent -- this is called from inside the
    // pipe transport and must not re-enter the window procedure.
    //
    // One-shot: the flag stays set for the whole shutdown window, and asking
    // twice would post a second Exit into a window that is already going away.
    static bool s_shutdownPosted = false;
    ServiceUpdateState current = {};
    bool haveCurrent = false;
    {
        AcquireSRWLockShared(&g_guiUpdateLock);
        current = g_guiUpdate.state;
        haveCurrent = g_guiUpdate.valid;
        ReleaseSRWLockShared(&g_guiUpdateLock);
    }
    if (haveCurrent && current.guiShutdownRequested && !s_shutdownPosted &&
        g_app.hMainWnd) {
        s_shutdownPosted = true;
        debug_log("gui update: service requested shutdown for an install; closing\n");
        PostMessageA(g_app.hMainWnd, WM_COMMAND,
                     MAKEWPARAM((WORD)TRAY_MENU_EXIT_ID, 0), 0);
    }
#endif
}

static bool gui_update_state(ServiceUpdateState* out) {
    if (!out) return false;
    AcquireSRWLockShared(&g_guiUpdateLock);
    bool valid = g_guiUpdate.valid;
    if (valid) *out = g_guiUpdate.state;
    ReleaseSRWLockShared(&g_guiUpdateLock);
    return valid;
}

// What, if anything, the user should be alerted to.  The single source for all
// three passive surfaces -- the orange Updates button, the tray tooltip suffix
// and the tray menu entry -- so they cannot disagree about whether there is
// news.  See update_presentation_policy.h for which decisions qualify and why
// NO_ASSET deliberately does not.
//
// A portable .7z copy is NOT filtered out here even though it cannot install in
// place.  It used to be described as if it were; it never was, and the code was
// right and the comment wrong.  Its user still wants to know a new release
// exists, and both surfaces open the dialog, which explains the portable case
// and offers the releases page.  Suppressing it would make portable users the
// only ones who are never told anything at all.
static GcUpdateAlert gui_update_alert() {
    ServiceUpdateState stateValue = {};
    if (!gui_update_state(&stateValue)) return GC_UPDATE_ALERT_NONE;
    return gc_update_alert_kind((GcUpdateDecision)stateValue.decision,
                                stateValue.availableVersion[0] != '\0');
}

static bool gui_update_is_available() {
    return gui_update_alert() != GC_UPDATE_ALERT_NONE;
}

#ifndef GREEN_CURVE_SERVICE_BINARY
// Repaint the Updates button when the alert flips.
//
// The button is owner-drawn from `gui_update_alert()`, and the state behind it
// changes on a service response rather than on any window message, so nothing
// would otherwise invalidate it: the orange outline would appear only when
// something else happened to repaint the button row.  Called from the tray
// refresh, which already runs on every poll tick, so this costs one integer
// compare per second and an InvalidateRect only on the transition.
//
// The last-painted value is the mirror, not the live one -- the same idiom
// ui_checkbox_state.h uses for the owner-drawn checkboxes, and for the same
// reason: an owner-drawn control stores nothing that could be asked instead.
static void gui_update_refresh_alert_presentation() {
    GcUpdateAlert alert = gui_update_alert();
    if ((int)alert == g_app.updateAlertPainted) return;
    debug_log("gui update: alert changed %d -> %d; repainting the Updates button\n",
              g_app.updateAlertPainted, (int)alert);
    g_app.updateAlertPainted = (int)alert;
    if (g_app.hUpdateBtn) InvalidateRect(g_app.hUpdateBtn, nullptr, TRUE);
}
#endif
