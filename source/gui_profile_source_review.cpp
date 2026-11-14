// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#include "profile_editor_review_session.h"

// Window-thread state only. The coordinator sees a fixed getter request and
// token; it never accesses this document, report or native source window.
static ProfileEditorReviewContext g_profileShownContext{};
static std::unique_ptr<ProfileEditorReviewJob> g_profileReviewJob;
static std::unique_ptr<ProfileDestinationReview> g_profileReview;
static gc_u64 g_profileReadToken = 0;
static bool g_profileReadPending = false;

static void gui_profile_source_invalidate_review(const char* reason) {
    const bool hadReview = g_profileReviewJob != nullptr;
    g_profileReviewJob.reset(); g_profileReview.reset(); g_profileReadPending = false;
    if (g_profileSourceWindow) {
        g_profileSourceWindow->clear_report();
        if (hadReview) g_profileSourceWindow->notice(reason);
    }
    if (hadReview) debug_log("profile-check: expired reason=%s\n", reason);
}
static void gui_profile_source_service_changed() {
    if (!g_profileReviewJob) return;
    if (!gui_service_model_ready(&g_app.guiServiceModel) || g_app.applyInFlight ||
        g_app.configuredGpuSelectionUnresolved ||
        !profile_editor_review_context_equal(g_profileReviewJob->context, g_profileShownContext))
        gui_profile_source_invalidate_review("Destination review expired; check the current GPU again");
}
static void gui_profile_source_observe_service(const ServiceResponse* response) {
    g_profileShownContext = response ? profile_editor_review_context(*response) : ProfileEditorReviewContext{};
    gui_profile_source_service_changed();
}
static void gui_profile_source_check(void*, const ProfileDocument& document) {
    char error[256]{};
    if (g_profileReadPending) { g_profileSourceWindow->notice("A destination check is already pending"); return; }
    gui_profile_source_invalidate_review("Checking the destination again");
    if (!gui_service_model_ready(&g_app.guiServiceModel) || g_app.applyInFlight ||
        !validate_configured_gpu_selection_for_client(error, sizeof(error)) || !g_profileShownContext.instance ||
        !hardware_capture_target_matches(g_app.selectedGpu, g_profileShownContext.target)) {
        g_profileSourceWindow->notice(error[0] ? error : "Show the selected GPU in a ready service session before checking"); return;
    }
    if (g_profileReadToken == ~(gc_u64)0) { g_profileSourceWindow->notice("Reopen the application before checking again"); return; }
    auto job = std::make_unique<ProfileEditorReviewJob>(); job->token = ++g_profileReadToken;
    job->source = document; job->sourceRevision = g_app.guiSource.revision; job->context = g_profileShownContext;
    if (!gui_service_io_queue_capture(job->context.target, profile_editor_review_domains(document), job->token, error, sizeof(error))) {
        g_profileSourceWindow->notice(error); return;
    }
    g_profileReviewJob = std::move(job); g_profileReadPending = true;
    g_profileSourceWindow->notice("Reading destination controls; source settings and GPU ownership stay unchanged");
}
static void gui_profile_source_handle_check(GuiServiceIoCompletion* completion) {
    const auto release = [](GuiServiceIoCompletion* value) { if (value) HeapFree(GetProcessHeap(), 0, value); };
    std::unique_ptr<GuiServiceIoCompletion, decltype(release)> owned(completion, release);
    if (!completion || !g_profileReviewJob || completion->captureToken != g_profileReviewJob->token) return;
    g_profileReadPending = false;
    if (!completion->transportSuccess || completion->presentationEpoch != gui_service_presentation_epoch() ||
        !g_app.guiSource.valid || !gui_service_model_ready(&g_app.guiServiceModel) || g_app.applyInFlight) {
        gui_profile_source_invalidate_review(completion->error[0] ? completion->error : "GPU session changed during the check; try again"); return;
    }
    if (completion->response.status != SERVICE_STATUS_OK) {
        gui_profile_source_invalidate_review(completion->response.message[0] ? completion->response.message : "Destination capture is unavailable"); return;
    }
    auto review = std::make_unique<ProfileDestinationReview>(); std::vector<std::string> lines; int result = PROFILE_COMMAND_INVALID;
    if (!profile_editor_review_complete(*g_profileReviewJob, completion->captureToken, g_app.guiSource.revision,
            g_profileShownContext, completion->response, lines, *review, result)) {
        gui_profile_source_invalidate_review("Source, GPU controls or ownership changed during the check; try again"); return;
    }
    debug_log("profile-check: completed token=%llu revision=%llu stage=%d result=%d lines=%u\n",
        (unsigned long long)completion->captureToken, (unsigned long long)g_app.guiSource.revision,
        (int)review->stage, result, (unsigned)lines.size());
    g_profileReview = std::move(review); g_profileSourceWindow->report(lines);
    g_profileSourceWindow->notice("Read-only destination report; no Apply authority is granted");
}
static void gui_profile_source_input_changed(void*) {
    gui_profile_source_invalidate_review("Source input changed; stage the value and check again");
}
static void gui_profile_source_check_allocation_failed(gc_u64 token) {
    if (g_profileReviewJob && g_profileReviewJob->token == token)
        gui_profile_source_invalidate_review("Could not allocate the destination result; try again");
}
