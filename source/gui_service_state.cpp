// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT

// Main-thread reducer/application layer for protocol-v11 service envelopes.
// This is the only runtime path that turns service responses into AppData/HWND
// state.  The service-I/O worker above posts immutable completions here.

#include "gui_draft_policy.h"
#include "gui_service_io_queue_policy.h"

static bool g_guiRenderTransactionActive = false;
static bool g_guiRebuildPreserveDraft = false;

static const char* gui_service_phase_name(GuiServicePhase phase) {
    switch (phase) {
        case GUI_SERVICE_DISCONNECTED: return "disconnected";
        case GUI_SERVICE_SYNCING: return "syncing";
        case GUI_SERVICE_DEVICE_MISSING: return "device missing";
        case GUI_SERVICE_RECOVERING: return "recovering";
        case GUI_SERVICE_DEGRADED: return "degraded";
        case GUI_SERVICE_READY: return "ready";
        default: return "unknown";
    }
}

static const char* gui_service_envelope_decision_name(
    GuiServiceEnvelopeDecision decision) {
    switch (decision) {
        case GUI_SERVICE_ENVELOPE_ACCEPTED: return "accepted";
        case GUI_SERVICE_ENVELOPE_REJECTED_CONNECTION: return "old connection";
        case GUI_SERVICE_ENVELOPE_REJECTED_INSTANCE: return "retired instance";
        case GUI_SERVICE_ENVELOPE_REJECTED_GENERATION: return "old GPU generation";
        case GUI_SERVICE_ENVELOPE_REJECTED_REVISION: return "old revision";
        case GUI_SERVICE_ENVELOPE_REJECTED_INVALID: return "invalid envelope";
        default: return "unknown decision";
    }
}

static bool gui_gpu_identity_equal(const GpuAdapterInfo* a,
    const GpuAdapterInfo* b) {
    if (!a || !b || !a->valid || !b->valid) return false;
    return a->deviceId == b->deviceId &&
        a->subSystemId == b->subSystemId &&
        a->pciRevisionId == b->pciRevisionId &&
        a->extDeviceId == b->extDeviceId &&
        a->pciDomain == b->pciDomain &&
        a->pciBus == b->pciBus &&
        a->pciDevice == b->pciDevice &&
        a->pciFunction == b->pciFunction;
}

static gc_u64 gui_render_topology_signature() {
    gc_u64 hash = g_app.guiServiceModel.topologySignature;
    if (!hash) hash = 1469598103934665603ULL;
    hash = service_state_hash_u32(hash, (gc_u32)g_app.numVisible);
    for (int vi = 0; vi < g_app.numVisible && vi < VF_NUM_POINTS; ++vi)
        hash = service_state_hash_u32(hash, (gc_u32)g_app.visibleMap[vi]);
    return hash ? hash : 1;
}

static void gui_draft_bind_current_ready_state() {
    g_app.guiDraft.gpu = g_app.selectedGpu;
    g_app.guiDraft.topologySignature = gui_render_topology_signature();
    g_app.guiDraft.attached = g_app.selectedGpu.valid &&
        gui_service_model_ready(&g_app.guiServiceModel);
    g_app.guiDraft.detached = false;
}

static void gui_draft_capture_clean_projection() {
    memset(g_app.guiDraft.curveMHz, 0, sizeof(g_app.guiDraft.curveMHz));
    memset(g_app.guiDraft.curveValueValid, 0,
        sizeof(g_app.guiDraft.curveValueValid));
    memset(g_app.guiDraft.curveText, 0, sizeof(g_app.guiDraft.curveText));
    for (int vi = 0; vi < g_app.numVisible; ++vi) {
        int ci = g_app.visibleMap[vi];
        if (ci < 0 || ci >= VF_NUM_POINTS) continue;
        unsigned int value = g_app.appliedCurveMHz[ci];
        if (!value) value = displayed_curve_mhz(g_app.curve[ci].freq_kHz);
        if (!value) continue;
        g_app.guiDraft.curveMHz[ci] = value;
        g_app.guiDraft.curveValueValid[ci] = true;
        StringCchPrintfA(g_app.guiDraft.curveText[ci],
            ARRAY_COUNT(g_app.guiDraft.curveText[ci]), "%u", value);
    }
    StringCchPrintfA(g_app.guiDraft.gpuOffsetText,
        ARRAY_COUNT(g_app.guiDraft.gpuOffsetText), "%d",
        g_app.guiGpuOffsetMHz);
    StringCchPrintfA(g_app.guiDraft.gpuOffsetExcludeLowText,
        ARRAY_COUNT(g_app.guiDraft.gpuOffsetExcludeLowText), "%d",
        g_app.guiGpuOffsetExcludeLowCount);
    StringCchPrintfA(g_app.guiDraft.memOffsetText,
        ARRAY_COUNT(g_app.guiDraft.memOffsetText), "%d",
        g_app.guiMemOffsetMHz);
    StringCchPrintfA(g_app.guiDraft.powerLimitText,
        ARRAY_COUNT(g_app.guiDraft.powerLimitText), "%d",
        g_app.guiPowerLimitPct);
    StringCchPrintfA(g_app.guiDraft.fanFixedText,
        ARRAY_COUNT(g_app.guiDraft.fanFixedText), "%d",
        g_app.guiFanFixedPercent);
    gui_draft_bind_current_ready_state();
}

