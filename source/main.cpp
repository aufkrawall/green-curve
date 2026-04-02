// Green Curve v0.3 - NVIDIA Blackwell VF Curve Editor
// Win32 GDI application

#include "app_shared.h"
#include "fan_curve.h"

static const char APP_LICENSE_TEXT[] =
    "MIT License\r\n"
    "\r\n"
    "Copyright (c) 2026 aufkrawall\r\n"
    "\r\n"
    "Permission is hereby granted, free of charge, to any person obtaining a copy\r\n"
    "of this software and associated documentation files (the \"Software\"), to deal\r\n"
    "in the Software without restriction, including without limitation the rights\r\n"
    "to use, copy, modify, merge, publish, distribute, sublicense, and/or sell\r\n"
    "copies of the Software, and to permit persons to whom the Software is\r\n"
    "furnished to do so, subject to the following conditions:\r\n"
    "\r\n"
    "The above copyright notice and this permission notice shall be included in all\r\n"
    "copies or substantial portions of the Software.\r\n"
    "\r\n"
    "THE SOFTWARE IS PROVIDED \"AS IS\", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR\r\n"
    "IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,\r\n"
    "FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE\r\n"
    "AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER\r\n"
    "LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,\r\n"
    "OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE\r\n"
    "SOFTWARE.";

static void* nvapi_qi(unsigned int id);
static bool nvapi_read_curve();
static bool nvapi_read_offsets();
static bool nvapi_read_pstates();
static void detect_clock_offsets();
static int uniform_curve_offset_khz();
static int clamp_freq_delta_khz(int freqDelta_kHz);
static bool nvapi_set_point(int pointIndex, int freqDelta_kHz);
static bool apply_curve_offsets_verified(const int* targetOffsets, const bool* pointMask, int maxBatchPasses);
static void close_startup_sync_thread_handle();
static void invalidate_main_window();
static void redraw_window_sync(HWND hwnd);
static void fill_window_background(HWND hwnd, HDC hdc);
static void flush_desktop_composition();
static void show_window_with_primed_first_frame(HWND hwnd, int nCmdShow);
static bool nvapi_set_gpu_offset(int offsetkHz);
static bool nvapi_set_mem_offset(int offsetkHz);
static bool nvapi_set_power_limit(int pct);
static bool activate_existing_instance_window();
static bool acquire_single_instance_mutex();
static void release_single_instance_mutex();
static void rebuild_visible_map();
static unsigned int get_edit_value(HWND hEdit);
static void populate_edits();
static void create_edit_controls(HWND hParent, HINSTANCE hInst);
static void apply_lock(int vi);
static unsigned int get_edit_value(HWND hEdit);
static void set_edit_value(HWND hEdit, unsigned int value);
static void unlock_all();
static int mem_display_mhz_from_driver_khz(int driver_kHz);
static int mem_display_mhz_from_driver_mhz(int driverMHz);
static unsigned int displayed_curve_mhz(unsigned int rawFreq_kHz);
static bool curve_targets_match_request(const DesiredSettings* desired, const bool* lockedTailMask, unsigned int lockMhz, char* detail, size_t detailSize);
static void apply_system_titlebar_theme(HWND hwnd);
static void show_license_dialog(HWND parent);
static void layout_bottom_buttons(HWND hParent);
static void debug_log(const char* fmt, ...);
static bool write_text_file_atomic(const char* path, const char* data, size_t dataSize, char* err, size_t errSize);
static bool write_log_snapshot(const char* path, char* err, size_t errSize);
static bool write_error_report_log(const char* summary, const char* details, char* err, size_t errSize);
static bool save_desired_to_config_with_startup(const char* path, const DesiredSettings* desired, bool useCurrentForUnset, int startupState, char* err, size_t errSize);
// Profile I/O
static bool load_profile_from_config(const char* path, int slot, DesiredSettings* desired, char* err, size_t errSize);
static bool save_profile_to_config(const char* path, int slot, const DesiredSettings* desired, char* err, size_t errSize);
static bool clear_profile_from_config(const char* path, int slot, char* err, size_t errSize);
static bool is_profile_slot_saved(const char* path, int slot);
static void refresh_profile_controls_from_config();
static void migrate_legacy_config_if_needed(const char* path);
static void layout_profile_controls(HWND hParent);
static void merge_desired_settings(DesiredSettings* base, const DesiredSettings* override);
static bool desired_has_any_action(const DesiredSettings* desired);
static bool capture_gui_apply_settings(DesiredSettings* desired, char* err, size_t errSize);
static void set_profile_status_text(const char* fmt, ...);
static void update_profile_state_label();
static void update_profile_action_buttons();
static bool maybe_confirm_profile_load_replace(int slot);
static void maybe_load_app_launch_profile_to_gui();
// Legacy config constants kept for existing save_desired_to_config_with_startup
#define CONFIG_STARTUP_PRESERVE (-1)
#define CONFIG_STARTUP_DISABLE   0
#define CONFIG_STARTUP_ENABLE    1
static bool capture_gui_config_settings(DesiredSettings* desired, char* err, size_t errSize);
static bool set_startup_task_enabled(bool enabled, char* err, size_t errSize);
static bool load_startup_enabled_from_config(const char* path, bool* enabled);
static bool is_startup_task_enabled();
static void sync_logon_combo_from_system();
static void schedule_logon_combo_sync();
static void destroy_backbuffer();

static void detect_locked_tail_from_curve();
static void close_nvml();
static const char* nvml_err_name(nvmlReturn_t r);
static bool nvml_ensure_ready();
static bool nvml_set_fan_auto(char* detail, size_t detailSize);
static bool nvml_set_fan_manual(int pct, bool* exactApplied, char* detail, size_t detailSize);
static void initialize_gui_fan_settings_from_live_state();
static int get_effective_live_fan_mode();
static bool fan_setting_matches_current(int wantMode, int wantPct, const FanCurveConfig* wantCurve);
static void update_fan_controls_enabled_state();
static void update_tray_icon();
static bool ensure_tray_icon();
static void remove_tray_icon();
static void hide_main_window_to_tray();
static void show_main_window_from_tray();
static void show_tray_menu(HWND hwnd);
static bool live_state_has_custom_oc();
static bool live_state_has_custom_fan();
static void stop_fan_curve_runtime();
static void start_fan_curve_runtime();
static void start_fixed_fan_runtime();
static void apply_fan_curve_tick();
static bool nvml_read_temperature(int* temperatureC, char* detail, size_t detailSize);
static bool is_start_on_logon_enabled(const char* path);
static bool set_start_on_logon_enabled(const char* path, bool enabled);
static bool should_enable_startup_task_from_config(const char* path);
static void apply_logon_startup_behavior();
static bool ensure_profile_slot_available_for_auto_action(int slot);
static bool is_gpu_offset_excluded_low_point(int pointIndex, int gpuOffsetMHz);
static int gpu_offset_component_mhz_for_point(int pointIndex, int gpuOffsetMHz, bool excludeLow70);
static bool detect_live_selective_gpu_offset_state(int* gpuOffsetMHzOut);
static int current_applied_gpu_offset_mhz();
static bool current_applied_gpu_offset_excludes_low_points();
static void open_fan_curve_dialog();
static void refresh_fan_curve_button_text();
static bool apply_desired_settings(const DesiredSettings* desired, bool interactive, char* result, size_t resultSize);

struct FanCurveDialogState {
    HWND hwnd;
    HWND enableChecks[FAN_CURVE_MAX_POINTS];
    HWND tempEdits[FAN_CURVE_MAX_POINTS];
    HWND percentEdits[FAN_CURVE_MAX_POINTS];
    HWND intervalCombo;
    HWND hysteresisCombo;
    HWND okButton;
    HWND cancelButton;
    FanCurveConfig working;
};

static FanCurveDialogState g_fanCurveDialog = {};
static HANDLE g_singleInstanceMutex = nullptr;
static const char APP_SINGLE_INSTANCE_MUTEX_NAME[] = "Local\\GreenCurveSingleInstance";

enum {
    FAN_DIALOG_ENABLE_BASE = 6100,
    FAN_DIALOG_TEMP_BASE = 6200,
    FAN_DIALOG_PERCENT_BASE = 6300,
    FAN_DIALOG_INTERVAL_ID = 6400,
    FAN_DIALOG_HYSTERESIS_ID = 6401,
    FAN_DIALOG_OK_ID = 6402,
    FAN_DIALOG_CANCEL_ID = 6403,
};

static const UINT FAN_FIXED_RUNTIME_INTERVAL_MS = 5000;
static const ULONGLONG FAN_RUNTIME_REAPPLY_INTERVAL_MS = 15000;
static const ULONGLONG FAN_RUNTIME_FAILURE_WINDOW_MS = 10000;

static const char* fan_mode_label(int mode) {
    switch (mode) {
        case FAN_MODE_FIXED: return "Fixed Custom";
        case FAN_MODE_CURVE: return "Custom Curve";
        default: return "Default / Auto";
    }
}

static const char* tray_mode_label(bool customOc, bool customFan) {
    if (customOc && customFan) return "OC + Custom Fan";
    if (customOc) return "OC";
    if (customFan) return "Custom Fan";
    return "Default";
}

static bool desired_has_custom_oc(const DesiredSettings* desired) {
    if (!desired) return false;
    if (desired->hasGpuOffset || desired->hasMemOffset || desired->hasPowerLimit) return true;
    for (int i = 0; i < VF_NUM_POINTS; i++) {
        if (desired->hasCurvePoint[i]) return true;
    }
    return false;
}

static const char* desired_fan_mode_label(const DesiredSettings* desired) {
    if (!desired || !desired->hasFan || desired->fanMode == FAN_MODE_AUTO) return nullptr;
    if (desired->fanMode == FAN_MODE_CURVE) return "Custom Fan Curve";
    return "Custom Fan";
}

static void desired_mode_label(const DesiredSettings* desired, char* mode, size_t modeSize) {
    if (!mode || modeSize == 0) return;

    bool customOc = desired_has_custom_oc(desired);
    const char* customFan = desired_fan_mode_label(desired);
    if (customOc && customFan) {
        StringCchPrintfA(mode, modeSize, "OC + %s", customFan);
    } else if (customOc) {
        StringCchCopyA(mode, modeSize, "OC");
    } else if (customFan) {
        StringCchCopyA(mode, modeSize, customFan);
    } else {
        StringCchCopyA(mode, modeSize, "Default");
    }
}

static void resolve_tray_icon_state(bool* customOcOut, bool* customFanOut) {
    bool customOc = live_state_has_custom_oc();
    bool customFan = live_state_has_custom_fan();

    if (g_app.configPath[0] != '\0') {
        int selectedSlot = get_config_int(g_app.configPath, "profiles", "selected_slot", CONFIG_DEFAULT_SLOT);
        if (selectedSlot >= 1 && selectedSlot <= CONFIG_NUM_SLOTS && is_profile_slot_saved(g_app.configPath, selectedSlot)) {
            DesiredSettings desired = {};
            char err[256] = {};
            if (load_profile_from_config(g_app.configPath, selectedSlot, &desired, err, sizeof(err))) {
                customOc = desired_has_custom_oc(&desired);
                customFan = desired.hasFan && desired.fanMode != FAN_MODE_AUTO;
            }
        }
    }

    if (customOcOut) *customOcOut = customOc;
    if (customFanOut) *customFanOut = customFan;
}

static void build_tray_tooltip(char* tip, size_t tipSize) {
    if (!tip || tipSize == 0) return;

    int selectedSlot = CONFIG_DEFAULT_SLOT;
    bool hasConfigPath = g_app.configPath[0] != '\0';
    bool hasSavedProfile = false;
    bool haveProfileMode = false;
    char mode[64] = {};
    if (hasConfigPath) {
        selectedSlot = get_config_int(g_app.configPath, "profiles", "selected_slot", CONFIG_DEFAULT_SLOT);
    }
    if (selectedSlot < 1 || selectedSlot > CONFIG_NUM_SLOTS) {
        selectedSlot = CONFIG_DEFAULT_SLOT;
    }

    char profilePart[64] = {};
    if (hasConfigPath) {
        hasSavedProfile = is_profile_slot_saved(g_app.configPath, selectedSlot);
        StringCchPrintfA(profilePart, ARRAY_COUNT(profilePart), "Profile %d (%s)", selectedSlot, hasSavedProfile ? "saved" : "empty");
        if (hasSavedProfile) {
            DesiredSettings desired = {};
            char err[256] = {};
            if (load_profile_from_config(g_app.configPath, selectedSlot, &desired, err, sizeof(err))) {
                desired_mode_label(&desired, mode, sizeof(mode));
                haveProfileMode = true;
            }
        }
    } else {
        StringCchPrintfA(profilePart, ARRAY_COUNT(profilePart), "Profile %d", selectedSlot);
    }

    if (!haveProfileMode) {
        bool customOc = live_state_has_custom_oc();
        bool customFan = live_state_has_custom_fan();
        StringCchCopyA(mode, ARRAY_COUNT(mode), tray_mode_label(customOc, customFan));
    }

    StringCchPrintfA(tip, tipSize, "Green Curve - %s | %s", mode, profilePart);
}

static int clamp_percent(int value) {
    if (value < 0) return 0;
    if (value > 100) return 100;
    return value;
}

static bool is_gpu_offset_excluded_low_point(int pointIndex, int gpuOffsetMHz) {
    if (pointIndex < 0 || pointIndex >= VF_NUM_POINTS) return false;
    if (g_app.curve[pointIndex].freq_kHz == 0) return false;

    (void)gpuOffsetMHz;

    int cutoffPointIndex = -1;
    int populatedCount = 0;
    for (int ci = 0; ci < VF_NUM_POINTS; ci++) {
        if (g_app.curve[ci].freq_kHz == 0) continue;
        if (populatedCount == 69) {
            cutoffPointIndex = ci;
            break;
        }
        populatedCount++;
    }

    if (cutoffPointIndex < 0) return true;

    auto sorts_before_or_equal = [&](int lhs, int rhs) -> bool {
        unsigned int lhsVolt = g_app.curve[lhs].volt_uV;
        unsigned int rhsVolt = g_app.curve[rhs].volt_uV;
        if (lhsVolt != rhsVolt) return lhsVolt < rhsVolt;

        unsigned int lhsFreq = g_app.curve[lhs].freq_kHz;
        unsigned int rhsFreq = g_app.curve[rhs].freq_kHz;
        if (lhsFreq != rhsFreq) return lhsFreq < rhsFreq;

        return lhs <= rhs;
    };

    return sorts_before_or_equal(pointIndex, cutoffPointIndex);
}

static int gpu_offset_component_mhz_for_point(int pointIndex, int gpuOffsetMHz, bool excludeLow70) {
    if (gpuOffsetMHz == 0) return 0;
    if (excludeLow70 && is_gpu_offset_excluded_low_point(pointIndex, gpuOffsetMHz)) return 0;
    return gpuOffsetMHz;
}

static bool detect_live_selective_gpu_offset_state(int* gpuOffsetMHzOut) {
    int candidateOffsets[VF_NUM_POINTS] = {};
    int candidateCount = 0;

    for (int ci = 0; ci < VF_NUM_POINTS; ci++) {
        if (g_app.curve[ci].freq_kHz == 0) continue;
        int offsetMHz = g_app.freqOffsets[ci] / 1000;
        if (offsetMHz == 0) continue;

        bool seen = false;
        for (int i = 0; i < candidateCount; i++) {
            if (candidateOffsets[i] == offsetMHz) {
                seen = true;
                break;
            }
        }
        if (!seen && candidateCount < VF_NUM_POINTS) {
            candidateOffsets[candidateCount++] = offsetMHz;
        }
    }

    for (int candidateIndex = 0; candidateIndex < candidateCount; candidateIndex++) {
        int candidateMHz = candidateOffsets[candidateIndex];
        bool sawExcludedPoint = false;
        bool sawIncludedPoint = false;
        bool match = true;

        for (int ci = 0; ci < VF_NUM_POINTS; ci++) {
            if (g_app.curve[ci].freq_kHz == 0) continue;

            bool excluded = is_gpu_offset_excluded_low_point(ci, candidateMHz);
            int actualOffsetMHz = g_app.freqOffsets[ci] / 1000;
            if (excluded) {
                sawExcludedPoint = true;
                if (actualOffsetMHz != 0) {
                    match = false;
                    break;
                }
            } else {
                sawIncludedPoint = true;
                if (actualOffsetMHz != candidateMHz) {
                    match = false;
                    break;
                }
            }
        }

        if (match && sawExcludedPoint && sawIncludedPoint) {
            if (gpuOffsetMHzOut) *gpuOffsetMHzOut = candidateMHz;
            return true;
        }
    }

    if (gpuOffsetMHzOut) *gpuOffsetMHzOut = 0;
    return false;
}

static int current_applied_gpu_offset_mhz() {
    if (g_app.appliedGpuOffsetExcludeLow70 && g_app.appliedGpuOffsetMHz != 0) {
        return g_app.appliedGpuOffsetMHz;
    }
    int detectedSelectiveOffsetMHz = 0;
    if (detect_live_selective_gpu_offset_state(&detectedSelectiveOffsetMHz)) {
        g_app.appliedGpuOffsetMHz = detectedSelectiveOffsetMHz;
        g_app.appliedGpuOffsetExcludeLow70 = true;
        return detectedSelectiveOffsetMHz;
    }
    return g_app.gpuClockOffsetkHz / 1000;
}

static bool current_applied_gpu_offset_excludes_low_points() {
    if (g_app.appliedGpuOffsetExcludeLow70 && current_applied_gpu_offset_mhz() != 0) {
        return true;
    }
    int detectedSelectiveOffsetMHz = 0;
    if (detect_live_selective_gpu_offset_state(&detectedSelectiveOffsetMHz)) {
        g_app.appliedGpuOffsetMHz = detectedSelectiveOffsetMHz;
        g_app.appliedGpuOffsetExcludeLow70 = true;
        return true;
    }
    return g_app.appliedGpuOffsetExcludeLow70 && current_applied_gpu_offset_mhz() != 0;
}

static void copy_fan_curve(FanCurveConfig* destination, const FanCurveConfig* source) {
    if (!destination || !source) return;
    memcpy(destination, source, sizeof(*destination));
}

static void ensure_valid_fan_curve_config(FanCurveConfig* curve) {
    if (!curve) return;

    if (curve->pollIntervalMs == 0) {
        fan_curve_set_default(curve);
        return;
    }

    fan_curve_normalize(curve);
    char err[256] = {};
    if (!fan_curve_validate(curve, err, sizeof(err))) {
        fan_curve_set_default(curve);
    }
}

