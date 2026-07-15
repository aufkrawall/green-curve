// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT

// ============================================================================
// GUI startup profile and independent per-user tray startup observer
// ============================================================================

#include "profile_startup_policy.h"

#ifndef GREEN_CURVE_SERVICE_BINARY

static void show_live_gpu_state_for_disabled_app_launch() {
    int selectedSlot = get_config_int(
        g_app.configPath, "profiles", "selected_slot", CONFIG_DEFAULT_SLOT);
    bool selectedSlotValid = selectedSlot >= 1 && selectedSlot <= CONFIG_NUM_SLOTS;
    bool selectedSlotSaved = selectedSlotValid
        && is_profile_slot_saved(g_app.configPath, selectedSlot);

    // Startup is a new editor session. Discard any incidental control-creation
    // notifications before asking the service for the authoritative live state.
    set_gui_state_dirty(false);
    g_app.guiHasUserModifiedValues = false;
    g_app.loadedSharedSlot = 0;

    ControlState control = {};
    bool haveControl = get_effective_control_state(&control);
    debug_log("startup live editor: snapshot=%d selectedSlot=%d saved=%d (saved slot deliberately not loaded) loaded=%d activeSource=%d activeSlot=%d gpu=%d mem=%d power=%d fanMode=%d lockCi=%d lockMHz=%u detail=%s\n",
        gui_service_model_ready(&g_app.guiServiceModel) ? 1 : 0,
        selectedSlot,
        selectedSlotSaved ? 1 : 0,
        g_app.loaded ? 1 : 0,
        (int)g_app.serviceActiveProfileSource,
        g_app.serviceActiveProfileSlot,
        haveControl && control.hasGpuOffset ? control.gpuOffsetMHz : 0,
        haveControl && control.hasMemOffset ? control.memOffsetMHz : 0,
        haveControl && control.hasPowerLimit ? control.powerLimitPct : 0,
        haveControl && control.hasFan ? control.fanMode : -1,
        g_app.lockedCi,
        g_app.lockedFreq,
        "accepted READY envelope");

    if (selectedSlotSaved) {
        set_profile_status_text(
            "Showing current GPU state. Slot %d is selected and saved; click Load to edit its saved values.",
            selectedSlot);
    } else {
        set_profile_status_text("Showing current GPU state. App start auto-load is disabled.");
    }
}