static void gui_draft_begin_user_edit() {
    if (gui_state_dirty()) return;
    gui_draft_capture_clean_projection();
    debug_log("GUI draft: began for topology=%llu GPU=%04X:%02X:%02X.%u\n",
        (unsigned long long)g_app.guiDraft.topologySignature,
        g_app.selectedGpu.pciDomain, g_app.selectedGpu.pciBus,
        g_app.selectedGpu.pciDevice, g_app.selectedGpu.pciFunction);
}

static void gui_draft_capture_curve_value(int ci, const char* text) {
    if (ci < 0 || ci >= VF_NUM_POINTS) return;
    gui_draft_begin_user_edit();
    int value = 0;
    bool valid = text && parse_int_strict(text, &value) && value > 0;
    g_app.guiDraft.curveValueValid[ci] = valid;
    g_app.guiDraft.curveMHz[ci] = valid ? (unsigned int)value : 0;
    StringCchCopyA(g_app.guiDraft.curveText[ci],
        ARRAY_COUNT(g_app.guiDraft.curveText[ci]), text ? text : "");
}

static void gui_draft_capture_text(char* destination,
    size_t destinationCount, const char* text) {
    gui_draft_begin_user_edit();
    if (!destination || destinationCount == 0) return;
    StringCchCopyA(destination, destinationCount, text ? text : "");
}

static void gui_draft_capture_desired(const DesiredSettings* desired) {
    if (!desired) return;
    gui_draft_begin_user_edit();
    if (!g_app.guiDraft.gpu.valid) {
        g_app.guiDraft.pendingDesired = *desired;
        g_app.guiDraft.pendingDesiredValid = true;
    }
    for (int ci = 0; ci < VF_NUM_POINTS; ++ci) {
        if (!desired->hasCurvePoint[ci]) continue;
        g_app.guiDraft.curveValueValid[ci] = desired->curvePointMHz[ci] > 0;
        g_app.guiDraft.curveMHz[ci] = desired->curvePointMHz[ci];
        StringCchPrintfA(g_app.guiDraft.curveText[ci],
            ARRAY_COUNT(g_app.guiDraft.curveText[ci]), "%u",
            desired->curvePointMHz[ci]);
    }
    if (desired->hasGpuOffset) {
        StringCchPrintfA(g_app.guiDraft.gpuOffsetText,
            ARRAY_COUNT(g_app.guiDraft.gpuOffsetText), "%d",
            desired->gpuOffsetMHz);
        StringCchPrintfA(g_app.guiDraft.gpuOffsetExcludeLowText,
            ARRAY_COUNT(g_app.guiDraft.gpuOffsetExcludeLowText), "%d",
            desired->gpuOffsetExcludeLowCount);
    }
    if (desired->hasMemOffset)
        StringCchPrintfA(g_app.guiDraft.memOffsetText,
            ARRAY_COUNT(g_app.guiDraft.memOffsetText), "%d",
            desired->memOffsetMHz);
    if (desired->hasPowerLimit)
        StringCchPrintfA(g_app.guiDraft.powerLimitText,
            ARRAY_COUNT(g_app.guiDraft.powerLimitText), "%d",
            desired->powerLimitPct);
    if (desired->hasFan && desired->fanMode == FAN_MODE_FIXED)
        StringCchPrintfA(g_app.guiDraft.fanFixedText,
            ARRAY_COUNT(g_app.guiDraft.fanFixedText), "%d",
            desired->fanPercent);
}

