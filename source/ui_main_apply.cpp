// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// The three main-window mutation commands (Apply / Refresh / Reset) and the
// high-overclock confirmation that guards a manual Apply.  Split out of
// ui_main_window.cpp, which had grown far past the file-size guideline.
//
// Everything here is on the MANUAL command path only.  Automation
// (auto-profile rules, hotkeys, tray picks, logon and app-launch applies)
// queues its mutations directly in auto_profile_win32.cpp and
// main_startup_profiles.cpp and must stay presentation-silent, so it never
// reaches this file and can never raise a dialog.

#include "oc_high_warning_policy.h"
#include "ui_mutation_completion.cpp"

// Read the configured warning thresholds.  0 (or any negative value) disables
// that domain, which is the escape hatch for users who deliberately run high
// clocks and do not want to be asked.
static OcHighWarnThresholds gui_high_oc_warn_thresholds() {
    OcHighWarnThresholds thresholds = oc_high_warn_default_thresholds();
    thresholds.gpuOffsetMHz = get_config_int(g_app.configPath, "ui",
        "high_oc_warn_gpu_offset_mhz", thresholds.gpuOffsetMHz);
    thresholds.memOffsetMHz = get_config_int(g_app.configPath, "ui",
        "high_oc_warn_mem_offset_mhz", thresholds.memOffsetMHz);
    return thresholds;
}

// Ask before a manual Apply raises a hand-typed clock past the danger line.
// Returns false only when the user explicitly declines.
static bool confirm_high_oc_apply(const DesiredSettings* desired,
                                  const OcApplyBaseline* baseline) {
    if (!desired || !baseline) return true;
    OcHighWarnInputs inputs = {};
    inputs.hasGpuOffset = desired->hasGpuOffset != 0;
    inputs.gpuHandTyped = !g_app.guiGpuOffsetFromProfileLoad;
    inputs.gpuOffsetMHz = desired->gpuOffsetMHz;
    inputs.currentGpuOffsetMHz = baseline->currentGpuOffsetMHz;
    inputs.hasMemOffset = desired->hasMemOffset != 0;
    inputs.memHandTyped = !g_app.guiMemOffsetFromProfileLoad;
    inputs.memOffsetMHz = desired->memOffsetMHz;
    inputs.currentMemOffsetMHz = baseline->currentMemOffsetMHz;

    OcHighWarnThresholds thresholds = gui_high_oc_warn_thresholds();
    OcHighWarnDecision decision = oc_high_warn_decide(&inputs, &thresholds);
    debug_log("high-OC gate: gpu=(has=%d typed=%d value=%d current=%d threshold=%d trigger=%d) "
              "mem=(has=%d typed=%d value=%d current=%d threshold=%d trigger=%d) warn=%d\n",
        inputs.hasGpuOffset ? 1 : 0, inputs.gpuHandTyped ? 1 : 0,
        inputs.gpuOffsetMHz, inputs.currentGpuOffsetMHz,
        thresholds.gpuOffsetMHz, decision.gpu ? 1 : 0,
        inputs.hasMemOffset ? 1 : 0, inputs.memHandTyped ? 1 : 0,
        inputs.memOffsetMHz, inputs.currentMemOffsetMHz,
        thresholds.memOffsetMHz, decision.mem ? 1 : 0,
        decision.warn ? 1 : 0);
    if (!decision.warn) return true;

    char message[512] = {};
    oc_high_warn_format_message(message, sizeof(message), &decision, &inputs,
                                &thresholds);
    int confirm = gc_message_box(g_app.hMainWnd, message, "Confirm High Overclock",
                              MB_YESNO | MB_ICONWARNING);
    if (confirm == IDYES) {
        record_ui_action("High-OC confirmation accepted (gpu=%d mem=%d)",
            decision.gpu ? inputs.gpuOffsetMHz : 0,
            decision.mem ? inputs.memOffsetMHz : 0);
        return true;
    }
    record_ui_action("High-OC confirmation declined (gpu=%d mem=%d)",
        decision.gpu ? inputs.gpuOffsetMHz : 0,
        decision.mem ? inputs.memOffsetMHz : 0);
    set_profile_status_text("Apply cancelled: high overclock not confirmed.");
    return false;
}

