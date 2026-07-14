// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT

static void accept_gui_mutation_response_on_main_thread(
    const GuiMutationCompletion* completion) {
    if (!completion) return;
    const GuiMutationWork& work = completion->work;
    const ServiceResponse& response = completion->response;
    g_app.serviceActiveDesired = response.desired;
    g_app.serviceActiveDesiredValid =
        response.snapshot.activeProfileSource != SERVICE_PROFILE_SOURCE_NONE;
    if (response.controlState.valid)
        apply_control_state_to_gui(&response.controlState);

    if (!(response.snapshot.initialized || response.snapshot.loaded ||
          response.snapshot.fanSupported || g_app.fanSupported)) return;
    if (work.kind == GUI_MUTATION_RESET && completion->success)
        clear_service_authoritative_state();
    apply_service_snapshot_to_app(&response.snapshot);

    if (work.kind == GUI_MUTATION_APPLY) {
        bool fanOnlyApply = desired_is_fan_only_apply_request(&work.desired);
        if (completion->success && !fanOnlyApply)
            capture_applied_curve_baseline(&work.desired);
        if (work.desired.hasLock && work.desired.lockCi >= 0 &&
            work.desired.lockCi < VF_NUM_POINTS && work.desired.lockMHz > 0) {
            g_app.lockedCi = work.desired.lockCi;
            g_app.lockedFreq = work.desired.lockMHz;
            g_app.lockMode = work.desired.lockMode;
            g_app.guiLockTracksAnchor = work.desired.lockTracksAnchor;
            g_app.lockedVi = -1;
            for (int vi = 0; vi < g_app.numVisible; vi++) {
                if (g_app.visibleMap[vi] == work.desired.lockCi) {
                    g_app.lockedVi = vi;
                    break;
                }
            }
            g_app.appliedLockVi = g_app.lockedVi;
            g_app.appliedLockCi = g_app.lockedCi;
            g_app.appliedLockFreq = g_app.lockedFreq;
            g_app.appliedLockMode = g_app.lockMode;
        }
    } else if (completion->success) {
        g_app.guiFanMode = FAN_MODE_AUTO;
        g_app.guiFanFixedPercent = 0;
        fan_curve_set_default(&g_app.guiFanCurve);
        g_app.lockedVi = -1;
        g_app.lockedCi = -1;
        g_app.lockedFreq = 0;
        g_app.lockMode = LOCK_MODE_NONE;
        memset(g_app.appliedCurveMHz, 0, sizeof(g_app.appliedCurveMHz));
        set_gui_state_dirty(false);
    }
}

static void handle_gui_mutation_completion(GuiMutationCompletion* completion) {
    if (!completion) return;
    if (completion->response.magic == SERVICE_PROTOCOL_MAGIC &&
        completion->response.version == SERVICE_PROTOCOL_VERSION) {
        bool recoveredHealth = !g_app.backgroundServiceAvailable ||
            g_app.backgroundServiceBroken;
        g_app.backgroundServiceAvailable = true;
        g_app.backgroundServiceBroken = false;
        g_app.backgroundServiceRunning = true;
        g_app.backgroundServiceError[0] = '\0';
        if (recoveredHealth) {
            debug_log("GUI mutation: valid operation response restored service availability without a redundant ping\n");
        }
    }
    accept_gui_mutation_response_on_main_thread(completion);
    const GuiMutationWork& work = completion->work;

    if (work.context == GUI_MUTATION_CONTEXT_MANUAL_APPLY) {
        if (completion->success) {
            bool fanOnlyApply = desired_is_fan_only_apply_request(&work.desired);
            set_gui_state_dirty(false);
            // Keep preserving VF editor intent after fan-only apply; the worker
            // response must not replace sparse curve edits with live telemetry.
            if (!fanOnlyApply) {
                populate_desired_into_gui(&work.desired);
                if (work.profileSource == SERVICE_PROFILE_SOURCE_SHARED_SLOT)
                    g_app.loadedSharedSlot = work.profileSlot;
            }
            sync_applied_profile_from_service_metadata();
        }
        populate_global_controls();
        invalidate_main_window();
        boost_fan_telemetry_for_ms(3000);
        refresh_live_fan_telemetry(true);
        MessageBoxA(g_app.hMainWnd, completion->result, "Green Curve",
            MB_OK | (completion->success ? MB_ICONINFORMATION : MB_ICONWARNING));
    } else if (work.context == GUI_MUTATION_CONTEXT_MANUAL_RESET) {
        if (completion->response.snapshot.initialized ||
            completion->response.snapshot.loaded || g_app.fanSupported) {
            rebuild_edit_controls();
            populate_global_controls();
            update_background_service_controls();
            invalidate_main_window();
        }
        boost_fan_telemetry_for_ms(3000);
        refresh_live_fan_telemetry(true);
        MessageBoxA(g_app.hMainWnd, completion->result, "Green Curve",
            MB_OK | (completion->success ? MB_ICONINFORMATION : MB_ICONWARNING));
    } else if (work.context == GUI_MUTATION_CONTEXT_AUTO_PROFILE) {
        if (completion->success)
            populate_desired_into_gui(&work.desired);
        auto_profile_on_mutation_completed(work.profileSlot, work.origin,
            completion->success, completion->result);
    } else if (work.context == GUI_MUTATION_CONTEXT_APP_LAUNCH) {
        if (completion->success)
            populate_desired_into_gui(&work.desired);
        app_launch_on_mutation_completed(work.profileSlot,
            completion->success, completion->result);
    }
    HeapFree(GetProcessHeap(), 0, completion);
    gui_mutation_acknowledge_and_dispatch_next();
}

static void gui_mutation_pending_was_superseded(GuiMutationUiContext context,
    int profileSlot, ServiceApplyOrigin origin, gc_u64 operationId) {
    (void)origin;
    debug_log("GUI mutation: pending operation=%llu context=%d slot=%d superseded before dispatch\n",
        (unsigned long long)operationId, (int)context, profileSlot);
    if (context == GUI_MUTATION_CONTEXT_AUTO_PROFILE) {
        auto_profile_on_mutation_superseded(profileSlot);
    } else if (context == GUI_MUTATION_CONTEXT_APP_LAUNCH) {
        set_profile_status_text(
            "App-start profile %d was superseded by a newer GPU operation.",
            profileSlot);
    }
}