static void gui_draft_mark_clean() {
    g_app.guiDraft.detached = false;
    g_app.guiDraft.pendingDesiredValid = false;
    memset(&g_app.guiDraft.pendingDesired, 0,
        sizeof(g_app.guiDraft.pendingDesired));
    if (gui_service_model_ready(&g_app.guiServiceModel))
        gui_draft_capture_clean_projection();
}

static void gui_draft_discard() {
    set_gui_state_dirty(false);
    g_app.guiHasUserModifiedValues = false;
    g_app.loadedSharedSlot = 0;
    g_app.guiDraft.attached = false;
    g_app.guiDraft.detached = false;
    g_app.guiDraft.pendingDesiredValid = false;
    memset(&g_app.guiDraft.pendingDesired, 0,
        sizeof(g_app.guiDraft.pendingDesired));
    memset(&g_app.guiDraft.gpu, 0, sizeof(g_app.guiDraft.gpu));
    g_app.guiDraft.topologySignature = 0;
    memset(g_app.guiDraft.curveMHz, 0, sizeof(g_app.guiDraft.curveMHz));
    memset(g_app.guiDraft.curveValueValid, 0,
        sizeof(g_app.guiDraft.curveValueValid));
    memset(g_app.guiDraft.curveText, 0, sizeof(g_app.guiDraft.curveText));
    debug_log("GUI draft: explicitly discarded by user\n");
}

static void gui_set_editor_enabled(bool ready) {
    bool allowDraft = ready && g_app.guiDraft.attached &&
        !g_app.guiDraft.detached;
    for (int vi = 0; vi < g_app.numVisible; ++vi) {
        bool tailDisabled = g_app.lockedVi >= 0 && vi > g_app.lockedVi;
        if (g_app.hEditsMhz[vi])
            EnableWindow(g_app.hEditsMhz[vi],
                allowDraft && !tailDisabled ? TRUE : FALSE);
        if (g_app.hLocks[vi])
            EnableWindow(g_app.hLocks[vi],
                allowDraft && !tailDisabled ? TRUE : FALSE);
    }
    HWND liveControls[] = {
        g_app.hGpuOffsetEdit, g_app.hGpuOffsetExcludeLowEdit,
        g_app.hMemOffsetEdit, g_app.hPowerLimitEdit, g_app.hFanEdit,
        g_app.hFanModeCombo, g_app.hFanCurveBtn,
        g_app.hApplyBtn, g_app.hResetBtn,
    };
    for (HWND control : liveControls) {
        if (control) EnableWindow(control, allowDraft ? TRUE : FALSE);
    }
    // A detached draft must not make its recovery path unreachable.  Keep GPU
    // selection available for a coherent READY model so the user can reselect
    // the draft's original GPU; editing and hardware mutations remain blocked.
    if (g_app.hGpuSelectCombo) {
        bool canSelectGpu = ready &&
            (g_app.adapterCount > 1 ||
             g_app.configuredGpuSelectionUnresolved);
        EnableWindow(g_app.hGpuSelectCombo, canSelectGpu ? TRUE : FALSE);
    }
    if (g_app.hRefreshBtn) EnableWindow(g_app.hRefreshBtn, TRUE);
}

static void gui_project_attached_draft_to_controls() {
    begin_programmatic_edit_update();
    for (int vi = 0; vi < g_app.numVisible; ++vi) {
        int ci = g_app.visibleMap[vi];
        if (ci < 0 || ci >= VF_NUM_POINTS) continue;
        const char* curveText = g_app.guiDraft.curveText[ci];
        if (g_app.hEditsMhz[vi]) SetWindowTextA(g_app.hEditsMhz[vi],
            curveText && curveText[0] ? curveText : "");
        if (g_app.hEditsMv[vi])
            set_edit_value(g_app.hEditsMv[vi],
                g_app.curve[ci].volt_uV / 1000);
        if (g_app.hLocks[vi]) {
            SendMessageA(g_app.hLocks[vi], BM_SETCHECK,
                vi == g_app.lockedVi ? BST_CHECKED : BST_UNCHECKED, 0);
        }
    }
    if (g_app.hGpuOffsetEdit)
        SetWindowTextA(g_app.hGpuOffsetEdit, g_app.guiDraft.gpuOffsetText);
    if (g_app.hGpuOffsetExcludeLowEdit)
        SetWindowTextA(g_app.hGpuOffsetExcludeLowEdit,
            g_app.guiDraft.gpuOffsetExcludeLowText);
    if (g_app.hMemOffsetEdit)
        SetWindowTextA(g_app.hMemOffsetEdit, g_app.guiDraft.memOffsetText);
    if (g_app.hPowerLimitEdit)
        SetWindowTextA(g_app.hPowerLimitEdit,
            g_app.guiDraft.powerLimitText);
    if (g_app.hFanEdit)
        SetWindowTextA(g_app.hFanEdit, g_app.guiDraft.fanFixedText);
    end_programmatic_edit_update();
    gui_set_editor_enabled(true);
}

