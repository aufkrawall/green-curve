// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT

// Read-only GUI notification for the exact selected display DEVINST.  The
// service remains the sole hardware owner; this callback only posts prompt
// presentation invalidation to the window thread.

enum GuiSelectedGpuPnpEvent {
    GUI_SELECTED_GPU_REMOVED = 1,
    GUI_SELECTED_GPU_STARTED = 2,
};

struct GuiSelectedGpuNotificationContext {
    volatile LONG active;
    HWND notifyWindow;
    WCHAR instanceId[MAX_DEVICE_ID_LEN];
};

static HCMNOTIFICATION g_guiSelectedGpuNotification = nullptr;
static GuiSelectedGpuNotificationContext
    g_guiSelectedGpuNotificationContext = {};
static GpuAdapterInfo g_guiSelectedGpuNotificationTarget = {};

static DWORD CALLBACK gui_selected_gpu_notification_callback(
    HCMNOTIFICATION, PVOID context, CM_NOTIFY_ACTION action,
    PCM_NOTIFY_EVENT_DATA eventData, DWORD eventDataSize) {
    GuiSelectedGpuNotificationContext* state =
        reinterpret_cast<GuiSelectedGpuNotificationContext*>(context);
    if (!state ||
        InterlockedCompareExchange(&state->active, 0, 0) == 0)
        return ERROR_SUCCESS;
    const size_t instanceIdOffset =
        offsetof(CM_NOTIFY_EVENT_DATA, u.DeviceInstance.InstanceId);
    if (!eventData ||
        eventData->FilterType != CM_NOTIFY_FILTER_TYPE_DEVICEINSTANCE ||
        eventDataSize < instanceIdOffset + sizeof(WCHAR))
        return ERROR_SUCCESS;
    size_t instanceIdChars =
        (eventDataSize - instanceIdOffset) / sizeof(WCHAR);
    bool terminated = false;
    for (size_t i = 0; i < instanceIdChars; ++i) {
        if (!eventData->u.DeviceInstance.InstanceId[i]) {
            terminated = true;
            break;
        }
    }
    if (!terminated || _wcsicmp(
            eventData->u.DeviceInstance.InstanceId,
            state->instanceId) != 0)
        return ERROR_SUCCESS;

    WPARAM postedEvent = 0;
    if (action == CM_NOTIFY_ACTION_DEVICEINSTANCEREMOVED)
        postedEvent = GUI_SELECTED_GPU_REMOVED;
    else if (action == CM_NOTIFY_ACTION_DEVICEINSTANCESTARTED)
        postedEvent = GUI_SELECTED_GPU_STARTED;
    if (postedEvent && state->notifyWindow)
        PostMessageW(state->notifyWindow, APP_WM_SELECTED_GPU_PNP,
            postedEvent, 0);
    return ERROR_SUCCESS;
}

static void gui_selected_gpu_notification_unregister() {
    InterlockedExchange(&g_guiSelectedGpuNotificationContext.active, 0);
    if (g_guiSelectedGpuNotification) {
        CONFIGRET result =
            CM_Unregister_Notification(g_guiSelectedGpuNotification);
        if (result != CR_SUCCESS)
            debug_log("GUI selected_gpu_pnp: unregister failed CM=0x%08lX; callback inactive\n",
                (unsigned long)result);
        g_guiSelectedGpuNotification = nullptr;
    }
    memset(&g_guiSelectedGpuNotificationTarget, 0,
        sizeof(g_guiSelectedGpuNotificationTarget));
}

