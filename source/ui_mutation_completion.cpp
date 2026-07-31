// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT

// Write the outcome of a finished mutation onto the profile status line, and
// return the severity it was presented at so the caller can decide whether that
// line is enough.  Setting a resident child label is NOT a presentation
// transition: no surface is created, nothing takes activation or focus -- which
// is why the presentation-silent tray/hotkey path is allowed to call it.
static gc_u32 set_mutation_result_status_line(const GuiMutationWork* work,
    const GuiMutationCompletion* completion, bool successForUi) {
    if (!work || !completion) return (gc_u32)SERVICE_OUTCOME_SEVERITY_ERROR;
    gc_u32 severity = gui_mutation_result_severity(successForUi,
        completion->response.outcomeSeverity);
    char label[64] = {};
    gui_mutation_result_profile_label(label, sizeof(label),
        (unsigned int)work->profileSource, work->profileSlot, CONFIG_NUM_SLOTS);
    char status[512] = {};
    gui_mutation_result_status_text(status, sizeof(status), work->kind,
        severity, label, completion->result);
    set_profile_status_text("%s", status);
    debug_log("mutation result: kind=%d context=%d origin=%u source=%u slot=%d "
        "successForUi=%d envelopeSeverity=%s presentedSeverity=%s prompt=%d\n",
        (int)work->kind, (int)work->context, (unsigned int)work->origin,
        (unsigned int)work->profileSource, work->profileSlot,
        successForUi ? 1 : 0,
        service_outcome_severity_name(completion->response.outcomeSeverity),
        service_outcome_severity_name(severity),
        gui_mutation_result_needs_prompt(severity) ? 1 : 0);
    return severity;
}

// Background and tray-selected profile completion is deliberately isolated in
// a presentation-silent operation.  It may update the resident GUI's model,
// controls, tray icon, status line, and deferred paint state, but it must never
// create/show a surface or take activation/focus.  build.py keeps that contract
// guarded against future additions of presentation or process-launch APIs here
// and in the auto-profile apply driver.
static void handle_auto_profile_mutation_completion_presentation_silent(
    const GuiMutationWork* work, const GuiMutationCompletion* completion,
    bool successForUi) {
    if (!work || !completion) return;
    if (successForUi) {
        populate_desired_into_gui(&work->desired);
        gui_draft_mark_clean();
    }
    // An explicit tray/hotkey pick is a user action, so the window says what
    // came of it exactly as the Apply button does -- it was reported that
    // switching profiles from the tray left the status line describing whatever
    // happened before it.  A rule-driven foreground switch stays silent: the
    // user did not ask for it, and it must not overwrite a line they are
    // reading.  Written BEFORE the completion callback below, which may queue
    // the next pick and set its own "Applying ..." line.
    if (service_apply_origin_is_explicit(work->origin))
        set_mutation_result_status_line(work, completion, successForUi);
    auto_profile_on_mutation_completed(work->profileSlot, work->origin,
        successForUi, completion->result);
}

// The result of a MANUAL Apply / Reset, i.e. the only two contexts a window is
// allowed to raise a dialog for.  A clean success is confirmed on the profile
// status line and nothing else; anything the user has to acknowledge still gets
// the modal box, with the service's own wording in it.  See
// gui_mutation_result_policy.h for why the severity is not derived here.
static void present_manual_mutation_result(const GuiMutationWork* work,
    const GuiMutationCompletion* completion, bool successForUi) {
    if (!work || !completion) return;
    gc_u32 severity = set_mutation_result_status_line(work, completion,
        successForUi);
    if (!gui_mutation_result_needs_prompt(severity)) return;
    gc_message_box(g_app.hMainWnd, completion->result, "Green Curve",
        MB_OK | MB_ICONWARNING);
}

