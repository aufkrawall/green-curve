// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT

// GUI half of the client/service GPU selection agreement
// (gpu_selection_sync_policy.h). Included by ui_main.cpp after the selector it
// repopulates; gui_service_state.cpp reaches gui_gpu_selection_after_read()
// through gui_mutation_forward.h.
//
// - A selector choice asks the service, through the next full state read, to
//   show that GPU. It is saved to [gpu] only once a READY snapshot shows it;
//   otherwise the selector returns to the shown GPU and says why.
// - After a READY full sync with no choice pending, a saved GPU that differs
//   from the shown one is requested once per GpuSelectionAttemptKey, so a
//   held intent cannot make every sync ask again.

static bool g_guiSelectionChoicePending = false;
static GpuAdapterInfo g_guiSelectionChoiceTarget = {};
static bool g_guiSelectionReconcilePending = false;
static GpuSelectionAttemptKey g_guiSelectionLastRefused = {};

static const GpuAdapterInfo* gui_gpu_selection_shown() {
    const unsigned int shown = g_app.selectedGpuIndex;
    return shown < g_app.adapterCount && shown < MAX_GPU_ADAPTERS &&
            g_app.adapters[shown].valid
        ? &g_app.adapters[shown] : nullptr;
}

// The saved [gpu] selection as a live adapter index, when it resolves stably.
static bool gui_gpu_selection_resolve_saved(unsigned int* indexOut) {
    if (!indexOut || !g_app.configPath[0] || g_app.adapterCount == 0) return false;
    ConfiguredGpuSelection configured = {};
    char err[256] = {};
    if (!load_configured_gpu_selection(g_app.configPath, &configured, err,
            sizeof(err))) return false;
    unsigned int resolved = configured.legacyIndex;
    const ConfiguredGpuResolveResult result = resolve_configured_gpu_selection(
        &configured, g_app.adapters, g_app.adapterCount, &resolved);
    const bool usable = result == CONFIGURED_GPU_RESOLVE_STABLE ||
        (result == CONFIGURED_GPU_RESOLVE_LEGACY_ORDINAL &&
         g_app.adapterCount == 1);
    if (!usable || resolved >= g_app.adapterCount ||
        resolved >= MAX_GPU_ADAPTERS || !g_app.adapters[resolved].valid)
        return false;
    *indexOut = resolved;
    return true;
}

static GpuSelectionAttemptKey gui_gpu_selection_attempt_key(
    unsigned int targetIndex) {
    GpuSelectionAttemptKey key = {};
    key.valid = true;
    key.serviceInstanceId = g_app.guiServiceModel.serviceInstanceId;
    key.gpuGeneration = g_app.guiServiceModel.gpuGeneration;
    key.serviceHoldsIntent = g_app.serviceActiveDesiredValid;
    key.targetIndex = targetIndex;
    return key;
}

// A refused request explains itself in the snapshot message. An older
// service that ignores the target answers with its ordinary status instead.
static const char* gui_gpu_selection_refusal_reason(
    const GuiServiceIoCompletion* completion) {
    const char* message = completion ? completion->response.message : nullptr;
    if (!message || !message[0] || strncmp(message, "snapshot ", 9) == 0)
        return "The background service kept its current GPU.";
    return message;
}

// Whether the shown GPU is about to change to a choice or the saved GPU, so a
// startup action that acts on the saved GPU waits instead of being refused
// one sync before the service shows it.
static bool gui_gpu_selection_switch_expected() {
    if (g_guiSelectionChoicePending || g_guiSelectionReconcilePending)
        return true;
    unsigned int saved = 0;
    const bool savedResolved = gui_gpu_selection_resolve_saved(&saved);
    const GpuSelectionAttemptKey key = gui_gpu_selection_attempt_key(saved);
    return gpu_selection_reconcile_decide(g_app.adapterCount, savedResolved,
        saved, g_app.selectedGpuIndex, &key, &g_guiSelectionLastRefused) ==
        GPU_SELECTION_RECONCILE_REQUEST;
}

static void gui_gpu_selection_choose(unsigned int index) {
    const unsigned int shown = g_app.selectedGpuIndex;
    unsigned int saved = 0;
    const bool savedMatchesShown = !g_app.configuredGpuSelectionUnresolved &&
        gui_gpu_selection_resolve_saved(&saved) && saved == shown;
    const GpuSelectorChoiceVerdict verdict = gpu_selector_choice_verdict(index,
        shown, g_app.adapterCount, savedMatchesShown,
        g_app.serviceActiveDesiredValid);
    debug_log("gpu selection: selector chose nvapi=%u shown=%u adapters=%u savedMatchesShown=%d intentHeld=%d verdict=%u\n",
        index, shown, g_app.adapterCount, savedMatchesShown ? 1 : 0,
        g_app.serviceActiveDesiredValid ? 1 : 0, (unsigned int)verdict);
    if (verdict == GPU_SELECTOR_CHOICE_NO_CHANGE) return;
    if (verdict == GPU_SELECTOR_CHOICE_INVALID) {
        populate_gpu_selector();
        return;
    }
    if (verdict == GPU_SELECTOR_CHOICE_REFUSE_INTENT_HELD) {
        populate_gpu_selector();
        const GpuAdapterInfo* owner = gui_gpu_selection_shown();
        char text[320] = {};
        StringCchPrintfA(text, ARRAY_COUNT(text),
            "GPU %u (%s) holds the active Green Curve settings. Reset it before selecting another GPU.",
            shown, owner && owner->name[0] ? owner->name : "NVIDIA GPU");
        set_profile_status_text("%s", text);
        gc_message_box(g_app.hMainWnd, text, "Green Curve",
            MB_OK | MB_ICONINFORMATION);
        return;
    }
    g_guiSelectionChoicePending = true;
    g_guiSelectionChoiceTarget = g_app.adapters[index];
    g_guiSelectionReconcilePending = false;
    gui_service_io_set_full_sync_target(&g_guiSelectionChoiceTarget);
    gui_mutation_advance_gpu_epoch("GPU selector");
    gui_service_begin_full_sync("GPU selection changed");
    set_profile_status_text("Selecting GPU %u...", index);
}

