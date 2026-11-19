// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
// "Import current GPU settings": a passive snapshot of the selected GPU,
// summarized in plain language and saved to a profile slot as an ordinary
// record (profile_capture_save.h). Reading and saving never change GPU
// settings, ownership or the editor draft.
#include "profile_editor_capture.h"
#include "profile_editor_review_session.h"
#include "profile_capture_save.h"
#include "nvml_pin_observation_sampler.h"

enum {
    CAPTURE_DLG_INTRO = 9300, CAPTURE_DLG_STATUS, CAPTURE_DLG_STYLE_LABEL, CAPTURE_DLG_STYLE,
    CAPTURE_DLG_DETECT, CAPTURE_DLG_DETECT_NOTE, CAPTURE_DLG_SLOT_LABEL, CAPTURE_DLG_SLOT, CAPTURE_DLG_SLOT_NOTE,
    CAPTURE_DLG_SAVE, CAPTURE_DLG_CANCEL, CAPTURE_DLG_RETRY,
    CAPTURE_DLG_GROUP_CHECK = 9320, CAPTURE_DLG_GROUP_VALUE = 9330
};
struct GuiProfileCaptureJob {
    gc_u64 token = 0;
    gc_u32 domains = 0;
    LONG epoch = 0;
    ProfileEditorReviewContext context{};
};
struct GuiCaptureDialog {
    HWND hwnd = nullptr, status = nullptr, styleLabel = nullptr, style = nullptr, detect = nullptr, detectNote = nullptr;
    HWND slotLabel = nullptr, slot = nullptr, slotNote = nullptr, save = nullptr, cancel = nullptr, retry = nullptr, intro = nullptr;
    HWND groupCheck[PROFILE_CAPTURE_GROUP_COUNT]{}, groupValue[PROFILE_CAPTURE_GROUP_COUNT]{};
    UiCheckboxState groupState[PROFILE_CAPTURE_GROUP_COUNT]{};
    GuiProfileDialogBrushes brushes;
    int preferredSlot = 1;
    bool ready = false, failed = false, rereadAfterPending = false;
};
static GuiCaptureDialog g_captureDialog;
static ProfileEditorReviewContext g_profileShownContext{};
static gc_u64 g_profileReadToken = 0;
static std::unique_ptr<GuiProfileCaptureJob> g_profileCaptureJob;
static std::unique_ptr<HardwareCaptureObservation> g_profileCaptureObservation;
static std::unique_ptr<ProfileCaptureProposals> g_profileCaptureProposals;
static ProfileCaptureProposalContext g_profileCaptureProposalContext{};
static std::unique_ptr<PinObservationResult> g_profilePinObservation;
typedef bool (*GuiPinObservationSamplerFn)(const HardwareCaptureBinding&, std::vector<PinWorkloadSample>&, char*, size_t);
static GuiPinObservationSamplerFn g_guiProfileCapturePinSampler = nullptr;
static bool g_profileCapturePending = false;

static bool gui_profile_capture_active() { return g_captureDialog.hwnd != nullptr; }
static bool gui_profile_capture_context_current() {
    return g_profileCaptureJob && gui_service_model_ready(&g_app.guiServiceModel) && !g_app.applyInFlight &&
        !g_app.configuredGpuSelectionUnresolved && g_profileCaptureJob->epoch == gui_service_presentation_epoch() &&
        profile_editor_review_context_equal(g_profileCaptureJob->context, g_profileShownContext);
}
static void gui_capture_status(const char* text) { gui_profile_dialog_set_text(g_captureDialog.status, text); }
static void gui_capture_clear_results() {
    g_profileCaptureObservation.reset(); g_profileCaptureProposals.reset(); g_profileCaptureProposalContext = {};
    g_profilePinObservation.reset(); g_captureDialog.ready = false;
}
static void gui_capture_forget_job() {
    if (g_profileCaptureJob && g_profileCapturePending) gui_service_io_cancel_capture(g_profileCaptureJob->token);
    g_profileCaptureJob.reset(); g_profileCapturePending = false;
}

