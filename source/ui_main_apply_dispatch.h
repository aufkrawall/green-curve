// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#pragma once
// Window-thread Apply dispatch, isolated for executing request-lifetime tests.
GC_NOINLINE static void apply_changes() {
    const bool mappedSource = g_app.guiSource.valid;
    if ((mappedSource && !gui_profile_source_mapped_ready()) || gui_profile_capture_active()) {
        set_profile_status_text("Review this source against the destination before Apply.");
        return;
    }
    if (!gui_service_model_ready(&g_app.guiServiceModel) ||
        (!mappedSource && (!g_app.guiDraft.attached || g_app.guiDraft.detached))) return;
    char gpuSelectionErr[256] = {};
    if (!validate_configured_gpu_selection_for_client(
            gpuSelectionErr, sizeof(gpuSelectionErr))) {
        gc_message_box(g_app.hMainWnd,
            gpuSelectionErr[0] ? gpuSelectionErr :
                "Select the intended GPU again before applying settings.",
            "Green Curve", MB_OK | MB_ICONWARNING);
        return;
    }
    set_pending_operation_source("GUI apply");
    record_ui_action("Apply clicked");
    auto desiredPtr = std::make_unique<DesiredSettings>();
    DesiredSettings& desired = *desiredPtr;
    auto baselinePtr = std::make_unique<OcApplyBaseline>();
    OcApplyBaseline& baseline = *baselinePtr;
    gc_u64 controlRevision = 0;
    char err[256] = {};
    if (mappedSource && !gui_profile_source_prepare_apply(desired, controlRevision, baseline)) {
        set_profile_status_text("The mapped source review expired; check this source again."); return;
    }
    if (!mappedSource && !capture_gui_apply_settings(&desired, &baseline, err, sizeof(err))) {
        write_error_report_log_for_user_failure("GUI apply validation failed", err);
        gc_message_box(g_app.hMainWnd, err, "Green Curve", MB_OK | MB_ICONERROR);
        return;
    }
    // A proven prefix conflict with the lock: offer to lower those points in
    // the draft (only the draft changes) instead of sending a request the
    // service would refuse.  Before the high-OC gate, which would otherwise
    // ask about a request that never leaves the editor.
    if (!mappedSource && gui_curve_prepare_intercept_apply(&desired)) return;
    // Gate AFTER capture: the captured request is the exact intent that would
    // reach the hardware, and capture has already established that something
    // actually changed, so an unchanged high clock never reaches the dialog.
    if (!confirm_high_oc_apply(&desired, &baseline, mappedSource)) return;
    // Same gate-after-capture rationale; inert unless the probe positively
    // reported a unified memory pool.
    if (!confirm_unified_memory_write(&desired)) return;
    int requestedSlot = (int)SendMessageA(g_app.hProfileCombo, CB_GETCURSEL, 0, 0);
    if (requestedSlot < 0) requestedSlot = CONFIG_DEFAULT_SLOT - 1;
    requestedSlot += 1;
    ServiceProfileSource requestedSource = SERVICE_PROFILE_SOURCE_USER_SLOT;
    if (g_app.loadedSharedSlot >= 1 && g_app.loadedSharedSlot <= CONFIG_NUM_SLOTS) {
        requestedSource = SERVICE_PROFILE_SOURCE_SHARED_SLOT;
        requestedSlot = g_app.loadedSharedSlot;
    }
    if (mappedSource) { requestedSource = SERVICE_PROFILE_SOURCE_AD_HOC; requestedSlot = 0; }
    // Modal confirmations can dispatch edits, reconnects or another capture.
    if (mappedSource) {
        auto checkedPtr = std::make_unique<DesiredSettings>();
        auto ignoredPtr = std::make_unique<OcApplyBaseline>();
        gc_u64 currentRevision = 0;
        if (!gui_profile_source_prepare_apply(*checkedPtr, currentRevision, *ignoredPtr) || currentRevision != controlRevision ||
            memcmp(checkedPtr.get(), &desired, sizeof(desired))) {
            set_profile_status_text("The mapped source changed during confirmation; check it again."); return;
        }
    }
    char queueStatus[512] = {};
    if (!gui_mutation_queue_apply(&desired, true, SERVICE_APPLY_ORIGIN_GUI,
            requestedSource, requestedSlot, GUI_MUTATION_CONTEXT_MANUAL_APPLY,
            "GUI apply", queueStatus, sizeof(queueStatus), controlRevision)) {
        gc_message_box(g_app.hMainWnd, queueStatus, "Green Curve",
            MB_OK | MB_ICONWARNING);
    } else {
        set_profile_status_text("%s", queueStatus);
    }
}