static void gui_gpu_selection_finish_choice(
    const GuiServiceIoCompletion* completion) {
    g_guiSelectionChoicePending = false;
    gui_service_io_set_full_sync_target(nullptr);
    const GpuAdapterInfo* shown = gui_gpu_selection_shown();
    const unsigned int chosen = g_guiSelectionChoiceTarget.nvapiIndex;
    if (gpu_selection_sync_accepted(&completion->requestedTarget, shown,
            g_app.adapterCount)) {
        char err[256] = {};
        if (!save_configured_gpu_selection_atomic(g_app.selectedGpuIndex,
                err, sizeof(err))) {
            debug_log("gpu selection: service shows nvapi=%u but persisting it failed: %s\n",
                chosen, err[0] ? err : "unknown error");
            gc_message_box(g_app.hMainWnd,
                err[0] ? err : "Failed to save the selected GPU.",
                "Green Curve", MB_OK | MB_ICONERROR);
        } else {
            g_app.selectedGpuExplicit = true;
            g_app.configuredGpuSelectionUnresolved = false;
            g_guiSelectionLastRefused = {};
            set_profile_status_text("Selected GPU %u.", chosen);
            debug_log("gpu selection: service shows chosen nvapi=%u; saved\n",
                chosen);
        }
    } else {
        const char* reason = gui_gpu_selection_refusal_reason(completion);
        debug_log("gpu selection: chosen nvapi=%u refused; service shows nvapi=%u: %s\n",
            chosen, g_app.selectedGpuIndex, reason);
        set_profile_status_text("GPU %u was not selected: %s", chosen, reason);
        char text[640] = {};
        StringCchPrintfA(text, ARRAY_COUNT(text),
            "GPU %u was not selected.\n\n%s", chosen, reason);
        gc_message_box(g_app.hMainWnd, text, "Green Curve",
            MB_OK | MB_ICONINFORMATION);
    }
    populate_gpu_selector();
}

static void gui_gpu_selection_after_read(
    const GuiServiceIoCompletion* completion, bool accepted) {
    if (!completion || completion->kind != GUI_SERVICE_IO_FULL_SYNC ||
        !accepted || !gui_service_model_ready(&g_app.guiServiceModel))
        return;  // a pending request keeps its target for the next full sync
    const GpuAdapterInfo* requested = completion->requestedTarget.valid
        ? &completion->requestedTarget : nullptr;
    if (g_guiSelectionChoicePending) {
        // Only the READY sync that carried this choice answers it.
        if (requested && requested->nvapiIndex ==
                g_guiSelectionChoiceTarget.nvapiIndex &&
            gui_gpu_identity_equal(requested, &g_guiSelectionChoiceTarget))
            gui_gpu_selection_finish_choice(completion);
        return;
    }
    if (g_guiSelectionReconcilePending && requested) {
        g_guiSelectionReconcilePending = false;
        gui_service_io_set_full_sync_target(nullptr);
        if (gpu_selection_sync_accepted(requested, gui_gpu_selection_shown(),
                g_app.adapterCount)) {
            debug_log("gpu selection: service now shows saved nvapi=%u\n",
                requested->nvapiIndex);
            set_profile_status_text("Showing the saved GPU %u.",
                requested->nvapiIndex);
        } else {
            g_guiSelectionLastRefused =
                gui_gpu_selection_attempt_key(requested->nvapiIndex);
            const char* reason = gui_gpu_selection_refusal_reason(completion);
            debug_log("gpu selection: saved nvapi=%u refused; service shows nvapi=%u: %s\n",
                requested->nvapiIndex, g_app.selectedGpuIndex, reason);
            set_profile_status_text(
                "Showing GPU %u, not the saved GPU %u: %s Select a GPU to use it.",
                g_app.selectedGpuIndex, requested->nvapiIndex, reason);
        }
        populate_gpu_selector();
        return;
    }
    unsigned int saved = 0;
    const bool savedResolved = gui_gpu_selection_resolve_saved(&saved);
    const GpuSelectionAttemptKey key = gui_gpu_selection_attempt_key(saved);
    const GpuSelectionReconcileAction action = gpu_selection_reconcile_decide(
        g_app.adapterCount, savedResolved, saved, g_app.selectedGpuIndex, &key,
        &g_guiSelectionLastRefused);
    if (action == GPU_SELECTION_RECONCILE_HELD) {
        debug_log_on_change("gpu selection: saved nvapi=%u stays unshown (shown=%u) until the service, GPU or active intent changes\n",
            saved, g_app.selectedGpuIndex);
        return;
    }
    if (action != GPU_SELECTION_RECONCILE_REQUEST) return;
    debug_log("gpu selection: saved nvapi=%u differs from shown nvapi=%u; asking the service to show it\n",
        saved, g_app.selectedGpuIndex);
    g_guiSelectionReconcilePending = true;
    gui_service_io_set_full_sync_target(&g_app.adapters[saved]);
    gui_mutation_advance_gpu_epoch("saved GPU selection");
    gui_service_begin_full_sync("saved GPU selection");
}
