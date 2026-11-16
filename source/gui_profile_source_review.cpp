// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#include "profile_editor_review_session.h"
#include "profile_mapped_draft.h"

// Window-thread state only. The coordinator sees a fixed getter request and
// token; it never accesses this document, report or native source window.
static ProfileEditorReviewContext g_profileShownContext{};
static std::unique_ptr<ProfileEditorReviewJob> g_profileReviewJob;
static std::unique_ptr<ProfileDestinationReview> g_profileReview;
static gc_u64 g_profileReadToken = 0;
static bool g_profileReadPending = false;

static bool gui_profile_source_mapped_ready() {
    return g_app.guiSource.valid && g_profileReview && g_profileReviewJob && g_profileReview->accepted &&
        !g_profileReadPending && !g_app.applyInFlight && !g_app.configuredGpuSelectionUnresolved &&
        gui_service_model_ready(&g_app.guiServiceModel) &&
        profile_editor_review_context_equal(g_profileReviewJob->context, g_profileShownContext) &&
        profile_mapped_draft_ready(*g_profileReview, g_app.guiSource.value, g_app.guiSource.revision);
}
static void gui_profile_source_accept_mapped(void*) {
    if (!g_profileReview || !g_profileReviewJob || !g_app.guiSource.valid || g_profileReadPending ||
        g_app.applyInFlight || !gui_service_model_ready(&g_app.guiServiceModel) ||
        !profile_editor_review_context_equal(g_profileReviewJob->context, g_profileShownContext) ||
        !profile_mapped_draft_accept(*g_profileReview, g_app.guiSource.value, g_app.guiSource.revision)) {
        g_profileSourceWindow->notice("Check the current source and destination before using a mapped draft"); return;
    }
    g_profileSourceWindow->mapped_ready(false);
    g_profileSourceWindow->notice("Mapped draft staged. Apply submits these exact settings for fresh service validation.");
    debug_log("profile-check: mapped draft accepted revision=%llu controlRevision=%llu\n",
        (unsigned long long)g_app.guiSource.revision, (unsigned long long)g_profileReview->capture.revision);
    gui_profile_source_refresh();
}
static bool gui_profile_source_prepare_apply(DesiredSettings& desired, gc_u64& controlRevision, OcApplyBaseline& baseline) {
    if (!gui_profile_source_mapped_ready() ||
        !profile_mapped_draft_prepare(*g_profileReview, g_app.guiSource.value, g_app.guiSource.revision, desired, controlRevision)) return false;
    const auto& o = g_profileReview->capture;
    const auto* graphics = hardware_capture_named_clock(o.graphics, o.graphicsAlternate, desired.authoring.graphicsSurface);
    const auto* memory = hardware_capture_named_clock(o.memory, o.memoryAlternate, desired.memOffsetSurface);
    baseline = {};
    if (graphics && graphics->valid) baseline.currentGpuOffsetMHz = (int)(graphics->value / 1000000);
    if (memory && memory->valid) baseline.currentMemOffsetMHz = (int)(memory->value / 1000000);
    if (o.xbarVoltage.valid) baseline.currentXbarMsvddOffsetUv = (int)o.xbarVoltage.value;
    return true;
}