static void handle_gui_mutation_completion(GuiMutationCompletion* completion) {
    if (!completion) return;
    const GuiMutationWork work = completion->work;

    bool contextCurrent = gui_service_completion_context_current(
        (gc_u64)work.gpuEpoch, (gc_u64)gui_mutation_gpu_epoch(),
        (gc_u64)work.presentationEpoch,
        (gc_u64)gui_service_presentation_epoch());
    bool hasEnvelope = completion->response.magic == SERVICE_PROTOCOL_MAGIC &&
        completion->response.version == SERVICE_PROTOCOL_VERSION &&
        completion->response.state.serviceInstanceId != 0;
    bool sameAuthority = hasEnvelope &&
        completion->response.state.serviceInstanceId ==
            g_app.guiServiceModel.serviceInstanceId &&
        completion->response.state.gpuGeneration ==
            g_app.guiServiceModel.gpuGeneration;
    GuiServiceEnvelopeDecision previewDecision =
        GUI_SERVICE_ENVELOPE_REJECTED_INVALID;
    GuiServiceModel previewModel = g_app.guiServiceModel;
    if (hasEnvelope && contextCurrent) {
        previewDecision = gui_service_model_accept(&previewModel,
            completion->connectionEpoch, &completion->response.state);
    }
    bool canCommitSuccess = gui_service_mutation_result_can_commit(
        completion->success, contextCurrent, sameAuthority,
        previewDecision, &previewModel);

    // Clear the draft only when a pure reducer preview proves that this
    // successful result belongs to the current presentation and the exact
    // service/GPU generation that authorized the mutation.  A PnP/tray/service
    // transition can otherwise deliver an old successful completion after live
    // authority was invalidated, silently destroying the preserved draft.
    if (canCommitSuccess) {
        set_gui_state_dirty(false);
        if (work.kind == GUI_MUTATION_RESET) {
            g_app.guiFanMode = FAN_MODE_AUTO;
            g_app.guiFanFixedPercent = 0;
            fan_curve_set_default(&g_app.guiFanCurve);
            g_app.lockedVi = -1;
            g_app.lockedCi = -1;
            g_app.lockedFreq = 0;
            g_app.lockMode = LOCK_MODE_NONE;
            memset(g_app.appliedCurveMHz, 0,
                sizeof(g_app.appliedCurveMHz));
        }
    }

    bool envelopeAccepted = false;
    if (hasEnvelope && contextCurrent) {
        envelopeAccepted = gui_service_accept_response_on_main_thread(
            &completion->response,
            completion->connectionEpoch, GUI_SERVICE_IO_FULL_SYNC, true,
            work.kind == GUI_MUTATION_RESET
                ? "reset result" : "apply result");
    } else if (hasEnvelope) {
        debug_log("GUI service state: dropped mutation completion operation=%llu gpuEpoch=%ld/%ld presentationEpoch=%ld/%ld instance=%llu revision=%llu generation=%llu\n",
            (unsigned long long)work.request.operationId,
            work.gpuEpoch, gui_mutation_gpu_epoch(),
            work.presentationEpoch, gui_service_presentation_epoch(),
            (unsigned long long)completion->response.state.serviceInstanceId,
            (unsigned long long)completion->response.state.stateRevision,
            (unsigned long long)completion->response.state.gpuGeneration);
    } else if (completion->transportAttempted && contextCurrent) {
        gui_service_handle_transport_failure(completion->connectionEpoch,
            completion->result, "mutation transport",
            completion->serviceInstalled, completion->serviceRunning);
    }

    bool successForUi = canCommitSuccess && envelopeAccepted &&
        gui_service_completion_context_current(
            (gc_u64)work.gpuEpoch, (gc_u64)gui_mutation_gpu_epoch(),
            (gc_u64)work.presentationEpoch,
            (gc_u64)gui_service_presentation_epoch()) &&
        gui_service_model_ready(&g_app.guiServiceModel);
    if (successForUi) {
        gui_draft_mark_clean();
    } else if (completion->success) {
        set_message(completion->result, sizeof(completion->result),
            "GPU operation completed, but the service or selected-GPU presentation changed before its result could be adopted. Unsaved draft preserved; synchronizing current state.");
    }

    if (work.context == GUI_MUTATION_CONTEXT_MANUAL_APPLY) {
        if (successForUi) {
            if (work.profileSource == SERVICE_PROFILE_SOURCE_SHARED_SLOT)
                g_app.loadedSharedSlot = work.profileSlot;
            sync_applied_profile_from_service_metadata();
        }
        present_manual_mutation_result(&work, completion, successForUi);
    } else if (work.context == GUI_MUTATION_CONTEXT_MANUAL_RESET) {
        present_manual_mutation_result(&work, completion, successForUi);
    } else if (work.context == GUI_MUTATION_CONTEXT_AUTO_PROFILE) {
        handle_auto_profile_mutation_completion_presentation_silent(
            &work, completion, successForUi);
    } else if (work.context == GUI_MUTATION_CONTEXT_APP_LAUNCH) {
        if (successForUi) {
            populate_desired_into_gui(&work.desired);
            gui_draft_mark_clean();
        }
        app_launch_on_mutation_completed(work.profileSlot,
            successForUi, completion->result);
    }

    HeapFree(GetProcessHeap(), 0, completion);
    gui_mutation_acknowledge_and_dispatch_next();
}

static void gui_mutation_pending_was_superseded(
    GuiMutationUiContext context, int profileSlot,
    ServiceApplyOrigin origin, gc_u64 operationId) {
    (void)origin;
    debug_log("GUI service I/O: pending operation=%llu context=%d slot=%d superseded before dispatch\n",
        (unsigned long long)operationId, (int)context, profileSlot);
    if (context == GUI_MUTATION_CONTEXT_AUTO_PROFILE) {
        auto_profile_on_mutation_superseded(profileSlot);
    } else if (context == GUI_MUTATION_CONTEXT_APP_LAUNCH) {
        set_profile_status_text(
            "App-start profile %d was superseded by a newer GPU operation.",
            profileSlot);
    }
}