// ---- presentation ---------------------------------------------------------
static bool gui_capture_pin_observable() {
    if (!g_captureDialog.ready || !g_profileCaptureObservation || !g_profileCaptureProposals) return false;
    const auto& o = *g_profileCaptureObservation;
    return !profile_capture_pin_has_stronger_tier(o, g_profileCaptureProposalContext) &&
        (o.capturedDomains & PROFILE_DOMAIN_VF_CURVE) && (o.requestedDomains & PROFILE_DOMAIN_LOCK) &&
        !(g_profilePinObservation && g_profileCaptureProposals->pin.selectable());
}
static unsigned int gui_capture_included() {
    unsigned int mask = 0;
    for (int g = 0; g < PROFILE_CAPTURE_GROUP_COUNT; ++g)
        if (IsWindowEnabled(g_captureDialog.groupCheck[g]) && ui_checkbox_state_get(&g_captureDialog.groupState[g]))
            mask |= 1u << g;
    return mask;
}
static void gui_capture_fill_styles() {
    HWND style = g_captureDialog.style;
    const LPARAM previous = gui_profile_dialog_combo_data(style, -1);
    SendMessageW(style, CB_RESETCONTENT, 0, 0);
    if (!g_profileCaptureProposals) return;
    const unsigned int available = profile_capture_available_representations(*g_profileCaptureProposals);
    for (auto r : {PROFILE_CAPTURE_FLATTEN, PROFILE_CAPTURE_PIN, PROFILE_CAPTURE_EXACT}) {
        if (!(available & profile_capture_representation_bit(r))) continue;
        std::string label = profile_capture_choice_label(*g_profileCaptureProposals, r);
        if (r == g_profileCaptureProposals->suggested && r != PROFILE_CAPTURE_EXACT) label += " (recommended)";
        gui_profile_dialog_combo_add(style, label.c_str(), (LPARAM)r);
    }
    gui_profile_dialog_combo_select(style, previous >= 0 && (available & profile_capture_representation_bit((ProfileCaptureRepresentation)previous))
        ? previous : (LPARAM)g_profileCaptureProposals->suggested);
}
static void gui_capture_update_slot_note() {
    const int slot = (int)gui_profile_dialog_combo_data(g_captureDialog.slot, 1);
    std::string note;
    if (is_profile_slot_saved(g_app.configPath, slot)) note = "Replaces the profile saved in slot " + std::to_string(slot) + ".";
    else note = "Slot " + std::to_string(slot) + " is empty.";
    note += " Saving does not change your GPU; apply the slot to let Green Curve manage these settings.";
    gui_profile_dialog_set_text(g_captureDialog.slotNote, note.c_str());
}
static void gui_capture_layout() {
    auto& d = g_captureDialog;
    if (!d.hwnd) return;
    const int w = 600, labelW = 170, valueX = 16 + labelW + 10, valueW = w - valueX - 16;
    int y = 14;
    gui_profile_dialog_place(d.intro, 16, y, w - 32, 36); y += 46;
    ProfileCaptureSummary summary{};
    if (d.ready && g_profileCaptureObservation)
        summary = profile_capture_summary(*g_profileCaptureObservation, g_profileCaptureProposals.get());
    for (int g = 0; g < PROFILE_CAPTURE_GROUP_COUNT; ++g) {
        const bool shown = !d.ready || profile_capture_group_shown(summary, g);
        const bool advancedPending = !d.ready && g == PROFILE_CAPTURE_GROUP_ADVANCED;
        gui_profile_dialog_place(d.groupCheck[g], 16, y, labelW, 24, shown && !advancedPending);
        gui_profile_dialog_place(d.groupValue[g], valueX, y + 3, valueW, 20, shown && !advancedPending);
        if (shown && !advancedPending) { fit_themed_checkbox_to_label(d.groupCheck[g]); y += 30; }
    }
    y += 8;
    const bool curve = d.ready && ui_checkbox_state_get(&d.groupState[PROFILE_CAPTURE_GROUP_CURVE]) &&
        IsWindowEnabled(d.groupCheck[PROFILE_CAPTURE_GROUP_CURVE]);
    gui_profile_dialog_place(d.styleLabel, 16, y + 4, labelW, 20, curve);
    gui_profile_dialog_place(d.style, valueX, y, valueW, 220, curve);
    if (curve) y += 34;
    const bool detect = curve && gui_capture_pin_observable();
    gui_profile_dialog_place(d.detect, valueX, y, 250, 28, detect);
    gui_profile_dialog_place(d.detectNote, valueX, y + 32, valueW, 34, detect);
    if (detect) y += 72;
    gui_profile_dialog_place(d.slotLabel, 16, y + 4, labelW, 20);
    gui_profile_dialog_place(d.slot, valueX, y, valueW, 220);
    y += 32;
    gui_profile_dialog_place(d.slotNote, valueX, y, valueW, 36);
    y += 46;
    gui_profile_dialog_place(d.status, 16, y, w - 32, 36);
    y += 44;
    gui_profile_dialog_place(d.retry, 16, y, 120, 28, d.failed);
    gui_profile_dialog_place(d.save, w - 16 - 100 - 10 - 130, y, 130, 28);
    gui_profile_dialog_place(d.cancel, w - 16 - 100, y, 100, 28);
    gui_profile_dialog_fit(d.hwnd, w, y + 42);
}
static void gui_capture_project() {
    auto& d = g_captureDialog;
    if (!d.hwnd) return;
    ProfileCaptureSummary summary{};
    if (d.ready && g_profileCaptureObservation)
        summary = profile_capture_summary(*g_profileCaptureObservation, g_profileCaptureProposals.get());
    for (int g = 0; g < PROFILE_CAPTURE_GROUP_COUNT; ++g) {
        const auto& row = profile_capture_group_row(summary, g);
        const bool usable = d.ready && row.available;
        if (!usable) ui_checkbox_state_set(&d.groupState[g], false);
        EnableWindow(d.groupCheck[g], usable);
        gui_profile_dialog_set_text(d.groupValue[g], !d.ready ? (d.failed ? "-" : "Reading...") : row.text.c_str());
        InvalidateRect(d.groupCheck[g], nullptr, FALSE);
    }
    EnableWindow(d.style, d.ready && SendMessageW(d.style, CB_GETCOUNT, 0, 0) > 1);
    EnableWindow(d.save, d.ready && gui_capture_included() != 0 && !g_profileCapturePending);
    EnableWindow(d.slot, d.ready);
    gui_capture_update_slot_note();
    gui_capture_layout();
}