static void gui_invalidate_live_authority(const char* reason) {
    g_app.serviceSnapshotAuthoritative = false;
    g_app.loaded = false;
    g_app.serviceControlStateValid = false;
    memset(&g_app.serviceControlState, 0, sizeof(g_app.serviceControlState));
    g_app.serviceActiveDesiredValid = false;
    memset(&g_app.serviceActiveDesired, 0, sizeof(g_app.serviceActiveDesired));
    g_app.serviceActiveProfileSource = SERVICE_PROFILE_SOURCE_NONE;
    g_app.serviceActiveProfileSlot = 0;
    g_app.gpuTemperatureValid = false;
    g_app.fanCurveRuntimeActive = false;
    g_app.fanFixedRuntimeActive = false;
    if (!gui_state_dirty()) {
        begin_programmatic_edit_update();
        for (int vi = 0; vi < g_app.numVisible; ++vi) {
            if (g_app.hEditsMhz[vi])
                SetWindowTextA(g_app.hEditsMhz[vi], "");
            if (g_app.hEditsMv[vi])
                SetWindowTextA(g_app.hEditsMv[vi], "");
        }
        HWND liveValueControls[] = {
            g_app.hGpuOffsetEdit, g_app.hGpuOffsetExcludeLowEdit,
            g_app.hMemOffsetEdit, g_app.hPowerLimitEdit,
            g_app.hFanEdit,
        };
        for (HWND control : liveValueControls)
            if (control) SetWindowTextA(control, "");
        end_programmatic_edit_update();
    }
    debug_log_on_change("GUI service state: live authority invalid (%s), draftDirty=%d attached=%d detached=%d\n",
        reason && reason[0] ? reason : "unknown",
        gui_state_dirty() ? 1 : 0,
        g_app.guiDraft.attached ? 1 : 0,
        g_app.guiDraft.detached ? 1 : 0);
}

static const char* gui_service_overlay_title() {
    switch (g_app.guiServiceModel.phase) {
        case GUI_SERVICE_SYNCING: return "Synchronizing GPU state";
        case GUI_SERVICE_DEVICE_MISSING: return "Selected GPU disconnected";
        case GUI_SERVICE_RECOVERING: return "GPU reconnecting";
        case GUI_SERVICE_DEGRADED: return "Live GPU state unavailable";
        case GUI_SERVICE_DISCONNECTED:
        default: return "Background service unavailable";
    }
}

static void gui_draw_service_overlay(HDC hdc, const RECT* client) {
    if (!hdc || !client ||
        gui_service_model_ready(&g_app.guiServiceModel)) return;
    RECT graph = {0, 0, client->right, main_layout_graph_height()};
    HBRUSH background = CreateSolidBrush(RGB(24, 25, 31));
    FillRect(hdc, &graph, background);
    DeleteObject(background);

    int width = nvmin(dp(560), nvmax(dp(260), graph.right - dp(40)));
    int height = gui_state_dirty() ? dp(112) : dp(82);
    RECT panel = {
        (graph.right - width) / 2,
        nvmax(dp(20), (graph.bottom - height) / 2),
        (graph.right + width) / 2,
        nvmax(dp(20), (graph.bottom - height) / 2) + height,
    };
    HBRUSH panelBrush = CreateSolidBrush(RGB(39, 41, 50));
    FillRect(hdc, &panel, panelBrush);
    DeleteObject(panelBrush);
    HBRUSH border = CreateSolidBrush(RGB(82, 86, 103));
    FrameRect(hdc, &panel, border);
    DeleteObject(border);

    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, COL_TEXT);
    HFONT previous = (HFONT)SelectObject(hdc,
        g_app.hCachedFont ? g_app.hCachedFont : GetStockObject(DEFAULT_GUI_FONT));
    RECT title = panel;
    title.top += dp(18);
    title.left += dp(18);
    title.right -= dp(18);
    DrawTextA(hdc, gui_service_overlay_title(), -1, &title,
        DT_CENTER | DT_TOP | DT_SINGLELINE | DT_NOPREFIX);
    SetTextColor(hdc, COL_LABEL);
    RECT detail = title;
    detail.top += dp(30);
    DrawTextA(hdc,
        gui_state_dirty()
            ? "Hardware actions are disabled. Unsaved draft preserved."
            : "Hardware actions are disabled until one coherent READY snapshot arrives.",
        -1, &detail, DT_CENTER | DT_TOP | DT_WORDBREAK | DT_NOPREFIX);
    SelectObject(hdc, previous);
}

