// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#include "profile_source_window.h"
#include "profile_slot_io_windows.h"
#include "profile_document_slot.h"
#include "profile_legacy_ini.h"
#include "nvml_pin_observation_sampler.h"

static std::unique_ptr<ProfileSourceWindow> g_profileSourceWindow;
static void gui_profile_source_refresh();
#include "gui_profile_source_review.cpp"
#include "gui_profile_capture.cpp"
#include "gui_profile_transfer.cpp"

static bool gui_profile_source_confirm_discard(void*) {
    return gc_message_box(g_app.hMainWnd,
        "Discard the unsaved source draft or unfinished input?",
        "Green Curve", MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) == IDYES;
}
static void gui_profile_source_refresh() {
    update_profile_state_label();
    gui_set_editor_enabled(gui_service_model_ready(&g_app.guiServiceModel));
    invalidate_main_window();
}
static void gui_profile_source_changed(void*) {
    gui_profile_source_invalidate_review("Source intent changed; check the destination again");
    debug_log("profile-source: staged revision=%llu points=%d\n",
        (unsigned long long)g_app.guiSource.revision, g_app.guiSource.value.topologyCount);
    gui_profile_source_refresh();
}
static void gui_profile_source_closed(void*) {
    gui_profile_source_invalidate_review("Source closed");
    g_app.guiSource = {};
    debug_log("profile-source: closed; live editor restored\n");
    // The abandoned legacy draft must be replaced by a coherent live envelope
    // before its controls can be used again.
    gui_service_begin_full_sync("source editor closed");
    gui_profile_source_refresh();
    set_profile_status_text("Source closed. The live GPU editor is available.");
}
static bool gui_profile_source_write(void*, const ProfileDocument& document, char* error, size_t size) {
    const int slot = selected_profile_slot_from_combo();
    WindowsProfileSlotIO io; ProfileCodecError failure{};
    if (!profile_document_slot_save(io, g_app.configPath, slot, document, failure)) {
        snprintf(error, size, "%s", failure.message);
        debug_log("profile-source: save refused slot=%d reason=%s\n", slot, failure.message);
        return false;
    }
    invalidate_tray_profile_cache();
    refresh_profile_controls_from_config();
    set_profile_status_text("Saved exact source intent to slot %d. GPU settings were not applied.", slot);
    debug_log("profile-source: saved slot=%d points=%d\n", slot, document.topologyCount);
    return true;
}
static void gui_profile_source_prepare_curve(void*, int modeValue) {
    (void)gui_profile_source_prepare_curve_with_confirm(modeValue, [](const char* title, const char* body) {
        return gc_message_box(g_app.hMainWnd, body, title, MB_YESNO | MB_ICONQUESTION) == IDYES;
    });
}
static bool gui_profile_source_open() {
    if (!g_profileSourceWindow) {
        ProfileSourceWindowCallbacks callbacks{};
        callbacks.save = gui_profile_source_write;
        callbacks.confirmDiscard = gui_profile_source_confirm_discard;
        callbacks.changed = gui_profile_source_changed;
        callbacks.closed = gui_profile_source_closed;
        callbacks.created = apply_system_titlebar_theme;
        callbacks.check = gui_profile_source_check;
        callbacks.inputChanged = gui_profile_source_input_changed;
        callbacks.previewChanged = gui_profile_source_preview_changed;
        callbacks.acceptMapped = gui_profile_source_accept_mapped;
        callbacks.captureRead = gui_profile_capture_read;
        callbacks.captureAccept = gui_profile_capture_accept;
        callbacks.captureCancel = gui_profile_capture_cancel;
        callbacks.captureDomainsChanged = gui_profile_capture_domains_changed;
        callbacks.convertDomain = gui_profile_source_convert_domain;
        callbacks.restoreExact = gui_profile_source_restore_exact;
        callbacks.prepareCurve = gui_profile_source_prepare_curve;
        callbacks.captureObservePin = gui_profile_capture_observe_pin;
        callbacks.captureConfirmPin = gui_profile_capture_confirm_pin;
        g_guiProfileCapturePinSampler = [](const HardwareCaptureBinding& binding, std::vector<PinWorkloadSample>& samples,
                char* err, size_t errSize) {
            return nvml_pin_observation_sample_burst(binding, PROFILE_PIN_OBS_DEFAULT_SAMPLES, samples, err, errSize,
                []() -> void* { return (void*)load_trusted_nvml_library(); },
                [](void* lib, const char* name) -> void* { return (void*)GetProcAddress((HMODULE)lib, name); },
                [](void* lib) { if (lib) FreeLibrary((HMODULE)lib); });
        };
        g_profileSourceWindow = std::make_unique<ProfileSourceWindow>(g_app.guiSource, callbacks);
    }
    return g_profileSourceWindow->create(g_app.hMainWnd, g_dpi, false);
}
// A false result selects the existing online legacy workflow. A true result
// means that this source load (including refusal/cancel) has been handled.
static bool gui_profile_source_load(int slot) {
    if (gui_profile_capture_active()) {
        g_profileSourceWindow->notice("Cancel capture before loading another source"); return true;
    }
    auto candidate = std::make_unique<ProfileDocument>();
    ProfileCodecError error{}; bool found = false;
    const bool offline = !gui_service_model_ready(&g_app.guiServiceModel);
    bool ok = false;
    {
        ConfigStorageLockGuard lock; WindowsProfileSlotIO io;
        std::string bytes, text;
        ok = lock.locked() && io.read(g_app.configPath, bytes, profile_slot::MAX_CONFIG_BYTES, true, error) &&
            profile_config::decode(bytes, text, &error) && profile_document_slot_decode(text, slot, *candidate, found, error);
        if (ok && !found && (offline || g_app.guiSource.valid))
            ok = profile_legacy_native_slot_decode(text, PROFILE_LEGACY_WINDOWS_INI, slot, *candidate, found, &error);
        if (!lock.locked()) (void)profile_codec::fail(&error, 0, "could not acquire profile storage lock");
    }
    if (ok && !found && !offline && !g_app.guiSource.valid) return false;
    if (!ok || !found) {
        gc_message_box(g_app.hMainWnd, error.message[0] ? error.message : "No source profile is available in this slot.",
            "Green Curve", MB_OK | MB_ICONERROR);
        return true;
    }
    // Freeze the decoded candidate before any modal confirmation can re-enter
    // the message loop. Cancellation preserves both documents and raw input.
    if ((g_app.guiStateDirty || g_app.guiHasUserModifiedValues || profile_editor_document_dirty(g_app.guiSource) ||
         (g_profileSourceWindow && g_profileSourceWindow->pending())) && !gui_profile_source_confirm_discard(nullptr)) return true;
    if (!gui_profile_source_open()) {
        gc_message_box(g_app.hMainWnd, "Could not open the profile source editor. The current draft is preserved.",
            "Green Curve", MB_OK | MB_ICONERROR);
        return true;
    }
    gui_draft_discard();
    gui_profile_source_invalidate_review("Another source was loaded");
    g_app.guiSource = {}; g_app.guiSource.valid = true;
    g_app.guiSource.value = g_app.guiSource.saved = *candidate;
    g_profileSourceWindow->reload(); g_profileSourceWindow->show();
    gui_profile_source_refresh();
    debug_log("profile-source: loaded slot=%d points=%d offline=%d\n", slot, candidate->topologyCount, offline ? 1 : 0);
    set_profile_status_text("Loaded slot %d as a source draft. Destination review is required before Apply.", slot);
    return true;
}
static bool gui_profile_source_save() {
    if (gui_profile_capture_active()) {
        g_profileSourceWindow->notice("Cancel capture before saving the previous draft"); return true;
    }
    if (!g_app.guiSource.valid) return false;
    if (g_profileSourceWindow && !g_profileSourceWindow->save()) g_profileSourceWindow->show();
    return true;
}
static bool gui_profile_source_route_message(MSG& message) {
    gui_profile_transfer_drain();
    if (g_profileTransferWindow && g_profileTransferWindow->route(message)) return true;
    return g_profileSourceWindow && g_profileSourceWindow->route(message);
}
static bool gui_profile_source_can_exit() {
    if (gui_profile_transfer_active()) {
        if (g_profileTransferWindow->committing()) return false;
        SendMessageW(g_profileTransferWindow->handle(), WM_CLOSE, 0, 0);
    }
    return !g_app.guiSource.valid ||
        ((!profile_editor_document_dirty(g_app.guiSource) && (!g_profileSourceWindow || !g_profileSourceWindow->pending())) ||
         gui_profile_source_confirm_discard(nullptr));
}
