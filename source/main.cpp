// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT

// Green Curve v0.8 - NVIDIA VF Curve Editor
// Win32 GDI application

#include "app_shared.h"
#include "fan_curve.h"
#include "win32_raii.h"

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

#ifndef STACK_SIZE_PARAM_IS_A_RESERVATION
#define STACK_SIZE_PARAM_IS_A_RESERVATION 0x00010000
#endif

#include <userenv.h>

static int g_programmaticEditUpdateDepth = 0;
static char g_recentUiActions[16][96] = {};
static unsigned int g_recentUiActionNext = 0;
static unsigned int g_recentUiActionCount = 0;
static char g_pendingOperationSource[64] = {};
static char g_lastOperationIntent[4096] = {};
static char g_lastOperationPlan[4096] = {};

#ifdef GREEN_CURVE_SERVICE_BINARY
#define OP_SNAPSHOT_SIZE 4096
#else
#define OP_SNAPSHOT_SIZE 24576
#endif

static char g_lastOperationBeforeSnapshot[OP_SNAPSHOT_SIZE] = {};
static char g_lastOperationAfterSnapshot[OP_SNAPSHOT_SIZE] = {};
static ULONGLONG g_debugSessionStartTickMs = 0;
static char g_userDataDir[MAX_PATH] = {};
static char g_cliLogPath[MAX_PATH] = {};
static char g_debugLogPath[MAX_PATH] = {};
static char g_debugEarlyLogPath[MAX_PATH] = {};
static char g_debugLogOpenPath[MAX_PATH] = {};
static char g_jsonPath[MAX_PATH] = {};
static char g_errorLogPath[MAX_PATH] = {};
static char g_lastApplyPhase[128] = {};
static ULONGLONG g_fanTelemetryBoostUntilTickMs = 0;
static HANDLE g_serviceStopEvent = nullptr;
static HANDLE g_serviceFanStopEvent = nullptr;
static HANDLE g_serviceFanThread = nullptr;
static HANDLE g_serviceRuntimeLock = nullptr;
static HANDLE g_servicePipeWakeEvent = nullptr;
static DWORD g_serviceRuntimeLockOwnerThreadId = 0;
static unsigned int g_serviceRuntimeLockDepth = 0;
static SERVICE_STATUS_HANDLE g_serviceStatusHandle = nullptr;
static SERVICE_STATUS g_serviceStatus = {};
static DesiredSettings g_serviceActiveDesired = {};
static bool g_serviceHasActiveDesired = false;
static ControlState g_serviceControlState = {};
static bool g_serviceControlStateValid = false;
static bool g_serviceUserPathsResolved = false;
static CRITICAL_SECTION g_debugLogLock = {};
static HANDLE g_debugLogFile = INVALID_HANDLE_VALUE;
static DWORD g_serviceUserPathsSessionId = (DWORD)-1;
static char g_serviceUserProfileDir[MAX_PATH] = {};
static ULONGLONG g_serviceRuntimeLastPulseLogTickMs = 0;
static ULONGLONG g_serviceFanThreadLastWaitLogTickMs = 0;
static DWORD g_serviceFanThreadLastWaitMs = 0;
static bool g_serviceFanThreadLastWaitCurve = false;
static bool g_serviceFanThreadLastWaitFixed = false;
static ULONGLONG g_serviceTelemetryLastHardwarePollTickMs = 0;
static char g_serviceTelemetryLastPollSource[64] = {};

static const int SERVICE_DEBUG_DEFAULT_ENABLED = 1; // Service logs are opt-out via [debug] enabled=0 or GREEN_CURVE_DEBUG=0.
static const DWORD SERVICE_PIPE_CLIENT_CONNECT_SLICE_MS = 250;
static const DWORD SERVICE_PIPE_CLIENT_SLEEP_SLICE_MS = 10;
static const DWORD SERVICE_PIPE_SERVER_IO_TIMEOUT_MS = 2000;
static const DWORD SERVICE_FAN_THREAD_STOP_TIMEOUT_MS = 5000;
static const DWORD SERVICE_RUNTIME_NOISY_LOG_INTERVAL_MS = 5000;
static const ULONGLONG SERVICE_TELEMETRY_IDLE_REFRESH_INTERVAL_MS = 5000;
static const ULONGLONG SERVICE_TELEMETRY_RUNTIME_STALE_GRACE_MS = 1000;
static const DWORD ELEVATED_HELPER_TIMEOUT_MS = 60000;