static void gui_render_service_phase_only() {
    gui_set_editor_enabled(false);
    update_background_service_controls();
    update_tray_icon();
    invalidate_main_window();
}

static void gui_service_begin_full_sync(const char* reason) {
    if (g_app.guiServiceModel.phase != GUI_SERVICE_SYNCING)
        gui_service_advance_presentation_epoch(reason);
    gc_u64 epoch = gui_service_io_connection_epoch();
    gui_service_model_begin_sync(&g_app.guiServiceModel, epoch);
    gui_invalidate_live_authority(reason);
    gui_render_service_phase_only();
    if (!gui_service_io_queue_full_sync(reason)) {
        gui_service_model_disconnect(&g_app.guiServiceModel, epoch);
        g_app.backgroundServiceAvailable = false;
        g_app.backgroundServiceBroken = true;
        StringCchCopyA(g_app.backgroundServiceError,
            ARRAY_COUNT(g_app.backgroundServiceError),
            "Could not start the background service synchronization worker");
        gui_render_service_phase_only();
    }
}

static void gui_service_retry_full_sync(const char* reason) {
    // Retry/backstop probes must not churn presentation. A successful envelope
    // will transition atomically to its accepted phase; a repeated failure is
    // handled change-gated below while the current overlay remains untouched.
    if (!gui_service_io_queue_full_sync(reason)) {
        debug_log_on_change("GUI service state: silent retry queue failed (%s)\n",
            reason && reason[0] ? reason : "background retry");
    }
}

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

