// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#include "profile_editor_capture.h"

static bool gui_profile_source_open();
static void gui_profile_source_refresh();
struct GuiProfileCaptureJob {
    gc_u64 token = 0;
    gc_u32 domains = 0;
    LONG epoch = 0;
    ProfileEditorReviewContext context{};
};
static std::unique_ptr<GuiProfileCaptureJob> g_profileCaptureJob;
static std::unique_ptr<HardwareCaptureObservation> g_profileCaptureObservation;
static std::unique_ptr<ProfileCaptureProposals> g_profileCaptureProposals;
static bool g_profileCapturePending = false;

static bool gui_profile_capture_active() {
    return g_profileSourceWindow && g_profileSourceWindow->capture_active();
}
static bool gui_profile_capture_needs_discard() {
    return g_app.guiSource.valid ? profile_editor_document_dirty(g_app.guiSource) ||
        (g_profileSourceWindow && g_profileSourceWindow->pending()) : g_app.guiStateDirty || g_app.guiHasUserModifiedValues;
}
static void gui_profile_capture_invalidate(const char* reason) {
    const bool hadJob = g_profileCaptureJob != nullptr;
    if (g_profileCaptureJob && g_profileCapturePending) gui_service_io_cancel_capture(g_profileCaptureJob->token);
    g_profileCaptureJob.reset(); g_profileCaptureObservation.reset(); g_profileCaptureProposals.reset(); g_profileCapturePending = false;
    if (gui_profile_capture_active()) { g_profileSourceWindow->capture_reset(); g_profileSourceWindow->notice(reason); }
    if (hadJob) debug_log("profile-capture: expired reason=%s\n", reason);
}
static bool gui_profile_capture_context_current() {
    return g_profileCaptureJob && gui_service_model_ready(&g_app.guiServiceModel) && !g_app.applyInFlight &&
        !g_app.configuredGpuSelectionUnresolved && g_profileCaptureJob->epoch == gui_service_presentation_epoch() &&
        profile_editor_review_context_equal(g_profileCaptureJob->context, g_profileShownContext);
}
static void gui_profile_capture_service_changed() {
    if (g_profileCaptureJob && !gui_profile_capture_context_current())
        gui_profile_capture_invalidate("GPU session or ownership changed; Read a new capture before accepting");
}
static void gui_profile_capture_begin() {
    if (gui_profile_transfer_active()) return;
    if (gui_profile_capture_active()) { g_profileSourceWindow->show(); return; }
    if (!gui_profile_source_open()) {
        gc_message_box(g_app.hMainWnd, "Could not open capture review. The previous draft is preserved.", "Green Curve", MB_OK | MB_ICONERROR); return;
    }
    gui_profile_source_invalidate_review("Capture opened; check the destination again before Apply");
    gui_profile_capture_invalidate("Choose settings to capture");
    g_profileSourceWindow->begin_capture(gui_profile_capture_needs_discard());
    g_profileSourceWindow->show(); gui_profile_source_refresh();
    set_profile_status_text("Capture review opened. The previous draft is preserved until explicit acceptance.");
    debug_log("profile-capture: opened\n");
}
static void gui_profile_capture_read(void*, gc_u32 domains) {
    if (!gui_profile_capture_active()) return;
    if (g_profileCapturePending) { g_profileSourceWindow->notice("A capture read is already pending"); return; }
    gui_profile_capture_invalidate("Reading selected GPU controls");
    char error[256]{};
    if (!gui_service_model_ready(&g_app.guiServiceModel) || g_app.applyInFlight ||
        !validate_configured_gpu_selection_for_client(error, sizeof(error)) || !g_profileShownContext.instance ||
        !hardware_capture_target_matches(g_app.selectedGpu, g_profileShownContext.target)) {
        g_profileSourceWindow->notice(error[0] ? error : "Show the selected GPU in a ready service session before reading"); return;
    }
    if (g_profileReadToken == ~(gc_u64)0) { g_profileSourceWindow->notice("Reopen the application before reading again"); return; }
    auto job = std::make_unique<GuiProfileCaptureJob>(); job->token = ++g_profileReadToken;
    job->context = g_profileShownContext; job->epoch = gui_service_presentation_epoch(); job->domains = domains;
    if (!gui_service_io_queue_capture(job->context.target, domains, job->token, error, sizeof(error))) {
        g_profileSourceWindow->notice(error); return;
    }
    g_profileCaptureJob = std::move(job); g_profileCapturePending = true;
    g_profileSourceWindow->notice("Reading programmable controls; the previous draft is unchanged");
}
// Return true only when this controller took ownership of the completion.
// The ordinary Check controller frees stale/unmatched capture replies.
static bool gui_profile_capture_handle_completion(GuiServiceIoCompletion* completion) {
    if (!completion || !g_profileCaptureJob || completion->captureToken != g_profileCaptureJob->token) return false;
    const auto release = [](GuiServiceIoCompletion* value) { HeapFree(GetProcessHeap(), 0, value); };
    std::unique_ptr<GuiServiceIoCompletion, decltype(release)> owned(completion, release);
    g_profileCapturePending = false;
    if (!gui_profile_capture_active() || !gui_profile_capture_context_current() ||
        completion->presentationEpoch != g_profileCaptureJob->epoch || !completion->transportSuccess) {
        gui_profile_capture_invalidate(completion->error[0] ? completion->error : "GPU session changed during capture; try again"); return true;
    }
    const auto& response = completion->response;
    if (response.status != SERVICE_STATUS_OK || !profile_editor_review_capture_matches(g_profileCaptureJob->context, response) ||
        response.capture.requestedDomains != g_profileCaptureJob->domains) {
        gui_profile_capture_invalidate(response.message[0] ? response.message : "Capture was invalid or the GPU controls changed; Read again"); return true;
    }
    auto ignored = std::make_unique<ProfileDocument>(); std::vector<std::string> lines;
    auto proposals = std::make_unique<ProfileCaptureProposals>();
    auto context = profile_capture_proposal_context(response);
    if (!context.pinAvailabilityKnown) {
        context.pinAvailabilityKnown = true;
        context.pinAvailable = (g_profileCaptureJob->context.domains & SERVICE_MUTATION_DOMAIN_LOCK) != 0;
    }
    const int result = profile_editor_capture_review(response.capture, false, PROFILE_CAPTURE_UNDECIDED, *ignored, lines,
        context, proposals.get());
    const bool ready = result == PROFILE_COMMAND_DECISION && response.capture.capturedDomains;
    g_profileCaptureObservation = std::make_unique<HardwareCaptureObservation>(response.capture);
    g_profileSourceWindow->capture_report(lines, ready, response.capture.omittedDomains,
        ready ? profile_capture_available_representations(*proposals) : 0u, proposals->suggested);
    g_profileCaptureProposals = ready ? std::move(proposals) : nullptr;
    g_profileSourceWindow->notice("Review settings, evidence and omissions; choose a representation to change only the draft");
    debug_log("profile-capture: reviewed token=%llu domains=0x%03X omitted=0x%03X ready=%d\n",
        (unsigned long long)completion->captureToken, (unsigned)g_profileCaptureJob->domains,
        (unsigned)response.capture.omittedDomains, ready ? 1 : 0);
    return true;
}
static bool gui_profile_capture_allocation_failed(gc_u64 token) {
    if (!g_profileCaptureJob || g_profileCaptureJob->token != token) return false;
    gui_profile_capture_invalidate("Could not allocate capture results; try again"); return true;
}
static bool gui_profile_capture_accept(void*, bool allowPartial, bool discard,
    ProfileCaptureRepresentation representation = PROFILE_CAPTURE_EXACT) {
    if (!gui_profile_capture_active() || !g_profileCaptureObservation || !g_profileCaptureProposals || !g_profileCaptureObservation->capturedDomains ||
        !gui_profile_capture_context_current()) {
        gui_profile_capture_invalidate("Read and review a current snapshot before accepting"); return false;
    }
    if (g_profileCaptureObservation->omittedDomains && !allowPartial) {
        g_profileSourceWindow->notice("Explicitly allow omitted settings before accepting this snapshot"); return false;
    }
    if (gui_profile_capture_needs_discard() && !discard) {
        g_profileSourceWindow->capture_require_discard();
        g_profileSourceWindow->notice("Explicitly confirm replacing the unsaved or unfinished draft"); return false;
    }
    auto source = std::make_unique<ProfileDocument>(); char why[192]{};
    if (!profile_capture_select_proposal(*g_profileCaptureProposals, representation, *source, why, sizeof(why))) {
        g_profileSourceWindow->notice(why[0] ? why : "This representation is unavailable; the previous draft is preserved");
        debug_log("profile-capture: proposal refused representation=%d reason=%s\n", (int)representation, why); return false;
    }
    auto candidate = std::make_unique<ProfileEditorCandidate>(); profile_editor_source_candidate(*source, *candidate);
    candidate->source.unsaved = true;
    gui_profile_source_invalidate_review("A captured source draft was accepted"); gui_draft_discard();
    g_app.guiSource = candidate->source;
    g_profileSourceWindow->reload(); gui_profile_capture_invalidate("Capture accepted");
    gui_profile_source_refresh();
    g_profileSourceWindow->notice("Captured source draft; Save stores it; GPU settings were not applied");
    set_profile_status_text("Captured source draft. Save stores it; GPU settings were not applied.");
    debug_log("profile-capture: accepted source=unsaved representation=%s domains=0x%03X\n",
        profile_capture_representation_name(representation), (unsigned)source->domains);
    return true;
}
static void gui_profile_capture_cancel(void*) {
    gui_profile_capture_invalidate("Capture cancelled"); gui_profile_source_refresh();
    set_profile_status_text("Capture cancelled. The previous draft and unfinished input were kept.");
}
static void gui_profile_capture_domains_changed(void*) {
    gui_profile_capture_invalidate("Capture settings changed; Read a new snapshot");
}
static void gui_profile_tools_append_menu(HMENU menu) {
    if (!menu) return;
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING | (gui_service_model_ready(&g_app.guiServiceModel) && !g_app.applyInFlight ? 0 : MF_GRAYED),
        PROFILE_CAPTURE_ID, L"Capture current GPU settings...");
    AppendMenuW(menu, MF_STRING | (g_app.applyInFlight || gui_profile_capture_active() ? MF_GRAYED : 0),
        PROFILE_TRANSFER_ID, L"Import / export saved profiles...");
}