// ---- reading --------------------------------------------------------------
static void gui_profile_capture_read() {
    if (!gui_profile_capture_active()) return;
    if (g_profileCapturePending) { g_captureDialog.rereadAfterPending = true; return; }
    gui_capture_forget_job(); gui_capture_clear_results();
    g_captureDialog.failed = false; g_captureDialog.rereadAfterPending = false;
    char error[256]{};
    if (!gui_service_model_ready(&g_app.guiServiceModel) || g_app.applyInFlight ||
        !validate_configured_gpu_selection_for_client(error, sizeof(error)) || !g_profileShownContext.instance ||
        !hardware_capture_target_matches(g_app.selectedGpu, g_profileShownContext.target)) {
        g_captureDialog.failed = true;
        gui_capture_status(error[0] ? error : "The GPU is not ready. Wait for the background service, then read again.");
        gui_capture_project(); return;
    }
    if (g_profileReadToken == ~(gc_u64)0) { gui_capture_status("Restart Green Curve before reading again."); return; }
    auto job = std::make_unique<GuiProfileCaptureJob>(); job->token = ++g_profileReadToken;
    job->context = g_profileShownContext; job->epoch = gui_service_presentation_epoch(); job->domains = PROFILE_CAPTURE_DEFAULT_DOMAINS;
    if (!gui_service_io_queue_capture(job->context.target, job->domains, job->token, error, sizeof(error))) {
        g_captureDialog.failed = true; gui_capture_status(error); gui_capture_project(); return;
    }
    g_profileCaptureJob = std::move(job); g_profileCapturePending = true;
    gui_capture_status("Reading the settings currently on your GPU...");
    gui_capture_project();
    debug_log("profile-capture: read queued token=%llu domains=0x%03X\n",
        (unsigned long long)g_profileCaptureJob->token, (unsigned)g_profileCaptureJob->domains);
}
static void gui_capture_failed(const char* reason) {
    gui_capture_forget_job(); gui_capture_clear_results();
    g_captureDialog.failed = true;
    gui_capture_status(reason);
    gui_capture_project();
    debug_log("profile-capture: read failed reason=%s\n", reason);
}
// Return true only when this controller took ownership of the completion.
static GC_NOINLINE bool gui_profile_capture_handle_completion(GuiServiceIoCompletion* completion) {
    if (!completion || !g_profileCaptureJob || completion->captureToken != g_profileCaptureJob->token) return false;
    const auto release = [](GuiServiceIoCompletion* value) { HeapFree(GetProcessHeap(), 0, value); };
    std::unique_ptr<GuiServiceIoCompletion, decltype(release)> owned(completion, release);
    g_profileCapturePending = false;
    if (!gui_profile_capture_active()) { g_profileCaptureJob.reset(); return true; }
    if (g_captureDialog.rereadAfterPending) { g_profileCaptureJob.reset(); gui_profile_capture_read(); return true; }
    if (!gui_profile_capture_context_current() || completion->presentationEpoch != g_profileCaptureJob->epoch ||
        !completion->transportSuccess) {
        gui_capture_failed(completion->error[0] ? completion->error : "The GPU session changed while reading. Read again.");
        return true;
    }
    const auto& response = completion->response;
    if (response.status != SERVICE_STATUS_OK || !profile_editor_review_capture_matches(g_profileCaptureJob->context, response) ||
        response.capture.requestedDomains != g_profileCaptureJob->domains) {
        gui_capture_failed(response.message[0] ? response.message : "The GPU settings changed while reading. Read again.");
        return true;
    }
    auto ignored = std::make_unique<ProfileDocument>(); std::vector<std::string> lines;
    auto proposals = std::make_unique<ProfileCaptureProposals>();
    auto context = profile_capture_proposal_context(response);
    if (!context.pinAvailabilityKnown) {
        context.pinAvailabilityKnown = true;
        context.pinAvailable = (g_profileCaptureJob->context.domains & SERVICE_MUTATION_DOMAIN_LOCK) != 0;
    }
    // Leaving a setting out is a row choice in this dialog, so unreadable
    // groups are reviewed as partial instead of blocking the whole snapshot.
    const int result = profile_editor_capture_review(response.capture, true, PROFILE_CAPTURE_UNDECIDED, *ignored, lines,
        context, proposals.get());
    if (result != PROFILE_COMMAND_DECISION || !response.capture.capturedDomains) {
        gui_capture_failed("No GPU settings could be read. Check that the background service is running and read again.");
        return true;
    }
    g_profileCaptureObservation = std::make_unique<HardwareCaptureObservation>(response.capture);
    g_profileCaptureProposalContext = context;
    g_profileCaptureProposals = std::move(proposals);
    g_captureDialog.ready = true; g_captureDialog.failed = false;
    for (int g = 0; g < PROFILE_CAPTURE_GROUP_COUNT; ++g) ui_checkbox_state_set(&g_captureDialog.groupState[g], true);
    gui_capture_fill_styles();
    gui_capture_status("Choose what to include and where to save it.");
    gui_capture_project();
    debug_log("profile-capture: reviewed token=%llu captured=0x%03X omitted=0x%03X choices=0x%X suggested=%s\n",
        (unsigned long long)completion->captureToken, (unsigned)response.capture.capturedDomains,
        (unsigned)response.capture.omittedDomains, profile_capture_available_representations(*g_profileCaptureProposals),
        profile_capture_representation_name(g_profileCaptureProposals->suggested));
    char proposalLine[512];
    profile_capture_proposal_log_line(g_profileCaptureProposals->flatten, "flatten", proposalLine, sizeof(proposalLine));
    debug_log("%s\n", proposalLine);
    profile_capture_proposal_log_line(g_profileCaptureProposals->pin, "pin", proposalLine, sizeof(proposalLine));
    debug_log("%s\n", proposalLine);
    return true;
}
static bool gui_profile_capture_allocation_failed(gc_u64 token) {
    if (!g_profileCaptureJob || g_profileCaptureJob->token != token) return false;
    g_profileCapturePending = false;
    gui_capture_failed("Not enough memory to read the GPU settings. Read again.");
    return true;
}
// A changed GPU session or ownership makes the snapshot stale: read again
// rather than offering settings that are no longer on the GPU.
static void gui_profile_capture_service_changed() {
    if (!gui_profile_capture_active() || !g_profileCaptureJob || gui_profile_capture_context_current()) return;
    debug_log("profile-capture: context changed; rereading\n");
    gui_capture_forget_job();
    if (gui_service_model_ready(&g_app.guiServiceModel) && !g_app.applyInFlight) gui_profile_capture_read();
    else gui_capture_failed("The GPU session changed. Read again once it is ready.");
}
static void gui_profile_tools_observe_service(const ServiceResponse* response) {
    g_profileShownContext = response ? profile_editor_review_context(*response) : ProfileEditorReviewContext{};
    gui_profile_capture_service_changed();
}

