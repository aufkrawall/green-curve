// Green Curve v0.1 - NVIDIA Blackwell VF Curve Editor
// Single-file Win32 GDI application
// Compile: zig c++ -O ReleaseSmall -fstrip -mwindows -o greencurve.exe main.cpp -luser32 -lgdi32 -ladvapi32 -lshell32

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

static int nvmin(int a, int b) { return a < b ? a : b; }
static int nvmax(int a, int b) { return a > b ? a : b; }
#define ARRAY_COUNT(a) (sizeof(a) / sizeof((a)[0]))
#include <shellapi.h>
#include <strsafe.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <ctype.h>
#include <stdarg.h>

// ============================================================================
// DPI Scaling
// ============================================================================

static int g_dpi = 96;
static float g_scale = 1.0f;

static int dp(int px) { return (int)((float)px * g_scale); }

static void init_dpi() {
    // Try GetDpiForWindow (Windows 10 1607+)
    HMODULE shcore = LoadLibraryA("shcore.dll");
    if (shcore) {
        typedef HRESULT (WINAPI *GetDpiForMonitor_t)(HANDLE, int, UINT*, UINT*);
        auto pGetDpiForMonitor = (GetDpiForMonitor_t)GetProcAddress(shcore, "GetDpiForMonitor");
        if (pGetDpiForMonitor) {
            UINT dpiX = 96, dpiY = 96;
            if (pGetDpiForMonitor(nullptr /*MONITOR_DEFAULTTOPRIMARY*/, 0, &dpiX, &dpiY) == 0) {
                g_dpi = (int)dpiX;
            }
        }
        FreeLibrary(shcore);
    }
    if (g_dpi == 96) {
        HDC hdc = GetDC(nullptr);
        g_dpi = GetDeviceCaps(hdc, LOGPIXELSX);
        ReleaseDC(nullptr, hdc);
    }
    g_scale = (float)g_dpi / 96.0f;
}

// ============================================================================
// Constants
// ============================================================================

#define VF_NUM_POINTS       128
#define VF_ENTRY_STRIDE     0x1C
#define VF_BUFFER_SIZE      0x1C28
#define VF_ENTRIES_OFFSET   0x48

#define VF_GET_STATUS_ID    0x21537AD4u
#define VF_GET_INFO_ID      0x507B4B59u
#define VF_GET_CONTROL_ID   0x23F1B133u
#define VF_SET_CONTROL_ID   0x0733E009u
#define NVAPI_INIT_ID       0x0150E828u
#define NVAPI_ENUM_GPU_ID   0xE5AC921Fu
#define NVAPI_GET_NAME_ID   0xCEEE8E9Fu

#define WINDOW_WIDTH        1180
#define WINDOW_HEIGHT       800
#define GRAPH_HEIGHT        420
#define APP_ICON_ID         101
#define APP_NAME            "Green Curve"
#define APP_VERSION         "0.1"
#define APP_TITLE           APP_NAME " v" APP_VERSION
#define APP_CLASS_NAME      "GreenCurveClass"
#define APP_EXE_NAME        "greencurve.exe"
#define APP_LOG_FILE        "greencurve_log.txt"
#define APP_CLI_LOG_FILE    "greencurve_cli_log.txt"
#define APP_JSON_FILE       "greencurve_curve.json"
#define APP_DEBUG_ENV       "GREEN_CURVE_DEBUG"
#define APPLY_BTN_ID        2000
#define REFRESH_BTN_ID      2001
#define RESET_BTN_ID        2003
#define SAVE_CFG_BTN_ID     2004
#define LICENSE_BTN_ID      2005
#define STARTUP_APPLY_ID    2006
#define LOCK_BASE_ID        3000  // lock checkboxes: 3000+vi
#define GPU_OFFSET_ID       2010
#define MEM_OFFSET_ID       2011
#define POWER_LIMIT_ID      2012
#define FAN_CONTROL_ID      2013

#define MAX_GPU_FANS        8
#define CONFIG_FILE_NAME    "config.ini"
#define STARTUP_TASK_PREFIX "Green Curve Startup - "
#define CONFIG_STARTUP_PRESERVE (-1)
#define CONFIG_STARTUP_DISABLE   0
#define CONFIG_STARTUP_ENABLE    1
#define NVML_PERF_STR_LEN   2048

#define MIN_VISIBLE_VOLT_mV 700
#define MIN_VISIBLE_FREQ_MHz 500

#define COL_BG              RGB(0x1E, 0x1E, 0x2E)
#define COL_GRID            RGB(0x40, 0x40, 0x55)
#define COL_AXIS            RGB(0x80, 0x80, 0x90)
#define COL_CURVE           RGB(0x40, 0xA0, 0xFF)
#define COL_POINT           RGB(0xFF, 0x60, 0x60)
#define COL_TEXT            RGB(0xE0, 0xE0, 0xE0)
#define COL_LABEL           RGB(0xA0, 0xA0, 0xB0)

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

// ============================================================================
// Types
// ============================================================================

typedef void* GPU_HANDLE;

typedef void* nvmlDevice_t;
typedef int nvmlReturn_t;

enum {
    NVML_SUCCESS = 0,
    NVML_ERROR_UNINITIALIZED = 1,
    NVML_ERROR_INVALID_ARGUMENT = 2,
    NVML_ERROR_NOT_SUPPORTED = 3,
    NVML_ERROR_NO_PERMISSION = 4,
    NVML_ERROR_ALREADY_INITIALIZED = 5,
    NVML_ERROR_NOT_FOUND = 6,
    NVML_ERROR_INSUFFICIENT_SIZE = 7,
    NVML_ERROR_FUNCTION_NOT_FOUND = 13,
    NVML_ERROR_GPU_IS_LOST = 15,
    NVML_ERROR_ARG_VERSION_MISMATCH = 25,
    NVML_ERROR_UNKNOWN = 999,
};

enum {
    NVML_CLOCK_GRAPHICS = 0,
    NVML_CLOCK_SM = 1,
    NVML_CLOCK_MEM = 2,
    NVML_CLOCK_VIDEO = 3,
};

enum {
    NVML_PSTATE_0 = 0,
    NVML_PSTATE_1 = 1,
    NVML_PSTATE_2 = 2,
    NVML_PSTATE_3 = 3,
    NVML_PSTATE_4 = 4,
    NVML_PSTATE_5 = 5,
    NVML_PSTATE_6 = 6,
    NVML_PSTATE_7 = 7,
    NVML_PSTATE_8 = 8,
    NVML_PSTATE_9 = 9,
    NVML_PSTATE_10 = 10,
    NVML_PSTATE_11 = 11,
    NVML_PSTATE_12 = 12,
    NVML_PSTATE_13 = 13,
    NVML_PSTATE_14 = 14,
    NVML_PSTATE_15 = 15,
    NVML_PSTATE_UNKNOWN = 32,
};

#define NVML_FAN_POLICY_TEMPERATURE_CONTINOUS_SW 0
#define NVML_FAN_POLICY_MANUAL 1

#define NVML_THERMAL_COOLER_SIGNAL_NONE 0
#define NVML_THERMAL_COOLER_SIGNAL_TOGGLE 1
#define NVML_THERMAL_COOLER_SIGNAL_VARIABLE 2

#define NVML_THERMAL_COOLER_TARGET_NONE (1 << 0)
#define NVML_THERMAL_COOLER_TARGET_GPU (1 << 1)
#define NVML_THERMAL_COOLER_TARGET_MEMORY (1 << 2)
#define NVML_THERMAL_COOLER_TARGET_POWER_SUPPLY (1 << 3)

#define NVML_STRUCT_VERSION(data, ver) (unsigned int)(sizeof(nvml##data##_v##ver##_t) | ((ver) << 24U))

typedef struct {
    unsigned int version;
    unsigned int type;
    unsigned int pstate;
    int clockOffsetMHz;
    int minClockOffsetMHz;
    int maxClockOffsetMHz;
} nvmlClockOffset_v1_t;
typedef nvmlClockOffset_v1_t nvmlClockOffset_t;
#define nvmlClockOffset_v1 NVML_STRUCT_VERSION(ClockOffset, 1)

typedef struct {
    unsigned int version;
    unsigned int fan;
    unsigned int speed;
} nvmlFanSpeedInfo_v1_t;
typedef nvmlFanSpeedInfo_v1_t nvmlFanSpeedInfo_t;
#define nvmlFanSpeedInfo_v1 NVML_STRUCT_VERSION(FanSpeedInfo, 1)

typedef unsigned int nvmlFanControlPolicy_t;

typedef struct {
    unsigned int version;
    unsigned int index;
    unsigned int signalType;
    unsigned int target;
} nvmlCoolerInfo_v1_t;
typedef nvmlCoolerInfo_v1_t nvmlCoolerInfo_t;
#define nvmlCoolerInfo_v1 NVML_STRUCT_VERSION(CoolerInfo, 1)

struct VFCurvePoint {
    unsigned int freq_kHz;
    unsigned int volt_uV;
};

struct AppData {
    HINSTANCE hInst;
    HWND hMainWnd;
    HWND hEditsMhz[VF_NUM_POINTS];
    HWND hEditsMv[VF_NUM_POINTS];
    HWND hLocks[VF_NUM_POINTS];  // lock checkboxes
    HWND hApplyBtn;
    HWND hRefreshBtn;
    HWND hResetBtn;
    HWND hLicenseBtn;
    HWND hStartupApplyCheck;
    // OC/PL edit fields
    HWND hGpuOffsetEdit;
    HWND hMemOffsetEdit;
    HWND hPowerLimitEdit;
    HWND hFanEdit;
    HWND hSaveCfgBtn;
    HDC hMemDC;
    HBITMAP hMemBmp;
    HBITMAP hOldBmp;

    // NvAPI
    HMODULE hNvApi;
    GPU_HANDLE gpuHandle;
    char gpuName[256];
    char configPath[MAX_PATH];

    // NVML
    bool nvmlReady;
    nvmlDevice_t nvmlDevice;

    // Curve data
    VFCurvePoint curve[VF_NUM_POINTS];
    int freqOffsets[VF_NUM_POINTS];
    int numPopulated;
    bool loaded;

    // Visible point mapping (only points in graph range)
    int visibleMap[VF_NUM_POINTS];  // visibleMap[i] = curve index of i-th visible point
    int numVisible;

    // Lock state
    int lockedVi;            // locked visible index (-1 = none)
    unsigned int lockedFreq; // MHz of locked point

    // Global OC/PL values (from driver)
    int gpuClockOffsetkHz;   // global GPU clock offset
    int memClockOffsetkHz;   // VRAM clock offset
    int gpuClockOffsetMinMHz;
    int gpuClockOffsetMaxMHz;
    int memClockOffsetMinMHz;
    int memClockOffsetMaxMHz;
    int offsetReadPstate;
    bool gpuOffsetRangeKnown;
    bool memOffsetRangeKnown;
    int powerLimitPct;       // power limit percentage (100 = default)
    int powerLimitDefaultmW; // default power limit in mW
    int powerLimitCurrentmW; // current power limit in mW
    int powerLimitMinmW;     // min power limit
    int powerLimitMaxmW;     // max power limit

    // Fan state
    bool fanSupported;
    bool fanRangeKnown;
    bool fanIsAuto;
    unsigned int fanCount;
    unsigned int fanMinPct;
    unsigned int fanMaxPct;
    unsigned int fanPercent[MAX_GPU_FANS];
    unsigned int fanRpm[MAX_GPU_FANS];
    unsigned int fanPolicy[MAX_GPU_FANS];
    unsigned int fanControlSignal[MAX_GPU_FANS];
    unsigned int fanTargetMask[MAX_GPU_FANS];
};

struct DesiredSettings {
    bool hasCurvePoint[VF_NUM_POINTS];
    unsigned int curvePointMHz[VF_NUM_POINTS];
    bool hasGpuOffset;
    int gpuOffsetMHz;
    bool hasMemOffset;
    int memOffsetMHz;
    bool hasPowerLimit;
    int powerLimitPct;
    bool hasFan;
    bool fanAuto;
    int fanPercent;
};

struct CliOptions {
    bool recognized;
    bool showHelp;
    bool dump;
    bool json;
    bool probe;
    bool reset;
    bool saveConfig;
    bool applyConfig;
    bool hasConfigPath;
    char configPath[MAX_PATH];
    char error[256];
    DesiredSettings desired;
};

static AppData g_app = {};

// ============================================================================
// Helpers / Config / NVML
// ============================================================================

typedef nvmlReturn_t (*nvmlInit_v2_t)();
typedef nvmlReturn_t (*nvmlShutdown_t)();
typedef nvmlReturn_t (*nvmlDeviceGetHandleByIndex_v2_t)(unsigned int, nvmlDevice_t*);
typedef nvmlReturn_t (*nvmlDeviceGetPowerManagementLimit_t)(nvmlDevice_t, unsigned int*);
typedef nvmlReturn_t (*nvmlDeviceGetPowerManagementDefaultLimit_t)(nvmlDevice_t, unsigned int*);
typedef nvmlReturn_t (*nvmlDeviceGetPowerManagementLimitConstraints_t)(nvmlDevice_t, unsigned int*, unsigned int*);
typedef nvmlReturn_t (*nvmlDeviceSetPowerManagementLimit_t)(nvmlDevice_t, unsigned int);
typedef nvmlReturn_t (*nvmlDeviceGetClockOffsets_t)(nvmlDevice_t, nvmlClockOffset_t*);
typedef nvmlReturn_t (*nvmlDeviceSetClockOffsets_t)(nvmlDevice_t, nvmlClockOffset_t*);
typedef nvmlReturn_t (*nvmlDeviceGetPerformanceState_t)(nvmlDevice_t, unsigned int*);
typedef nvmlReturn_t (*nvmlDeviceGetGpcClkVfOffset_t)(nvmlDevice_t, int*);
typedef nvmlReturn_t (*nvmlDeviceGetMemClkVfOffset_t)(nvmlDevice_t, int*);
typedef nvmlReturn_t (*nvmlDeviceGetGpcClkMinMaxVfOffset_t)(nvmlDevice_t, int*, int*);
typedef nvmlReturn_t (*nvmlDeviceGetMemClkMinMaxVfOffset_t)(nvmlDevice_t, int*, int*);
typedef nvmlReturn_t (*nvmlDeviceSetGpcClkVfOffset_t)(nvmlDevice_t, int);
typedef nvmlReturn_t (*nvmlDeviceSetMemClkVfOffset_t)(nvmlDevice_t, int);
typedef nvmlReturn_t (*nvmlDeviceGetNumFans_t)(nvmlDevice_t, unsigned int*);
typedef nvmlReturn_t (*nvmlDeviceGetMinMaxFanSpeed_t)(nvmlDevice_t, unsigned int*, unsigned int*);
typedef nvmlReturn_t (*nvmlDeviceGetFanControlPolicy_v2_t)(nvmlDevice_t, unsigned int, unsigned int*);
typedef nvmlReturn_t (*nvmlDeviceSetFanControlPolicy_t)(nvmlDevice_t, unsigned int, unsigned int);
typedef nvmlReturn_t (*nvmlDeviceGetFanSpeed_v2_t)(nvmlDevice_t, unsigned int, unsigned int*);
typedef nvmlReturn_t (*nvmlDeviceGetTargetFanSpeed_t)(nvmlDevice_t, unsigned int, unsigned int*);
typedef nvmlReturn_t (*nvmlDeviceGetFanSpeedRPM_t)(nvmlDevice_t, nvmlFanSpeedInfo_t*);
typedef nvmlReturn_t (*nvmlDeviceSetFanSpeed_v2_t)(nvmlDevice_t, unsigned int, unsigned int);
typedef nvmlReturn_t (*nvmlDeviceSetDefaultFanSpeed_v2_t)(nvmlDevice_t, unsigned int);
typedef nvmlReturn_t (*nvmlDeviceGetCoolerInfo_t)(nvmlDevice_t, nvmlCoolerInfo_t*);

