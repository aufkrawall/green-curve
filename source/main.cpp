// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT

// Green Curve v0.8 - NVIDIA VF Curve Editor
// Win32 GDI application

#include "app_shared.h"
#include "fan_curve.h"
#include "win32_raii.h"
#include <dbghelp.h>

// Global VEH thread-ID tracking — the VEH only kills the fan runtime thread via
// ExitThread; other threads (pipe server, etc.) also get killed but their
// corresponding watchdogs recreate them.
static DWORD g_fanRuntimeThreadId = 0;

// The pipe handle created by the pipe server thread.  Stored globally so the
// main loop's watchdog can close the orphaned handle when the thread is killed
// by the VEH, before creating a new pipe server thread.
static HANDLE g_servicePipeHandle = INVALID_HANDLE_VALUE;

// Flag set by the VEH when it handles any nvml/nvapi crash.  The snapshot
// handler checks this flag and skips refresh_global_state() (which calls
// NVML directly) to avoid crashing the pipe server thread.  Cleared by
// service_runtime_pulse() after successful NVML recovery.
static volatile LONG g_nvmlVhCrashed = 0;

// Crash back-off state: tick and count of consecutive NVML crashes (set by
// VEH).  service_runtime_pulse() uses these to throttle NVML retries after a
// driver restart that keeps producing stale device handles.
static volatile ULONGLONG g_nvmlCrashTickMs = 0;
static volatile LONG g_nvmlCrashCount = 0;

// Upper bound of the NVML crash recovery window.  After a GPU device reconnect
// / driver restart (e.g. restart64.exe), NVML reads access-violate for a few
// seconds while the GPU kernel driver settles.  During this window, pipe-server
// snapshots, telemetry, and hardware_initialize() avoid issuing NVML reads and
// serve cached data while the recovery thread reloads the driver libraries.
// The time cap guarantees telemetry self-heals even if no fan runtime thread is
// active.
// Reduced from 35s to 15s in build 191 — modern GPU drivers settle within
// ~5-10s after a device reconnect; 15s provides comfortable margin without
// excessive user wait.
#define NVML_CRASH_RECOVERY_WINDOW_MS 15000ULL

// Minimum interval between two consecutive launch_recovery_thread() calls.
// Without this, the wedge watchdog / main-loop monitor can spawn a new
// recovery every ~3 s when the previous recovery wedged, producing a
// visible "recovery loop" in the log.  10 s gives the driver time to
// actually settle before we try again, and is well below the
// NVML_CRASH_RECOVERY_WINDOW_MS so it doesn't suppress the legitimate
// single recovery on a fresh reconnect.
#define SERVICE_RECOVERY_RELAUNCH_INTERVAL_MS 10000ULL

// True while we are inside the NVML crash recovery window: the VEH has flagged
// a stale-handle crash (g_nvmlCrashCount > 0) and the GPU kernel driver has not
// yet had time to settle.  Callers use this to skip NVML reads that would
// access-violate and kill the calling thread during the transitional window.
static bool nvml_crash_recovery_active() {
    LONG count = g_nvmlCrashCount;
    ULONGLONG tick = g_nvmlCrashTickMs;
    if (count <= 0 || tick == 0) return false;
    return (GetTickCount64() - tick) < NVML_CRASH_RECOVERY_WINDOW_MS;
}

// Legacy restart request path retained for service-control fallbacks.  Driver
// reconnect / upgrade recovery is in-process: stale GPU handles are closed,
// libraries are reloaded, and the active desired profile is re-applied without
// restarting the service.
static volatile LONG g_serviceRestartRequested = 0;
static void request_service_restart(const char* reason);

// Declared in cfg_glue.cpp — set process-wide security mitigation policies.
extern "C" void initialize_process_mitigations();

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

