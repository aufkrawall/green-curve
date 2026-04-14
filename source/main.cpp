// Green Curve v0.8 - NVIDIA VF Curve Editor
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
static bool nvapi_get_interface_version_string(char* text, size_t textSize);
static bool nvapi_get_error_message(int status, char* text, size_t textSize);
static const char* gpu_family_name(GpuFamily family);
static bool nvapi_read_gpu_metadata();
static bool select_vf_backend_for_current_gpu();
static const VfBackendSpec* probe_backend_for_current_gpu();
static bool vf_curve_global_gpu_offset_supported();
static bool vf_backend_is_best_guess(const VfBackendSpec* backend);
static bool should_show_best_guess_warning();
static void show_best_guess_support_warning(HWND parent);
static void detect_clock_offsets();
static int uniform_curve_offset_khz();
static void set_curve_offset_range_khz(int minkHz, int maxkHz);
static bool get_curve_offset_range_khz(int* minkHz, int* maxkHz);
static int clamp_freq_delta_khz(int freqDelta_kHz);
static bool nvapi_set_point(int pointIndex, int freqDelta_kHz);
static bool apply_curve_offsets_verified(const int* targetOffsets, const bool* pointMask, int maxBatchPasses);
static void close_startup_sync_thread_handle();
static void invalidate_main_window();
static bool refresh_global_state(char* detail, size_t detailSize);
static void populate_global_controls();
static void redraw_window_sync(HWND hwnd);
static void fill_window_background(HWND hwnd, HDC hdc);
static void flush_desktop_composition();
static void show_window_with_primed_first_frame(HWND hwnd, int nCmdShow);
static bool is_system_dark_theme_active();
static void initialize_dark_mode_support();
static void refresh_menu_theme_cache();
static void allow_dark_mode_for_window(HWND hwnd);
static const char* ui_font_face_name();
static HFONT get_ui_font();
static void apply_ui_font(HWND hwnd);
static void apply_ui_font_to_children(HWND parent);
static HFONT create_ui_sized_font(int heightPx, int weight = FW_NORMAL);
static int themed_combo_item_height();
static void style_input_control(HWND hwnd);
static void style_combo_control(HWND hwnd);
static void install_themed_combo_subclass(HWND hwnd);
static void paint_themed_combo_overlay(HWND hwnd, HDC hdc);
static void paint_themed_combo_full_custom(HWND hwnd, HDC hdc);
static LRESULT CALLBACK themed_combo_wndproc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
static bool is_themed_combo_id(UINT id);
static void draw_themed_combo_item(const DRAWITEMSTRUCT* dis);
static void measure_themed_combo_item(MEASUREITEMSTRUCT* mis);
static void draw_themed_button(const DRAWITEMSTRUCT* dis);
static bool is_themed_button_id(UINT id);
static bool is_themed_checkbox_id(UINT id);
static bool is_fan_dialog_checkbox_id(UINT id);
static void draw_checkbox_tick_smooth(HDC hdc, const RECT* box, COLORREF color);
static void write_error_report_log_for_user_failure(const char* summary, const char* details = nullptr);
static void draw_curve_polyline_smooth(HDC hdc, const POINT* pts, int count, int widthPx, COLORREF color);
static void draw_curve_points_ringed(HDC hdc, const POINT* pts, int count, int innerRadiusPx, int outerRadiusPx);
static bool nvapi_set_gpu_offset(int offsetkHz);
static bool nvapi_set_mem_offset(int offsetkHz);
static bool nvapi_set_power_limit(int pct);
static bool activate_existing_instance_window();
static bool acquire_single_instance_mutex();
static void release_single_instance_mutex();
static void rebuild_visible_map();
static bool read_live_curve_snapshot_settled(int attempts, DWORD delayMs, bool* lastOffsetsOkOut = nullptr);
static unsigned int get_edit_value(HWND hEdit);
static void populate_edits();
static void create_edit_controls(HWND hParent, HINSTANCE hInst);
static void apply_lock(int vi);
static void set_edit_value(HWND hEdit, unsigned int value);
static void unlock_all();
static int mem_display_mhz_from_driver_khz(int driver_kHz);
static int mem_display_mhz_from_driver_mhz(int driverMHz);
static unsigned int displayed_curve_mhz(unsigned int rawFreq_kHz);
static int curve_delta_khz_for_target_display_mhz_unclamped(int pointIndex, unsigned int displayMHz);
static void set_curve_target_mismatch_detail(int pointIndex, unsigned int actualMHz, unsigned int targetMHz, bool lockTail, char* detail, size_t detailSize);
static bool curve_targets_match_request(const DesiredSettings* desired, const bool* lockedTailMask, unsigned int lockMhz, char* detail, size_t detailSize);
static void apply_system_titlebar_theme(HWND hwnd);
static void show_license_dialog(HWND parent);
static void layout_bottom_buttons(HWND hParent);
static void debug_log(const char* fmt, ...);
static bool write_text_file_atomic(const char* path, const char* data, size_t dataSize, char* err, size_t errSize);
static bool write_log_snapshot(const char* path, char* err, size_t errSize);
static bool write_probe_report(const char* path, char* err, size_t errSize);
static bool write_error_report_log(const char* summary, const char* details, char* err, size_t errSize);
static bool save_desired_to_config_with_startup(const char* path, const DesiredSettings* desired, bool useCurrentForUnset, int startupState, char* err, size_t errSize);
static bool should_suppress_startup_ui();
// Profile I/O
static bool load_profile_from_config(const char* path, int slot, DesiredSettings* desired, char* err, size_t errSize);
static bool save_profile_to_config(const char* path, int slot, const DesiredSettings* desired, char* err, size_t errSize);
static bool clear_profile_from_config(const char* path, int slot, char* err, size_t errSize);
static bool is_profile_slot_saved(const char* path, int slot);
static void refresh_profile_controls_from_config();
static void migrate_legacy_config_if_needed(const char* path);
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
static void shutdown_gdiplus();
static bool desired_settings_have_explicit_state(const DesiredSettings* desired, bool requireCurve, char* err, size_t errSize);

static void detect_locked_tail_from_curve();
static void close_nvml();
static const char* nvml_err_name(nvmlReturn_t r);
static bool nvml_ensure_ready();
static bool nvml_set_fan_auto(char* detail, size_t detailSize);
static bool nvml_set_fan_manual(int pct, bool* exactApplied, char* detail, size_t detailSize);
static void initialize_gui_fan_settings_from_live_state(bool syncGuiCurve = true);
static int get_effective_live_fan_mode();
static bool fan_setting_matches_current(int wantMode, int wantPct, const FanCurveConfig* wantCurve);
static bool nvml_manual_fan_matches_target(int pct, bool* matches, char* detail, size_t detailSize);
static int current_displayed_fan_percent();
static int current_manual_fan_target_percent();
static bool manual_fan_readback_matches_target(int wantPct, int actualPct, unsigned int requestedPct);
static void refresh_live_fan_telemetry(bool redrawControls = true);
static bool window_should_redraw_fan_controls();
static void sync_fan_ui_from_cached_state(bool redrawControls = true);
static void update_fan_telemetry_timer();
static bool fan_manual_control_available(char* detail, size_t detailSize);
static bool validate_manual_fan_percent_for_runtime(int pct, char* detail, size_t detailSize);
static bool validate_fan_curve_for_runtime(const FanCurveConfig* curve, char* detail, size_t detailSize);
static void update_fan_controls_enabled_state();
static void update_tray_icon();
static void ensure_tray_profile_cache();
static bool ensure_tray_icon();
static void remove_tray_icon();
static void hide_main_window_to_tray();
static void show_main_window_from_tray();
static void show_tray_menu(HWND hwnd);
static bool live_state_has_custom_oc();
static bool live_state_has_custom_fan();
static void stop_fan_curve_runtime(bool restoreFanAutoOnExit = false);
static void start_fan_curve_runtime();
static void start_fixed_fan_runtime();
static void apply_fan_curve_tick();
static bool nvml_read_temperature(int* temperatureC, char* detail, size_t detailSize);
static bool nvml_read_power_limit();
static bool nvml_read_clock_offsets(char* detail, size_t detailSize);
static bool nvml_read_fans(char* detail, size_t detailSize);
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
static LRESULT CALLBACK LicenseDialogProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
static void layout_license_dialog(HWND hwnd);

static const VfBackendSpec g_vfBackendBlackwell = {
    "blackwell",
    GPU_FAMILY_BLACKWELL,
    true,
    true,
    true,
    false,
    0x21537AD4u,
    0x507B4B59u,
    0x23F1B133u,
    0x0733E009u,
    0x1C28,
    1,
    0x04,
    0x24,
    0x48,
    0x1C,
    0x182C,
    1,
    0x04,
    0x14,
    0x2420,
    1,
    0x04,
    0x44,
    0x24,
    0x14,
    15,
};

static const VfBackendSpec g_vfBackendLovelace = {
    "lovelace",
    GPU_FAMILY_LOVELACE,
    true,
    true,
    true,
    false,
    0x21537AD4u,
    0x507B4B59u,
    0x23F1B133u,
    0x0733E009u,
    0x1C28,
    1,
    0x04,
    0x24,
    0x48,
    0x1C,
    0x182C,
    1,
    0x04,
    0x14,
    0x2420,
    1,
    0x04,
    0x44,
    0x24,
    0x14,
    15,
};

static const VfBackendSpec g_vfBackendAmpere = {
    "ampere",
    GPU_FAMILY_AMPERE,
    true,
    true,
    true,
    true,
    0x21537AD4u,
    0x507B4B59u,
    0x23F1B133u,
    0x0733E009u,
    0x1C28,
    1,
    0x04,
    0x24,
    0x48,
    0x1C,
    0x182C,
    1,
    0x04,
    0x14,
    0x2420,
    1,
    0x04,
    0x44,
    0x24,
    0x14,
    15,
};

static const VfBackendSpec g_vfBackendTuring = {
    "turing",
    GPU_FAMILY_TURING,
    true,
    true,
    true,
    true,
    0x21537AD4u,
    0x507B4B59u,
    0x23F1B133u,
    0x0733E009u,
    0x1C28,
    1,
    0x04,
    0x24,
    0x48,
    0x1C,
    0x182C,
    1,
    0x04,
    0x14,
    0x2420,
    1,
    0x04,
    0x44,
    0x24,
    0x14,
    15,
};

static const VfBackendSpec g_vfBackendPascal = {
    "pascal",
    GPU_FAMILY_PASCAL,
    true,
    true,
    true,
    true,
    0x21537AD4u,
    0x507B4B59u,
    0x23F1B133u,
    0x0733E009u,
    0x1C28,
    1,
    0x04,
    0x24,
    0x48,
    0x1C,
    0x182C,
    1,
    0x04,
    0x14,
    0x2420,
    1,
    0x04,
    0x44,
    0x24,
    0x14,
    15,
};

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

struct LicenseDialogState {
    HWND hwnd;
    HWND textEdit;
    HWND closeButton;
    HWND owner;
};

static FanCurveDialogState g_fanCurveDialog = {};
static LicenseDialogState g_licenseDialog = {};
static HANDLE g_singleInstanceMutex = nullptr;
static UINT g_taskbarCreatedMessage = 0;
static const char APP_SINGLE_INSTANCE_MUTEX_NAME[] = "Local\\GreenCurveSingleInstance";

enum {
    FAN_DIALOG_ENABLE_BASE = 6100,
    FAN_DIALOG_TEMP_BASE = 6200,
    FAN_DIALOG_PERCENT_BASE = 6300,
    FAN_DIALOG_INTERVAL_ID = 6400,
    FAN_DIALOG_HYSTERESIS_ID = 6401,
    FAN_DIALOG_OK_ID = 6402,
    FAN_DIALOG_CANCEL_ID = 6403,
    LICENSE_DIALOG_TEXT_ID = 6500,
    LICENSE_DIALOG_CLOSE_ID = 6501,
};

enum {
    APP_MODE_DEFAULT = 0,
    APP_MODE_ALLOW_DARK = 1,
};

typedef BOOL (WINAPI *AllowDarkModeForWindowFn)(HWND, BOOL);
typedef int (WINAPI *SetPreferredAppModeFn)(int);
typedef void (WINAPI *FlushMenuThemesFn)();
typedef BOOL (WINAPI *SystemParametersInfoForDpiFn)(UINT, UINT, PVOID, UINT, UINT);

static AllowDarkModeForWindowFn s_fnAllowDarkModeForWindow = nullptr;
static SetPreferredAppModeFn s_fnSetPreferredAppMode = nullptr;
static FlushMenuThemesFn s_fnFlushMenuThemes = nullptr;
static bool s_darkModeResolved = false;

static HFONT s_hUiFont = nullptr;
static LOGFONTA s_uiBaseLogFont = {};
static bool s_uiBaseLogFontReady = false;

static const UINT FAN_FIXED_RUNTIME_INTERVAL_MS = 5000;
static const UINT FAN_TELEMETRY_INTERVAL_MS = 1000;
static UINT fan_telemetry_interval_for_window_state() {
    return FAN_TELEMETRY_INTERVAL_MS;
}
static const ULONGLONG FAN_RUNTIME_REAPPLY_INTERVAL_MS = 15000;
static const ULONGLONG FAN_RUNTIME_FAILURE_WINDOW_MS = 10000;

static const char* gpu_family_name(GpuFamily family) {
    switch (family) {
        case GPU_FAMILY_PASCAL: return "pascal";
        case GPU_FAMILY_TURING: return "turing";
        case GPU_FAMILY_AMPERE: return "ampere";
        case GPU_FAMILY_LOVELACE: return "lovelace";
        case GPU_FAMILY_BLACKWELL: return "blackwell";
        default: return "unknown";
    }
}

static bool select_vf_backend_for_current_gpu() {
    g_app.vfBackend = nullptr;
    g_app.gpuFamily = GPU_FAMILY_UNKNOWN;

    switch (g_app.gpuArchitecture) {
        case NV_GPU_ARCHITECTURE_GP100:
            g_app.gpuFamily = GPU_FAMILY_PASCAL;
            g_app.vfBackend = &g_vfBackendPascal;
            return true;
        case NV_GPU_ARCHITECTURE_TU100:
            g_app.gpuFamily = GPU_FAMILY_TURING;
            g_app.vfBackend = &g_vfBackendTuring;
            return true;
        case NV_GPU_ARCHITECTURE_GA100:
            g_app.gpuFamily = GPU_FAMILY_AMPERE;
            g_app.vfBackend = &g_vfBackendAmpere;
            return true;
        case NV_GPU_ARCHITECTURE_AD100:
            g_app.gpuFamily = GPU_FAMILY_LOVELACE;
            g_app.vfBackend = &g_vfBackendLovelace;
            return true;
        case NV_GPU_ARCHITECTURE_GB200:
            g_app.gpuFamily = GPU_FAMILY_BLACKWELL;
            g_app.vfBackend = &g_vfBackendBlackwell;
            return true;
        default:
            return false;
    }
}

static const VfBackendSpec* probe_backend_for_current_gpu() {
    if (g_app.vfBackend) return g_app.vfBackend;

    // Keep probe collection available on unrecognized architectures without
    // enabling live VF reads/writes in the normal runtime path.
    return &g_vfBackendBlackwell;
}

static bool nvapi_get_interface_version_string(char* text, size_t textSize) {
    if (!text || textSize == 0) return false;
    text[0] = 0;

    typedef int (*version_t)(char*);
    auto getVersion = (version_t)nvapi_qi(NVAPI_GET_INTERFACE_VERSION_STRING_ID);
    if (!getVersion) return false;
    return getVersion(text) == 0;
}

static bool nvapi_get_error_message(int status, char* text, size_t textSize) {
    if (!text || textSize == 0) return false;
    text[0] = 0;

    typedef int (*error_message_t)(int, char*);
    auto getErrorMessage = (error_message_t)nvapi_qi(NVAPI_GET_ERROR_MESSAGE_ID);
    if (!getErrorMessage) return false;

    char shortText[64] = {};
    if (getErrorMessage(status, shortText) != 0) return false;
    StringCchCopyA(text, textSize, shortText);
    return true;
}

static bool nvapi_read_gpu_metadata() {
    g_app.gpuArchInfoValid = false;
    g_app.gpuPciInfoValid = false;
    g_app.gpuArchitecture = 0;
    g_app.gpuImplementation = 0;
    g_app.gpuChipRevision = 0;
    g_app.gpuDeviceId = 0;
    g_app.gpuSubSystemId = 0;
    g_app.gpuPciRevisionId = 0;
    g_app.gpuExtDeviceId = 0;

    typedef int (*get_arch_t)(GPU_HANDLE, nvapiGpuArchInfo_t*);
    auto getArchInfo = (get_arch_t)nvapi_qi(NVAPI_GPU_GET_ARCH_INFO_ID);
    if (getArchInfo) {
        nvapiGpuArchInfo_t info = {};
        info.version = NVAPI_GPU_ARCH_INFO_VER2;
        if (getArchInfo(g_app.gpuHandle, &info) == 0) {
            g_app.gpuArchitecture = info.architecture;
            g_app.gpuImplementation = info.implementation;
            g_app.gpuChipRevision = info.revision;
            g_app.gpuArchInfoValid = true;
        }
    }

    typedef int (*get_pci_t)(GPU_HANDLE, unsigned int*, unsigned int*, unsigned int*, unsigned int*);
    auto getPciIdentifiers = (get_pci_t)nvapi_qi(NVAPI_GPU_GET_PCI_IDENTIFIERS_ID);
    if (getPciIdentifiers) {
        if (getPciIdentifiers(
                g_app.gpuHandle,
                &g_app.gpuDeviceId,
                &g_app.gpuSubSystemId,
                &g_app.gpuPciRevisionId,
                &g_app.gpuExtDeviceId) == 0) {
            g_app.gpuPciInfoValid = true;
        }
    }

    return select_vf_backend_for_current_gpu() || g_app.gpuArchInfoValid || g_app.gpuPciInfoValid;
}

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

void invalidate_tray_profile_cache() {
    g_app.trayProfileCacheValid = false;
    g_app.trayLastRenderedValid = false;
    g_app.trayLastRenderedState = TRAY_ICON_STATE_DEFAULT;
    g_app.trayProfileCacheProfilePart[0] = 0;
    g_app.trayLastRenderedTip[0] = 0;
}

static void ensure_tray_profile_cache() {
    if (g_app.trayProfileCacheValid) return;

    g_app.trayProfileCacheValid = true;
    g_app.trayProfileCacheProfilePart[0] = 0;

    int selectedSlot = CONFIG_DEFAULT_SLOT;
    bool hasConfigPath = g_app.configPath[0] != '\0';
    if (hasConfigPath) {
        selectedSlot = get_config_int(g_app.configPath, "profiles", "selected_slot", CONFIG_DEFAULT_SLOT);
    }
    if (selectedSlot < 1 || selectedSlot > CONFIG_NUM_SLOTS) {
        selectedSlot = CONFIG_DEFAULT_SLOT;
    }

    if (!hasConfigPath) {
        StringCchPrintfA(
            g_app.trayProfileCacheProfilePart,
            ARRAY_COUNT(g_app.trayProfileCacheProfilePart),
            "Profile %d",
            selectedSlot);
        return;
    }

    bool hasSavedProfile = is_profile_slot_saved(g_app.configPath, selectedSlot);
    StringCchPrintfA(
        g_app.trayProfileCacheProfilePart,
        ARRAY_COUNT(g_app.trayProfileCacheProfilePart),
        "Profile %d (%s)",
        selectedSlot,
        hasSavedProfile ? "saved" : "empty");
}

static void build_tray_tooltip(char* tip, size_t tipSize) {
    if (!tip || tipSize == 0) return;

    ensure_tray_profile_cache();

    char mode[64] = {};
    bool customOc = live_state_has_custom_oc();
    bool customFan = live_state_has_custom_fan();
    StringCchCopyA(mode, ARRAY_COUNT(mode), tray_mode_label(customOc, customFan));

    const char* profilePart = g_app.trayProfileCacheProfilePart[0]
        ? g_app.trayProfileCacheProfilePart
        : "Profile 1";
    StringCchPrintfA(tip, tipSize, "Green Curve - %s | %s", mode, profilePart);
}

static int clamp_percent(int value) {
    if (value < 0) return 0;
    if (value > 100) return 100;
    return value;
}

static int current_displayed_fan_percent() {
    return g_app.fanCount ? (int)g_app.fanPercent[0] : 0;
}

static int current_manual_fan_target_percent() {
    if (!g_app.fanCount) return 0;
    if (g_app.fanTargetPercent[0] > 0) return (int)g_app.fanTargetPercent[0];
    return current_displayed_fan_percent();
}

static bool window_should_redraw_fan_controls() {
    if (!g_app.hMainWnd || !IsWindowVisible(g_app.hMainWnd)) return false;
    return g_app.guiFanMode != FAN_MODE_FIXED || GetFocus() != g_app.hFanEdit;
}

static void sync_fan_ui_from_cached_state(bool redrawControls) {
    initialize_gui_fan_settings_from_live_state(false);
    update_tray_icon();
    if (redrawControls) {
        update_fan_controls_enabled_state();
    }
}

static void update_fan_telemetry_timer() {
    if (!g_app.hMainWnd) return;
    KillTimer(g_app.hMainWnd, FAN_TELEMETRY_TIMER_ID);
    if (!IsWindowVisible(g_app.hMainWnd)) return;
    SetTimer(g_app.hMainWnd, FAN_TELEMETRY_TIMER_ID, fan_telemetry_interval_for_window_state(), nullptr);
}

