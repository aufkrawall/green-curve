// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#if defined(_WIN32)
#include "profile_review_fixture.h"
#include "app_shared.h"
#include "gui_mutation_queue_policy.h"
#include "service_request_deadline_policy.h"
#include "gui_service_io_types.h"

namespace gui_profile_source_review_fixture {
static AppData g_app{};
struct Window {
    int reports = 0, clears = 0;
    std::string status;
    std::vector<std::string> lines;
    void clear_report() { ++clears; lines.clear(); }
    void notice(const char* message) { status = message; }
    void report(const std::vector<std::string>& report) { ++reports; lines = report; }
};
static std::unique_ptr<Window> g_profileSourceWindow;
static int sends = 0;
static LONG epoch = 1;
static bool selection = true, queue = true;
static GpuAdapterInfo sentTarget{};
static gc_u32 sentDomains = 0;
static gc_u64 sentToken = 0;
static void debug_log(const char*, ...) {}
static LONG gui_service_presentation_epoch() { return epoch; }
static bool validate_configured_gpu_selection_for_client(char*, size_t) { return selection; }
static bool gui_service_io_queue_capture(const GpuAdapterInfo& target, gc_u32 domains, gc_u64 token, char* error, size_t size) {
    ++sends; sentTarget = target; sentDomains = domains; sentToken = token;
    if (!queue) { snprintf(error, size, "fixture queue refusal"); return false; }
    return true;
}
#include "gui_profile_source_review.cpp"
static GuiServiceIoCompletion* completion(const ServiceResponse& response, gc_u64 token = 0) {
    auto* out = (GuiServiceIoCompletion*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(GuiServiceIoCompletion));
    if (out) {
        out->kind = GUI_SERVICE_IO_CAPTURE; out->transportSuccess = true; out->response = response;
        out->captureToken = token ? token : sentToken; out->presentationEpoch = epoch;
    }
    return out;
}
static int run() {
    g_profileSourceWindow = std::make_unique<Window>();
    auto live = std::make_unique<ServiceResponse>(); profile_review_fixture::live(*live);
    g_app.guiServiceModel.phase = GUI_SERVICE_READY;
    g_app.guiServiceModel.hasAcceptedEnvelope = true;
    g_app.guiSource.valid = true; g_app.guiSource.value = profile_review_fixture::source();
    g_app.selectedGpu = live->snapshot.adapters[0];
    gui_profile_source_observe_service(live.get());
    const auto start = [&] { gui_profile_source_check(nullptr, g_app.guiSource.value); };
    start();
    if (sends != 1 || !g_profileReadPending || !g_profileReviewJob || sentTarget.pciBus != 6 ||
        sentDomains != profile_editor_review_domains(g_app.guiSource.value)) return 9371;
    start(); if (sends != 1 || !g_profileReadPending) return 9372;
    profile_review_fixture::capture(*live, g_app.guiSource.value);
    auto* answer = completion(*live); if (!answer) return 9373;
    gui_profile_source_handle_check(answer);
    if (g_profileReadPending || !g_profileReview || !g_profileReview->planned() || g_profileSourceWindow->reports != 1 ||
        g_app.guiSource.value.memoryOffsetHz != -123500000 || g_app.serviceActiveDesiredValid) return 9374;
    ++live->state.stateRevision; gui_profile_source_observe_service(live.get());
    if (!g_profileReview || g_profileSourceWindow->lines.empty()) return 9375;
    start(); const auto obsolete = sentToken;
    gui_profile_source_input_changed(nullptr);
    if (g_profileReviewJob || g_profileReview || g_profileReadPending || !g_profileSourceWindow->lines.empty()) return 9376;
    answer = completion(*live, obsolete); if (!answer) return 9373;
    gui_profile_source_handle_check(answer);
    if (g_profileReviewJob || g_profileSourceWindow->reports != 1) return 9377;
    start(); ++g_app.guiSource.revision;
    answer = completion(*live); if (!answer) return 9373;
    gui_profile_source_handle_check(answer);
    if (g_profileReviewJob || g_profileSourceWindow->reports != 1) return 9378;
    start(); g_app.applyInFlight = true; gui_profile_source_service_changed();
    if (g_profileReviewJob || g_profileReadPending) return 9379;
    g_app.applyInFlight = false; start();
    live->state.activeDesiredValid = 1; live->desired.hasMemOffset = 1; live->desired.memOffsetMHz = 0;
    gui_profile_source_observe_service(live.get());
    if (g_profileReviewJob || g_profileReadPending) return 9380;
    live->state.activeDesiredValid = 0; live->desired = {}; gui_profile_source_observe_service(live.get());
    start(); answer = completion(*live); if (!answer) return 9373;
    ++epoch; gui_profile_source_handle_check(answer);
    if (g_profileReviewJob || g_profileReadPending) return 9381;
    start(); answer = completion(*live); if (!answer) return 9373;
    answer->transportSuccess = false; snprintf(answer->error, sizeof(answer->error), "fixture transport refusal");
    gui_profile_source_handle_check(answer);
    if (g_profileSourceWindow->status != "fixture transport refusal" || g_profileReadPending) return 9382;
    start(); answer = completion(*live); if (!answer) return 9373;
    answer->response.status = SERVICE_STATUS_ERROR; snprintf(answer->response.message, sizeof(answer->response.message), "fixture capture refusal");
    gui_profile_source_handle_check(answer);
    if (g_profileSourceWindow->status != "fixture capture refusal" || g_profileReadPending) return 9383;
    start(); gui_profile_source_check_allocation_failed(sentToken);
    if (g_profileReviewJob || g_profileReadPending) return 9384;
    const auto count = sends; selection = false; start(); selection = true;
    ++g_app.selectedGpu.pciBus; start(); --g_app.selectedGpu.pciBus;
    g_app.guiServiceModel.phase = GUI_SERVICE_DISCONNECTED; start(); g_app.guiServiceModel.phase = GUI_SERVICE_READY;
    if (sends != count || g_profileReviewJob) return 9385;
    queue = false; start(); queue = true;
    if (g_profileReviewJob || g_profileReadPending || g_profileSourceWindow->status != "fixture queue refusal") return 9386;
    g_profileSourceWindow.reset(); return 0;
}
}
int run_gui_profile_source_review_tests() { return gui_profile_source_review_fixture::run(); }
#else
int run_gui_profile_source_review_tests() { return 0; }
#endif