// Heartbeat written by the fan runtime thread at the START of every pulse
// attempt (just before any NVML call) and again on completion.  The main-loop
// watchdog uses it to detect a fan thread WEDGED inside nvml.dll on a dead
// driver (a hang the VEH cannot catch) and launch in-process recovery.
static volatile ULONGLONG g_serviceFanPulseHeartbeatMs = 0;
static volatile LONG g_serviceFanPulseInFlight = 0;

// Timestamp of the most recent successful in-process GPU recovery completion,
// or 0 if no recovery has completed.  Read by the snapshot handler to report
// recovery status to the GUI.
static ULONGLONG g_serviceLastRecoveryTickMs = 0;
static volatile LONG g_serviceRecoveryReapplyPending = 0;
static DWORD g_serviceRecoveryReapplyAttempts = 0;
static ULONGLONG g_serviceRecoveryReapplyNextTickMs = 0;

// Reapply thread — runs the recovery reapply on a dedicated thread instead of
// the main service loop.  If VEH kills this thread (NVML/NvAPI crash on a
// still-transitional driver), the main loop survives and can retry.
static HANDLE g_serviceReapplyThread = nullptr;
static volatile DWORD g_serviceReapplyThreadId = 0;

// Set when a reapply is in-flight after recovery and not yet confirmed
// applied to hardware.  Cleared on successful reapply.  The GUI uses this
// flag to show "reapplying..." instead of a misleading "settings active".
static volatile LONG g_serviceReapplyInProgress = 0;

static const int SERVICE_DEBUG_DEFAULT_ENABLED = 1; // Service logs are opt-out via [debug] enabled=0 or GREEN_CURVE_DEBUG=0.
static const DWORD SERVICE_PIPE_CLIENT_CONNECT_SLICE_MS = 250;
static const DWORD SERVICE_PIPE_CLIENT_SLEEP_SLICE_MS = 10;
static const DWORD SERVICE_PIPE_SERVER_IO_TIMEOUT_MS = 2000;
static const DWORD SERVICE_FAN_THREAD_STOP_TIMEOUT_MS = 5000;
static const DWORD SERVICE_FAN_WATCHDOG_INTERVAL_MS = 3000;
// A healthy fan pulse completes in well under a second (a few NVML reads/writes
// plus short verified-readback sleeps).  If one is still in flight after this
// long it is wedged inside nvml.dll on a dead driver; force a clean restart.
static const ULONGLONG SERVICE_FAN_PULSE_WEDGE_TIMEOUT_MS = 12000;
static const DWORD SERVICE_RUNTIME_NOISY_LOG_INTERVAL_MS = 5000;
static const ULONGLONG SERVICE_TELEMETRY_IDLE_REFRESH_INTERVAL_MS = 5000;
static const ULONGLONG SERVICE_TELEMETRY_RUNTIME_STALE_GRACE_MS = 1000;
static const DWORD SERVICE_RECOVERY_REAPPLY_RETRY_INTERVAL_MS = 5000;
static const DWORD SERVICE_RECOVERY_REAPPLY_MAX_ATTEMPTS = 12;
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
static LONG CALLBACK green_curve_vectored_handler(EXCEPTION_POINTERS* info);
static bool service_resolve_active_user_paths_for_startup(const char* context);
static DWORD WINAPI service_fan_runtime_thread_proc(void*);
static DWORD WINAPI service_pipe_server_thread_proc(void*);
static bool ensure_service_fan_runtime_thread();
static void stop_service_fan_runtime_thread();
static void service_runtime_pulse();
static void service_write_restart_reapply_snapshot();
static void service_clear_restart_reapply_snapshot();
static void service_launch_startup_reapply();
static void service_queue_recovery_reapply(const char* reason, DWORD delayMs);
static void service_maybe_launch_recovery_from_main_loop(const char* source);
static void service_maybe_launch_recovery_reapply_thread();
static void service_check_reapply_thread_health();
static void mark_service_telemetry_cache_updated(const char* source);
static bool service_refresh_telemetry_for_request(char* detail, size_t detailSize);
static bool service_apply_desired_settings(const DesiredSettings* desired, bool interactive, char* result, size_t resultSize);
static bool service_reset_all(char* result, size_t resultSize);
// GPU driver-recovery is handled by restarting the service process; record each
// restart for persisted restart-loop protection (defined in main_service_runtime.cpp).
static void service_record_restart_event();
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