static int get_effective_live_fan_mode() {
    if (g_app.fanCurveRuntimeActive) return FAN_MODE_CURVE;
    return g_app.fanIsAuto ? FAN_MODE_AUTO : FAN_MODE_FIXED;
}

static void initialize_gui_fan_settings_from_live_state() {
    ensure_valid_fan_curve_config(&g_app.guiFanCurve);
    ensure_valid_fan_curve_config(&g_app.activeFanCurve);

    g_app.activeFanMode = get_effective_live_fan_mode();
    g_app.activeFanFixedPercent = g_app.fanCount ? (int)g_app.fanPercent[0] : 50;
    if (g_app.guiFanMode < FAN_MODE_AUTO || g_app.guiFanMode > FAN_MODE_CURVE) {
        g_app.guiFanMode = g_app.activeFanMode;
    }
    if (g_app.guiFanFixedPercent <= 0) {
        g_app.guiFanFixedPercent = g_app.activeFanFixedPercent;
    }
    g_app.guiFanFixedPercent = clamp_percent(g_app.guiFanFixedPercent);
    if (g_app.activeFanMode == FAN_MODE_CURVE) {
        copy_fan_curve(&g_app.guiFanCurve, &g_app.activeFanCurve);
    }
}

static bool is_start_on_logon_enabled(const char* path) {
    return get_config_int(path, "startup", "start_program_on_logon", 0) != 0;
}

static bool set_start_on_logon_enabled(const char* path, bool enabled) {
    return set_config_int(path, "startup", "start_program_on_logon", enabled ? 1 : 0);
}

static bool should_enable_startup_task_from_config(const char* path) {
    if (!path || !*path) return false;
    if (is_start_on_logon_enabled(path)) return true;
    return get_config_int(path, "profiles", "logon_slot", 0) > 0;
}

static bool live_state_has_custom_oc() {
    if (g_app.gpuClockOffsetkHz != 0) return true;
    if (g_app.memClockOffsetkHz != 0) return true;
    if (g_app.powerLimitPct != 100) return true;
    for (int i = 0; i < VF_NUM_POINTS; i++) {
        if (g_app.freqOffsets[i] != 0) return true;
    }
    return false;
}

static bool live_state_has_custom_fan() {
    return get_effective_live_fan_mode() != FAN_MODE_AUTO;
}

static void refresh_fan_curve_button_text() {
    if (!g_app.hFanCurveBtn) return;

    ensure_valid_fan_curve_config(&g_app.guiFanCurve);

    char summary[96] = {};
    if (g_app.guiFanMode == FAN_MODE_CURVE) {
        fan_curve_format_summary(&g_app.guiFanCurve, summary, sizeof(summary));
        char text[128] = {};
        StringCchPrintfA(text, ARRAY_COUNT(text), "Curve: %s", summary);
        SetWindowTextA(g_app.hFanCurveBtn, text);
    } else {
        SetWindowTextA(g_app.hFanCurveBtn, "Edit Curve...");
    }
}

static void update_fan_controls_enabled_state() {
    if (g_app.hFanModeCombo) {
        SendMessageA(g_app.hFanModeCombo, CB_SETCURSEL, (WPARAM)g_app.guiFanMode, 0);
        EnableWindow(g_app.hFanModeCombo, g_app.fanSupported ? TRUE : FALSE);
    }
    if (g_app.hFanEdit) {
        char buf[32] = {};
        StringCchPrintfA(buf, ARRAY_COUNT(buf), "%d", clamp_percent(g_app.guiFanFixedPercent));
        SetWindowTextA(g_app.hFanEdit, buf);
        EnableWindow(g_app.hFanEdit, (g_app.fanSupported && g_app.guiFanMode == FAN_MODE_FIXED) ? TRUE : FALSE);
    }
    if (g_app.hFanCurveBtn) {
        EnableWindow(g_app.hFanCurveBtn,
            (g_app.fanSupported && g_app.guiFanMode == FAN_MODE_CURVE) ? TRUE : FALSE);
        refresh_fan_curve_button_text();
    }
}

static void update_tray_icon() {
    if (!g_app.hMainWnd) return;

    bool hasCustomOc = live_state_has_custom_oc();
    bool hasCustomFan = live_state_has_custom_fan();
    resolve_tray_icon_state(&hasCustomOc, &hasCustomFan);
    int state = TRAY_ICON_STATE_DEFAULT;
    if (hasCustomOc && hasCustomFan) {
        state = TRAY_ICON_STATE_OC_FAN;
    } else if (hasCustomOc) {
        state = TRAY_ICON_STATE_OC;
    } else if (hasCustomFan) {
        state = TRAY_ICON_STATE_FAN;
    }
    g_app.trayIconState = state;

    if (!g_app.trayIconAdded) return;

    NOTIFYICONDATAA nid = {};
    nid.cbSize = sizeof(nid);
    nid.hWnd = g_app.hMainWnd;
    nid.uID = 1;
    nid.uFlags = NIF_ICON | NIF_TIP | NIF_SHOWTIP;
    nid.hIcon = g_app.trayIcons[state] ? g_app.trayIcons[state] : LoadIcon(nullptr, IDI_APPLICATION);
    char tip[128] = {};
    build_tray_tooltip(tip, sizeof(tip));
    StringCchCopyA(nid.szTip, ARRAY_COUNT(nid.szTip), tip);
    Shell_NotifyIconA(NIM_MODIFY, &nid);
}

static bool ensure_tray_icon() {
    if (!g_app.hMainWnd) return false;
    if (g_app.trayIconAdded) {
        update_tray_icon();
        return true;
    }

    NOTIFYICONDATAA nid = {};
    nid.cbSize = sizeof(nid);
    nid.hWnd = g_app.hMainWnd;
    nid.uID = 1;
    nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
    nid.uCallbackMessage = APP_WM_TRAYICON;
    nid.hIcon = g_app.trayIcons[g_app.trayIconState] ? g_app.trayIcons[g_app.trayIconState] : LoadIcon(nullptr, IDI_APPLICATION);
    StringCchCopyA(nid.szTip, ARRAY_COUNT(nid.szTip), "Green Curve");

    if (!Shell_NotifyIconA(NIM_ADD, &nid)) return false;
    nid.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconA(NIM_SETVERSION, &nid);
    g_app.trayIconAdded = true;
    update_tray_icon();
    return true;
}

static void remove_tray_icon() {
    if (!g_app.trayIconAdded || !g_app.hMainWnd) return;
    NOTIFYICONDATAA nid = {};
    nid.cbSize = sizeof(nid);
    nid.hWnd = g_app.hMainWnd;
    nid.uID = 1;
    Shell_NotifyIconA(NIM_DELETE, &nid);
    g_app.trayIconAdded = false;
}

static void hide_main_window_to_tray() {
    if (!g_app.hMainWnd) return;
    ensure_tray_icon();
    ShowWindow(g_app.hMainWnd, SW_HIDE);
}

static void show_main_window_from_tray() {
    if (!g_app.hMainWnd) return;
    ShowWindow(g_app.hMainWnd, SW_RESTORE);
    ShowWindow(g_app.hMainWnd, SW_SHOW);
    SetForegroundWindow(g_app.hMainWnd);
    g_app.startHiddenToTray = false;
}

static void show_tray_menu(HWND hwnd) {
    if (!hwnd) return;
    HMENU menu = CreatePopupMenu();
    if (!menu) return;
    AppendMenuA(menu, MF_STRING, TRAY_MENU_SHOW_ID, IsWindowVisible(hwnd) ? "Show Window" : "Open Green Curve");
    AppendMenuA(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuA(menu, MF_STRING, TRAY_MENU_EXIT_ID, "Exit");

    POINT pt = {};
    GetCursorPos(&pt);
    SetForegroundWindow(hwnd);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN | TPM_LEFTALIGN, pt.x, pt.y, 0, hwnd, nullptr);
    DestroyMenu(menu);
}

static bool activate_existing_instance_window() {
    for (int attempt = 0; attempt < 20; attempt++) {
        HWND existing = FindWindowA(APP_CLASS_NAME, nullptr);
        if (existing && IsWindow(existing)) {
            HWND target = GetLastActivePopup(existing);
            if (!target || !IsWindow(target)) target = existing;
            ShowWindow(existing, SW_SHOW);
            ShowWindow(existing, SW_RESTORE);
            if (target != existing) {
                ShowWindow(target, SW_SHOW);
                ShowWindow(target, SW_RESTORE);
            }
            BringWindowToTop(target);
            SetForegroundWindow(target);
            return true;
        }
        if (attempt + 1 < 20) Sleep(50);
    }
    return false;
}

static bool acquire_single_instance_mutex() {
    if (g_singleInstanceMutex) return true;

    g_singleInstanceMutex = CreateMutexA(nullptr, TRUE, APP_SINGLE_INSTANCE_MUTEX_NAME);
    if (!g_singleInstanceMutex) return true;
    if (GetLastError() != ERROR_ALREADY_EXISTS) return true;

    CloseHandle(g_singleInstanceMutex);
    g_singleInstanceMutex = nullptr;
    activate_existing_instance_window();
    return false;
}

static void release_single_instance_mutex() {
    if (!g_singleInstanceMutex) return;
    ReleaseMutex(g_singleInstanceMutex);
    CloseHandle(g_singleInstanceMutex);
    g_singleInstanceMutex = nullptr;
}

static unsigned int fan_runtime_failure_limit() {
    UINT intervalMs = g_app.fanFixedRuntimeActive
        ? FAN_FIXED_RUNTIME_INTERVAL_MS
        : (UINT)g_app.activeFanCurve.pollIntervalMs;
    if (intervalMs < 250) intervalMs = 250;

    unsigned int limit = (unsigned int)((FAN_RUNTIME_FAILURE_WINDOW_MS + intervalMs - 1) / intervalMs);
    if (limit < 3) limit = 3;
    if (limit > 10) limit = 10;
    return limit;
}

static void mark_fan_runtime_success(ULONGLONG now) {
    g_app.fanRuntimeConsecutiveFailures = 0;
    g_app.fanRuntimeLastApplyTickMs = now;
}

static void handle_fan_runtime_failure(const char* action, const char* detail) {
    if (!g_app.fanCurveRuntimeActive && !g_app.fanFixedRuntimeActive) return;

    g_app.fanRuntimeLastApplyTickMs = 0;
    g_app.fanRuntimeConsecutiveFailures++;

    unsigned int limit = fan_runtime_failure_limit();
    debug_log("fan runtime failure %u/%u: %s%s%s\n",
        g_app.fanRuntimeConsecutiveFailures,
        limit,
        action ? action : "fan runtime failure",
        (detail && detail[0]) ? " - " : "",
        (detail && detail[0]) ? detail : "");

    if (g_app.fanRuntimeConsecutiveFailures < limit) return;

    char summary[512] = {};
    if (action && action[0] && detail && detail[0]) {
        set_message(summary, sizeof(summary), "%s: %s", action, detail);
    } else if (action && action[0]) {
        set_message(summary, sizeof(summary), "%s", action);
    } else if (detail && detail[0]) {
        set_message(summary, sizeof(summary), "%s", detail);
    } else {
        set_message(summary, sizeof(summary), "Custom fan runtime failed repeatedly");
    }

    char autoDetail[128] = {};
    bool autoRestored = nvml_set_fan_auto(autoDetail, sizeof(autoDetail));
    stop_fan_curve_runtime();
    if (autoRestored) {
        g_app.activeFanMode = FAN_MODE_AUTO;
        g_app.activeFanFixedPercent = g_app.fanCount ? (int)g_app.fanPercent[0] : 0;
    }

    char reportDetails[768] = {};
    if (autoRestored) {
        if (autoDetail[0]) {
            set_message(reportDetails, sizeof(reportDetails),
                "%s. Driver auto fan restored (%s).", summary, autoDetail);
        } else {
            set_message(reportDetails, sizeof(reportDetails),
                "%s. Driver auto fan restored.", summary);
        }
    } else {
        if (autoDetail[0]) {
            set_message(reportDetails, sizeof(reportDetails),
                "%s. Attempt to restore driver auto fan failed: %s", summary, autoDetail);
        } else {
            set_message(reportDetails, sizeof(reportDetails),
                "%s. Attempt to restore driver auto fan failed.", summary);
        }
    }

    char logErr[256] = {};
    if (!write_error_report_log(
            "Fan control runtime disabled after repeated failures",
            reportDetails,
            logErr,
            sizeof(logErr)) &&
        logErr[0]) {
        debug_log("fan runtime error log failed: %s\n", logErr);
    }

    if (g_app.hProfileStatusLabel) {
        set_profile_status_text(
            autoRestored
                ? "Custom fan runtime disabled after repeated failures. Driver auto fan restored. See greencurve_log.txt."
                : "Custom fan runtime disabled after repeated failures. Could not confirm driver auto fan restore. See greencurve_log.txt.");
    }
    update_tray_icon();
}

static void stop_fan_curve_runtime() {
    if (g_app.hMainWnd) {
        KillTimer(g_app.hMainWnd, FAN_CURVE_TIMER_ID);
    }
    bool hadRuntime = g_app.fanCurveRuntimeActive || g_app.fanFixedRuntimeActive;
    g_app.fanCurveRuntimeActive = false;
    g_app.fanFixedRuntimeActive = false;
    g_app.fanCurveHasLastAppliedTemp = false;
    g_app.fanRuntimeConsecutiveFailures = 0;
    g_app.fanRuntimeLastApplyTickMs = 0;
    if (hadRuntime && (g_app.activeFanMode == FAN_MODE_CURVE || g_app.activeFanMode == FAN_MODE_FIXED)) {
        g_app.activeFanMode = g_app.fanIsAuto ? FAN_MODE_AUTO : FAN_MODE_FIXED;
    }
    update_tray_icon();
}

static bool nvml_read_temperature(int* temperatureC, char* detail, size_t detailSize) {
    if (temperatureC) *temperatureC = 0;
    g_app.gpuTemperatureValid = false;
    if (!nvml_ensure_ready() || !g_nvml_api.getTemperature) {
        set_message(detail, detailSize, "GPU temperature unsupported");
        return false;
    }

    unsigned int value = 0;
    nvmlReturn_t r = g_nvml_api.getTemperature(g_app.nvmlDevice, NVML_TEMPERATURE_GPU, &value);
    if (r != NVML_SUCCESS) {
        set_message(detail, detailSize, "%s", nvml_err_name(r));
        return false;
    }

    g_app.gpuTemperatureC = (int)value;
    g_app.gpuTemperatureValid = true;
    if (temperatureC) *temperatureC = (int)value;
    return true;
}

static void apply_fan_curve_tick() {
    if (!g_app.fanSupported) return;

    ULONGLONG now = GetTickCount64();

    if (g_app.fanFixedRuntimeActive) {
        int targetPercent = clamp_percent(g_app.activeFanFixedPercent);
        bool exact = false;
        char detail[128] = {};
        if (!nvml_set_fan_manual(targetPercent, &exact, detail, sizeof(detail)) || !exact) {
            if (!detail[0] && !exact) {
                set_message(detail, sizeof(detail), "Fan readback did not confirm %d%%", targetPercent);
            }
            handle_fan_runtime_failure("Fixed fan runtime apply failed", detail);
            return;
        }

        g_app.activeFanMode = FAN_MODE_FIXED;
        g_app.activeFanFixedPercent = targetPercent;
        mark_fan_runtime_success(now);
        update_tray_icon();
        return;
    }

    if (!g_app.fanCurveRuntimeActive) return;

    int currentTempC = 0;
    char detail[128] = {};
    if (!nvml_read_temperature(&currentTempC, detail, sizeof(detail))) {
        handle_fan_runtime_failure("Fan curve temperature poll failed", detail);
        return;
    }

    int targetPercent = fan_curve_interpolate_percent(&g_app.activeFanCurve, currentTempC);
    bool shouldApply = false;
    if (!g_app.fanCurveHasLastAppliedTemp) {
        shouldApply = true;
    } else if (targetPercent > g_app.fanCurveLastAppliedPercent) {
        shouldApply = true;
    } else if (targetPercent < g_app.fanCurveLastAppliedPercent) {
        int minDrop = g_app.activeFanCurve.hysteresisC;
        if (minDrop < 0) minDrop = 0;
        if (currentTempC <= g_app.fanCurveLastAppliedTempC - minDrop) {
            shouldApply = true;
        }
    }

    if (!shouldApply) {
        shouldApply = (g_app.fanRuntimeLastApplyTickMs == 0) ||
            ((now - g_app.fanRuntimeLastApplyTickMs) >= FAN_RUNTIME_REAPPLY_INTERVAL_MS);
    }

    if (!shouldApply) return;

    bool exact = false;
    if (!nvml_set_fan_manual(targetPercent, &exact, detail, sizeof(detail)) || !exact) {
        if (!detail[0] && !exact) {
            set_message(detail, sizeof(detail), "Fan readback did not confirm %d%%", targetPercent);
        }
        handle_fan_runtime_failure("Fan curve runtime apply failed", detail);
        return;
    }

    g_app.activeFanMode = FAN_MODE_CURVE;
    g_app.activeFanFixedPercent = targetPercent;
    g_app.fanCurveLastAppliedPercent = targetPercent;
    g_app.fanCurveLastAppliedTempC = currentTempC;
    g_app.fanCurveHasLastAppliedTemp = true;
    mark_fan_runtime_success(now);
    update_tray_icon();
}

static void start_fan_curve_runtime() {
    if (!g_app.fanSupported || !g_app.hMainWnd) return;
    fan_curve_normalize(&g_app.activeFanCurve);
    char err[256] = {};
    if (!fan_curve_validate(&g_app.activeFanCurve, err, sizeof(err))) {
        return;
    }

    g_app.activeFanMode = FAN_MODE_CURVE;
    g_app.fanCurveRuntimeActive = true;
    g_app.fanFixedRuntimeActive = false;
    g_app.fanCurveHasLastAppliedTemp = false;
    g_app.fanRuntimeConsecutiveFailures = 0;
    g_app.fanRuntimeLastApplyTickMs = 0;

    KillTimer(g_app.hMainWnd, FAN_CURVE_TIMER_ID);
    if (!SetTimer(g_app.hMainWnd, FAN_CURVE_TIMER_ID, (UINT)g_app.activeFanCurve.pollIntervalMs, nullptr)) {
        stop_fan_curve_runtime();
        return;
    }

    if (g_app.fanCurveRuntimeActive) {
        apply_fan_curve_tick();
    }
    update_tray_icon();
}