struct NvmlApi {
    nvmlInit_v2_t init;
    nvmlShutdown_t shutdown;
    nvmlDeviceGetHandleByIndex_v2_t getHandleByIndex;
    nvmlDeviceGetPowerManagementLimit_t getPowerLimit;
    nvmlDeviceGetPowerManagementDefaultLimit_t getPowerDefaultLimit;
    nvmlDeviceGetPowerManagementLimitConstraints_t getPowerConstraints;
    nvmlDeviceSetPowerManagementLimit_t setPowerLimit;
    nvmlDeviceGetClockOffsets_t getClockOffsets;
    nvmlDeviceSetClockOffsets_t setClockOffsets;
    nvmlDeviceGetPerformanceState_t getPerformanceState;
    nvmlDeviceGetGpcClkVfOffset_t getGpcClkVfOffset;
    nvmlDeviceGetMemClkVfOffset_t getMemClkVfOffset;
    nvmlDeviceGetGpcClkMinMaxVfOffset_t getGpcClkMinMaxVfOffset;
    nvmlDeviceGetMemClkMinMaxVfOffset_t getMemClkMinMaxVfOffset;
    nvmlDeviceSetGpcClkVfOffset_t setGpcClkVfOffset;
    nvmlDeviceSetMemClkVfOffset_t setMemClkVfOffset;
    nvmlDeviceGetNumFans_t getNumFans;
    nvmlDeviceGetMinMaxFanSpeed_t getMinMaxFanSpeed;
    nvmlDeviceGetFanControlPolicy_v2_t getFanControlPolicy;
    nvmlDeviceSetFanControlPolicy_t setFanControlPolicy;
    nvmlDeviceGetFanSpeed_v2_t getFanSpeed;
    nvmlDeviceGetTargetFanSpeed_t getTargetFanSpeed;
    nvmlDeviceGetFanSpeedRPM_t getFanSpeedRpm;
    nvmlDeviceSetFanSpeed_v2_t setFanSpeed;
    nvmlDeviceSetDefaultFanSpeed_v2_t setDefaultFanSpeed;
    nvmlDeviceGetCoolerInfo_t getCoolerInfo;
};

static NvmlApi g_nvml_api = {};
static HMODULE g_nvml = nullptr;
static bool g_debug_logging = false;

static bool nvapi_read_curve();
static bool nvapi_read_offsets();
static bool nvapi_read_pstates();
static bool nvapi_set_point(int pointIndex, int freqDelta_kHz);
static bool nvapi_set_gpu_offset(int offsetkHz);
static bool nvapi_set_mem_offset(int offsetkHz);
static bool nvapi_set_power_limit(int pct);
static void rebuild_visible_map();
static unsigned int get_edit_value(HWND hEdit);
static void populate_edits();
static void create_edit_controls(HWND hParent, HINSTANCE hInst);
static int mem_display_mhz_from_driver_khz(int driver_kHz);
static int mem_display_mhz_from_driver_mhz(int driverMHz);
static unsigned int displayed_curve_mhz(unsigned int rawFreq_kHz);
static void apply_system_titlebar_theme(HWND hwnd);
static bool fan_setting_matches_current(bool wantAuto, int wantPct);
static void show_license_dialog(HWND parent);
static void layout_bottom_buttons(HWND hParent);
static void debug_log(const char* fmt, ...);
static bool write_text_file_atomic(const char* path, const char* data, size_t dataSize, char* err, size_t errSize);
static bool write_log_snapshot(const char* path, char* err, size_t errSize);
static bool save_desired_to_config_with_startup(const char* path, const DesiredSettings* desired, bool useCurrentForUnset, int startupState, char* err, size_t errSize);
static bool capture_gui_config_settings(DesiredSettings* desired, char* err, size_t errSize);
static bool set_startup_task_enabled(bool enabled, char* err, size_t errSize);
static bool is_startup_task_enabled();
static void sync_startup_checkbox_from_system();
static bool startup_checkbox_checked();
static void detect_locked_tail_from_curve();
static void close_nvml();

static void show_license_dialog(HWND parent) {
    MessageBoxA(parent, APP_LICENSE_TEXT, APP_NAME " License", MB_OK | MB_ICONINFORMATION);
}

static int layout_rows_per_column() {
    return (g_app.numVisible + 5) / 6;
}

static int layout_bottom_buttons_y() {
    return dp(GRAPH_HEIGHT) + dp(14) + layout_rows_per_column() * dp(20) + dp(38);
}

static int minimum_client_height() {
    int btnBottom = layout_bottom_buttons_y() + dp(30);
    return nvmax(dp(WINDOW_HEIGHT), btnBottom + dp(12));
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
    int btnY = layout_bottom_buttons_y();
    if (g_app.hApplyBtn)
        SetWindowPos(g_app.hApplyBtn, nullptr, dp(8), btnY, dp(110), dp(30), SWP_NOZORDER);
    if (g_app.hRefreshBtn)
        SetWindowPos(g_app.hRefreshBtn, nullptr, dp(130), btnY, dp(90), dp(30), SWP_NOZORDER);
    if (g_app.hResetBtn)
        SetWindowPos(g_app.hResetBtn, nullptr, dp(232), btnY, dp(90), dp(30), SWP_NOZORDER);
    if (g_app.hSaveCfgBtn)
        SetWindowPos(g_app.hSaveCfgBtn, nullptr, dp(334), btnY, dp(130), dp(30), SWP_NOZORDER);
    if (g_app.hStartupApplyCheck) {
        int startupX = dp(480);
        int startupRight = rc.right - dp(100);
        int startupW = nvmax(dp(190), startupRight - startupX);
        SetWindowPos(g_app.hStartupApplyCheck, nullptr, startupX, btnY + dp(6), startupW, dp(20), SWP_NOZORDER);
    }
    if (g_app.hLicenseBtn)
        SetWindowPos(g_app.hLicenseBtn, nullptr, rc.right - dp(8) - dp(80), btnY, dp(80), dp(30), SWP_NOZORDER);
}

static bool startup_checkbox_checked() {
    return g_app.hStartupApplyCheck && SendMessageA(g_app.hStartupApplyCheck, BM_GETCHECK, 0, 0) == BST_CHECKED;
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

static void trim_ascii(char* s) {
    if (!s) return;
    int len = (int)strlen(s);
    int start = 0;
    while (start < len && (unsigned char)s[start] <= ' ') start++;
    int end = len;
    while (end > start && (unsigned char)s[end - 1] <= ' ') end--;
    if (start > 0 && end > start) memmove(s, s + start, (size_t)(end - start));
    if (end <= start) {
        s[0] = 0;
    } else {
        s[end - start] = 0;
    }
}

static bool streqi_ascii(const char* a, const char* b) {
    if (!a || !b) return false;
    while (*a && *b) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return false;
        ++a;
        ++b;
    }
    return *a == 0 && *b == 0;
}