static void gui_apply_ready_envelope(const ServiceResponse* response,
    GuiServiceIoKind kind, bool redrawControls, const char* reason,
    gc_u64 oldRenderTopology, bool wasDirty,
    bool authorityOrTopologyChanged) {
    // Treat the complete authoritative adoption as one programmatic editor
    // transaction. Nested SetWindowText/selection notifications must never
    // manufacture a dirty draft that can later preserve stale OC values across
    // a service/GPU generation change.
    begin_programmatic_edit_update();
    bool rebaseUnkeyedDesired = wasDirty &&
        !g_app.guiDraft.gpu.valid &&
        g_app.guiDraft.pendingDesiredValid;
    DesiredSettings pendingDesired = {};
    int pendingSharedSlot = g_app.loadedSharedSlot;
    if (rebaseUnkeyedDesired) {
        pendingDesired = g_app.guiDraft.pendingDesired;
        // Let the coherent response establish every omitted field. The saved
        // sparse overlay is reapplied after any topology rebuild below.
        set_gui_state_dirty(false);
    }
    char intentComparison[160] = {};
    bool activeDesiredChanged =
        g_app.serviceActiveDesiredValid != response->state.activeDesiredValid ||
        (response->state.activeDesiredValid &&
         !desired_settings_match_active_service_intent(
             &g_app.serviceActiveDesired, &response->desired,
             intentComparison, sizeof(intentComparison)));
    bool controlsMissing = g_app.numVisible > 0 && !g_app.hEditsMhz[0];
    bool suppressRedraw = gui_state_adoption_requires_redraw_suppression(
        kind == GUI_SERVICE_IO_TELEMETRY, authorityOrTopologyChanged,
        activeDesiredChanged, g_guiForceFullRefresh, controlsMissing);
    GuiTopLevelRedrawTransaction redrawTransaction = {};
    if (suppressRedraw) {
        g_guiRenderTransactionActive = true;
        gui_top_level_redraw_begin(&redrawTransaction, g_app.hMainWnd,
            reason);
    }
    apply_service_snapshot_to_app(&response->snapshot);
    g_app.serviceSnapshotAuthoritative = true;
    if (!suppressRedraw && g_guiForceFullRefresh) {
        // Snapshot adoption can discover an out-of-band Reset that requires a
        // full editor/lock rebase. Start suppression before any HWND projection
        // below even though the request arrived through the telemetry lane.
        suppressRedraw = true;
        g_guiRenderTransactionActive = true;
        gui_top_level_redraw_begin(&redrawTransaction, g_app.hMainWnd,
            reason);
    }
    g_app.serviceActiveDesiredValid = response->state.activeDesiredValid;
    if (response->state.activeDesiredValid) {
        g_app.serviceActiveDesired = response->desired;
        // A normal telemetry envelope updates live data and the model without
        // rewriting every edit HWND once per second.  Project active intent
        // only when it actually changed or this is a structural sync.
        if (activeDesiredChanged || kind != GUI_SERVICE_IO_TELEMETRY)
            apply_service_desired_to_gui(&response->desired);
    } else {
        memset(&g_app.serviceActiveDesired, 0,
            sizeof(g_app.serviceActiveDesired));
        memset(g_app.appliedCurveMHz, 0,
            sizeof(g_app.appliedCurveMHz));
    }
    if ((response->state.validSections &
            SERVICE_STATE_SECTION_APPLIED_CONTROLS) != 0) {
        apply_control_state_to_gui(&response->controlState);
    }

    gc_u64 newRenderTopology = gui_render_topology_signature();
    bool topologyChanged = oldRenderTopology != newRenderTopology;
    if (wasDirty && !g_app.guiDraft.gpu.valid &&
        g_app.selectedGpu.valid) {
        // Profiles may be loaded while the service is disconnected before the
        // process has ever accepted adapter identity. Bind that unkeyed draft
        // exactly once, to the first coherent READY identity/topology.
        g_app.guiDraft.gpu = g_app.selectedGpu;
        g_app.guiDraft.topologySignature = newRenderTopology;
        debug_log("GUI draft: bound previously unkeyed reconnect draft to first READY GPU/topology=%llu\n",
            (unsigned long long)newRenderTopology);
    }
    bool sameGpu = gui_gpu_identity_equal(
        &g_app.guiDraft.gpu, &g_app.selectedGpu);
    bool sameTopology = g_app.guiDraft.topologySignature == newRenderTopology;
    GuiDraftReconcileDecision draftDecision = gui_draft_reconcile_decide(
        wasDirty, sameGpu, sameTopology);
    if (draftDecision != GUI_DRAFT_REBASE_CLEAN) {
        g_app.guiDraft.attached = draftDecision == GUI_DRAFT_ATTACH_DIRTY;
        g_app.guiDraft.detached = draftDecision == GUI_DRAFT_DETACH_DIRTY;
        debug_log("GUI draft: recovery attach=%d sameGpu=%d sameTopology=%d oldTopology=%llu newTopology=%llu\n",
            g_app.guiDraft.attached ? 1 : 0, sameGpu ? 1 : 0,
            sameTopology ? 1 : 0,
            (unsigned long long)g_app.guiDraft.topologySignature,
            (unsigned long long)newRenderTopology);
    } else {
        gui_draft_bind_current_ready_state();
    }

    bool needsFullRender = gui_state_adoption_requires_full_render(
        kind == GUI_SERVICE_IO_FULL_SYNC, authorityOrTopologyChanged,
        activeDesiredChanged, topologyChanged, rebaseUnkeyedDesired,
        g_guiForceFullRefresh, controlsMissing);
    if (needsFullRender) {
        g_guiForceFullRefresh = false;
        if (topologyChanged || (g_app.numVisible > 0 && !g_app.hEditsMhz[0])) {
            g_guiRebuildPreserveDraft = wasDirty && g_app.guiDraft.attached;
            rebuild_edit_controls();
            g_guiRebuildPreserveDraft = false;
        }
        populate_global_controls();
        if (rebaseUnkeyedDesired && g_app.guiDraft.attached) {
            // Build a complete clean baseline from this READY generation, then
            // layer the offline sparse profile over it. This prevents omitted
            // globals/points from becoming empty text or startup defaults.
            populate_edits();
            gui_draft_capture_clean_projection();
            populate_desired_into_gui(&pendingDesired);
            g_app.loadedSharedSlot = pendingSharedSlot;
            set_gui_state_dirty(true);
            gui_draft_capture_desired(&pendingDesired);
            g_app.guiDraft.pendingDesiredValid = false;
            memset(&g_app.guiDraft.pendingDesired, 0,
                sizeof(g_app.guiDraft.pendingDesired));
            debug_log("GUI draft: rebased pre-READY sparse desired overlay on coherent live baseline\n");
        } else if (wasDirty && g_app.guiDraft.attached) {
            gui_project_attached_draft_to_controls();
        } else {
            populate_edits();
            if (!wasDirty) gui_draft_capture_clean_projection();
        }
        sync_applied_profile_from_service_metadata();
        update_background_service_controls();
        gui_set_editor_enabled(true);
        debug_log("GUI render transaction: reason=%s topologyChanged=%d old=%llu new=%llu dirty=%d attached=%d\n",
            reason && reason[0] ? reason : "state envelope",
            topologyChanged ? 1 : 0,
            (unsigned long long)oldRenderTopology,
            (unsigned long long)newRenderTopology,
            wasDirty ? 1 : 0, g_app.guiDraft.attached ? 1 : 0);
    } else {
        // Stable telemetry updates only the live fan projection and tray. It
        // must not re-enable/repaint the entire editor/control tree each tick.
        sync_fan_ui_from_cached_state(redrawControls);
    }
    if (suppressRedraw) {
        UINT redrawFlags = RDW_INVALIDATE | RDW_ALLCHILDREN;
        if (needsFullRender || redrawControls)
            redrawFlags |= RDW_UPDATENOW;
        gui_top_level_redraw_end(&redrawTransaction, redrawFlags, reason);
        g_guiRenderTransactionActive = false;
    }
    update_tray_icon();
    gui_selected_gpu_notification_refresh(&g_app.selectedGpu);
    if (authorityOrTopologyChanged && !wasDirty &&
        !response->state.activeDesiredValid) {
        set_profile_status_text(
            "GPU state reacquired. No Green Curve settings are currently active; showing authoritative live GPU values.");
        debug_log("GUI service state: new authority has no active desired intent; clean editor rebased to authoritative live values\n");
    }
    if (!g_app.logonServiceReadinessPending)
        KillTimer(g_app.hMainWnd, SERVICE_RECONNECT_TIMER_ID);
    end_programmatic_edit_update();
}