// ---- actions --------------------------------------------------------------
static void gui_capture_detect_lock() {
    if (!gui_capture_pin_observable() || !gui_profile_capture_context_current()) return;
    gui_capture_status("Sampling clocks under load...");
    std::vector<PinWorkloadSample> samples;
    char err[192]{};
    if (!g_guiProfileCapturePinSampler) snprintf(err, sizeof(err), "clock sampling is unavailable");
    else if (!g_guiProfileCapturePinSampler(g_profileCaptureObservation->binding, samples, err, sizeof(err))) samples.clear();
    auto obs = std::make_unique<PinObservationResult>();
    profile_pin_observation_analyze(*g_profileCaptureObservation, false, samples.data(), (int)samples.size(), *obs);
    debug_log("%s\n", obs->summary);
    if (obs->status != PIN_OBS_SUSPECTED_PIN) {
        std::string text = std::string("No clock lock detected: ") + obs->explanation;
        if (err[0] && samples.empty()) text += std::string(" (") + err + ")";
        gui_capture_status(text.c_str());
        return;
    }
    if (!profile_capture_apply_pin_observation(*g_profileCaptureObservation, g_profileCaptureProposalContext, *obs,
            *g_profileCaptureProposals, obs->proposedPinHz) || !g_profileCaptureProposals->pin.selectable()) {
        gui_capture_status(g_profileCaptureProposals->pin.reason[0] ? g_profileCaptureProposals->pin.reason :
            "A clock lock was detected but cannot be saved for this GPU.");
        return;
    }
    g_profilePinObservation = std::move(obs);
    gui_capture_fill_styles();
    gui_profile_dialog_combo_select(g_captureDialog.style, PROFILE_CAPTURE_PIN);
    gui_capture_status("Detected a clock lock under load. Check the frequency before saving.");
    debug_log("profile-capture: observed pin hz=%lld selectable=1\n", (long long)g_profileCaptureProposals->pin.frequencyHz);
    gui_capture_project();
}
static void gui_capture_save() {
    auto& d = g_captureDialog;
    if (!d.ready || !g_profileCaptureObservation || !g_profileCaptureProposals) return;
    if (!gui_profile_capture_context_current()) { gui_profile_capture_service_changed(); return; }
    const auto representation = (ProfileCaptureRepresentation)gui_profile_dialog_combo_data(d.style, PROFILE_CAPTURE_EXACT);
    const int slot = (int)gui_profile_dialog_combo_data(d.slot, 1);
    const unsigned int included = gui_capture_included();
    auto record = std::make_unique<DesiredSettings>();
    char why[192]{};
    if (!profile_capture_record(*g_profileCaptureObservation, *g_profileCaptureProposals, representation, included,
            *record, why, sizeof(why))) {
        std::string text = std::string("Cannot save: ") + why;
        gui_capture_status(text.c_str());
        debug_log("profile-capture: save refused representation=%s included=0x%X reason=%s\n",
            profile_capture_representation_name(representation), included, why);
        return;
    }
    char err[256]{};
    if (!save_profile_to_config(g_app.configPath, slot, record.get(), err, sizeof(err))) {
        std::string text = std::string("Saving failed: ") + err;
        gui_capture_status(text.c_str());
        write_error_report_log_for_user_failure("Captured profile save failed", err);
        return;
    }
    debug_log("profile-capture: saved slot=%d representation=%s included=0x%X scope=0x%X\n", slot,
        profile_capture_representation_name(representation), included, (unsigned)record->applicationScope);
    invalidate_tray_profile_cache();
    refresh_profile_controls_from_config();
    if (g_app.hProfileCombo && !gui_state_dirty()) SendMessageA(g_app.hProfileCombo, CB_SETCURSEL, (WPARAM)(slot - 1), 0);
    DestroyWindow(d.hwnd);
    set_profile_status_text("Saved the GPU's current settings to slot %d. Apply it to let Green Curve manage them.", slot);
}