static void maybe_load_app_launch_profile_to_gui() {
    int appLaunchSlot = get_config_int(g_app.configPath, "profiles", "app_launch_slot", 0);
    StartupEditorSource editorSource = startup_editor_source(
        g_app.launchedFromLogon, appLaunchSlot, CONFIG_NUM_SLOTS);
    if (editorSource == STARTUP_EDITOR_SOURCE_LOGON_SERVICE) {
        set_profile_status_text("Ready. Skipped app-start auto-load for the logon launch.");
        return;
    }
    if (editorSource == STARTUP_EDITOR_SOURCE_LIVE_SNAPSHOT) {
        if (!gui_service_model_ready(&g_app.guiServiceModel)) {
            g_app.appLaunchEvaluationPending = true;
            set_profile_status_text("Waiting for a coherent GPU snapshot before showing live state...");
            return;
        }
        show_live_gpu_state_for_disabled_app_launch();
        return;
    }
    char gpuSelectionErr[256] = {};
    if (!validate_configured_gpu_selection_for_client(
            gpuSelectionErr, sizeof(gpuSelectionErr))) {
        debug_log("app-start profile: blocked because the durable GPU identity is unresolved: %s\n",
            gpuSelectionErr[0] ? gpuSelectionErr : "unknown error");
        set_profile_status_text(
            "App-start apply was blocked because the previously selected GPU is missing or ambiguous. Select the intended GPU again.");
        return;
    }
    if (!is_profile_slot_saved(g_app.configPath, appLaunchSlot)) {
        bool disabled = set_config_int(g_app.configPath, "profiles", "app_launch_slot", 0);
        set_profile_status_text(disabled
            ? "App start slot %d was empty and has been disabled."
            : "App start slot %d is empty, but disabling its persisted assignment failed.",
            appLaunchSlot);
        refresh_profile_controls_from_config();
        layout_bottom_buttons(g_app.hMainWnd);
        return;
    }
    DesiredSettings desired = {};
    char err[256] = {};
    if (!load_profile_from_config(g_app.configPath, appLaunchSlot, &desired, err, sizeof(err))) {
        if (!set_config_int(g_app.configPath, "profiles", "app_launch_slot", 0)) {
            debug_log("app-start profile: failed to persist disabling invalid slot %d\n",
                appLaunchSlot);
        }
        refresh_profile_controls_from_config();
        write_error_report_log_for_user_failure("App-start profile load failed", err);
        set_profile_status_text("App start load failed: %s", err[0] ? err : "unknown error");
        return;
    }
    if (!desired_settings_have_explicit_state(&desired, true, err, sizeof(err))) {
        if (!set_config_int(g_app.configPath, "profiles", "app_launch_slot", 0)) {
            debug_log("app-start profile: failed to persist disabling rejected slot %d\n",
                appLaunchSlot);
        }
        refresh_profile_controls_from_config();
        write_error_report_log_for_user_failure("App-start profile rejected", err);
        set_profile_status_text("App start slot %d was rejected: %s", appLaunchSlot, err);
        return;
    }
    if (!gui_service_model_ready(&g_app.guiServiceModel)) {
        g_app.appLaunchEvaluationPending = true;
        set_profile_status_text("App start slot %d is waiting for GPU reconnection.",
            appLaunchSlot);
        return;
    }
    if (g_app.serviceActiveDesiredValid) {
        char matchDetail[256] = {};
        if (desired_settings_match_active_service_intent(&desired,
                &g_app.serviceActiveDesired, matchDetail,
                sizeof(matchDetail))) {
            debug_log("maybe_load_app_launch_profile_to_gui: slot %d already active in background service; skipping reset-before-apply (%s)\n",
                appLaunchSlot,
                matchDetail[0] ? matchDetail : "match");
            populate_desired_into_gui(&desired);
            if (!set_config_int(g_app.configPath, "profiles", "selected_slot", appLaunchSlot)) {
                debug_log("app-start profile: matching active slot %d loaded but selected_slot persistence failed\n",
                    appLaunchSlot);
            }
            sync_applied_profile_from_service_metadata();
            refresh_profile_controls_from_config();
            set_profile_status_text("Loaded slot %d into the GUI. Background service already has matching active settings, so app-start apply was skipped.", appLaunchSlot);
            return;
        }
        debug_log("maybe_load_app_launch_profile_to_gui: slot %d needs reset-before-apply; accepted active intent check=%s\n",
            appLaunchSlot,
            matchDetail[0] ? matchDetail : "mismatch");
    } else {
        debug_log("maybe_load_app_launch_profile_to_gui: slot %d cannot use active-service skip (usingService=%d available=%d)\n",
            appLaunchSlot,
            g_app.usingBackgroundService ? 1 : 0,
            g_app.backgroundServiceAvailable ? 1 : 0);
    }
    debug_log("maybe_load_app_launch_profile_to_gui: applying slot %d with reset-before-apply\n", appLaunchSlot);
    desired.resetOcBeforeApply = true;
    char queueStatus[512] = {};
    if (!gui_mutation_queue_apply(&desired, false,
            SERVICE_APPLY_ORIGIN_APP_LAUNCH,
            SERVICE_PROFILE_SOURCE_USER_SLOT, appLaunchSlot,
            GUI_MUTATION_CONTEXT_APP_LAUNCH, "app-launch apply",
            queueStatus, sizeof(queueStatus))) {
        set_profile_status_text("App start apply for slot %d could not be queued: %s",
            appLaunchSlot, queueStatus[0] ? queueStatus : "unknown error");
        return;
    }
    set_profile_status_text("App start slot %d is applying in the background.",
        appLaunchSlot);
}

static void app_launch_on_mutation_completed(int slot, bool success,
    const char* result) {
    if (success) {
        if (!set_config_int(g_app.configPath, "profiles", "selected_slot", slot)) {
            debug_log("app-start profile: applied slot %d but selected_slot persistence failed\n",
                slot);
        }
        sync_applied_profile_from_service_metadata();
        refresh_profile_controls_from_config();
        set_profile_status_text(
            "Loaded slot %d into the GUI and applied it through the background service on app start.",
            slot);
        invalidate_main_window();
        return;
    }
    gui_service_begin_full_sync("app-start mutation failed");
    set_profile_status_text(
        "App start apply for slot %d failed; coherent live state is being resynchronized: %s",
        slot, result && result[0] ? result : "unknown error");
}