static void* nvapi_qi(unsigned int id);
static bool nvapi_init();
static bool gpu_adapter_has_same_pci_identity(const GpuAdapterInfo* a, const GpuAdapterInfo* b);
static void format_gpu_adapter_label(const GpuAdapterInfo* adapter, char* out, size_t outSize);
static bool nvapi_enum_gpu();
static bool nvapi_get_name();
static void reset_gpu_runtime_selection();
static void populate_gpu_selector();
static void apply_gpu_selection_from_ui();
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
static bool show_best_guess_support_warning(HWND parent);
static void detect_clock_offsets();
static int uniform_curve_offset_khz();
static void set_curve_offset_range_khz(int minkHz, int maxkHz);
static bool get_curve_offset_range_khz(int* minkHz, int* maxkHz);
static int clamp_freq_delta_khz(int freqDelta_kHz);
static bool nvapi_set_point(int pointIndex, int freqDelta_kHz);
static bool apply_curve_offsets_verified(const int* targetOffsets, const bool* pointMask, int maxBatchPasses);
static void close_startup_sync_thread_handle();
static void invalidate_main_window();
static bool ensure_directory_recursive_windows(const char* path, char* err, size_t errSize);
static bool ensure_parent_directory_for_file(const char* path, char* err, size_t errSize);
static bool get_known_folder_path_utf8(REFKNOWNFOLDERID folderId, char* out, size_t outSize);
static bool resolve_data_paths(char* err, size_t errSize);
static void clear_service_authoritative_state();
static int format_log_timestamp_prefix(char* out, size_t outSize);
static const char* cli_log_path();
static const char* debug_log_path();
static const char* service_early_debug_log_path();
static const char* json_snapshot_path();
static const char* error_log_path();
static bool parse_wide_int_arg(LPWSTR text, int* out);
static bool copy_wide_to_utf8(LPWSTR text, char* out, int outSize);
static bool copy_wide_to_ansi(LPWSTR text, char* out, int outSize);
static bool utf8_to_wide(const char* text, WCHAR* out, int outCount);
static bool get_current_user_sam_name(WCHAR* out, DWORD outCount);
static bool hardware_initialize(char* detail, size_t detailSize);
static void populate_service_snapshot(ServiceSnapshot* snapshot);
static void populate_control_state(ControlState* state);
static void apply_service_snapshot_to_app(const ServiceSnapshot* snapshot);
static void apply_service_desired_to_gui(const DesiredSettings* desired);
static void apply_control_state_to_gui(const ControlState* state);
static bool get_effective_control_state(ControlState* stateOut);
static void service_capture_owner_identity(const char* user, DWORD sessionId);
static bool get_pipe_client_identity(HANDLE pipe, char* userOut, size_t userOutSize, DWORD* sessionIdOut, DWORD* pidOut, char* err, size_t errSize);
static bool service_caller_is_authorized(HANDLE pipe, const char* source, char* err, size_t errSize, char* callerUserOut, size_t callerUserOutSize, DWORD* callerSessionIdOut, DWORD* callerPidOut);
static bool get_active_interactive_session_id(DWORD* sessionIdOut);
static void ensure_service_runtime_lock();
static void lock_service_runtime();
static void unlock_service_runtime();
static bool service_runtime_lock_held_by_current_thread();
static void service_set_pending_operation_source(const char* source);
static void set_last_apply_phase(const char* phase);
static LONG WINAPI green_curve_unhandled_exception_filter(EXCEPTION_POINTERS* info);
static bool service_resolve_active_user_paths_for_startup(const char* context);
static DWORD WINAPI service_fan_runtime_thread_proc(void*);
static DWORD WINAPI service_pipe_server_thread_proc(void*);
static bool ensure_service_fan_runtime_thread();
static void stop_service_fan_runtime_thread();
static void service_runtime_pulse();
static void mark_service_telemetry_cache_updated(const char* source);
static bool service_refresh_telemetry_for_request(char* detail, size_t detailSize);
static bool service_apply_desired_settings(const DesiredSettings* desired, bool interactive, char* result, size_t resultSize);
static bool service_reset_all(char* result, size_t resultSize);
static bool background_service_pipe_name(WCHAR* out, size_t outCount);
static bool service_is_installed();
static bool service_is_running();
static bool query_background_service_state(bool* installedOut, bool* runningOut);
static bool refresh_background_service_state();
static bool service_send_request(const ServiceRequest* request, ServiceResponse* response, DWORD timeoutMs, char* err, size_t errSize);
static bool service_client_ping(char* err, size_t errSize);
static bool service_client_get_snapshot(ServiceSnapshot* snapshot, char* err, size_t errSize);
static bool service_client_get_telemetry(ServiceSnapshot* snapshot, char* err, size_t errSize);
static bool service_client_apply_desired(const DesiredSettings* desired, const char* source, bool interactive, char* result, size_t resultSize, ServiceSnapshot* snapshotOut);
static bool service_client_reset(char* result, size_t resultSize, ServiceSnapshot* snapshotOut);
static bool service_client_get_active_desired(DesiredSettings* desired, ServiceSnapshot* snapshotOut, char* err, size_t errSize);
static bool service_install_or_remove(bool enable, char* err, size_t errSize);
static bool wait_for_background_service_ready(DWORD timeoutMs, char* err, size_t errSize);
static bool launch_service_admin_helper(bool enable, char* err, size_t errSize);
static void begin_background_service_toggle(bool enable);
static void end_background_service_toggle();
static bool is_elevated();
static bool config_file_exists();
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
static void destroy_edit_controls(HWND hParent);
static void apply_lock(int vi);
static void set_edit_value(HWND hEdit, unsigned int value);
static void unlock_all();
static int mem_display_mhz_from_driver_khz(int driver_kHz);
static int mem_display_mhz_from_driver_mhz(int driverMHz);
static int mem_driver_khz_from_display_mhz(int displayMHz);
static unsigned int displayed_curve_mhz(unsigned int rawFreq_kHz);
static int curve_delta_khz_for_target_display_mhz_unclamped(int pointIndex, unsigned int displayMHz);
static void set_curve_target_mismatch_detail(int pointIndex, unsigned int actualMHz, unsigned int targetMHz, bool lockTail, char* detail, size_t detailSize);
static bool curve_targets_match_request(const DesiredSettings* desired, const bool* lockedTailMask, unsigned int lockMhz, char* detail, size_t detailSize);
static void apply_system_titlebar_theme(HWND hwnd);
static void show_license_dialog(HWND parent);
static void layout_bottom_buttons(HWND hParent);
static void debug_log(const char* fmt, ...);
static void debug_log_session_marker(const char* phase, const char* kind, const char* extra = nullptr);
static void close_debug_log_file();
static HMODULE load_system_library_a(const char* name);
static bool find_trusted_nvidia_smi_path_w(WCHAR* out, size_t outCount);
static bool find_trusted_nvidia_smi_path_a(char* out, size_t outSize);
static bool file_is_regular_no_reparse_w(const WCHAR* path);
static bool write_text_file_atomic(const char* path, const char* data, size_t dataSize, char* err, size_t errSize);
static bool write_text_file_atomic_service(const char* path, const char* data, size_t dataSize, char* err, size_t errSize);
static bool write_log_snapshot(const char* path, char* err, size_t errSize);
static bool write_json_snapshot(const char* path, char* err, size_t errSize);
static bool write_probe_report(const char* path, char* err, size_t errSize);
static bool write_error_report_log(const char* summary, const char* details, char* err, size_t errSize);
static void clear_last_operation_details();
static void set_pending_operation_source(const char* source);
static void record_ui_action(const char* fmt, ...);
static void build_recent_ui_actions_text(char* out, size_t outSize);
static void build_point_list_from_flags(const bool* flags, char* out, size_t outSize, int maxItems = 24);
static void describe_live_gpu_offset_state(char* out, size_t outSize);
static void build_operation_intent_summary(const DesiredSettings* desired, bool interactive, char* out, size_t outSize);
static void capture_last_operation_snapshot(char* dst, size_t dstSize);
static bool build_state_snapshot_text(char* text, size_t textSize);
static void begin_programmatic_edit_update();
static void end_programmatic_edit_update();
static bool programmatic_edit_update_active();
static bool save_desired_to_config_with_startup(const char* path, const DesiredSettings* desired, bool useCurrentForUnset, int startupState, char* err, size_t errSize);
static void initialize_desired_settings_defaults(DesiredSettings* desired);
static void copy_fan_curve(FanCurveConfig* destination, const FanCurveConfig* source);
static bool should_suppress_startup_ui();
// Profile I/O
static bool load_desired_settings_from_ini(const char* path, DesiredSettings* desired, char* err, size_t errSize);
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
static void update_background_service_controls();
static bool maybe_confirm_profile_load_replace(int slot);
static bool maybe_load_selected_profile_to_gui_without_apply();
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
static void boost_fan_telemetry_for_ms(DWORD durationMs);
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
static bool is_gpu_offset_excluded_low_point(int pointIndex, int gpuOffsetMHz, int excludeLowCount);
static int gpu_offset_component_mhz_for_point(int pointIndex, int gpuOffsetMHz, int excludeLowCount);
static bool detect_live_selective_gpu_offset_state(int* gpuOffsetMHzOut, int* representativeOffsetkHzOut = nullptr);
static bool live_selective_gpu_offset_matches_requested_state_with_tolerance(int gpuOffsetMHz, int toleranceMHz);
static bool live_selective_gpu_offset_matches_requested_state(int gpuOffsetMHz);
static bool load_runtime_selective_gpu_offset_request(int* gpuOffsetMHzOut, int* excludeLowCountOut);
static bool load_matching_runtime_selective_gpu_offset_request(int* gpuOffsetMHzOut, int* excludeLowCountOut);
static bool live_curve_has_any_nonzero_offsets();
static bool service_active_desired_gpu_offset_fallback(int* gpuOffsetMHzOut, int* excludeLowCountOut);
static bool refresh_service_snapshot_and_active_desired(char* err, size_t errSize, DesiredSettings* activeDesiredOut = nullptr);
static void build_full_live_desired_settings(DesiredSettings* desired);
static bool load_curve_points_explicit_from_section(const char* path, const char* section, DesiredSettings* desired, char* err, size_t errSize);
static bool curve_section_uses_base_plus_gpu_offset_semantics(const char* path, const char* section, const DesiredSettings* desired);
static void restore_curve_points_from_base_plus_gpu_offset(DesiredSettings* desired);
static bool can_save_curve_as_base_plus_gpu_offset(const DesiredSettings* desired, int gpuOffsetMHz, int excludeLowCount);
static int curve_base_khz_for_point(int pointIndex);
static void update_desired_lock_from_live_curve(DesiredSettings* desired);
static void persist_runtime_selective_gpu_offset_request(int gpuOffsetMHz, int excludeLowCount);
static void clear_runtime_selective_gpu_offset_request();
static void resolve_displayed_live_gpu_offset_state_for_gui(int* gpuOffsetMHzOut, int* excludeLowCountOut);
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