// ---- window ---------------------------------------------------------------
static LRESULT CALLBACK gui_capture_dialog_proc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto& d = g_captureDialog;
    switch (msg) {
        case WM_COMMAND: {
            const int id = LOWORD(wParam), note = HIWORD(wParam);
            if (note == BN_CLICKED && id >= CAPTURE_DLG_GROUP_CHECK && id < CAPTURE_DLG_GROUP_CHECK + PROFILE_CAPTURE_GROUP_COUNT) {
                ui_checkbox_state_toggle(&d.groupState[id - CAPTURE_DLG_GROUP_CHECK]);
                gui_capture_project(); return 0;
            }
            if (note == CBN_SELCHANGE && id == CAPTURE_DLG_SLOT) { gui_capture_update_slot_note(); return 0; }
            if (note != BN_CLICKED) break;
            if (id == CAPTURE_DLG_SAVE) gui_capture_save();
            else if (id == CAPTURE_DLG_CANCEL) SendMessageW(hwnd, WM_CLOSE, 0, 0);
            else if (id == CAPTURE_DLG_RETRY) gui_profile_capture_read();
            else if (id == CAPTURE_DLG_DETECT) gui_capture_detect_lock();
            return 0;
        }
        case WM_DRAWITEM: {
            const auto* dis = (const DRAWITEMSTRUCT*)lParam;
            if (dis && dis->CtlID >= CAPTURE_DLG_GROUP_CHECK && dis->CtlID < CAPTURE_DLG_GROUP_CHECK + PROFILE_CAPTURE_GROUP_COUNT) {
                draw_themed_checkbox_control(dis, ui_checkbox_state_get(&d.groupState[dis->CtlID - CAPTURE_DLG_GROUP_CHECK]), true);
                return TRUE;
            }
            if (dis) draw_themed_button(dis);
            return TRUE;
        }
        case WM_CTLCOLORSTATIC: {
            const int id = GetDlgCtrlID((HWND)lParam);
            const bool strong = (id >= CAPTURE_DLG_GROUP_VALUE && id < CAPTURE_DLG_GROUP_VALUE + PROFILE_CAPTURE_GROUP_COUNT) ||
                id == CAPTURE_DLG_STATUS || id == CAPTURE_DLG_INTRO;
            return gui_profile_dialog_color(d.brushes, msg, (HDC)wParam, (HWND)lParam, strong);
        }
        case WM_CTLCOLOREDIT: case WM_CTLCOLORLISTBOX: case WM_CTLCOLORBTN:
            return gui_profile_dialog_color(d.brushes, msg, (HDC)wParam, (HWND)lParam, true);
        case WM_ERASEBKGND: gui_profile_dialog_paint_background(hwnd, (HDC)wParam); return 1;
        case WM_THEMECHANGED: case WM_SETTINGCHANGE: apply_system_titlebar_theme(hwnd); allow_dark_mode_for_window(hwnd); break;
        case WM_CLOSE: DestroyWindow(hwnd); return 0;
        case WM_DESTROY:
            gui_capture_forget_job(); gui_capture_clear_results();
            gui_profile_dialog_release(d.brushes);
            d = GuiCaptureDialog{};
            gui_profile_dialog_restore_owner();
            update_profile_action_buttons(); update_profile_state_label();
            debug_log("profile-capture: closed\n");
            return 0;
        default: break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}