// Ask before a memory-clock write on a unified pool.  This is a different risk
// class from the high-OC gate above: on a discrete board an out-of-range memory
// offset is rejected or clamped by the driver and the blast radius is the GPU,
// but on an integrated part the "video memory" IS the system RAM the CPU is
// executing from.
//
// Gated on positively-reported UNIFIED topology only.  UNKNOWN (a failed DXGI
// query, session-0 service, or any unmatched adapter) and DEDICATED both return
// true immediately, so no discrete GPU and no existing x64 install can ever
// reach this dialog.  Returns false only when the user explicitly declines.
static bool confirm_unified_memory_write(const DesiredSettings* desired) {
    if (!desired || !desired->hasMemOffset) return true;
    if (desired->memOffsetMHz == 0) return true;
    if (!gpu_capability_memory_write_is_risky(&g_app.gpuCapability)) return true;

    debug_log("unified-memory gate: memOffset=%d MHz topology=%s -> confirming\n",
              desired->memOffsetMHz,
              gpu_memory_topology_name(g_app.gpuCapability.memoryTopology));

    char message[640] = {};
    StringCchPrintfA(message, ARRAY_COUNT(message),
        "This GPU shares one memory pool with the CPU (unified memory).\n\n"
        "You are about to apply a memory clock offset of %+d MHz. On a discrete "
        "graphics card that would only affect the card's own VRAM. Here it targets "
        "the same physical RAM the operating system and every running program are "
        "using, so an unstable value can corrupt data or hang the whole system, not "
        "just the GPU.\n\n"
        "Green Curve will still attempt the write if you continue.\n\n"
        "Apply the memory offset anyway?",
        desired->memOffsetMHz);

    int confirm = gc_message_box(g_app.hMainWnd, message,
                                 "Confirm Unified-Memory Clock Change",
                                 MB_YESNO | MB_ICONWARNING);
    if (confirm == IDYES) {
        record_ui_action("Unified-memory confirmation accepted (mem=%d)",
                         desired->memOffsetMHz);
        return true;
    }
    record_ui_action("Unified-memory confirmation declined (mem=%d)",
                     desired->memOffsetMHz);
    set_profile_status_text("Apply cancelled: unified-memory clock change not confirmed.");
    return false;
}

static void apply_changes() {
    if (!gui_service_model_ready(&g_app.guiServiceModel) ||
        !g_app.guiDraft.attached || g_app.guiDraft.detached) return;
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
    DesiredSettings desired = {};
    OcApplyBaseline baseline = {};
    char err[256] = {};
    if (!capture_gui_apply_settings(&desired, &baseline, err, sizeof(err))) {
        write_error_report_log_for_user_failure("GUI apply validation failed", err);
        gc_message_box(g_app.hMainWnd, err, "Green Curve", MB_OK | MB_ICONERROR);
        return;
    }
    // Gate AFTER capture: the captured request is the exact intent that would
    // reach the hardware, and capture has already established that something
    // actually changed, so an unchanged high clock never reaches the dialog.
    if (!confirm_high_oc_apply(&desired, &baseline)) return;
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
    char queueStatus[512] = {};
    if (!gui_mutation_queue_apply(&desired, true, SERVICE_APPLY_ORIGIN_GUI,
            requestedSource, requestedSlot, GUI_MUTATION_CONTEXT_MANUAL_APPLY,
            "GUI apply", queueStatus, sizeof(queueStatus))) {
        gc_message_box(g_app.hMainWnd, queueStatus, "Green Curve",
            MB_OK | MB_ICONWARNING);
    } else {
        set_profile_status_text("%s", queueStatus);
    }
}

#include "ui_main_control_lifecycle.cpp"

static void refresh_curve() {
    if (g_app.usingBackgroundService && !g_app.isServiceProcess) {
        if (g_app.guiDraft.detached && gui_state_dirty()) {
            int discard = gc_message_box(g_app.hMainWnd,
                "The preserved draft belongs to a different GPU or VF topology.\n\nDiscard that draft and refresh this GPU?",
                "Discard Detached Draft", MB_YESNO | MB_ICONWARNING);
            if (discard != IDYES) return;
            gui_draft_discard();
        }
        gui_service_begin_full_sync("manual refresh");
        return;
    }
}

static void reset_curve() {
    if (!gui_service_model_ready(&g_app.guiServiceModel) ||
        !g_app.guiDraft.attached || g_app.guiDraft.detached) return;
    char gpuSelectionErr[256] = {};
    if (!validate_configured_gpu_selection_for_client(
            gpuSelectionErr, sizeof(gpuSelectionErr))) {
        gc_message_box(g_app.hMainWnd,
            gpuSelectionErr[0] ? gpuSelectionErr :
                "Select the intended GPU again before resetting settings.",
            "Green Curve", MB_OK | MB_ICONWARNING);
        return;
    }

    int confirm = gc_message_box(g_app.hMainWnd,
        "Reset will restore all GPU settings to their default values.\n\nContinue?",
        "Confirm Reset",
        MB_YESNO | MB_ICONWARNING);
    if (confirm != IDYES) return;

    if (g_app.usingBackgroundService && !g_app.isServiceProcess) {
        char queueStatus[512] = {};
        if (!gui_mutation_queue_reset(queueStatus, sizeof(queueStatus))) {
            gc_message_box(g_app.hMainWnd, queueStatus, "Green Curve",
                MB_OK | MB_ICONWARNING);
        } else {
            set_profile_status_text("%s", queueStatus);
        }
        return;
    }

}
