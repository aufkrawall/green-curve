// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// What the GUI does when a state read does NOT come back (F-READ-MISS).
//
// Until 2026-09-12 there was one answer: gui_service_handle_transport_failure().
// That is the right answer for a service that is gone -- it advances the GPU
// epoch, discards live authority, drops the applied-profile indicator to
// "Manual settings" and rebuilds every control -- and the wrong one for a
// service that merely did not answer in time.
//
// The incident that separated the two: a C: volume stall (Windows Volsnap event
// 25, "reduce the I/O load on the system") blocked a debug-log flush that the
// service was making from inside its serialized dispatch lock for 19.078
// seconds.  Requests queued behind it recorded 15375 / 10391 / 4735 ms of
// dispatch-queue wait; the GUI's 4000 ms telemetry and 5750 ms full-sync
// deadlines expired -- correctly, the service really had blown its own
// contract -- and the user watched the window disconnect, blank its VF fields,
// lose its profile identity and rebuild itself, twice, for a service that never
// stopped running and answered normally 19 seconds later.
//
// The stall itself is fixed at its root (debug_log_queue_policy.h: producing a
// diagnostic line no longer touches the disk).  This file fixes the reaction,
// which has to be fixed independently: no deadline can be long enough for every
// stall an operating system can produce, so "the answer did not arrive" must
// never be allowed to MEAN "the service is gone".  Only the transport's own
// reachability evidence decides that -- see service_request_deadline_policy.h,
// and linux_daemon_deadline_policy.h for the Linux original of the same rule.
//
// What a stale read deliberately does NOT do: advance either epoch, invalidate
// live authority, disconnect the model, touch the draft, or re-enable anything.
// The values on screen were true when they were read, they are labelled stale
// on the status line, and the next successful read replaces them atomically
// through the normal accept path.

static void gui_service_handle_transport_failure(gc_u64 connectionEpoch,
    const char* error, const char* reason, bool serviceInstalled,
    bool serviceRunning) {
    GuiServicePhase previousPhase = g_app.guiServiceModel.phase;
    bool wasReady = gui_service_model_ready(&g_app.guiServiceModel);
    bool hadLiveAuthority = g_app.serviceSnapshotAuthoritative ||
        g_app.loaded || g_app.serviceControlStateValid ||
        g_app.serviceActiveDesiredValid || g_app.gpuTemperatureValid;
    bool nextBroken = serviceInstalled;
    char nextError[ARRAY_COUNT(g_app.backgroundServiceError)] = {};
    StringCchCopyA(nextError, ARRAY_COUNT(nextError),
        error && error[0] ? error : "Background service connection lost");
    // The disconnected "not installed" presentation deliberately does not
    // expose transport details, so changing those details must not repaint it.
    bool visibleErrorChanged = serviceInstalled &&
        strcmp(g_app.backgroundServiceError, nextError) != 0;
    bool renderChanged = gui_service_failure_requires_render(
        previousPhase, hadLiveAuthority,
        g_app.backgroundServiceInstalled, g_app.backgroundServiceRunning,
        g_app.backgroundServiceBroken,
        serviceInstalled, serviceRunning, nextBroken,
        visibleErrorChanged);
    g_app.guiManualResyncPending = false;
    gui_service_model_disconnect(&g_app.guiServiceModel, connectionEpoch);
    if (wasReady) gui_mutation_advance_gpu_epoch("service transport lost");
    if (hadLiveAuthority) gui_invalidate_live_authority(reason);
    g_app.backgroundServiceAvailable = false;
    g_app.backgroundServiceInstalled = serviceInstalled;
    g_app.backgroundServiceRunning = serviceRunning;
    g_app.backgroundServiceBroken = nextBroken;
    StringCchCopyA(g_app.backgroundServiceError,
        ARRAY_COUNT(g_app.backgroundServiceError), nextError);
    debug_log_on_change("GUI service state: transport failure epoch=%llu reason=%s error=%s renderChanged=%d\n",
        (unsigned long long)connectionEpoch,
        reason && reason[0] ? reason : "unknown",
        g_app.backgroundServiceError, renderChanged ? 1 : 0);
    if (renderChanged) gui_render_service_phase_only();
    start_service_reconnect_timer_if_needed();
}

// Called by the accept path. A read that answered is proof the service is
// keeping up again, so the stale label is cleared where the fresh values land
// rather than on a timer.
static void gui_service_note_read_answered() {
    if (!g_app.serviceReadStale && g_app.serviceReadMissStreak == 0) return;
    debug_log("GUI service state: read miss cleared after %u consecutive miss(es); live state is current again\n",
        g_app.serviceReadMissStreak);
    g_app.serviceReadStale = false;
    g_app.serviceReadMissStreak = 0;
    update_background_service_controls();
}

static void gui_service_handle_stale_read(
    const GuiServiceIoCompletion* completion) {
    if (!completion) return;
    if (g_app.serviceReadMissStreak < 0xFFFFFFFFu)
        ++g_app.serviceReadMissStreak;
    bool alreadyStale = g_app.serviceReadStale;
    g_app.serviceReadStale = true;
    debug_log("GUI service state: read miss reason=%s streak=%u deadlineExpired=%d requestSubmitted=%d error=%s -- the service is reachable, so the live presentation is KEPT (stale), not torn down\n",
        completion->reason[0] ? completion->reason : "unknown",
        g_app.serviceReadMissStreak,
        completion->sendOutcome.deadlineExpired ? 1 : 0,
        completion->sendOutcome.requestSubmitted ? 1 : 0,
        completion->error[0] ? completion->error : "none");
    // Only the status line changes, and only on the transition into staleness:
    // repainting the whole service control surface once per missed read would
    // be its own visible churn, which is the class of defect this fixes.
    if (!alreadyStale) update_background_service_controls();
}

// The single entry point for a read that did not answer. Which of the two
// answers applies is decided by the transport's reachability evidence, never by
// the error text or by how long the read took.
static void gui_service_handle_read_miss(
    const GuiServiceIoCompletion* completion) {
    if (!completion) return;
    if (service_client_read_miss_preserves_presentation(
            completion->sendOutcome)) {
        gui_service_handle_stale_read(completion);
        return;
    }
    g_app.serviceReadStale = false;
    g_app.serviceReadMissStreak = 0;
    gui_service_handle_transport_failure(completion->connectionEpoch,
        completion->error, completion->reason,
        completion->serviceInstalled, completion->serviceRunning);
}