static bool gui_service_accept_response_on_main_thread(
    const ServiceResponse* response, gc_u64 connectionEpoch,
    GuiServiceIoKind kind, bool redrawControls, const char* reason) {
    if (!response) return false;
    GuiServicePhase previousPhase = g_app.guiServiceModel.phase;
    gc_u64 previousInstance = g_app.guiServiceModel.serviceInstanceId;
    gc_u64 previousGeneration = g_app.guiServiceModel.gpuGeneration;
    gc_u64 previousTopology = g_app.guiServiceModel.topologySignature;
    gc_u64 oldRenderTopology = gui_render_topology_signature();
    bool wasDirty = gui_state_dirty();

    GuiServiceEnvelopeDecision decision = gui_service_model_accept(
        &g_app.guiServiceModel, connectionEpoch, &response->state);
    if (decision != GUI_SERVICE_ENVELOPE_ACCEPTED) {
        debug_log("GUI service state: rejected envelope decision=%s epoch=%llu instance=%llu revision=%llu gpuGeneration=%llu phase=%u valid=0x%02X reason=%s\n",
            gui_service_envelope_decision_name(decision),
            (unsigned long long)connectionEpoch,
            (unsigned long long)response->state.serviceInstanceId,
            (unsigned long long)response->state.stateRevision,
            (unsigned long long)response->state.gpuGeneration,
            response->state.gpuPhase, response->state.validSections,
            reason && reason[0] ? reason : "completion");
        return false;
    }

    bool authorityChanged = previousInstance != 0 &&
        (previousInstance != response->state.serviceInstanceId ||
         previousGeneration != response->state.gpuGeneration);
    bool leftReady = previousPhase == GUI_SERVICE_READY &&
        g_app.guiServiceModel.phase != GUI_SERVICE_READY;
    if (authorityChanged || leftReady) {
        gui_mutation_advance_gpu_epoch(authorityChanged
            ? "service/GPU generation changed" : "GPU left READY state");
    }

    g_app.backgroundServiceAvailable = true;
    g_app.backgroundServiceInstalled = true;
    g_app.backgroundServiceRunning = true;
    g_app.backgroundServiceBroken = false;
    g_app.backgroundServiceError[0] = '\0';
    debug_log_on_change("GUI service state: phase=%s epoch=%llu instance=%llu revision=%llu gpuGeneration=%llu valid=0x%02X\n",
        gui_service_phase_name(g_app.guiServiceModel.phase),
        (unsigned long long)connectionEpoch,
        (unsigned long long)response->state.serviceInstanceId,
        (unsigned long long)response->state.stateRevision,
        (unsigned long long)response->state.gpuGeneration,
        response->state.validSections);

    if (!gui_service_model_ready(&g_app.guiServiceModel)) {
        gui_invalidate_live_authority(gui_service_phase_name(
            g_app.guiServiceModel.phase));
        gui_render_service_phase_only();
        start_service_reconnect_timer_if_needed();
        return true;
    }

    bool authorityOrTopologyChanged = authorityChanged ||
        previousTopology != response->state.topologySignature;
    gui_apply_ready_envelope(response, kind, redrawControls, reason,
        oldRenderTopology, wasDirty, authorityOrTopologyChanged);
    if (g_app.logonServiceReadinessPending)
        apply_logon_startup_behavior();
    if (g_app.appLaunchEvaluationPending) {
        g_app.appLaunchEvaluationPending = false;
        maybe_load_app_launch_profile_to_gui();
    }
    if (!g_app.logonServiceReadinessPending)
        KillTimer(g_app.hMainWnd, SERVICE_RECONNECT_TIMER_ID);
    return true;
}

