// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT

#ifndef GREEN_CURVE_APP_SHARED_H
#define GREEN_CURVE_APP_SHARED_H

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0600
#endif
#ifndef _WIN32_IE
#define _WIN32_IE 0x0600
#endif

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <commctrl.h>
#include <setupapi.h>
#include <sddl.h>
#include <shlobj.h>
#include <shellapi.h>
#include <strsafe.h>
#include <wtsapi32.h>
#else
// Linux: opaque stand-ins for the few Win32 types/macros in the shared
// declarations below.  The GPU backend reaches the same nvapi64.dll/NVML logic
// through libnvidia-api.so.1 / libnvidia-ml.so.1; only the UI/service handle
// fields of AppData are unused here.
#include "win32_compat.h"
#endif
#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ARRAY_COUNT(a) (sizeof(a) / sizeof((a)[0]))

// Platform-neutral GPU/IPC data model (NVAPI IDs + structs, NVML typedefs,
// VfBackendSpec, DesiredSettings + IPC validator, ServiceRequest/Response,
// NvmlApi).  Shared verbatim with the Linux backend.
#include "gpu_core.h"
// OS-abstraction shim (dynamic loading, sleep, atomics, threads, bounded
// strings, subprocess capture) used by the shared backend.
#include "platform.h"

// Emit a debug line only when its formatted text changes from the previous call
// AT THIS CALL SITE.  Cuts repetition of high-frequency idempotent state lines
// (pure capability/cache queries logged on every poll) WITHOUT losing
// information: the first occurrence and every subsequent change are still
// logged.  Each call site keeps its own static cache.  Best-effort under
// threads — a benign race can at most emit a duplicate or drop one line, never
// overflow (StringCch* is bounded).  Use ONLY for idempotent state lines, never
// for event/transition logs that must always appear.
#if defined(_WIN32)
#define debug_log_on_change(fmt, ...) \
    do { \
        static char s_dbgOnChangePrev__[256]; \
        char dbgOnChangeCur__[256]; \
        StringCchPrintfA(dbgOnChangeCur__, sizeof(dbgOnChangeCur__), fmt, ##__VA_ARGS__); \
        if (strcmp(dbgOnChangeCur__, s_dbgOnChangePrev__) != 0) { \
            StringCchCopyA(s_dbgOnChangePrev__, sizeof(s_dbgOnChangePrev__), dbgOnChangeCur__); \
            debug_log("%s", dbgOnChangeCur__); \
        } \
    } while (0)
#else
#define debug_log_on_change(fmt, ...) \
    do { \
        static char s_dbgOnChangePrev__[256]; \
        char dbgOnChangeCur__[256]; \
        gc_snprintf(dbgOnChangeCur__, sizeof(dbgOnChangeCur__), fmt, ##__VA_ARGS__); \
        if (strcmp(dbgOnChangeCur__, s_dbgOnChangePrev__) != 0) { \
            gc_strlcpy(s_dbgOnChangePrev__, sizeof(s_dbgOnChangePrev__), dbgOnChangeCur__); \
            debug_log("%s", dbgOnChangeCur__); \
        } \
    } while (0)
#endif

int nvmin(int a, int b);
int nvmax(int a, int b);

extern int g_dpi;
extern float g_scale;
extern CRITICAL_SECTION g_configLock;
extern CRITICAL_SECTION g_appLock;

int dp(int px);
void init_dpi();

// VF_NUM_POINTS + NVAPI entry-point IDs moved to gpu_core.h

#define WINDOW_WIDTH        1180
#define WINDOW_HEIGHT       800
#define GRAPH_HEIGHT        420
#define APP_ICON_ID         101
#define TRAY_ICON_DEFAULT_ID 111
#define TRAY_ICON_OC_ID     112
#define TRAY_ICON_FAN_ID    113
#define TRAY_ICON_OC_FAN_ID 114
#define APP_NAME            "Green Curve"
#ifndef APP_VERSION
#define APP_VERSION         "0.16"
#endif
#ifndef APP_BUILD_NUMBER
#define APP_BUILD_NUMBER    0
#endif
#define APP_TITLE           APP_NAME " v" APP_VERSION
#define APP_CLASS_NAME      "GreenCurveClass"
#define APP_EXE_NAME        "greencurve.exe"
#define APP_SERVICE_EXE_NAME "greencurve-service.exe"
#define APP_SERVICE_EXE_NAME_W L"greencurve-service.exe"
#define APP_LOG_FILE        "greencurve_log.txt"
#define APP_CLI_LOG_FILE    "greencurve_cli_log.txt"
#define APP_DEBUG_LOG_FILE  "greencurve_debug.txt"
#define APP_JSON_FILE       "greencurve_curve.json"
#define APP_DEBUG_ENV       "GREEN_CURVE_DEBUG"
#define APP_DEBUG_DEFAULT_ENABLED 1
#define APP_WM_SYNC_STARTUP (WM_APP + 1)
#define APP_WM_TRAYICON     (WM_APP + 2)
#define APP_WM_SERVICE_STATUS (WM_APP + 3)
#define APP_WM_DEFERRED_RELAUNCH (WM_APP + 4)
#define APPLY_BTN_ID        2000
#define REFRESH_BTN_ID      2001
#define RESET_BTN_ID        2003
#define LICENSE_BTN_ID      2005
#define GPU_OFFSET_EXCLUDE_LOW_LABEL_ID 2006
#define PROFILE_COMBO_ID    2020
#define PROFILE_LOAD_ID     2021
#define PROFILE_SAVE_ID     2022
#define PROFILE_CLEAR_ID    2023
#define APP_LAUNCH_COMBO_ID 2024
#define LOGON_COMBO_ID      2025
#define PROFILE_LABEL_ID    2026
#define PROFILE_STATE_ID    2027
#define APP_LAUNCH_LABEL_ID 2028
#define LOGON_LABEL_ID      2029
#define SHARE_ALL_USERS_CHECK_ID 2041
#define MACHINE_LOGON_MENU_PUBLISH_ID 2042
#define MACHINE_LOGON_MENU_CLEAR_MACHINE_SLOT_ID 2043
#define SHARED_PROFILES_BTN_ID 2044
#define MACHINE_LOGON_MENU_RESTRICT_ID 2045
// Base for the per-slot items in the "Shared profiles" popup menu (BASE+slot).
#define SHARED_PROFILE_MENU_BASE 2400
// CB_SETITEMDATA tag for the unified "Apply profile after user log in:" combo:
// item data 0 = no personal choice (use admin default if any), 1..CONFIG_NUM_SLOTS
// = per-user logon_slot, (LOGON_COMBO_SHARED_FLAG | N) = admin shared bank slot N
// (logon_shared_slot).  The flag sits above the slot range so there is no overlap.
#define LOGON_COMBO_SHARED_FLAG 0x100
#define PROFILE_STATUS_ID   2030
#define START_ON_LOGON_CHECK_ID 2031
#define FAN_MODE_COMBO_ID   2032
#define FAN_CURVE_BTN_ID    2033
#define GPU_OFFSET_EXCLUDE_LOW_EDIT_ID 2034
#define LOGON_HINT_ID       2035
#define START_ON_LOGON_LABEL_ID 2036
#define SERVICE_ENABLE_CHECK_ID 2037
#define SERVICE_ENABLE_LABEL_ID 2038
#define SERVICE_STATUS_ID   2039
#define GPU_SELECT_COMBO_ID 2040
#define LOCK_BASE_ID        3000
#define GPU_OFFSET_ID       2010
#define MEM_OFFSET_ID       2011
#define POWER_LIMIT_ID      2012
#define FAN_CONTROL_ID      2013
#define TRAY_MENU_SHOW_ID   2100
#define TRAY_MENU_EXIT_ID   2101
#define LOCK_CTX_NONE_ID    2110
#define LOCK_CTX_FLATTEN_ID 2111
#define LOCK_CTX_PIN_ID     2112

#define FAN_CURVE_TIMER_ID  1
#define FAN_TELEMETRY_TIMER_ID 2
#define SERVICE_RECONNECT_TIMER_ID 3
// FAN_CURVE_MAX_POINTS, FAN_CURVE_MAX_HYSTERESIS_C, MAX_GPU_FANS,
// MAX_GPU_ADAPTERS moved to gpu_core.h
#define CONFIG_FILE_NAME    "config.ini"
// Machine-wide shared profile bank + all-users default logon assignment.
// Lives under %ProgramData%\Green Curve (a fixed, all-users-readable,
// admin-write known folder) — see resolve_machine_config_path().
#define MACHINE_CONFIG_FILE_NAME "shared-profiles.ini"
// Legacy name/location: older builds stored the bank as "machine.ini" next to
// the installed service binary. migrate_legacy_machine_config() copies it into
// the %ProgramData% location on first elevated service start, then removes it.
#define LEGACY_MACHINE_CONFIG_FILE_NAME "machine.ini"
#define STARTUP_TASK_PREFIX "Green Curve Startup - "
// CONFIG_NUM_SLOTS, CONFIG_DEFAULT_SLOT, NVML_PERF_STR_LEN,
// MIN_VISIBLE_VOLT_mV, MIN_VISIBLE_FREQ_MHz moved to gpu_core.h

#define COL_BG              RGB(0x18, 0x18, 0x28)
#define COL_PANEL           RGB(0x18, 0x18, 0x28)
#define COL_INPUT           RGB(0x12, 0x12, 0x1C)
#define COL_GRID            RGB(0x40, 0x40, 0x55)
#define COL_AXIS            RGB(0x80, 0x80, 0x90)
#define COL_CURVE           RGB(0x50, 0xD0, 0x80)
#define COL_POINT           RGB(0xFF, 0x60, 0x60)
#define COL_TEXT            RGB(0xE0, 0xE0, 0xE0)
#define COL_LABEL           RGB(0xA0, 0xA0, 0xB0)
#define COL_BUTTON          RGB(0x2B, 0x42, 0x66)
#define COL_BUTTON_PRESSED  RGB(0x23, 0x36, 0x52)
#define COL_BUTTON_BORDER   RGB(0x78, 0x9A, 0xD8)
#define COL_BUTTON_DISABLED RGB(0x2A, 0x2A, 0x38)

// LockMode, NVML/NVAPI types+enums, VfBackendSpec, VFCurvePoint, fan structs,
// ControlState, GpuAdapterInfo moved to gpu_core.h
struct AppData {
    HINSTANCE hInst;
    HWND hMainWnd;
    HWND hEditsMhz[VF_NUM_POINTS];
    HWND hEditsMv[VF_NUM_POINTS];
    HWND hLocks[VF_NUM_POINTS];
    HWND hLockTooltip;
    HWND hApplyBtn;
    HWND hRefreshBtn;
    HWND hResetBtn;
    HWND hLicenseBtn;
    HWND hGpuOffsetEdit;
    HWND hGpuOffsetExcludeLowEdit;
    HWND hGpuOffsetExcludeLowLabel;
    HWND hMemOffsetEdit;
    HWND hPowerLimitEdit;
    HWND hFanEdit;
    HWND hFanModeCombo;
    HWND hFanCurveBtn;
    HWND hProfileCombo;
    HWND hProfileLoadBtn;
    HWND hProfileSaveBtn;
    HWND hProfileClearBtn;
    HWND hProfileLabel;
    HWND hProfileStateLabel;
    HWND hAppLaunchCombo;
    HWND hLogonCombo;
    HWND hShareAllUsersCheck;
    HWND hSharedProfilesBtn;
    HWND hAppLaunchLabel;
    HWND hLogonLabel;
    HWND hProfileStatusLabel;
    HWND hStartOnLogonCheck;
    HWND hStartOnLogonLabel;
    HWND hLogonHintLabel;
    HWND hServiceEnableCheck;
    int machineLogonSlotCache;
    // Shared-only policy (admin restricts non-admins to admin-published profiles).
    // restrictPolicyActive: the machine policy flag (cached from the shared bank).
    // currentUserIsLocalAdmin: whether THIS user is a machine admin (even unelevated).
    // loadedSharedSlot: the shared bank slot currently loaded into the editor
    //   (0 = none/custom); a clean apply of it is sent as an authoritative
    //   "apply shared slot N" so the service applies its own copy under policy.
    bool restrictPolicyActive;
    bool currentUserIsLocalAdmin;
    int loadedSharedSlot;
    HWND hServiceEnableLabel;
    HWND hServiceStatusLabel;
    HWND hGpuSelectCombo;

    HBRUSH hWindowClassBrush;
    HANDLE hStartupSyncThread;
    bool startupSyncInFlight;
    bool applyInFlight;
    HDC hMemDC;
    HBITMAP hMemBmp;
    HBITMAP hOldBmp;

    // Cached GDI objects for graph rendering
    HPEN hCachedGridPen;
    HPEN hCachedAxisPen;
    HFONT hCachedFont;
    HFONT hCachedFontSmall;

    HMODULE hNvApi;
    GPU_HANDLE gpuHandle;
    unsigned int selectedGpuIndex;
    unsigned int selectedNvmlIndex;
    bool selectedGpuExplicit;
    bool selectedGpuIdentityValid;
    bool selectedGpuOrdinalFallback;
    GpuAdapterInfo adapters[MAX_GPU_ADAPTERS];
    unsigned int adapterCount;
    GpuAdapterInfo selectedGpu;
    char gpuName[256];
    char configPath[MAX_PATH];
    unsigned int gpuArchitecture;
    unsigned int gpuImplementation;
    unsigned int gpuChipRevision;
    unsigned int gpuDeviceId;
    unsigned int gpuSubSystemId;
    unsigned int gpuPciRevisionId;
    unsigned int gpuExtDeviceId;
    bool gpuArchInfoValid;
    bool gpuPciInfoValid;
    GpuFamily gpuFamily;
    const VfBackendSpec* vfBackend;

    bool nvmlReady;
    nvmlDevice_t nvmlDevice;

    VFCurvePoint curve[VF_NUM_POINTS];
    int freqOffsets[VF_NUM_POINTS];
    int populatedOrdinal[VF_NUM_POINTS]; // ordinal index (0-based) per populated point, -1 for unpopulated
    int numPopulated;
    bool loaded;

    int visibleMap[VF_NUM_POINTS];
    int numVisible;

    int lockedVi;
    int lockedCi;
    unsigned int lockedFreq;
    LockMode lockMode;
    bool guiLockTracksAnchor;

    int gpuClockOffsetkHz;
    int memClockOffsetkHz;
    int gpuClockOffsetMinMHz;
    int gpuClockOffsetMaxMHz;
    int memClockOffsetMinMHz;
    int memClockOffsetMaxMHz;
    int curveOffsetMinkHz;
    int curveOffsetMaxkHz;
    int offsetReadPstate;
    bool gpuOffsetRangeKnown;
    bool memOffsetRangeKnown;
    bool curveOffsetRangeKnown;
    int pstateGpuOffsetkHz;
    unsigned int pstateMemMaxMHz;
    int powerLimitPct;
    int powerLimitDefaultmW;
    int powerLimitCurrentmW;
    int powerLimitMinmW;
    int powerLimitMaxmW;

    bool smiClocksRead;
    unsigned int smiMemMaxMHz;

    bool vfInfoCached;
    unsigned int vfNumClocks;
    unsigned char vfMask[32];

    bool fanSupported;
    bool fanRangeKnown;
    bool fanIsAuto;
    unsigned int fanCount;
    unsigned int fanMinPct;
    unsigned int fanMaxPct;
    unsigned int fanPercent[MAX_GPU_FANS];
    unsigned int fanTargetPercent[MAX_GPU_FANS];
    unsigned int fanRpm[MAX_GPU_FANS];
    unsigned int fanPolicy[MAX_GPU_FANS];
    unsigned int fanControlSignal[MAX_GPU_FANS];
    unsigned int fanTargetMask[MAX_GPU_FANS];

    int gpuTemperatureC;
    bool gpuTemperatureValid;

    bool guiCurvePointExplicit[VF_NUM_POINTS];
    bool guiStateDirty;
    bool guiHasUserModifiedValues;
    int guiGpuOffsetMHz;
    int guiGpuOffsetExcludeLowCount;
    int appliedGpuOffsetMHz;
    int appliedGpuOffsetExcludeLowCount;
    bool lastApplyUsedGpuOffset;
    int appliedLockVi;
    int appliedLockCi;
    unsigned int appliedLockFreq;
    LockMode appliedLockMode;

    int guiFanMode;
    int guiFanFixedPercent;
    FanCurveConfig guiFanCurve;

    int activeFanMode;
    int activeFanFixedPercent;
    FanCurveConfig activeFanCurve;
    bool fanCurveRuntimeActive;
    bool fanFixedRuntimeActive;
    int fanCurveLastAppliedPercent;
    int fanCurveLastAppliedTempC;
    bool fanCurveHasLastAppliedTemp;
    unsigned int fanRuntimeConsecutiveFailures;
    ULONGLONG fanRuntimeLastApplyTickMs;

    // --- Driver upgrade resilience ---
    bool deviceRemoved;               // GPU display adapter removed (driver uninstall in progress)
    bool pendingDeviceRecovery;       // Service has detected device removal; run recovery after arrival
    unsigned long long deviceRemoveTimeMs; // When device was last removed (tick count)

    bool launchedFromLogon;
    bool startHiddenToTray;
    bool isServiceProcess;
    bool usingBackgroundService;
    bool backgroundServiceInstalled;
    bool backgroundServiceRunning;
    bool backgroundServiceAvailable;
    bool backgroundServiceBroken;
    char backgroundServiceError[256];
    bool serviceSnapshotAuthoritative;
    bool serviceControlStateValid;
    ControlState serviceControlState;
    bool backgroundServiceToggleInFlight;
    bool backgroundServiceToggleTargetEnabled;
    bool backgroundServicePendingRelaunch;
    bool trayIconAdded;
    int trayIconState;
    HICON trayIcons[4];
    bool trayProfileCacheValid;
    bool trayLastRenderedValid;
    int trayLastRenderedState;
    char trayProfileCacheProfilePart[64];
    char trayLastRenderedTip[128];
    char backgroundServiceOwnerUser[256];
    DWORD backgroundServiceOwnerSessionId;
    ULONGLONG backgroundServiceOwnerUtcMs;
};

// DesiredSettings, validate_desired_settings_for_ipc(),
// lock_mode_sync_allowed() moved to gpu_core.h
struct CliOptions {
    bool recognized;
    bool showHelp;
    bool dump;
    bool json;
    bool probe;
    bool hasProbeOutputPath;
    bool reset;
    bool saveConfig;
    bool applyConfig;
    bool logonStart;
    bool serviceInstall;
    bool serviceRemove;
    bool startupTaskEnable;
    bool startupTaskDisable;
    bool setMachineLogonSlot;
    bool clearMachineLogonSlot;
    int machineLogonSlotValue;
    bool publishSlotToMachine;
    bool clearMachineSlot;
    int machineSlotValue;
    bool shareSlot;
    bool unshareSlot;
    int shareSlotValue;
    bool setRestrictPolicy;
    int restrictPolicyValue;
    bool hasConfigPath;
    char configPath[MAX_PATH];
    char probeOutputPath[MAX_PATH];
    char error[256];
    DesiredSettings desired;
};

// Service protocol (magic/version/commands), ServiceSnapshot,
// ServiceRequest/Response, NVML typedefs + NvmlApi moved to gpu_core.h
extern AppData g_app;
extern NvmlApi g_nvml_api;
extern HMODULE g_nvml;
extern bool g_debug_logging;
// F-DOM-1: the unrecognized-GPU best-effort VF warning re-shows once per GUI
// session unless the user disables it persistently; set once the warning has
// been acknowledged in this process.
extern bool g_bestGuessWarningShownThisSession;
// F-REL-2e: set when a telemetry poll detects the service reset to defaults and
// clears a stale adopted GUI lock — requests the next poll to do the same full
// visual resync the Refresh button does (the poll otherwise skips it so it never
// wipes in-progress edits).
extern bool g_guiForceFullRefresh;

// In-process GPU recovery flags — set/read atomically across threads.
// g_serviceGpuRecovering: 1 while recovery thread is active (close stale
//   handles, re-init NVML/NvAPI, reapply settings).  Pipe server serves
//   cached data.  nvml_ensure_ready returns false.
// g_nvapiRecoveryInProgress: 1 while NvAPI handles are being closed and
//   reloaded during recovery.  All NvAPI call sites check this and
//   return early.
// g_serviceInitInProgress: 1 while the recovery thread is performing the
//   in-process NVML/NvAPI re-init (Phase C).  nvml_ensure_ready() and
//   nvapi_qi() check this flag and bypass their normal "crash recovery
//   in progress, return not-ready" early-return so the re-init can run.
//   Critically, the broader crash-recovery safety guard
//   (g_nvmlCrashCount / g_nvmlCrashTickMs) stays SET throughout the
//   recovery, so hardware_initialize() correctly skips
//   refresh_global_state() (the dangerous NVML reads on a still-
//   transitional driver).  Cleared in Phase E after the reapply succeeds.
extern volatile LONG g_serviceGpuRecovering;
extern volatile LONG g_nvapiRecoveryInProgress;
extern volatile LONG g_serviceInitInProgress;

// Recovery thread TID + handle.  Set by launch_recovery_thread() after
// CreateThread succeeds; cleared by service_recovery_thread_proc() on
// normal return.  Used by:
//   - green_curve_vectored_handler (main_diagnostics.cpp) to detect
//     when the VEH is killing the recovery thread mid-apply and clear
//     the three stuck recovery flags above.
//   - launch_recovery_thread (main_service_runtime.cpp) to defensively
//     detect a dead previous recovery thread (e.g. one that was killed
//     by a non-nvml/nvapi crash that the VEH did not handle) and clear
//     the stuck flags before launching a new one.
// The handle is NOT CloseHandle'd by launch_recovery_thread; it is
// closed lazily by either the VEH-side cleanup (RC5a) or the
// launch-side cleanup (RC5b) on the next launch.  This is the same
// pattern as g_fanRuntimeThreadId (main.cpp:15).
extern volatile DWORD g_serviceRecoveryThreadId;
extern volatile HANDLE g_serviceRecoveryThreadHandle;

// Timestamp of the most recent launch_recovery_thread() call (any caller).
// Used by the wedge watchdog, main-loop monitor, and fan-runtime pulse
// to bound how often a new recovery can be spawned — without this, a
// wedged recovery produces a tight ~3 s loop of redundant recoveries
// that all wedge in the same place.  See
// SERVICE_RECOVERY_RELAUNCH_INTERVAL_MS in source/main.cpp.
extern volatile ULONGLONG g_serviceLastRecoveryAttemptMs;

typedef int (*NvApiFunc)(void*, void*);

struct HeapBuffer {
    void* ptr;
    size_t bufSize;
    HeapBuffer(size_t size) : ptr(calloc(1, size)), bufSize(size) { (void)(ptr != nullptr || size == 0); }
    ~HeapBuffer() { free(ptr); }
    HeapBuffer(const HeapBuffer&) = delete;
    HeapBuffer& operator=(const HeapBuffer&) = delete;
    operator unsigned char*() const { return (unsigned char*)ptr; }
    operator bool() const { return ptr != nullptr; }
    size_t size() const { return bufSize; }
    bool write_at(size_t offset, const void* data, size_t len) const {
        if (!ptr || !data || offset > bufSize || len > bufSize - offset) return false;
        memcpy((unsigned char*)ptr + offset, data, len);
        return true;
    }
    bool read_at(size_t offset, void* data, size_t len) const {
        if (!ptr || !data || offset > bufSize || len > bufSize - offset) return false;
        memcpy(data, (unsigned char*)ptr + offset, len);
        return true;
    }
};

void trim_ascii(char* s);
bool streqi_ascii(const char* a, const char* b);
bool parse_int_strict(const char* s, int* out);
bool parse_cli_point_arg_w(const WCHAR* arg, int* pointIndexOut);
bool gpu_family_uses_best_guess_backend(GpuFamily family);
void set_message(char* dst, size_t dstSize, const char* fmt, ...);
bool parse_fan_value(const char* text, bool* isAuto, int* pct);
bool enter_config_storage_lock(HANDLE* acquiredMutex);
void leave_config_storage_lock(HANDLE acquiredMutex);
bool config_section_has_keys(const char* path, const char* section);
int get_config_int(const char* path, const char* section, const char* key, int defaultVal);
bool set_config_int(const char* path, const char* section, const char* key, int value);
void invalidate_tray_profile_cache();

// Machine-wide default logon profile (admin-configured, applies to all users).
bool resolve_machine_config_path(char* out, size_t outSize);
bool get_machine_logon_slot(int* slotOut);
bool set_machine_logon_slot(int slot, char* err, size_t errSize);
bool clear_machine_logon_slot(char* err, size_t errSize);
bool is_machine_profile_slot_saved(int slot);
bool copy_profile_slot_to_machine_config(const char* srcPath, int slot, char* err, size_t errSize);
bool clear_machine_profile_slot(int slot, char* err, size_t errSize);

// One coherent "share with all users" operation: publish slot data into the
// shared bank AND set it as the all-users default logon profile. unshare
// reverses both (clearing the default only when it points at this slot). Both
// require elevation. These back the GUI "Share with all users" checkbox.
bool share_profile_slot_for_all_users(const char* srcPath, int slot, char* err, size_t errSize);
bool unshare_profile_slot_for_all_users(int slot, char* err, size_t errSize);

// Shared-only policy: when enabled, the service only honors APPLY from a
// non-admin caller when it names a shared bank slot (the service applies its own
// copy). Stored in the protected shared bank; admin-only write. Read is open.
bool get_machine_restrict_policy(bool* enabledOut);
bool set_machine_restrict_policy(bool enable, char* err, size_t errSize);
// True if the current process's user is a member of the local Administrators
// group, even when running unelevated (UAC-filtered tokens carry it deny-only).
bool current_user_is_local_admin();

// What a session's logon auto-apply should resolve to.  Decoupled from the I/O
// so the policy decision is pure and unit-testable (see
// resolve_logon_profile_source).  All logon paths (client + service) share it.
enum LogonProfileSource {
    LOGON_PROFILE_SOURCE_NONE = 0,       // nothing to apply
    LOGON_PROFILE_SOURCE_SHARED_BANK,    // user-chosen logon_shared_slot (authoritative bank copy)
    LOGON_PROFILE_SOURCE_PER_USER,       // per-user logon_slot content
    LOGON_PROFILE_SOURCE_MACHINE_DEFAULT // machine-wide default logon profile (authoritative bank copy)
};

// Pure policy decision for logon auto-apply under restrict_non_admin_to_shared.
//   policyActive    : the shared-only machine policy is enabled.
//   isAdmin         : the target user is a machine administrator.
//   logonSharedSlot : per-user "apply admin shared bank slot N at my logon" (0 = unset).
//   bankSlotSaved   : logonSharedSlot names a currently-published bank slot.
//   hasPerUserSlot  : a saved per-user logon_slot profile is available.
//   hasMachineDefault: a published machine-wide default logon profile is available.
// A user's explicit shared-bank choice always wins (it is authoritative and
// policy-safe).  A restricted (policy && !admin) user otherwise gets ONLY the
// machine-wide shared default — never their own per-user custom OC — which both
// fixes auto-apply for restricted users and closes the service-router bypass.
LogonProfileSource resolve_logon_profile_source(bool policyActive, bool isAdmin,
    int logonSharedSlot, bool bankSlotSaved, bool hasPerUserSlot, bool hasMachineDefault);

// True if the SCM-registered service binary directory is located under a user
// profile, which means other users may be unable to launch the GUI binary.
bool service_install_dir_is_under_user_profile();

// True if the RUNNING process's own binary directory is located under a user
// profile.  Fires pre-install / in portable use (when there is no SCM service
// dir yet), covering the case where a restricted user cannot even execute the
// GUI binary because it lives inside another user's profile.
bool running_exe_dir_is_under_user_profile();

#endif
