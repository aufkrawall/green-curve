// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#if defined(_WIN32)
#include "profile_review_fixture.h"
#include "app_shared.h"
#include "gui_mutation_queue_policy.h"
#include "service_request_deadline_policy.h"
#include "gui_service_io_types.h"
#include <cstdarg>

namespace gui_profile_source_review_fixture {
static AppData g_app{};
struct Window {
    int reports = 0, clears = 0, reloads = 0;
    std::string status;
    std::vector<std::string> lines;
    bool mapped = false;
    ProfileConversionPlan conversions{};
    void mapped_ready(bool ready) { mapped = ready; }
    void conversion_options(const ProfileConversionPlan& plan) { conversions = plan; }
    void reload() { ++reloads; clear_report(); }
    void clear_report() { ++clears; lines.clear(); mapped = false; conversions = {}; }
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
static void gui_service_io_cancel_capture(gc_u64) {}
static void gui_profile_capture_service_changed() {}
static void gui_profile_source_refresh() {}
static void set_message(char* out, size_t size, const char* format, ...) {
    va_list args; va_start(args, format); vsnprintf(out, size, format, args); va_end(args);
}
#include "gui_mutation_request_binding.h"
static bool gui_service_io_queue_capture(const GpuAdapterInfo& target, gc_u32 domains, gc_u64 token, char* error, size_t size) {
    ++sends; sentTarget = target; sentDomains = domains; sentToken = token;
    if (!queue) { snprintf(error, size, "fixture queue refusal"); return false; }
    return true;
}
#include "gui_profile_source_review.cpp"
static int applies = 0, highConfirms = 0, memoryConfirms = 0, legacyCaptures = 0;
static bool highAllowed = true, memoryAllowed = true, expireInConfirmation = false, mappedWarning = false;
static ServiceRequest applied{};
static bool gui_profile_capture_active() { return false; }
static void set_profile_status_text(const char*, ...) {}
static void set_pending_operation_source(const char*) {}
static void record_ui_action(const char*, ...) {}
static void write_error_report_log_for_user_failure(const char*, const char*) {}
static int fixture_message_box(HWND, const char*, const char*, UINT) { return IDOK; }
static bool capture_gui_apply_settings(DesiredSettings*, OcApplyBaseline*, char*, size_t) { ++legacyCaptures; return false; }
static bool gui_curve_prepare_intercept_apply(DesiredSettings*) { return false; }
static bool confirm_high_oc_apply(const DesiredSettings*, const OcApplyBaseline*, bool mappedSource) {
    ++highConfirms; mappedWarning = mappedSource; return highAllowed;
}
static bool confirm_unified_memory_write(const DesiredSettings*) {
    ++memoryConfirms;
    if (expireInConfirmation) gui_profile_source_input_changed(nullptr);
    return memoryAllowed;
}
static bool gui_mutation_queue_apply(const DesiredSettings* desired, bool interactive, ServiceApplyOrigin origin,
    ServiceProfileSource source, int slot, GuiMutationUiContext, const char*, char* error, size_t size, gc_u64 controls) {
    applied = {}; applied.magic = SERVICE_PROTOCOL_MAGIC; applied.version = SERVICE_PROTOCOL_VERSION;
    applied.command = SERVICE_CMD_APPLY; applied.callerPid = 1; applied.operationId = 2;
    applied.flags = interactive ? SERVICE_REQUEST_FLAG_INTERACTIVE : 0u; applied.applyOrigin = origin;
    applied.desired = *desired; applied.targetGpu = g_app.selectedGpu; applied.profileSource = source; applied.profileSlot = slot;
    applied.expectedControlFingerprint = controls; applied.expectedControlDomains = controls ? 511u : 0u;
    if (!gui_mutation_stamp_request(&applied, error, size) || service_request_reject_reason(&applied)) return false;
    ++applies; return true;
}
#define gc_message_box fixture_message_box
#include "ui_main_apply_dispatch.h"
#undef gc_message_box
static GuiServiceIoCompletion* completion(const ServiceResponse& response, gc_u64 token = 0) {
    auto* out = (GuiServiceIoCompletion*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(GuiServiceIoCompletion));
    if (out) {
        out->kind = GUI_SERVICE_IO_CAPTURE; out->transportSuccess = true; out->response = response;
        out->captureToken = token ? token : sentToken; out->presentationEpoch = epoch;
    }
    return out;
}
static int mapped_dispatch_tests() {
    auto live = std::make_unique<ServiceResponse>(); profile_review_fixture::live(*live);
    g_app.guiServiceModel.serviceInstanceId = live->state.serviceInstanceId;
    g_app.guiServiceModel.gpuGeneration = live->state.gpuGeneration;
    g_app.guiServiceModel.topologySignature = live->state.topologySignature;
    g_app.guiDraft.attached = false; g_app.guiDraft.detached = true; g_app.loadedSharedSlot = 3;
    g_app.guiGpuOffsetFromProfileLoad = g_app.guiMemOffsetFromProfileLoad = true;
    queue = selection = true;
    const auto prepare = [&] {
        gui_profile_source_observe_service(live.get()); gui_profile_source_check(nullptr, g_app.guiSource.value);
        profile_review_fixture::capture(*live, g_app.guiSource.value);
        gui_profile_source_handle_check(completion(*live)); gui_profile_source_accept_mapped(nullptr);
    };
    prepare();
    if (!gui_profile_source_mapped_ready()) return 9660;
    apply_changes();
    if (applies != 1 || applied.desired.memOffsetHz != -123500000 || applied.desired.memOffsetSurface != CLOCK_OFFSET_SURFACE_P0 ||
        applied.expectedControlFingerprint != live->capture.revision || applied.expectedControlDomains != 511u ||
        applied.profileSource != SERVICE_PROFILE_SOURCE_AD_HOC || applied.profileSlot || !mappedWarning ||
        legacyCaptures || highConfirms != 1 || memoryConfirms != 1 || g_app.serviceActiveDesiredValid ||
        g_app.guiSource.value.memoryOffsetHz != -123500000) return 9661;
    highAllowed = false; apply_changes();
    if (applies != 1 || memoryConfirms != 1) return 9662;
    highAllowed = true; memoryAllowed = false; apply_changes();
    if (applies != 1) return 9663;
    memoryAllowed = true; expireInConfirmation = true; apply_changes();
    if (applies != 1 || gui_profile_source_mapped_ready()) return 9664;
    expireInConfirmation = false; prepare(); selection = false; apply_changes();
    if (applies != 1) return 9665;
    selection = true; g_app.guiServiceModel.gpuGeneration = 0; apply_changes();
    if (applies != 1) return 9666;
    if (service_client_uses_loaded_shared_reference(SERVICE_PROFILE_SOURCE_AD_HOC, 3, false) ||
        !service_client_uses_loaded_shared_reference(SERVICE_PROFILE_SOURCE_SHARED_SLOT, 3, false) ||
        service_client_uses_loaded_shared_reference(SERVICE_PROFILE_SOURCE_SHARED_SLOT, 3, true)) return 9667;
    return 0;
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
    DesiredSettings mapped{}; gc_u64 controlRevision = 0; OcApplyBaseline baseline{};
    if (!g_profileSourceWindow->mapped || gui_profile_source_mapped_ready() ||
        gui_profile_source_prepare_apply(mapped, controlRevision, baseline)) return 9650;
    gui_profile_source_accept_mapped(nullptr);
    if (!gui_profile_source_mapped_ready() || g_profileSourceWindow->mapped ||
        !gui_profile_source_prepare_apply(mapped, controlRevision, baseline) || mapped.memOffsetHz != -123500000 ||
        !controlRevision || controlRevision != live->capture.revision || g_app.serviceActiveDesiredValid || sends != 1) return 9651;
    ++live->state.stateRevision; gui_profile_source_observe_service(live.get());
    if (!g_profileReview || g_profileSourceWindow->lines.empty()) return 9375;
    start(); const auto obsolete = sentToken;
    gui_profile_source_input_changed(nullptr);
    if (g_profileReviewJob || g_profileReview || g_profileReadPending || !g_profileSourceWindow->lines.empty()) return 9376;
    if (gui_profile_source_mapped_ready() || gui_profile_source_prepare_apply(mapped, controlRevision, baseline)) return 9652;
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
    gui_profile_source_check(nullptr, g_app.guiSource.value, 0, CLOCK_OFFSET_SURFACE_GLOBAL);
    if (!g_profileReviewJob || g_profileReviewJob->memory != CLOCK_OFFSET_SURFACE_GLOBAL ||
        g_app.guiSource.value.memorySurface != CLOCK_OFFSET_SURFACE_P0) return 9389;
    gui_profile_source_preview_changed(nullptr);
    if (g_profileReviewJob || g_profileReadPending) return 9390;
    if (const int failure = mapped_dispatch_tests()) return failure;

    // UNIT-07 & CAP-10 GUI review callbacks: batch conversion updates draft (keeps saved exact) and restore-tail works without recapture.
    g_app.guiSource = {}; g_app.guiSource.valid = true;
    g_app.guiSource.value = g_app.guiSource.saved = profile_review_fixture::source();
    g_app.guiSource.value.memoryOffsetHz = -500;
    g_app.guiSource.saved.memoryOffsetHz = -500;
    g_app.guiSource.revision = 50;
    gui_profile_source_observe_service(live.get());
    start();
    profile_review_fixture::capture(*live, g_app.guiSource.value);
    answer = completion(*live); if (!answer) return 9373;
    gui_profile_source_handle_check(answer);
    if (!g_profileSourceWindow->conversions.anyConvertible() || !g_profileReview ||
        g_profileSourceWindow->status.find("conversion") == std::string::npos) return 9394;
    const int reloadsBefore = g_profileSourceWindow->reloads;
    gui_profile_source_convert_domain(nullptr, PROFILE_CONVERT_MEMORY);
    if (g_app.guiSource.value.memoryOffsetHz != -500000 || g_app.guiSource.saved.memoryOffsetHz != -500 ||
        g_app.guiSource.revision != 51 || g_profileSourceWindow->reloads != reloadsBefore + 1 ||
        g_profileReview || g_profileSourceWindow->conversions.anyConvertible()) return 9395;
    auto flatSource = profile_document_make_empty();
    flatSource.domains = PROFILE_DOMAIN_VF_CURVE | PROFILE_DOMAIN_LOCK;
    flatSource.topologyCount = 2; flatSource.sourceTopologyComplete = true;
    flatSource.topology[0] = {10, 22, 662000, 1}; flatSource.topology[1] = {11, 23, 668250, 1};
    flatSource.points[0] = {PROFILE_POINT_CAPTURED_OFFSET, 0, 15000500, false, 0, true, 15000500};
    flatSource.points[1] = {PROFILE_POINT_ABSENT, 0, 0, false, 0, true, -25000500};
    flatSource.lock.present = true; flatSource.lock.mode = LOCK_MODE_FLATTEN;
    flatSource.lock.anchorTopologyId = 11; flatSource.lock.targetFrequencyHz = 1000000000;
    g_app.guiSource.value = g_app.guiSource.saved = flatSource; g_app.guiSource.revision = 60;
    const int sendsBeforeRestore = sends;
    gui_profile_source_restore_exact(nullptr);
    if (sends != sendsBeforeRestore || g_app.guiSource.value.lock.present || !g_app.guiSource.saved.lock.present ||
        g_app.guiSource.value.points[1].kind != PROFILE_POINT_CAPTURED_OFFSET ||
        g_app.guiSource.value.points[1].capturedOffsetHz != -25000500) return 9396;
    g_app.guiSource.value = flatSource; g_app.guiSource.value.points[1].hasObservedOffset = false;
    gui_profile_source_restore_exact(nullptr);
    if (!g_app.guiSource.value.lock.present || g_profileSourceWindow->status.find("missing") == std::string::npos) return 9397;
    g_profileSourceWindow.reset(); return 0;
}
}
int run_gui_profile_source_review_tests() { return gui_profile_source_review_fixture::run(); }
#else
int run_gui_profile_source_review_tests() { return 0; }
#endif
