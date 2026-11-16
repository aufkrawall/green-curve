// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#if defined(_WIN32)
#include "profile_review_fixture.h"
#include "app_shared.h"
#include "gui_mutation_queue_policy.h"
#include "service_request_deadline_policy.h"
#include "gui_service_io_types.h"
#include "gui_passive_capture.h"
#include "profile_capture_proposal_fixture.h"

namespace gui_profile_capture_fixture {
static AppData g_app{};
struct Window {
    bool active = false, inputPending = true, ready = false, needsDiscard = false;
    bool pinObservable = false, pinSuspected = false;
    gc_i64 pinProposedMHz = 0;
    int reloads = 0;
    std::string input = "unfinished", status;
    std::vector<std::string> lines;
    gc_u32 omissions = 0;
    unsigned int representations = 0;
    ProfileCaptureRepresentation choice = PROFILE_CAPTURE_EXACT;
    bool capture_active() const { return active; }
    bool pending() const { return inputPending; }
    void begin_capture(bool discard) { active = true; needsDiscard = discard; }
    void capture_reset() { ready = false; representations = 0; pinObservable = pinSuspected = false; pinProposedMHz = 0; lines.clear(); }
    void capture_pin_state(bool observable, bool suspected, gc_i64 mhz) {
        pinObservable = observable; pinSuspected = suspected; pinProposedMHz = mhz;
    }
    void capture_report(const std::vector<std::string>& text, bool valid, gc_u32 omitted,
        unsigned int available = profile_capture_representation_bit(PROFILE_CAPTURE_EXACT), ProfileCaptureRepresentation suggested = PROFILE_CAPTURE_EXACT) {
        lines = text; ready = valid; omissions = omitted; representations = available; choice = suggested;
    }
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
static int proposal_cases() {
    auto live = std::make_unique<ServiceResponse>(); profile_review_fixture::live(*live);
    g_app.guiServiceModel.phase = GUI_SERVICE_READY; g_app.guiServiceModel.hasAcceptedEnvelope = true;
    g_app.selectedGpu = live->snapshot.adapters[0]; g_app.guiStateDirty = false;
    g_app.guiSource = {}; g_app.guiSource.valid = true;
    g_app.guiSource.value = g_app.guiSource.saved = profile_review_fixture::source();
    g_app.serviceActiveDesired = {}; g_app.serviceActiveDesiredValid = false;
    g_profileSourceWindow->active = false; g_profileSourceWindow->inputPending = true;
    g_profileSourceWindow->input = "previous unfinished value";
    queue = selection = open = true;
    auto bank = std::make_unique<ProfileBank>(); bank->count = 1;
    const auto encoded = [&] { std::string text; bank->documents[0] = g_app.guiSource.value; (void)profile_bank_encode(*bank, text); return text; };
    const auto original = encoded(); const int readsBefore = sends, discardsBefore = discards;
    const auto read = [&] {
        g_profileShownContext = profile_editor_review_context(*live);
        live->capture = profile_capture_proposal_fixture::curve();
        live->capture.binding = hardware_capture_binding(live->snapshot.adapters[0]);
        live->capture.fingerprint = hardware_capture_fingerprint(live->capture);
        live->capture.revision = service_control_review_revision(live->capture.fingerprint,
            live->state.activeDesiredValid ? &live->desired : nullptr);
        gui_profile_capture_begin(); gui_profile_capture_read(nullptr, live->capture.requestedDomains);
        auto* answer = completion(*live); return answer && deliver(answer);
    };
    if (!read() || !g_profileCaptureProposals || !g_profileCaptureProposals->flatten.selectable() ||
        !(g_profileSourceWindow->representations & profile_capture_representation_bit(PROFILE_CAPTURE_FLATTEN)) ||
        (g_profileSourceWindow->representations & profile_capture_representation_bit(PROFILE_CAPTURE_PIN)) ||
        g_profileSourceWindow->choice != PROFILE_CAPTURE_EXACT) return 9725;
    if (gui_profile_capture_accept(nullptr, true, true, PROFILE_CAPTURE_PIN) || encoded() != original ||
        discards != discardsBefore || !g_profileCaptureProposals || g_profileSourceWindow->input != "previous unfinished value") return 9726;
    if (gui_profile_capture_accept(nullptr, false, true, PROFILE_CAPTURE_FLATTEN) ||
        gui_profile_capture_accept(nullptr, true, false, PROFILE_CAPTURE_FLATTEN) || encoded() != original) return 9727;
    if (!gui_profile_capture_accept(nullptr, true, true, PROFILE_CAPTURE_FLATTEN) ||
        g_app.guiSource.value.lock.mode != LOCK_MODE_FLATTEN || g_app.guiSource.value.lock.anchorTopologyId != 22 ||
        g_app.guiSource.value.points[1].kind != PROFILE_POINT_ABSENT || g_app.guiSource.value.points[1].observedOffsetHz != -100000500 ||
        g_profileCaptureProposals || g_profileCaptureObservation || sends != readsBefore + 1 || discards != discardsBefore + 1 ||
        g_app.serviceActiveDesiredValid) return 9728;
    live->state.activeDesiredValid = 1; live->desired.hasLock = 1; live->desired.lockMode = LOCK_MODE_HARD;
    live->desired.lockCi = 22; live->desired.lockMHz = 1000;
    g_app.serviceActiveDesiredValid = true; g_app.serviceActiveDesired = live->desired;
    const auto owned = service_control_review_revision(0, &g_app.serviceActiveDesired);
    if (!read() || !g_profileCaptureProposals->pin.selectable() ||
        !(g_profileSourceWindow->representations & profile_capture_representation_bit(PROFILE_CAPTURE_PIN)) ||
        g_profileSourceWindow->choice != PROFILE_CAPTURE_EXACT) return 9729;
    live->capture.points[1].offsetHz = -777; // The completed envelope is no longer the frontend's frozen proposal.
    if (!gui_profile_capture_accept(nullptr, true, true, PROFILE_CAPTURE_PIN) ||
        g_app.guiSource.value.lock.mode != LOCK_MODE_HARD || g_app.guiSource.value.lock.targetFrequencyHz != 1000000000 ||
        g_app.guiSource.value.lock.provenance != PROFILE_LOCK_PROVENANCE_APPLIED_INTENT ||
        g_app.guiSource.value.source.pinKnowledge != PROFILE_PIN_UNKNOWN ||
        g_app.guiSource.value.points[1].observedOffsetHz != -100000500 || sends != readsBefore + 2 ||
        discards != discardsBefore + 2 || service_control_review_revision(0, &g_app.serviceActiveDesired) != owned) return 9730;
    const auto accepted = encoded();
    live->snapshot.health.availableMutationDomains &= ~SERVICE_MUTATION_DOMAIN_LOCK;
    if (!read() || !g_profileCaptureProposals || g_profileCaptureProposals->pin.selectable() ||
        (g_profileSourceWindow->representations & profile_capture_representation_bit(PROFILE_CAPTURE_PIN)) ||
        gui_profile_capture_accept(nullptr, true, true, PROFILE_CAPTURE_PIN) || encoded() != accepted) return 9731;
    g_profileSourceWindow->active = false; gui_profile_capture_cancel(nullptr);
    if (g_profileCaptureProposals || g_profileCaptureObservation || g_profileCaptureJob || encoded() != accepted ||
        discards != discardsBefore + 2 || service_control_review_revision(0, &g_app.serviceActiveDesired) != owned) return 9732;

    // CAP-11 GUI capture observation: passive read leaves pin unconfirmed; idle keeps unknown;
    // confirmed observation preserves USER_CONFIRMED_OBSERVATION, edited preserves USER, never auto-selected.
    live->state.activeDesiredValid = 0; live->desired = {}; g_app.serviceActiveDesiredValid = false;
    g_app.serviceActiveDesired = {};
    live->snapshot.health.availableMutationDomains |= SERVICE_MUTATION_DOMAIN_LOCK;
    if (!read() || !g_profileSourceWindow->pinObservable || g_profileSourceWindow->pinSuspected ||
        (g_profileSourceWindow->representations & profile_capture_representation_bit(PROFILE_CAPTURE_PIN))) return 9930;
    static std::vector<PinWorkloadSample> nextGuiSamples;
    g_guiProfileCapturePinSampler = [](const HardwareCaptureBinding&, std::vector<PinWorkloadSample>& out, char*, size_t) {
        out = nextGuiSamples; return true;
    };
    PinWorkloadSample idleGui{}; idleGui.clockValid = true; idleGui.graphicsClockHz = 210000000LL;
    idleGui.utilValid = true; idleGui.utilGpuPct = 0; idleGui.throttleValid = true; idleGui.throttleReasons = PIN_OBS_REASON_GPU_IDLE;
    nextGuiSamples.assign(4, idleGui);
    gui_profile_capture_observe_pin(nullptr);
    if (g_profileSourceWindow->pinSuspected || g_profilePinObservation ||
        (g_profileSourceWindow->representations & profile_capture_representation_bit(PROFILE_CAPTURE_PIN))) return 9931;
    PinWorkloadSample activeGui{}; activeGui.clockValid = true; activeGui.graphicsClockHz = 900000000LL;
    activeGui.utilValid = true; activeGui.utilGpuPct = 92;
    nextGuiSamples.assign(5, activeGui);
    gui_profile_capture_observe_pin(nullptr);
    if (!g_profileSourceWindow->pinSuspected || g_profileSourceWindow->pinProposedMHz != 900 ||
        g_profileSourceWindow->choice != PROFILE_CAPTURE_EXACT ||
        (g_profileSourceWindow->representations & profile_capture_representation_bit(PROFILE_CAPTURE_PIN))) return 9932;
    gui_profile_capture_confirm_pin(nullptr, 900000000LL);
    if (!(g_profileSourceWindow->representations & profile_capture_representation_bit(PROFILE_CAPTURE_PIN)) ||
        g_profileSourceWindow->choice != PROFILE_CAPTURE_EXACT || !g_profileCaptureProposals->pin.selectable() ||
        g_profileCaptureProposals->pin.provenance != PROFILE_LOCK_PROVENANCE_USER_CONFIRMED_OBSERVATION) return 9933;
    gui_profile_capture_confirm_pin(nullptr, 950000000LL);
    if (g_profileCaptureProposals->pin.provenance != PROFILE_LOCK_PROVENANCE_USER ||
        g_profileCaptureProposals->pin.frequencyHz != 950000000LL) return 9934;
    gui_profile_capture_confirm_pin(nullptr, 900000000LL);
    if (!gui_profile_capture_accept(nullptr, true, true, PROFILE_CAPTURE_PIN) ||
        g_app.guiSource.value.lock.mode != LOCK_MODE_HARD || g_app.guiSource.value.lock.targetFrequencyHz != 900000000LL ||
        g_app.guiSource.value.lock.provenance != PROFILE_LOCK_PROVENANCE_USER_CONFIRMED_OBSERVATION) return 9935;
    g_guiProfileCapturePinSampler = nullptr;
    return 0;
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
    if (const int failure = proposal_cases()) return failure;
    g_profileSourceWindow.reset(); return 0;
}
}
int run_gui_profile_capture_tests() { return gui_profile_capture_fixture::run(); }
#else
int run_gui_profile_capture_tests() { return 0; }
#endif