static bool parse_int_strict(const char* s, int* out) {
    if (!s || !*s || !out) return false;
    char* end = nullptr;
    long v = strtol(s, &end, 10);
    if (!end || *end != 0) return false;
    if (v < -2147483647L - 1L || v > 2147483647L) return false;
    *out = (int)v;
    return true;
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

static void set_message(char* dst, size_t dstSize, const char* fmt, ...) {
    if (!dst || dstSize == 0) return;
    va_list ap;
    va_start(ap, fmt);
    StringCchVPrintfA(dst, dstSize, fmt, ap);
    va_end(ap);
    dst[dstSize - 1] = 0;
}

static bool parse_fan_value(const char* text, bool* isAuto, int* pct) {
    if (!isAuto || !pct) return false;
    char buf[64] = {};
    if (text) StringCchCopyA(buf, ARRAY_COUNT(buf), text);
    trim_ascii(buf);
    if (buf[0] == 0 || streqi_ascii(buf, "auto")) {
        *isAuto = true;
        *pct = 0;
        return true;
    }
    int value = 0;
    if (!parse_int_strict(buf, &value)) return false;
    if (value < 0 || value > 100) return false;
    *isAuto = false;
    *pct = value;
    return true;
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

static bool load_desired_settings_from_ini(const char* path, DesiredSettings* desired, char* err, size_t errSize) {
    if (!path || !desired) return false;
    memset(desired, 0, sizeof(*desired));
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

    GetPrivateProfileStringA("controls", "fan", "", fanBuf, sizeof(fanBuf), path);
    trim_ascii(fanBuf);
    if (fanBuf[0]) {
        desired->hasFan = true;
        if (!parse_fan_value(fanBuf, &desired->fanAuto, &desired->fanPercent)) {
            set_message(err, errSize, "Invalid fan setting in %s", path);
            return false;
        }
    }

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

static void merge_desired_settings(DesiredSettings* base, const DesiredSettings* override) {
    if (!base || !override) return;
    if (override->hasGpuOffset) {
        base->hasGpuOffset = true;
        base->gpuOffsetMHz = override->gpuOffsetMHz;
    }
    if (override->hasMemOffset) {
        base->hasMemOffset = true;
        base->memOffsetMHz = override->memOffsetMHz;
    }
    if (override->hasPowerLimit) {
        base->hasPowerLimit = true;
        base->powerLimitPct = override->powerLimitPct;
    }
    if (override->hasFan) {
        base->hasFan = true;
        base->fanAuto = override->fanAuto;
        base->fanPercent = override->fanPercent;
    }
    for (int i = 0; i < VF_NUM_POINTS; i++) {
        if (override->hasCurvePoint[i]) {
            base->hasCurvePoint[i] = true;
            base->curvePointMHz[i] = override->curvePointMHz[i];
        }
    }
}

static bool desired_has_any_action(const DesiredSettings* desired) {
    if (!desired) return false;
    if (desired->hasGpuOffset || desired->hasMemOffset || desired->hasPowerLimit || desired->hasFan) return true;
    for (int i = 0; i < VF_NUM_POINTS; i++) {
        if (desired->hasCurvePoint[i]) return true;
    }
    return false;
}

static bool capture_gui_desired_settings(DesiredSettings* desired, bool includeCurrentGlobals, bool expandLockedTail, bool captureAllCurvePoints, char* err, size_t errSize) {
    if (!desired) return false;
    memset(desired, 0, sizeof(*desired));

    char buf[64] = {};
    int currentGpuOffsetMHz = g_app.gpuClockOffsetkHz / 1000;
    get_window_text_safe(g_app.hGpuOffsetEdit, buf, sizeof(buf));
    int gpuOffsetMHz = currentGpuOffsetMHz;
    if (buf[0]) {
        if (!parse_int_strict(buf, &gpuOffsetMHz)) {
            set_message(err, errSize, "Invalid GPU offset");
            return false;
        }
    }

    bool hasLock = g_app.lockedVi >= 0 && g_app.lockedVi < g_app.numVisible;
    int lockCi = -1;
    int effectiveLockTargetMHz = 0;
    int gpuOffsetDeltaMHz = gpuOffsetMHz - currentGpuOffsetMHz;
    if (hasLock) {
        lockCi = g_app.visibleMap[g_app.lockedVi];
        char lockBuf[32] = {};
        get_window_text_safe(g_app.hEditsMhz[g_app.lockedVi], lockBuf, sizeof(lockBuf));
        if (!parse_int_strict(lockBuf, &effectiveLockTargetMHz) || effectiveLockTargetMHz <= 0) {
            set_message(err, errSize, "Invalid MHz value for point %d", lockCi);
            return false;
        }
        int currentLockMHz = (int)displayed_curve_mhz(g_app.curve[lockCi].freq_kHz);
        if (effectiveLockTargetMHz == currentLockMHz && gpuOffsetMHz != currentGpuOffsetMHz) {
            effectiveLockTargetMHz += gpuOffsetMHz - currentGpuOffsetMHz;
            if (effectiveLockTargetMHz <= 0) effectiveLockTargetMHz = 1;
        }
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
        unsigned int currentMHz = displayed_curve_mhz(g_app.curve[ci].freq_kHz);
        int effectiveMHz = mhz;
        if (gpuOffsetDeltaMHz != 0 && (unsigned int)mhz == currentMHz) {
            effectiveMHz += gpuOffsetDeltaMHz;
            if (effectiveMHz <= 0) effectiveMHz = 1;
        }
        if (captureAllCurvePoints || (unsigned int)effectiveMHz != currentMHz) {
            desired->hasCurvePoint[ci] = true;
            desired->curvePointMHz[ci] = (unsigned int)effectiveMHz;
        }
    }

    if (includeCurrentGlobals || gpuOffsetMHz != currentGpuOffsetMHz) {
        desired->hasGpuOffset = true;
        desired->gpuOffsetMHz = gpuOffsetMHz;
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

    get_window_text_safe(g_app.hFanEdit, buf, sizeof(buf));
    bool fanAuto = false;
    int fanPercent = 0;
    if (!parse_fan_value(buf, &fanAuto, &fanPercent)) {
        set_message(err, errSize, "Invalid fan value, use auto or 0-100");
        return false;
    }
    if (includeCurrentGlobals || !fan_setting_matches_current(fanAuto, fanPercent)) {
        desired->hasFan = true;
        desired->fanAuto = fanAuto;
        desired->fanPercent = fanPercent;
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
        L"    <Description>Apply Green Curve startup settings at user logon.</Description>\r\n"
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
        L"      <Arguments>--apply-config --config &quot;%ls&quot;</Arguments>\r\n"
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
    if (!xml_escape_wide(exePath, exeEsc, ARRAY_COUNT(exeEsc), false) ||
        !xml_escape_wide(cfgPath, cfgEsc, ARRAY_COUNT(cfgEsc), true) ||
        !xml_escape_wide(userName, userEsc, ARRAY_COUNT(userEsc), false)) {
        CloseHandle(h);
        DeleteFileW(xmlPath);
        set_message(err, errSize, "Failed escaping startup task XML");
        return false;
    }

    WCHAR xml[8192] = {};
    HRESULT hr = StringCchPrintfW(xml, ARRAY_COUNT(xml), xmlFmt, userEsc, userEsc, userEsc, exeEsc, cfgEsc);
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

    char buf[16] = {};
    GetPrivateProfileStringA("startup", "apply_on_launch", "", buf, sizeof(buf), path);
    trim_ascii(buf);
    if (!buf[0]) return false;

    int value = 0;
    if (!parse_int_strict(buf, &value)) return false;
    *enabled = value != 0;
    return true;
}

static void sync_startup_checkbox_from_system() {
    if (!g_app.hStartupApplyCheck) return;

    bool checked = is_startup_task_enabled();
    if (!checked) {
        bool configEnabled = false;
        if (load_startup_enabled_from_config(g_app.configPath, &configEnabled) && configEnabled) {
            char err[256] = {};
            if (set_startup_task_enabled(true, err, sizeof(err))) checked = true;
        }
    }

    SendMessageA(g_app.hStartupApplyCheck, BM_SETCHECK, checked ? BST_CHECKED : BST_UNCHECKED, 0);
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
        g_app.memClockOffsetkHz = cur * 1000;
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

    Sleep(50);
    int mn = 0, mx = 0, cur = 0;
    if (!nvml_get_offset_range(domain, &mn, &mx, &cur, detail, detailSize)) {
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
    Sleep(50);
    nvml_read_fans(detail, detailSize);
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
    Sleep(50);
    nvml_read_fans(detail, detailSize);
    bool ok = (g_app.fanCount > 0);
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
    if (exactApplied) *exactApplied = ok;
    return true;
}

static bool fan_setting_matches_current(bool wantAuto, int wantPct) {
    if (!g_app.fanSupported) return false;
    if (wantAuto) return g_app.fanIsAuto;
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
    return ok1 || ok2 || ok3 || ok4;
}

static void populate_global_controls() {
    if (g_app.hGpuOffsetEdit) {
        char buf[32];
        StringCchPrintfA(buf, ARRAY_COUNT(buf), "%d", g_app.gpuClockOffsetkHz / 1000);
        SetWindowTextA(g_app.hGpuOffsetEdit, buf);
        EnableWindow(g_app.hGpuOffsetEdit, g_app.gpuOffsetRangeKnown ? TRUE : FALSE);
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
    if (g_app.hFanEdit) {
        if (!g_app.fanSupported || g_app.fanIsAuto) {
            SetWindowTextA(g_app.hFanEdit, "auto");
        } else {
            char buf[32];
            StringCchPrintfA(buf, ARRAY_COUNT(buf), "%u", g_app.fanPercent[0]);
            SetWindowTextA(g_app.hFanEdit, buf);
        }
        EnableWindow(g_app.hFanEdit, g_app.fanSupported ? TRUE : FALSE);
    }
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
    int memOffset = desired && desired->hasMemOffset ? desired->memOffsetMHz : mem_display_mhz_from_driver_khz(g_app.memClockOffsetkHz);
    int powerPct = desired && desired->hasPowerLimit ? desired->powerLimitPct : g_app.powerLimitPct;
    bool fanAuto = desired && desired->hasFan ? desired->fanAuto : g_app.fanIsAuto;
    int fanPct = desired && desired->hasFan ? desired->fanPercent : (g_app.fanCount ? (int)g_app.fanPercent[0] : 0);

    StringCchPrintfA(buf, ARRAY_COUNT(buf), "%d", gpuOffset);
    if (!WritePrivateProfileStringA("controls", "gpu_offset_mhz", buf, path)) {
        set_message(err, errSize, "Failed to write gpu_offset_mhz");
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
    if (fanAuto) {
        if (!WritePrivateProfileStringA("controls", "fan", "auto", path)) {
            set_message(err, errSize, "Failed to write fan");
            return false;
        }
    } else {
        StringCchPrintfA(buf, ARRAY_COUNT(buf), "%d", fanPct);
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

    return true;
}

static unsigned int displayed_curve_mhz(unsigned int rawFreq_kHz) {
    return (unsigned int)(displayed_curve_khz(rawFreq_kHz) / 1000);
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
    return (driver_kHz / 1000) / 2;
}

static int mem_driver_khz_from_display_mhz(int displayMHz) {
    return displayMHz * 2 * 1000;
}

static int mem_display_mhz_from_driver_mhz(int driverMHz) {
    return driverMHz / 2;
}

static bool restore_curve_offsets(const int* targetOffsets, const bool* populated, bool forceWriteAll) {
    if (!targetOffsets || !populated) return false;

    bool anyWrite = false;
    bool allOk = true;
    for (int i = 0; i < VF_NUM_POINTS; i++) {
        if (!populated[i]) continue;
        if (!forceWriteAll && g_app.freqOffsets[i] == targetOffsets[i]) continue;

        anyWrite = true;
        if (!nvapi_set_point(i, targetOffsets[i])) allOk = false;
        Sleep(2);
    }

    if (anyWrite) {
        Sleep(100);
        nvapi_read_curve();
        nvapi_read_offsets();
        rebuild_visible_map();
    }

    return allOk;
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

static bool apply_desired_settings(const DesiredSettings* desired, bool interactive, char* result, size_t resultSize) {
    if (!desired) {
        set_message(result, resultSize, "No desired settings");
        return false;
    }

    int successCount = 0;
    int failCount = 0;
    bool hasLock = (g_app.lockedVi >= 0) && interactive;
    bool hasCurveEdits = false;
    int lockCi = -1;
    unsigned int lockMhz = 0;
    bool memOffsetValid = true;
    bool shouldApplyMemOffset = false;
    int targetMemkHz = 0;
    int preservedCurveOffsets[VF_NUM_POINTS] = {};
    bool preservedCurvePopulated[VF_NUM_POINTS] = {};

    for (int ci = 0; ci < VF_NUM_POINTS; ci++) {
        if (desired->hasCurvePoint[ci]) {
            hasCurveEdits = true;
            break;
        }
    }

    if (hasLock) {
        lockCi = g_app.visibleMap[g_app.lockedVi];
        lockMhz = desired->hasCurvePoint[lockCi] ? desired->curvePointMHz[lockCi] : get_edit_value(g_app.hEditsMhz[g_app.lockedVi]);
    }

    if (desired->hasMemOffset) {
        if (!g_app.memOffsetRangeKnown ||
            (desired->memOffsetMHz >= g_app.memClockOffsetMinMHz && desired->memOffsetMHz <= g_app.memClockOffsetMaxMHz)) {
            targetMemkHz = mem_driver_khz_from_display_mhz(desired->memOffsetMHz);
            shouldApplyMemOffset = (g_app.memClockOffsetkHz != targetMemkHz);
        } else {
            memOffsetValid = false;
        }
    }

    if (desired->hasGpuOffset) {
        if (!g_app.gpuOffsetRangeKnown ||
            (desired->gpuOffsetMHz >= g_app.gpuClockOffsetMinMHz && desired->gpuOffsetMHz <= g_app.gpuClockOffsetMaxMHz)) {
            int targetOffsetkHz = desired->gpuOffsetMHz * 1000;
            if (g_app.gpuClockOffsetkHz != targetOffsetkHz) {
                if (nvapi_set_gpu_offset(targetOffsetkHz)) {
                    successCount++;
                    if (hasCurveEdits || hasLock || shouldApplyMemOffset) {
                        nvapi_read_curve();
                        nvapi_read_offsets();
                        rebuild_visible_map();
                    }
                } else {
                    failCount++;
                }
            }
        } else {
            failCount++;
        }
    }

    if (desired->hasMemOffset) {
        if (memOffsetValid) {
            if (shouldApplyMemOffset) {
                for (int ci = 0; ci < VF_NUM_POINTS; ci++) {
                    preservedCurveOffsets[ci] = g_app.freqOffsets[ci];
                    preservedCurvePopulated[ci] = g_app.curve[ci].freq_kHz > 0;
                }

                if (nvapi_set_mem_offset(targetMemkHz)) {
                    successCount++;

                    bool curveOk = nvapi_read_curve();
                    bool offsetOk = nvapi_read_offsets();
                    rebuild_visible_map();

                    if (!restore_curve_offsets(preservedCurveOffsets, preservedCurvePopulated, !(curveOk && offsetOk))) {
                        failCount++;
                    }
                } else {
                    failCount++;
                }
            }
        } else {
            failCount++;
        }
    }

    if (hasLock) {
        int lockDelta = curve_delta_khz_for_target_display_mhz(lockCi, lockMhz);
        if (nvapi_set_point(lockCi, lockDelta)) successCount++; else failCount++;
        Sleep(20);
        nvapi_read_curve();
        nvapi_read_offsets();
        rebuild_visible_map();
    }

    for (int ci = 0; ci < VF_NUM_POINTS; ci++) {
        if (!desired->hasCurvePoint[ci]) continue;
        if (g_app.curve[ci].freq_kHz == 0) continue;
        if (hasLock && ci >= lockCi) continue;

        int delta_kHz = curve_delta_khz_for_target_display_mhz(ci, desired->curvePointMHz[ci]);

        if (delta_kHz != 0 || hasLock) {
            if (nvapi_set_point(ci, delta_kHz)) successCount++; else failCount++;
            Sleep(2);
        }
    }

    if (hasLock && lockMhz > 0) {
        for (int vi = 0; vi < g_app.numVisible; vi++) {
            int ci = g_app.visibleMap[vi];
            if (ci <= lockCi) continue;
            if (g_app.curve[ci].freq_kHz == 0) continue;
            int delta_kHz = curve_delta_khz_for_target_display_mhz(ci, lockMhz);
            if (delta_kHz == 0) continue;
            if (nvapi_set_point(ci, delta_kHz)) successCount++; else failCount++;
            Sleep(2);
        }
    }

    Sleep(100);
    nvapi_read_curve();
    nvapi_read_offsets();
    rebuild_visible_map();

    if (hasLock && lockMhz > 0) {
        for (int pass = 0; pass < 6; pass++) {
            bool retried = false;
            for (int vi = 0; vi < g_app.numVisible; vi++) {
                int ci = g_app.visibleMap[vi];
                if (ci < lockCi) continue;
                if (g_app.curve[ci].freq_kHz == 0) continue;
                unsigned int gotMhz = displayed_curve_mhz(g_app.curve[ci].freq_kHz);
                if (gotMhz == lockMhz) continue;
                int retryDelta = curve_delta_khz_for_target_display_mhz(ci, lockMhz);
                if (nvapi_set_point(ci, retryDelta)) {
                    successCount++;
                    retried = true;
                } else {
                    failCount++;
                }
                Sleep(5);
            }
            if (!retried) break;
            Sleep(50);
            nvapi_read_curve();
            nvapi_read_offsets();
            rebuild_visible_map();
        }
    }

    if (hasLock) {
        if (lockCi >= 0 && lockCi < VF_NUM_POINTS && g_app.curve[lockCi].freq_kHz > 0) {
            g_app.lockedFreq = displayed_curve_mhz(g_app.curve[lockCi].freq_kHz);
        } else {
            g_app.lockedFreq = lockMhz;
        }
    }
    if (desired->hasPowerLimit) {
        int currentPowerPct = g_app.powerLimitPct;
        if (desired->powerLimitPct != currentPowerPct) {
            if (nvapi_set_power_limit(desired->powerLimitPct)) successCount++; else failCount++;
        }
    }
    if (desired->hasFan) {
        if (!fan_setting_matches_current(desired->fanAuto, desired->fanPercent)) {
            bool exact = false;
            char detail[128] = {};
            bool ok = false;
            if (desired->fanAuto) {
                ok = nvml_set_fan_auto(detail, sizeof(detail));
            } else if (!g_app.fanRangeKnown ||
                       (desired->fanPercent >= (int)g_app.fanMinPct && desired->fanPercent <= (int)g_app.fanMaxPct) ||
                       desired->fanPercent == 0) {
                ok = nvml_set_fan_manual(desired->fanPercent, &exact, detail, sizeof(detail));
            }
            if (ok) successCount++;
            else failCount++;
        }
    }

    char detail[128] = {};
    refresh_global_state(detail, sizeof(detail));
    populate_global_controls();
    if (interactive) {
        populate_edits();
        InvalidateRect(g_app.hMainWnd, nullptr, TRUE);
    }

    if (successCount == 0 && failCount == 0) {
        set_message(result, resultSize, "No setting changes needed.");
    } else if (failCount == 0) {
        set_message(result, resultSize, "Applied %d setting changes successfully.", successCount);
    } else {
        set_message(result, resultSize, "Applied %d OK, %d failed.", successCount, failCount);
    }
    return failCount == 0;
}

// ============================================================================
// NvAPI Interface
// ============================================================================

typedef int (*NvApiFunc)(void*, void*);

static void* nvapi_qi(unsigned int id) {
    typedef void* (*qi_func)(unsigned int);
    static qi_func qi = nullptr;
    if (!qi) {
        g_app.hNvApi = LoadLibraryA("nvapi64.dll");
        if (!g_app.hNvApi) {
            g_app.hNvApi = LoadLibraryA("nvapi.dll");
        }
        if (!g_app.hNvApi) return nullptr;
        qi = (qi_func)GetProcAddress(g_app.hNvApi, "nvapi_QueryInterface");
        if (!qi) return nullptr;
    }
    return qi(id);
}

static bool nvapi_init() {
    typedef int (*init_t)();
    auto init = (init_t)nvapi_qi(NVAPI_INIT_ID);
    if (!init) return false;
    return init() == 0;
}

static bool nvapi_enum_gpu() {
    typedef int (*enum_t)(GPU_HANDLE*, int*);
    auto enumGpu = (enum_t)nvapi_qi(NVAPI_ENUM_GPU_ID);
    if (!enumGpu) return false;
    int count = 0;
    GPU_HANDLE handles[64] = {};
    int ret = enumGpu(handles, &count);
    if (ret != 0 || count < 1) return false;
    g_app.gpuHandle = handles[0];
    return true;
}

static bool nvapi_get_name() {
    typedef int (*name_t)(GPU_HANDLE, char*);
    auto getName = (name_t)nvapi_qi(NVAPI_GET_NAME_ID);
    if (!getName) return false;
    return getName(g_app.gpuHandle, g_app.gpuName) == 0;
}

static bool nvapi_read_curve() {
    auto getInfo = (NvApiFunc)nvapi_qi(VF_GET_INFO_ID);
    auto getStatus = (NvApiFunc)nvapi_qi(VF_GET_STATUS_ID);
    if (!getStatus) return false;

    unsigned char mask[32] = {};
    unsigned int numClocks = 15;

    if (getInfo) {
        unsigned char ibuf[0x182C] = {};
        const unsigned int version = (1u << 16) | 0x182C;
        memcpy(&ibuf[0], &version, sizeof(version));
        memset(&ibuf[4], 0xFF, 32);
        if (getInfo(g_app.gpuHandle, ibuf) == 0) {
            memcpy(mask, &ibuf[4], 32);
            memcpy(&numClocks, &ibuf[0x14], sizeof(numClocks));
        } else {
            memset(mask, 0xFF, 16);
        }
    } else {
        memset(mask, 0xFF, 16);
    }

    unsigned char buf[VF_BUFFER_SIZE] = {};
    {
        const unsigned int version = (1u << 16) | VF_BUFFER_SIZE;
        memcpy(&buf[0], &version, sizeof(version));
    }
    memcpy(&buf[4], mask, 32);
    memcpy(&buf[0x24], &numClocks, sizeof(numClocks));

    int ret = getStatus(g_app.gpuHandle, buf);
    if (ret != 0) return false;

    g_app.numPopulated = 0;
    for (int i = 0; i < VF_NUM_POINTS; i++) {
        unsigned int freq = 0, volt = 0;
        memcpy(&freq, &buf[VF_ENTRIES_OFFSET + i * VF_ENTRY_STRIDE], sizeof(freq));
        memcpy(&volt, &buf[VF_ENTRIES_OFFSET + i * VF_ENTRY_STRIDE + 4], sizeof(volt));
        g_app.curve[i].freq_kHz = freq;
        g_app.curve[i].volt_uV = volt;
        if (freq > 0) g_app.numPopulated++;
    }
    g_app.loaded = true;
    return true;
}

static bool nvapi_read_offsets() {
    auto func = (NvApiFunc)nvapi_qi(VF_GET_CONTROL_ID);
    if (!func) return false;

    unsigned char mask[32] = {};
    auto getInfo = (NvApiFunc)nvapi_qi(VF_GET_INFO_ID);
    if (getInfo) {
        unsigned char ibuf[0x182C] = {};
        const unsigned int version = (1u << 16) | 0x182C;
        memcpy(&ibuf[0], &version, sizeof(version));
        memset(&ibuf[4], 0xFF, 32);
        if (getInfo(g_app.gpuHandle, ibuf) == 0) {
            memcpy(mask, &ibuf[4], 32);
        } else {
            memset(mask, 0xFF, 16);
        }
    } else {
        memset(mask, 0xFF, 16);
    }

    const unsigned int CTRL_SIZE = 0x2420;
    unsigned char buf[0x2420] = {};
    {
        const unsigned int version = (1u << 16) | CTRL_SIZE;
        memcpy(&buf[0], &version, sizeof(version));
    }
    memcpy(&buf[4], mask, 32);

    int ret = func(g_app.gpuHandle, buf);
    if (ret != 0) return false;

    for (int i = 0; i < VF_NUM_POINTS; i++) {
        int delta = 0;
        memcpy(&delta, &buf[0x44 + i * 0x24 + 0x14], sizeof(delta));
        g_app.freqOffsets[i] = delta;
    }
    return true;
}

static bool nvapi_set_point(int pointIndex, int freqDelta_kHz) {
    auto func = (NvApiFunc)nvapi_qi(VF_SET_CONTROL_ID);
    if (!func) return false;
    if (pointIndex < 0 || pointIndex >= VF_NUM_POINTS) return false;
    if (freqDelta_kHz < -1000000 || freqDelta_kHz > 1000000) return false;

    auto getFunc = (NvApiFunc)nvapi_qi(VF_GET_CONTROL_ID);
    if (!getFunc) return false;

    unsigned char mask[32] = {};
    auto getInfo = (NvApiFunc)nvapi_qi(VF_GET_INFO_ID);
    if (getInfo) {
        unsigned char ibuf[0x182C] = {};
        const unsigned int version = (1u << 16) | 0x182C;
        memcpy(&ibuf[0], &version, sizeof(version));
        memset(&ibuf[4], 0xFF, 32);
        if (getInfo(g_app.gpuHandle, ibuf) == 0) {
            memcpy(mask, &ibuf[4], 32);
        } else {
            memset(mask, 0xFF, 16);
        }
    } else {
        memset(mask, 0xFF, 16);
    }

    const unsigned int CTRL_SIZE = 0x2420;
    unsigned char buf[0x2420] = {};
    {
        const unsigned int version = (1u << 16) | CTRL_SIZE;
        memcpy(&buf[0], &version, sizeof(version));
    }
    memcpy(&buf[4], mask, 32);

    int ret = getFunc(g_app.gpuHandle, buf);
    if (ret != 0) return false;

    memset(&buf[4], 0, 32);
    buf[4 + pointIndex / 8] = (unsigned char)(1 << (pointIndex % 8));
    memcpy(&buf[0x44 + pointIndex * 0x24 + 0x14], &freqDelta_kHz, sizeof(freqDelta_kHz));

    ret = func(g_app.gpuHandle, buf);
    return ret == 0;
}

// Pstates20 struct size and version for Blackwell
// NVML-based OC/PL functions
static bool nvml_read_power_limit() {
    if (!nvml_ensure_ready()) return false;
    if (!g_nvml_api.getPowerLimit || !g_nvml_api.getPowerDefaultLimit) return false;

    unsigned int cur = 0, def = 0;
    if (g_nvml_api.getPowerLimit(g_app.nvmlDevice, &cur) != NVML_SUCCESS) return false;
    if (g_nvml_api.getPowerDefaultLimit(g_app.nvmlDevice, &def) != NVML_SUCCESS) def = cur;

    g_app.powerLimitCurrentmW = (int)cur;
    g_app.powerLimitDefaultmW = def > 0 ? (int)def : (int)cur;
    g_app.powerLimitMinmW = 0;
    g_app.powerLimitMaxmW = 0;

    if (g_nvml_api.getPowerConstraints) {
        unsigned int mn = 0, mx = 0;
        if (g_nvml_api.getPowerConstraints(g_app.nvmlDevice, &mn, &mx) == NVML_SUCCESS) {
            g_app.powerLimitMinmW = (int)mn;
            g_app.powerLimitMaxmW = (int)mx;
        }
    }

    if (g_app.powerLimitDefaultmW > 0)
        g_app.powerLimitPct = (g_app.powerLimitCurrentmW * 100 + g_app.powerLimitDefaultmW / 2) / g_app.powerLimitDefaultmW;
    else
        g_app.powerLimitPct = 100;

    if (g_app.powerLimitPct < 0) g_app.powerLimitPct = 0;

    return true;
}

static bool nvapi_read_pstates() {
    // Read clock offsets from NvAPI Pstates20 (works for reading on Blackwell)
    auto func = (NvApiFunc)nvapi_qi(0x6FF81213u);
    if (!func) return false;

    unsigned char buf[0x1CF8] = {};
    {
        const unsigned int version = (2u << 16) | 0x1CF8;
        memcpy(&buf[0], &version, sizeof(version));
    }
    if (func(g_app.gpuHandle, buf) != 0) return false;

    memcpy(&g_app.gpuClockOffsetkHz, &buf[0x3C], sizeof(g_app.gpuClockOffsetkHz));
    memcpy(&g_app.memClockOffsetkHz, &buf[0x54], sizeof(g_app.memClockOffsetkHz));
    if (!g_app.gpuOffsetRangeKnown) {
        g_app.gpuClockOffsetMinMHz = g_app.gpuClockOffsetkHz / 1000;
        g_app.gpuClockOffsetMaxMHz = g_app.gpuClockOffsetkHz / 1000;
    }
    if (!g_app.memOffsetRangeKnown) {
        g_app.memClockOffsetMinMHz = mem_display_mhz_from_driver_khz(g_app.memClockOffsetkHz);
        g_app.memClockOffsetMaxMHz = mem_display_mhz_from_driver_khz(g_app.memClockOffsetkHz);
    }
    return true;
}

static bool nvapi_set_gpu_offset(int offsetkHz) {
    if (g_app.gpuClockOffsetkHz == offsetkHz) return true;
    bool exact = false;
    char detail[128] = {};
    bool ok = nvml_set_clock_offset_domain(NVML_CLOCK_GRAPHICS, offsetkHz / 1000, &exact, detail, sizeof(detail));
    if (ok && exact) {
        nvml_read_clock_offsets(detail, sizeof(detail));
        return true;
    }
    return false;
}

static bool nvapi_set_mem_offset(int offsetkHz) {
    if (g_app.memClockOffsetkHz == offsetkHz) return true;
    bool exact = false;
    char detail[128] = {};
    bool ok = nvml_set_clock_offset_domain(NVML_CLOCK_MEM, offsetkHz / 1000, &exact, detail, sizeof(detail));
    if (ok && exact) {
        nvml_read_clock_offsets(detail, sizeof(detail));
        return true;
    }
    return false;
}

static bool nvapi_set_power_limit(int pct) {
    if (pct < 50 || pct > 150) return false;
    if (g_app.powerLimitDefaultmW <= 0) return false;

    int watts = (g_app.powerLimitDefaultmW * pct + 50000) / 100000;
    if (watts < 1) return false;
    unsigned int targetmW = (unsigned int)watts * 1000u;

    if (g_app.powerLimitMinmW > 0 && targetmW < (unsigned int)g_app.powerLimitMinmW) return false;
    if (g_app.powerLimitMaxmW > 0 && targetmW > (unsigned int)g_app.powerLimitMaxmW) return false;

    if (nvml_ensure_ready() && g_nvml_api.setPowerLimit) {
        nvmlReturn_t r = g_nvml_api.setPowerLimit(g_app.nvmlDevice, targetmW);
        if (r == NVML_SUCCESS) {
            nvml_read_power_limit();
            return true;
        }
        debug_log("Power limit via NVML failed: %s\n", nvml_err_name(r));
    }

    char exePath[MAX_PATH] = {};
    DWORD pathLen = SearchPathA(nullptr, "nvidia-smi.exe", nullptr, ARRAY_COUNT(exePath), exePath, nullptr);
    if (pathLen == 0 || pathLen >= ARRAY_COUNT(exePath)) {
        StringCchCopyA(exePath, ARRAY_COUNT(exePath), "nvidia-smi.exe");
    }

    char cmdLine[256] = {};
    StringCchPrintfA(cmdLine, ARRAY_COUNT(cmdLine), "\"%s\" -pl %d", exePath, watts);

    STARTUPINFOA si = {};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi = {};

    if (!CreateProcessA(nullptr, cmdLine, nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        debug_log("Power limit via nvidia-smi failed to launch (error %lu)\n", GetLastError());
        return false;
    }

    WaitForSingleObject(pi.hProcess, 5000);
    DWORD exitCode = 0;
    GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    if (exitCode == 0) {
        nvml_read_power_limit();
    }
    else debug_log("Power limit via nvidia-smi failed with exit code %lu\n", exitCode);
    return exitCode == 0;
}

static void rebuild_visible_map() {
    g_app.numVisible = 0;
    for (int i = 0; i < VF_NUM_POINTS; i++) {
        unsigned int freq_mhz = displayed_curve_mhz(g_app.curve[i].freq_kHz);
        unsigned int volt_mv = g_app.curve[i].volt_uV / 1000;
        if (volt_mv >= MIN_VISIBLE_VOLT_mV && freq_mhz >= MIN_VISIBLE_FREQ_MHz) {
            g_app.visibleMap[g_app.numVisible++] = i;
        }
    }
}

static void detect_locked_tail_from_curve() {
    g_app.lockedVi = -1;
    g_app.lockedFreq = 0;

    if (g_app.numVisible < 2) return;

    for (int vi = 0; vi < g_app.numVisible - 1; vi++) {
        int ci = g_app.visibleMap[vi];
        unsigned int lockFreq = displayed_curve_mhz(g_app.curve[ci].freq_kHz);
        if (lockFreq == 0) continue;

        bool hasTail = false;
        bool allSame = true;
        for (int j = vi + 1; j < g_app.numVisible; j++) {
            int cj = g_app.visibleMap[j];
            unsigned int freq = displayed_curve_mhz(g_app.curve[cj].freq_kHz);
            if (freq == 0) {
                allSame = false;
                break;
            }
            hasTail = true;
            if (freq != lockFreq) {
                allSame = false;
                break;
            }
        }

        if (hasTail && allSame) {
            g_app.lockedVi = vi;
            g_app.lockedFreq = lockFreq;
            return;
        }
    }
}

// ============================================================================
// UAC Elevation
// ============================================================================

static bool is_elevated() {
    HANDLE hToken = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &hToken))
        return false;

    TOKEN_ELEVATION elev = {};
    DWORD size = 0;
    bool result = false;
    if (GetTokenInformation(hToken, TokenElevation, &elev, sizeof(elev), &size)) {
        result = elev.TokenIsElevated != 0;
    }
    CloseHandle(hToken);
    return result;
}

static bool is_elevated_flag(LPWSTR lpCmdLine) {
    return wcsstr(lpCmdLine, L"--elevated") != nullptr;
}

static void request_elevation() {
    WCHAR path[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, path, MAX_PATH);

    SHELLEXECUTEINFOW sei = {};
    sei.cbSize = sizeof(sei);
    sei.lpVerb = L"runas";
    sei.lpFile = path;
    sei.lpParameters = L"--elevated";
    sei.nShow = SW_NORMAL;

    if (ShellExecuteExW(&sei)) {
        ExitProcess(0);
    }
    // If user cancelled, just continue without elevation
}

static void apply_system_titlebar_theme(HWND hwnd) {
    if (!hwnd) return;
    HMODULE d = LoadLibraryA("dwmapi.dll");
    if (!d) return;
    typedef HRESULT (WINAPI *DwmSetWindowAttribute_t)(HWND, DWORD, LPCVOID, DWORD);
    auto setAttr = (DwmSetWindowAttribute_t)GetProcAddress(d, "DwmSetWindowAttribute");
    if (setAttr) {
        DWORD lightValue = 1;
        DWORD type = 0, size = sizeof(lightValue);
        LONG useDark = 0;
        HKEY hKey = nullptr;
        if (RegOpenKeyExA(HKEY_CURRENT_USER,
                "Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                0, KEY_READ, &hKey) == ERROR_SUCCESS) {
            if (RegQueryValueExA(hKey, "AppsUseLightTheme", nullptr, (DWORD*)&type, (LPBYTE)&lightValue, &size) == ERROR_SUCCESS) {
                useDark = (lightValue == 0) ? 1 : 0;
            }
            RegCloseKey(hKey);
        }
        setAttr(hwnd, 20, &useDark, sizeof(useDark));
        setAttr(hwnd, 19, &useDark, sizeof(useDark));
    }
    FreeLibrary(d);
}

static void clear_debug_log_file() {
    if (!g_debug_logging) return;
    DeleteFileA(APP_LOG_FILE);
}

// ============================================================================
// GDI Graph Drawing
// ============================================================================

static void create_backbuffer(HWND hwnd) {
    HDC hdc = GetDC(hwnd);
    RECT rc;
    GetClientRect(hwnd, &rc);
    g_app.hMemDC = CreateCompatibleDC(hdc);
    g_app.hMemBmp = CreateCompatibleBitmap(hdc, rc.right, rc.bottom);
    g_app.hOldBmp = (HBITMAP)SelectObject(g_app.hMemDC, g_app.hMemBmp);
    ReleaseDC(hwnd, hdc);
}

static void destroy_backbuffer() {
    if (g_app.hMemDC) {
        SelectObject(g_app.hMemDC, g_app.hOldBmp);
        DeleteObject(g_app.hMemBmp);
        DeleteDC(g_app.hMemDC);
        g_app.hMemDC = nullptr;
    }
}

static void draw_graph(HDC hdc, RECT* rc) {
    int w = rc->right;
    int h = dp(GRAPH_HEIGHT);

    // Background
    HBRUSH bgBrush = CreateSolidBrush(COL_BG);
    RECT graphRc = {0, 0, w, h};
    FillRect(hdc, &graphRc, bgBrush);
    DeleteObject(bgBrush);

    if (!g_app.loaded) {
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, COL_TEXT);
        const char* msg = "Reading VF curve...";
        TextOutA(hdc, w / 2 - 60, h / 2 - 8, msg, (int)strlen(msg));
        return;
    }

    // Axis ranges
    const int MIN_VOLT_mV = 700;
    const int MAX_VOLT_mV = 1250;
    const int MIN_FREQ_MHz = 500;
    const int MAX_FREQ_MHz = 3400;

    // DPI-scaled margins
    int ml = dp(70), mr = dp(30), mt = dp(35), mb = dp(55);
    int pw = w - ml - mr;
    int ph = h - mt - mb;

    // Helper: map voltage mV to X pixel
    auto volt_to_x = [&](unsigned int mv) -> int {
        if (mv < (unsigned)MIN_VOLT_mV) mv = MIN_VOLT_mV;
        if (mv > (unsigned)MAX_VOLT_mV) mv = MAX_VOLT_mV;
        return ml + (int)((long long)(mv - MIN_VOLT_mV) * pw / (MAX_VOLT_mV - MIN_VOLT_mV));
    };

    // Helper: map frequency MHz to Y pixel
    auto freq_to_y = [&](unsigned int mhz) -> int {
        if (mhz < (unsigned)MIN_FREQ_MHz) mhz = MIN_FREQ_MHz;
        if (mhz > (unsigned)MAX_FREQ_MHz) mhz = MAX_FREQ_MHz;
        return mt + ph - (int)((long long)(mhz - MIN_FREQ_MHz) * ph / (MAX_FREQ_MHz - MIN_FREQ_MHz));
    };

    // GDI objects
    HPEN gridPen = CreatePen(PS_DOT, 1, COL_GRID);
    HPEN axisPen = CreatePen(PS_SOLID, dp(2), COL_AXIS);
    HPEN oldPen = (HPEN)SelectObject(hdc, gridPen);
    HFONT hFont = CreateFontA(dp(13), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, "Segoe UI");
    HFONT hFontSmall = CreateFontA(dp(11), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, "Segoe UI");
    HFONT oldFont = (HFONT)SelectObject(hdc, hFont);
    SetBkMode(hdc, TRANSPARENT);

    // Vertical grid lines (voltage axis, every 50mV, label every 100mV)
    for (int mv = MIN_VOLT_mV; mv <= MAX_VOLT_mV; mv += 50) {
            int x = volt_to_x((unsigned int)mv);
        SelectObject(hdc, gridPen);
        MoveToEx(hdc, x, mt, nullptr);
        LineTo(hdc, x, mt + ph);

        if (mv % 100 == 0) {
            SelectObject(hdc, hFontSmall);
            SetTextColor(hdc, COL_LABEL);
            char buf[16];
            StringCchPrintfA(buf, ARRAY_COUNT(buf), "%d", mv);
            SIZE sz;
            GetTextExtentPoint32A(hdc, buf, (int)strlen(buf), &sz);
            TextOutA(hdc, x - sz.cx / 2, mt + ph + dp(4), buf, (int)strlen(buf));
        }
    }

    // Horizontal grid lines (frequency axis, every 500MHz, label every 500MHz)
    for (int mhz = MIN_FREQ_MHz; mhz <= MAX_FREQ_MHz; mhz += 500) {
        int y = freq_to_y((unsigned int)mhz);
        SelectObject(hdc, gridPen);
        MoveToEx(hdc, ml, y, nullptr);
        LineTo(hdc, ml + pw, y);

        SelectObject(hdc, hFontSmall);
        SetTextColor(hdc, COL_LABEL);
        char buf[16];
        StringCchPrintfA(buf, ARRAY_COUNT(buf), "%d", mhz);
        SIZE sz;
        GetTextExtentPoint32A(hdc, buf, (int)strlen(buf), &sz);
        TextOutA(hdc, ml - sz.cx - dp(6), y - sz.cy / 2, buf, (int)strlen(buf));
    }

    // Axes
    SelectObject(hdc, axisPen);
    MoveToEx(hdc, ml, mt, nullptr);
    LineTo(hdc, ml, mt + ph);
    MoveToEx(hdc, ml, mt + ph, nullptr);
    LineTo(hdc, ml + pw, mt + ph);

    // Axis titles
    SelectObject(hdc, hFont);
    SetTextColor(hdc, COL_TEXT);
    const char* xTitle = "Voltage (mV)";
    SIZE sz;
    GetTextExtentPoint32A(hdc, xTitle, (int)strlen(xTitle), &sz);
    TextOutA(hdc, ml + pw / 2 - sz.cx / 2, mt + ph + dp(24), xTitle, (int)strlen(xTitle));

    const char* yTitle = "Frequency (MHz)";
    GetTextExtentPoint32A(hdc, yTitle, (int)strlen(yTitle), &sz);
    // Rotate for Y axis is hard in GDI, place horizontally left of Y labels
    TextOutA(hdc, dp(2), mt - dp(4), yTitle, (int)strlen(yTitle));

    // Build polyline: sort curve points by voltage, only plot within our ranges
    POINT pts[VF_NUM_POINTS];
    int nPts = 0;
    for (int i = 0; i < VF_NUM_POINTS; i++) {
        unsigned int freq_mhz = displayed_curve_mhz(g_app.curve[i].freq_kHz);
        unsigned int volt_mv = g_app.curve[i].volt_uV / 1000;
        if (freq_mhz == 0 && volt_mv == 0) continue;
        // Only plot points within our visible range
        if (volt_mv < (unsigned)MIN_VOLT_mV || volt_mv > (unsigned)MAX_VOLT_mV) continue;
        if (freq_mhz < (unsigned)MIN_FREQ_MHz || freq_mhz > (unsigned)MAX_FREQ_MHz) continue;
        pts[nPts].x = volt_to_x(volt_mv);
        pts[nPts].y = freq_to_y(freq_mhz);
        nPts++;
    }

    if (nPts > 1) {
        HPEN curvePen = CreatePen(PS_SOLID, dp(2), COL_CURVE);
        SelectObject(hdc, curvePen);
        Polyline(hdc, pts, nPts);
        SelectObject(hdc, oldPen);
        DeleteObject(curvePen);
    }

    // Data points (filled circles)
    HBRUSH ptBrush = CreateSolidBrush(COL_POINT);
    HBRUSH oldBrush = (HBRUSH)SelectObject(hdc, ptBrush);
    HPEN ptPen = CreatePen(PS_SOLID, 1, COL_POINT);
    SelectObject(hdc, ptPen);

    int r = dp(3);
    for (int i = 0; i < nPts; i++) {
        Ellipse(hdc, pts[i].x - r, pts[i].y - r, pts[i].x + r, pts[i].y + r);
    }

    SelectObject(hdc, oldBrush);
    SelectObject(hdc, oldPen);
    DeleteObject(ptBrush);
    DeleteObject(ptPen);

    // Frequency labels on curve (every 8 visible points)
    SelectObject(hdc, hFontSmall);
    SetTextColor(hdc, RGB(0xFF, 0xFF, 0x80));
    for (int i = 0; i < nPts; i += nvmax(1, nPts / 10)) {
        // Find original curve index for this point
        int visIdx = 0;
        for (int j = 0; j < VF_NUM_POINTS; j++) {
            unsigned int freq_mhz = displayed_curve_mhz(g_app.curve[j].freq_kHz);
            unsigned int volt_mv = g_app.curve[j].volt_uV / 1000;
            if (freq_mhz == 0 && volt_mv == 0) continue;
            if (volt_mv < (unsigned)MIN_VOLT_mV || volt_mv > (unsigned)MAX_VOLT_mV) continue;
            if (freq_mhz < (unsigned)MIN_FREQ_MHz || freq_mhz > (unsigned)MAX_FREQ_MHz) continue;
            if (visIdx == i) {
                char buf[32];
                StringCchPrintfA(buf, ARRAY_COUNT(buf), "%u", freq_mhz);
                SIZE sz2;
                GetTextExtentPoint32A(hdc, buf, (int)strlen(buf), &sz2);
                TextOutA(hdc, pts[i].x - sz2.cx / 2, pts[i].y - dp(16), buf, (int)strlen(buf));
                break;
            }
            visIdx++;
        }
    }

    // Info line at top
    SelectObject(hdc, hFont);
    SetTextColor(hdc, COL_TEXT);
    unsigned int actualMaxFreq = 0;
    unsigned int actualMaxVolt = 0;
    for (int i = 0; i < VF_NUM_POINTS; i++) {
        if (g_app.curve[i].freq_kHz > actualMaxFreq) {
            actualMaxFreq = g_app.curve[i].freq_kHz;
            actualMaxVolt = g_app.curve[i].volt_uV;
        }
    }
    char info[512];
    StringCchPrintfA(info, ARRAY_COUNT(info), "%s  |  %d pts  |  Peak: %u MHz @ %u mV",
                     g_app.gpuName, g_app.numPopulated,
                     displayed_curve_mhz(actualMaxFreq), actualMaxVolt / 1000);
    TextOutA(hdc, ml + dp(6), dp(4), info, (int)strlen(info));

    // Cleanup
    SelectObject(hdc, oldFont);
    DeleteObject(hFont);
    DeleteObject(hFontSmall);
    DeleteObject(gridPen);
    DeleteObject(axisPen);
}

// ============================================================================
// Edit Controls
// ============================================================================

static void set_edit_value(HWND hEdit, unsigned int value) {
    char buf[16];
    StringCchPrintfA(buf, ARRAY_COUNT(buf), "%u", value);
    SetWindowTextA(hEdit, buf);
}

static unsigned int get_edit_value(HWND hEdit) {
    char buf[16] = {};
    GetWindowTextA(hEdit, buf, sizeof(buf));
    return (unsigned int)strtoul(buf, nullptr, 10);
}

static void populate_edits() {
    for (int vi = 0; vi < g_app.numVisible; vi++) {
        int ci = g_app.visibleMap[vi];
        set_edit_value(g_app.hEditsMhz[vi], displayed_curve_mhz(g_app.curve[ci].freq_kHz));
        set_edit_value(g_app.hEditsMv[vi], g_app.curve[ci].volt_uV / 1000);
        SendMessageA(g_app.hEditsMhz[vi], EM_SETREADONLY, FALSE, 0);
        EnableWindow(g_app.hEditsMhz[vi], TRUE);
        EnableWindow(g_app.hEditsMv[vi], TRUE);
        SendMessageA(g_app.hLocks[vi], BM_SETCHECK, BST_UNCHECKED, 0);
        EnableWindow(g_app.hLocks[vi], TRUE);
        InvalidateRect(g_app.hLocks[vi], nullptr, TRUE);
    }
    // Re-apply lock state if active
    if (g_app.lockedVi >= 0 && g_app.lockedVi < g_app.numVisible) {
        SendMessageA(g_app.hLocks[g_app.lockedVi], BM_SETCHECK, BST_CHECKED, 0);
        set_edit_value(g_app.hEditsMhz[g_app.lockedVi], g_app.lockedFreq);
        for (int j = g_app.lockedVi + 1; j < g_app.numVisible; j++) {
            set_edit_value(g_app.hEditsMhz[j], g_app.lockedFreq);
            SendMessageA(g_app.hEditsMhz[j], EM_SETREADONLY, TRUE, 0);
            EnableWindow(g_app.hLocks[j], FALSE);
            InvalidateRect(g_app.hLocks[j], nullptr, TRUE);
        }
    }
    populate_global_controls();
}

static void apply_lock(int vi) {
    // Uncheck and re-enable previous lock
    if (g_app.lockedVi >= 0 && g_app.lockedVi < g_app.numVisible) {
        SendMessageA(g_app.hLocks[g_app.lockedVi], BM_SETCHECK, BST_UNCHECKED, 0);
        EnableWindow(g_app.hLocks[g_app.lockedVi], TRUE);
        InvalidateRect(g_app.hLocks[g_app.lockedVi], nullptr, TRUE);
    }

    // Check this one
    SendMessageA(g_app.hLocks[vi], BM_SETCHECK, BST_CHECKED, 0);
    g_app.lockedVi = vi;
    g_app.lockedFreq = get_edit_value(g_app.hEditsMhz[vi]);
    EnableWindow(g_app.hLocks[vi], TRUE);
    InvalidateRect(g_app.hLocks[vi], nullptr, TRUE);

    // Set all subsequent MHz fields to locked value, make read-only, disable lock checkboxes
    for (int j = vi + 1; j < g_app.numVisible; j++) {
        set_edit_value(g_app.hEditsMhz[j], g_app.lockedFreq);
        SendMessageA(g_app.hEditsMhz[j], EM_SETREADONLY, TRUE, 0);
        EnableWindow(g_app.hLocks[j], FALSE);
        InvalidateRect(g_app.hLocks[j], nullptr, TRUE);
    }
}

static void unlock_all() {
    g_app.lockedVi = -1;
    g_app.lockedFreq = 0;

    for (int vi = 0; vi < g_app.numVisible; vi++) {
        SendMessageA(g_app.hEditsMhz[vi], EM_SETREADONLY, FALSE, 0);
        int ci = g_app.visibleMap[vi];
        set_edit_value(g_app.hEditsMhz[vi], displayed_curve_mhz(g_app.curve[ci].freq_kHz));
        SendMessageA(g_app.hLocks[vi], BM_SETCHECK, BST_UNCHECKED, 0);
        EnableWindow(g_app.hLocks[vi], TRUE);
        InvalidateRect(g_app.hLocks[vi], nullptr, TRUE);
    }
}

static void create_edit_controls(HWND hParent, HINSTANCE hInst) {
    int cbW = dp(16);
    int editW = dp(65);
    int labelW = dp(32);
    int gap = dp(2);
    int rowH = dp(20);
    int headerH = dp(16);
    // Layout: [#] [☑] [MHz edit] [mV edit]
    int colW = labelW + cbW + editW + editW + gap * 3 + dp(8);
    int numCols = 6;
    int rowsPerCol = (g_app.numVisible + numCols - 1) / numCols;

    int graphH = dp(GRAPH_HEIGHT);
    int startY = graphH + dp(14);

    // Column headers
    for (int col = 0; col < numCols; col++) {
        int x = dp(8) + col * colW;
        int y = startY - headerH - dp(2);

        // Header: "Lk" over checkbox area, "MHz" over MHz edit, "mV" over mV edit
        CreateWindowExA(0, "STATIC", "Lk",
            WS_CHILD | WS_VISIBLE | SS_CENTER,
            x + labelW + gap, y, cbW, headerH,
            hParent, nullptr, hInst, nullptr);
        CreateWindowExA(0, "STATIC", "MHz",
            WS_CHILD | WS_VISIBLE | SS_CENTER,
            x + labelW + cbW + gap * 2, y, editW, headerH,
            hParent, nullptr, hInst, nullptr);
        CreateWindowExA(0, "STATIC", "mV",
            WS_CHILD | WS_VISIBLE | SS_CENTER,
            x + labelW + cbW + gap * 2 + editW + gap, y, editW, headerH,
            hParent, nullptr, hInst, nullptr);
    }

    for (int vi = 0; vi < g_app.numVisible; vi++) {
        int ci = g_app.visibleMap[vi];
        int col = vi / rowsPerCol;
        int row = vi % rowsPerCol;
        int x = dp(8) + col * colW;
        int y = startY + row * rowH;
        
        // Point label
        char label[16];
        StringCchPrintfA(label, ARRAY_COUNT(label), "%3d", ci);
        CreateWindowExA(0, "STATIC", label,
            WS_CHILD | WS_VISIBLE | SS_RIGHT,
            x, y + dp(1), labelW - gap, rowH - dp(2),
            hParent, nullptr, hInst, nullptr);

        // Lock checkbox (after point number)
        g_app.hLocks[vi] = CreateWindowExA(
            0, "BUTTON", "",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
            x + labelW, y + dp(1), cbW, rowH - dp(2),
            hParent, (HMENU)(INT_PTR)(LOCK_BASE_ID + vi), hInst, nullptr);

        // MHz edit
        g_app.hEditsMhz[vi] = CreateWindowExA(
            WS_EX_CLIENTEDGE, "EDIT", "0",
            WS_CHILD | WS_VISIBLE | ES_NUMBER | ES_RIGHT | ES_AUTOHSCROLL,
            x + labelW + cbW + gap * 2, y, editW, rowH - dp(2),
            hParent, (HMENU)(INT_PTR)(1000 + vi), hInst, nullptr);

        // mV edit (read-only)
        g_app.hEditsMv[vi] = CreateWindowExA(
            WS_EX_CLIENTEDGE, "EDIT", "0",
            WS_CHILD | WS_VISIBLE | ES_RIGHT | ES_AUTOHSCROLL | ES_READONLY,
            x + labelW + cbW + gap * 2 + editW + gap, y, editW, rowH - dp(2),
            hParent, (HMENU)(INT_PTR)(1000 + VF_NUM_POINTS + vi), hInst, nullptr);
    }

    // Global control fields below edits
    int ocY = startY + rowsPerCol * rowH + dp(6);
    int fieldW = dp(78);

    CreateWindowExA(0, "STATIC", "GPU Offset (MHz):",
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        dp(8), ocY + dp(2), dp(126), dp(18),
        hParent, nullptr, hInst, nullptr);
    g_app.hGpuOffsetEdit = CreateWindowExA(
        WS_EX_CLIENTEDGE, "EDIT", "0",
        WS_CHILD | WS_VISIBLE | ES_RIGHT | ES_AUTOHSCROLL,
        dp(136), ocY, fieldW, dp(20),
        hParent, (HMENU)(INT_PTR)GPU_OFFSET_ID, hInst, nullptr);

    CreateWindowExA(0, "STATIC", "Mem Offset (MHz):",
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        dp(230), ocY + dp(2), dp(126), dp(18),
        hParent, nullptr, hInst, nullptr);
    g_app.hMemOffsetEdit = CreateWindowExA(
        WS_EX_CLIENTEDGE, "EDIT", "0",
        WS_CHILD | WS_VISIBLE | ES_RIGHT | ES_AUTOHSCROLL,
        dp(358), ocY, fieldW, dp(20),
        hParent, (HMENU)(INT_PTR)MEM_OFFSET_ID, hInst, nullptr);

    CreateWindowExA(0, "STATIC", "Power Limit (%):",
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        dp(452), ocY + dp(2), dp(100), dp(18),
        hParent, nullptr, hInst, nullptr);
    g_app.hPowerLimitEdit = CreateWindowExA(
        WS_EX_CLIENTEDGE, "EDIT", "100",
        WS_CHILD | WS_VISIBLE | ES_NUMBER | ES_RIGHT | ES_AUTOHSCROLL,
        dp(552), ocY, fieldW, dp(20),
        hParent, (HMENU)(INT_PTR)POWER_LIMIT_ID, hInst, nullptr);

    CreateWindowExA(0, "STATIC", "Fan (%/auto):",
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        dp(650), ocY + dp(2), dp(98), dp(18),
        hParent, nullptr, hInst, nullptr);
    g_app.hFanEdit = CreateWindowExA(
        WS_EX_CLIENTEDGE, "EDIT", "auto",
        WS_CHILD | WS_VISIBLE | ES_RIGHT | ES_AUTOHSCROLL,
        dp(748), ocY, dp(86), dp(20),
        hParent, (HMENU)(INT_PTR)FAN_CONTROL_ID, hInst, nullptr);

    layout_bottom_buttons(hParent);

    if (g_app.loaded) populate_global_controls();

    if (g_app.loaded) populate_edits();

    sync_startup_checkbox_from_system();
}

// ============================================================================
// Main Window
// ============================================================================

static void apply_changes() {
    if (!g_app.loaded) return;
    DesiredSettings desired = {};
    char err[256] = {};
    if (!capture_gui_apply_settings(&desired, err, sizeof(err))) {
        MessageBoxA(g_app.hMainWnd, err, "Green Curve", MB_OK | MB_ICONERROR);
        return;
    }
    char result[256] = {};
    bool ok = apply_desired_settings(&desired, true, result, sizeof(result));
    MessageBoxA(g_app.hMainWnd, result, "Green Curve", MB_OK | (ok ? MB_ICONINFORMATION : MB_ICONWARNING));
}

static void destroy_edit_controls(HWND hParent) {
    HWND child = GetWindow(hParent, GW_CHILD);
    while (child) {
        HWND next = GetWindow(child, GW_HWNDNEXT);
        LONG_PTR id = GetWindowLongPtr(child, GWLP_ID);
        if (id != APPLY_BTN_ID && id != REFRESH_BTN_ID && id != RESET_BTN_ID && id != SAVE_CFG_BTN_ID && id != LICENSE_BTN_ID && id != STARTUP_APPLY_ID) {
            DestroyWindow(child);
        }
        child = next;
    }
    for (int i = 0; i < VF_NUM_POINTS; i++) {
        g_app.hEditsMhz[i] = nullptr;
        g_app.hEditsMv[i] = nullptr;
        g_app.hLocks[i] = nullptr;
    }
    g_app.hGpuOffsetEdit = nullptr;
    g_app.hMemOffsetEdit = nullptr;
    g_app.hPowerLimitEdit = nullptr;
    g_app.hFanEdit = nullptr;
}

static void refresh_curve() {
    if (nvapi_read_curve() && nvapi_read_offsets()) {
        rebuild_visible_map();
        detect_locked_tail_from_curve();
        char detail[128] = {};
        refresh_global_state(detail, sizeof(detail));

        // Recreate edit controls for new visible set
        destroy_edit_controls(g_app.hMainWnd);
        create_edit_controls(g_app.hMainWnd, g_app.hInst);
        InvalidateRect(g_app.hMainWnd, nullptr, TRUE);

        debug_log("Green Curve: Refreshed - %d points loaded\n", g_app.numPopulated);
        for (int i = 0; i < VF_NUM_POINTS && i < 10; i++) {
            if (g_app.curve[i].freq_kHz > 0) {
                debug_log("  Point %d: %u MHz @ %u mV (offset: %d kHz)\n",
                    i, displayed_curve_mhz(g_app.curve[i].freq_kHz),
                    g_app.curve[i].volt_uV / 1000,
                    g_app.freqOffsets[i]);
            }
        }
    } else {
        debug_log("Green Curve: Failed to read VF curve\n");
    }
}

static void reset_curve() {
    if (!g_app.loaded) return;

    // Explicitly set freqDelta = 0 for every populated point
    // This directly writes zero offset instead of computing an inverse
    auto getFunc = (NvApiFunc)nvapi_qi(VF_GET_CONTROL_ID);
    auto setFunc = (NvApiFunc)nvapi_qi(VF_SET_CONTROL_ID);
    auto getInfo = (NvApiFunc)nvapi_qi(VF_GET_INFO_ID);

    if (!getFunc || !setFunc) {
        MessageBoxA(g_app.hMainWnd, "NvAPI functions not available.", "Green Curve", MB_OK | MB_ICONERROR);
        return;
    }

    unsigned char mask[32] = {};
    if (getInfo) {
        unsigned char ibuf[0x182C] = {};
        const unsigned int version = (1u << 16) | 0x182C;
        memcpy(&ibuf[0], &version, sizeof(version));
        memset(&ibuf[4], 0xFF, 32);
        if (getInfo(g_app.gpuHandle, ibuf) == 0) {
            memcpy(mask, &ibuf[4], 32);
        } else {
            memset(mask, 0xFF, 16);
        }
    } else {
        memset(mask, 0xFF, 16);
    }

    const unsigned int CTRL_SIZE = 0x2420;
    int successCount = 0;
    int failCount = 0;

    for (int ci = 0; ci < VF_NUM_POINTS; ci++) {
        if (g_app.curve[ci].freq_kHz == 0) continue;
        if (g_app.freqOffsets[ci] == 0) continue;

        // Read current control state
        unsigned char buf[0x2420] = {};
        const unsigned int version = (1u << 16) | CTRL_SIZE;
        memcpy(&buf[0], &version, sizeof(version));
        memcpy(&buf[4], mask, 32);
        int ret = getFunc(g_app.gpuHandle, buf);
        if (ret != 0) { failCount++; continue; }

        // Set single-bit mask and freqDelta = 0
        memset(&buf[4], 0, 32);
        buf[4 + ci / 8] = (unsigned char)(1 << (ci % 8));
        {
            const int zero = 0;
            memcpy(&buf[0x44 + ci * 0x24 + 0x14], &zero, sizeof(zero));
        }

        ret = setFunc(g_app.gpuHandle, buf);
        if (ret == 0) successCount++; else failCount++;
        Sleep(2);
    }

    // Zero offsets in memory
    memset(g_app.freqOffsets, 0, sizeof(g_app.freqOffsets));
    Sleep(100);

    // Re-read to confirm
    nvapi_read_curve();
    nvapi_read_offsets();
    rebuild_visible_map();
    detect_locked_tail_from_curve();

    // Reset global controls to defaults
    nvapi_set_gpu_offset(0);
    nvapi_set_mem_offset(0);
    nvapi_set_power_limit(100);
    char detail[128] = {};
    nvml_set_fan_auto(detail, sizeof(detail));
    refresh_global_state(detail, sizeof(detail));

    // Recreate edit controls
    destroy_edit_controls(g_app.hMainWnd);
    create_edit_controls(g_app.hMainWnd, g_app.hInst);
    InvalidateRect(g_app.hMainWnd, nullptr, TRUE);

    char msg[128];
    StringCchPrintfA(msg, ARRAY_COUNT(msg), "Reset %d offsets to 0 (%d failed).", successCount, failCount);
    MessageBoxA(g_app.hMainWnd, msg, "Green Curve", MB_OK | MB_ICONINFORMATION);
}

// ============================================================================
// Main Window
// ============================================================================

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_CREATE:
            create_backbuffer(hwnd);
            apply_system_titlebar_theme(hwnd);
            ensure_main_window_min_size(hwnd);
            layout_bottom_buttons(hwnd);
            return 0;

        case WM_SIZE: {
            destroy_backbuffer();
            create_backbuffer(hwnd);
            layout_bottom_buttons(hwnd);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }

        case WM_ERASEBKGND:
            return 1;

        case WM_SETTINGCHANGE:
        case WM_THEMECHANGED:
            apply_system_titlebar_theme(hwnd);
            break;

        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);
            RECT rc;
            GetClientRect(hwnd, &rc);

            int graphH = dp(GRAPH_HEIGHT);

            HBRUSH bg = CreateSolidBrush(COL_BG);
            FillRect(g_app.hMemDC, &rc, bg);
            DeleteObject(bg);

            draw_graph(g_app.hMemDC, &rc);

            HPEN sepPen = CreatePen(PS_SOLID, 1, COL_GRID);
            HPEN oldPen = (HPEN)SelectObject(g_app.hMemDC, sepPen);
            MoveToEx(g_app.hMemDC, 0, graphH, nullptr);
            LineTo(g_app.hMemDC, rc.right, graphH);
            SelectObject(g_app.hMemDC, oldPen);
            DeleteObject(sepPen);

            BitBlt(hdc, 0, 0, rc.right, rc.bottom, g_app.hMemDC, 0, 0, SRCCOPY);
            EndPaint(hwnd, &ps);
            return 0;
        }

        case WM_DRAWITEM: {
            const DRAWITEMSTRUCT* dis = (const DRAWITEMSTRUCT*)lParam;
            if (dis && dis->CtlType == ODT_BUTTON && dis->CtlID >= LOCK_BASE_ID && dis->CtlID < LOCK_BASE_ID + VF_NUM_POINTS) {
                draw_lock_checkbox(dis);
                return TRUE;
            }
            return FALSE;
        }

        case WM_COMMAND:
            if (LOWORD(wParam) == APPLY_BTN_ID) {
                apply_changes();
            } else if (LOWORD(wParam) == REFRESH_BTN_ID) {
                refresh_curve();
            } else if (LOWORD(wParam) == RESET_BTN_ID) {
                reset_curve();
            } else if (LOWORD(wParam) == SAVE_CFG_BTN_ID) {
                DesiredSettings desired = {};
                char err[256] = {};
                if (!capture_gui_config_settings(&desired, err, sizeof(err))) {
                    MessageBoxA(g_app.hMainWnd, err, "Green Curve", MB_OK | MB_ICONERROR);
                } else if (save_current_gui_state_to_config(startup_checkbox_checked() ? CONFIG_STARTUP_ENABLE : CONFIG_STARTUP_DISABLE,
                           err, sizeof(err))) {
                    if (startup_checkbox_checked() && !set_startup_task_enabled(true, err, sizeof(err))) {
                        SendMessageA(g_app.hStartupApplyCheck, BM_SETCHECK, BST_UNCHECKED, 0);
                        MessageBoxA(g_app.hMainWnd, err, "Green Curve", MB_OK | MB_ICONERROR);
                        break;
                    }
                    MessageBoxA(g_app.hMainWnd, "Config saved to config.ini.", "Green Curve", MB_OK | MB_ICONINFORMATION);
                } else {
                    MessageBoxA(g_app.hMainWnd, err, "Green Curve", MB_OK | MB_ICONERROR);
                }
            } else if (LOWORD(wParam) == STARTUP_APPLY_ID) {
                char err[256] = {};
                bool enable = startup_checkbox_checked();
                if (enable) {
                    if (!save_current_gui_state_for_startup(err, sizeof(err)) ||
                        !set_startup_task_enabled(true, err, sizeof(err))) {
                        SendMessageA(g_app.hStartupApplyCheck, BM_SETCHECK, BST_UNCHECKED, 0);
                        MessageBoxA(g_app.hMainWnd, err, "Green Curve", MB_OK | MB_ICONERROR);
                    }
                } else {
                    if (!save_desired_to_config_with_startup(g_app.configPath, nullptr, true, CONFIG_STARTUP_DISABLE, err, sizeof(err)) ||
                        !set_startup_task_enabled(false, err, sizeof(err))) {
                        SendMessageA(g_app.hStartupApplyCheck, BM_SETCHECK, BST_CHECKED, 0);
                        MessageBoxA(g_app.hMainWnd, err, "Green Curve", MB_OK | MB_ICONERROR);
                    }
                }
            } else if (LOWORD(wParam) >= LOCK_BASE_ID && LOWORD(wParam) < LOCK_BASE_ID + VF_NUM_POINTS) {
                // Lock checkbox clicked
                int vi = LOWORD(wParam) - LOCK_BASE_ID;
                if (vi == g_app.lockedVi) {
                    SendMessageA(g_app.hLocks[vi], BM_SETCHECK, BST_UNCHECKED, 0);
                    InvalidateRect(g_app.hLocks[vi], nullptr, TRUE);
                    unlock_all();
                } else {
                    SendMessageA(g_app.hLocks[vi], BM_SETCHECK, BST_CHECKED, 0);
                    InvalidateRect(g_app.hLocks[vi], nullptr, TRUE);
                    apply_lock(vi);
                }
            } else if (LOWORD(wParam) == LICENSE_BTN_ID) {
                show_license_dialog(g_app.hMainWnd);
            }
            return 0;

        case WM_GETMINMAXINFO: {
            MINMAXINFO* mmi = (MINMAXINFO*)lParam;
            if (mmi) {
                SIZE minSize = main_window_min_size();
                mmi->ptMinTrackSize.x = minSize.cx;
                mmi->ptMinTrackSize.y = minSize.cy;
            }
            return 0;
        }

        case WM_CTLCOLOREDIT: {
            HDC hdcEdit = (HDC)wParam;
            SetTextColor(hdcEdit, COL_TEXT);
            SetBkColor(hdcEdit, RGB(0x1A, 0x1A, 0x2A));
            static HBRUSH hEditBr = CreateSolidBrush(RGB(0x1A, 0x1A, 0x2A));
            return (LRESULT)hEditBr;
        }

        case WM_CTLCOLORSTATIC: {
            HDC hdcStatic = (HDC)wParam;
            SetTextColor(hdcStatic, COL_LABEL);
            SetBkColor(hdcStatic, RGB(0x22, 0x22, 0x32));
            static HBRUSH hBr = CreateSolidBrush(RGB(0x22, 0x22, 0x32));
            return (LRESULT)hBr;
        }

        case WM_DESTROY:
            destroy_backbuffer();
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcA(hwnd, msg, wParam, lParam);
}

// ============================================================================
// Entry Point
// ============================================================================

static void dump_curve_to_file(const char* path) {
    if (!g_debug_logging) return;
    char err[256] = {};
    if (!write_log_snapshot(path, err, sizeof(err))) {
        debug_log("Failed to write log snapshot: %s\n", err);
    }
}

// CLI mode: --dump or --json
// Returns true if CLI handled (should exit), false if should run GUI
static bool handle_cli(LPWSTR wCmdLine) {
    CliOptions opts = {};
    if (!parse_cli_options(wCmdLine, &opts)) {
        char err[256] = {};
        const char* text = opts.error[0] ? opts.error : "Failed to parse CLI";
        write_text_file_atomic(APP_CLI_LOG_FILE, text, strlen(text), err, sizeof(err));
        return true;
    }
    if (!opts.recognized) return false;
    set_default_config_path();
    if (opts.hasConfigPath) StringCchCopyA(g_app.configPath, ARRAY_COUNT(g_app.configPath), opts.configPath);

    // Elevate if needed (VF curve read requires admin)
    if (!is_elevated()) {
        WCHAR path[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, path, MAX_PATH);
        SHELLEXECUTEINFOW sei = {};
        sei.cbSize = sizeof(sei);
        sei.lpVerb = L"runas";
        sei.lpFile = path;
        sei.lpParameters = wCmdLine;  // pass through all args
        sei.nShow = SW_HIDE;  // no window flash
        sei.fMask = SEE_MASK_NOCLOSEPROCESS;
        if (ShellExecuteExW(&sei) && sei.hProcess) {
            WaitForSingleObject(sei.hProcess, INFINITE);
            CloseHandle(sei.hProcess);
        }
        return true;
    }

    // CLI always writes to file since we're a GUI subsystem app
    const char* logPath = APP_CLI_LOG_FILE;
    FILE* logf = fopen(logPath, "w");
    if (!logf) return false;

    #define CLI_LOG(...) do { fprintf(logf, __VA_ARGS__); fflush(logf); } while(0)

    CLI_LOG("Green Curve CLI mode started\n");

    if (opts.showHelp) {
        CLI_LOG("Green Curve v0.1 - NVIDIA Blackwell VF Curve Editor\n");
        CLI_LOG("Usage:\n");
        CLI_LOG("  greencurve.exe              Launch GUI\n");
        CLI_LOG("  greencurve.exe --dump       Write VF curve to greencurve_cli_log.txt\n");
        CLI_LOG("  greencurve.exe --json       Write VF curve to greencurve_curve.json\n");
        CLI_LOG("  greencurve.exe --probe      Probe NvAPI/NVML control support\n");
        CLI_LOG("  greencurve.exe --gpu-offset <mhz> --mem-offset <mhz> --power-limit <pct>\n");
        CLI_LOG("  greencurve.exe --fan <auto|0-100> --point49 <mhz> ... --point126 <mhz>\n");
        CLI_LOG("  greencurve.exe --apply-config [--config <path>]\n");
        CLI_LOG("  greencurve.exe --save-config [--config <path>]\n");
        CLI_LOG("  greencurve.exe --reset      Reset curve/global controls to defaults\n");
        CLI_LOG("  greencurve.exe --help       This help\n");
        fclose(logf);
        DeleteFileA(APP_CLI_LOG_FILE);
        return true;
    }

    CLI_LOG("Green Curve: Initializing NvAPI...\n");

    if (!nvapi_init()) {
        CLI_LOG("ERROR: Failed to initialize NvAPI.\n");
        fclose(logf);
        return true;
    }
    CLI_LOG("Green Curve: NvAPI initialized.\n");

    if (!nvapi_enum_gpu()) {
        CLI_LOG("ERROR: No NVIDIA GPU found.\n");
        fclose(logf);
        return true;
    }
    CLI_LOG("Green Curve: GPU enumerated.\n");

    nvapi_get_name();
    CLI_LOG("Green Curve: GPU name: %s\n", g_app.gpuName);

    CLI_LOG("Green Curve: Reading VF curve...\n");
    if (!nvapi_read_curve()) {
        CLI_LOG("ERROR: Failed to read VF curve.\n");
        fclose(logf);
        return true;
    }
    CLI_LOG("Green Curve: VF curve read OK - %d populated points.\n", g_app.numPopulated);

    CLI_LOG("Green Curve: Reading VF offsets...\n");
    if (!nvapi_read_offsets()) {
        CLI_LOG("WARNING: Failed to read VF offsets (non-fatal).\n");
    } else {
        CLI_LOG("Green Curve: VF offsets read OK.\n");
    }

    // Read global OC/PL values
    {
        char detail[128] = {};
        refresh_global_state(detail, sizeof(detail));
    }

    if (opts.applyConfig) {
        DesiredSettings cfg = {};
        char err[256] = {};
        if (!load_desired_settings_from_ini(g_app.configPath, &cfg, err, sizeof(err))) {
            CLI_LOG("ERROR: %s\n", err);
            fclose(logf);
            return true;
        }
        merge_desired_settings(&cfg, &opts.desired);
        char result[256] = {};
        bool ok = apply_desired_settings(&cfg, false, result, sizeof(result));
        CLI_LOG("%s\n", result);
        if (!ok) {
            fclose(logf);
            return true;
        }
    } else if (desired_has_any_action(&opts.desired)) {
        char result[256] = {};
        bool ok = apply_desired_settings(&opts.desired, false, result, sizeof(result));
        CLI_LOG("%s\n", result);
        if (!ok) {
            fclose(logf);
            return true;
        }
    }

    if (opts.reset) {
        for (int ci = 0; ci < VF_NUM_POINTS; ci++) {
            if (g_app.curve[ci].freq_kHz == 0) continue;
            if (g_app.freqOffsets[ci] == 0) continue;
            nvapi_set_point(ci, 0);
            Sleep(2);
        }
        nvapi_set_gpu_offset(0);
        nvapi_set_mem_offset(0);
        nvapi_set_power_limit(100);
        char detail[128] = {};
        nvml_set_fan_auto(detail, sizeof(detail));
        refresh_global_state(detail, sizeof(detail));
        CLI_LOG("Reset applied.\n");
    }

    if (opts.saveConfig) {
        DesiredSettings saveDesired = {};
        bool useDesired = false;
        if (opts.applyConfig) {
            if (!load_desired_settings_from_ini(g_app.configPath, &saveDesired, opts.error, sizeof(opts.error))) {
                CLI_LOG("ERROR: %s\n", opts.error);
                fclose(logf);
                return true;
            }
            merge_desired_settings(&saveDesired, &opts.desired);
            useDesired = true;
        } else if (desired_has_any_action(&opts.desired)) {
            saveDesired = opts.desired;
            useDesired = true;
        }
        char err[256] = {};
        if (!save_desired_to_config(g_app.configPath, useDesired ? &saveDesired : nullptr, !useDesired, err, sizeof(err))) {
            CLI_LOG("ERROR: %s\n", err);
            fclose(logf);
            return true;
        }
        CLI_LOG("Config written to %s\n", g_app.configPath);
    }

    if (opts.dump) {
        CLI_LOG("\n--- VF Curve Table ---\n");
        CLI_LOG("GPU offset: %d MHz\n", g_app.gpuClockOffsetkHz / 1000);
        CLI_LOG("Mem offset: %d MHz\n", mem_display_mhz_from_driver_khz(g_app.memClockOffsetkHz));
        CLI_LOG("Power limit: %d%%\n", g_app.powerLimitPct);
        if (g_app.fanSupported) {
            CLI_LOG("Fan mode: %s\n", g_app.fanIsAuto ? "auto" : "manual");
            for (unsigned int fan = 0; fan < g_app.fanCount; fan++) {
                CLI_LOG("  Fan %u: %u%% / %u RPM / policy=%u\n", fan, g_app.fanPercent[fan], g_app.fanRpm[fan], g_app.fanPolicy[fan]);
            }
        } else {
            CLI_LOG("Fan mode: unsupported\n");
        }
        CLI_LOG("\n");
        CLI_LOG("%-6s  %-10s  %-10s  %-12s\n", "Point", "Freq(MHz)", "Volt(mV)", "Offset(kHz)");
        CLI_LOG("------  ----------  ----------  ------------\n");
        for (int i = 0; i < VF_NUM_POINTS; i++) {
            if (g_app.curve[i].freq_kHz > 0 || g_app.curve[i].volt_uV > 0) {
                CLI_LOG("%-6d  %-10u  %-10u  %-12d\n",
                    i,
                    displayed_curve_mhz(g_app.curve[i].freq_kHz),
                    g_app.curve[i].volt_uV / 1000,
                    g_app.freqOffsets[i]);
            }
        }
    }

    if (opts.probe) {
        CLI_LOG("\n=== NvAPI Probe: OC/PL Functions ===\n\n");

        // Helper: try a function with given ID, size, and version
        auto probe_func = [&](unsigned int id, const char* name, unsigned int structSize, unsigned int version) {
            auto func = (NvApiFunc)nvapi_qi(id);
            if (!func) {
                CLI_LOG("[%08X] %-40s  NOT FOUND\n", id, name);
                return -99;
            }
            unsigned char buf[0x4000] = {};
            const unsigned int header = (version << 16) | structSize;
            memcpy(&buf[0], &header, sizeof(header));
            if (structSize > 0x44) memset(&buf[4], 0xFF, 32);
            int ret = func(g_app.gpuHandle, buf);
            if (ret == 0) {
                CLI_LOG("[%08X] %-40s  OK (ver=0x%04X size=0x%04X)\n", id, name, version, structSize);
                // Dump first 64 bytes
                CLI_LOG("  Data: ");
                for (int i = 0; i < 64; i++) CLI_LOG("%02X ", buf[i]);
                CLI_LOG("\n");
            } else {
                CLI_LOG("[%08X] %-40s  ERR %d (0x%08X) (ver=%u size=0x%X)\n",
                    id, name, ret, (unsigned int)ret, version, structSize);
            }
            return ret;
        };

        // --- Power Policies ---
        CLI_LOG("--- Power Limit ---\n");
        // GetPowerPoliciesInfo (0x34206D86)
        probe_func(0x34206D86u, "PowerPoliciesGetInfo", 0x28, 1);
        // GetPowerPoliciesStatus (0x355C8B8C)
        probe_func(0x355C8B8C, "PowerPoliciesGetStatus", 0x50, 1);
        // SetPowerPoliciesStatus (0xAD95F5ED) - just probe get, don't set
        auto setPL = (NvApiFunc)nvapi_qi(0xAD95F5ED);
        CLI_LOG("[%08X] %-40s  %s\n", 0xAD95F5EDu, "PowerPoliciesSetStatus",
            setPL ? "FOUND" : "NOT FOUND");

        // --- Pstates (Global Clock Offset) ---
        CLI_LOG("\n--- Pstates (Global Clock Offset) ---\n");
        // Try Pstates20 get with various sizes
        const unsigned int psSizes[] = {0x0008, 0x0018, 0x0048, 0x00B0, 0x01C8, 0x0410,
                                         0x0840, 0x1098, 0x1C94, 0x2420, 0x3000};
        for (unsigned int sz : psSizes) {
            int r = probe_func(0x6FF81213u, "GPU_GetPstates20", sz, 2);
            if (r == 0) break;
            r = probe_func(0x6FF81213u, "GPU_GetPstates20", sz, 3);
            if (r == 0) break;
        }

        // Try Pstates20 set existence
        auto setPS = (NvApiFunc)nvapi_qi(0x0F4DAE6B);
        CLI_LOG("[%08X] %-40s  %s\n", 0x0F4DAE6Bu, "GPU_SetPstates20",
            setPS ? "FOUND" : "NOT FOUND");

        // Try GetPstatesInfoEx (0x6048B02F)
        for (unsigned int sz : psSizes) {
            int r = probe_func(0x6048B02Fu, "GPU_GetPstatesInfoEx", sz, 1);
            if (r == 0) break;
        }

        // --- VRAM Clock (clock domain 4 = memory) ---
        CLI_LOG("\n--- VRAM Clock ---\n");
        // Try VF points for memory domain
        auto getStatus = (NvApiFunc)nvapi_qi(VF_GET_STATUS_ID);
        if (getStatus) {
            // Try with different mask for mem domain
            for (unsigned int dom = 1; dom < 16; dom++) {
                unsigned char buf[VF_BUFFER_SIZE] = {};
                const unsigned int version = (1u << 16) | VF_BUFFER_SIZE;
                memcpy(&buf[0], &version, sizeof(version));
                memset(&buf[4], 0xFF, 32);
                memcpy(&buf[0x24], &dom, sizeof(dom));
                int ret = getStatus(g_app.gpuHandle, buf);
                if (ret == 0) {
                    unsigned int f0 = 0, v0 = 0;
                    memcpy(&f0, &buf[VF_ENTRIES_OFFSET], sizeof(f0));
                    memcpy(&v0, &buf[VF_ENTRIES_OFFSET + 4], sizeof(v0));
                    if (f0 > 0 || v0 > 0) {
                        CLI_LOG("VF GetStatus dom=%2u: OK, point0=%u kHz / %u uV\n", dom, f0, v0);
                    }
                }
            }
        }

        // Try common VRAM clock functions
        probe_func(0x507B4B59u, "VF_GetInfo (already known)", 0x182C, 1);

        char detail[128] = {};
        CLI_LOG("\n--- NVML Global Controls ---\n");
        if (nvml_read_clock_offsets(detail, sizeof(detail))) {
            CLI_LOG("Graphics offset: %d MHz (range %d..%d, pstate %d)\n",
                g_app.gpuClockOffsetkHz / 1000, g_app.gpuClockOffsetMinMHz, g_app.gpuClockOffsetMaxMHz, g_app.offsetReadPstate);
            CLI_LOG("Memory offset: %d MHz (range %d..%d)\n",
                mem_display_mhz_from_driver_khz(g_app.memClockOffsetkHz), g_app.memClockOffsetMinMHz, g_app.memClockOffsetMaxMHz);
        } else {
            CLI_LOG("Clock offsets: %s\n", detail);
        }
        if (nvml_read_fans(detail, sizeof(detail))) {
            CLI_LOG("Fans: %u (range %u..%u), mode=%s\n", g_app.fanCount, g_app.fanMinPct, g_app.fanMaxPct, g_app.fanIsAuto ? "auto" : "manual");
            for (unsigned int fan = 0; fan < g_app.fanCount; fan++) {
                CLI_LOG("  Fan %u: pct=%u rpm=%u policy=%u signal=%u target=0x%X\n",
                    fan, g_app.fanPercent[fan], g_app.fanRpm[fan], g_app.fanPolicy[fan], g_app.fanControlSignal[fan], g_app.fanTargetMask[fan]);
            }
        } else {
            CLI_LOG("Fans: %s\n", detail);
        }
        CLI_LOG("\n=== Probe complete ===\n");
    }

    if (opts.json) {
        char err[256] = {};
        if (write_json_snapshot(APP_JSON_FILE, err, sizeof(err))) {
            CLI_LOG("JSON written to %s\n", APP_JSON_FILE);
        } else {
            CLI_LOG("ERROR: %s\n", err);
        }
    }

    CLI_LOG("\nGreen Curve CLI done.\n");
    fclose(logf);
    DeleteFileA(APP_CLI_LOG_FILE);
    #undef CLI_LOG
    return true;
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE /*hPrev*/, LPSTR /*lpCmdLine*/, int nCmdShow) {
    LPWSTR wCmdLine = GetCommandLineW();

    g_debug_logging = (GetEnvironmentVariableA(APP_DEBUG_ENV, nullptr, 0) > 0);

    // CLI mode - handle --dump, --json, --help
    if (handle_cli(wCmdLine)) {
        return 0;
    }

    // Initialize DPI awareness
    SetProcessDPIAware();
    init_dpi();

    // GUI mode: Check UAC elevation
    if (!is_elevated() && !is_elevated_flag(wCmdLine)) {
        request_elevation();
    }

    g_app.hInst = hInstance;
    set_default_config_path();

    // Init NvAPI
    if (!nvapi_init()) {
        MessageBoxA(nullptr, "Failed to initialize NvAPI.\nIs an NVIDIA GPU and driver installed?",
                     "Green Curve - Error", MB_OK | MB_ICONERROR);
        return 1;
    }

    if (!nvapi_enum_gpu()) {
        MessageBoxA(nullptr, "No NVIDIA GPU found.", "Green Curve - Error", MB_OK | MB_ICONERROR);
        return 1;
    }

    nvapi_get_name();

    // Read initial curve
    bool curveOk = nvapi_read_curve();
    nvapi_read_offsets();

    if (!curveOk) {
        MessageBoxA(nullptr,
            "Failed to read VF curve from GPU.\n"
            "This may require administrator privileges or a supported GPU.",
            "Green Curve - Error", MB_OK | MB_ICONERROR);
        return 1;
    }

    clear_debug_log_file();
    dump_curve_to_file(APP_LOG_FILE);

    // Build visible map after initial read
    rebuild_visible_map();
    detect_locked_tail_from_curve();

    // Read global OC/PL/fan values
    {
        char detail[128] = {};
        refresh_global_state(detail, sizeof(detail));
    }

    // Register window class
    auto load_app_icon = [hInstance](int cx, int cy) -> HICON {
        HICON icon = (HICON)LoadImageA(hInstance, MAKEINTRESOURCEA(APP_ICON_ID), IMAGE_ICON, cx, cy, LR_SHARED);
        if (!icon) icon = LoadIcon(nullptr, IDI_APPLICATION);
        return icon;
    };

    WNDCLASSEXA wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = APP_CLASS_NAME;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    wc.hIcon = load_app_icon(GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON));
    wc.hIconSm = load_app_icon(GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON));
    wc.style = 0;  // no CS_HREDRAW/CS_VREDRAW to reduce flicker

    if (!RegisterClassExA(&wc)) {
        MessageBoxA(nullptr, "Failed to register window class.", "Green Curve", MB_OK | MB_ICONERROR);
        return 1;
    }

    // Create main window
    SIZE initialSize = main_window_min_size();
    int winW = initialSize.cx;
    int winH = initialSize.cy;
    g_app.hMainWnd = CreateWindowExA(
        0, APP_CLASS_NAME,
        APP_TITLE,
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
        CW_USEDEFAULT, CW_USEDEFAULT,
        winW, winH,
        nullptr, nullptr, hInstance, nullptr
    );

    if (!g_app.hMainWnd) {
        MessageBoxA(nullptr, "Failed to create window.", "Green Curve", MB_OK | MB_ICONERROR);
        return 1;
    }

    SendMessageA(g_app.hMainWnd, WM_SETICON, ICON_BIG, (LPARAM)wc.hIcon);
    SendMessageA(g_app.hMainWnd, WM_SETICON, ICON_SMALL, (LPARAM)wc.hIconSm);

    // Create buttons (positioned by create_edit_controls)
    g_app.hApplyBtn = CreateWindowExA(
        0, "BUTTON", "Apply Changes",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        0, 0, dp(110), dp(30),
        g_app.hMainWnd, (HMENU)(INT_PTR)APPLY_BTN_ID, hInstance, nullptr
    );

    g_app.hRefreshBtn = CreateWindowExA(
        0, "BUTTON", "Refresh",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        0, 0, dp(90), dp(30),
        g_app.hMainWnd, (HMENU)(INT_PTR)REFRESH_BTN_ID, hInstance, nullptr
    );

    g_app.hResetBtn = CreateWindowExA(
        0, "BUTTON", "Reset",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        0, 0, dp(90), dp(30),
        g_app.hMainWnd, (HMENU)(INT_PTR)RESET_BTN_ID, hInstance, nullptr
    );

    g_app.hLicenseBtn = CreateWindowExA(
        0, "BUTTON", "License",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        0, 0, dp(80), dp(30),
        g_app.hMainWnd, (HMENU)(INT_PTR)LICENSE_BTN_ID, hInstance, nullptr
    );

    g_app.hSaveCfgBtn = CreateWindowExA(
        0, "BUTTON", "Save To Config",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        0, 0, dp(130), dp(30),
        g_app.hMainWnd, (HMENU)(INT_PTR)SAVE_CFG_BTN_ID, hInstance, nullptr
    );

    g_app.hStartupApplyCheck = CreateWindowExA(
        0, "BUTTON", "Apply saved config on startup",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
        0, 0, dp(220), dp(20),
        g_app.hMainWnd, (HMENU)(INT_PTR)STARTUP_APPLY_ID, hInstance, nullptr
    );

    sync_startup_checkbox_from_system();

    layout_bottom_buttons(g_app.hMainWnd);

    // Create edit controls
    create_edit_controls(g_app.hMainWnd, hInstance);

    ShowWindow(g_app.hMainWnd, nCmdShow);
    UpdateWindow(g_app.hMainWnd);

    // Message loop
    MSG msg = {};
    while (GetMessageA(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }

    close_nvml();

    return (int)msg.wParam;
}
