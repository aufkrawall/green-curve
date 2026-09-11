// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT

// The once-per-machine "may Green Curve check for updates?" question.
//
// Split out of gui_update_dialog.cpp only to keep both shards within the
// project's source-size guideline; it is included at the END of that file
// because it calls gui_update_set_policy(), which is defined there. Not an
// include boundary for reuse.

// ---------------------------------------------------------------------------
// The once-per-machine question
// ---------------------------------------------------------------------------

// `GC_UPDATE_AUTO_CHECK_UNSET` was designed as a question and shipped as a
// permanent state, because nothing ever asked it.  The only place it surfaced
// was a line of body text inside this dialog, so a user who never opened the
// dialog got no update checks ever and no hint that any existed -- and UNSET is
// the default, so that was everybody.
//
// Asking is what makes the default defensible: the answer is the user's, the
// outbound request is disclosed in the words before they give it, and "no"
// is recorded as OFF so it is never asked again on this machine.
//
// It is NOT asked from a timer, a tray start or a background window; see
// gc_update_should_prompt_auto_check() for each gate and why it exists.
void gui_update_maybe_prompt_first_run(HWND parent) {
    // Per process, and set BEFORE the modal box opens.  The caller is a poll
    // tick, so without this a service that cannot record the answer would turn
    // every subsequent tick into another dialog.
    static bool s_asked = false;

    ServiceUpdateState stateValue = {};
    bool haveState = gui_update_state(&stateValue);
    HWND owner = parent ? parent : g_app.hMainWnd;
    bool interactive = owner && IsWindowVisible(owner) && IsWindowEnabled(owner) &&
                       !g_app.applyInFlight;
    if (!gc_update_should_prompt_auto_check(
            haveState ? (GcUpdateAutoCheck)stateValue.autoCheck
                      : GC_UPDATE_AUTO_CHECK_UNSET,
            haveState, interactive, s_asked)) {
        return;
    }
    s_asked = true;
    debug_log("gui update: asking for the automatic-check preference (first run)\n");

    int answer = gc_message_box(owner,
        "Should Green Curve check for updates automatically?\n\n"
        "It contacts GitHub about once a day and reveals your IP address, "
        "the installed version and your architecture. Nothing is ever "
        "installed without you clicking Install.\n\n"
        "You can change this later under Updates.",
        "Green Curve", MB_YESNO | MB_ICONQUESTION);
    // A dismissed dialog (0) is not a "no": it is no answer, and recording one
    // would consume the single question this machine gets.  Left UNSET so the
    // next GUI start asks again.
    if (answer != IDYES && answer != IDNO) {
        debug_log("gui update: the first-run question was dismissed; leaving the preference unset\n");
        return;
    }

    GcUpdateAutoCheck wanted = (answer == IDYES) ? GC_UPDATE_AUTO_CHECK_ON
                                                 : GC_UPDATE_AUTO_CHECK_OFF;
    // Dispatched off the message thread like every other update command: this
    // runs during startup, and a blocking round trip here delays the first
    // frame for as long as the service takes to answer.
    //
    // Failure is not reported to the user: they answered a question they did
    // not ask for, and a box on top of that is noise. The preference stays
    // UNSET, so the next start asks again, and the dialog still shows the state
    // and the toggle.
    if (!gui_update_set_policy(wanted, GC_UPDATE_INTERVAL_DEFAULT_SECONDS,
                               false)) {
        debug_log("gui update: could not dispatch the first-run preference\n");
        return;
    }
    debug_log("gui update: first-run preference %s dispatched\n",
              wanted == GC_UPDATE_AUTO_CHECK_ON ? "ON" : "OFF");
}