static void gui_profile_source_invalidate_review(const char* reason) {
    const bool hadReview = g_profileReviewJob != nullptr;
    if (g_profileReviewJob && g_profileReadPending) gui_service_io_cancel_capture(g_profileReviewJob->token);
    g_profileReviewJob.reset(); g_profileReview.reset(); g_profileReadPending = false;
    if (g_profileSourceWindow) {
        g_profileSourceWindow->clear_report();
        if (hadReview) g_profileSourceWindow->notice(reason);
    }
    if (hadReview) debug_log("profile-check: expired reason=%s\n", reason);
}
static void gui_profile_source_service_changed() {
    gui_profile_capture_service_changed();
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
static void gui_profile_source_check(void*, const ProfileDocument& document, gc_u32 graphics = 0, gc_u32 memory = 0) {
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
    job->graphics = graphics; job->memory = memory;
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
    debug_log("profile-check: completed token=%llu revision=%llu graphics=%u memory=%u stage=%d result=%d lines=%u\n",
        (unsigned long long)completion->captureToken, (unsigned long long)g_app.guiSource.revision,
        (unsigned)g_profileReviewJob->graphics, (unsigned)g_profileReviewJob->memory,
        (int)review->stage, result, (unsigned)lines.size());
    g_profileReview = std::move(review); g_profileSourceWindow->report(lines);
    g_profileSourceWindow->mapped_ready(profile_mapped_draft_ready(*g_profileReview, g_app.guiSource.value, g_app.guiSource.revision));
    g_profileSourceWindow->conversion_options(g_profileReview->conversions);
    g_profileSourceWindow->notice(g_profileReview->conversions.requiresDecision()
        ? "Batch conversion or domain review is required before using this draft; source file is unchanged"
        : "Review mapping, scope and feasibility; Use mapped draft stages these settings without applying them");
}
static void gui_profile_source_convert_domain(void*, int kindValue) {
    if (!g_app.guiSource.valid || g_profileReadPending || g_app.applyInFlight || !g_profileReview || !g_profileReviewJob ||
        g_profileReview->sourceRevision != g_app.guiSource.revision || !gui_service_model_ready(&g_app.guiServiceModel) ||
        !profile_editor_review_context_equal(g_profileReviewJob->context, g_profileShownContext) ||
        kindValue < PROFILE_CONVERT_VF_OFFSETS || kindValue > PROFILE_CONVERT_LOCK_FREQUENCY) {
        if (g_profileSourceWindow) g_profileSourceWindow->notice("Check the current source and destination before converting a domain");
        return;
    }
    const auto* cand = g_profileReview->conversions.find((ProfileConversionKind)kindValue);
    ProfileDestinationTopology dest{}; char why[256]{}, summary[256]{};
    if (!cand || !profile_destination_from_capture(g_profileReview->capture, dest, why, sizeof(why))) {
        if (g_profileSourceWindow) g_profileSourceWindow->notice(why[0] ? why : "Conversion candidate is no longer current; Check again");
        return;
    }
    auto derived = std::make_unique<ProfileDocument>();
    if (!profile_conversion_accept_item(*cand, g_app.guiSource.value, g_app.guiSource.revision,
            g_profileReview->capture, dest, g_profileReview->routes, *derived,
            summary, sizeof(summary), why, sizeof(why))) {
        debug_log("%s\n", summary);
        if (g_profileSourceWindow) g_profileSourceWindow->notice(why[0] ? why : "Domain conversion was refused");
        return;
    }
    debug_log("%s\n", summary);
    g_app.guiSource.value = *derived; ++g_app.guiSource.revision;
    gui_profile_source_invalidate_review("Domain conversion updated source draft");
    if (g_profileSourceWindow) {
        g_profileSourceWindow->reload();
        g_profileSourceWindow->notice("Converted selected domain in source draft (saved slot unchanged); Check again before Apply");
    }
    gui_profile_source_refresh();
}
static void gui_profile_source_restore_exact(void*) {
    if (!g_app.guiSource.valid || g_profileReadPending || g_app.applyInFlight) {
        if (g_profileSourceWindow) g_profileSourceWindow->notice("Wait for pending operations before restoring exact tail");
        return;
    }
    char why[256]{}; auto restored = std::make_unique<ProfileDocument>();
    if (!profile_document_restore_captured_tail(g_app.guiSource.value, *restored, why, sizeof(why))) {
        debug_log("profile-source: restore exact refused reason=%s\n", why);
        if (g_profileSourceWindow) g_profileSourceWindow->notice(why[0] ? why : "Cannot restore exact captured tail");
        return;
    }
    g_app.guiSource.value = *restored; ++g_app.guiSource.revision;
    debug_log("profile-source: restored exact captured tail revision=%llu\n", (unsigned long long)g_app.guiSource.revision);
    gui_profile_source_invalidate_review("Restored exact captured VF tail offsets without recapture");
    if (g_profileSourceWindow) {
        g_profileSourceWindow->reload();
        g_profileSourceWindow->notice("Restored exact captured VF tail offsets without recapture (saved slot unchanged)");
    }
    gui_profile_source_refresh();
}
static void gui_profile_source_input_changed(void*) {
    gui_profile_source_invalidate_review("Source input changed; stage the value and check again");
}
static void gui_profile_source_preview_changed(void*) {
    gui_profile_source_invalidate_review("Preview controls changed; check the destination again");
}
static void gui_profile_source_check_allocation_failed(gc_u64 token) {
    if (g_profileReviewJob && g_profileReviewJob->token == token)
        gui_profile_source_invalidate_review("Could not allocate the destination result; try again");
}