static GC_NOINLINE void gui_profile_capture_begin(int preferredSlot) {
    if (gui_profile_capture_active()) { SetForegroundWindow(g_captureDialog.hwnd); return; }
    if (gui_profile_transfer_active() || g_app.applyInFlight) {
        set_profile_status_text("Finish the open profile dialog or GPU operation first."); return;
    }
    if (!g_guiProfileCapturePinSampler)
        g_guiProfileCapturePinSampler = [](const HardwareCaptureBinding& binding, std::vector<PinWorkloadSample>& samples,
                char* err, size_t errSize) {
            return nvml_pin_observation_sample_burst(binding, PROFILE_PIN_OBS_DEFAULT_SAMPLES, samples, err, errSize,
                []() -> void* { return (void*)load_trusted_nvml_library(); },
                [](void* lib, const char* name) -> void* { return (void*)GetProcAddress((HMODULE)lib, name); },
                [](void* lib) { if (lib) FreeLibrary((HMODULE)lib); });
        };
    auto& d = g_captureDialog;
    d = GuiCaptureDialog{};
    d.preferredSlot = preferredSlot;
    d.hwnd = gui_profile_dialog_create(L"GreenCurveCaptureDialog", "Import current GPU settings", gui_capture_dialog_proc, 600, 420);
    if (!d.hwnd) { set_profile_status_text("Could not open the capture dialog."); return; }
    d.intro = gui_profile_dialog_label(d.hwnd, "Copies the settings currently on your GPU, for example ones set by another tool, "
        "into a profile slot. Nothing on the GPU is changed.", CAPTURE_DLG_INTRO);
    for (int g = 0; g < PROFILE_CAPTURE_GROUP_COUNT; ++g) {
        d.groupCheck[g] = gui_profile_dialog_button(d.hwnd, profile_capture_group_name(g), CAPTURE_DLG_GROUP_CHECK + g);
        d.groupValue[g] = gui_profile_dialog_label(d.hwnd, "Reading...", CAPTURE_DLG_GROUP_VALUE + g, SS_ENDELLIPSIS);
    }
    d.styleLabel = gui_profile_dialog_label(d.hwnd, "Save the curve as", CAPTURE_DLG_STYLE_LABEL);
    d.style = gui_profile_dialog_combo(d.hwnd, CAPTURE_DLG_STYLE);
    d.detect = gui_profile_dialog_button(d.hwnd, "Detect clock lock under load...", CAPTURE_DLG_DETECT);
    d.detectNote = gui_profile_dialog_label(d.hwnd, "Only if another tool pins the clock: start a game or benchmark first, "
        "then detect. Green Curve samples the clocks for a few seconds.", CAPTURE_DLG_DETECT_NOTE);
    d.slotLabel = gui_profile_dialog_label(d.hwnd, "Save to", CAPTURE_DLG_SLOT_LABEL);
    d.slot = gui_profile_dialog_combo(d.hwnd, CAPTURE_DLG_SLOT);
    for (int s = 1; s <= CONFIG_NUM_SLOTS; ++s) gui_profile_dialog_combo_add(d.slot, gui_profile_slot_label(s).c_str(), s);
    gui_profile_dialog_combo_select(d.slot, profile_capture_default_slot(preferredSlot,
        [](int s) { return is_profile_slot_saved(g_app.configPath, s); }, CONFIG_NUM_SLOTS));
    d.slotNote = gui_profile_dialog_label(d.hwnd, "", CAPTURE_DLG_SLOT_NOTE);
    d.status = gui_profile_dialog_label(d.hwnd, "", CAPTURE_DLG_STATUS);
    d.retry = gui_profile_dialog_button(d.hwnd, "Read again", CAPTURE_DLG_RETRY);
    d.save = gui_profile_dialog_button(d.hwnd, "Save profile", CAPTURE_DLG_SAVE);
    d.cancel = gui_profile_dialog_button(d.hwnd, "Cancel", CAPTURE_DLG_CANCEL);
    gui_profile_dialog_show(d.hwnd, d.cancel);
    update_profile_action_buttons();
    debug_log("profile-capture: opened preferredSlot=%d\n", preferredSlot);
    gui_profile_capture_read();
}