static void start_fixed_fan_runtime() {
    if (!g_app.fanSupported || !g_app.hMainWnd) return;

    g_app.activeFanFixedPercent = clamp_percent(g_app.activeFanFixedPercent);
    g_app.activeFanMode = FAN_MODE_FIXED;
    g_app.fanCurveRuntimeActive = false;
    g_app.fanFixedRuntimeActive = true;
    g_app.fanCurveHasLastAppliedTemp = false;
    g_app.fanRuntimeConsecutiveFailures = 0;
    g_app.fanRuntimeLastApplyTickMs = 0;

    KillTimer(g_app.hMainWnd, FAN_CURVE_TIMER_ID);
    if (!SetTimer(g_app.hMainWnd, FAN_CURVE_TIMER_ID, FAN_FIXED_RUNTIME_INTERVAL_MS, nullptr)) {
        stop_fan_curve_runtime();
        return;
    }

    apply_fan_curve_tick();
    update_tray_icon();
}

static void show_license_dialog(HWND parent) {
    MessageBoxA(parent, APP_LICENSE_TEXT, APP_NAME " License", MB_OK | MB_ICONINFORMATION);
}

static int layout_rows_per_column() {
    return (g_app.numVisible + 5) / 6;
}

static int layout_global_controls_y() {
    return dp(GRAPH_HEIGHT) + dp(14) + layout_rows_per_column() * dp(20) + dp(6);
}

static int layout_bottom_buttons_y() {
    return layout_global_controls_y() + dp(56);
}

static int layout_bottom_panel_bottom_y() {
    int buttonsY = layout_bottom_buttons_y();
    int profileY = buttonsY + dp(40);
    int autoY = profileY + dp(34);
    int statusY = autoY + dp(32);
    return statusY + dp(18);
}

static int minimum_client_height() {
    return nvmax(dp(WINDOW_HEIGHT), layout_bottom_panel_bottom_y() + dp(12));
}

static SIZE adjusted_window_size_for_client(int clientWidth, int clientHeight, DWORD style, DWORD exStyle) {
    RECT rc = { 0, 0, clientWidth, clientHeight };
    typedef BOOL (WINAPI *AdjustWindowRectExForDpi_t)(LPRECT, DWORD, BOOL, DWORD, UINT);
    static AdjustWindowRectExForDpi_t adjustForDpi = (AdjustWindowRectExForDpi_t)GetProcAddress(GetModuleHandleA("user32.dll"), "AdjustWindowRectExForDpi");
    if (adjustForDpi) {
        adjustForDpi(&rc, style, FALSE, exStyle, (UINT)g_dpi);
    } else {
        AdjustWindowRectEx(&rc, style, FALSE, exStyle);
    }
    SIZE size = {};
    size.cx = rc.right - rc.left;
    size.cy = rc.bottom - rc.top;
    return size;
}

static SIZE main_window_min_size() {
    return adjusted_window_size_for_client(dp(WINDOW_WIDTH), minimum_client_height(), WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, 0);
}