static void gui_selected_gpu_notification_refresh(
    const GpuAdapterInfo* target) {
    if (!target || !target->valid || !g_app.hMainWnd) return;
    if (g_guiSelectedGpuNotification &&
        gui_gpu_identity_equal(
            &g_guiSelectedGpuNotificationTarget, target))
        return;

    gui_selected_gpu_notification_unregister();
    ServiceSelectedGpuDevinstMatch selected = {};
    char err[256] = {};
    if (!service_find_selected_gpu_devinst(
            target, &selected, err, sizeof(err))) {
        debug_log("GUI selected_gpu_pnp: exact notification unavailable: %s\n",
            err[0] ? err : "unknown mapping error");
        return;
    }

    CM_NOTIFY_FILTER filter = {};
    filter.cbSize = sizeof(filter);
    filter.FilterType = CM_NOTIFY_FILTER_TYPE_DEVICEINSTANCE;
    StringCchCopyW(filter.u.DeviceInstance.InstanceId,
        ARRAY_COUNT(filter.u.DeviceInstance.InstanceId),
        selected.instanceId);
    g_guiSelectedGpuNotificationContext.notifyWindow = g_app.hMainWnd;
    StringCchCopyW(g_guiSelectedGpuNotificationContext.instanceId,
        ARRAY_COUNT(g_guiSelectedGpuNotificationContext.instanceId),
        selected.instanceId);
    InterlockedExchange(&g_guiSelectedGpuNotificationContext.active, 1);
    CONFIGRET result = CM_Register_Notification(&filter,
        &g_guiSelectedGpuNotificationContext,
        gui_selected_gpu_notification_callback,
        &g_guiSelectedGpuNotification);
    if (result != CR_SUCCESS || !g_guiSelectedGpuNotification) {
        InterlockedExchange(
            &g_guiSelectedGpuNotificationContext.active, 0);
        g_guiSelectedGpuNotification = nullptr;
        debug_log("GUI selected_gpu_pnp: register failed CM=0x%08lX\n",
            (unsigned long)result);
        return;
    }
    g_guiSelectedGpuNotificationTarget = *target;
    debug_log("GUI selected_gpu_pnp: registered exact selected adapter notification\n");
}

static void gui_handle_selected_gpu_pnp_event(
    GuiSelectedGpuPnpEvent event) {
    GuiServicePhase phaseBeforeEvent = g_app.guiServiceModel.phase;
    gui_service_advance_presentation_epoch(
        event == GUI_SELECTED_GPU_REMOVED
            ? "selected GPU removed" : "selected GPU started");
    reset_gui_gdi_generation(event == GUI_SELECTED_GPU_REMOVED
        ? "selected GPU removed" : "selected GPU started");
    bool generationFenceArmed =
        gui_service_model_require_new_gpu_generation(
            &g_app.guiServiceModel);
    debug_log("GUI selected_gpu_pnp: event=%s priorPhase=%s generationFence=%s minimumGeneration=%llu instance=%llu\n",
        event == GUI_SELECTED_GPU_REMOVED ? "removed" : "started",
        gui_service_phase_name(phaseBeforeEvent),
        generationFenceArmed ? "armed" : "unchanged",
        (unsigned long long)g_app.guiServiceModel.minimumGpuGeneration,
        (unsigned long long)g_app.guiServiceModel.serviceInstanceId);
    gui_mutation_advance_gpu_epoch(event == GUI_SELECTED_GPU_REMOVED
        ? "GUI exact selected GPU removal"
        : "GUI exact selected GPU start");
    if (event == GUI_SELECTED_GPU_REMOVED) {
        g_app.guiServiceModel.phase = GUI_SERVICE_DEVICE_MISSING;
        g_app.guiServiceModel.validSections = 0;
        gui_invalidate_live_authority("exact selected GPU removal");
        debug_log("GUI selected_gpu_pnp: selected adapter removed; live presentation invalidated\n");
    } else if (event == GUI_SELECTED_GPU_STARTED) {
        g_app.guiServiceModel.phase = GUI_SERVICE_RECOVERING;
        g_app.guiServiceModel.validSections = 0;
        gui_invalidate_live_authority("exact selected GPU arrival");
        debug_log("GUI selected_gpu_pnp: selected adapter started; recovery sync queued\n");
    } else {
        return;
    }
    // Windows can make a hidden top-level window visible while rebuilding the
    // display stack. Reassert the user's tray residency before any recovery
    // render; visibility changes remain reserved for explicit user activation.
    enforce_main_window_tray_state(event == GUI_SELECTED_GPU_REMOVED
        ? "selected GPU removed" : "selected GPU started");
    gui_render_service_phase_only();
    gui_service_io_queue_full_sync(event == GUI_SELECTED_GPU_REMOVED
        ? "selected GPU removal notification"
        : "selected GPU arrival notification");
    start_service_reconnect_timer_if_needed();
}
