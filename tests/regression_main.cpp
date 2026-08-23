// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// Pure regression harness.  Extracted verbatim from the build.py string
// literal that used to hold it, so exit codes and behaviour are unchanged.
// Built and run by `python build.py --test`; it never touches GPU hardware.
//
// Exit code 0 = pass; any non-zero value identifies the exact assertion.
#include "fan_curve.h"
#include <string>
#include "lock_checkbox_policy.h"
#include "main_layout_policy.h"
#include "ui_theme_metrics.h"
#include "ui_checkbox_state.h"
#include "service_acl.h"
#include "vf_backends.h"
#include "driver_self_test_policy.h"
#include "nvapi_module_policy.h"
#include "service_lifecycle_policy.h"
#include "service_recovery_policy.h"
#include "service_ipc_throttle_policy.h"
#include "debug_log_rotation_policy.h"
#include "selected_gpu_pnp_policy.h"
#include "gpu_selection_policy.h"
#include "linux_gpu_selection.h"
#include "linux_mutation_authority.h"
#include "linux_gpu_binding_policy.h"
#include "linux_architecture_policy.h"
#include "linux_vf_validation.h"
#include "linux_tui_authority.h"
#include "linux_tui_edit_policy.h"
#include "linux_tui_diagnostic_policy.h"
#include "intent_readback_status.h"
#include "control_readback_policy.h"
#include "service_client_precondition_policy.h"
#include "linux_service_install_policy.h"
#include "linux_daemon_transport_policy.h"
#include "linux_systemd_notify_policy.h"
#include "linux_daemon_state.h"
#include "linux_transaction.h"
#include "linux_curve_targets.h"
#include "fan_runtime_policy.h"
#include "desired_settings_ui_policy.h"
#include "service_operation_tracker.h"
#include "gui_mutation_queue_policy.h"
#include "gui_service_io_queue_policy.h"
#include "gui_draft_policy.h"
#include "service_apply_severity_policy.h"
#include "profile_save_policy.h"
#include "profile_ownership_policy.h"
#include "gui_tray_callback_policy.h"
#include "applied_profile_indicator_policy.h"
#include "service_profile_identity_policy.h"
#include "gui_apply_in_flight_policy.h"
#include "gui_mutation_result_policy.h"
#include "gui_window_redraw_policy.h"
#include "gui_service_resync_policy.h"
#include "service_health_probe_policy.h"
#include "profile_persistence_policy.h"
#include "profile_startup_policy.h"
#include "startup_snapshot_policy.h"
#include "startup_task_definition_policy.h"
#include "oc_range_hint_policy.h"
#include "oc_high_warning_policy.h"
#include "message_box_policy.h"
#include "gui_pending_changes_policy.h"
#include "gui_graph_axis_policy.h"
#include "gui_service_actionability_policy.h"
// The setup program's pure policy: the payload container format and its bounds
// checks, the silent-mode command line, and the upgrade plan.  All three are
// platform-neutral, so they are compiled and asserted on both hosts.
#include "installer_archive_policy.h"
#include "installer_cli_policy.h"
#include "installer_plan_policy.h"
#include "installer_uninstall_policy.h"
#include "installer_ui_click_policy.h"
// The in-app updater's pure policy: version ordering (which is what refuses a
// downgrade), the signed manifest grammar and its cross-field bindings, the
// URL/redirect allowlist, and the check/install schedule.  Every one of these
// is a gate on running a downloaded executable as SYSTEM, so they are asserted
// on both hosts rather than only where they ship.
#include "update_version_policy.h"
#include "update_manifest_policy.h"
#include "update_url_policy.h"
#include "update_schedule_policy.h"
#include "update_install_policy.h"
#include "update_restore_policy.h"
// How an available update reaches the user passively.  Pure, and asserted here
// rather than only in the window, because the failure mode it exists to fix is
// silence -- which no build error, no crash and no log line reveals.
#include "update_presentation_policy.h"
#include "update_channel_policy.h"
#include "machine_dir_policy.h"
// The response read loops, behind their transport seam.  The reason this is
// worth a seam at all is that its central property -- refusing an oversized
// body BEFORE the chunk is written -- regresses without any visible symptom.
#include "update_transport_policy.h"
#include "linux_terminal_policy.h"
#include "linux_debug_log.h"
// Where a crash artifact is allowed to go and which ones may be deleted.  Pure
// and platform-neutral on purpose: both rules are security-relevant and neither
// is observable after the fact, so they are asserted here rather than only in
// the Windows/Linux code that consumes them.
#include "crash_artifact_policy.h"
#include "linux_tui_layout.cpp"
#include "linux_tui_layout_vf.cpp"
#include "linux_tui_layout_fan_profiles.cpp"

// This harness builds and runs on BOTH hosts.  Everything platform-neutral —
// fan-curve math, every layout/lock/lifecycle/auto-profile policy, and the
// whole linux_* family — is compiled unconditionally, so a Linux developer
// gets real coverage instead of none.  Only suites that exercise Win32
// *implementation* code (config-INI storage and its named mutex, service/
// directory DACLs, protected temp dirs, the startup-task XML shard) are
// Windows-gated below; there is no Linux equivalent for them to assert
// against, and the Linux counterparts are covered by the native
// linux_transport_regression fixture plus the linux_* policy suites here.
#if defined(_WIN32)
// The task classifier lives in an amalgamated Windows shard.  Supply only the
// surrounding declarations needed to compile that shard into this fixture;
// executable tests call the pure XML classifier and never query Task Scheduler.
static char g_userDataDir[MAX_PATH] = {};
static WCHAR g_forcedStartupUserSam[512] = {};
bool utf8_to_wide(const char*, WCHAR*, int);
static bool get_current_user_sam_name(WCHAR*, DWORD) { return false; }
#include "main_startup_task_definition.cpp"
#endif // _WIN32

bool is_curve_point_visible_in_gui(int) { return true; }
void debug_log(const char*, ...) {}
// The XBAR transaction/layout header is included after the harness's host-side
// NvAPI seam declarations, so fake get/set/measure functions can pin readback.
typedef int (*NvApiFunc)(void*, void*);
#include "gpu_backend_xbar.h"
#include "log_redaction_policy.h"
#include "update_worker_recovery_policy.h"

#if defined(_WIN32)
// The real CNG verifier, compiled straight into the harness so the
// known-answer test below exercises the shipping code rather than a copy.
// Windows-only because BCrypt is; the pure policy above is asserted on
// both hosts.
#include <bcrypt.h>
#include "update_verify_keys.h"
#include "main_service_update_verify.cpp"
#endif
#if defined(_WIN32)
// Fixture stand-in for the amalgamation's helper (main_gpu_front.cpp), which
// resolves the excluded-low set from LIVE VF topology (g_app.populatedOrdinal).
// This pure harness has no GPU to populate that from, so it models the
// no-exclusions case: every point carries the whole offset. The legacy-format
// cases below all use gpuOffsetMHz == 0 or no exclusion count, where the two
// agree exactly.
static int gpu_offset_component_mhz_for_point(int, int gpuOffsetMHz, int) {
    return gpuOffsetMHz;
}
#include "config_profile_repair.cpp"
#endif

void invalidate_tray_profile_cache() {}

#if defined(_WIN32)
// Production auto-profile persistence uses the amalgamated atomic whole-file
// section writer.  The pure fixture starts with an empty temporary INI, so this
// deterministic stand-in only needs to commit the supplied complete sections.
static bool write_config_sections_atomic(const char* path,
    const char* newSectionsData, const char* const*, int,
    char* err, size_t errSize) {
    HANDLE file = gc_CreateFileUtf8(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        set_message(err, errSize, "fixture CreateFile failed");
        return false;
    }
    DWORD size = (DWORD)strlen(newSectionsData);
    DWORD written = 0;
    bool ok = WriteFile(file, newSectionsData, size, &written, nullptr) &&
        written == size && FlushFileBuffers(file) != FALSE;
    CloseHandle(file);
    return ok;
}
#endif // _WIN32

// Auto-profile pure core (resolver + controller state machine).  Included
// directly so the coalescing/hysteresis/manual-pin logic and the rule resolver
// are exercised without a GPU or an interactive desktop.
#include "auto_profile_rules.cpp"
#include "auto_profile_controller.cpp"
#include "hotkeys.cpp"

static ServiceResponse fake_ready_service_response(gc_u64 instance,
    gc_u64 revision, gc_u64 generation) {
    ServiceResponse response = {};
    response.magic = SERVICE_PROTOCOL_MAGIC;
    response.version = SERVICE_PROTOCOL_VERSION;
    response.status = SERVICE_STATUS_OK;
    response.servicePid = 4242;
    response.state.serviceInstanceId = instance;
    response.state.stateRevision = revision;
    response.state.gpuGeneration = generation;
    response.state.gpuPhase = SERVICE_GPU_PHASE_READY;
    response.state.validSections = SERVICE_STATE_SECTION_READY_REQUIRED |
        SERVICE_STATE_SECTION_FAN_TELEMETRY;
    response.snapshot.initialized = 1;
    response.snapshot.loaded = 1;
    response.snapshot.adapterCount = 1;
    response.snapshot.selectedAdapterIndex = 0;
    response.snapshot.adapters[0].valid = 1;
    response.snapshot.adapters[0].pciInfoValid = 1;
    response.snapshot.adapters[0].nvapiIndex = 0;
    response.snapshot.adapters[0].deviceId = 0x268410DEu;
    response.snapshot.adapters[0].subSystemId = 0x17AA3A5Cu;
    response.snapshot.adapters[0].pciRevisionId = 0xA1u;
    response.snapshot.adapters[0].extDeviceId = 0x12345678u;
    response.snapshot.adapters[0].pciBus = 9;
    response.snapshot.adapters[0].pciDevice = 3;
    response.snapshot.curve[7].freq_kHz = 1500000;
    response.snapshot.curve[7].volt_uV = 800000;
    response.snapshot.curve[11].freq_kHz = 1800000;
    response.snapshot.curve[11].volt_uV = 900000;
    response.snapshot.numPopulated = 2;
    response.snapshot.health.reason = SERVICE_GPU_HEALTH_NONE;
    response.snapshot.health.architectureSource =
        SERVICE_GPU_ARCH_SOURCE_NVAPI;
    response.snapshot.health.availableMutationDomains =
        SERVICE_MUTATION_DOMAIN_ALL;
    response.snapshot.health.vfSnapshotFresh = 1;
    response.snapshot.powerLimitPct = 100;
    response.snapshot.activeFanMode = FAN_MODE_AUTO;
    response.controlState.valid = 1;
    response.controlState.hasGpuOffset = 1;
    response.controlState.hasMemOffset = 1;
    response.controlState.hasPowerLimit = 1;
    response.controlState.powerLimitPct = 100;
    response.controlState.hasFan = 1;
    response.controlState.fanMode = FAN_MODE_AUTO;
    response.state.topologySignature =
        service_snapshot_topology_signature(&response.snapshot);
    return response;
}

struct FakeServiceActivationContext {
    LinuxServiceActivationStep seen[LINUX_SERVICE_STEP_COUNT];
    unsigned int count;
    int failAt;
};

static bool fake_service_activation_runner(
    void* opaque, LinuxServiceActivationStep step,
    char* error, size_t errorSize) {
    FakeServiceActivationContext* context =
        (FakeServiceActivationContext*)opaque;
    if (!context || context->count >= LINUX_SERVICE_STEP_COUNT) return false;
    context->seen[context->count++] = step;
    if ((int)step == context->failAt) {
        if (error && errorSize) {
            const char* detail = "injected activation failure";
            size_t index = 0;
            for (; index + 1 < errorSize && detail[index]; ++index)
                error[index] = detail[index];
            error[index] = 0;
        }
        return false;
    }
    return true;
}

#if defined(_WIN32)
static unsigned int g_nativeLockClickedCount = 0;
static unsigned int g_nativeLockDoubleClickedCount = 0;
static bool g_nativeTrayHiddenIntent = false;
static bool g_nativeTrayHideEnforcementActive = false;
static unsigned int g_nativeTrayHideEnforcementCount = 0;

static LRESULT CALLBACK native_lock_test_parent_proc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    (void)lParam;
    if (msg == WM_COMMAND && LOWORD(wParam) == 7001) {
        if (HIWORD(wParam) == BN_CLICKED) g_nativeLockClickedCount++;
        if (HIWORD(wParam) == BN_DBLCLK) g_nativeLockDoubleClickedCount++;
        return 0;
    }
    if (msg == WM_DRAWITEM) return TRUE;
    return DefWindowProcA(hwnd, msg, wParam, lParam);
}

static bool run_native_ownerdraw_button_notification_test() {
    HINSTANCE instance = GetModuleHandleA(nullptr);
    const char* className = "GreenCurveLockPolicyRegressionWindow";
    WNDCLASSA wc = {};
    wc.lpfnWndProc = native_lock_test_parent_proc;
    wc.hInstance = instance;
    wc.lpszClassName = className;
    ATOM atom = RegisterClassA(&wc);
    if (!atom && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;
    HWND parent = CreateWindowExA(0, className, "", WS_OVERLAPPED,
                                  0, 0, 100, 100, nullptr, nullptr, instance, nullptr);
    if (!parent) return false;
    HWND button = CreateWindowExA(0, "BUTTON", "",
                                  WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                                  0, 0, 30, 20, parent, (HMENU)(INT_PTR)7001, instance, nullptr);
    if (!button) {
        DestroyWindow(parent);
        return false;
    }
    g_nativeLockClickedCount = 0;
    g_nativeLockDoubleClickedCount = 0;
    SendMessageA(button, BM_CLICK, 0, 0);
    SendMessageA(button, WM_LBUTTONDBLCLK, MK_LBUTTON, MAKELPARAM(5, 5));
    bool ok = g_nativeLockClickedCount == 1 && g_nativeLockDoubleClickedCount == 1;
    // The installer's fast-click regression: a real double-click on a
    // BS_OWNERDRAW button is DOWN, UP, DBLCLK, UP, and the second click is
    // delivered as BN_DBLCLK -- NOT as a second BN_CLICKED, and the trailing
    // release adds nothing.  A handler that filters to BN_CLICKED alone
    // swallows the second click of every fast double-click.
    if (ok) {
        g_nativeLockClickedCount = 0;
        g_nativeLockDoubleClickedCount = 0;
        SendMessageA(button, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(5, 5));
        SendMessageA(button, WM_LBUTTONUP, 0, MAKELPARAM(5, 5));
        SendMessageA(button, WM_LBUTTONDBLCLK, MK_LBUTTON, MAKELPARAM(5, 5));
        SendMessageA(button, WM_LBUTTONUP, 0, MAKELPARAM(5, 5));
        ok = g_nativeLockClickedCount == 1 &&
             g_nativeLockDoubleClickedCount == 1;
    }
    DestroyWindow(parent);
    UnregisterClassA(className, instance);
    return ok;
}

static LRESULT CALLBACK native_reconnect_test_parent_proc(
    HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (gui_tray_hidden_intent_requires_rehide(
            g_nativeTrayHiddenIntent,
            IsWindowVisible(hwnd) != FALSE,
            g_nativeTrayHideEnforcementActive)) {
        g_nativeTrayHideEnforcementActive = true;
        g_nativeTrayHideEnforcementCount++;
        ShowWindow(hwnd, SW_HIDE);
        g_nativeTrayHideEnforcementActive = false;
    }
    if (msg == WM_WINDOWPOSCHANGING && lParam) {
        WINDOWPOS* position = reinterpret_cast<WINDOWPOS*>(lParam);
        bool showRequested = (position->flags & SWP_SHOWWINDOW) != 0;
        if (gui_tray_hidden_intent_blocks_show(
                g_nativeTrayHiddenIntent, showRequested)) {
            position->flags &= ~SWP_SHOWWINDOW;
            position->flags |= SWP_HIDEWINDOW | SWP_NOACTIVATE;
        }
    }
    return DefWindowProcA(hwnd, msg, wParam, lParam);
}

static bool run_native_reconnect_projection_and_dib_test() {
    HINSTANCE instance = GetModuleHandleA(nullptr);
    const char* className = "GreenCurveReconnectRegressionWindow";
    WNDCLASSA wc = {};
    wc.lpfnWndProc = native_reconnect_test_parent_proc;
    wc.hInstance = instance;
    wc.lpszClassName = className;
    ATOM atom = RegisterClassA(&wc);
    if (!atom && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;
    HWND parent = CreateWindowExA(0, className, "", WS_OVERLAPPED,
        0, 0, 160, 80, nullptr, nullptr, instance, nullptr);
    HWND edit = parent ? CreateWindowExA(0, "EDIT", "0", WS_CHILD,
        0, 0, 80, 20, parent, nullptr, instance, nullptr) : nullptr;
    HWND apply = parent ? CreateWindowExA(0, "BUTTON", "Apply", WS_CHILD,
        0, 24, 80, 20, parent, nullptr, instance, nullptr) : nullptr;
    if (!parent || !edit || !apply) {
        if (parent) DestroyWindow(parent);
        UnregisterClassA(className, instance);
        return false;
    }

    g_nativeTrayHiddenIntent = true;
    g_nativeTrayHideEnforcementActive = false;
    g_nativeTrayHideEnforcementCount = 0;
    ShowWindow(parent, SW_SHOW);
    bool ok = IsWindowVisible(parent) == FALSE;

    // Simulate a display-stack visibility reconstruction which bypasses the
    // SWP_SHOWWINDOW precondition.  The next window message must enforce the
    // durable postcondition without recursion or a user activation.
    g_nativeTrayHideEnforcementCount = 0;
    LONG_PTR hiddenStyle = GetWindowLongPtrA(parent, GWL_STYLE);
    SetWindowLongPtrA(parent, GWL_STYLE, hiddenStyle | WS_VISIBLE);
    SendMessageA(parent, WM_NULL, 0, 0);
    ok = ok && g_nativeTrayHideEnforcementCount > 0 &&
        IsWindowVisible(parent) == FALSE;

    g_nativeTrayHiddenIntent = false;
    ShowWindow(parent, SW_SHOW);
    ok = ok && IsWindowVisible(parent) != FALSE;
    ShowWindow(parent, SW_HIDE);

    // DefWindowProc's top-level WM_SETREDRAW implementation adds WS_VISIBLE
    // when redraw is enabled. This is the exact tray-profile ghost-window
    // mechanism: a raw redraw pair resurrects a hidden owner before WM_PAINT.
    g_nativeTrayHiddenIntent = true;
    SendMessageA(parent, WM_SETREDRAW, FALSE, 0);
    SendMessageA(parent, WM_SETREDRAW, TRUE, 0);
    ok = ok && IsWindowVisible(parent) != FALSE;
    SendMessageA(parent, WM_NULL, 0, 0);
    ok = ok && IsWindowVisible(parent) == FALSE;

    GuiServiceModel model = {};
    gui_service_model_initialize(&model);
    ServiceResponse ready = fake_ready_service_response(51, 1, 1);
    ok = ok && gui_service_model_accept(&model, 1, &ready.state) ==
        GUI_SERVICE_ENVELOPE_ACCEPTED;
    EnableWindow(apply, gui_service_phase_actions_enabled(model.phase));

    ServiceStateEnvelope missing = ready.state;
    missing.stateRevision = 2;
    missing.gpuGeneration = 2;
    missing.gpuPhase = SERVICE_GPU_PHASE_DEVICE_MISSING;
    missing.validSections = SERVICE_STATE_SECTION_ADAPTER_IDENTITY |
        SERVICE_STATE_SECTION_ACTIVE_INTENT;
    missing.topologySignature = 0;
    ok = ok && gui_service_model_accept(&model, 1, &missing) ==
        GUI_SERVICE_ENVELOPE_ACCEPTED;
    EnableWindow(apply, gui_service_phase_actions_enabled(model.phase));
    char preserved[16] = {};
    GetWindowTextA(edit, preserved, sizeof(preserved));
    ok = ok && !IsWindowEnabled(apply) && strcmp(preserved, "0") == 0;

    ServiceResponse recovered = fake_ready_service_response(51, 3, 2);
    ok = ok && gui_service_model_accept(&model, 1, &recovered.state) ==
        GUI_SERVICE_ENVELOPE_ACCEPTED;
    // The production transaction no longer redraw-toggles the owner at all --
    // neither direction of that pair is visibility-neutral on a top-level
    // window -- so the projection is just the control updates plus one settled
    // repaint, deferred here because the owner is tray-hidden.
    bool projectionInitiallyVisible = IsWindowVisible(parent) != FALSE;
    ok = ok && !gui_redraw_toggle_is_visibility_safe(
        (unsigned int)GetWindowLongPtrA(parent, GWL_STYLE));
    SetWindowTextA(edit, "125");
    EnableWindow(apply, gui_service_phase_actions_enabled(model.phase));
    RedrawWindow(parent, nullptr, nullptr,
        gui_top_level_redraw_may_paint_synchronously(
            projectionInitiallyVisible, g_nativeTrayHiddenIntent)
            ? (RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_UPDATENOW)
            : (RDW_INVALIDATE | RDW_ALLCHILDREN));
    GetWindowTextA(edit, preserved, sizeof(preserved));
    ok = ok && IsWindowEnabled(apply) && strcmp(preserved, "125") == 0 &&
        IsWindowVisible(parent) == FALSE;

    BITMAPINFO info = {};
    info.bmiHeader.biSize = sizeof(info.bmiHeader);
    info.bmiHeader.biWidth = 8;
    info.bmiHeader.biHeight = -8;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void* sourcePixels = nullptr;
    void* targetPixels = nullptr;
    HBITMAP sourceBitmap = CreateDIBSection(nullptr, &info, DIB_RGB_COLORS,
        &sourcePixels, nullptr, 0);
    HBITMAP targetBitmap = CreateDIBSection(nullptr, &info, DIB_RGB_COLORS,
        &targetPixels, nullptr, 0);
    HDC sourceDc = CreateCompatibleDC(nullptr);
    HDC targetDc = CreateCompatibleDC(nullptr);
    HGDIOBJ oldSource = sourceDc && sourceBitmap
        ? SelectObject(sourceDc, sourceBitmap) : nullptr;
    HGDIOBJ oldTarget = targetDc && targetBitmap
        ? SelectObject(targetDc, targetBitmap) : nullptr;
    if (!sourcePixels || !targetPixels || !oldSource || !oldTarget) {
        ok = false;
    } else {
        ((gc_u32*)sourcePixels)[0] = 0x00112233u;
        ok = ok && BitBlt(targetDc, 0, 0, 8, 8, sourceDc, 0, 0, SRCCOPY) &&
            ((gc_u32*)targetPixels)[0] == 0x00112233u;
    }
    if (oldSource) SelectObject(sourceDc, oldSource);
    if (oldTarget) SelectObject(targetDc, oldTarget);
    if (sourceDc) DeleteDC(sourceDc);
    if (targetDc) DeleteDC(targetDc);
    if (sourceBitmap) DeleteObject(sourceBitmap);
    if (targetBitmap) DeleteObject(targetBitmap);
    DestroyWindow(parent);
    UnregisterClassA(className, instance);
    return ok;
}

// The reported Refresh flicker: an OPEN window dropped out of the taskbar
// window list for the length of every structural projection.  Cause and fix are
// both about one bit.  `DefWindowProc` implements WM_SETREDRAW by clearing and
// restoring WS_VISIBLE, and the shell tracks top-level windows by exactly that
// bit, so the old suppression pair read as hide-then-show.  A headless fixture
// cannot query the taskbar, but it can assert the input the shell reads: the
// pair really does clear the bit on a window that stays on screen, and the
// replacement projection leaves it set from beginning to end while still
// landing every control update.
static bool run_native_visible_projection_taskbar_presence_test() {
    HINSTANCE instance = GetModuleHandleA(nullptr);
    const char* className = "GreenCurveVisibleProjectionRegressionWindow";
    WNDCLASSA wc = {};
    wc.lpfnWndProc = DefWindowProcA;
    wc.hInstance = instance;
    wc.lpszClassName = className;
    ATOM atom = RegisterClassA(&wc);
    if (!atom && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;
    // Off-screen: this is a genuinely visible top-level window, and the fixture
    // should not flash one across the developer's desktop to prove it.
    HWND parent = CreateWindowExA(0, className, "", WS_OVERLAPPEDWINDOW,
        -32000, -32000, 200, 120, nullptr, nullptr, instance, nullptr);
    HWND edit = parent ? CreateWindowExA(0, "EDIT", "0",
        WS_CHILD | WS_VISIBLE, 0, 0, 80, 20, parent, nullptr, instance,
        nullptr) : nullptr;
    if (!parent || !edit) {
        if (parent) DestroyWindow(parent);
        UnregisterClassA(className, instance);
        return false;
    }
    ShowWindow(parent, SW_SHOWNOACTIVATE);
    bool ok = IsWindowVisible(parent) != FALSE;

    // Only a child's WS_VISIBLE is private to the process.
    ok = ok && !gui_redraw_toggle_is_visibility_safe(
        (unsigned int)GetWindowLongPtrA(parent, GWL_STYLE));
    ok = ok && gui_redraw_toggle_is_visibility_safe(
        (unsigned int)GetWindowLongPtrA(edit, GWL_STYLE));

    // The mechanism that removed the taskbar button, on a visible window.
    SendMessageA(parent, WM_SETREDRAW, FALSE, 0);
    bool toggleClearedVisibleStyle =
        (GetWindowLongPtrA(parent, GWL_STYLE) & WS_VISIBLE) == 0;
    SendMessageA(parent, WM_SETREDRAW, TRUE, 0);
    ok = ok && toggleClearedVisibleStyle &&
        (GetWindowLongPtrA(parent, GWL_STYLE) & WS_VISIBLE) != 0;

    // The replacement: suppression is internal, so a full structural
    // projection never changes anything the shell can observe.  While it runs,
    // invalidation must stay deferred; only the settled repaint paints.
    bool beganVisible = IsWindowVisible(parent) != FALSE;
    ok = ok && gui_top_level_redraw_may_paint_synchronously(beganVisible, false);
    ok = ok && gui_window_invalidation_must_defer(true, false, beganVisible);
    ok = ok && !gui_window_invalidation_must_defer(false, false, beganVisible);
    SetWindowTextA(edit, "125");
    EnableWindow(edit, FALSE);
    EnableWindow(edit, TRUE);
    bool visibleThroughout =
        (GetWindowLongPtrA(parent, GWL_STYLE) & WS_VISIBLE) != 0;
    RedrawWindow(parent, nullptr, nullptr,
        RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_UPDATENOW);
    char projected[16] = {};
    GetWindowTextA(edit, projected, sizeof(projected));
    ok = ok && visibleThroughout && strcmp(projected, "125") == 0 &&
        (GetWindowLongPtrA(parent, GWL_STYLE) & WS_VISIBLE) != 0 &&
        IsWindowVisible(parent) != FALSE;

    DestroyWindow(parent);
    UnregisterClassA(className, instance);
    return ok;
}
#endif

struct FakeLinuxTransaction {
    unsigned int failPhase;
    unsigned int calls[7];
    unsigned int callCount;
    unsigned int rollbackMask;
    bool rollbackOk;
};

static bool fake_linux_transaction_step(void* opaque, unsigned int phase) {
    FakeLinuxTransaction* fake = (FakeLinuxTransaction*)opaque;
    if (fake->callCount < 7) fake->calls[fake->callCount++] = phase;
    return phase != fake->failPhase;
}

static bool fake_linux_transaction_rollback(void* opaque, unsigned int attempted) {
    FakeLinuxTransaction* fake = (FakeLinuxTransaction*)opaque;
    fake->rollbackMask = attempted;
    return fake->rollbackOk;
}

// A scripted HTTP response body, standing in for WinHTTP.
//
// It models the one thing about the real transport that the loops actually
// depend on: the body arrives in server-chosen chunks, announced by
// `available` and then handed over by `read`.  The two are separate calls, and
// a hostile server controls how much it announces -- which is why the loops may
// never treat an announcement as a bound on anything but their own buffer.
//
// `failAvailableAt` / `failReadAt` inject a transport failure at a chosen call,
// and `shortReadAt` makes one read return zero bytes with data still pending --
// the "stalled" case, distinct from the zero-available that ends a body.
struct FakeHttpBody {
    const unsigned char* bytes;
    size_t length;
    size_t offset;
    size_t announce;        // bytes reported per `available` call (0 = all)
    int availableCalls;
    int readCalls;
    int failAvailableAt;    // 1-based call index, 0 = never
    int failReadAt;
    int shortReadAt;
};

static bool fake_http_available(void* ctx, size_t* bytes) {
    FakeHttpBody* body = (FakeHttpBody*)ctx;
    body->availableCalls++;
    if (body->failAvailableAt == body->availableCalls) return false;
    size_t remaining = body->length - body->offset;
    size_t announce = body->announce ? body->announce : remaining;
    if (announce > remaining) announce = remaining;
    if (bytes) *bytes = announce;
    return true;
}

static bool fake_http_read(void* ctx, void* buffer, size_t want, size_t* got) {
    FakeHttpBody* body = (FakeHttpBody*)ctx;
    body->readCalls++;
    if (body->failReadAt == body->readCalls) return false;
    if (body->shortReadAt == body->readCalls) {
        if (got) *got = 0;
        return true;
    }
    size_t remaining = body->length - body->offset;
    size_t give = want < remaining ? want : remaining;
    if (give) memcpy(buffer, body->bytes + body->offset, give);
    body->offset += give;
    if (got) *got = give;
    return true;
}

// Counts what actually reached disk.  `written` is the measurement that proves
// the oversize abort happened mid-transfer rather than after: a loop that
// checked the total at the end would leave every offered byte here.
struct FakeSink {
    size_t written;
    int writeCalls;
    int failWriteAt;
};

static bool fake_sink_write(void* ctx, const void* buffer, size_t bytes) {
    FakeSink* sink = (FakeSink*)ctx;
    (void)buffer;
    sink->writeCalls++;
    if (sink->failWriteAt == sink->writeCalls) return false;
    sink->written += bytes;
    return true;
}

#if defined(_WIN32)
// DACL fixtures used to name their scratch directory by PID alone and delete it
// with a bare RemoveDirectoryW.  A protected DACL denies the creator delete
// rights, so cleanup silently failed and the directory leaked; when Windows
// later reused that PID the fixture failed at CreateDirectoryW for no real
// reason.  Create a genuinely unique directory, and tear it down by first
// restoring inheritance with the same helper production uninstall uses.
static bool gc_make_unique_temp_dir(const wchar_t* prefix, wchar_t* out,
                                    size_t outCount) {
    if (!prefix || !out || outCount == 0) return false;
    wchar_t tempDir[MAX_PATH] = {};
    if (GetTempPathW(MAX_PATH, tempDir) == 0) return false;
    for (unsigned int attempt = 0; attempt < 64; ++attempt) {
        LARGE_INTEGER counter = {};
        QueryPerformanceCounter(&counter);
        if (FAILED(StringCchPrintfW(out, outCount, L"%ls%ls_%lu_%llx_%u",
                                    tempDir, prefix,
                                    (unsigned long)GetCurrentProcessId(),
                                    (unsigned long long)counter.QuadPart,
                                    attempt)))
            return false;
        if (CreateDirectoryW(out, nullptr)) return true;
        if (GetLastError() != ERROR_ALREADY_EXISTS) return false;
    }
    return false;
}

static bool gc_remove_protected_temp_dir(const wchar_t* path) {
    if (!path || !path[0]) return false;
    char restoreErr[256] = {};
    restore_inherited_dacl(path, restoreErr, sizeof(restoreErr));
    return RemoveDirectoryW(path) != FALSE;
}
#endif

// Assertion codes run into the thousands, but a POSIX exit status is only the
// low 8 bits: 1670 would surface as 134, which is itself a real assertion
// number.  Report the true code on stderr and map any failure to a single
// non-aliasing status so a Linux run is never misread.
static int run_all_tests(int argc, char** argv);

int main(int argc, char** argv) {
    int code = run_all_tests(argc, argv);
    if (code != 0) {
        fprintf(stderr, "regression assertion failed: code %d\n", code);
#if !defined(_WIN32)
        return code > 0 && code < 126 ? code : 1;
#endif
    }
    return code;
}

static int run_all_tests(int argc, char** argv) {
    if (argc == 2 && strcmp(argv[1], "--capture-success") == 0) {
        for (int i = 0; i < 4096; ++i) fputs("capture-data-", stdout);
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "--capture-failure") == 0) {
        fputs("expected child failure", stderr);
        return 7;
    }

    InitializeCriticalSection(&g_configLock);

    if (APP_DEBUG_DEFAULT_ENABLED != 1) return 20;

    // Bounded append cursors count bytes rather than exposing StringCch's
    // HRESULT convention, retain prefixes, and saturate safely on truncation.
    {
        char combined[16] = {};
        size_t used = gc_appendf(combined, sizeof(combined), 0, "GPU");
        used = gc_appendf(combined, sizeof(combined), used, " %d", 42);
        if (used != 6 || strcmp(combined, "GPU 42") != 0) return 1140;
        used = gc_appendf(combined, sizeof(combined), used,
            " verification text is deliberately too long");
        if (used != sizeof(combined) - 1 || combined[used] != 0 ||
            strncmp(combined, "GPU 42", 6) != 0) return 1141;
        if (gc_appendf(combined, sizeof(combined), used, "ignored") != used ||
            combined[used] != 0) return 1142;
    }

    // Subprocess capture succeeds only for exit code zero and must continue
    // draining output after the caller's bounded capture buffer fills.
    {
        const char* successArgs[] = { argv[0], "--capture-success", nullptr };
        char captured[16] = {};
        if (!pl_run_capture(successArgs, captured, sizeof(captured), 5000) ||
            strncmp(captured, "capture-data-", 13) != 0) return 1143;
        const char* failureArgs[] = { argv[0], "--capture-failure", nullptr };
        if (pl_run_capture(failureArgs, captured, sizeof(captured), 5000) ||
            strncmp(captured, "expected child", 14) != 0) return 1144;
    }

    // Merely selecting a saved slot must not make saved OC intent look live on
    // the next GUI launch. Only explicit app-launch automation may source the
    // startup editor from a profile; disabled/invalid assignments show hardware.
    if (startup_editor_source(false, 0, 5) != STARTUP_EDITOR_SOURCE_LIVE_SNAPSHOT) return 722;
    if (startup_editor_source(false, -1, 5) != STARTUP_EDITOR_SOURCE_LIVE_SNAPSHOT) return 723;
    if (startup_editor_source(false, 6, 5) != STARTUP_EDITOR_SOURCE_LIVE_SNAPSHOT) return 724;
    if (startup_editor_source(false, 2, 5) != STARTUP_EDITOR_SOURCE_APP_LAUNCH_PROFILE) return 725;
    if (startup_editor_source(true, 2, 5) != STARTUP_EDITOR_SOURCE_LOGON_SERVICE) return 726;

    // Responsive main-window layout: the reported 3440x1440/custom-140% case
    // must fit by shrinking only the graph, while pathological effective work
    // areas retain the full content canvas and advertise scroll overflow.
    {
        const int dpi = 134; // Windows custom 140% is approximately 134 DPI.
        const int width = main_layout_scale_px(MAIN_LAYOUT_BASE_WIDTH_LOGICAL, dpi);
        MainLayoutPlan reported = main_layout_build_plan(width, 1330, dpi, 87);
        if (reported.columns != 6 || reported.rowsPerColumn != 15) return 700;
        if (reported.horizontalOverflow || reported.verticalOverflow) return 701;
        if (reported.contentHeight != reported.viewportHeight) return 702;
        if (reported.graphHeight < main_layout_scale_px(MAIN_LAYOUT_GRAPH_MIN_HEIGHT_LOGICAL, dpi) ||
            reported.graphHeight >= main_layout_scale_px(MAIN_LAYOUT_GRAPH_PREFERRED_HEIGHT_LOGICAL, dpi)) return 703;

        MainLayoutPlan expanded = main_layout_build_plan(3000, 1330, dpi, 87);
        if (expanded.columns != 11 || expanded.rowsPerColumn != 8) return 704;
        if (expanded.verticalOverflow || expanded.horizontalOverflow) return 705;

        MainLayoutPlan constrained = main_layout_build_plan(1200, 650, 288, 87);
        if (!constrained.horizontalOverflow || !constrained.verticalOverflow) return 706;
        if (constrained.graphHeight !=
            main_layout_scale_px(MAIN_LAYOUT_GRAPH_MIN_HEIGHT_LOGICAL, 288)) return 707;

        MainLayoutPlan empty = main_layout_build_plan(1920, 1000, 144, 0);
        if (empty.columns != 0 || empty.rowsPerColumn != 0) return 708;

        const int dpis[] = { 96, 110, 120, 134, 144, 168, 192, 216, 240, 288 };
        const int widths[] = { 720, 960, 1280, 1600, 1920, 2560, 3440, 3840 };
        const int heights[] = { 480, 650, 720, 900, 1000, 1040, 1330, 1376, 1400, 2120 };
        for (int testDpi : dpis) {
            for (int testWidth : widths) {
                for (int testHeight : heights) {
                    MainLayoutPlan plan = main_layout_build_plan(
                        testWidth, testHeight, testDpi, 87);
                    if (plan.columns < 1 || plan.columns > MAIN_LAYOUT_MAX_COLUMNS ||
                        plan.rowsPerColumn != (87 + plan.columns - 1) / plan.columns) return 709;
                    if (plan.graphHeight < main_layout_scale_px(
                            MAIN_LAYOUT_GRAPH_MIN_HEIGHT_LOGICAL, testDpi) ||
                        plan.graphHeight > main_layout_scale_px(
                            MAIN_LAYOUT_GRAPH_MAX_HEIGHT_LOGICAL, testDpi)) return 710;
                    if (!(plan.graphHeight < plan.pointStartY &&
                          plan.pointStartY <= plan.globalControlsY &&
                          plan.globalControlsY < plan.buttonsY &&
                          plan.buttonsY < plan.profileY &&
                          plan.profileY < plan.autoY && plan.autoY < plan.sharedY &&
                          plan.sharedY < plan.serviceY && plan.serviceY < plan.hintY &&
                          plan.hintY < plan.statusY && plan.statusY < plan.contentHeight)) return 711;
                    if (plan.horizontalOverflow != (plan.contentWidth > testWidth) ||
                        plan.verticalOverflow != (plan.contentHeight > testHeight)) return 712;
                    MainLayoutPointCell previous = {};
                    for (int vi = 0; vi < 87; ++vi) {
                        MainLayoutPointCell cell = main_layout_point_cell(plan, vi);
                        if (cell.left < 0 || cell.top < plan.pointStartY ||
                            cell.right > plan.contentWidth ||
                            cell.bottom > plan.globalControlsY -
                                main_layout_scale_px(6, testDpi)) return 713;
                        if (vi > 0 && cell.left == previous.left &&
                            cell.top != previous.bottom) return 714;
                        if (vi > 0 && cell.left != previous.left &&
                            (cell.left <= previous.left || cell.top != plan.pointStartY)) return 715;
                        previous = cell;
                    }
                    // The three right-anchored controls (GPU selector, fan-curve
                    // button, License button) share one right edge that mirrors
                    // the left inset, at every DPI and width.
                    int margin = main_layout_scale_px(
                        MAIN_LAYOUT_SIDE_MARGIN_LOGICAL, testDpi);
                    int gpuW = main_layout_scale_px(420, testDpi);
                    int fanW = main_layout_scale_px(160, testDpi);
                    int licenseW = main_layout_scale_px(118, testDpi);
                    int gpuX = main_layout_right_anchored_x(
                        plan.contentWidth, gpuW, margin,
                        margin + main_layout_scale_px(42 + 6, testDpi));
                    int fanX = main_layout_right_anchored_x(
                        plan.contentWidth, fanW, margin,
                        main_layout_scale_px(1006, testDpi));
                    int licenseX = main_layout_right_anchored_x(
                        plan.contentWidth, licenseW, margin,
                        margin + main_layout_scale_px(254 + 98 + 8, testDpi));
                    if (gpuX + gpuW != plan.contentWidth - margin) return 716;
                    if (fanX + fanW != plan.contentWidth - margin) return 1660;
                    if (licenseX + licenseW != plan.contentWidth - margin) return 1661;
                    // ... and the right inset equals the left inset used by
                    // every row, including the point grid's own left edge.
                    if (main_layout_point_cell(plan, 0).left != margin) return 1662;
                    // The fan-curve button never overlaps hFanEdit, which ends
                    // at dp(942) + dp(56), and the GPU selector always leaves
                    // room for its "GPU:" label.
                    if (fanX < main_layout_scale_px(942 + 56, testDpi)) return 1663;
                    if (gpuX < margin + main_layout_scale_px(42 + 6, testDpi)) return 1664;
                    if (plan.graphHeight - main_layout_scale_px(35 + 55, testDpi) <
                        main_layout_scale_px(160, testDpi)) return 717;
                }
            }
        }
        MainLayoutSize grown = main_layout_grow_size(
            1792, 1123, 1792, 1513, 3840, 2080);
        if (grown.width != 1792 || grown.height != 1513) return 718;
        MainLayoutSize workClamped = main_layout_grow_size(
            1792, 1123, 1792, 1513, 3440, 1376);
        if (workClamped.width != 1792 || workClamped.height != 1376) return 719;
        MainLayoutSize noShrink = main_layout_grow_size(
            2200, 1700, 1792, 1513, 3840, 2080);
        if (noShrink.width != 2200 || noShrink.height != 1700) return 720;
        // Captured 4K/150% regression: the service populated 78 VF points after
        // initial empty-state sizing. Growing to the populated preferred height
        // must restore the full graph and six-column grid without overflow.
        int populatedHeight = main_layout_preferred_client_height(1770, 144, 78);
        MainLayoutPlan populated = main_layout_build_plan(1770, populatedHeight, 144, 78);
        if (populated.columns != 6 || populated.rowsPerColumn != 13
            || populated.graphHeight != main_layout_scale_px(420, 144)
            || populated.horizontalOverflow || populated.verticalOverflow) return 721;

        MainLayoutRect work = { 100, 50, 2100, 1250 };
        MainLayoutRect centered = main_layout_center_rect(work, 1000, 600);
        if (centered.left != 600 || centered.top != 350 ||
            centered.right != 1600 || centered.bottom != 950) return 727;
        MainLayoutRect oversized = main_layout_center_rect(work, 4000, 3000);
        if (oversized.left != work.left || oversized.top != work.top ||
            oversized.right != work.right || oversized.bottom != work.bottom) return 728;
        MainLayoutRect current = { 300, 200, 1300, 800 };
        MainLayoutRect resized = main_layout_resize_around_center(
            current, 1200, 800);
        if (resized.left != 200 || resized.top != 100 ||
            resized.right != 1400 || resized.bottom != 900) return 729;

        const int checkboxDpi = 144;
        int unlabeledBox = ui_theme_checkbox_box_size(
            ui_theme_scale_px(16, checkboxDpi),
            ui_theme_scale_px(16, checkboxDpi), checkboxDpi);
        int labeledBox = ui_theme_checkbox_box_size(
            ui_theme_scale_px(240, checkboxDpi),
            ui_theme_scale_px(22, checkboxDpi), checkboxDpi);
        if (unlabeledBox != ui_theme_scale_px(14, checkboxDpi) ||
            labeledBox != unlabeledBox) return 730;

        // F-CHECKBOX-HIT (1640-1646): a labeled checkbox is clickable across the
        // box, the gap, and its caption -- and stops at the caption's last
        // pixel, so no empty background silently toggles anything.
        const int hitDpis[] = { 96, 120, 144, 192 };
        for (int dpiIndex = 0; dpiIndex < 4; dpiIndex++) {
            const int hitDpi = hitDpis[dpiIndex];
            int height = ui_theme_scale_px(20, hitDpi);
            int box = ui_theme_checkbox_box_size(0, height, hitDpi);
            int inset = ui_theme_checkbox_box_inset(hitDpi);
            int gap = ui_theme_checkbox_label_gap(hitDpi);
            int labelLeft =
                ui_theme_labeled_checkbox_label_offset(height, hitDpi);
            // The caption starts exactly where the renderer draws it.
            if (labelLeft != inset + box + gap) return 1640;
            int textWidth = ui_theme_scale_px(175, hitDpi);
            int width =
                ui_theme_labeled_checkbox_width(textWidth, height, hitDpi);
            // Every pixel of the caption is inside the control ...
            if (width < labelLeft + textWidth) return 1641;
            // ... and nothing but the balancing inset follows it.
            if (width - (labelLeft + textWidth) != inset) return 1642;
            // The gap between box and caption is covered, so there is no dead
            // stripe in the middle of the control.
            if (labelLeft - (inset + box) != gap || gap <= 0) return 1643;
            // A wider caption widens the control by exactly that much.
            if (ui_theme_labeled_checkbox_width(
                    textWidth + 40, height, hitDpi) != width + 40) return 1644;
            // The pre-F-CHECKBOX-HIT fixed widths were generous by eye; the fit
            // must not hand out that empty background as a toggle.
            if (width >= ui_theme_scale_px(280, hitDpi)) return 1645;
        }
        // A captionless control still leaves room for the box itself rather
        // than collapsing to a negative or zero width.
        if (ui_theme_labeled_checkbox_width(0, ui_theme_scale_px(20, 96), 96) !=
            ui_theme_labeled_checkbox_label_offset(
                ui_theme_scale_px(20, 96), 96) +
            ui_theme_checkbox_box_inset(96)) return 1646;
        if (ui_theme_labeled_checkbox_width(-500, 20, 96) < 1) return 1646;

        UiCheckboxState checkboxState = {};
        if (ui_checkbox_state_get(&checkboxState)) return 735;
        ui_checkbox_state_set(&checkboxState, true);
        if (!ui_checkbox_state_get(&checkboxState)) return 736;
        if (ui_checkbox_state_toggle(&checkboxState) ||
            ui_checkbox_state_get(&checkboxState)) return 737;

        // Owner-draw repaint gate.  The tick of "Share slot N with all users",
        // "Background service installed" and "Start program to tray on log in"
        // is DERIVED at paint time, so the projection must compare the value it
        // would paint against the value last painted.  The old gate asked the
        // control with BM_GETCHECK, which a BS_OWNERDRAW button never stores:
        // it always answered "unchecked", so the checked -> unchecked
        // transition was reported as "no change" and no repaint was ever
        // requested.  Unsharing a slot left the tick on screen until restart.
        UiCheckboxState painted = {};
        // Nothing painted yet but the truth is "checked": must repaint.
        if (!ui_checkbox_state_needs_repaint(&painted, true)) return 1650;
        ui_checkbox_state_set(&painted, true);   // draw_themed_button() records
        // Steady state stays inert so background probes cannot flicker.
        if (ui_checkbox_state_needs_repaint(&painted, true)) return 1651;
        // The regression: clearing the shared state MUST request a repaint.
        if (!ui_checkbox_state_needs_repaint(&painted, false)) return 1652;
        ui_checkbox_state_set(&painted, false);
        if (ui_checkbox_state_needs_repaint(&painted, false)) return 1653;
        if (!ui_checkbox_state_needs_repaint(&painted, true)) return 1654;
        // An unknown last-painted value can never be proven current.
        if (!ui_checkbox_state_needs_repaint(nullptr, false) ||
            !ui_checkbox_state_needs_repaint(nullptr, true)) return 1655;
    }

    // The WCHAR CLI parser used to sit in the Win32-only config_utils.cpp, so
    // this block was #if defined(_WIN32) and could not even link on Linux.  It
    // moved to config_text_utils.cpp on 2026-07-28 and now runs on both hosts.
    // (Only the Windows CLI is WCHAR-based; the Linux CLI is parsed by
    // linux_cli_options.cpp and covered by its own assertions. This is coverage
    // of the parser itself, which is shared.)
    int point = -1;
    if (!parse_cli_point_arg_w(L"--point0", &point) || point != 0) return 21;
    if (!parse_cli_point_arg_w(L"--point127", &point) || point != 127) return 22;
    if (parse_cli_point_arg_w(L"--point128", &point)) return 23;
    if (parse_cli_point_arg_w(L"--pointabc", &point)) return 24;
    if (parse_cli_point_arg_w(L"--point-1", &point)) return 25;

    // F-LNX-DEDUP (523-527, 1900-1917).  These three parsers moved out of the
    // Win32-only config_utils.cpp into config_text_utils.cpp, which the Linux
    // binary now links; linux_port.cpp's verbatim duplicate of parse_fan_value
    // was deleted in the same change.  Everything below therefore exercises the
    // code the Linux daemon actually runs, which nothing did before.
    {
        // Win32 treats INI section names case-insensitively.  The production
        // direct-file section replacer uses this helper, so a hand-edited
        // [Profiles] header cannot survive beside a new [profiles] copy.
        if (!config_section_header_matches_ascii("[profiles]", "profiles")) return 523;
        if (!config_section_header_matches_ascii("[Profiles]", "profiles")) return 524;
        if (!config_section_header_matches_ascii("[PROFILES]", "profiles")) return 525;
        if (config_section_header_matches_ascii("[profiles_old]", "profiles")) return 526;
        if (config_section_header_matches_ascii("[profile]", "profiles")) return 527;

        // _strnicmp was replaced by an explicit ASCII fold.  The case that
        // separates the two is a line SHORTER than the section name: the fold
        // must stop at the NUL instead of reading past the end of the buffer.
        // ASan/UBSan turn a regression here into a hard failure.
        if (config_section_header_matches_ascii("[prof", "profiles")) return 1900;
        if (config_section_header_matches_ascii("[", "profiles")) return 1901;
        if (config_section_header_matches_ascii("[]", "profiles")) return 1902;
        // An empty section name matches "[]" only, and the ']' check still runs.
        if (!config_section_header_matches_ascii("[]", "")) return 1903;
        if (config_section_header_matches_ascii("[a]", "")) return 1904;
        // Not a header at all / null inputs must not crash.
        if (config_section_header_matches_ascii("profiles]", "profiles")) return 1905;
        if (config_section_header_matches_ascii(nullptr, "profiles")) return 1906;
        if (config_section_header_matches_ascii("[profiles]", nullptr)) return 1907;

        // parse_fan_value had NO regression coverage before this change, on
        // either platform, despite the Linux daemon running its own copy.
        bool isAuto = false;
        int pct = -1;
        if (!parse_fan_value("auto", &isAuto, &pct) || !isAuto || pct != 0) return 1908;
        if (!parse_fan_value("AUTO", &isAuto, &pct) || !isAuto) return 1909;
        if (!parse_fan_value("", &isAuto, &pct) || !isAuto) return 1910;
        if (!parse_fan_value(nullptr, &isAuto, &pct) || !isAuto) return 1911;
        if (!parse_fan_value("  50  ", &isAuto, &pct) || isAuto || pct != 50) return 1912;
        if (!parse_fan_value("0", &isAuto, &pct) || isAuto || pct != 0) return 1913;
        if (!parse_fan_value("100", &isAuto, &pct) || isAuto || pct != 100) return 1914;
        if (parse_fan_value("101", &isAuto, &pct)) return 1915;
        if (parse_fan_value("-1", &isAuto, &pct)) return 1916;
        if (parse_fan_value("fast", &isAuto, &pct)) return 1917;
        // StringCchCopyA was replaced by gc_snprintf("%s").  Both truncate into
        // a 64-byte buffer, so an over-long input must still be rejected rather
        // than parsing whatever fitted.  A 70-digit run truncates to 63 digits,
        // which parse_int_strict must reject as out of range either way.
        {
            char overlong[80];
            memset(overlong, '9', sizeof(overlong));
            overlong[sizeof(overlong) - 1] = 0;
            if (parse_fan_value(overlong, &isAuto, &pct)) return 1918;
        }
        // Null out-parameters are rejected, not dereferenced.
        if (parse_fan_value("50", nullptr, &pct)) return 1919;
        if (parse_fan_value("50", &isAuto, nullptr)) return 1920;
    }

    if (gpu_family_uses_best_guess_backend(GPU_FAMILY_PASCAL)) return 26;
    if (gpu_family_uses_best_guess_backend(GPU_FAMILY_TURING)) return 27;
    if (gpu_family_uses_best_guess_backend(GPU_FAMILY_AMPERE)) return 28;
    if (gpu_family_uses_best_guess_backend(GPU_FAMILY_LOVELACE)) return 29;
    if (gpu_family_uses_best_guess_backend(GPU_FAMILY_BLACKWELL)) return 30;
    if (!gpu_family_uses_best_guess_backend(GPU_FAMILY_UNKNOWN)) return 67;
    {
        GpuFamily fam = GPU_FAMILY_UNKNOWN;
        const VfBackendSpec* spec = vf_backend_for_architecture(NV_GPU_ARCHITECTURE_GB200, &fam);
        if (!spec || fam != GPU_FAMILY_BLACKWELL || spec->bestGuessOnly) return 171;
        spec = vf_backend_for_architecture(NV_GPU_ARCHITECTURE_AD100, &fam);
        if (!spec || fam != GPU_FAMILY_LOVELACE || spec->bestGuessOnly) return 172;
        spec = vf_backend_for_architecture(NV_GPU_ARCHITECTURE_GA100, &fam);
        if (!spec || fam != GPU_FAMILY_AMPERE || spec->bestGuessOnly) return 173;
        spec = vf_backend_for_architecture(0xDEADBEEFu, &fam);
        if (!spec || fam != GPU_FAMILY_UNKNOWN || !spec->bestGuessOnly) return 174;
    }

    // F-CAP: per-domain control-surface capability (integrated-SoC support).
    // The first block pins the CORE INVARIANT — an unprobed or fully-available
    // capability set must subtract nothing and must leave every new path inert,
    // which is what guarantees x64 / discrete Blackwell-Lovelace-Ampere
    // behavior is unchanged.
    {
        GpuCapabilityProbe unprobed = {};
        if (gpu_capability_available_domains(&unprobed) != SERVICE_MUTATION_DOMAIN_ALL) return 2100;
        if (gpu_capability_missing_domains(&unprobed) != 0) return 2101;
        if (!gpu_capability_is_complete(&unprobed)) return 2102;
        if (gpu_capability_surface_class(&unprobed) != GPU_CONTROL_SURFACE_FULL) return 2103;
        if (gpu_capability_memory_write_is_risky(&unprobed)) return 2104;
        // A null probe must behave exactly like an unprobed one.
        if (gpu_capability_available_domains(nullptr) != SERVICE_MUTATION_DOMAIN_ALL) return 2105;
        if (!gpu_capability_is_complete(nullptr)) return 2106;
        if (gpu_capability_memory_write_is_risky(nullptr)) return 2107;

        // Every domain explicitly AVAILABLE is still complete and inert.
        GpuCapabilityProbe healthy = {};
        for (int i = 0; i < GPU_CAP_DOMAIN_COUNT; ++i)
            healthy.domain[i] = GPU_DOMAIN_CAP_AVAILABLE;
        healthy.memoryTopology = GPU_MEMORY_TOPOLOGY_DEDICATED;
        if (!gpu_capability_is_complete(&healthy)) return 2108;
        if (gpu_capability_available_domains(&healthy) != SERVICE_MUTATION_DOMAIN_ALL) return 2109;
        if (gpu_capability_surface_class(&healthy) != GPU_CONTROL_SURFACE_FULL) return 2110;
        if (gpu_capability_memory_write_is_risky(&healthy)) return 2111;
        // No validated discrete family may ever raise the limited-surface warning.
        if (gpu_requires_limited_control_warning(GPU_FAMILY_BLACKWELL, &healthy)) return 2112;
        if (gpu_requires_limited_control_warning(GPU_FAMILY_LOVELACE, &healthy)) return 2113;
        if (gpu_requires_limited_control_warning(GPU_FAMILY_AMPERE, &healthy)) return 2114;
        if (gpu_requires_limited_control_warning(GPU_FAMILY_TURING, &healthy)) return 2115;
        if (gpu_requires_limited_control_warning(GPU_FAMILY_PASCAL, &healthy)) return 2116;
        if (gpu_requires_limited_control_warning(GPU_FAMILY_BLACKWELL, &unprobed)) return 2117;
        if (gpu_requires_limited_control_warning(GPU_FAMILY_BLACKWELL, nullptr)) return 2118;
    }

    // GB10-shaped part: reports Blackwell (so the VF layout stays correct) but
    // the board-level domains a discrete card exposes are missing.
    {
        GpuCapabilityProbe soc = {};
        for (int i = 0; i < GPU_CAP_DOMAIN_COUNT; ++i)
            soc.domain[i] = GPU_DOMAIN_CAP_AVAILABLE;
        soc.memoryTopology = GPU_MEMORY_TOPOLOGY_UNIFIED;
        gpu_capability_set(&soc, SERVICE_MUTATION_DOMAIN_MEM_OFFSET, GPU_DOMAIN_CAP_ABSENT);
        gpu_capability_set(&soc, SERVICE_MUTATION_DOMAIN_POWER, GPU_DOMAIN_CAP_REFUSED);
        gpu_capability_set(&soc, SERVICE_MUTATION_DOMAIN_FAN, GPU_DOMAIN_CAP_ABSENT);

        if (gpu_capability_is_complete(&soc)) return 2130;
        gc_u32 missing = gpu_capability_missing_domains(&soc);
        if (missing != (SERVICE_MUTATION_DOMAIN_MEM_OFFSET |
                        SERVICE_MUTATION_DOMAIN_POWER |
                        SERVICE_MUTATION_DOMAIN_FAN)) return 2131;
        // VF curve and lock survive: the layout is right, so writes stay enabled.
        gc_u32 available = gpu_capability_available_domains(&soc);
        if (!(available & SERVICE_MUTATION_DOMAIN_VF_CURVE)) return 2132;
        if (!(available & SERVICE_MUTATION_DOMAIN_LOCK)) return 2133;
        if (available & SERVICE_MUTATION_DOMAIN_MEM_OFFSET) return 2134;
        if (gpu_capability_surface_class(&soc) != GPU_CONTROL_SURFACE_PARTIAL) return 2135;
        if (!gpu_capability_memory_write_is_risky(&soc)) return 2136;
        // Recognized family + incomplete surface is exactly the new warning.
        if (!gpu_requires_limited_control_warning(GPU_FAMILY_BLACKWELL, &soc)) return 2137;
        // An unrecognized family already warns; it must not warn twice.
        if (gpu_requires_limited_control_warning(GPU_FAMILY_UNKNOWN, &soc)) return 2138;
    }

    // Every write domain missing = monitor-only.
    {
        GpuCapabilityProbe monitor = {};
        for (int i = 0; i < GPU_CAP_DOMAIN_COUNT; ++i)
            monitor.domain[i] = GPU_DOMAIN_CAP_ABSENT;
        if (gpu_capability_surface_class(&monitor) != GPU_CONTROL_SURFACE_MONITOR_ONLY) return 2140;
        if (gpu_capability_available_domains(&monitor) != 0) return 2141;
        if (gpu_capability_is_complete(&monitor)) return 2142;
    }

    // Index/mask round-trip and out-of-range rejection.
    {
        for (int i = 0; i < GPU_CAP_DOMAIN_COUNT; ++i) {
            if (gpu_capability_index_for_mask(gpu_capability_mask_for_index(i)) != i) return 2150;
        }
        if (gpu_capability_mask_for_index(-1) != 0) return 2151;
        if (gpu_capability_mask_for_index(GPU_CAP_DOMAIN_COUNT) != 0) return 2152;
        if (gpu_capability_index_for_mask(0) != -1) return 2153;
        if (gpu_capability_index_for_mask(1u << 20) != -1) return 2154;
        // An unknown mask or an out-of-range capability value must be ignored,
        // not silently written over a neighbouring domain.
        GpuCapabilityProbe guard = {};
        gpu_capability_set(&guard, 1u << 20, GPU_DOMAIN_CAP_ABSENT);
        if (!gpu_capability_is_complete(&guard)) return 2155;
        gpu_capability_set(&guard, SERVICE_MUTATION_DOMAIN_FAN, 99u);
        if (!gpu_capability_is_complete(&guard)) return 2156;
        gpu_capability_set(nullptr, SERVICE_MUTATION_DOMAIN_FAN, GPU_DOMAIN_CAP_ABSENT);
        if (gpu_capability_get(nullptr, SERVICE_MUTATION_DOMAIN_FAN) != GPU_DOMAIN_CAP_UNPROBED) return 2157;
    }

    // Observation -> capability classification.  The old-driver case is the
    // one that protects existing x64 installs from a new warning.
    {
        GpuDomainObservation obs = {};
        // Entry point missing = older driver, NOT absent hardware.
        obs.entryPointPresent = 0;
        obs.readSucceeded = 0;
        if (gpu_capability_classify(&obs) != GPU_DOMAIN_CAP_UNPROBED) return 2160;

        obs.entryPointPresent = 1;
        obs.readSucceeded = 1;
        obs.hardwareAbsent = 0;
        if (gpu_capability_classify(&obs) != GPU_DOMAIN_CAP_AVAILABLE) return 2161;

        obs.readSucceeded = 0;
        if (gpu_capability_classify(&obs) != GPU_DOMAIN_CAP_REFUSED) return 2162;

        // Positive "no such unit" beats a successful read.
        obs.readSucceeded = 1;
        obs.hardwareAbsent = 1;
        if (gpu_capability_classify(&obs) != GPU_DOMAIN_CAP_ABSENT) return 2163;

        if (gpu_capability_classify(nullptr) != GPU_DOMAIN_CAP_UNPROBED) return 2164;

        // An entirely unresolved driver must leave the surface complete, so a
        // legacy-driver x64 install never sees the limited-surface warning.
        GpuCapabilityProbe legacy = {};
        GpuDomainObservation none = {};
        for (int i = 0; i < GPU_CAP_DOMAIN_COUNT; ++i)
            gpu_capability_set(&legacy, gpu_capability_mask_for_index(i),
                               gpu_capability_classify(&none));
        if (!gpu_capability_is_complete(&legacy)) return 2165;
        if (gpu_requires_limited_control_warning(GPU_FAMILY_BLACKWELL, &legacy)) return 2166;
    }

    // Memory-topology classification from the adapter memory split.  The
    // discrete cases must never come out UNIFIED: that is what keeps the
    // unified-memory confirmation off every existing x64 install.
    {
        const unsigned long long MiB = 1024ull * 1024ull;
        // RTX 5070-class discrete board: 12 GiB dedicated.
        if (gpu_memory_topology_from_sizes(12288ull * MiB, 16384ull * MiB) !=
            GPU_MEMORY_TOPOLOGY_DEDICATED) return 2170;
        // Small-VRAM discrete board still reads dedicated.
        if (gpu_memory_topology_from_sizes(2048ull * MiB, 8192ull * MiB) !=
            GPU_MEMORY_TOPOLOGY_DEDICATED) return 2171;
        // GB10-shaped unified part: no dedicated pool, large shared pool.
        if (gpu_memory_topology_from_sizes(0ull, 131072ull * MiB) !=
            GPU_MEMORY_TOPOLOGY_UNIFIED) return 2172;
        // A failed/empty query must stay UNKNOWN, never UNIFIED.
        if (gpu_memory_topology_from_sizes(0ull, 0ull) !=
            GPU_MEMORY_TOPOLOGY_UNKNOWN) return 2173;
        // Exactly at the floor counts as dedicated.
        if (gpu_memory_topology_from_sizes(GPU_DEDICATED_VRAM_FLOOR_BYTES, 0ull) !=
            GPU_MEMORY_TOPOLOGY_DEDICATED) return 2174;

        // The confirmation fires only on positively-reported UNIFIED.
        GpuCapabilityProbe topo = {};
        topo.memoryTopology = GPU_MEMORY_TOPOLOGY_DEDICATED;
        if (gpu_capability_memory_write_is_risky(&topo)) return 2175;
        topo.memoryTopology = GPU_MEMORY_TOPOLOGY_UNKNOWN;
        if (gpu_capability_memory_write_is_risky(&topo)) return 2176;
        topo.memoryTopology = GPU_MEMORY_TOPOLOGY_UNIFIED;
        if (!gpu_capability_memory_write_is_risky(&topo)) return 2177;

        // Topology/family contradiction: a recognized DISCRETE family reporting
        // a UNIFIED pool is an integrated part wearing that family's
        // architecture id.  This is what stops a GB10-class board that reports
        // Blackwell AND answers every domain from being silently treated as a
        // validated discrete card.
        GpuCapabilityProbe integrated = {};
        for (int i = 0; i < GPU_CAP_DOMAIN_COUNT; ++i)
            integrated.domain[i] = GPU_DOMAIN_CAP_AVAILABLE;
        integrated.memoryTopology = GPU_MEMORY_TOPOLOGY_UNIFIED;
        // Nothing is missing, so the per-domain probe alone would find no
        // reason to warn.  The contradiction is the only evidence there is.
        if (!gpu_capability_is_complete(&integrated)) return 2190;
        if (gpu_capability_surface_class(&integrated) != GPU_CONTROL_SURFACE_FULL) return 2191;
        if (!gpu_capability_topology_contradicts_discrete(&integrated)) return 2192;
        if (!gpu_requires_limited_control_warning(GPU_FAMILY_BLACKWELL, &integrated)) return 2193;
        if (!gpu_requires_limited_control_warning(GPU_FAMILY_LOVELACE, &integrated)) return 2194;
        // An unrecognized family already warns via the best-guess tier and must
        // not warn twice, contradiction or not.
        if (gpu_requires_limited_control_warning(GPU_FAMILY_UNKNOWN, &integrated)) return 2195;

        // No-regression half: a healthy discrete GPU, an unanswered topology
        // query, and a wholly unprobed build must all stay silent.
        GpuCapabilityProbe discrete = {};
        for (int i = 0; i < GPU_CAP_DOMAIN_COUNT; ++i)
            discrete.domain[i] = GPU_DOMAIN_CAP_AVAILABLE;
        discrete.memoryTopology = GPU_MEMORY_TOPOLOGY_DEDICATED;
        if (gpu_capability_topology_contradicts_discrete(&discrete)) return 2196;
        if (gpu_requires_limited_control_warning(GPU_FAMILY_BLACKWELL, &discrete)) return 2197;
        discrete.memoryTopology = GPU_MEMORY_TOPOLOGY_UNKNOWN;
        if (gpu_capability_topology_contradicts_discrete(&discrete)) return 2198;
        if (gpu_requires_limited_control_warning(GPU_FAMILY_BLACKWELL, &discrete)) return 2199;
        GpuCapabilityProbe zeroed = {};
        if (gpu_capability_topology_contradicts_discrete(&zeroed)) return 2200;
        if (gpu_requires_limited_control_warning(GPU_FAMILY_BLACKWELL, &zeroed)) return 2201;
        if (gpu_capability_topology_contradicts_discrete(nullptr)) return 2202;

        // The complete probe must round-trip through the five bytes reserved in
        // ServiceGpuHealth.  This is the producer/consumer contract that keeps
        // GUI warnings and the unified-memory confirmation from reading a
        // process-local zeroed probe.
        topo.domain[0] = GPU_DOMAIN_CAP_UNPROBED;
        topo.domain[1] = GPU_DOMAIN_CAP_AVAILABLE;
        topo.domain[2] = GPU_DOMAIN_CAP_REFUSED;
        topo.domain[3] = GPU_DOMAIN_CAP_ABSENT;
        topo.domain[4] = GPU_DOMAIN_CAP_AVAILABLE;
        topo.domain[5] = GPU_DOMAIN_CAP_REFUSED;
        topo.domain[6] = GPU_DOMAIN_CAP_ABSENT;
        gc_u32 packed = gpu_capability_pack_domains(&topo);
        if ((packed & ~SERVICE_GPU_CAPABILITY_PACKED_MASK) != 0) return 2178;
        GpuCapabilityProbe decoded = {};
        gpu_capability_unpack_domains(&decoded, packed,
                                      GPU_MEMORY_TOPOLOGY_UNIFIED);
        for (int i = 0; i < GPU_CAP_DOMAIN_COUNT; ++i)
            if (decoded.domain[i] != topo.domain[i]) return 2179;
        if (decoded.memoryTopology != GPU_MEMORY_TOPOLOGY_UNIFIED) return 2190;

        ServiceResponse wire = fake_ready_service_response(17, 3, 2);
        wire.snapshot.health.capabilityMemoryTopology =
            GPU_MEMORY_TOPOLOGY_UNIFIED;
        wire.snapshot.health.capabilityDomainsPacked = packed;
        if (!validate_service_response_for_ipc(&wire)) return 2191;
        ServiceResponse badTopology = wire;
        badTopology.snapshot.health.capabilityMemoryTopology =
            SERVICE_GPU_MEMORY_TOPOLOGY_MAX + 1;
        if (validate_service_response_for_ipc(&badTopology)) return 2192;
        ServiceResponse badPacked = wire;
        badPacked.snapshot.health.capabilityDomainsPacked =
            SERVICE_GPU_CAPABILITY_PACKED_MASK + 1u;
        if (validate_service_response_for_ipc(&badPacked)) return 2193;

        // Compatibility with a protocol-v16 producer from before these bytes
        // were assigned: all zero still means unknown/unprobed, never missing.
        GpuCapabilityProbe legacyWire = {};
        gpu_capability_unpack_domains(&legacyWire, 0, 0);
        if (!gpu_capability_is_complete(&legacyWire) ||
            legacyWire.memoryTopology != GPU_MEMORY_TOPOLOGY_UNKNOWN)
            return 2194;
    }

    // NVAPI module preference.  Verified against the RTX Spark developer-preview
    // driver 616.00, which ships nvapia64.dll (ARM64), nvapi64.dll (x64) and
    // nvapi.dll (x86) side by side: a native arm64 process can only load the
    // first.  Pinned here because the arm64 ordering is otherwise untestable
    // without arm64 hardware.
    {
        const char* a0 = nvapi_module_candidate(NVAPI_HOST_ARCH_ARM64, 0);
        if (!a0 || strcmp(a0, "nvapia64.dll") != 0) return 2180;
        const char* a1 = nvapi_module_candidate(NVAPI_HOST_ARCH_ARM64, 1);
        if (!a1 || strcmp(a1, "nvapi64.dll") != 0) return 2181;
        if (nvapi_module_candidate(NVAPI_HOST_ARCH_ARM64, 2) != nullptr) return 2182;

        // x64 ordering must be exactly what it always was — this is the
        // no-regression pin for every existing Windows x64 install.
        const char* x0 = nvapi_module_candidate(NVAPI_HOST_ARCH_X64, 0);
        if (!x0 || strcmp(x0, "nvapi64.dll") != 0) return 2183;
        const char* x1 = nvapi_module_candidate(NVAPI_HOST_ARCH_X64, 1);
        if (!x1 || strcmp(x1, "nvapi.dll") != 0) return 2184;
        if (nvapi_module_candidate(NVAPI_HOST_ARCH_X64, 2) != nullptr) return 2185;

        // x64 must never reach for the arm64 image, nor arm64 for the x86 one.
        for (int i = 0; i < 4; ++i) {
            const char* x = nvapi_module_candidate(NVAPI_HOST_ARCH_X64, i);
            if (x && strcmp(x, "nvapia64.dll") == 0) return 2186;
            const char* a = nvapi_module_candidate(NVAPI_HOST_ARCH_ARM64, i);
            if (a && strcmp(a, "nvapi.dll") == 0) return 2187;
        }
        if (nvapi_module_candidate(NVAPI_HOST_ARCH_X64, -1) != nullptr) return 2188;
    }

    // Windows driver self-test verdict/exit policy.  FULL is permitted only
    // when NVML and both private VF reads answered; these cases would have
    // falsely printed FULL and returned process code 0 before the fix.
    {
        DriverSelfTestFacts facts = {
            true, true, true, true, true, true, true,
            GPU_CONTROL_SURFACE_FULL
        };
        if (driver_self_test_verdict(&facts) != DRIVER_SELF_TEST_FULL ||
            driver_self_test_exit_code(DRIVER_SELF_TEST_FULL) != 0) return 2200;
        facts.controlRead = false;
        if (driver_self_test_verdict(&facts) != DRIVER_SELF_TEST_PARTIAL ||
            driver_self_test_exit_code(DRIVER_SELF_TEST_PARTIAL) != 2) return 2201;
        facts.controlRead = true;
        facts.nvmlReady = false;
        if (driver_self_test_verdict(&facts) != DRIVER_SELF_TEST_PARTIAL)
            return 2202;
        facts.nvmlReady = true;
        facts.elevated = false;
        facts.curveRead = false;
        if (driver_self_test_verdict(&facts) != DRIVER_SELF_TEST_INCONCLUSIVE)
            return 2203;
        facts.elevated = true;
        facts.curveRead = true;
        facts.nvapiInitialized = false;
        if (driver_self_test_verdict(&facts) != DRIVER_SELF_TEST_UNUSABLE ||
            driver_self_test_exit_code(DRIVER_SELF_TEST_UNUSABLE) != 1)
            return 2204;
        facts.nvapiInitialized = true;
        facts.vfWritable = false;
        facts.surface = GPU_CONTROL_SURFACE_MONITOR_ONLY;
        if (driver_self_test_verdict(&facts) != DRIVER_SELF_TEST_MONITOR_ONLY ||
            driver_self_test_exit_code(DRIVER_SELF_TEST_MONITOR_ONLY) != 2)
            return 2205;
    }

    FanCurveConfig cfg = {};
    fan_curve_set_default(&cfg);
    fan_curve_normalize(&cfg);
    char err[128] = {};
    if (!fan_curve_validate(&cfg, err, sizeof(err))) return 1;
    if (fan_curve_active_count(&cfg) != 5) return 2;
    if (fan_curve_interpolate_percent(&cfg, 30) != 20) return 3;
    int mid = fan_curve_interpolate_percent(&cfg, 52);
    if (mid < 42 || mid > 48) return 4;
    cfg.points[1].fanPercent = 10;
    if (fan_curve_validate(&cfg, err, sizeof(err))) return 5;
    cfg.points[1].fanPercent = 35;
    cfg.pollIntervalMs = 333;
    fan_curve_normalize(&cfg);
    if (cfg.pollIntervalMs != 250) return 6;

    FanCurveConfig invalidFanCurve = {};
    invalidFanCurve.pollIntervalMs = 333;
    invalidFanCurve.hysteresisC = 99;
    fan_curve_normalize(&invalidFanCurve);
    if (!fan_curve_validate(&invalidFanCurve, err, sizeof(err))) return 7;
    if (fan_curve_active_count(&invalidFanCurve) != 5) return 8;
    if (invalidFanCurve.pollIntervalMs != 250) return 9;
    if (invalidFanCurve.hysteresisC != FAN_CURVE_MAX_HYSTERESIS_C) return 10;
    FanCurveConfig onePointFanCurve = {};
    onePointFanCurve.pollIntervalMs = 500;
    onePointFanCurve.hysteresisC = 1;
    onePointFanCurve.points[7] = { gc_bool8_from_bool(true), 99, 100 };
    fan_curve_normalize(&onePointFanCurve);
    if (!fan_curve_validate(&onePointFanCurve, err, sizeof(err))) return 11;

    int parsedInt = 0;
    if (!parse_int_strict("2147483647", &parsedInt) || parsedInt != 2147483647) return 12;
    if (!parse_int_strict("-2147483648", &parsedInt) || parsedInt != (-2147483647 - 1)) return 13;
    if (parse_int_strict("999999999999999999999999", &parsedInt)) return 14;
    if (parse_int_strict("-999999999999999999999999", &parsedInt)) return 15;

    if (argc < 2 || !argv[1] || !argv[1][0]) return 31;
#if defined(_WIN32)
    // Win32 config storage: the named-mutex lock and the Unicode INI writer.
    // Linux config storage is a different implementation (linux_port.cpp).
    DeleteFileA(argv[1]);
    HANDLE configMutex = nullptr;
    if (!enter_config_storage_lock(&configMutex)) return 32;
    leave_config_storage_lock(configMutex);
    HANDLE configPeer = OpenMutexA(SYNCHRONIZE | MUTEX_MODIFY_STATE,
        FALSE, "Global\\GreenCurveConfigMutex-v2");
    if (!configPeer) return 522;
    CloseHandle(configPeer);
    HANDLE overprivilegedConfigPeer = OpenMutexA(MUTEX_ALL_ACCESS,
        FALSE, "Global\\GreenCurveConfigMutex-v2");
    if (overprivilegedConfigPeer) {
        CloseHandle(overprivilegedConfigPeer);
        return 528;
    }
    if (enter_config_storage_lock(nullptr)) return 529;
    if (get_config_int(argv[1], "debug", "enabled", 77) != 77) return 33;
    if (!set_config_int(argv[1], "debug", "enabled", APP_DEBUG_DEFAULT_ENABLED)) {
        fprintf(stderr, "Unicode config write failed for [%s] (Win32 error %lu)\n",
            argv[1], (unsigned long)GetLastError());
        return 34;
    }
    if (!config_section_has_keys(argv[1], "debug")) return 35;
    if (get_config_int(argv[1], "debug", "enabled", 0) != 1) return 36;
    if (!set_config_int(argv[1], "runtime", "selective_gpu_offset_mhz", 45)) return 37;
    if (get_config_int(argv[1], "runtime", "selective_gpu_offset_mhz", 0) != 45) return 38;
    // Lock mode (none/flatten/pin) must round-trip through the profile INI.
    // Pin (hard) loss on save was the user-reported bug; this guards the
    // serialize/parse contract the GUI relies on.
    if (!set_config_int(argv[1], "profile1", "lock_mode", LOCK_MODE_HARD)) return 39;
    if (get_config_int(argv[1], "profile1", "lock_mode", 0) != LOCK_MODE_HARD) return 46;
    if (!set_config_int(argv[1], "profile1", "lock_mode", LOCK_MODE_FLATTEN)) return 47;
    if (get_config_int(argv[1], "profile1", "lock_mode", 0) != LOCK_MODE_FLATTEN) return 48;
    {
        ConfiguredGpuSelection published = {};
        published.stableIdentityPresent = true;
        published.legacyIndex = 2;
        published.identity.valid = true;
        published.identity.pciInfoValid = true;
        published.identity.deviceId = 0x268410DEu;
        published.identity.subSystemId = 0x17AA3A5Cu;
        published.identity.pciRevisionId = 0xA1u;
        published.identity.extDeviceId = 0x12345678u;
        published.identity.pciDomain = 0;
        published.identity.pciBus = 9;
        published.identity.pciDevice = 3;
        published.identity.pciFunction = 1;
        char section[1024] = {};
        char gpuErr[256] = {};
        if (!format_configured_gpu_selection_section("profile2_gpu",
                &published, section, sizeof(section), gpuErr,
                sizeof(gpuErr))) return 731;
        const char* replaced[] = { "profile2_gpu" };
        if (!write_config_sections_atomic(argv[1], section, replaced, 1,
                gpuErr, sizeof(gpuErr))) return 732;
        ConfiguredGpuSelection loaded = {};
        if (!load_configured_gpu_selection_from_section(argv[1],
                "profile2_gpu", &loaded, gpuErr, sizeof(gpuErr))) return 733;
        if (!loaded.stableIdentityPresent || loaded.legacyIndex != 2 ||
            !configured_gpu_base_identity_matches(
                &published.identity, &loaded.identity) ||
            loaded.identity.pciBus != 9 || loaded.identity.pciDevice != 3 ||
            loaded.identity.pciFunction != 1) return 734;
    }
    gc_DeleteFileUtf8(argv[1]);
#endif // _WIN32

    // F-08-001: IPC object size and field layout sanity
    {
        if (sizeof(ServiceRequest) > 65535) return 70;
        if (sizeof(ServiceResponse) > 262143) return 71;
    }

    // F-08-001: validate_desired_settings_for_ipc extreme edge cases
    {
        DesiredSettings ds = {};
        validate_desired_settings_for_ipc(nullptr);
        validate_desired_settings_for_ipc(&ds);
        ds.hasPowerLimit = 7; ds.powerLimitPct = 0;
        ds.hasGpuOffset = 9; ds.gpuOffsetMHz = -50000;
        ds.hasMemOffset = 11; ds.memOffsetMHz = 99999;
        ds.hasFan = 13; ds.fanPercent = -100;
        ds.fanAuto = 15;
        ds.resetOcBeforeApply = 17;
        for (int ci = 0; ci < VF_NUM_POINTS; ci++) {
            ds.hasCurvePoint[ci] = 19;
            ds.curvePointMHz[ci] = 9999999u;
        }
        ds.fanCurve.points[0].enabled = 21;
        validate_desired_settings_for_ipc(&ds);
        if (ds.hasPowerLimit != 1 || ds.hasGpuOffset != 1 || ds.hasMemOffset != 1) return 68;
        if (ds.hasFan != 1 || ds.fanAuto != 1 || ds.resetOcBeforeApply != 1) return 69;
        if (ds.hasCurvePoint[0] != 1 || ds.fanCurve.points[0].enabled != 1) return 79;
        if (ds.powerLimitPct != 50) return 72;
        if (ds.gpuOffsetMHz != -1000) return 73;
        if (ds.memOffsetMHz != 5000) return 74;
        if (ds.fanPercent != 0) return 75;
        if (ds.curvePointMHz[0] != 5000u) return 76;
        // Lock mode must clamp to the valid tri-state range at the IPC boundary.
        ds.lockMode = (LockMode)999;
        validate_desired_settings_for_ipc(&ds);
        if (ds.lockMode != LOCK_MODE_HARD) return 77;
        ds.lockMode = (LockMode)(-5);
        validate_desired_settings_for_ipc(&ds);
        if (ds.lockMode != LOCK_MODE_NONE) return 78;
    }

    // Protocol-v12 response envelopes are strict, complete, and generation
    // stamped. Numeric defaults (0 offsets / 100% power / Auto fan) are valid
    // authoritative values rather than "missing" sentinels.
    {
        ServiceResponse resp = fake_ready_service_response(7, 1, 1);
        if (!validate_service_response_for_ipc(&resp)) return 154;
        if (resp.controlState.gpuOffsetMHz != 0 ||
            resp.controlState.memOffsetMHz != 0 ||
            resp.controlState.powerLimitPct != 100 ||
            resp.controlState.fanMode != FAN_MODE_AUTO) return 155;
        ServiceResponse malformed = resp;
        malformed.snapshot.initialized = 99;
        if (validate_service_response_for_ipc(&malformed)) return 156;
        malformed = resp;
        malformed.state.reservedBool[0] = 1;
        if (validate_service_response_for_ipc(&malformed)) return 157;
        malformed = resp;
        malformed.state.serviceInstanceId = 0;
        if (validate_service_response_for_ipc(&malformed)) return 158;
        malformed = resp;
        malformed.state.topologySignature ^= 1;
        if (validate_service_response_for_ipc(&malformed)) return 1101;
        malformed = resp;
        malformed.state.validSections &=
            ~SERVICE_STATE_SECTION_APPLIED_CONTROLS;
        if (validate_service_response_for_ipc(&malformed)) return 1102;
        malformed = resp;
        malformed.snapshot.health.availableMutationDomains = 0x80000000u;
        if (validate_service_response_for_ipc(&malformed)) return 1140;
        // v18 outcome severity. A successful answer may be clean or carry a
        // warning; anything else is damaged, because both producers derive the
        // field from `status` at their single write-out point.
        ServiceResponse warned = resp;
        warned.outcomeSeverity = SERVICE_OUTCOME_SEVERITY_WARNING;
        if (!validate_service_response_for_ipc(&warned)) return 2290;
        malformed = resp;
        malformed.outcomeSeverity = SERVICE_OUTCOME_SEVERITY_ERROR;
        if (validate_service_response_for_ipc(&malformed)) return 2291;
        malformed = resp;
        malformed.outcomeSeverity = 7;
        if (validate_service_response_for_ipc(&malformed)) return 2292;
        malformed = resp;
        malformed.outcomeSeverityReserved = 1;
        if (validate_service_response_for_ipc(&malformed)) return 2293;
    }

    // The one rule behind that field: it is DERIVED from status, at the single
    // point each producer writes a response out, so no command handler can
    // forget it and none can contradict itself. (2294-2299)
    {
        if (service_response_resolve_outcome_severity(SERVICE_STATUS_OK,
                SERVICE_OUTCOME_SEVERITY_SUCCESS) !=
            SERVICE_OUTCOME_SEVERITY_SUCCESS) return 2294;
        if (service_response_resolve_outcome_severity(SERVICE_STATUS_OK,
                SERVICE_OUTCOME_SEVERITY_WARNING) !=
            SERVICE_OUTCOME_SEVERITY_WARNING) return 2295;
        // A failed operation is an ERROR no matter what a handler recorded, and
        // a successful one can never be reported as an error.
        if (service_response_resolve_outcome_severity(SERVICE_STATUS_ERROR,
                SERVICE_OUTCOME_SEVERITY_SUCCESS) !=
            SERVICE_OUTCOME_SEVERITY_ERROR) return 2296;
        if (service_response_resolve_outcome_severity(SERVICE_STATUS_STALE_STATE,
                SERVICE_OUTCOME_SEVERITY_WARNING) !=
            SERVICE_OUTCOME_SEVERITY_ERROR) return 2297;
        if (service_response_resolve_outcome_severity(SERVICE_STATUS_OK,
                SERVICE_OUTCOME_SEVERITY_ERROR) !=
            SERVICE_OUTCOME_SEVERITY_WARNING) return 2298;
        // Idempotent: the guard resolves before persisting and the write-out
        // stamp resolves again over the same response.
        for (gc_u32 status = SERVICE_STATUS_OK;
             status <= SERVICE_STATUS_STALE_STATE; ++status) {
            for (gc_u32 recorded = 0; recorded <= 3; ++recorded) {
                gc_u32 once = service_response_resolve_outcome_severity(status,
                    recorded);
                if (service_response_resolve_outcome_severity(status, once) !=
                    once) return 2299;
                if (!service_outcome_severity_matches_status(status, once))
                    return 2299;
            }
        }
        // The producer rule for a hardware apply: a failed step outranks
        // everything, and a committed-but-unmatched point is a warning rather
        // than a silent success.
        if (service_apply_outcome_severity(0, 0, 0) !=
            SERVICE_OUTCOME_SEVERITY_SUCCESS) return 2300;
        if (service_apply_outcome_severity(0, 2, 0) !=
            SERVICE_OUTCOME_SEVERITY_WARNING) return 2301;
        if (service_apply_outcome_severity(0, 0, 3) !=
            SERVICE_OUTCOME_SEVERITY_WARNING) return 2302;
        if (service_apply_outcome_severity(1, 0, 0) !=
            SERVICE_OUTCOME_SEVERITY_ERROR) return 2303;
        if (service_apply_outcome_severity(1, 5, 5) !=
            SERVICE_OUTCOME_SEVERITY_ERROR) return 2304;
        // A hard NVML pin is verified by NVML itself.  VF tail readback
        // mismatches are diagnostics, not a reason to warn; boost-region
        // partials and failed steps still outrank that.
        if (service_apply_outcome_severity_for_lock_mode(true, 0, 0, 3) !=
            SERVICE_OUTCOME_SEVERITY_SUCCESS) return 2330;
        if (service_apply_outcome_severity_for_lock_mode(true, 0, 2, 3) !=
            SERVICE_OUTCOME_SEVERITY_WARNING) return 2331;
        if (service_apply_outcome_severity_for_lock_mode(true, 1, 0, 3) !=
            SERVICE_OUTCOME_SEVERITY_ERROR) return 2332;
        if (service_apply_outcome_severity_for_lock_mode(false, 0, 0, 3) !=
            SERVICE_OUTCOME_SEVERITY_WARNING) return 2333;
    }

    // F-OFFSET-GRID: the driver snaps a clock offset to a per-domain grid and
    // reports the snapped value while still returning NVML_SUCCESS. Measured on
    // an RTX 5070 / driver 610.43.03. Demanding an exact readback made every
    // odd memory offset roll the whole apply back.
    {
        if (nvml_clock_offset_grid_step(NVML_CLOCK_MEM) != 2) return 1700;
        if (nvml_clock_offset_grid_step(NVML_CLOCK_GRAPHICS) != 1) return 1701;
        // Exact round-trips always match.
        for (int value = -6; value <= 8; ++value)
            if (!nvml_clock_offset_readback_matches(value, value, 1))
                return 1702;
        // The measured memory grid: snaps toward zero, both directions.
        const int memPairs[][2] = {
            {15, 14}, {1, 0}, {3, 2}, {5, 4}, {7, 6},
            {-5, -4}, {-3, -2}, {-1, 0},
        };
        for (const auto& pair : memPairs)
            if (!nvml_clock_offset_readback_matches(pair[0], pair[1], 2))
                return 1703;
        // A snap of a full grid step or more is NOT the driver honouring the
        // request and must still fail closed.
        if (nvml_clock_offset_readback_matches(15, 13, 2)) return 1704;
        if (nvml_clock_offset_readback_matches(30, 0, 2)) return 1705;
        // Overshoot and sign flips must fail closed too.
        if (nvml_clock_offset_readback_matches(14, 15, 2)) return 1706;
        if (nvml_clock_offset_readback_matches(-1, 1, 2)) return 1707;
        if (nvml_clock_offset_readback_matches(1, -1, 2)) return 1708;
        // On the exact graphics grid an odd readback is a real failure.
        if (nvml_clock_offset_readback_matches(15, 14, 1)) return 1709;
    }

    // F-INTENT-READBACK: active desired settings are ownership/configuration
    // metadata, not proof of the current hardware state. The shared comparison
    // powers both the TUI warning and text/JSON live exports.
    {
        if (nvml_configured_clock_offset_pstate() != NVML_PSTATE_0)
            return 1710;
        ServiceResponse live = fake_ready_service_response(31, 4, 2);
        live.state.activeDesiredValid = 1;
        live.desired.hasGpuOffset = 1;
        live.desired.gpuOffsetMHz = 225;
        live.desired.hasMemOffset = 1;
        live.desired.memOffsetMHz = 3000;
        live.desired.hasPowerLimit = 1;
        live.desired.powerLimitPct = 90;
        live.desired.hasFan = 1;
        live.desired.fanMode = FAN_MODE_FIXED;
        live.desired.fanPercent = 45;
        live.snapshot.gpuOffsetRangeKnown = 1;
        live.snapshot.memOffsetRangeKnown = 1;
        live.snapshot.gpuClockOffsetkHz = 225000;
        live.snapshot.memClockOffsetkHz = 3000000;
        live.snapshot.powerLimitDefaultmW = 250000;
        live.snapshot.powerLimitCurrentmW = 225000;
        live.snapshot.fanSupported = 1;
        live.snapshot.fanRangeKnown = 1;
        live.snapshot.fanMinPct = 30;
        live.snapshot.fanMaxPct = 100;
        live.snapshot.fanCount = 2;
        live.snapshot.fanPolicy[0] = NVML_FAN_POLICY_MANUAL;
        live.snapshot.fanPolicy[1] = NVML_FAN_POLICY_MANUAL;
        live.snapshot.fanTargetPercent[0] = 45;
        live.snapshot.fanTargetPercent[1] = 45;
        live.controlState.gpuOffsetMHz = 225;
        live.controlState.gpuOffsetReadbackValid = 1;
        live.controlState.memOffsetMHz = 3000;
        live.controlState.memOffsetReadbackValid = 1;
        live.controlState.powerLimitPct = 90;
        live.controlState.powerLimitReadbackValid = 1;
        live.controlState.fanPolicyReadbackValid = 1;
        live.controlState.fanTargetReadbackValid = 1;
        IntentReadbackStatus status = compare_intent_to_readback(&live);
        gc_u32 independent = SERVICE_MUTATION_DOMAIN_GPU_OFFSET |
            SERVICE_MUTATION_DOMAIN_MEM_OFFSET |
            SERVICE_MUTATION_DOMAIN_POWER |
            SERVICE_MUTATION_DOMAIN_FAN;
        if (status.requestedDomains != independent ||
            status.checkedDomains != independent ||
            status.divergedDomains != 0 ||
            status.unavailableDomains != 0 ||
            !intent_readback_matches(&status)) return 1711;

        // LACT-like replacement of only the memory offset is disclosed as a
        // memory override; configured intent itself remains unchanged.
        live.controlState.memOffsetMHz = 0;
        live.snapshot.memClockOffsetkHz = 0;
        status = compare_intent_to_readback(&live);
        if (status.divergedDomains != SERVICE_MUTATION_DOMAIN_MEM_OFFSET ||
            live.desired.memOffsetMHz != 3000 ||
            intent_readback_matches(&status)) return 1712;

        // An unreadable domain is partial/unknown, never silently "matching".
        live.controlState.memOffsetMHz = 3000;
        live.controlState.memOffsetReadbackValid = 0;
        status = compare_intent_to_readback(&live);
        if (status.divergedDomains != 0 ||
            status.unavailableDomains != SERVICE_MUTATION_DOMAIN_MEM_OFFSET)
            return 1713;

        // A curve-backed GPU offset compares only owned points. A reset curve
        // differs by hundreds of MHz and is detected, while excluded points do
        // not participate.
        live.controlState.memOffsetReadbackValid = 1;
        live.desired.gpuOffsetExcludeLowCount = 1;
        live.snapshot.freqOffsets[7] = 0;
        live.snapshot.freqOffsets[11] = 225000;
        status = compare_intent_to_readback(&live);
        gc_u32 curveRequested = (independent &
            ~((gc_u32)SERVICE_MUTATION_DOMAIN_GPU_OFFSET)) |
            SERVICE_MUTATION_DOMAIN_VF_CURVE;
        if (status.requestedDomains != curveRequested ||
            status.divergedDomains != 0 ||
            status.maxVfDeltaMHz != 0) return 1714;
        live.snapshot.freqOffsets[11] = 0;
        status = compare_intent_to_readback(&live);
        if ((status.divergedDomains &
                SERVICE_MUTATION_DOMAIN_VF_CURVE) == 0 ||
            status.maxVfDeltaMHz != 225) return 1715;

        // Absolute targets tolerate ordinary boost/temperature drift, but not
        // a reset-sized departure.
        live.desired.gpuOffsetExcludeLowCount = 0;
        live.desired.hasGpuOffset = 0;
        live.desired.hasCurvePoint[11] = 1;
        live.desired.curvePointMHz[11] = 1800;
        live.snapshot.curve[11].freq_kHz = 1820000;
        status = compare_intent_to_readback(&live);
        if (status.divergedDomains != 0 ||
            status.maxVfDeltaMHz != 20) return 1716;
        live.snapshot.curve[11].freq_kHz = 1700000;
        status = compare_intent_to_readback(&live);
        if ((status.divergedDomains &
                SERVICE_MUTATION_DOMAIN_VF_CURVE) == 0 ||
            status.maxVfDeltaMHz != 100) return 1717;

        live.state.activeDesiredValid = 0;
        status = compare_intent_to_readback(&live);
        if (status.requestedDomains || status.checkedDomains ||
            status.divergedDomains || status.unavailableDomains)
            return 1718;
    }

    // F-INTENT-READBACK-FAN: an external controller that puts the fans back on
    // the automatic curve is an override, not an unknown. Windows only reads
    // the target duty while a fan is manual, so requiring a target readback
    // before reporting a policy takeover would have hidden exactly the case
    // the feature exists to disclose.
    {
        ServiceResponse live = fake_ready_service_response(31, 4, 2);
        live.state.activeDesiredValid = 1;
        live.desired.hasFan = 1;
        live.desired.fanMode = FAN_MODE_FIXED;
        live.desired.fanPercent = 55;
        live.snapshot.fanSupported = 1;
        live.snapshot.fanRangeKnown = 1;
        live.snapshot.fanMinPct = 30;
        live.snapshot.fanMaxPct = 100;
        live.snapshot.fanCount = 2;
        live.snapshot.fanPolicy[0] = NVML_FAN_POLICY_MANUAL;
        live.snapshot.fanPolicy[1] = NVML_FAN_POLICY_MANUAL;
        live.snapshot.fanTargetPercent[0] = 55;
        live.snapshot.fanTargetPercent[1] = 55;
        live.controlState.fanPolicyReadbackValid = 1;
        live.controlState.fanTargetReadbackValid = 1;
        IntentReadbackStatus status = compare_intent_to_readback(&live);
        if (status.requestedDomains != SERVICE_MUTATION_DOMAIN_FAN ||
            status.checkedDomains != SERVICE_MUTATION_DOMAIN_FAN ||
            status.divergedDomains != 0 ||
            status.unavailableDomains != 0) return 1730;

        // Policy taken over and the target getter went quiet with it.
        live.snapshot.fanPolicy[0] = NVML_FAN_POLICY_TEMPERATURE_CONTINOUS_SW;
        live.snapshot.fanPolicy[1] = NVML_FAN_POLICY_TEMPERATURE_CONTINOUS_SW;
        live.controlState.fanTargetReadbackValid = 0;
        status = compare_intent_to_readback(&live);
        if (status.divergedDomains != SERVICE_MUTATION_DOMAIN_FAN ||
            status.unavailableDomains != 0 ||
            intent_readback_matches(&status)) return 1731;

        // Policy still matches, but the duty genuinely cannot be read: that is
        // unavailable, never a match.
        live.snapshot.fanPolicy[0] = NVML_FAN_POLICY_MANUAL;
        live.snapshot.fanPolicy[1] = NVML_FAN_POLICY_MANUAL;
        status = compare_intent_to_readback(&live);
        if (status.divergedDomains != 0 ||
            status.unavailableDomains != SERVICE_MUTATION_DOMAIN_FAN ||
            intent_readback_matches(&status)) return 1732;

        // An unreadable policy leaves the whole domain unavailable.
        live.controlState.fanTargetReadbackValid = 1;
        live.controlState.fanPolicyReadbackValid = 0;
        status = compare_intent_to_readback(&live);
        if (status.checkedDomains != 0 ||
            status.unavailableDomains != SERVICE_MUTATION_DOMAIN_FAN)
            return 1733;

        // An AUTO intent needs no target readback at all.
        live.controlState.fanPolicyReadbackValid = 1;
        live.controlState.fanTargetReadbackValid = 0;
        live.desired.fanMode = FAN_MODE_AUTO;
        live.snapshot.fanPolicy[0] = NVML_FAN_POLICY_TEMPERATURE_CONTINOUS_SW;
        live.snapshot.fanPolicy[1] = NVML_FAN_POLICY_TEMPERATURE_CONTINOUS_SW;
        status = compare_intent_to_readback(&live);
        if (status.checkedDomains != SERVICE_MUTATION_DOMAIN_FAN ||
            status.divergedDomains != 0 ||
            status.unavailableDomains != 0 ||
            !intent_readback_matches(&status)) return 1734;
    }

    // F-WIN-READBACK: the Windows ControlState publisher. This is the producer
    // side of the same v14 contract the Linux daemon implements; without it the
    // service ships all-zero validity while still substituting intent for
    // failed reads. The logic is a pure header precisely so it is covered here
    // rather than only on a Windows host.
    {
        ControlState state = {};
        ControlReadbackFacts facts = {};
        bool policyKnown[MAX_GPU_FANS] = {};
        bool targetKnown[MAX_GPU_FANS] = {};
        facts.gpuOffsetFromHardware = true;
        facts.memOffsetRead = true;
        facts.powerRead = true;
        facts.powerDefaultmW = 250000;
        facts.powerCurrentmW = 225000;
        facts.fanSupported = true;
        facts.fanCount = 2;
        policyKnown[0] = policyKnown[1] = true;
        targetKnown[0] = targetKnown[1] = true;
        facts.fanPolicyKnown = policyKnown;
        facts.fanTargetKnown = targetKnown;
        apply_control_readback_validity(&state, &facts);
        if (!state.gpuOffsetReadbackValid || !state.memOffsetReadbackValid ||
            !state.powerLimitReadbackValid || !state.fanPolicyReadbackValid ||
            !state.fanTargetReadbackValid) return 1740;

        // One silent fan makes the whole domain unknown: the comparison spans
        // every fan, so a partial answer must not selectively match.
        targetKnown[1] = false;
        apply_control_readback_validity(&state, &facts);
        if (state.fanTargetReadbackValid || !state.fanPolicyReadbackValid)
            return 1741;
        targetKnown[1] = true;

        // A percentage derived from a missing default is arithmetic, not a read.
        facts.powerDefaultmW = 0;
        apply_control_readback_validity(&state, &facts);
        if (state.powerLimitReadbackValid) return 1742;
        facts.powerDefaultmW = 250000;

        // Intent-derived GPU offsets are never published as readback.
        facts.gpuOffsetFromHardware = false;
        apply_control_readback_validity(&state, &facts);
        if (state.gpuOffsetReadbackValid) return 1743;

        // No fans present cannot be a fan readback.
        facts.fanSupported = false;
        apply_control_readback_validity(&state, &facts);
        if (state.fanPolicyReadbackValid || state.fanTargetReadbackValid)
            return 1744;
        facts.fanSupported = true;
        facts.fanCount = 0;
        apply_control_readback_validity(&state, &facts);
        if (state.fanPolicyReadbackValid) return 1745;

        // A rollback zeroes the cached scalars; the validity must go with them
        // so a failed re-read cannot publish an invented "reset to stock".
        HardwareReadbackValidity validity = {};
        validity.gpuOffset = validity.memOffset = true;
        validity.powerLimit = validity.pstate = true;
        validity.fan.policy[0] = true;
        invalidate_scalar_readbacks(&validity);
        if (validity.gpuOffset || validity.memOffset || validity.powerLimit ||
            validity.pstate) return 1746;
        // Fan provenance is refreshed wholesale by the fan read, not here.
        if (!validity.fan.policy[0]) return 1747;

        // Clock-offset detection replaces the GPU scalar unconditionally, so it
        // owns the answer: no populated curve and no Pstates20 read means the
        // published 0 is not a reading.
        if (gpu_offset_readback_after_detection(true, 0, false)) return 1748;
        if (!gpu_offset_readback_after_detection(true, 12, false)) return 1749;
        if (!gpu_offset_readback_after_detection(false, 0, true)) return 1750;
        if (gpu_offset_readback_after_detection(false, 12, false)) return 1751;
    }

    // F-RESET-FLAGS: RESET carries no flags. The interactive bit is only ever
    // consumed on the APPLY path, and the RESET validator rejects any flag, so
    // a client that sets it unconditionally has every Reset refused with
    // "invalid protocol fields" -- which is exactly what the Linux client did,
    // leaving Reset broken in both the CLI and the TUI.
    {
        if (service_request_flags_for_command(SERVICE_CMD_RESET, true) != 0u)
            return 1690;
        if (service_request_flags_for_command(SERVICE_CMD_RESET, false) != 0u)
            return 1691;
        if (service_request_flags_for_command(SERVICE_CMD_APPLY, true) !=
            SERVICE_REQUEST_FLAG_INTERACTIVE) return 1692;
        if (service_request_flags_for_command(SERVICE_CMD_APPLY, false) != 0u)
            return 1693;
        // An interactive RESET built through the helper must validate.
        ServiceRequest reset = {};
        reset.magic = SERVICE_PROTOCOL_MAGIC;
        reset.version = SERVICE_PROTOCOL_VERSION;
        reset.command = SERVICE_CMD_RESET;
        reset.callerPid = 1;
        reset.operationId = 99;
        reset.expectedServiceInstanceId = 7;
        reset.expectedGpuGeneration = 2;
        reset.expectedTopologySignature = 3;
        reset.targetGpu.valid = 1;
        reset.targetGpu.pciInfoValid = 1;
        reset.flags = service_request_flags_for_command(SERVICE_CMD_RESET, true);
        if (!validate_service_request_for_ipc(&reset)) return 1694;
        // The old unconditional behaviour must still be rejected.
        reset.flags = SERVICE_REQUEST_FLAG_INTERACTIVE;
        if (validate_service_request_for_ipc(&reset)) return 1695;
    }

    // Protocol-v13 request validation, mutation preconditions, and field layout.
    {
        if (SERVICE_PROTOCOL_MAGIC != 0x47535643u) return 80;
        if (SERVICE_PROTOCOL_VERSION != 21) return 81;
        // These are release gates, not incidental layout observations. A field
        // addition that changes a fixed-size IPC structure must bump the wire
        // version; otherwise mixed old/new peers pass the header handshake and
        // then disagree on the number of body bytes to read.
        if (sizeof(ServiceRequest) != 1416 ||
            sizeof(ControlState) != 176 ||
            sizeof(DesiredSettings) != 824 ||
            sizeof(ServiceSnapshot) != 4224 ||
            sizeof(ServiceResponse) != 7048) return 4521;
        if (offsetof(ServiceRequest, expectedServiceInstanceId) <=
            offsetof(ServiceRequest, operationId) ||
            offsetof(ServiceResponse, state) <=
            offsetof(ServiceResponse, serviceVersion)) return 1103;
        ServiceRequest request = {};
        request.magic = SERVICE_PROTOCOL_MAGIC;
        request.version = SERVICE_PROTOCOL_VERSION;
        request.command = SERVICE_CMD_PING;
        request.callerPid = 1;
        if (!validate_service_request_for_ipc(&request)) return 1104;
        request.command = SERVICE_CMD_APPLY;
        request.operationId = 55;
        request.applyOrigin = SERVICE_APPLY_ORIGIN_GUI;
        request.expectedServiceInstanceId = 7;
        if (validate_service_request_for_ipc(&request)) return 1105;
        request.expectedGpuGeneration = 2;
        request.expectedTopologySignature = 3;
        if (validate_service_request_for_ipc(&request)) return 1106;
        request.targetGpu.valid = 1;
        request.targetGpu.pciInfoValid = 1;
        if (!validate_service_request_for_ipc(&request)) return 1107;
        request.flags = 0x80000000u;
        if (validate_service_request_for_ipc(&request)) return 1108;
        request.flags = 0;
        request.desired.hasFan = 2;
        if (validate_service_request_for_ipc(&request)) return 1109;
        request.desired.hasFan = 0;
        memset(request.source, 'x', sizeof(request.source));
        if (validate_service_request_for_ipc(&request)) return 1110;

        ServiceRequest scoped = {};
        scoped.magic = SERVICE_PROTOCOL_MAGIC;
        scoped.version = SERVICE_PROTOCOL_VERSION;
        scoped.command = SERVICE_CMD_APPLY;
        scoped.callerPid = 1;
        scoped.operationId = 77;
        scoped.applyOrigin = SERVICE_APPLY_ORIGIN_GUI;
        scoped.desired.hasPowerLimit = true;
        scoped.desired.powerLimitPct = 90;
        if (validate_service_request_for_ipc(&scoped)) return 1254;
        scoped.expectedServiceInstanceId = 7;
        scoped.expectedGpuGeneration = 3;
        scoped.targetGpu = fake_ready_service_response(7, 3, 1)
            .snapshot.adapters[0];
        if (!validate_service_request_for_ipc(&scoped)) return 1255;
        scoped.desired.hasCurvePoint[7] = true;
        scoped.desired.curvePointMHz[7] = 1700;
        if (validate_service_request_for_ipc(&scoped)) return 1256;
        scoped.expectedTopologySignature = 99;
        if (!validate_service_request_for_ipc(&scoped)) return 1257;
        scoped.command = SERVICE_CMD_RESET;
        scoped.desired = {};
        if (!validate_service_request_for_ipc(&scoped)) return 1258;
        scoped.expectedTopologySignature = 0;
        if (validate_service_request_for_ipc(&scoped)) return 1259;
    }

    // Server-derived mutation domains remain atomic while the editor may
    // construct a genuinely independent degraded draft. Sparse successful
    // applies merge into, rather than erase, durable intent in other domains.
    {
        DesiredSettings desired = {};
        desired.hasPowerLimit = true;
        desired.powerLimitPct = 90;
        if (service_desired_mutation_domains(&desired) !=
                SERVICE_MUTATION_DOMAIN_POWER) return 1200;
        desired = {};
        desired.hasGpuOffset = true;
        desired.gpuOffsetMHz = 100;
        if (service_desired_mutation_domains(&desired) !=
                SERVICE_MUTATION_DOMAIN_GPU_OFFSET) return 1201;
        desired.gpuOffsetExcludeLowCount = 8;
        if (service_desired_mutation_domains(&desired) !=
                SERVICE_MUTATION_DOMAIN_VF_CURVE) return 1202;
        desired = {};
        desired.hasSysClkOffsetKhz = true;
        desired.sysClkOffsetKhz = 50000;
        if (service_desired_mutation_domains(&desired) !=
                SERVICE_MUTATION_DOMAIN_SYS_CLK) return 4530;
        desired = {};
        desired.hasLock = true;
        desired.lockMode = LOCK_MODE_HARD;
        desired.lockCi = 32;
        desired.lockMHz = 2200;
        if (service_desired_mutation_domains(&desired) !=
                (SERVICE_MUTATION_DOMAIN_VF_CURVE |
                 SERVICE_MUTATION_DOMAIN_LOCK)) return 1203;

        DesiredSettings projected = {};
        projected.resetOcBeforeApply = true;
        projected.hasGpuOffset = true;
        projected.gpuOffsetMHz = 150;
        projected.gpuOffsetExcludeLowCount = 4;
        projected.hasMemOffset = true;
        projected.memOffsetMHz = 700;
        projected.hasPowerLimit = true;
        projected.powerLimitPct = 85;
        projected.hasCurvePoint[12] = true;
        projected.curvePointMHz[12] = 1900;
        projected.hasLock = true;
        projected.lockMode = LOCK_MODE_FLATTEN;
        projected.lockCi = 12;
        projected.lockMHz = 1900;
        projected.hasFan = true;
        projected.fanMode = FAN_MODE_FIXED;
        projected.fanPercent = 55;
        service_project_desired_to_available_domains(&projected,
            SERVICE_MUTATION_DOMAIN_MEM_OFFSET |
            SERVICE_MUTATION_DOMAIN_POWER);
        if (projected.resetOcBeforeApply || projected.hasGpuOffset ||
            !projected.hasMemOffset || !projected.hasPowerLimit ||
            projected.hasCurvePoint[12] || projected.hasLock ||
            projected.hasFan ||
            service_desired_mutation_domains(&projected) !=
                (SERVICE_MUTATION_DOMAIN_MEM_OFFSET |
                 SERVICE_MUTATION_DOMAIN_POWER)) return 1204;

        DesiredSettings previous = {};
        previous.hasCurvePoint[20] = true;
        previous.curvePointMHz[20] = 2000;
        previous.hasFan = true;
        previous.fanMode = FAN_MODE_FIXED;
        previous.fanPercent = 50;
        DesiredSettings powerOnly = {};
        powerOnly.hasPowerLimit = true;
        powerOnly.powerLimitPct = 80;
        powerOnly.resetOcBeforeApply = false;
        DesiredSettings merged = service_merge_desired_after_mutation(
            &previous, &powerOnly);
        if (!merged.hasCurvePoint[20] || merged.curvePointMHz[20] != 2000 ||
            !merged.hasFan || merged.fanPercent != 50 ||
            !merged.hasPowerLimit || merged.powerLimitPct != 80 ||
            merged.resetOcBeforeApply) return 1205;

        DesiredSettings vfOnly = {};
        vfOnly.hasCurvePoint[7] = true;
        vfOnly.curvePointMHz[7] = 1700;
        DesiredSettings vfMerged = service_merge_desired_after_mutation(
            &previous, &vfOnly);
        if (vfMerged.hasCurvePoint[20] ||
            vfMerged.curvePointMHz[20] != 0 ||
            !vfMerged.hasCurvePoint[7] ||
            vfMerged.curvePointMHz[7] != 1700 ||
            !vfMerged.hasFan || vfMerged.fanPercent != 50) return 1260;
        DesiredSettings resetOnly = {};
        resetOnly.resetOcBeforeApply = true;
        DesiredSettings resetMerged = service_merge_desired_after_mutation(
            &previous, &resetOnly);
        if (resetMerged.hasCurvePoint[20] ||
            resetMerged.curvePointMHz[20] != 0 ||
            !resetMerged.hasFan || resetMerged.fanPercent != 50 ||
            resetMerged.resetOcBeforeApply) return 1720;
        DesiredSettings mixed = powerOnly;
        mixed.hasCurvePoint[7] = true;
        mixed.curvePointMHz[7] = 1700;
        if (service_unavailable_mutation_domains(
                SERVICE_CMD_APPLY, &powerOnly,
                SERVICE_MUTATION_DOMAIN_POWER) != 0 ||
            service_unavailable_mutation_domains(
                SERVICE_CMD_APPLY, &vfOnly,
                SERVICE_MUTATION_DOMAIN_POWER) !=
                    SERVICE_MUTATION_DOMAIN_VF_CURVE ||
            service_unavailable_mutation_domains(
                SERVICE_CMD_APPLY, &mixed,
                SERVICE_MUTATION_DOMAIN_POWER) !=
                    SERVICE_MUTATION_DOMAIN_VF_CURVE ||
            service_unavailable_mutation_domains(
                SERVICE_CMD_RESET, nullptr,
                SERVICE_MUTATION_DOMAIN_POWER) !=
                    (SERVICE_MUTATION_DOMAIN_ALL &
                     ~SERVICE_MUTATION_DOMAIN_POWER)) return 1248;

        ServiceResponse authority = fake_ready_service_response(29, 6, 3);
        const GpuAdapterInfo* currentGpu = &authority.snapshot.adapters[0];
        ServiceRequest attached = {};
        attached.command = SERVICE_CMD_APPLY;
        attached.expectedServiceInstanceId = authority.state.serviceInstanceId;
        attached.expectedGpuGeneration = authority.state.gpuGeneration;
        attached.expectedTopologySignature =
            authority.state.topologySignature + 1;
        attached.targetGpu = *currentGpu;
        attached.desired = powerOnly;
        if (!linux_mutation_authority_matches(&attached,
                authority.state.serviceInstanceId,
                authority.state.gpuGeneration,
                authority.state.topologySignature,
                currentGpu, SERVICE_MUTATION_DOMAIN_POWER)) return 1249;
        attached.desired = vfOnly;
        if (linux_mutation_authority_matches(&attached,
                authority.state.serviceInstanceId,
                authority.state.gpuGeneration,
                authority.state.topologySignature,
                currentGpu, SERVICE_MUTATION_DOMAIN_VF_CURVE)) return 1250;
        attached.expectedTopologySignature = authority.state.topologySignature;
        if (!linux_mutation_authority_matches(&attached,
                authority.state.serviceInstanceId,
                authority.state.gpuGeneration,
                authority.state.topologySignature,
                currentGpu, SERVICE_MUTATION_DOMAIN_VF_CURVE)) return 1251;
        attached.command = SERVICE_CMD_RESET;
        attached.expectedTopologySignature = 0;
        if (linux_mutation_authority_matches(&attached,
                authority.state.serviceInstanceId,
                authority.state.gpuGeneration,
                authority.state.topologySignature,
                currentGpu, SERVICE_MUTATION_DOMAIN_ALL)) return 1252;
    }

    // The v12 health payload is bounded and typed; invalid enum, boolean,
    // string, or domain values never become authoritative client state.
    {
        ServiceResponse valid = fake_ready_service_response(30, 2, 1);
        ServiceResponse malformed = valid;
        malformed.snapshot.health.reason =
            SERVICE_GPU_HEALTH_STATE_UNCERTAIN + 1;
        if (validate_service_response_for_ipc(&malformed)) return 1206;
        malformed = valid;
        malformed.snapshot.health.architectureSource =
            SERVICE_GPU_ARCH_SOURCE_FUTURE_GUESS + 1;
        if (validate_service_response_for_ipc(&malformed)) return 1207;
        malformed = valid;
        memset(malformed.snapshot.health.detail, 'x',
               sizeof(malformed.snapshot.health.detail));
        if (validate_service_response_for_ipc(&malformed)) return 1208;
        malformed = valid;
        malformed.snapshot.health.capabilityDomainsPacked =
            SERVICE_GPU_CAPABILITY_PACKED_MASK + 1u;
        if (validate_service_response_for_ipc(&malformed)) return 1209;
    }

    // Draft authority is exact by daemon instance, selected physical GPU, and
    // GPU generation. Topology fences only VF-dependent drafts, so an
    // independent power draft survives a harmless curve topology refresh.
    {
        ServiceResponse response = fake_ready_service_response(31, 2, 4);
        DesiredSettings power = {};
        power.hasPowerLimit = true;
        power.powerLimitPct = 85;
        LinuxTuiDraftBinding powerBinding = {};
        if (!linux_tui_bind_draft(&powerBinding, &response, &power) ||
            !linux_tui_draft_binding_matches(&powerBinding, &response, &power))
            return 1210;
        ServiceResponse topologyChanged = response;
        topologyChanged.snapshot.curve[7].volt_uV += 1000;
        topologyChanged.state.topologySignature =
            service_snapshot_topology_signature(&topologyChanged.snapshot);
        if (topologyChanged.state.topologySignature ==
                response.state.topologySignature ||
            !linux_tui_draft_binding_matches(&powerBinding,
                &topologyChanged, &power)) return 1211;

        DesiredSettings vf = {};
        vf.hasCurvePoint[7] = true;
        vf.curvePointMHz[7] = 1700;
        LinuxTuiDraftBinding vfBinding = {};
        if (!linux_tui_bind_draft(&vfBinding, &response, &vf) ||
            linux_tui_draft_binding_matches(&vfBinding,
                &topologyChanged, &vf)) return 1212;
        ServiceResponse restarted = response;
        restarted.state.serviceInstanceId++;
        if (linux_tui_draft_binding_matches(&powerBinding, &restarted, &power))
            return 1213;
        ServiceResponse regenerated = response;
        regenerated.state.gpuGeneration++;
        if (linux_tui_draft_binding_matches(&powerBinding,
                &regenerated, &power)) return 1214;
        ServiceResponse otherGpu = response;
        otherGpu.snapshot.adapters[0].pciBus++;
        if (linux_tui_draft_binding_matches(&powerBinding, &otherGpu, &power))
            return 1215;

        ServiceResponse degraded = response;
        degraded.state.gpuPhase = SERVICE_GPU_PHASE_DEGRADED;
        degraded.state.validSections &=
            ~SERVICE_STATE_SECTION_CURVE_TOPOLOGY;
        degraded.state.topologySignature = 0;
        degraded.snapshot.loaded = false;
        degraded.snapshot.vfReadSupported = false;
        degraded.snapshot.vfWriteSupported = false;
        degraded.snapshot.numPopulated = 0;
        memset(degraded.snapshot.curve, 0, sizeof(degraded.snapshot.curve));
        degraded.snapshot.health.reason = SERVICE_GPU_HEALTH_VF_STATUS_FAILED;
        degraded.snapshot.health.driverStatus = -6;
        degraded.snapshot.health.vfSnapshotFresh = false;
        degraded.snapshot.health.availableMutationDomains =
            SERVICE_MUTATION_DOMAIN_POWER;
        gc_strlcpy(degraded.snapshot.health.detail,
            sizeof(degraded.snapshot.health.detail),
            "getStatus status=-6 (HANDLE_INVALIDATED)");
        if (!validate_service_response_for_ipc(&degraded)) return 1216;
        LinuxTuiDraftBinding degradedPower = {};
        if (!linux_tui_bind_draft(&degradedPower, &degraded, &power))
            return 1217;
        LinuxTuiDraftBinding degradedVf = {};
        if (linux_tui_bind_draft(&degradedVf, &degraded, &vf)) return 1218;
        if (strcmp(linux_tui_gpu_phase_name(SERVICE_GPU_PHASE_RECOVERING),
                   "recovering") != 0 ||
            strcmp(linux_tui_gpu_phase_name(SERVICE_GPU_PHASE_DEGRADED),
                   "degraded") != 0) return 1219;
    }

    // The fixed prefix diagnoses old short peers before a version-sized body;
    // detailed EOF/timeout names are stable for UI diagnostics.
    {
        ServiceWirePrefix current = {
            SERVICE_PROTOCOL_MAGIC, SERVICE_PROTOCOL_VERSION};
        ServiceWirePrefix previous = {
            SERVICE_PROTOCOL_MAGIC, SERVICE_PROTOCOL_VERSION - 1};
        ServiceWirePrefix old = {SERVICE_PROTOCOL_MAGIC, 8};
        ServiceWirePrefix bad = {0, SERVICE_PROTOCOL_VERSION};
        if (sizeof(ServiceWirePrefix) != 8 ||
            service_wire_prefix_disposition(&current) !=
                SERVICE_WIRE_PREFIX_CURRENT ||
            service_wire_prefix_disposition(&previous) !=
                SERVICE_WIRE_PREFIX_VERSION_MISMATCH ||
            service_wire_prefix_disposition(&old) !=
                SERVICE_WIRE_PREFIX_VERSION_MISMATCH ||
            service_wire_prefix_disposition(&bad) !=
                SERVICE_WIRE_PREFIX_BAD_MAGIC ||
            strcmp(daemon_io_failure_classification(DAEMON_IO_TIMEOUT, 0),
                   "timeout") != 0 ||
            strcmp(daemon_io_failure_classification(DAEMON_IO_EOF, 0),
                   "EOF") != 0 ||
            strcmp(daemon_io_failure_classification(DAEMON_IO_EOF, 7),
                   "truncated EOF") != 0) return 1243;
        char status[256] = {};
        linux_tui_format_failure(status, sizeof(status),
            "Daemon refresh failed",
            "daemon protocol mismatch (client 12, daemon 8); reinstall/restart greencurve.service");
        if (!strstr(status, "protocol mismatch") ||
            !strstr(status, "daemon 8") ||
            strcmp(status, "Daemon unavailable") == 0) return 1246;
        LinuxDaemonPermissionFacts permission = {};
        permission.socketMetadataAvailable = true;
        permission.socketPath = "/run/greencurve/greencurve.sock";
        permission.connectError = "Permission denied";
        permission.socketOwnerUid = 0;
        permission.socketGroupName = "greencurve";
        permission.socketGroupId = 993;
        permission.socketMode = 0660;
        permission.processEuid = 1000;
        permission.processPrimaryGid = 1000;
        permission.supplementaryGreencurve = 1;
        linux_daemon_format_permission_facts(
            &permission, status, sizeof(status));
        if (!strstr(status, "owner_uid=0") ||
            !strstr(status, "group=greencurve(993)") ||
            !strstr(status, "mode=0660") ||
            !strstr(status, "primary_gid=1000") ||
            !strstr(status, "supplementary greencurve=yes")) return 1247;
    }

    // Deterministic service activation always reloads, enables, restarts,
    // verifies active state and pathname authorization, and then verifies the
    // daemon build/protocol.
    {
        char error[128] = {};
        FakeServiceActivationContext success = {};
        success.failAt = -1;
        LinuxServiceActivationResult result = linux_service_run_activation(
            fake_service_activation_runner, &success, error, sizeof(error));
        if (!result.success || result.completedSteps != LINUX_SERVICE_STEP_COUNT ||
            success.count != LINUX_SERVICE_STEP_COUNT) return 1220;
        if (LINUX_SERVICE_STEP_VERIFY_SOCKET !=
                LINUX_SERVICE_STEP_IS_ACTIVE + 1 ||
            LINUX_SERVICE_STEP_VERIFY_PROTOCOL !=
                LINUX_SERVICE_STEP_VERIFY_SOCKET + 1 ||
            !strstr(linux_service_activation_step_name(
                LINUX_SERVICE_STEP_VERIFY_SOCKET), "pathname")) return 1275;
        for (unsigned int step = 0; step < LINUX_SERVICE_STEP_COUNT; ++step)
            if (success.seen[step] != (LinuxServiceActivationStep)step)
                return 1221;
        for (int fail = 0; fail < LINUX_SERVICE_STEP_COUNT; ++fail) {
            FakeServiceActivationContext failure = {};
            failure.failAt = fail;
            error[0] = 0;
            result = linux_service_run_activation(
                fake_service_activation_runner, &failure,
                error, sizeof(error));
            if (result.success || result.failedStep != fail ||
                result.completedSteps != (unsigned int)fail ||
                failure.count != (unsigned int)fail + 1 || !error[0])
                return 1222;
        }
    }

    // F-LNX-PCI: the NVML pciInfo ABI. `kDriverPciInfo` is a byte-exact
    // capture of what nvmlDeviceGetPciInfo_v3 writes on driver 610.43.03
    // (RTX 5070, 0000:07:00.0). A leading `busId[32]` shifts every integer by
    // 16 bytes, so pciDeviceId/pciSubSystemId decode as the ASCII ":07:" /
    // "00.0" out of the trailing string and bind fails closed against NvAPI.
    {
        static const unsigned char kDriverPciInfo[68] = {
            '0','0','0','0',':','0','7',':','0','0','.','0',0,0,0,0,
            0x00,0x00,0x00,0x00,              // domain = 0
            0x07,0x00,0x00,0x00,              // bus = 7
            0x00,0x00,0x00,0x00,              // device = 0
            0xDE,0x10,0x04,0x2F,              // pciDeviceId = 0x2F0410DE
            0x43,0x10,0xE7,0x89,              // pciSubSystemId = 0x89E71043
            '0','0','0','0','0','0','0','0',':','0','7',':','0','0','.','0',
            0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
        };
        nvmlPciInfo_t pci = {};
        memcpy(&pci, kDriverPciInfo, sizeof(pci));
        if (pci.domain != 0u || pci.bus != 7u || pci.device != 0u) return 1670;
        if (pci.pciDeviceId != 0x2F0410DEu) return 1671;
        if (pci.pciSubSystemId != 0x89E71043u) return 1672;
        if (strcmp(nvml_pci_bus_id_text(&pci), "00000000:07:00.0") != 0)
            return 1673;
        // _v2 and the v1 entry point stop after the integers and never fill
        // the trailing busId; the legacy string must still parse.
        nvmlPciInfo_t legacyOnly = {};
        memcpy(&legacyOnly, kDriverPciInfo, offsetof(nvmlPciInfo_t, busId));
        if (strcmp(nvml_pci_bus_id_text(&legacyOnly), "0000:07:00.0") != 0)
            return 1674;
        // The decoded identity must bind against the NvAPI observation the
        // same driver reports for that GPU.
        GpuAdapterInfo adapter = {};
        adapter.valid = true;
        adapter.pciInfoValid = pci.pciDeviceId != 0;
        adapter.deviceId = pci.pciDeviceId;
        adapter.subSystemId = pci.pciSubSystemId;
        adapter.pciBus = pci.bus;
        adapter.pciDevice = pci.device;
        LinuxNvapiIdentityObservation observed = {};
        observed.busValid = true;
        observed.slotValid = true;
        observed.bus = 7;
        observed.slot = 0;
        observed.pciIdentityValid = true;
        observed.deviceId = 0x2F0410DEu;
        observed.extDeviceId = 0x00002F04u;
        observed.subSystemId = 0x89E71043u;
        LinuxGpuBindingDecision bound = linux_gpu_binding_decide(
            &adapter, 1, 1, &observed);
        if (bound.adapterIndex != 0 || bound.identifiersConflict ||
            bound.deviceConflict || bound.subsystemConflict) return 1675;
    }

    // NvAPI/NVML binding: multi-GPU requires one exact nonconflicting match;
    // a sole pair may use the explicit nonconflicting fallback.
    {
        GpuAdapterInfo adapters[2] = {};
        for (int index = 0; index < 2; ++index) {
            adapters[index].valid = true;
            adapters[index].pciInfoValid = true;
            adapters[index].pciBus = 4 + index;
            adapters[index].pciDevice = 0;
            adapters[index].deviceId = index == 0
                ? 0x268410DEu : 0x2B8510DEu;
            adapters[index].subSystemId = 0x1000u + index;
        }
        LinuxNvapiIdentityObservation observation = {};
        observation.busValid = true;
        observation.slotValid = true;
        observation.bus = 5;
        observation.slot = 0;
        observation.pciIdentityValid = true;
        observation.deviceId = 0x2B85u;
        observation.subSystemId = 0x1001u;
        LinuxGpuBindingDecision binding = linux_gpu_binding_decide(
            adapters, 2, 2, &observation);
        if (binding.adapterIndex != 1 ||
            binding.method != LINUX_GPU_MATCH_EXACT_PCI) return 1223;
        observation.deviceId = 0x10DE2B85u;
        binding = linux_gpu_binding_decide(adapters, 2, 2, &observation);
        if (binding.adapterIndex != 1 || binding.identifiersConflict ||
            binding.deviceConflict) return 1270;
        observation.deviceId = 0x2684u;
        binding = linux_gpu_binding_decide(adapters, 2, 2, &observation);
        if (binding.adapterIndex != -1 || binding.candidateIndex != 1 ||
            !binding.identifiersConflict || !binding.deviceConflict ||
            binding.subsystemConflict)
            return 1224;
        observation.deviceId = 0x2B85u;
        observation.pciIdentityValid = false;
        binding = linux_gpu_binding_decide(adapters, 2, 2, &observation);
        if (binding.adapterIndex != -1) return 1225;
        observation.pciIdentityValid = true;
        observation.deviceId = 0;
        binding = linux_gpu_binding_decide(adapters, 2, 2, &observation);
        if (binding.adapterIndex != -1 || binding.identifiersConflict)
            return 1276;
        observation.deviceId = 0x2684u;
        observation.extDeviceId = 0x10DE2B85u;
        binding = linux_gpu_binding_decide(adapters, 2, 2, &observation);
        if (binding.adapterIndex != 1 || binding.identifiersConflict ||
            binding.deviceConflict) return 1277;
        observation.extDeviceId = 0;
        GpuAdapterInfo ambiguous[2] = {adapters[0], adapters[1]};
        ambiguous[1].pciBus = ambiguous[0].pciBus;
        observation.bus = ambiguous[0].pciBus;
        observation.pciIdentityValid = true;
        observation.deviceId = 0x2684u;
        observation.subSystemId = ambiguous[0].subSystemId;
        binding = linux_gpu_binding_decide(ambiguous, 2, 2, &observation);
        if (binding.adapterIndex != -2) return 1226;

        observation.bus = 99;
        observation.slot = 7;
        binding = linux_gpu_binding_decide(adapters, 1, 1, &observation);
        if (binding.adapterIndex != 0 ||
            binding.method != LINUX_GPU_MATCH_SOLE_NONCONFLICTING)
            return 1227;
        observation.deviceId = 0x2B85u;
        binding = linux_gpu_binding_decide(adapters, 1, 1, &observation);
        if (binding.adapterIndex != -1 || !binding.identifiersConflict)
            return 1228;
        observation.pciIdentityValid = false;
        binding = linux_gpu_binding_decide(adapters, 1, 1, &observation);
        if (binding.adapterIndex != 0 ||
            binding.method != LINUX_GPU_MATCH_SOLE_NONCONFLICTING)
            return 1229;
        GpuAdapterInfo transient = adapters[0];
        transient.pciInfoValid = false;
        transient.deviceId = 0;
        transient.subSystemId = 0;
        if (!linux_gpu_same_pci_nonconflicting(&adapters[0], &transient))
            return 1260;
        transient.pciBus++;
        if (linux_gpu_same_pci_nonconflicting(&adapters[0], &transient))
            return 1261;

        if (!linux_gpu_device_id_matches(0x10DE2B85u, 0x2B8510DEu) ||
            !linux_gpu_device_id_matches(0x2B85u, 0x2B8510DEu) ||
            linux_gpu_device_id_matches(0x268410DEu, 0x2B8510DEu))
            return 1271;
        if (!linux_gpu_subsystem_id_matches(0x14583842u, 0x38421458u) ||
            !linux_gpu_subsystem_id_matches(0x1458u, 0x38421458u) ||
            linux_gpu_subsystem_id_matches(0x14583842u, 0x99993842u))
            return 1272;

        GpuAdapterInfo sole = adapters[0];
        sole.subSystemId = 0x14583842u;
        observation.bus = 99;
        observation.slot = 7;
        observation.pciIdentityValid = true;
        observation.deviceId = 0x10DE2684u;
        observation.subSystemId = 0x38421458u;
        binding = linux_gpu_binding_decide(&sole, 1, 1, &observation);
        if (binding.adapterIndex != 0 ||
            binding.method != LINUX_GPU_MATCH_SOLE_NONCONFLICTING ||
            binding.identifiersConflict) return 1273;
        observation.subSystemId = 0x99993842u;
        binding = linux_gpu_binding_decide(&sole, 1, 1, &observation);
        if (binding.adapterIndex != -1 || binding.candidateIndex != 0 ||
            !binding.identifiersConflict || binding.deviceConflict ||
            !binding.subsystemConflict) return 1274;
    }

    // Type=notify readiness reports only the post-replay/listen state and
    // carries enough build/protocol/health identity for systemctl diagnostics.
    {
        ServiceGpuHealth health = {};
        health.reason = SERVICE_GPU_HEALTH_VF_CONTROL_FAILED;
        gc_strlcpy(health.detail, sizeof(health.detail),
            "getControl status=-6");
        char payload[384] = {};
        // Derived, not spelled out: the payload must advertise whatever version
        // this build speaks, so a protocol bump cannot leave systemd reporting
        // a stale number with the assertion still green.
        char expectedProtocol[32] = {};
        snprintf(expectedProtocol, sizeof(expectedProtocol), "protocol %d",
                 (int)SERVICE_PROTOCOL_VERSION);
        if (!linux_systemd_build_ready_payload(
                &health, payload, sizeof(payload)) ||
            strncmp(payload, "READY=1\nSTATUS=", 15) != 0 ||
            !strstr(payload, expectedProtocol) ||
            !strstr(payload, "VF control read failed") ||
            !strstr(payload, "getControl status=-6")) return 1244;
        char tooSmall[8] = {};
        if (linux_systemd_build_ready_payload(
                &health, tooSmall, sizeof(tooSmall))) return 1245;
    }

    // Architecture is queried once, with documented NVML mapping only as a
    // fallback. Blackwell is known; unknown future NvAPI values retain the
    // future ABI probe rather than being authorized by a guessed family.
    {
        LinuxArchitectureDecision retained = linux_choose_architecture(
            0, NV_GPU_ARCHITECTURE_AD100,
            NVML_SUCCESS, NVML_DEVICE_ARCH_BLACKWELL,
            false, 0, GPU_FAMILY_UNKNOWN);
        if (retained.family != GPU_FAMILY_LOVELACE ||
            retained.architecture != NV_GPU_ARCHITECTURE_AD100 ||
            retained.source != SERVICE_GPU_ARCH_SOURCE_NVAPI) return 1230;
        LinuxArchitectureDecision blackwell = linux_choose_architecture(
            -1, 0, NVML_SUCCESS, NVML_DEVICE_ARCH_BLACKWELL,
            false, 0, GPU_FAMILY_UNKNOWN);
        if (blackwell.family != GPU_FAMILY_BLACKWELL ||
            blackwell.architecture != NV_GPU_ARCHITECTURE_GB200 ||
            blackwell.source != SERVICE_GPU_ARCH_SOURCE_NVML) return 1231;
        LinuxArchitectureDecision future = linux_choose_architecture(
            0, 0x000001D0u, NVML_ERROR_NOT_SUPPORTED,
            NVML_DEVICE_ARCH_UNKNOWN, false, 0, GPU_FAMILY_UNKNOWN);
        if (future.family != GPU_FAMILY_UNKNOWN ||
            future.architecture != 0x000001D0u ||
            future.source != SERVICE_GPU_ARCH_SOURCE_FUTURE_GUESS)
            return 1232;
        LinuxArchitectureDecision futureNvml = linux_choose_architecture(
            -1, 0, NVML_SUCCESS, NVML_DEVICE_ARCH_RUBIN,
            false, 0, GPU_FAMILY_UNKNOWN);
        if (futureNvml.family != GPU_FAMILY_UNKNOWN ||
            futureNvml.source != SERVICE_GPU_ARCH_SOURCE_FUTURE_GUESS)
            return 1262;
        LinuxArchitectureDecision cached = linux_choose_architecture(
            -6, 0, NVML_ERROR_GPU_IS_LOST, NVML_DEVICE_ARCH_UNKNOWN,
            true, NV_GPU_ARCHITECTURE_GB200, GPU_FAMILY_BLACKWELL);
        if (cached.family != GPU_FAMILY_BLACKWELL ||
            cached.source != SERVICE_GPU_ARCH_SOURCE_CACHED) return 1233;
    }

    // F-LNX-VFDOMAIN: the NvAPI status table concatenates every clock domain,
    // so the fixed 128-point read window can run past the graphics curve.
    // Values are the real RTX 5070 / driver 610.43.03 table: graphics is 127
    // points, index 127 starts a second domain at 405 MHz @ 540 mV, 128 a
    // third, and 129..131 are the ~14 GHz GDDR7 memory domain.  Before this
    // rule those foreign points broke the ordered-voltage invariant and failed
    // the whole snapshot closed, leaving the GPU with no VF read or write.
    {
        VFCurvePoint table[VF_NUM_POINTS] = {};
        for (int index = 0; index < 127; ++index) {
            table[index].freq_kHz = 180000u + (unsigned int)index * 23000u;
            table[index].volt_uV = 450000u + (unsigned int)index * 6000u;
        }
        // Index 127: first point of the next clock domain.
        table[127].freq_kHz = 405000u;
        table[127].volt_uV = 540000u;
        if (linux_vf_graphics_domain_length(table, VF_NUM_POINTS) != 127)
            return 1680;
        // The truncated curve must then validate; the untruncated one must not.
        int offsets[VF_NUM_POINTS] = {};
        unsigned char mask[32] = {};
        for (int index = 0; index < VF_NUM_POINTS; ++index)
            mask[index / 8] |= (unsigned char)(1u << (index % 8));
        char domainWhy[160] = {};
        if (linux_vf_snapshot_structurally_valid(
                true, true, true, mask, sizeof(mask), 15,
                table, offsets, 128, domainWhy, sizeof(domainWhy)))
            return 1681;
        VFCurvePoint truncated[VF_NUM_POINTS] = {};
        memcpy(truncated, table, sizeof(truncated));
        truncated[127].freq_kHz = 0;
        truncated[127].volt_uV = 0;
        if (!linux_vf_snapshot_structurally_valid(
                true, true, true, mask, sizeof(mask), 15,
                truncated, offsets, 127, domainWhy, sizeof(domainWhy)))
            return 1682;
        // A curve that fills the window with no foreign domain keeps all 128.
        VFCurvePoint full[VF_NUM_POINTS] = {};
        for (int index = 0; index < VF_NUM_POINTS; ++index) {
            full[index].freq_kHz = 600000u + (unsigned int)index * 15000u;
            full[index].volt_uV = 700000u + (unsigned int)index * 5000u;
        }
        if (linux_vf_graphics_domain_length(full, VF_NUM_POINTS) != VF_NUM_POINTS)
            return 1683;
        // Equal voltages are a legitimate plateau, not a boundary, and an
        // unpopulated hole must not end the domain either.
        full[40].volt_uV = full[39].volt_uV;
        full[41].freq_kHz = 0;
        full[41].volt_uV = 0;
        if (linux_vf_graphics_domain_length(full, VF_NUM_POINTS) != VF_NUM_POINTS)
            return 1684;
    }

    // Atomic VF validation accepts a legitimate non-monotonic live frequency
    // curve, but rejects incomplete reads, malformed voltage topology,
    // implausible values, count drift, and rejected structure versions.
    {
        VFCurvePoint curve[VF_NUM_POINTS] = {};
        int offsets[VF_NUM_POINTS] = {};
        unsigned char mask[32] = {};
        for (int index = 0; index < 16; ++index) {
            curve[index].freq_kHz = 600000u + (unsigned int)index * 50000u;
            curve[index].volt_uV = 700000u + (unsigned int)index * 10000u;
            mask[index / 8] |= (unsigned char)(1u << (index % 8));
        }
        curve[9].freq_kHz = curve[8].freq_kHz - 25000u;
        char why[160] = {};
        if (!linux_vf_snapshot_structurally_valid(
                true, true, true, mask, sizeof(mask), 15,
                curve, offsets, 16, why, sizeof(why))) return 1234;
        VFCurvePoint malformed[VF_NUM_POINTS] = {};
        memcpy(malformed, curve, sizeof(curve));
        malformed[10].volt_uV = malformed[9].volt_uV - 1000u;
        if (linux_vf_snapshot_structurally_valid(
                true, true, true, mask, sizeof(mask), 15,
                malformed, offsets, 16, why, sizeof(why))) return 1235;
        memcpy(malformed, curve, sizeof(curve));
        malformed[4].freq_kHz = 7000000u;
        if (linux_vf_snapshot_structurally_valid(
                true, true, true, mask, sizeof(mask), 15,
                malformed, offsets, 16, why, sizeof(why))) return 1236;
        if (linux_vf_snapshot_structurally_valid(
                true, true, false, mask, sizeof(mask), 15,
                curve, offsets, 16, why, sizeof(why))) return 1237;
        if (!linux_vf_snapshot_authoritative(true, true, true, true, true) ||
            linux_vf_snapshot_authoritative(true, true, false, true, true) ||
            linux_vf_snapshot_authoritative(true, true, true, true, false))
            return 1253;
        if (linux_vf_snapshot_structurally_valid(
                true, true, true, mask, sizeof(mask), 15,
                curve, offsets, 15, why, sizeof(why))) return 1238;
        if (!linux_vf_returned_version_valid((1u << 16) | 0x182Cu,
                1, 0x182C) ||
            linux_vf_returned_version_valid((2u << 16) | 0x182Cu,
                1, 0x182C) ||
            linux_vf_returned_version_valid((1u << 16) | 0x1820u,
                1, 0x182C)) return 1239;
    }

    // Topology is stable across telemetry and adapter ordinal reorder, but
    // detects every populated-map entry (including same-count map changes).
    {
        ServiceResponse base = fake_ready_service_response(8, 1, 1);
        gc_u64 signature = base.state.topologySignature;
        ServiceSnapshot telemetry = base.snapshot;
        telemetry.curve[7].freq_kHz += 500000;
        if (service_snapshot_topology_signature(&telemetry) != signature)
            return 1111;
        ServiceSnapshot changedMap = base.snapshot;
        changedMap.curve[11] = {};
        changedMap.curve[12].freq_kHz = 1800000;
        changedMap.curve[12].volt_uV = 900000;
        if (service_snapshot_topology_signature(&changedMap) == signature)
            return 1112;
        ServiceSnapshot reordered = base.snapshot;
        reordered.adapterCount = 2;
        reordered.selectedAdapterIndex = 1;
        reordered.adapters[1] = reordered.adapters[0];
        reordered.adapters[1].nvapiIndex = 0;
        reordered.adapters[0] = {};
        reordered.adapters[0].valid = 1;
        reordered.adapters[0].deviceId = 0x999910DEu;
        if (service_snapshot_topology_signature(&reordered) != signature)
            return 1113;
        ServiceResponse empty = base;
        memset(empty.snapshot.curve, 0, sizeof(empty.snapshot.curve));
        empty.snapshot.numPopulated = 0;
        empty.state.topologySignature =
            service_snapshot_topology_signature(&empty.snapshot);
        if (validate_service_response_for_ipc(&empty)) return 1114;
    }

    // Pure GUI reducer: coherent reconnect sequence, stale completion fences,
    // service restart retirement, partial validity, capabilities, and drafts.
    {
        GuiServiceModel model = {};
        gui_service_model_initialize(&model);
        if (model.phase != GUI_SERVICE_DISCONNECTED) return 1115;
        gui_service_model_begin_sync(&model, 1);
        if (model.phase != GUI_SERVICE_SYNCING) return 1116;
        ServiceResponse ready = fake_ready_service_response(21, 1, 1);
        if (gui_service_model_accept(&model, 1, &ready.state) !=
                GUI_SERVICE_ENVELOPE_ACCEPTED ||
            model.phase != GUI_SERVICE_READY) return 1117;
        if (gui_service_model_accept(&model, 1, &ready.state) !=
                GUI_SERVICE_ENVELOPE_REJECTED_REVISION) return 1118;

        ServiceStateEnvelope missing = ready.state;
        missing.stateRevision = 2;
        missing.gpuGeneration = 2;
        missing.gpuPhase = SERVICE_GPU_PHASE_DEVICE_MISSING;
        missing.validSections = SERVICE_STATE_SECTION_ADAPTER_IDENTITY |
            SERVICE_STATE_SECTION_ACTIVE_INTENT;
        missing.topologySignature = 0;
        if (gui_service_model_accept(&model, 1, &missing) !=
                GUI_SERVICE_ENVELOPE_ACCEPTED ||
            model.phase != GUI_SERVICE_DEVICE_MISSING) return 1119;
        ServiceStateEnvelope recovering = missing;
        recovering.stateRevision = 3;
        recovering.gpuPhase = SERVICE_GPU_PHASE_REAPPLYING;
        if (gui_service_model_accept(&model, 1, &recovering) !=
                GUI_SERVICE_ENVELOPE_ACCEPTED ||
            model.phase != GUI_SERVICE_RECOVERING) return 1120;
        ServiceResponse recovered = fake_ready_service_response(21, 4, 2);
        if (gui_service_model_accept(&model, 1, &recovered.state) !=
                GUI_SERVICE_ENVELOPE_ACCEPTED ||
            !gui_service_model_ready(&model)) return 1121;

        if (!gui_service_model_require_new_gpu_generation(&model))
            return 1147;
        recovered.state.stateRevision = 5;
        if (gui_service_model_accept(&model, 1, &recovered.state) !=
                GUI_SERVICE_ENVELOPE_REJECTED_GENERATION) return 1122;
        recovered.state.stateRevision = 6;
        recovered.state.gpuGeneration = 3;
        if (gui_service_model_accept(&model, 1, &recovered.state) !=
                GUI_SERVICE_ENVELOPE_ACCEPTED) return 1123;

        ServiceStateEnvelope partial = recovered.state;
        partial.stateRevision = 7;
        partial.validSections = SERVICE_STATE_SECTION_ADAPTER_IDENTITY;
        partial.topologySignature = 0;
        if (gui_service_model_accept(&model, 1, &partial) !=
                GUI_SERVICE_ENVELOPE_ACCEPTED ||
            model.phase != GUI_SERVICE_DEGRADED) return 1124;

        gui_service_model_begin_sync(&model, 2);
        ServiceResponse restarted = fake_ready_service_response(22, 1, 1);
        if (gui_service_model_accept(&model, 2, &restarted.state) !=
                GUI_SERVICE_ENVELOPE_ACCEPTED) return 1125;
        ready.state.stateRevision = 99;
        if (gui_service_model_accept(&model, 1, &ready.state) !=
                GUI_SERVICE_ENVELOPE_REJECTED_CONNECTION) return 1126;
        if (gui_service_model_accept(&model, 2, &ready.state) !=
                GUI_SERVICE_ENVELOPE_REJECTED_INSTANCE) return 1127;

        // A service restart can be accepted in RECOVERING before Windows posts
        // the GUI's matching DEVICEINSTANCESTARTED notification. That late
        // local cue must not demand generation 2 from the fresh instance,
        // whose generation 1 already represents the new hardware authority.
        GuiServiceModel restartThenStart = {};
        gui_service_model_initialize(&restartThenStart);
        ServiceResponse oldReady = fake_ready_service_response(31, 5, 7);
        if (gui_service_model_accept(&restartThenStart, 1,
                &oldReady.state) != GUI_SERVICE_ENVELOPE_ACCEPTED)
            return 1148;
        gui_service_model_disconnect(&restartThenStart, 2);
        ServiceResponse freshRecovering =
            fake_ready_service_response(32, 1, 1);
        freshRecovering.state.gpuPhase = SERVICE_GPU_PHASE_RECOVERING;
        freshRecovering.state.validSections =
            SERVICE_STATE_SECTION_ACTIVE_INTENT;
        freshRecovering.state.topologySignature = 0;
        if (gui_service_model_accept(&restartThenStart, 2,
                &freshRecovering.state) != GUI_SERVICE_ENVELOPE_ACCEPTED ||
            restartThenStart.phase != GUI_SERVICE_RECOVERING)
            return 1149;
        if (gui_service_model_require_new_gpu_generation(
                &restartThenStart) ||
            restartThenStart.minimumGpuGeneration != 0)
            return 1150;
        ServiceResponse freshReady = fake_ready_service_response(32, 2, 1);
        if (gui_service_model_accept(&restartThenStart, 2,
                &freshReady.state) != GUI_SERVICE_ENVELOPE_ACCEPTED ||
            !gui_service_model_ready(&restartThenStart))
            return 1151;

        for (int phase = GUI_SERVICE_DISCONNECTED;
                phase <= GUI_SERVICE_READY; ++phase) {
            bool expected = phase == GUI_SERVICE_READY;
            if (gui_service_phase_actions_enabled((GuiServicePhase)phase) !=
                    expected) return 1128;
            if (!gui_service_phase_tray_text((GuiServicePhase)phase)[0])
                return 1129;
        }
        if (gui_draft_reconcile_decide(false, false, false) !=
                GUI_DRAFT_REBASE_CLEAN ||
            gui_draft_reconcile_decide(true, true, true) !=
                GUI_DRAFT_ATTACH_DIRTY ||
            gui_draft_reconcile_decide(true, false, true) !=
                GUI_DRAFT_DETACH_DIRTY ||
            gui_draft_reconcile_decide(true, true, false) !=
                GUI_DRAFT_DETACH_DIRTY) return 1130;
        if (!gui_service_completion_context_current(4, 4, 9, 9) ||
            gui_service_completion_context_current(3, 4, 9, 9) ||
            gui_service_completion_context_current(4, 4, 8, 9)) return 1141;
        if (!gui_service_mutation_result_can_commit(true, true, true,
                GUI_SERVICE_ENVELOPE_ACCEPTED, &model) ||
            gui_service_mutation_result_can_commit(true, false, true,
                GUI_SERVICE_ENVELOPE_ACCEPTED, &model) ||
            gui_service_mutation_result_can_commit(true, true, false,
                GUI_SERVICE_ENVELOPE_ACCEPTED, &model) ||
            gui_service_mutation_result_can_commit(true, true, true,
                GUI_SERVICE_ENVELOPE_REJECTED_GENERATION, &model)) return 1142;
        if (gui_service_failure_requires_render(
                GUI_SERVICE_DISCONNECTED, false,
                false, false, false, false, false, false, false)) return 1147;
        if (!gui_service_failure_requires_render(
                GUI_SERVICE_SYNCING, false,
                false, false, false, false, false, false, false) ||
            !gui_service_failure_requires_render(
                GUI_SERVICE_DISCONNECTED, true,
                false, false, false, false, false, false, false) ||
            !gui_service_failure_requires_render(
                GUI_SERVICE_DISCONNECTED, false,
                false, false, false, true, false, true, false) ||
            !gui_service_failure_requires_render(
                GUI_SERVICE_DISCONNECTED, false,
                true, false, true, true, false, true, true)) return 1148;
    }

    // Tray callback negotiation: version-4 selection and legacy mouse
    // notifications are mutually exclusive, so one click cannot enqueue two
    // reconnect/render transactions.
    {
        if (!gui_tray_callback_opens_window(true,
                GUI_TRAY_CALLBACK_V4_SELECT) ||
            !gui_tray_callback_opens_window(true,
                GUI_TRAY_CALLBACK_V4_KEY_SELECT) ||
            gui_tray_callback_opens_window(true,
                GUI_TRAY_CALLBACK_LEGACY_PRIMARY_UP) ||
            gui_tray_callback_opens_window(true,
                GUI_TRAY_CALLBACK_LEGACY_PRIMARY_DOUBLE) ||
            !gui_tray_callback_opens_window(false,
                GUI_TRAY_CALLBACK_LEGACY_PRIMARY_UP) ||
            gui_tray_callback_opens_window(false,
                GUI_TRAY_CALLBACK_V4_SELECT)) return 1143;
        if (!gui_tray_callback_opens_context_menu(true,
                GUI_TRAY_CALLBACK_V4_CONTEXT) ||
            gui_tray_callback_opens_context_menu(true,
                GUI_TRAY_CALLBACK_LEGACY_CONTEXT) ||
            !gui_tray_callback_opens_context_menu(false,
                GUI_TRAY_CALLBACK_LEGACY_CONTEXT) ||
            gui_tray_callback_opens_context_menu(false,
                GUI_TRAY_CALLBACK_V4_CONTEXT)) return 1144;
        // F-TRAY-MENU (1155-1158).  The tray context menu used to be tracked
        // against the main window, and the SetForegroundWindow() the shell
        // demands before TrackPopupMenu() raises its target: right-clicking
        // the tray icon threw an open but occluded window in front of whatever
        // the user was working in.  Its own owner is neutral only when the
        // user cannot see it AND it can still be activated.
        //           (isMainWindow, visible, appWindow, toolWindow, noActivate)
        if (!gui_tray_menu_owner_is_neutral(false, false, false, true, false))
            return 1155;
        // The main window fails the rule however it is styled -- that is the
        // reported bug, not a style question.
        if (gui_tray_menu_owner_is_neutral(true, false, false, true, false) ||
            gui_tray_menu_owner_is_neutral(true, true, false, true, false))
            return 1156;
        // A visible or taskbar-listed owner is a raise the user can see, and a
        // window missing WS_EX_TOOLWINDOW turns up in Alt-Tab.
        if (gui_tray_menu_owner_is_neutral(false, true, false, true, false) ||
            gui_tray_menu_owner_is_neutral(false, false, true, true, false) ||
            gui_tray_menu_owner_is_neutral(false, false, false, false, false))
            return 1157;
        // WS_EX_NOACTIVATE cannot take the foreground, so it would reinstate
        // the popup that outlives the click meant to dismiss it.
        if (gui_tray_menu_owner_is_neutral(false, false, false, true, true))
            return 1158;
        if (!gui_tray_hidden_intent_blocks_show(true, true) ||
            gui_tray_hidden_intent_blocks_show(true, false) ||
            gui_tray_hidden_intent_blocks_show(false, true) ||
            !gui_tray_hidden_intent_requires_rehide(true, true, false) ||
            gui_tray_hidden_intent_requires_rehide(true, false, false) ||
            gui_tray_hidden_intent_requires_rehide(false, true, false) ||
            gui_tray_hidden_intent_requires_rehide(true, true, true)) return 1149;
        // Was gui_top_level_redraw_uses_wm_setredraw(): the transaction used to
        // toggle top-level redraw whenever the window was visible.  It no
        // longer toggles it at all -- see the visibility-neutral projection
        // suite below -- because the FALSE half of that pair is what dropped an
        // open window out of the taskbar list.
        if (!gui_top_level_redraw_may_paint_synchronously(true, false) ||
            gui_top_level_redraw_may_paint_synchronously(false, false)) return 1150;
    }

    // Stable telemetry is data-only. Authority, active intent, topology,
    // forced refresh, or missing controls promote it to one full transaction.
    {
        if (gui_state_adoption_requires_redraw_suppression(
                true, false, false, false, false) ||
            gui_state_adoption_requires_full_render(
                false, false, false, false, false, false, false)) return 1145;
        if (!gui_state_adoption_requires_redraw_suppression(
                true, true, false, false, false) ||
            !gui_state_adoption_requires_redraw_suppression(
                true, false, true, false, false) ||
            !gui_state_adoption_requires_full_render(
                false, false, false, true, false, false, false) ||
            !gui_state_adoption_requires_full_render(
                true, false, false, false, false, false, false)) return 1146;
    }

    // F-12-001: Backend spec VF_NUM_POINTS sanity
    {
        if (VF_NUM_POINTS != 128) return 90;
    }

    // F-15-002: degenerate/empty fan curve interpolation returns 100 (safe fallback)
    {
        FanCurveConfig empty = {};
        if (fan_curve_interpolate_percent(&empty, 50) != 100) return 40;
    }

    // F-LNX-FANDUP (1930-1949).  linux_port.cpp used to carry its own
    // fan_curve_normalize, and it had diverged in two ways that mattered.  The
    // Linux binary now links this implementation, so these finally cover the
    // code the daemon runs.  Every case below FAILED against the old copy.
    {
        // (1) The out-of-bounds write.  With fewer than two enabled points the
        // old copy set enabledCount = 2 and fell through into the disabled
        // loop, writing config->points[2 + i] for i up to disabledCount - 1 --
        // points[8] with one enabled point, points[9] with none, in an
        // 8-element array.  The guard bytes below catch it even without ASan;
        // under --asan the overflow aborts the run outright.
        struct GuardedCurve {
            FanCurveConfig config;
            unsigned char canary[sizeof(FanCurvePoint) * 4];
        };
        for (int enabledPoints = 0; enabledPoints <= 1; enabledPoints++) {
            GuardedCurve guarded;
            memset(&guarded, 0xA5, sizeof(guarded));
            memset(&guarded.config, 0, sizeof(guarded.config));
            guarded.config.pollIntervalMs = 1000;
            guarded.config.hysteresisC = 2;
            for (int i = 0; i < FAN_CURVE_MAX_POINTS; i++) {
                guarded.config.points[i].enabled =
                    gc_bool8_from_bool(i < enabledPoints);
                guarded.config.points[i].temperatureC = 40 + i;
                guarded.config.points[i].fanPercent = 30 + i;
            }
            fan_curve_normalize(&guarded.config);
            for (size_t i = 0; i < sizeof(guarded.canary); i++) {
                if (guarded.canary[i] != 0xA5u) return 1930 + enabledPoints;
            }
            // (2) The thermal divergence.  The old copy kept only the first two
            // default points, so the curve topped out at 35% and the fan stayed
            // there at every temperature above 45C.  The safe default has five
            // points and reaches 90%.
            if (fan_curve_active_count(&guarded.config) != 5) return 1932 + enabledPoints;
            if (fan_curve_interpolate_percent(&guarded.config, 85) != 90) return 1934 + enabledPoints;
            if (fan_curve_interpolate_percent(&guarded.config, 95) != 90) return 1936 + enabledPoints;
            // The user's poll interval and hysteresis survive the reset.
            if (guarded.config.pollIntervalMs != 1000) return 1938 + enabledPoints;
            if (guarded.config.hysteresisC != 2) return 1940 + enabledPoints;
        }

        // A normal curve is untouched by all of the above.
        FanCurveConfig ordinary = {};
        fan_curve_set_default(&ordinary);
        fan_curve_normalize(&ordinary);
        if (fan_curve_active_count(&ordinary) != 5) return 1942;
        if (fan_curve_interpolate_percent(&ordinary, 30) != 20) return 1943;
        if (fan_curve_interpolate_percent(&ordinary, 85) != 90) return 1944;

        // The degree sign must be encoded for the platform's display layer: a
        // lone 0xB0 is invalid UTF-8 and renders as a replacement glyph in the
        // Linux TUI, while Windows writes CP-1252 through the ANSI entry points.
        char summary[64] = {};
        fan_curve_format_summary(&ordinary, summary, sizeof(summary));
        const char* degree = strstr(summary, GC_DEGREE "C");
        if (!degree) return 1945;
#if !defined(_WIN32)
        // Every byte of the summary must be valid UTF-8 (ASCII or a 0xC2 lead
        // followed by a continuation byte); a bare 0xB0 must never appear.
        for (const unsigned char* p = (const unsigned char*)summary; *p; ++p) {
            if (*p < 0x80) continue;
            if (*p == 0xC2u && (p[1] & 0xC0u) == 0x80u) { ++p; continue; }
            return 1946;
        }
#endif
    }

    // fan_curve_clamp_percentages
    {
        FanCurveConfig clampCfg = {};
        fan_curve_set_default(&clampCfg);
        fan_curve_clamp_percentages(&clampCfg, 40, 80);
        for (int i = 0; i < FAN_CURVE_MAX_POINTS; i++) {
            if (clampCfg.points[i].enabled) {
                if (clampCfg.points[i].fanPercent < 40 || clampCfg.points[i].fanPercent > 80) return 41;
            }
        }
    }

    // fan_curve_equals
    {
        FanCurveConfig a = {}, b = {};
        fan_curve_set_default(&a);
        fan_curve_set_default(&b);
        if (!fan_curve_equals(&a, &b)) return 42;
        a.pollIntervalMs = 999;
        if (fan_curve_equals(&a, &b)) return 43;
    }

    // fan_curve_has_high_temp_low_fan_warning
    {
        FanCurveConfig safe = {};
        fan_curve_set_default(&safe);
        if (fan_curve_has_high_temp_low_fan_warning(&safe)) return 44;
        FanCurveConfig danger = {};
        danger.points[0] = { gc_bool8_from_bool(true), 85, 20 };
        danger.points[1] = { gc_bool8_from_bool(true), 95, 30 };
        if (!fan_curve_has_high_temp_low_fan_warning(&danger)) return 45;
    }

    // validate_desired_settings_for_ipc clamps out-of-range values
    {
        DesiredSettings ds = {};
        ds.hasPowerLimit = true;
        ds.powerLimitPct = 10;
        ds.hasGpuOffset = true;
        ds.gpuOffsetMHz = 5000;
        ds.hasMemOffset = true;
        ds.memOffsetMHz = -9000;
        ds.hasFan = true;
        ds.fanPercent = 200;
        for (int ci = 0; ci < VF_NUM_POINTS; ci++) {
            ds.hasCurvePoint[ci] = true;
            ds.curvePointMHz[ci] = 9999u;
        }
        validate_desired_settings_for_ipc(&ds);
        if (ds.powerLimitPct != 50) return 50;
        if (ds.gpuOffsetMHz != 1000) return 51;
        if (ds.memOffsetMHz != -5000) return 52;
        if (ds.fanPercent != 100) return 53;
        if (ds.curvePointMHz[0] != 5000u) return 54;
    }

    // F-SEC-4: IPC validator also clamps index/mode/curve fields so a hostile
    // unprivileged caller cannot drive an out-of-bounds index, an unknown fan
    // mode, or out-of-range fan-curve values into the LocalSystem service.
    {
        DesiredSettings ds = {};
        ds.lockCi = 9999;
        validate_desired_settings_for_ipc(&ds);
        if (ds.lockCi != VF_NUM_POINTS - 1) return 55;
        ds.lockCi = -42;
        validate_desired_settings_for_ipc(&ds);
        if (ds.lockCi != -1) return 56;
        ds.gpuOffsetExcludeLowCount = -5;
        validate_desired_settings_for_ipc(&ds);
        if (ds.gpuOffsetExcludeLowCount != 0) return 57;
        ds.gpuOffsetExcludeLowCount = 9999;
        validate_desired_settings_for_ipc(&ds);
        if (ds.gpuOffsetExcludeLowCount != VF_NUM_POINTS) return 58;
        ds.hasFan = true;
        ds.fanMode = 99;
        validate_desired_settings_for_ipc(&ds);
        if (ds.fanMode != FAN_MODE_CURVE) return 59;
        ds.fanMode = -7;
        validate_desired_settings_for_ipc(&ds);
        if (ds.fanMode != FAN_MODE_AUTO) return 60;
        ds.fanCurve.points[0].fanPercent = 250;
        ds.fanCurve.points[0].temperatureC = 9999;
        ds.fanCurve.points[1].fanPercent = -30;
        ds.fanCurve.points[1].temperatureC = -50;
        ds.fanCurve.hysteresisC = 999;
        ds.fanCurve.pollIntervalMs = 0;
        validate_desired_settings_for_ipc(&ds);
        if (ds.fanCurve.points[0].fanPercent != 100) return 61;
        if (ds.fanCurve.points[0].temperatureC != 150) return 62;
        if (ds.fanCurve.points[1].fanPercent != 0) return 63;
        if (ds.fanCurve.points[1].temperatureC != 0) return 64;
        if (ds.fanCurve.hysteresisC != FAN_CURVE_MAX_HYSTERESIS_C) return 65;
        if (ds.fanCurve.pollIntervalMs != 1) return 66;
    }

#if defined(_WIN32)
    // Windows access-control suites (F-SEC-1/1b/6, F-15).  A DACL is a Win32
    // security construct with no Linux analogue; the equivalent Linux
    // fail-closed proof is the daemon socket ownership/mode check, asserted by
    // linux_socket_path_permissions and the native transport fixture.

    // F-SEC-1: protected service-binary DACL round-trip.  apply_* must produce a
    // hardened DACL (inheritance disabled, no non-admin write); restore_* must
    // revert it so the file inherits its parent directory's ACLs again.  This
    // verifies the exact security property without needing a second identity.
    {
        wchar_t tempDir[MAX_PATH] = {};
        if (GetTempPathW(MAX_PATH, tempDir) == 0) return 110;
        wchar_t aclFile[MAX_PATH] = {};
        StringCchPrintfW(aclFile, MAX_PATH, L"%lsgc_acl_%lu.bin", tempDir, GetCurrentProcessId());
        HANDLE ah = CreateFileW(aclFile, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (ah == INVALID_HANDLE_VALUE) return 111;
        CloseHandle(ah);
        char aclErr[160] = {};
        // A fresh temp file inherits the (non-protected) temp dir ACL.
        if (service_binary_dacl_is_hardened(aclFile)) { DeleteFileW(aclFile); return 112; }
        if (!apply_protected_service_binary_dacl(aclFile, aclErr, sizeof(aclErr))) { DeleteFileW(aclFile); return 113; }
        if (!service_binary_dacl_is_hardened(aclFile)) { DeleteFileW(aclFile); return 114; }
        if (!restore_inherited_dacl(aclFile, aclErr, sizeof(aclErr))) { DeleteFileW(aclFile); return 115; }
        if (service_binary_dacl_is_hardened(aclFile)) { DeleteFileW(aclFile); return 116; }
        DeleteFileW(aclFile);
    }

    // F-SEC-1b: protected service install directory DACL.  The service binary
    // is installed adjacent to the GUI; the containing directory must also be
    // protected so a non-admin cannot delete/recreate the file by
    // parent-directory rights.
    {
        wchar_t svcDir[MAX_PATH] = {};
        if (!gc_make_unique_temp_dir(L"gc_service_dir_acl", svcDir, MAX_PATH))
            return 167;
        char aclErr[256] = {};
        if (service_binary_dacl_is_hardened(svcDir)) { gc_remove_protected_temp_dir(svcDir); return 168; }
        if (!apply_protected_service_dir_dacl(svcDir, aclErr, sizeof(aclErr))) { gc_remove_protected_temp_dir(svcDir); return 169; }
        if (!service_binary_dacl_is_hardened(svcDir)) { gc_remove_protected_temp_dir(svcDir); return 170; }
        // Cleanup must be asserted, not best-effort: a protected DACL denies
        // the creator delete rights, so the old bare RemoveDirectoryW silently
        // failed and leaked a PID-named directory.  Once a PID was reused the
        // next run failed at 167 for no real reason.
        if (!gc_remove_protected_temp_dir(svcDir)) return 171;
    }

    // F-SEC-6: machine-wide config DACL round-trip.  The .ini must be readable
    // by standard users (so the GUI can show the current default) but writable
    // only by SYSTEM/Administrators.
    {
        wchar_t tempDir[MAX_PATH] = {};
        if (GetTempPathW(MAX_PATH, tempDir) == 0) return 117;
        wchar_t mcFile[MAX_PATH] = {};
        StringCchPrintfW(mcFile, MAX_PATH, L"%lsgc_machine_acl_%lu.ini", tempDir, GetCurrentProcessId());
        HANDLE mh = CreateFileW(mcFile, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (mh == INVALID_HANDLE_VALUE) return 118;
        CloseHandle(mh);
        char aclErr[256] = {};
        if (machine_config_dacl_is_hardened(mcFile)) { DeleteFileW(mcFile); return 119; }
        if (!apply_protected_machine_config_dacl(mcFile, aclErr, sizeof(aclErr))) { DeleteFileW(mcFile); return 120; }
        if (!machine_config_dacl_is_hardened(mcFile)) { DeleteFileW(mcFile); return 121; }
        DeleteFileW(mcFile);
    }

    // F-15: shared-bank DIRECTORY DACL round-trip.  %ProgramData%\Green Curve
    // must be standard-user-readable (list) but writable only by SYSTEM /
    // Administrators, so a non-admin cannot plant or delete shared bank files.
    {
        wchar_t mcDir[MAX_PATH] = {};
        if (!gc_make_unique_temp_dir(L"gc_machine_acl_dir", mcDir, MAX_PATH))
            return 131;
        char aclErr[256] = {};
        if (machine_config_dacl_is_hardened(mcDir)) { gc_remove_protected_temp_dir(mcDir); return 132; }
        if (!apply_protected_machine_config_dir_dacl(mcDir, aclErr, sizeof(aclErr))) { gc_remove_protected_temp_dir(mcDir); return 133; }
        if (!machine_config_dacl_is_hardened(mcDir)) { gc_remove_protected_temp_dir(mcDir); return 134; }
        if (!gc_remove_protected_temp_dir(mcDir)) return 139;
    }
#endif // _WIN32

    // Shared-only policy: the "apply shared slot N" request flag must encode the
    // slot in bits 8..15 and the marker in bit 30, WITHOUT colliding with the
    // interactive bit (bit 0).  The service uses this to apply its own copy of an
    // admin shared profile for restricted callers.
    {
        DWORD f = SERVICE_REQUEST_FLAG_INTERACTIVE | SERVICE_REQUEST_FLAG_SHARED_SLOT |
                  ((3u & SERVICE_REQUEST_SHARED_SLOT_MASK) << SERVICE_REQUEST_SHARED_SLOT_SHIFT);
        if (!(f & SERVICE_REQUEST_FLAG_SHARED_SLOT)) return 135;
        if (!(f & SERVICE_REQUEST_FLAG_INTERACTIVE)) return 136;
        if (((f >> SERVICE_REQUEST_SHARED_SLOT_SHIFT) & SERVICE_REQUEST_SHARED_SLOT_MASK) != 3u) return 137;
        DWORD f2 = SERVICE_REQUEST_FLAG_SHARED_SLOT | ((5u & SERVICE_REQUEST_SHARED_SLOT_MASK) << SERVICE_REQUEST_SHARED_SLOT_SHIFT);
        if ((f2 & SERVICE_REQUEST_FLAG_INTERACTIVE) != 0) return 138;
        if (((f2 >> SERVICE_REQUEST_SHARED_SLOT_SHIFT) & SERVICE_REQUEST_SHARED_SLOT_MASK) != 5u) return 139;
    }

    // Pin-bug root-cause guard: the snapshot lockMode sync must NEVER adopt
    // the service's (previously applied) mode while the GUI holds divergent
    // pending lock intent (FLATTEN->HARD click / loaded HARD profile) or
    // unsaved edits.  Before this gate existed, the per-second telemetry
    // snapshot silently reverted a HARD pin to FLATTEN within ~1 s, making
    // the pin un-appliable ("No changes to apply") and saving it wrong.
    {
        // Divergent intent (clean): user clicked FLATTEN->HARD, snapshot still FLATTEN.
        if (lock_mode_sync_allowed(LOCK_MODE_HARD, LOCK_MODE_FLATTEN, false)) return 122;
        // Divergent intent (dirty): same, with other unsaved edits.
        if (lock_mode_sync_allowed(LOCK_MODE_HARD, LOCK_MODE_FLATTEN, true)) return 123;
        // No divergence but dirty: never resync mid-edit.
        if (lock_mode_sync_allowed(LOCK_MODE_FLATTEN, LOCK_MODE_FLATTEN, true)) return 124;
        // Clean, no divergence: adoption allowed (e.g. curve-detected FLATTEN
        // while the service authoritatively reports HARD at the same point).
        if (!lock_mode_sync_allowed(LOCK_MODE_FLATTEN, LOCK_MODE_FLATTEN, false)) return 125;
        if (!lock_mode_sync_allowed(LOCK_MODE_NONE, LOCK_MODE_NONE, false)) return 126;
    }

    // Lock-checkbox activation policy: one accepted transition per armed
    // mouse/key gesture, non-click notifications are inert, and a startup or
    // service synchronization between press and release fails safe instead of
    // turning the newly arrived FLATTEN state into HARD.
    {
        LockUiStateStamp none = {-1, -1, 0u, LOCK_MODE_NONE};
        LockUiStateStamp flatten = {27, 76, 2957u, LOCK_MODE_FLATTEN};
        if (lock_mode_after_activation(false, LOCK_MODE_NONE) != LOCK_MODE_FLATTEN) return 624;
        if (lock_mode_after_activation(true, LOCK_MODE_FLATTEN) != LOCK_MODE_HARD) return 625;
        if (lock_mode_after_activation(true, LOCK_MODE_HARD) != LOCK_MODE_NONE) return 626;
        if (decide_lock_activation(BN_SETFOCUS, BN_CLICKED, false, false, -1, 27, none, none)
                != LOCK_ACTIVATION_IGNORE_NOTIFICATION) return 627;
        if (decide_lock_activation(BN_DBLCLK, BN_CLICKED, false, false, -1, 27, none, none)
                != LOCK_ACTIVATION_IGNORE_NOTIFICATION) return 628;
        if (decide_lock_activation(BN_CLICKED, BN_CLICKED, false, false, -1, 27, none, none)
                != LOCK_ACTIVATION_ACCEPT_UNARMED) return 629;
        if (decide_lock_activation(BN_CLICKED, BN_CLICKED, true, false, 27, 27, none, none)
                != LOCK_ACTIVATION_ACCEPT_ARMED) return 630;
        if (decide_lock_activation(BN_CLICKED, BN_CLICKED, true, true, 27, 27, none, none)
                != LOCK_ACTIVATION_REJECT_ALREADY_CONSUMED) return 631;
        if (decide_lock_activation(BN_CLICKED, BN_CLICKED, true, false, 26, 27, none, none)
                != LOCK_ACTIVATION_REJECT_WRONG_CONTROL) return 632;
        if (decide_lock_activation(BN_CLICKED, BN_CLICKED, true, false, 27, 27, none, flatten)
                != LOCK_ACTIVATION_REJECT_STATE_CHANGED) return 633;
        if (!lock_ui_state_stamp_equal(flatten, flatten) || lock_ui_state_stamp_equal(none, flatten)) return 634;
#if defined(_WIN32)
        if (!run_native_ownerdraw_button_notification_test()) return 635;
        if (!run_native_reconnect_projection_and_dib_test()) return 1131;
        if (!run_native_visible_projection_taskbar_presence_test()) return 2275;
#endif
    }

    // A coherent projection transaction must be invisible to the shell.  The
    // old suppression cleared the top-level WS_VISIBLE bit, which took the
    // window out of the taskbar list for the length of a Refresh and put it
    // back afterwards; before that, restoring the bit resurrected a tray-hidden
    // owner.  Only a child window may be redraw-toggled.
    {
        const unsigned int topLevel = 0x00C00000u;  // WS_CAPTION
        const unsigned int child = GUI_REDRAW_STYLE_CHILD;
        const unsigned int visibleChild =
            GUI_REDRAW_STYLE_CHILD | GUI_REDRAW_STYLE_VISIBLE;
        if (gui_redraw_toggle_is_visibility_safe(topLevel)) return 2276;
        if (gui_redraw_toggle_is_visibility_safe(
                topLevel | GUI_REDRAW_STYLE_VISIBLE)) return 2277;
        if (!gui_redraw_toggle_is_visibility_safe(child)) return 2278;
        if (!gui_redraw_toggle_is_visibility_safe(visibleChild)) return 2279;

        // Painting: only over a window that was already on screen and is not
        // meant to be living in the tray.
        if (!gui_top_level_redraw_may_paint_synchronously(true, false)) return 2280;
        if (gui_top_level_redraw_may_paint_synchronously(true, true)) return 2281;
        if (gui_top_level_redraw_may_paint_synchronously(false, false)) return 2282;
        if (gui_top_level_redraw_may_paint_synchronously(false, true)) return 2283;

        // Invalidation while a transaction owns the frame stays deferred; that
        // used to be implied by the window reading as invisible during the
        // WM_SETREDRAW pair and has to be stated now.
        if (!gui_window_invalidation_must_defer(true, false, true)) return 2284;
        if (!gui_window_invalidation_must_defer(false, true, true)) return 2285;
        if (!gui_window_invalidation_must_defer(false, false, false)) return 2286;
        if (gui_window_invalidation_must_defer(false, false, true)) return 2287;
    }

    // Re-reading the state of the GPU already on screen is not a reconnect.
    // Routing manual Refresh through the sync transition dropped live authority
    // before the read was even sent, so the tray icon left its OC/fan theme --
    // claiming no settings were in effect -- and the sync overlay flashed.
    {
        if (gui_service_resync_decide(true, true, false) !=
            GUI_SERVICE_RESYNC_PRESERVE_PRESENTATION) return 2288;
        // A read aimed at a different GPU/service invalidates the presentation
        // by construction.
        if (gui_service_resync_decide(true, true, true) !=
            GUI_SERVICE_RESYNC_TRANSITION) return 2289;
        // Nothing truthful to preserve without a coherent READY model and live
        // authority behind it.
        if (gui_service_resync_decide(false, true, false) !=
            GUI_SERVICE_RESYNC_TRANSITION) return 2290;
        if (gui_service_resync_decide(true, false, false) !=
            GUI_SERVICE_RESYNC_TRANSITION) return 2291;
        if (gui_service_resync_decide(false, false, false) !=
            GUI_SERVICE_RESYNC_TRANSITION) return 2292;
    }

    // lockMHz is clamped at the IPC boundary like the curve points (it feeds
    // NVML locked-clocks and flatten-tail targets); 0 = "no target" stays 0.
    {
        DesiredSettings ds = {};
        ds.lockMHz = 4000000000u;
        validate_desired_settings_for_ipc(&ds);
        if (ds.lockMHz != 5000u) return 127;
        ds.lockMHz = 0u;
        validate_desired_settings_for_ipc(&ds);
        if (ds.lockMHz != 0u) return 128;
        ds.lockMHz = 2800u;
        validate_desired_settings_for_ipc(&ds);
        if (ds.lockMHz != 2800u) return 129;
    }

    // Logon auto-apply policy decision (resolve_logon_profile_source).  This is
    // the core of the restrict_non_admin_to_shared logon fix: a restricted user
    // (policy ON && not admin) must NEVER get their per-user custom OC applied at
    // logon — only an explicit published shared choice or the machine-wide shared
    // default.  Admins / unrestricted machines keep the legacy per-user-first
    // behavior.  The "restricted + per-user slot => NOT per-user" cases (142/143)
    // would have failed before the fix (bypass).
    {
        // Explicit, published shared choice always wins (any user).
        if (resolve_logon_profile_source(true, false, 3, true, 1, true, true) != LOGON_PROFILE_SOURCE_SHARED_BANK) return 140;
        if (resolve_logon_profile_source(false, true, 2, true, 0, false, false) != LOGON_PROFILE_SOURCE_SHARED_BANK) return 141;
        // Restricted + only a per-user slot => bypass closed (none, no default).
        if (resolve_logon_profile_source(true, false, 0, false, 1, true, false) != LOGON_PROFILE_SOURCE_NONE) return 142;
        // Restricted + per-user slot + machine default => the shared default.
        if (resolve_logon_profile_source(true, false, 0, false, 1, true, true) != LOGON_PROFILE_SOURCE_MACHINE_DEFAULT) return 143;
        // An explicit shared choice never silently degrades to another profile.
        if (resolve_logon_profile_source(true, false, 4, false, 0, false, true) != LOGON_PROFILE_SOURCE_PENDING) return 144;
        // Admin under policy keeps the per-user slot.
        if (resolve_logon_profile_source(true, true, 0, false, 2, true, true) != LOGON_PROFILE_SOURCE_PER_USER) return 145;
        // Policy off, non-admin: per-user slot honored.
        if (resolve_logon_profile_source(false, false, 0, false, 2, true, true) != LOGON_PROFILE_SOURCE_PER_USER) return 146;
        // Policy off, non-admin, no per-user but machine default => default.
        if (resolve_logon_profile_source(false, false, 0, false, 0, false, true) != LOGON_PROFILE_SOURCE_MACHINE_DEFAULT) return 147;
        // Nothing available => none (both unrestricted and restricted).
        if (resolve_logon_profile_source(false, false, 0, false, 0, false, false) != LOGON_PROFILE_SOURCE_NONE) return 148;
        if (resolve_logon_profile_source(true, false, 0, false, 0, false, false) != LOGON_PROFILE_SOURCE_NONE) return 149;
        // Unrestricted explicit personal slot that is missing remains pending.
        if (resolve_logon_profile_source(false, false, 0, false, 3, false, true) != LOGON_PROFILE_SOURCE_PENDING) return 601;
    }

    // Stable GPU selection survives enumeration reordering and fails closed
    // when an identical identity is ambiguous or absent.
    {
        ConfiguredGpuSelection configured = {};
        configured.stableIdentityPresent = true;
        configured.legacyIndex = 0;
        configured.identity.valid = true;
        configured.identity.pciInfoValid = true;
        configured.identity.deviceId = 0x1234;
        configured.identity.subSystemId = 0x5678;
        configured.identity.pciRevisionId = 1;
        configured.identity.extDeviceId = 2;
        configured.identity.pciBus = 9;
        configured.identity.pciDevice = 0;
        GpuAdapterInfo adapters[2] = {};
        adapters[0] = configured.identity;
        adapters[0].pciBus = 3;
        adapters[1] = configured.identity;
        unsigned int resolved = 99;
        if (resolve_configured_gpu_selection(&configured, adapters, 2, &resolved) != CONFIGURED_GPU_RESOLVE_STABLE || resolved != 1) return 602;
        adapters[1].pciBus = 3;
        if (resolve_configured_gpu_selection(&configured, adapters, 2, &resolved) != CONFIGURED_GPU_RESOLVE_NOT_FOUND) return 603;
        configured.identity.pciBus = 0;
        configured.identity.pciDevice = 0;
        adapters[0] = configured.identity;
        adapters[1] = configured.identity;
        if (resolve_configured_gpu_selection(&configured, adapters, 2, &resolved) != CONFIGURED_GPU_RESOLVE_AMBIGUOUS) return 604;
    }

    // explicit_vf_points_v1 makes lock_ci=-1 an unlocked custom curve, not a
    // legacy captured-live-curve cleanup marker.
    if (profile_should_strip_legacy_unlocked_curve(true, -1, true)) return 605;
    if (!profile_should_strip_legacy_unlocked_curve(true, -1, false)) return 606;
    if (profile_lock_mode_after_load(true, false, LOCK_MODE_NONE) !=
        LOCK_MODE_FLATTEN) return 900;
    if (profile_lock_mode_after_load(true, true, LOCK_MODE_HARD) !=
        LOCK_MODE_HARD) return 901;
    if (profile_lock_mode_after_load(false, true, LOCK_MODE_HARD) !=
        LOCK_MODE_NONE) return 902;
    if (profile_slot_reference_after_clear(3, 3, 0) != 0 ||
        profile_slot_reference_after_clear(2, 3, 0) != 2) return 903;
    if (profile_lock_mode_after_load(true, true, LOCK_MODE_NONE) !=
        LOCK_MODE_FLATTEN) return 907;

    // Apply origins are security policy, not diagnostic strings.  Only a
    // successful explicit user action may clear a sticky automatic-restore
    // lockout.  Every automatic origin, including app-launch/foreground, must
    // honor it.  Service startup has no apply origin at all.
    {
        if (!service_apply_origin_is_explicit(SERVICE_APPLY_ORIGIN_GUI)) return 160;
        if (!service_apply_origin_is_explicit(SERVICE_APPLY_ORIGIN_CLI)) return 161;
        if (!service_apply_origin_is_explicit(SERVICE_APPLY_ORIGIN_HOTKEY)) return 162;
        if (!service_apply_origin_is_explicit(SERVICE_APPLY_ORIGIN_TRAY)) return 163;
        if (service_apply_origin_is_explicit(SERVICE_APPLY_ORIGIN_APP_LAUNCH)) return 164;
        if (service_apply_origin_is_explicit(SERVICE_APPLY_ORIGIN_FOREGROUND)) return 165;
        if (service_apply_origin_is_explicit(SERVICE_APPLY_ORIGIN_LOGON)) return 166;
        if (service_apply_origin_is_explicit(SERVICE_APPLY_ORIGIN_STANDBY)) return 167;
        if (service_apply_origin_is_explicit(SERVICE_APPLY_ORIGIN_DRIVER_RECOVERY)) return 168;
        if (service_apply_origin_is_explicit(SERVICE_APPLY_ORIGIN_UNSPECIFIED)) return 169;

        if (service_apply_origin_is_automatic(SERVICE_APPLY_ORIGIN_GUI)) return 170;
        if (!service_apply_origin_is_automatic(SERVICE_APPLY_ORIGIN_APP_LAUNCH)) return 171;
        if (!service_apply_origin_is_automatic(SERVICE_APPLY_ORIGIN_FOREGROUND)) return 172;
        if (!service_apply_origin_is_automatic(SERVICE_APPLY_ORIGIN_LOGON)) return 173;
        if (!service_apply_origin_is_automatic(SERVICE_APPLY_ORIGIN_STANDBY)) return 174;
        if (!service_apply_origin_is_automatic(SERVICE_APPLY_ORIGIN_DRIVER_RECOVERY)) return 175;

        // Client APPLY may carry explicit actions and the two GUI-owned
        // automation origins only. Logon has a settings-free command, while
        // standby/driver recovery are authorized only by the lifecycle worker.
        if (!service_apply_origin_is_client_apply(SERVICE_APPLY_ORIGIN_GUI)) return 555;
        if (!service_apply_origin_is_client_apply(SERVICE_APPLY_ORIGIN_CLI)) return 556;
        if (!service_apply_origin_is_client_apply(SERVICE_APPLY_ORIGIN_HOTKEY)) return 557;
        if (!service_apply_origin_is_client_apply(SERVICE_APPLY_ORIGIN_TRAY)) return 558;
        if (!service_apply_origin_is_client_apply(SERVICE_APPLY_ORIGIN_APP_LAUNCH)) return 559;
        if (!service_apply_origin_is_client_apply(SERVICE_APPLY_ORIGIN_FOREGROUND)) return 560;
        if (service_apply_origin_is_client_apply(SERVICE_APPLY_ORIGIN_LOGON)) return 561;
        if (service_apply_origin_is_client_apply(SERVICE_APPLY_ORIGIN_STANDBY)) return 562;
        if (service_apply_origin_is_client_apply(SERVICE_APPLY_ORIGIN_DRIVER_RECOVERY)) return 563;
        if (service_apply_origin_is_client_apply(SERVICE_APPLY_ORIGIN_UNSPECIFIED)) return 564;

        if (SERVICE_CMD_LOGON_HANDOFF == SERVICE_CMD_APPLY) return 176;
        if (SERVICE_CMD_LOGON_HANDOFF == SERVICE_CMD_RESET) return 177;

        ServiceSnapshot snapshot = {};
        snapshot.activeProfileSource = 999;
        snapshot.activeProfileSlot = 999;
        snapshot.lastLifecycleTrigger = 999;
        snapshot.lastLifecycleResult = 999;
        snapshot.autoRestoreLockoutReason = 999;
        validate_service_snapshot_for_ipc(&snapshot);
        if (snapshot.activeProfileSource != SERVICE_PROFILE_SOURCE_NONE) return 178;
        if (snapshot.activeProfileSlot != 0) return 179;
        if (snapshot.lastLifecycleTrigger != SERVICE_LIFECYCLE_TRIGGER_NONE) return 180;
        if (snapshot.lastLifecycleResult != SERVICE_LIFECYCLE_RESULT_NONE) return 181;
        // Invalid lockout metadata fails closed rather than silently rearming.
        if (snapshot.autoRestoreLockoutReason != SERVICE_AUTO_RESTORE_LOCKOUT_AUTOMATIC_APPLY_FAILED) return 182;
    }

    // Exact selected-GPU PnP identity policy.  A full PCI hardware ID is
    // required, NvAPI's combined device/vendor ID layouts are normalized, and
    // a known BDF must match.  Missing/partial/ambiguous identities fail closed
    // for PnP recovery authorization without affecting hardware-write policy.
    {
        SelectedGpuPciHardwareId parsed = {};
        const wchar_t* exact =
            L"PCI\\VEN_10DE&DEV_2684&SUBSYS_17AA3A5C&REV_A1";
        if (!selected_gpu_pnp_parse_hardware_id(exact, &parsed)) return 500;
        if (parsed.vendorId != 0x10DEu || parsed.deviceId != 0x2684u ||
            parsed.subsystemId != 0x17AA3A5Cu || parsed.revisionId != 0xA1u) return 501;
        if (!selected_gpu_pnp_parse_hardware_id(
                L"pci\\ven_10de&dev_2684&subsys_17aa3a5c&rev_a1",
                &parsed)) return 502;
        if (selected_gpu_pnp_parse_hardware_id(
                L"PCI\\VEN_10DE&DEV_2684&SUBSYS_17AA3A5C", &parsed)) return 503;
        if (selected_gpu_pnp_parse_hardware_id(
                L"PCI\\VEN_10DE&DEV_02684&SUBSYS_17AA3A5C&REV_A1",
                &parsed)) return 504;

        GpuAdapterInfo target = {};
        target.valid = 1;
        target.pciInfoValid = 1;
        target.deviceId = 0x268410DEu;
        target.subSystemId = 0x17AA3A5Cu;
        target.pciRevisionId = 0xA1u;
        if (!selected_gpu_pnp_parse_hardware_id(exact, &parsed) ||
            !selected_gpu_pnp_hardware_id_matches_target(
                &target, &parsed)) return 505;
        target.deviceId = 0x10DE2684u;
        if (!selected_gpu_pnp_hardware_id_matches_target(
                &target, &parsed)) return 506;
        target.deviceId = 0x2684u;
        if (!selected_gpu_pnp_hardware_id_matches_target(
                &target, &parsed)) return 507;
        target.deviceId = 0x268510DEu;
        if (selected_gpu_pnp_hardware_id_matches_target(
                &target, &parsed)) return 508;
        target.deviceId = 0x268410DEu;
        target.subSystemId ^= 1u;
        if (selected_gpu_pnp_hardware_id_matches_target(
                &target, &parsed)) return 509;
        target.subSystemId ^= 1u;
        target.pciRevisionId = 0xA2u;
        if (selected_gpu_pnp_hardware_id_matches_target(
                &target, &parsed)) return 510;
        target.pciRevisionId = 0xA1u;
        SelectedGpuPciHardwareId wrongVendor = {};
        if (!selected_gpu_pnp_parse_hardware_id(
                L"PCI\\VEN_1234&DEV_2684&SUBSYS_17AA3A5C&REV_A1",
                &wrongVendor)) return 520;
        if (selected_gpu_pnp_hardware_id_matches_target(
                &target, &wrongVendor)) return 521;

        const wchar_t ids[] =
            L"PCI\\VEN_10DE&DEV_2684\0"
            L"PCI\\VEN_10DE&DEV_2684&SUBSYS_17AA3A5C&REV_A1\0\0";
        if (!selected_gpu_pnp_hardware_id_list_matches_target(
                &target, ids, ARRAY_COUNT(ids))) return 511;
        // GpuAdapterInfo has no historical BDF-valid bit. An all-zero location
        // must fall back to the unique full hardware ID rather than pretending
        // that 0000:00:00.0 was independently corroborated.
        if (selected_gpu_pnp_target_has_bdf(&target)) return 565;
        target.pciBus = 7;
        target.pciDevice = 0;
        target.pciFunction = 0;
        if (!selected_gpu_pnp_target_has_bdf(&target)) return 512;
        if (!selected_gpu_pnp_bdf_matches_target(
                &target, true, 7, 0, 0)) return 513;
        if (selected_gpu_pnp_bdf_matches_target(
                &target, true, 8, 0, 0)) return 514;
        if (selected_gpu_pnp_bdf_matches_target(
                &target, true, 7, 1, 0)) return 566;
        if (selected_gpu_pnp_bdf_matches_target(
                &target, true, 7, 0, 1)) return 567;
        if (selected_gpu_pnp_bdf_matches_target(
                &target, false, 0, 0, 0)) return 515;
        target.pciDomain = 1;
        if (selected_gpu_pnp_target_has_bdf(&target)) return 516;
        if (selected_gpu_pnp_resolve_match_count(0) !=
                SELECTED_GPU_PNP_MATCH_NONE) return 517;
        if (selected_gpu_pnp_resolve_match_count(1) !=
                SELECTED_GPU_PNP_MATCH_UNIQUE) return 518;
        if (selected_gpu_pnp_resolve_match_count(2) !=
                SELECTED_GPU_PNP_MATCH_AMBIGUOUS) return 519;
    }

    // Automatic-restore policy: a configured logon profile may be applied at
    // the user's Windows logon unless safety has been latched off. Standby
    // restores current active intent immediately (unless locked out); driver
    // recovery is deliberately stricter and needs a successful apply that
    // survived the full 10-minute proving period. These boundary cases
    // must stay independent of scheduler timing and never become a continuous
    // curve-drift correction policy.
    {
        if (!should_auto_apply_logon_profile(true, false)) return 400;
        if (should_auto_apply_logon_profile(false, false)) return 401;
        if (should_auto_apply_logon_profile(true, true)) return 402;
        if (!should_auto_restore_after_standby_resume(true, false)) return 403;
        if (should_auto_restore_after_standby_resume(false, false)) return 404;
        if (should_auto_restore_after_standby_resume(true, true)) return 405;
        if (should_auto_restore_after_driver_event(false, AUTO_RESTORE_STABILITY_WINDOW_MS, false)) return 406;
        if (should_auto_restore_after_driver_event(true, AUTO_RESTORE_STABILITY_WINDOW_MS - 1, false)) return 407;
        if (!should_auto_restore_after_driver_event(true, AUTO_RESTORE_STABILITY_WINDOW_MS, false)) return 408;
        if (should_auto_restore_after_driver_event(true, AUTO_RESTORE_STABILITY_WINDOW_MS + 1, true)) return 409;
        if (service_should_preserve_proof_after_standby(
                true, AUTO_RESTORE_STABILITY_WINDOW_MS - 1,
                AUTO_RESTORE_STABILITY_WINDOW_MS)) return 626;
        if (!service_should_preserve_proof_after_standby(
                true, AUTO_RESTORE_STABILITY_WINDOW_MS,
                AUTO_RESTORE_STABILITY_WINDOW_MS)) return 627;
        if (service_should_preserve_proof_after_standby(
                false, AUTO_RESTORE_STABILITY_WINDOW_MS + 1,
                AUTO_RESTORE_STABILITY_WINDOW_MS)) return 628;
    }

    // Fake unbiased clock: prove the exact 9:59/10:00 boundary without sleep.
    // "Wall time spent asleep" is intentionally absent; unchanged awake ticks
    // leave the proof age unchanged.  Old/cross-boot/ambiguous stamps fail closed.
    {
        const ServiceBootIdentity boot = { 0x12345678ULL, 0x9abcdef0ULL };
        const ServiceBootIdentity anotherBoot = { 0x12345678ULL, 0x9abcdef1ULL };
        const ServiceBootIdentity anotherHighBoot = { 0x12345679ULL, 0x9abcdef0ULL };
        const ServiceBootIdentity invalidBoot = {};
        const uint64_t appliedAwake = 100000000ULL;
        ServiceOcApplyProofStamp stamp = {};
        stamp.magic = SERVICE_OC_APPLY_STAMP_MAGIC;
        stamp.version = SERVICE_OC_APPLY_STAMP_VERSION;
        stamp.size = sizeof(stamp);
        stamp.bootIdentity = boot;
        stamp.awakeTime100ns = appliedAwake;
        uint64_t ageMs = 0;
        if (!service_compute_proof_age_ms(&stamp, boot,
                appliedAwake + 599000ULL * 10000ULL, &ageMs) || ageMs != 599000ULL) return 432;
        if (should_auto_restore_after_driver_event(true, ageMs, false)) return 433;
        if (!service_compute_proof_age_ms(&stamp, boot,
                appliedAwake + 600000ULL * 10000ULL, &ageMs) || ageMs != 600000ULL) return 434;
        if (!should_auto_restore_after_driver_event(true, ageMs, false)) return 435;

        // Ten minutes of wall-clock standby with no awake-tick progress proves nothing.
        if (!service_compute_proof_age_ms(&stamp, boot, appliedAwake, &ageMs) || ageMs != 0) return 436;
        if (should_auto_restore_after_driver_event(true, ageMs, false)) return 437;
        if (service_compute_proof_age_ms(&stamp, anotherBoot,
                appliedAwake + 600000ULL * 10000ULL, &ageMs)) return 438;
        if (service_compute_proof_age_ms(&stamp, anotherHighBoot,
                appliedAwake + 600000ULL * 10000ULL, &ageMs)) return 624;
        if (service_compute_proof_age_ms(&stamp, invalidBoot,
                appliedAwake + 600000ULL * 10000ULL, &ageMs)) return 625;
        stamp.version = 1;
        if (service_compute_proof_age_ms(&stamp, boot,
                appliedAwake + 600000ULL * 10000ULL, &ageMs)) return 439;
        stamp.version = SERVICE_OC_APPLY_STAMP_VERSION;
        stamp.reserved = 1;
        if (service_compute_proof_age_ms(&stamp, boot,
                appliedAwake + 600000ULL * 10000ULL, &ageMs)) return 440;
        stamp.reserved = 0;

        // Every successful reapply starts a fresh proof, rather than inheriting
        // the old application age.
        stamp.awakeTime100ns = appliedAwake + 600000ULL * 10000ULL;
        if (!service_compute_proof_age_ms(&stamp, boot, stamp.awakeTime100ns, &ageMs) || ageMs != 0) return 441;

        ServiceRecoveryClockEntry history[] = {
            { boot, stamp.awakeTime100ns - 10000ULL },
            { boot, stamp.awakeTime100ns - 20000ULL },
            { boot, stamp.awakeTime100ns - 30000ULL },
            { anotherBoot, stamp.awakeTime100ns },
        };
        if (service_count_recent_recovery_clock_entries(history, ARRAY_COUNT(history),
                boot, stamp.awakeTime100ns, 300000ULL) != 3) return 442;
        // No awake-time progress leaves the persistent spam count intact.
        if (service_count_recent_recovery_clock_entries(history, ARRAY_COUNT(history),
                boot, stamp.awakeTime100ns, 300000ULL) != 3) return 443;
        ServiceRecoveryEvidenceKey evidence[] = {
            { 0xAAULL, 0x11ULL },
            { 0xBBULL, 0x22ULL },
        };
        ServiceRecoveryEvidenceKey corroborating = { 0xAAULL, 0x11ULL };
        ServiceRecoveryEvidenceKey distinct = { 0xAAULL, 0x12ULL };
        if (!service_recovery_evidence_already_recorded(
                evidence, ARRAY_COUNT(evidence), corroborating)) return 456;
        if (service_recovery_evidence_already_recorded(
                evidence, ARRAY_COUNT(evidence), distinct)) return 457;

        // SCM does not guarantee a valid PID while STOP_PENDING. A cleanly
        // exited, pinned parent may therefore transition through pid=0 or a
        // stale value without being mistaken for a different generation.
        if (service_classify_controlled_recovery_scm_stop_state(
                true, false, 0, 20184) !=
                SERVICE_CONTROLLED_RECOVERY_SCM_STOPPED) return 650;
        if (service_classify_controlled_recovery_scm_stop_state(
                false, true, 0, 20184) !=
                SERVICE_CONTROLLED_RECOVERY_SCM_WAIT_FOR_STOPPED) return 651;
        if (service_classify_controlled_recovery_scm_stop_state(
                false, true, 9999, 20184) !=
                SERVICE_CONTROLLED_RECOVERY_SCM_WAIT_FOR_STOPPED) return 652;
        if (service_classify_controlled_recovery_scm_stop_state(
                false, false, 20184, 20184) !=
                SERVICE_CONTROLLED_RECOVERY_SCM_WAIT_FOR_STOPPED) return 653;
        if (service_classify_controlled_recovery_scm_stop_state(
                false, false, 9999, 20184) !=
                SERVICE_CONTROLLED_RECOVERY_SCM_REJECT) return 654;
        if (service_classify_controlled_recovery_scm_stop_state(
                false, true, 0, 0) !=
                SERVICE_CONTROLLED_RECOVERY_SCM_REJECT) return 655;
    }

    // Deterministic lifecycle reducer tests.  Hardware writes are represented
    // only by incrementing fakeWrites when the reducer issues an authorization;
    // there are no clocks, sleeps, threads, GPU calls, or timing assumptions.
    {
        auto identity = [](gc_u32 session, gc_u64 auth, const char* sid) {
            ServiceLifecycleIdentity value = {};
            value.valid = 1;
            value.sessionId = session;
            value.authenticationId = auth;
            gc_strlcpy(value.sid, ARRAY_COUNT(value.sid), sid);
            return value;
        };
        ServiceLifecycleIdentity loginA = identity(7, 1001, "S-1-5-21-test");
        ServiceLifecycleIdentity loginB = identity(7, 1002, "S-1-5-21-test");
        int fakeWrites = 0;

        // Task-only profile-2 login: readiness may signal repeatedly, but the
        // single write transition is authorized exactly once.
        ServiceLifecycleState state = {};
        ServiceLifecycleEvent event = {};
        event.type = SERVICE_LIFECYCLE_EVENT_TASK_HANDOFF;
        event.identity = loginA;
        ServiceLifecycleDecision decision = service_lifecycle_reduce(&state, &event);
        if (!state.logonPending || !decision.wakeWorker || !decision.attemptLogonPrerequisites) return 410;
        event = {};
        event.type = SERVICE_LIFECYCLE_EVENT_PREREQUISITE_SIGNAL;
        decision = service_lifecycle_reduce(&state, &event);
        if (!decision.attemptLogonPrerequisites) return 411;
        decision = service_lifecycle_reduce(&state, &event);
        if (!decision.attemptLogonPrerequisites || fakeWrites != 0) return 412;
        event = {};
        event.type = SERVICE_LIFECYCLE_EVENT_LOGON_WRITE_STARTED;
        event.identity = loginA;
        decision = service_lifecycle_reduce(&state, &event);
        if (decision.authorizeLogonWrite) ++fakeWrites;
        if (fakeWrites != 1) return 413;
        decision = service_lifecycle_reduce(&state, &event);
        if (decision.authorizeLogonWrite) ++fakeWrites;
        if (fakeWrites != 1) return 414;
        event.type = SERVICE_LIFECYCLE_EVENT_LOGON_WRITE_FINISHED;
        event.success = 1;
        event.writeAttempted = 1;
        decision = service_lifecycle_reduce(&state, &event);
        if (decision.result != SERVICE_LIFECYCLE_RESULT_APPLIED || state.logonPending) return 415;

        // A racing WTS event for the same login is coalesced.  Reusing session
        // ID and SID with a new authentication LUID is a distinct login.
        event = {};
        event.type = SERVICE_LIFECYCLE_EVENT_WTS_LOGON;
        event.identity = loginA;
        decision = service_lifecycle_reduce(&state, &event);
        if (!decision.coalesced || state.logonPending || fakeWrites != 1) return 416;
        event.identity = loginB;
        decision = service_lifecycle_reduce(&state, &event);
        if (decision.coalesced || !state.logonPending || state.logonGeneration != 2) return 417;
        event.type = SERVICE_LIFECYCLE_EVENT_TASK_HANDOFF;
        decision = service_lifecycle_reduce(&state, &event);
        if (!decision.coalesced || state.logonGeneration != 2) return 418;
        event.type = SERVICE_LIFECYCLE_EVENT_LOGOFF;
        decision = service_lifecycle_reduce(&state, &event);
        if (!decision.cancelled || state.logonPending ||
            decision.result != SERVICE_LIFECYCLE_RESULT_CANCELLED_LOGOFF) return 419;

        // A pending login can be superseded or locked out without authorizing a
        // write.  A zero-initialized/ordinary-start state is inert.
        ServiceLifecycleState inert = {};
        event = {};
        event.type = SERVICE_LIFECYCLE_EVENT_NONE;
        decision = service_lifecycle_reduce(&inert, &event);
        if (decision.authorizeLogonWrite || decision.authorizeStandbyWrite ||
            decision.authorizeDriverWrite || decision.wakeWorker) return 420;
        event.type = SERVICE_LIFECYCLE_EVENT_TASK_HANDOFF;
        event.identity = loginA;
        service_lifecycle_reduce(&inert, &event);
        event = {};
        event.type = SERVICE_LIFECYCLE_EVENT_EXPLICIT_SUPERSEDE;
        decision = service_lifecycle_reduce(&inert, &event);
        if (!decision.cancelled || inert.logonPending) return 421;
        event.type = SERVICE_LIFECYCLE_EVENT_LOCKOUT;
        service_lifecycle_reduce(&inert, &event);
        event = {};
        event.type = SERVICE_LIFECYCLE_EVENT_TASK_HANDOFF;
        event.identity = loginB;
        decision = service_lifecycle_reduce(&inert, &event);
        if (decision.result != SERVICE_LIFECYCLE_RESULT_LOCKED_OUT || inert.logonPending) return 422;

        // A failure before the hardware boundary is a prerequisite failure: it
        // releases the one-write authorization and keeps intent pending. Once
        // writeAttempted is true, success or failure is terminal and no later
        // readiness signal may replay the write.
        ServiceLifecycleState logonFailure = {};
        event = {};
        event.type = SERVICE_LIFECYCLE_EVENT_TASK_HANDOFF;
        event.identity = loginA;
        service_lifecycle_reduce(&logonFailure, &event);
        event.type = SERVICE_LIFECYCLE_EVENT_LOGON_WRITE_STARTED;
        decision = service_lifecycle_reduce(&logonFailure, &event);
        if (!decision.authorizeLogonWrite) return 580;
        event.type = SERVICE_LIFECYCLE_EVENT_LOGON_WRITE_FINISHED;
        event.success = 0;
        event.writeAttempted = 0;
        decision = service_lifecycle_reduce(&logonFailure, &event);
        if (decision.result != SERVICE_LIFECYCLE_RESULT_TRANSIENT_NOT_READY ||
            !logonFailure.logonPending || logonFailure.logonWriteIssued) return 581;
        event = {};
        event.type = SERVICE_LIFECYCLE_EVENT_PREREQUISITE_SIGNAL;
        decision = service_lifecycle_reduce(&logonFailure, &event);
        if (!decision.wakeWorker || !decision.attemptLogonPrerequisites) return 582;
        event = {};
        event.type = SERVICE_LIFECYCLE_EVENT_LOGON_WRITE_STARTED;
        event.identity = loginA;
        decision = service_lifecycle_reduce(&logonFailure, &event);
        if (!decision.authorizeLogonWrite) return 583;
        event.type = SERVICE_LIFECYCLE_EVENT_LOGON_WRITE_FINISHED;
        event.success = 0;
        event.writeAttempted = 1;
        decision = service_lifecycle_reduce(&logonFailure, &event);
        if (decision.result != SERVICE_LIFECYCLE_RESULT_FAILED ||
            logonFailure.logonPending) return 584;
        event = {};
        event.type = SERVICE_LIFECYCLE_EVENT_PREREQUISITE_SIGNAL;
        decision = service_lifecycle_reduce(&logonFailure, &event);
        if (decision.wakeWorker || decision.attemptLogonPrerequisites ||
            decision.authorizeLogonWrite) return 585;

        ServiceLifecycleState standbyFailure = {};
        event = {};
        event.type = SERVICE_LIFECYCLE_EVENT_SUSPEND;
        service_lifecycle_reduce(&standbyFailure, &event);
        event.type = SERVICE_LIFECYCLE_EVENT_RESUME;
        service_lifecycle_reduce(&standbyFailure, &event);
        event = {};
        event.type = SERVICE_LIFECYCLE_EVENT_STANDBY_WRITE_STARTED;
        decision = service_lifecycle_reduce(&standbyFailure, &event);
        if (!decision.authorizeStandbyWrite) return 586;
        event.type = SERVICE_LIFECYCLE_EVENT_STANDBY_WRITE_FINISHED;
        event.success = 0;
        event.writeAttempted = 0;
        decision = service_lifecycle_reduce(&standbyFailure, &event);
        if (decision.result != SERVICE_LIFECYCLE_RESULT_TRANSIENT_NOT_READY ||
            !standbyFailure.standbyPending || standbyFailure.standbyWriteIssued) return 587;
        event = {};
        event.type = SERVICE_LIFECYCLE_EVENT_STANDBY_WRITE_STARTED;
        decision = service_lifecycle_reduce(&standbyFailure, &event);
        if (!decision.authorizeStandbyWrite) return 588;
        event.type = SERVICE_LIFECYCLE_EVENT_STANDBY_WRITE_FINISHED;
        event.success = 0;
        event.writeAttempted = 1;
        decision = service_lifecycle_reduce(&standbyFailure, &event);
        if (decision.result != SERVICE_LIFECYCLE_RESULT_FAILED ||
            standbyFailure.standbyPending) return 589;

        ServiceLifecycleState driverFailure = {};
        event = {};
        event.type = SERVICE_LIFECYCLE_EVENT_DRIVER_RECOVERY;
        event.driverProofReady = 1;
        service_lifecycle_reduce(&driverFailure, &event);
        event.type = SERVICE_LIFECYCLE_EVENT_DRIVER_WRITE_STARTED;
        event.controlledRecoveryValidated = 1;
        decision = service_lifecycle_reduce(&driverFailure, &event);
        if (!decision.authorizeDriverWrite) return 590;
        event.type = SERVICE_LIFECYCLE_EVENT_DRIVER_WRITE_FINISHED;
        event.success = 0;
        event.writeAttempted = 0;
        decision = service_lifecycle_reduce(&driverFailure, &event);
        if (decision.result != SERVICE_LIFECYCLE_RESULT_TRANSIENT_NOT_READY ||
            !driverFailure.driverPending || driverFailure.driverWriteIssued) return 591;
        event = {};
        event.type = SERVICE_LIFECYCLE_EVENT_DRIVER_WRITE_STARTED;
        event.driverProofReady = 1;
        event.controlledRecoveryValidated = 1;
        decision = service_lifecycle_reduce(&driverFailure, &event);
        if (!decision.authorizeDriverWrite) return 592;
        event.type = SERVICE_LIFECYCLE_EVENT_DRIVER_WRITE_FINISHED;
        event.success = 0;
        event.writeAttempted = 1;
        decision = service_lifecycle_reduce(&driverFailure, &event);
        if (decision.result != SERVICE_LIFECYCLE_RESULT_FAILED ||
            driverFailure.driverPending) return 593;
        event = {};
        event.type = SERVICE_LIFECYCLE_EVENT_DRIVER_PROOF_SIGNAL;
        event.driverProofReady = 1;
        decision = service_lifecycle_reduce(&driverFailure, &event);
        if (decision.wakeWorker || decision.authorizeDriverWrite) return 594;

        // One full-intent authorization per suspend generation, with no driver
        // proof input. Duplicate resume/write-start notifications coalesce.
        ServiceLifecycleState power = {};
        event = {};
        event.type = SERVICE_LIFECYCLE_EVENT_SUSPEND;
        service_lifecycle_reduce(&power, &event);
        event.type = SERVICE_LIFECYCLE_EVENT_RESUME;
        decision = service_lifecycle_reduce(&power, &event);
        if (!decision.wakeWorker || !power.standbyPending) return 423;
        // The later Windows resume notification is a real readiness cue when
        // the first serialized probe was too early. It wakes the same pending
        // generation but must not create or authorize another write.
        decision = service_lifecycle_reduce(&power, &event);
        if (!decision.wakeWorker || !decision.coalesced ||
            !power.standbyPending || power.suspendGeneration != 1) return 424;
        event = {};
        event.type = SERVICE_LIFECYCLE_EVENT_STANDBY_WRITE_STARTED;
        decision = service_lifecycle_reduce(&power, &event);
        int standbyWrites = decision.authorizeStandbyWrite ? 1 : 0;
        decision = service_lifecycle_reduce(&power, &event);
        if (decision.authorizeStandbyWrite) ++standbyWrites;
        if (standbyWrites != 1) return 425;
        event.type = SERVICE_LIFECYCLE_EVENT_STANDBY_WRITE_FINISHED;
        event.success = 1;
        event.writeAttempted = 1;
        decision = service_lifecycle_reduce(&power, &event);
        if (decision.result != SERVICE_LIFECYCLE_RESULT_APPLIED || power.standbyPending) return 426;

        // Sparse fan/memory/power requests do not own the VF/lock domain and
        // therefore must never reset an existing hard clock lock. A reset,
        // curve, GPU-offset, or explicit lock request intentionally replaces it.
        DesiredSettings lockDomain = {};
        if (service_request_replaces_lock_domain(&lockDomain)) return 570;
        lockDomain.hasFan = 1;
        if (service_request_replaces_lock_domain(&lockDomain)) return 571;
        lockDomain = {};
        lockDomain.hasMemOffset = 1;
        if (service_request_replaces_lock_domain(&lockDomain)) return 572;
        lockDomain = {};
        lockDomain.hasPowerLimit = 1;
        if (service_request_replaces_lock_domain(&lockDomain)) return 573;
        lockDomain = {};
        lockDomain.resetOcBeforeApply = 1;
        if (!service_request_replaces_lock_domain(&lockDomain)) return 574;
        lockDomain = {};
        lockDomain.hasGpuOffset = 1;
        if (!service_request_replaces_lock_domain(&lockDomain)) return 575;
        lockDomain = {};
        lockDomain.hasCurvePoint[77] = 1;
        if (!service_request_replaces_lock_domain(&lockDomain)) return 576;
        lockDomain = {};
        lockDomain.hasLock = 1;
        if (!service_request_replaces_lock_domain(&lockDomain)) return 577;

        // Standby restoration copies the complete in-memory intent.  Sparse
        // curve-only, lock-only, fan-only and combined requests retain every
        // owned field.  Reset-to-stock is added for owned VF policy, which
        // includes a LOCK: every lock mode composes a VF anchor/tail write, so
        // replaying one without a clean baseline pins onto whatever the driver
        // is already holding.  A fan-, memory- or power-only restore must
        // still not reset unrelated OC -- those fields may belong to another
        // tool.  (Case 2 expected `false` until 2026-07-31; the sibling
        // predicate service_intent_owns_vf_cleanup() had always counted a lock,
        // so a lock-only profile reset its baseline when it REPLACED another
        // profile but not when standby or a Linux boot replay restored it.)
        DesiredSettings restoreCases[4] = {};
        restoreCases[0].hasGpuOffset = 1;
        restoreCases[0].gpuOffsetMHz = 125;
        restoreCases[0].hasMemOffset = 1;
        restoreCases[0].memOffsetMHz = 700;
        restoreCases[0].hasPowerLimit = 1;
        restoreCases[0].powerLimitPct = 92;
        restoreCases[0].hasCurvePoint[77] = 1;
        restoreCases[0].curvePointMHz[77] = 2460;
        restoreCases[0].hasLock = 1;
        restoreCases[0].lockCi = 77;
        restoreCases[0].lockMHz = 2460;
        restoreCases[0].hasFan = 1;
        restoreCases[0].fanMode = FAN_MODE_FIXED;
        restoreCases[0].fanPercent = 61;
        restoreCases[1].hasCurvePoint[88] = 1;
        restoreCases[1].curvePointMHz[88] = 2515;
        restoreCases[2].hasLock = 1;
        restoreCases[2].lockCi = 91;
        restoreCases[2].lockMHz = 2550;
        restoreCases[2].lockMode = LOCK_MODE_HARD;
        restoreCases[3].hasFan = 1;
        restoreCases[3].fanMode = FAN_MODE_FIXED;
        restoreCases[3].fanPercent = 55;
        //                              combined, curve, lock, fan
        const bool expectReset[4] = { true, true, true, false };
        for (int restoreCase = 0; restoreCase < 4; ++restoreCase) {
            DesiredSettings request = {};
            if (!service_build_full_restore_request(&restoreCases[restoreCase], &request) ||
                request.resetOcBeforeApply != expectReset[restoreCase]) {
                return 480 + restoreCase;
            }
            request.resetOcBeforeApply = 0;
            if (!desired_settings_equal(
                    &request, &restoreCases[restoreCase])) {
                return 484 + restoreCase;
            }
        }

        // Named-profile transitions replace ownership instead of inheriting
        // omitted controls from another account/profile. Hardware cleanup may
        // write defaults for fields Green Curve previously owned, while the
        // new ownership declaration itself remains byte-for-byte unchanged.
        DesiredSettings previousProfile = restoreCases[0];
        previousProfile.hasXbarOffsetKhz = 1;
        previousProfile.xbarOffsetKhz = 120000;
        previousProfile.hasXbarMsvddOffsetUv = 1;
        previousProfile.xbarMsvddOffsetUv = 20000;
        DesiredSettings nextFanOnly = {};
        nextFanOnly.hasFan = 1;
        nextFanOnly.fanMode = FAN_MODE_FIXED;
        nextFanOnly.fanPercent = 47;
        DesiredSettings transition = {};
        if (!service_build_profile_transition_request(
                &previousProfile, &nextFanOnly, &transition)) return 530;
        if (!transition.resetOcBeforeApply || !transition.hasGpuOffset ||
            transition.gpuOffsetMHz != 0 || !transition.hasMemOffset ||
            transition.memOffsetMHz != 0 || !transition.hasPowerLimit ||
            transition.powerLimitPct != 100 || !transition.hasFan ||
            transition.fanMode != FAN_MODE_FIXED ||
            transition.fanPercent != 47 ||
            !transition.hasXbarOffsetKhz || transition.xbarOffsetKhz != 0 ||
            !transition.hasXbarMsvddOffsetUv ||
            transition.xbarMsvddOffsetUv != 0) return 531;
        if (!nextFanOnly.hasFan || nextFanOnly.hasGpuOffset ||
            nextFanOnly.hasMemOffset || nextFanOnly.hasPowerLimit ||
            nextFanOnly.hasXbarOffsetKhz ||
            nextFanOnly.hasXbarMsvddOffsetUv) return 532;

        // A partial XBAR declaration preserves only the sibling it omits.
        DesiredSettings previousClockOnly = {};
        previousClockOnly.hasXbarOffsetKhz = 1;
        previousClockOnly.xbarOffsetKhz = 90000;
        previousClockOnly.hasXbarMsvddOffsetUv = 1;
        previousClockOnly.xbarMsvddOffsetUv = 15000;
        DesiredSettings nextClockOnly = {};
        nextClockOnly.hasXbarOffsetKhz = 1;
        nextClockOnly.xbarOffsetKhz = -30000;
        transition = {};
        if (!service_build_profile_transition_request(
                &previousClockOnly, &nextClockOnly, &transition) ||
            !transition.hasXbarOffsetKhz || transition.xbarOffsetKhz != -30000 ||
            !transition.hasXbarMsvddOffsetUv ||
            transition.xbarMsvddOffsetUv != 0) return 4520;

        // A previous profile owning a SYS offset and a next profile omitting it
        // must emit an explicit stock reset for exactly that field.
        {
            DesiredSettings prevSys = {};
            prevSys.hasSysClkOffsetKhz = 1;
            prevSys.sysClkOffsetKhz = 75000;
            DesiredSettings nextNoSys = {};
            nextNoSys.hasXbarOffsetKhz = 1;
            nextNoSys.xbarOffsetKhz = 10000;
            DesiredSettings sysTransition = {};
            if (!service_build_profile_transition_request(
                    &prevSys, &nextNoSys, &sysTransition)) return 4531;
            // SYS-only transitions do not own VF policy, so they must not
            // force the full OC baseline reset (same rule as XBAR).
            if (sysTransition.resetOcBeforeApply ||
                !sysTransition.hasSysClkOffsetKhz ||
                sysTransition.sysClkOffsetKhz != 0) return 4532;
            // The next profile's owned XBAR intent travels unchanged in the
            // same request; only the omitted SYS field is rewritten to stock.
            if (!sysTransition.hasXbarOffsetKhz ||
                sysTransition.xbarOffsetKhz != 10000) return 4533;
        }

        DesiredSettings previousFanOnly = nextFanOnly;
        DesiredSettings nextCurveOnly = {};
        nextCurveOnly.hasCurvePoint[90] = 1;
        nextCurveOnly.curvePointMHz[90] = 2505;
        transition = {};
        if (!service_build_profile_transition_request(
                &previousFanOnly, &nextCurveOnly, &transition) ||
            !transition.resetOcBeforeApply || !transition.hasFan ||
            !transition.fanAuto || transition.fanMode != FAN_MODE_AUTO ||
            !transition.hasCurvePoint[90]) return 533;

        // Arriving at a lock-only profile from nothing still needs the clean
        // baseline the lock's own anchor/tail write assumes -- the same reason
        // LEAVING one already forced it.
        DesiredSettings nextLockOnly = {};
        nextLockOnly.hasLock = 1;
        nextLockOnly.lockCi = 91;
        nextLockOnly.lockMHz = 2540;
        transition = {};
        if (!service_build_profile_transition_request(
                nullptr, &nextLockOnly, &transition) ||
            !transition.resetOcBeforeApply) return 534;
        transition = {};
        if (!service_build_profile_transition_request(
                &nextLockOnly, &nextFanOnly, &transition) ||
            !transition.resetOcBeforeApply || transition.hasGpuOffset) return 535;
        // A memory-only or power-only intent is still left alone: those fields
        // can belong to another tool, and resetting the OC baseline for them
        // would clear settings Green Curve never claimed.
        DesiredSettings sparseNonVf[2] = {};
        sparseNonVf[0].hasMemOffset = 1;
        sparseNonVf[0].memOffsetMHz = 500;
        sparseNonVf[1].hasPowerLimit = 1;
        sparseNonVf[1].powerLimitPct = 90;
        for (int sparseCase = 0; sparseCase < 2; ++sparseCase) {
            DesiredSettings sparseRequest = {};
            if (!service_build_full_restore_request(&sparseNonVf[sparseCase],
                                                    &sparseRequest) ||
                sparseRequest.resetOcBeforeApply) return 536 + sparseCase;
        }

        // Driver recovery waits for explicit proof readiness. It dominates a
        // coincident standby and a duplicate recovery event cannot authorize a
        // second write after WRITE_STARTED.
        ServiceLifecycleState recovery = {};
        event = {};
        event.type = SERVICE_LIFECYCLE_EVENT_SUSPEND;
        service_lifecycle_reduce(&recovery, &event);
        event.type = SERVICE_LIFECYCLE_EVENT_RESUME;
        service_lifecycle_reduce(&recovery, &event);
        event = {};
        event.type = SERVICE_LIFECYCLE_EVENT_DRIVER_RECOVERY;
        event.driverProofReady = 0;
        decision = service_lifecycle_reduce(&recovery, &event);
        if (decision.wakeWorker || !recovery.driverPending || recovery.standbyPending) return 427;
        event.type = SERVICE_LIFECYCLE_EVENT_DRIVER_WRITE_STARTED;
        decision = service_lifecycle_reduce(&recovery, &event);
        if (decision.authorizeDriverWrite) return 428;
        event.type = SERVICE_LIFECYCLE_EVENT_DRIVER_PROOF_SIGNAL;
        event.driverProofReady = 1;
        decision = service_lifecycle_reduce(&recovery, &event);
        if (!decision.wakeWorker) return 429;
        event.type = SERVICE_LIFECYCLE_EVENT_DRIVER_WRITE_STARTED;
        decision = service_lifecycle_reduce(&recovery, &event);
        if (decision.authorizeDriverWrite) return 488; // old/non-nonce process
        event.controlledRecoveryValidated = 1;
        decision = service_lifecycle_reduce(&recovery, &event);
        int driverWrites = decision.authorizeDriverWrite ? 1 : 0;
        event.type = SERVICE_LIFECYCLE_EVENT_DRIVER_RECOVERY;
        decision = service_lifecycle_reduce(&recovery, &event);
        event.type = SERVICE_LIFECYCLE_EVENT_DRIVER_WRITE_STARTED;
        decision = service_lifecycle_reduce(&recovery, &event);
        if (decision.authorizeDriverWrite) ++driverWrites;
        if (driverWrites != 1) return 430;

        // Global/null DBT_DEVNODES_CHANGED is read-only and cannot create or
        // authorize restoration work.
        ServiceLifecycleState devnodes = {};
        event = {};
        event.type = SERVICE_LIFECYCLE_EVENT_DEVNODES_CHANGED;
        decision = service_lifecycle_reduce(&devnodes, &event);
        if (!decision.readOnlyReenumerate || decision.authorizeLogonWrite ||
            decision.authorizeStandbyWrite || decision.authorizeDriverWrite ||
            devnodes.logonPending || devnodes.standbyPending || devnodes.driverPending) return 431;
    }

#if defined(_WIN32)
    // Windows Task Scheduler XML: the classifier lives in an amalgamated Win32
    // shard.  Linux logon/startup behaviour is systemd unit generation, covered
    // by linux_service_install_policy assertions above.

    // Executable startup-task XML fixtures.  These call the production pure
    // classifier and cover Task Scheduler's omitted defaults, SID principals,
    // quoting, compatible delay/elevation, and broken identity/action state.
    {
        const WCHAR* expectedUser = L"TEST\\User";
        const WCHAR* expectedExe = L"C:\\Program Files\\Green Curve\\greencurve.exe";
        const WCHAR* expectedConfig = L"C:\\Users\\Test User\\config.ini";
        const WCHAR* expectedWorkingDir = L"C:\\Program Files\\Green Curve";
        std::wstring canonical = LR"XML(
<Task>
  <Triggers><LogonTrigger><UserId>TEST\User</UserId></LogonTrigger></Triggers>
  <Principals><Principal><UserId>TEST\User</UserId><LogonType>InteractiveToken</LogonType></Principal></Principals>
  <Settings><StartWhenAvailable>true</StartWhenAvailable><MultipleInstancesPolicy>IgnoreNew</MultipleInstancesPolicy><DisallowStartIfOnBatteries>false</DisallowStartIfOnBatteries><StopIfGoingOnBatteries>false</StopIfGoingOnBatteries><ExecutionTimeLimit>PT3M</ExecutionTimeLimit></Settings>
  <Actions><Exec><Command>C:\Program Files\Green Curve\greencurve.exe</Command><Arguments>--logon-start --config &quot;C:\Users\Test User\config.ini&quot;</Arguments><WorkingDirectory>C:\Program Files\Green Curve</WorkingDirectory></Exec></Actions>
</Task>)XML";
        auto classify = [&](const std::wstring& xml, const WCHAR* user = nullptr) {
            char detail[512] = {};
            return startup_task_definition_classify_xml(xml.c_str(),
                user ? user : expectedUser, expectedExe, expectedConfig,
                expectedWorkingDir, detail, sizeof(detail));
        };
        auto replace_all = [](std::wstring value, const std::wstring& from,
                              const std::wstring& to) {
            size_t pos = 0;
            while ((pos = value.find(from, pos)) != std::wstring::npos) {
                value.replace(pos, from.size(), to);
                pos += to.size();
            }
            return value;
        };
        auto insert_after = [](std::wstring value, const std::wstring& marker,
                               const std::wstring& addition) {
            size_t pos = value.find(marker);
            if (pos != std::wstring::npos) value.insert(pos + marker.size(), addition);
            return value;
        };

        // Enabled and RunLevel are schema defaults and may be omitted or empty.
        {
            char canonicalDetail[512] = {};
            StartupTaskDefinitionClass canonicalClass =
                startup_task_definition_classify_xml(canonical.c_str(), expectedUser,
                    expectedExe, expectedConfig, expectedWorkingDir,
                    canonicalDetail, sizeof(canonicalDetail));
            if (canonicalClass != STARTUP_TASK_DEFINITION_CANONICAL) {
                fprintf(stderr, "canonical startup-task fixture classified %d: %s\n",
                    (int)canonicalClass, canonicalDetail);
                return 444;
            }
        }

        // Mirror the XML emitted by write_startup_task_xml(), including the
        // explicit values Task Scheduler may preserve in its query output. The
        // reported regression was caused by generator/verifier drift around
        // Enabled and LeastPrivilege, so omitted-default fixtures are not enough.
        std::wstring generatedCanonical = LR"XML(
<Task version="1.3" xmlns="http://schemas.microsoft.com/windows/2004/02/mit/task">
  <RegistrationInfo><Author>TEST\User</Author><Description>Notify the Green Curve service of an authenticated user logon.</Description></RegistrationInfo>
  <Triggers><LogonTrigger><Enabled>true</Enabled><UserId>TEST\User</UserId></LogonTrigger></Triggers>
  <Principals><Principal id="Author"><UserId>TEST\User</UserId><LogonType>InteractiveToken</LogonType><RunLevel>LeastPrivilege</RunLevel></Principal></Principals>
  <Settings>
    <MultipleInstancesPolicy>IgnoreNew</MultipleInstancesPolicy>
    <DisallowStartIfOnBatteries>false</DisallowStartIfOnBatteries>
    <StopIfGoingOnBatteries>false</StopIfGoingOnBatteries>
    <AllowHardTerminate>true</AllowHardTerminate>
    <StartWhenAvailable>true</StartWhenAvailable>
    <IdleSettings><StopOnIdleEnd>false</StopOnIdleEnd><RestartOnIdle>false</RestartOnIdle></IdleSettings>
    <AllowStartOnDemand>true</AllowStartOnDemand>
    <Enabled>true</Enabled><Hidden>false</Hidden><RunOnlyIfIdle>false</RunOnlyIfIdle><WakeToRun>false</WakeToRun>
    <ExecutionTimeLimit>PT3M</ExecutionTimeLimit><Priority>7</Priority>
  </Settings>
  <Actions Context="Author"><Exec><Command>C:\Program Files\Green Curve\greencurve.exe</Command><WorkingDirectory>C:\Program Files\Green Curve</WorkingDirectory><Arguments>--logon-start --config &quot;C:\Users\Test User\config.ini&quot;</Arguments></Exec></Actions>
</Task>)XML";
        {
            char generatedDetail[512] = {};
            StartupTaskDefinitionClass generatedClass =
                startup_task_definition_classify_xml(generatedCanonical.c_str(),
                    expectedUser, expectedExe, expectedConfig,
                    expectedWorkingDir, generatedDetail,
                    sizeof(generatedDetail));
            if (generatedClass != STARTUP_TASK_DEFINITION_CANONICAL) {
                fprintf(stderr, "generated startup-task fixture classified %d: %s\n",
                    (int)generatedClass, generatedDetail);
                return 600;
            }
        }
        std::wstring emptyDefaults = insert_after(canonical, L"<LogonTrigger>", L"<Enabled/>");
        emptyDefaults = insert_after(emptyDefaults, L"<Principal>", L"<RunLevel/>");
        emptyDefaults = insert_after(emptyDefaults, L"<Settings>", L"<Enabled/>");
        if (classify(emptyDefaults) != STARTUP_TASK_DEFINITION_CANONICAL) return 445;

        std::wstring sidPrincipal = replace_all(canonical, L"TEST\\User", L"S-1-5-18");
        if (classify(sidPrincipal, L"S-1-5-18") != STARTUP_TASK_DEFINITION_CANONICAL) return 446;

        std::wstring delayed = insert_after(canonical, L"<LogonTrigger>", L"<Delay>PT30S</Delay>");
        if (classify(delayed) != STARTUP_TASK_DEFINITION_COMPATIBLE_LEGACY) return 447;
        std::wstring highest = insert_after(canonical, L"<Principal>", L"<RunLevel>HighestAvailable</RunLevel>");
        if (classify(highest) != STARTUP_TASK_DEFINITION_COMPATIBLE_LEGACY) return 448;
        std::wstring oldLimit = replace_all(canonical, L"PT3M", L"PT0S");
        if (classify(oldLimit) != STARTUP_TASK_DEFINITION_COMPATIBLE_LEGACY) return 449;
        std::wstring omittedSafeDefault = replace_all(canonical,
            L"<MultipleInstancesPolicy>IgnoreNew</MultipleInstancesPolicy>", L"");
        if (classify(omittedSafeDefault) != STARTUP_TASK_DEFINITION_COMPATIBLE_LEGACY) return 540;

        std::wstring triggerDisabled = insert_after(canonical, L"<LogonTrigger>", L"<Enabled>false</Enabled>");
        if (classify(triggerDisabled) != STARTUP_TASK_DEFINITION_BROKEN) return 450;
        std::wstring taskDisabled = insert_after(canonical, L"<Settings>", L"<Enabled>false</Enabled>");
        if (classify(taskDisabled) != STARTUP_TASK_DEFINITION_BROKEN) return 451;
        std::wstring wrongUser = replace_all(canonical, L"TEST\\User", L"TEST\\Other");
        if (classify(wrongUser) != STARTUP_TASK_DEFINITION_BROKEN) return 452;
        std::wstring staleExe = replace_all(canonical,
            L"C:\\Program Files\\Green Curve\\greencurve.exe",
            L"C:\\Old\\greencurve.exe");
        if (classify(staleExe) != STARTUP_TASK_DEFINITION_BROKEN) return 453;
        std::wstring staleConfig = replace_all(canonical,
            L"C:\\Users\\Test User\\config.ini", L"C:\\Old\\config.ini");
        if (classify(staleConfig) != STARTUP_TASK_DEFINITION_BROKEN) return 454;
        std::wstring staleAction = insert_after(canonical, L"<Actions>",
            L"<Exec><Command>C:\\Other.exe</Command><Arguments>--bad</Arguments><WorkingDirectory>C:\\</WorkingDirectory></Exec>");
        if (classify(staleAction) != STARTUP_TASK_DEFINITION_BROKEN) return 455;
        std::wstring extraTrigger = insert_after(canonical, L"<Triggers>",
            L"<BootTrigger><Enabled>true</Enabled></BootTrigger>");
        if (classify(extraTrigger) != STARTUP_TASK_DEFINITION_BROKEN) return 541;
        std::wstring repeatedTrigger = insert_after(canonical, L"<LogonTrigger>",
            L"<Repetition><Interval>PT1M</Interval></Repetition>");
        if (classify(repeatedTrigger) != STARTUP_TASK_DEFINITION_BROKEN) return 542;
        std::wstring pt1s = replace_all(canonical, L"PT3M", L"PT1S");
        if (classify(pt1s) != STARTUP_TASK_DEFINITION_BROKEN) return 543;
        std::wstring queued = replace_all(canonical, L"IgnoreNew", L"Queue");
        if (classify(queued) != STARTUP_TASK_DEFINITION_BROKEN) return 544;
        std::wstring parallel = replace_all(canonical, L"IgnoreNew", L"Parallel");
        if (classify(parallel) != STARTUP_TASK_DEFINITION_BROKEN) return 545;
        std::wstring stopExisting = replace_all(canonical, L"IgnoreNew", L"StopExisting");
        if (classify(stopExisting) != STARTUP_TASK_DEFINITION_BROKEN) return 546;
        std::wstring batteryGated = replace_all(canonical,
            L"<DisallowStartIfOnBatteries>false</DisallowStartIfOnBatteries>",
            L"<DisallowStartIfOnBatteries>true</DisallowStartIfOnBatteries>");
        if (classify(batteryGated) != STARTUP_TASK_DEFINITION_BROKEN) return 547;
        std::wstring idleGated = insert_after(canonical, L"<Settings>",
            L"<RunOnlyIfIdle>true</RunOnlyIfIdle>");
        if (classify(idleGated) != STARTUP_TASK_DEFINITION_BROKEN) return 548;
        std::wstring restartOnFailure = insert_after(canonical, L"<Settings>",
            L"<RestartOnFailure><Interval>PT1M</Interval><Count>3</Count></RestartOnFailure>");
        if (classify(restartOnFailure) != STARTUP_TASK_DEFINITION_BROKEN) return 549;
        std::wstring unavailable = replace_all(canonical,
            L"<StartWhenAvailable>true</StartWhenAvailable>",
            L"<StartWhenAvailable>false</StartWhenAvailable>");
        if (classify(unavailable) != STARTUP_TASK_DEFINITION_BROKEN) return 550;
        std::wstring missingBatteryPolicy = replace_all(canonical,
            L"<DisallowStartIfOnBatteries>false</DisallowStartIfOnBatteries>", L"");
        if (classify(missingBatteryPolicy) != STARTUP_TASK_DEFINITION_BROKEN) return 551;
        std::wstring hiddenTask = insert_after(canonical, L"<Settings>",
            L"<Hidden>true</Hidden>");
        if (classify(hiddenTask) != STARTUP_TASK_DEFINITION_BROKEN) return 552;
        std::wstring wrongPriority = insert_after(canonical, L"<Settings>",
            L"<Priority>4</Priority>");
        if (classify(wrongPriority) != STARTUP_TASK_DEFINITION_BROKEN) return 553;
        std::wstring noHardTerminate = insert_after(canonical, L"<Settings>",
            L"<AllowHardTerminate>false</AllowHardTerminate>");
        if (classify(noHardTerminate) != STARTUP_TASK_DEFINITION_BROKEN) return 554;
    }
#endif // _WIN32

#if defined(_WIN32)
    // Win32 config-INI storage suites: the logon transaction, stable
    // selected-GPU identity parsing, logon_shared_slot round-trip and the
    // profile-repair readback all drive config_utils.cpp, which is a Win32
    // implementation.  Linux profile/INI storage is linux_port*.cpp.

    // The mutually-exclusive logon selection is one locked transaction.
    // Injected commit failure must leave BOTH old keys intact; successful
    // commit must update both while preserving unrelated [profiles] keys.
    {
    gc_DeleteFileUtf8(argv[1]);
        if (!set_config_int(argv[1], "profiles", "selected_slot", 4)) return 458;
        if (!set_config_int(argv[1], "profiles", "applied_slot", 3)) return 459;
        if (!set_config_int(argv[1], "profiles", "logon_slot", 1)) return 460;
        if (!set_config_int(argv[1], "profiles", "logon_shared_slot", 0)) return 461;

        auto failCommit = +[](const char*, const char*, void*, char* err, size_t errSize) -> bool {
            set_message(err, errSize, "injected config transaction failure");
            return false;
        };
        char txErr[256] = {};
        if (update_logon_profile_selection_transaction(argv[1], 0, 3,
                failCommit, nullptr, txErr, sizeof(txErr))) return 462;
        if (get_config_int(argv[1], "profiles", "logon_slot", -1) != 1 ||
            get_config_int(argv[1], "profiles", "logon_shared_slot", -1) != 0) return 463;

        auto commitWholeText = +[](const char* path, const char* text, void*,
                                   char* err, size_t errSize) -> bool {
            HANDLE file = CreateFileA(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr);
            if (file == INVALID_HANDLE_VALUE) {
                set_message(err, errSize, "test commit create failed");
                return false;
            }
            DWORD length = (DWORD)strlen(text);
            DWORD written = 0;
            bool ok = WriteFile(file, text, length, &written, nullptr) != FALSE &&
                written == length && FlushFileBuffers(file) != FALSE;
            CloseHandle(file);
            if (!ok) set_message(err, errSize, "test commit write failed");
            return ok;
        };
        if (!update_logon_profile_selection_transaction(argv[1], 0, 3,
                commitWholeText, nullptr, txErr, sizeof(txErr))) return 464;
        if (get_config_int(argv[1], "profiles", "logon_slot", -1) != 0 ||
            get_config_int(argv[1], "profiles", "logon_shared_slot", -1) != 3) return 465;
        if (get_config_int(argv[1], "profiles", "selected_slot", -1) != 4 ||
            get_config_int(argv[1], "profiles", "applied_slot", -1) != 3) return 466;
        if (update_logon_profile_selection_transaction(argv[1], 2, 3,
                commitWholeText, nullptr, txErr, sizeof(txErr))) return 467;

        // Async combo synchronization uses tagged item data, not list index.
        if (logon_profile_selection_item_data(2, 0) != 2) return 468;
        if (logon_profile_selection_item_data(0, 3) != (LOGON_COMBO_SHARED_FLAG | 3)) return 469;
        if (logon_profile_selection_item_data(2, 3) != (LOGON_COMBO_SHARED_FLAG | 3)) return 470;
        if (logon_profile_selection_item_data(-1, 99) != 0) return 471;

        // Applied ownership is service metadata, never live VF-MHz equality.
        if (applied_user_slot_from_service_profile(SERVICE_PROFILE_SOURCE_USER_SLOT, 2) != 2) return 472;
        if (applied_user_slot_from_service_profile(SERVICE_PROFILE_SOURCE_SHARED_SLOT, 2) != 0) return 473;
        if (applied_user_slot_from_service_profile(SERVICE_PROFILE_SOURCE_MACHINE_SLOT, 2) != 0) return 474;
        if (applied_user_slot_from_service_profile(SERVICE_PROFILE_SOURCE_AD_HOC, 2) != 0) return 475;
        if (applied_user_slot_from_service_profile(SERVICE_PROFILE_SOURCE_USER_SLOT, 0) != 0) return 476;

        // The [Profiles]/[profiles] case-insensitivity assertions (523-527)
        // moved out of this Win32-only block to the platform-neutral section
        // near assertion 21; the helper is no longer Win32-only.
    }

    // Stable selected-GPU identity config parsing is coherent and bounded.
    {
        DeleteFileA(argv[1]);
        if (!set_config_int(argv[1], "gpu", "selected_index", 1) ||
            !set_config_int(argv[1], "gpu", "selected_identity_version", 1) ||
            !set_config_string(argv[1], "gpu", "selected_device_id", "00001234") ||
            !set_config_string(argv[1], "gpu", "selected_subsystem_id", "00005678") ||
            !set_config_string(argv[1], "gpu", "selected_revision_id", "00000001") ||
            !set_config_string(argv[1], "gpu", "selected_ext_device_id", "00000002") ||
            !set_config_int(argv[1], "gpu", "selected_bdf_valid", 1) ||
            !set_config_int(argv[1], "gpu", "selected_pci_domain", 0) ||
            !set_config_int(argv[1], "gpu", "selected_pci_bus", 9) ||
            !set_config_int(argv[1], "gpu", "selected_pci_device", 0) ||
            !set_config_int(argv[1], "gpu", "selected_pci_function", 0)) return 609;
        ConfiguredGpuSelection configured = {};
        char configErr[128] = {};
        if (!load_configured_gpu_selection(argv[1], &configured,
                configErr, sizeof(configErr)) ||
            !configured.stableIdentityPresent || configured.legacyIndex != 1 ||
            configured.identity.deviceId != 0x1234 ||
            configured.identity.pciBus != 9) return 610;
        if (!set_config_string(argv[1], "gpu", "selected_device_id", "-1") ||
            load_configured_gpu_selection(argv[1], &configured,
                configErr, sizeof(configErr))) return 611;
        DeleteFileA(argv[1]);
    }

    // logon_shared_slot round-trips through the profile INI like the other
    // [profiles] keys (the save/clear rewriters must re-emit it; guarded by the
    // source regression checks for the actual rewrite paths).
    {
        if (!set_config_int(argv[1], "profiles", "logon_shared_slot", 3)) return 150;
        if (get_config_int(argv[1], "profiles", "logon_shared_slot", 0) != 3) return 151;
        if (!set_config_int(argv[1], "profiles", "logon_shared_slot", 0)) return 152;
        if (get_config_int(argv[1], "profiles", "logon_shared_slot", -1) != 0) return 153;
        DeleteFileA(argv[1]);
    }

    // Profile repair must tolerate INT_MIN offsets without signed overflow.
    // Under UBSan the old abs(INT_MIN) implementation aborted in this case.
    {
        DesiredSettings repair = {};
        repair.hasLock = true;
        repair.lockCi = 10;
        repair.lockMHz = 2000;
        repair.hasCurvePoint[7] = true;
        repair.curvePointMHz[7] = 1700;
        repair.hasCurvePoint[8] = true;
        repair.curvePointMHz[8] = 1900;
        repair.hasCurvePoint[9] = true;
        repair.curvePointMHz[9] = 1950;
        for (int ci = 10; ci < 14; ci++) {
            repair.hasCurvePoint[ci] = true;
            repair.curvePointMHz[ci] = 2000;
        }
        DeleteFileA(argv[1]);
        if (!set_config_int(argv[1], "curve", "point7_offset_khz", (-2147483647 - 1))) return 159;
        // The read mode is explicit: the repair runs inside both the editor
        // load and the drift-free ownership read, and neither may abort on a
        // stored offset of INT_MIN.
        repair_profile_locked_curve_readback_artifacts(argv[1], "curve", 1,
            &repair, PROFILE_READ_FOR_EDITOR);
        if (!repair.hasCurvePoint[7]) return 160;
        repair_profile_locked_curve_readback_artifacts(argv[1], "curve", 1,
            &repair, PROFILE_READ_FOR_OWNERSHIP);
        if (!repair.hasCurvePoint[7]) return 2244;
        DeleteFileA(argv[1]);
    }
#endif // _WIN32

    // CommandLineToArgvW-compatible quoting for elevated helper argv.
#if defined(_WIN32)
    {
        WCHAR cmd[512] = {};
        if (!pl_append_quoted_arg_w(cmd, ARRAY_COUNT(cmd), L"--config")) return 161;
        if (!pl_append_quoted_arg_w(cmd, ARRAY_COUNT(cmd), L"C:\\Path With Spaces\\quote\\\"tail\\\\config.ini")) return 162;
        if (!pl_append_quoted_arg_w(cmd, ARRAY_COUNT(cmd), L"--flag")) return 163;
        int parsedArgc = 0;
        LPWSTR* parsed = CommandLineToArgvW(cmd, &parsedArgc);
        if (!parsed) return 164;
        bool quoteOk = parsedArgc == 3 &&
            wcscmp(parsed[0], L"--config") == 0 &&
            wcscmp(parsed[1], L"C:\\Path With Spaces\\quote\\\"tail\\\\config.ini") == 0 &&
            wcscmp(parsed[2], L"--flag") == 0;
        LocalFree((HLOCAL)parsed);
        if (!quoteOk) return 165;
    }
#endif

    // F-LNX-TUI: one cell grid owns painting and hit testing across the full
    // responsive size/tab matrix. Every action rectangle must remain in bounds
    // and disjoint at compact, medium and wide breakpoints.
    {
        DesiredSettings tuiDesired = {};
        tuiDesired.hasGpuOffset = true;
        tuiDesired.gpuOffsetMHz = 475;
        tuiDesired.gpuOffsetExcludeLowCount = 70;
        tuiDesired.hasMemOffset = true;
        tuiDesired.memOffsetMHz = 3000;
        tuiDesired.hasPowerLimit = true;
        tuiDesired.powerLimitPct = 100;
        tuiDesired.hasLock = true;
        tuiDesired.lockCi = 76;
        tuiDesired.lockMHz = 2957;
        tuiDesired.lockMode = LOCK_MODE_FLATTEN;
        fan_curve_set_default(&tuiDesired.fanCurve);
        ServiceResponse tuiService = {};
        tuiService.state.serviceInstanceId = 1;
        tuiService.state.stateRevision = 1;
        tuiService.state.gpuGeneration = 1;
        tuiService.state.topologySignature = 1;
        tuiService.state.gpuPhase = SERVICE_GPU_PHASE_READY;
        tuiService.state.activeDesiredValid = true;
        tuiService.snapshot.initialized = true;
        tuiService.snapshot.loaded = true;
        tuiService.snapshot.adapterCount = 1;
        tuiService.snapshot.adapters[0].valid = true;
        tuiService.snapshot.selectedAdapterIndex = 0;
        tuiService.snapshot.numPopulated = VF_NUM_POINTS;
        for (int i = 0; i < VF_NUM_POINTS; ++i) {
            tuiService.snapshot.curve[i].volt_uV =
                (760u + (unsigned)i * 4u) * 1000u;
            tuiService.snapshot.curve[i].freq_kHz =
                (600u + (unsigned)i * 18u) * 1000u;
            tuiService.snapshot.freqOffsets[i] = i >= 70 ? 475000 : 0;
        }
        TuiViewModel vm = {};
        vm.desired = &tuiDesired;
        vm.service = &tuiService;
        vm.currentSlot = 1;
        vm.selectedPoint = 76;
        vm.vfScroll = 68;
        vm.serviceOnline = true;
        vm.selectedGpu = "0000:01:00.0 NVIDIA GeForce RTX";
        vm.gpuCount = 1;
        vm.configPath = "/home/user/.config/greencurve/config.ini";
        vm.status = "test";
        const int sizes[][2] = {
            {72,24}, {80,30}, {99,35}, {100,30}, {120,40},
            {139,48}, {140,36}, {160,48}, {220,70}
        };
        for (const auto& size : sizes) {
            for (int tab = TUI_TAB_VF; tab <= TUI_TAB_PROFILES; ++tab) {
                vm.tab = (TuiTab)tab;
                TuiLayout layout;
                build_tui_layout(vm, size[0], size[1], &layout);
                if (layout.tooSmall) return 200;
                if ((int)layout.cells.size() != size[0] * size[1]) return 201;
                if (layout.actions.empty()) return 202;
                if (!tui_layout_actions_valid(layout)) return 203;
                bool sawApply = false, sawReset = false, sawQuit = false;
                bool sawCurrentTab = false;
                for (const ClickAction& action : layout.actions) {
                    if (action.type == ACTION_APPLY) sawApply = true;
                    if (action.type == ACTION_APPLY_RESET) sawReset = true;
                    if (action.type == ACTION_QUIT) sawQuit = true;
                    if (action.type == ACTION_TAB_SET && action.value == tab)
                        sawCurrentTab = true;
                }
                if (!sawApply || !sawReset || !sawQuit || !sawCurrentTab) {
                    fprintf(stderr,
                        "TUI matrix missing action size=%dx%d tab=%d apply=%d reset=%d quit=%d tabAction=%d\n",
                        size[0], size[1], tab, sawApply, sawReset, sawQuit,
                        sawCurrentTab);
                    return 204;
                }
                if (size[0] >= 140 && size[1] >= 36 &&
                    layout.breakpoint != TUI_BREAKPOINT_WIDE) return 205;
                if (size[0] >= 100 && size[0] < 140 &&
                    layout.breakpoint != TUI_BREAKPOINT_MEDIUM) return 206;
            }
        }
        TuiLayout tooSmall;
        build_tui_layout(vm, 71, 23, &tooSmall);
        if (!tooSmall.tooSmall || !tooSmall.actions.empty()) return 207;

        vm.tab = TUI_TAB_VF;
        TuiPointValues excluded = tui_point_values(vm, 69);
        TuiPointValues offset = tui_point_values(vm, 70);
        TuiPointValues knee = tui_point_values(vm, 76);
        TuiPointValues tail = tui_point_values(vm, 77);
        if (excluded.rule != TUI_POINT_EXCLUDED ||
            excluded.targetMHz != excluded.liveMHz) return 208;
        if (offset.rule != TUI_POINT_GPU_OFFSET || offset.deltaMHz != 475)
            return 209;
        if (knee.rule != TUI_POINT_FLATTEN_KNEE || knee.targetMHz != 2957)
            return 210;
        if (tail.rule != TUI_POINT_FLATTEN_TAIL || tail.targetMHz != 2957)
            return 211;

        TuiLayout graphLayout;
        build_tui_layout(vm, 160, 48, &graphLayout);
        int graphPoint = tui_nearest_graph_point(vm, graphLayout.graphRect,
            graphLayout.graphRect.x + graphLayout.graphRect.width / 2);
        if (graphPoint < 50 || graphPoint > 78) return 212;

        // The VF graph carries labelled axes: clock up the left edge, voltage
        // along the bottom.  Before this the panel ended with one centred
        // "760-1268 mV - 600-2900 MHz" summary, so the two endpoints were the
        // only numbers on it and nothing in between could be read off the
        // trace.  Checked against the rendered CELLS, not against the drawing
        // code, so a layout change that pushes a label out of the panel fails
        // here rather than on someone's terminal.
        {
            auto cellsAt = [&](const TuiLayout& layout, int row, int column,
                               int width) {
                std::string out;
                for (int i = 0; i < width; ++i) {
                    int x = column + i;
                    if (row < 1 || x < 1 || row > layout.height ||
                        x > layout.width) continue;
                    out += layout.cells[(size_t)(row - 1) * layout.width +
                                        (x - 1)].glyph;
                }
                return out;
            };
            const TuiRect& plot = graphLayout.graphRect;
            if (plot.width <= 0 || plot.height <= 0) return 2069;

            // Voltage endpoints as the view model actually holds them; the
            // labels must name these, not a rounded stand-in.
            unsigned int minMv = 0, maxMv = 0;
            for (int i = 0; i < VF_NUM_POINTS; ++i) {
                if (tuiService.snapshot.curve[i].freq_kHz == 0) continue;
                unsigned int mv = tuiService.snapshot.curve[i].volt_uV / 1000u;
                if (!minMv || mv < minMv) minMv = mv;
                if (mv > maxMv) maxMv = mv;
            }
            if (!minMv || maxMv <= minMv) return 2070;

            // X axis: the row directly below the plot, still inside the panel.
            std::string xAxis = cellsAt(graphLayout, plot.y + plot.height,
                                        plot.x, plot.width);
            char expectMin[16] = {};
            char expectMax[24] = {};
            snprintf(expectMin, sizeof(expectMin), "%u", minMv);
            snprintf(expectMax, sizeof(expectMax), "%u mV", maxMv);
            // The lowest voltage starts the axis...
            if (xAxis.compare(0, strlen(expectMin), expectMin) != 0) {
                fprintf(stderr, "VF x-axis does not start at %s: \"%s\"\n",
                        expectMin, xAxis.c_str());
                return 2071;
            }
            // ...and the highest carries the unit.  It is placed first, from
            // the right, precisely so it can never be the tick dropped for
            // want of room -- an earlier version reserved the right edge for a
            // standalone "mV" and silently suppressed the maximum voltage.
            if (xAxis.find(expectMax) == std::string::npos) {
                fprintf(stderr, "VF x-axis is missing \"%s\": \"%s\"\n",
                        expectMax, xAxis.c_str());
                return 2072;
            }

            // Y axis: right-aligned MHz in the reserved left margin, highest at
            // the top row and lowest at the bottom one.
            std::string yTop = cellsAt(graphLayout, plot.y, plot.x - 5, 4);
            std::string yBottom = cellsAt(graphLayout, plot.y + plot.height - 1,
                                          plot.x - 5, 4);
            // strtol, not atoi: the tidy ratchet rejects unchecked string ->
            // number conversions, and a label that failed to render must read
            // as 0 here rather than as an unspecified value.
            int topMHz = (int)strtol(yTop.c_str(), nullptr, 10);
            int bottomMHz = (int)strtol(yBottom.c_str(), nullptr, 10);
            if (topMHz <= 0 || topMHz <= bottomMHz || bottomMHz < 0) {
                fprintf(stderr, "VF y-axis top=\"%s\" bottom=\"%s\"\n",
                        yTop.c_str(), yBottom.c_str());
                return 2073;
            }
            // The unit sits on the free row between the panel title and the
            // plot, so naming the axis never costs the graph a row.
            if (cellsAt(graphLayout, plot.y - 1, plot.x - 5, 4).find("MHz") ==
                std::string::npos) return 2074;

            // Labels are inside the panel: the margin is reserved by moving the
            // plot right, never by drawing over the border.
            if (plot.x - 5 < 3) return 2075;

            // A tick names the same voltage a click on that column selects, so
            // the axis cannot disagree with the graph's own hit testing.
            int clicked = tui_nearest_graph_point(vm, plot, plot.x);
            if (clicked < 0 ||
                tuiService.snapshot.curve[clicked].volt_uV / 1000u != minMv)
                return 2076;

            // Every laid-out size that draws a graph gets both axes; the
            // narrow end must degrade by dropping labels, never the plot.
            const int graphSizes[][2] = {{100,24}, {100,30}, {120,40}, {139,48},
                                         {140,36}, {160,48}, {220,70}};
            for (const auto& size : graphSizes) {
                TuiLayout axisLayout;
                build_tui_layout(vm, size[0], size[1], &axisLayout);
                if (axisLayout.tooSmall) return 2077;
                if (!tui_layout_actions_valid(axisLayout)) return 2078;
                const TuiRect& p = axisLayout.graphRect;
                if (p.width < 12 || p.height < 4) return 2079;
                std::string axisRow = cellsAt(axisLayout, p.y + p.height, p.x,
                                              p.width);
                if (axisRow.find(" mV") == std::string::npos) {
                    fprintf(stderr, "VF x-axis unit missing at %dx%d: \"%s\"\n",
                            size[0], size[1], axisRow.c_str());
                    return 2080;
                }
                if (cellsAt(axisLayout, p.y - 1, p.x - 5, 4).find("MHz") ==
                    std::string::npos) {
                    fprintf(stderr, "VF y-axis unit missing at %dx%d\n",
                            size[0], size[1]);
                    return 2081;
                }
            }
        }

        ServiceResponse degraded = fake_ready_service_response(50, 3, 2);
        degraded.state.gpuPhase = SERVICE_GPU_PHASE_DEGRADED;
        degraded.state.validSections &=
            ~SERVICE_STATE_SECTION_CURVE_TOPOLOGY;
        degraded.state.topologySignature = 0;
        degraded.snapshot.loaded = false;
        degraded.snapshot.vfReadSupported = false;
        degraded.snapshot.vfWriteSupported = false;
        degraded.snapshot.numPopulated = 0;
        memset(degraded.snapshot.curve, 0, sizeof(degraded.snapshot.curve));
        degraded.snapshot.health.reason = SERVICE_GPU_HEALTH_VF_STATUS_FAILED;
        degraded.snapshot.health.driverStatus = -6;
        degraded.snapshot.health.availableMutationDomains =
            SERVICE_MUTATION_DOMAIN_POWER;
        degraded.snapshot.health.vfSnapshotFresh = false;
        gc_strlcpy(degraded.snapshot.health.detail,
            sizeof(degraded.snapshot.health.detail),
            "getStatus status=-6 (HANDLE_INVALIDATED)");
        DesiredSettings degradedDesired = {};
        degradedDesired.hasPowerLimit = true;
        degradedDesired.powerLimitPct = 85;
        vm.desired = &degradedDesired;
        vm.service = &degraded;
        vm.serviceOnline = true;
        vm.draftAttached = true;
        vm.dirty = true;
        vm.tab = TUI_TAB_VF;
        TuiLayout degradedLayout;
        build_tui_layout(vm, 160, 48, &degradedLayout);
        std::string degradedScreen;
        for (int row = 0; row < degradedLayout.height; ++row) {
            for (int column = 0; column < degradedLayout.width; ++column)
                degradedScreen += degradedLayout.cells[
                    (size_t)row * degradedLayout.width + column].glyph;
            degradedScreen += '\n';
        }
        if (degradedScreen.find("DAEMON ONLINE") == std::string::npos ||
            degradedScreen.find("GPU DEGRADED") == std::string::npos ||
            degradedScreen.find("VF status read failed") == std::string::npos ||
            degradedScreen.find("Refresh to rebind") == std::string::npos ||
            degradedScreen.find("Waiting for a complete") != std::string::npos)
            return 1240;
        for (const ClickAction& action : degradedLayout.actions) {
            if (action.type == ACTION_FIELD_EDIT &&
                action.index == TUI_FIELD_VF_TARGET) return 1241;
        }
        vm.serviceOnline = false;
        vm.service = nullptr;
        TuiLayout offlineLayout;
        build_tui_layout(vm, 160, 48, &offlineLayout);
        std::string offlineScreen;
        for (const TuiCell& cell : offlineLayout.cells)
            offlineScreen += cell.glyph;
        if (offlineScreen.find("DAEMON OFFLINE") == std::string::npos ||
            offlineScreen.find("Daemon offline") == std::string::npos)
            return 1242;

        // Numeric-field editing: entering a field pre-selects its contents, so
        // the first keystroke REPLACES the value.  Before this, the buffer was
        // seeded with the current value and every digit appended to it, so
        // clicking a field showing 100 and typing 50 produced 10050.
        {
            char text[32] = {};
            bool selectAll = false;

            // First entry: selected, so the first digit wipes the old value.
            snprintf(text, sizeof(text), "%d", 100);
            selectAll = true;
            if (!tui_edit_insert_character(text, sizeof(text), &selectAll, '5'))
                return 2082;
            if (strcmp(text, "5") != 0 || selectAll) return 2083;
            // The selection is consumed by the FIRST character only; the rest
            // of the number types normally.
            if (!tui_edit_insert_character(text, sizeof(text), &selectAll, '0'))
                return 2084;
            if (strcmp(text, "50") != 0) return 2085;

            // Second click into the same open field: selection dropped, buffer
            // kept, so typing amends the existing number.
            snprintf(text, sizeof(text), "%d", 100);
            selectAll = false;
            if (!tui_edit_insert_character(text, sizeof(text), &selectAll, '5'))
                return 2086;
            if (strcmp(text, "1005") != 0) return 2087;

            // A sign replaces a selected value (typing "-50" over "100"), and
            // is still refused once digits are present.
            snprintf(text, sizeof(text), "%d", 100);
            selectAll = true;
            if (!tui_edit_insert_character(text, sizeof(text), &selectAll, '-'))
                return 2088;
            if (strcmp(text, "-") != 0 || selectAll) return 2089;
            if (!tui_edit_insert_character(text, sizeof(text), &selectAll, '5'))
                return 2090;
            if (tui_edit_insert_character(text, sizeof(text), &selectAll, '-'))
                return 2091;
            if (strcmp(text, "-5") != 0) return 2092;

            // A REJECTED keystroke must not be what silently discards the
            // value the user can still see highlighted.
            snprintf(text, sizeof(text), "%d", 100);
            selectAll = true;
            if (tui_edit_insert_character(text, sizeof(text), &selectAll, 'x'))
                return 2093;
            if (strcmp(text, "100") != 0 || !selectAll) return 2094;

            // Backspace over a selection clears the whole value, as in any GUI
            // field; with no selection it removes one character.
            tui_edit_backspace(text, &selectAll);
            if (text[0] != 0 || selectAll) return 2095;
            snprintf(text, sizeof(text), "%d", 100);
            selectAll = false;
            tui_edit_backspace(text, &selectAll);
            if (strcmp(text, "10") != 0) return 2096;
            tui_edit_backspace(text, &selectAll);
            tui_edit_backspace(text, &selectAll);
            tui_edit_backspace(text, &selectAll);   // already empty
            if (text[0] != 0) return 2097;

            // A full buffer refuses more input rather than overrunning.
            for (size_t i = 0; i + 1 < sizeof(text); ++i) text[i] = '7';
            text[sizeof(text) - 1] = 0;
            selectAll = false;
            if (tui_edit_insert_character(text, sizeof(text), &selectAll, '7'))
                return 2098;
            if (strlen(text) != sizeof(text) - 1) return 2099;

            // Only a click on the field that is actually open keeps its buffer;
            // anything else commits and opens a fresh, pre-selected editor.
            if (!tui_edit_click_targets_active_field(true, TUI_FIELD_VF_TARGET,
                    7, TUI_FIELD_VF_TARGET, 7)) return 2206;
            if (tui_edit_click_targets_active_field(true, TUI_FIELD_VF_TARGET,
                    7, TUI_FIELD_VF_TARGET, 8)) return 2207;
            if (tui_edit_click_targets_active_field(true, TUI_FIELD_VF_TARGET,
                    7, TUI_FIELD_GPU_OFFSET, 7)) return 2208;
            if (tui_edit_click_targets_active_field(false, TUI_FIELD_VF_TARGET,
                    7, TUI_FIELD_VF_TARGET, 7)) return 2209;
        }

        std::string degLine = "ab\xC2\xB0""cd";
        if (tui_display_columns(degLine) != 5) return 213;
        if (tui_column_to_byte_offset(degLine, 4) != 4) return 214;

        // A Linux path or driver string can contain malformed/truncated UTF-8.
        // The cell writer must consume the invalid lead byte once, not jump
        // beyond the NUL terminator and render unrelated adjacent memory.
        TuiLayout utf8Layout = {};
        utf8Layout.width = 4;
        utf8Layout.height = 1;
        utf8Layout.cells.resize(4);
        TuiCanvas utf8Canvas(vm, &utf8Layout);
        utf8Canvas.clear();
        char truncatedUtf8[6] = {'A', (char)0xE2, 0, 0, 'X', 0};
        utf8Canvas.text(1, 1, 4, truncatedUtf8, TUI_STYLE_TEXT);
        if (strcmp(utf8Layout.cells[0].glyph, "A") != 0 ||
            (unsigned char)utf8Layout.cells[1].glyph[0] != 0xE2 ||
            strcmp(utf8Layout.cells[2].glyph, " ") != 0) return 215;
    }

    // F-AUTO-PROFILE: the auto-profile rule resolver is a pure, ordered,
    // first-match-wins decision.  These guard the matching contract (exe /
    // title / class / fullscreen, require_focus, default fallback) the whole
    // feature depends on.
    {
        // Case-insensitive substring helper.
        if (!auto_profile_text_contains_ci("Google Chrome", "chrome")) return 220;
        if (auto_profile_text_contains_ci("Google Chrome", "firefox")) return 221;
        if (auto_profile_text_contains_ci("abc", "")) return 222;   // empty pattern never matches
        if (auto_profile_text_contains_ci(nullptr, "x")) return 223;

        // Match-type name round-trip.
        if (auto_profile_match_type_from_name("exe") != AUTO_MATCH_EXE) return 224;
        if (auto_profile_match_type_from_name("fullscreen") != AUTO_MATCH_FULLSCREEN) return 225;
        if (auto_profile_match_type_from_name("bogus") != AUTO_MATCH_NONE) return 226;
        if (strcmp(auto_profile_match_type_name(AUTO_MATCH_TITLE), "title") != 0) return 227;

        AutoProfileConfig cfg = {};
        auto_profile_config_set_defaults(&cfg);
        cfg.enabled = true;
        cfg.defaultSlot = 1;
        cfg.ruleCount = 3;
        cfg.rules[0] = { AUTO_MATCH_EXE, "game.exe", true, 2 };
        cfg.rules[1] = { AUTO_MATCH_TITLE, "YouTube", true, 3 };
        cfg.rules[2] = { AUTO_MATCH_FULLSCREEN, "", true, 4 };

        ForegroundInfo fg = {};
        fg.valid = true;
        ProcessPresence pres = {};

        // exe focus match (case-insensitive).
        gc_strlcpy(fg.exeName, sizeof(fg.exeName), "GAME.EXE");
        gc_strlcpy(fg.title, sizeof(fg.title), "loading");
        fg.isFullscreen = false;
        if (resolve_auto_profile_slot(&cfg, &fg, &pres) != 2) return 228;

        // title match when exe does not match.
        gc_strlcpy(fg.exeName, sizeof(fg.exeName), "chrome.exe");
        gc_strlcpy(fg.title, sizeof(fg.title), "Cats - YouTube");
        if (resolve_auto_profile_slot(&cfg, &fg, &pres) != 3) return 229;

        // fullscreen fallback rule.
        gc_strlcpy(fg.exeName, sizeof(fg.exeName), "someapp.exe");
        gc_strlcpy(fg.title, sizeof(fg.title), "no keyword");
        fg.isFullscreen = true;
        if (resolve_auto_profile_slot(&cfg, &fg, &pres) != 4) return 230;

        // first-match-wins: exe rule outranks the later title + fullscreen rules.
        gc_strlcpy(fg.exeName, sizeof(fg.exeName), "game.exe");
        gc_strlcpy(fg.title, sizeof(fg.title), "YouTube");
        fg.isFullscreen = true;
        if (resolve_auto_profile_slot(&cfg, &fg, &pres) != 2) return 231;

        // no match → default slot.
        gc_strlcpy(fg.exeName, sizeof(fg.exeName), "notepad.exe");
        gc_strlcpy(fg.title, sizeof(fg.title), "Untitled");
        fg.isFullscreen = false;
        if (resolve_auto_profile_slot(&cfg, &fg, &pres) != 1) return 232;

        // require_focus honored: a running-but-not-foreground exe does NOT match
        // a focus-required rule.
        pres.rulePresent[0] = true;   // pretend game.exe is running in background
        if (resolve_auto_profile_slot(&cfg, &fg, &pres) != 1) return 233;

        // focus-optional exe rule matches on presence alone.
        AutoProfileConfig bg = {};
        auto_profile_config_set_defaults(&bg);
        bg.enabled = true;
        bg.defaultSlot = 1;
        bg.ruleCount = 1;
        bg.rules[0] = { AUTO_MATCH_EXE, "bg.exe", false, 5 };
        ForegroundInfo other = {};
        other.valid = true;
        gc_strlcpy(other.exeName, sizeof(other.exeName), "explorer.exe");
        ProcessPresence bgPres = {};
        bgPres.rulePresent[0] = true;
        if (resolve_auto_profile_slot(&bg, &other, &bgPres) != 5) return 234;
        bgPres.rulePresent[0] = false;
        if (resolve_auto_profile_slot(&bg, &other, &bgPres) != 1) return 235;
    }

    // F-AUTO-PROFILE: controller state machine — coalescing, cooldown, and the
    // manual-pin hotkey semantics (same slot twice → back to auto).
    {
        AutoProfileConfig cfg = {};
        auto_profile_config_set_defaults(&cfg);
        cfg.enabled = true;
        cfg.defaultSlot = 1;
        cfg.switchDebounceMs = 800;
        cfg.minSwitchIntervalMs = 4000;

        AutoProfileController c = {};
        ap_controller_init(&c, &cfg);
        c.appliedSlot = 1;   // assume default is applied

        // Coalescing: A(1)->B(2)->A(1) within debounce yields NO switch to B.
        AutoProfileAction a = ap_on_target_resolved(&c, 2, 0, false);
        if (a.kind != AP_ACTION_ARM_DEBOUNCE) return 236;
        a = ap_on_target_resolved(&c, 1, 200, false);
        if (a.kind != AP_ACTION_NONE) return 237;
        a = ap_on_debounce_fire(&c, 1, 800, false);
        if (a.kind != AP_ACTION_NONE || c.appliedSlot != 1) return 238;

        // Sustained B: one apply after debounce.
        a = ap_on_target_resolved(&c, 2, 1000, false);
        if (a.kind != AP_ACTION_ARM_DEBOUNCE) return 239;
        a = ap_on_debounce_fire(&c, 2, 1800, false);
        if (a.kind != AP_ACTION_APPLY_SLOT || a.slot != 2) return 240;
        ap_on_applied(&c, 2, 1800);
        if (c.appliedSlot != 2) return 241;

        // Cooldown: a switch to 3 shortly after must defer until minInterval.
        a = ap_on_target_resolved(&c, 3, 1900, false);
        if (a.kind != AP_ACTION_ARM_DEBOUNCE) return 242;
        a = ap_on_debounce_fire(&c, 3, 2700, false);   // 2700-1800=900 < 4000
        if (a.kind != AP_ACTION_ARM_DEBOUNCE) return 243;
        a = ap_on_debounce_fire(&c, 3, 5800, false);   // 5800-1800=4000 >= 4000
        if (a.kind != AP_ACTION_APPLY_SLOT || a.slot != 3) return 244;
        ap_on_applied(&c, 3, 5800);

        // Suppression (main window open) → auto does not drive.
        a = ap_on_target_resolved(&c, 1, 6000, true);
        if (a.kind != AP_ACTION_NONE) return 245;

        // Manual pin via hotkey.
        AutoProfileController m = {};
        ap_controller_init(&m, &cfg);
        m.appliedSlot = 1;
        a = ap_on_hotkey(&m, 3);
        if (a.kind != AP_ACTION_APPLY_SLOT || a.slot != 3 || m.mode != AP_MODE_MANUAL) return 246;
        ap_on_applied(&m, 3, 100);
        // While pinned, auto does not drive.
        a = ap_on_target_resolved(&m, 2, 200, false);
        if (a.kind != AP_ACTION_NONE) return 247;
        // Same slot again → back to auto.
        a = ap_on_hotkey(&m, 3);
        if (a.kind != AP_ACTION_RESUME_AUTO || m.mode != AP_MODE_AUTO) return 248;
        // Different slot pins that slot.
        a = ap_on_hotkey(&m, 2);
        if (a.kind != AP_ACTION_APPLY_SLOT || a.slot != 2 || m.mode != AP_MODE_MANUAL || m.pinnedSlot != 2) return 249;

        // enter_manual_custom suspends auto.
        AutoProfileController mc = {};
        ap_controller_init(&mc, &cfg);
        ap_enter_manual_custom(&mc);
        if (ap_controller_is_driving(&mc, false)) return 250;

        // Master toggle: disable reverts to default; enable resumes auto.
        AutoProfileController t = {};
        ap_controller_init(&t, &cfg);
        t.appliedSlot = 2;
        a = ap_set_enabled(&t, false);
        if (a.kind != AP_ACTION_APPLY_SLOT || a.slot != 1) return 251;
        a = ap_set_enabled(&t, true);
        if (a.kind != AP_ACTION_RESUME_AUTO || !t.autoEnabled || t.mode != AP_MODE_AUTO) return 252;

        // A profile pick while auto is DISABLED must not record a manual pin.
        // The pin only exists to override automatic switching, nothing is
        // switching, and "same slot again" (the documented way to release it)
        // is unreachable while disabled — so the pin would be permanent.
        AutoProfileConfig off = {};
        auto_profile_config_set_defaults(&off);
        off.enabled = false;
        off.defaultSlot = 2;
        off.ruleCount = 1;
        off.rules[0] = { AUTO_MATCH_EXE, "brave.exe", false, 1 };
        AutoProfileController d = {};
        ap_controller_init(&d, &off);
        a = ap_on_hotkey(&d, 1);
        if (a.kind != AP_ACTION_APPLY_SLOT || a.slot != 1) return 253;
        if (d.mode != AP_MODE_AUTO || d.pinnedSlot != 0) return 253;
        ap_on_applied(&d, 1, 100);

        // ...and enabling afterwards (the configuration dialog's checkbox, which
        // reaches the controller through ap_apply_config_change rather than
        // ap_set_enabled) must resume AUTO and actually drive again.  This is
        // the reported defect: enable auto, and nothing ever switches.
        AutoProfileConfig on = off;
        on.enabled = true;
        a = ap_apply_config_change(&d, &on);
        if (a.kind != AP_ACTION_RESUME_AUTO || !d.autoEnabled || d.mode != AP_MODE_AUTO) return 254;
        if (!ap_controller_is_driving(&d, false)) return 254;
        a = ap_on_target_resolved(&d, 2, 200, false);
        if (a.kind != AP_ACTION_ARM_DEBOUNCE) return 255;

        // Even a pin taken while auto was ENABLED does not survive a disable +
        // re-enable through the config path (the transition owns the reset).
        AutoProfileController p = {};
        ap_controller_init(&p, &cfg);          // cfg.enabled = true, default 1
        a = ap_on_hotkey(&p, 3);
        if (a.kind != AP_ACTION_APPLY_SLOT || p.mode != AP_MODE_MANUAL) return 268;
        ap_on_applied(&p, 3, 100);
        AutoProfileConfig pOff = cfg;
        pOff.enabled = false;
        a = ap_apply_config_change(&p, &pOff);   // dialog disable: revert to default
        if (a.kind != AP_ACTION_APPLY_SLOT || a.slot != 1 || p.autoEnabled) return 269;
        AutoProfileConfig pOn = cfg;
        pOn.enabled = true;
        a = ap_apply_config_change(&p, &pOn);
        if (a.kind != AP_ACTION_RESUME_AUTO || p.mode != AP_MODE_AUTO || p.pinnedSlot != 0) return 270;

        // No enable change → no transition, but edited values are still adopted
        // and a deliberate pin (taken while enabled) is preserved: editing a
        // rule must not silently unpin the profile the user just chose.
        AutoProfileController k = {};
        ap_controller_init(&k, &cfg);
        a = ap_on_hotkey(&k, 4);
        if (a.kind != AP_ACTION_APPLY_SLOT || k.mode != AP_MODE_MANUAL) return 271;
        AutoProfileConfig edited = cfg;
        edited.defaultSlot = 5;
        edited.switchDebounceMs = 1500;
        edited.minSwitchIntervalMs = 6000;
        a = ap_apply_config_change(&k, &edited);
        if (a.kind != AP_ACTION_NONE) return 272;
        if (k.defaultSlot != 5 || k.debounceMs != 1500 || k.minIntervalMs != 6000) return 272;
        if (k.mode != AP_MODE_MANUAL || k.pinnedSlot != 4) return 272;
        if (ap_controller_is_driving(&k, false)) return 272;
    }

#if defined(_WIN32)
    // Win32 config-INI storage again (auto_profile_config_save/load live in the
    // Windows auto-profile shard); the pure resolver above runs on both hosts.
    // F-AUTO-PROFILE: auto-profile config INI round-trips through the shared
    // get/set_config_* helpers (needs the argv[1] temp INI).
    {
        DeleteFileA(argv[1]);
        AutoProfileConfig w = {};
        auto_profile_config_set_defaults(&w);
        w.enabled = true;
        w.defaultSlot = 2;
        w.switchDebounceMs = 500;
        w.minSwitchIntervalMs = 5000;
        w.suppressWhenWindowOpen = false;
        w.ruleCount = 2;
        w.rules[0] = { AUTO_MATCH_EXE, "game.exe", true, 3 };
        w.rules[1] = { AUTO_MATCH_TITLE, "YouTube", true, 4 };
        char hotkeys[CONFIG_NUM_SLOTS + 1][64] = {};
        gc_strlcpy(hotkeys[3], ARRAY_COUNT(hotkeys[3]), "ctrl+alt+f3");
        if (!auto_profile_config_save(argv[1], &w, hotkeys)) return 256;

        AutoProfileConfig r = {};
        auto_profile_config_load(argv[1], &r);
        if (!r.enabled || r.defaultSlot != 2) return 257;
        if (r.switchDebounceMs != 500 || r.minSwitchIntervalMs != 5000 || r.suppressWhenWindowOpen) return 258;
        if (r.ruleCount != 2 ||
            r.rules[0].matchType != AUTO_MATCH_EXE || strcmp(r.rules[0].pattern, "game.exe") != 0 ||
            !r.rules[0].requireFocus || r.rules[0].slot != 3 ||
            r.rules[1].matchType != AUTO_MATCH_TITLE || strcmp(r.rules[1].pattern, "YouTube") != 0 ||
            r.rules[1].slot != 4) return 259;
        char hotkeyReadback[64] = {};
        if (!get_config_string(argv[1], "hotkeys", "slot3", "",
                hotkeyReadback, ARRAY_COUNT(hotkeyReadback)) ||
            strcmp(hotkeyReadback, "ctrl+alt+f3") != 0) return 607;
        if (config_section_has_keys(argv[1], "auto_rule3")) return 608;
        DeleteFileA(argv[1]);
    }
#endif // _WIN32

    // F-AUTO-PROFILE: per-slot hotkey string parse/format round-trip + rejection.
    {
        HotkeyBinding b = {};
        if (!hotkey_parse("ctrl+alt+f2", &b)) return 260;
        if (b.vk != VK_F2 || b.mods != (MOD_CONTROL | MOD_ALT)) return 261;
        char text[64] = {};
        if (!hotkey_format(&b, text, sizeof(text)) || strcmp(text, "ctrl+alt+f2") != 0) return 262;

        HotkeyBinding b2 = {};
        if (!hotkey_parse("CTRL+SHIFT+A", &b2)) return 263;   // case-insensitive
        if (b2.vk != 'A' || b2.mods != (MOD_CONTROL | MOD_SHIFT)) return 264;

        HotkeyBinding b3 = {};
        if (hotkey_parse("ctrl+alt", &b3)) return 265;        // no key
        if (hotkey_parse("ctrl+bogus", &b3)) return 266;      // unknown key token
        // A bare key parses (mods==0); the dialog is what rejects modifier-less binds.
        HotkeyBinding b4 = {};
        if (!hotkey_parse("f5", &b4) || b4.mods != 0 || b4.vk != VK_F5) return 267;
    }

    // Linux daemon state records reject corruption, truncation/version drift,
    // and invalid state before any startup replay can reach hardware.
    {
        GpuAdapterInfo target = {};
        target.valid = true;
        target.pciInfoValid = true;
        target.pciBus = 1;
        target.deviceId = 0x268410deu;
        DesiredSettings desired = {};
        LinuxDaemonStateRecord record = {};
        linux_daemon_record_initialize(&record, LINUX_DAEMON_RECORD_ACTIVE, &target, &desired);
        if (!linux_daemon_record_valid(&record)) return 609;
        LinuxDaemonStateRecord corrupt = record;
        corrupt.desired.gpuOffsetMHz ^= 1;
        if (linux_daemon_record_valid(&corrupt)) return 610;
        corrupt = record; corrupt.size--;
        if (linux_daemon_record_valid(&corrupt)) return 611;
        corrupt = record; corrupt.version++;
        if (linux_daemon_record_valid(&corrupt)) return 612;
        corrupt = record; corrupt.state = 99; corrupt.checksum = linux_daemon_record_checksum(&corrupt);
        if (linux_daemon_record_valid(&corrupt)) return 613;
    }

    // F-LNX-AUTORESTORE: the crash-loop guard on unattended writes.
    //
    // The loop this breaks: the daemon starts, replays a committed setting that
    // hangs the driver, crashes, systemd restarts it, and it replays the same
    // setting -- forever.  Windows never had it because an ordinary service
    // start there is non-mutating; Linux keeps `restore-last` because it has no
    // logon coordinator, so it needs the counter instead.
    {
        LinuxAutoRestoreGuard guard = {};
        // No boot id yet: adopting one is a NEW boot, not the same one.
        if (linux_auto_restore_guard_adopt_boot(&guard, "boot-a")) return 3140;
        if (strcmp(guard.bootId, "boot-a") != 0) return 3141;
        // Re-adopting the same identity must not clear the counter, or every
        // restart within one boot would look like a fresh start.
        guard.startAttempts = 2;
        if (!linux_auto_restore_guard_adopt_boot(&guard, "boot-a")) return 3142;
        if (guard.startAttempts != 2) return 3143;
        // A different boot resets the counter but KEEPS the sticky lockout:
        // "these settings kill this driver" does not stop being true because
        // the machine rebooted.
        linux_auto_restore_note_lockout(&guard,
            SERVICE_AUTO_RESTORE_LOCKOUT_UNSTABLE_APPLY);
        if (linux_auto_restore_guard_adopt_boot(&guard, "boot-b")) return 3144;
        if (guard.startAttempts != 0 || !guard.lockedOut) return 3145;
        if (strcmp(guard.bootId, "boot-b") != 0) return 3146;
        // The reason is as sticky as the flag: a new boot must not downgrade a
        // crash-loop lockout into the generic "a write failed" wording.
        if (guard.lockoutReason != SERVICE_AUTO_RESTORE_LOCKOUT_UNSTABLE_APPLY)
            return 3162;

        // Lockout dominates every trigger, including resume.
        if (linux_auto_restore_decide(&guard,
                LINUX_AUTO_RESTORE_TRIGGER_BOOT_RESTORE_LAST, false) !=
            LINUX_AUTO_RESTORE_DENY_LOCKED_OUT) return 3147;
        if (linux_auto_restore_decide(&guard,
                LINUX_AUTO_RESTORE_TRIGGER_RESUME, false) !=
            LINUX_AUTO_RESTORE_DENY_LOCKED_OUT) return 3148;
        // Automatic success never re-arms; only an explicit Apply/Reset does.
        if (!linux_auto_restore_note_explicit_success(&guard)) return 3149;
        if (guard.lockedOut || guard.startAttempts != 0) return 3150;
        // Cleared with the flag, so no snapshot can publish a reason for a
        // lockout that is no longer in force.
        if (guard.lockoutReason != SERVICE_AUTO_RESTORE_LOCKOUT_NONE)
            return 3163;
        // A guard that is already clean reports no change, so the daemon does
        // not rewrite the record on every successful Apply.
        if (linux_auto_restore_note_explicit_success(&guard)) return 3151;

        // Exactly LINUX_AUTO_RESTORE_MAX_START_ATTEMPTS start-time writes per
        // boot, then refusal -- and the refusal is the caller's cue to latch.
        for (int attempt = 0; attempt < LINUX_AUTO_RESTORE_MAX_START_ATTEMPTS;
             ++attempt) {
            if (linux_auto_restore_decide(&guard,
                    LINUX_AUTO_RESTORE_TRIGGER_BOOT_PROFILE, false) !=
                LINUX_AUTO_RESTORE_ALLOW) return 3152;
            linux_auto_restore_note_start_attempt(&guard);
        }
        if (linux_auto_restore_decide(&guard,
                LINUX_AUTO_RESTORE_TRIGGER_BOOT_RESTORE_LAST, false) !=
            LINUX_AUTO_RESTORE_DENY_ATTEMPTS_EXHAUSTED) return 3153;
        // A resume is a machine event that cannot repeat by itself, so it is
        // not rationed: a laptop must not stop restoring its curve on the
        // fourth lid open.
        if (linux_auto_restore_decide(&guard,
                LINUX_AUTO_RESTORE_TRIGGER_RESUME, false) !=
            LINUX_AUTO_RESTORE_ALLOW) return 3154;
        // A clean stop is evidence the starts were not a loop.
        if (!linux_auto_restore_note_clean_stop(&guard)) return 3155;
        if (guard.startAttempts != 0) return 3156;
        if (linux_auto_restore_note_clean_stop(&guard)) return 3157;
        if (linux_auto_restore_decide(&guard,
                LINUX_AUTO_RESTORE_TRIGGER_BOOT_RESTORE_LAST, false) !=
            LINUX_AUTO_RESTORE_ALLOW) return 3158;
        // ... but it does NOT clear a lockout: that still needs a user.
        linux_auto_restore_note_lockout(&guard,
            SERVICE_AUTO_RESTORE_LOCKOUT_UNSTABLE_APPLY);
        linux_auto_restore_note_clean_stop(&guard);
        if (!guard.lockedOut) return 3159;
        if (linux_auto_restore_decide(&guard,
                LINUX_AUTO_RESTORE_TRIGGER_NONE, false) !=
            LINUX_AUTO_RESTORE_DENY_NO_TRIGGER) return 3160;
        if (linux_auto_restore_decide(nullptr,
                LINUX_AUTO_RESTORE_TRIGGER_RESUME, false) !=
            LINUX_AUTO_RESTORE_DENY_NO_TRIGGER) return 3161;
        // The first cause wins.  A later automatic refusal is a consequence of
        // the original latch, so it must not overwrite the reason that explains
        // why automatic restoration stopped in the first place.
        linux_auto_restore_note_lockout(&guard,
            SERVICE_AUTO_RESTORE_LOCKOUT_AUTOMATIC_APPLY_FAILED);
        if (guard.lockoutReason != SERVICE_AUTO_RESTORE_LOCKOUT_UNSTABLE_APPLY)
            return 3164;
    }

    // What populate_snapshot() publishes in autoRestoreLockoutReason.
    //
    // The regression: the Linux daemon derived that field from g_stateUncertain
    // alone.  That flag is set by a FAILED WRITE, but the crash-loop arm latches
    // the guard without ever issuing one -- so a daemon that had permanently
    // stopped restoring settings at boot published LOCKOUT_NONE and looked
    // completely healthy in the TUI and in --status.  Windows has always fed the
    // same field from its real lockout state.
    {
        LinuxAutoRestoreGuard guard = {};
        // Clean guard, settled state: nothing to report.
        if (linux_auto_restore_published_lockout_reason(&guard, false) !=
            SERVICE_AUTO_RESTORE_LOCKOUT_NONE) return 3180;
        // Uncertain state with a clean guard still reports a lockout: the
        // rollback or the record did not settle, so no unattended write may run
        // until a user resolves it.
        if (linux_auto_restore_published_lockout_reason(&guard, true) !=
            SERVICE_AUTO_RESTORE_LOCKOUT_AUTOMATIC_APPLY_FAILED) return 3181;
        // THE BUG: exhausting the per-boot attempts latches the guard with no
        // write and therefore no uncertain state.  This must not publish NONE.
        for (int attempt = 0; attempt < LINUX_AUTO_RESTORE_MAX_START_ATTEMPTS;
             ++attempt)
            linux_auto_restore_note_start_attempt(&guard);
        if (linux_auto_restore_decide(&guard,
                LINUX_AUTO_RESTORE_TRIGGER_BOOT_RESTORE_LAST, false) !=
            LINUX_AUTO_RESTORE_DENY_ATTEMPTS_EXHAUSTED) return 3182;
        linux_auto_restore_note_lockout(&guard,
            SERVICE_AUTO_RESTORE_LOCKOUT_UNSTABLE_APPLY);
        if (linux_auto_restore_published_lockout_reason(&guard, false) !=
            SERVICE_AUTO_RESTORE_LOCKOUT_UNSTABLE_APPLY) return 3183;
        // A failed hardware write is a different statement and keeps its own.
        LinuxAutoRestoreGuard failed = {};
        linux_auto_restore_note_lockout(&failed,
            SERVICE_AUTO_RESTORE_LOCKOUT_AUTOMATIC_APPLY_FAILED);
        if (linux_auto_restore_published_lockout_reason(&failed, true) !=
            SERVICE_AUTO_RESTORE_LOCKOUT_AUTOMATIC_APPLY_FAILED) return 3184;
        // A locked-out guard whose reason is missing or out of range is
        // incoherent, but it must still publish a lockout: reporting NONE here
        // is exactly the failure this whole path exists to stop.
        LinuxAutoRestoreGuard incoherent = {};
        incoherent.lockedOut = 1;
        if (linux_auto_restore_published_lockout_reason(&incoherent, false) !=
            SERVICE_AUTO_RESTORE_LOCKOUT_AUTOMATIC_APPLY_FAILED) return 3185;
        incoherent.lockoutReason = 999u;
        if (linux_auto_restore_published_lockout_reason(&incoherent, false) !=
            SERVICE_AUTO_RESTORE_LOCKOUT_AUTOMATIC_APPLY_FAILED) return 3186;
        // A caller that cannot explain its latch gets the generic reason rather
        // than an incoherent guard.
        LinuxAutoRestoreGuard unexplained = {};
        linux_auto_restore_note_lockout(&unexplained,
            SERVICE_AUTO_RESTORE_LOCKOUT_NONE);
        if (!unexplained.lockedOut ||
            unexplained.lockoutReason !=
                SERVICE_AUTO_RESTORE_LOCKOUT_AUTOMATIC_APPLY_FAILED)
            return 3187;
        if (linux_auto_restore_published_lockout_reason(nullptr, false) !=
            SERVICE_AUTO_RESTORE_LOCKOUT_NONE) return 3188;
        if (linux_auto_restore_published_lockout_reason(nullptr, true) !=
            SERVICE_AUTO_RESTORE_LOCKOUT_AUTOMATIC_APPLY_FAILED) return 3189;
    }

    // The uncertain flag is a GATE, not just a published label.  The
    // regression: resume never consulted it (the TUI/JSON said "locked out"
    // while a resume write could still run), and a pre-write "GPU not
    // available" failure set it even though nothing was written -- so one
    // transient resume failure disabled the fan reassertion thread and
    // published a lockout until an explicit Apply/Reset.
    {
        LinuxAutoRestoreGuard guard = {};
        linux_auto_restore_guard_adopt_boot(&guard, "boot-uncertain");
        // A clean guard with unsettled daemon state denies every automatic
        // trigger, including the otherwise unrationed resume.
        if (linux_auto_restore_decide(&guard,
                LINUX_AUTO_RESTORE_TRIGGER_BOOT_RESTORE_LAST, true) !=
            LINUX_AUTO_RESTORE_DENY_STATE_UNCERTAIN) return 3228;
        if (linux_auto_restore_decide(&guard,
                LINUX_AUTO_RESTORE_TRIGGER_BOOT_PROFILE, true) !=
            LINUX_AUTO_RESTORE_DENY_STATE_UNCERTAIN) return 3229;
        if (linux_auto_restore_decide(&guard,
                LINUX_AUTO_RESTORE_TRIGGER_RESUME, true) !=
            LINUX_AUTO_RESTORE_DENY_STATE_UNCERTAIN) return 3230;
        // Settled state falls back to the ordinary rules.
        if (linux_auto_restore_decide(&guard,
                LINUX_AUTO_RESTORE_TRIGGER_RESUME, false) !=
            LINUX_AUTO_RESTORE_ALLOW) return 3231;
        // A latch outranks the uncertain flag, matching the published reason.
        linux_auto_restore_note_lockout(&guard,
            SERVICE_AUTO_RESTORE_LOCKOUT_UNSTABLE_APPLY);
        if (linux_auto_restore_decide(&guard,
                LINUX_AUTO_RESTORE_TRIGGER_RESUME, true) !=
            LINUX_AUTO_RESTORE_DENY_LOCKED_OUT) return 3232;
        if (linux_auto_restore_decide(&guard,
                LINUX_AUTO_RESTORE_TRIGGER_NONE, true) !=
            LINUX_AUTO_RESTORE_DENY_NO_TRIGGER) return 3233;
        // An exhausted boot and an uncertain daemon: the unsettled state is
        // the earlier, more specific explanation.
        LinuxAutoRestoreGuard exhausted = {};
        linux_auto_restore_guard_adopt_boot(&exhausted, "boot-uncertain-2");
        exhausted.startAttempts = LINUX_AUTO_RESTORE_MAX_START_ATTEMPTS;
        if (linux_auto_restore_decide(&exhausted,
                LINUX_AUTO_RESTORE_TRIGGER_BOOT_PROFILE, true) !=
            LINUX_AUTO_RESTORE_DENY_STATE_UNCERTAIN) return 3234;
        if (linux_auto_restore_decide(&exhausted,
                LINUX_AUTO_RESTORE_TRIGGER_BOOT_PROFILE, false) !=
            LINUX_AUTO_RESTORE_DENY_ATTEMPTS_EXHAUSTED) return 3235;
    }

    // The persisted guard round-trips and rejects tampering.  It authorizes an
    // unattended hardware write, so a record an unprivileged user could forge
    // -- or a half-written one -- must not read back as "three fresh attempts".
    {
        LinuxAutoRestoreGuard guard = {};
        // Adopt first: adopting an identity the guard does not already carry is
        // a NEW boot, which zeroes the attempt count by design.
        linux_auto_restore_guard_adopt_boot(&guard, "0f9c1d3e-boot");
        linux_auto_restore_note_lockout(&guard,
            SERVICE_AUTO_RESTORE_LOCKOUT_UNSTABLE_APPLY);
        guard.startAttempts = 2;
        LinuxDaemonRestoreGuardRecord record = {};
        linux_daemon_guard_initialize(&record, &guard);
        if (!linux_daemon_guard_valid(&record)) return 3170;
        LinuxAutoRestoreGuard restored = {};
        linux_daemon_guard_to_policy(&record, &restored);
        if (restored.lockedOut != 1 || restored.startAttempts != 2 ||
            strcmp(restored.bootId, "0f9c1d3e-boot") != 0) return 3171;
        // The reason survives the restart.  Without this the next boot's
        // snapshot would report the generic failed-write reason for a lockout
        // that no write ever caused.
        if (restored.lockoutReason !=
            SERVICE_AUTO_RESTORE_LOCKOUT_UNSTABLE_APPLY) return 3177;
        LinuxDaemonRestoreGuardRecord corrupt = record;
        corrupt.startAttempts = 0;
        if (linux_daemon_guard_valid(&corrupt)) return 3172;
        corrupt = record; corrupt.version++;
        if (linux_daemon_guard_valid(&corrupt)) return 3173;
        corrupt = record; corrupt.size--;
        if (linux_daemon_guard_valid(&corrupt)) return 3174;
        // A non-boolean lockedOut means something other than this code wrote
        // the file, so it is not trusted as permission to write the GPU.
        corrupt = record; corrupt.lockedOut = 2;
        corrupt.checksum = linux_daemon_guard_checksum(&corrupt);
        if (linux_daemon_guard_valid(&corrupt)) return 3175;
        // An unterminated boot id would make the identity comparison read past
        // the field.
        corrupt = record;
        for (unsigned int i = 0; i < sizeof(corrupt.bootId); ++i)
            corrupt.bootId[i] = 'x';
        corrupt.checksum = linux_daemon_guard_checksum(&corrupt);
        if (linux_daemon_guard_valid(&corrupt)) return 3176;
        // Coherent or rejected.  A latched lockout with no reason, and a clear
        // guard carrying one, are each half a fact -- and the snapshot would
        // publish that half as the truth.
        corrupt = record;
        corrupt.lockoutReason = SERVICE_AUTO_RESTORE_LOCKOUT_NONE;
        corrupt.checksum = linux_daemon_guard_checksum(&corrupt);
        if (linux_daemon_guard_valid(&corrupt)) return 3178;
        corrupt = record;
        corrupt.lockedOut = 0;
        corrupt.checksum = linux_daemon_guard_checksum(&corrupt);
        if (linux_daemon_guard_valid(&corrupt)) return 3179;
        corrupt = record;
        corrupt.lockoutReason =
            (gc_u32)SERVICE_AUTO_RESTORE_LOCKOUT_AUTOMATIC_APPLY_FAILED + 1u;
        corrupt.checksum = linux_daemon_guard_checksum(&corrupt);
        if (linux_daemon_guard_valid(&corrupt)) return 3190;
        // A clean guard round-trips with no reason attached.
        LinuxAutoRestoreGuard clean = {};
        linux_auto_restore_guard_adopt_boot(&clean, "a1b2c3d4-boot");
        clean.startAttempts = 1;
        LinuxDaemonRestoreGuardRecord cleanRecord = {};
        linux_daemon_guard_initialize(&cleanRecord, &clean);
        if (!linux_daemon_guard_valid(&cleanRecord)) return 3191;
        if (cleanRecord.lockoutReason != SERVICE_AUTO_RESTORE_LOCKOUT_NONE)
            return 3192;
        // A guard latched by assigning the flag alone still serializes into a
        // coherent record, so a field-by-field mutation cannot produce a file
        // this code would refuse to read back.
        LinuxAutoRestoreGuard raw = {};
        linux_auto_restore_guard_adopt_boot(&raw, "e5f6a7b8-boot");
        raw.lockedOut = 1;
        LinuxDaemonRestoreGuardRecord rawRecord = {};
        linux_daemon_guard_initialize(&rawRecord, &raw);
        if (!linux_daemon_guard_valid(&rawRecord)) return 3193;
        if (rawRecord.lockoutReason !=
            SERVICE_AUTO_RESTORE_LOCKOUT_AUTOMATIC_APPLY_FAILED) return 3194;
    }

    // The resume edge carries nothing: no settings, no target, no operation id.
    // It is a machine-generated notification on a group-reachable socket, and
    // accepting a payload on it would turn that into an apply nobody typed.
    {
        ServiceRequest resume = {};
        resume.magic = SERVICE_PROTOCOL_MAGIC;
        resume.version = SERVICE_PROTOCOL_VERSION;
        resume.command = SERVICE_CMD_RESUME_RESTORE;
        resume.callerPid = 4242;
        if (service_request_flags_for_command(SERVICE_CMD_RESUME_RESTORE, true)
            != 0u) return 3180;
        if (!validate_service_request_for_ipc(&resume)) return 3181;
        ServiceRequest tampered = resume;
        tampered.desired.hasGpuOffset = 1;
        tampered.desired.gpuOffsetMHz = 300;
        if (validate_service_request_for_ipc(&tampered)) return 3189;
        tampered = resume; tampered.targetGpu.valid = 1;
        if (validate_service_request_for_ipc(&tampered)) return 3182;
        tampered = resume; tampered.operationId = 7;
        if (validate_service_request_for_ipc(&tampered)) return 3183;
        tampered = resume; tampered.flags = SERVICE_REQUEST_FLAG_INTERACTIVE;
        if (validate_service_request_for_ipc(&tampered)) return 3184;
        tampered = resume; tampered.applyOrigin = SERVICE_APPLY_ORIGIN_STANDBY;
        if (validate_service_request_for_ipc(&tampered)) return 3185;
        tampered = resume; tampered.resetOcBeforeApply = 1;
        if (validate_service_request_for_ipc(&tampered)) return 3186;
        tampered = resume; tampered.profileSlot = 3;
        if (validate_service_request_for_ipc(&tampered)) return 3187;
        // One past the command set is still unknown, so a future command
        // cannot fall through this daemon's switch.  Anchored to the HIGHEST
        // command rather than to RESUME_RESTORE: the v19 updater commands were
        // added after it, and an anchor that does not move turns this from a
        // boundary check into an assertion that a real command is rejected.
        tampered = resume; tampered.command = SERVICE_CMD_SET_UPDATE_POLICY + 1;
        if (validate_service_request_for_ipc(&tampered)) return 3188;
    }

    // The production Linux mutation engine stops at every possible phase
    // failure, rolls back all attempted (including possibly partial) phases,
    // and exposes rollback uncertainty without publishing success.
    {
        const unsigned int phases[] = {
            LINUX_MUTATION_RESET_BASELINE, LINUX_MUTATION_GPU_OFFSET,
            LINUX_MUTATION_MEM_OFFSET, LINUX_MUTATION_POWER,
            LINUX_MUTATION_CURVE, LINUX_MUTATION_LOCK, LINUX_MUTATION_FAN,
        };
        unsigned int requested = 0;
        for (unsigned int phase : phases) requested |= phase;
        for (unsigned int failIndex = 0; failIndex < 7; ++failIndex) {
            FakeLinuxTransaction fake = {};
            fake.failPhase = phases[failIndex];
            fake.rollbackOk = true;
            LinuxMutationResult result = linux_execute_transaction(
                requested, fake_linux_transaction_step,
                fake_linux_transaction_rollback, &fake);
            if (result.success || !result.rollbackAttempted || !result.rollbackSucceeded) return 619;
            if (result.failedPhases != phases[failIndex] || fake.callCount != failIndex + 1) return 620;
            if (fake.rollbackMask != result.attemptedPhases ||
                (result.completedPhases & phases[failIndex])) return 621;
        }
        FakeLinuxTransaction rollbackFailure = {};
        rollbackFailure.failPhase = LINUX_MUTATION_POWER;
        LinuxMutationResult failed = linux_execute_transaction(
            requested, fake_linux_transaction_step,
            fake_linux_transaction_rollback, &rollbackFailure);
        if (failed.success || failed.rollbackSucceeded || !failed.rollbackAttempted) return 622;
        FakeLinuxTransaction success = {};
        success.rollbackOk = true;
        LinuxMutationResult complete = linux_execute_transaction(
            requested, fake_linux_transaction_step,
            fake_linux_transaction_rollback, &success);
        if (!complete.success || complete.attemptedPhases != requested ||
            complete.completedPhases != requested || complete.failedPhases) return 623;
    }

    // Linux PCI identity remains stable across API enumeration reordering and
    // fails closed for missing, duplicate, or cross-API-mismatched devices.
    {
        GpuAdapterInfo requested = {};
        requested.valid = requested.pciInfoValid = true;
        requested.pciBus = 2; requested.pciDevice = 3;
        requested.deviceId = 0x268410deu; requested.subSystemId = 0x1234u;
        GpuAdapterInfo adapters[2] = {};
        adapters[0] = requested;
        adapters[0].pciBus = 1;
        adapters[1] = requested;
        if (linux_resolve_gpu_identity(&requested, adapters, 2) != 1) return 614;
        GpuAdapterInfo reordered[2] = { adapters[1], adapters[0] };
        if (linux_resolve_gpu_identity(&requested, reordered, 2) != 0) return 615;
        if (linux_resolve_gpu_identity(&requested, adapters, 1) != -1) return 616;
        GpuAdapterInfo duplicate[2] = { requested, requested };
        if (linux_resolve_gpu_identity(&requested, duplicate, 2) != -2) return 617;
        GpuAdapterInfo mismatch = requested;
        mismatch.deviceId = 0x999910deu;
        if (linux_resolve_gpu_identity(&requested, &mismatch, 1) != -1) return 618;
        if (!linux_gpu_switch_preserves_active_intent(
                false, nullptr, &requested)) return 904;
        if (!linux_gpu_switch_preserves_active_intent(
                true, &requested, &adapters[1])) return 905;
        GpuAdapterInfo otherGpu = requested;
        otherGpu.pciBus = 4;
        if (linux_gpu_switch_preserves_active_intent(
                true, &requested, &otherGpu)) return 906;
        if (linux_next_gpu_selection_index(false, 0, 2, 1) != 0 ||
            linux_next_gpu_selection_index(false, 0, 2, -1) != 1 ||
            linux_next_gpu_selection_index(true, 0, 2, -1) != 1 ||
            linux_next_gpu_selection_index(true, 1, 2, 1) != 0 ||
            linux_next_gpu_selection_index(false, 0, 0, 1) != -1) return 908;
    }

    // F-01-001: lifecycle identity — session_and_user matches on sessionId+SID
    // alone, even when authenticationId (LUID) differs. This guards against the
    // auth LUID race between OpenProcessToken and WTSQueryUserToken on boot.
    {
        ServiceLifecycleIdentity a = {};
        a.valid = true; a.sessionId = 1;
        a.authenticationId = 100;
        gc_strlcpy(a.sid, sizeof(a.sid), "S-1-5-21-123");
        ServiceLifecycleIdentity b = {};
        b.valid = true; b.sessionId = 1;
        b.authenticationId = 200; // Different LUID!
        gc_strlcpy(b.sid, sizeof(b.sid), "S-1-5-21-123");
        if (!service_lifecycle_identity_equal_session_and_user(&a, &b)) return 738;
        if (service_lifecycle_identity_equal(&a, &b)) return 739;
        if (service_lifecycle_identity_equal_session_and_user(nullptr, &b)) return 740;
        if (service_lifecycle_identity_equal_session_and_user(&a, nullptr)) return 741;
        ServiceLifecycleIdentity invalid = {};
        if (service_lifecycle_identity_equal_session_and_user(&invalid, &b)) return 742;
        if (service_lifecycle_identity_equal_session_and_user(&a, &invalid)) return 743;
        ServiceLifecycleIdentity noSid = {};
        noSid.valid = true; noSid.sessionId = 1;
        noSid.authenticationId = 100;
        if (service_lifecycle_identity_equal_session_and_user(&noSid, &b)) return 744;
        ServiceLifecycleIdentity diffSession = {};
        diffSession.valid = true; diffSession.sessionId = 2;
        diffSession.authenticationId = 100;
        gc_strlcpy(diffSession.sid, sizeof(diffSession.sid), "S-1-5-21-123");
        if (service_lifecycle_identity_equal_session_and_user(&diffSession, &a)) return 745;
        ServiceLifecycleIdentity diffSid = {};
        diffSid.valid = true; diffSid.sessionId = 1;
        diffSid.authenticationId = 100;
        gc_strlcpy(diffSid.sid, sizeof(diffSid.sid), "S-1-5-21-999");
        if (service_lifecycle_identity_equal_session_and_user(&diffSid, &a)) return 746;
    }

#if defined(_WIN32)
    // Win32 config-INI storage (see the note above the logon-transaction suite).
    // F-01-002: profile point visibility round-trip — curve point visibility
    // flags must survive save/load through the INI config. Regression for the
    // "all points written as hidden" bug in machine-config sharing.
    {
        if (!set_config_int(argv[1], "vis_test", "point0_visible", 0)) return 747;
        if (get_config_int(argv[1], "vis_test", "point0_visible", 1) != 0) return 748;
        if (!set_config_int(argv[1], "vis_test", "point0_visible", 1)) return 749;
        if (get_config_int(argv[1], "vis_test", "point0_visible", 0) != 1) return 750;
        if (!set_config_int(argv[1], "vis_test", "point127_visible", 0)) return 751;
        if (get_config_int(argv[1], "vis_test", "point127_visible", 1) != 0) return 752;
        if (!set_config_int(argv[1], "vis_test", "point127_visible", 1)) return 753;
        if (get_config_int(argv[1], "vis_test", "point127_visible", 0) != 1) return 754;

        // GPU section auto-save round-trip: [gpu] section with PCI identity
        // must survive save/load to enable profile sharing on single-GPU.
        if (!set_config_int(argv[1], "gpu", "slot", 1)) return 755;
        if (!set_config_int(argv[1], "gpu", "device_id", 0x268410de)) return 756;
        if (get_config_int(argv[1], "gpu", "slot", 0) != 1) return 757;
        if (get_config_int(argv[1], "gpu", "device_id", 0) != 0x268410de) return 758;
        if (!config_section_has_keys(argv[1], "gpu")) return 759;
    }
#endif // _WIN32

    // F-02-002: controlled recovery SCM stop disposition — STOP_PENDING with
    // stale/zero PID must be classified as WAIT_FOR_STOPPED (not REJECT), since
    // QueryServiceStatusEx does not guarantee a valid process ID in that state.
    {
        if (service_classify_controlled_recovery_scm_stop_state(
            true, false, 0, 12345) != SERVICE_CONTROLLED_RECOVERY_SCM_STOPPED) return 760;
        if (service_classify_controlled_recovery_scm_stop_state(
            false, true, 0, 12345) != SERVICE_CONTROLLED_RECOVERY_SCM_WAIT_FOR_STOPPED) return 761;
        if (service_classify_controlled_recovery_scm_stop_state(
            false, false, 12345, 12345) != SERVICE_CONTROLLED_RECOVERY_SCM_WAIT_FOR_STOPPED) return 762;
        if (service_classify_controlled_recovery_scm_stop_state(
            false, false, 99999, 12345) != SERVICE_CONTROLLED_RECOVERY_SCM_REJECT) return 763;
        if (service_classify_controlled_recovery_scm_stop_state(
            false, true, 0, 0) != SERVICE_CONTROLLED_RECOVERY_SCM_REJECT) return 764;
        if (service_classify_controlled_recovery_scm_stop_state(
            false, false, 0, 0) != SERVICE_CONTROLLED_RECOVERY_SCM_REJECT) return 765;
        if (service_classify_controlled_recovery_scm_stop_state(
            false, false, 0, 12345) != SERVICE_CONTROLLED_RECOVERY_SCM_REJECT) return 766;
    }

    // F-02-002: controlled recovery authorization gate — all 7 fields must be
    // true for the gate to authorize a controlled recovery start.
    {
        ServiceControlledRecoveryStartGate gate = {};
        if (service_controlled_recovery_start_is_authorized(gate)) return 770;
        gate.argumentsValid = true;
        if (service_controlled_recovery_start_is_authorized(gate)) return 771;
        gate.explicitlyRequested = true;
        if (service_controlled_recovery_start_is_authorized(gate)) return 772;
        gate.scmStartReasonKnown = true;
        if (service_controlled_recovery_start_is_authorized(gate)) return 773;
        gate.scmDemandStart = true;
        if (service_controlled_recovery_start_is_authorized(gate)) return 774;
        gate.authorizationValid = true;
        if (service_controlled_recovery_start_is_authorized(gate)) return 775;
        gate.helperValidated = true;
        if (service_controlled_recovery_start_is_authorized(gate)) return 776;
        gate.snapshotValid = true;
        if (!service_controlled_recovery_start_is_authorized(gate)) return 777;
    }

    // F-02-006: lifecycle event sequencing — different notification orderings
    // produce expected recovery decisions without races or lockup.
    {
        ServiceLifecycleIdentity identity = {};
        identity.valid = true; identity.sessionId = 1;
        identity.authenticationId = 100;
        gc_strlcpy(identity.sid, sizeof(identity.sid), "S-1-5-21-123");

        // Standby resume followed by driver recovery: driver dominates
        ServiceLifecycleState state = {};
        ServiceLifecycleEvent suspend = {};
        suspend.type = SERVICE_LIFECYCLE_EVENT_SUSPEND;
        service_lifecycle_reduce(&state, &suspend);
        ServiceLifecycleEvent standbyResume = {};
        standbyResume.type = SERVICE_LIFECYCLE_EVENT_RESUME;
        ServiceLifecycleDecision d1 = service_lifecycle_reduce(&state, &standbyResume);
        if (d1.trigger != SERVICE_LIFECYCLE_TRIGGER_STANDBY_RESUME) return 780;
        if (!state.standbyPending) return 781;

        ServiceLifecycleEvent driverRecovery = {};
        driverRecovery.type = SERVICE_LIFECYCLE_EVENT_DRIVER_RECOVERY;
        driverRecovery.driverProofReady = true;
        ServiceLifecycleDecision d2 = service_lifecycle_reduce(&state, &driverRecovery);
        if (!d2.wakeWorker) return 782;
        if (d2.trigger != SERVICE_LIFECYCLE_TRIGGER_DRIVER_RECOVERY) return 783;
        if (state.standbyPending) return 784; // must cancel standby

        // Lockout cancels all pending operations
        ServiceLifecycleState state2 = {};
        ServiceLifecycleEvent logon = {};
        logon.type = SERVICE_LIFECYCLE_EVENT_WTS_LOGON;
        logon.identity = identity;
        ServiceLifecycleDecision d3 = service_lifecycle_reduce(&state2, &logon);
        if (d3.result != SERVICE_LIFECYCLE_RESULT_PENDING) return 785;
        ServiceLifecycleEvent lockout = {};
        lockout.type = SERVICE_LIFECYCLE_EVENT_LOCKOUT;
        ServiceLifecycleDecision d4 = service_lifecycle_reduce(&state2, &lockout);
        if (!d4.cancelled) return 786;
        if (d4.result != SERVICE_LIFECYCLE_RESULT_LOCKED_OUT) return 787;
        if (!state2.lockedOut) return 788;

        // Explicit supersede clears pending logon without applying
        ServiceLifecycleState state3 = {};
        ServiceLifecycleEvent taskHandoff = {};
        taskHandoff.type = SERVICE_LIFECYCLE_EVENT_TASK_HANDOFF;
        taskHandoff.identity = identity;
        (void)service_lifecycle_reduce(&state3, &taskHandoff);
        if (!state3.logonPending) return 789;
        ServiceLifecycleEvent supersede = {};
        supersede.type = SERVICE_LIFECYCLE_EVENT_EXPLICIT_SUPERSEDE;
        ServiceLifecycleDecision d6 = service_lifecycle_reduce(&state3, &supersede);
        if (!d6.cancelled) return 790;
        if (d6.result != SERVICE_LIFECYCLE_RESULT_SUPERSEDED) return 791;
        if (state3.logonPending) return 792;

        // DEVNODES_CHANGED is diagnostic-only, never sets pending
        ServiceLifecycleState state4 = {};
        ServiceLifecycleEvent devnodes = {};
        devnodes.type = SERVICE_LIFECYCLE_EVENT_DEVNODES_CHANGED;
        ServiceLifecycleDecision d7 = service_lifecycle_reduce(&state4, &devnodes);
        if (!d7.readOnlyReenumerate) return 793;
        if (d7.wakeWorker) return 794;
    }

    // F-02-002: proof age computation — boundaries and validation
    {
        ServiceBootIdentity boot = { 0xABCD, 0x1234 };
        ServiceOcApplyProofStamp validStamp = {};
        validStamp.magic = SERVICE_OC_APPLY_STAMP_MAGIC;
        validStamp.version = SERVICE_OC_APPLY_STAMP_VERSION;
        validStamp.size = sizeof(ServiceOcApplyProofStamp);
        validStamp.bootIdentity = boot;
        validStamp.awakeTime100ns = 10000000; // 1 second

        uint64_t ageMs = 0;
        bool valid = service_compute_proof_age_ms(
            &validStamp, boot, 20000000, &ageMs);
        if (!valid) return 800;
        if (ageMs != 1000) return 801; // 1 sec awake = 1000ms

        // Wrong boot identity: reject
        ServiceBootIdentity otherBoot = { 0xFFFF, 0 };
        if (service_compute_proof_age_ms(
            &validStamp, otherBoot, 20000000, &ageMs)) return 802;

        // Time underflow: reject
        if (service_compute_proof_age_ms(
            &validStamp, boot, 5000000, &ageMs)) return 803;

        // Bad magic: reject
        ServiceOcApplyProofStamp badMagic = validStamp;
        badMagic.magic = 0xDEAD;
        if (service_compute_proof_age_ms(
            &badMagic, boot, 20000000, &ageMs)) return 804;

        // Zero stamp: reject
        ServiceOcApplyProofStamp zero = {};
        if (service_compute_proof_age_ms(
            &zero, boot, 20000000, &ageMs)) return 805;

        // Null stamp: reject
        if (service_compute_proof_age_ms(
            nullptr, boot, 20000000, &ageMs)) return 806;
    }

    // F-02-002: recovery clock window counting — bounds and boot identity
    {
        ServiceBootIdentity boot = { 0xA, 0xB };
        ServiceBootIdentity otherBoot = { 0xC, 0xD };
        ServiceRecoveryClockEntry entries[4] = {};
        entries[0].bootIdentity = boot;
        entries[0].awakeTime100ns = 10000000;
        entries[1].bootIdentity = boot;
        entries[1].awakeTime100ns = 20000000;
        entries[2].bootIdentity = otherBoot;
        entries[2].awakeTime100ns = 15000000;
        entries[3].bootIdentity = boot;
        entries[3].awakeTime100ns = 0; // zero is skipped

        unsigned int recent = service_count_recent_recovery_clock_entries(
            entries, 4, boot, 50000000, 60000);
        if (recent != 2) return 810; // entries 0 and 1 within 60s window

        // Zero current awake time: returns 0
        if (service_count_recent_recovery_clock_entries(
            entries, 4, boot, 0, 60000) != 0) return 811;

        // Null entries: returns 0
        if (service_count_recent_recovery_clock_entries(
            nullptr, 4, boot, 50000000, 60000) != 0) return 812;

        // Zero entries count: returns 0
        if (service_count_recent_recovery_clock_entries(
            entries, 0, boot, 50000000, 60000) != 0) return 813;
    }

    // F-02-002: recovery evidence dedup
    {
        ServiceRecoveryEvidenceKey keys[3] = {};
        keys[0] = { 1, 2 };
        keys[1] = { 3, 4 };
        keys[2] = { 5, 6 };
        if (!service_recovery_evidence_already_recorded(keys, 3, { 1, 2 })) return 820;
        if (!service_recovery_evidence_already_recorded(keys, 3, { 3, 4 })) return 821;
        if (!service_recovery_evidence_already_recorded(keys, 3, { 5, 6 })) return 822;
        if (service_recovery_evidence_already_recorded(keys, 3, { 7, 8 })) return 823;
        // Invalid key must not be dedup-checked
        if (service_recovery_evidence_already_recorded(keys, 3, { 0, 0 })) return 824;
        // Null keys: returns false
        if (service_recovery_evidence_already_recorded(nullptr, 3, { 1, 2 })) return 825;
    }

    // F-02-002: standby proof preservation logic
    {
        if (!service_should_preserve_proof_after_standby(true, 600000, 600000)) return 830;
        // Under required age
        if (service_should_preserve_proof_after_standby(true, 599999, 600000)) return 831;
        // Invalid proof
        if (service_should_preserve_proof_after_standby(false, 600000, 600000)) return 832;
        // Zero required age (no preservation needed)
        if (service_should_preserve_proof_after_standby(true, 600000, 0)) return 833;
    }

    // Protocol-v10 mutations are idempotent and retain a bounded query cache.
    {
        ServiceOperationTracker tracker = {};
        const ServiceOperationRecord* existing = nullptr;
        if (service_operation_begin(&tracker, 42, &existing) !=
            SERVICE_OPERATION_BEGIN_STARTED) return 1000;
        const ServiceOperationRecord* record = service_operation_find(&tracker, 42);
        if (!record || record->state != SERVICE_OPERATION_IN_PROGRESS) return 1001;
        if (service_operation_begin(&tracker, 42, &existing) !=
            SERVICE_OPERATION_BEGIN_DUPLICATE || existing != record) return 1002;
        if (!service_operation_complete(&tracker, 42, SERVICE_STATUS_OK,
                SERVICE_OUTCOME_SEVERITY_WARNING, "done")) return 1003;
        record = service_operation_find(&tracker, 42);
        if (!record || record->state != SERVICE_OPERATION_SUCCEEDED ||
            strcmp(record->message, "done") != 0) return 1004;
        // A retry of this id is answered from the record, so the severity has
        // to be part of what was recorded -- replaying a warning as a clean
        // success is exactly the bug the field exists to prevent.
        if (record->outcomeSeverity != SERVICE_OUTCOME_SEVERITY_WARNING)
            return 1010;
        if (service_operation_complete(&tracker, 42, SERVICE_STATUS_ERROR,
                SERVICE_OUTCOME_SEVERITY_ERROR, "twice")) return 1005;
        if (!service_operation_restore(&tracker, 77,
                SERVICE_OPERATION_OUTCOME_UNKNOWN, SERVICE_STATUS_ERROR,
                SERVICE_OUTCOME_SEVERITY_ERROR, "restart uncertainty"))
            return 1006;
        record = service_operation_find(&tracker, 77);
        if (!record || record->state != SERVICE_OPERATION_OUTCOME_UNKNOWN)
            return 1007;
        // A caller that hands in a severity the status cannot carry gets the
        // resolved one, never the one it asked for.
        if (!service_operation_restore(&tracker, 78, SERVICE_OPERATION_FAILED,
                SERVICE_STATUS_ERROR, SERVICE_OUTCOME_SEVERITY_SUCCESS,
                "failed but claimed clean")) return 1011;
        record = service_operation_find(&tracker, 78);
        if (!record || record->outcomeSeverity != SERVICE_OUTCOME_SEVERITY_ERROR)
            return 1012;
        for (gc_u64 id = 100; id < 116; ++id) {
            if (service_operation_begin(&tracker, id, nullptr) !=
                SERVICE_OPERATION_BEGIN_STARTED) return 1008;
        }
        if (service_operation_find(&tracker, 42) != nullptr) return 1009;
    }

    // Linux curve composition matches Windows precedence and never turns a
    // selective/lock-composed request into an NVML-global offset.
    {
        VFCurvePoint curve[VF_NUM_POINTS] = {};
        int current[VF_NUM_POINTS] = {};
        for (int i = 0; i < 8; ++i) {
            curve[i].freq_kHz = 1000000u + (unsigned int)i * 100000u;
            curve[i].volt_uV = 700000u + (unsigned int)i * 10000u;
            current[i] = 10000;
        }
        int targets[VF_NUM_POINTS] = {};
        bool mask[VF_NUM_POINTS] = {};
        DesiredSettings desired = {};
        desired.hasGpuOffset = true;
        desired.gpuOffsetMHz = 50;
        desired.gpuOffsetExcludeLowCount = 2;
        desired.hasCurvePoint[3] = true;
        desired.curvePointMHz[3] = 1500;
        desired.hasLock = true;
        desired.lockMode = LOCK_MODE_FLATTEN;
        desired.lockCi = 5;
        desired.lockMHz = 1600;
        LinuxCurveTargetBuildResult built = linux_build_curve_targets(
            curve, current, &desired, -900000, targets, mask);
        if (!built.composedGpuOffset || !built.lockTail || built.pointCount != 6)
            return 1010;
        if (mask[0] || mask[1] || !mask[2] || targets[2] != 50000)
            return 1011;
        if (!mask[3] || targets[3] != 210000 || targets[4] != 50000)
            return 1012;
        if (targets[5] != 110000 || targets[6] != -900000 ||
            targets[7] != -900000) return 1013;

        DesiredSettings globalOnly = {};
        globalOnly.hasGpuOffset = true;
        globalOnly.gpuOffsetMHz = 75;
        built = linux_build_curve_targets(curve, current, &globalOnly,
            -900000, targets, mask);
        if (built.composedGpuOffset || built.pointCount != 0) return 1014;

        DesiredSettings hard = desired;
        hard.lockMode = LOCK_MODE_HARD;
        hard.hasCurvePoint[6] = true;
        hard.curvePointMHz[6] = 1900;
        built = linux_build_curve_targets(curve, current, &hard,
            -900000, targets, mask);
        if (!built.lockTail || targets[6] != 10000) return 1015;

        VFCurvePoint sparse[VF_NUM_POINTS] = {};
        int sparseCurrent[VF_NUM_POINTS] = {};
        sparse[0].freq_kHz = 1000000;
        sparse[3].freq_kHz = 1300000;
        sparse[5].freq_kHz = 1500000;
        DesiredSettings sparseOffset = {};
        sparseOffset.hasGpuOffset = true;
        sparseOffset.gpuOffsetMHz = 25;
        sparseOffset.gpuOffsetExcludeLowCount = 2;
        built = linux_build_curve_targets(sparse, sparseCurrent,
            &sparseOffset, -900000, targets, mask);
        if (mask[0] || mask[3] || !mask[5] || targets[5] != 25000)
            return 1034;

        DesiredSettings previous = {};
        previous.hasCurvePoint[3] = true;
        previous.curvePointMHz[3] = 1500;
        DesiredSettings replacement = {};
        replacement.hasCurvePoint[5] = true;
        replacement.curvePointMHz[5] = 1600;
        int cleanupPointCount = 0;
        built = linux_build_curve_transition_targets(
            curve, current, &previous, &replacement, -900000,
            targets, mask, &cleanupPointCount);
        if (cleanupPointCount != 1 || built.pointCount != 2)
            return 2051;
        if (!mask[3] || targets[3] != 0 ||
            !mask[5] || targets[5] != 110000)
            return 2052;

        DesiredSettings previousComposed = desired;
        DesiredSettings committedComposed = desired;
        committedComposed.hasCurvePoint[3] = false;
        committedComposed.curvePointMHz[3] = 0;
        committedComposed.hasCurvePoint[4] = true;
        committedComposed.curvePointMHz[4] = 1550;
        built = linux_build_curve_transition_targets(
            curve, current, &previousComposed, &committedComposed,
            -900000, targets, mask, &cleanupPointCount);
        if (cleanupPointCount != 0 || built.pointCount != 6 ||
            !mask[3] || targets[3] != 50000 ||
            !mask[4] || targets[4] != 160000)
            return 2053;
    }

    // The applied-profile indicator (the tray menu's personal-slot tick).
    //
    // Regression: a profile Load with nothing applied used to clear the tick,
    // because the confirming read of the stored profile was projected onto the
    // LIVE VF curve and so flipped with boost drift.  The decision itself is
    // now pure; the drift-free read that feeds it is pinned by a source gate
    // (tools/ui_gates.py::check_applied_profile_indicator_is_drift_free).
    {
        AppliedProfileIndicatorInputs in = {};
        in.maxSlots = 5;
        AppliedProfileIndicatorReason reason = APPLIED_PROFILE_REASON_APPLIED;

        // No accepted snapshot: never guess an owner from stale state.
        if (applied_profile_indicator_slot(in, &reason) != 0 ||
            reason != APPLIED_PROFILE_REASON_NO_AUTHORITY) return 2054;

        // The healthy case, and the one the bug destroyed.
        in.serviceAuthoritative = true;
        in.activeDesiredValid = true;
        in.profileSource = SERVICE_PROFILE_SOURCE_USER_SLOT;
        in.profileSlot = 3;
        in.profileReadable = true;
        in.intentMatchesProfile = true;
        if (applied_profile_indicator_slot(in, &reason) != 3 ||
            reason != APPLIED_PROFILE_REASON_APPLIED) return 2055;

        // A genuinely edited slot still stops claiming to be applied: this is
        // the behaviour the fix must preserve, not remove.
        in.intentMatchesProfile = false;
        if (applied_profile_indicator_slot(in, &reason) != 0 ||
            reason != APPLIED_PROFILE_REASON_PROFILE_EDITED) return 2056;

        // A slot that has been deleted or gone malformed reports its own
        // reason rather than being folded into "edited".
        in.profileReadable = false;
        if (applied_profile_indicator_slot(in, &reason) != 0 ||
            reason != APPLIED_PROFILE_REASON_PROFILE_UNREADABLE) return 2057;

        // Shared and machine banks are separate banks with separate contents;
        // they must never tick the same-numbered personal slot.
        in.profileReadable = true;
        in.intentMatchesProfile = true;
        in.profileSource = SERVICE_PROFILE_SOURCE_SHARED_SLOT;
        if (applied_profile_indicator_slot(in, &reason) != 0 ||
            reason != APPLIED_PROFILE_REASON_NOT_A_USER_SLOT) return 2058;
        in.profileSource = SERVICE_PROFILE_SOURCE_MACHINE_SLOT;
        if (applied_profile_indicator_slot(in, &reason) != 0) return 2059;
        in.profileSource = SERVICE_PROFILE_SOURCE_AD_HOC;
        if (applied_profile_indicator_slot(in, &reason) != 0) return 2060;
        in.profileSource = SERVICE_PROFILE_SOURCE_NONE;
        if (applied_profile_indicator_slot(in, &reason) != 0) return 2061;

        // Out-of-range slots are refused before they can index anything.
        in.profileSource = SERVICE_PROFILE_SOURCE_USER_SLOT;
        in.profileSlot = 0;
        if (applied_profile_indicator_slot(in, &reason) != 0 ||
            reason != APPLIED_PROFILE_REASON_NOT_A_USER_SLOT) return 2062;
        in.profileSlot = 6;
        if (applied_profile_indicator_slot(in, &reason) != 0 ||
            reason != APPLIED_PROFILE_REASON_NOT_A_USER_SLOT) return 2063;
        in.profileSlot = 5;
        if (applied_profile_indicator_slot(in, &reason) != 5) return 2064;

        // The service owns the slot but published no readable intent: there is
        // nothing to compare against, so the tick is cleared, and the reason
        // says which of the two "cleared" branches it was.
        in.activeDesiredValid = false;
        if (applied_profile_indicator_slot(in, &reason) != 0 ||
            reason != APPLIED_PROFILE_REASON_NO_ACTIVE_INTENT) return 2065;

        // A null reason pointer is allowed; the slot answer is unchanged.
        in.activeDesiredValid = true;
        if (applied_profile_indicator_slot(in, nullptr) != 5) return 2066;

        // Every reason has a distinct, non-empty name: the debug log is the
        // whole diagnosis path for "why did my tick disappear".
        const AppliedProfileIndicatorReason reasons[] = {
            APPLIED_PROFILE_REASON_NO_AUTHORITY,
            APPLIED_PROFILE_REASON_NOT_A_USER_SLOT,
            APPLIED_PROFILE_REASON_NO_ACTIVE_INTENT,
            APPLIED_PROFILE_REASON_PROFILE_UNREADABLE,
            APPLIED_PROFILE_REASON_PROFILE_EDITED,
            APPLIED_PROFILE_REASON_APPLIED,
        };
        for (size_t a = 0; a < sizeof(reasons) / sizeof(reasons[0]); ++a) {
            const char* nameA = applied_profile_indicator_reason_name(reasons[a]);
            if (!nameA || !nameA[0] || strcmp(nameA, "unknown") == 0) return 2067;
            for (size_t b = a + 1; b < sizeof(reasons) / sizeof(reasons[0]); ++b) {
                if (strcmp(nameA,
                        applied_profile_indicator_reason_name(reasons[b])) == 0)
                    return 2068;
            }
        }
    }

    // The SERVICE-side half of the same story: which claimed profile identity a
    // successful APPLY is allowed to record.
    //
    // Regression: the identity was only ever proven against the request PAYLOAD.
    // A GUI Apply is deliberately a delta -- capture_gui_apply_settings() drops
    // domains the editor is not changing, most often the fan -- so it could never
    // equal a complete stored record, and applying a profile from the main window
    // was recorded as ad-hoc.  The tray menu then showed no tick and the tooltip
    // said "Manual settings" until the same profile was re-picked from the tray,
    // which is the only path that had ever sent a whole record.
    {
        const int maxSlots = 5;
        const unsigned int user =
            (unsigned int)SERVICE_PROFILE_SOURCE_USER_SLOT;
        const unsigned int shared =
            (unsigned int)SERVICE_PROFILE_SOURCE_SHARED_SLOT;

        // Nothing proved: the apply owns the GPU without belonging to a slot.
        if (service_profile_identity_outcome(user, 2, maxSlots, false, false) !=
            SERVICE_PROFILE_IDENTITY_AD_HOC) return 2210;

        // The whole-record path (tray, hotkey, app start) is unchanged.
        if (service_profile_identity_outcome(user, 2, maxSlots, true, false) !=
            SERVICE_PROFILE_IDENTITY_FROM_REQUEST) return 2211;

        // The defect: a delta payload whose RESULTING intent is the record now
        // earns the same slot identity.
        if (service_profile_identity_outcome(user, 2, maxSlots, false, true) !=
            SERVICE_PROFILE_IDENTITY_FROM_ACTIVE_INTENT) return 2212;

        // The payload proof outranks the post-write one: it is established
        // before the hardware is touched, and only it may replace ownership.
        if (service_profile_identity_outcome(user, 2, maxSlots, true, true) !=
            SERVICE_PROFILE_IDENTITY_FROM_REQUEST) return 2213;

        // Shared slots take both routes as well; they are a separate bank, not
        // a masquerade of the same-numbered personal slot.
        if (service_profile_identity_outcome(shared, 1, maxSlots, false, true) !=
            SERVICE_PROFILE_IDENTITY_FROM_ACTIVE_INTENT) return 2214;

        // Sources that can never name a stored record stay ad-hoc no matter how
        // convincingly the comparisons came out.
        if (service_profile_identity_outcome(
                (unsigned int)SERVICE_PROFILE_SOURCE_MACHINE_SLOT, 1, maxSlots,
                true, true) != SERVICE_PROFILE_IDENTITY_AD_HOC) return 2215;
        if (service_profile_identity_outcome(
                (unsigned int)SERVICE_PROFILE_SOURCE_AD_HOC, 1, maxSlots,
                true, true) != SERVICE_PROFILE_IDENTITY_AD_HOC) return 2216;
        if (service_profile_identity_outcome(
                (unsigned int)SERVICE_PROFILE_SOURCE_NONE, 1, maxSlots,
                true, true) != SERVICE_PROFILE_IDENTITY_AD_HOC) return 2217;

        // Out-of-range slots are refused before anything can index them.
        if (service_profile_identity_outcome(user, 0, maxSlots, true, true) !=
            SERVICE_PROFILE_IDENTITY_AD_HOC) return 2218;
        if (service_profile_identity_outcome(user, maxSlots + 1, maxSlots, true,
                true) != SERVICE_PROFILE_IDENTITY_AD_HOC) return 2219;
        if (service_profile_identity_outcome(user, -1, maxSlots, true, true) !=
            SERVICE_PROFILE_IDENTITY_AD_HOC) return 2220;
        if (!service_profile_metadata_claims_slot(user, maxSlots, maxSlots))
            return 2221;
        if (service_profile_metadata_claims_slot(user, maxSlots + 1, maxSlots))
            return 2222;

        // Only the whole-record proof may replace active ownership.  A delta
        // treated as a complete declaration would return every domain it omits
        // to defaults -- which is exactly how an Apply that never mentioned the
        // fan would reset a running fan curve.
        if (!service_profile_identity_replaces_active_intent(
                SERVICE_PROFILE_IDENTITY_FROM_REQUEST)) return 2223;
        if (service_profile_identity_replaces_active_intent(
                SERVICE_PROFILE_IDENTITY_FROM_ACTIVE_INTENT)) return 2224;
        if (service_profile_identity_replaces_active_intent(
                SERVICE_PROFILE_IDENTITY_AD_HOC)) return 2225;

        // Every outcome has a distinct, non-empty name; the debug log is the
        // whole diagnosis path for "why was my profile recorded as ad-hoc".
        const ServiceProfileIdentityOutcome outcomes[] = {
            SERVICE_PROFILE_IDENTITY_AD_HOC,
            SERVICE_PROFILE_IDENTITY_FROM_REQUEST,
            SERVICE_PROFILE_IDENTITY_FROM_ACTIVE_INTENT,
        };
        for (size_t a = 0; a < sizeof(outcomes) / sizeof(outcomes[0]); ++a) {
            const char* nameA = service_profile_identity_outcome_name(outcomes[a]);
            if (!nameA || !nameA[0] || strcmp(nameA, "unknown") == 0) return 2226;
            for (size_t b = a + 1; b < sizeof(outcomes) / sizeof(outcomes[0]); ++b) {
                if (strcmp(nameA,
                        service_profile_identity_outcome_name(outcomes[b])) == 0)
                    return 2227;
            }
        }
    }

    // The transitional presentation while a hardware write is in flight.  An
    // apply deliberately takes seconds (reset to a stock baseline, settle, then
    // write), and without this the tray kept advertising the OLD profile's
    // theme and tooltip for that whole window.
    {
        // Steady state: unchanged theme selection.
        if (gui_apply_in_flight_tray_icon_state(false, false, false) !=
            TRAY_ICON_STATE_DEFAULT) return 2228;
        if (gui_apply_in_flight_tray_icon_state(false, true, false) !=
            TRAY_ICON_STATE_OC) return 2229;
        if (gui_apply_in_flight_tray_icon_state(false, false, true) !=
            TRAY_ICON_STATE_FAN) return 2230;
        if (gui_apply_in_flight_tray_icon_state(false, true, true) !=
            TRAY_ICON_STATE_OC_FAN) return 2231;
        // An active lock/pin is part of the OC domain even when no offset,
        // power, or curve delta is nonzero, so a pinned-clock + custom-fan
        // profile reads as OC + Custom Fan instead of plain Custom Fan.
        if (!gui_tray_live_state_has_custom_oc(
                false, false, false, false, true)) return 4031;
        if (gui_tray_live_state_has_custom_oc(
                false, false, false, false, false)) return 4032;
        if (!gui_tray_live_state_has_custom_oc(
                true, false, false, false, false)) return 4033;
        if (!gui_tray_live_state_has_custom_oc(
                false, true, false, false, false)) return 4034;
        if (!gui_tray_live_state_has_custom_oc(
                false, false, true, false, false)) return 4035;
        if (!gui_tray_live_state_has_custom_oc(
                false, false, false, true, false)) return 4036;

        // In flight outranks all four: mid-write neither the old nor the new
        // OC/fan state is truthfully what the GPU has.
        if (gui_apply_in_flight_tray_icon_state(true, false, false) !=
            TRAY_ICON_STATE_PENDING) return 2232;
        if (gui_apply_in_flight_tray_icon_state(true, true, true) !=
            TRAY_ICON_STATE_PENDING) return 2233;
        if (gui_apply_in_flight_tray_icon_state(true, true, false) !=
            TRAY_ICON_STATE_PENDING) return 2234;
        if (gui_apply_in_flight_tray_icon_state(true, false, true) !=
            TRAY_ICON_STATE_PENDING) return 2235;

        // Every theme has its own icon slot; the pending one must not alias an
        // existing state or index past the loaded array.
        if (TRAY_ICON_STATE_PENDING == TRAY_ICON_STATE_DEFAULT ||
            TRAY_ICON_STATE_PENDING == TRAY_ICON_STATE_OC ||
            TRAY_ICON_STATE_PENDING == TRAY_ICON_STATE_FAN ||
            TRAY_ICON_STATE_PENDING == TRAY_ICON_STATE_OC_FAN) return 2236;
        if (TRAY_ICON_STATE_PENDING >= TRAY_ICON_STATE_COUNT) return 2237;

        // Both surfaces say the same thing, because both are built from the
        // one shared phrase.
        char tip[128] = {};
        gui_apply_in_flight_tray_tooltip(tip, sizeof(tip));
        if (!strstr(tip, GUI_APPLY_IN_FLIGHT_PHRASE)) return 2238;
        if (!strstr(tip, "Green Curve")) return 2239;
        char status[512] = {};
        gui_apply_in_flight_status_text(status, sizeof(status));
        if (!strstr(status, GUI_APPLY_IN_FLIGHT_PHRASE)) return 2240;
        // Only the banner has room to explain that the wait is deliberate. The
        // status label is one clipped line, so the short form has to stay short.
        char detail[256] = {};
        gui_apply_in_flight_detail_text(detail, sizeof(detail));
        if (strlen(detail) <= strlen(status)) return 2241;

        // A tiny buffer truncates but stays NUL-terminated, and a null one is
        // ignored rather than crashing a tray update.
        char tiny[6] = {};
        gui_apply_in_flight_tray_tooltip(tiny, sizeof(tiny));
        if (tiny[sizeof(tiny) - 1] != '\0') return 2242;
        gui_apply_in_flight_status_text(tiny, sizeof(tiny));
        if (tiny[sizeof(tiny) - 1] != '\0') return 2243;
        gui_apply_in_flight_tray_tooltip(nullptr, 0);
        gui_apply_in_flight_status_text(nullptr, 0);

        gui_apply_in_flight_detail_text(tiny, sizeof(tiny));
        if (tiny[sizeof(tiny) - 1] != '\0') return 2245;
        gui_apply_in_flight_detail_text(nullptr, 0);
    }

    // ------------------------------------------------------------------
    // What the window does with a manual Apply / Reset RESULT (2305-2319)
    //
    // A clean success confirms itself on the status line and raises nothing; a
    // warning or an error still interrupts.  The old behaviour boxed every
    // outcome, including the ordinary one, which is how the box that matters
    // became something to dismiss without reading.
    // ------------------------------------------------------------------
    {
        // Only a result the window fully adopted may be presented at the
        // severity the service reported.  Anything else is an error here, even
        // when the envelope claims success -- the envelope then describes an
        // operation this window can no longer attribute.
        if (gui_mutation_result_severity(true, SERVICE_OUTCOME_SEVERITY_SUCCESS)
            != SERVICE_OUTCOME_SEVERITY_SUCCESS) return 2305;
        if (gui_mutation_result_severity(true, SERVICE_OUTCOME_SEVERITY_WARNING)
            != SERVICE_OUTCOME_SEVERITY_WARNING) return 2306;
        if (gui_mutation_result_severity(false, SERVICE_OUTCOME_SEVERITY_SUCCESS)
            != SERVICE_OUTCOME_SEVERITY_ERROR) return 2307;
        // A transport failure leaves the response zeroed, i.e. it carries the
        // SUCCESS value by default.  That must never become a silent success.
        ServiceResponse never = {};
        if (gui_mutation_result_severity(false, never.outcomeSeverity) !=
            SERVICE_OUTCOME_SEVERITY_ERROR) return 2308;
        // An envelope that somehow claims ERROR on an adopted success is still
        // not silent: it degrades to a warning rather than to nothing.
        if (gui_mutation_result_severity(true, SERVICE_OUTCOME_SEVERITY_ERROR)
            != SERVICE_OUTCOME_SEVERITY_WARNING) return 2309;

        if (gui_mutation_result_needs_prompt(SERVICE_OUTCOME_SEVERITY_SUCCESS))
            return 2310;
        if (!gui_mutation_result_needs_prompt(SERVICE_OUTCOME_SEVERITY_WARNING))
            return 2311;
        if (!gui_mutation_result_needs_prompt(SERVICE_OUTCOME_SEVERITY_ERROR))
            return 2312;

        // The profile is named the way the tray names it, from the REQUEST.
        char label[64] = {};
        gui_mutation_result_profile_label(label, sizeof(label),
            SERVICE_PROFILE_SOURCE_USER_SLOT, 3, 8);
        if (strcmp(label, "Profile 3") != 0) return 2313;
        gui_mutation_result_profile_label(label, sizeof(label),
            SERVICE_PROFILE_SOURCE_SHARED_SLOT, 2, 8);
        if (strcmp(label, "Shared profile 2") != 0) return 2314;
        gui_mutation_result_profile_label(label, sizeof(label),
            SERVICE_PROFILE_SOURCE_MACHINE_SLOT, 1, 8);
        if (strcmp(label, "Machine profile 1") != 0) return 2315;
        gui_mutation_result_profile_label(label, sizeof(label),
            SERVICE_PROFILE_SOURCE_AD_HOC, 0, 8);
        if (strcmp(label, "Manual settings") != 0) return 2316;
        // A personal slot outside the configured range is not a slot; it must
        // not be announced as "Profile 9" on an eight-slot build.
        gui_mutation_result_profile_label(label, sizeof(label),
            SERVICE_PROFILE_SOURCE_USER_SLOT, 9, 8);
        if (strcmp(label, "Manual settings") != 0) return 2317;

        char status[512] = {};
        gui_mutation_result_status_text(status, sizeof(status),
            GUI_MUTATION_APPLY, SERVICE_OUTCOME_SEVERITY_SUCCESS, "Profile 3",
            "Applied 4 setting changes successfully.");
        // The clean line states the outcome and nothing else: the service's own
        // count is not what the user asked about.
        if (strcmp(status, "Profile 3 applied.") != 0) return 2318;
        gui_mutation_result_status_text(status, sizeof(status),
            GUI_MUTATION_APPLY, SERVICE_OUTCOME_SEVERITY_WARNING, "Profile 3",
            "3 of 8 boost points matched.");
        if (strncmp(status, "Profile 3 applied with warnings: ", 33) != 0)
            return 2319;
        if (!strstr(status, "3 of 8 boost points matched.")) return 2319;
        gui_mutation_result_status_text(status, sizeof(status),
            GUI_MUTATION_APPLY, SERVICE_OUTCOME_SEVERITY_ERROR, "Profile 3",
            "GPU offset was not accepted by the driver");
        if (strncmp(status, "Profile 3 was not applied: ", 27) != 0) return 2320;
        gui_mutation_result_status_text(status, sizeof(status),
            GUI_MUTATION_RESET, SERVICE_OUTCOME_SEVERITY_SUCCESS, "Profile 3",
            "Reset applied.");
        // A reset names no profile: it is the removal of one.
        if (strcmp(status, "GPU settings reset to defaults.") != 0) return 2321;
        if (strstr(status, "Profile 3")) return 2321;
        gui_mutation_result_status_text(status, sizeof(status),
            GUI_MUTATION_RESET, SERVICE_OUTCOME_SEVERITY_ERROR, "", "");
        if (strcmp(status, "GPU reset failed.") != 0) return 2322;
        // An empty label still produces a sentence rather than a leading space.
        gui_mutation_result_status_text(status, sizeof(status),
            GUI_MUTATION_APPLY, SERVICE_OUTCOME_SEVERITY_SUCCESS, "", "");
        if (strcmp(status, "Settings applied.") != 0) return 2323;

        // The queue line names the same thing the completion line does, so the
        // two read as one sentence rather than two unrelated ones.
        char queued[256] = {};
        gui_mutation_queued_status_text(queued, sizeof(queued),
            GUI_MUTATION_APPLY, "Profile 3");
        if (strcmp(queued, "Applying Profile 3 to the GPU...") != 0) return 2324;
        gui_mutation_queued_status_text(queued, sizeof(queued),
            GUI_MUTATION_RESET, "Profile 3");
        if (strcmp(queued, "Resetting the GPU to its defaults...") != 0)
            return 2325;

        // Bounded and null-safe like every other formatter on these surfaces.
        char tinyStatus[8] = {};
        gui_mutation_result_status_text(tinyStatus, sizeof(tinyStatus),
            GUI_MUTATION_APPLY, SERVICE_OUTCOME_SEVERITY_WARNING, "Profile 3",
            "long detail that cannot fit");
        if (tinyStatus[sizeof(tinyStatus) - 1] != '\0') return 2326;
        gui_mutation_queued_status_text(tinyStatus, sizeof(tinyStatus),
            GUI_MUTATION_APPLY, "Profile 3");
        if (tinyStatus[sizeof(tinyStatus) - 1] != '\0') return 2326;
        char tinyLabel[4] = {};
        gui_mutation_result_profile_label(tinyLabel, sizeof(tinyLabel),
            SERVICE_PROFILE_SOURCE_USER_SLOT, 3, 8);
        if (tinyLabel[sizeof(tinyLabel) - 1] != '\0') return 2326;
        gui_mutation_result_status_text(nullptr, 0, GUI_MUTATION_APPLY,
            SERVICE_OUTCOME_SEVERITY_SUCCESS, nullptr, nullptr);
        gui_mutation_queued_status_text(nullptr, 0, GUI_MUTATION_APPLY, nullptr);
        gui_mutation_result_profile_label(nullptr, 0,
            SERVICE_PROFILE_SOURCE_USER_SLOT, 3, 8);
    }

    // The indeterminate sweep animating that banner. Indeterminate on purpose:
    // an apply has no meaningful percentage and inventing one would be a lie
    // about progress.
    {
        const int track = 400;
        GuiApplySweep first = gui_apply_in_flight_sweep(track, 0);
        // A quarter-width bar, starting fully off the left edge -- a bar that
        // pops in at the edges reads as a glitch, not as motion.
        if (first.width != track / 4) return 2247;
        if (first.x != -first.width) return 2248;

        // It advances monotonically across one cycle and clears the right edge
        // at the end of it.
        int previousX = first.x;
        for (unsigned int frame = 1; frame < GUI_APPLY_IN_FLIGHT_SWEEP_FRAMES;
             ++frame) {
            GuiApplySweep step = gui_apply_in_flight_sweep(track, frame);
            if (step.width != first.width) return 2249;
            if (step.x <= previousX) return 2250;
            previousX = step.x;
        }
        // At the end of a cycle the bar is partly past the right edge but has
        // not vanished; it leaves the track as the next cycle re-enters it.
        GuiApplySweep last = gui_apply_in_flight_sweep(
            track, GUI_APPLY_IN_FLIGHT_SWEEP_FRAMES - 1);
        if (last.x >= track) return 2251;
        if (last.x + last.width <= 0) return 2252;

        // And it wraps: frame N and frame N + one cycle are the same picture,
        // so a long apply cannot walk the bar off into nothing.
        for (unsigned int frame = 0; frame < 3; ++frame) {
            GuiApplySweep a = gui_apply_in_flight_sweep(track, frame);
            GuiApplySweep b = gui_apply_in_flight_sweep(
                track, frame + GUI_APPLY_IN_FLIGHT_SWEEP_FRAMES);
            if (a.x != b.x || a.width != b.width) return 2253;
        }
        // A frame counter that has run for days must still be drawable.
        // (Not named `far`: windows.h still defines that as a legacy keyword.)
        GuiApplySweep distant = gui_apply_in_flight_sweep(track, 0xFFFFFFF0u);
        if (distant.width != first.width) return 2254;
        if (distant.x < -distant.width || distant.x >= track) return 2255;

        // Degenerate tracks produce nothing to draw rather than a negative
        // width the GDI fill would interpret as an inverted rectangle.
        if (gui_apply_in_flight_sweep(0, 4).width != 0) return 2256;
        if (gui_apply_in_flight_sweep(-40, 4).width != 0) return 2257;
        // A track too narrow to hold a quarter still yields a visible bar.
        if (gui_apply_in_flight_sweep(2, 0).width != 1) return 2258;
    }

    // Where that banner is allowed to sit.
    //
    // Regression: it was first pinned to the top of the CLIENT area, which put
    // it straight over the GPU selector row. Every control in this window is a
    // child window, and a child paints OVER its parent instead of being clipped
    // by it -- so the selector was not hidden, it appeared to bleed through the
    // banner. The band must start at or below the header strip, always.
    {
        const int header = MAIN_LAYOUT_GRAPH_TOP_MARGIN_LOGICAL;
        const int bannerH = 70;

        GuiApplyBannerBand band = gui_apply_in_flight_banner_band(
            MAIN_LAYOUT_GRAPH_PREFERRED_HEIGHT_LOGICAL, header, bannerH);
        if (!band.visible) return 2259;
        // The invariant that broke.
        if (band.top < header) return 2260;
        if (band.bottom - band.top != bannerH) return 2261;
        if (band.bottom > MAIN_LAYOUT_GRAPH_PREFERRED_HEIGHT_LOGICAL) return 2262;

        // Holds at every graph height the layout can actually produce, so a
        // narrow window cannot slide the banner back up over the selector.
        for (int h = MAIN_LAYOUT_GRAPH_MIN_HEIGHT_LOGICAL;
             h <= MAIN_LAYOUT_GRAPH_MAX_HEIGHT_LOGICAL; ++h) {
            GuiApplyBannerBand each = gui_apply_in_flight_banner_band(
                h, header, bannerH);
            if (!each.visible) return 2263;
            if (each.top < header) return 2264;
            if (each.bottom > h) return 2265;
        }

        // Too short to hold the whole banner below the strip: no banner rather
        // than a shrunken one creeping back over the row. The tray icon and the
        // status line still report the state.
        if (gui_apply_in_flight_banner_band(header + bannerH - 1, header,
                bannerH).visible) return 2266;
        if (gui_apply_in_flight_banner_band(header, header, bannerH).visible)
            return 2267;
        // Exactly enough room is enough.
        if (!gui_apply_in_flight_banner_band(header + bannerH, header,
                bannerH).visible) return 2268;

        // Degenerate inputs draw nothing instead of an inverted rectangle.
        if (gui_apply_in_flight_banner_band(0, header, bannerH).visible)
            return 2269;
        if (gui_apply_in_flight_banner_band(-10, header, bannerH).visible)
            return 2270;
        if (gui_apply_in_flight_banner_band(400, header, 0).visible) return 2271;
        if (gui_apply_in_flight_banner_band(400, header, -5).visible) return 2272;
        if (gui_apply_in_flight_banner_band(400, -1, bannerH).visible) return 2273;
        // A zero-height header strip is legal (no reserved row), and then the
        // banner may legitimately start at the very top.
        GuiApplyBannerBand flush = gui_apply_in_flight_banner_band(400, 0, bannerH);
        if (!flush.visible || flush.top != 0) return 2274;
    }

    // Fan policy honors configured interval and hysteresis, while explicit
    // desired-state changes can force an immediate target refresh.
    {
        FanCurveConfig curve = {};
        fan_curve_set_default(&curve);
        curve.pollIntervalMs = 1250;
        curve.hysteresisC = 2;
        FanRuntimeState state = {};
        FanRuntimeDecision first = fan_runtime_next_action(&state, &curve, 30,
            false);
        if (!first.shouldWrite || first.targetPercent != 20 ||
            first.nextPollMs != 1250) return 1016;
        FanRuntimeDecision raised = fan_runtime_next_action(&state, &curve, 32,
            false);
        if (raised.targetPercent == first.targetPercent) return 1017;
        FanRuntimeDecision held = fan_runtime_next_action(&state, &curve, 31,
            false);
        if (held.targetPercent != raised.targetPercent) return 1018;
        FanRuntimeDecision cooled = fan_runtime_next_action(&state, &curve, 30,
            false);
        if (cooled.targetPercent != first.targetPercent) return 1035;
        FanRuntimeDecision forced = fan_runtime_next_action(&state, &curve, 31,
            true);
        if (forced.targetPercent != fan_curve_interpolate_percent(&curve, 31))
            return 1019;
    }

    // Shared fan failure escalation ladder.  The Linux daemon previously
    // ignored telemetry loss entirely, so a failed temperature read left the
    // fan pinned at the last written duty forever with no journal entry.
    // Both outcomes must now walk the identical ladder on both platforms.
    {
        FanRuntimeFailureDecision ok = fan_runtime_observe_result(
            2, FAN_RUNTIME_OUTCOME_SUCCESS, 3);
        if (ok.failureCount != 0 || ok.shouldLogFailure ||
            ok.escalation != FAN_RUNTIME_ESCALATION_NONE) return 1300;

        FanRuntimeFailureDecision write1 = fan_runtime_observe_result(
            0, FAN_RUNTIME_OUTCOME_WRITE_FAILED, 3);
        if (write1.failureCount != 1 || !write1.shouldLogFailure ||
            write1.escalation != FAN_RUNTIME_ESCALATION_NONE) return 1301;
        FanRuntimeFailureDecision write2 = fan_runtime_observe_result(
            1, FAN_RUNTIME_OUTCOME_WRITE_FAILED, 3);
        if (write2.failureCount != 2 ||
            write2.escalation != FAN_RUNTIME_ESCALATION_NONE) return 1302;
        FanRuntimeFailureDecision write3 = fan_runtime_observe_result(
            2, FAN_RUNTIME_OUTCOME_WRITE_FAILED, 3);
        if (write3.failureCount != 3 ||
            write3.escalation != FAN_RUNTIME_ESCALATION_RESTORE_AUTO)
            return 1303;

        // Telemetry loss escalates identically to a refused write.
        FanRuntimeFailureDecision temp1 = fan_runtime_observe_result(
            0, FAN_RUNTIME_OUTCOME_TELEMETRY_FAILED, 3);
        if (temp1.failureCount != 1 || !temp1.shouldLogFailure ||
            temp1.escalation != FAN_RUNTIME_ESCALATION_NONE) return 1304;
        FanRuntimeFailureDecision temp3 = fan_runtime_observe_result(
            2, FAN_RUNTIME_OUTCOME_TELEMETRY_FAILED, 3);
        if (temp3.escalation != FAN_RUNTIME_ESCALATION_RESTORE_AUTO)
            return 1305;

        // Mixed write/telemetry failures share one counter.
        FanRuntimeFailureDecision mixed = fan_runtime_observe_result(
            temp1.failureCount, FAN_RUNTIME_OUTCOME_WRITE_FAILED, 3);
        if (mixed.failureCount != 2 ||
            mixed.escalation != FAN_RUNTIME_ESCALATION_NONE) return 1306;

        // A zero limit falls back to the documented default rather than
        // escalating on the very first failure.
        FanRuntimeFailureDecision defaulted = fan_runtime_observe_result(
            0, FAN_RUNTIME_OUTCOME_WRITE_FAILED, 0);
        if (defaulted.escalation != FAN_RUNTIME_ESCALATION_NONE) return 1307;
        if (fan_runtime_observe_result(FAN_RUNTIME_DEFAULT_FAILURE_LIMIT - 1,
                FAN_RUNTIME_OUTCOME_WRITE_FAILED, 0).escalation !=
            FAN_RUNTIME_ESCALATION_RESTORE_AUTO) return 1308;

        // The counter saturates instead of wrapping back through zero, so a
        // long-running failure can never silently re-enter the "healthy" state.
        FanRuntimeFailureDecision saturated = fan_runtime_observe_result(
            0xFFFFFFFFu, FAN_RUNTIME_OUTCOME_WRITE_FAILED, 3);
        if (saturated.failureCount != 0xFFFFFFFFu ||
            saturated.escalation != FAN_RUNTIME_ESCALATION_RESTORE_AUTO)
            return 1309;

        // Handing the fan back to the driver is the recovery; if the driver
        // refuses, maximum cooling is the only safe remaining action.
        if (fan_runtime_escalation_after_auto_restore(true) !=
            FAN_RUNTIME_ESCALATION_NONE) return 1310;
        if (fan_runtime_escalation_after_auto_restore(false) !=
            FAN_RUNTIME_ESCALATION_EMERGENCY_MAX) return 1311;
        if (FAN_RUNTIME_EMERGENCY_PERCENT != 100) return 1312;
    }

    // Manual fan write verification.  Linux gated fixed/curve applies on
    // `nvmlDeviceGetFanSpeed_v2 == requested`, but that entry point reports
    // *measured* duty: on an idle RTX 5070 it reads 0 while the zero-RPM fan
    // stop holds, so every accepted write was declared a failure and the whole
    // apply rolled back.  The intent readback is the actual readback.
    {
        // The exact case that broke manual fan control: write 35%, driver
        // accepts and reports intent 35%, fan has not spun up so measured is 0.
        if (!fan_manual_write_confirmed(35, 0, 35, true)) return 1313;
        // Same situation with no intent getter at all: the measured value is
        // all there is, and it does not confirm the write.
        if (fan_manual_write_confirmed(35, 0, 0, false)) return 1314;
        // Measured lags/overshoots within tolerance while spinning up.
        if (!fan_manual_write_confirmed(55, 57, 0, false)) return 1315;
        if (!fan_manual_write_confirmed(55, 53, 0, false)) return 1316;
        if (fan_manual_write_confirmed(55, 59, 0, false)) return 1317;
        // A driver that reports a different intent than we asked for did not
        // take the write, however the tachometer happens to read.
        if (fan_manual_write_confirmed(35, 0, 80, true)) return 1318;
        // A genuine 0% intent confirms a 0% write; without an intent getter a
        // measured 0 is ambiguous (a stopped fan reads 0 under driver auto too)
        // and only exact 0 may confirm it.
        if (!fan_manual_write_confirmed(0, 0, 0, true)) return 1319;
        if (!fan_manual_write_confirmed(0, 0, 0, false)) return 1340;
        if (fan_manual_write_confirmed(0, 2, 0, false)) return 1341;

        // The driver silently clamps a duty into its advertised range, so
        // Green Curve clamps first and verifies against the clamped value.
        // An RTX 5070 reports 30..100 and answers a write of 10% with 30%.
        if (fan_manual_effective_percent(10, 30, 100, true) != 30) return 1342;
        if (fan_manual_effective_percent(35, 30, 100, true) != 35) return 1343;
        if (fan_manual_effective_percent(120, 30, 100, true) != 100) return 1344;
        if (fan_manual_effective_percent(-5, 30, 100, true) != 30) return 1345;
        // Range unknown: clamp to the protocol range only, never invent a floor.
        if (fan_manual_effective_percent(10, 0, 0, false) != 10) return 1346;
        if (fan_manual_effective_percent(150, 0, 0, false) != 100) return 1347;
        // A nonsensical range from the driver is ignored rather than trusted.
        if (fan_manual_effective_percent(35, 80, 20, true) != 35) return 1348;
    }

    // Power limit percentage is relative to the *board default*, so above 100%
    // is a normal request.  The Linux UI normalizer ran it through the generic
    // 0..100 percent clamp, so a 105% target was silently rewritten to 100% on
    // load, on save, and on every TUI refresh after an apply.
    {
        if (POWER_LIMIT_MIN_PCT != 50 || POWER_LIMIT_MAX_PCT != 150) return 1349;
        if (clamp_power_limit_pct(105) != 105) return 1350;
        if (clamp_power_limit_pct(120) != 120) return 1351;
        if (clamp_power_limit_pct(150) != 150) return 1352;
        if (clamp_power_limit_pct(151) != 150) return 1353;
        if (clamp_power_limit_pct(49) != 50) return 1354;
        if (clamp_power_limit_pct(0) != 50) return 1355;

        // The whole reported symptom, end to end: the value the TUI shows comes
        // from normalize_desired_settings_for_ui() over the daemon's active
        // desired state.
        DesiredSettings powered = {};
        initialize_desired_settings_defaults(&powered);
        powered.hasPowerLimit = true;
        powered.powerLimitPct = 105;
        normalize_desired_settings_for_ui(&powered);
        if (powered.powerLimitPct != 105) return 1356;
        // An unset power limit still defaults to 100%, not to the 50% floor.
        DesiredSettings unset = {};
        initialize_desired_settings_defaults(&unset);
        unset.powerLimitPct = 0;
        normalize_desired_settings_for_ui(&unset);
        if (unset.powerLimitPct != 100) return 1357;

        // The IPC trust boundary uses the same range, so a 105% request
        // survives the daemon hop instead of being clamped at the door.
        DesiredSettings overWire = {};
        initialize_desired_settings_defaults(&overWire);
        overWire.hasPowerLimit = true;
        overWire.powerLimitPct = 105;
        validate_desired_settings_for_ipc(&overWire);
        if (overWire.powerLimitPct != 105) return 1358;
        overWire.powerLimitPct = 900;
        validate_desired_settings_for_ipc(&overWire);
        if (overWire.powerLimitPct != POWER_LIMIT_MAX_PCT) return 1359;
    }

    // Profile Save's capture source: live hardware defaults are only safe when
    // neither the editor nor the applied state owns curve/lock intent.  An
    // applied hard pin is intent, not drift, so it must go through the GUI
    // capture or the saved slot stops matching the active profile and the tray
    // tick disappears.
    {
        if (profile_save_uses_gui_capture(false, false, false)) return 1360;
        if (!profile_save_uses_gui_capture(true, false, false)) return 1361;
        if (!profile_save_uses_gui_capture(false, true, false)) return 1362;
        if (!profile_save_uses_gui_capture(false, false, true)) return 1363;
    }

    // The ownership check may ignore an unclaimed fan domain after a delta
    // Apply, but the strict pre-write claim check must not.
    {
        if (!profile_ownership_fan_mismatch_allowed(true, true, false)) return 1364;
        if (profile_ownership_fan_mismatch_allowed(false, true, false)) return 1365;
        if (profile_ownership_fan_mismatch_allowed(true, false, true)) return 1366;
        if (profile_ownership_fan_mismatch_allowed(true, false, false)) return 1367;
        if (profile_ownership_fan_mismatch_allowed(true, true, true)) return 1368;
    }

    // accept() failure classification.  Treating every non-EINTR errno as
    // fatal ended the daemon's serve loop permanently, and it exited zero so
    // systemd's Restart=on-failure never brought it back.
    {
        const int transient[] = {EINTR, ECONNABORTED, EAGAIN, EPROTO,
                                 ENOBUFS, ENOMEM, EPERM};
        for (size_t i = 0; i < sizeof(transient) / sizeof(transient[0]); i++) {
            if (daemon_accept_disposition(transient[i]) != DAEMON_ACCEPT_RETRY)
                return 1320;
            if (daemon_accept_error_is_fatal(transient[i])) return 1321;
        }
        if (daemon_accept_disposition(EMFILE) != DAEMON_ACCEPT_RECLAIM_FD)
            return 1322;
        if (daemon_accept_disposition(ENFILE) != DAEMON_ACCEPT_RECLAIM_FD)
            return 1323;
        if (daemon_accept_error_is_fatal(EMFILE)) return 1324;
        if (daemon_accept_error_is_fatal(ENFILE)) return 1325;
        const int fatal[] = {EBADF, EINVAL, ENOTSOCK, EOPNOTSUPP};
        for (size_t i = 0; i < sizeof(fatal) / sizeof(fatal[0]); i++) {
            if (daemon_accept_disposition(fatal[i]) != DAEMON_ACCEPT_FATAL)
                return 1326;
            if (!daemon_accept_error_is_fatal(fatal[i])) return 1327;
        }
    }

    // Hotkey tokens come from [hotkeys] slotN in the user config.  The f-key
    // digit run used to be accumulated without a bound, so a long numeric
    // suffix overflowed int (undefined behaviour under -fno-sanitize-recover).
    {
        HotkeyBinding b = {};
        if (!hotkey_parse("f24", &b)) return 1330;
        if (b.vk != 0x70 + 23) return 1331;
        if (hotkey_parse("f25", &b)) return 1332;
        if (hotkey_parse("f0", &b)) return 1333;
        // Would overflow a 32-bit int before the range check.
        if (hotkey_parse("f99999999999999999999", &b)) return 1334;
        if (hotkey_parse("ctrl+f2147483648", &b)) return 1335;
        // Over-length input must be rejected, not silently truncated into a
        // different valid binding.
        {
            char overlong[200] = {};
            for (size_t i = 0; i < sizeof(overlong) - 1; i++) overlong[i] = 'a';
            if (hotkey_parse(overlong, &b)) return 1336;
            char padded[200] = {};
            size_t pad = 0;
            while (pad < sizeof(padded) - 6) padded[pad++] = ' ';
            padded[pad++] = 'c'; padded[pad++] = 't'; padded[pad++] = 'r';
            padded[pad++] = 'l'; padded[pad++] = '+';
            if (hotkey_parse(padded, &b)) return 1337;
        }
        // Still accepts the ordinary bindings the dialog and config produce.
        if (!hotkey_parse("ctrl+alt+f2", &b)) return 1338;
        if (!hotkey_parse("f1", &b) || b.vk != 0x70) return 1339;
    }

    // One active mutation plus one latest pending request; Reset cannot be
    // overtaken by a later Apply.
    {
        if (gui_mutation_queue_decide(false, false, GUI_MUTATION_APPLY,
                GUI_MUTATION_APPLY) != GUI_MUTATION_QUEUE_START) return 1020;
        if (gui_mutation_queue_decide(true, false, GUI_MUTATION_APPLY,
                GUI_MUTATION_APPLY) != GUI_MUTATION_QUEUE_PENDING) return 1021;
        if (gui_mutation_queue_decide(true, true, GUI_MUTATION_APPLY,
                GUI_MUTATION_APPLY) != GUI_MUTATION_QUEUE_REPLACE_PENDING)
            return 1022;
        if (gui_mutation_queue_decide(true, true, GUI_MUTATION_APPLY,
                GUI_MUTATION_RESET) != GUI_MUTATION_QUEUE_REPLACE_PENDING)
            return 1023;
        if (gui_mutation_queue_decide(true, true, GUI_MUTATION_RESET,
                GUI_MUTATION_APPLY) != GUI_MUTATION_QUEUE_KEEP_PENDING_RESET)
            return 1024;
    }

    // Read scheduling is deterministic: full-sync requests coalesce and
    // telemetry never overtakes writes, admin changes, or full synchronization.
    {
        if (gui_full_sync_queue_decide(false) !=
                GUI_SERVICE_READ_QUEUE_NEW) return 1132;
        if (gui_full_sync_queue_decide(true) !=
                GUI_SERVICE_READ_COALESCE) return 1133;
        if (gui_telemetry_queue_decide(true, false, false, false, false) !=
                GUI_SERVICE_READ_DROP_BEHIND_PRIORITY_WORK) return 1134;
        if (gui_telemetry_queue_decide(false, true, false, false, false) !=
                GUI_SERVICE_READ_DROP_BEHIND_PRIORITY_WORK) return 1135;
        if (gui_telemetry_queue_decide(false, false, true, false, false) !=
                GUI_SERVICE_READ_DROP_BEHIND_PRIORITY_WORK) return 1136;
        if (gui_telemetry_queue_decide(false, false, false, true, false) !=
                GUI_SERVICE_READ_DROP_BEHIND_PRIORITY_WORK) return 1137;
        if (gui_telemetry_queue_decide(false, false, false, false, true) !=
                GUI_SERVICE_READ_COALESCE) return 1138;
        if (gui_telemetry_queue_decide(false, false, false, false, false) !=
                GUI_SERVICE_READ_QUEUE_NEW) return 1139;
    }

    // The advertised overclock ranges are the driver window INTERSECTED with the
    // caps the apply path actually enforces.  Advertising the raw driver range
    // would promise GPU offsets the IPC validator silently clamps and power
    // percentages gpu_backend_apply rejects outright.
    {
        OcRangeInputs in = {};
        in.gpuKnown = true;
        in.gpuMinMHz = -2500;      // wider than the +/-1000 IPC bound
        in.gpuMaxMHz = 2500;
        in.memKnown = true;
        in.memMinMHz = -9000;      // wider than the +/-5000 IPC bound
        in.memMaxMHz = 9000;
        OcRangeBounds gpu = oc_range_gpu_offset(&in);
        if (!gpu.known || gpu.min != -1000 || gpu.max != 1000) return 1400;
        OcRangeBounds mem = oc_range_mem_offset(&in);
        if (!mem.known || mem.min != -5000 || mem.max != 5000) return 1401;

        // A narrower driver window is reported as-is, signs included.
        in.gpuMinMHz = -250;
        in.gpuMaxMHz = 400;
        gpu = oc_range_gpu_offset(&in);
        if (!gpu.known || gpu.min != -250 || gpu.max != 400) return 1402;

        // An unreported range must not print a fabricated interval, and an
        // inverted one is treated as no information rather than nonsense.
        in.gpuKnown = false;
        if (oc_range_gpu_offset(&in).known) return 1403;
        in.gpuKnown = true;
        in.gpuMinMHz = 400;
        in.gpuMaxMHz = -250;
        if (oc_range_gpu_offset(&in).known) return 1404;
    }

    // Power percent is derived from milliwatts: round the minimum up and the
    // maximum down so every advertised percent is actually accepted, then clip
    // to the enforced 50..150 gate.
    {
        OcRangeInputs in = {};
        in.powerDefaultmW = 300000;
        in.powerMinmW = 200000;    // 66.67% -> 67 after ceil
        in.powerMaxmW = 400000;    // 133.33% -> 133 after floor
        OcRangeBounds power = oc_range_power_pct(&in);
        if (!power.known || power.min != 67 || power.max != 133) return 1405;

        // A driver window wider than the enforced gate is clipped to the gate.
        in.powerMinmW = 30000;     // 10%
        in.powerMaxmW = 900000;    // 300%
        power = oc_range_power_pct(&in);
        if (power.min != 50 || power.max != 150) return 1406;

        // No constraints reported: the enforced gate IS the answer, and it is
        // known, so the caption never claims the power range is a mystery.
        OcRangeInputs empty = {};
        power = oc_range_power_pct(&empty);
        if (!power.known || power.min != 50 || power.max != 150) return 1407;

        // A default of zero cannot be divided by; fall back, do not crash.
        in.powerDefaultmW = 0;
        power = oc_range_power_pct(&in);
        if (!power.known || power.min != 50 || power.max != 150) return 1408;
    }

    // Hover tooltips are the only place the ranges are shown (cue banners
    // cannot paint on these never-empty edits), so each one must stay ASCII,
    // fit its buffer, name its own bounds, and keep marking the memory range
    // advisory -- F-DOM-1 applies past that range on purpose.
    {
        OcRangeInputs in = {};
        in.gpuKnown = true;
        in.gpuMinMHz = -1000;
        in.gpuMaxMHz = 1000;
        in.memKnown = true;
        in.memMinMHz = -2000;
        in.memMaxMHz = 3000;
        in.powerDefaultmW = 300000;
        in.powerMinmW = 200000;
        in.powerMaxmW = 400000;

        char tip[512] = {};
        oc_range_format_gpu_tip(tip, sizeof(tip), &in);
        size_t tipLen = strlen(tip);
        if (tipLen == 0 || tipLen >= sizeof(tip) - 1) return 1409;
        for (size_t i = 0; i < tipLen; i++) {
            unsigned char ch = (unsigned char)tip[i];
            if (ch != '\r' && ch != '\n' && (ch < 0x20 || ch > 0x7E)) return 1410;
        }
        if (!strstr(tip, "-1000..+1000 MHz")) return 1411;

        oc_range_format_mem_tip(tip, sizeof(tip), &in);
        if (!strstr(tip, "-2000..+3000 MHz")) return 1412;
        if (!strstr(tip, "Advisory only:")) return 1413;

        oc_range_format_power_tip(tip, sizeof(tip), &in);
        if (!strstr(tip, "67..133 %")) return 1414;

        in.gpuKnown = false;
        oc_range_format_gpu_tip(tip, sizeof(tip), &in);
        if (!strstr(tip, "unknown")) return 1418;

        // Every field tooltip must survive a small buffer without running off
        // the end; truncation is acceptable, a missing terminator is not.
        char tiny[24] = {};
        oc_range_format_gpu_tip(tiny, sizeof(tiny), &in);
        if (strlen(tiny) >= sizeof(tiny)) return 1415;
        oc_range_format_mem_tip(tiny, sizeof(tiny), &in);
        if (strlen(tiny) >= sizeof(tiny)) return 1416;
        oc_range_format_power_tip(tiny, sizeof(tiny), &in);
        if (strlen(tiny) >= sizeof(tiny)) return 1417;
    }

    // The high-overclock confirmation asks once about a genuinely new risk and
    // never nags: hand-typed only, at or past the threshold, and only when the
    // clock is actually being raised.
    {
        OcHighWarnThresholds thresholds = oc_high_warn_default_thresholds();
        if (thresholds.gpuOffsetMHz != 200 || thresholds.memOffsetMHz != 2000)
            return 1420;

        OcHighWarnInputs in = {};
        in.hasGpuOffset = true;
        in.gpuHandTyped = true;
        in.currentGpuOffsetMHz = 0;

        in.gpuOffsetMHz = 199;
        if (oc_high_warn_decide(&in, &thresholds).warn) return 1421;
        in.gpuOffsetMHz = 200;      // the threshold itself must warn
        if (!oc_high_warn_decide(&in, &thresholds).gpu) return 1422;

        // Lowering an already-running high clock reduces risk: stay silent even
        // though the new value is still far past the threshold.
        in.gpuOffsetMHz = 300;
        in.currentGpuOffsetMHz = 400;
        if (oc_high_warn_decide(&in, &thresholds).warn) return 1423;
        // Raising it past what is applied warns again.
        in.currentGpuOffsetMHz = 250;
        if (!oc_high_warn_decide(&in, &thresholds).gpu) return 1424;

        // A profile-sourced value is the user's own reviewed intent.
        in.currentGpuOffsetMHz = 0;
        in.gpuOffsetMHz = 500;
        in.gpuHandTyped = false;
        if (oc_high_warn_decide(&in, &thresholds).warn) return 1425;
        in.gpuHandTyped = true;
        if (!oc_high_warn_decide(&in, &thresholds).gpu) return 1426;

        // A request that carries no GPU offset at all cannot warn about one,
        // whatever stale number the struct happens to hold.
        in.hasGpuOffset = false;
        if (oc_high_warn_decide(&in, &thresholds).warn) return 1427;
        in.hasGpuOffset = true;

        // Memory has its own threshold and its own provenance.
        in.hasMemOffset = true;
        in.memHandTyped = true;
        in.currentMemOffsetMHz = 0;
        in.memOffsetMHz = 1999;
        OcHighWarnDecision decision = oc_high_warn_decide(&in, &thresholds);
        if (!decision.gpu || decision.mem) return 1428;
        in.memOffsetMHz = 2500;
        decision = oc_high_warn_decide(&in, &thresholds);
        if (!decision.gpu || !decision.mem || !decision.warn) return 1429;

        // One dialog names both domains.
        char message[512] = {};
        oc_high_warn_format_message(message, sizeof(message), &decision, &in,
                                    &thresholds);
        if (!strstr(message, "GPU offset +500 MHz")) return 1430;
        if (!strstr(message, "Memory offset +2500 MHz")) return 1431;
        if (!strstr(message, "Apply anyway?")) return 1432;

        // A zero threshold is the documented opt-out and disables only its own
        // domain.
        OcHighWarnThresholds gpuOff = thresholds;
        gpuOff.gpuOffsetMHz = 0;
        decision = oc_high_warn_decide(&in, &gpuOff);
        if (decision.gpu || !decision.mem) return 1433;
        OcHighWarnThresholds bothOff = {};
        if (oc_high_warn_decide(&in, &bothOff).warn) return 1434;

        // Nothing to warn about produces no message at all, so a caller can
        // never show an empty dialog.
        OcHighWarnDecision silent = {};
        message[0] = 'x';
        oc_high_warn_format_message(message, sizeof(message), &silent, &in,
                                    &thresholds);
        if (message[0] != '\0') return 1435;
    }

    // The themed message box replaces the stock one, so it has to reproduce the
    // stock button sets exactly -- and it must never turn a dismissal into a
    // confirmation.
    {
        MessageBoxButtonSet set = message_box_button_set(GC_MB_OK);
        if (set.count != 1 || set.id[0] != GC_ID_OK) return 1440;
        if (set.escapeId != GC_ID_OK || set.defaultIndex != 0) return 1441;

        set = message_box_button_set(GC_MB_YESNO);
        if (set.count != 2 || set.id[0] != GC_ID_YES || set.id[1] != GC_ID_NO)
            return 1442;
        // Dismissing "apply this high overclock?" must mean No.
        if (set.escapeId != GC_ID_NO) return 1443;

        set = message_box_button_set(GC_MB_YESNOCANCEL);
        if (set.count != 3 || set.id[2] != GC_ID_CANCEL) return 1444;
        if (set.escapeId != GC_ID_CANCEL) return 1445;

        set = message_box_button_set(GC_MB_OKCANCEL);
        if (set.count != 2 || set.id[0] != GC_ID_OK || set.id[1] != GC_ID_CANCEL)
            return 1446;
        set = message_box_button_set(GC_MB_RETRYCANCEL);
        if (set.count != 2 || set.id[0] != GC_ID_RETRY) return 1447;

        // Icon and default-button flags ride in the same word as the type and
        // must not change which buttons appear.
        set = message_box_button_set(GC_MB_YESNO | GC_MB_ICONWARNING);
        if (set.count != 2 || set.id[0] != GC_ID_YES) return 1448;
        set = message_box_button_set(GC_MB_YESNO | GC_MB_DEFBUTTON2);
        if (set.defaultIndex != 1) return 1449;
        // A default button past the end of the set falls back to the first one
        // rather than indexing off the array.
        set = message_box_button_set(GC_MB_YESNO | GC_MB_DEFBUTTON3);
        if (set.defaultIndex != 0) return 1450;
        set = message_box_button_set(GC_MB_YESNOCANCEL | GC_MB_DEFBUTTON3);
        if (set.defaultIndex != 2) return 1451;
        // An unknown type degrades to a plain OK box, never to zero buttons.
        set = message_box_button_set(0x0000000E);
        if (set.count != 1 || set.id[0] != GC_ID_OK) return 1452;
    }

    // Message box geometry: the dialog sizes itself around the measured text,
    // right-aligns the button group, and never lets the buttons overlap the
    // body or leave the client area.
    {
        MessageBoxLayoutInput in = {};
        in.dpi = 96;
        in.hasIcon = true;
        in.iconSize = 32;
        in.textWidth = 300;
        in.textHeight = 60;
        in.buttonCount = 2;
        in.buttonWidth = 88;
        in.buttonHeight = 28;
        MessageBoxLayoutPlan plan = message_box_build_layout(&in);

        int margin = 16;
        if (plan.textX != margin + 32 + 14) return 1460;
        if (plan.clientWidth != margin * 2 + (plan.textX - margin) + 300)
            return 1461;
        // Buttons sit below the body and inside the right margin.
        if (plan.buttonY < plan.textY + in.textHeight) return 1462;
        int buttonsRight = plan.firstButtonX + in.buttonCount * in.buttonWidth +
                           (in.buttonCount - 1) * 8;
        if (buttonsRight != plan.clientWidth - margin) return 1463;
        if (plan.firstButtonX < margin) return 1464;
        if (plan.clientHeight < plan.buttonY + in.buttonHeight + margin)
            return 1465;
        // The shorter of icon/text is centered against the taller one.
        if (plan.iconY <= plan.textY) return 1466;

        // A wide button row (three buttons, long labels) widens the dialog
        // instead of pushing the buttons off the left edge.
        in.textWidth = 40;
        in.buttonCount = 3;
        in.buttonWidth = 140;
        plan = message_box_build_layout(&in);
        if (plan.firstButtonX < margin) return 1467;
        if (plan.clientWidth < 3 * 140 + 2 * 8 + margin * 2) return 1468;

        // No icon means no icon gutter.
        in.hasIcon = false;
        plan = message_box_build_layout(&in);
        if (plan.textX != margin) return 1469;

        // Margins and gaps scale with DPI; the icon size is supplied by the
        // caller already in physical pixels, so it is used verbatim.
        in.hasIcon = true;
        in.dpi = 192;
        in.iconSize = 64;
        MessageBoxLayoutPlan hidpi = message_box_build_layout(&in);
        if (hidpi.textX != 32 + 64 + 28) return 1470;
        MessageBoxLayoutPlan empty = message_box_build_layout(nullptr);
        if (empty.clientWidth != 0 || empty.clientHeight != 0) return 1471;
    }

    // F-PENDING: one predicate colours the fields orange AND decides whether
    // Apply is reachable, so it has to be conservative in exactly one direction.
    // Scalars first (GPU offset, exclude-low, memory offset, power limit, fixed
    // fan percent).
    {
        GuiPendingScalar scalar = {};
        scalar.draftValid = true;
        scalar.draftValue = 150;
        scalar.appliedValue = 150;
        if (gui_pending_scalar_changed(scalar)) return 1480;

        scalar.draftValue = 175;
        if (!gui_pending_scalar_changed(scalar)) return 1481;

        // Typing the applied value back into a field is not a change, which is
        // what replaces the old "No changes to apply" error box.
        scalar.draftValue = 150;
        if (gui_pending_scalar_changed(scalar)) return 1482;

        // Negative offsets and a zero baseline are ordinary values, not
        // sentinels.
        scalar.draftValue = -50;
        scalar.appliedValue = 0;
        if (!gui_pending_scalar_changed(scalar)) return 1483;
        scalar.draftValue = 0;
        if (gui_pending_scalar_changed(scalar)) return 1484;

        // THE anti-lockout rule: half-typed or garbage text always counts as a
        // pending change so Apply stays pressable and reports the real parse
        // error, whatever the (meaningless) parsed value happens to be.
        scalar.draftValid = false;
        scalar.draftValue = 0;
        scalar.appliedValue = 0;
        if (!gui_pending_scalar_changed(scalar)) return 1485;
    }

    // Curve points compare against drift-free applied INTENT. Expected boost /
    // temperature drift in the live curve must never light a point orange, and a
    // point the editor does not own is not the editor's claim to make.
    {
        GuiPendingCurvePoint point = {};
        point.visible = true;
        point.ownedByEditor = true;
        point.liveKnown = true;
        point.draftValid = true;
        point.draftMHz = 2800;
        point.appliedMHz = 2800;
        if (gui_pending_curve_point_changed(point)) return 1486;

        point.draftMHz = 2850;
        if (!gui_pending_curve_point_changed(point)) return 1487;

        // Not visible / not owned / no live readback: nothing to say.
        point.visible = false;
        if (gui_pending_curve_point_changed(point)) return 1488;
        point.visible = true;
        point.ownedByEditor = false;
        point.appliedMHz = 0;
        if (gui_pending_curve_point_changed(point)) return 1489;
        point.ownedByEditor = true;
        point.liveKnown = false;
        if (gui_pending_curve_point_changed(point)) return 1490;
        point.liveKnown = true;

        // An owned point with no applied baseline is exactly what a freshly
        // typed or freshly loaded point looks like, so it is pending even
        // though there is nothing to compare it with.
        point.appliedMHz = 0;
        point.draftMHz = 2850;
        if (!gui_pending_curve_point_changed(point)) return 1491;
        // ... but only when the editor actually owns it.
        point.ownedByEditor = false;
        if (gui_pending_curve_point_changed(point)) return 1492;
        point.ownedByEditor = true;

        // Unparseable text keeps Apply reachable here too.
        point.appliedMHz = 2800;
        point.draftValid = false;
        point.draftMHz = 2800;
        if (!gui_pending_curve_point_changed(point)) return 1493;

        // The ownership gate wins over invalid text only when neither side
        // owns the point: an unowned point cannot manufacture a pending change
        // out of stale draft contents.
        point.ownedByEditor = false;
        point.appliedMHz = 0;
        if (gui_pending_curve_point_changed(point)) return 1494;

        // The reported profile-switch case: the applied state owns a point the
        // loaded profile does not, so reset-before-apply releases it back to
        // stock and the preview must show that release.
        point.appliedMHz = 547;
        point.draftValid = false;
        if (!gui_pending_curve_point_changed(point)) return 4024;
        // Even a stale draft value from the previous profile cannot hide the
        // release: the editor no longer asserts this point.
        point.draftValid = true;
        point.draftMHz = 547;
        if (!gui_pending_curve_point_changed(point)) return 4025;
        // Neither side owns the point -> no change.
        point.appliedMHz = 0;
        if (gui_pending_curve_point_changed(point)) return 4026;
    }

    // Which number IS the lock target. capture_gui_desired_settings() prefers the
    // draft value at the anchor over g_app.lockedFreq, so the preview and the diff
    // must too. lockedFreq lags a profile projection -- apply_lock() infers it
    // from the draft, which the projection fills in only afterwards -- and with
    // profile 2 loaded on a stock GPU that made the graph preview the flattened
    // tail at the stock 2482 MHz instead of the profile's 2957 MHz.
    // tracksAnchor=false with equal offset components keeps the historical
    // contract: the base is the whole answer.
    {
        if (gui_pending_lock_target_mhz(true, true, 2957, 2482, false, 0, 0) != 2957) return 1552;
        // No attached draft, or nothing valid at the anchor: lockedFreq is all
        // there is.
        if (gui_pending_lock_target_mhz(false, true, 2957, 2482, false, 0, 0) != 2482) return 1553;
        if (gui_pending_lock_target_mhz(true, false, 2957, 2482, false, 0, 0) != 2482) return 1554;
        if (gui_pending_lock_target_mhz(false, false, 2957, 2482, false, 0, 0) != 2482) return 1555;
        // The draft wins even when it is the smaller value: this is a preference
        // for the fresher source, not for the higher clock.
        if (gui_pending_lock_target_mhz(true, true, 2400, 2500, false, 0, 0) != 2400) return 1556;
        // A zero draft anchor still wins when it is valid, so callers keep their
        // own "is there a lock at all" gate rather than getting a silent fallback.
        if (gui_pending_lock_target_mhz(true, true, 0, 2482, false, 0, 0) != 0) return 1557;
    }

    // A lock that TRACKS its anchor is not an absolute target: capture adds the
    // anchor's GPU-offset component delta (pending minus applied), so a freshly
    // ticked flatten lock with +475 MHz previews the tail at stock+475. The
    // regression: the preview used the raw draft anchor (2482) while Apply
    // wrote 2957, so the graph showed the tail at stock until an Apply.
    {
        if (gui_pending_lock_target_mhz(true, true, 2482, 2482, true, 0, 475) != 2957) return 3208;
        // No attached draft: lockedFreq is the base and still tracks the delta.
        if (gui_pending_lock_target_mhz(false, true, 2957, 2482, true, 0, 475) != 2957) return 3209;
        // An absolute lock (anchor retyped or persisted absolute) is written
        // as-is even while the global offset moves.
        if (gui_pending_lock_target_mhz(true, true, 2482, 2482, false, 0, 475) != 2482) return 3210;
        // Components equal -> no adjustment, so an offset that did not move
        // cannot double-count into a tracking lock.
        if (gui_pending_lock_target_mhz(true, true, 2482, 2482, true, 475, 475) != 2482) return 3211;
        // A negative delta that sinks the target clamps to 1, mirroring
        // capture's `if (effectiveLockTargetMHz <= 0) effectiveLockTargetMHz = 1;`.
        if (gui_pending_lock_target_mhz(true, true, 100, 100, true, 0, -200) != 1) return 3212;
        // A valid zero draft anchor still wins as the base, then tracks.
        if (gui_pending_lock_target_mhz(true, true, 0, 2482, true, 0, 475) != 475) return 3213;
        // An incremental movement (already-applied 475 -> pending 500) shifts
        // the target by exactly the delta, not by the whole pending offset.
        if (gui_pending_lock_target_mhz(true, true, 2482, 2482, true, 475, 500) != 2507) return 3214;
    }

    // The VF graph's frequency axis is derived from the plotted data instead
    // of a fixed 500..3400 MHz band.  The regression: with a high pending or
    // applied clock (e.g. 3450 MHz from a large GPU offset) the fixed ceiling
    // clamped the curve flat against the top of the graph.
    {
        GuiGraphAxisRange range = gui_graph_frequency_axis(0, 0, 500, 500, 1000, 200);
        if (range.minMHz != 500 || range.maxMHz != 1500) return 3215;
        // High clocks: ceiling rounds 3650 up to the next 500 boundary, floor
        // rounds 1600 down to 1500, and nothing touches the top edge.
        range = gui_graph_frequency_axis(1800, 3450, 500, 500, 1000, 200);
        if (range.minMHz != 1500 || range.maxMHz != 4000) return 3216;
        // Minimum span: a tight 2000..2100 curve still gets a 1000 MHz axis.
        range = gui_graph_frequency_axis(2000, 2100, 500, 500, 1000, 200);
        if (range.minMHz != 1500 || range.maxMHz != 2500) return 3217;
        // Floor clamp: data near the hard 500 MHz floor cannot push below it.
        range = gui_graph_frequency_axis(600, 800, 500, 500, 1000, 200);
        if (range.minMHz != 500 || range.maxMHz != 1500) return 3218;
        // A single populated point sizes the axis around itself.
        range = gui_graph_frequency_axis(2500, 2500, 500, 500, 1000, 200);
        if (range.minMHz != 2000 || range.maxMHz != 3000) return 3219;
        // Very high clocks get a matching high axis, not a clamped ceiling.
        range = gui_graph_frequency_axis(5000, 5200, 500, 500, 1000, 200);
        if (range.minMHz != 4500 || range.maxMHz != 5500) return 3220;
        // Low floors are raised so the curve does not float in empty space.
        range = gui_graph_frequency_axis(1400, 3000, 500, 500, 1000, 200);
        if (range.minMHz != 1000 || range.maxMHz != 3500) return 3221;
    }

    // The voltage axis starts at the first visible VF point's voltage instead
    // of a fixed 700 mV, so a curve that begins at 750 mV does not waste the
    // whole 700-750 mV cell on the left.
    {
        GuiGraphVoltageRange range = gui_graph_voltage_axis(0, 0, 700, 1250, 50, 300, 10);
        if (range.minMv != 700 || range.maxMv != 1250) return 3222;
        // Normal visible curve: floor rounds 750 down, ceiling rounds 1245 up.
        range = gui_graph_voltage_axis(760, 1235, 700, 1250, 50, 300, 10);
        if (range.minMv != 750 || range.maxMv != 1250) return 3223;
        // Low end sits at the hard floor; the right edge follows the data.
        range = gui_graph_voltage_axis(700, 1100, 700, 1250, 50, 300, 10);
        if (range.minMv != 700 || range.maxMv != 1150) return 3224;
        // Minimum span expands downward first when the curve is tight.
        range = gui_graph_voltage_axis(750, 760, 700, 1250, 50, 300, 10);
        if (range.minMv != 700 || range.maxMv != 1000) return 3225;
        // A high-voltage-only curve expands down rather than off the 1250 cap.
        range = gui_graph_voltage_axis(1180, 1200, 700, 1250, 50, 300, 10);
        if (range.minMv != 950 || range.maxMv != 1250) return 3226;
        // The 1250 mV cap is never exceeded.
        range = gui_graph_voltage_axis(1240, 1245, 700, 1250, 50, 300, 10);
        if (range.minMv != 950 || range.maxMv != 1250) return 3227;

        // The axis helpers round on the grid with true floor/ceil semantics
        // instead of C++ truncating division, so a negative pre-clamp value
        // cannot step the wrong way once a caller's clamp lets it through.
        if (gui_grid_floor(350, 500) != 0) return 4020;
        if (gui_grid_floor(-350, 500) != -500) return 4020;
        if (gui_grid_floor(-500, 500) != -500) return 4020;
        if (gui_grid_ceil(350, 500) != 500) return 4020;
        if (gui_grid_ceil(-350, 500) != 0) return 4020;
        if (gui_grid_ceil(-500, 500) != -500) return 4020;
        if (gui_grid_floor(1255, 50) != 1250 ||
            gui_grid_ceil(1255, 50) != 1300) return 4021;
        // A negative pre-clamp floor stays the next grid boundary DOWN, not
        // zero, when the caller's minimum allows it.
        GuiGraphAxisRange freqRange =
            gui_graph_frequency_axis(100, 3000, -1000, 500, 1000, 200);
        if (freqRange.minMHz != -500 || freqRange.maxMHz != 3500) return 4022;
        GuiGraphVoltageRange voltRange =
            gui_graph_voltage_axis(30, 1200, -1000, 1250, 50, 300, 50);
        if (voltRange.minMv != -50 || voltRange.maxMv != 1250) return 4023;
    }

    // The voltage lock is not in GuiDraft, and moving it rewrites the whole
    // tail on Apply, so it is compared as its own domain.
    {
        GuiPendingLock lock = {};
        if (gui_pending_lock_changed(lock)) return 1496;

        lock.draftActive = true;
        lock.draftCi = 75;
        lock.draftMHz = 2800;
        lock.draftMode = 1;
        if (!gui_pending_lock_changed(lock)) return 1497;   // lock added

        lock.appliedActive = true;
        lock.appliedCi = 75;
        lock.appliedMHz = 2800;
        lock.appliedMode = 1;
        if (gui_pending_lock_changed(lock)) return 1498;    // unchanged

        lock.draftActive = false;
        if (!gui_pending_lock_changed(lock)) return 1499;   // lock removed
        lock.draftActive = true;

        lock.draftCi = 76;
        if (!gui_pending_lock_changed(lock)) return 1500;   // moved
        lock.draftCi = 75;

        lock.draftMHz = 2810;
        if (!gui_pending_lock_changed(lock)) return 1501;   // retargeted
        lock.draftMHz = 2800;

        lock.draftMode = 2;
        if (!gui_pending_lock_changed(lock)) return 1502;   // FLATTEN -> HARD
        lock.draftMode = 1;
        if (gui_pending_lock_changed(lock)) return 1503;

        // Two inactive locks are equal no matter what the stale fields hold.
        lock.draftActive = false;
        lock.appliedActive = false;
        lock.draftCi = 3;
        lock.draftMHz = 999;
        lock.draftMode = 2;
        if (gui_pending_lock_changed(lock)) return 1504;
    }

    // The GPU only represents frequencies on its own VF grid, so a requested
    // lock target and the value the hardware then reports for it differ by up to
    // one step: a profile asking for 2957 MHz lands on 2962 and stays there. The
    // apply path already treats that as on target and keeps the requested value.
    // Comparing exactly here left the editor permanently dirty -- re-loading the
    // very profile that is applied previewed a tail change forever, because
    // re-applying can never close a gap the hardware cannot represent.
    {
        GuiPendingLock lock = {};
        lock.draftActive = true;
        lock.appliedActive = true;
        lock.draftCi = 76;
        lock.appliedCi = 76;
        lock.draftMode = 1;
        lock.appliedMode = 1;
        lock.draftMHz = 2957;
        lock.appliedMHz = 2962;
        lock.toleranceMHz = 8;
        if (gui_pending_lock_changed(lock)) return 1580;
        // Symmetric: which side is higher must not matter.
        lock.draftMHz = 2962;
        lock.appliedMHz = 2957;
        if (gui_pending_lock_changed(lock)) return 1581;

        // Exactly at the tolerance is still on target; one beyond is a change.
        lock.draftMHz = 2957;
        lock.appliedMHz = 2965;
        if (gui_pending_lock_changed(lock)) return 1582;
        lock.appliedMHz = 2966;
        if (!gui_pending_lock_changed(lock)) return 1583;

        // A zero tolerance keeps the old exact behaviour, which is what an
        // unpopulated point reports.
        lock.appliedMHz = 2958;
        lock.toleranceMHz = 0;
        if (!gui_pending_lock_changed(lock)) return 1584;
        lock.appliedMHz = 2957;
        if (gui_pending_lock_changed(lock)) return 1585;

        // Tolerance applies ONLY to the frequency. A moved anchor or a changed
        // mode is a real change however close the frequencies are.
        lock.toleranceMHz = 8;
        lock.appliedMHz = 2962;
        lock.draftCi = 75;
        if (!gui_pending_lock_changed(lock)) return 1586;
        lock.draftCi = 76;
        lock.draftMode = 2;
        if (!gui_pending_lock_changed(lock)) return 1587;
        lock.draftMode = 1;
        if (gui_pending_lock_changed(lock)) return 1588;

        // Adding or removing a lock is never within tolerance.
        lock.appliedActive = false;
        if (!gui_pending_lock_changed(lock)) return 1589;
    }

    // The summary is what the Apply button and the colours both read.
    {
        GuiPendingSummary summary = {};
        if (gui_pending_any(&summary)) return 1506;
        if (gui_pending_any(nullptr)) return 1507;
        if (gui_pending_apply_button_enabled(true, &summary)) return 1508;

        // Every domain on its own is enough to make Apply reachable.
        const unsigned int domains[] = {
            GUI_PENDING_CURVE, GUI_PENDING_LOCK, GUI_PENDING_GPU_OFFSET,
            GUI_PENDING_GPU_EXCLUDE, GUI_PENDING_MEM_OFFSET,
            GUI_PENDING_POWER_LIMIT, GUI_PENDING_FAN_MODE,
            GUI_PENDING_FAN_FIXED, GUI_PENDING_FAN_CURVE,
        };
        for (unsigned int domain : domains) {
            GuiPendingSummary one = {};
            one.domainMask = domain;
            if (!gui_pending_any(&one)) return 1509;
            if (!gui_pending_apply_button_enabled(true, &one)) return 1510;
            // Pending changes only ever REMOVE the button; they never override
            // the service/draft readiness gate.
            if (gui_pending_apply_button_enabled(false, &one)) return 1511;
            if (!gui_pending_domain_set(&one, domain)) return 1512;
            if (gui_pending_domain_set(&one, ~domain)) return 1513;
        }

        // The bits are distinct, so one changed domain cannot mask another.
        unsigned int seen = 0;
        for (unsigned int domain : domains) {
            if (seen & domain) return 1514;
            seen |= domain;
        }

        // Repaint gating: identical summaries must compare equal so a stable
        // telemetry tick invalidates nothing.
        GuiPendingSummary a = {};
        a.domainMask = GUI_PENDING_CURVE;
        a.changedPointCount = 4;
        GuiPendingSummary b = a;
        if (!gui_pending_summary_equal(&a, &b)) return 1515;
        b.changedPointCount = 5;
        if (gui_pending_summary_equal(&a, &b)) return 1516;
        b = a;
        b.domainMask |= GUI_PENDING_LOCK;
        if (gui_pending_summary_equal(&a, &b)) return 1517;
    }

    // With no background service installed the editor was correctly dead while
    // the whole profile row still read as live -- and two of those controls did
    // not merely look wrong: Save would have persisted an all-zero control state
    // as a profile, and Load wrote an editor that is disabled and blank. The line
    // is capability, not visual consistency: grey out what cannot work, keep what
    // still works and every escape hatch.
    {
        // The eight states that matter, in the order they occur in practice.
        struct ActionabilityCase { const char* name; GuiServiceActionability in; };
        ActionabilityCase cases[] = {
            // installed, available, toggleInFlight, ready, attached, detached, loaded
            { "not installed",        { false, false, false, false, false, false, false } },
            { "installed, stopped",   { true,  false, false, false, false, false, false } },
            { "toggle in flight",     { true,  true,  true,  false, false, false, false } },
            { "available, syncing",   { true,  true,  false, false, true,  false, false } },
            { "ready, attached",      { true,  true,  false, true,  true,  false, true  } },
            { "ready, detached",      { true,  true,  false, true,  true,  true,  true  } },
            { "ready, unattached",    { true,  true,  false, true,  false, false, true  } },
            { "ready, not loaded",    { true,  true,  false, true,  true,  false, false } },
        };

        for (const ActionabilityCase& item : cases) {
            const GuiServiceActionability* in = &item.in;

            // The escape-hatch invariant: Refresh, the service-install checkbox
            // and License must never be unreachable, or an unavailable service
            // becomes a state with no way out. Likewise the purely local config
            // actions -- notably the logon combo, which is the ONLY GUI path that
            // can change logon_shared_slot, a setting clear_profile_from_config()
            // deliberately preserves.
            if (!gui_service_capability_enabled(in, GUI_SERVICE_CAP_RECOVERY))
                return 1600;

            bool reachable = in->installed && in->available && !in->toggleInFlight;

            // Rule 2 and its exemption: a control that persists a
            // service-dependent setting is greyed at its default and enabled
            // while set, so the setting can always be cleared. Without this a
            // logon assignment would keep a Task Scheduler entry registered with
            // no GUI path to remove it -- clearing the profile does not help,
            // because clear_profile_from_config() preserves logon_shared_slot.
            if (gui_service_config_control_actionable(in, false) != reachable)
                return 1601;
            if (!gui_service_config_control_actionable(in, true)) return 1630;
            if (gui_service_capability_enabled(in, GUI_SERVICE_CAP_PROFILE_EDIT)
                    != reachable) return 1602;
            if (gui_service_capability_enabled(in, GUI_SERVICE_CAP_AUTOMATION)
                    != reachable) return 1603;

            bool editor = in->ready && in->draftAttached && !in->draftDetached;
            if (gui_service_capability_enabled(in, GUI_SERVICE_CAP_EDITOR)
                    != editor) return 1604;
            if (gui_service_capability_enabled(in, GUI_SERVICE_CAP_HARDWARE_MUTATION)
                    != (editor && in->loaded)) return 1605;

            // Applying is strictly narrower than editing, in every state.
            if (gui_service_capability_enabled(in, GUI_SERVICE_CAP_HARDWARE_MUTATION) &&
                    !gui_service_capability_enabled(in, GUI_SERVICE_CAP_EDITOR))
                return 1606;

            // The two helpers the imperative code calls directly must agree with
            // the table.
            if (gui_service_reachable(in) != reachable) return 1607;
            if (gui_service_editor_actionable(in) != editor) return 1608;
        }

        // The reported bug: with nothing installed the whole profile row goes
        // dead -- Load, Save, Clear, the slot combo, the shared picker and
        // Auto-Profiles -- while the escape hatches stay live and an assignment
        // that is already set stays clearable.
        const GuiServiceActionability* notInstalled = &cases[0].in;
        if (gui_service_capability_enabled(notInstalled, GUI_SERVICE_CAP_PROFILE_EDIT))
            return 1609;
        if (gui_service_capability_enabled(notInstalled, GUI_SERVICE_CAP_AUTOMATION))
            return 1610;
        if (!gui_service_capability_enabled(notInstalled, GUI_SERVICE_CAP_RECOVERY))
            return 1611;
        if (gui_service_config_control_actionable(notInstalled, false)) return 1631;
        if (!gui_service_config_control_actionable(notInstalled, true)) return 1632;

        // The deliberate asymmetry: a service that is present but still syncing
        // keeps profile editing available, because a profile loaded then is
        // preserved as a pre-READY overlay and rebased onto the first coherent
        // snapshot. Gating this on readiness would delete that feature.
        const GuiServiceActionability* syncing = &cases[3].in;
        if (!gui_service_capability_enabled(syncing, GUI_SERVICE_CAP_PROFILE_EDIT))
            return 1612;
        if (gui_service_capability_enabled(syncing, GUI_SERVICE_CAP_EDITOR))
            return 1613;

        // An in-flight uninstall must not leave Save pressable while the service
        // is being removed, even though both flags still read true.
        const GuiServiceActionability* inFlight = &cases[2].in;
        if (gui_service_capability_enabled(inFlight, GUI_SERVICE_CAP_PROFILE_EDIT))
            return 1614;

        // Null input denies everything rather than crashing a projection pass.
        if (gui_service_capability_enabled(nullptr, GUI_SERVICE_CAP_RECOVERY))
            return 1615;
        if (gui_service_reachable(nullptr)) return 1616;
        if (gui_service_editor_actionable(nullptr)) return 1617;
        // No out-of-range capability case: loading a value outside the enum is
        // itself undefined behaviour and the sanitizer build rejects it. The
        // switch keeps its `default: return false;` as defence for a future
        // member added without updating this table, which cannot be tested here.
    }

    // The profile row is re-gated whenever the first envelope after a disconnect
    // forces a full render. That chain rests on a non-obvious property:
    // gui_service_model_disconnect() ZEROES topologySignature, so the next
    // envelope always looks like a topology change and can never be treated as
    // stable telemetry. Pin it, because the re-gate coverage depends on it.
    {
        GuiServiceModel model = {};
        gui_service_model_initialize(&model);
        model.topologySignature = 0x1234567890ABCDEFULL;
        model.hasAcceptedEnvelope = true;
        model.phase = GUI_SERVICE_READY;
        gui_service_model_disconnect(&model, 7);
        if (model.topologySignature != 0) return 1620;
        if (model.phase != GUI_SERVICE_DISCONNECTED) return 1621;
        if (gui_service_model_ready(&model)) return 1622;
    }

    // The tray tooltip names the profile actually APPLIED to the GPU. Loading a
    // profile deliberately does not touch hardware, so reporting the combo's
    // selection made the tray claim "Profile 1" while slot 2 was running.
    {
        char label[64] = {};
        // applied_slot wins outright; it is what every apply path writes.
        gui_tray_format_active_profile(label, sizeof(label), 2,
            GUI_TRAY_PROFILE_USER_SLOT, 2, 5);
        if (strcmp(label, "Profile 2") != 0) return 1560;

        // Shared and machine banks are separate banks with separate contents and
        // must never be folded into the same-numbered personal slot.
        gui_tray_format_active_profile(label, sizeof(label), 0,
            GUI_TRAY_PROFILE_SHARED_SLOT, 3, 5);
        if (strcmp(label, "Shared profile 3") != 0) return 1561;
        gui_tray_format_active_profile(label, sizeof(label), 0,
            GUI_TRAY_PROFILE_MACHINE_SLOT, 1, 5);
        if (strcmp(label, "Machine profile 1") != 0) return 1562;

        // A hand-typed Apply owns the GPU without belonging to any slot; so does
        // a user slot whose saved intent was edited away from what is running,
        // which is exactly when applied_slot gets cleared to 0.
        gui_tray_format_active_profile(label, sizeof(label), 0,
            GUI_TRAY_PROFILE_AD_HOC, 0, 5);
        if (strcmp(label, "Manual settings") != 0) return 1563;
        gui_tray_format_active_profile(label, sizeof(label), 0,
            GUI_TRAY_PROFILE_USER_SLOT, 2, 5);
        if (strcmp(label, "Manual settings") != 0) return 1564;

        // Nothing active at all.
        gui_tray_format_active_profile(label, sizeof(label), 0,
            GUI_TRAY_PROFILE_NONE, 0, 5);
        if (strcmp(label, "No profile") != 0) return 1565;

        // Out-of-range slots never fabricate a profile name.
        gui_tray_format_active_profile(label, sizeof(label), 9,
            GUI_TRAY_PROFILE_NONE, 0, 5);
        if (strcmp(label, "No profile") != 0) return 1566;
        gui_tray_format_active_profile(label, sizeof(label), -1,
            GUI_TRAY_PROFILE_NONE, 0, 5);
        if (strcmp(label, "No profile") != 0) return 1567;
        gui_tray_format_active_profile(label, sizeof(label), 0,
            GUI_TRAY_PROFILE_SHARED_SLOT, 6, 5);
        if (strcmp(label, "No profile") != 0) return 1568;
        gui_tray_format_active_profile(label, sizeof(label), 0,
            GUI_TRAY_PROFILE_SHARED_SLOT, 0, 5);
        if (strcmp(label, "No profile") != 0) return 1569;

        // A tiny buffer truncates but stays NUL-terminated, and a null one is
        // simply ignored rather than crashing the tray update.
        char tiny[4] = {};
        gui_tray_format_active_profile(tiny, sizeof(tiny), 2,
            GUI_TRAY_PROFILE_USER_SLOT, 2, 5);
        if (tiny[sizeof(tiny) - 1] != '\0') return 1570;
        gui_tray_format_active_profile(nullptr, 0, 2,
            GUI_TRAY_PROFILE_USER_SLOT, 2, 5);
    }

    // A change to the GLOBAL GPU offset moves points nobody typed. Two profiles
    // that own no curve points at all and differ only in gpu_offset_mhz (475
    // with exclude_low_count=70 vs 0) looked like a no-op on the curve, even
    // though every non-excluded point was about to drop by 475 MHz.
    {
        GuiPendingOffsetShift shift = {};
        shift.appliedOffsetComponentMHz = 475;
        shift.pendingOffsetComponentMHz = 0;
        if (!gui_pending_offset_shift_changed(shift)) return 1540;

        // ... and in the other direction.
        shift.appliedOffsetComponentMHz = 0;
        shift.pendingOffsetComponentMHz = 475;
        if (!gui_pending_offset_shift_changed(shift)) return 1541;

        // An excluded point has component 0 on both sides: the offset changed,
        // but this point does not move, so it must not light up.
        shift.appliedOffsetComponentMHz = 0;
        shift.pendingOffsetComponentMHz = 0;
        if (gui_pending_offset_shift_changed(shift)) return 1542;

        // Same non-zero component on both sides (e.g. only exclude_low_count
        // moved, past this point) is likewise no movement.
        shift.appliedOffsetComponentMHz = 475;
        shift.pendingOffsetComponentMHz = 475;
        if (gui_pending_offset_shift_changed(shift)) return 1543;

        // The two carve-outs, both matching gpu_backend_apply.cpp: under a
        // UNIFORM offset an explicitly owned point is written as an absolute
        // target so the global offset does not stack onto it, and the locked
        // tail is pinned to the lock target whatever the offset does.
        shift.appliedOffsetComponentMHz = 475;
        shift.pendingOffsetComponentMHz = 0;
        shift.ownedByEditor = true;
        if (gui_pending_offset_shift_changed(shift)) return 1544;
        shift.ownedByEditor = false;
        shift.inLockedTail = true;
        if (gui_pending_offset_shift_changed(shift)) return 1545;
        // The tail rule wins even for a point that is also owned.
        shift.ownedByEditor = true;
        if (gui_pending_offset_shift_changed(shift)) return 1546;
        shift.inLockedTail = false;
        shift.ownedByEditor = false;
        if (!gui_pending_offset_shift_changed(shift)) return 1547;

        // A SELECTIVE offset (either side excludes low VF points) is applied
        // through per-point VF deltas in the curve batch, which re-places every
        // populated point regardless of guiCurvePointExplicit.  An applied
        // +475/exclude-70 profile decodes into owned absolute curve points, so
        // retyping the offset or the exclude count must still preview the move.
        shift.ownedByEditor = true;
        shift.selectiveOffsetActive = true;
        shift.appliedOffsetComponentMHz = 475;
        shift.pendingOffsetComponentMHz = 200;
        if (!gui_pending_offset_shift_changed(shift)) return 4000;
        // Lowering the exclude count moves previously excluded owned points
        // into the offset region (0 -> +475).
        shift.appliedOffsetComponentMHz = 0;
        shift.pendingOffsetComponentMHz = 475;
        if (!gui_pending_offset_shift_changed(shift)) return 4001;
        // Same component on both sides still does not move this point.
        shift.appliedOffsetComponentMHz = 475;
        shift.pendingOffsetComponentMHz = 475;
        if (gui_pending_offset_shift_changed(shift)) return 4002;
        // The locked tail outranks the selective offset too.
        shift.inLockedTail = true;
        shift.pendingOffsetComponentMHz = 200;
        if (gui_pending_offset_shift_changed(shift)) return 4003;
        shift.inLockedTail = false;
        shift.ownedByEditor = false;
        shift.selectiveOffsetActive = false;
        if (!gui_pending_offset_shift_changed(shift)) return 4004;

        // The selective/curve-batch offset mode is a pure mirror of
        // gpu_backend_apply.cpp's gpuPolicyViaCurveBatch gate.  The applied
        // side is the resolver's effective exclude count (already gated on a
        // nonzero offset at its sources), so it is NOT re-gated here; the
        // pending side normalizes like the backend's desired settings
        // (exclude counts only while the offset is nonzero).
        if (gui_pending_offset_mode_selective(0, 0, 0, 0)) return 4010;
        if (gui_pending_offset_mode_selective(475, 0, 500, 0)) return 4011;
        if (!gui_pending_offset_mode_selective(475, 70, 475, 70)) return 4012;
        if (!gui_pending_offset_mode_selective(0, 70, 0, 0)) return 4013;
        if (!gui_pending_offset_mode_selective(475, 70, 0, 0)) return 4014;
        if (gui_pending_offset_mode_selective(475, 0, 0, 70)) return 4015;
        if (!gui_pending_offset_mode_selective(0, 0, 475, 70)) return 4016;

        // Negative offsets are ordinary values, not sentinels.
        shift.appliedOffsetComponentMHz = 0;
        shift.pendingOffsetComponentMHz = -200;
        if (!gui_pending_offset_shift_changed(shift)) return 1548;
        shift.appliedOffsetComponentMHz = -200;
        if (gui_pending_offset_shift_changed(shift)) return 1549;
    }

    // The graph preview record: the EDITOR half of every plotted point. The
    // graph materialises it against live readback, and the repaint gate compares
    // it, so the same value decides what is drawn and when it is drawn.
    //
    // Resolution order mirrors gpu_backend_apply.cpp: locked tail, then a
    // moved global-offset component on an unowned point, then the draft value.
    {
        GuiGraphPreviewInput in = {};

        // Nothing the editor determines: live readback stands.
        GuiGraphPreviewPoint out = gui_graph_preview_point(in);
        if (out.absoluteMHz != 0 || out.offsetFromStockBase) return 3195;

        // Draft value wins over live readback.
        in.draftValid = true;
        in.draftMHz = 2800;
        out = gui_graph_preview_point(in);
        if (out.absoluteMHz != 2800 || out.offsetFromStockBase) return 3196;

        // The locked tail is pinned to the lock target, NOT to the previous
        // readback the disabled tail edit controls keep showing.
        in.inLockedTail = true;
        in.lockTargetMHz = 2950;
        out = gui_graph_preview_point(in);
        if (out.absoluteMHz != 2950 || out.offsetFromStockBase) return 3197;

        // An unresolved lock (target 0) is not a request to plot 0 MHz; the
        // draft still answers for the point.
        in.lockTargetMHz = 0;
        out = gui_graph_preview_point(in);
        if (out.absoluteMHz != 2800 || out.offsetFromStockBase) return 3198;
        in.inLockedTail = false;

        // A point whose global-offset component moved is projected from stock,
        // because its live readback still carries the APPLIED component.
        in.offsetProjectionValid = true;
        in.appliedOffsetComponentMHz = 0;
        in.pendingOffsetComponentMHz = 475;
        out = gui_graph_preview_point(in);
        if (!out.offsetFromStockBase || out.offsetComponentMHz != 475 ||
            out.absoluteMHz != 0) return 3199;

        // ... but not one the editor explicitly owns under a UNIFORM offset:
        // the offset does not stack onto an absolute target, so the draft
        // value stands.
        in.ownedByEditor = true;
        out = gui_graph_preview_point(in);
        if (out.absoluteMHz != 2800 || out.offsetFromStockBase) return 3200;
        // A SELECTIVE offset re-places owned points too, so the offset
        // projection outranks the draft exactly like the curve batch does.
        in.offsetMovesOwnedPoints = true;
        out = gui_graph_preview_point(in);
        if (!out.offsetFromStockBase || out.offsetComponentMHz != 475 ||
            out.absoluteMHz != 0) return 4005;
        in.offsetMovesOwnedPoints = false;
        in.ownedByEditor = false;

        // ... and the locked tail outranks the offset entirely.
        in.inLockedTail = true;
        in.lockTargetMHz = 2950;
        out = gui_graph_preview_point(in);
        if (out.absoluteMHz != 2950 || out.offsetFromStockBase) return 3201;
        in.inLockedTail = false;
        in.lockTargetMHz = 0;

        // A hard NVML pin flattens the ENTIRE curve, not just the tail.  The
        // preview must answer with the lock target even for a pre-anchor point
        // whose draft or offset component would otherwise win.
        in = {};
        in.hardPinned = true;
        in.lockTargetMHz = 2950;
        out = gui_graph_preview_point(in);
        if (out.absoluteMHz != 2950 || out.offsetFromStockBase) return 4006;
        in.draftValid = true;
        in.draftMHz = 2800;
        out = gui_graph_preview_point(in);
        if (out.absoluteMHz != 2950 || out.offsetFromStockBase) return 4007;
        in.offsetProjectionValid = true;
        in.pendingOffsetComponentMHz = 475;
        out = gui_graph_preview_point(in);
        if (out.absoluteMHz != 2950 || out.offsetFromStockBase) return 4008;
        // Without the pin, the draft answers again.
        in.hardPinned = false;
        in.offsetProjectionValid = false;
        in.pendingOffsetComponentMHz = 0;
        out = gui_graph_preview_point(in);
        if (out.absoluteMHz != 2800 || out.offsetFromStockBase) return 4009;

        // Component unchanged -> the point does not move, so the editor does
        // not claim it and live readback stands (via the draft snapshot).
        in.appliedOffsetComponentMHz = 475;
        if (gui_graph_preview_point(in).offsetFromStockBase) return 3202;

        // A point the applied state owns but the editor releases is projected
        // from stock even when the stale draft still holds the applied value.
        in = {};
        in.releasedToStock = true;
        in.draftValid = true;
        in.draftMHz = 547;
        out = gui_graph_preview_point(in);
        if (!out.offsetFromStockBase || out.offsetComponentMHz != 0 ||
            out.absoluteMHz != 0) return 4027;
        // An owned point is not released; its draft value stands.
        in.ownedByEditor = true;
        out = gui_graph_preview_point(in);
        if (out.absoluteMHz != 547 || out.offsetFromStockBase) return 4028;
        // With a valid offset projection the release carries the PENDING
        // component on the stock base (excluded points naturally carry 0).
        in.ownedByEditor = false;
        in.offsetProjectionValid = true;
        in.pendingOffsetComponentMHz = 475;
        out = gui_graph_preview_point(in);
        if (!out.offsetFromStockBase || out.offsetComponentMHz != 475 ||
            out.absoluteMHz != 0) return 4029;
        // A pending hard pin still outranks the release.
        in.hardPinned = true;
        in.lockTargetMHz = 2950;
        out = gui_graph_preview_point(in);
        if (out.absoluteMHz != 2950 || out.offsetFromStockBase) return 4030;
    }

    // THE regression: the repaint gate must move when the plotted VALUE moves,
    // not only when the set of pending points moves.
    //
    // Retyping the global GPU offset from +100 to +150 shifts every unowned
    // point but marks exactly the same ones, so a gate built on the pending mask
    // and the changed-point set alone saw no difference and never invalidated
    // the graph. The preview only appeared once something else repainted the
    // window -- which is why clicking Refresh "fixed" it, and why the
    // exclude-low field, which DOES change the marked set, looked healthy.
    {
        GuiGraphPreviewInput in = {};
        in.offsetProjectionValid = true;
        in.appliedOffsetComponentMHz = 0;
        in.draftValid = true;
        in.draftMHz = 2800;

        in.pendingOffsetComponentMHz = 100;
        GuiGraphPreviewPoint before = gui_graph_preview_point(in);
        in.pendingOffsetComponentMHz = 150;
        GuiGraphPreviewPoint after = gui_graph_preview_point(in);
        if (gui_graph_preview_point_equal(before, after)) return 3203;
        if (!gui_graph_preview_point_equal(before, before)) return 3204;

        // Same for a per-point edit that keeps an already-pending point pending:
        // 2900 -> 2950 leaves the marked set identical, and used to repaint
        // nothing.
        GuiGraphPreviewInput typed = {};
        typed.draftValid = true;
        typed.draftMHz = 2900;
        before = gui_graph_preview_point(typed);
        typed.draftMHz = 2950;
        after = gui_graph_preview_point(typed);
        if (gui_graph_preview_point_equal(before, after)) return 3205;

        // ... and for a lock retarget over the tail.
        GuiGraphPreviewInput locked = {};
        locked.inLockedTail = true;
        locked.lockTargetMHz = 2900;
        before = gui_graph_preview_point(locked);
        locked.lockTargetMHz = 2950;
        after = gui_graph_preview_point(locked);
        if (gui_graph_preview_point_equal(before, after)) return 3206;

        // Equality must not read the padding between offsetFromStockBase and
        // offsetComponentMHz: two records that agree on every field are equal
        // however they were built.
        GuiGraphPreviewPoint stock = {};
        stock.offsetFromStockBase = true;
        stock.offsetComponentMHz = 475;
        GuiGraphPreviewInput rebuild = {};
        rebuild.offsetProjectionValid = true;
        rebuild.pendingOffsetComponentMHz = 475;
        if (!gui_graph_preview_point_equal(stock,
                gui_graph_preview_point(rebuild))) return 3207;
    }

    // The pending curve is drawn only over the stretches that actually differ.
    // Overlaying the complete pending curve covered the applied one along its
    // whole length, so loading a profile that changes two points looked like a
    // change to all 128 -- and looked "right" only when most points happened to
    // differ, which is why the bug survived the first hardware pass.
    {
        // Nothing changed -> nothing drawn at all.
        bool none[5] = { false, false, false, false, false };
        GuiPendingRun run = gui_pending_next_changed_run(none, 5, 0);
        if (run.valid) return 1520;
        if (run.nextScan != 5) return 1521;

        // One interior point: expanded by one neighbour on each side, so the
        // dashed line leaves and rejoins the applied curve where they are equal.
        bool one[5] = { false, false, true, false, false };
        run = gui_pending_next_changed_run(one, 5, 0);
        if (!run.valid || run.drawFirst != 1 || run.drawLast != 3) return 1522;
        if (run.nextScan != 3) return 1523;
        if (gui_pending_next_changed_run(one, 5, run.nextScan).valid) return 1524;

        // Runs at the array edges cannot expand past the ends.
        bool head[4] = { true, false, false, false };
        run = gui_pending_next_changed_run(head, 4, 0);
        if (!run.valid || run.drawFirst != 0 || run.drawLast != 1) return 1525;
        bool tail[4] = { false, false, false, true };
        run = gui_pending_next_changed_run(tail, 4, 0);
        if (!run.valid || run.drawFirst != 2 || run.drawLast != 3) return 1526;

        // Two separate stretches are reported separately and the scan
        // terminates; a contiguous stretch is reported as ONE run.
        bool two[9] = { false, true, true, false, false, false, true, false, false };
        run = gui_pending_next_changed_run(two, 9, 0);
        if (!run.valid || run.drawFirst != 0 || run.drawLast != 3) return 1527;
        run = gui_pending_next_changed_run(two, 9, run.nextScan);
        if (!run.valid || run.drawFirst != 5 || run.drawLast != 7) return 1528;
        run = gui_pending_next_changed_run(two, 9, run.nextScan);
        if (run.valid) return 1529;

        // Every point changed -> one run spanning everything (the case the old
        // full-length draw was accidentally right about).
        bool all[3] = { true, true, true };
        run = gui_pending_next_changed_run(all, 3, 0);
        if (!run.valid || run.drawFirst != 0 || run.drawLast != 2) return 1530;
        if (gui_pending_next_changed_run(all, 3, run.nextScan).valid) return 1531;

        // Degenerate inputs must not walk off the array or loop forever.
        if (gui_pending_next_changed_run(nullptr, 5, 0).valid) return 1532;
        if (gui_pending_next_changed_run(one, 0, 0).valid) return 1533;
        if (gui_pending_next_changed_run(one, 5, -1).valid) return 1534;
        if (gui_pending_next_changed_run(one, 5, 5).valid) return 1535;
        bool single[1] = { true };
        run = gui_pending_next_changed_run(single, 1, 0);
        if (!run.valid || run.drawFirst != 0 || run.drawLast != 0) return 1536;
        if (run.nextScan != 1) return 1537;
    }

    // Sweep the whole scalar input space: invalid draft text can never resolve
    // to "unchanged", which is what would grey out Apply mid-edit.
    {
        const int values[] = { -5000, -1, 0, 1, 100, 5000 };
        for (int draft : values) {
            for (int applied : values) {
                GuiPendingScalar invalid = {};
                invalid.draftValid = false;
                invalid.draftValue = draft;
                invalid.appliedValue = applied;
                if (!gui_pending_scalar_changed(invalid)) return 1518;

                GuiPendingScalar valid = invalid;
                valid.draftValid = true;
                if (gui_pending_scalar_changed(valid) != (draft != applied))
                    return 1519;
            }
        }
    }

    // A pipe timeout caused by this GUI's known long mutation is expected busy,
    // not service failure. SCM stop/removal and service-process callers still
    // require a real probe/state transition.
    {
        if (!service_health_probe_should_defer(false, true, true, true))
            return 1040;
        if (service_health_probe_should_defer(false, false, true, true))
            return 1041;
        if (service_health_probe_should_defer(false, true, true, false))
            return 1042;
        if (service_health_probe_should_defer(true, true, true, true))
            return 1043;
    }

    {
        LinuxDaemonStateRecord state = {};
        linux_daemon_record_initialize(&state, LINUX_DAEMON_RECORD_ACTIVE,
            nullptr, nullptr, 0x123456789ULL, SERVICE_OPERATION_SUCCEEDED);
        if (!linux_daemon_record_valid(&state) ||
            state.version != LINUX_DAEMON_RECORD_VERSION ||
            state.operationId != 0x123456789ULL ||
            state.operationState != SERVICE_OPERATION_SUCCEEDED) return 1025;
        LinuxDaemonOperationRecord operation = {};
        linux_daemon_operation_initialize(&operation, 99,
            SERVICE_OPERATION_IN_PROGRESS, SERVICE_STATUS_ERROR,
            SERVICE_OUTCOME_SEVERITY_ERROR, "started");
        if (!linux_daemon_operation_valid(&operation) ||
            operation.operationId != 99 ||
            strcmp(operation.message, "started") != 0) return 1032;
        // The persisted record carries the severity a retry will replay, and it
        // goes through the same resolver the wire does.
        if (operation.outcomeSeverity != SERVICE_OUTCOME_SEVERITY_ERROR)
            return 1034;
        LinuxDaemonOperationRecord warned = {};
        linux_daemon_operation_initialize(&warned, 100,
            SERVICE_OPERATION_SUCCEEDED, SERVICE_STATUS_OK,
            SERVICE_OUTCOME_SEVERITY_WARNING, "partial verify");
        if (!linux_daemon_operation_valid(&warned) ||
            warned.outcomeSeverity != SERVICE_OUTCOME_SEVERITY_WARNING)
            return 1035;
        operation.checksum ^= 1u;
        if (linux_daemon_operation_valid(&operation)) return 1033;
    }

    // ------------------------------------------------------------------
    // Installer payload container (1750-1769)
    //
    // The setup program validates the whole container before it writes a
    // single file, so these assertions cover exactly the cases that separate
    // "refuse to install" from "extract something hostile or truncated".
    // ------------------------------------------------------------------
    {
        // The canonical CRC-32 check value; this pins the polynomial and the
        // reflection, which build.py's zlib.crc32 has to agree with.
        if (gc_crc32("123456789", 9, 0) != 0xCBF43926u) return 1750;
        if (gc_crc32("", 0, 0) != 0u) return 1751;

        if (!gc_archive_name_is_safe("greencurve.exe")) return 1752;
        if (!gc_archive_name_is_safe("README.md")) return 1752;
        if (gc_archive_name_is_safe("")) return 1753;
        if (gc_archive_name_is_safe(".")) return 1753;
        if (gc_archive_name_is_safe("..")) return 1753;
        if (gc_archive_name_is_safe("sub\\evil.exe")) return 1754;
        if (gc_archive_name_is_safe("sub/evil.exe")) return 1754;
        if (gc_archive_name_is_safe("C:evil.exe")) return 1754;
        if (gc_archive_name_is_safe("evil*.exe")) return 1755;
        // A trailing dot or space is silently stripped by Win32 path handling,
        // so the extracted name would differ from the verified one.
        if (gc_archive_name_is_safe("evil.exe.")) return 1756;
        if (gc_archive_name_is_safe("evil.exe ")) return 1756;
    }
    {
        // Build a two-file container by hand, exactly as build.py does.
        static unsigned char container[512];
        const char* payloadA = "alpha-bytes";
        const char* payloadB = "beta";
        const size_t sizeA = strlen(payloadA);
        const size_t sizeB = strlen(payloadB);
        GcArchiveHeader header = {};
        for (int i = 0; i < GC_ARCHIVE_MAGIC_LEN; ++i)
            header.magic[i] = GC_ARCHIVE_MAGIC[i];
        header.fileCount = 2;
        const uint64_t dataStart = sizeof(GcArchiveHeader) + 2 * sizeof(GcArchiveEntry);
        GcArchiveEntry entries[2] = {};
        snprintf(entries[0].name, sizeof(entries[0].name), "%s", "greencurve.exe");
        entries[0].dataOffset = dataStart;
        entries[0].dataSize = sizeA;
        entries[0].dataCrc32 = gc_crc32(payloadA, sizeA, 0);
        snprintf(entries[1].name, sizeof(entries[1].name), "%s", "uninstall.exe");
        entries[1].dataOffset = dataStart + sizeA;
        entries[1].dataSize = sizeB;
        entries[1].dataCrc32 = gc_crc32(payloadB, sizeB, 0);
        entries[1].flags = GC_ARCHIVE_FLAG_UNINSTALLER;
        const uint64_t total = dataStart + sizeA + sizeB;
        memcpy(container, &header, sizeof(header));
        memcpy(container + sizeof(header), entries, sizeof(entries));
        for (size_t i = 0; i < sizeA; ++i)
            container[dataStart + i] = (unsigned char)payloadA[i];
        for (size_t i = 0; i < sizeB; ++i)
            container[dataStart + sizeA + i] = (unsigned char)payloadB[i];

        const GcArchiveEntry* parsed = nullptr;
        uint32_t count = 0;
        if (gc_archive_validate(container, total, &parsed, &count) != GC_ARCHIVE_OK) return 1757;
        if (count != 2 || !parsed) return 1758;
        if (strcmp(parsed[0].name, "greencurve.exe") != 0) return 1759;
        if (parsed[1].flags != GC_ARCHIVE_FLAG_UNINSTALLER) return 1759;

        // A flipped payload byte must be caught, because the alternative is
        // writing a corrupted service binary into Program Files.
        container[dataStart] ^= 0x01;
        if (gc_archive_validate(container, total, nullptr, nullptr) != GC_ARCHIVE_ERR_CRC) return 1760;
        container[dataStart] ^= 0x01;

        // An entry pointing back into the directory (or past the end) is the
        // classic malformed-archive read primitive.
        GcArchiveEntry overlapping = entries[0];
        overlapping.dataOffset = sizeof(GcArchiveHeader);
        memcpy(container + sizeof(header), &overlapping, sizeof(overlapping));
        if (gc_archive_validate(container, total, nullptr, nullptr) != GC_ARCHIVE_ERR_RANGE) return 1761;
        GcArchiveEntry past = entries[0];
        past.dataOffset = total;
        past.dataSize = 16;
        memcpy(container + sizeof(header), &past, sizeof(past));
        if (gc_archive_validate(container, total, nullptr, nullptr) != GC_ARCHIVE_ERR_RANGE) return 1762;

        // Duplicate names make extraction order decide what lands on disk.
        GcArchiveEntry duplicate = entries[1];
        snprintf(duplicate.name, sizeof(duplicate.name), "%s", "GREENCURVE.EXE");
        memcpy(container + sizeof(header), entries, sizeof(entries));
        memcpy(container + sizeof(header) + sizeof(GcArchiveEntry), &duplicate, sizeof(duplicate));
        if (gc_archive_validate(container, total, nullptr, nullptr) != GC_ARCHIVE_ERR_NAME) return 1763;
        memcpy(container + sizeof(header), entries, sizeof(entries));

        if (gc_archive_validate(container, sizeof(GcArchiveHeader), nullptr, nullptr) !=
            GC_ARCHIVE_ERR_TRUNCATED) return 1764;
        GcArchiveHeader emptyHeader = header;
        emptyHeader.fileCount = 0;
        memcpy(container, &emptyHeader, sizeof(emptyHeader));
        if (gc_archive_validate(container, total, nullptr, nullptr) != GC_ARCHIVE_ERR_COUNT) return 1765;
        emptyHeader.fileCount = GC_ARCHIVE_MAX_FILES + 1;
        memcpy(container, &emptyHeader, sizeof(emptyHeader));
        if (gc_archive_validate(container, total, nullptr, nullptr) != GC_ARCHIVE_ERR_COUNT) return 1766;
        memcpy(container, &header, sizeof(header));
        GcArchiveHeader wrongMagic = header;
        wrongMagic.magic[0] = 'X';
        memcpy(container, &wrongMagic, sizeof(wrongMagic));
        if (gc_archive_validate(container, total, nullptr, nullptr) != GC_ARCHIVE_ERR_MAGIC) return 1767;
    }
    {
        // Footer validation: the file layout must be exactly
        // stub || blob || footer, with nothing unaccounted for.
        const uint64_t stubSize = 4096;
        const uint64_t blobSize = 1024;
        const uint64_t fileSize = stubSize + blobSize + sizeof(GcPayloadFooter);
        GcPayloadFooter footer = {};
        memcpy(footer.magic, GC_PAYLOAD_FOOTER_MAGIC, GC_PAYLOAD_FOOTER_MAGIC_LEN);
        footer.method = GC_PAYLOAD_METHOD_XPRESS_HUFF;
        footer.archiveOffset = stubSize;
        footer.compressedSize = blobSize;
        footer.uncompressedSize = 4096;
        footer.archiveCrc32 = 0x12345678u;
        footer.footerCrc32 = gc_payload_footer_expected_crc(&footer);
        if (gc_payload_validate_footer(&footer, fileSize, 1u << 20) != GC_PAYLOAD_OK) return 1768;

        GcPayloadFooter damaged = footer;
        damaged.magic[3] = 'Z';
        if (gc_payload_validate_footer(&damaged, fileSize, 1u << 20) != GC_PAYLOAD_ERR_NO_FOOTER) return 1769;
        damaged = footer;
        damaged.uncompressedSize ^= 0x40u;  // CRC not recomputed
        if (gc_payload_validate_footer(&damaged, fileSize, 1u << 20) != GC_PAYLOAD_ERR_FOOTER_CRC) return 1770;
        damaged = footer;
        damaged.method = 99;
        damaged.footerCrc32 = gc_payload_footer_expected_crc(&damaged);
        if (gc_payload_validate_footer(&damaged, fileSize, 1u << 20) != GC_PAYLOAD_ERR_METHOD) return 1771;
        damaged = footer;
        damaged.compressedSize = blobSize - 1;  // leaves an unexplained gap
        damaged.footerCrc32 = gc_payload_footer_expected_crc(&damaged);
        if (gc_payload_validate_footer(&damaged, fileSize, 1u << 20) != GC_PAYLOAD_ERR_RANGE) return 1772;
        damaged = footer;
        damaged.archiveOffset = 0;  // no stub at all
        damaged.compressedSize = fileSize - sizeof(GcPayloadFooter);
        damaged.footerCrc32 = gc_payload_footer_expected_crc(&damaged);
        if (gc_payload_validate_footer(&damaged, fileSize, 1u << 20) != GC_PAYLOAD_ERR_RANGE) return 1773;
        damaged = footer;
        damaged.footerCrc32 = gc_payload_footer_expected_crc(&damaged);
        // A corrupt size must be refused BEFORE it is used as an allocation.
        if (gc_payload_validate_footer(&footer, fileSize, 1024) != GC_PAYLOAD_ERR_SIZE) return 1774;
        damaged = footer;
        damaged.method = GC_PAYLOAD_METHOD_STORE;  // stored payloads cannot shrink
        damaged.footerCrc32 = gc_payload_footer_expected_crc(&damaged);
        if (gc_payload_validate_footer(&damaged, fileSize, 1u << 20) != GC_PAYLOAD_ERR_SIZE) return 1775;
    }

    // ------------------------------------------------------------------
    // Upgrade settings transfer: the apply preconditions (1801-1804)
    //
    // The first real upgrade restore failed with "Service request contains
    // invalid protocol fields" because the transfer applied without first
    // fetching a READY envelope, so the instance id, GPU generation, topology
    // signature, and target adapter all went out as zero. These assertions pin
    // the contract that made it invalid, so a future caller that skips the
    // preamble fails here instead of on a user's machine.
    // ------------------------------------------------------------------
    {
        ServiceRequest request = {};
        request.magic = SERVICE_PROTOCOL_MAGIC;
        request.version = SERVICE_PROTOCOL_VERSION;
        request.command = SERVICE_CMD_APPLY;
        request.callerPid = 4321;
        request.operationId = 99;
        request.applyOrigin = SERVICE_APPLY_ORIGIN_CLI;
        request.profileSource = SERVICE_PROFILE_SOURCE_AD_HOC;
        request.desired.hasPowerLimit = true;
        request.desired.powerLimitPct = 100;

        // Exactly the request the broken transfer sent: every precondition zero.
        if (validate_service_request_for_ipc(&request)) return 1801;

        request.expectedServiceInstanceId = 1234;
        request.expectedGpuGeneration = 7;
        if (validate_service_request_for_ipc(&request)) return 1802;  // still no target GPU

        request.targetGpu.valid = true;
        if (!validate_service_request_for_ipc(&request)) return 1803;

        // A VF-domain apply additionally needs the topology signature, which is
        // the case the restore actually hits (it carries curve points).
        request.desired.hasCurvePoint[10] = true;
        request.desired.curvePointMHz[10] = 1500;
        if (validate_service_request_for_ipc(&request)) return 1804;
        request.expectedTopologySignature = 55;
        if (!validate_service_request_for_ipc(&request)) return 1804;

        // The restore is typed as a client apply; an automatic origin would be
        // refused, which is what keeps it inside the explicit-apply contract.
        if (!service_apply_origin_is_client_apply(SERVICE_APPLY_ORIGIN_CLI)) return 1805;
        if (service_apply_origin_is_client_apply(SERVICE_APPLY_ORIGIN_LOGON)) return 1805;
    }

    // ------------------------------------------------------------------
    // The synchronous client stamps the state its mutation names (2006-2020)
    //
    // 1801-1805 above pinned the wire CONTRACT and passed while the product
    // still broke it: fetching a READY envelope was made a prerequisite of the
    // upgrade restore, but nothing ever carried that envelope's identity onto
    // the request built from it, so every CLI apply, the installer's settings
    // restore, and --service-remove's reset went out with three zeros and were
    // refused as malformed. These assertions cover the missing link — envelope
    // -> identity -> stamped request -> validator — so the chain cannot be
    // half-implemented again.
    // ------------------------------------------------------------------
    {
        ServiceClientStateIdentity identity = {};
        service_client_identity_clear(&identity);
        if (service_client_identity_complete(&identity)) return 2006;
        if (service_client_identity_adopt(&identity, nullptr)) return 2006;

        ServiceResponse ready = fake_ready_service_response(77, 5, 3);

        // Only a READY envelope may be adopted: a mutation stamped from a
        // syncing or degraded one names a state the service does not consider
        // current and would be refused as stale.
        ServiceStateEnvelope syncing = ready.state;
        syncing.gpuPhase = SERVICE_GPU_PHASE_RECOVERING;
        if (service_client_identity_adopt(&identity, &syncing)) return 2007;
        if (service_client_identity_complete(&identity)) return 2007;

        ServiceStateEnvelope partial = ready.state;
        partial.validSections &= ~(gc_u32)SERVICE_STATE_SECTION_ADAPTER_IDENTITY;
        if (service_client_identity_adopt(&identity, &partial)) return 2008;

        ServiceStateEnvelope noTopology = ready.state;
        noTopology.topologySignature = 0;
        if (service_client_identity_adopt(&identity, &noTopology)) return 2009;
        ServiceStateEnvelope noInstance = ready.state;
        noInstance.serviceInstanceId = 0;
        if (service_client_identity_adopt(&identity, &noInstance)) return 2009;
        ServiceStateEnvelope noGeneration = ready.state;
        noGeneration.gpuGeneration = 0;
        if (service_client_identity_adopt(&identity, &noGeneration)) return 2009;

        if (!service_client_identity_adopt(&identity, &ready.state)) return 2010;
        if (!service_client_identity_complete(&identity)) return 2010;
        if (identity.serviceInstanceId != ready.state.serviceInstanceId ||
            identity.gpuGeneration != ready.state.gpuGeneration ||
            identity.topologySignature != ready.state.topologySignature)
            return 2010;

        // Watching the service go away must not leave the old identity behind
        // for the next mutation to keep stamping.
        if (service_client_identity_adopt(&identity, &syncing)) return 2011;
        if (service_client_identity_complete(&identity)) return 2011;
        if (identity.serviceInstanceId || identity.gpuGeneration ||
            identity.topologySignature) return 2011;

        // Exactly the request the synchronous path used to build: complete in
        // every other respect, and unstamped.
        ServiceRequest apply = {};
        apply.magic = SERVICE_PROTOCOL_MAGIC;
        apply.version = SERVICE_PROTOCOL_VERSION;
        apply.command = SERVICE_CMD_APPLY;
        apply.callerPid = 4448;
        apply.operationId = 11851789927392885705ull;
        apply.applyOrigin = SERVICE_APPLY_ORIGIN_CLI;
        apply.profileSource = SERVICE_PROFILE_SOURCE_AD_HOC;
        apply.targetGpu = ready.snapshot.adapters[0];
        apply.desired.hasCurvePoint[7] = true;
        apply.desired.curvePointMHz[7] = 1500;

        ServiceClientStateIdentity empty = {};
        if (service_client_stamp_mutation_preconditions(&apply, &empty))
            return 2012;
        if (service_client_stamp_mutation_preconditions(&apply, nullptr))
            return 2012;
        if (service_client_stamp_mutation_preconditions(nullptr, &identity))
            return 2012;
        // A refused stamp leaves the request unsendable rather than partly
        // filled, so the transport gate below can be a single check.
        if (service_client_mutation_is_stamped(&apply)) return 2012;

        // The defect itself: the service refuses it, and says why.
        if (validate_service_request_for_ipc(&apply)) return 2013;
        const char* reason = service_request_reject_reason(&apply);
        if (!reason || !strstr(reason, "precondition")) return 2013;

        // ... and the fix: adopting the envelope the caller already read makes
        // the very same request valid.
        if (!service_client_identity_adopt(&identity, &ready.state)) return 2014;
        if (!service_client_stamp_mutation_preconditions(&apply, &identity))
            return 2014;
        if (!service_client_mutation_is_stamped(&apply)) return 2015;
        if (apply.expectedServiceInstanceId != ready.state.serviceInstanceId ||
            apply.expectedGpuGeneration != ready.state.gpuGeneration ||
            apply.expectedTopologySignature != ready.state.topologySignature)
            return 2015;
        if (service_request_reject_reason(&apply) != nullptr) return 2014;
        if (!validate_service_request_for_ipc(&apply)) return 2014;

        // The reset that --service-remove performs before deleting the service
        // travels the same path; it was refused for the same reason, leaving an
        // uninstalled machine overclocked.
        ServiceRequest reset = {};
        reset.magic = SERVICE_PROTOCOL_MAGIC;
        reset.version = SERVICE_PROTOCOL_VERSION;
        reset.command = SERVICE_CMD_RESET;
        reset.callerPid = 23824;
        reset.operationId = 4242;
        reset.targetGpu = ready.snapshot.adapters[0];
        if (validate_service_request_for_ipc(&reset)) return 2016;
        if (!service_client_stamp_mutation_preconditions(&reset, &identity))
            return 2016;
        if (!validate_service_request_for_ipc(&reset)) return 2016;
    }

    // ------------------------------------------------------------------
    // A refusal reaches the client that caused it (2017-2020)
    //
    // The service answers a request it rejects before authorization with a
    // message and no state at all — the pipe ACL admits every local user, so
    // only an authorized caller receives authoritative state. The client
    // validator treated that missing envelope as a damaged response, discarded
    // the message, and reported a transport failure instead; the mutation path
    // then queried the operation and announced "still pending or unknown".
    // Every protocol and authorization refusal was undiagnosable because of it.
    // ------------------------------------------------------------------
    {
        ServiceResponse refusal = {};
        refusal.magic = SERVICE_PROTOCOL_MAGIC;
        refusal.version = SERVICE_PROTOCOL_VERSION;
        refusal.status = SERVICE_STATUS_ERROR;
        // Stamped exactly as the producers stamp it: a payload-free refusal is
        // still a response, so the severity rule covers it too.
        refusal.outcomeSeverity = SERVICE_OUTCOME_SEVERITY_ERROR;
        refusal.serviceBuildNumber = 470;
        gc_strlcpy(refusal.serviceVersion, sizeof(refusal.serviceVersion), "0.21");
        gc_strlcpy(refusal.message, sizeof(refusal.message),
            "Service request contains invalid protocol fields");
        if (!service_response_payload_is_absent(&refusal)) return 2017;
        if (!validate_service_response_for_ipc(&refusal)) return 2017;
        // The shortcut that lets a stateless refusal through does not skip the
        // severity rule: a refusal claiming a clean outcome is damaged.
        ServiceResponse cleanRefusal = refusal;
        cleanRefusal.outcomeSeverity = SERVICE_OUTCOME_SEVERITY_SUCCESS;
        if (validate_service_response_for_ipc(&cleanRefusal)) return 2017;

        // Success may never be stateless: an OK answer that publishes nothing
        // would let a client read stock values as the service's own state.
        ServiceResponse statelessOk = refusal;
        statelessOk.status = SERVICE_STATUS_OK;
        statelessOk.outcomeSeverity = SERVICE_OUTCOME_SEVERITY_SUCCESS;
        if (validate_service_response_for_ipc(&statelessOk)) return 2018;

        // "Absent" means the whole payload, not merely a plausible-looking
        // header: a half-populated envelope is still a damaged response.
        ServiceResponse halfEnvelope = refusal;
        halfEnvelope.state.serviceInstanceId = 99;
        if (service_response_payload_is_absent(&halfEnvelope)) return 2019;
        if (validate_service_response_for_ipc(&halfEnvelope)) return 2019;
        ServiceResponse straySnapshot = refusal;
        straySnapshot.snapshot.adapterCount = 1;
        if (service_response_payload_is_absent(&straySnapshot)) return 2019;
        if (validate_service_response_for_ipc(&straySnapshot)) return 2019;
        ServiceResponse strayControls = refusal;
        strayControls.controlState.valid = 1;
        if (service_response_payload_is_absent(&strayControls)) return 2019;
        ServiceResponse strayDesired = refusal;
        strayDesired.desired.hasPowerLimit = 1;
        if (service_response_payload_is_absent(&strayDesired)) return 2019;

        // A well-formed answer that does carry state is unaffected, and the
        // header rules still apply to a stateless refusal.
        ServiceResponse ok = fake_ready_service_response(12, 3, 1);
        if (service_response_payload_is_absent(&ok)) return 2020;
        if (!validate_service_response_for_ipc(&ok)) return 2020;
        ServiceResponse wrongVersion = refusal;
        wrongVersion.version = SERVICE_PROTOCOL_VERSION + 1;
        if (validate_service_response_for_ipc(&wrongVersion)) return 2020;
        ServiceResponse unterminated = refusal;
        memset(unterminated.message, 'x', sizeof(unterminated.message));
        if (validate_service_response_for_ipc(&unterminated)) return 2020;
    }

    // ------------------------------------------------------------------
    // Installer command line (1776-1784)
    // ------------------------------------------------------------------
    {
        GcInstallerOptions options = {};
        const char* silent[] = {"/S"};
        gc_installer_parse_options(1, silent, &options);
        if (!options.valid || !options.silent || options.mode != GC_INSTALLER_MODE_INSTALL) return 1776;

        const char* longSilent[] = {"--silent"};
        gc_installer_parse_options(1, longSilent, &options);
        if (!options.valid || !options.silent) return 1776;

        // The NSIS spelling is kept so existing scripts keep working.
        const char* nsisDir[] = {"/D=C:\\Tools\\Green Curve"};
        gc_installer_parse_options(1, nsisDir, &options);
        if (!options.valid || !options.hasDirectory ||
            strcmp(options.directory, "C:\\Tools\\Green Curve") != 0) return 1777;

        const char* longDir[] = {"--dir", "D:\\Apps\\Green Curve"};
        gc_installer_parse_options(2, longDir, &options);
        if (!options.valid || !options.hasDirectory ||
            strcmp(options.directory, "D:\\Apps\\Green Curve") != 0) return 1778;

        // A missing value must fail rather than silently install to the default.
        const char* danglingDir[] = {"--dir"};
        gc_installer_parse_options(1, danglingDir, &options);
        if (options.valid) return 1779;

        // A typo in an unattended update must be loud, not ignored.
        const char* unknown[] = {"--slient"};
        gc_installer_parse_options(1, unknown, &options);
        if (options.valid) return 1780;

        const char* toggles[] = {"--no-start-menu", "--desktop", "--no-launch"};
        gc_installer_parse_options(3, toggles, &options);
        if (!options.valid || options.startMenuShortcut != GC_TOGGLE_OFF ||
            options.desktopShortcut != GC_TOGGLE_ON ||
            options.launchAfterInstall != GC_TOGGLE_OFF) return 1781;

        const char* removal[] = {"--uninstall", "/S"};
        gc_installer_parse_options(2, removal, &options);
        if (!options.valid || options.mode != GC_INSTALLER_MODE_UNINSTALL || !options.silent) return 1782;

        const char* contradiction[] = {"--uninstall", "--dir", "C:\\X\\Y"};
        gc_installer_parse_options(3, contradiction, &options);
        if (options.valid) return 1783;

        const char* help[] = {"/?"};
        gc_installer_parse_options(1, help, &options);
        if (!options.valid || options.mode != GC_INSTALLER_MODE_HELP) return 1784;

        const char* logName[] = {"/log=setup-failure.log"};
        gc_installer_parse_options(1, logName, &options);
        if (!options.valid || !options.hasLogPath ||
            strcmp(options.logPath, "setup-failure.log") != 0) return 1806;
        const char* unsafeLog[] = {
            "--log", "C:\\Windows\\System32\\drivers\\etc\\hosts"};
        gc_installer_parse_options(2, unsafeLog, &options);
        if (options.valid) return 1807;
        const char* traversalLog[] = {"/log=..\\setup.log"};
        gc_installer_parse_options(1, traversalLog, &options);
        if (options.valid) return 1807;
    }

    // ------------------------------------------------------------------
    // Install plan (1785-1799)
    // ------------------------------------------------------------------
    {
        // Trailing separators and case must not make one install look like two.
        if (!gc_install_paths_equal("C:\\Program Files\\Green Curve",
                                    "c:/program files/green curve\\")) return 1785;
        if (gc_install_paths_equal("C:\\A", "C:\\B")) return 1785;
        if (gc_install_paths_equal("", "")) return 1785;

        char joined[GC_INSTALLER_MAX_PATH_CHARS] = {};
        if (!gc_install_default_directory("C:\\Program Files\\", joined, sizeof(joined)) ||
            strcmp(joined, "C:\\Program Files\\Green Curve") != 0) return 1786;

        const char* reason = nullptr;
        if (gc_install_directory_is_acceptable("C:\\", &reason)) return 1787;
        if (gc_install_directory_is_acceptable("Green Curve", &reason)) return 1787;
        if (gc_install_directory_is_acceptable("C:\\Apps\\..\\Green Curve", &reason)) return 1787;
        if (gc_install_directory_is_acceptable("C:\\Apps\\Green*Curve", &reason)) return 1787;
        if (!gc_install_directory_is_acceptable("C:\\Program Files\\Green Curve", &reason)) return 1788;
        if (!gc_install_directory_is_acceptable("\\\\server\\share\\Green Curve", &reason)) return 1788;
        // A component that merely begins with a dot is a normal folder name.
        if (!gc_install_directory_is_acceptable("C:\\Apps\\.hidden\\Green Curve", &reason)) return 1788;
    }
    {
        const char* defaultDirectory = "C:\\Program Files\\Green Curve";
        GcInstallerOptions options = {};
        GcPriorInstall prior = {};
        GcInstallPlan plan = {};

        // Fresh interactive install: documented defaults, no upgrade work.
        gc_installer_options_defaults(&options);
        gc_install_build_plan(&options, &prior, defaultDirectory, &plan);
        if (!plan.valid || plan.isUpgrade || plan.directoryChanged || plan.repointService ||
            plan.captureActiveSettings) return 1789;
        if (strcmp(plan.targetDirectory, defaultDirectory) != 0) return 1790;
        if (!plan.createStartMenuShortcut || plan.createDesktopShortcut || !plan.launchAfterInstall) return 1791;

        // A silent run must not pop a window open at the end.
        options.silent = true;
        gc_install_build_plan(&options, &prior, defaultDirectory, &plan);
        if (plan.launchAfterInstall) return 1792;
        options.launchAfterInstall = GC_TOGGLE_ON;
        gc_install_build_plan(&options, &prior, defaultDirectory, &plan);
        if (!plan.launchAfterInstall) return 1792;

        // Upgrade in place: same folder, service already correct, nothing moves.
        gc_installer_options_defaults(&options);
        prior.present = true;
        snprintf(prior.directory, sizeof(prior.directory), "%s", "C:\\Program Files\\Green Curve");
        snprintf(prior.version, sizeof(prior.version), "%s", "0.21");
        prior.serviceRegistered = true;
        snprintf(prior.serviceDirectory, sizeof(prior.serviceDirectory), "%s",
                 "C:\\Program Files\\Green Curve\\");
        prior.startMenuShortcut = GC_TOGGLE_OFF;
        prior.desktopShortcut = GC_TOGGLE_ON;
        gc_install_build_plan(&options, &prior, defaultDirectory, &plan);
        if (!plan.valid || !plan.isUpgrade || plan.directoryChanged) return 1793;
        if (plan.repointService) return 1794;
        if (!plan.captureActiveSettings) return 1795;
        // Shortcut choices the user already made are repeated, not reset.
        if (plan.createStartMenuShortcut || !plan.createDesktopShortcut) return 1796;
        // ...unless the command line says otherwise.
        options.startMenuShortcut = GC_TOGGLE_ON;
        options.desktopShortcut = GC_TOGGLE_OFF;
        gc_install_build_plan(&options, &prior, defaultDirectory, &plan);
        if (!plan.createStartMenuShortcut || plan.createDesktopShortcut) return 1797;

        // Upgrade that moves: the SCM registration has to follow the files.
        gc_installer_options_defaults(&options);
        options.hasDirectory = true;
        snprintf(options.directory, sizeof(options.directory), "%s", "D:\\Apps\\Green Curve");
        gc_install_build_plan(&options, &prior, defaultDirectory, &plan);
        if (!plan.valid || !plan.isUpgrade || !plan.directoryChanged || !plan.repointService) return 1798;
        if (strcmp(plan.targetDirectory, "D:\\Apps\\Green Curve") != 0) return 1798;
        if (strcmp(plan.previousDirectory, "C:\\Program Files\\Green Curve") != 0) return 1798;

        // A service registered somewhere else entirely (hand-installed portable
        // copy) must still be re-pointed at a fresh install.
        GcPriorInstall portable = {};
        portable.present = true;
        snprintf(portable.directory, sizeof(portable.directory), "%s", "C:\\Users\\x\\Downloads\\greencurve");
        portable.serviceRegistered = true;
        snprintf(portable.serviceDirectory, sizeof(portable.serviceDirectory), "%s",
                 "C:\\Users\\x\\Downloads\\greencurve");
        gc_installer_options_defaults(&options);
        options.hasDirectory = true;
        snprintf(options.directory, sizeof(options.directory), "%s", defaultDirectory);
        gc_install_build_plan(&options, &portable, defaultDirectory, &plan);
        if (!plan.repointService || !plan.directoryChanged) return 1799;

        // A rejected directory must produce an invalid plan with a reason, not
        // a silent fallback to the default.
        gc_installer_options_defaults(&options);
        options.hasDirectory = true;
        snprintf(options.directory, sizeof(options.directory), "%s", "C:\\");
        gc_install_build_plan(&options, &prior, defaultDirectory, &plan);
        if (plan.valid || !plan.error[0]) return 1800;

        // ------------------------------------------------------------------
        // Which binary is asked for the live settings (2021-2025)
        //
        // Asking only the payload's binary is wrong across a protocol bump: the
        // new client refuses the old service's responses, reports that nothing
        // is applied, and the upgrade restores nothing while reporting success.
        // The installed binary always matches the running service -- but only a
        // build that knows the verb may be run with it, because an older one
        // treats it as an unknown argument and opens its window instead.
        // ------------------------------------------------------------------
        if (gc_version_at_least("0.20", 0, 21)) return 2021;
        if (gc_version_at_least("0.9", 0, 21)) return 2021;
        if (!gc_version_at_least("0.21", 0, 21)) return 2022;
        if (!gc_version_at_least("0.21.4", 0, 21)) return 2022;
        if (!gc_version_at_least("0.22", 0, 21)) return 2022;
        if (!gc_version_at_least("1.0", 0, 21)) return 2022;
        // Anything unreadable is treated as too old rather than probed.
        if (gc_version_at_least("", 0, 21)) return 2023;
        if (gc_version_at_least(nullptr, 0, 21)) return 2023;
        if (gc_version_at_least("dev", 0, 21)) return 2023;
        if (gc_version_at_least("0", 0, 21)) return 2023;
        if (gc_version_at_least("0.", 0, 21)) return 2023;

        gc_installer_options_defaults(&options);
        gc_install_build_plan(&options, &prior, defaultDirectory, &plan);
        if (!plan.captureActiveSettings || !plan.captureFromInstalledBinary)
            return 2024;
        if (strcmp(plan.captureBinaryDirectory,
                   "C:\\Program Files\\Green Curve\\") != 0) return 2024;

        // No recorded version (a hand-registered portable copy, which has no
        // Add/Remove Programs entry at all): the installed binary might predate
        // the verb, so only the payload's binary is asked.
        gc_install_build_plan(&options, &portable, defaultDirectory, &plan);
        if (!plan.captureActiveSettings || plan.captureFromInstalledBinary)
            return 2025;
        // A recorded version that describes a DIFFERENT directory than the one
        // the service runs from says nothing about the binary that would be
        // asked, so it does not authorize running it.
        GcPriorInstall mismatched = prior;
        snprintf(mismatched.serviceDirectory, sizeof(mismatched.serviceDirectory),
                 "%s", "C:\\Users\\x\\Downloads\\greencurve");
        gc_install_build_plan(&options, &mismatched, defaultDirectory, &plan);
        if (plan.captureFromInstalledBinary) return 2025;
        GcPriorInstall old = prior;
        snprintf(old.version, sizeof(old.version), "%s", "0.20");
        gc_install_build_plan(&options, &old, defaultDirectory, &plan);
        if (!plan.captureActiveSettings || plan.captureFromInstalledBinary)
            return 2025;

        // ------------------------------------------------------------------
        // The recorded capability outranks the version (2036-2040)
        //
        // A version number answers "does this build know the verb?" only at
        // release granularity, and it was already wrong once: --export-active-
        // settings arrived with this installer, twelve commits after VERSION
        // had moved to 0.21.  Builds therefore exist that satisfy >= 0.21 and
        // open their window when handed the argument, which is a setup that
        // sits in front of an unwanted GUI until the timeout expires.  Every
        // install this installer writes records the answer instead.
        // ------------------------------------------------------------------
        GcPriorInstall recorded = prior;
        recorded.settingsExport = GC_TOGGLE_ON;
        // The marker carries a build the version alone would reject.
        snprintf(recorded.version, sizeof(recorded.version), "%s", "0.20");
        gc_install_build_plan(&options, &recorded, defaultDirectory, &plan);
        if (!plan.captureFromInstalledBinary) return 2036;
        // ...and refuses one the version alone would have accepted, which is
        // the case that was actually broken.
        GcPriorInstall recordedAbsent = prior;
        recordedAbsent.settingsExport = GC_TOGGLE_OFF;
        if (!gc_version_at_least(recordedAbsent.version, 0, 21)) return 2037;
        gc_install_build_plan(&options, &recordedAbsent, defaultDirectory, &plan);
        if (plan.captureFromInstalledBinary) return 2037;
        // An install predating the marker still gets the version fallback,
        // so the first upgrade over one does not lose its settings.
        GcPriorInstall unrecorded = prior;
        unrecorded.settingsExport = GC_TOGGLE_UNSET;
        gc_install_build_plan(&options, &unrecorded, defaultDirectory, &plan);
        if (!plan.captureFromInstalledBinary) return 2038;
        // The marker never overrides the directory rule: a service running from
        // somewhere else is not the binary the ARP entry describes, whatever
        // that entry recorded about it.
        GcPriorInstall recordedElsewhere = recorded;
        snprintf(recordedElsewhere.serviceDirectory,
                 sizeof(recordedElsewhere.serviceDirectory), "%s",
                 "C:\\Users\\x\\Downloads\\greencurve");
        gc_install_build_plan(&options, &recordedElsewhere, defaultDirectory,
                              &plan);
        if (plan.captureFromInstalledBinary) return 2039;
        // Nor does it invent a capture where no service is registered at all.
        GcPriorInstall recordedNoService = recorded;
        recordedNoService.serviceRegistered = false;
        gc_install_build_plan(&options, &recordedNoService, defaultDirectory,
                              &plan);
        if (plan.captureActiveSettings || plan.captureFromInstalledBinary)
            return 2040;

        // ------------------------------------------------------------------
        // What an uninstall is allowed to remove (2041-2050)
        //
        // Two things used to outlive an uninstall: the per-user logon task in
        // Task Scheduler and the uninstaller's own binary (which was handed to
        // the session manager, so the install folder stayed until the next
        // restart).  Both removals need an "is this ours?" rule, and a rule
        // that matches too widely is the one mistake here that is not
        // recoverable -- so each one is pinned in both directions.
        // ------------------------------------------------------------------

        // The task name is "<prefix><sanitized SAM name>": the prefix carries
        // the whole identity claim, and the user part must be present.
        if (!gc_uninstall_task_name_is_ours("Green Curve Startup - MACHINE_testuser"))
            return 2041;
        if (!gc_uninstall_task_name_is_ours("Green Curve Startup - x")) return 2041;
        // Case is not significant: Task Scheduler compares names that way and
        // a hand-edited task must still be recognized.
        if (!gc_uninstall_task_name_is_ours("green curve startup - MACHINE_testuser"))
            return 2042;
        // Anchored prefix, not a substring search, and never the bare prefix:
        // neither was ever registered by this program.
        if (gc_uninstall_task_name_is_ours("Green Curve Startup - ")) return 2043;
        if (gc_uninstall_task_name_is_ours("Backup before Green Curve Startup - nightly"))
            return 2043;
        if (gc_uninstall_task_name_is_ours("Green Curve")) return 2043;
        if (gc_uninstall_task_name_is_ours("Green Curve Startup")) return 2043;
        if (gc_uninstall_task_name_is_ours("")) return 2043;
        if (gc_uninstall_task_name_is_ours(nullptr)) return 2043;
        // A same-named task belonging to something else survives.
        if (gc_uninstall_task_name_is_ours("NVIDIA GeForce Experience SelfUpdate"))
            return 2044;

        // HKCU\...\Run is shared with every other program on the machine and
        // the value name is a plain product string, so the command has to name
        // our executable before the value may be deleted.
        if (!gc_uninstall_command_references(
                "\"C:\\Program Files\\Green Curve\\greencurve.exe\" --tray-start "
                "--config \"C:\\Users\\x\\AppData\\Roaming\\Green Curve\\config.ini\"",
                "greencurve.exe")) return 2045;
        // Case-insensitive: the registry preserves whatever spelling was
        // written, and a moved or hand-edited entry is still ours.
        if (!gc_uninstall_command_references(
                "\"D:\\Apps\\GreenCurve\\GreenCurve.EXE\" --tray-start", "greencurve.exe"))
            return 2045;
        if (!gc_uninstall_command_references(
                "D:\\Apps\\GreenCurve\\greencurve.exe --tray-start", "greencurve.exe"))
            return 2045;
        // A different program that happens to use the same value name keeps
        // its autostart.
        if (gc_uninstall_command_references(
                "\"C:\\Program Files\\Green Curve Wallpaper\\wallpaper.exe\" /run",
                "greencurve.exe")) return 2046;
        if (gc_uninstall_command_references(
                "\"C:\\Tools\\notgreencurve.exe\" --tray-start",
                "greencurve.exe")) return 2046;
        if (gc_uninstall_command_references(
                "\"C:\\Tools\\greencurve.exe.backup\" --tray-start",
                "greencurve.exe")) return 2046;
        if (gc_uninstall_command_references(
                "\"C:\\Windows\\System32\\cmd.exe\" /c greencurve.exe",
                "greencurve.exe")) return 2046;
        if (gc_uninstall_command_references(
                "\"C:\\Tools\\runner.exe\" --program greencurve.exe",
                "greencurve.exe")) return 2046;
        if (gc_uninstall_command_references(
                "\"C:\\Program Files\\Green Curve\\greencurve.exe --tray-start",
                "greencurve.exe")) return 2046;
        // A truncated tail must not match past the end of the string.
        if (gc_uninstall_command_references("greencurve.ex", "greencurve.exe")) return 2046;
        if (gc_uninstall_command_references("", "greencurve.exe")) return 2046;
        if (gc_uninstall_command_references(nullptr, "greencurve.exe")) return 2046;
        if (gc_uninstall_command_references("greencurve.exe", nullptr)) return 2046;

        // The uninstaller deletes its own running image only when it IS the
        // installed copy.  gc_uninstall_execute() also runs inside the setup
        // stub launched with --uninstall, and that binary normally sits in a
        // downloads folder -- it used to be scheduled for deletion regardless,
        // which quietly took the user's setup file with it.
        if (!gc_uninstall_self_is_installed_copy(
                "C:\\Program Files\\Green Curve\\uninstall.exe",
                "C:\\Program Files\\Green Curve")) return 2047;
        // One trailing separator and forward slashes are the same install.
        if (!gc_uninstall_self_is_installed_copy(
                "C:\\Program Files\\Green Curve\\uninstall.exe",
                "C:\\Program Files\\Green Curve\\")) return 2047;
        if (!gc_uninstall_self_is_installed_copy(
                "C:/Program Files/Green Curve/uninstall.exe",
                "C:\\Program Files\\Green Curve")) return 2047;
        if (!gc_uninstall_self_is_installed_copy(
                "c:\\program files\\GREEN CURVE\\uninstall.exe",
                "C:\\Program Files\\Green Curve")) return 2047;
        // The setup stub in a downloads folder is not the installed copy.
        if (gc_uninstall_self_is_installed_copy(
                "C:\\Users\\x\\Downloads\\greencurve-0.21-windows-x64-setup.exe",
                "C:\\Program Files\\Green Curve")) return 2048;
        // Neither is a copy in a subdirectory of the installation: removing the
        // folder would not have removed it anyway.
        if (gc_uninstall_self_is_installed_copy(
                "C:\\Program Files\\Green Curve\\backup\\uninstall.exe",
                "C:\\Program Files\\Green Curve")) return 2048;
        // ...nor a sibling directory whose name merely starts the same way.
        if (gc_uninstall_self_is_installed_copy(
                "C:\\Program Files\\Green Curve 2\\uninstall.exe",
                "C:\\Program Files\\Green Curve")) return 2048;
        if (gc_uninstall_self_is_installed_copy(nullptr, "C:\\Program Files\\Green Curve"))
            return 2049;
        if (gc_uninstall_self_is_installed_copy(
                "C:\\Program Files\\Green Curve\\uninstall.exe", nullptr)) return 2049;
        if (gc_uninstall_self_is_installed_copy("uninstall.exe",
                "C:\\Program Files\\Green Curve")) return 2049;
        if (gc_uninstall_self_is_installed_copy("", "")) return 2049;

        char directory[GC_INSTALLER_MAX_PATH_CHARS] = {};
        if (!gc_uninstall_directory_of("C:\\Program Files\\Green Curve\\uninstall.exe",
                                       directory, sizeof(directory)) ||
            strcmp(directory, "C:\\Program Files\\Green Curve") != 0) return 2050;
        // A file at a drive root yields the bare drive, never a stray empty
        // string that would compare equal to something.
        if (!gc_uninstall_directory_of("C:\\uninstall.exe", directory, sizeof(directory)) ||
            strcmp(directory, "C:") != 0) return 2050;
        // Nothing to split: a relative path has no directory to compare.
        if (gc_uninstall_directory_of("uninstall.exe", directory, sizeof(directory)))
            return 2050;
        if (gc_uninstall_directory_of("\\uninstall.exe", directory, sizeof(directory)))
            return 2050;
        char tiny[4] = {};
        if (gc_uninstall_directory_of("C:\\Program Files\\uninstall.exe", tiny, sizeof(tiny)))
            return 2050;
    }

    // The setup wizard's click filter (3236-3239).  A fast double-click on a
    // BS_OWNERDRAW action button is delivered as BN_CLICKED followed by
    // BN_DBLCLK -- the second click is NOT a second BN_CLICKED (see the native
    // fixture).  Filtering to BN_CLICKED alone swallowed it, so rapid
    // navigation through the installer pages ignored clicks.  Checkboxes stay
    // one-toggle-per-gesture: accepting BN_DBLCLK there would undo the first
    // click.
    {
        // Every real click is a click, on any control.
        if (!gc_wizard_notification_is_click(GC_WIZARD_NOTIFY_CLICKED, false))
            return 3236;
        if (!gc_wizard_notification_is_click(GC_WIZARD_NOTIFY_CLICKED, true))
            return 3236;
        // The OS-classified double-click half is a real click on an action
        // button...
        if (!gc_wizard_notification_is_click(GC_WIZARD_NOTIFY_DBLCLK, false))
            return 3237;
        // ...but not on a checkbox, which must not toggle twice per gesture.
        if (gc_wizard_notification_is_click(GC_WIZARD_NOTIFY_DBLCLK, true))
            return 3238;
        // Every other notification code stays inert on both control kinds.
        if (gc_wizard_notification_is_click(GC_WIZARD_NOTIFY_DBLCLK + 1, false))
            return 3239;
        if (gc_wizard_notification_is_click(1u, true)) return 3239;
        if (gc_wizard_notification_is_click(0xFFFFu, false)) return 3239;
    }

#if defined(_WIN32)
    {
        Win32Utf8Path invalid("\xC3\x28");
        if (invalid.valid_for("\xC3\x28")) return 1026;
    }
    {
        std::string unicodePath = argv[1];
        size_t separator = unicodePath.find_last_of("\\/");
        if (separator != std::string::npos) unicodePath.resize(separator + 1);
        unicodePath += u8"配置_тест_😀_profile.ini";
        gc_DeleteFileUtf8(unicodePath.c_str());
        if (!set_config_int(unicodePath.c_str(), "unicode", "value", 73))
            return 1027;
        if (get_config_int(unicodePath.c_str(), "unicode", "value", 0) != 73)
            return 1028;
        char canonical[MAX_PATH] = {};
        if (!gc_GetFullPathNameUtf8(unicodePath.c_str(), ARRAY_COUNT(canonical),
                canonical, nullptr) || !strstr(canonical, u8"配置_тест_😀"))
            return 1029;
        HANDLE unicodeFile = gc_CreateFileUtf8(unicodePath.c_str(),
            GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL, nullptr);
        if (unicodeFile == INVALID_HANDLE_VALUE) return 1030;
        CloseHandle(unicodeFile);
        if (!gc_DeleteFileUtf8(unicodePath.c_str())) return 1031;
    }
#endif

    // F-LNX-TERM: terminal selection for a launch with no controlling terminal.
    // Double-clicking the binary in a file manager used to print "requires an
    // interactive terminal" to a stderr nobody could see and exit 1.
    {
        // Only konsole and xterm exist on this fake system.
        struct FakeTerminals {
            const char* const* names;
            unsigned int count;
        };
        auto probe = [](const char* command, void* context) -> bool {
            const FakeTerminals* fake = (const FakeTerminals*)context;
            for (unsigned int i = 0; i < fake->count; ++i)
                if (strcmp(command, fake->names[i]) == 0) return true;
            return false;
        };

        const char* kdeSet[] = {"konsole", "xterm"};
        FakeTerminals kde = {kdeSet, 2};
        LinuxTerminalChoice choice = linux_terminal_select("KDE", probe, &kde);
        if (!choice.found || strcmp(choice.command, "konsole") != 0 ||
            choice.style != LINUX_TERMINAL_ARG_DASH_E) return 1810;

        // A colon-separated desktop list must still match its real token, and
        // GNOME must not be answered with the xterm fallback when a GNOME
        // terminal is present.
        const char* gnomeSet[] = {"gnome-terminal", "xterm"};
        FakeTerminals gnome = {gnomeSet, 2};
        choice = linux_terminal_select("ubuntu:GNOME", probe, &gnome);
        if (!choice.found || strcmp(choice.command, "gnome-terminal") != 0 ||
            choice.style != LINUX_TERMINAL_ARG_DOUBLE_DASH) return 1811;

        // The desktop's preferred terminal is absent: fall back rather than
        // fail, because any terminal beats no window at all.
        const char* onlyXterm[] = {"xterm"};
        FakeTerminals sparse = {onlyXterm, 1};
        choice = linux_terminal_select("KDE", probe, &sparse);
        if (!choice.found || strcmp(choice.command, "xterm") != 0) return 1812;

        // Nothing installed must fail closed, not return a command that would
        // make execvp fail with a useless message.
        FakeTerminals empty = {nullptr, 0};
        choice = linux_terminal_select("KDE", probe, &empty);
        if (choice.found || choice.command) return 1813;

        // An unknown desktop still gets the generic order.
        const char* footOnly[] = {"foot"};
        FakeTerminals wl = {footOnly, 1};
        choice = linux_terminal_select("SomeNewDesktop", probe, &wl);
        if (!choice.found || strcmp(choice.command, "foot") != 0 ||
            choice.style != LINUX_TERMINAL_ARG_DIRECT) return 1814;

        // argv styles: the launcher passes a real vector, never a shell string,
        // so each style's fixed prefix has to be exact.
        const char* prefix[2] = {nullptr, nullptr};
        if (linux_terminal_style_prefix(LINUX_TERMINAL_ARG_DIRECT, prefix) != 0)
            return 1815;
        if (linux_terminal_style_prefix(LINUX_TERMINAL_ARG_DASH_X, prefix) != 1 ||
            strcmp(prefix[0], "-x") != 0) return 1816;
        if (linux_terminal_style_prefix(
                LINUX_TERMINAL_ARG_START_DOUBLE_DASH, prefix) != 2 ||
            strcmp(prefix[0], "start") != 0 || strcmp(prefix[1], "--") != 0)
            return 1817;

        // Relaunch is for a graphical launch only: a real terminal, a headless
        // session, and an already-relaunched process must all decline.
        if (linux_terminal_should_relaunch(true, true, false, "wayland-0", ":0"))
            return 1818;
        if (linux_terminal_should_relaunch(false, false, false, nullptr, nullptr))
            return 1819;
        if (linux_terminal_should_relaunch(false, false, true, "wayland-0", ":0"))
            return 1820;
        if (!linux_terminal_should_relaunch(false, false, false, "wayland-0", nullptr))
            return 1821;
        // Redirected stdout alone (`greencurve --tui > log`) is still a
        // terminal launch: only losing BOTH streams means no terminal.
        if (linux_terminal_should_relaunch(true, false, false, nullptr, ":0") == false)
            return 1822;
    }

    // F-LNX-DEBUGLOG: the log lands next to config.ini for clients, and in the
    // daemon's state directory because systemd mounts /usr read-only for the
    // unit (ProtectSystem=full) -- the daemon cannot write beside its binary.
    {
        char path[4096] = {};
        if (!linux_debug_log_resolve_path("/opt/gc/config.ini", false,
                                          "/var/lib/greencurve",
                                          path, sizeof(path)) ||
            strcmp(path, "/opt/gc/" LINUX_DEBUG_LOG_FILE_NAME) != 0) return 1830;
        if (!linux_debug_log_resolve_path("/opt/gc/config.ini", true,
                                          "/var/lib/greencurve",
                                          path, sizeof(path)) ||
            strcmp(path, "/var/lib/greencurve/" LINUX_DEBUG_LOG_FILE_NAME) != 0)
            return 1831;
        // The daemon role without a state directory must fail rather than
        // silently writing into the current working directory.
        if (linux_debug_log_resolve_path("/opt/gc/config.ini", true, nullptr,
                                         path, sizeof(path))) return 1832;

        // Enable resolution mirrors Windows: opt-out by default, an explicit
        // "0" in the environment wins over the config, any other value forces
        // it on even when the config disabled it.
        if (!linux_debug_log_enabled_for(nullptr, 1)) return 1833;
        if (linux_debug_log_enabled_for(nullptr, 0)) return 1834;
        if (linux_debug_log_enabled_for("0", 1)) return 1835;
        if (!linux_debug_log_enabled_for("1", 0)) return 1836;
        if (!linux_debug_log_enabled_for("yes", 0)) return 1837;
    }

    // F-LNX-STARTUP: the daemon's boot-apply policy record.  It authorizes an
    // unattended hardware write, so an incomplete or tampered record must never
    // validate, and "apply nothing" must not carry settings that a later schema
    // change could resurrect.
    {
        GpuAdapterInfo target = {};
        target.valid = true;
        target.pciInfoValid = true;
        target.pciDomain = 0;
        target.pciBus = 7;
        DesiredSettings desired = {};
        desired.hasGpuOffset = true;
        desired.gpuOffsetMHz = 300;

        LinuxDaemonStartupRecord record = {};
        linux_daemon_startup_initialize(&record, SERVICE_STARTUP_POLICY_PROFILE,
                                        3, "profile 3", &target, &desired);
        if (!linux_daemon_startup_valid(&record)) return 1840;
        if (record.profileSlot != 3 || record.desired.gpuOffsetMHz != 300 ||
            strcmp(record.profileName, "profile 3") != 0) return 1841;

        // Zero-initialized is RESTORE_LAST, which is what every build before
        // protocol v13 did; it must validate and carry nothing.
        LinuxDaemonStartupRecord none = {};
        linux_daemon_startup_initialize(&none, SERVICE_STARTUP_POLICY_NONE,
                                        3, "profile 3", &target, &desired);
        if (!linux_daemon_startup_valid(&none)) return 1842;
        if (none.profileSlot != 0 || none.targetGpu.valid ||
            none.desired.hasGpuOffset || none.profileName[0]) return 1843;

        // A PROFILE record without an exact GPU is exactly the case that must
        // not reach the hardware at boot.
        LinuxDaemonStartupRecord noGpu = {};
        linux_daemon_startup_initialize(&noGpu, SERVICE_STARTUP_POLICY_PROFILE,
                                        2, "profile 2", nullptr, &desired);
        if (linux_daemon_startup_valid(&noGpu)) return 1844;

        // Out-of-range slot, unknown mode, and any single flipped byte must all
        // be rejected by the checksum/bounds pair.
        LinuxDaemonStartupRecord badSlot = record;
        badSlot.profileSlot = CONFIG_NUM_SLOTS + 1;
        badSlot.checksum = linux_daemon_startup_checksum(&badSlot);
        if (linux_daemon_startup_valid(&badSlot)) return 1845;
        LinuxDaemonStartupRecord badMode = record;
        badMode.mode = SERVICE_STARTUP_POLICY_MODE_COUNT;
        badMode.checksum = linux_daemon_startup_checksum(&badMode);
        if (linux_daemon_startup_valid(&badMode)) return 1846;
        LinuxDaemonStartupRecord tampered = record;
        tampered.desired.gpuOffsetMHz = 900;  // checksum deliberately stale
        if (linux_daemon_startup_valid(&tampered)) return 1847;
        LinuxDaemonStartupRecord wrongSize = record;
        wrongSize.size = (gc_u32)sizeof(wrongSize) + 1;
        wrongSize.checksum = linux_daemon_startup_checksum(&wrongSize);
        if (linux_daemon_startup_valid(&wrongSize)) return 1848;

        // The wire request that sets the policy: PROFILE needs a slot and an
        // exact GPU; the other modes must carry neither, so a stale target
        // cannot ride along on a request that ignores it.
        ServiceRequest request = {};
        request.magic = SERVICE_PROTOCOL_MAGIC;
        request.version = SERVICE_PROTOCOL_VERSION;
        request.command = SERVICE_CMD_SET_STARTUP_POLICY;
        request.callerPid = 4242;
        request.startupMode = SERVICE_STARTUP_POLICY_PROFILE;
        request.profileSlot = 3;
        request.targetGpu = target;
        request.desired = desired;
        if (!validate_service_request_for_ipc(&request)) return 1850;
        ServiceRequest noSlot = request;
        noSlot.profileSlot = 0;
        if (validate_service_request_for_ipc(&noSlot)) return 1851;
        ServiceRequest noTarget = request;
        memset(&noTarget.targetGpu, 0, sizeof(noTarget.targetGpu));
        if (validate_service_request_for_ipc(&noTarget)) return 1852;
        ServiceRequest strayTarget = request;
        strayTarget.startupMode = SERVICE_STARTUP_POLICY_NONE;
        strayTarget.profileSlot = 0;
        if (validate_service_request_for_ipc(&strayTarget)) return 1853;
        ServiceRequest cleanNone = request;
        cleanNone.startupMode = SERVICE_STARTUP_POLICY_NONE;
        cleanNone.profileSlot = 0;
        memset(&cleanNone.targetGpu, 0, sizeof(cleanNone.targetGpu));
        if (!validate_service_request_for_ipc(&cleanNone)) return 1854;
        // startupMode is meaningless on every other command and must be zero.
        ServiceRequest strayMode = {};
        strayMode.magic = SERVICE_PROTOCOL_MAGIC;
        strayMode.version = SERVICE_PROTOCOL_VERSION;
        strayMode.command = SERVICE_CMD_GET_SNAPSHOT;
        strayMode.callerPid = 4242;
        strayMode.startupMode = SERVICE_STARTUP_POLICY_PROFILE;
        if (validate_service_request_for_ipc(&strayMode)) return 1849;

        // The published envelope must stay coherent: a slot is meaningful only
        // for PROFILE, so any other pairing is a malformed envelope.
        ServiceStateEnvelope envelope = {};
        ServiceSnapshot snapshot = {};
        DesiredSettings envelopeDesired = {};
        ControlState controls = {};
        // Identity is mandatory on any envelope; without it the checks below
        // would pass for the wrong reason.
        envelope.serviceInstanceId = 11;
        envelope.stateRevision = 4;
        envelope.gpuGeneration = 1;
        if (!validate_service_state_envelope_for_ipc(&envelope, &snapshot,
                &envelopeDesired, &controls)) return 1855;
        envelope.startupPolicyMode = SERVICE_STARTUP_POLICY_PROFILE;
        envelope.startupPolicySlot = 0;
        if (validate_service_state_envelope_for_ipc(&envelope, &snapshot,
                &envelopeDesired, &controls)) return 1856;
        envelope.startupPolicySlot = 2;
        if (!validate_service_state_envelope_for_ipc(&envelope, &snapshot,
                &envelopeDesired, &controls)) return 1857;
        envelope.startupPolicyMode = SERVICE_STARTUP_POLICY_NONE;
        if (validate_service_state_envelope_for_ipc(&envelope, &snapshot,
                &envelopeDesired, &controls)) return 1858;
        envelope.startupPolicySlot = 0;
        if (!validate_service_state_envelope_for_ipc(&envelope, &snapshot,
                &envelopeDesired, &controls)) return 1859;
    }

    // F-LNX-STARTUP-SYNC: the boot-apply snapshot follows the profile it names.
    //
    // The reported failure: fan hysteresis 4 °C and a 2000 ms poll interval
    // were edited, saved to slot 1, applied, and came back as the 2 °C/1000 ms
    // defaults after a reboot, because `profile 1` boot-apply kept writing the
    // snapshot captured when the policy was first bound.  Every assertion below
    // fails on the pre-fix code.
    {
        DesiredSettings edited = {};
        initialize_desired_settings_defaults(&edited);
        edited.fanMode = FAN_MODE_CURVE;
        edited.fanCurve.pollIntervalMs = 2000;
        edited.fanCurve.hysteresisC = 4;
        normalize_desired_settings_for_ui(&edited);
        DesiredSettings snapshotted = {};
        initialize_desired_settings_defaults(&snapshotted);
        snapshotted.fanMode = FAN_MODE_CURVE;
        normalize_desired_settings_for_ui(&snapshotted);
        // Precondition for the whole suite: the two differ ONLY in the two
        // fields that silently reverted, and the shared comparison sees it.
        if (snapshotted.fanCurve.pollIntervalMs != 1000 ||
            snapshotted.fanCurve.hysteresisC != 2) return 1970;
        if (desired_settings_equal(&edited, &snapshotted)) return 1971;

        // Saving the slot the policy boots must push the new content.
        if (startup_snapshot_sync_after_profile_write(true,
                SERVICE_STARTUP_POLICY_PROFILE, 1, 1, true) !=
            STARTUP_SNAPSHOT_SYNC_REFRESH) return 1972;
        // A different slot, or a policy that is not profile-bound, owes nothing.
        if (startup_snapshot_sync_after_profile_write(true,
                SERVICE_STARTUP_POLICY_PROFILE, 2, 1, true) !=
            STARTUP_SNAPSHOT_SYNC_NONE) return 1973;
        if (startup_snapshot_sync_after_profile_write(true,
                SERVICE_STARTUP_POLICY_RESTORE_LAST, 0, 1, true) !=
            STARTUP_SNAPSHOT_SYNC_NONE) return 1974;
        if (startup_snapshot_sync_after_profile_write(true,
                SERVICE_STARTUP_POLICY_NONE, 0, 1, true) !=
            STARTUP_SNAPSHOT_SYNC_NONE) return 1975;
        // Clearing the bound slot must stop the boot write, not leave it
        // applying a profile the user just deleted.
        if (startup_snapshot_sync_after_profile_write(true,
                SERVICE_STARTUP_POLICY_PROFILE, 1, 1, false) !=
            STARTUP_SNAPSHOT_SYNC_UNBIND) return 1976;
        // An offline daemon cannot be told, and that has to be said out loud
        // rather than reported as a successful save.
        if (startup_snapshot_sync_after_profile_write(false,
                SERVICE_STARTUP_POLICY_PROFILE, 1, 1, true) !=
            STARTUP_SNAPSHOT_SYNC_UNREACHABLE) return 1977;
        // Reachability must not manufacture work for an unbound slot.
        if (startup_snapshot_sync_after_profile_write(false,
                SERVICE_STARTUP_POLICY_PROFILE, 2, 1, true) !=
            STARTUP_SNAPSHOT_SYNC_NONE) return 1978;
        // Out-of-range slots are never a binding.
        if (startup_snapshot_sync_after_profile_write(true,
                SERVICE_STARTUP_POLICY_PROFILE, 0, 0, true) !=
            STARTUP_SNAPSHOT_SYNC_NONE) return 1979;
        if (startup_snapshot_sync_after_profile_write(true,
                SERVICE_STARTUP_POLICY_PROFILE, CONFIG_NUM_SLOTS + 1,
                CONFIG_NUM_SLOTS + 1, true) !=
            STARTUP_SNAPSHOT_SYNC_NONE) return 1980;

        // Divergence detection: exactly the reported case must read as stale.
        if (startup_snapshot_state_for(SERVICE_STARTUP_POLICY_PROFILE, 1, true,
                &snapshotted, true, &edited) !=
            STARTUP_SNAPSHOT_STATE_DIVERGED) return 1981;
        if (startup_snapshot_state_for(SERVICE_STARTUP_POLICY_PROFILE, 1, true,
                &edited, true, &edited) !=
            STARTUP_SNAPSHOT_STATE_IN_SYNC) return 1982;
        // Never claim sync that was not checked, and never claim divergence for
        // a policy that has no snapshot at all.
        if (startup_snapshot_state_for(SERVICE_STARTUP_POLICY_PROFILE, 1, false,
                nullptr, true, &edited) !=
            STARTUP_SNAPSHOT_STATE_UNKNOWN) return 1983;
        if (startup_snapshot_state_for(SERVICE_STARTUP_POLICY_RESTORE_LAST, 0,
                true, &snapshotted, true, &edited) !=
            STARTUP_SNAPSHOT_STATE_NOT_APPLICABLE) return 1984;
        if (startup_snapshot_state_for(SERVICE_STARTUP_POLICY_PROFILE, 1, true,
                &snapshotted, false, nullptr) !=
            STARTUP_SNAPSHOT_STATE_PROFILE_MISSING) return 1985;
        if (!startup_snapshot_state_is_stale(STARTUP_SNAPSHOT_STATE_DIVERGED) ||
            !startup_snapshot_state_is_stale(
                STARTUP_SNAPSHOT_STATE_PROFILE_MISSING) ||
            startup_snapshot_state_is_stale(STARTUP_SNAPSHOT_STATE_IN_SYNC) ||
            startup_snapshot_state_is_stale(STARTUP_SNAPSHOT_STATE_UNKNOWN) ||
            startup_snapshot_state_is_stale(
                STARTUP_SNAPSHOT_STATE_NOT_APPLICABLE)) return 1986;

        // Daemon-side guard: a refresh may only update a policy that already
        // names the same slot, so it can neither create a boot-apply nor move
        // one to another slot or GPU.
        if (!startup_snapshot_refresh_allowed(SERVICE_STARTUP_POLICY_PROFILE,
                1, 1)) return 1987;
        if (startup_snapshot_refresh_allowed(SERVICE_STARTUP_POLICY_PROFILE,
                1, 2)) return 1988;
        if (startup_snapshot_refresh_allowed(SERVICE_STARTUP_POLICY_NONE,
                0, 1)) return 1989;
        if (startup_snapshot_refresh_allowed(SERVICE_STARTUP_POLICY_RESTORE_LAST,
                0, 1)) return 1990;
        if (startup_snapshot_refresh_allowed(SERVICE_STARTUP_POLICY_PROFILE,
                0, 0)) return 1991;

        // The refreshed record keeps the binding and only replaces settings:
        // that is what makes a content refresh safe to send from a client that
        // does not know which GPU the policy is bound to.
        GpuAdapterInfo boundGpu = {};
        boundGpu.valid = true;
        boundGpu.pciInfoValid = true;
        boundGpu.pciBus = 7;
        LinuxDaemonStartupRecord bound = {};
        linux_daemon_startup_initialize(&bound, SERVICE_STARTUP_POLICY_PROFILE,
                                        1, "profile 1", &boundGpu, &snapshotted);
        if (!linux_daemon_startup_valid(&bound)) return 1992;
        LinuxDaemonStartupRecord refreshed = {};
        linux_daemon_startup_initialize(&refreshed, bound.mode,
                                        bound.profileSlot, bound.profileName,
                                        &bound.targetGpu, &edited);
        if (!linux_daemon_startup_valid(&refreshed)) return 1993;
        if (refreshed.mode != bound.mode ||
            refreshed.profileSlot != bound.profileSlot ||
            strcmp(refreshed.profileName, bound.profileName) != 0 ||
            refreshed.targetGpu.pciBus != bound.targetGpu.pciBus ||
            !refreshed.targetGpu.valid ||
            !refreshed.targetGpu.pciInfoValid) return 1994;
        if (refreshed.desired.fanCurve.pollIntervalMs != 2000 ||
            refreshed.desired.fanCurve.hysteresisC != 4) return 1995;
        if (startup_snapshot_state_for(SERVICE_STARTUP_POLICY_PROFILE, 1, true,
                &refreshed.desired, true, &edited) !=
            STARTUP_SNAPSHOT_STATE_IN_SYNC) return 1996;

        // The IPC trust boundary must not silently rewrite a normalized curve:
        // if it did, a refreshed snapshot could never compare equal to the
        // profile the client just saved.
        DesiredSettings roundTripped = edited;
        validate_desired_settings_for_ipc(&roundTripped);
        if (!desired_settings_equal(&roundTripped, &edited)) return 1997;

        // Wire contract for the refresh command: it names a slot and carries
        // neither a GPU nor a startup mode, because the binding and the mode
        // belong to the stored record.
        ServiceRequest refresh = {};
        refresh.magic = SERVICE_PROTOCOL_MAGIC;
        refresh.version = SERVICE_PROTOCOL_VERSION;
        refresh.command = SERVICE_CMD_REFRESH_STARTUP_PROFILE;
        refresh.callerPid = 4242;
        refresh.profileSlot = 1;
        refresh.desired = edited;
        if (!validate_service_request_for_ipc(&refresh)) return 1998;
        ServiceRequest refreshNoSlot = refresh;
        refreshNoSlot.profileSlot = 0;
        if (validate_service_request_for_ipc(&refreshNoSlot)) return 1999;
        ServiceRequest refreshBadSlot = refresh;
        refreshBadSlot.profileSlot = CONFIG_NUM_SLOTS + 1;
        if (validate_service_request_for_ipc(&refreshBadSlot)) return 2000;
        ServiceRequest refreshWithGpu = refresh;
        refreshWithGpu.targetGpu = boundGpu;
        if (validate_service_request_for_ipc(&refreshWithGpu)) return 2001;
        ServiceRequest refreshWithMode = refresh;
        refreshWithMode.startupMode = SERVICE_STARTUP_POLICY_PROFILE;
        if (validate_service_request_for_ipc(&refreshWithMode)) return 2002;
        ServiceRequest refreshWithOperation = refresh;
        refreshWithOperation.operationId = 9;
        if (validate_service_request_for_ipc(&refreshWithOperation)) return 2003;
        ServiceRequest refreshWithPreconditions = refresh;
        refreshWithPreconditions.expectedServiceInstanceId = 5;
        refreshWithPreconditions.expectedGpuGeneration = 6;
        if (validate_service_request_for_ipc(&refreshWithPreconditions))
            return 2004;
        // Nothing beyond the new command may be accepted by the range check.
        ServiceRequest beyondRange = refresh;
        beyondRange.command = SERVICE_CMD_REFRESH_STARTUP_PROFILE + 1;
        if (validate_service_request_for_ipc(&beyondRange)) return 2005;

        // ------------------------------------------------------------------
        // How the snapshot reaches the client (2026-2035)
        //
        // Everything above proved the DECISIONS with hand-supplied snapshots,
        // and all of it passed while the product was broken: v15 returned the
        // snapshot in `desired`, the member the daemon's end-of-request stamp
        // rewrites with the ACTIVE intent, so the client compared whatever was
        // applied against the profile.  A user who applied anything other than
        // profile 1 saw "PROFILE 1 STALE" for a snapshot that was perfectly in
        // sync.  These assertions are about the carriage, and they are what the
        // suite was missing.
        // ------------------------------------------------------------------

        // The exact shape of the bug: active intent and boot snapshot differ,
        // and only one of them answers "what boots".  Reading `desired` here
        // reports divergence that does not exist.
        ServiceResponse policy = fake_ready_service_response(21, 5, 1);
        policy.state.startupPolicyMode = SERVICE_STARTUP_POLICY_PROFILE;
        policy.state.startupPolicySlot = 1;
        policy.state.activeDesiredValid = 1;
        policy.desired = snapshotted;     // applied right now
        policy.startupProfile = edited;   // what boots
        policy.startupProfileValid = 1;
        if (!validate_service_response_for_ipc(&policy)) return 2026;
        if (startup_snapshot_state_for(SERVICE_STARTUP_POLICY_PROFILE, 1,
                policy.startupProfileValid != 0, &policy.startupProfile,
                true, &edited) != STARTUP_SNAPSHOT_STATE_IN_SYNC) return 2027;
        if (startup_snapshot_state_for(SERVICE_STARTUP_POLICY_PROFILE, 1, true,
                &policy.desired, true, &edited) !=
            STARTUP_SNAPSHOT_STATE_DIVERGED) return 2028;
        // The two members are independent storage; publishing one must never
        // be observable in the other.
        if (desired_settings_equal(&policy.desired, &policy.startupProfile))
            return 2029;

        // Coherence: a published snapshot must belong to a policy that is
        // actually `profile N`, or the client is holding settings that boot
        // nothing.
        ServiceResponse strayMode = policy;
        strayMode.state.startupPolicyMode = SERVICE_STARTUP_POLICY_NONE;
        strayMode.state.startupPolicySlot = 0;
        if (service_response_startup_profile_is_coherent(&strayMode))
            return 2030;
        if (validate_service_response_for_ipc(&strayMode)) return 2030;
        ServiceResponse straySlot = policy;
        straySlot.state.startupPolicySlot = 0;
        if (validate_service_response_for_ipc(&straySlot)) return 2031;
        // Not-valid means genuinely empty, so a client can never read stale
        // bytes out of a response that did not carry a snapshot.
        ServiceResponse silentPayload = policy;
        silentPayload.startupProfileValid = 0;
        if (service_response_startup_profile_is_coherent(&silentPayload))
            return 2032;
        if (validate_service_response_for_ipc(&silentPayload)) return 2032;
        ServiceResponse nonBoolean = policy;
        nonBoolean.startupProfileValid = 2;
        if (validate_service_response_for_ipc(&nonBoolean)) return 2033;
        ServiceResponse strayReserved = policy;
        strayReserved.startupProfileReserved[3] = 1;
        if (validate_service_response_for_ipc(&strayReserved)) return 2034;

        // A response that carries no snapshot at all stays valid, and a
        // stateless refusal is still recognized as payload-free with the new
        // member present.
        ServiceResponse noSnapshot = fake_ready_service_response(21, 6, 1);
        if (noSnapshot.startupProfileValid) return 2035;
        if (!validate_service_response_for_ipc(&noSnapshot)) return 2035;
        ServiceResponse refusalWithSnapshot = {};
        refusalWithSnapshot.magic = SERVICE_PROTOCOL_MAGIC;
        refusalWithSnapshot.version = SERVICE_PROTOCOL_VERSION;
        refusalWithSnapshot.status = SERVICE_STATUS_ERROR;
        refusalWithSnapshot.outcomeSeverity = SERVICE_OUTCOME_SEVERITY_ERROR;
        if (!service_response_payload_is_absent(&refusalWithSnapshot))
            return 2035;
        refusalWithSnapshot.startupProfile = edited;
        if (service_response_payload_is_absent(&refusalWithSnapshot))
            return 2035;
    }

    // F-LNX-GROUPADVICE (1950-1965): the install summary told every user to run
    // `usermod -aG greencurve`, including users who already had the group and
    // users whose enrollment greencurve-setup.sh was about to perform on the
    // next line.  linux_daemon_transport.cpp already established the rule this
    // now follows: never advertise a remedy on a run that does not need one.
    {
        // Already a member: confirm, prescribe nothing.
        if (linux_group_enrollment_advice("testuser", true, true) !=
            LINUX_GROUP_ADVICE_ALREADY_ENROLLED) return 1950;
        // Known account, not a member: name it so the command is pasteable.
        if (linux_group_enrollment_advice("testuser", true, false) !=
            LINUX_GROUP_ADVICE_ENROLL_NAMED) return 1951;
        // No account resolved (root console, no SUDO_USER): generic form.
        if (linux_group_enrollment_advice("", true, false) !=
            LINUX_GROUP_ADVICE_ENROLL_UNKNOWN) return 1952;
        if (linux_group_enrollment_advice(nullptr, true, false) !=
            LINUX_GROUP_ADVICE_ENROLL_UNKNOWN) return 1953;
        // Membership is not believable without a resolved account, so the flag
        // must not promote an empty name to "already enrolled".
        if (linux_group_enrollment_advice("", true, true) !=
            LINUX_GROUP_ADVICE_ENROLL_UNKNOWN) return 1954;
        if (linux_group_enrollment_advice(nullptr, true, true) !=
            LINUX_GROUP_ADVICE_ENROLL_UNKNOWN) return 1955;
        // No group at all: membership cannot be meaningful either way.
        if (linux_group_enrollment_advice("testuser", false, true) !=
            LINUX_GROUP_ADVICE_ENROLL_UNKNOWN) return 1956;
        if (linux_group_enrollment_advice("testuser", false, false) !=
            LINUX_GROUP_ADVICE_ENROLL_UNKNOWN) return 1957;

        // The formatted text: an enrolled user must not be handed a command,
        // because that is the whole complaint this fixes.
        char advice[512] = {};
        linux_format_group_enrollment_advice(
            LINUX_GROUP_ADVICE_ALREADY_ENROLLED, "testuser", advice, sizeof(advice));
        if (!strstr(advice, "testuser")) return 1958;
        if (strstr(advice, "usermod")) return 1959;
        if (strstr(advice, "newgrp")) return 1960;

        linux_format_group_enrollment_advice(
            LINUX_GROUP_ADVICE_ENROLL_NAMED, "testuser", advice, sizeof(advice));
        if (!strstr(advice, "sudo usermod -aG greencurve testuser")) return 1961;
        // The named form must not fall back to the shell variable, or the
        // resolved account was pointless.
        if (strstr(advice, "$USER")) return 1962;

        linux_format_group_enrollment_advice(
            LINUX_GROUP_ADVICE_ENROLL_UNKNOWN, "", advice, sizeof(advice));
        if (!strstr(advice, "sudo usermod -aG greencurve \"$USER\"")) return 1963;

        // Bounded output: a tiny buffer truncates but stays NUL-terminated, and
        // a null account never reaches printf as a null %s.
        char tiny[8];
        memset(tiny, 0x5A, sizeof(tiny));
        linux_format_group_enrollment_advice(
            LINUX_GROUP_ADVICE_ENROLL_NAMED, nullptr, tiny, sizeof(tiny));
        if (tiny[sizeof(tiny) - 1] != 0) return 1964;
        // Zero size must not write at all.
        char untouched = 0x33;
        linux_format_group_enrollment_advice(
            LINUX_GROUP_ADVICE_ALREADY_ENROLLED, "testuser", &untouched, 0);
        if (untouched != 0x33) return 1965;
    }

    // F-LNX-VFSCROLL: wheel and page scrolling must stop at BOTH ends of the VF
    // list.  The old clamp used VF_NUM_POINTS - 1, so scrolling down kept
    // "working" long after the last populated point had left the table.
    {
        ServiceResponse service = {};
        DesiredSettings desired = {};
        service.snapshot.loaded = true;
        // 40 populated points, matching the sparse tables real drivers report.
        for (int i = 0; i < 40; ++i) {
            service.snapshot.curve[i].freq_kHz = 600000u + (unsigned int)i * 20000u;
            service.snapshot.curve[i].volt_uV = 700000u + (unsigned int)i * 5000u;
        }
        TuiViewModel vm = {};
        vm.service = &service;
        vm.desired = &desired;

        // 10 visible rows -> the last page starts at the 10th point from the
        // end, i.e. index 30.
        if (tui_vf_max_first_visible(vm, 10) != 30) return 1860;
        if (tui_vf_max_first_visible(vm, 1) != 39) return 1861;
        // The whole list fits: there is nothing to scroll at all.
        if (tui_vf_max_first_visible(vm, 40) != 0) return 1862;
        if (tui_vf_max_first_visible(vm, 200) != 0) return 1863;
        // A degenerate layout must not produce a negative or wild offset.
        if (tui_vf_max_first_visible(vm, 0) != 0) return 1864;

        // Holes in the table are skipped when drawing, so they must not count
        // toward a page either: removing five populated points moves the last
        // page five entries further back.
        for (int i = 20; i < 25; ++i) {
            service.snapshot.curve[i].freq_kHz = 0;
            service.snapshot.curve[i].volt_uV = 0;
        }
        if (tui_vf_max_first_visible(vm, 10) != 30) return 1865;
        if (tui_vf_max_first_visible(vm, 20) != 15) return 1866;

        // No populated points at all (daemon offline): scrolling is pinned to 0.
        ServiceResponse emptyService = {};
        TuiViewModel emptyVm = {};
        emptyVm.service = &emptyService;
        emptyVm.desired = &desired;
        if (tui_vf_max_first_visible(emptyVm, 10) != 0) return 1867;

        // Reveal-on-select, never reveal-on-render. Two independent code paths
        // used to drag the view back to the selected point -- the per-frame
        // reveal and the 1 Hz refresh clamp -- so the wheel walked a few rows
        // down and snapped back forever. The reveal must fire only when the
        // selection actually moved since it was last honoured.
        if (!tui_selection_needs_reveal(76, -1)) return 1868;   // first frame
        if (tui_selection_needs_reveal(76, 76)) return 1869;    // scrolled away
        if (!tui_selection_needs_reveal(80, 76)) return 1870;   // user selected
        if (tui_selection_needs_reveal(-1, -1)) return 1871;    // no selection
        if (tui_selection_needs_reveal(-1, 76)) return 1872;    // selection lost

        // The reveal puts the selection near the TOP with bounded context, not
        // on the last row. Bottom-aligning it filled a tall terminal with the
        // driver's flat low-voltage floor -- 45 consecutive 180 MHz points on
        // an RTX 5070 -- and pushed the part of the curve being edited off the
        // bottom of the table.
        ServiceResponse full = {};
        for (int i = 0; i < 127; ++i) {
            full.snapshot.curve[i].freq_kHz = 180000u + (unsigned int)i * 20000u;
            full.snapshot.curve[i].volt_uV = 450000u + (unsigned int)i * 6000u;
        }
        TuiViewModel fullVm = {};
        fullVm.service = &full;
        fullVm.desired = &desired;
        // 48 rows, selection 76 -> lead = 12, so the view starts at 64 and the
        // selection sits 12 rows down with the whole tail below it.
        if (tui_vf_reveal_first_visible(fullVm, 76, 48) != 64) return 1873;
        if (tui_vf_reveal_first_visible(fullVm, 76, 16) != 72) return 1874;
        // Near the start there is nothing above to show: clamp at 0 rather
        // than producing a negative offset.
        if (tui_vf_reveal_first_visible(fullVm, 2, 48) != 0) return 1875;
        if (tui_vf_reveal_first_visible(fullVm, 0, 16) != 0) return 1876;
        // Near the end the lead would scroll past the last page, so the
        // end-of-list bound wins (127 points, 16 rows -> 111).
        if (tui_vf_reveal_first_visible(fullVm, 126, 16) != 111) return 1877;
        // Degenerate inputs never produce an out-of-range offset.
        if (tui_vf_reveal_first_visible(fullVm, -1, 16) != 0) return 1878;
        if (tui_vf_reveal_first_visible(fullVm, 76, 0) != 0) return 1879;
        // A single-row table still shows the selection itself.
        if (tui_vf_reveal_first_visible(fullVm, 76, 1) != 76) return 1880;

        // The driver's flat low-voltage floor is not listed at all. On an RTX
        // 5070 points 0..44 are every one of them 180 MHz at rising voltage:
        // 45 identical rows that cannot be meaningfully edited, because a
        // target written there is clamped straight back to the floor.
        ServiceResponse floored = {};
        for (int i = 0; i < 45; ++i) {                 // the floor
            floored.snapshot.curve[i].freq_kHz = 180000u;
            floored.snapshot.curve[i].volt_uV = 450000u + (unsigned int)i * 6000u;
        }
        for (int i = 45; i < 127; ++i) {               // the curve
            floored.snapshot.curve[i].freq_kHz = 202000u + (unsigned int)(i - 45) * 30000u;
            floored.snapshot.curve[i].volt_uV = 735000u + (unsigned int)(i - 45) * 6000u;
        }
        TuiViewModel flooredVm = {};
        flooredVm.service = &floored;
        flooredVm.desired = &desired;
        flooredVm.selectedPoint = -1;
        if (tui_vf_first_listed_point(flooredVm) != 45) return 1881;
        if (tui_vf_hidden_low_point_count(flooredVm) != 45) return 1882;
        // Scrolling cannot walk back into the hidden floor from either helper.
        if (tui_vf_max_first_visible(flooredVm, 16) != 111) return 1883;
        if (tui_vf_reveal_first_visible(flooredVm, 46, 16) != 45) return 1884;
        // A whole list that fits still starts at the first listed point, not 0.
        if (tui_vf_max_first_visible(flooredVm, 200) != 45) return 1885;

        // A selection inside the floor lowers the bound: a profile lock or a
        // graph click can legitimately land there, and an invisible selection
        // would be worse than the rows it saves.
        flooredVm.selectedPoint = 10;
        if (tui_vf_first_listed_point(flooredVm) != 10) return 1886;
        if (tui_vf_hidden_low_point_count(flooredVm) != 10) return 1887;
        flooredVm.selectedPoint = -1;

        // An ordinary rising curve is never trimmed: the leading "run" is one
        // point long, which is not a floor.
        if (tui_vf_first_listed_point(fullVm) != 0) return 1888;
        if (tui_vf_hidden_low_point_count(fullVm) != 0) return 1889;

        // A run shorter than the threshold is also left alone.
        ServiceResponse shortRun = {};
        for (int i = 0; i < 3; ++i) {
            shortRun.snapshot.curve[i].freq_kHz = 180000u;
            shortRun.snapshot.curve[i].volt_uV = 450000u + (unsigned int)i * 6000u;
        }
        for (int i = 3; i < 40; ++i) {
            shortRun.snapshot.curve[i].freq_kHz = 300000u + (unsigned int)i * 20000u;
            shortRun.snapshot.curve[i].volt_uV = 500000u + (unsigned int)i * 6000u;
        }
        TuiViewModel shortVm = {};
        shortVm.service = &shortRun;
        shortVm.desired = &desired;
        shortVm.selectedPoint = -1;
        if (tui_vf_first_listed_point(shortVm) != 0) return 1890;

        // A curve that is flat all the way through must not produce an empty
        // table -- there would be nothing left to show or edit.
        ServiceResponse allFlat = {};
        for (int i = 0; i < 30; ++i) {
            allFlat.snapshot.curve[i].freq_kHz = 180000u;
            allFlat.snapshot.curve[i].volt_uV = 450000u + (unsigned int)i * 6000u;
        }
        TuiViewModel flatVm = {};
        flatVm.service = &allFlat;
        flatVm.desired = &desired;
        flatVm.selectedPoint = -1;
        if (tui_vf_first_listed_point(flatVm) != 0) return 1891;
        if (tui_vf_hidden_low_point_count(flatVm) != 0) return 1892;

        // Offline: no points, no bound, no hidden count.
        if (tui_vf_first_listed_point(emptyVm) != 0) return 1893;
        if (tui_vf_hidden_low_point_count(emptyVm) != 0) return 1894;
    }

    // -----------------------------------------------------------------------
    // F-CRASH: crash-artifact location and rotation policy
    //
    // Every assertion here would have failed before the fix it guards.  The two
    // that matter most are the working-directory fallback (a dump nobody finds,
    // and for the service a SYSTEM dump in a world-readable place) and the
    // cross-prefix ordering bug (rotation deleting the newest terminal crash
    // dump because "greencurve_crash_" sorts before "greencurve_veh_").
    // -----------------------------------------------------------------------
    {
        // --- Directory source ------------------------------------------------
        // User scope with a resolved config directory: use it.
        if (gc_crash_dir_source(false, "C:\\Users\\x\\AppData\\Local\\Green Curve", true)
                != GC_CRASH_DIR_USER_CONFIG) return 1900;
        // User scope with nothing cached: re-derive from the environment, never
        // from the working directory.
        if (gc_crash_dir_source(false, "", true) != GC_CRASH_DIR_USER_ENV) return 1901;
        if (gc_crash_dir_source(false, nullptr, true) != GC_CRASH_DIR_USER_ENV) return 1902;
        // Machine scope always uses the machine directory, even when a perfectly
        // good user directory is cached — a LocalSystem dump in a user-readable
        // path is the disclosure this whole policy exists to prevent.
        if (gc_crash_dir_source(true, "C:\\Users\\x\\AppData\\Local\\Green Curve", true)
                != GC_CRASH_DIR_MACHINE) return 1903;
        // ...and when the machine directory cannot be resolved it loses the dump
        // rather than borrowing the user's.  This asymmetry is the point.
        if (gc_crash_dir_source(true, "C:\\Users\\x\\AppData\\Local\\Green Curve", false)
                != GC_CRASH_DIR_NONE) return 1904;
        if (gc_crash_dir_source(true, "", false) != GC_CRASH_DIR_NONE) return 1905;

        // --- The forbidden fallback -----------------------------------------
        if (gc_crash_dir_is_acceptable(".")) return 1906;
        if (gc_crash_dir_is_acceptable("./")) return 1907;
        if (gc_crash_dir_is_acceptable(".\\")) return 1908;
        if (gc_crash_dir_is_acceptable("")) return 1909;
        if (gc_crash_dir_is_acceptable(nullptr)) return 1910;
        // A leading dot is only forbidden when it IS the working directory; a
        // real hidden directory is a legitimate destination.
        if (!gc_crash_dir_is_acceptable(".config/greencurve")) return 1911;
        if (!gc_crash_dir_is_acceptable("C:\\Users\\x\\AppData\\Local\\Green Curve")) return 1912;
        if (!gc_crash_dir_is_acceptable("/var/lib/greencurve")) return 1913;

        // --- Linux directory rule -------------------------------------------
        char dir[4096] = {};
        if (!gc_linux_crash_dir("/opt/greencurve/config.ini", false, "/var/lib/greencurve",
                                dir, sizeof(dir))) return 1914;
        if (strcmp(dir, "/opt/greencurve") != 0) return 1915;
        // The daemon cannot write next to its own binary (systemd mounts /usr
        // read-only for the unit), so it uses the state directory instead.
        if (!gc_linux_crash_dir("/usr/local/bin/config.ini", true, "/var/lib/greencurve",
                                dir, sizeof(dir))) return 1916;
        if (strcmp(dir, "/var/lib/greencurve") != 0) return 1917;
        // A config path with no directory component means /proc/self/exe was
        // unreadable and the caller fell back to a bare name — i.e. the working
        // directory.  Refused, same as on Windows.
        if (gc_linux_crash_dir("config.ini", false, "/var/lib/greencurve",
                               dir, sizeof(dir))) return 1918;
        if (gc_linux_crash_dir("", false, "/var/lib/greencurve", dir, sizeof(dir))) return 1919;
        if (gc_linux_crash_dir(nullptr, false, "/var/lib/greencurve", dir, sizeof(dir))) return 1920;
        // A daemon with no state directory writes nothing rather than guessing.
        if (gc_linux_crash_dir("/opt/greencurve/config.ini", true, "", dir, sizeof(dir))) return 1921;
        if (gc_linux_crash_dir("/opt/greencurve/config.ini", true, ".", dir, sizeof(dir))) return 1922;
        // Root-level config: the directory is "/", not the empty string.
        if (!gc_linux_crash_dir("/config.ini", false, "/var/lib/greencurve",
                                dir, sizeof(dir))) return 1923;
        if (strcmp(dir, "/") != 0) return 1924;
        // A destination too small to hold the directory fails instead of
        // truncating into some other directory's path.
        char tiny[8] = {};
        if (gc_linux_crash_dir("/opt/greencurve/config.ini", false, "/var/lib/greencurve",
                               tiny, sizeof(tiny))) return 1925;
        if (gc_linux_crash_dir("/opt/x/config.ini", false, nullptr, nullptr, 0)) return 1926;

        // --- Attribution: never delete a file we did not write ---------------
        const char* crashName = "greencurve_crash_20260731_101530_250_pid1234.dmp";
        const char* vehName   = "greencurve_veh_20260730_090000_000_pid99.dmp";
        char stamp[GC_CRASH_STAMP_LENGTH + 1] = {};
        if (!gc_crash_artifact_stamp(crashName, stamp)) return 1927;
        if (strcmp(stamp, "20260731_101530_250") != 0) return 1928;
        if (!gc_crash_artifact_stamp(vehName, stamp)) return 1929;
        if (strcmp(stamp, "20260730_090000_000") != 0) return 1930;
        // Foreign files are not candidates, however plausible they look.
        if (gc_crash_artifact_stamp("MEMORY.DMP", stamp)) return 1931;
        if (gc_crash_artifact_stamp("greencurve_debug.txt", stamp)) return 1932;
        if (gc_crash_artifact_stamp("nvidia_crash_20260731_101530_250_pid1.dmp", stamp)) return 1933;
        if (gc_crash_artifact_stamp(nullptr, stamp)) return 1934;
        // Neither are our own names with a damaged or hand-edited stamp:
        // deleting on a guessed ordering is worse than keeping one file extra.
        if (gc_crash_artifact_stamp("greencurve_crash_2026073_101530_250_pid1.dmp", stamp)) return 1935;
        if (gc_crash_artifact_stamp("greencurve_crash_20260731-101530_250_pid1.dmp", stamp)) return 1936;
        if (gc_crash_artifact_stamp("greencurve_crash_20260731_10153X_250_pid1.dmp", stamp)) return 1937;
        if (gc_crash_artifact_stamp("greencurve_crash_20260731_101530_250.dmp", stamp)) return 1938;
        // Prefix-only and truncated names are rejected rather than accepted on
        // a partially-matched stamp.  Every one of these is a name rotation
        // could plausibly meet — a dump interrupted mid-write, or one a user
        // renamed — and accepting any of them would order the sweep on a stamp
        // that is not really there.
        //
        // Copied into exactly-sized heap buffers rather than used as string
        // literals: literals sit next to each other in .rodata, so a scan that
        // ran past the terminator would read a neighbour and still look correct
        // here.  A malloc'd buffer puts an ASan redzone immediately after the
        // NUL, so the assertion tests the boundary instead of assuming it, and
        // it matches what the real callers pass — WIN32_FIND_DATAA::cFileName
        // and dirent::d_name are both fixed-size arrays.
        {
            static const char* const truncated[] = {
                GC_CRASH_DUMP_PREFIX,
                GC_VEH_DUMP_PREFIX,
                GC_CRASH_DUMP_PREFIX "2026",
                GC_CRASH_DUMP_PREFIX "20260731_101530_250",   // stamp but no "_pid"
                GC_CRASH_DUMP_PREFIX "20260731_101530_25",    // one digit short
            };
            for (size_t i = 0; i < sizeof(truncated) / sizeof(truncated[0]); i++) {
                size_t length = strlen(truncated[i]);
                char* exact = (char*)malloc(length + 1);
                if (!exact) return 1939;
                memcpy(exact, truncated[i], length + 1);
                bool accepted = gc_crash_artifact_stamp(exact, stamp);
                free(exact);
                if (accepted) return 1959 + (int)i;
            }
        }
        // A rejected name must leave the output empty, not partially filled.
        if (stamp[0] != 0) return 1940;

        // --- Ordering: the regression this policy exists for -----------------
        // The whole-name comparison rotation used to do would call the 2026
        // crash dump "older" than the 2020 VEH dump, because 'c' < 'v'.  It
        // therefore deleted the newest terminal crash dump — the one file that
        // explains why the process died — and kept stale recovered ones.
        const char* newCrash = "greencurve_crash_20260731_101530_250_pid1234.dmp";
        const char* oldVeh   = "greencurve_veh_20200101_000000_000_pid1.dmp";
        if (strcmp(newCrash, oldVeh) >= 0) return 1941;   // the trap is real
        if (gc_crash_artifact_is_older(newCrash, oldVeh)) return 1942; // and avoided
        if (!gc_crash_artifact_is_older(oldVeh, newCrash)) return 1943;
        // Ordering within one prefix still works.
        if (!gc_crash_artifact_is_older(
                "greencurve_crash_20260731_101530_249_pid1.dmp",
                "greencurve_crash_20260731_101530_250_pid1.dmp")) return 1944;
        if (gc_crash_artifact_is_older(
                "greencurve_crash_20260731_101531_000_pid1.dmp",
                "greencurve_crash_20260731_101530_999_pid1.dmp")) return 1945;
        // Same millisecond, two processes: the order must still be total and
        // antisymmetric, or the sweep could stall picking a victim.
        const char* tieA = "greencurve_crash_20260731_101530_250_pid1.dmp";
        const char* tieB = "greencurve_crash_20260731_101530_250_pid2.dmp";
        if (!gc_crash_artifact_is_older(tieA, tieB)) return 1946;
        if (gc_crash_artifact_is_older(tieB, tieA)) return 1947;
        if (gc_crash_artifact_is_older(tieA, tieA)) return 1948;
        // An unattributable incumbent is always replaced (it can never be the
        // victim), and an unattributable candidate never becomes one.
        if (!gc_crash_artifact_is_older(newCrash, "MEMORY.DMP")) return 1949;
        if (gc_crash_artifact_is_older("MEMORY.DMP", newCrash)) return 1950;

        // --- Budgets ---------------------------------------------------------
        if (gc_crash_rotation_needed(0, GC_CRASH_ARTIFACT_MAX_KEEP)) return 1951;
        if (gc_crash_rotation_needed(GC_CRASH_ARTIFACT_MAX_KEEP,
                                     GC_CRASH_ARTIFACT_MAX_KEEP)) return 1952;
        if (!gc_crash_rotation_needed(GC_CRASH_ARTIFACT_MAX_KEEP + 1,
                                      GC_CRASH_ARTIFACT_MAX_KEEP)) return 1953;
        if (gc_crash_breadcrumb_needs_reset(0)) return 1954;
        if (gc_crash_breadcrumb_needs_reset(GC_CRASH_BREADCRUMB_MAX_BYTES - 1)) return 1955;
        if (!gc_crash_breadcrumb_needs_reset(GC_CRASH_BREADCRUMB_MAX_BYTES)) return 1956;

        // The two prefixes must stay distinct, or one kind's budget would evict
        // the other's and the per-kind sweep would be meaningless.
        if (strcmp(GC_CRASH_DUMP_PREFIX, GC_VEH_DUMP_PREFIX) == 0) return 1957;
        // The stamp length must match the format the writers actually emit
        // ("YYYYMMDD_HHMMSS_mmm"), or every name becomes unattributable and
        // rotation silently stops deleting anything.
        if (GC_CRASH_STAMP_LENGTH != 19) return 1958;
    }

    // ------------------------------------------------------------------
    // In-app updater policy (4100-4199).
    //
    // These are not display helpers.  Each one gates whether a downloaded
    // executable gets run as SYSTEM, so the negative cases matter more than
    // the positive ones and are enumerated accordingly.
    // ------------------------------------------------------------------

    // --- Version parsing and ordering (4100-4119) ---------------------
    {
        GcUpdateVersion v;
        gc_update_version_parse("0.22.2", &v);
        if (!v.valid || v.major != 0 || v.minor != 22 || v.patch != 2) return 4100;
        // The raw text is preserved, because the release asset names embed it
        // verbatim and "0.22" and "0.22.0" are the same version but different
        // filenames.
        if (strcmp(v.text, "0.22.2") != 0) return 4101;

        gc_update_version_parse("0.22", &v);
        if (!v.valid || v.major != 0 || v.minor != 22 || v.patch != 0) return 4102;
        if (strcmp(v.text, "0.22") != 0) return 4103;

        // Everything that is not exactly MAJOR.MINOR[.PATCH] is refused.  A
        // version the updater cannot parse must never be treated as "probably
        // old" -- that would turn a malformed field into a forced upgrade.
        static const char* const bad[] = {
            "", "1", "v1.0", "1.2.3.4", "1.2.", ".1.2", "1..2", "1.2.3-beta",
            " 1.2", "1.2 ", "01.2", "1.02", "1.2.03", "-1.0", "+1.0", "1,2",
            "1.2.3 ", "abc", "1.a", "9999999.0", "0.0.0.0", ".", "..",
        };
        for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) {
            GcUpdateVersion rejected;
            gc_update_version_parse(bad[i], &rejected);
            if (rejected.valid) return 4104;
        }
        // A single zero component is legal; a padded one is not.
        gc_update_version_parse("0.0.1", &v);
        if (!v.valid) return 4105;

        GcUpdateVersion a, b;
        gc_update_version_parse("0.9", &a);
        gc_update_version_parse("0.10", &b);
        // Numeric ordering, not lexicographic: 0.10 is newer than 0.9.
        if (gc_update_version_compare(&a, &b) >= 0) return 4106;
        if (gc_update_version_compare(&b, &a) <= 0) return 4107;
        if (gc_update_version_compare(&a, &a) != 0) return 4108;

        // "0.22" and "0.22.0" are the same version despite different text.
        GcUpdateVersion short22, long22;
        gc_update_version_parse("0.22", &short22);
        gc_update_version_parse("0.22.0", &long22);
        if (gc_update_version_compare(&short22, &long22) != 0) return 4109;

        // The downgrade gate: strictly newer, and equal is NOT newer.
        if (!gc_update_is_newer(&a, &b)) return 4110;
        if (gc_update_is_newer(&b, &a)) return 4111;
        if (gc_update_is_newer(&a, &a)) return 4112;

        // An invalid version can never look newer than a real one, in either
        // position.  This is the arm that keeps a garbage field from being an
        // upgrade trigger.
        GcUpdateVersion invalid;
        gc_update_version_parse("garbage", &invalid);
        if (gc_update_is_newer(&a, &invalid)) return 4113;
        if (gc_update_is_newer(&invalid, &a)) return 4114;
        if (gc_update_version_compare(&a, &invalid) != 0) return 4115;

        // min_from: at or above the floor passes, below it does not, and an
        // absent floor is not a failure.
        GcUpdateVersion floor21;
        gc_update_version_parse("0.21", &floor21);
        if (!gc_update_meets_minimum_from(&b, nullptr)) return 4116;
        if (gc_update_meets_minimum_from(&b, &floor21)) return 4117;   // 0.10 < 0.21
        GcUpdateVersion v22;
        gc_update_version_parse("0.22", &v22);
        if (!gc_update_meets_minimum_from(&v22, &floor21)) return 4118;
        if (!gc_update_meets_minimum_from(&floor21, &floor21)) return 4119;

        // --- Patch-release ordering and asset naming (4301-4319) ------
        //
        // A PATCH release off a two-component version, which is the shape a
        // hotfix takes: 0.23 -> 0.23.1.  Worth asserting on its own because the
        // patch component is the only one that can be ABSENT, so this is the
        // one upgrade direction that compares a parsed field against a default
        // rather than against another parsed field.  Everything above moves the
        // minor.
        GcUpdateVersion base, patch1, patch2;
        gc_update_version_parse("0.23", &base);
        gc_update_version_parse("0.23.1", &patch1);
        gc_update_version_parse("0.23.2", &patch2);
        if (!base.valid || !patch1.valid || !patch2.valid) return 4301;
        if (!gc_update_is_newer(&base, &patch1)) return 4302;   // 0.23   -> 0.23.1
        if (!gc_update_is_newer(&patch1, &patch2)) return 4303; // 0.23.1 -> 0.23.2
        // And the same gate in reverse: a patch release must never be
        // downgraded to the version it fixed.
        if (gc_update_is_newer(&patch1, &base)) return 4304;
        if (gc_update_is_newer(&patch2, &patch1)) return 4305;
        // A minor bump still outranks any patch of the older minor.
        GcUpdateVersion minor24;
        gc_update_version_parse("0.24", &minor24);
        if (!gc_update_is_newer(&patch2, &minor24)) return 4306;

        // The text is carried through verbatim, because the release asset
        // names are built from it: "0.23.1" and "0.23" compare as different
        // versions AND name different files, and a patch release whose name
        // was rebuilt from the integers would look for the wrong asset.
        char patchAsset[GC_UPDATE_ASSET_NAME_MAX_CHARS] = {};
        if (!gc_update_expected_asset_name(patch1.text, GC_UPDATE_ARCH_X64,
                                           patchAsset, sizeof(patchAsset)) ||
            strcmp(patchAsset, "greencurve-0.23.1-windows-x64-setup.exe") != 0) {
            return 4307;
        }
        if (!gc_update_expected_asset_name(patch1.text, GC_UPDATE_ARCH_ARM64,
                                           patchAsset, sizeof(patchAsset)) ||
            strcmp(patchAsset, "greencurve-0.23.1-windows-arm64-setup.exe") != 0) {
            return 4308;
        }
        // A patch floor is honoured like any other: 0.23 is BELOW a 0.23.1
        // floor even though the two share a minor.
        if (gc_update_meets_minimum_from(&base, &patch1)) return 4309;
        if (!gc_update_meets_minimum_from(&patch1, &patch1)) return 4310;
    }

    // --- Manifest parsing and binding (4120-4149) ---------------------
    {
        static const char kGood[] =
            "# comment\n"
            "\n"
            "format=1\n"
            "version=0.23.0\n"
            "min_from=0.21\n"
            "x64_file=greencurve-0.23.0-windows-x64-setup.exe\n"
            "x64_size=200000\n"
            "x64_sha256=09931428a6e4293292cfc1be8e490d26a52fc9713b61cb84175c40802f2d7cfe\n"
            "arm64_file=greencurve-0.23.0-windows-arm64-setup.exe\n"
            "arm64_size=190000\n"
            "arm64_sha256=94cf3d99cd91075f246efa1de0363323e5ebf06859c5eec940b6bacac4f8ec3a\n";

        GcUpdateManifest manifest;
        gc_update_manifest_parse(kGood, strlen(kGood), &manifest);
        if (!manifest.valid) return 4120;
        if (!manifest.x64.present || !manifest.arm64.present) return 4121;
        if (manifest.x64.size != 200000ULL) return 4122;
        if (!manifest.hasMinimumFrom) return 4123;
        if (strcmp(manifest.version.text, "0.23.0") != 0) return 4124;

        // CRLF must parse identically -- the signer writes LF, but a manifest
        // that survived a text-mode round trip should fail its SIGNATURE, not
        // confuse the parser into a different reading.
        static const char kCrlf[] =
            "format=1\r\n"
            "version=0.23.0\r\n"
            "x64_file=greencurve-0.23.0-windows-x64-setup.exe\r\n"
            "x64_size=200000\r\n"
            "x64_sha256=09931428a6e4293292cfc1be8e490d26a52fc9713b61cb84175c40802f2d7cfe\r\n";
        GcUpdateManifest crlf;
        gc_update_manifest_parse(kCrlf, strlen(kCrlf), &crlf);
        if (!crlf.valid || !crlf.x64.present) return 4125;

        // Every one of these must be refused outright.  The comment on each
        // line is the attack or accident it stands for.
        static const char* const bad[] = {
            // No format key at all.
            "version=0.23.0\nx64_file=greencurve-0.23.0-windows-x64-setup.exe\n",
            // A format this build does not understand: refuse the whole
            // document rather than half-read it.
            "format=2\nversion=0.23.0\n",
            // Unknown key -- rejected, not ignored, exactly like an unknown
            // installer switch.
            "format=1\nversion=0.23.0\nx64_extra=1\n",
            // Duplicate key: "last one wins" is a smuggling primitive.
            "format=1\nversion=0.23.0\nversion=0.24.0\n",
            // Partial arch triple: size and hash without a name.
            "format=1\nversion=0.23.0\nx64_size=200000\n"
            "x64_sha256=09931428a6e4293292cfc1be8e490d26a52fc9713b61cb84175c40802f2d7cfe\n",
            // Digest too short.
            "format=1\nversion=0.23.0\n"
            "x64_file=greencurve-0.23.0-windows-x64-setup.exe\nx64_size=200000\n"
            "x64_sha256=0993\n",
            // Digest uppercase: one canonical spelling only.
            "format=1\nversion=0.23.0\n"
            "x64_file=greencurve-0.23.0-windows-x64-setup.exe\nx64_size=200000\n"
            "x64_sha256=09931428A6E4293292CFC1BE8E490D26A52FC9713B61CB84175C40802F2D7CFE\n",
            // Zero size.
            "format=1\nversion=0.23.0\n"
            "x64_file=greencurve-0.23.0-windows-x64-setup.exe\nx64_size=0\n"
            "x64_sha256=09931428a6e4293292cfc1be8e490d26a52fc9713b61cb84175c40802f2d7cfe\n",
            // Absurd size, past the download ceiling.
            "format=1\nversion=0.23.0\n"
            "x64_file=greencurve-0.23.0-windows-x64-setup.exe\n"
            "x64_size=99999999999999\n"
            "x64_sha256=09931428a6e4293292cfc1be8e490d26a52fc9713b61cb84175c40802f2d7cfe\n",
            // THE MIX-AND-MATCH CASE: a real, correctly-formed arm64 asset
            // name sitting in the x64 slot.  Hash and size are internally
            // consistent; only the name binding catches this.
            "format=1\nversion=0.23.0\n"
            "x64_file=greencurve-0.23.0-windows-arm64-setup.exe\nx64_size=200000\n"
            "x64_sha256=09931428a6e4293292cfc1be8e490d26a52fc9713b61cb84175c40802f2d7cfe\n",
            // An older asset renamed into a newer release's manifest.
            "format=1\nversion=0.23.0\n"
            "x64_file=greencurve-0.22.2-windows-x64-setup.exe\nx64_size=200000\n"
            "x64_sha256=09931428a6e4293292cfc1be8e490d26a52fc9713b61cb84175c40802f2d7cfe\n",
            // A path traversal wearing an asset name.
            "format=1\nversion=0.23.0\n"
            "x64_file=..\\\\evil.exe\nx64_size=200000\n"
            "x64_sha256=09931428a6e4293292cfc1be8e490d26a52fc9713b61cb84175c40802f2d7cfe\n",
            // Describes nothing.
            "format=1\nversion=0.23.0\n",
            // Malformed lines.
            "format=1\nversion\n",
            "format=1\n=0.23.0\n",
            "format=1\nversion=\n",
            // Whitespace around the separator is not tolerated.
            "format = 1\nversion=0.23.0\n",
        };
        for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) {
            GcUpdateManifest rejected;
            gc_update_manifest_parse(bad[i], strlen(bad[i]), &rejected);
            if (rejected.valid) return 4126;
            // Every refusal must carry a reason; an empty message would make a
            // field report undiagnosable.
            if (!rejected.error[0]) return 4127;
        }

        // Degenerate inputs at the boundary.
        GcUpdateManifest edge;
        gc_update_manifest_parse(nullptr, 0, &edge);
        if (edge.valid) return 4128;
        gc_update_manifest_parse(kGood, 0, &edge);
        if (edge.valid) return 4129;
        gc_update_manifest_parse(kGood, GC_UPDATE_MANIFEST_MAX_BYTES + 1, &edge);
        if (edge.valid) return 4130;
        // An embedded NUL would truncate a value the signer never wrote.
        static const char kNul[] = "format=1\nversion=0.23\0.0\n";
        gc_update_manifest_parse(kNul, sizeof(kNul) - 1, &edge);
        if (edge.valid) return 4131;

        // Asset-name acceptance in its own right.
        if (gc_update_asset_name_is_acceptable("")) return 4132;
        if (gc_update_asset_name_is_acceptable("a/b.exe")) return 4133;
        if (gc_update_asset_name_is_acceptable("a\\b.exe")) return 4134;
        if (gc_update_asset_name_is_acceptable("C:evil.exe")) return 4135;
        if (gc_update_asset_name_is_acceptable("evil.exe ")) return 4136;
        if (gc_update_asset_name_is_acceptable("evil.exe.")) return 4137;
        if (gc_update_asset_name_is_acceptable(".hidden")) return 4138;
        if (gc_update_asset_name_is_acceptable("a*.exe")) return 4139;
        if (!gc_update_asset_name_is_acceptable("greencurve-0.23.0-windows-x64-setup.exe"))
            return 4140;

        // The expected-name builder must agree with what release.yml emits.
        char expected[GC_UPDATE_ASSET_NAME_MAX_CHARS];
        if (!gc_update_expected_asset_name("0.23.0", GC_UPDATE_ARCH_X64,
                                           expected, sizeof(expected))) return 4141;
        if (strcmp(expected, "greencurve-0.23.0-windows-x64-setup.exe") != 0) return 4142;
        if (!gc_update_expected_asset_name("0.23.0", GC_UPDATE_ARCH_ARM64,
                                           expected, sizeof(expected))) return 4143;
        if (strcmp(expected, "greencurve-0.23.0-windows-arm64-setup.exe") != 0) return 4144;
        if (gc_update_expected_asset_name("0.23.0", GC_UPDATE_ARCH_UNKNOWN,
                                          expected, sizeof(expected))) return 4145;
        char tiny[8];
        if (gc_update_expected_asset_name("0.23.0", GC_UPDATE_ARCH_X64,
                                          tiny, sizeof(tiny))) return 4146;

        // --- The decision table --------------------------------------
        GcUpdateVersion installed;
        gc_update_version_parse("0.22.2", &installed);
        if (gc_update_decide(&manifest, &installed, GC_UPDATE_ARCH_X64) !=
            GC_UPDATE_DECISION_AVAILABLE) return 4147;

        // Same version installed: up to date, and specifically NOT an install.
        GcUpdateVersion same;
        gc_update_version_parse("0.23.0", &same);
        if (gc_update_decide(&manifest, &same, GC_UPDATE_ARCH_X64) !=
            GC_UPDATE_DECISION_UP_TO_DATE) return 4148;

        // A REPLAY of an older release.  This is the case neither the
        // attestation nor the published .sha256 can catch, because the old
        // artifact is genuinely signed, genuinely attested and correctly
        // hashed.  Only the version comparison refuses it.
        GcUpdateVersion newer;
        gc_update_version_parse("1.0.0", &newer);
        if (gc_update_decide(&manifest, &newer, GC_UPDATE_ARCH_X64) !=
            GC_UPDATE_DECISION_REJECTED) return 4149;
    }

    // --- Decision edge cases and URL policy (4150-4174) ---------------
    {
        static const char kX64Only[] =
            "format=1\n"
            "version=0.23.0\n"
            "min_from=0.21\n"
            "x64_file=greencurve-0.23.0-windows-x64-setup.exe\n"
            "x64_size=200000\n"
            "x64_sha256=09931428a6e4293292cfc1be8e490d26a52fc9713b61cb84175c40802f2d7cfe\n";
        GcUpdateManifest manifest;
        gc_update_manifest_parse(kX64Only, strlen(kX64Only), &manifest);
        if (!manifest.valid) return 4150;

        GcUpdateVersion installed;
        gc_update_version_parse("0.22.2", &installed);
        // No asset for this machine's architecture is its own outcome, not a
        // silent "up to date".
        if (gc_update_decide(&manifest, &installed, GC_UPDATE_ARCH_ARM64) !=
            GC_UPDATE_DECISION_NO_ASSET) return 4151;
        if (gc_update_decide(&manifest, &installed, GC_UPDATE_ARCH_UNKNOWN) !=
            GC_UPDATE_DECISION_REJECTED) return 4152;

        // Below the declared floor: the user is sent to a manual install
        // rather than through a silent upgrade that would lose settings.
        GcUpdateVersion ancient;
        gc_update_version_parse("0.20", &ancient);
        if (gc_update_decide(&manifest, &ancient, GC_UPDATE_ARCH_X64) !=
            GC_UPDATE_DECISION_MANUAL_REQUIRED) return 4153;

        GcUpdateManifest invalid = {};
        if (gc_update_decide(&invalid, &installed, GC_UPDATE_ARCH_X64) !=
            GC_UPDATE_DECISION_REJECTED) return 4154;
        if (gc_update_decide(&manifest, nullptr, GC_UPDATE_ARCH_X64) !=
            GC_UPDATE_DECISION_REJECTED) return 4155;
        if (gc_update_select_asset(&manifest, GC_UPDATE_ARCH_ARM64)) return 4156;
        if (!gc_update_select_asset(&manifest, GC_UPDATE_ARCH_X64)) return 4157;

        // --- URLs ----------------------------------------------------
        if (!gc_update_url_is_acceptable(
                "https://github.com/aufkrawall/green-curve/releases/latest/download/x"))
            return 4158;
        if (!gc_update_url_is_acceptable("https://objects.githubusercontent.com/a?b=c"))
            return 4159;

        static const char* const badUrls[] = {
            // Plaintext, in every spelling.
            "http://github.com/a",
            "HTTPS://github.com/a",
            "ftp://github.com/a",
            "//github.com/a",
            "github.com/a",
            "",
            // Credentials in the authority.  This is the one that defeats a
            // naive "starts with https://github.com" check: the real host is
            // evil.example.
            "https://github.com@evil.example/a",
            "https://user:pass@github.com/a",
            // Off-allowlist hosts, including lookalikes.
            "https://evil.example/a",
            "https://github.com.evil.example/a",
            "https://notgithub.com/a",
            "https://raw.githubusercontent.com/a",
            // An explicit port has one spelling: none.
            "https://github.com:443/a",
            "https://github.com:8443/a",
            // A fragment is never sent on the wire, so its presence means the
            // caller and the HTTP stack disagree about the request.
            "https://github.com/a#b",
            // Control characters and spaces in the path.
            "https://github.com/a b",
            "https://github.com/a\tb",
            // Malformed authority.
            "https://",
            "https:///a",
            "https://.github.com/a",
            "https://github.com./a",
        };
        for (size_t i = 0; i < sizeof(badUrls) / sizeof(badUrls[0]); ++i) {
            if (gc_update_url_is_acceptable(badUrls[i])) return 4160;
        }
        if (gc_update_url_is_acceptable(nullptr)) return 4161;

        // A REAL GitHub signed asset URL must be accepted.
        //
        // This is a regression fixture, not a hypothetical.  The first live run
        // of the updater failed with "redirect without a usable Location
        // header" because GC_UPDATE_URL_MAX_CHARS was 512 while GitHub's second
        // redirect hop is ~945 characters -- SAS parameters, a JWT and a
        // response-content-disposition.  The header was refused as oversized,
        // and the message read like a server fault rather than a client limit.
        //
        // Captured from the live 0.23 release on 2026-08-14 with the signature,
        // JWT and account identifiers replaced by same-length filler: the
        // fixture needs the shape and the length, and a real SAS token has no
        // business in a tracked file.
        {
            static const char kSignedAssetUrl[] =
                "https://release-assets.githubusercontent.com/github-production-relea"
                "se-asset/1195457404/00000000-0000-0000-0000-000000000000?sp=r&sv=201"
                "8-11-09&sr=b&spr=https&se=2026-08-14T02%3A42%3A50Z&rscd=attachment%3"
                "B+filename%3Dgreencurve-update-manifest.txt&rsct=application%2Foctet"
                "-stream&skoid=000000000000000000000000000000000000&sktid=00000000000"
                "0000000000000000000000000&skt=2026-08-14T01%3A42%3A16Z&ske=2026-08-1"
                "4T02%3A42%3A50Z&sks=b&skv=2018-11-09&sig=AAAAAAAAAAAAAAAAAAAAAAAAAAA"
                "AAAAAAAAAAAAA%3D&jwt=BBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBB"
                "BBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBB"
                "BBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBB&re"
                "sponse-content-disposition=attachment%3B%20filename%3Dgreencurve-upd"
                "ate-manifest.txt&response-content-type=application%2Foctet-stream";
            const size_t signedLen = sizeof(kSignedAssetUrl) - 1;
            // Room to spare, or the next parameter GitHub adds breaks every
            // installed client again -- which is exactly what happened here.
            if (signedLen * 2 >= GC_UPDATE_URL_MAX_CHARS) return 4174;
            if (!gc_update_url_is_acceptable(kSignedAssetUrl)) return 4174;

            // It must survive parsing intact.  The query string carries the
            // signature, so a parser that dropped or truncated it would build a
            // URL that fetches nothing.
            GcUpdateUrl signedUrl;
            gc_update_url_parse(kSignedAssetUrl, &signedUrl);
            if (!signedUrl.valid) return 4174;
            if (strcmp(signedUrl.host, "release-assets.githubusercontent.com") != 0)
                return 4174;
            if (strlen(signedUrl.path) !=
                signedLen - strlen("https://") - strlen(signedUrl.host)) return 4174;
            // And it must pass as a redirect target at the hop it really
            // arrives on (the second), not merely in isolation.
            if (!gc_update_redirect_is_acceptable(kSignedAssetUrl, 1)) return 4174;
        }

        // Redirects: bounded AND filtered.  Both halves matter -- a bounded
        // chain of hostile hops is still hostile.
        if (!gc_update_redirect_is_acceptable("https://objects.githubusercontent.com/a", 0))
            return 4162;
        if (!gc_update_redirect_is_acceptable("https://objects.githubusercontent.com/a",
                                              GC_UPDATE_MAX_REDIRECTS - 1)) return 4163;
        if (gc_update_redirect_is_acceptable("https://objects.githubusercontent.com/a",
                                             GC_UPDATE_MAX_REDIRECTS)) return 4164;
        if (gc_update_redirect_is_acceptable("https://evil.example/a", 0)) return 4165;
        if (gc_update_redirect_is_acceptable("http://github.com/a", 0)) return 4166;
        if (gc_update_redirect_is_acceptable("https://github.com/a", -1)) return 4167;

        // --- URL construction ----------------------------------------
        char url[GC_UPDATE_URL_MAX_CHARS];
        if (!gc_update_build_latest_url(GC_UPDATE_MANIFEST_ASSET, url, sizeof(url)))
            return 4168;
        if (strcmp(url,
                   "https://github.com/aufkrawall/green-curve/releases/latest/download/"
                   "greencurve-update-manifest.txt") != 0) return 4169;
        if (!gc_update_build_latest_url(GC_UPDATE_SIGNATURE_ASSET, url, sizeof(url)))
            return 4170;

        if (!gc_update_build_asset_url("0.23.0",
                                       "greencurve-0.23.0-windows-x64-setup.exe",
                                       url, sizeof(url))) return 4171;
        if (strcmp(url,
                   "https://github.com/aufkrawall/green-curve/releases/download/0.23.0/"
                   "greencurve-0.23.0-windows-x64-setup.exe") != 0) return 4172;
        // The builders re-validate their inputs even though callers only pass
        // signature-checked values, so a future reordering of the steps cannot
        // quietly produce a URL out of unverified data.
        if (gc_update_build_asset_url("../evil", "a.exe", url, sizeof(url))) return 4173;
        if (gc_update_build_asset_url("0.23.0", "../evil.exe", url, sizeof(url)))
            return 4173;
        char shortBuf[16];
        if (gc_update_build_asset_url("0.23.0",
                                      "greencurve-0.23.0-windows-x64-setup.exe",
                                      shortBuf, sizeof(shortBuf))) return 4174;
    }

    // --- Schedule and the install gate (4175-4199) --------------------
    {
        if (gc_update_clamp_interval(0) != GC_UPDATE_INTERVAL_MIN_SECONDS) return 4175;
        if (gc_update_clamp_interval(-1) != GC_UPDATE_INTERVAL_MIN_SECONDS) return 4176;
        if (gc_update_clamp_interval(1 << 30) != GC_UPDATE_INTERVAL_MAX_SECONDS) return 4177;
        if (gc_update_clamp_interval(GC_UPDATE_INTERVAL_DEFAULT_SECONDS) !=
            GC_UPDATE_INTERVAL_DEFAULT_SECONDS) return 4178;

        // No failures means the plain interval.
        if (gc_update_next_check_delay(GC_UPDATE_INTERVAL_DEFAULT_SECONDS, 0) !=
            GC_UPDATE_INTERVAL_DEFAULT_SECONDS) return 4179;
        // The first retry is short so a transient network drop recovers fast.
        if (gc_update_next_check_delay(GC_UPDATE_INTERVAL_DEFAULT_SECONDS, 1) !=
            GC_UPDATE_RETRY_BASE_SECONDS) return 4180;
        // Backoff is monotonic and never exceeds the steady-state interval --
        // a machine that failed once must not end up checking LESS often than
        // one that never tried.
        int previous = 0;
        for (int failures = 1; failures <= 40; ++failures) {
            int delay = gc_update_next_check_delay(GC_UPDATE_INTERVAL_DEFAULT_SECONDS,
                                                   failures);
            if (delay < previous) return 4181;
            if (delay > GC_UPDATE_INTERVAL_DEFAULT_SECONDS) return 4182;
            previous = delay;
        }
        if (previous != GC_UPDATE_INTERVAL_DEFAULT_SECONDS) return 4183;

        // Due-ness.  Never checked is due; exactly at the boundary is due.
        if (!gc_update_check_is_due(0, 1000, 100)) return 4184;
        if (gc_update_check_is_due(1000, 1050, 100)) return 4185;
        if (!gc_update_check_is_due(1000, 1100, 100)) return 4186;
        if (!gc_update_check_is_due(1000, 1101, 100)) return 4187;
        // A clock that moved BACKWARDS is due rather than waiting forever: a
        // corrected clock or a restored snapshot would otherwise leave a
        // future timestamp and a permanently silent updater.
        if (!gc_update_check_is_due(5000, 1000, 100)) return 4188;

        // The automatic gate is off unless the user said yes.  UNSET is not
        // consent -- an upgrade from a build without this feature must not
        // silently start making outbound requests.
        if (gc_update_auto_check_allowed(GC_UPDATE_AUTO_CHECK_UNSET, 0, 100000,
                                         GC_UPDATE_INTERVAL_DEFAULT_SECONDS, 0))
            return 4189;
        if (gc_update_auto_check_allowed(GC_UPDATE_AUTO_CHECK_OFF, 0, 100000,
                                         GC_UPDATE_INTERVAL_DEFAULT_SECONDS, 0))
            return 4190;
        if (!gc_update_auto_check_allowed(GC_UPDATE_AUTO_CHECK_ON, 0, 100000,
                                          GC_UPDATE_INTERVAL_DEFAULT_SECONDS, 0))
            return 4191;

        // Downloading changes nothing about the running system, so it is a
        // weaker gate than installing -- but the AUTOMATIC path still requires
        // the setting, because nobody asked for that traffic.
        if (gc_update_download_allowed(GC_UPDATE_AUTO_CHECK_OFF, false, true, false))
            return 4192;
        if (gc_update_download_allowed(GC_UPDATE_AUTO_CHECK_UNSET, false, true, false))
            return 4192;
        if (gc_update_download_allowed(GC_UPDATE_AUTO_CHECK_ON, false, false, false))
            return 4193;
        if (gc_update_download_allowed(GC_UPDATE_AUTO_CHECK_ON, false, true, true))
            return 4194;
        if (!gc_update_download_allowed(GC_UPDATE_AUTO_CHECK_ON, false, true, false))
            return 4195;

        // A USER-REQUESTED check downloads whatever the setting says.  Pressing
        // "Check now" is the consent that the setting exists to infer, and the
        // first live run proved the cost of getting this wrong: the manual path
        // found an update, declined to download it because auto-check was UNSET,
        // and left Install greyed out with no explanation.
        if (!gc_update_download_allowed(GC_UPDATE_AUTO_CHECK_UNSET, true, true, false))
            return 4196;
        if (!gc_update_download_allowed(GC_UPDATE_AUTO_CHECK_OFF, true, true, false))
            return 4196;
        if (!gc_update_download_allowed(GC_UPDATE_AUTO_CHECK_ON, true, true, false))
            return 4196;
        // It does not, however, invent an update that is not there, and it does
        // not re-download one already staged.
        if (gc_update_download_allowed(GC_UPDATE_AUTO_CHECK_UNSET, true, false, false))
            return 4196;
        if (gc_update_download_allowed(GC_UPDATE_AUTO_CHECK_UNSET, true, true, true))
            return 4196;

        // --- The install gate ----------------------------------------
        // A fully clean, fully consented state is the only ALLOWED one.
        GcUpdateInstallGate gate = {};
        gate.userConsented = true;
        gate.packageStaged = true;
        gate.packageVerified = true;
        gate.isInstalledCopy = true;
        if (gc_update_install_decision(&gate) != GC_UPDATE_INSTALL_ALLOWED) return 4190;

        // Each refusal arm in turn.  Consent is checked first so the log's
        // first line is never a technical detail when the real answer is that
        // nobody asked for this.
        {
            GcUpdateInstallGate g = gate;
            g.userConsented = false;
            if (gc_update_install_decision(&g) != GC_UPDATE_INSTALL_NO_CONSENT) return 4197;
        }
        {
            GcUpdateInstallGate g = gate;
            g.packageVerified = false;
            if (gc_update_install_decision(&g) != GC_UPDATE_INSTALL_NOT_VERIFIED) return 4197;
        }
        {
            GcUpdateInstallGate g = gate;
            g.packageStaged = false;
            if (gc_update_install_decision(&g) != GC_UPDATE_INSTALL_NO_PACKAGE) return 4197;
        }
        {
            GcUpdateInstallGate g = gate;
            g.applyInFlight = true;
            if (gc_update_install_decision(&g) != GC_UPDATE_INSTALL_BUSY_APPLYING) return 4197;
        }
        {
            GcUpdateInstallGate g = gate;
            g.foregroundAppActive = true;
            if (gc_update_install_decision(&g) != GC_UPDATE_INSTALL_BUSY_FOREGROUND)
                return 4197;
        }
        {
            GcUpdateInstallGate g = gate;
            g.isInstalledCopy = false;
            if (gc_update_install_decision(&g) != GC_UPDATE_INSTALL_NOT_INSTALLED_COPY)
                return 4197;
        }
        {
            GcUpdateInstallGate g = gate;
            g.installAlreadyRunning = true;
            if (gc_update_install_decision(&g) != GC_UPDATE_INSTALL_ALREADY_RUNNING)
                return 4197;
        }
        // An unverified package is refused even when everything else is set,
        // and a null gate is a refusal rather than a crash.
        if (gc_update_install_decision(nullptr) != GC_UPDATE_INSTALL_NO_CONSENT) return 4198;

        // Every refusal has readable text; an empty one would surface to the
        // user as a status line that says nothing.
        for (int code = GC_UPDATE_INSTALL_ALLOWED;
             code <= GC_UPDATE_INSTALL_ALREADY_RUNNING; ++code) {
            const char* text = gc_update_install_refusal_text((GcUpdateInstallRefusal)code);
            if (!text || !text[0]) return 4199;
        }
    }

    // --- v19 update commands on the wire (4200-4229) ------------------
    //
    // The update commands are TRIGGERS, never payloads.  The service ends a
    // successful update by launching an installer with SYSTEM rights, and it
    // resolves which file that is entirely from compiled-in constants and a
    // signature-verified manifest.  A command that let an unprivileged GUI name
    // a path, a version or a digest would be a local privilege escalation
    // however carefully the named file were checked afterwards -- so the
    // validator refuses those fields at the boundary rather than trusting four
    // handlers to each remember not to look at them.
    {
        ServiceRequest base = {};
        base.magic = SERVICE_PROTOCOL_MAGIC;
        base.version = SERVICE_PROTOCOL_VERSION;
        base.callerPid = 4242;

        const gc_u32 readOnly[3] = {
            SERVICE_CMD_GET_UPDATE_STATE,
            SERVICE_CMD_CHECK_FOR_UPDATE,
            SERVICE_CMD_INSTALL_UPDATE,
        };
        for (size_t i = 0; i < 3; ++i) {
            ServiceRequest request = base;
            request.command = readOnly[i];
            if (!validate_service_request_for_ipc(&request)) return 4200;

            // Every mutation field is refused, one at a time.  Enumerated
            // rather than tested in bulk so a future field that stops being
            // checked names itself.
            ServiceRequest tampered = request;
            tampered.path[0] = 'C';
            if (validate_service_request_for_ipc(&tampered)) return 4201;
            tampered = request; tampered.operationId = 9;
            if (validate_service_request_for_ipc(&tampered)) return 4202;
            tampered = request; tampered.flags = SERVICE_REQUEST_FLAG_INTERACTIVE;
            if (validate_service_request_for_ipc(&tampered)) return 4203;
            tampered = request; tampered.targetGpu.valid = 1;
            if (validate_service_request_for_ipc(&tampered)) return 4204;
            tampered = request; tampered.profileSlot = 3;
            if (validate_service_request_for_ipc(&tampered)) return 4205;
            tampered = request; tampered.profileSource = SERVICE_PROFILE_SOURCE_AD_HOC;
            if (validate_service_request_for_ipc(&tampered)) return 4206;
            tampered = request; tampered.applyOrigin = SERVICE_APPLY_ORIGIN_STANDBY;
            if (validate_service_request_for_ipc(&tampered)) return 4207;
            tampered = request; tampered.resetOcBeforeApply = 1;
            if (validate_service_request_for_ipc(&tampered)) return 4208;
            tampered = request; tampered.startupMode = SERVICE_STARTUP_POLICY_PROFILE;
            if (validate_service_request_for_ipc(&tampered)) return 4209;
            tampered = request; tampered.expectedServiceInstanceId = 5;
            tampered.expectedGpuGeneration = 6; tampered.targetGpu.valid = 1;
            if (validate_service_request_for_ipc(&tampered)) return 4210;
            // Settings must not ride along either.  The handlers never read
            // `desired` for these commands, but refusing it here keeps the
            // contract checkable at the boundary.
            tampered = request; tampered.desired.hasGpuOffset = 1;
            tampered.desired.gpuOffsetMHz = 300;
            if (validate_service_request_for_ipc(&tampered)) return 4211;
            // The policy fields belong to SET_UPDATE_POLICY alone.
            tampered = request; tampered.updateAutoCheck = GC_UPDATE_AUTO_CHECK_ON;
            if (validate_service_request_for_ipc(&tampered)) return 4212;
            tampered = request;
            tampered.updateIntervalSeconds = GC_UPDATE_INTERVAL_DEFAULT_SECONDS;
            if (validate_service_request_for_ipc(&tampered)) return 4213;
        }

        // SET_UPDATE_POLICY is the one update command carrying client data, and
        // it carries exactly two bounded integers.
        {
            ServiceRequest policy = base;
            policy.command = SERVICE_CMD_SET_UPDATE_POLICY;
            policy.updateAutoCheck = GC_UPDATE_AUTO_CHECK_ON;
            policy.updateIntervalSeconds = GC_UPDATE_INTERVAL_DEFAULT_SECONDS;
            if (!validate_service_request_for_ipc(&policy)) return 4214;

            // OFF and UNSET are both legitimate answers; UNSET is how a machine
            // that has never been asked is distinguished from one that said no.
            ServiceRequest variant = policy;
            variant.updateAutoCheck = GC_UPDATE_AUTO_CHECK_OFF;
            if (!validate_service_request_for_ipc(&variant)) return 4215;
            variant.updateAutoCheck = GC_UPDATE_AUTO_CHECK_UNSET;
            if (!validate_service_request_for_ipc(&variant)) return 4216;

            // An out-of-range interval is REFUSED, not clamped.  The clamp
            // exists for a hand-edited config file; a request is machine-written
            // and a bad one means the client is confused, which should be loud.
            variant = policy;
            variant.updateIntervalSeconds = GC_UPDATE_INTERVAL_MIN_SECONDS - 1;
            if (validate_service_request_for_ipc(&variant)) return 4217;
            variant.updateIntervalSeconds = GC_UPDATE_INTERVAL_MAX_SECONDS + 1;
            if (validate_service_request_for_ipc(&variant)) return 4218;
            variant.updateIntervalSeconds = 0;
            if (validate_service_request_for_ipc(&variant)) return 4219;
            variant = policy; variant.updateAutoCheck = GC_UPDATE_AUTO_CHECK_ON + 1;
            if (validate_service_request_for_ipc(&variant)) return 4220;
            // The boundaries themselves are accepted.
            variant = policy;
            variant.updateIntervalSeconds = GC_UPDATE_INTERVAL_MIN_SECONDS;
            if (!validate_service_request_for_ipc(&variant)) return 4221;
            variant.updateIntervalSeconds = GC_UPDATE_INTERVAL_MAX_SECONDS;
            if (!validate_service_request_for_ipc(&variant)) return 4222;
            // And it is still refused a path like every other update command.
            variant = policy; variant.path[0] = 'C';
            if (validate_service_request_for_ipc(&variant)) return 4223;
        }

        // The refusal reason names the field, so a live rejection is
        // diagnosable from one log line rather than "malformed request".
        {
            ServiceRequest tampered = base;
            tampered.command = SERVICE_CMD_INSTALL_UPDATE;
            tampered.path[0] = 'C';
            const char* reason = service_request_reject_reason(&tampered);
            if (!reason || !strstr(reason, "path")) return 4224;
        }

        // The wire buffer must hold the longest version the parser accepts, or
        // a legitimate release would be truncated on its way to the GUI.
        if (SERVICE_UPDATE_VERSION_CHARS != GC_UPDATE_VERSION_MAX_CHARS) return 4225;
        {
            GcUpdateVersion longest;
            gc_update_version_parse("999999.999999.999999", &longest);
            if (!longest.valid) return 4226;
            if (strlen(longest.text) + 1 > SERVICE_UPDATE_VERSION_CHARS) return 4227;
        }
        // A command past the end of the v19 set is still unknown.
        {
            ServiceRequest tampered = base;
            tampered.command = SERVICE_CMD_SET_UPDATE_POLICY + 1;
            if (validate_service_request_for_ipc(&tampered)) return 4228;
            tampered.command = SERVICE_CMD_NONE;
            if (validate_service_request_for_ipc(&tampered)) return 4229;
        }
    }

#if defined(_WIN32)
    // --- The signer and the verifier agree (4230-4249) ----------------
    //
    // tools/update_signing.py produces signatures in pure Python; the service
    // checks them with CNG.  Two independent implementations have to agree on
    // curve, hash, signature encoding and byte order, and a disagreement is
    // invisible until it is total: every published update would be refused in
    // the field with nothing failing here.  So this is a known-answer test
    // across the boundary, not a round trip within one implementation.
    //
    // The key is the RFC 6979 A.2.5 P-256 test vector -- published material,
    // not a secret -- and the signature below was produced by the Python signer
    // over the exact manifest bytes below.  If either side changes its format,
    // this stops verifying.
    {
        static const char kManifest[] =
            "format=1\n"
            "version=0.23.0\n"
            "min_from=0.21\n"
            "x64_file=greencurve-0.23.0-windows-x64-setup.exe\n"
            "x64_size=200000\n"
            "x64_sha256=09931428a6e4293292cfc1be8e490d26a52fc9713b61cb84175c40802f2d7cfe\n";
        static const unsigned char kVectorPublicKey[64] = {
            0x60, 0xFE, 0xD4, 0xBA, 0x25, 0x5A, 0x9D, 0x31,
            0xC9, 0x61, 0xEB, 0x74, 0xC6, 0x35, 0x6D, 0x68,
            0xC0, 0x49, 0xB8, 0x92, 0x3B, 0x61, 0xFA, 0x6C,
            0xE6, 0x69, 0x62, 0x2E, 0x60, 0xF2, 0x9F, 0xB6,
            0x79, 0x03, 0xFE, 0x10, 0x08, 0xB8, 0xBC, 0x99,
            0xA4, 0x1A, 0xE9, 0xE9, 0x56, 0x28, 0xBC, 0x64,
            0xF2, 0xF1, 0xB2, 0x0C, 0x2D, 0x7E, 0x9F, 0x51,
            0x77, 0xA3, 0xC2, 0x94, 0xD4, 0x46, 0x22, 0x99,
        };
        // Base64 of the same value, as the .sig asset actually carries it.
        static const char kVectorSignatureB64[] =
            "0p0hoHC2sgGRNDoeHDsmPuxl+Hzug5Et9n4wilza7IIYqBUGjQZueG9Izi5U/8uE"
            "6c+hcHY6hS/u3Vsr2GKh5Q==";

        const size_t manifestLen = sizeof(kManifest) - 1;

        unsigned char signature[GC_UPDATE_SIGNATURE_BYTES] = {};
        size_t signatureLen = 0;
        if (!gc_update_base64_decode(kVectorSignatureB64,
                                     sizeof(kVectorSignatureB64) - 1,
                                     signature, sizeof(signature), &signatureLen))
            return 4230;
        if (signatureLen != GC_UPDATE_SIGNATURE_BYTES) return 4231;

        unsigned char digest[32] = {};
        if (!gc_update_sha256_buffer(kManifest, manifestLen, digest)) return 4232;

        // THE CROSS-IMPLEMENTATION ASSERTION.
        if (!gc_update_verify_with_key(kVectorPublicKey, digest,
                                       signature, signatureLen)) return 4233;

        // A single flipped bit anywhere in the manifest must break it.  Checked
        // at three positions rather than one because a verifier that hashed
        // only a prefix would pass a test that only edited the start.
        const size_t flipAt[3] = { 0, manifestLen / 2, manifestLen - 1 };
        for (size_t i = 0; i < 3; ++i) {
            char tampered[sizeof(kManifest)];
            memcpy(tampered, kManifest, sizeof(kManifest));
            tampered[flipAt[i]] = (char)(tampered[flipAt[i]] ^ 0x01);
            unsigned char tamperedDigest[32] = {};
            if (!gc_update_sha256_buffer(tampered, manifestLen, tamperedDigest))
                return 4234;
            if (gc_update_verify_with_key(kVectorPublicKey, tamperedDigest,
                                          signature, signatureLen)) return 4235;
        }
        // Truncation must break it too -- a shorter manifest is a different
        // document, not a prefix of an accepted one.
        {
            unsigned char shortDigest[32] = {};
            if (!gc_update_sha256_buffer(kManifest, manifestLen - 1, shortDigest))
                return 4236;
            if (gc_update_verify_with_key(kVectorPublicKey, shortDigest,
                                          signature, signatureLen)) return 4237;
        }
        // A corrupted signature, and a signature under the wrong key.
        {
            unsigned char corrupted[GC_UPDATE_SIGNATURE_BYTES];
            memcpy(corrupted, signature, sizeof(corrupted));
            corrupted[0] = (unsigned char)(corrupted[0] ^ 0x01);
            if (gc_update_verify_with_key(kVectorPublicKey, digest,
                                          corrupted, sizeof(corrupted))) return 4238;
            memcpy(corrupted, signature, sizeof(corrupted));
            corrupted[63] = (unsigned char)(corrupted[63] ^ 0x01);
            if (gc_update_verify_with_key(kVectorPublicKey, digest,
                                          corrupted, sizeof(corrupted))) return 4239;
        }
        // The project's own release key must not verify a signature made with
        // the test vector key.  This is the arm that would catch the test
        // fixture accidentally being signed with the real key.
        if (gc_update_verify_with_key(GC_UPDATE_PUBLIC_KEY_ACTIVE, digest,
                                      signature, signatureLen)) return 4240;
        if (gc_update_verify_with_key(GC_UPDATE_PUBLIC_KEY_NEXT, digest,
                                      signature, signatureLen)) return 4241;
        // A wrong signature length is refused rather than padded or truncated.
        if (gc_update_verify_with_key(kVectorPublicKey, digest, signature, 63))
            return 4242;
        if (gc_update_verify_with_key(kVectorPublicKey, digest, signature, 0))
            return 4243;
        // An all-zero key slot is not a valid point and must be refused as
        // unpopulated rather than reaching CNG at all.
        {
            static const unsigned char kEmpty[64] = {};
            if (gc_update_key_is_populated(kEmpty)) return 4244;
            if (gc_update_verify_with_key(kEmpty, digest, signature, signatureLen))
                return 4245;
            if (!gc_update_key_is_populated(GC_UPDATE_PUBLIC_KEY_ACTIVE)) return 4246;
            if (!gc_update_key_is_populated(GC_UPDATE_PUBLIC_KEY_NEXT)) return 4247;
            // The shipped keys must differ, or "rotation" would be a no-op that
            // looks like it works.
            if (memcmp(GC_UPDATE_PUBLIC_KEY_ACTIVE, GC_UPDATE_PUBLIC_KEY_NEXT,
                       GC_UPDATE_PUBLIC_KEY_BYTES) == 0) return 4248;
        }
        // And the manifest the fixture signs must be one the parser accepts,
        // or this test would be proving agreement about an unusable document.
        {
            GcUpdateManifest parsed;
            gc_update_manifest_parse(kManifest, manifestLen, &parsed);
            if (!parsed.valid) return 4249;
        }
        // A malleable high-S signature verifies mathematically but violates
        // the signer's canonical encoding.  The verifier must reject it.
        {
            unsigned char highS[GC_UPDATE_SIGNATURE_BYTES];
            memcpy(highS, signature, sizeof(highS));
            static const unsigned char complementaryS[32] = {
                0xE7, 0x57, 0xEA, 0xF8, 0x72, 0xF9, 0x91, 0x88,
                0x90, 0xB7, 0x31, 0xD1, 0xAB, 0x00, 0x34, 0x7A,
                0xD3, 0x17, 0x59, 0x3D, 0x30, 0xDD, 0x19, 0x55,
                0x04, 0xDC, 0x6F, 0x97, 0x24, 0x00, 0x83, 0x6C,
            };
            memcpy(highS + 32, complementaryS, sizeof(complementaryS));
            if (gc_update_verify_with_key(kVectorPublicKey, digest,
                                          highS, sizeof(highS))) return 4300;
        }
    }

    // --- Base64 decoding (4250-4259) ----------------------------------
    //
    // The decoder is strict on purpose: a permissive one accepts several
    // spellings of a single signature, and "which bytes were signed" must have
    // exactly one answer.
    {
        unsigned char out[GC_UPDATE_SIGNATURE_BYTES] = {};
        size_t outLen = 0;
        // A trailing newline is the one benign mutation a text file suffers.
        static const char kWithNewline[] =
            "0p0hoHC2sgGRNDoeHDsmPuxl+Hzug5Et9n4wilza7IIYqBUGjQZueG9Izi5U/8uE"
            "6c+hcHY6hS/u3Vsr2GKh5Q==\r\n";
        if (!gc_update_base64_decode(kWithNewline, sizeof(kWithNewline) - 1,
                                     out, sizeof(out), &outLen)) return 4250;
        if (outLen != GC_UPDATE_SIGNATURE_BYTES) return 4251;

        static const char* const bad[] = {
            "",                       // empty
            "AAA",                    // not a multiple of four
            "AAAAA",                  // ditto
            "A===",                   // over-padded
            "====",                   // all padding
            "AA=A",                   // data after padding
            "AA A=",                  // embedded space
            "AAAA\nAAAA",             // embedded newline
            "-_-_",                   // URL-safe alphabet is a different encoding
            "AAAA=AAA",               // padding in the middle
        };
        for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) {
            unsigned char scratch[GC_UPDATE_SIGNATURE_BYTES] = {};
            size_t len = 0;
            if (gc_update_base64_decode(bad[i], strlen(bad[i]), scratch,
                                        sizeof(scratch), &len)) return 4252;
        }
        // A value that decodes to more than the caller's buffer is refused
        // rather than truncated -- a truncated signature must never be
        // presented to the verifier as a complete one.
        {
            unsigned char tiny[8] = {};
            size_t len = 0;
            if (gc_update_base64_decode(kWithNewline, sizeof(kWithNewline) - 1,
                                        tiny, sizeof(tiny), &len)) return 4253;
        }
        if (gc_update_base64_decode(nullptr, 0, out, sizeof(out), &outLen))
            return 4254;
        }
#endif  // _WIN32

    // --- The installer command line (4260-4269) -----------------------
    //
    // The updater drives the setup program, so the string it emits and the
    // parser that reads it have to agree.  Nothing tested that, and the first
    // real install died on it: an unquoted `/D=C:\Program Files\Green Curve`
    // was split by CommandLineToArgvW into `/D=C:\Program`, `Files\Green`,
    // `Curve`, so setup parsed the directory as `C:\Program`, met an unknown
    // switch, and refused the whole line -- exit 3, before any step, hence not
    // even a failure log.  The default install directory contains a space, so
    // this failed for EVERY standard installation.
    {
        char cmd[GC_UPDATE_COMMAND_LINE_MAX_CHARS] = {};
        if (!gc_update_build_installer_command_line(
                "C:\\ProgramData\\Green Curve\\updates\\setup.exe",
                "C:\\Program Files\\Green Curve", false, nullptr,
                cmd, sizeof(cmd))) return 4260;
        // Both space-bearing paths must be quoted, and the directory must use
        // the `--dir <value>` form: `/D=` is positional by NSIS convention and
        // that convention does not survive argv splitting.
        if (!strstr(cmd, "\"C:\\ProgramData\\Green Curve\\updates\\setup.exe\""))
            return 4261;
        if (!strstr(cmd, "--dir \"C:\\Program Files\\Green Curve\"")) return 4262;
        if (!strstr(cmd, " /S ")) return 4263;
        if (!strstr(cmd, "--no-launch")) return 4264;
        // `/D=` must not reappear: it is the form that broke.
        if (strstr(cmd, "/D=")) return 4265;

        // The relaunch flag is always passed EXPLICITLY rather than left to
        // silent mode's default, and it follows something the service measured:
        // how many GUI processes it actually closed before starting setup.
        char relaunch[GC_UPDATE_COMMAND_LINE_MAX_CHARS] = {};
        if (!gc_update_build_installer_command_line(
                "C:\\ProgramData\\Green Curve\\updates\\setup.exe",
                "C:\\Program Files\\Green Curve", true, "7",
                relaunch, sizeof(relaunch)))
            return 4264;
        if (!strstr(relaunch, " --launch ")) return 4264;
        if (!strstr(relaunch, " --launch-session 7 ")) return 4264;
        if (strstr(relaunch, "--no-launch")) return 4264;

        // Paths that cannot be expressed as one argv entry are refused rather
        // than escaped -- this becomes the command line of a SYSTEM process.
        char scratch[GC_UPDATE_COMMAND_LINE_MAX_CHARS] = {};
        if (gc_update_build_installer_command_line(
                "C:\\a\\setup.exe", "C:\\evil\" --dir C:\\elsewhere",
                false, nullptr, scratch, sizeof(scratch))) return 4266;
        // A trailing backslash would escape the closing quote and swallow the
        // rest of the line.
        if (gc_update_build_installer_command_line(
                "C:\\a\\setup.exe", "C:\\Program Files\\Green Curve\\",
                false, nullptr, scratch, sizeof(scratch))) return 4266;
        if (gc_update_build_installer_command_line("C:\\a\\setup.exe", "", false,
                                                    nullptr, scratch, sizeof(scratch)))
            return 4266;
        if (gc_update_build_installer_command_line(nullptr, "C:\\x", false,
                                                    nullptr, scratch, sizeof(scratch)))
            return 4266;
        // A buffer too small fails rather than emitting a truncated command.
        char tiny[24] = {};
        if (gc_update_build_installer_command_line(
                "C:\\ProgramData\\Green Curve\\updates\\setup.exe",
                "C:\\Program Files\\Green Curve", false, nullptr,
                tiny, sizeof(tiny)))
            return 4267;
        if (tiny[0]) return 4267;

#if defined(_WIN32)
        // THE ROUND TRIP that was missing: split the emitted string exactly as
        // Windows will, and feed it to the REAL installer parser.  This is the
        // assertion that would have caught the shipped bug.
        {
            WCHAR wide[GC_UPDATE_COMMAND_LINE_MAX_CHARS] = {};
            if (MultiByteToWideChar(CP_UTF8, 0, cmd, -1, wide,
                                    (int)ARRAY_COUNT(wide)) <= 0) return 4268;
            int wideCount = 0;
            LPWSTR* wideArgv = CommandLineToArgvW(wide, &wideCount);
            if (!wideArgv || wideCount < 2) { if (wideArgv) LocalFree((HLOCAL)wideArgv); return 4268; }

            static char argBuf[16][512];
            const char* argv[16];
            int count = 0;
            for (int i = 1; i < wideCount && count < 16; ++i) {
                if (WideCharToMultiByte(CP_UTF8, 0, wideArgv[i], -1, argBuf[count],
                                        (int)sizeof(argBuf[count]), nullptr, nullptr) <= 0) {
                    LocalFree((HLOCAL)wideArgv);
                    return 4268;
                }
                argv[count] = argBuf[count];
                count++;
            }
            LocalFree((HLOCAL)wideArgv);

            GcInstallerOptions options;
            gc_installer_parse_options(count, argv, &options);
            if (!options.valid) return 4269;
            if (!options.silent) return 4269;
            if (!options.hasDirectory) return 4269;
            // The directory must arrive WHOLE.  `C:\Program` is precisely the
            // shipped bug, and it is a directory that exists, so a weaker
            // assertion here would have passed.
            if (strcmp(options.directory, "C:\\Program Files\\Green Curve") != 0)
                return 4269;
            if (options.launchAfterInstall != GC_TOGGLE_OFF) return 4269;
            if (options.hasLaunchSession) return 4269;
            if (options.mode != GC_INSTALLER_MODE_INSTALL) return 4269;
        }
#endif
    }

    // --- Update restore transaction and launch-session policy (4270-4289) --
    {
        char scratch[GC_UPDATE_COMMAND_LINE_MAX_CHARS] = {};
        if (gc_update_restore_decide("0.23", "0.23", 0) !=
            GC_UPDATE_RESTORE_APPLY) return 4270;
        if (gc_update_restore_decide("0.23", "0.22", 0) !=
            GC_UPDATE_RESTORE_DISCARD) return 4271;
        if (gc_update_restore_decide("0.23", "0.23", -1) !=
            GC_UPDATE_RESTORE_DISCARD) return 4272;
        if (gc_update_restore_decide(
                "0.23", "0.23", GC_UPDATE_RESTORE_MAX_AGE_SECONDS + 1) !=
            GC_UPDATE_RESTORE_DISCARD) return 4273;
        if (gc_update_restore_decide("bad", "0.23", 0) !=
            GC_UPDATE_RESTORE_DISCARD) return 4274;

        // The legacy capture: 0.23 had no version binding at all, so the file
        // its GUI hands to the newer build carries NO expected_version key.
        // Reading that as an invalid version made every real 0.23 machine lose
        // its settings on the first update it ever performs (observed live
        // 2026-08-17 on the 0.23 -> 0.23.1 hop).  Reverting to a plain
        // `!expected.valid` refusal fails 4279 and 4280.
        if (!gc_update_restore_is_legacy_capture("")) return 4279;
        if (!gc_update_restore_is_legacy_capture(nullptr)) return 4279;
        if (gc_update_restore_is_legacy_capture("0.23")) return 4279;
        if (gc_update_restore_decide("", "0.23.1", 0) !=
            GC_UPDATE_RESTORE_APPLY) return 4280;
        if (gc_update_restore_decide(nullptr, "0.23.1", 30) !=
            GC_UPDATE_RESTORE_APPLY) return 4280;
        // ...but freshness is the ENTIRE gate once there is no version to
        // compare, so the legacy window is an hour, not the version-bound day.
        if (gc_update_restore_decide(
                "", "0.23.1", GC_UPDATE_RESTORE_LEGACY_MAX_AGE_SECONDS) !=
            GC_UPDATE_RESTORE_APPLY) return 4281;
        if (gc_update_restore_decide(
                "", "0.23.1", GC_UPDATE_RESTORE_LEGACY_MAX_AGE_SECONDS + 1) !=
            GC_UPDATE_RESTORE_DISCARD) return 4282;
        // A version-bound capture keeps the full day, because the running build
        // already proves the install it was captured for completed.
        if (gc_update_restore_decide(
                "0.23.1", "0.23.1", GC_UPDATE_RESTORE_LEGACY_MAX_AGE_SECONDS + 1) !=
            GC_UPDATE_RESTORE_APPLY) return 4283;
        // An unmeasurable age is a failure, not a young file, in both shapes.
        if (gc_update_restore_decide("", "0.23.1", -1) !=
            GC_UPDATE_RESTORE_DISCARD) return 4284;
        // Corruption must NOT collapse into the legacy case: present-but-junk
        // is still refused, which is what keeps the relaxation to one shape.
        if (gc_update_restore_decide("bad", "0.23.1", 0) !=
            GC_UPDATE_RESTORE_DISCARD) return 4285;
        if (gc_update_restore_decide("", "not-a-version", 0) !=
            GC_UPDATE_RESTORE_DISCARD) return 4286;

        if (!gc_update_session_id_is_quotable("7")) return 4275;
        if (gc_update_session_id_is_quotable("07")) return 4276;
        if (gc_update_session_id_is_quotable("4294967296")) return 4277;
        if (gc_update_session_id_is_quotable("7a")) return 4278;

        const char* valid[] = {"--launch", "--launch-session", "7"};
        const char* noLaunch[] = {"--launch-session", "7"};
        const char* badSession[] = {"--launch", "--launch-session", "7a"};
        const char* tooHigh[] = {"--launch", "--launch-session", "4294967296"};
        GcInstallerOptions options;
        gc_installer_parse_options(3, valid, &options);
        if (!options.valid || !options.hasLaunchSession ||
            options.launchSessionId != 7) return 4279;
        gc_installer_parse_options(2, noLaunch, &options);
        if (options.valid) return 4280;
        gc_installer_parse_options(3, badSession, &options);
        if (options.valid) return 4281;
        gc_installer_parse_options(3, tooHigh, &options);
        if (options.valid) return 4282;
        if (gc_update_build_installer_command_line(
                "C:\\a\\setup.exe", "C:\\x", true, "bad",
                scratch, sizeof(scratch))) return 4283;
    }

    // --- Published update state is a protocol boundary too (4290-4299) ---
    {
        ServiceUpdateState update = {};
        update.intervalSeconds = GC_UPDATE_INTERVAL_DEFAULT_SECONDS;
        if (!validate_service_update_state_for_ipc(&update)) return 4290;
        update.phase = SERVICE_UPDATE_PHASE_FAILED + 1;
        if (validate_service_update_state_for_ipc(&update)) return 4291;
        update.phase = SERVICE_UPDATE_PHASE_IDLE;
        update.decision = GC_UPDATE_DECISION_NO_ASSET + 1;
        if (validate_service_update_state_for_ipc(&update)) return 4292;
        update.decision = GC_UPDATE_DECISION_REJECTED;
        update.autoCheck = GC_UPDATE_AUTO_CHECK_ON + 1;
        if (validate_service_update_state_for_ipc(&update)) return 4293;
        update.autoCheck = GC_UPDATE_AUTO_CHECK_UNSET;
        update.intervalSeconds = GC_UPDATE_INTERVAL_MIN_SECONDS - 1;
        if (validate_service_update_state_for_ipc(&update)) return 4294;
        update.intervalSeconds = GC_UPDATE_INTERVAL_DEFAULT_SECONDS;
        update.updateReserved[0] = 1;
        if (validate_service_update_state_for_ipc(&update)) return 4295;
        update.updateReserved[0] = 0;
        memset(update.availableVersion, 'x', sizeof(update.availableVersion));
        if (validate_service_update_state_for_ipc(&update)) return 4296;
    }

    // --- Telling the user an update exists (4320-4359) -------------------
    //
    // The whole feature's failure mode is SILENCE: a user who is never told is
    // indistinguishable, from inside the program, from one who was told and did
    // nothing.  Nothing else in the build catches that, so these are the only
    // assertions standing between "an update was found" and "and nobody
    // mentioned it".
    {
        // --- Which decisions are worth interrupting somebody for (4320-4329)
        if (gc_update_alert_kind(GC_UPDATE_DECISION_AVAILABLE, true) !=
            GC_UPDATE_ALERT_AVAILABLE) return 4320;
        // The case that had NO surface at all before: newer, but below the
        // release's min_from floor, so the user is stranded until they fetch it
        // by hand.  It was mentioned only inside a dialog nobody had a reason to
        // open, which made the update they most needed to act on the quietest.
        if (gc_update_alert_kind(GC_UPDATE_DECISION_MANUAL_REQUIRED, true) !=
            GC_UPDATE_ALERT_MANUAL) return 4321;
        // Unactionable, therefore silent: no build for this architecture is our
        // packaging fault, and a daily badge the user cannot clear is nagging.
        if (gc_update_alert_kind(GC_UPDATE_DECISION_NO_ASSET, true) !=
            GC_UPDATE_ALERT_NONE) return 4322;
        if (gc_update_alert_kind(GC_UPDATE_DECISION_UP_TO_DATE, true) !=
            GC_UPDATE_ALERT_NONE) return 4323;
        // A REJECTED decision is what a service restart used to leave behind
        // for up to a full interval.  It must never light anything up.
        if (gc_update_alert_kind(GC_UPDATE_DECISION_REJECTED, true) !=
            GC_UPDATE_ALERT_NONE) return 4324;
        // No version text means nothing to render into "Update to ...".
        if (gc_update_alert_kind(GC_UPDATE_DECISION_AVAILABLE, false) !=
            GC_UPDATE_ALERT_NONE) return 4325;

        // --- The tray menu caption (4330-4339) ---------------------------
        char label[96] = {};
        if (!gc_update_tray_menu_label(GC_UPDATE_ALERT_AVAILABLE, "0.30",
                                       label, sizeof(label)) ||
            strcmp(label, "Update to 0.30...") != 0) return 4330;
        // Different words, because clicking this one cannot install anything.
        if (!gc_update_tray_menu_label(GC_UPDATE_ALERT_MANUAL, "0.30",
                                       label, sizeof(label)) ||
            strcmp(label, "Get update 0.30 (manual install)...") != 0) return 4331;
        if (gc_update_tray_menu_label(GC_UPDATE_ALERT_NONE, "0.30",
                                      label, sizeof(label)) || label[0]) return 4332;
        if (gc_update_tray_menu_label(GC_UPDATE_ALERT_AVAILABLE, "",
                                      label, sizeof(label)) || label[0]) return 4333;
        // Refused rather than truncated: half a version number in a menu is
        // worse than letting the button and the tooltip carry the news.
        char tiny[8] = {};
        if (gc_update_tray_menu_label(GC_UPDATE_ALERT_AVAILABLE, "0.30",
                                      tiny, sizeof(tiny)) || tiny[0]) return 4334;
        // The longest version the wire can carry still fits the real buffer.
        if (!gc_update_tray_menu_label(GC_UPDATE_ALERT_MANUAL,
                                       "999999.999999.999999",
                                       label, sizeof(label))) return 4335;

        // --- The tooltip (4340-4349) -------------------------------------
        char suffix[64] = {};
        if (!gc_update_tray_tooltip_suffix(GC_UPDATE_ALERT_AVAILABLE, "0.30",
                                           suffix, sizeof(suffix)) ||
            strcmp(suffix, " | Update 0.30") != 0) return 4340;
        if (!gc_update_tray_tooltip_suffix(GC_UPDATE_ALERT_MANUAL, "0.30",
                                           suffix, sizeof(suffix)) ||
            strcmp(suffix, " | Update 0.30 (manual)") != 0) return 4341;
        if (gc_update_tray_tooltip_suffix(GC_UPDATE_ALERT_NONE, "0.30",
                                          suffix, sizeof(suffix)) ||
            suffix[0]) return 4342;

        // Composition, at the real NOTIFYICONDATA szTip bound.
        char tip[128] = {};
        gc_update_compose_tray_tooltip("Green Curve - OC | Profile 1",
                                       " | Update 0.30", tip, sizeof(tip));
        if (strcmp(tip, "Green Curve - OC | Profile 1 | Update 0.30") != 0) return 4343;
        // No update: the tooltip is exactly what it always was, byte for byte.
        gc_update_compose_tray_tooltip("Green Curve - OC | Profile 1", "",
                                       tip, sizeof(tip));
        if (strcmp(tip, "Green Curve - OC | Profile 1") != 0) return 4344;
        // THE INVERSION.  A user-chosen profile name can fill the tooltip on its
        // own, and the ordinary text describes a state the window also shows --
        // whereas the suffix is the only passive notice an update exists.  So
        // the base is what gets truncated, never the suffix.
        {
            char longBase[200] = {};
            memset(longBase, 'A', sizeof(longBase) - 1);
            gc_update_compose_tray_tooltip(longBase, " | Update 0.30",
                                           tip, sizeof(tip));
            size_t tipLen = strlen(tip);
            if (tipLen != sizeof(tip) - 1) return 4345;
            if (strcmp(tip + tipLen - strlen(" | Update 0.30"),
                       " | Update 0.30") != 0) return 4346;
        }
        // Degenerate: a suffix that cannot fit at all still leaves a terminated
        // tooltip rather than an unterminated one going to Shell_NotifyIcon.
        {
            char small[8] = {};
            gc_update_compose_tray_tooltip("Green Curve", " | Update 0.30",
                                           small, sizeof(small));
            if (strlen(small) >= sizeof(small)) return 4347;
            if (strcmp(small, "Green C") != 0) return 4348;
        }

        // --- The once-per-machine question (4350-4359) -------------------
        // Asked exactly when the setting has never been answered, the service
        // told us so, and there is a window in front of the user to ask in.
        if (!gc_update_should_prompt_auto_check(GC_UPDATE_AUTO_CHECK_UNSET,
                                                true, true, false)) return 4350;
        // Answered already -- either way -- is never asked again.
        if (gc_update_should_prompt_auto_check(GC_UPDATE_AUTO_CHECK_ON,
                                               true, true, false)) return 4351;
        if (gc_update_should_prompt_auto_check(GC_UPDATE_AUTO_CHECK_OFF,
                                               true, true, false)) return 4352;
        // No service answer: the question would be one whose answer we cannot
        // record, which spends the single prompt for nothing.
        if (gc_update_should_prompt_auto_check(GC_UPDATE_AUTO_CHECK_UNSET,
                                               false, true, false)) return 4353;
        // A logon start goes straight to the tray.  Ambushing a user who is
        // watching their desktop appear is how an updater earns distrust.
        if (gc_update_should_prompt_auto_check(GC_UPDATE_AUTO_CHECK_UNSET,
                                               true, false, false)) return 4354;
        // Already asked in this process: a service that could not save the
        // answer must not turn the next poll tick into a second dialog.
        if (gc_update_should_prompt_auto_check(GC_UPDATE_AUTO_CHECK_UNSET,
                                               true, true, true)) return 4355;
    }

    // --- The persisted last-check time (4360-4369) -----------------------
    //
    // Two 32-bit halves, because the config store is int-valued.  Asserted
    // because the failure has NO visible symptom: a mis-joined timestamp does
    // not crash, does not fail a build and does not log -- it just makes the
    // updater stop checking (a far-future value never comes due) or check on
    // every tick (a negative one always is).  Either way the first person to
    // notice is a user who stopped getting updates.
    {
        int high = 0, low = 0;
        const long long kCases[] = {
            0,                    // never checked
            1,                    // the epoch's first second
            1760000000LL,         // a real timestamp, comfortably inside int
            0x7FFFFFFFLL,         // the last one a signed int holds
            0x80000000LL,         // 2038: the low half no longer fits an int
            0xFFFFFFFFLL,         // the low half exactly saturated
            0x100000000LL,        // the first value needing the high half
            0x123456789ALL,       // both halves populated
        };
        for (size_t i = 0; i < sizeof(kCases) / sizeof(kCases[0]); ++i) {
            gc_update_split_timestamp(kCases[i], &high, &low);
            if (gc_update_join_timestamp(high, low) != kCases[i]) return 4360;
        }
        // A negative time is not a time, and is normalized at the WRITE end so
        // a corrupted value cannot round-trip back into itself.
        gc_update_split_timestamp(-1, &high, &low);
        if (high != 0 || low != 0) return 4361;
        if (gc_update_join_timestamp(high, low) != 0) return 4362;
        // 2038 specifically: the low half is stored as the negative int with
        // the same bits, which is the whole reason the read reinterprets it as
        // unsigned rather than sign-extending.
        gc_update_split_timestamp(0x80000000LL, &high, &low);
        if (high != 0 || low >= 0) return 4363;
        if (gc_update_join_timestamp(high, low) != 0x80000000LL) return 4364;
        // A hand-edited pair naming a time no clock reaches reads as "never",
        // which is always due -- the safe direction.  The alternative is an
        // updater that waits for a moment that never arrives.
        if (gc_update_join_timestamp(-1, -1) != 0) return 4365;
        // And "never checked" is due, which is what makes that safe.
        if (!gc_update_check_is_due(gc_update_join_timestamp(-1, -1),
                                    1760000000LL,
                                    GC_UPDATE_INTERVAL_DEFAULT_SECONDS)) return 4366;
    }

    // --- The response read loops (4370-4399) -----------------------------
    //
    // Reachable at all only because the transport sits behind a seam.  These
    // loops run in a LocalSystem service against bytes chosen by whatever the
    // redirect chain ended at, and their central property -- refusing an
    // oversized body BEFORE writing it -- has no symptom when it regresses:
    // a late check still refuses the update, the digest still fails, nothing
    // logs anything unusual.  It just writes the attacker's whole response to
    // disk first.
    {
        static const unsigned char kBody[] =
            "format=1\nversion=0.30\nx64_size=1\n";
        const size_t kBodyLen = sizeof(kBody) - 1;

        // --- Small documents: manifest and signature (4370-4379) ---------
        {
            FakeHttpBody body = {};
            body.bytes = kBody;
            body.length = kBodyLen;
            GcUpdateReader reader = {};
            reader.available = fake_http_available;
            reader.read = fake_http_read;
            reader.ctx = &body;

            char out[256] = {};
            size_t outLen = 0;
            if (gc_update_read_document(&reader, out, sizeof(out), &outLen) !=
                GC_UPDATE_FETCH_OK) return 4370;
            if (outLen != kBodyLen || memcmp(out, kBody, kBodyLen) != 0) return 4371;
            // Always terminated: every consumer treats this as a C string, and
            // the signature is verified over exactly `outLen` bytes.
            if (out[outLen] != '\0') return 4372;

            // Arriving in several server-chosen pieces must produce the same
            // bytes.  A loop that trusted the first announcement as the whole
            // body would pass the test above and fail this one.
            FakeHttpBody chunked = {};
            chunked.bytes = kBody;
            chunked.length = kBodyLen;
            chunked.announce = 7;
            reader.ctx = &chunked;
            memset(out, 0, sizeof(out));
            if (gc_update_read_document(&reader, out, sizeof(out), &outLen) !=
                GC_UPDATE_FETCH_OK) return 4373;
            if (outLen != kBodyLen || memcmp(out, kBody, kBodyLen) != 0) return 4374;

            // The ceiling is the CALLER's buffer, never anything the server
            // said. One byte too large is refused, not truncated -- a truncated
            // manifest could parse as a valid but different one.
            FakeHttpBody big = {};
            big.bytes = kBody;
            big.length = kBodyLen;
            reader.ctx = &big;
            char tight[8] = {};
            if (gc_update_read_document(&reader, tight, sizeof(tight), &outLen) !=
                GC_UPDATE_FETCH_TOO_LARGE) return 4375;
            // Refused before a single byte was copied: the check runs on the
            // announcement, so `read` is never reached.
            if (big.readCalls != 0) return 4376;
            // Exactly filling the buffer, terminator included, still succeeds.
            char exact[sizeof(kBody)] = {};
            FakeHttpBody fits = {};
            fits.bytes = kBody;
            fits.length = kBodyLen;
            reader.ctx = &fits;
            if (gc_update_read_document(&reader, exact, sizeof(exact), &outLen) !=
                GC_UPDATE_FETCH_OK) return 4377;

            // A 200 with no body is a failure, not an empty manifest.
            FakeHttpBody empty = {};
            empty.bytes = kBody;
            empty.length = 0;
            reader.ctx = &empty;
            if (gc_update_read_document(&reader, out, sizeof(out), &outLen) !=
                GC_UPDATE_FETCH_EMPTY) return 4378;

            // Transport faults are distinguished from each other, because each
            // is a different sentence in the log and "the update did nothing"
            // is otherwise undiagnosable.
            FakeHttpBody queryFails = {};
            queryFails.bytes = kBody;
            queryFails.length = kBodyLen;
            queryFails.failAvailableAt = 1;
            reader.ctx = &queryFails;
            if (gc_update_read_document(&reader, out, sizeof(out), &outLen) !=
                GC_UPDATE_FETCH_QUERY_FAILED) return 4379;
        }

        // --- Streaming the asset (4380-4399) -----------------------------
        {
            unsigned char payload[512] = {};
            for (size_t i = 0; i < sizeof(payload); ++i)
                payload[i] = (unsigned char)(i & 0xFF);
            unsigned char chunk[64] = {};

            FakeHttpBody body = {};
            body.bytes = payload;
            body.length = sizeof(payload);
            body.announce = 100;   // deliberately not a multiple of the chunk
            GcUpdateReader reader = {};
            reader.available = fake_http_available;
            reader.read = fake_http_read;
            reader.ctx = &body;
            FakeSink sink = {};
            GcUpdateSink out = {};
            out.write = fake_sink_write;
            out.ctx = &sink;

            unsigned long long total = 0;
            if (gc_update_stream_asset(&reader, &out, sizeof(payload), chunk,
                                       sizeof(chunk), &total) !=
                GC_UPDATE_FETCH_OK) return 4380;
            if (total != sizeof(payload) || sink.written != sizeof(payload)) return 4381;
            // An announcement larger than the scratch buffer is split rather
            // than overrunning it.
            if (body.readCalls <= (int)(sizeof(payload) / sizeof(chunk)) - 1) return 4382;

            // ***THE ONE THIS SEAM EXISTS FOR***
            //
            // The server sends more than the signed manifest declared. The
            // abort must happen BEFORE the overflowing chunk is written, so
            // the sink must hold no more than the declared size -- a loop that
            // checked `total` at the end would have written all 512 bytes here
            // and still returned a refusal, passing any test that only looked
            // at the return value.
            const unsigned long long kDeclared = 200;
            FakeHttpBody oversize = {};
            oversize.bytes = payload;
            oversize.length = sizeof(payload);
            oversize.announce = 64;
            reader.ctx = &oversize;
            FakeSink oversizeSink = {};
            out.ctx = &oversizeSink;
            total = 0;
            if (gc_update_stream_asset(&reader, &out, kDeclared, chunk,
                                       sizeof(chunk), &total) !=
                GC_UPDATE_FETCH_TOO_LARGE) return 4383;
            if (oversizeSink.written > kDeclared) return 4384;
            // And the stream stopped: the rest of the response was never even
            // read, so a hostile server cannot spend the service's time either.
            if (oversize.offset >= sizeof(payload)) return 4385;
            // The caller logs this to say where the abort happened; zero would
            // make the diagnostic that proves it worked look like it never ran.
            if (total != oversizeSink.written) return 4386;

            // Short: fewer bytes than the manifest declared. Caught here rather
            // than left to the digest, so the log says "the server sent less
            // than declared" instead of "the hash did not match".
            FakeHttpBody shortBody = {};
            shortBody.bytes = payload;
            shortBody.length = 100;
            reader.ctx = &shortBody;
            FakeSink shortSink = {};
            out.ctx = &shortSink;
            total = 0;
            if (gc_update_stream_asset(&reader, &out, sizeof(payload), chunk,
                                       sizeof(chunk), &total) !=
                GC_UPDATE_FETCH_SIZE_MISMATCH) return 4387;
            if (total != 100) return 4388;

            // A signed manifest naming an impossible size is refused before the
            // body is touched at all.
            FakeHttpBody untouched = {};
            untouched.bytes = payload;
            untouched.length = sizeof(payload);
            reader.ctx = &untouched;
            FakeSink noSink = {};
            out.ctx = &noSink;
            if (gc_update_stream_asset(&reader, &out, 0, chunk, sizeof(chunk),
                                       &total) !=
                GC_UPDATE_FETCH_BAD_EXPECTED_SIZE) return 4389;
            if (gc_update_stream_asset(&reader, &out, GC_UPDATE_ASSET_MAX_BYTES + 1,
                                       chunk, sizeof(chunk), &total) !=
                GC_UPDATE_FETCH_BAD_EXPECTED_SIZE) return 4390;
            if (untouched.availableCalls != 0) return 4391;

            // A read that returns zero with data still pending is a stall, not
            // an end of body -- the distinction that keeps a truncated transfer
            // from being staged as a complete one.
            FakeHttpBody stalled = {};
            stalled.bytes = payload;
            stalled.length = sizeof(payload);
            stalled.announce = 64;
            stalled.shortReadAt = 2;
            reader.ctx = &stalled;
            FakeSink stalledSink = {};
            out.ctx = &stalledSink;
            if (gc_update_stream_asset(&reader, &out, sizeof(payload), chunk,
                                       sizeof(chunk), &total) !=
                GC_UPDATE_FETCH_READ_FAILED) return 4392;

            // A failing sink (a full disk mid-download) stops the transfer
            // rather than being counted as written.
            FakeHttpBody writeFail = {};
            writeFail.bytes = payload;
            writeFail.length = sizeof(payload);
            writeFail.announce = 64;
            reader.ctx = &writeFail;
            FakeSink failing = {};
            failing.failWriteAt = 3;
            out.ctx = &failing;
            if (gc_update_stream_asset(&reader, &out, sizeof(payload), chunk,
                                       sizeof(chunk), &total) !=
                GC_UPDATE_FETCH_WRITE_FAILED) return 4393;
            if (failing.written != 2 * sizeof(chunk)) return 4394;
        }
    }

    // --- The tray notification (4400-4419) ---------------------------------
    //
    // The tray icon carried no unprompted signal at all: the tooltip suffix
    // needs a hover and the orange button needs the main window open, so a user
    // running minimised was told nothing unless they right-clicked the icon.
    // Reported live 2026-08-17.  These assert the edge and the wording.
    {
        char title[64] = {};
        char body[256] = {};

        // Fires on the NONE -> alerting edge, once.
        if (!gc_update_should_notify(GC_UPDATE_ALERT_NONE,
                                     GC_UPDATE_ALERT_AVAILABLE, true, false))
            return 4400;
        if (!gc_update_should_notify(GC_UPDATE_ALERT_NONE,
                                     GC_UPDATE_ALERT_MANUAL, true, false))
            return 4401;
        // Not on a level, and not on a downward edge.
        if (gc_update_should_notify(GC_UPDATE_ALERT_AVAILABLE,
                                    GC_UPDATE_ALERT_AVAILABLE, true, false))
            return 4402;
        if (gc_update_should_notify(GC_UPDATE_ALERT_AVAILABLE,
                                    GC_UPDATE_ALERT_NONE, true, false))
            return 4403;
        // Once per process: a check that fails and then succeeds must not
        // announce the same release twice.
        if (gc_update_should_notify(GC_UPDATE_ALERT_NONE,
                                    GC_UPDATE_ALERT_AVAILABLE, true, true))
            return 4404;
        // No tray icon: refused, and the caller must be able to try again on a
        // later tick, which is why this is a separate input from `already`.
        if (gc_update_should_notify(GC_UPDATE_ALERT_NONE,
                                    GC_UPDATE_ALERT_AVAILABLE, false, false))
            return 4405;

        // The wording distinguishes the two alert kinds, because offering
        // "ready to install" for a MANUAL_REQUIRED release would promise
        // something the updater then refuses to do.
        if (!gc_update_compose_notification(GC_UPDATE_ALERT_AVAILABLE, "0.30",
                                            title, sizeof(title), body,
                                            sizeof(body))) return 4406;
        if (!strstr(body, "0.30")) return 4407;
        if (!strstr(body, "ready to install")) return 4408;
        if (strstr(body, "manually")) return 4409;

        if (!gc_update_compose_notification(GC_UPDATE_ALERT_MANUAL, "0.30",
                                            title, sizeof(title), body,
                                            sizeof(body))) return 4410;
        if (!strstr(body, "0.30")) return 4411;
        if (!strstr(body, "manually")) return 4412;
        if (strstr(body, "ready to install")) return 4413;

        // Nothing to say without an alert or without a version, and the buffers
        // are emptied rather than left holding the previous call's text.
        if (gc_update_compose_notification(GC_UPDATE_ALERT_NONE, "0.30", title,
                                           sizeof(title), body, sizeof(body)))
            return 4414;
        if (title[0] || body[0]) return 4415;
        if (gc_update_compose_notification(GC_UPDATE_ALERT_AVAILABLE, "", title,
                                           sizeof(title), body, sizeof(body)))
            return 4416;
        if (title[0] || body[0]) return 4417;

        // Refused rather than truncated: a half-written version number in a
        // notification is worse than no notification, and every other surface
        // still carries the news.
        char tiny[8] = {};
        if (gc_update_compose_notification(GC_UPDATE_ALERT_AVAILABLE, "0.30",
                                           title, sizeof(title), tiny,
                                           sizeof(tiny))) return 4418;
        if (tiny[0]) return 4419;
    }

    // --- Channel trust: replay and suppression (4420-4449) -----------------
    //
    // The one attack a signing key cannot answer: an attacker with network
    // position replays an OLD signed manifest, which verifies, parses and
    // reports "up to date" while a fixed release exists. Or drops the traffic
    // entirely, and the user is pinned on a vulnerable build in silence.
    // A signed `issued=` field would be the textbook fix and is unavailable --
    // the manifest parser refuses unknown keys, in 0.23 too, so any new field
    // permanently breaks every deployed client.
    {
        char mark[GC_UPDATE_VERSION_MAX_CHARS] = {};
        char text[256] = {};

        // The high-water mark only ever goes up.
        if (!gc_update_channel_note_version("", "0.23", mark, sizeof(mark)))
            return 4420;
        if (strcmp(mark, "0.23") != 0) return 4421;
        if (!gc_update_channel_note_version("0.23", "0.30", mark, sizeof(mark)))
            return 4422;
        if (strcmp(mark, "0.30") != 0) return 4423;
        if (!gc_update_channel_note_version("0.30", "0.23", mark, sizeof(mark)))
            return 4424;
        if (strcmp(mark, "0.30") != 0) return 4425;
        // Junk on either side never destroys a good mark.
        if (!gc_update_channel_note_version("0.30", "nonsense", mark, sizeof(mark)))
            return 4426;
        if (strcmp(mark, "0.30") != 0) return 4427;
        if (gc_update_channel_note_version("", "", mark, sizeof(mark))) return 4428;

        // A channel that goes backwards is the replay signal.
        if (gc_update_channel_state(GC_UPDATE_AUTO_CHECK_ON, 1000, 1000, "0.30",
                                    "0.23") != GC_UPDATE_CHANNEL_REGRESSED)
            return 4429;
        // ...and it outranks staleness, which would otherwise bury it.
        if (gc_update_channel_state(GC_UPDATE_AUTO_CHECK_ON, 1,
                                    1 + GC_UPDATE_CHANNEL_STALE_SECONDS * 2,
                                    "0.30", "0.23") != GC_UPDATE_CHANNEL_REGRESSED)
            return 4430;
        // Equal and newer are both fine.
        if (gc_update_channel_state(GC_UPDATE_AUTO_CHECK_ON, 1000, 1000, "0.30",
                                    "0.30") != GC_UPDATE_CHANNEL_OK)
            return 4431;
        if (gc_update_channel_state(GC_UPDATE_AUTO_CHECK_ON, 1000, 1000, "0.30",
                                    "0.31") != GC_UPDATE_CHANNEL_OK)
            return 4432;

        // Silence, but only when the machine was supposed to be listening.
        long long now = 10 + GC_UPDATE_CHANNEL_STALE_SECONDS * 2;
        if (gc_update_channel_state(GC_UPDATE_AUTO_CHECK_ON, 10, now, "", "") !=
            GC_UPDATE_CHANNEL_STALE) return 4433;
        if (gc_update_channel_state(GC_UPDATE_AUTO_CHECK_OFF, 10, now, "", "") !=
            GC_UPDATE_CHANNEL_OK) return 4434;
        if (gc_update_channel_state(GC_UPDATE_AUTO_CHECK_UNSET, 10, now, "", "") !=
            GC_UPDATE_CHANNEL_OK) return 4435;
        // Never checked is new, not stale.
        if (gc_update_channel_state(GC_UPDATE_AUTO_CHECK_ON, 0, now, "", "") !=
            GC_UPDATE_CHANNEL_OK) return 4436;
        // A clock that went backwards is a broken clock, not an attack.
        if (gc_update_channel_state(GC_UPDATE_AUTO_CHECK_ON, now, 10, "", "") !=
            GC_UPDATE_CHANNEL_OK) return 4437;
        // Exactly at the threshold is not yet stale; one second past it is.
        if (gc_update_channel_state(GC_UPDATE_AUTO_CHECK_ON, 0 + 1,
                                    1 + GC_UPDATE_CHANNEL_STALE_SECONDS, "", "") !=
            GC_UPDATE_CHANNEL_OK) return 4438;
        if (gc_update_channel_state(GC_UPDATE_AUTO_CHECK_ON, 1,
                                    2 + GC_UPDATE_CHANNEL_STALE_SECONDS, "", "") !=
            GC_UPDATE_CHANNEL_STALE) return 4439;

        // Wording: describes what was observed, never the inference. Telling a
        // user they are under attack when the maintainer withdrew a build is
        // how the next warning gets ignored.
        if (!gc_update_channel_warning(GC_UPDATE_CHANNEL_REGRESSED, text,
                                       sizeof(text))) return 4440;
        if (!strstr(text, "older release")) return 4441;
        if (strstr(text, "attack")) return 4442;
        if (!gc_update_channel_warning(GC_UPDATE_CHANNEL_STALE, text, sizeof(text)))
            return 4443;
        if (!strstr(text, "month")) return 4444;
        if (gc_update_channel_warning(GC_UPDATE_CHANNEL_OK, text, sizeof(text)))
            return 4445;
        if (text[0]) return 4446;

        // The channel balloon is independent of the update balloon, because a
        // suppression attack produces alert == NONE and would otherwise be the
        // one thing the user is never told.
        if (!gc_update_should_notify_channel(GC_UPDATE_CHANNEL_STALE, true, false))
            return 4447;
        if (gc_update_should_notify_channel(GC_UPDATE_CHANNEL_OK, true, false))
            return 4448;
        if (gc_update_should_notify_channel(GC_UPDATE_CHANNEL_STALE, false, false))
            return 4449;
        char ctitle[64] = {};
        char cbody[256] = {};
        if (!gc_update_compose_channel_notification(GC_UPDATE_CHANNEL_REGRESSED,
                                                    ctitle, sizeof(ctitle), cbody,
                                                    sizeof(cbody))) return 4420;
        if (!ctitle[0] || !cbody[0]) return 4421;
    }

    // --- What may live in the machine config directory (4450-4469) --------
    //
    // service_cleanup_legacy_programdata() sweeps %ProgramData%\Green Curve on
    // every service start and used to preserve shared-profiles.ini alone, i.e.
    // "delete every file I do not recognise". The update-manifest cache lives
    // in that directory by design, so it was deleted on every restart before it
    // could ever be read -- silently, because a missing cache is the documented
    // ordinary state. `update cache: restored` had never once appeared in a log
    // on any machine. Reverting to the single-name check fails 4452.
    {
        const char* kConfig = "shared-profiles.ini";
        if (!gc_machine_dir_file_is_current(kConfig, kConfig)) return 4450;
        if (!gc_machine_dir_file_is_current("update-manifest.cache", kConfig))
            return 4451;
        if (!gc_machine_dir_file_is_current("update-manifest.cache.sig", kConfig))
            return 4452;
        // Legacy artifacts are still swept -- that is what the function is for.
        if (gc_machine_dir_file_is_current("service_boot_start.bin", kConfig))
            return 4453;
        if (gc_machine_dir_file_is_current("greencurve_debug.txt", kConfig))
            return 4454;
        if (gc_machine_dir_file_is_current("crash.dmp", kConfig)) return 4455;
        if (gc_machine_dir_file_is_current("", kConfig)) return 4456;
        if (gc_machine_dir_file_is_current(nullptr, kConfig)) return 4457;
        // Windows filenames are case-insensitive: a sweeper matching only the
        // exact case would delete the file it had just written under another.
        if (!gc_machine_dir_file_is_current("Update-Manifest.Cache", kConfig))
            return 4458;
        if (!gc_machine_dir_file_is_current("SHARED-PROFILES.INI", kConfig))
            return 4459;
        // A prefix must not be mistaken for the whole name.
        if (gc_machine_dir_file_is_current("update-manifest.cache.bak", kConfig))
            return 4460;
        if (gc_machine_dir_file_is_current("update-manifest", kConfig)) return 4461;
    }

    // --- XBAR ClkDomains version-checked schema (4500-4519) ----------------
    {
        static unsigned char templateBuf[XBAR_CONTROL_BUF_SIZE];
        static unsigned char stockTemplate[XBAR_CONTROL_BUF_SIZE];
        static unsigned char lastWrite[XBAR_CONTROL_BUF_SIZE];
        static int setCalls = 0;
        static bool corruptReadback = false;
        memset(templateBuf, 0, sizeof(templateBuf));
        xbar_put_u32(templateBuf, 0, XBAR_NVAPI_CLK_DOMAINS_VERSION);
        xbar_put_u32(templateBuf, XBAR_CONTROL_MASK_OFFSET,
                     XBAR_CONTROL_DOMAIN_MASK);
        const unsigned int base = 0x124;
        const unsigned int stride = 0x304;
        for (unsigned int i = 0; i < 8; ++i)
            xbar_put_u32(templateBuf, base + i * stride, XBAR_DOMAIN_MARKER);
        xbar_put_i32(templateBuf, base + stride + XBAR_FREQ_OFFSET_FIELD, 60000);
        xbar_put_i32(templateBuf, base + stride + XBAR_MSVDD_OFFSET_FIELD, 20000);
        memcpy(stockTemplate, templateBuf, sizeof(stockTemplate));
        auto fakeGet = [](void*, void* payload) -> int {
            if (!payload) return -1;
            memcpy(payload, templateBuf, sizeof(templateBuf));
            if (corruptReadback)
                xbar_put_i32((unsigned char*)payload,
                             base + stride + XBAR_FREQ_OFFSET_FIELD, 1);
            return 0;
        };
        auto fakeSet = [](void*, void* payload) -> int {
            if (!payload) return -1;
            memcpy(lastWrite, payload, sizeof(lastWrite));
            // Model the driver retaining the complete submitted block.
            memcpy(templateBuf, payload, sizeof(templateBuf));
            ++setCalls;
            return 0;
        };
        auto fakeMeasure = [](void*, void* payload) -> int {
            if (!payload) return -1;
            unsigned int* p = (unsigned int*)payload;
            p[2] = 1530832;
            return 0;
        };
        NvApiFunc get = (NvApiFunc)+fakeGet;
        NvApiFunc set = (NvApiFunc)+fakeSet;
        NvApiFunc measure = (NvApiFunc)+fakeMeasure;
        int fakeGpuHandle = 0;
        void* gpu = &fakeGpuHandle;
        XbarControlSnapshot snap{};
        if (!xbar_probe(get, measure, gpu, &snap)) return 4500;
        if (snap.entryBase != base || snap.entryStride != stride) return 4501;
        if (snap.domainIndex != 1 || snap.freqOffsetKhz != 60000 ||
            snap.msvddOffsetUv != 20000 || snap.measuredKhz != 1530832)
            return 4502;
        if (snap.versionWord != XBAR_NVAPI_CLK_DOMAINS_VERSION ||
            snap.schemaStatus != XBAR_SCHEMA_STATUS_OK)
            return 4520;
        if (snap.freqFieldOffset != base + stride + XBAR_FREQ_OFFSET_FIELD ||
            snap.msvddFieldOffset != base + stride + XBAR_MSVDD_OFFSET_FIELD)
            return 4521;
        setCalls = 0;
        corruptReadback = false;
        if (!xbar_write(get, set, measure, gpu, &snap,
                        450000, 10000, true, true)) return 4503;
        if (setCalls != 1 || snap.freqOffsetKhz != 450000 ||
            snap.msvddOffsetUv != 10000) return 4504;
        unsigned int writtenBase = snap.entryBase +
            snap.domainIndex * snap.entryStride;
        if ((int)xbar_get_u32(lastWrite, writtenBase + XBAR_FREQ_OFFSET_FIELD) != 450000 ||
            (int)xbar_get_u32(lastWrite, writtenBase + XBAR_MSVDD_OFFSET_FIELD) != 10000)
            return 4505;
        xbar_put_i32(lastWrite, writtenBase + XBAR_FREQ_OFFSET_FIELD, 60000);
        xbar_put_i32(lastWrite, writtenBase + XBAR_MSVDD_OFFSET_FIELD, 20000);
        if (memcmp(lastWrite, stockTemplate, sizeof(stockTemplate)) != 0) return 4506;
        corruptReadback = true;
        if (xbar_write(get, set, measure, gpu, &snap,
                       450000, 10000, true, true)) return 4507;
        // The corruption makes the post-set READBACK succeed with a wrong
        // value, so the write refuses at the exact-readback comparison: the
        // snapshot stays valid with OK schema status and carries the driver's
        // actual (corrupted) value.
        if (!snap.valid || snap.schemaStatus != XBAR_SCHEMA_STATUS_OK ||
            snap.freqOffsetKhz != 1) return 4522;
        corruptReadback = false;

        // A future/malformed response with a plausible-looking repeated marker
        // at another offset must leave XBAR unavailable rather than become a
        // candidate preimage for a privileged SET_CONTROL transaction.
        {
            static unsigned char hostile[XBAR_CONTROL_BUF_SIZE];
            memset(hostile, 0, sizeof(hostile));
            xbar_put_u32(hostile, 0, XBAR_NVAPI_CLK_DOMAINS_VERSION);
            xbar_put_u32(hostile, XBAR_CONTROL_MASK_OFFSET,
                         XBAR_CONTROL_DOMAIN_MASK);
            for (unsigned int i = 0; i < 12; ++i)
                xbar_put_u32(hostile, 0x200 + i * 0x180, XBAR_DOMAIN_MARKER);
            XbarBufferLayout badLayout{};
            if (xbar_layout_for_buffer(hostile, &g_xbarSchemas[0], &badLayout))
                return 4508;
            xbar_put_u32(templateBuf, base + 5 * stride,
                         XBAR_DOMAIN_MARKER + 1);
            if (xbar_layout_for_buffer(templateBuf, &g_xbarSchemas[0],
                                       &badLayout)) return 4509;
            xbar_put_u32(templateBuf, base + 5 * stride, XBAR_DOMAIN_MARKER);
        }

        // Schema dispatch is keyed by the REPORTED version word.  The pinned
        // V2 row must resolve with its exact validated geometry, and any other
        // word must resolve to nothing.
        {
            const XbarClkDomainsSchema* known =
                xbar_schema_for_version_word(XBAR_NVAPI_CLK_DOMAINS_VERSION);
            if (!known) return 4516;
            if (known->versionWord != 0x000261A4u ||
                known->entryBase != base || known->entryStride != stride ||
                known->domainCount != 8 || known->entryIndex != 1 ||
                known->entryMarker != XBAR_DOMAIN_MARKER ||
                known->freqOffsetField != XBAR_FREQ_OFFSET_FIELD ||
                known->msvddOffsetField != XBAR_MSVDD_OFFSET_FIELD ||
                known->requestMask != XBAR_CONTROL_DOMAIN_MASK) return 4517;
            if (xbar_schema_for_version_word(0x00011234u) != nullptr)
                return 4518;
            if (xbar_schema_for_version_word(0) != nullptr) return 4519;
        }

        // An older-generation driver that answers with its own unknown schema
        // version must be refused for reads AND never reach SET_CONTROL for a
        // write: the fresh-preimage read inside xbar_write refuses first.
        {
            static unsigned char unknownVer[XBAR_CONTROL_BUF_SIZE];
            memset(unknownVer, 0, sizeof(unknownVer));
            // Plausible-looking V2-shaped body under a hypothetical older
            // version word: the version check alone must refuse it.
            xbar_put_u32(unknownVer, 0, 0x00011234u);
            xbar_put_u32(unknownVer, XBAR_CONTROL_MASK_OFFSET,
                         XBAR_CONTROL_DOMAIN_MASK);
            for (unsigned int i = 0; i < 8; ++i)
                xbar_put_u32(unknownVer, base + i * stride,
                             XBAR_DOMAIN_MARKER);
            xbar_put_i32(unknownVer, base + stride + XBAR_FREQ_OFFSET_FIELD,
                         60000);
            xbar_put_i32(unknownVer, base + stride + XBAR_MSVDD_OFFSET_FIELD,
                         20000);
            auto unknownGet = [](void*, void* payload) -> int {
                if (!payload) return -1;
                memcpy(payload, unknownVer, sizeof(unknownVer));
                return 0;
            };
            NvApiFunc uget = (NvApiFunc)+unknownGet;
            // Snapshots carry the full 0x13000 control buffer: keep them
            // static so several in one function cannot overflow the stack.
            static XbarControlSnapshot usnap;
            memset(&usnap, 0, sizeof(usnap));
            if (xbar_read_control(uget, gpu, &usnap)) return 4510;
            if (usnap.valid) return 4511;
            if (usnap.schemaStatus != XBAR_SCHEMA_STATUS_UNKNOWN_VERSION)
                return 4523;
            static int unknownSetCalls;
            unknownSetCalls = 0;
            auto countingSet = [](void*, void*) -> int {
                ++unknownSetCalls;
                return 0;
            };
            NvApiFunc uset = (NvApiFunc)+countingSet;
            static XbarControlSnapshot wsnap;
            memset(&wsnap, 0, sizeof(wsnap));
            if (xbar_write(uget, uset, measure, gpu, &wsnap,
                           450000, 10000, true, true)) return 4512;
            if (unknownSetCalls != 0) return 4513;
        }

        // A driver that rejects the requested struct version outright must
        // fail the read cleanly and leave the snapshot invalid.
        {
            auto rejectingGet = [](void*, void*) -> int { return -9; };
            NvApiFunc rget = (NvApiFunc)+rejectingGet;
            static XbarControlSnapshot rsnap;
            memset(&rsnap, 0, sizeof(rsnap));
            rsnap.valid = true;  // stale proof must not survive a failed read
            rsnap.schemaStatus = XBAR_SCHEMA_STATUS_OK;
            if (xbar_read_control(rget, gpu, &rsnap)) return 4514;
            if (rsnap.valid) return 4515;
            // -9 is the driver rejecting our struct-version vocabulary: that
            // is unknown-tooling-knowledge, not a domain refusal.
            if (rsnap.schemaStatus != XBAR_SCHEMA_STATUS_UNKNOWN_VERSION)
                return 4524;
        }
    }

    DeleteCriticalSection(&g_configLock);
    // Privacy redaction is a diagnostic contract: stable enough to correlate
    // events, opaque enough that default support logs do not carry raw account,
    // SID/LUID, or user-profile path strings.
    {
        // Updater worker recovery: a transport failure cannot erase a package
        // that is still staged against the latest signed manifest, but any
        // missing precondition must return to FAILED rather than install.
        char a[32] = {};
        char b[32] = {};
        gc_log_identifier_token("alice", a, sizeof(a));
        gc_log_identifier_token("alice", b, sizeof(b));
        if (strcmp(a, b) != 0 || strstr(a, "alice") != nullptr) return 4522;
        gc_log_identifier_token("bob", b, sizeof(b));
        if (strcmp(a, b) == 0) return 4523;
        gc_log_identifier_token("", b, sizeof(b));
        if (strcmp(b, "-") != 0) return 4524;

        gc_log_path_token("%PROGRAMDATA%\\Green Curve\\config.ini", a, sizeof(a));
        gc_log_path_token("%PROGRAMDATA%\\Green Curve\\config.ini", b, sizeof(b));
        if (strcmp(a, b) != 0 || strstr(a, "Green Curve") != nullptr ||
            strncmp(a, "[path #", 7) != 0) return 4525;

        char low[32] = {};
        char high[32] = {};
        gc_log_u64_token(0x1122334455667788ULL, low, sizeof(low));
        gc_log_u64_token(0x8877665544332211ULL, high, sizeof(high));
        if (strcmp(low, high) == 0) return 4531;
        if (strncmp(low, "[id #", 5) != 0) return 4532;

        if (gc_update_failed_check_recovery(true, true, true, true) !=
            GC_UPDATE_FAILED_CHECK_KEEP_READY) return 4527;
        if (gc_update_failed_check_recovery(false, true, true, true) !=
            GC_UPDATE_FAILED_CHECK_MARK_FAILED ||
            gc_update_failed_check_recovery(true, false, true, true) !=
                GC_UPDATE_FAILED_CHECK_MARK_FAILED ||
            gc_update_failed_check_recovery(true, true, false, true) !=
                GC_UPDATE_FAILED_CHECK_MARK_FAILED ||
            gc_update_failed_check_recovery(true, true, true, false) !=
                GC_UPDATE_FAILED_CHECK_MARK_FAILED) return 4528;

        if (gc_update_staged_check_action(true, true) !=
            GC_UPDATE_STAGED_KEEP) return 4529;
        if (gc_update_staged_check_action(false, true) !=
                GC_UPDATE_STAGED_DISCARD ||
            gc_update_staged_check_action(true, false) !=
                GC_UPDATE_STAGED_DISCARD) return 4530;
    }

    // ------------------------------------------------------------------
    // Pipe transport admission policy (service_ipc_throttle_policy.h).
    // Pure boundaries for the transition-safe IPC throttling: classification,
    // wrap-safe time, bucket refill/spend costs, identity table lifecycle,
    // handoff reserve isolation, and the no-permanent-lockout guarantee.
    // All charges inside one section use a fixed timestamp unless a refill
    // interval is exactly what is being measured.
    // ------------------------------------------------------------------
    {
        // Key capacity must mirror the lifecycle identity's SID storage so
        // the transport copy can never truncate a real SID.
        if (ServiceIpcThrottleKey::kSidBytes !=
            (int)sizeof(((ServiceLifecycleIdentity*)nullptr)->sid)) return 4600;

        // Wrap-safe monotonic comparison.
        if (!service_ipc_time_at_or_after(5, 5)) return 4601;
        if (!service_ipc_time_at_or_after(6, 5)) return 4602;
        if (service_ipc_time_at_or_after(5, 6)) return 4603;
        // Classic tick-counter wrap: 5 is "after" MAX-10 (signed diff +15),
        // and MAX-10 is before 5 (signed diff -15).
        if (!service_ipc_time_at_or_after(5, 0ULL - 10)) return 4604;
        if (service_ipc_time_at_or_after(0ULL - 10, 5)) return 4605;

        // Classification: every valid command enum maps to its lane; unknown
        // values (and 0) must never fall into a served class.
        {
            struct ClassCase { unsigned int cmd; ServiceIpcRequestClass cls; };
            const ClassCase cases[] = {
                {1, SERVICE_IPC_CLASS_NORMAL},       // PING
                {2, SERVICE_IPC_CLASS_NORMAL},       // GET_SNAPSHOT
                {3, SERVICE_IPC_CLASS_NORMAL},       // GET_TELEMETRY
                {4, SERVICE_IPC_CLASS_NORMAL},       // APPLY
                {5, SERVICE_IPC_CLASS_LIFECYCLE},    // RESET
                {6, SERVICE_IPC_CLASS_NORMAL},       // GET_ACTIVE_DESIRED
                {7, SERVICE_IPC_CLASS_BULK_OUTPUT},  // WRITE_LOG_SNAPSHOT
                {8, SERVICE_IPC_CLASS_BULK_OUTPUT},  // WRITE_JSON_SNAPSHOT
                {9, SERVICE_IPC_CLASS_BULK_OUTPUT},  // WRITE_PROBE_REPORT
                {10, SERVICE_IPC_CLASS_HANDOFF},     // LOGON_HANDOFF
                {11, SERVICE_IPC_CLASS_NORMAL},      // GET_OPERATION_RESULT
                {12, SERVICE_IPC_CLASS_NORMAL},      // GET_STARTUP_POLICY
                {13, SERVICE_IPC_CLASS_NORMAL},      // SET_STARTUP_POLICY
                {14, SERVICE_IPC_CLASS_NORMAL},      // REFRESH_STARTUP_PROFILE
                {15, SERVICE_IPC_CLASS_NORMAL},      // RESUME_RESTORE
                {16, SERVICE_IPC_CLASS_NORMAL},      // GET_UPDATE_STATE
                {17, SERVICE_IPC_CLASS_NORMAL},      // CHECK_FOR_UPDATE
                {18, SERVICE_IPC_CLASS_LIFECYCLE},   // INSTALL_UPDATE
                {19, SERVICE_IPC_CLASS_LIFECYCLE},   // SET_UPDATE_POLICY
            };
            for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
                if (service_ipc_classify_command(cases[i].cmd) !=
                    cases[i].cls) return 4610;
            }
            if (service_ipc_classify_command(0) !=
                SERVICE_IPC_CLASS_UNKNOWN) return 4611;
            if (service_ipc_classify_command(20) !=
                SERVICE_IPC_CLASS_UNKNOWN) return 4612;
            if (service_ipc_classify_command(0xFFFFFFFFu) !=
                SERVICE_IPC_CLASS_UNKNOWN) return 4613;
        }

        ServiceIpcAdmissionTable table;

        ServiceIpcThrottleKey key;
        key.fill(7, 0x1122334455667788ULL, "S-1-5-21-1-2-3-1001");
        if (!key.valid) return 4620;

        // Burst boundary: a fresh identity sustains exactly BURST/COST
        // completed requests at one instant, then RATE-refuses.
        table.reset(100000);
        if (service_ipc_decide_admission(&table, key,
                SERVICE_IPC_CLASS_NORMAL, 100000) !=
            SERVICE_IPC_ADMITTED) return 4621;
        for (unsigned int i = 0;
             i < SERVICE_IPC_BUCKET_BURST_MILLI / 1000 /
                 SERVICE_IPC_COST_SUCCESS;
             ++i) {
            service_ipc_charge(&table, key, SERVICE_IPC_CLASS_NORMAL,
                SERVICE_IPC_COST_SUCCESS, 100000);
        }
        if (service_ipc_decide_admission(&table, key,
                SERVICE_IPC_CLASS_NORMAL, 100001) !=
            SERVICE_IPC_REJECTED_RATE) return 4622;

        // Refill: one second restores exactly RATE milli-tokens (20
        // requests' worth), so admission recovers -- then refuses again once
        // everything refilled has been drained.
        service_ipc_charge(&table, key, SERVICE_IPC_CLASS_NORMAL,
            SERVICE_IPC_COST_SUCCESS, 101000);
        if (service_ipc_decide_admission(&table, key,
                SERVICE_IPC_CLASS_NORMAL, 101001) !=
            SERVICE_IPC_ADMITTED) return 4623;
        for (;; ) {
            if (service_ipc_decide_admission(&table, key,
                    SERVICE_IPC_CLASS_NORMAL, 101002) !=
                SERVICE_IPC_ADMITTED) break;
            service_ipc_charge(&table, key, SERVICE_IPC_CLASS_NORMAL,
                SERVICE_IPC_COST_SUCCESS, 101002);
        }
        if (service_ipc_decide_admission(&table, key,
                SERVICE_IPC_CLASS_NORMAL, 101003) !=
            SERVICE_IPC_REJECTED_RATE) return 4624;

        // Transport faults cost COST_TRANSPORT_FAULT x SUCCESS: seven stalled
        // bodies still leave one request's worth (80 - 70), an eighth does not.
        table.reset(500000);
        service_ipc_charge(&table, key, SERVICE_IPC_CLASS_NORMAL,
            SERVICE_IPC_COST_TRANSPORT_FAULT * 7, 500000);
        if (service_ipc_decide_admission(&table, key,
                SERVICE_IPC_CLASS_NORMAL, 500001) ==
            SERVICE_IPC_REJECTED_RATE) return 4625;
        service_ipc_charge(&table, key, SERVICE_IPC_CLASS_NORMAL,
            SERVICE_IPC_COST_TRANSPORT_FAULT, 500000);
        if (service_ipc_decide_admission(&table, key,
                SERVICE_IPC_CLASS_NORMAL, 500010) !=
            SERVICE_IPC_REJECTED_RATE) return 4626;

        // Handoff reserve independence: a drained normal bucket must not
        // starve the settings-free logon handoff lane.
        table.reset(700000);
        for (unsigned int i = 0;
             i < SERVICE_IPC_BUCKET_BURST_MILLI / 1000 /
                 SERVICE_IPC_COST_SUCCESS;
             ++i) {
            service_ipc_charge(&table, key, SERVICE_IPC_CLASS_NORMAL,
                SERVICE_IPC_COST_SUCCESS, 700000);
        }
        if (service_ipc_decide_admission(&table, key,
                SERVICE_IPC_CLASS_NORMAL, 700100) !=
            SERVICE_IPC_REJECTED_RATE) return 4627;
        if (service_ipc_decide_admission(&table, key,
                SERVICE_IPC_CLASS_HANDOFF, 700100) !=
            SERVICE_IPC_ADMITTED) return 4628;
        // The reserve itself is bounded: HANDOFF_RESERVE/HANDOFF_COST
        // handoffs at one instant, then refusal until refill.
        for (unsigned int i = 0;
             i < SERVICE_IPC_HANDOFF_RESERVE_MILLI / 1000 /
                 SERVICE_IPC_COST_SUCCESS;
             ++i) {
            service_ipc_charge(&table, key, SERVICE_IPC_CLASS_HANDOFF,
                SERVICE_IPC_COST_SUCCESS, 700100);
        }
        if (service_ipc_decide_admission(&table, key,
                SERVICE_IPC_CLASS_HANDOFF, 700110) !=
            SERVICE_IPC_REJECTED_RATE) return 4629;

        // A new authentication LUID/session starts with fresh quotas: fast
        // user switching must not inherit the previous session's penalties.
        ServiceIpcThrottleKey freshKey;
        freshKey.fill(9, 0x99AABBCCDDEEFF01ULL, "S-1-5-21-1-2-3-1002");
        if (service_ipc_decide_admission(&table, freshKey,
                SERVICE_IPC_CLASS_NORMAL, 700200) !=
            SERVICE_IPC_ADMITTED) return 4630;
        ServiceIpcThrottleKey relaunchKey;
        relaunchKey.fill(7, 0xFEEDFACE00000001ULL, "S-1-5-21-1-2-3-1001");
        if (relaunchKey.equals(key)) return 4631;
        if (service_ipc_decide_admission(&table, relaunchKey,
                SERVICE_IPC_CLASS_HANDOFF, 700200) !=
            SERVICE_IPC_ADMITTED) return 4632;

        // Idle expiry: an entry unused past the idle window reads as fresh.
        table.reset(900000);
        service_ipc_charge(&table, key, SERVICE_IPC_CLASS_NORMAL,
            SERVICE_IPC_COST_BAD_COMMAND * 16, 900000);
        if (service_ipc_decide_admission(&table, key,
                SERVICE_IPC_CLASS_NORMAL, 900001) !=
            SERVICE_IPC_REJECTED_RATE) return 4633;
        if (service_ipc_decide_admission(&table, key,
                SERVICE_IPC_CLASS_NORMAL,
                900000 + SERVICE_IPC_IDLE_EXPIRY_MS + 1) !=
            SERVICE_IPC_ADMITTED) return 4634;

        // Full-table behavior: flooding every slot must not lock anybody out
        // permanently -- the least-recently-seen entry is evicted and a
        // victim reads as fresh again.
        table.reset(1000000);
        char sidText[64];
        ServiceIpcThrottleKey lastKey;
        lastKey.fill(1, 1, "S-1-5-21-1-2-3-LAST");
        service_ipc_charge(&table, lastKey, SERVICE_IPC_CLASS_NORMAL,
            SERVICE_IPC_BUCKET_BURST_MILLI / 1000 / SERVICE_IPC_COST_SUCCESS,
            1000000);
        for (unsigned int i = 0; i < SERVICE_IPC_TABLE_SLOTS + 8; ++i) {
            ServiceIpcThrottleKey flood;
            snprintf(sidText, sizeof(sidText), "S-1-5-21-1-2-3-FLOOD%u", i);
            flood.fill(2 + (i % 5), 0xC0FFEE0000000000ULL + i, sidText);
            if (flood.equals(lastKey)) return 4637;
            service_ipc_charge(&table, flood, SERVICE_IPC_CLASS_NORMAL,
                SERVICE_IPC_COST_TRANSPORT_FAULT, 1000000 + i);
        }
        if (service_ipc_decide_admission(&table, lastKey,
                SERVICE_IPC_CLASS_NORMAL, 1000200) !=
            SERVICE_IPC_ADMITTED) return 4636;

        // Unknown commands are refused regardless of bucket state.
        if (service_ipc_decide_admission(&table, key,
                SERVICE_IPC_CLASS_UNKNOWN, 1100000) !=
            SERVICE_IPC_REJECTED_CAPACITY) return 4638;

        // Invalid keys bypass identity accounting entirely: admission stays
        // open (the transport drops unidentifiable connections on other
        // grounds) while their charges land only in the small anon budget.
        {
            ServiceIpcThrottleKey anonKey;
            anonKey.clear();
            table.reset(1200000);
            for (unsigned int i = 0; i < SERVICE_IPC_ANON_BUDGET_MAX * 2; ++i) {
                service_ipc_charge(&table, anonKey, SERVICE_IPC_CLASS_NORMAL,
                    SERVICE_IPC_COST_SUCCESS, 1200000 + i);
            }
            if (service_ipc_decide_admission(&table, anonKey,
                    SERVICE_IPC_CLASS_NORMAL, 1200050) !=
                SERVICE_IPC_ADMITTED) return 4639;
            // The anonymous budget itself is bounded and refills slowly.
            if (table.anonymousBucket.tokensMilli >
                SERVICE_IPC_ANON_BUDGET_MAX * 1000ULL) return 4640;
        }

        // Exactly ONE charge per finished connection. The runtime glue maps
        // how an exchange ended through service_ipc_connection_cost_tokens();
        // the mapping must be total and refusal-free-of-charge, or refused
        // connections would harvest tokens from their own refusals.
        {
            struct OutcomeCostCase {
                ServiceIpcConnectionOutcome outcome;
                unsigned int costTokens;
            };
            const OutcomeCostCase outcomeCosts[] = {
                {SERVICE_IPC_CONNECTION_EXCHANGED,
                 SERVICE_IPC_COST_SUCCESS},
                {SERVICE_IPC_CONNECTION_PROTOCOL_MISMATCH,
                 SERVICE_IPC_COST_BAD_COMMAND},
                {SERVICE_IPC_CONNECTION_UNKNOWN_COMMAND,
                 SERVICE_IPC_COST_BAD_COMMAND},
                {SERVICE_IPC_CONNECTION_IDENTITY_UNKNOWN,
                 SERVICE_IPC_COST_TRANSPORT_FAULT},
                {SERVICE_IPC_CONNECTION_ADMISSION_REFUSED, 0},
                {SERVICE_IPC_CONNECTION_TRANSPORT_FAULT,
                 SERVICE_IPC_COST_TRANSPORT_FAULT},
            };
            for (size_t i = 0;
                 i < sizeof(outcomeCosts) / sizeof(outcomeCosts[0]); ++i) {
                if (service_ipc_connection_cost_tokens(
                        outcomeCosts[i].outcome) !=
                    outcomeCosts[i].costTokens) return 4641;
            }

            // A refused connection leaves its bucket untouched: charging the
            // zero refusal cost is a strict no-op, not a hidden refill.
            table.reset(1500000);
            service_ipc_charge(&table, key, SERVICE_IPC_CLASS_NORMAL,
                SERVICE_IPC_COST_SUCCESS, 1500000);
            ServiceIpcIdentitySlot* chargedSlot = table.find(
                key, service_ipc_key_hash(key));
            if (!chargedSlot) return 4642;
            unsigned long long balanceAfterSuccess =
                chargedSlot->normalBucket.tokensMilli;
            service_ipc_charge(&table, key, SERVICE_IPC_CLASS_NORMAL,
                service_ipc_connection_cost_tokens(
                    SERVICE_IPC_CONNECTION_ADMISSION_REFUSED), 1500000);
            chargedSlot = table.find(key, service_ipc_key_hash(key));
            if (!chargedSlot ||
                chargedSlot->normalBucket.tokensMilli != balanceAfterSuccess)
                return 4643;
        }
    }

    // ------------------------------------------------------------------
    // Debug log size-cap rotation (debug_log_rotation_policy.h). Pure
    // boundaries for the unbounded-log fix: rotate at/after the cap only,
    // never below it, and never on a misconfigured non-positive cap.
    // ------------------------------------------------------------------
    {
        // The compiled-in cap must stay sane: large enough to be useful
        // history, small enough to bound disk usage.
        if (gc_debug_log_rotation::kRotateBytes < 1 * 1024 * 1024)
            return 4700;
        if (gc_debug_log_rotation::kRotateBytes > 256 * 1024 * 1024)
            return 4701;

        const long long cap = (long long)gc_debug_log_rotation::kRotateBytes;

        // Boundary: below the cap no rotation, exactly at the cap rotates.
        if (gc_debug_log_rotation::should_rotate(cap - 1)) return 4702;
        if (!gc_debug_log_rotation::should_rotate(cap)) return 4703;
        if (!gc_debug_log_rotation::should_rotate(cap + 1)) return 4704;
        if (!gc_debug_log_rotation::should_rotate(cap * 1000)) return 4705;

        // Zero/negative sizes never rotate; a non-positive cap must disable
        // rotation rather than turn into "always rotate".
        if (gc_debug_log_rotation::should_rotate(0, cap)) return 4706;
        if (gc_debug_log_rotation::should_rotate(-1, cap)) return 4707;
        if (gc_debug_log_rotation::should_rotate(cap - 1, 0)) return 4708;
        if (gc_debug_log_rotation::should_rotate(cap, -1)) return 4709;

        // The marker a rotated file opens with: present, newline-terminated,
        // and free of format specifiers so it can never be misread as one.
        {
            const char* marker = gc_debug_log_rotation::marker_line();
            if (!marker || !marker[0]) return 4710;
            if (marker[strlen(marker) - 1] != '\n') return 4711;
            if (strchr(marker, '%') != nullptr) return 4712;
        }
    }

    return 0;
}