static const VfBackendSpec g_vfBackendFuture = {
    "future",
    GPU_FAMILY_UNKNOWN,
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
    HBRUSH hEditBrush;
    HBRUSH hListBrush;
    HBRUSH hInputBrush;
    HBRUSH hStaticBrush;
};

struct LicenseDialogState {
    HWND hwnd;
    HWND textEdit;
    HWND closeButton;
    HWND owner;
    HBRUSH hInputBrush;
    HBRUSH hBgBrush;
};

static FanCurveDialogState g_fanCurveDialog = {};
static LicenseDialogState g_licenseDialog = {};
static HANDLE g_singleInstanceMutex = nullptr;
#ifndef GREEN_CURVE_SERVICE_BINARY
static UINT g_taskbarCreatedMessage = 0;
#endif
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
#include "main_gpu_front.cpp"
#include "main_gpu_state.cpp"
#include "main_fan_runtime.cpp"
#include "main_shell.cpp"
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

#ifndef GREEN_CURVE_SERVICE_BINARY
#include "ui_main.cpp"
#include "entry.cpp"
#else
int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) {
    SetProcessDPIAware();
    init_dpi();
    InitializeCriticalSection(&g_debugLogLock);
    SERVICE_TABLE_ENTRYW table[] = {
        { (LPWSTR)L"GreenCurveService", service_main },
        { nullptr, nullptr },
    };
    StartServiceCtrlDispatcherW(table);
    return 0;
}
#endif