// The separate --tray-start GUI may begin before the auto-started service has
// created its pipe and initialized the GPU.  The service's session coordinator
// is the sole automatic hardware writer; this state simply keeps the GUI
// reconnect timer alive until it can show a real service snapshot.
static const ULONGLONG LOGON_SERVICE_READINESS_MAX_DEFER_MS = 120000ULL;

static void clear_pending_logon_service_readiness(const char* outcome) {
    if (!g_app.logonServiceReadinessPending) return;
    debug_log("logon tray client: readiness wait finished (%s), deferredAttempts=%u\n",
        outcome && outcome[0] ? outcome : "unspecified",
        g_app.logonServiceReadinessDeferredAttempts);
    g_app.logonServiceReadinessPending = false;
    g_app.logonServiceReadinessDeadlineTickMs = 0;
    g_app.logonServiceReadinessDeferredAttempts = 0;
}

static bool defer_logon_service_readiness(const char* reason) {
    const ULONGLONG now = GetTickCount64();
    if (!g_app.logonServiceReadinessPending) {
        g_app.logonServiceReadinessPending = true;
        g_app.logonServiceReadinessDeadlineTickMs = now + LOGON_SERVICE_READINESS_MAX_DEFER_MS;
        g_app.logonServiceReadinessDeferredAttempts = 0;
        debug_log("logon tray client: waiting for service snapshot (deadline=%llu ms, reason=%s)\n",
            g_app.logonServiceReadinessDeadlineTickMs,
            reason && reason[0] ? reason : "unknown");
    }

    if (g_app.logonServiceReadinessDeadlineTickMs != 0 && now >= g_app.logonServiceReadinessDeadlineTickMs) {
        clear_pending_logon_service_readiness("snapshot was not ready within two minutes");
        set_profile_status_text("The background service has not produced a ready GPU snapshot yet. Automatic logon profile application remains service-owned.");
        start_service_reconnect_timer_if_needed();
        return false;
    }

    g_app.logonServiceReadinessDeferredAttempts++;
    debug_log_on_change("logon tray client: waiting for service snapshot (installed=%d running=%d available=%d, reason=%s)\n",
        g_app.backgroundServiceInstalled ? 1 : 0,
        g_app.backgroundServiceRunning ? 1 : 0,
        g_app.backgroundServiceAvailable ? 1 : 0,
        reason && reason[0] ? reason : "unknown");
    set_profile_status_text("Waiting for the background service and GPU driver before completing tray startup...");
    start_service_reconnect_timer_if_needed();
    return true;
}

// Probe the actual service snapshot, not just SCM's RUNNING state.  A running
// service has not necessarily created its pipe or initialized NVAPI/NVML yet.
static bool logon_service_snapshot_ready(char* reason, size_t reasonSize) {
    if (reason && reasonSize) reason[0] = 0;
    if (gui_service_model_ready(&g_app.guiServiceModel)) return true;
    set_message(reason, reasonSize,
        "service has not accepted a READY snapshot (phase %d)",
        (int)g_app.guiServiceModel.phase);
    return false;
}

static void apply_logon_startup_behavior() {
    if (!g_app.launchedFromLogon) {
        clear_pending_logon_service_readiness("launch is no longer a logon launch");
        return;
    }

    const bool startProgramAtLogon = is_start_on_logon_enabled(g_app.configPath);
    g_app.startHiddenToTray = startProgramAtLogon;

    char readiness[256] = {};
    if (!logon_service_snapshot_ready(readiness, sizeof(readiness))) {
        defer_logon_service_readiness(readiness[0] ? readiness : "service or GPU not ready");
        return;
    }

    if (g_app.logonServiceReadinessPending) {
        debug_log("logon tray client: service snapshot became ready after %u deferred checks\n",
            g_app.logonServiceReadinessDeferredAttempts);
        clear_pending_logon_service_readiness("service snapshot ready");
    }
    debug_log("logon tray client: initialized service snapshot ready; automatic logon profile application is service-owned\n");
    set_profile_status_text(startProgramAtLogon
        ? "Started in the tray at Windows logon. The background service applies the configured logon profile."
        : "The background service applies the configured logon profile.");
}

// Called by the GUI reconnect timer.  It retries only UI/service-snapshot
// readiness; this code path intentionally contains no profile load, reset, or
// hardware apply.
static void retry_pending_logon_service_readiness() {
    if (!g_app.logonServiceReadinessPending) return;
    debug_log("logon tray client: reconnect timer retrying service snapshot check (attempt=%u)\n",
        g_app.logonServiceReadinessDeferredAttempts + 1);
    apply_logon_startup_behavior();
}

#endif