// Request a controlled service-process restart for GPU driver recovery.
// This is the single recovery action: in-process NVML/NvAPI reload after a
// device reconnect / driver upgrade is unreliable, so we restart the process
// and let a fresh instance map clean driver DLLs (see launch_recovery_thread).
// Idempotent (first caller wins).  Snapshots the active OC/fan profile so the
// fresh process re-applies it, records the restart for persisted loop
// protection, and signals the main loop to exit non-zero so the SCM failure
// action relaunches us.  Deliberately does NOT touch NVML/NvAPI here (those
// calls can hang on a dead/transitional driver); the NVML-free exit happens in
// the service_main loop's restart branch.
static void request_service_restart(const char* reason) {
    if (InterlockedExchange(&g_serviceRestartRequested, 1) != 0) return;
    debug_log("request_service_restart: %s — exiting for clean driver DLL state, SCM will relaunch\n",
        reason ? reason : "(unspecified)");
    // Persist active OC/fan settings so the fresh process re-applies them.
    service_write_restart_reapply_snapshot();
    // Record this restart so the fresh process can detect a restart loop (a
    // genuinely broken driver that crashes every process) and stop reapplying.
    service_record_restart_event();
    if (g_serviceStopEvent) SetEvent(g_serviceStopEvent);
}
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
static void log_locked_tail_drift_diagnostics();
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
static int desired_curve_point_count(const DesiredSettings* desired);
static bool desired_updates_curve_or_gpu_offset_state(const DesiredSettings* desired);
static bool desired_has_nonfan_apply_fields(const DesiredSettings* desired);
static bool desired_is_fan_only_apply_request(const DesiredSettings* desired);
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
static void close_nvapi();
static const char* nvml_err_name(nvmlReturn_t r);
static bool nvml_ensure_ready();
// Forward declaration for recovery entry point — defined in main_service_runtime.cpp
static void launch_recovery_thread();
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
static void start_service_reconnect_timer_if_needed();
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
static bool detect_live_selective_gpu_offset_state(int* gpuOffsetMHzOut, int* representativeOffsetkHzOut = nullptr, int* detectedExcludeLowCountOut = nullptr);
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
static void repair_profile_locked_curve_readback_artifacts(const char* path, const char* section, int slot, DesiredSettings* desired);
static bool can_save_curve_as_base_plus_gpu_offset(const DesiredSettings* desired, int gpuOffsetMHz, int excludeLowCount);
static int curve_base_khz_for_point(int pointIndex);
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

static_assert(0x48u + (VF_NUM_POINTS - 1u) * 0x1Cu + 4u <= 0x1C28u, "VF status buffer overflow for shared backend layout");
static_assert(0x04u + 32u <= 0x182Cu, "VF info buffer overflow for shared backend layout");
static_assert(0x44u + (VF_NUM_POINTS - 1u) * 0x24u + 4u <= 0x2420u, "VF control buffer overflow for shared backend layout");

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
#include "desired_settings_helpers.cpp"
#include "main_gpu_state.cpp"
#include "main_tail_diagnostics.cpp"
#include "main_fan_runtime.cpp"
#include "config_profile_repair.cpp"
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
    initialize_process_mitigations();
    InitializeCriticalSection(&g_debugLogLock);
    SERVICE_TABLE_ENTRYW table[] = {
        { (LPWSTR)L"GreenCurveService", service_main },
        { nullptr, nullptr },
    };
    StartServiceCtrlDispatcherW(table);
    return 0;
}
#endif