static bool fan_manual_control_available(char* detail, size_t detailSize) {
    if (!nvml_ensure_ready()) {
        set_message(detail, detailSize, "NVML not ready");
        return false;
    }
    if (!g_app.fanSupported || g_app.fanCount == 0) {
        char refreshDetail[128] = {};
        if (!nvml_read_fans(refreshDetail, sizeof(refreshDetail))) {
            set_message(detail, detailSize, "%s", refreshDetail[0] ? refreshDetail : "Manual fan control unsupported on this GPU");
            return false;
        }
    }
    if (!g_app.fanSupported || g_app.fanCount == 0) {
        set_message(detail, detailSize, "Manual fan control unsupported on this GPU");
        return false;
    }
    if (!g_nvml_api.setFanSpeed) {
        set_message(detail, detailSize, "Manual fan control unsupported by this NVIDIA driver");
        return false;
    }
    return true;
}

static bool validate_manual_fan_percent_for_runtime(int pct, char* detail, size_t detailSize) {
    if (pct < 0 || pct > 100) {
        set_message(detail, detailSize, "Requested %d%% is outside the valid range 0..100%%", pct);
        return false;
    }
    if (pct == 0) {
        if (g_app.fanRangeKnown && g_app.fanMinPct == 0) return true;
        if (g_app.fanRangeKnown) {
            set_message(detail, detailSize,
                "Requested 0%% manual fan is blocked because the GPU reports a supported range of %u..%u%%",
                g_app.fanMinPct,
                g_app.fanMaxPct);
        } else {
            set_message(detail, detailSize,
                "Requested 0%% manual fan is blocked because the GPU did not report zero-speed support");
        }
        return false;
    }
    if (!g_app.fanRangeKnown) return true;
    if (pct < (int)g_app.fanMinPct || pct > (int)g_app.fanMaxPct) {
        set_message(detail, detailSize,
            "Requested %d%% is outside the supported range %u..%u%%",
            pct,
            g_app.fanMinPct,
            g_app.fanMaxPct);
        return false;
    }
    return true;
}

static bool validate_fan_curve_for_runtime(const FanCurveConfig* curve, char* detail, size_t detailSize) {
    if (!curve) {
        set_message(detail, detailSize, "No fan curve config");
        return false;
    }
    for (int i = 0; i < FAN_CURVE_MAX_POINTS; i++) {
        if (!curve->points[i].enabled) continue;
        char pointDetail[256] = {};
        if (!validate_manual_fan_percent_for_runtime(curve->points[i].fanPercent, pointDetail, sizeof(pointDetail))) {
            set_message(detail, detailSize, "Fan curve point %d is invalid: %s", i + 1, pointDetail);
            return false;
        }
    }
    return true;
}

static bool manual_fan_readback_matches_target(int wantPct, int actualPct, unsigned int requestedPct) {
    if (wantPct == 0) return actualPct == 0;
    if (actualPct >= wantPct - 2 && actualPct <= wantPct + 2) return true;

    int requested = (int)requestedPct;
    if (requested <= 0) return false;
    return requested >= wantPct - 2 && requested <= wantPct + 2;
}

static bool is_gpu_offset_excluded_low_point(int pointIndex, int gpuOffsetMHz) {
    if (pointIndex < 0 || pointIndex >= VF_NUM_POINTS) return false;
    if (g_app.curve[pointIndex].freq_kHz == 0) return false;

    (void)gpuOffsetMHz;

    int populatedCount = 0;
    for (int ci = 0; ci < VF_NUM_POINTS; ci++) {
        if (g_app.curve[ci].freq_kHz == 0) continue;
        if (ci == pointIndex) return true;
        populatedCount++;
        if (populatedCount >= 70) return false;
    }
    return true;
}

static int gpu_offset_component_mhz_for_point(int pointIndex, int gpuOffsetMHz, bool excludeLow70) {
    if (gpuOffsetMHz == 0) return 0;
    if (excludeLow70 && is_gpu_offset_excluded_low_point(pointIndex, gpuOffsetMHz)) return 0;
    return gpuOffsetMHz;
}

static bool vf_backend_is_best_guess(const VfBackendSpec* backend) {
    return backend && backend->bestGuessOnly;
}

static bool should_show_best_guess_warning() {
    if (!g_app.vfBackend || !vf_backend_is_best_guess(g_app.vfBackend)) return false;
    if (!g_app.configPath[0]) return true;

    char key[64] = {};
    StringCchPrintfA(key, ARRAY_COUNT(key), "hide_best_guess_warning_%s", gpu_family_name(g_app.gpuFamily));
    return get_config_int(g_app.configPath, "warnings", key, 0) == 0;
}

static void show_best_guess_support_warning(HWND parent) {
    if (!should_show_best_guess_warning()) return;

    char message[768] = {};
    StringCchPrintfA(message, ARRAY_COUNT(message),
        "Detected %s GPU (%s).\n\n"
        "Green Curve is enabling VF curve support on this family by best guess using the same private backend layout that works on Blackwell and Lovelace. It may work normally, but it is not yet verified on this architecture.\n\n"
        "Check applied clocks and offsets carefully after changes.",
        gpu_family_name(g_app.gpuFamily),
        g_app.gpuName[0] ? g_app.gpuName : "NVIDIA GPU");

    bool dontShowAgainChecked = false;
    bool handled = false;
    HMODULE comctl = LoadLibraryA("comctl32.dll");
    if (comctl) {
        typedef HRESULT (WINAPI *TaskDialogIndirect_t)(const TASKDIALOGCONFIG*, int*, int*, BOOL*);
        auto taskDialogIndirect = (TaskDialogIndirect_t)GetProcAddress(comctl, "TaskDialogIndirect");
        if (taskDialogIndirect) {
            TASKDIALOGCONFIG config = {};
            config.cbSize = sizeof(config);
            config.hwndParent = parent;
            config.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION | TDF_POSITION_RELATIVE_TO_WINDOW;
            config.dwCommonButtons = TDCBF_OK_BUTTON | TDCBF_CANCEL_BUTTON;
            config.pszWindowTitle = L"Green Curve - Experimental GPU Support";
            config.pszMainIcon = TD_WARNING_ICON;

            WCHAR mainInstruction[128] = {};
            StringCchPrintfW(mainInstruction, ARRAY_COUNT(mainInstruction), L"Experimental %hs support enabled", gpu_family_name(g_app.gpuFamily));
            config.pszMainInstruction = mainInstruction;

            WCHAR content[1024] = {};
            StringCchPrintfW(content, ARRAY_COUNT(content),
                L"Detected %hs GPU (%hs).\n\n"
                L"Green Curve is enabling VF curve support on this family by best guess using the same private backend layout that works on Blackwell and Lovelace. It may work normally, but it is not yet verified on this architecture.\n\n"
                L"Check applied clocks and offsets carefully after changes.",
                gpu_family_name(g_app.gpuFamily),
                g_app.gpuName[0] ? g_app.gpuName : "NVIDIA GPU");
            config.pszContent = content;
            config.pszVerificationText = L"Do not show this warning again for this GPU family";

            int button = 0;
            BOOL verification = FALSE;
            HRESULT hr = taskDialogIndirect(&config, &button, nullptr, &verification);
            if (SUCCEEDED(hr)) {
                handled = true;
                if (button == IDCANCEL) {
                    remove_tray_icon();
                    release_single_instance_mutex();
                    ExitProcess(0);
                }
                dontShowAgainChecked = verification == TRUE;
            }
        }
        FreeLibrary(comctl);
    }

    if (!handled) {
        int result = MessageBoxA(parent, message, "Green Curve - Experimental GPU Support", MB_OKCANCEL | MB_ICONWARNING);
        if (result == IDCANCEL) {
            remove_tray_icon();
            release_single_instance_mutex();
            ExitProcess(0);
        }

        int dontShowAgain = MessageBoxA(parent,
            "Do not show this experimental support warning again for this GPU family?",
            "Green Curve - Experimental GPU Support",
            MB_YESNO | MB_ICONQUESTION);
        dontShowAgainChecked = dontShowAgain == IDYES;
    }

    if (dontShowAgainChecked && g_app.configPath[0]) {
        char key[64] = {};
        StringCchPrintfA(key, ARRAY_COUNT(key), "hide_best_guess_warning_%s", gpu_family_name(g_app.gpuFamily));
        set_config_int(g_app.configPath, "warnings", key, 1);
    }
}

static bool vf_curve_global_gpu_offset_supported() {
    const VfBackendSpec* backend = g_app.vfBackend;
    if (!backend || !backend->writeSupported) {
        debug_log("vf_curve_global_gpu_offset_supported: no backend or not writable\n");
        return false;
    }
    bool supported = backend->family == GPU_FAMILY_BLACKWELL;
    debug_log("vf_curve_global_gpu_offset_supported: family=%d supported=%d\n", backend->family, supported ? 1 : 0);
    return supported;
}

static bool is_locked_tail_detection_point(int pointIndex) {
    return g_app.lockedCi >= 0 && pointIndex >= g_app.lockedCi;
}