static void gui_service_handle_admin_completion(
    const GuiServiceIoCompletion* completion) {
    end_background_service_toggle();
    g_app.backgroundServiceInstalled = completion->serviceInstalled;
    g_app.backgroundServiceRunning = completion->serviceRunning;
    g_app.backgroundServiceAvailable = false;
    g_app.backgroundServiceBroken = completion->serviceInstalled &&
        !completion->serviceRunning;
    update_background_service_controls();
    if (!completion->transportSuccess) {
        StringCchCopyA(g_app.backgroundServiceError,
            ARRAY_COUNT(g_app.backgroundServiceError),
            completion->error[0] ? completion->error :
            "Failed updating the background service");
        set_profile_status_text("Background service change failed: %s",
            g_app.backgroundServiceError);
        MessageBoxA(g_app.hMainWnd, g_app.backgroundServiceError,
            "Green Curve", MB_OK | MB_ICONERROR);
    } else if (completion->adminEnable) {
        g_app.backgroundServiceError[0] = '\0';
        set_profile_status_text(completion->adminRepair
            ? "Background service repaired. Synchronizing live GPU state..."
            : "Background service installed. Synchronizing live GPU state...");
    } else {
        gui_service_model_disconnect(&g_app.guiServiceModel,
            completion->connectionEpoch);
        gui_invalidate_live_authority("background service removed");
        set_profile_status_text(
            "Background service removed. Live GPU control is unavailable until it is installed again.");
        gui_render_service_phase_only();
    }
    schedule_logon_combo_sync();
    debug_log("GUI service I/O: admin completion enable=%d repair=%d success=%d installed=%d running=%d epoch=%llu error=%s\n",
        completion->adminEnable ? 1 : 0,
        completion->adminRepair ? 1 : 0,
        completion->transportSuccess ? 1 : 0,
        completion->serviceInstalled ? 1 : 0,
        completion->serviceRunning ? 1 : 0,
        (unsigned long long)completion->connectionEpoch,
        completion->error[0] ? completion->error : "none");
}

static void handle_gui_service_io_completion(
    GuiServiceIoCompletion* completion) {
    if (!completion) return;
    if (completion->kind == GUI_SERVICE_IO_ADMIN_TOGGLE) {
        gui_service_handle_admin_completion(completion);
        HeapFree(GetProcessHeap(), 0, completion);
        return;
    }
    if (completion->presentationEpoch !=
            gui_service_presentation_epoch()) {
        debug_log("GUI service state: dropped read completion from presentation epoch %ld (current=%ld kind=%d reason=%s)\n",
            completion->presentationEpoch,
            gui_service_presentation_epoch(), (int)completion->kind,
            completion->reason);
        HeapFree(GetProcessHeap(), 0, completion);
        return;
    }
    if (!completion->transportSuccess) {
        gui_service_handle_transport_failure(completion->connectionEpoch,
            completion->error, completion->reason,
            completion->serviceInstalled, completion->serviceRunning);
    } else {
        gui_service_accept_response_on_main_thread(&completion->response,
            completion->connectionEpoch, completion->kind,
            completion->redrawControls, completion->reason);
    }
    HeapFree(GetProcessHeap(), 0, completion);
}