static void ensure_main_window_min_size(HWND hwnd) {
    if (!hwnd) return;
    RECT client = {};
    GetClientRect(hwnd, &client);
    int needClientW = dp(WINDOW_WIDTH);
    int needClientH = minimum_client_height();
    if (client.right >= needClientW && client.bottom >= needClientH) return;

    RECT window = {};
    GetWindowRect(hwnd, &window);
    SIZE needWindow = main_window_min_size();
    int currentW = window.right - window.left;
    int currentH = window.bottom - window.top;
    SetWindowPos(hwnd, nullptr, 0, 0,
        nvmax(currentW, needWindow.cx),
        nvmax(currentH, needWindow.cy),
        SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}

static void layout_bottom_buttons(HWND hParent) {
    if (!hParent) return;
    RECT rc = {};
    GetClientRect(hParent, &rc);
    const int margin = dp(8);
    const int gap = dp(6);
    const int buttonH = dp(30);
    const int smallButtonW = dp(76);
    const int comboDropH = dp(220);
    const int buttonsY = layout_bottom_buttons_y();
    const int profileY = buttonsY + dp(40);
    const int autoY = profileY + dp(34);
    const int statusY = autoY + dp(32);

    if (g_app.hApplyBtn)
        SetWindowPos(g_app.hApplyBtn, nullptr, margin, buttonsY, dp(132), buttonH, SWP_NOZORDER);
    if (g_app.hRefreshBtn)
        SetWindowPos(g_app.hRefreshBtn, nullptr, margin + dp(144), buttonsY, dp(98), buttonH, SWP_NOZORDER);
    if (g_app.hResetBtn)
        SetWindowPos(g_app.hResetBtn, nullptr, margin + dp(254), buttonsY, dp(98), buttonH, SWP_NOZORDER);
    if (g_app.hLicenseBtn)
        SetWindowPos(g_app.hLicenseBtn, nullptr, rc.right - margin - dp(118), buttonsY, dp(118), buttonH, SWP_NOZORDER);

    if (g_app.hProfileLabel)
        SetWindowPos(g_app.hProfileLabel, nullptr, margin, profileY + dp(4), dp(72), dp(18), SWP_NOZORDER);
    if (g_app.hProfileCombo)
        SetWindowPos(g_app.hProfileCombo, nullptr, margin + dp(76), profileY, dp(156), comboDropH, SWP_NOZORDER);
    if (g_app.hProfileLoadBtn)
        SetWindowPos(g_app.hProfileLoadBtn, nullptr, margin + dp(244), profileY, smallButtonW, dp(28), SWP_NOZORDER);
    if (g_app.hProfileSaveBtn)
        SetWindowPos(g_app.hProfileSaveBtn, nullptr, margin + dp(244) + smallButtonW + gap, profileY, smallButtonW, dp(28), SWP_NOZORDER);
    if (g_app.hProfileClearBtn)
        SetWindowPos(g_app.hProfileClearBtn, nullptr, margin + dp(244) + (smallButtonW + gap) * 2, profileY, smallButtonW, dp(28), SWP_NOZORDER);
    if (g_app.hProfileStateLabel) {
        int stateX = margin + dp(244) + (smallButtonW + gap) * 3 + dp(12);
        int stateW = nvmax(dp(140), rc.right - stateX - margin);
        SetWindowPos(g_app.hProfileStateLabel, nullptr, stateX, profileY + dp(4), stateW, dp(18), SWP_NOZORDER);
    }

    if (g_app.hAppLaunchLabel)
        SetWindowPos(g_app.hAppLaunchLabel, nullptr, margin, autoY + dp(4), dp(170), dp(18), SWP_NOZORDER);
    if (g_app.hAppLaunchCombo)
        SetWindowPos(g_app.hAppLaunchCombo, nullptr, margin + dp(174), autoY, dp(170), comboDropH, SWP_NOZORDER);
    if (g_app.hLogonLabel)
        SetWindowPos(g_app.hLogonLabel, nullptr, margin + dp(366), autoY + dp(4), dp(208), dp(18), SWP_NOZORDER);
    if (g_app.hLogonCombo)
        SetWindowPos(g_app.hLogonCombo, nullptr, margin + dp(578), autoY, dp(170), comboDropH, SWP_NOZORDER);
    if (g_app.hStartOnLogonCheck)
        SetWindowPos(g_app.hStartOnLogonCheck, nullptr, margin + dp(760), autoY + dp(2), dp(320), dp(24), SWP_NOZORDER);
    if (g_app.hProfileStatusLabel)
        SetWindowPos(g_app.hProfileStatusLabel, nullptr, margin, statusY, nvmax(dp(300), rc.right - margin * 2), dp(18), SWP_NOZORDER);
}


static void set_default_config_path() {
    if (g_app.configPath[0]) return;
    char path[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, path, MAX_PATH);
    char* slash = strrchr(path, '\\');
    if (!slash) slash = strrchr(path, '/');
    if (slash) {
        slash[1] = 0;
        StringCchCatA(path, ARRAY_COUNT(path), CONFIG_FILE_NAME);
    } else {
        StringCchCopyA(path, ARRAY_COUNT(path), CONFIG_FILE_NAME);
    }
    StringCchCopyA(g_app.configPath, ARRAY_COUNT(g_app.configPath), path);
}

static const char* nvml_err_name(nvmlReturn_t r) {
    switch (r) {
        case NVML_SUCCESS: return "NVML_SUCCESS";
        case NVML_ERROR_UNINITIALIZED: return "NVML_ERROR_UNINITIALIZED";
        case NVML_ERROR_INVALID_ARGUMENT: return "NVML_ERROR_INVALID_ARGUMENT";
        case NVML_ERROR_NOT_SUPPORTED: return "NVML_ERROR_NOT_SUPPORTED";
        case NVML_ERROR_NO_PERMISSION: return "NVML_ERROR_NO_PERMISSION";
        case NVML_ERROR_ALREADY_INITIALIZED: return "NVML_ERROR_ALREADY_INITIALIZED";
        case NVML_ERROR_NOT_FOUND: return "NVML_ERROR_NOT_FOUND";
        case NVML_ERROR_INSUFFICIENT_SIZE: return "NVML_ERROR_INSUFFICIENT_SIZE";
        case NVML_ERROR_FUNCTION_NOT_FOUND: return "NVML_ERROR_FUNCTION_NOT_FOUND";
        case NVML_ERROR_GPU_IS_LOST: return "NVML_ERROR_GPU_IS_LOST";
        case NVML_ERROR_ARG_VERSION_MISMATCH: return "NVML_ERROR_ARGUMENT_VERSION_MISMATCH";
        default: return "NVML_ERROR_OTHER";
    }
}

static void debug_log(const char* fmt, ...) {
    if (!g_debug_logging || !fmt) return;
    char buf[1024] = {};
    va_list ap;
    va_start(ap, fmt);
    StringCchVPrintfA(buf, ARRAY_COUNT(buf), fmt, ap);
    va_end(ap);
    OutputDebugStringA(buf);
}

static bool write_text_file_atomic(const char* path, const char* data, size_t dataSize, char* err, size_t errSize) {
    if (!path || !data) {
        set_message(err, errSize, "Invalid file write arguments");
        return false;
    }

    char tempPath[MAX_PATH] = {};
    StringCchPrintfA(tempPath, ARRAY_COUNT(tempPath), "%s.tmp", path);

    HANDLE h = CreateFileA(tempPath, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        set_message(err, errSize, "Cannot create %s (error %lu)", tempPath, GetLastError());
        return false;
    }

    DWORD totalWritten = 0;
    bool ok = true;
    while (totalWritten < dataSize) {
        DWORD chunk = 0;
        DWORD toWrite = (DWORD)nvmin((int)(dataSize - totalWritten), 1 << 20);
        if (!WriteFile(h, data + totalWritten, toWrite, &chunk, nullptr) || chunk == 0) {
            ok = false;
            set_message(err, errSize, "Failed writing %s (error %lu)", tempPath, GetLastError());
            break;
        }
        totalWritten += chunk;
    }
    if (ok && !FlushFileBuffers(h)) {
        ok = false;
        set_message(err, errSize, "Failed flushing %s (error %lu)", tempPath, GetLastError());
    }
    CloseHandle(h);

    if (!ok) {
        DeleteFileA(tempPath);
        return false;
    }

    if (!MoveFileExA(tempPath, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        set_message(err, errSize, "Failed finalizing %s (error %lu)", path, GetLastError());
        DeleteFileA(tempPath);
        return false;
    }
    return true;
}

static bool write_log_snapshot(const char* path, char* err, size_t errSize) {
    char* text = (char*)malloc(65536);
    if (!text) {
        set_message(err, errSize, "Out of memory generating log");
        return false;
    }

    size_t used = 0;
    auto appendf = [&](const char* fmt, ...) -> bool {
        if (used >= 65536) return false;
        va_list ap;
        va_start(ap, fmt);
        int written = _vsnprintf_s(text + used, 65536 - used, _TRUNCATE, fmt, ap);
        va_end(ap);
        if (written < 0) {
            used = 65535;
            text[65535] = 0;
            return false;
        }
        used += (size_t)written;
        return true;
    };

    appendf("GPU: %s\r\n", g_app.gpuName);
    appendf("Populated points: %d\r\n\r\n", g_app.numPopulated);
    appendf("GPU offset: %d MHz", g_app.gpuClockOffsetkHz / 1000);
    if (g_app.gpuOffsetRangeKnown) appendf(" (range %d..%d)", g_app.gpuClockOffsetMinMHz, g_app.gpuClockOffsetMaxMHz);
    appendf("\r\n");
    appendf("Mem offset: %d MHz", mem_display_mhz_from_driver_khz(g_app.memClockOffsetkHz));
    if (g_app.memOffsetRangeKnown) appendf(" (range %d..%d)", g_app.memClockOffsetMinMHz, g_app.memClockOffsetMaxMHz);
    appendf("\r\n");
    appendf("Power limit: %d%% (%d mW current / %d mW default)\r\n", g_app.powerLimitPct, g_app.powerLimitCurrentmW, g_app.powerLimitDefaultmW);
    if (g_app.fanSupported) {
        appendf("Fan: %s\r\n", g_app.fanIsAuto ? "auto" : "manual");
        for (unsigned int fan = 0; fan < g_app.fanCount; fan++) {
            appendf("  Fan %u: %u%% / %u RPM / policy=%u signal=%u target=0x%X\r\n",
                fan, g_app.fanPercent[fan], g_app.fanRpm[fan], g_app.fanPolicy[fan], g_app.fanControlSignal[fan], g_app.fanTargetMask[fan]);
        }
    } else {
        appendf("Fan: unsupported\r\n");
    }
    appendf("\r\n%-6s  %-10s  %-10s  %-12s\r\n", "Point", "Freq(MHz)", "Volt(mV)", "Offset(kHz)");
    appendf("------  ----------  ----------  ------------\r\n");
    for (int i = 0; i < VF_NUM_POINTS; i++) {
        if (g_app.curve[i].freq_kHz > 0 || g_app.curve[i].volt_uV > 0) {
            appendf("%-6d  %-10u  %-10u  %-12d\r\n",
                i,
                displayed_curve_mhz(g_app.curve[i].freq_kHz),
                g_app.curve[i].volt_uV / 1000,
                g_app.freqOffsets[i]);
        }
    }

    bool ok = write_text_file_atomic(path, text, used, err, errSize);
    free(text);
    return ok;
}

static bool write_error_report_log(const char* summary, const char* details, char* err, size_t errSize) {
    char* text = (char*)malloc(73728);
    if (!text) {
        set_message(err, errSize, "Out of memory generating error log");
        return false;
    }

    size_t used = 0;
    auto appendf = [&](const char* fmt, ...) -> bool {
        if (used >= 73728) return false;
        va_list ap;
        va_start(ap, fmt);
        int written = _vsnprintf_s(text + used, 73728 - used, _TRUNCATE, fmt, ap);
        va_end(ap);
        if (written < 0) {
            used = 73727;
            text[73727] = 0;
            return false;
        }
        used += (size_t)written;
        return true;
    };

    SYSTEMTIME now = {};
    GetLocalTime(&now);
    appendf("Green Curve error report\r\n");
    appendf("Generated: %04u-%02u-%02u %02u:%02u:%02u\r\n\r\n",
        now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond);
    if (summary && *summary) appendf("Summary: %s\r\n", summary);
    if (details && *details) appendf("Details: %s\r\n", details);
    appendf("\r\nCurrent state snapshot\r\n======================\r\n");
    appendf("GPU: %s\r\n", g_app.gpuName);
    appendf("Populated points: %d\r\n\r\n", g_app.numPopulated);
    appendf("GPU offset: %d MHz", g_app.gpuClockOffsetkHz / 1000);
    if (g_app.gpuOffsetRangeKnown) appendf(" (range %d..%d)", g_app.gpuClockOffsetMinMHz, g_app.gpuClockOffsetMaxMHz);
    appendf("\r\n");
    appendf("Mem offset: %d MHz", mem_display_mhz_from_driver_khz(g_app.memClockOffsetkHz));
    if (g_app.memOffsetRangeKnown) appendf(" (range %d..%d)", g_app.memClockOffsetMinMHz, g_app.memClockOffsetMaxMHz);
    appendf("\r\n");
    appendf("Power limit: %d%% (%d mW current / %d mW default)\r\n", g_app.powerLimitPct, g_app.powerLimitCurrentmW, g_app.powerLimitDefaultmW);
    if (g_app.fanSupported) {
        appendf("Fan: %s\r\n", g_app.fanIsAuto ? "auto" : "manual");
        for (unsigned int fan = 0; fan < g_app.fanCount; fan++) {
            appendf("  Fan %u: %u%% / %u RPM / policy=%u signal=%u target=0x%X\r\n",
                fan, g_app.fanPercent[fan], g_app.fanRpm[fan], g_app.fanPolicy[fan], g_app.fanControlSignal[fan], g_app.fanTargetMask[fan]);
        }
    } else {
        appendf("Fan: unsupported\r\n");
    }
    appendf("\r\n%-6s  %-10s  %-10s  %-12s\r\n", "Point", "Freq(MHz)", "Volt(mV)", "Offset(kHz)");
    appendf("------  ----------  ----------  ------------\r\n");
    for (int i = 0; i < VF_NUM_POINTS; i++) {
        if (g_app.curve[i].freq_kHz > 0 || g_app.curve[i].volt_uV > 0) {
            appendf("%-6d  %-10u  %-10u  %-12d\r\n",
                i,
                displayed_curve_mhz(g_app.curve[i].freq_kHz),
                g_app.curve[i].volt_uV / 1000,
                g_app.freqOffsets[i]);
        }
    }

    bool ok = write_text_file_atomic(APP_LOG_FILE, text, used, err, errSize);
    free(text);
    return ok;
}

static bool write_json_snapshot(const char* path, char* err, size_t errSize) {
    char* json = (char*)malloc(131072);
    if (!json) {
        set_message(err, errSize, "Out of memory generating JSON");
        return false;
    }

    size_t used = 0;
    auto append = [&](const char* fmt, ...) -> bool {
        if (used >= 131072) return false;
        va_list ap;
        va_start(ap, fmt);
        int written = _vsnprintf_s(json + used, 131072 - used, _TRUNCATE, fmt, ap);
        va_end(ap);
        if (written < 0) {
            used = 131071;
            json[131071] = 0;
            return false;
        }
        used += (size_t)written;
        return true;
    };

    append("{\n  \"gpu\": \"");
    for (const unsigned char* p = (const unsigned char*)g_app.gpuName; p && *p; ++p) {
        switch (*p) {
            case '\\': append("\\\\"); break;
            case '"': append("\\\""); break;
            case '\n': append("\\n"); break;
            case '\r': append("\\r"); break;
            case '\t': append("\\t"); break;
            default:
                if (*p < 32) append("\\u%04x", *p);
                else append("%c", *p);
                break;
        }
    }
    append("\",\n  \"populated\": %d,\n", g_app.numPopulated);
    append("  \"gpu_offset_mhz\": %d,\n", g_app.gpuClockOffsetkHz / 1000);
    append("  \"mem_offset_mhz\": %d,\n", mem_display_mhz_from_driver_khz(g_app.memClockOffsetkHz));
    append("  \"power_limit_pct\": %d,\n", g_app.powerLimitPct);
    if (g_app.fanSupported) {
        if (g_app.fanIsAuto) append("  \"fan\": \"auto\",\n");
        else append("  \"fan\": %u,\n", g_app.fanPercent[0]);
    } else {
        append("  \"fan\": null,\n");
    }
    append("  \"fans\": [\n");
    for (unsigned int fan = 0; fan < g_app.fanCount; fan++) {
        append("    {\"index\": %u, \"percent\": %u, \"rpm\": %u, \"policy\": %u, \"signal\": %u, \"target\": %u}%s\n",
            fan, g_app.fanPercent[fan], g_app.fanRpm[fan], g_app.fanPolicy[fan], g_app.fanControlSignal[fan], g_app.fanTargetMask[fan],
            (fan + 1 < g_app.fanCount) ? "," : "");
    }
    append("  ],\n  \"points\": [\n");
    bool first = true;
    for (int i = 0; i < VF_NUM_POINTS; i++) {
        if (g_app.curve[i].freq_kHz > 0 || g_app.curve[i].volt_uV > 0) {
            append("%s    {\"index\": %d, \"freq_mhz\": %u, \"volt_mv\": %u, \"offset_khz\": %d}",
                first ? "" : ",\n",
                i,
                displayed_curve_mhz(g_app.curve[i].freq_kHz),
                g_app.curve[i].volt_uV / 1000,
                g_app.freqOffsets[i]);
            first = false;
        }
    }
    append("\n  ]\n}\n");

    bool ok = write_text_file_atomic(path, json, used, err, errSize);
    free(json);
    return ok;
}

static void close_nvml() {
    if (g_app.nvmlReady && g_nvml_api.shutdown) {
        g_nvml_api.shutdown();
    }
    g_app.nvmlReady = false;
    g_app.nvmlDevice = nullptr;
    if (g_nvml) {
        FreeLibrary(g_nvml);
        g_nvml = nullptr;
    }
    memset(&g_nvml_api, 0, sizeof(g_nvml_api));
}

static bool get_window_text_safe(HWND hwnd, char* buf, int bufSize) {
    if (!buf || bufSize < 1) return false;
    buf[0] = 0;
    if (!hwnd) return false;
    GetWindowTextA(hwnd, buf, bufSize);
    buf[bufSize - 1] = 0;
    trim_ascii(buf);
    return true;
}

static void initialize_desired_settings_defaults(DesiredSettings* desired) {
    if (!desired) return;
    memset(desired, 0, sizeof(*desired));
    desired->fanAuto = true;
    desired->fanMode = FAN_MODE_AUTO;
    fan_curve_set_default(&desired->fanCurve);
}

static void set_desired_fan_from_legacy_value(DesiredSettings* desired, bool fanAuto, int fanPercent) {
    if (!desired) return;
    desired->hasFan = true;
    desired->fanAuto = fanAuto;
    desired->fanMode = fanAuto ? FAN_MODE_AUTO : FAN_MODE_FIXED;
    desired->fanPercent = fanPercent;
}

static const char* fan_mode_to_config_value(int mode) {
    switch (mode) {
        case FAN_MODE_FIXED: return "fixed";
        case FAN_MODE_CURVE: return "curve";
        default: return "auto";
    }
}

static bool parse_fan_mode_config_value(const char* text, int* mode) {
    if (!text || !*text || !mode) return false;
    if (streqi_ascii(text, "auto") || streqi_ascii(text, "default")) {
        *mode = FAN_MODE_AUTO;
        return true;
    }
    if (streqi_ascii(text, "fixed") || streqi_ascii(text, "manual")) {
        *mode = FAN_MODE_FIXED;
        return true;
    }
    if (streqi_ascii(text, "curve")) {
        *mode = FAN_MODE_CURVE;
        return true;
    }
    return false;
}

static bool load_fan_curve_config_from_section(const char* path, const char* section, FanCurveConfig* curve, char* err, size_t errSize) {
    if (!curve) return false;
    if (!path || !section || !*section) return false;

    if (!config_section_has_keys(path, section)) return true;

    char buf[64] = {};
    GetPrivateProfileStringA(section, "poll_interval_ms", "", buf, sizeof(buf), path);
    trim_ascii(buf);
    if (buf[0]) {
        if (!parse_int_strict(buf, &curve->pollIntervalMs)) {
            set_message(err, errSize, "Invalid fan curve poll interval in %s", section);
            return false;
        }
    }

    GetPrivateProfileStringA(section, "hysteresis_c", "", buf, sizeof(buf), path);
    trim_ascii(buf);
    if (buf[0]) {
        if (!parse_int_strict(buf, &curve->hysteresisC)) {
            set_message(err, errSize, "Invalid fan curve hysteresis in %s", section);
            return false;
        }
    }

    for (int i = 0; i < FAN_CURVE_MAX_POINTS; i++) {
        char key[32] = {};

        StringCchPrintfA(key, ARRAY_COUNT(key), "enabled%d", i);
        GetPrivateProfileStringA(section, key, "", buf, sizeof(buf), path);
        trim_ascii(buf);
        if (buf[0]) {
            int value = 0;
            if (!parse_int_strict(buf, &value)) {
                set_message(err, errSize, "Invalid fan curve enabled flag in %s", section);
                return false;
            }
            curve->points[i].enabled = value != 0;
        }

        StringCchPrintfA(key, ARRAY_COUNT(key), "temp%d", i);
        GetPrivateProfileStringA(section, key, "", buf, sizeof(buf), path);
        trim_ascii(buf);
        if (buf[0]) {
            if (!parse_int_strict(buf, &curve->points[i].temperatureC)) {
                set_message(err, errSize, "Invalid fan curve temperature in %s", section);
                return false;
            }
        }

        StringCchPrintfA(key, ARRAY_COUNT(key), "pct%d", i);
        GetPrivateProfileStringA(section, key, "", buf, sizeof(buf), path);
        trim_ascii(buf);
        if (buf[0]) {
            if (!parse_int_strict(buf, &curve->points[i].fanPercent)) {
                set_message(err, errSize, "Invalid fan curve percentage in %s", section);
                return false;
            }
        }
    }

    fan_curve_normalize(curve);
    return fan_curve_validate(curve, err, errSize);
}

static void append_fan_curve_section_text(char* cfg, size_t cfgSize, size_t* used, const char* sectionName, const FanCurveConfig* curve) {
    if (!cfg || !used || !sectionName || !curve) return;

    auto appendf = [&](const char* fmt, ...) {
        if (*used >= cfgSize - 1) return;
        va_list ap;
        va_start(ap, fmt);
        int n = _vsnprintf_s(cfg + *used, cfgSize - *used, _TRUNCATE, fmt, ap);
        va_end(ap);
        if (n > 0) *used += (size_t)n;
    };

    appendf("[%s]\r\n", sectionName);
    appendf("poll_interval_ms=%d\r\n", curve->pollIntervalMs);
    appendf("hysteresis_c=%d\r\n", curve->hysteresisC);
    for (int i = 0; i < FAN_CURVE_MAX_POINTS; i++) {
        appendf("enabled%d=%d\r\n", i, curve->points[i].enabled ? 1 : 0);
        appendf("temp%d=%d\r\n", i, curve->points[i].temperatureC);
        appendf("pct%d=%d\r\n", i, curve->points[i].fanPercent);
    }
    appendf("\r\n");
}

static bool load_desired_settings_from_ini(const char* path, DesiredSettings* desired, char* err, size_t errSize) {
    if (!path || !desired) return false;
    initialize_desired_settings_defaults(desired);
    char fanBuf[64] = {};
    char buf[64] = {};

    GetPrivateProfileStringA("controls", "gpu_offset_mhz", "", buf, sizeof(buf), path);
    trim_ascii(buf);
    if (buf[0]) {
        int v = 0;
        if (!parse_int_strict(buf, &v)) {
            set_message(err, errSize, "Invalid gpu_offset_mhz in %s", path);
            return false;
        }
        desired->hasGpuOffset = true;
        desired->gpuOffsetMHz = v;
    }

    GetPrivateProfileStringA("controls", "gpu_offset_exclude_low_70", "", buf, sizeof(buf), path);
    trim_ascii(buf);
    if (buf[0]) {
        int value = 0;
        if (!parse_int_strict(buf, &value)) {
            set_message(err, errSize, "Invalid gpu_offset_exclude_low_70 in %s", path);
            return false;
        }
        desired->gpuOffsetExcludeLow70 = value != 0;
    }

    GetPrivateProfileStringA("controls", "mem_offset_mhz", "", buf, sizeof(buf), path);
    trim_ascii(buf);
    if (buf[0]) {
        int v = 0;
        if (!parse_int_strict(buf, &v)) {
            set_message(err, errSize, "Invalid mem_offset_mhz in %s", path);
            return false;
        }
        desired->hasMemOffset = true;
        desired->memOffsetMHz = v;
    }

    GetPrivateProfileStringA("controls", "power_limit_pct", "", buf, sizeof(buf), path);
    trim_ascii(buf);
    if (buf[0]) {
        int v = 0;
        if (!parse_int_strict(buf, &v)) {
            set_message(err, errSize, "Invalid power_limit_pct in %s", path);
            return false;
        }
        desired->hasPowerLimit = true;
        desired->powerLimitPct = v;
    }

    GetPrivateProfileStringA("controls", "fan_mode", "", buf, sizeof(buf), path);
    trim_ascii(buf);
    if (buf[0]) {
        int fanMode = FAN_MODE_AUTO;
        if (!parse_fan_mode_config_value(buf, &fanMode)) {
            set_message(err, errSize, "Invalid fan_mode in %s", path);
            return false;
        }
        desired->hasFan = true;
        desired->fanMode = fanMode;
        desired->fanAuto = fanMode == FAN_MODE_AUTO;
    }

    GetPrivateProfileStringA("controls", "fan", "", fanBuf, sizeof(fanBuf), path);
    trim_ascii(fanBuf);
    if (fanBuf[0]) {
        bool fanAuto = false;
        int fanPercent = 0;
        if (!parse_fan_value(fanBuf, &fanAuto, &fanPercent)) {
            set_message(err, errSize, "Invalid fan setting in %s", path);
            return false;
        }
        if (!desired->hasFan || desired->fanMode != FAN_MODE_CURVE) {
            set_desired_fan_from_legacy_value(desired, fanAuto, fanPercent);
        }
    }

    GetPrivateProfileStringA("controls", "fan_fixed_pct", "", buf, sizeof(buf), path);
    trim_ascii(buf);
    if (buf[0]) {
        int value = 0;
        if (!parse_int_strict(buf, &value)) {
            set_message(err, errSize, "Invalid fan_fixed_pct in %s", path);
            return false;
        }
        desired->hasFan = true;
        desired->fanMode = (desired->fanMode == FAN_MODE_CURVE) ? FAN_MODE_CURVE : FAN_MODE_FIXED;
        desired->fanAuto = false;
        desired->fanPercent = clamp_percent(value);
    }

    if (!load_fan_curve_config_from_section(path, "fan_curve", &desired->fanCurve, err, errSize)) return false;

    for (int i = 0; i < VF_NUM_POINTS; i++) {
        char key[32];
        StringCchPrintfA(key, ARRAY_COUNT(key), "point%d", i);
        GetPrivateProfileStringA("curve", key, "", buf, sizeof(buf), path);
        trim_ascii(buf);
        if (!buf[0]) continue;
        int v = 0;
        if (!parse_int_strict(buf, &v) || v <= 0) {
            set_message(err, errSize, "Invalid curve point %d in %s", i, path);
            return false;
        }
        desired->hasCurvePoint[i] = true;
        desired->curvePointMHz[i] = (unsigned int)v;
    }

    return true;
}

#include "config_profiles.cpp"

#include "fan_curve_dialog.cpp"

static bool capture_gui_desired_settings(DesiredSettings* desired, bool includeCurrentGlobals, bool expandLockedTail, bool captureAllCurvePoints, char* err, size_t errSize) {
    if (!desired) return false;
    initialize_desired_settings_defaults(desired);

    char buf[64] = {};
    int parsedCurveMHz[VF_NUM_POINTS] = {};
    bool parsedCurveHave[VF_NUM_POINTS] = {};
    bool currentGpuOffsetExcludeLow70 = g_app.appliedGpuOffsetExcludeLow70;
    bool currentActiveGpuOffsetExcludeLow70 = current_applied_gpu_offset_excludes_low_points();
    int currentGpuOffsetMHz = current_applied_gpu_offset_mhz();
    get_window_text_safe(g_app.hGpuOffsetEdit, buf, sizeof(buf));
    int gpuOffsetMHz = currentGpuOffsetMHz;
    if (buf[0]) {
        if (!parse_int_strict(buf, &gpuOffsetMHz)) {
            set_message(err, errSize, "Invalid GPU offset");
            return false;
        }
    }
    bool gpuOffsetExcludeLow70 = g_app.guiGpuOffsetExcludeLow70;
    if (g_app.hGpuOffsetExcludeLowCheck) {
        gpuOffsetExcludeLow70 = SendMessageA(g_app.hGpuOffsetExcludeLowCheck, BM_GETCHECK, 0, 0) == BST_CHECKED;
    }
    bool desiredActiveGpuOffsetExcludeLow70 = gpuOffsetExcludeLow70 && gpuOffsetMHz != 0;
    g_app.guiGpuOffsetMHz = gpuOffsetMHz;
    g_app.guiGpuOffsetExcludeLow70 = gpuOffsetExcludeLow70;

    bool hasLock = g_app.lockedVi >= 0 && g_app.lockedVi < g_app.numVisible;
    int lockCi = -1;
    int effectiveLockTargetMHz = 0;
    bool lockTargetSynthesizedFromGpuOffset = false;
    unsigned int currentLockMHz = 0;
    bool anyExplicitCurvePoint = captureAllCurvePoints || desiredActiveGpuOffsetExcludeLow70 || currentActiveGpuOffsetExcludeLow70;
    if (hasLock) {
        lockCi = g_app.visibleMap[g_app.lockedVi];
        char lockBuf[32] = {};
        get_window_text_safe(g_app.hEditsMhz[g_app.lockedVi], lockBuf, sizeof(lockBuf));
        if (!parse_int_strict(lockBuf, &effectiveLockTargetMHz) || effectiveLockTargetMHz <= 0) {
            set_message(err, errSize, "Invalid MHz value for point %d", lockCi);
            return false;
        }
        currentLockMHz = displayed_curve_mhz(g_app.curve[lockCi].freq_kHz);
        int currentLockGpuOffsetMHz = gpu_offset_component_mhz_for_point(lockCi, currentGpuOffsetMHz, currentActiveGpuOffsetExcludeLow70);
        int desiredLockGpuOffsetMHz = gpu_offset_component_mhz_for_point(lockCi, gpuOffsetMHz, desiredActiveGpuOffsetExcludeLow70);
        if (effectiveLockTargetMHz == (int)currentLockMHz && desiredLockGpuOffsetMHz != currentLockGpuOffsetMHz) {
            effectiveLockTargetMHz += desiredLockGpuOffsetMHz - currentLockGpuOffsetMHz;
            if (effectiveLockTargetMHz <= 0) effectiveLockTargetMHz = 1;
            lockTargetSynthesizedFromGpuOffset = true;
        }
        if (!captureAllCurvePoints && effectiveLockTargetMHz != (int)currentLockMHz) anyExplicitCurvePoint = true;
    }

    for (int vi = 0; vi < g_app.numVisible; vi++) {
        int ci = g_app.visibleMap[vi];
        int mhz = 0;
        if (hasLock && expandLockedTail && vi >= g_app.lockedVi) {
            mhz = effectiveLockTargetMHz;
        } else if (hasLock && vi > g_app.lockedVi) {
            continue;
        } else {
            char pointBuf[32] = {};
            get_window_text_safe(g_app.hEditsMhz[vi], pointBuf, sizeof(pointBuf));
            if (!parse_int_strict(pointBuf, &mhz) || mhz <= 0) {
                set_message(err, errSize, "Invalid MHz value for point %d", ci);
                return false;
            }
        }
        parsedCurveMHz[ci] = mhz;
        parsedCurveHave[ci] = true;
        unsigned int currentMHz = displayed_curve_mhz(g_app.curve[ci].freq_kHz);
        if (!captureAllCurvePoints && (!hasLock || vi < g_app.lockedVi) && (unsigned int)mhz != currentMHz) {
            anyExplicitCurvePoint = true;
        }
    }

    bool synthCurveFromGpuOffset = captureAllCurvePoints || hasLock || anyExplicitCurvePoint;
    int previousRequestedCurveMHz = 0;
    int previousRequestedCurveCi = -1;

    for (int vi = 0; vi < g_app.numVisible; vi++) {
        int ci = g_app.visibleMap[vi];
        if (!parsedCurveHave[ci]) continue;
        bool lockTailPoint = hasLock && expandLockedTail && vi >= g_app.lockedVi;
        int mhz = lockTailPoint ? effectiveLockTargetMHz : parsedCurveMHz[ci];
        unsigned int currentMHz = displayed_curve_mhz(g_app.curve[ci].freq_kHz);
        int effectiveMHz = mhz;
        bool synthesizedFromGpuOffset = synthCurveFromGpuOffset && (unsigned int)mhz == currentMHz;
        if (synthesizedFromGpuOffset) {
            int currentPointGpuOffsetMHz = gpu_offset_component_mhz_for_point(ci, currentGpuOffsetMHz, currentActiveGpuOffsetExcludeLow70);
            int desiredPointGpuOffsetMHz = gpu_offset_component_mhz_for_point(ci, gpuOffsetMHz, desiredActiveGpuOffsetExcludeLow70);
            if (desiredPointGpuOffsetMHz != currentPointGpuOffsetMHz) {
                effectiveMHz += desiredPointGpuOffsetMHz - currentPointGpuOffsetMHz;
                if (effectiveMHz <= 0) effectiveMHz = 1;
            }
        }
        if (previousRequestedCurveCi >= 0 && effectiveMHz < previousRequestedCurveMHz) {
            if (synthesizedFromGpuOffset || (lockTailPoint && lockTargetSynthesizedFromGpuOffset)) {
                effectiveMHz = previousRequestedCurveMHz;
                if (lockTailPoint) {
                    effectiveLockTargetMHz = effectiveMHz;
                }
            } else {
                set_message(err, errSize,
                    "Curve point %d (%d MHz) is below point %d (%d MHz). The VF curve must remain non-decreasing.",
                    ci, effectiveMHz, previousRequestedCurveCi, previousRequestedCurveMHz);
                return false;
            }
        }
        previousRequestedCurveMHz = effectiveMHz;
        previousRequestedCurveCi = ci;
        if (captureAllCurvePoints || (unsigned int)effectiveMHz != currentMHz) {
            desired->hasCurvePoint[ci] = true;
            desired->curvePointMHz[ci] = (unsigned int)effectiveMHz;
        }
    }

    if (includeCurrentGlobals || gpuOffsetMHz != currentGpuOffsetMHz || gpuOffsetExcludeLow70 != currentGpuOffsetExcludeLow70) {
        desired->hasGpuOffset = true;
        desired->gpuOffsetMHz = gpuOffsetMHz;
        desired->gpuOffsetExcludeLow70 = gpuOffsetExcludeLow70;
    }

    int currentMemOffsetMHz = mem_display_mhz_from_driver_khz(g_app.memClockOffsetkHz);
    get_window_text_safe(g_app.hMemOffsetEdit, buf, sizeof(buf));
    int memOffsetMHz = currentMemOffsetMHz;
    if (buf[0]) {
        if (!parse_int_strict(buf, &memOffsetMHz)) {
            set_message(err, errSize, "Invalid memory offset");
            return false;
        }
    }
    if (includeCurrentGlobals || memOffsetMHz != currentMemOffsetMHz) {
        desired->hasMemOffset = true;
        desired->memOffsetMHz = memOffsetMHz;
    }

    int currentPowerLimitPct = g_app.powerLimitPct;
    get_window_text_safe(g_app.hPowerLimitEdit, buf, sizeof(buf));
    int powerLimitPct = currentPowerLimitPct;
    if (buf[0]) {
        if (!parse_int_strict(buf, &powerLimitPct)) {
            set_message(err, errSize, "Invalid power limit");
            return false;
        }
    }
    if (includeCurrentGlobals || powerLimitPct != currentPowerLimitPct) {
        desired->hasPowerLimit = true;
        desired->powerLimitPct = powerLimitPct;
    }

    int selectedFanMode = g_app.guiFanMode;
    if (g_app.hFanModeCombo) {
        LRESULT selection = SendMessageA(g_app.hFanModeCombo, CB_GETCURSEL, 0, 0);
        if (selection >= 0 && selection <= FAN_MODE_CURVE) {
            selectedFanMode = (int)selection;
        }
    }
    get_window_text_safe(g_app.hFanEdit, buf, sizeof(buf));
    int fanPercent = g_app.guiFanFixedPercent;
    if (selectedFanMode == FAN_MODE_FIXED) {
        if (!parse_int_strict(buf, &fanPercent)) {
            set_message(err, errSize, "Invalid fixed fan percentage");
            return false;
        }
        fanPercent = clamp_percent(fanPercent);
    } else if (selectedFanMode == FAN_MODE_AUTO) {
        fanPercent = 0;
    }

    FanCurveConfig guiCurve = g_app.guiFanCurve;
    fan_curve_normalize(&guiCurve);
    if (!fan_curve_validate(&guiCurve, err, errSize)) {
        return false;
    }

    if (includeCurrentGlobals || !fan_setting_matches_current(selectedFanMode, fanPercent, &guiCurve)) {
        desired->hasFan = true;
        desired->fanMode = selectedFanMode;
        desired->fanAuto = selectedFanMode == FAN_MODE_AUTO;
        desired->fanPercent = fanPercent;
        copy_fan_curve(&desired->fanCurve, &guiCurve);
    }

    return true;
}

static bool save_desired_to_config(const char* path, const DesiredSettings* desired, bool useCurrentForUnset, char* err, size_t errSize) {
    return save_desired_to_config_with_startup(path, desired, useCurrentForUnset, CONFIG_STARTUP_PRESERVE, err, errSize);
}

static bool save_current_gui_state_to_config(int startupState, char* err, size_t errSize) {
    DesiredSettings desired = {};
    if (!capture_gui_config_settings(&desired, err, errSize)) return false;
    return save_desired_to_config_with_startup(g_app.configPath, &desired, false, startupState, err, errSize);
}

static bool save_current_gui_state_for_startup(char* err, size_t errSize) {
    return save_current_gui_state_to_config(CONFIG_STARTUP_ENABLE, err, errSize);
}

static bool desired_requires_resident_runtime(const DesiredSettings* desired) {
    return desired && desired->hasFan && desired->fanMode != FAN_MODE_AUTO;
}

static bool logon_profile_requires_resident_runtime(const char* path) {
    if (!path || !*path) return false;

    int logonSlot = get_config_int(path, "profiles", "logon_slot", 0);
    if (logonSlot < 1 || logonSlot > CONFIG_NUM_SLOTS) return false;

    DesiredSettings desired = {};
    char err[256] = {};
    if (!load_profile_from_config(path, logonSlot, &desired, err, sizeof(err))) {
        return false;
    }
    return desired_requires_resident_runtime(&desired);
}

static bool parse_wide_int_arg(LPWSTR text, int* out) {
    if (!text || !out) return false;
    char buf[64] = {};
    int n = WideCharToMultiByte(CP_UTF8, 0, text, -1, buf, (int)sizeof(buf), nullptr, nullptr);
    if (n <= 0) return false;
    trim_ascii(buf);
    return parse_int_strict(buf, out);
}

static bool copy_wide_to_utf8(LPWSTR text, char* out, int outSize) {
    if (!text || !out || outSize < 1) return false;
    int n = WideCharToMultiByte(CP_UTF8, 0, text, -1, out, outSize, nullptr, nullptr);
    if (n <= 0) return false;
    trim_ascii(out);
    return true;
}

static bool utf8_to_wide(const char* text, WCHAR* out, int outCount) {
    if (!text || !out || outCount < 1) return false;
    int n = MultiByteToWideChar(CP_UTF8, 0, text, -1, out, outCount);
    if (n <= 0) return false;
    out[outCount - 1] = 0;
    return true;
}

static bool get_current_user_sam_name(WCHAR* out, DWORD outCount) {
    if (!out || outCount == 0) return false;
    out[0] = 0;

    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;

    DWORD needed = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &needed);
    if (needed == 0) {
        CloseHandle(token);
        return false;
    }

    TOKEN_USER* user = (TOKEN_USER*)malloc(needed);
    if (!user) {
        CloseHandle(token);
        return false;
    }

    WCHAR name[256] = {};
    WCHAR domain[256] = {};
    DWORD nameLen = ARRAY_COUNT(name);
    DWORD domainLen = ARRAY_COUNT(domain);
    SID_NAME_USE use = SidTypeUnknown;
    bool ok = false;
    if (GetTokenInformation(token, TokenUser, user, needed, &needed) &&
        LookupAccountSidW(nullptr, user->User.Sid, name, &nameLen, domain, &domainLen, &use)) {
        if (domain[0]) ok = SUCCEEDED(StringCchPrintfW(out, outCount, L"%ls\\%ls", domain, name));
        else ok = SUCCEEDED(StringCchCopyW(out, outCount, name));
    }

    free(user);
    CloseHandle(token);
    return ok;
}

static bool xml_escape_wide(const WCHAR* text, WCHAR* out, size_t outCount, bool escapeQuotes) {
    if (!text || !out || outCount == 0) return false;
    size_t pos = 0;
    for (const WCHAR* p = text; *p; ++p) {
        const WCHAR* repl = nullptr;
        switch (*p) {
            case L'&': repl = L"&amp;"; break;
            case L'<': repl = L"&lt;"; break;
            case L'>': repl = L"&gt;"; break;
            case L'\"': repl = escapeQuotes ? L"&quot;" : nullptr; break;
            case L'\'': repl = escapeQuotes ? L"&apos;" : nullptr; break;
            default: break;
        }
        if (repl) {
            size_t replLen = wcslen(repl);
            if (pos + replLen >= outCount) return false;
            memcpy(out + pos, repl, replLen * sizeof(WCHAR));
            pos += replLen;
        } else {
            if (pos + 1 >= outCount) return false;
            out[pos++] = *p;
        }
    }
    out[pos] = 0;
    return true;
}

static bool get_startup_task_name(WCHAR* out, size_t outCount) {
    if (!out || outCount == 0) return false;
    WCHAR userName[512] = {};
    if (!get_current_user_sam_name(userName, ARRAY_COUNT(userName))) return false;
    for (WCHAR* p = userName; *p; ++p) {
        if (*p == L'\\' || *p == L'/' || *p == L':' || *p == L'*' || *p == L'?' ||
            *p == L'"' || *p == L'<' || *p == L'>' || *p == L'|') {
            *p = L'_';
        }
    }
    HRESULT hr = StringCchPrintfW(out, outCount, L"%S%ls", STARTUP_TASK_PREFIX, userName);
    return SUCCEEDED(hr);
}

static bool write_startup_task_xml(const WCHAR* xmlPath, const WCHAR* exePath, const WCHAR* cfgPath, char* err, size_t errSize) {
    if (!xmlPath || !exePath || !cfgPath) {
        set_message(err, errSize, "Invalid startup task xml arguments");
        return false;
    }

    const bool residentRuntimeRequired = logon_profile_requires_resident_runtime(g_app.configPath);
    const bool launchProgramAtLogon = is_start_on_logon_enabled(g_app.configPath) || residentRuntimeRequired;
    const WCHAR* description = residentRuntimeRequired
        ? L"Launch Green Curve in the tray at user logon to maintain manual fan control."
        : (launchProgramAtLogon
            ? L"Launch Green Curve in the tray at user logon."
            : L"Apply Green Curve startup settings at user logon.");

    HANDLE h = CreateFileW(xmlPath, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        set_message(err, errSize, "Cannot create startup task XML (error %lu)", GetLastError());
        return false;
    }

    const WCHAR* xmlFmt =
        L"<?xml version=\"1.0\" encoding=\"UTF-16\"?>\r\n"
        L"<Task version=\"1.3\" xmlns=\"http://schemas.microsoft.com/windows/2004/02/mit/task\">\r\n"
        L"  <RegistrationInfo>\r\n"
        L"    <Author>%ls</Author>\r\n"
        L"    <Description>%ls</Description>\r\n"
        L"  </RegistrationInfo>\r\n"
        L"  <Triggers>\r\n"
        L"    <LogonTrigger>\r\n"
        L"      <Enabled>true</Enabled>\r\n"
        L"      <UserId>%ls</UserId>\r\n"
        L"      <Delay>PT15S</Delay>\r\n"
        L"    </LogonTrigger>\r\n"
        L"  </Triggers>\r\n"
        L"  <Principals>\r\n"
        L"    <Principal id=\"Author\">\r\n"
        L"      <UserId>%ls</UserId>\r\n"
        L"      <LogonType>InteractiveToken</LogonType>\r\n"
        L"      <RunLevel>HighestAvailable</RunLevel>\r\n"
        L"    </Principal>\r\n"
        L"  </Principals>\r\n"
        L"  <Settings>\r\n"
        L"    <MultipleInstancesPolicy>IgnoreNew</MultipleInstancesPolicy>\r\n"
        L"    <DisallowStartIfOnBatteries>false</DisallowStartIfOnBatteries>\r\n"
        L"    <StopIfGoingOnBatteries>false</StopIfGoingOnBatteries>\r\n"
        L"    <AllowHardTerminate>true</AllowHardTerminate>\r\n"
        L"    <StartWhenAvailable>true</StartWhenAvailable>\r\n"
        L"    <AllowStartOnDemand>true</AllowStartOnDemand>\r\n"
        L"    <Enabled>true</Enabled>\r\n"
        L"    <Hidden>false</Hidden>\r\n"
        L"    <ExecutionTimeLimit>PT10M</ExecutionTimeLimit>\r\n"
        L"    <Priority>7</Priority>\r\n"
        L"  </Settings>\r\n"
        L"  <Actions Context=\"Author\">\r\n"
        L"    <Exec>\r\n"
        L"      <Command>%ls</Command>\r\n"
        L"      <Arguments>%ls</Arguments>\r\n"
        L"    </Exec>\r\n"
        L"  </Actions>\r\n"
        L"</Task>\r\n";

    WCHAR userName[512] = {};
    if (!get_current_user_sam_name(userName, ARRAY_COUNT(userName))) {
        CloseHandle(h);
        DeleteFileW(xmlPath);
        set_message(err, errSize, "Failed to determine current user");
        return false;
    }

    WCHAR exeEsc[2048] = {};
    WCHAR cfgEsc[2048] = {};
    WCHAR userEsc[1024] = {};
    WCHAR argsEsc[2048] = {};
    if (!xml_escape_wide(exePath, exeEsc, ARRAY_COUNT(exeEsc), false) ||
        !xml_escape_wide(cfgPath, cfgEsc, ARRAY_COUNT(cfgEsc), true) ||
        !xml_escape_wide(userName, userEsc, ARRAY_COUNT(userEsc), false)) {
        CloseHandle(h);
        DeleteFileW(xmlPath);
        set_message(err, errSize, "Failed escaping startup task XML");
        return false;
    }

    HRESULT argsHr = StringCchPrintfW(
        argsEsc,
        ARRAY_COUNT(argsEsc),
        launchProgramAtLogon ? L"--logon-start --config &quot;%ls&quot;" : L"--apply-config --config &quot;%ls&quot;",
        cfgEsc);
    if (FAILED(argsHr)) {
        CloseHandle(h);
        DeleteFileW(xmlPath);
        set_message(err, errSize, "Startup task arguments too long");
        return false;
    }

    WCHAR xml[8192] = {};
    HRESULT hr = StringCchPrintfW(xml, ARRAY_COUNT(xml), xmlFmt, userEsc, description, userEsc, userEsc, exeEsc, argsEsc);
    if (FAILED(hr)) {
        CloseHandle(h);
        DeleteFileW(xmlPath);
        set_message(err, errSize, "Startup task XML too long");
        return false;
    }

    DWORD bytesToWrite = (DWORD)(wcslen(xml) * sizeof(WCHAR));
    WORD bom = 0xFEFF;
    DWORD written = 0;
    bool ok = WriteFile(h, &bom, sizeof(bom), &written, nullptr) != 0 && written == sizeof(bom);
    if (ok) ok = WriteFile(h, xml, bytesToWrite, &written, nullptr) != 0 && written == bytesToWrite;
    CloseHandle(h);
    if (!ok) {
        DeleteFileW(xmlPath);
        set_message(err, errSize, "Failed writing startup task XML (error %lu)", GetLastError());
        return false;
    }
    return true;
}

static bool run_process_wait(const WCHAR* applicationName, WCHAR* commandLine, DWORD timeoutMs, DWORD* exitCode, char* err, size_t errSize) {
    if (exitCode) *exitCode = (DWORD)-1;
    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi = {};
    if (!CreateProcessW(applicationName, commandLine, nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        set_message(err, errSize, "CreateProcess failed (%lu)", GetLastError());
        return false;
    }
    DWORD waitResult = WaitForSingleObject(pi.hProcess, timeoutMs);
    if (waitResult == WAIT_TIMEOUT) {
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        set_message(err, errSize, "Command timed out");
        return false;
    }
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    if (exitCode) *exitCode = code;
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return true;
}

static bool run_schtasks_command(const WCHAR* args, DWORD* exitCode, char* err, size_t errSize) {
    WCHAR schtasksPath[MAX_PATH] = {};
    UINT pathLen = GetSystemDirectoryW(schtasksPath, ARRAY_COUNT(schtasksPath));
    if (pathLen == 0 || pathLen >= ARRAY_COUNT(schtasksPath) ||
        FAILED(StringCchCatW(schtasksPath, ARRAY_COUNT(schtasksPath), L"\\schtasks.exe"))) {
        set_message(err, errSize, "Failed locating schtasks.exe");
        return false;
    }

    WCHAR commandLine[2048] = {};
    if (FAILED(StringCchPrintfW(commandLine, ARRAY_COUNT(commandLine), L"\"%ls\" %ls", schtasksPath, args))) {
        set_message(err, errSize, "Scheduled task command too long");
        return false;
    }
    return run_process_wait(schtasksPath, commandLine, 15000, exitCode, err, errSize);
}

static bool is_startup_task_enabled() {
    WCHAR taskName[256] = {};
    if (!get_startup_task_name(taskName, ARRAY_COUNT(taskName))) return false;

    WCHAR queryArgs[512] = {};
    if (FAILED(StringCchPrintfW(queryArgs, ARRAY_COUNT(queryArgs), L"/query /tn \"%ls\"", taskName))) return false;

    DWORD exitCode = 0;
    char err[128] = {};
    if (!run_schtasks_command(queryArgs, &exitCode, err, sizeof(err))) return false;
    return exitCode == 0;
}

static bool load_startup_enabled_from_config(const char* path, bool* enabled) {
    if (enabled) *enabled = false;
    if (!path || !enabled) return false;

    if (is_start_on_logon_enabled(path)) {
        *enabled = true;
        return true;
    }

    if (get_config_int(path, "profiles", "logon_slot", 0) > 0) {
        *enabled = true;
        return true;
    }

    char buf[16] = {};
    GetPrivateProfileStringA("startup", "apply_on_launch", "", buf, sizeof(buf), path);
    trim_ascii(buf);
    if (!buf[0]) return false;

    int value = 0;
    if (!parse_int_strict(buf, &value)) return false;
    *enabled = value != 0;
    return true;
}

static void sync_logon_combo_from_system() {
    if (!g_app.hLogonCombo) return;

    int logonSlot = get_config_int(g_app.configPath, "profiles", "logon_slot", 0);
    if (logonSlot < 0 || logonSlot > CONFIG_NUM_SLOTS) logonSlot = 0;
    bool shouldEnableTask = should_enable_startup_task_from_config(g_app.configPath);
    bool residentRuntimeRequired = logon_profile_requires_resident_runtime(g_app.configPath);
    bool taskExists = is_startup_task_enabled();

    if (logonSlot > 0 && !is_profile_slot_saved(g_app.configPath, logonSlot)) {
        logonSlot = 0;
        set_config_int(g_app.configPath, "profiles", "logon_slot", 0);
        shouldEnableTask = should_enable_startup_task_from_config(g_app.configPath);
        residentRuntimeRequired = false;
    }

    if (shouldEnableTask && (!taskExists || residentRuntimeRequired)) {
        char err[256] = {};
        if (!set_startup_task_enabled(true, err, sizeof(err)) && err[0]) {
            debug_log("startup task sync failed: %s\n", err);
        }
        taskExists = is_startup_task_enabled();
    }
    SendMessageA(g_app.hLogonCombo, CB_SETCURSEL, (WPARAM)logonSlot, 0);
    update_profile_state_label();
}

static DWORD WINAPI logon_sync_thread_proc(void* param) {
    HWND hwnd = (HWND)param;
    int logonSlot = get_config_int(g_app.configPath, "profiles", "logon_slot", 0);
    if (logonSlot < 0 || logonSlot > CONFIG_NUM_SLOTS) logonSlot = 0;
    bool shouldEnableTask = should_enable_startup_task_from_config(g_app.configPath);
    bool residentRuntimeRequired = logon_profile_requires_resident_runtime(g_app.configPath);
    bool taskExists = is_startup_task_enabled();

    if (logonSlot > 0 && !is_profile_slot_saved(g_app.configPath, logonSlot)) {
        logonSlot = 0;
        set_config_int(g_app.configPath, "profiles", "logon_slot", 0);
        shouldEnableTask = should_enable_startup_task_from_config(g_app.configPath);
        residentRuntimeRequired = false;
    }

    if (shouldEnableTask && (!taskExists || residentRuntimeRequired)) {
        char err[256] = {};
        if (!set_startup_task_enabled(true, err, sizeof(err)) && err[0]) {
            debug_log("startup task sync failed: %s\n", err);
        }
        taskExists = is_startup_task_enabled();
    }
    PostMessageA(hwnd, APP_WM_SYNC_STARTUP, (WPARAM)logonSlot, 0);
    return 0;
}

static void schedule_logon_combo_sync() {
    if (!g_app.hMainWnd || g_app.startupSyncInFlight) return;
    g_app.startupSyncInFlight = true;
    DWORD threadId = 0;
    HANDLE thread = CreateThread(nullptr, 0, logon_sync_thread_proc, g_app.hMainWnd, 0, &threadId);
    if (!thread) {
        g_app.startupSyncInFlight = false;
        close_startup_sync_thread_handle();
        sync_logon_combo_from_system();
        return;
    }
    close_startup_sync_thread_handle();
    g_app.hStartupSyncThread = thread;
}

static bool set_startup_task_enabled(bool enabled, char* err, size_t errSize) {
    WCHAR taskName[256] = {};
    if (!get_startup_task_name(taskName, ARRAY_COUNT(taskName))) {
        set_message(err, errSize, "Failed to determine startup task name");
        return false;
    }

    DWORD exitCode = 0;
    if (!enabled) {
        WCHAR deleteArgs[512] = {};
        if (FAILED(StringCchPrintfW(deleteArgs, ARRAY_COUNT(deleteArgs), L"/delete /tn \"%ls\" /f", taskName))) {
            set_message(err, errSize, "Scheduled task delete command too long");
            return false;
        }
        if (!run_schtasks_command(deleteArgs, &exitCode, err, errSize)) return false;
        if (exitCode != 0 && exitCode != 1) {
            set_message(err, errSize, "Failed deleting startup task (exit %lu)", exitCode);
            return false;
        }
        if (is_startup_task_enabled()) {
            set_message(err, errSize, "Startup task still exists after delete");
            return false;
        }
        return true;
    }

    WCHAR exePath[MAX_PATH] = {};
    WCHAR cfgPath[MAX_PATH] = {};
    WCHAR tempDir[MAX_PATH] = {};
    WCHAR xmlPath[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exePath, ARRAY_COUNT(exePath));
    if (!utf8_to_wide(g_app.configPath, cfgPath, ARRAY_COUNT(cfgPath))) {
        set_message(err, errSize, "Failed converting config path");
        return false;
    }

    DWORD tempLen = GetTempPathW(ARRAY_COUNT(tempDir), tempDir);
    if (tempLen == 0 || tempLen >= ARRAY_COUNT(tempDir) || !GetTempFileNameW(tempDir, L"gct", 0, xmlPath)) {
        set_message(err, errSize, "Failed creating startup task temp file");
        return false;
    }

    if (!write_startup_task_xml(xmlPath, exePath, cfgPath, err, errSize)) {
        DeleteFileW(xmlPath);
        return false;
    }

    WCHAR createArgs[2048] = {};
    HRESULT hr = StringCchPrintfW(createArgs, ARRAY_COUNT(createArgs),
        L"/create /f /tn \"%ls\" /xml \"%ls\"",
        taskName, xmlPath);
    if (FAILED(hr)) {
        DeleteFileW(xmlPath);
        set_message(err, errSize, "Scheduled task create command too long");
        return false;
    }

    bool runOk = run_schtasks_command(createArgs, &exitCode, err, errSize);
    DeleteFileW(xmlPath);
    if (!runOk) return false;
    if (exitCode != 0) {
        set_message(err, errSize, "Failed creating startup task (exit %lu)", exitCode);
        return false;
    }
    if (!is_startup_task_enabled()) {
        set_message(err, errSize, "Startup task creation did not persist");
        return false;
    }
    return true;
}

static bool parse_cli_options(LPWSTR cmdLine, CliOptions* opts) {
    if (!opts) return false;
    memset(opts, 0, sizeof(*opts));
    initialize_desired_settings_defaults(&opts->desired);

    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(cmdLine, &argc);
    if (!argv) return false;

    for (int i = 1; i < argc; i++) {
        LPWSTR arg = argv[i];
        if (!arg) continue;
        if (wcscmp(arg, L"--help") == 0 || wcscmp(arg, L"-h") == 0) {
            opts->recognized = true;
            opts->showHelp = true;
        } else if (wcscmp(arg, L"--dump") == 0) {
            opts->recognized = true;
            opts->dump = true;
        } else if (wcscmp(arg, L"--json") == 0) {
            opts->recognized = true;
            opts->json = true;
        } else if (wcscmp(arg, L"--probe") == 0) {
            opts->recognized = true;
            opts->probe = true;
        } else if (wcscmp(arg, L"--reset") == 0) {
            opts->recognized = true;
            opts->reset = true;
        } else if (wcscmp(arg, L"--save-config") == 0) {
            opts->recognized = true;
            opts->saveConfig = true;
        } else if (wcscmp(arg, L"--apply-config") == 0) {
            opts->recognized = true;
            opts->applyConfig = true;
        } else if (wcscmp(arg, L"--logon-start") == 0) {
            opts->recognized = true;
            opts->logonStart = true;
        } else if (wcscmp(arg, L"--config") == 0) {
            opts->recognized = true;
            if (i + 1 >= argc || !copy_wide_to_utf8(argv[++i], opts->configPath, MAX_PATH)) {
                set_message(opts->error, sizeof(opts->error), "Invalid --config path");
                LocalFree(argv);
                return false;
            }
            opts->hasConfigPath = true;
        } else if (wcscmp(arg, L"--gpu-offset") == 0) {
            opts->recognized = true;
            int v = 0;
            if (i + 1 >= argc || !parse_wide_int_arg(argv[++i], &v)) {
                set_message(opts->error, sizeof(opts->error), "Invalid --gpu-offset value");
                LocalFree(argv);
                return false;
            }
            opts->desired.hasGpuOffset = true;
            opts->desired.gpuOffsetMHz = v;
        } else if (wcscmp(arg, L"--mem-offset") == 0) {
            opts->recognized = true;
            int v = 0;
            if (i + 1 >= argc || !parse_wide_int_arg(argv[++i], &v)) {
                set_message(opts->error, sizeof(opts->error), "Invalid --mem-offset value");
                LocalFree(argv);
                return false;
            }
            opts->desired.hasMemOffset = true;
            opts->desired.memOffsetMHz = v;
        } else if (wcscmp(arg, L"--power-limit") == 0) {
            opts->recognized = true;
            int v = 0;
            if (i + 1 >= argc || !parse_wide_int_arg(argv[++i], &v)) {
                set_message(opts->error, sizeof(opts->error), "Invalid --power-limit value");
                LocalFree(argv);
                return false;
            }
            opts->desired.hasPowerLimit = true;
            opts->desired.powerLimitPct = v;
        } else if (wcscmp(arg, L"--fan") == 0) {
            opts->recognized = true;
            char buf[64] = {};
            if (i + 1 >= argc || !copy_wide_to_utf8(argv[++i], buf, sizeof(buf)) ||
                !parse_fan_value(buf, &opts->desired.fanAuto, &opts->desired.fanPercent)) {
                set_message(opts->error, sizeof(opts->error), "Invalid --fan value, use auto or 0-100");
                LocalFree(argv);
                return false;
            }
            opts->desired.hasFan = true;
            opts->desired.fanMode = opts->desired.fanAuto ? FAN_MODE_AUTO : FAN_MODE_FIXED;
        } else if (wcsncmp(arg, L"--point", 7) == 0) {
            opts->recognized = true;
            int idx = _wtoi(arg + 7);
            int v = 0;
            if (idx < 0 || idx >= VF_NUM_POINTS || i + 1 >= argc || !parse_wide_int_arg(argv[++i], &v) || v <= 0) {
                set_message(opts->error, sizeof(opts->error), "Invalid --pointN value");
                LocalFree(argv);
                return false;
            }
            opts->desired.hasCurvePoint[idx] = true;
            opts->desired.curvePointMHz[idx] = (unsigned int)v;
        }
    }

    LocalFree(argv);
    return true;
}

static bool nvml_resolve(void** out, const char* name) {
    if (!g_nvml) return false;
    *out = (void*)GetProcAddress(g_nvml, name);
    return *out != nullptr;
}

static bool nvml_ensure_ready();
static bool nvml_read_power_limit();

static bool nvml_get_offset_range(unsigned int domain, int* minMHz, int* maxMHz, int* currentMHz, char* detail, size_t detailSize) {
    if (!nvml_ensure_ready()) {
        set_message(detail, detailSize, "NVML not ready");
        return false;
    }

    bool ok = false;
    if (g_nvml_api.getClockOffsets && g_nvml_api.getPerformanceState) {
        unsigned int pstate = NVML_PSTATE_UNKNOWN;
        if (g_nvml_api.getPerformanceState(g_app.nvmlDevice, &pstate) == NVML_SUCCESS) {
            nvmlClockOffset_t info = {};
            info.version = nvmlClockOffset_v1;
            info.type = domain;
            info.pstate = pstate;
            nvmlReturn_t r = g_nvml_api.getClockOffsets(g_app.nvmlDevice, &info);
            if (r == NVML_SUCCESS) {
                if (minMHz) *minMHz = info.minClockOffsetMHz;
                if (maxMHz) *maxMHz = info.maxClockOffsetMHz;
                if (currentMHz) *currentMHz = info.clockOffsetMHz;
                g_app.offsetReadPstate = (int)pstate;
                ok = true;
            }
        }
    }

    if (!ok) {
        int mn = 0, mx = 0, cur = 0;
        nvmlReturn_t r1 = NVML_ERROR_NOT_SUPPORTED;
        nvmlReturn_t r2 = NVML_ERROR_NOT_SUPPORTED;
        if (domain == NVML_CLOCK_GRAPHICS) {
            if (g_nvml_api.getGpcClkMinMaxVfOffset) r1 = g_nvml_api.getGpcClkMinMaxVfOffset(g_app.nvmlDevice, &mn, &mx);
            if (g_nvml_api.getGpcClkVfOffset) r2 = g_nvml_api.getGpcClkVfOffset(g_app.nvmlDevice, &cur);
        } else if (domain == NVML_CLOCK_MEM) {
            if (g_nvml_api.getMemClkMinMaxVfOffset) r1 = g_nvml_api.getMemClkMinMaxVfOffset(g_app.nvmlDevice, &mn, &mx);
            if (g_nvml_api.getMemClkVfOffset) r2 = g_nvml_api.getMemClkVfOffset(g_app.nvmlDevice, &cur);
        }
        if (r1 == NVML_SUCCESS || r2 == NVML_SUCCESS) {
            if (minMHz) *minMHz = mn;
            if (maxMHz) *maxMHz = mx;
            if (currentMHz) *currentMHz = cur;
            ok = true;
        } else {
            set_message(detail, detailSize, "%s / %s", nvml_err_name(r1), nvml_err_name(r2));
        }
    }

    return ok;
}

static bool nvml_read_clock_offsets(char* detail, size_t detailSize) {
    int mn = 0, mx = 0, cur = 0;
    bool gpuOk = nvml_get_offset_range(NVML_CLOCK_GRAPHICS, &mn, &mx, &cur, detail, detailSize);
    if (gpuOk) {
        g_app.gpuClockOffsetMinMHz = mn;
        g_app.gpuClockOffsetMaxMHz = mx;
        g_app.gpuClockOffsetkHz = cur * 1000;
        g_app.gpuOffsetRangeKnown = true;
    } else {
        g_app.gpuOffsetRangeKnown = false;
    }

    bool memOk = nvml_get_offset_range(NVML_CLOCK_MEM, &mn, &mx, &cur, detail, detailSize);
    if (memOk) {
        g_app.memClockOffsetMinMHz = mem_display_mhz_from_driver_mhz(mn);
        g_app.memClockOffsetMaxMHz = mem_display_mhz_from_driver_mhz(mx);
        g_app.memClockOffsetkHz = (cur * 1000) / 2;
        g_app.memOffsetRangeKnown = true;
    } else {
        g_app.memOffsetRangeKnown = false;
    }

    return gpuOk || memOk;
}

static bool nvml_set_clock_offset_domain(unsigned int domain, int offsetMHz, bool* exactApplied, char* detail, size_t detailSize) {
    if (exactApplied) *exactApplied = false;
    if (!nvml_ensure_ready()) {
        set_message(detail, detailSize, "NVML not ready");
        return false;
    }

    int saneLimitMHz = (domain == NVML_CLOCK_MEM) ? 10000 : 5000;
    if (offsetMHz < -saneLimitMHz || offsetMHz > saneLimitMHz) {
        set_message(detail, detailSize, "Offset out of sane range");
        return false;
    }

    nvmlReturn_t r = NVML_ERROR_NOT_SUPPORTED;
    if (g_nvml_api.setClockOffsets && g_nvml_api.getPerformanceState) {
        unsigned int statesToTry[2] = { NVML_PSTATE_UNKNOWN, NVML_PSTATE_0 };
        if (g_nvml_api.getPerformanceState(g_app.nvmlDevice, &statesToTry[0]) != NVML_SUCCESS) {
            statesToTry[0] = NVML_PSTATE_0;
        }
        for (int si = 0; si < 2 && r != NVML_SUCCESS; si++) {
            unsigned int pstate = statesToTry[si];
            if (pstate == NVML_PSTATE_UNKNOWN) continue;
            nvmlClockOffset_t info = {};
            info.version = nvmlClockOffset_v1;
            info.type = domain;
            info.pstate = pstate;
            info.clockOffsetMHz = offsetMHz;
            r = g_nvml_api.setClockOffsets(g_app.nvmlDevice, &info);
            if (r == NVML_SUCCESS) g_app.offsetReadPstate = (int)pstate;
        }
    }

    if (r != NVML_SUCCESS) {
        if (domain == NVML_CLOCK_GRAPHICS && g_nvml_api.setGpcClkVfOffset) {
            r = g_nvml_api.setGpcClkVfOffset(g_app.nvmlDevice, offsetMHz);
        } else if (domain == NVML_CLOCK_MEM && g_nvml_api.setMemClkVfOffset) {
            r = g_nvml_api.setMemClkVfOffset(g_app.nvmlDevice, offsetMHz);
        }
    }

    if (r != NVML_SUCCESS) {
        set_message(detail, detailSize, "%s", nvml_err_name(r));
        return false;
    }

    int mn = 0, mx = 0, cur = 0;
    bool readOk = false;
    for (int attempt = 0; attempt < 8; attempt++) {
        if (attempt > 0) Sleep(10);
        if (!nvml_get_offset_range(domain, &mn, &mx, &cur, detail, detailSize)) continue;
        readOk = true;
        if (cur == offsetMHz) break;
    }
    if (!readOk) {
        set_message(detail, detailSize, "write OK, readback failed");
        return true;
    }
    if (exactApplied) *exactApplied = (cur == offsetMHz);
    return true;
}

static bool nvml_read_fans(char* detail, size_t detailSize) {
    if (!nvml_ensure_ready()) {
        g_app.fanSupported = false;
        set_message(detail, detailSize, "NVML not ready");
        return false;
    }

    memset(g_app.fanPercent, 0, sizeof(g_app.fanPercent));
    memset(g_app.fanRpm, 0, sizeof(g_app.fanRpm));
    memset(g_app.fanPolicy, 0, sizeof(g_app.fanPolicy));
    memset(g_app.fanControlSignal, 0, sizeof(g_app.fanControlSignal));
    memset(g_app.fanTargetMask, 0, sizeof(g_app.fanTargetMask));
    g_app.fanCount = 0;
    g_app.fanMinPct = 0;
    g_app.fanMaxPct = 100;
    g_app.fanSupported = false;
    g_app.fanRangeKnown = false;
    g_app.fanIsAuto = true;

    if (!g_nvml_api.getNumFans) {
        set_message(detail, detailSize, "nvmlDeviceGetNumFans missing");
        return false;
    }

    unsigned int count = 0;
    nvmlReturn_t r = g_nvml_api.getNumFans(g_app.nvmlDevice, &count);
    if (r != NVML_SUCCESS || count == 0) {
        set_message(detail, detailSize, "%s", nvml_err_name(r));
        return false;
    }

    g_app.fanSupported = true;
    g_app.fanCount = count > MAX_GPU_FANS ? MAX_GPU_FANS : count;

    if (g_nvml_api.getMinMaxFanSpeed) {
        unsigned int mn = 0, mx = 0;
        if (g_nvml_api.getMinMaxFanSpeed(g_app.nvmlDevice, &mn, &mx) == NVML_SUCCESS) {
            g_app.fanMinPct = mn;
            g_app.fanMaxPct = mx;
            g_app.fanRangeKnown = true;
        }
    }

    bool allAuto = true;
    for (unsigned int fan = 0; fan < g_app.fanCount; fan++) {
        if (g_nvml_api.getFanControlPolicy) {
            unsigned int pol = 0;
            if (g_nvml_api.getFanControlPolicy(g_app.nvmlDevice, fan, &pol) == NVML_SUCCESS) {
                g_app.fanPolicy[fan] = pol;
                if (pol != NVML_FAN_POLICY_TEMPERATURE_CONTINOUS_SW) allAuto = false;
            }
        }
        bool isAutoForFan = true;
        if (g_nvml_api.getFanControlPolicy) {
            unsigned int pol = g_app.fanPolicy[fan];
            isAutoForFan = (pol == NVML_FAN_POLICY_TEMPERATURE_CONTINOUS_SW);
        }
        if (g_nvml_api.getFanSpeed) {
            unsigned int pct = 0;
            if (g_nvml_api.getFanSpeed(g_app.nvmlDevice, fan, &pct) == NVML_SUCCESS) {
                g_app.fanPercent[fan] = pct;
            }
        }
        if (g_nvml_api.getTargetFanSpeed && !isAutoForFan) {
            unsigned int target = 0;
            if (g_nvml_api.getTargetFanSpeed(g_app.nvmlDevice, fan, &target) == NVML_SUCCESS && target > 0) {
                g_app.fanPercent[fan] = target;
            }
        }
        if (g_nvml_api.getFanSpeedRpm) {
            nvmlFanSpeedInfo_t info = {};
            info.version = nvmlFanSpeedInfo_v1;
            info.fan = fan;
            if (g_nvml_api.getFanSpeedRpm(g_app.nvmlDevice, &info) == NVML_SUCCESS) {
                g_app.fanRpm[fan] = info.speed;
            }
        }
        if (g_nvml_api.getCoolerInfo) {
            nvmlCoolerInfo_t info = {};
            info.version = nvmlCoolerInfo_v1;
            info.index = fan;
            if (g_nvml_api.getCoolerInfo(g_app.nvmlDevice, &info) == NVML_SUCCESS) {
                g_app.fanControlSignal[fan] = info.signalType;
                g_app.fanTargetMask[fan] = info.target;
            }
        }
    }
    g_app.fanIsAuto = allAuto;
    return true;
}

static bool nvml_set_fan_auto(char* detail, size_t detailSize) {
    if (!nvml_ensure_ready() || !g_app.fanSupported || !g_nvml_api.setDefaultFanSpeed) {
        set_message(detail, detailSize, "Fan auto unsupported");
        return false;
    }
    for (unsigned int fan = 0; fan < g_app.fanCount; fan++) {
        nvmlReturn_t r = g_nvml_api.setDefaultFanSpeed(g_app.nvmlDevice, fan);
        if (r != NVML_SUCCESS) {
            set_message(detail, detailSize, "fan %u: %s", fan, nvml_err_name(r));
            return false;
        }
    }
    for (int attempt = 0; attempt < 8; attempt++) {
        if (attempt > 0) Sleep(10);
        if (nvml_read_fans(detail, detailSize) && g_app.fanIsAuto) return true;
    }
    return nvml_read_fans(detail, detailSize);
}

static bool nvml_set_fan_manual(int pct, bool* exactApplied, char* detail, size_t detailSize) {
    if (exactApplied) *exactApplied = false;
    if (!nvml_ensure_ready() || !g_app.fanSupported || !g_nvml_api.setFanSpeed) {
        set_message(detail, detailSize, "Fan manual unsupported");
        return false;
    }
    for (unsigned int fan = 0; fan < g_app.fanCount; fan++) {
        if (g_nvml_api.setFanControlPolicy) {
            g_nvml_api.setFanControlPolicy(g_app.nvmlDevice, fan, NVML_FAN_POLICY_MANUAL);
        }
        nvmlReturn_t r = g_nvml_api.setFanSpeed(g_app.nvmlDevice, fan, (unsigned int)pct);
        if (r != NVML_SUCCESS) {
            set_message(detail, detailSize, "fan %u: %s", fan, nvml_err_name(r));
            return false;
        }
    }
    bool ok = false;
    for (int attempt = 0; attempt < 8; attempt++) {
        if (attempt > 0) Sleep(10);
        if (!nvml_read_fans(detail, detailSize)) continue;
        ok = (g_app.fanCount > 0);
        for (unsigned int fan = 0; fan < g_app.fanCount; fan++) {
            int got = (int)g_app.fanPercent[fan];
            if (pct == 0) {
                if (got != 0) ok = false;
            } else {
                if (got == 0 && g_app.fanRpm[fan] > 0 && g_nvml_api.getTargetFanSpeed) {
                    unsigned int target = 0;
                    if (g_nvml_api.getTargetFanSpeed(g_app.nvmlDevice, fan, &target) == NVML_SUCCESS) {
                        got = (int)target;
                        g_app.fanPercent[fan] = target;
                    }
                }
                if (got < pct - 2 || got > pct + 2) ok = false;
            }
        }
        if (ok) break;
    }
    if (exactApplied) *exactApplied = ok;
    return true;
}

static bool fan_setting_matches_current(int wantMode, int wantPct, const FanCurveConfig* wantCurve) {
    if (!g_app.fanSupported) return false;
    if (wantMode != g_app.activeFanMode) return false;
    if (wantMode == FAN_MODE_AUTO) return g_app.fanIsAuto;
    if (wantMode == FAN_MODE_CURVE) {
        return wantCurve && fan_curve_equals(wantCurve, &g_app.activeFanCurve);
    }
    if (g_app.fanIsAuto || g_app.fanCount == 0) return false;
    for (unsigned int fan = 0; fan < g_app.fanCount; fan++) {
        int gotPct = (int)g_app.fanPercent[fan];
        if (gotPct < wantPct - 2 || gotPct > wantPct + 2) return false;
    }
    return true;
}

static bool nvml_ensure_ready() {
    if (g_app.nvmlReady && g_app.nvmlDevice) return true;
    if (!g_nvml) {
        g_nvml = LoadLibraryA("C:\\Program Files\\NVIDIA Corporation\\NVSMI\\nvml.dll");
        if (!g_nvml) g_nvml = LoadLibraryA("nvml.dll");
    }
    if (!g_nvml) return false;

    if (!g_nvml_api.init) {
        nvml_resolve((void**)&g_nvml_api.init, "nvmlInit_v2");
        nvml_resolve((void**)&g_nvml_api.shutdown, "nvmlShutdown");
        nvml_resolve((void**)&g_nvml_api.getHandleByIndex, "nvmlDeviceGetHandleByIndex_v2");
        nvml_resolve((void**)&g_nvml_api.getPowerLimit, "nvmlDeviceGetPowerManagementLimit");
        nvml_resolve((void**)&g_nvml_api.getPowerDefaultLimit, "nvmlDeviceGetPowerManagementDefaultLimit");
        nvml_resolve((void**)&g_nvml_api.getPowerConstraints, "nvmlDeviceGetPowerManagementLimitConstraints");
        nvml_resolve((void**)&g_nvml_api.setPowerLimit, "nvmlDeviceSetPowerManagementLimit");
        nvml_resolve((void**)&g_nvml_api.getClockOffsets, "nvmlDeviceGetClockOffsets");
        nvml_resolve((void**)&g_nvml_api.setClockOffsets, "nvmlDeviceSetClockOffsets");
        nvml_resolve((void**)&g_nvml_api.getPerformanceState, "nvmlDeviceGetPerformanceState");
        nvml_resolve((void**)&g_nvml_api.getGpcClkVfOffset, "nvmlDeviceGetGpcClkVfOffset");
        nvml_resolve((void**)&g_nvml_api.getMemClkVfOffset, "nvmlDeviceGetMemClkVfOffset");
        nvml_resolve((void**)&g_nvml_api.getGpcClkMinMaxVfOffset, "nvmlDeviceGetGpcClkMinMaxVfOffset");
        nvml_resolve((void**)&g_nvml_api.getMemClkMinMaxVfOffset, "nvmlDeviceGetMemClkMinMaxVfOffset");
        nvml_resolve((void**)&g_nvml_api.setGpcClkVfOffset, "nvmlDeviceSetGpcClkVfOffset");
        nvml_resolve((void**)&g_nvml_api.setMemClkVfOffset, "nvmlDeviceSetMemClkVfOffset");
        nvml_resolve((void**)&g_nvml_api.getNumFans, "nvmlDeviceGetNumFans");
        nvml_resolve((void**)&g_nvml_api.getMinMaxFanSpeed, "nvmlDeviceGetMinMaxFanSpeed");
        nvml_resolve((void**)&g_nvml_api.getFanControlPolicy, "nvmlDeviceGetFanControlPolicy_v2");
        nvml_resolve((void**)&g_nvml_api.setFanControlPolicy, "nvmlDeviceSetFanControlPolicy");
        nvml_resolve((void**)&g_nvml_api.getFanSpeed, "nvmlDeviceGetFanSpeed_v2");
        nvml_resolve((void**)&g_nvml_api.getTargetFanSpeed, "nvmlDeviceGetTargetFanSpeed");
        nvml_resolve((void**)&g_nvml_api.getFanSpeedRpm, "nvmlDeviceGetFanSpeedRPM");
        nvml_resolve((void**)&g_nvml_api.setFanSpeed, "nvmlDeviceSetFanSpeed_v2");
        nvml_resolve((void**)&g_nvml_api.setDefaultFanSpeed, "nvmlDeviceSetDefaultFanSpeed_v2");
        nvml_resolve((void**)&g_nvml_api.getCoolerInfo, "nvmlDeviceGetCoolerInfo");
        nvml_resolve((void**)&g_nvml_api.getTemperature, "nvmlDeviceGetTemperature");
        nvml_resolve((void**)&g_nvml_api.getClock, "nvmlDeviceGetClock");
        nvml_resolve((void**)&g_nvml_api.getMaxClock, "nvmlDeviceGetMaxClock");
    }

    if (!g_nvml_api.init || !g_nvml_api.getHandleByIndex) return false;
    nvmlReturn_t r = g_nvml_api.init();
    if (r != NVML_SUCCESS && r != NVML_ERROR_ALREADY_INITIALIZED) return false;
    r = g_nvml_api.getHandleByIndex(0, &g_app.nvmlDevice);
    if (r != NVML_SUCCESS) return false;
    g_app.nvmlReady = true;
    return true;
}

static bool refresh_global_state(char* detail, size_t detailSize) {
    bool ok1 = nvapi_read_pstates();
    bool ok2 = nvml_read_power_limit();
    bool ok3 = nvml_read_clock_offsets(detail, detailSize);
    bool ok4 = nvml_read_fans(detail, detailSize);
    if (!ok3 && !ok1) ok1 = nvapi_read_pstates();
    detect_clock_offsets();
    initialize_gui_fan_settings_from_live_state();
    update_tray_icon();
    return ok1 || ok2 || ok3 || ok4;
}

static bool nvapi_get_vf_info_cached(unsigned char* maskOut, unsigned int* numClocksOut) {
    if (!g_app.vfInfoCached) {
        memset(g_app.vfMask, 0, sizeof(g_app.vfMask));
        memset(g_app.vfMask, 0xFF, 16);
        g_app.vfNumClocks = 15;

        auto getInfo = (NvApiFunc)nvapi_qi(VF_GET_INFO_ID);
        if (getInfo) {
            unsigned char ibuf[0x182C] = {};
            const unsigned int version = (1u << 16) | 0x182C;
            memcpy(&ibuf[0], &version, sizeof(version));
            memset(&ibuf[4], 0xFF, 32);
            if (getInfo(g_app.gpuHandle, ibuf) == 0) {
                memcpy(g_app.vfMask, &ibuf[4], sizeof(g_app.vfMask));
                memcpy(&g_app.vfNumClocks, &ibuf[0x14], sizeof(g_app.vfNumClocks));
                if (g_app.vfNumClocks == 0) g_app.vfNumClocks = 15;
            }
        }
        g_app.vfInfoCached = true;
    }

    if (maskOut) memcpy(maskOut, g_app.vfMask, sizeof(g_app.vfMask));
    if (numClocksOut) *numClocksOut = g_app.vfNumClocks ? g_app.vfNumClocks : 15;
    return true;
}

static int clamp_freq_delta_khz(int freqDelta_kHz) {
    if (freqDelta_kHz > 1000000) return 1000000;
    if (freqDelta_kHz < -1000000) return -1000000;
    return freqDelta_kHz;
}

static bool nvapi_read_control_table(unsigned char* buf, size_t bufSize) {
    const unsigned int CTRL_SIZE = 0x2420;
    if (!buf || bufSize < CTRL_SIZE) return false;

    auto getFunc = (NvApiFunc)nvapi_qi(VF_GET_CONTROL_ID);
    if (!getFunc) return false;

    unsigned char mask[32] = {};
    nvapi_get_vf_info_cached(mask, nullptr);

    memset(buf, 0, CTRL_SIZE);
    const unsigned int version = (1u << 16) | CTRL_SIZE;
    memcpy(&buf[0], &version, sizeof(version));
    memcpy(&buf[4], mask, sizeof(mask));
    return getFunc(g_app.gpuHandle, buf) == 0;
}

static bool apply_curve_offsets_verified(const int* targetOffsets, const bool* pointMask, int maxBatchPasses) {
    if (!targetOffsets || !pointMask) return false;

    bool desiredMask[VF_NUM_POINTS] = {};
    int desiredOffsets[VF_NUM_POINTS] = {};
    bool pendingMask[VF_NUM_POINTS] = {};
    int desiredCount = 0;

    for (int i = 0; i < VF_NUM_POINTS; i++) {
        if (!pointMask[i]) continue;
        if (g_app.curve[i].freq_kHz == 0) continue;
        desiredMask[i] = true;
        pendingMask[i] = true;
        desiredOffsets[i] = clamp_freq_delta_khz(targetOffsets[i]);
        desiredCount++;
    }
    if (desiredCount == 0) return true;

    if (maxBatchPasses < 1) maxBatchPasses = 1;

    auto setFunc = (NvApiFunc)nvapi_qi(VF_SET_CONTROL_ID);
    if (!setFunc) return false;

    unsigned char baseControl[0x2420] = {};
    if (!nvapi_read_control_table(baseControl, sizeof(baseControl))) return false;

    bool anyWrite = false;
    int batchedPoints = 0;
    for (int i = 0; i < VF_NUM_POINTS; i++) {
        if (desiredMask[i]) batchedPoints++;
    }
    bool allowBatch = batchedPoints > 1;
    bool batchFailed = false;
    if (!allowBatch) maxBatchPasses = 0;
    for (int pass = 0; pass < maxBatchPasses; pass++) {
        unsigned char buf[0x2420] = {};
        memcpy(buf, baseControl, sizeof(buf));

        unsigned char writeMask[32] = {};
        bool anyPendingWrite = false;
        for (int i = 0; i < VF_NUM_POINTS; i++) {
            if (!pendingMask[i]) continue;
            int currentDelta = 0;
            memcpy(&currentDelta, &buf[0x44 + i * 0x24 + 0x14], sizeof(currentDelta));
            if (currentDelta == desiredOffsets[i]) {
                pendingMask[i] = false;
                continue;
            }
            memcpy(&buf[0x44 + i * 0x24 + 0x14], &desiredOffsets[i], sizeof(desiredOffsets[i]));
            writeMask[i / 8] |= (unsigned char)(1u << (i % 8));
            anyPendingWrite = true;
        }

        if (!anyPendingWrite) break;

        memcpy(&buf[4], writeMask, sizeof(writeMask));
        int setRet = setFunc(g_app.gpuHandle, buf);
        debug_log("curve batch pass %d: points=%d ret=%d\n", pass + 1, desiredCount, setRet);
        if (setRet != 0) {
            batchFailed = true;
            break;
        }
        anyWrite = true;

        bool readOk = false;
        for (int verifyTry = 0; verifyTry < 6; verifyTry++) {
            if (verifyTry > 0) Sleep(10);
            if (nvapi_read_offsets()) {
                readOk = true;
                break;
            }
        }
        if (!readOk) {
            batchFailed = true;
            break;
        }

        bool anyPending = false;
        for (int i = 0; i < VF_NUM_POINTS; i++) {
            if (!desiredMask[i]) continue;
            pendingMask[i] = (g_app.freqOffsets[i] != desiredOffsets[i]);
            if (pendingMask[i]) anyPending = true;
            memcpy(&baseControl[0x44 + i * 0x24 + 0x14], &g_app.freqOffsets[i], sizeof(g_app.freqOffsets[i]));
        }
        if (!anyPending) break;
    }

    bool allOk = !batchFailed;
    bool hasPending = false;
    for (int i = 0; i < VF_NUM_POINTS; i++) {
        if (!desiredMask[i]) continue;
        if (g_app.freqOffsets[i] != desiredOffsets[i]) {
            pendingMask[i] = true;
            hasPending = true;
        } else {
            pendingMask[i] = false;
        }
    }

    if (hasPending) {
        for (int i = 0; i < VF_NUM_POINTS; i++) {
            if (!pendingMask[i]) continue;
            bool pointOk = nvapi_set_point(i, desiredOffsets[i]);
            debug_log("curve fallback point %d target=%d ok=%d\n", i, desiredOffsets[i], pointOk ? 1 : 0);
            if (!pointOk) {
                allOk = false;
            } else {
                anyWrite = true;
            }
        }

        bool readOk = false;
        for (int verifyTry = 0; verifyTry < 6; verifyTry++) {
            if (verifyTry > 0) Sleep(10);
            if (nvapi_read_offsets()) {
                readOk = true;
                break;
            }
        }
        if (!readOk) {
            allOk = false;
        }
    }

    if (anyWrite) {
        if (!nvapi_read_curve()) allOk = false;
        rebuild_visible_map();
    }

    for (int i = 0; i < VF_NUM_POINTS; i++) {
        if (!desiredMask[i]) continue;
        if (g_app.freqOffsets[i] != desiredOffsets[i]) allOk = false;
    }

    return allOk;
}

static void close_startup_sync_thread_handle() {
    if (g_app.hStartupSyncThread) {
        CloseHandle(g_app.hStartupSyncThread);
        g_app.hStartupSyncThread = nullptr;
    }
}

static void populate_global_controls() {
    initialize_gui_fan_settings_from_live_state();
    int liveGpuOffsetMHz = g_app.gpuClockOffsetkHz / 1000;
    if (!g_app.guiGpuOffsetExcludeLow70) {
        g_app.guiGpuOffsetMHz = liveGpuOffsetMHz;
    }
    if (!g_app.appliedGpuOffsetExcludeLow70) {
        g_app.appliedGpuOffsetExcludeLow70 = false;
        g_app.appliedGpuOffsetMHz = liveGpuOffsetMHz;
    }
    if (g_app.hGpuOffsetEdit) {
        char buf[32];
        int gpuOffsetToShow = g_app.guiGpuOffsetExcludeLow70 ? g_app.guiGpuOffsetMHz : liveGpuOffsetMHz;
        StringCchPrintfA(buf, ARRAY_COUNT(buf), "%d", gpuOffsetToShow);
        SetWindowTextA(g_app.hGpuOffsetEdit, buf);
        EnableWindow(g_app.hGpuOffsetEdit, g_app.gpuOffsetRangeKnown ? TRUE : FALSE);
    }
    if (g_app.hGpuOffsetExcludeLowCheck) {
        SendMessageA(g_app.hGpuOffsetExcludeLowCheck, BM_SETCHECK,
            (WPARAM)(g_app.guiGpuOffsetExcludeLow70 ? BST_CHECKED : BST_UNCHECKED), 0);
        EnableWindow(g_app.hGpuOffsetExcludeLowCheck, g_app.gpuOffsetRangeKnown ? TRUE : FALSE);
    }
    if (g_app.hMemOffsetEdit) {
        char buf[32];
        StringCchPrintfA(buf, ARRAY_COUNT(buf), "%d", mem_display_mhz_from_driver_khz(g_app.memClockOffsetkHz));
        SetWindowTextA(g_app.hMemOffsetEdit, buf);
        EnableWindow(g_app.hMemOffsetEdit, g_app.memOffsetRangeKnown ? TRUE : FALSE);
    }
    if (g_app.hPowerLimitEdit) {
        char buf[32];
        StringCchPrintfA(buf, ARRAY_COUNT(buf), "%d", g_app.powerLimitPct);
        SetWindowTextA(g_app.hPowerLimitEdit, buf);
    }
    update_fan_controls_enabled_state();
}

static int displayed_curve_khz(unsigned int rawFreq_kHz) {
    long long v = (long long)rawFreq_kHz;
    if (v < 0) v = 0;
    return (int)v;
}

static bool capture_gui_apply_settings(DesiredSettings* desired, char* err, size_t errSize) {
    return capture_gui_desired_settings(desired, false, true, false, err, errSize);
}

static bool capture_gui_config_settings(DesiredSettings* desired, char* err, size_t errSize) {
    return capture_gui_desired_settings(desired, true, true, true, err, errSize);
}

static bool save_desired_to_config_with_startup(const char* path, const DesiredSettings* desired, bool useCurrentForUnset, int startupState, char* err, size_t errSize) {
    if (!path || !*path) {
        set_message(err, errSize, "No config path");
        return false;
    }

    if (startupState != CONFIG_STARTUP_PRESERVE) {
        const char* startupText = startupState == CONFIG_STARTUP_ENABLE ? "1" : "0";
        if (!WritePrivateProfileStringA("startup", "apply_on_launch", startupText, path)) {
            set_message(err, errSize, "Failed to write %s", path);
            return false;
        }
    }

    char buf[64];

    int gpuOffset = desired && desired->hasGpuOffset ? desired->gpuOffsetMHz : (g_app.gpuClockOffsetkHz / 1000);
    bool gpuOffsetExcludeLow70 = desired && desired->hasGpuOffset ? desired->gpuOffsetExcludeLow70 : g_app.guiGpuOffsetExcludeLow70;
    int memOffset = desired && desired->hasMemOffset ? desired->memOffsetMHz : mem_display_mhz_from_driver_khz(g_app.memClockOffsetkHz);
    int powerPct = desired && desired->hasPowerLimit ? desired->powerLimitPct : g_app.powerLimitPct;
    int fanMode = desired && desired->hasFan ? desired->fanMode : g_app.activeFanMode;
    int fanPct = desired && desired->hasFan ? clamp_percent(desired->fanPercent) : g_app.activeFanFixedPercent;
    const FanCurveConfig* fanCurve = desired && desired->hasFan ? &desired->fanCurve : &g_app.activeFanCurve;

    StringCchPrintfA(buf, ARRAY_COUNT(buf), "%d", gpuOffset);
    if (!WritePrivateProfileStringA("controls", "gpu_offset_mhz", buf, path)) {
        set_message(err, errSize, "Failed to write gpu_offset_mhz");
        return false;
    }
    if (!WritePrivateProfileStringA("controls", "gpu_offset_exclude_low_70", gpuOffsetExcludeLow70 ? "1" : "0", path)) {
        set_message(err, errSize, "Failed to write gpu_offset_exclude_low_70");
        return false;
    }
    StringCchPrintfA(buf, ARRAY_COUNT(buf), "%d", memOffset);
    if (!WritePrivateProfileStringA("controls", "mem_offset_mhz", buf, path)) {
        set_message(err, errSize, "Failed to write mem_offset_mhz");
        return false;
    }
    StringCchPrintfA(buf, ARRAY_COUNT(buf), "%d", powerPct);
    if (!WritePrivateProfileStringA("controls", "power_limit_pct", buf, path)) {
        set_message(err, errSize, "Failed to write power_limit_pct");
        return false;
    }
    if (!WritePrivateProfileStringA("controls", "fan_mode", fan_mode_to_config_value(fanMode), path)) {
        set_message(err, errSize, "Failed to write fan_mode");
        return false;
    }
    StringCchPrintfA(buf, ARRAY_COUNT(buf), "%d", fanPct);
    if (!WritePrivateProfileStringA("controls", "fan_fixed_pct", buf, path)) {
        set_message(err, errSize, "Failed to write fan_fixed_pct");
        return false;
    }
    if (fanMode == FAN_MODE_AUTO) {
        if (!WritePrivateProfileStringA("controls", "fan", "auto", path)) {
            set_message(err, errSize, "Failed to write fan");
            return false;
        }
    } else {
        if (!WritePrivateProfileStringA("controls", "fan", buf, path)) {
            set_message(err, errSize, "Failed to write fan");
            return false;
        }
    }

    WritePrivateProfileStringA("curve", nullptr, nullptr, path);
    for (int i = 0; i < VF_NUM_POINTS; i++) {
        bool have = desired && desired->hasCurvePoint[i];
        unsigned int mhz = 0;
        if (have) {
            mhz = desired->curvePointMHz[i];
        } else if (useCurrentForUnset && g_app.curve[i].freq_kHz > 0) {
            mhz = displayed_curve_mhz(g_app.curve[i].freq_kHz);
        }
        if (mhz == 0) continue;
        char key[32];
        StringCchPrintfA(key, ARRAY_COUNT(key), "point%d", i);
        StringCchPrintfA(buf, ARRAY_COUNT(buf), "%u", mhz);
        if (!WritePrivateProfileStringA("curve", key, buf, path)) {
            set_message(err, errSize, "Failed to write curve section");
            return false;
        }
    }

    WritePrivateProfileStringA("fan_curve", nullptr, nullptr, path);
    StringCchPrintfA(buf, ARRAY_COUNT(buf), "%d", fanCurve->pollIntervalMs);
    if (!WritePrivateProfileStringA("fan_curve", "poll_interval_ms", buf, path)) {
        set_message(err, errSize, "Failed to write fan curve poll interval");
        return false;
    }
    StringCchPrintfA(buf, ARRAY_COUNT(buf), "%d", fanCurve->hysteresisC);
    if (!WritePrivateProfileStringA("fan_curve", "hysteresis_c", buf, path)) {
        set_message(err, errSize, "Failed to write fan curve hysteresis");
        return false;
    }
    for (int i = 0; i < FAN_CURVE_MAX_POINTS; i++) {
        char key[32] = {};
        StringCchPrintfA(key, ARRAY_COUNT(key), "enabled%d", i);
        if (!WritePrivateProfileStringA("fan_curve", key, fanCurve->points[i].enabled ? "1" : "0", path)) {
            set_message(err, errSize, "Failed to write fan curve enabled flag");
            return false;
        }
        StringCchPrintfA(key, ARRAY_COUNT(key), "temp%d", i);
        StringCchPrintfA(buf, ARRAY_COUNT(buf), "%d", fanCurve->points[i].temperatureC);
        if (!WritePrivateProfileStringA("fan_curve", key, buf, path)) {
            set_message(err, errSize, "Failed to write fan curve temperature");
            return false;
        }
        StringCchPrintfA(key, ARRAY_COUNT(key), "pct%d", i);
        StringCchPrintfA(buf, ARRAY_COUNT(buf), "%d", fanCurve->points[i].fanPercent);
        if (!WritePrivateProfileStringA("fan_curve", key, buf, path)) {
            set_message(err, errSize, "Failed to write fan curve percentage");
            return false;
        }
    }

    return true;
}

static unsigned int displayed_curve_mhz(unsigned int rawFreq_kHz) {
    return (unsigned int)(displayed_curve_khz(rawFreq_kHz) / 1000);
}

static unsigned int curve_point_verify_tolerance_mhz(int pointIndex) {
    if (pointIndex < 0 || pointIndex >= VF_NUM_POINTS || g_app.curve[pointIndex].freq_kHz == 0) {
        return 1;
    }

    unsigned int actualMHz = displayed_curve_mhz(g_app.curve[pointIndex].freq_kHz);
    auto nearest_distinct_neighbor_distance_mhz = [&](int startIndex, int step) -> unsigned int {
        for (int ci = startIndex; ci >= 0 && ci < VF_NUM_POINTS; ci += step) {
            if (g_app.curve[ci].freq_kHz == 0) continue;
            unsigned int neighborMHz = displayed_curve_mhz(g_app.curve[ci].freq_kHz);
            if (neighborMHz == actualMHz) continue;
            return actualMHz > neighborMHz ? (actualMHz - neighborMHz) : (neighborMHz - actualMHz);
        }
        return 0;
    };

    unsigned int leftDistanceMHz = nearest_distinct_neighbor_distance_mhz(pointIndex - 1, -1);
    unsigned int rightDistanceMHz = nearest_distinct_neighbor_distance_mhz(pointIndex + 1, 1);
    unsigned int minDistanceMHz = 0;
    if (leftDistanceMHz && rightDistanceMHz) {
        minDistanceMHz = (unsigned int)nvmin((int)leftDistanceMHz, (int)rightDistanceMHz);
    } else {
        minDistanceMHz = leftDistanceMHz ? leftDistanceMHz : rightDistanceMHz;
    }

    if (minDistanceMHz == 0) return 8;

    unsigned int toleranceMHz = (minDistanceMHz + 1) / 2;
    if (toleranceMHz < 1) toleranceMHz = 1;
    if (toleranceMHz > 8) toleranceMHz = 8;
    return toleranceMHz;
}

static bool curve_targets_match_request(const DesiredSettings* desired, const bool* lockedTailMask, unsigned int lockMhz, char* detail, size_t detailSize) {
    if (!desired) {
        set_message(detail, detailSize, "No requested curve state to verify");
        return false;
    }

    auto matches_target = [](int pointIndex, unsigned int actualMHz, unsigned int targetMHz) -> bool {
        unsigned int toleranceMHz = curve_point_verify_tolerance_mhz(pointIndex);
        int diff = (int)actualMHz - (int)targetMHz;
        return diff >= -(int)toleranceMHz && diff <= (int)toleranceMHz;
    };

    for (int ci = 0; ci < VF_NUM_POINTS; ci++) {
        if (!desired->hasCurvePoint[ci]) continue;
        if (lockedTailMask && lockedTailMask[ci]) continue;
        if (g_app.curve[ci].freq_kHz == 0) continue;

        unsigned int actualMHz = displayed_curve_mhz(g_app.curve[ci].freq_kHz);
        unsigned int targetMHz = desired->curvePointMHz[ci];
        if (!matches_target(ci, actualMHz, targetMHz)) {
            set_message(detail, detailSize,
                "VF point %d verified at %u MHz instead of requested %u MHz",
                ci, actualMHz, targetMHz);
            return false;
        }
    }

    if (lockedTailMask && lockMhz > 0) {
        bool sawTailPoint = false;
        for (int ci = 0; ci < VF_NUM_POINTS; ci++) {
            if (!lockedTailMask[ci]) continue;
            if (g_app.curve[ci].freq_kHz == 0) continue;

            sawTailPoint = true;
            unsigned int actualMHz = displayed_curve_mhz(g_app.curve[ci].freq_kHz);
            if (!matches_target(ci, actualMHz, lockMhz)) {
                set_message(detail, detailSize,
                    "Lock tail verified at %u MHz @ %u mV instead of requested %u MHz",
                    actualMHz,
                    g_app.curve[ci].volt_uV / 1000,
                    lockMhz);
                return false;
            }
        }
        if (!sawTailPoint) {
            set_message(detail, detailSize, "No VF points were available to verify the curve lock");
            return false;
        }
    }

    if (detail && detailSize > 0) detail[0] = 0;
    return true;
}

static int raw_curve_khz_from_display_mhz(unsigned int displayMHz) {
    long long v = (long long)displayMHz * 1000LL;
    if (v < 0) v = 0;
    return (int)v;
}

static int curve_base_khz_for_point(int pointIndex) {
    if (pointIndex < 0 || pointIndex >= VF_NUM_POINTS) return 0;
    long long base = (long long)g_app.curve[pointIndex].freq_kHz - (long long)g_app.freqOffsets[pointIndex];
    if (base < 0) base = 0;
    return (int)base;
}

static int curve_delta_khz_for_target_display_mhz(int pointIndex, unsigned int displayMHz) {
    long long target = (long long)raw_curve_khz_from_display_mhz(displayMHz);
    long long base = (long long)curve_base_khz_for_point(pointIndex);
    long long delta = target - base;
    if (delta > 1000000LL) delta = 1000000LL;
    if (delta < -1000000LL) delta = -1000000LL;
    return (int)delta;
}

static int mem_display_mhz_from_driver_khz(int driver_kHz) {
    return driver_kHz / 1000; // actual clock kHz to actual MHz
}

static int mem_driver_khz_from_display_mhz(int displayMHz) {
    return displayMHz * 1000; // actual clock kHz
}

static int mem_display_mhz_from_driver_mhz(int driverMHz) {
    return driverMHz / 2; // NVML memory offset MHz is effective; UI mirrors actual MHz like Afterburner
}

static void invalidate_main_window() {
    if (!g_app.hMainWnd) return;
    InvalidateRect(g_app.hMainWnd, nullptr, FALSE);
    UpdateWindow(g_app.hMainWnd);
}

static void redraw_window_sync(HWND hwnd) {
    if (!hwnd) return;
    RedrawWindow(hwnd, nullptr, nullptr,
        RDW_INVALIDATE | RDW_UPDATENOW | RDW_ALLCHILDREN | RDW_ERASE | RDW_FRAME);
}

static void flush_desktop_composition() {
    typedef HRESULT (WINAPI *dwm_flush_t)();
    static dwm_flush_t dwmFlush = nullptr;
    static bool resolved = false;
    if (!resolved) {
        HMODULE dwm = LoadLibraryA("dwmapi.dll");
        if (dwm) dwmFlush = (dwm_flush_t)GetProcAddress(dwm, "DwmFlush");
        resolved = true;
    }
    if (dwmFlush) dwmFlush();
}

static void show_window_with_primed_first_frame(HWND hwnd, int nCmdShow) {
    if (!hwnd) return;

    RECT wr = {};
    GetWindowRect(hwnd, &wr);
    int winW = wr.right - wr.left;
    int winH = wr.bottom - wr.top;

    SetWindowPos(hwnd, nullptr, -32000, -32000, 0, 0,
        SWP_NOZORDER | SWP_NOSIZE | SWP_NOACTIVATE);
    ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    redraw_window_sync(hwnd);
    flush_desktop_composition();

    SetWindowPos(hwnd, nullptr, wr.left, wr.top, winW, winH,
        SWP_NOZORDER | SWP_NOACTIVATE);
    ShowWindow(hwnd, nCmdShow);
    redraw_window_sync(hwnd);
}

static void draw_lock_checkbox(const DRAWITEMSTRUCT* dis) {
    if (!dis) return;
    HDC hdc = dis->hDC;
    RECT rc = dis->rcItem;
    bool disabled = (dis->itemState & ODS_DISABLED) != 0;
    bool checked = SendMessageA(dis->hwndItem, BM_GETCHECK, 0, 0) == BST_CHECKED;
    if (!checked && !disabled) {
        int vi = (int)dis->CtlID - LOCK_BASE_ID;
        if (vi >= 0 && vi == g_app.lockedVi) checked = true;
    }

    HBRUSH bg = CreateSolidBrush(COL_BG);
    FillRect(hdc, &rc, bg);
    DeleteObject(bg);

    int boxSize = nvmin(rc.right - rc.left, rc.bottom - rc.top) - dp(4);
    if (boxSize < dp(10)) boxSize = dp(10);
    RECT box = {
        rc.left + (rc.right - rc.left - boxSize) / 2,
        rc.top + (rc.bottom - rc.top - boxSize) / 2,
        rc.left + (rc.right - rc.left - boxSize) / 2 + boxSize,
        rc.top + (rc.bottom - rc.top - boxSize) / 2 + boxSize,
    };

    COLORREF border = disabled ? RGB(0x6A, 0x6A, 0x78) : COL_TEXT;
    COLORREF fill = disabled ? RGB(0x36, 0x36, 0x46) : RGB(0x16, 0x16, 0x24);
    HBRUSH fillBr = CreateSolidBrush(fill);
    FillRect(hdc, &box, fillBr);
    DeleteObject(fillBr);

    HPEN pen = CreatePen(PS_SOLID, 1, border);
    HPEN oldPen = (HPEN)SelectObject(hdc, pen);
    HBRUSH oldBrush = (HBRUSH)SelectObject(hdc, GetStockObject(HOLLOW_BRUSH));
    Rectangle(hdc, box.left, box.top, box.right, box.bottom);
    SelectObject(hdc, oldBrush);
    DeleteObject(SelectObject(hdc, oldPen));

    if (checked) {
        HPEN checkPen = CreatePen(PS_SOLID, 2, disabled ? RGB(0x96, 0x96, 0xA6) : RGB(0xE8, 0xE8, 0xF0));
        oldPen = (HPEN)SelectObject(hdc, checkPen);
        int x1 = box.left + boxSize / 5;
        int y1 = box.top + boxSize / 2;
        int x2 = box.left + boxSize / 2 - 1;
        int y2 = box.bottom - boxSize / 4;
        int x3 = box.right - boxSize / 6;
        int y3 = box.top + boxSize / 4;
        MoveToEx(hdc, x1, y1, nullptr);
        LineTo(hdc, x2, y2);
        LineTo(hdc, x3, y3);
        DeleteObject(SelectObject(hdc, oldPen));
    }

    if (dis->itemState & ODS_FOCUS) {
        RECT focus = rc;
        InflateRect(&focus, -1, -1);
        DrawFocusRect(hdc, &focus);
    }
}

#include "gpu_backend.cpp"

#include "ui_main.cpp"

#include "entry.cpp"
