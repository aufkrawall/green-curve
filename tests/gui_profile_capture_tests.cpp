// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#if defined(_WIN32)
#include "profile_review_fixture.h"
#include "app_shared.h"
#include "gui_mutation_queue_policy.h"
#include "service_request_deadline_policy.h"
#include "gui_service_io_types.h"
#include "gui_passive_capture.h"

namespace gui_profile_capture_fixture {
static AppData g_app{};
struct Window {
    bool active = false, inputPending = true, ready = false, needsDiscard = false;
    int reloads = 0;
    std::string input = "unfinished", status;
    std::vector<std::string> lines;
    gc_u32 omissions = 0;
    bool capture_active() const { return active; }
    bool pending() const { return inputPending; }
    void begin_capture(bool discard) { active = true; needsDiscard = discard; }
    void capture_reset() { ready = false; lines.clear(); }
    void capture_report(const std::vector<std::string>& text, bool valid, gc_u32 omitted) { lines = text; ready = valid; omissions = omitted; }
    void capture_require_discard() { needsDiscard = true; }
    void notice(const char* text) { status = text; }
    void show() {}
    void reload() { active = false; inputPending = false; input.clear(); ++reloads; }
};
static std::unique_ptr<Window> g_profileSourceWindow;
static ProfileEditorReviewContext g_profileShownContext{};
static gc_u64 g_profileReadToken = 0;
static LONG epoch = 1;
static int sends = 0, discards = 0, cancels = 0, refreshes = 0;
static bool selection = true, queue = true, open = true;
static GuiPassiveCaptureWork sent{};
static void debug_log(const char*, ...) {}
static void set_profile_status_text(const char*, ...) {}
static int fixture_message_box(HWND, const char*, const char*, UINT) { return IDOK; }
static LONG gui_service_presentation_epoch() { return epoch; }
static bool gui_profile_source_open() { return open; }
static bool gui_profile_transfer_active() { return false; }
static void gui_profile_source_refresh() { ++refreshes; }
static void gui_profile_source_invalidate_review(const char*) {}
static bool validate_configured_gpu_selection_for_client(char*, size_t) { return selection; }
static void gui_service_io_cancel_capture(gc_u64) { ++cancels; }
static void gui_draft_discard() { ++discards; g_app.guiStateDirty = false; g_app.guiHasUserModifiedValues = false; }
static bool gui_service_io_queue_capture(const GpuAdapterInfo& target, gc_u32 domains, gc_u64 token, char* error, size_t size) {
    if (!queue || !gui_passive_capture_prepare(target, domains, token, sent)) { snprintf(error, size, "fixture queue refusal"); return false; }
    sent.request.callerPid = 1;
    if (service_request_reject_reason(&sent.request)) return false;
    ++sends; return true;
}
#define gc_message_box fixture_message_box
#include "gui_profile_capture.cpp"
#undef gc_message_box
static GuiServiceIoCompletion* completion(const ServiceResponse& response, gc_u64 token = 0) {
    auto* out = (GuiServiceIoCompletion*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(GuiServiceIoCompletion));
    if (out) {
        out->kind = GUI_SERVICE_IO_CAPTURE; out->transportSuccess = true; out->response = response;
        out->captureToken = token ? token : sent.token; out->presentationEpoch = epoch;
    }
    return out;
}
static bool deliver(GuiServiceIoCompletion* answer) {
    if (gui_profile_capture_handle_completion(answer)) return true;
    HeapFree(GetProcessHeap(), 0, answer); return false;
}
static int run() {
    g_profileSourceWindow = std::make_unique<Window>();
    auto live = std::make_unique<ServiceResponse>(); profile_review_fixture::live(*live);
    live->state.activeDesiredValid = 1; live->desired.hasMemOffset = 1; live->desired.memOffsetMHz = 50;
    g_app.serviceActiveDesiredValid = true; g_app.serviceActiveDesired = live->desired;
    const auto ownership = service_control_review_revision(0, &g_app.serviceActiveDesired);
    g_profileShownContext = profile_editor_review_context(*live);
    g_app.guiServiceModel.phase = GUI_SERVICE_READY; g_app.guiServiceModel.hasAcceptedEnvelope = true;
    g_app.selectedGpu = live->snapshot.adapters[0];
    g_app.guiSource.valid = true; g_app.guiSource.value = g_app.guiSource.saved = profile_review_fixture::source();
    auto bank = std::make_unique<ProfileBank>(); bank->count = 1;
    const auto encoded = [&] { std::string out; bank->documents[0] = g_app.guiSource.value; (void)profile_bank_encode(*bank, out); return out; };
    const auto original = encoded(); if (original.empty()) return 9411;
    gui_profile_capture_begin();
    if (!gui_profile_capture_active() || !g_profileSourceWindow->needsDiscard || encoded() != original || discards) return 9412;
    profile_review_fixture::capture(*live, g_app.guiSource.value);
    live->capture.memory.value = -123500000;
    live->capture.fingerprint = hardware_capture_fingerprint(live->capture);
    live->capture.revision = service_control_review_revision(live->capture.fingerprint, &live->desired);
    const gc_u32 domains = live->capture.requestedDomains;
    gui_profile_capture_read(nullptr, domains);
    if (sends != 1 || !g_profileCapturePending || sent.request.targetGpu.pciBus != 6 ||
        sent.request.command != SERVICE_CMD_CAPTURE_CURRENT || encoded() != original || discards) return 9413;
    gui_profile_capture_read(nullptr, domains); if (sends != 1) return 9414;
    auto* answer = completion(*live); if (!answer) return 9415;
    if (!deliver(answer) || g_profileCapturePending || !g_profileCaptureObservation || !g_profileSourceWindow->ready ||
        g_profileSourceWindow->lines.empty() || !(g_profileSourceWindow->omissions & PROFILE_DOMAIN_LOCK) ||
        encoded() != original || g_profileSourceWindow->input != "unfinished") return 9416;
    if (gui_profile_capture_accept(nullptr, false, true) || gui_profile_capture_accept(nullptr, true, false) ||
        encoded() != original || discards || sends != 1) return 9417;
    if (!gui_profile_capture_accept(nullptr, true, true) || !g_app.guiSource.valid || !g_app.guiSource.unsaved ||
        !profile_editor_document_dirty(g_app.guiSource) || discards != 1 || sends != 1 || gui_profile_capture_active() ||
        g_profileCaptureObservation || g_profileCaptureJob || g_profileSourceWindow->reloads != 1) return 9418;
    const auto& captured = g_app.guiSource.value;
    if (captured.points[0].kind != PROFILE_POINT_CAPTURED_OFFSET || captured.points[0].capturedOffsetHz ||
        !captured.hasMemoryOffset || captured.memoryOffsetHz != -123500000 || captured.memorySurface != CLOCK_OFFSET_SURFACE_P0 ||
        !captured.graphicsOffset.present || captured.graphicsOffset.offsetHz ||
        !g_app.serviceActiveDesiredValid || service_control_review_revision(0, &g_app.serviceActiveDesired) != ownership) return 9419;
    const auto accepted = encoded();
    g_profileSourceWindow->input = "another unfinished value"; g_profileSourceWindow->inputPending = true;
    gui_profile_capture_begin(); gui_profile_capture_read(nullptr, domains); const auto obsolete = sent.token;
    g_profileSourceWindow->active = false; gui_profile_capture_cancel(nullptr);
    if (g_profileCaptureJob || g_profileCaptureObservation || g_profileCapturePending || cancels != 1 ||
        encoded() != accepted || g_profileSourceWindow->input != "another unfinished value" || discards != 1) return 9420;
    answer = completion(*live, obsolete); if (!answer) return 9415;
    if (deliver(answer) || encoded() != accepted) return 9421;
    gui_profile_capture_begin(); gui_profile_capture_read(nullptr, domains);
    ++g_profileShownContext.ownership; gui_profile_capture_service_changed();
    if (g_profileCaptureJob || g_profileCapturePending || g_profileSourceWindow->ready) return 9422;
    g_profileShownContext = profile_editor_review_context(*live);
    gui_profile_capture_read(nullptr, domains);
    answer = completion(*live); if (!answer) return 9415;
    ++answer->response.capture.fingerprint;
    if (!deliver(answer) || g_profileCaptureJob || g_profileCaptureObservation || encoded() != accepted) return 9423;
    gui_profile_capture_read(nullptr, domains); ++epoch; gui_profile_capture_service_changed();
    if (g_profileCaptureJob || g_profileCapturePending || encoded() != accepted) return 9424;
    gui_profile_capture_read(nullptr, domains);
    if (gui_profile_capture_allocation_failed(sent.token + 1) || !gui_profile_capture_allocation_failed(sent.token) ||
        g_profileCaptureJob || g_profileCapturePending) return 9425;
    queue = false; gui_profile_capture_read(nullptr, domains); queue = true;
    if (g_profileCaptureJob || g_profileCapturePending || g_profileSourceWindow->status != "fixture queue refusal") return 9426;
    const int count = sends; selection = false; gui_profile_capture_read(nullptr, domains); selection = true;
    g_app.guiServiceModel.phase = GUI_SERVICE_DISCONNECTED; gui_profile_capture_read(nullptr, domains);
    g_app.guiServiceModel.phase = GUI_SERVICE_READY;
    if (sends != count || g_profileCaptureJob || encoded() != accepted || discards != 1) return 9427;
    // A capture opened over the legacy live editor cannot discard its invalid
    // text on cancel. The canonical model stays absent throughout this flow.
    g_profileSourceWindow->active = false; gui_profile_capture_cancel(nullptr);
    g_app.guiSource.valid = false; g_app.guiStateDirty = true; g_profileSourceWindow->inputPending = false;
    snprintf(g_app.guiDraft.memOffsetText, sizeof(g_app.guiDraft.memOffsetText), "invalid legacy input");
    gui_profile_capture_begin();
    if (!g_profileSourceWindow->needsDiscard) return 9428;
    g_profileSourceWindow->active = false; gui_profile_capture_cancel(nullptr);
    if (g_app.guiSource.valid || !g_app.guiStateDirty || strcmp(g_app.guiDraft.memOffsetText, "invalid legacy input") || discards != 1) return 9429;
    HMENU menu = CreatePopupMenu(); if (!menu) return 9430;
    gui_profile_tools_append_menu(menu);
    const bool menuValid = GetMenuItemCount(menu) == 3 && !(GetMenuState(menu, PROFILE_CAPTURE_ID, MF_BYCOMMAND) & MF_GRAYED) &&
        !(GetMenuState(menu, PROFILE_TRANSFER_ID, MF_BYCOMMAND) & MF_GRAYED);
    DestroyMenu(menu); if (!menuValid) return 9431;
    g_app.guiServiceModel.phase = GUI_SERVICE_DISCONNECTED;
    menu = CreatePopupMenu(); if (!menu) return 9430;
    gui_profile_tools_append_menu(menu);
    const bool offlineMenu = (GetMenuState(menu, PROFILE_CAPTURE_ID, MF_BYCOMMAND) & MF_GRAYED) &&
        !(GetMenuState(menu, PROFILE_TRANSFER_ID, MF_BYCOMMAND) & MF_GRAYED);
    DestroyMenu(menu); if (!offlineMenu) return 9536;
    g_profileSourceWindow.reset(); return 0;
}
}
int run_gui_profile_capture_tests() { return gui_profile_capture_fixture::run(); }
#else
int run_gui_profile_capture_tests() { return 0; }
#endif