static bool detect_live_selective_gpu_offset_state(int* gpuOffsetMHzOut) {
    if (!vf_curve_global_gpu_offset_supported()) {
        if (gpuOffsetMHzOut) *gpuOffsetMHzOut = 0;
        return false;
    }

    static const int TOLERANCE_MHZ = 30;

    int candidateOffsets[VF_NUM_POINTS] = {};
    int candidateCount = 0;

    for (int ci = 0; ci < VF_NUM_POINTS; ci++) {
        if (g_app.curve[ci].freq_kHz == 0) continue;
        int offsetMHz = g_app.freqOffsets[ci] / 1000;
        if (offsetMHz == 0) continue;

        bool seen = false;
        for (int i = 0; i < candidateCount; i++) {
            if (abs(candidateOffsets[i] - offsetMHz) <= TOLERANCE_MHZ) {
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
        int toleranceKHz = TOLERANCE_MHZ * 1000;
        bool sawExcludedPoint = false;
        bool sawIncludedPoint = false;
        bool skippedLockedTail = false;
        int includedMatchSumKHz = 0;
        int includedMatchCount = 0;
        int includedConsideredCount = 0;
        int excludedViolations = 0;
        int excludedTotal = 0;
        int candidateTargetKHz = candidateMHz * 1000;

        for (int ci = 0; ci < VF_NUM_POINTS; ci++) {
            if (g_app.curve[ci].freq_kHz == 0) continue;

            if (is_locked_tail_detection_point(ci)) {
                skippedLockedTail = true;
                continue;
            }

            bool excluded = is_gpu_offset_excluded_low_point(ci, candidateMHz);
            int actualOffsetKHz = g_app.freqOffsets[ci];
            if (excluded) {
                sawExcludedPoint = true;
                excludedTotal++;
                if (abs(actualOffsetKHz) > toleranceKHz) {
                    excludedViolations++;
                }
            } else {
                sawIncludedPoint = true;
                includedConsideredCount++;
                if (abs(actualOffsetKHz - candidateTargetKHz) <= toleranceKHz) {
                    includedMatchSumKHz += actualOffsetKHz;
                    includedMatchCount++;
                }
            }
        }

        if (!sawIncludedPoint || !sawExcludedPoint) continue;
        if (includedMatchCount == 0) continue;

        int minimumMatches = skippedLockedTail ? 2 : 3;
        if (includedMatchCount < minimumMatches) continue;
        if (includedMatchCount * 2 < includedConsideredCount) continue;
        if (excludedTotal > 0 && excludedViolations * 3 > excludedTotal) continue;

        if (sawIncludedPoint) {
            int matchedAverageKHz = includedMatchSumKHz / includedMatchCount;
            int detectedMHz = (matchedAverageKHz >= 0 ? (matchedAverageKHz + 500) : (matchedAverageKHz - 500)) / 1000;
            debug_log("detect_live_selective: found selective offset=%d MHz (candidate=%d MHz, matches=%d/%d, skippedTail=%d)\n",
                detectedMHz, candidateMHz, includedMatchCount, includedConsideredCount, skippedLockedTail ? 1 : 0);
            if (gpuOffsetMHzOut) *gpuOffsetMHzOut = detectedMHz;
            return true;
        }
    }

    debug_log("detect_live_selective: no selective offset pattern found (candidates=%d)\n", candidateCount);
    if (gpuOffsetMHzOut) *gpuOffsetMHzOut = 0;
    return false;
}

static bool live_selective_gpu_offset_matches_requested_state(int gpuOffsetMHz) {
    if (!vf_curve_global_gpu_offset_supported() || gpuOffsetMHz == 0) return false;

    static const int MATCH_TOLERANCE_MHZ = 12;
    int toleranceKHz = MATCH_TOLERANCE_MHZ * 1000;
    int targetOffsetKHz = gpuOffsetMHz * 1000;
    bool sawExcludedPoint = false;
    bool sawIncludedPoint = false;
    bool skippedLockedTail = false;
    int includedMatchCount = 0;
    int includedConsideredCount = 0;
    int excludedViolations = 0;
    int excludedTotal = 0;

    for (int ci = 0; ci < VF_NUM_POINTS; ci++) {
        if (g_app.curve[ci].freq_kHz == 0) continue;

        if (is_locked_tail_detection_point(ci)) {
            skippedLockedTail = true;
            continue;
        }

        bool excluded = is_gpu_offset_excluded_low_point(ci, gpuOffsetMHz);
        int actualOffsetKHz = g_app.freqOffsets[ci];
        if (excluded) {
            sawExcludedPoint = true;
            excludedTotal++;
            if (abs(actualOffsetKHz) > toleranceKHz) excludedViolations++;
        } else {
            sawIncludedPoint = true;
            includedConsideredCount++;
            if (abs(actualOffsetKHz - targetOffsetKHz) <= toleranceKHz) includedMatchCount++;
        }
    }

    if (!sawIncludedPoint || !sawExcludedPoint) return false;
    int minimumMatches = skippedLockedTail ? 2 : 3;
    if (includedMatchCount < minimumMatches) return false;
    if (includedMatchCount * 2 < includedConsideredCount) return false;
    if (excludedTotal > 0 && excludedViolations * 3 > excludedTotal) return false;
    return true;
}

static int current_applied_gpu_offset_mhz() {
    if (!vf_curve_global_gpu_offset_supported()) {
        int offsetMHz = g_app.gpuClockOffsetkHz / 1000;
        debug_log("current_applied_gpu_offset_mhz: not Blackwell, returning NVML offset=%d kHz -> %d MHz\n", g_app.gpuClockOffsetkHz, offsetMHz);
        g_app.appliedGpuOffsetMHz = offsetMHz;
        g_app.appliedGpuOffsetExcludeLow70 = false;
        return g_app.appliedGpuOffsetMHz;
    }
    if (g_app.appliedGpuOffsetExcludeLow70 && g_app.appliedGpuOffsetMHz != 0
        && live_selective_gpu_offset_matches_requested_state(g_app.appliedGpuOffsetMHz)) {
        debug_log("current_applied_gpu_offset_mhz: preserving session selective value=%d MHz\n", g_app.appliedGpuOffsetMHz);
        return g_app.appliedGpuOffsetMHz;
    }
    int detectedSelectiveOffsetMHz = 0;
    if (detect_live_selective_gpu_offset_state(&detectedSelectiveOffsetMHz)) {
        debug_log("current_applied_gpu_offset_mhz: detected selective offset=%d MHz\n", detectedSelectiveOffsetMHz);
        g_app.appliedGpuOffsetMHz = detectedSelectiveOffsetMHz;
        g_app.appliedGpuOffsetExcludeLow70 = true;
        return detectedSelectiveOffsetMHz;
    }
    if (g_app.appliedGpuOffsetExcludeLow70 && g_app.appliedGpuOffsetMHz != 0) {
        debug_log("current_applied_gpu_offset_mhz: using session selective fallback=%d MHz\n", g_app.appliedGpuOffsetMHz);
        return g_app.appliedGpuOffsetMHz;
    }
    int offsetMHz = g_app.gpuClockOffsetkHz / 1000;
    debug_log("current_applied_gpu_offset_mhz: no selective detected, uniform offset=%d kHz -> %d MHz\n", g_app.gpuClockOffsetkHz, offsetMHz);
    g_app.appliedGpuOffsetMHz = offsetMHz;
    g_app.appliedGpuOffsetExcludeLow70 = false;
    return offsetMHz;
}

static bool current_applied_gpu_offset_excludes_low_points() {
    if (!vf_curve_global_gpu_offset_supported()) {
        g_app.appliedGpuOffsetExcludeLow70 = false;
        return false;
    }

    if (g_app.appliedGpuOffsetExcludeLow70 && g_app.appliedGpuOffsetMHz != 0
        && live_selective_gpu_offset_matches_requested_state(g_app.appliedGpuOffsetMHz)) {
        return true;
    }

    int detectedSelectiveOffsetMHz = 0;
    if (detect_live_selective_gpu_offset_state(&detectedSelectiveOffsetMHz)) {
        g_app.appliedGpuOffsetMHz = detectedSelectiveOffsetMHz;
        g_app.appliedGpuOffsetExcludeLow70 = true;
        return true;
    }
    if (g_app.appliedGpuOffsetExcludeLow70 && g_app.appliedGpuOffsetMHz != 0) {
        return true;
    }
    g_app.appliedGpuOffsetMHz = g_app.gpuClockOffsetkHz / 1000;
    g_app.appliedGpuOffsetExcludeLow70 = false;
    return false;
}

static void resolve_effective_gpu_offset_state_for_config_save(const DesiredSettings* desired, int* gpuOffsetMHzOut, bool* excludeLow70Out) {
    int gpuOffsetMHz = 0;
    bool excludeLow70 = false;

    if (desired && desired->hasGpuOffset) {
        gpuOffsetMHz = desired->gpuOffsetMHz;
        excludeLow70 = desired->gpuOffsetExcludeLow70;
    } else {
        gpuOffsetMHz = current_applied_gpu_offset_mhz();
        excludeLow70 = current_applied_gpu_offset_excludes_low_points();
    }

    if (gpuOffsetMHz == 0) excludeLow70 = false;
    if (gpuOffsetMHzOut) *gpuOffsetMHzOut = gpuOffsetMHz;
    if (excludeLow70Out) *excludeLow70Out = excludeLow70;
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

static void initialize_gui_fan_settings_from_live_state(bool syncGuiCurve) {
    ensure_valid_fan_curve_config(&g_app.guiFanCurve);
    ensure_valid_fan_curve_config(&g_app.activeFanCurve);

    g_app.activeFanMode = get_effective_live_fan_mode();
    if (g_app.activeFanMode == FAN_MODE_FIXED) {
        g_app.activeFanFixedPercent = current_manual_fan_target_percent();
    }
    if (g_app.guiFanMode < FAN_MODE_AUTO || g_app.guiFanMode > FAN_MODE_CURVE) {
        g_app.guiFanMode = g_app.activeFanMode;
    }
    if (g_app.guiFanMode == FAN_MODE_FIXED) {
        if (g_app.guiFanFixedPercent <= 0) {
            g_app.guiFanFixedPercent = g_app.activeFanFixedPercent > 0 ? g_app.activeFanFixedPercent : 50;
        }
    } else {
        g_app.guiFanFixedPercent = current_displayed_fan_percent();
    }
    g_app.guiFanFixedPercent = clamp_percent(g_app.guiFanFixedPercent);
    if (syncGuiCurve && g_app.activeFanMode == FAN_MODE_CURVE) {
        copy_fan_curve(&g_app.guiFanCurve, &g_app.activeFanCurve);
    }
}

static void refresh_live_fan_telemetry(bool redrawControls) {
    char detail[128] = {};
    if (!nvml_read_fans(detail, sizeof(detail))) return;
    sync_fan_ui_from_cached_state(redrawControls);
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
    if (!g_app.vfBackend || !g_app.vfBackend->writeSupported) return false;
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
        bool dropdownOpen = SendMessageA(g_app.hFanModeCombo, CB_GETDROPPEDSTATE, 0, 0) != 0;
        if (!dropdownOpen) {
            SendMessageA(g_app.hFanModeCombo, CB_SETCURSEL, (WPARAM)g_app.guiFanMode, 0);
        }
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
    int state = TRAY_ICON_STATE_DEFAULT;
    if (hasCustomOc && hasCustomFan) {
        state = TRAY_ICON_STATE_OC_FAN;
    } else if (hasCustomOc) {
        state = TRAY_ICON_STATE_OC;
    } else if (hasCustomFan) {
        state = TRAY_ICON_STATE_FAN;
    }
    g_app.trayIconState = state;

    char tip[128] = {};
    build_tray_tooltip(tip, sizeof(tip));

    if (!g_app.trayIconAdded) return;

    if (g_app.trayLastRenderedValid &&
        g_app.trayLastRenderedState == state &&
        strcmp(g_app.trayLastRenderedTip, tip) == 0) {
        return;
    }

    NOTIFYICONDATAA nid = {};
    nid.cbSize = sizeof(nid);
    nid.hWnd = g_app.hMainWnd;
    nid.uID = 1;
    nid.uFlags = NIF_ICON | NIF_TIP | NIF_SHOWTIP;
    nid.hIcon = g_app.trayIcons[state] ? g_app.trayIcons[state] : LoadIcon(nullptr, IDI_APPLICATION);
    StringCchCopyA(nid.szTip, ARRAY_COUNT(nid.szTip), tip);
    if (Shell_NotifyIconA(NIM_MODIFY, &nid)) {
        g_app.trayLastRenderedValid = true;
        g_app.trayLastRenderedState = state;
        StringCchCopyA(g_app.trayLastRenderedTip, ARRAY_COUNT(g_app.trayLastRenderedTip), tip);
    } else {
        g_app.trayLastRenderedValid = false;
    }
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
    g_app.trayLastRenderedValid = false;
}

static void hide_main_window_to_tray() {
    if (!g_app.hMainWnd) return;
    ensure_tray_icon();
    ShowWindow(g_app.hMainWnd, SW_HIDE);
    update_fan_telemetry_timer();
}

static void show_main_window_from_tray() {
    if (!g_app.hMainWnd) return;
    ShowWindow(g_app.hMainWnd, SW_RESTORE);
    ShowWindow(g_app.hMainWnd, SW_SHOW);
    update_fan_telemetry_timer();
    SetForegroundWindow(g_app.hMainWnd);
    g_app.startHiddenToTray = false;
}

static void show_tray_menu(HWND hwnd) {
    if (!hwnd) return;
    refresh_menu_theme_cache();
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
        sync_fan_ui_from_cached_state(window_should_redraw_fan_controls());
    } else if (g_app.hMainWnd) {
        refresh_live_fan_telemetry(window_should_redraw_fan_controls());
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

static void stop_fan_curve_runtime(bool restoreFanAutoOnExit) {
    if (restoreFanAutoOnExit && (g_app.fanCurveRuntimeActive || g_app.fanFixedRuntimeActive)) {
        char detail[128] = {};
        if (g_app.fanSupported && !g_app.fanIsAuto && nvml_set_fan_auto(detail, sizeof(detail))) {
            g_app.fanIsAuto = true;
        }
    }

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
    if (g_app.hMainWnd) {
        refresh_live_fan_telemetry(true);
    }
    update_fan_telemetry_timer();
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
    ULONGLONG now = GetTickCount64();

    if (g_app.fanFixedRuntimeActive) {
        int targetPercent = clamp_percent(g_app.activeFanFixedPercent);
        bool needsReapply = (g_app.fanRuntimeLastApplyTickMs == 0) ||
            ((now - g_app.fanRuntimeLastApplyTickMs) >= FAN_RUNTIME_REAPPLY_INTERVAL_MS);
        if (!needsReapply) {
            bool matches = false;
            char detail[128] = {};
            if (!nvml_manual_fan_matches_target(targetPercent, &matches, detail, sizeof(detail))) {
                handle_fan_runtime_failure("Fixed fan runtime verify failed", detail);
                return;
            }
            if (matches) {
                g_app.activeFanMode = FAN_MODE_FIXED;
                g_app.activeFanFixedPercent = targetPercent;
                g_app.fanRuntimeConsecutiveFailures = 0;
                sync_fan_ui_from_cached_state(window_should_redraw_fan_controls());
                return;
            }
        }

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
        sync_fan_ui_from_cached_state(window_should_redraw_fan_controls());
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
    sync_fan_ui_from_cached_state(window_should_redraw_fan_controls());
}

static void start_fan_curve_runtime() {
    if (!g_app.fanSupported || !g_app.hMainWnd) return;
    fan_curve_normalize(&g_app.activeFanCurve);
    char err[256] = {};
    if (!fan_manual_control_available(err, sizeof(err))) {
        return;
    }
    if (!fan_curve_validate(&g_app.activeFanCurve, err, sizeof(err))) {
        return;
    }
    if (!validate_fan_curve_for_runtime(&g_app.activeFanCurve, err, sizeof(err))) {
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

    update_fan_telemetry_timer();

    if (g_app.fanCurveRuntimeActive) {
        apply_fan_curve_tick();
    }
    update_tray_icon();
}

static void start_fixed_fan_runtime() {
    if (!g_app.fanSupported || !g_app.hMainWnd) return;

    g_app.activeFanFixedPercent = clamp_percent(g_app.activeFanFixedPercent);
    char detail[256] = {};
    if (!fan_manual_control_available(detail, sizeof(detail))) return;
    if (!validate_manual_fan_percent_for_runtime(g_app.activeFanFixedPercent, detail, sizeof(detail))) return;
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

    update_fan_telemetry_timer();

    apply_fan_curve_tick();
    update_tray_icon();
}

static void show_license_dialog(HWND parent) {
    if (g_licenseDialog.hwnd) {
        ShowWindow(g_licenseDialog.hwnd, SW_SHOW);
        SetForegroundWindow(g_licenseDialog.hwnd);
        return;
    }

    WNDCLASSEXA wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = LicenseDialogProc;
    wc.hInstance = g_app.hInst;
    wc.lpszClassName = "GreenCurveLicenseDialog";
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = g_app.hWindowClassBrush;
    wc.hIcon = (HICON)SendMessageA(g_app.hMainWnd, WM_GETICON, ICON_SMALL, 0);
    WNDCLASSEXA existing = {};
    if (!GetClassInfoExA(g_app.hInst, wc.lpszClassName, &existing)) {
        RegisterClassExA(&wc);
    }

    RECT ownerRect = {};
    if (parent) GetWindowRect(parent, &ownerRect);
    int width = dp(760);
    int height = dp(560);
    int x = parent ? ownerRect.left + ((ownerRect.right - ownerRect.left) - width) / 2 : CW_USEDEFAULT;
    int y = parent ? ownerRect.top + dp(30) : CW_USEDEFAULT;

    g_licenseDialog.owner = parent;
    g_licenseDialog.hwnd = CreateWindowExA(
        WS_EX_DLGMODALFRAME,
        wc.lpszClassName,
        APP_NAME " License",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME,
        x, y, width, height,
        parent, nullptr, g_app.hInst, nullptr);
    if (!g_licenseDialog.hwnd) return;

    if (parent) EnableWindow(parent, FALSE);
    ShowWindow(g_licenseDialog.hwnd, SW_SHOW);
    UpdateWindow(g_licenseDialog.hwnd);
}

static void layout_license_dialog(HWND hwnd) {
    if (!hwnd || !g_licenseDialog.textEdit || !g_licenseDialog.closeButton) return;
    RECT rc = {};
    GetClientRect(hwnd, &rc);
    int margin = dp(16);
    int buttonW = dp(92);
    int buttonH = dp(30);
    SetWindowPos(g_licenseDialog.textEdit, nullptr,
        margin, margin, nvmax(dp(320), rc.right - margin * 2), nvmax(dp(220), rc.bottom - margin * 3 - buttonH),
        SWP_NOZORDER);
    SetWindowPos(g_licenseDialog.closeButton, nullptr,
        rc.right - margin - buttonW, rc.bottom - margin - buttonH, buttonW, buttonH,
        SWP_NOZORDER);
}

static LRESULT CALLBACK LicenseDialogProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_CREATE: {
            apply_system_titlebar_theme(hwnd);
            allow_dark_mode_for_window(hwnd);

            g_licenseDialog.textEdit = CreateWindowExA(
                WS_EX_CLIENTEDGE, "EDIT", APP_LICENSE_TEXT,
                WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_READONLY,
                0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)LICENSE_DIALOG_TEXT_ID, g_app.hInst, nullptr);
            g_licenseDialog.closeButton = CreateWindowExA(
                0, "BUTTON", "Close",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)LICENSE_DIALOG_CLOSE_ID, g_app.hInst, nullptr);

            style_input_control(g_licenseDialog.textEdit);
            apply_ui_font(g_licenseDialog.textEdit);
            apply_ui_font(g_licenseDialog.closeButton);
            layout_license_dialog(hwnd);
            return 0;
        }

        case WM_SIZE:
            layout_license_dialog(hwnd);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;

        case WM_SETTINGCHANGE:
        case WM_THEMECHANGED:
            apply_system_titlebar_theme(hwnd);
            allow_dark_mode_for_window(hwnd);
            return 0;

        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLOREDIT: {
            HDC hdc = (HDC)wParam;
            HWND hCtl = (HWND)lParam;
            if (hCtl == g_licenseDialog.textEdit) {
                SetTextColor(hdc, COL_TEXT);
                SetBkColor(hdc, COL_INPUT);
                static HBRUSH hInputBrush = CreateSolidBrush(COL_INPUT);
                return (LRESULT)hInputBrush;
            }
            SetTextColor(hdc, COL_LABEL);
            SetBkColor(hdc, COL_BG);
            static HBRUSH hBgBrush = CreateSolidBrush(COL_BG);
            return (LRESULT)hBgBrush;
        }

        case WM_DRAWITEM: {
            const DRAWITEMSTRUCT* dis = (const DRAWITEMSTRUCT*)lParam;
            if (dis && dis->CtlType == ODT_BUTTON && dis->CtlID == LICENSE_DIALOG_CLOSE_ID) {
                draw_themed_button(dis);
                return TRUE;
            }
            break;
        }

        case WM_COMMAND:
            if (LOWORD(wParam) == LICENSE_DIALOG_CLOSE_ID && HIWORD(wParam) == BN_CLICKED) {
                DestroyWindow(hwnd);
                return 0;
            }
            break;

        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;

        case WM_DESTROY:
            g_licenseDialog.textEdit = nullptr;
            g_licenseDialog.closeButton = nullptr;
            g_licenseDialog.hwnd = nullptr;
            if (g_licenseDialog.owner) {
                EnableWindow(g_licenseDialog.owner, TRUE);
                SetForegroundWindow(g_licenseDialog.owner);
                g_licenseDialog.owner = nullptr;
            }
            return 0;
    }
    return DefWindowProcA(hwnd, msg, wParam, lParam);
}

static int layout_rows_per_column() {
    return (g_app.numVisible + 5) / 6;
}

static int layout_global_controls_y() {
    return dp(GRAPH_HEIGHT) + dp(20) + layout_rows_per_column() * dp(20) + dp(6);
}

static int layout_bottom_buttons_y() {
    return layout_global_controls_y() + dp(56);
}

static int layout_bottom_panel_bottom_y() {
    int buttonsY = layout_bottom_buttons_y();
    int profileY = buttonsY + dp(40);
    int autoY = profileY + dp(34);
    int hintY = autoY + dp(26);
    int statusY = hintY + dp(40);
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
    const int hintY = autoY + dp(26);
    const int statusY = hintY + dp(40);

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
        SetWindowPos(g_app.hStartOnLogonCheck, nullptr, margin + dp(760), autoY + dp(4), dp(16), dp(16), SWP_NOZORDER);
    if (g_app.hStartOnLogonLabel)
        SetWindowPos(g_app.hStartOnLogonLabel, nullptr, margin + dp(784), autoY + dp(3), dp(296), dp(18), SWP_NOZORDER);
    if (g_app.hLogonHintLabel)
        SetWindowPos(g_app.hLogonHintLabel, nullptr, margin, hintY, nvmax(dp(320), rc.right - margin * 2), dp(34), SWP_NOZORDER);
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
    invalidate_tray_profile_cache();
}

static bool should_suppress_startup_ui() {
    return g_app.launchedFromLogon || g_app.startHiddenToTray;
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

    char debugPath[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, debugPath, MAX_PATH);
    char* slash = strrchr(debugPath, '\\');
    if (!slash) slash = strrchr(debugPath, '/');
    if (slash) slash[1] = 0; else debugPath[0] = 0;
    StringCchCatA(debugPath, MAX_PATH, APP_DEBUG_LOG_FILE);

    HANDLE h = CreateFileA(debugPath, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        WriteFile(h, buf, (DWORD)strlen(buf), &written, nullptr);
        CloseHandle(h);
    }
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
    int curveMinkHz = 0;
    int curveMaxkHz = 0;
    bool curveRangeKnown = get_curve_offset_range_khz(&curveMinkHz, &curveMaxkHz);
    appendf("VF curve delta clamp: %d..%d kHz", curveMinkHz, curveMaxkHz);
    appendf("%s\r\n",
        g_app.curveOffsetRangeKnown ? " (driver curve range)" :
        curveRangeKnown ? " (graphics offset fallback)" :
        " (default fallback)");
    appendf("Mem offset: %d MHz", mem_display_mhz_from_driver_khz(g_app.memClockOffsetkHz));
    if (g_app.memOffsetRangeKnown) appendf(" (range %d..%d)", g_app.memClockOffsetMinMHz, g_app.memClockOffsetMaxMHz);
    appendf("\r\n");
    appendf("Power limit: %d%% (%d mW current / %d mW default)\r\n", g_app.powerLimitPct, g_app.powerLimitCurrentmW, g_app.powerLimitDefaultmW);
    if (g_app.fanSupported) {
        appendf("Fan: %s\r\n", g_app.fanIsAuto ? "auto" : "manual");
        for (unsigned int fan = 0; fan < g_app.fanCount; fan++) {
            appendf("  Fan %u: %u%% / %u RPM / policy=%u signal=%u target=0x%X requested=%u%%\r\n",
                fan, g_app.fanPercent[fan], g_app.fanRpm[fan], g_app.fanPolicy[fan], g_app.fanControlSignal[fan], g_app.fanTargetMask[fan], g_app.fanTargetPercent[fan]);
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
    int curveMinkHz = 0;
    int curveMaxkHz = 0;
    bool curveRangeKnown = get_curve_offset_range_khz(&curveMinkHz, &curveMaxkHz);
    appendf("VF curve delta clamp: %d..%d kHz", curveMinkHz, curveMaxkHz);
    appendf("%s\r\n",
        g_app.curveOffsetRangeKnown ? " (driver curve range)" :
        curveRangeKnown ? " (graphics offset fallback)" :
        " (default fallback)");
    appendf("Mem offset: %d MHz", mem_display_mhz_from_driver_khz(g_app.memClockOffsetkHz));
    if (g_app.memOffsetRangeKnown) appendf(" (range %d..%d)", g_app.memClockOffsetMinMHz, g_app.memClockOffsetMaxMHz);
    appendf("\r\n");
    appendf("Power limit: %d%% (%d mW current / %d mW default)\r\n", g_app.powerLimitPct, g_app.powerLimitCurrentmW, g_app.powerLimitDefaultmW);
    if (g_app.fanSupported) {
        appendf("Fan: %s\r\n", g_app.fanIsAuto ? "auto" : "manual");
        for (unsigned int fan = 0; fan < g_app.fanCount; fan++) {
            appendf("  Fan %u: %u%% / %u RPM / policy=%u signal=%u target=0x%X requested=%u%%\r\n",
                fan, g_app.fanPercent[fan], g_app.fanRpm[fan], g_app.fanPolicy[fan], g_app.fanControlSignal[fan], g_app.fanTargetMask[fan], g_app.fanTargetPercent[fan]);
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
        append("    {\"index\": %u, \"percent\": %u, \"requested_percent\": %u, \"rpm\": %u, \"policy\": %u, \"signal\": %u, \"target\": %u}%s\n",
            fan, g_app.fanPercent[fan], g_app.fanTargetPercent[fan], g_app.fanRpm[fan], g_app.fanPolicy[fan], g_app.fanControlSignal[fan], g_app.fanTargetMask[fan],
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

static bool write_probe_report(const char* path, char* err, size_t errSize) {
    char* json = (char*)malloc(524288);
    if (!json) {
        set_message(err, errSize, "Out of memory generating probe report");
        return false;
    }

    size_t used = 0;
    auto append = [&](const char* fmt, ...) -> bool {
        if (used >= 524288) return false;
        va_list ap;
        va_start(ap, fmt);
        int written = _vsnprintf_s(json + used, 524288 - used, _TRUNCATE, fmt, ap);
        va_end(ap);
        if (written < 0) {
            used = 524287;
            json[524287] = 0;
            return false;
        }
        used += (size_t)written;
        return true;
    };

    auto append_json_string = [&](const char* text) {
        append("\"");
        for (const unsigned char* p = (const unsigned char*)(text ? text : ""); *p; ++p) {
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
        append("\"");
    };

    auto append_hex_bytes = [&](const unsigned char* bytes, unsigned int count) {
        for (unsigned int i = 0; i < count; i++) {
            append("%02X", bytes[i]);
            if (i + 1 < count) append(" ");
        }
    };

    enum ProbeKind {
        PROBE_KIND_GENERIC = 0,
        PROBE_KIND_INFO = 1,
        PROBE_KIND_STATUS = 2,
        PROBE_KIND_CONTROL = 3,
    };

    struct ProbeCallResult {
        bool found;
        bool callable;
        int ret;
        unsigned int size;
        unsigned char buf[0x4000];
        char errorText[64];
    };

    const VfBackendSpec* selected = probe_backend_for_current_gpu();
    unsigned char ffMask[32] = {};
    memset(ffMask, 0xFF, sizeof(ffMask));

    auto run_probe_call = [&](unsigned int id,
                              unsigned int size,
                              unsigned int version,
                              ProbeKind kind,
                              const VfBackendSpec* layout,
                              const unsigned char* maskSeed,
                              size_t maskSeedLen,
                              bool hasNumClocksSeed,
                              unsigned int numClocksSeed) -> ProbeCallResult {
        ProbeCallResult result = {};
        result.ret = -9999;
        result.size = size;
        result.errorText[0] = 0;

        auto func = (NvApiFunc)nvapi_qi(id);
        if (!func) return result;
        result.found = true;

        if (size > sizeof(result.buf)) return result;
        result.callable = true;

        const unsigned int header = (version << 16) | size;
        memcpy(&result.buf[0], &header, sizeof(header));

        if (layout && maskSeed && maskSeedLen > 0) {
            unsigned int maskOffset = 0;
            if (kind == PROBE_KIND_INFO) maskOffset = layout->infoMaskOffset;
            else if (kind == PROBE_KIND_STATUS) maskOffset = layout->statusMaskOffset;
            else if (kind == PROBE_KIND_CONTROL) maskOffset = layout->controlMaskOffset;
            if (maskOffset + maskSeedLen <= size) {
                memcpy(&result.buf[maskOffset], maskSeed, maskSeedLen);
            }
        }

        if (layout && kind == PROBE_KIND_STATUS && hasNumClocksSeed) {
            if (layout->statusNumClocksOffset + sizeof(numClocksSeed) <= size) {
                memcpy(&result.buf[layout->statusNumClocksOffset], &numClocksSeed, sizeof(numClocksSeed));
            }
        }

        result.ret = func(g_app.gpuHandle, result.buf);
        if (result.ret != 0) {
            nvapi_get_error_message(result.ret, result.errorText, sizeof(result.errorText));
        }
        return result;
    };

    ProbeCallResult seedInfo = run_probe_call(
        selected->getInfoId,
        selected->infoBufferSize,
        selected->infoVersion,
        PROBE_KIND_INFO,
        selected,
        ffMask,
        sizeof(ffMask),
        false,
        0);

    unsigned char cachedMask[32] = {};
    unsigned int cachedNumClocks = selected->defaultNumClocks;
    bool cachedSeedAvailable = false;
    if (seedInfo.ret == 0 &&
        selected->infoMaskOffset + sizeof(cachedMask) <= seedInfo.size &&
        selected->infoNumClocksOffset + sizeof(cachedNumClocks) <= seedInfo.size) {
        memcpy(cachedMask, &seedInfo.buf[selected->infoMaskOffset], sizeof(cachedMask));
        memcpy(&cachedNumClocks, &seedInfo.buf[selected->infoNumClocksOffset], sizeof(cachedNumClocks));
        if (cachedNumClocks == 0) cachedNumClocks = selected->defaultNumClocks;
        cachedSeedAvailable = true;
    }

    auto append_probe_result = [&](const char* label,
                                   unsigned int id,
                                   unsigned int size,
                                   unsigned int version,
                                   ProbeKind kind,
                                   const VfBackendSpec* layout,
                                   const char* seedSource,
                                   const unsigned char* maskSeed,
                                   size_t maskSeedLen,
                                   bool hasNumClocksSeed,
                                   unsigned int numClocksSeed,
                                   bool includeFullBytes) {
        ProbeCallResult result = run_probe_call(id, size, version, kind, layout, maskSeed, maskSeedLen, hasNumClocksSeed, numClocksSeed);

        append("      {\n");
        append("        \"label\": ");
        append_json_string(label);
        append(",\n        \"id\": \"0x%08X\",\n", id);
        append("        \"size\": %u,\n", size);
        append("        \"version\": %u,\n", version);
        if (seedSource && *seedSource) {
            append("        \"seed_source\": ");
            append_json_string(seedSource);
            append(",\n");
        }
        if (maskSeed && maskSeedLen > 0) {
            append("        \"seed_mask_hex\": \"");
            append_hex_bytes(maskSeed, (unsigned int)maskSeedLen);
            append("\",");
            append("\n");
        }
        if (hasNumClocksSeed) {
            append("        \"seed_num_clocks\": %u,\n", numClocksSeed);
        }

        if (!result.found) {
            append("        \"found\": false\n");
            append("      }");
            return;
        }

        append("        \"found\": true,\n");
        if (!result.callable) {
            append("        \"callable\": false,\n");
            append("        \"error\": \"buffer too large for built-in probe\"\n");
            append("      }");
            return;
        }

        append("        \"callable\": true,\n");
        append("        \"result\": %d,\n", result.ret);
        append("        \"result_hex\": \"0x%08X\",\n", (unsigned int)result.ret);
        append("        \"result_text\": ");
        append_json_string(result.errorText[0] ? result.errorText : (result.ret == 0 ? "NVAPI_OK" : ""));
        append(",\n");
        append("        \"first_bytes\": \"");
        unsigned int dumpCount = result.size < 64 ? result.size : 64;
        append_hex_bytes(result.buf, dumpCount);
        append("\"");

        if (result.ret == 0 && includeFullBytes) {
            append(",\n        \"full_bytes\": \"");
            append_hex_bytes(result.buf, result.size);
            append("\"");
        }

        if (result.ret == 0 && layout) {
            if (kind == PROBE_KIND_INFO) {
                if (layout->infoMaskOffset + 32 <= result.size && layout->infoNumClocksOffset + 4 <= result.size) {
                    unsigned int assumedNumClocks = 0;
                    memcpy(&assumedNumClocks, &result.buf[layout->infoNumClocksOffset], sizeof(assumedNumClocks));
                    append(",\n        \"assumed_info\": {\n");
                    append("          \"mask_hex\": \"");
                    append_hex_bytes(&result.buf[layout->infoMaskOffset], 32);
                    append("\",\n");
                    append("          \"num_clocks\": %u\n", assumedNumClocks);
                    append("        }");
                }
            } else if (kind == PROBE_KIND_STATUS) {
                int populated = 0;
                unsigned int firstFreq = 0;
                unsigned int firstVolt = 0;
                unsigned int maxFreq = 0;
                if (layout->statusEntriesOffset + 8 <= result.size) {
                    for (int i = 0; i < VF_NUM_POINTS; i++) {
                        unsigned int entryOffset = layout->statusEntriesOffset + (unsigned int)i * layout->statusEntryStride;
                        if (entryOffset + 8 > result.size) break;
                        unsigned int freq = 0;
                        unsigned int volt = 0;
                        memcpy(&freq, &result.buf[entryOffset], sizeof(freq));
                        memcpy(&volt, &result.buf[entryOffset + 4], sizeof(volt));
                        if (freq > 0) {
                            populated++;
                            if (firstFreq == 0) {
                                firstFreq = freq;
                                firstVolt = volt;
                            }
                            if (freq > maxFreq) maxFreq = freq;
                        }
                    }
                    append(",\n        \"assumed_status_parse\": {\n");
                    append("          \"populated_points\": %d,\n", populated);
                    append("          \"first_freq_khz\": %u,\n", firstFreq);
                    append("          \"first_volt_uv\": %u,\n", firstVolt);
                    append("          \"max_freq_khz\": %u\n", maxFreq);
                    append("        }");
                }
            }
        }

        append("\n      }");
    };

    char nvapiVersion[64] = {};
    nvapi_get_interface_version_string(nvapiVersion, sizeof(nvapiVersion));
    SYSTEMTIME now = {};
    GetLocalTime(&now);

    struct PublicCallSummary {
        bool found;
        bool callable;
        int ret;
        char errorText[64];
    };
    auto run_public_summary = [&](unsigned int id, unsigned int size, unsigned int version) -> PublicCallSummary {
        PublicCallSummary summary = {};
        ProbeCallResult result = run_probe_call(id, size, version, PROBE_KIND_GENERIC, nullptr, nullptr, 0, false, 0);
        summary.found = result.found;
        summary.callable = result.callable;
        summary.ret = result.ret;
        StringCchCopyA(summary.errorText, ARRAY_COUNT(summary.errorText), result.errorText);
        return summary;
    };

    const unsigned int psSizes[] = {0x0008, 0x0018, 0x0048, 0x00B0, 0x01C8, 0x0410,
                                     0x0840, 0x1098, 0x1C94, 0x2420, 0x3000};
    int psV2Results[ARRAY_COUNT(psSizes)] = {};
    int psV3Results[ARRAY_COUNT(psSizes)] = {};
    for (size_t i = 0; i < ARRAY_COUNT(psSizes); i++) {
        PublicCallSummary v2 = run_public_summary(0x6FF81213u, psSizes[i], 2);
        PublicCallSummary v3 = run_public_summary(0x6FF81213u, psSizes[i], 3);
        psV2Results[i] = v2.found && v2.callable ? v2.ret : -9999;
        psV3Results[i] = v3.found && v3.callable ? v3.ret : -9999;
    }

    int psOffsetsV2[13] = {};
    bool psOffsetsV2Valid = false;
    ProbeCallResult psScanV2 = run_probe_call(0x6FF81213u, 0x1CF8, 2, PROBE_KIND_GENERIC, nullptr, nullptr, 0, false, 0);
    if (psScanV2.found && psScanV2.callable && psScanV2.ret == 0) {
        psOffsetsV2Valid = true;
        for (int i = 0; i < 13; i++) {
            memcpy(&psOffsetsV2[i], &psScanV2.buf[0x30 + i * 4], sizeof(psOffsetsV2[i]));
        }
    }

    int psOffsetsV3[13] = {};
    bool psOffsetsV3Valid = false;
    ProbeCallResult psScanV3 = run_probe_call(0x6FF81213u, 0x1CF8, 3, PROBE_KIND_GENERIC, nullptr, nullptr, 0, false, 0);
    if (psScanV3.found && psScanV3.callable && psScanV3.ret == 0) {
        psOffsetsV3Valid = true;
        for (int i = 0; i < 13; i++) {
            memcpy(&psOffsetsV3[i], &psScanV3.buf[0x30 + i * 4], sizeof(psOffsetsV3[i]));
        }
    }

    PublicCallSummary powerPoliciesGetInfo = run_public_summary(0x34206D86u, 0x28, 1);
    PublicCallSummary powerPoliciesGetStatus = run_public_summary(0x355C8B8Cu, 0x50, 1);
    bool powerPoliciesSetStatusFound = nvapi_qi(0xAD95F5EDu) != nullptr;
    bool pstates20SetFound = nvapi_qi(0x0F4DAE6Bu) != nullptr;
    bool pstatesInfoExFound = nvapi_qi(0x6048B02Fu) != nullptr;
    bool selectedSetControlFound = nvapi_qi(selected->setControlId) != nullptr;
    bool nvmlReady = nvml_ensure_ready();

    int liveGpuOffsetMHz = g_app.gpuClockOffsetkHz / 1000;
    int liveMemOffsetMHz = mem_display_mhz_from_driver_khz(g_app.memClockOffsetkHz);
    int livePowerLimitPct = g_app.powerLimitPct;
    bool liveFanSupported = g_app.fanSupported;
    int rawGpuOffsetMinMHz = g_app.gpuClockOffsetMinMHz;
    int rawGpuOffsetMaxMHz = g_app.gpuClockOffsetMaxMHz;
    int rawMemOffsetMinMHz = g_app.memClockOffsetMinMHz;
    int rawMemOffsetMaxMHz = g_app.memClockOffsetMaxMHz;
    int rawOffsetReadPstate = g_app.offsetReadPstate;
    int detectedGpuOffsetMHz = g_app.gpuClockOffsetkHz / 1000;
    int detectedMemOffsetMHz = mem_display_mhz_from_driver_khz(g_app.memClockOffsetkHz);
    int detectedPowerLimitPct = g_app.powerLimitPct;
    bool detectedFanSupported = g_app.fanSupported;

    char offsetDetail[128] = {};
    if (nvml_read_clock_offsets(offsetDetail, sizeof(offsetDetail))) {
        liveGpuOffsetMHz = g_app.gpuClockOffsetkHz / 1000;
        liveMemOffsetMHz = mem_display_mhz_from_driver_khz(g_app.memClockOffsetkHz);
        rawGpuOffsetMinMHz = g_app.gpuClockOffsetMinMHz;
        rawGpuOffsetMaxMHz = g_app.gpuClockOffsetMaxMHz;
        rawMemOffsetMinMHz = g_app.memClockOffsetMinMHz;
        rawMemOffsetMaxMHz = g_app.memClockOffsetMaxMHz;
        rawOffsetReadPstate = g_app.offsetReadPstate;
    }

    if (nvml_read_power_limit()) {
        livePowerLimitPct = g_app.powerLimitPct;
    }

    char fanDetail[128] = {};
    if (nvml_read_fans(fanDetail, sizeof(fanDetail))) {
        liveFanSupported = g_app.fanSupported;
    }

    append("{\n");
    append("  \"tool\": "); append_json_string(APP_NAME); append(",\n");
    append("  \"version\": "); append_json_string(APP_VERSION); append(",\n");
    append("  \"generated_at\": \"%04u-%02u-%02uT%02u:%02u:%02u\",\n",
        now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond);
    append("  \"gpu\": {\n");
    append("    \"name\": "); append_json_string(g_app.gpuName); append(",\n");
    append("    \"family\": "); append_json_string(gpu_family_name(g_app.gpuFamily)); append(",\n");
    append("    \"arch_info_valid\": %s,\n", g_app.gpuArchInfoValid ? "true" : "false");
    append("    \"pci_info_valid\": %s,\n", g_app.gpuPciInfoValid ? "true" : "false");
    append("    \"architecture\": \"0x%08X\",\n", g_app.gpuArchitecture);
    append("    \"implementation\": \"0x%08X\",\n", g_app.gpuImplementation);
    append("    \"chip_revision\": \"0x%08X\",\n", g_app.gpuChipRevision);
    append("    \"device_id\": \"0x%08X\",\n", g_app.gpuDeviceId);
    append("    \"subsystem_id\": \"0x%08X\",\n", g_app.gpuSubSystemId);
    append("    \"pci_revision_id\": \"0x%08X\",\n", g_app.gpuPciRevisionId);
    append("    \"ext_device_id\": \"0x%08X\"\n", g_app.gpuExtDeviceId);
    append("  },\n");
    append("  \"selected_backend\": {\n");
    append("    \"name\": "); append_json_string(g_app.vfBackend ? g_app.vfBackend->name : "none"); append(",\n");
    append("    \"supported\": %s,\n", (g_app.vfBackend && g_app.vfBackend->supported) ? "true" : "false");
    append("    \"read_supported\": %s,\n", (g_app.vfBackend && g_app.vfBackend->readSupported) ? "true" : "false");
    append("    \"write_supported\": %s,\n", (g_app.vfBackend && g_app.vfBackend->writeSupported) ? "true" : "false");
    append("    \"best_guess_only\": %s,\n", (g_app.vfBackend && g_app.vfBackend->bestGuessOnly) ? "true" : "false");
    append("    \"probe_layout_fallback\": %s,\n", g_app.vfBackend ? "false" : "true");
    append("    \"get_status_id\": \"0x%08X\",\n", selected->getStatusId);
    append("    \"get_info_id\": \"0x%08X\",\n", selected->getInfoId);
    append("    \"get_control_id\": \"0x%08X\",\n", selected->getControlId);
    append("    \"set_control_id\": \"0x%08X\",\n", selected->setControlId);
    append("    \"status_buffer_size\": %u,\n", selected->statusBufferSize);
    append("    \"info_buffer_size\": %u,\n", selected->infoBufferSize);
    append("    \"control_buffer_size\": %u\n", selected->controlBufferSize);
    append("  },\n");
    append("  \"backend_layout_assumptions\": {\n");
    append("    \"family\": "); append_json_string(gpu_family_name(selected->family)); append(",\n");
    append("    \"supported\": %s,\n", selected->supported ? "true" : "false");
    append("    \"read_supported\": %s,\n", selected->readSupported ? "true" : "false");
    append("    \"write_supported\": %s,\n", selected->writeSupported ? "true" : "false");
    append("    \"best_guess_only\": %s,\n", selected->bestGuessOnly ? "true" : "false");
    append("    \"get_info\": {\n");
    append("      \"id\": \"0x%08X\",\n", selected->getInfoId);
    append("      \"buffer_size\": %u,\n", selected->infoBufferSize);
    append("      \"version\": %u,\n", selected->infoVersion);
    append("      \"mask_offset\": %u,\n", selected->infoMaskOffset);
    append("      \"num_clocks_offset\": %u\n", selected->infoNumClocksOffset);
    append("    },\n");
    append("    \"get_status\": {\n");
    append("      \"id\": \"0x%08X\",\n", selected->getStatusId);
    append("      \"buffer_size\": %u,\n", selected->statusBufferSize);
    append("      \"version\": %u,\n", selected->statusVersion);
    append("      \"mask_offset\": %u,\n", selected->statusMaskOffset);
    append("      \"num_clocks_offset\": %u,\n", selected->statusNumClocksOffset);
    append("      \"entries_offset\": %u,\n", selected->statusEntriesOffset);
    append("      \"entry_stride\": %u\n", selected->statusEntryStride);
    append("    },\n");
    append("    \"get_control\": {\n");
    append("      \"id\": \"0x%08X\",\n", selected->getControlId);
    append("      \"buffer_size\": %u,\n", selected->controlBufferSize);
    append("      \"version\": %u,\n", selected->controlVersion);
    append("      \"mask_offset\": %u,\n", selected->controlMaskOffset);
    append("      \"entry_base_offset\": %u,\n", selected->controlEntryBaseOffset);
    append("      \"entry_stride\": %u,\n", selected->controlEntryStride);
    append("      \"entry_delta_offset\": %u\n", selected->controlEntryDeltaOffset);
    append("    },\n");
    append("    \"set_control\": {\n");
    append("      \"id\": \"0x%08X\"\n", selected->setControlId);
    append("    },\n");
    append("    \"default_num_clocks\": %u\n", selected->defaultNumClocks);
    append("  },\n");
    append("  \"nvapi\": {\n");
    append("    \"version_string\": "); append_json_string(nvapiVersion); append("\n");
    append("  },\n");
    append("  \"vf_seed\": {\n");
    append("    \"selected_info_probe_ok\": %s,\n", seedInfo.ret == 0 ? "true" : "false");
    append("    \"assumed_mask_available\": %s,\n", cachedSeedAvailable ? "true" : "false");
    append("    \"assumed_num_clocks\": %u,\n", cachedNumClocks);
    append("    \"assumed_mask_hex\": \"");
    append_hex_bytes(cachedSeedAvailable ? cachedMask : ffMask, 32);
    append("\"\n");
    append("  },\n");
    append("  \"live_state\": {\n");
    append("    \"curve_loaded\": %s,\n", g_app.loaded ? "true" : "false");
    append("    \"populated_points\": %d,\n", g_app.numPopulated);
    append("    \"gpu_offset_mhz\": %d,\n", liveGpuOffsetMHz);
    append("    \"mem_offset_mhz\": %d,\n", liveMemOffsetMHz);
    append("    \"gpu_offset_range_min_mhz\": %d,\n", rawGpuOffsetMinMHz);
    append("    \"gpu_offset_range_max_mhz\": %d,\n", rawGpuOffsetMaxMHz);
    append("    \"mem_offset_range_min_mhz\": %d,\n", rawMemOffsetMinMHz);
    append("    \"mem_offset_range_max_mhz\": %d,\n", rawMemOffsetMaxMHz);
    append("    \"offset_read_pstate\": %d,\n", rawOffsetReadPstate);
    append("    \"power_limit_pct\": %d,\n", livePowerLimitPct);
    append("    \"fan_supported\": %s\n", liveFanSupported ? "true" : "false");
    append("  },\n");
    append("  \"detected_state\": {\n");
    append("    \"gpu_offset_mhz\": %d,\n", detectedGpuOffsetMHz);
    append("    \"mem_offset_mhz\": %d,\n", detectedMemOffsetMHz);
    append("    \"power_limit_pct\": %d,\n", detectedPowerLimitPct);
    append("    \"fan_supported\": %s\n", detectedFanSupported ? "true" : "false");
    append("  },\n");
    append("  \"nvml_capabilities\": {\n");
    append("    \"ready\": %s,\n", nvmlReady ? "true" : "false");
    append("    \"get_power_limit\": %s,\n", g_nvml_api.getPowerLimit ? "true" : "false");
    append("    \"get_power_default_limit\": %s,\n", g_nvml_api.getPowerDefaultLimit ? "true" : "false");
    append("    \"get_power_constraints\": %s,\n", g_nvml_api.getPowerConstraints ? "true" : "false");
    append("    \"set_power_limit\": %s,\n", g_nvml_api.setPowerLimit ? "true" : "false");
    append("    \"get_clock_offsets\": %s,\n", g_nvml_api.getClockOffsets ? "true" : "false");
    append("    \"set_clock_offsets\": %s,\n", g_nvml_api.setClockOffsets ? "true" : "false");
    append("    \"get_perf_state\": %s,\n", g_nvml_api.getPerformanceState ? "true" : "false");
    append("    \"get_gpc_clk_vf_offset\": %s,\n", g_nvml_api.getGpcClkVfOffset ? "true" : "false");
    append("    \"get_mem_clk_vf_offset\": %s,\n", g_nvml_api.getMemClkVfOffset ? "true" : "false");
    append("    \"get_gpc_clk_minmax_vf_offset\": %s,\n", g_nvml_api.getGpcClkMinMaxVfOffset ? "true" : "false");
    append("    \"get_mem_clk_minmax_vf_offset\": %s,\n", g_nvml_api.getMemClkMinMaxVfOffset ? "true" : "false");
    append("    \"set_gpc_clk_vf_offset\": %s,\n", g_nvml_api.setGpcClkVfOffset ? "true" : "false");
    append("    \"set_mem_clk_vf_offset\": %s,\n", g_nvml_api.setMemClkVfOffset ? "true" : "false");
    append("    \"get_num_fans\": %s,\n", g_nvml_api.getNumFans ? "true" : "false");
    append("    \"get_minmax_fan_speed\": %s,\n", g_nvml_api.getMinMaxFanSpeed ? "true" : "false");
    append("    \"get_fan_control_policy\": %s,\n", g_nvml_api.getFanControlPolicy ? "true" : "false");
    append("    \"set_fan_control_policy\": %s,\n", g_nvml_api.setFanControlPolicy ? "true" : "false");
    append("    \"get_fan_speed\": %s,\n", g_nvml_api.getFanSpeed ? "true" : "false");
    append("    \"get_target_fan_speed\": %s,\n", g_nvml_api.getTargetFanSpeed ? "true" : "false");
    append("    \"get_fan_speed_rpm\": %s,\n", g_nvml_api.getFanSpeedRpm ? "true" : "false");
    append("    \"set_fan_speed\": %s,\n", g_nvml_api.setFanSpeed ? "true" : "false");
    append("    \"set_default_fan_speed\": %s,\n", g_nvml_api.setDefaultFanSpeed ? "true" : "false");
    append("    \"get_cooler_info\": %s,\n", g_nvml_api.getCoolerInfo ? "true" : "false");
    append("    \"get_temperature\": %s,\n", g_nvml_api.getTemperature ? "true" : "false");
    append("    \"get_clock\": %s,\n", g_nvml_api.getClock ? "true" : "false");
    append("    \"get_max_clock\": %s\n", g_nvml_api.getMaxClock ? "true" : "false");
    append("  },\n");
    append("  \"public_probe\": {\n");
    append("    \"power_policies_get_info\": {\n");
    append("      \"found\": %s,\n", powerPoliciesGetInfo.found ? "true" : "false");
    append("      \"callable\": %s,\n", powerPoliciesGetInfo.callable ? "true" : "false");
    append("      \"result\": %d,\n", powerPoliciesGetInfo.ret);
    append("      \"result_text\": "); append_json_string(powerPoliciesGetInfo.errorText[0] ? powerPoliciesGetInfo.errorText : (powerPoliciesGetInfo.ret == 0 ? "NVAPI_OK" : "")); append("\n");
    append("    },\n");
    append("    \"power_policies_get_status\": {\n");
    append("      \"found\": %s,\n", powerPoliciesGetStatus.found ? "true" : "false");
    append("      \"callable\": %s,\n", powerPoliciesGetStatus.callable ? "true" : "false");
    append("      \"result\": %d,\n", powerPoliciesGetStatus.ret);
    append("      \"result_text\": "); append_json_string(powerPoliciesGetStatus.errorText[0] ? powerPoliciesGetStatus.errorText : (powerPoliciesGetStatus.ret == 0 ? "NVAPI_OK" : "")); append("\n");
    append("    },\n");
    append("    \"power_policies_set_status_found\": %s,\n", powerPoliciesSetStatusFound ? "true" : "false");
    append("    \"pstates20_set_found\": %s,\n", pstates20SetFound ? "true" : "false");
    append("    \"pstates_info_ex_found\": %s,\n", pstatesInfoExFound ? "true" : "false");
    append("    \"selected_set_control_found\": %s,\n", selectedSetControlFound ? "true" : "false");
    append("    \"pstates20_sizes\": [\n");
    for (size_t i = 0; i < ARRAY_COUNT(psSizes); i++) {
        append("      {\"size\": %u, \"v2_result\": %d, \"v3_result\": %d}%s\n",
            psSizes[i], psV2Results[i], psV3Results[i], (i + 1 < ARRAY_COUNT(psSizes)) ? "," : "");
    }
    append("    ],\n");
    append("    \"pstates20_offset_scan_v2\": {\n");
    for (int i = 0; i < 13; i++) {
        unsigned int offset = 0x30u + (unsigned int)i * 4u;
        append("      \"0x%03X\": %d%s\n", offset, psOffsetsV2Valid ? psOffsetsV2[i] : 0, (i < 12) ? "," : "");
    }
    append("    },\n");
    append("    \"pstates20_offset_scan_v2_valid\": %s,\n", psOffsetsV2Valid ? "true" : "false");
    append("    \"pstates20_offset_scan_v3\": {\n");
    for (int i = 0; i < 13; i++) {
        unsigned int offset = 0x30u + (unsigned int)i * 4u;
        append("      \"0x%03X\": %d%s\n", offset, psOffsetsV3Valid ? psOffsetsV3[i] : 0, (i < 12) ? "," : "");
    }
    append("    },\n");
    append("    \"pstates20_offset_scan_v3_valid\": %s\n", psOffsetsV3Valid ? "true" : "false");
    append("  },\n");
    const unsigned char* selectedMaskSeed = cachedSeedAvailable ? cachedMask : ffMask;
    const char* selectedMaskSeedSource = cachedSeedAvailable ? "cached_get_info_mask" : "ff_mask_fallback";
    unsigned int selectedNumClocksSeed = cachedSeedAvailable ? cachedNumClocks : selected->defaultNumClocks;

    append("  \"control_layout_probe\": {\n");
    ProbeCallResult controlSeed = run_probe_call(selected->getControlId, selected->controlBufferSize, selected->controlVersion,
        PROBE_KIND_CONTROL, selected, selectedMaskSeed, sizeof(cachedMask), false, 0);
    if (controlSeed.ret == 0) {
        int controlPreviewCount = 16;
        append("    \"current_assumption\": {\n");
        append("      \"entry_base_offset\": %u,\n", selected->controlEntryBaseOffset);
        append("      \"entry_stride\": %u,\n", selected->controlEntryStride);
        append("      \"entry_delta_offset\": %u,\n", selected->controlEntryDeltaOffset);
        append("      \"first_deltas\": [");
        for (int i = 0; i < controlPreviewCount; i++) {
            unsigned int deltaOffset = selected->controlEntryBaseOffset + (unsigned int)i * selected->controlEntryStride + selected->controlEntryDeltaOffset;
            int delta = 0;
            if (deltaOffset + sizeof(delta) <= controlSeed.size) memcpy(&delta, &controlSeed.buf[deltaOffset], sizeof(delta));
            append("%s%d", i ? ", " : "", delta);
        }
        append("]\n");
        append("    },\n");

        append("    \"candidate_layouts\": [\n");
        bool firstCandidate = true;
        unsigned int candidateStrides[] = { 24, 28, 32, 36, 40 };
        unsigned int candidateBases[] = { 32, 44, 56, 68, 80 };
        unsigned int candidateDeltaOffsets[] = { 8, 12, 16, 20, 24, 28 };
        for (unsigned int stride : candidateStrides) {
            for (unsigned int baseOffset : candidateBases) {
                for (unsigned int deltaOffset : candidateDeltaOffsets) {
                    int deltas[8] = {};
                    bool inRange = true;
                    int minDelta = 0;
                    int maxDelta = 0;
                    for (int i = 0; i < 8; i++) {
                        unsigned int offset = baseOffset + (unsigned int)i * stride + deltaOffset;
                        if (offset + sizeof(int) > controlSeed.size) {
                            inRange = false;
                            break;
                        }
                        memcpy(&deltas[i], &controlSeed.buf[offset], sizeof(int));
                        if (i == 0 || deltas[i] < minDelta) minDelta = deltas[i];
                        if (i == 0 || deltas[i] > maxDelta) maxDelta = deltas[i];
                    }
                    if (!inRange) continue;
                    if (minDelta < -1500000 || maxDelta > 1500000) continue;
                    append(firstCandidate ? "" : ",\n");
                    firstCandidate = false;
                    append("      {\n");
                    append("        \"entry_base_offset\": %u,\n", baseOffset);
                    append("        \"entry_stride\": %u,\n", stride);
                    append("        \"entry_delta_offset\": %u,\n", deltaOffset);
                    append("        \"first_deltas\": [");
                    for (int i = 0; i < 8; i++) append("%s%d", i ? ", " : "", deltas[i]);
                    append("]\n");
                    append("      }");
                }
            }
        }
        append("\n    ]\n");
    } else {
        append("    \"error\": \"selected get_control probe failed\"\n");
    }
    append("  },\n");
    append("  \"vf_probe\": [\n");

    append_probe_result("selected_get_info", selected->getInfoId, selected->infoBufferSize, selected->infoVersion,
        PROBE_KIND_INFO, selected, "ff_mask_seed", ffMask, sizeof(ffMask), false, 0, true);
    append(",\n");
    append_probe_result("selected_get_info_v2", selected->getInfoId, selected->infoBufferSize, 2,
        PROBE_KIND_INFO, selected, "ff_mask_seed", ffMask, sizeof(ffMask), false, 0, false);
    append(",\n");
    append_probe_result("selected_get_info_alt_size_minus_4", selected->getInfoId, selected->infoBufferSize >= 4 ? (selected->infoBufferSize - 4) : selected->infoBufferSize, selected->infoVersion,
        PROBE_KIND_INFO, selected, "ff_mask_seed", ffMask, sizeof(ffMask), false, 0, false);
    append(",\n");
    append_probe_result("selected_get_info_alt_size_plus_4", selected->getInfoId, selected->infoBufferSize + 4, selected->infoVersion,
        PROBE_KIND_INFO, selected, "ff_mask_seed", ffMask, sizeof(ffMask), false, 0, false);
    append(",\n");
    append_probe_result("selected_get_status_ff_seed", selected->getStatusId, selected->statusBufferSize, selected->statusVersion,
        PROBE_KIND_STATUS, selected, "ff_mask_seed", ffMask, sizeof(ffMask), true, selected->defaultNumClocks, false);
    append(",\n");
    append_probe_result("selected_get_status_cached_seed", selected->getStatusId, selected->statusBufferSize, selected->statusVersion,
        PROBE_KIND_STATUS, selected, selectedMaskSeedSource, selectedMaskSeed, sizeof(cachedMask), true, selectedNumClocksSeed, true);
    append(",\n");
    append_probe_result("selected_get_control_ff_seed", selected->getControlId, selected->controlBufferSize, selected->controlVersion,
        PROBE_KIND_CONTROL, selected, "ff_mask_seed", ffMask, sizeof(ffMask), false, 0, false);
    append(",\n");
    append_probe_result("selected_get_control_cached_seed", selected->getControlId, selected->controlBufferSize, selected->controlVersion,
        PROBE_KIND_CONTROL, selected, selectedMaskSeedSource, selectedMaskSeed, sizeof(cachedMask), false, 0, true);
    append(",\n");
    append_probe_result("selected_get_status_cached_seed_v2", selected->getStatusId, selected->statusBufferSize, 2,
        PROBE_KIND_STATUS, selected, selectedMaskSeedSource, selectedMaskSeed, sizeof(cachedMask), true, selectedNumClocksSeed, false);
    append(",\n");
    append_probe_result("selected_get_control_cached_seed_v2", selected->getControlId, selected->controlBufferSize, 2,
        PROBE_KIND_CONTROL, selected, selectedMaskSeedSource, selectedMaskSeed, sizeof(cachedMask), false, 0, false);
    append(",\n");
    append_probe_result("selected_get_status_cached_seed_alt_size_minus_4", selected->getStatusId, selected->statusBufferSize >= 4 ? (selected->statusBufferSize - 4) : selected->statusBufferSize, selected->statusVersion,
        PROBE_KIND_STATUS, selected, selectedMaskSeedSource, selectedMaskSeed, sizeof(cachedMask), true, selectedNumClocksSeed, false);
    append(",\n");
    append_probe_result("selected_get_status_cached_seed_alt_size_plus_4", selected->getStatusId, selected->statusBufferSize + 4, selected->statusVersion,
        PROBE_KIND_STATUS, selected, selectedMaskSeedSource, selectedMaskSeed, sizeof(cachedMask), true, selectedNumClocksSeed, false);
    append(",\n");
    append_probe_result("blackwell_get_status_cached_seed", g_vfBackendBlackwell.getStatusId, g_vfBackendBlackwell.statusBufferSize, g_vfBackendBlackwell.statusVersion,
        PROBE_KIND_STATUS, &g_vfBackendBlackwell, selectedMaskSeedSource, selectedMaskSeed, sizeof(cachedMask), true, selectedNumClocksSeed, false);
    append("\n  ]\n");
    append("}\n");

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
    bool hasExplicitFanMode = false;

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

    GetPrivateProfileStringA("controls", "lock_ci", "", buf, sizeof(buf), path);
    trim_ascii(buf);
    if (buf[0]) {
        int value = -1;
        if (!parse_int_strict(buf, &value)) {
            set_message(err, errSize, "Invalid lock_ci in %s", path);
            return false;
        }
        desired->hasLock = value >= 0;
        desired->lockCi = value;
    }

    GetPrivateProfileStringA("controls", "lock_mhz", "", buf, sizeof(buf), path);
    trim_ascii(buf);
    if (buf[0]) {
        int value = 0;
        if (!parse_int_strict(buf, &value) || value < 0) {
            set_message(err, errSize, "Invalid lock_mhz in %s", path);
            return false;
        }
        if (value > 0) {
            desired->hasLock = true;
            desired->lockMHz = (unsigned int)value;
        }
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
        hasExplicitFanMode = true;
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
        if (!hasExplicitFanMode) {
            set_desired_fan_from_legacy_value(desired, fanAuto, fanPercent);
        } else if (desired->fanMode == FAN_MODE_FIXED && !fanAuto) {
            desired->hasFan = true;
            desired->fanAuto = false;
            desired->fanPercent = clamp_percent(fanPercent);
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
        if (!hasExplicitFanMode || desired->fanMode == FAN_MODE_FIXED) {
            desired->hasFan = true;
            desired->fanMode = FAN_MODE_FIXED;
            desired->fanAuto = false;
            desired->fanPercent = clamp_percent(value);
        }
    }

    if (!load_fan_curve_config_from_section(path, "fan_curve", &desired->fanCurve, err, errSize)) return false;

    char curveSemantics[64] = {};
    GetPrivateProfileStringA("curve", "curve_semantics", "", curveSemantics, sizeof(curveSemantics), path);
    trim_ascii(curveSemantics);
    bool legacyCurveSemantics = curveSemantics[0] == 0;

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

    bool basePlusGpuOffsetCurve = streqi_ascii(curveSemantics, "base_plus_gpu_offset");
    if (basePlusGpuOffsetCurve && desired->hasGpuOffset && desired->gpuOffsetMHz != 0) {
        for (int i = 0; i < VF_NUM_POINTS; i++) {
            if (!desired->hasCurvePoint[i]) continue;
            int offsetCompmhz = gpu_offset_component_mhz_for_point(i, desired->gpuOffsetMHz, desired->gpuOffsetExcludeLow70);
            int absoluteMhz = (int)desired->curvePointMHz[i] + offsetCompmhz;
            if (absoluteMhz <= 0) {
                desired->hasCurvePoint[i] = false;
                desired->curvePointMHz[i] = 0;
                continue;
            }
            desired->curvePointMHz[i] = (unsigned int)absoluteMhz;
        }
    } else if (legacyCurveSemantics && desired->hasGpuOffset && desired->gpuOffsetMHz != 0) {
        for (int i = 0; i < VF_NUM_POINTS; i++) {
            desired->hasCurvePoint[i] = false;
            desired->curvePointMHz[i] = 0;
        }
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
    bool currentActiveGpuOffsetExcludeLow70 = current_applied_gpu_offset_excludes_low_points();
    int currentGpuOffsetMHz = current_applied_gpu_offset_mhz();
    bool currentGpuOffsetExcludeLow70 = currentActiveGpuOffsetExcludeLow70;
    get_window_text_safe(g_app.hGpuOffsetEdit, buf, sizeof(buf));
    int gpuOffsetMHz = currentGpuOffsetMHz;
    if (buf[0]) {
        if (!parse_int_strict(buf, &gpuOffsetMHz)) {
            set_message(err, errSize, "Invalid GPU offset");
            return false;
        }
    }
    bool gpuOffsetExcludeLow70 = g_app.guiGpuOffsetExcludeLow70;
    bool desiredActiveGpuOffsetExcludeLow70 = gpuOffsetExcludeLow70 && gpuOffsetMHz != 0;
    debug_log("capture_gui: gpuOffsetMHz=%d excludeState=%d desiredExclude=%d currentExclude=%d\n",
        gpuOffsetMHz, gpuOffsetExcludeLow70 ? 1 : 0, desiredActiveGpuOffsetExcludeLow70 ? 1 : 0, currentGpuOffsetExcludeLow70 ? 1 : 0);
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
        currentLockMHz = displayed_curve_mhz(g_app.curve[lockCi].freq_kHz);
        effectiveLockTargetMHz = (int)g_app.lockedFreq;
        if (effectiveLockTargetMHz <= 0) {
            char lockBuf[32] = {};
            get_window_text_safe(g_app.hEditsMhz[g_app.lockedVi], lockBuf, sizeof(lockBuf));
            if (!lockBuf[0] && captureAllCurvePoints) {
                effectiveLockTargetMHz = (int)currentLockMHz;
            } else if (!parse_int_strict(lockBuf, &effectiveLockTargetMHz) || effectiveLockTargetMHz <= 0) {
                set_message(err, errSize, "Invalid MHz value for point %d", lockCi);
                return false;
            }
        }
        int currentLockGpuOffsetMHz = gpu_offset_component_mhz_for_point(lockCi, currentGpuOffsetMHz, currentActiveGpuOffsetExcludeLow70);
        int desiredLockGpuOffsetMHz = gpu_offset_component_mhz_for_point(lockCi, gpuOffsetMHz, desiredActiveGpuOffsetExcludeLow70);
        if (effectiveLockTargetMHz == (int)currentLockMHz && desiredLockGpuOffsetMHz != currentLockGpuOffsetMHz) {
            effectiveLockTargetMHz += desiredLockGpuOffsetMHz - currentLockGpuOffsetMHz;
            if (effectiveLockTargetMHz <= 0) effectiveLockTargetMHz = 1;
            lockTargetSynthesizedFromGpuOffset = true;
        }
        if (!captureAllCurvePoints && effectiveLockTargetMHz != (int)currentLockMHz) anyExplicitCurvePoint = true;
        desired->hasLock = true;
        desired->lockCi = lockCi;
        desired->lockMHz = (unsigned int)effectiveLockTargetMHz;
    }

    for (int vi = 0; vi < g_app.numVisible; vi++) {
        int ci = g_app.visibleMap[vi];
        unsigned int currentMHz = displayed_curve_mhz(g_app.curve[ci].freq_kHz);
        int mhz = 0;
        if (hasLock && expandLockedTail && vi >= g_app.lockedVi) {
            mhz = effectiveLockTargetMHz;
        } else if (hasLock && vi > g_app.lockedVi) {
            continue;
        } else {
            char pointBuf[32] = {};
            get_window_text_safe(g_app.hEditsMhz[vi], pointBuf, sizeof(pointBuf));
            if (!pointBuf[0] && captureAllCurvePoints) {
                mhz = (int)currentMHz;
            } else if (!parse_int_strict(pointBuf, &mhz) || mhz <= 0) {
                set_message(err, errSize, "Invalid MHz value for point %d", ci);
                return false;
            }
        }
        parsedCurveMHz[ci] = mhz;
        parsedCurveHave[ci] = true;
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
        bool synthesizedFromGpuOffset = synthCurveFromGpuOffset
            && !g_app.guiCurvePointExplicit[ci]
            && (unsigned int)mhz == currentMHz;
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

    const WCHAR* description = L"Launch Green Curve at user logon using the current startup configuration.";

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
        L"    <IdleSettings>\r\n"
        L"      <StopOnIdleEnd>false</StopOnIdleEnd>\r\n"
        L"      <RestartOnIdle>false</RestartOnIdle>\r\n"
        L"    </IdleSettings>\r\n"
        L"    <AllowStartOnDemand>true</AllowStartOnDemand>\r\n"
        L"    <Enabled>true</Enabled>\r\n"
        L"    <Hidden>false</Hidden>\r\n"
        L"    <RunOnlyIfIdle>false</RunOnlyIfIdle>\r\n"
        L"    <WakeToRun>false</WakeToRun>\r\n"
        L"    <ExecutionTimeLimit>%ls</ExecutionTimeLimit>\r\n"
        L"    <Priority>7</Priority>\r\n"
        L"  </Settings>\r\n"
        L"  <Actions Context=\"Author\">\r\n"
        L"    <Exec>\r\n"
        L"      <Command>%ls</Command>\r\n"
        L"      <WorkingDirectory>%ls</WorkingDirectory>\r\n"
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
    WCHAR workDir[MAX_PATH] = {};
    WCHAR workDirEsc[2048] = {};
    const WCHAR* executionTimeLimit = L"PT0S";
    StringCchCopyW(workDir, ARRAY_COUNT(workDir), exePath);
    WCHAR* slash = wcsrchr(workDir, L'\\');
    if (!slash) slash = wcsrchr(workDir, L'/');
    if (slash) *slash = 0;
    if (!xml_escape_wide(exePath, exeEsc, ARRAY_COUNT(exeEsc), false) ||
        !xml_escape_wide(cfgPath, cfgEsc, ARRAY_COUNT(cfgEsc), true) ||
        !xml_escape_wide(userName, userEsc, ARRAY_COUNT(userEsc), false) ||
        !xml_escape_wide(workDir, workDirEsc, ARRAY_COUNT(workDirEsc), false)) {
        CloseHandle(h);
        DeleteFileW(xmlPath);
        set_message(err, errSize, "Failed escaping startup task XML");
        return false;
    }

    HRESULT argsHr = StringCchPrintfW(
        argsEsc,
        ARRAY_COUNT(argsEsc),
        L"--logon-start --config &quot;%ls&quot;",
        cfgEsc);
    if (FAILED(argsHr)) {
        CloseHandle(h);
        DeleteFileW(xmlPath);
        set_message(err, errSize, "Startup task arguments too long");
        return false;
    }

    WCHAR xml[8192] = {};
    HRESULT hr = StringCchPrintfW(xml, ARRAY_COUNT(xml), xmlFmt, userEsc, description, userEsc, userEsc, executionTimeLimit, exeEsc, workDirEsc, argsEsc);
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
    bool taskExists = is_startup_task_enabled();

    if (logonSlot > 0 && !is_profile_slot_saved(g_app.configPath, logonSlot)) {
        logonSlot = 0;
        set_config_int(g_app.configPath, "profiles", "logon_slot", 0);
        shouldEnableTask = should_enable_startup_task_from_config(g_app.configPath);
    }

    if (shouldEnableTask != taskExists) {
        char err[256] = {};
        if (!set_startup_task_enabled(shouldEnableTask, err, sizeof(err)) && err[0]) {
            debug_log("startup task sync failed: %s\n", err);
        }
    }
    SendMessageA(g_app.hLogonCombo, CB_SETCURSEL, (WPARAM)logonSlot, 0);
    update_profile_state_label();
}

static DWORD WINAPI logon_sync_thread_proc(void* param) {
    HWND hwnd = (HWND)param;
    int logonSlot = get_config_int(g_app.configPath, "profiles", "logon_slot", 0);
    if (logonSlot < 0 || logonSlot > CONFIG_NUM_SLOTS) logonSlot = 0;
    bool shouldEnableTask = should_enable_startup_task_from_config(g_app.configPath);
    bool taskExists = is_startup_task_enabled();

    if (logonSlot > 0 && !is_profile_slot_saved(g_app.configPath, logonSlot)) {
        logonSlot = 0;
        set_config_int(g_app.configPath, "profiles", "logon_slot", 0);
        shouldEnableTask = should_enable_startup_task_from_config(g_app.configPath);
    }

    if (shouldEnableTask != taskExists) {
        char err[256] = {};
        if (!set_startup_task_enabled(shouldEnableTask, err, sizeof(err)) && err[0]) {
            debug_log("startup task sync failed: %s\n", err);
        }
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
        if (!is_startup_task_enabled()) return true;
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

    if (is_startup_task_enabled()) return true;

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
        } else if (wcscmp(arg, L"--probe-output") == 0) {
            opts->recognized = true;
            if (i + 1 >= argc || !copy_wide_to_utf8(argv[++i], opts->probeOutputPath, MAX_PATH)) {
                set_message(opts->error, sizeof(opts->error), "Invalid --probe-output path");
                LocalFree(argv);
                return false;
            }
            opts->hasProbeOutputPath = true;
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
        if (!g_app.curveOffsetRangeKnown) {
            set_curve_offset_range_khz(mn * 1000, mx * 1000);
        }
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
        set_message(detail, detailSize, "NVML not ready");
        return false;
    }

    memset(g_app.fanPercent, 0, sizeof(g_app.fanPercent));
    memset(g_app.fanTargetPercent, 0, sizeof(g_app.fanTargetPercent));
    memset(g_app.fanRpm, 0, sizeof(g_app.fanRpm));
    memset(g_app.fanPolicy, 0, sizeof(g_app.fanPolicy));
    memset(g_app.fanControlSignal, 0, sizeof(g_app.fanControlSignal));
    memset(g_app.fanTargetMask, 0, sizeof(g_app.fanTargetMask));
    g_app.fanCount = 0;
    g_app.fanMinPct = 0;
    g_app.fanMaxPct = 100;
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
        bool policyKnown = false;
        if (g_nvml_api.getFanControlPolicy) {
            unsigned int pol = 0;
            if (g_nvml_api.getFanControlPolicy(g_app.nvmlDevice, fan, &pol) == NVML_SUCCESS) {
                g_app.fanPolicy[fan] = pol;
                policyKnown = true;
                if (pol != NVML_FAN_POLICY_TEMPERATURE_CONTINOUS_SW) allAuto = false;
            }
        }
        bool isAutoForFan = true;
        if (policyKnown) {
            unsigned int pol = g_app.fanPolicy[fan];
            isAutoForFan = (pol == NVML_FAN_POLICY_TEMPERATURE_CONTINOUS_SW);
        }
        if (g_nvml_api.getFanSpeed) {
            unsigned int pct = 0;
            if (g_nvml_api.getFanSpeed(g_app.nvmlDevice, fan, &pct) == NVML_SUCCESS) {
                g_app.fanPercent[fan] = pct;
            }
        }
        bool shouldReadTarget = !policyKnown
            ? (g_app.fanCurveRuntimeActive || g_app.fanFixedRuntimeActive || g_app.activeFanMode != FAN_MODE_AUTO)
            : !isAutoForFan;
        if (g_nvml_api.getTargetFanSpeed && shouldReadTarget) {
            unsigned int target = 0;
            if (g_nvml_api.getTargetFanSpeed(g_app.nvmlDevice, fan, &target) == NVML_SUCCESS) {
                g_app.fanTargetPercent[fan] = target;
                if (!policyKnown && target > 0) allAuto = false;
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
    if (!nvml_ensure_ready() || !g_app.fanSupported || (!g_nvml_api.setDefaultFanSpeed && !g_nvml_api.setFanControlPolicy)) {
        set_message(detail, detailSize, "Fan auto unsupported");
        return false;
    }
    for (unsigned int fan = 0; fan < g_app.fanCount; fan++) {
        bool changed = false;
        if (g_nvml_api.setDefaultFanSpeed) {
            nvmlReturn_t r = g_nvml_api.setDefaultFanSpeed(g_app.nvmlDevice, fan);
            if (r != NVML_SUCCESS) {
                set_message(detail, detailSize, "fan %u: %s", fan, nvml_err_name(r));
                return false;
            }
            changed = true;
        }
        if (g_nvml_api.setFanControlPolicy) {
            nvmlReturn_t r = g_nvml_api.setFanControlPolicy(g_app.nvmlDevice, fan, NVML_FAN_POLICY_TEMPERATURE_CONTINOUS_SW);
            if (r == NVML_SUCCESS) {
                changed = true;
            } else if (!g_nvml_api.setDefaultFanSpeed) {
                set_message(detail, detailSize, "fan %u: %s", fan, nvml_err_name(r));
                return false;
            }
        }
        if (!changed) {
            set_message(detail, detailSize, "Fan auto unsupported");
            return false;
        }
    }
    for (int attempt = 0; attempt < 8; attempt++) {
        if (attempt > 0) Sleep(10);
        if (nvml_read_fans(detail, detailSize) && g_app.fanIsAuto) return true;
    }
    if (!nvml_read_fans(detail, detailSize)) return false;
    if (!g_app.fanIsAuto) {
        set_message(detail, detailSize, "Fan readback did not confirm driver auto mode");
        return false;
    }
    return true;
}

static bool desired_settings_have_explicit_state(const DesiredSettings* desired, bool requireCurve, char* err, size_t errSize) {
    if (!desired) {
        set_message(err, errSize, "No desired settings");
        return false;
    }

    if (!desired->hasGpuOffset) {
        set_message(err, errSize, "Profile is missing gpu_offset_mhz");
        return false;
    }
    if (!desired->hasMemOffset) {
        set_message(err, errSize, "Profile is missing mem_offset_mhz");
        return false;
    }
    if (!desired->hasPowerLimit) {
        set_message(err, errSize, "Profile is missing power_limit_pct");
        return false;
    }
    if (!desired->hasFan) {
        set_message(err, errSize, "Profile is missing fan_mode/fan settings");
        return false;
    }

    if (requireCurve) {
        bool haveCurvePoint = false;
        for (int i = 0; i < VF_NUM_POINTS; i++) {
            if (desired->hasCurvePoint[i]) {
                haveCurvePoint = true;
                break;
            }
        }
        if (!haveCurvePoint) {
            set_message(err, errSize, "Profile is missing VF curve points");
            return false;
        }
    }

    if (err && errSize > 0) err[0] = 0;
    return true;
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
            if (!manual_fan_readback_matches_target(pct, got, g_app.fanTargetPercent[fan])) ok = false;
        }
        if (ok) break;
    }
    if (exactApplied) *exactApplied = ok;
    if (!ok && detail && detailSize > 0 && !detail[0]) {
        set_message(detail, detailSize, "Fan readback did not confirm %d%%", pct);
    }
    return true;
}

static bool nvml_manual_fan_matches_target(int pct, bool* matches, char* detail, size_t detailSize) {
    if (matches) *matches = false;
    if (!nvml_read_fans(detail, detailSize)) return false;
    if (g_app.fanCount == 0) {
        set_message(detail, detailSize, "No fans detected");
        return false;
    }
    if (g_app.fanIsAuto) {
        set_message(detail, detailSize, "Driver fan policy reverted to auto");
        return true;
    }

    bool ok = true;
    for (unsigned int fan = 0; fan < g_app.fanCount; fan++) {
        int got = (int)g_app.fanPercent[fan];
        if (!manual_fan_readback_matches_target(pct, got, g_app.fanTargetPercent[fan])) ok = false;
    }
    if (!ok) {
        set_message(detail, detailSize, "Fan readback did not confirm %d%%", pct);
    }
    if (matches) *matches = ok;
    return true;
}

static bool fan_setting_matches_current(int wantMode, int wantPct, const FanCurveConfig* wantCurve) {
    if (!g_app.fanSupported) return false;
    if (wantMode != g_app.activeFanMode) return false;
    if (wantMode == FAN_MODE_AUTO) return g_app.fanIsAuto;
    if (wantMode == FAN_MODE_CURVE) {
        return !g_app.fanIsAuto && wantCurve && fan_curve_equals(wantCurve, &g_app.activeFanCurve);
    }
    if (g_app.fanIsAuto || g_app.fanCount == 0) return false;
    for (unsigned int fan = 0; fan < g_app.fanCount; fan++) {
        int gotPct = (int)g_app.fanPercent[fan];
        if (!manual_fan_readback_matches_target(wantPct, gotPct, g_app.fanTargetPercent[fan])) return false;
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
    detect_locked_tail_from_curve();
    (void)current_applied_gpu_offset_excludes_low_points();
    (void)current_applied_gpu_offset_mhz();
    initialize_gui_fan_settings_from_live_state();
    update_tray_icon();
    return ok1 || ok2 || ok3 || ok4;
}

static bool nvapi_get_vf_info_cached(unsigned char* maskOut, unsigned int* numClocksOut) {
    const VfBackendSpec* backend = g_app.vfBackend;
    if (!backend) return false;

    if (!g_app.vfInfoCached) {
        memset(g_app.vfMask, 0, sizeof(g_app.vfMask));
        memset(g_app.vfMask, 0xFF, 16);
        g_app.vfNumClocks = backend->defaultNumClocks;

        auto getInfo = (NvApiFunc)nvapi_qi(backend->getInfoId);
        if (getInfo) {
            unsigned char ibuf[0x4000] = {};
            if (backend->infoBufferSize > sizeof(ibuf)) return false;
            const unsigned int version = (backend->infoVersion << 16) | backend->infoBufferSize;
            memcpy(&ibuf[0], &version, sizeof(version));
            if (backend->infoMaskOffset + sizeof(g_app.vfMask) <= backend->infoBufferSize) {
                memset(&ibuf[backend->infoMaskOffset], 0xFF, sizeof(g_app.vfMask));
            }
            if (getInfo(g_app.gpuHandle, ibuf) == 0) {
                if (backend->infoMaskOffset + sizeof(g_app.vfMask) <= backend->infoBufferSize) {
                    memcpy(g_app.vfMask, &ibuf[backend->infoMaskOffset], sizeof(g_app.vfMask));
                }
                if (backend->infoNumClocksOffset + sizeof(g_app.vfNumClocks) <= backend->infoBufferSize) {
                    memcpy(&g_app.vfNumClocks, &ibuf[backend->infoNumClocksOffset], sizeof(g_app.vfNumClocks));
                }
                if (g_app.vfNumClocks == 0) g_app.vfNumClocks = backend->defaultNumClocks;
            }
        }
        g_app.vfInfoCached = true;
    }

    if (maskOut) memcpy(maskOut, g_app.vfMask, sizeof(g_app.vfMask));
    if (numClocksOut) *numClocksOut = g_app.vfNumClocks ? g_app.vfNumClocks : backend->defaultNumClocks;
    return true;
}

static int clamp_freq_delta_khz(int freqDelta_kHz) {
    int minkHz = 0;
    int maxkHz = 0;
    get_curve_offset_range_khz(&minkHz, &maxkHz);
    if (freqDelta_kHz > maxkHz) return maxkHz;
    if (freqDelta_kHz < minkHz) return minkHz;
    return freqDelta_kHz;
}

static void set_curve_offset_range_khz(int minkHz, int maxkHz) {
    if (minkHz > maxkHz) return;
    g_app.curveOffsetMinkHz = minkHz;
    g_app.curveOffsetMaxkHz = maxkHz;
    g_app.curveOffsetRangeKnown = true;
}

static bool get_curve_offset_range_khz(int* minkHz, int* maxkHz) {
    int minValue = -1000000;
    int maxValue = 1000000;
    bool known = false;

    if (g_app.curveOffsetRangeKnown && g_app.curveOffsetMinkHz <= g_app.curveOffsetMaxkHz) {
        minValue = g_app.curveOffsetMinkHz;
        maxValue = g_app.curveOffsetMaxkHz;
        known = true;
    } else if (g_app.gpuOffsetRangeKnown && g_app.gpuClockOffsetMinMHz <= g_app.gpuClockOffsetMaxMHz) {
        minValue = g_app.gpuClockOffsetMinMHz * 1000;
        maxValue = g_app.gpuClockOffsetMaxMHz * 1000;
        known = true;
    }

    if (minkHz) *minkHz = minValue;
    if (maxkHz) *maxkHz = maxValue;
    return known;
}

static bool nvapi_read_control_table(unsigned char* buf, size_t bufSize) {
    const VfBackendSpec* backend = g_app.vfBackend;
    if (!backend) return false;
    if (!buf || bufSize < backend->controlBufferSize) return false;

    auto getFunc = (NvApiFunc)nvapi_qi(backend->getControlId);
    if (!getFunc) return false;

    unsigned char mask[32] = {};
    if (!nvapi_get_vf_info_cached(mask, nullptr)) return false;

    memset(buf, 0, backend->controlBufferSize);
    const unsigned int version = (backend->controlVersion << 16) | backend->controlBufferSize;
    memcpy(&buf[0], &version, sizeof(version));
    if (backend->controlMaskOffset + sizeof(mask) > backend->controlBufferSize) return false;
    memcpy(&buf[backend->controlMaskOffset], mask, sizeof(mask));
    return getFunc(g_app.gpuHandle, buf) == 0;
}

static bool apply_curve_offsets_verified(const int* targetOffsets, const bool* pointMask, int maxBatchPasses) {
    if (!targetOffsets || !pointMask) return false;

    const VfBackendSpec* backend = g_app.vfBackend;
    if (!backend || !backend->writeSupported) return false;

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

    auto setFunc = (NvApiFunc)nvapi_qi(backend->setControlId);
    if (!setFunc) return false;

    unsigned char baseControl[0x4000] = {};
    if (backend->controlBufferSize > sizeof(baseControl)) return false;
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
        unsigned char buf[0x4000] = {};
        memcpy(buf, baseControl, backend->controlBufferSize);

        unsigned char writeMask[32] = {};
        bool anyPendingWrite = false;
        for (int i = 0; i < VF_NUM_POINTS; i++) {
            if (!pendingMask[i]) continue;
            int currentDelta = 0;
            unsigned int deltaOffset = backend->controlEntryBaseOffset + (unsigned int)i * backend->controlEntryStride + backend->controlEntryDeltaOffset;
            if (deltaOffset + sizeof(currentDelta) > backend->controlBufferSize) return false;
            memcpy(&currentDelta, &buf[deltaOffset], sizeof(currentDelta));
            if (currentDelta == desiredOffsets[i]) {
                pendingMask[i] = false;
                continue;
            }
            memcpy(&buf[deltaOffset], &desiredOffsets[i], sizeof(desiredOffsets[i]));
            writeMask[i / 8] |= (unsigned char)(1u << (i % 8));
            anyPendingWrite = true;
        }

        if (!anyPendingWrite) break;

        memcpy(&buf[backend->controlMaskOffset], writeMask, sizeof(writeMask));
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
            unsigned int deltaOffset = backend->controlEntryBaseOffset + (unsigned int)i * backend->controlEntryStride + backend->controlEntryDeltaOffset;
            if (deltaOffset + sizeof(g_app.freqOffsets[i]) > backend->controlBufferSize) return false;
            memcpy(&baseControl[deltaOffset], &g_app.freqOffsets[i], sizeof(g_app.freqOffsets[i]));
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
    bool liveGpuOffsetExcludeLow70 = current_applied_gpu_offset_excludes_low_points();
    int liveGpuOffsetMHz = current_applied_gpu_offset_mhz();
    g_app.appliedGpuOffsetExcludeLow70 = liveGpuOffsetExcludeLow70;
    g_app.appliedGpuOffsetMHz = liveGpuOffsetMHz;
    g_app.guiGpuOffsetExcludeLow70 = liveGpuOffsetExcludeLow70;
    g_app.guiGpuOffsetMHz = liveGpuOffsetMHz;
    if (g_app.hGpuOffsetEdit) {
        char buf[32];
        int gpuOffsetToShow = g_app.guiGpuOffsetMHz;
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
    if (!desired) return false;

    DesiredSettings full = {};
    if (!capture_gui_desired_settings(&full, false, true, false, err, errSize)) return false;

    DesiredSettings fanOnly = {};
    initialize_desired_settings_defaults(&fanOnly);

    int currentGpuOffsetMHz = current_applied_gpu_offset_mhz();
    bool currentGpuOffsetExcludeLow70 = current_applied_gpu_offset_excludes_low_points();
    int currentMemOffsetMHz = mem_display_mhz_from_driver_khz(g_app.memClockOffsetkHz);
    int currentPowerLimitPct = g_app.powerLimitPct;

    bool gpuUnchanged = !full.hasGpuOffset || (full.gpuOffsetMHz == currentGpuOffsetMHz && full.gpuOffsetExcludeLow70 == currentGpuOffsetExcludeLow70);
    bool memUnchanged = !full.hasMemOffset || (full.memOffsetMHz == currentMemOffsetMHz);
    bool powerUnchanged = !full.hasPowerLimit || (full.powerLimitPct == currentPowerLimitPct);

    bool curveUnchanged = true;
    for (int i = 0; i < VF_NUM_POINTS; i++) {
        if (!full.hasCurvePoint[i]) continue;
        if (g_app.curve[i].freq_kHz == 0) continue;
        if (g_app.lockedVi >= 0 && g_app.lockedVi < g_app.numVisible) {
            bool inLockedTail = false;
            for (int vi = g_app.lockedVi; vi < g_app.numVisible; vi++) {
                if (g_app.visibleMap[vi] == i) {
                    inLockedTail = true;
                    break;
                }
            }
            if (inLockedTail) continue;
        }
        unsigned int currentMHz = displayed_curve_mhz(g_app.curve[i].freq_kHz);
        if (full.curvePointMHz[i] != currentMHz) {
            curveUnchanged = false;
            break;
        }
    }

    if (gpuUnchanged && memUnchanged && powerUnchanged && curveUnchanged && full.hasFan) {
        debug_log("capture_gui_apply_settings: fan-only apply shortcut taken\n");
        *desired = fanOnly;
        desired->hasFan = true;
        desired->fanMode = full.fanMode;
        desired->fanAuto = full.fanAuto;
        desired->fanPercent = full.fanPercent;
        copy_fan_curve(&desired->fanCurve, &full.fanCurve);
        return true;
    }

    *desired = full;
    return true;
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

    if (!WritePrivateProfileStringA("debug", "enabled", g_debug_logging ? "1" : "0", path)) {
        set_message(err, errSize, "Failed to write debug");
        return false;
    }

    int gpuOffset = 0;
    bool gpuOffsetExcludeLow70 = false;
    resolve_effective_gpu_offset_state_for_config_save(desired, &gpuOffset, &gpuOffsetExcludeLow70);
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
    StringCchPrintfA(buf, ARRAY_COUNT(buf), "%d", desired && desired->hasLock ? desired->lockCi : (g_app.lockedCi >= 0 ? g_app.lockedCi : -1));
    if (!WritePrivateProfileStringA("controls", "lock_ci", buf, path)) {
        set_message(err, errSize, "Failed to write lock_ci");
        return false;
    }
    StringCchPrintfA(buf, ARRAY_COUNT(buf), "%u", desired && desired->hasLock ? desired->lockMHz : g_app.lockedFreq);
    if (!WritePrivateProfileStringA("controls", "lock_mhz", buf, path)) {
        set_message(err, errSize, "Failed to write lock_mhz");
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
    if (!WritePrivateProfileStringA("curve", "curve_semantics", "base_plus_gpu_offset", path)) {
        set_message(err, errSize, "Failed to write curve_semantics");
        return false;
    }
    int saveGpuOffsetMHz = gpuOffset;
    for (int i = 0; i < VF_NUM_POINTS; i++) {
        bool have = desired && desired->hasCurvePoint[i];
        unsigned int mhz = 0;
        if (have) {
            mhz = desired->curvePointMHz[i];
        } else if (useCurrentForUnset && g_app.curve[i].freq_kHz > 0) {
            mhz = displayed_curve_mhz(g_app.curve[i].freq_kHz);
        }
        if (mhz != 0 && saveGpuOffsetMHz != 0) {
            int offsetCompmhz = gpu_offset_component_mhz_for_point(i, saveGpuOffsetMHz, gpuOffsetExcludeLow70);
            int baseMhz = (int)mhz - offsetCompmhz;
            mhz = baseMhz > 0 ? (unsigned int)baseMhz : 0;
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

    invalidate_tray_profile_cache();
    return true;
}

static unsigned int displayed_curve_mhz(unsigned int rawFreq_kHz) {
    return (unsigned int)((displayed_curve_khz(rawFreq_kHz) + 500) / 1000);
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
            set_curve_target_mismatch_detail(ci, actualMHz, targetMHz, false, detail, detailSize);
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
                set_curve_target_mismatch_detail(ci, actualMHz, lockMhz, true, detail, detailSize);
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

static int curve_delta_khz_for_target_display_mhz_unclamped(int pointIndex, unsigned int displayMHz) {
    long long target = (long long)raw_curve_khz_from_display_mhz(displayMHz);
    long long base = (long long)curve_base_khz_for_point(pointIndex);
    long long delta = target - base;
    return (int)delta;
}

static int curve_delta_khz_for_target_display_mhz(int pointIndex, unsigned int displayMHz) {
    return clamp_freq_delta_khz(curve_delta_khz_for_target_display_mhz_unclamped(pointIndex, displayMHz));
}

static void set_curve_target_mismatch_detail(int pointIndex, unsigned int actualMHz, unsigned int targetMHz, bool lockTail, char* detail, size_t detailSize) {
    int requiredDeltaKHz = curve_delta_khz_for_target_display_mhz_unclamped(pointIndex, targetMHz);
    int minkHz = 0;
    int maxkHz = 0;
    bool rangeKnown = get_curve_offset_range_khz(&minkHz, &maxkHz);
    unsigned int voltMV = 0;
    if (pointIndex >= 0 && pointIndex < VF_NUM_POINTS) {
        voltMV = g_app.curve[pointIndex].volt_uV / 1000;
    }

    if (rangeKnown && requiredDeltaKHz < minkHz) {
        if (lockTail) {
            set_message(detail, detailSize,
                "Lock tail hit the minimum curve offset at %u mV: reaching %u MHz needs %d kHz, but the supported range is %d..%d kHz (actual %u MHz)",
                voltMV, targetMHz, requiredDeltaKHz, minkHz, maxkHz, actualMHz);
        } else {
            set_message(detail, detailSize,
                "VF point %d hit the minimum curve offset: reaching %u MHz needs %d kHz, but the supported range is %d..%d kHz (actual %u MHz)",
                pointIndex, targetMHz, requiredDeltaKHz, minkHz, maxkHz, actualMHz);
        }
        return;
    }

    if (rangeKnown && requiredDeltaKHz > maxkHz) {
        if (lockTail) {
            set_message(detail, detailSize,
                "Lock tail hit the maximum curve offset at %u mV: reaching %u MHz needs %d kHz, but the supported range is %d..%d kHz (actual %u MHz)",
                voltMV, targetMHz, requiredDeltaKHz, minkHz, maxkHz, actualMHz);
        } else {
            set_message(detail, detailSize,
                "VF point %d hit the maximum curve offset: reaching %u MHz needs %d kHz, but the supported range is %d..%d kHz (actual %u MHz)",
                pointIndex, targetMHz, requiredDeltaKHz, minkHz, maxkHz, actualMHz);
        }
        return;
    }

    if (lockTail) {
        set_message(detail, detailSize,
            "Lock tail verified at %u MHz @ %u mV instead of requested %u MHz",
            actualMHz, voltMV, targetMHz);
    } else {
        set_message(detail, detailSize,
            "VF point %d verified at %u MHz instead of requested %u MHz",
            pointIndex, actualMHz, targetMHz);
    }
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
    redraw_window_sync(g_app.hMainWnd);
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

static bool is_system_dark_theme_active() {
    DWORD value = 1;
    DWORD type = 0;
    DWORD size = sizeof(value);
    HKEY hKey = nullptr;
    if (RegOpenKeyExA(HKEY_CURRENT_USER,
            "Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
            0, KEY_READ, &hKey) != ERROR_SUCCESS) {
        return false;
    }
    bool dark = false;
    if (RegQueryValueExA(hKey, "AppsUseLightTheme", nullptr, &type, (LPBYTE)&value, &size) == ERROR_SUCCESS) {
        dark = value == 0;
    }
    RegCloseKey(hKey);
    return dark;
}

static void initialize_dark_mode_support() {
    if (s_darkModeResolved) return;
    s_darkModeResolved = true;

    HMODULE ux = LoadLibraryA("uxtheme.dll");
    if (!ux) return;

    s_fnAllowDarkModeForWindow = (AllowDarkModeForWindowFn)GetProcAddress(ux, MAKEINTRESOURCEA(133));
    s_fnSetPreferredAppMode = (SetPreferredAppModeFn)GetProcAddress(ux, MAKEINTRESOURCEA(135));
    s_fnFlushMenuThemes = (FlushMenuThemesFn)GetProcAddress(ux, MAKEINTRESOURCEA(136));

    if (s_fnSetPreferredAppMode) {
        s_fnSetPreferredAppMode(is_system_dark_theme_active() ? APP_MODE_ALLOW_DARK : APP_MODE_DEFAULT);
    }
    if (s_fnFlushMenuThemes) {
        s_fnFlushMenuThemes();
    }
}

static void refresh_menu_theme_cache() {
    initialize_dark_mode_support();
    if (s_fnSetPreferredAppMode) {
        s_fnSetPreferredAppMode(is_system_dark_theme_active() ? APP_MODE_ALLOW_DARK : APP_MODE_DEFAULT);
    }
    if (s_fnFlushMenuThemes) {
        s_fnFlushMenuThemes();
    }
}

static void allow_dark_mode_for_window(HWND hwnd) {
    if (!hwnd) return;
    initialize_dark_mode_support();
    if (s_fnAllowDarkModeForWindow) {
        s_fnAllowDarkModeForWindow(hwnd, is_system_dark_theme_active() ? TRUE : FALSE);
    }
}

static const char* ui_font_face_name() {
    return "Segoe UI";
}

static HFONT create_ui_sized_font(int heightPx, int weight) {
    LOGFONTA lf = {};
    if (s_uiBaseLogFontReady) {
        lf = s_uiBaseLogFont;
    } else {
        NONCLIENTMETRICSA ncm = {};
        ncm.cbSize = sizeof(ncm);
        if (SystemParametersInfoA(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0)) {
            lf = ncm.lfMessageFont;
        } else {
            lf.lfCharSet = DEFAULT_CHARSET;
            lf.lfOutPrecision = OUT_DEFAULT_PRECIS;
            lf.lfClipPrecision = CLIP_DEFAULT_PRECIS;
            lf.lfPitchAndFamily = DEFAULT_PITCH | FF_SWISS;
            StringCchCopyA(lf.lfFaceName, ARRAY_COUNT(lf.lfFaceName), ui_font_face_name());
        }
    }

    lf.lfHeight = -nvmax(1, heightPx);
    lf.lfWeight = weight;
    lf.lfItalic = FALSE;
    lf.lfUnderline = FALSE;
    lf.lfStrikeOut = FALSE;
    lf.lfQuality = CLEARTYPE_QUALITY;
    lf.lfOutPrecision = OUT_TT_PRECIS;
    lf.lfClipPrecision = CLIP_DEFAULT_PRECIS;
    lf.lfPitchAndFamily = DEFAULT_PITCH | FF_SWISS;
    StringCchCopyA(lf.lfFaceName, ARRAY_COUNT(lf.lfFaceName), ui_font_face_name());
    return CreateFontIndirectA(&lf);
}

static HFONT get_ui_font() {
    if (s_hUiFont) return s_hUiFont;

    NONCLIENTMETRICSA ncm = {};
    ncm.cbSize = sizeof(ncm);
    if (SystemParametersInfoA(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0)) {
        s_uiBaseLogFont = ncm.lfMessageFont;
        s_uiBaseLogFontReady = true;
    } else {
        memset(&s_uiBaseLogFont, 0, sizeof(s_uiBaseLogFont));
        s_uiBaseLogFont.lfHeight = -MulDiv(9, g_dpi, 72);
        s_uiBaseLogFont.lfWeight = FW_NORMAL;
        s_uiBaseLogFont.lfCharSet = DEFAULT_CHARSET;
        s_uiBaseLogFont.lfOutPrecision = OUT_TT_PRECIS;
        s_uiBaseLogFont.lfClipPrecision = CLIP_DEFAULT_PRECIS;
        s_uiBaseLogFont.lfQuality = CLEARTYPE_QUALITY;
        s_uiBaseLogFont.lfPitchAndFamily = DEFAULT_PITCH | FF_SWISS;
        StringCchCopyA(s_uiBaseLogFont.lfFaceName, ARRAY_COUNT(s_uiBaseLogFont.lfFaceName), ui_font_face_name());
        s_uiBaseLogFontReady = true;
    }

    s_hUiFont = create_ui_sized_font(dp(12), FW_NORMAL);
    return s_hUiFont;
}

static void apply_ui_font(HWND hwnd) {
    if (!hwnd) return;
    HFONT font = get_ui_font();
    if (!font) return;
    SendMessageA(hwnd, WM_SETFONT, (WPARAM)font, FALSE);
}

static void apply_ui_font_to_children(HWND parent) {
    if (!parent) return;
    HWND child = GetWindow(parent, GW_CHILD);
    while (child) {
        apply_ui_font(child);
        child = GetWindow(child, GW_HWNDNEXT);
    }
}

static int themed_combo_item_height() {
    return dp(22);
}

struct GdpStartupInput { UINT32 ver; void* debugCb; int suppressBg; int suppressExt; };
enum { GDP_SMOOTH_AA = 4, GDP_UNIT_PX = 2 };

typedef int  (WINAPI *GdpStartupFn)(ULONG_PTR*, const GdpStartupInput*, void*);
typedef void (WINAPI *GdpShutdownFn)(ULONG_PTR);
typedef int  (WINAPI *GdpGfxFromHDCFn)(HDC, void**);
typedef int  (WINAPI *GdpDelGfxFn)(void*);
typedef int  (WINAPI *GdpSetSmoothFn)(void*, int);
typedef int  (WINAPI *GdpMakePenFn)(unsigned int, float, int, void**);
typedef int  (WINAPI *GdpDelPenFn)(void*);
typedef int  (WINAPI *GdpDrawBeziersIFn)(void*, void*, const POINT*, int);
typedef int  (WINAPI *GdpDrawLinesIFn)(void*, void*, const POINT*, int);
typedef int  (WINAPI *GdpFillEllipseIFn)(void*, void*, int, int, int, int);
typedef int  (WINAPI *GdpDrawEllipseIFn)(void*, void*, int, int, int, int);
typedef int  (WINAPI *GdpMakeBrushFn)(unsigned int, void**);
typedef int  (WINAPI *GdpDelBrushFn)(void*);

static HMODULE       s_gdp_dll = nullptr;
static ULONG_PTR     s_gdp_token = 0;
static bool          s_gdp_tried = false;
static bool          s_gdp_ok = false;
static GdpGfxFromHDCFn  s_fnGfxHDC;
static GdpDelGfxFn      s_fnDelGfx;
static GdpSetSmoothFn   s_fnSmooth;
static GdpMakePenFn     s_fnMakePen;
static GdpDelPenFn      s_fnDelPen;
static GdpDrawBeziersIFn s_fnBeziers;
static GdpDrawLinesIFn   s_fnLines;
static GdpFillEllipseIFn s_fnFillEllipse;
static GdpDrawEllipseIFn s_fnDrawEllipse;
static GdpMakeBrushFn    s_fnMakeBrush;
static GdpDelBrushFn     s_fnDelBrush;
static GdpShutdownFn     s_fnShutdown;

static unsigned int colorref_to_argb(COLORREF c) {
    return 0xFF000000u | ((c & 0xFF) << 16) | (c & 0xFF00) | ((c >> 16) & 0xFF);
}

static bool gdiplus_ensure() {
    if (s_gdp_tried) return s_gdp_ok;
    s_gdp_tried = true;
    s_gdp_dll = LoadLibraryA("gdiplus.dll");
    if (!s_gdp_dll) return false;
    auto r = [](const char* n) -> void* { return (void*)GetProcAddress(s_gdp_dll, n); };
    GdpStartupFn startup = (GdpStartupFn)r("GdiplusStartup");
    s_fnShutdown    = (GdpShutdownFn)r("GdiplusShutdown");
    s_fnGfxHDC      = (GdpGfxFromHDCFn)r("GdipCreateFromHDC");
    s_fnDelGfx      = (GdpDelGfxFn)r("GdipDeleteGraphics");
    s_fnSmooth      = (GdpSetSmoothFn)r("GdipSetSmoothingMode");
    s_fnMakePen     = (GdpMakePenFn)r("GdipCreatePen1");
    s_fnDelPen      = (GdpDelPenFn)r("GdipDeletePen");
    s_fnBeziers     = (GdpDrawBeziersIFn)r("GdipDrawBeziersI");
    s_fnLines       = (GdpDrawLinesIFn)r("GdipDrawLinesI");
    s_fnFillEllipse = (GdpFillEllipseIFn)r("GdipFillEllipseI");
    s_fnDrawEllipse = (GdpDrawEllipseIFn)r("GdipDrawEllipseI");
    s_fnMakeBrush   = (GdpMakeBrushFn)r("GdipCreateSolidFill");
    s_fnDelBrush    = (GdpDelBrushFn)r("GdipDeleteBrush");
    if (!startup || !s_fnShutdown || !s_fnGfxHDC || !s_fnDelGfx || !s_fnSmooth ||
        !s_fnMakePen || !s_fnDelPen || !s_fnBeziers || !s_fnLines ||
        !s_fnFillEllipse || !s_fnDrawEllipse || !s_fnMakeBrush || !s_fnDelBrush)
        return false;
    GdpStartupInput inp = { 1, nullptr, 0, 0 };
    if (startup(&s_gdp_token, &inp, nullptr) != 0) return false;
    s_gdp_ok = true;
    return true;
}

static void shutdown_gdiplus() {
    if (s_gdp_ok && s_fnShutdown) s_fnShutdown(s_gdp_token);
    s_gdp_ok = false;
    s_gdp_token = 0;
    if (s_gdp_dll) { FreeLibrary(s_gdp_dll); s_gdp_dll = nullptr; }
}

static void draw_curve_polyline_smooth(HDC hdc, const POINT* pts, int count, int widthPx, COLORREF color) {
    if (!hdc || !pts || count < 2) return;
    if (gdiplus_ensure()) {
        void* gfx = nullptr;
        s_fnGfxHDC(hdc, &gfx);
        if (gfx) {
            s_fnSmooth(gfx, GDP_SMOOTH_AA);
            void* pen = nullptr;
            s_fnMakePen(colorref_to_argb(color), (float)nvmax(1, widthPx), GDP_UNIT_PX, &pen);
            if (pen) {
                if (count < 3) {
                    s_fnLines(gfx, pen, pts, count);
                } else {
                    POINT bez[VF_NUM_POINTS * 3] = {};
                    int bc = 0;
                    bez[bc++] = pts[0];
                    for (int i = 0; i < count - 1; i++) {
                        POINT p0 = (i > 0) ? pts[i - 1] : pts[i];
                        POINT p1 = pts[i];
                        POINT p2 = pts[i + 1];
                        POINT p3 = (i + 2 < count) ? pts[i + 2] : pts[i + 1];
                        int c1y = p1.y + (p2.y - p0.y) / 6;
                        int c2y = p2.y - (p3.y - p1.y) / 6;
                        int yLo = nvmin(p1.y, p2.y);
                        int yHi = nvmax(p1.y, p2.y);
                        if (c1y < yLo) c1y = yLo;
                        if (c1y > yHi) c1y = yHi;
                        if (c2y < yLo) c2y = yLo;
                        if (c2y > yHi) c2y = yHi;
                        bez[bc++] = { p1.x + (p2.x - p0.x) / 6, c1y };
                        bez[bc++] = { p2.x - (p3.x - p1.x) / 6, c2y };
                        bez[bc++] = p2;
                    }
                    s_fnBeziers(gfx, pen, bez, bc);
                }
                s_fnDelPen(pen);
            }
            s_fnDelGfx(gfx);
        }
        return;
    }
    HPEN pen = CreatePen(PS_SOLID, nvmax(1, widthPx), color);
    HPEN oldPen = (HPEN)SelectObject(hdc, pen);
    int oldMode = SetBkMode(hdc, TRANSPARENT);
    if (count < 3) {
        Polyline(hdc, pts, count);
    } else {
        POINT bez[VF_NUM_POINTS * 3] = {};
        int bc = 0;
        bez[bc++] = pts[0];
        for (int i = 0; i < count - 1; i++) {
            POINT p0 = (i > 0) ? pts[i - 1] : pts[i];
            POINT p1 = pts[i];
            POINT p2 = pts[i + 1];
            POINT p3 = (i + 2 < count) ? pts[i + 2] : pts[i + 1];
            int c1y = p1.y + (p2.y - p0.y) / 6;
            int c2y = p2.y - (p3.y - p1.y) / 6;
            int yLo = nvmin(p1.y, p2.y);
            int yHi = nvmax(p1.y, p2.y);
            if (c1y < yLo) c1y = yLo;
            if (c1y > yHi) c1y = yHi;
            if (c2y < yLo) c2y = yLo;
            if (c2y > yHi) c2y = yHi;
            bez[bc++] = { p1.x + (p2.x - p0.x) / 6, c1y };
            bez[bc++] = { p2.x - (p3.x - p1.x) / 6, c2y };
            bez[bc++] = p2;
        }
        PolyBezier(hdc, bez, bc);
    }
    SetBkMode(hdc, oldMode);
    SelectObject(hdc, oldPen);
    DeleteObject(pen);
}

static void draw_curve_points_ringed(HDC hdc, const POINT* pts, int count, int innerRadiusPx, int outerRadiusPx) {
    if (!hdc || !pts || count < 1) return;
    if (gdiplus_ensure()) {
        void* gfx = nullptr;
        s_fnGfxHDC(hdc, &gfx);
        if (gfx) {
            s_fnSmooth(gfx, GDP_SMOOTH_AA);
            void* ringPen = nullptr;
            s_fnMakePen(colorref_to_argb(COL_CURVE), 1.0f, GDP_UNIT_PX, &ringPen);
            void* fillBr = nullptr;
            s_fnMakeBrush(colorref_to_argb(COL_POINT), &fillBr);
            if (ringPen) {
                for (int i = 0; i < count; i++) {
                    s_fnDrawEllipse(gfx, ringPen,
                        pts[i].x - outerRadiusPx, pts[i].y - outerRadiusPx,
                        outerRadiusPx * 2 + 1, outerRadiusPx * 2 + 1);
                }
                s_fnDelPen(ringPen);
            }
            if (fillBr) {
                for (int i = 0; i < count; i++) {
                    s_fnFillEllipse(gfx, fillBr,
                        pts[i].x - innerRadiusPx, pts[i].y - innerRadiusPx,
                        innerRadiusPx * 2 + 1, innerRadiusPx * 2 + 1);
                }
                s_fnDelBrush(fillBr);
            }
            s_fnDelGfx(gfx);
        }
        return;
    }
    HBRUSH fillBrush = CreateSolidBrush(COL_POINT);
    HPEN ringPen = CreatePen(PS_SOLID, 1, COL_CURVE);
    HPEN oldPen = (HPEN)SelectObject(hdc, ringPen);
    HBRUSH oldBrush = (HBRUSH)SelectObject(hdc, GetStockObject(HOLLOW_BRUSH));
    for (int i = 0; i < count; i++) {
        Ellipse(hdc,
            pts[i].x - outerRadiusPx,
            pts[i].y - outerRadiusPx,
            pts[i].x + outerRadiusPx + 1,
            pts[i].y + outerRadiusPx + 1);
    }
    SelectObject(hdc, fillBrush);
    for (int i = 0; i < count; i++) {
        Ellipse(hdc,
            pts[i].x - innerRadiusPx,
            pts[i].y - innerRadiusPx,
            pts[i].x + innerRadiusPx + 1,
            pts[i].y + innerRadiusPx + 1);
    }
    SelectObject(hdc, oldBrush);
    SelectObject(hdc, oldPen);
    DeleteObject(fillBrush);
    DeleteObject(ringPen);
}

static void style_input_control(HWND hwnd) {
    if (!hwnd) return;
    LONG_PTR exStyle = GetWindowLongPtrA(hwnd, GWL_EXSTYLE);
    exStyle &= ~WS_EX_CLIENTEDGE;
    SetWindowLongPtrA(hwnd, GWL_EXSTYLE, exStyle);
    SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    apply_ui_font(hwnd);
}

static void style_combo_control(HWND hwnd) {
    if (!hwnd) return;

    typedef HRESULT (WINAPI *set_window_theme_t)(HWND, LPCWSTR, LPCWSTR);
    static set_window_theme_t setWindowTheme = nullptr;
    static bool resolved = false;
    if (!resolved) {
        HMODULE ux = LoadLibraryA("uxtheme.dll");
        if (ux) setWindowTheme = (set_window_theme_t)GetProcAddress(ux, "SetWindowTheme");
        resolved = true;
    }
    // Use DarkMode_CFD so WM_CTLCOLORSTATIC works for text
    if (setWindowTheme) {
        setWindowTheme(hwnd, is_system_dark_theme_active() ? L"DarkMode_CFD" : L"CFD", nullptr);
    }
    allow_dark_mode_for_window(hwnd);
    COMBOBOXINFO info = {};
    info.cbSize = sizeof(info);
    if (GetComboBoxInfo(hwnd, &info)) {
        if (info.hwndList) {
            allow_dark_mode_for_window(info.hwndList);
            apply_ui_font(info.hwndList);
        }
        if (info.hwndItem) {
            allow_dark_mode_for_window(info.hwndItem);
            apply_ui_font(info.hwndItem);
        }
    }
    install_themed_combo_subclass(hwnd);
    apply_ui_font(hwnd);
    SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
}

static bool is_themed_combo_id(UINT id) {
    return id == FAN_MODE_COMBO_ID || id == PROFILE_COMBO_ID || id == APP_LAUNCH_COMBO_ID ||
           id == LOGON_COMBO_ID || id == FAN_DIALOG_INTERVAL_ID || id == FAN_DIALOG_HYSTERESIS_ID;
}

static void paint_themed_combo_overlay(HWND hwnd, HDC hdc) {
    if (!hwnd || !hdc) return;
    COMBOBOXINFO info = {};
    info.cbSize = sizeof(info);
    if (!GetComboBoxInfo(hwnd, &info)) return;

    RECT client = {};
    GetClientRect(hwnd, &client);
    RECT buttonRc = info.rcButton;
    MapWindowPoints(nullptr, hwnd, (POINT*)&buttonRc, 2);
    bool disabled = !IsWindowEnabled(hwnd);
    bool dropped = SendMessageA(hwnd, CB_GETDROPPEDSTATE, 0, 0) != 0;

    // Ensure buttonRc has valid coordinates
    if (buttonRc.right <= buttonRc.left || buttonRc.bottom <= buttonRc.top) {
        buttonRc.left = client.right - dp(20);
        buttonRc.right = client.right;
        buttonRc.top = client.top;
        buttonRc.bottom = client.bottom;
    }

    // Paint over the button area with our theming
    HRGN hOldClip = CreateRectRgn(0, 0, 0, 0);
    int clipResult = GetClipRgn(hdc, hOldClip);
    HRGN hButtonRegion = CreateRectRgnIndirect(&buttonRc);
    SelectClipRgn(hdc, hButtonRegion);
    DeleteObject(hButtonRegion);

    // Fill button area with panel color (or pressed color when dropped)
    HBRUSH buttonBr = CreateSolidBrush(dropped ? COL_BUTTON_PRESSED : COL_PANEL);
    FillRect(hdc, &buttonRc, buttonBr);
    DeleteObject(buttonBr);

    // Draw separator line between text and button
    HPEN sepPen = CreatePen(PS_SOLID, 1, disabled ? RGB(0x56, 0x56, 0x64) : COL_BUTTON_BORDER);
    HPEN oldSepPen = (HPEN)SelectObject(hdc, sepPen);
    MoveToEx(hdc, buttonRc.left, buttonRc.top + 1, nullptr);
    LineTo(hdc, buttonRc.left, buttonRc.bottom - 1);
    SelectObject(hdc, oldSepPen);
    DeleteObject(sepPen);

    // Draw dropdown arrow triangle (theme-matched muted blue-gray)
    int centerX = buttonRc.left + (buttonRc.right - buttonRc.left) / 2;
    int centerY = buttonRc.top + (buttonRc.bottom - buttonRc.top) / 2;
    
    POINT tri[3] = {
        { centerX - dp(3), centerY - dp(2) },
        { centerX + dp(3), centerY - dp(2) },
        { centerX, centerY + dp(2) }
    };
    COLORREF arrowColor = disabled ? COL_LABEL : RGB(0xB0, 0xC0, 0xD0);
    HBRUSH arrowBr = CreateSolidBrush(arrowColor);
    HBRUSH oldArrowBrush = (HBRUSH)SelectObject(hdc, arrowBr);
    HPEN oldArrowPen = (HPEN)SelectObject(hdc, GetStockObject(NULL_PEN));
    Polygon(hdc, tri, 3);
    SelectObject(hdc, oldArrowPen);
    SelectObject(hdc, oldArrowBrush);
    DeleteObject(arrowBr);

    // Restore clipping
    if (clipResult == 1) {
        SelectClipRgn(hdc, hOldClip);
    } else {
        SelectClipRgn(hdc, nullptr);
    }
    DeleteObject(hOldClip);

    // Paint over Windows' default border with our themed border
    HBRUSH borderBrush = CreateSolidBrush(disabled ? RGB(0x56, 0x56, 0x64) : COL_BUTTON_BORDER);
    // Top edge
    RECT topEdge = { client.left, client.top, client.right, client.top + 1 };
    FillRect(hdc, &topEdge, borderBrush);
    // Bottom edge  
    RECT bottomEdge = { client.left, client.bottom - 1, client.right, client.bottom };
    FillRect(hdc, &bottomEdge, borderBrush);
    // Left edge
    RECT leftEdge = { client.left, client.top + 1, client.left + 1, client.bottom - 1 };
    FillRect(hdc, &leftEdge, borderBrush);
    // Right edge
    RECT rightEdge = { client.right - 1, client.top + 1, client.right, client.bottom - 1 };
    FillRect(hdc, &rightEdge, borderBrush);
    DeleteObject(borderBrush);
}

static void paint_themed_combo_full_custom(HWND hwnd, HDC hdc) {
    if (!hwnd || !hdc) return;
    COMBOBOXINFO info = {};
    info.cbSize = sizeof(info);
    if (!GetComboBoxInfo(hwnd, &info)) return;

    RECT client = {};
    GetClientRect(hwnd, &client);
    RECT buttonRc = info.rcButton;
    MapWindowPoints(nullptr, hwnd, (POINT*)&buttonRc, 2);
    bool disabled = !IsWindowEnabled(hwnd);
    bool dropped = SendMessageA(hwnd, CB_GETDROPPEDSTATE, 0, 0) != 0;

    // Ensure buttonRc has valid coordinates
    if (buttonRc.right <= buttonRc.left || buttonRc.bottom <= buttonRc.top) {
        buttonRc.left = client.right - dp(20);
        buttonRc.right = client.right;
        buttonRc.top = client.top;
        buttonRc.bottom = client.bottom;
    }

    // Fill entire background with input color first
    HBRUSH bgBrush = CreateSolidBrush(COL_INPUT);
    FillRect(hdc, &client, bgBrush);
    DeleteObject(bgBrush);

    // Fill button area with panel color (or pressed color when dropped)
    HBRUSH buttonBr = CreateSolidBrush(dropped ? COL_BUTTON_PRESSED : COL_PANEL);
    FillRect(hdc, &buttonRc, buttonBr);
    DeleteObject(buttonBr);

    // Draw the text
    int sel = (int)SendMessageA(hwnd, CB_GETCURSEL, 0, 0);
    char textBuf[256] = {};
    if (sel != CB_ERR) {
        SendMessageA(hwnd, CB_GETLBTEXT, (WPARAM)sel, (LPARAM)textBuf);
    }

    if (textBuf[0]) {
        RECT textRc = client;
        textRc.left += dp(6);
        textRc.right = buttonRc.left - dp(4);
        textRc.top += 2;
        textRc.bottom -= 2;
        
        if (textRc.right > textRc.left) {
            SetBkMode(hdc, TRANSPARENT);
            SetTextColor(hdc, disabled ? COL_LABEL : COL_TEXT);
            
            HFONT uiFont = get_ui_font();
            HFONT oldFont = nullptr;
            if (uiFont) {
                oldFont = (HFONT)SelectObject(hdc, uiFont);
            }
            
            DrawTextA(hdc, textBuf, -1, &textRc, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
            
            if (oldFont) {
                SelectObject(hdc, oldFont);
            }
        }
    }

    // Draw separator line between text and button
    HPEN sepPen = CreatePen(PS_SOLID, 1, disabled ? RGB(0x56, 0x56, 0x64) : COL_BUTTON_BORDER);
    HPEN oldSepPen = (HPEN)SelectObject(hdc, sepPen);
    MoveToEx(hdc, buttonRc.left, buttonRc.top + 1, nullptr);
    LineTo(hdc, buttonRc.left, buttonRc.bottom - 1);
    SelectObject(hdc, oldSepPen);
    DeleteObject(sepPen);

    // Draw dropdown arrow triangle (theme-matched muted blue-gray)
    int centerX = buttonRc.left + (buttonRc.right - buttonRc.left) / 2;
    int centerY = buttonRc.top + (buttonRc.bottom - buttonRc.top) / 2;
    
    POINT tri[3] = {
        { centerX - dp(3), centerY - dp(2) },
        { centerX + dp(3), centerY - dp(2) },
        { centerX, centerY + dp(2) }
    };
    COLORREF arrowColor = disabled ? COL_LABEL : RGB(0xB0, 0xC0, 0xD0);
    HBRUSH arrowBr = CreateSolidBrush(arrowColor);
    HBRUSH oldArrowBrush = (HBRUSH)SelectObject(hdc, arrowBr);
    HPEN oldArrowPen = (HPEN)SelectObject(hdc, GetStockObject(NULL_PEN));
    Polygon(hdc, tri, 3);
    SelectObject(hdc, oldArrowPen);
    SelectObject(hdc, oldArrowBrush);
    DeleteObject(arrowBr);

    // Draw themed border around entire control
    HBRUSH borderBrush = CreateSolidBrush(disabled ? RGB(0x56, 0x56, 0x64) : COL_BUTTON_BORDER);
    // Top edge
    RECT topEdge = { client.left, client.top, client.right, client.top + 1 };
    FillRect(hdc, &topEdge, borderBrush);
    // Bottom edge  
    RECT bottomEdge = { client.left, client.bottom - 1, client.right, client.bottom };
    FillRect(hdc, &bottomEdge, borderBrush);
    // Left edge
    RECT leftEdge = { client.left, client.top + 1, client.left + 1, client.bottom - 1 };
    FillRect(hdc, &leftEdge, borderBrush);
    // Right edge
    RECT rightEdge = { client.right - 1, client.top + 1, client.right, client.bottom - 1 };
    FillRect(hdc, &rightEdge, borderBrush);
    DeleteObject(borderBrush);
}

static LRESULT CALLBACK themed_combo_wndproc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    WNDPROC original = (WNDPROC)GetPropA(hwnd, "GreenCurveComboOrigProc");
    if (!original) return DefWindowProcA(hwnd, msg, wParam, lParam);

    switch (msg) {
        case WM_PAINT: {
            // Let Windows draw with its theming - we accept the default border/arrow
            // The WM_CTLCOLORSTATIC handler ensures text is readable
            LRESULT result = CallWindowProcA(original, hwnd, msg, wParam, lParam);
            return result;
        }

        case WM_ERASEBKGND:
            // Prevent default erasing to avoid flicker
            return 1;

        case WM_NCPAINT:
        case WM_ENABLE:
        case WM_THEMECHANGED:
        case WM_SETTINGCHANGE:
        case CB_SHOWDROPDOWN:
        case WM_MOUSELEAVE:
        case WM_MOUSEMOVE:
        case WM_LBUTTONDOWN:
        case WM_LBUTTONUP: {
            LRESULT result = CallWindowProcA(original, hwnd, msg, wParam, lParam);
            InvalidateRect(hwnd, nullptr, FALSE);
            return result;
        }

        case WM_NCDESTROY:
            RemovePropA(hwnd, "GreenCurveComboOrigProc");
            SetWindowLongPtrA(hwnd, GWLP_WNDPROC, (LONG_PTR)original);
            break;
    }

    return CallWindowProcA(original, hwnd, msg, wParam, lParam);
}

static void install_themed_combo_subclass(HWND hwnd) {
    if (!hwnd) return;
    if (GetPropA(hwnd, "GreenCurveComboOrigProc")) return;
    WNDPROC original = (WNDPROC)GetWindowLongPtrA(hwnd, GWLP_WNDPROC);
    if (!original) return;
    SetPropA(hwnd, "GreenCurveComboOrigProc", (HANDLE)original);
    SetWindowLongPtrA(hwnd, GWLP_WNDPROC, (LONG_PTR)themed_combo_wndproc);
}

static void draw_themed_combo_item(const DRAWITEMSTRUCT* dis) {
    if (!dis || dis->CtlType != ODT_COMBOBOX) return;

    HDC hdc = dis->hDC;
    RECT rc = dis->rcItem;
    bool selected = (dis->itemState & ODS_SELECTED) != 0;
    bool focus = (dis->itemState & ODS_FOCUS) != 0;

    // Determine colors based on state
    COLORREF bgColor;
    COLORREF textColor;
    if (selected) {
        bgColor = COL_BUTTON;  // Use button color for selection
        textColor = RGB(0xF0, 0xF4, 0xFF);  // Bright text for selected
    } else {
        bgColor = COL_INPUT;  // Dark input background
        textColor = COL_TEXT;  // Normal text
    }

    // Fill background
    HBRUSH bgBrush = CreateSolidBrush(bgColor);
    FillRect(hdc, &rc, bgBrush);
    DeleteObject(bgBrush);

    // Draw focus rectangle if focused
    if (focus) {
        HPEN focusPen = CreatePen(PS_DOT, 1, COL_BUTTON_BORDER);
        HPEN oldPen = (HPEN)SelectObject(hdc, focusPen);
        HBRUSH oldBrush = (HBRUSH)SelectObject(hdc, GetStockObject(HOLLOW_BRUSH));
        Rectangle(hdc, rc.left + 1, rc.top + 1, rc.right - 1, rc.bottom - 1);
        SelectObject(hdc, oldBrush);
        SelectObject(hdc, oldPen);
        DeleteObject(focusPen);
    }

    // Get item text
    char text[256] = {};
    if (dis->itemID != (UINT)-1) {
        SendMessageA(dis->hwndItem, CB_GETLBTEXT, dis->itemID, (LPARAM)text);
    }

    // Draw text
    if (text[0]) {
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, textColor);
        HFONT font = (HFONT)SendMessageA(dis->hwndItem, WM_GETFONT, 0, 0);
        HFONT oldFont = (HFONT)SelectObject(hdc, font ? font : get_ui_font());
        RECT textRc = rc;
        textRc.left += dp(6);
        textRc.right -= dp(6);
        DrawTextA(hdc, text, -1, &textRc, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
        SelectObject(hdc, oldFont);
    }
}

static void measure_themed_combo_item(MEASUREITEMSTRUCT* mis) {
    if (!mis) return;
    // Set item height to match our themed controls
    mis->itemHeight = dp(18);
}

static void draw_checkbox_tick_smooth(HDC hdc, const RECT* box, COLORREF color) {
    if (!hdc || !box) return;

    POINT pts[3] = {
        { box->left + (box->right - box->left) * 22 / 100, box->top + (box->bottom - box->top) * 54 / 100 },
        { box->left + (box->right - box->left) * 44 / 100, box->top + (box->bottom - box->top) * 74 / 100 },
        { box->left + (box->right - box->left) * 78 / 100, box->top + (box->bottom - box->top) * 28 / 100 },
    };

    if (gdiplus_ensure()) {
        void* gfx = nullptr;
        s_fnGfxHDC(hdc, &gfx);
        if (gfx) {
            s_fnSmooth(gfx, GDP_SMOOTH_AA);
            void* pen = nullptr;
            float width = (float)nvmax(2, (box->right - box->left) / 5);
            s_fnMakePen(colorref_to_argb(color), width, GDP_UNIT_PX, &pen);
            if (pen) {
                s_fnLines(gfx, pen, pts, 3);
                s_fnDelPen(pen);
            }
            s_fnDelGfx(gfx);
            return;
        }
    }

    HPEN pen = CreatePen(PS_SOLID, nvmax(2, (box->right - box->left) / 5), color);
    HPEN oldPen = (HPEN)SelectObject(hdc, pen);
    MoveToEx(hdc, pts[0].x, pts[0].y, nullptr);
    LineTo(hdc, pts[1].x, pts[1].y);
    LineTo(hdc, pts[2].x, pts[2].y);
    SelectObject(hdc, oldPen);
    DeleteObject(pen);
}

static bool is_themed_button_id(UINT id) {
    switch (id) {
        case APPLY_BTN_ID:
        case REFRESH_BTN_ID:
        case RESET_BTN_ID:
        case LICENSE_BTN_ID:
        case PROFILE_LOAD_ID:
        case PROFILE_SAVE_ID:
        case PROFILE_CLEAR_ID:
        case FAN_CURVE_BTN_ID:
        case FAN_DIALOG_OK_ID:
        case FAN_DIALOG_CANCEL_ID:
            return true;
    }
    return false;
}

static bool is_themed_checkbox_id(UINT id) {
    return id == GPU_OFFSET_EXCLUDE_LOW_CHECK_ID || id == START_ON_LOGON_CHECK_ID || is_fan_dialog_checkbox_id(id);
}

static bool is_fan_dialog_checkbox_id(UINT id) {
    return id >= FAN_DIALOG_ENABLE_BASE && id < FAN_DIALOG_ENABLE_BASE + FAN_CURVE_MAX_POINTS;
}

static bool themed_checkbox_checked_state(UINT id, HWND hwnd) {
    if (id == GPU_OFFSET_EXCLUDE_LOW_CHECK_ID) return g_app.guiGpuOffsetExcludeLow70;
    if (id == START_ON_LOGON_CHECK_ID) return is_start_on_logon_enabled(g_app.configPath);
    if (is_fan_dialog_checkbox_id(id)) {
        int pointIndex = (int)id - FAN_DIALOG_ENABLE_BASE;
        if (pointIndex >= 0 && pointIndex < FAN_CURVE_MAX_POINTS) {
            return g_fanCurveDialog.working.points[pointIndex].enabled;
        }
    }
    return hwnd && (SendMessageA(hwnd, BM_GETCHECK, 0, 0) == BST_CHECKED);
}

static void write_error_report_log_for_user_failure(const char* summary, const char* details) {
    char logErr[256] = {};
    if (!write_error_report_log(summary, details, logErr, sizeof(logErr)) && logErr[0]) {
        debug_log("error report log failed: %s\n", logErr);
    }
}

static void draw_themed_button(const DRAWITEMSTRUCT* dis) {
    if (!dis) return;

    HDC hdc = dis->hDC;
    RECT rc = dis->rcItem;
    bool disabled = (dis->itemState & ODS_DISABLED) != 0;
    bool pressed = (dis->itemState & ODS_SELECTED) != 0;
    bool focused = (dis->itemState & ODS_FOCUS) != 0;
    bool checkbox = is_themed_checkbox_id(dis->CtlID);
    bool checked = checkbox && themed_checkbox_checked_state(dis->CtlID, dis->hwndItem);
    HFONT controlFont = dis->hwndItem ? (HFONT)SendMessageA(dis->hwndItem, WM_GETFONT, 0, 0) : nullptr;
    HFONT oldFont = (HFONT)SelectObject(hdc, controlFont ? controlFont : get_ui_font());

    HBRUSH bg = CreateSolidBrush(COL_BG);
    FillRect(hdc, &rc, bg);
    DeleteObject(bg);
    SetBkMode(hdc, TRANSPARENT);

    if (checkbox) {
        char text[64] = {};
        GetWindowTextA(dis->hwndItem, text, ARRAY_COUNT(text));
        bool labeledCheckbox = is_fan_dialog_checkbox_id(dis->CtlID) && text[0];
        int controlW = rc.right - rc.left;
        int controlH = rc.bottom - rc.top;
        int boxSize = labeledCheckbox
            ? nvmin(controlH - dp(4), dp(16))
            : nvmin(controlW, controlH) - dp(2);
        if (boxSize < dp(12)) boxSize = dp(12);
        if (boxSize > controlW) boxSize = controlW;
        if (boxSize > controlH) boxSize = controlH;
        int boxLeft = labeledCheckbox
            ? rc.left + dp(2)
            : rc.left + (controlW - boxSize) / 2;
        RECT box = {
            boxLeft,
            rc.top + (controlH - boxSize) / 2,
            boxLeft + boxSize,
            rc.top + (controlH - boxSize) / 2 + boxSize,
        };

        COLORREF fill = disabled ? COL_BUTTON_DISABLED : (checked ? COL_BUTTON : COL_PANEL);
        COLORREF border = disabled ? RGB(0x5A, 0x5A, 0x68) : COL_BUTTON_BORDER;
        HBRUSH fillBr = CreateSolidBrush(fill);
        FillRect(hdc, &box, fillBr);
        DeleteObject(fillBr);

        HPEN pen = CreatePen(PS_SOLID, 1, border);
        HPEN oldPen = (HPEN)SelectObject(hdc, pen);
        HBRUSH oldBrush = (HBRUSH)SelectObject(hdc, GetStockObject(HOLLOW_BRUSH));
        Rectangle(hdc, box.left, box.top, box.right + 1, box.bottom + 1);
        SelectObject(hdc, oldBrush);
        DeleteObject(SelectObject(hdc, oldPen));

        if (checked) {
            draw_checkbox_tick_smooth(hdc, &box, disabled ? COL_LABEL : RGB(0xE8, 0xF2, 0xFF));
        }

        if (labeledCheckbox) {
            RECT textRc = rc;
            textRc.left = box.right + dp(8);
            SetTextColor(hdc, disabled ? COL_LABEL : COL_TEXT);
            DrawTextA(hdc, text, -1, &textRc, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        }
    } else {
        COLORREF fill = disabled ? COL_BUTTON_DISABLED : (pressed ? COL_BUTTON_PRESSED : COL_BUTTON);
        HBRUSH fillBr = CreateSolidBrush(fill);
        FillRect(hdc, &rc, fillBr);
        DeleteObject(fillBr);

        HPEN borderPen = CreatePen(PS_SOLID, 1, disabled ? RGB(0x56, 0x56, 0x64) : COL_BUTTON_BORDER);
        HPEN oldPen = (HPEN)SelectObject(hdc, borderPen);
        HBRUSH oldBrush = (HBRUSH)SelectObject(hdc, GetStockObject(HOLLOW_BRUSH));
        Rectangle(hdc, rc.left, rc.top, rc.right, rc.bottom);
        SelectObject(hdc, oldBrush);
        DeleteObject(SelectObject(hdc, oldPen));

        char text[128] = {};
        GetWindowTextA(dis->hwndItem, text, ARRAY_COUNT(text));
        RECT textRc = rc;
        if (pressed) OffsetRect(&textRc, 0, 1);
        SetTextColor(hdc, disabled ? COL_LABEL : RGB(0xF0, 0xF4, 0xFF));
        DrawTextA(hdc, text, -1, &textRc, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    }

    if (focused) {
        RECT focus = rc;
        InflateRect(&focus, -3, -3);
        DrawFocusRect(hdc, &focus);
    }
    SelectObject(hdc, oldFont);
}

static void draw_lock_checkbox(const DRAWITEMSTRUCT* dis) {
    if (!dis) return;
    HDC hdc = dis->hDC;
    RECT rc = dis->rcItem;
    bool disabled = (dis->itemState & ODS_DISABLED) != 0;
    int vi = (int)dis->CtlID - LOCK_BASE_ID;
    bool checked = (vi >= 0 && vi < g_app.numVisible && vi == g_app.lockedVi);

    HBRUSH bg = CreateSolidBrush(COL_BG);
    FillRect(hdc, &rc, bg);
    DeleteObject(bg);

    int boxSize = nvmin(rc.right - rc.left, rc.bottom - rc.top) - dp(2);
    if (boxSize < dp(10)) boxSize = dp(10);
    RECT box = {
        rc.left + (rc.right - rc.left - boxSize) / 2,
        rc.top + (rc.bottom - rc.top - boxSize) / 2,
        rc.left + (rc.right - rc.left - boxSize) / 2 + boxSize,
        rc.top + (rc.bottom - rc.top - boxSize) / 2 + boxSize,
    };

    COLORREF border = disabled ? RGB(0x5A, 0x5A, 0x68) : COL_BUTTON_BORDER;
    COLORREF fill = disabled ? COL_BUTTON_DISABLED : (checked ? COL_BUTTON : COL_PANEL);
    HBRUSH fillBr = CreateSolidBrush(fill);
    FillRect(hdc, &box, fillBr);
    DeleteObject(fillBr);

    HPEN pen = CreatePen(PS_SOLID, 1, border);
    HPEN oldPen = (HPEN)SelectObject(hdc, pen);
    HBRUSH oldBrush = (HBRUSH)SelectObject(hdc, GetStockObject(HOLLOW_BRUSH));
    Rectangle(hdc, box.left, box.top, box.right + 1, box.bottom + 1);
    SelectObject(hdc, oldBrush);
    DeleteObject(SelectObject(hdc, oldPen));

    if (checked) {
        draw_checkbox_tick_smooth(hdc, &box, disabled ? COL_LABEL : RGB(0xE8, 0xF2, 0xFF));
    }

    if (dis->itemState & ODS_FOCUS) {
        RECT focus = rc;
        InflateRect(&focus, -2, -2);
        DrawFocusRect(hdc, &focus);
    }
}

#include "gpu_backend.cpp"

#include "ui_main.cpp"

#include "entry.cpp"
