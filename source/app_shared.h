#ifndef GREEN_CURVE_APP_SHARED_H
#define GREEN_CURVE_APP_SHARED_H

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0600
#endif
#ifndef _WIN32_IE
#define _WIN32_IE 0x0600
#endif

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <commctrl.h>
#include <sddl.h>
#include <shlobj.h>
#include <shellapi.h>
#include <strsafe.h>
#include <wtsapi32.h>
#include <ctype.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ARRAY_COUNT(a) (sizeof(a) / sizeof((a)[0]))

int nvmin(int a, int b);
int nvmax(int a, int b);

extern int g_dpi;
extern float g_scale;
extern CRITICAL_SECTION g_configLock;

int dp(int px);
void init_dpi();

#define VF_NUM_POINTS       128
#define NVAPI_INIT_ID       0x0150E828u
#define NVAPI_ENUM_GPU_ID   0xE5AC921Fu
#define NVAPI_GET_NAME_ID   0xCEEE8E9Fu
#define NVAPI_GET_INTERFACE_VERSION_STRING_ID 0x01053FA5u
#define NVAPI_GET_ERROR_MESSAGE_ID 0x6C2D048Cu
#define NVAPI_GPU_GET_PCI_IDENTIFIERS_ID 0x2DDFB66Eu
#define NVAPI_GPU_GET_ARCH_INFO_ID 0xD8265D24u

#define WINDOW_WIDTH        1180
#define WINDOW_HEIGHT       800
#define GRAPH_HEIGHT        420
#define APP_ICON_ID         101
#define TRAY_ICON_DEFAULT_ID 111
#define TRAY_ICON_OC_ID     112
#define TRAY_ICON_FAN_ID    113
#define TRAY_ICON_OC_FAN_ID 114
#define APP_NAME            "Green Curve"
#define APP_VERSION         "0.11"
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
#define PROFILE_STATUS_ID   2030
#define START_ON_LOGON_CHECK_ID 2031
#define FAN_MODE_COMBO_ID   2032
#define FAN_CURVE_BTN_ID    2033
#define GPU_OFFSET_EXCLUDE_LOW_CHECK_ID 2034
#define LOGON_HINT_ID       2035
#define START_ON_LOGON_LABEL_ID 2036
#define SERVICE_ENABLE_CHECK_ID 2037
#define SERVICE_ENABLE_LABEL_ID 2038
#define SERVICE_STATUS_ID   2039
#define LOCK_BASE_ID        3000
#define GPU_OFFSET_ID       2010
#define MEM_OFFSET_ID       2011
#define POWER_LIMIT_ID      2012
#define FAN_CONTROL_ID      2013
#define TRAY_MENU_SHOW_ID   2100
#define TRAY_MENU_EXIT_ID   2101

#define FAN_CURVE_TIMER_ID  1
#define FAN_TELEMETRY_TIMER_ID 2
#define FAN_CURVE_MAX_POINTS 8
#define FAN_CURVE_MAX_HYSTERESIS_C 10

#define MAX_GPU_FANS        8
#define CONFIG_FILE_NAME    "config.ini"
#define STARTUP_TASK_PREFIX "Green Curve Startup - "
#define CONFIG_NUM_SLOTS    5
#define CONFIG_DEFAULT_SLOT 1
#define NVML_PERF_STR_LEN   2048

#define MIN_VISIBLE_VOLT_mV 700
#define MIN_VISIBLE_FREQ_MHz 500

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
    NVML_TEMPERATURE_GPU = 0,
};

enum {
    NVAPI_GPU_PUBLIC_CLOCK_GRAPHICS = 0,
    NVAPI_GPU_PUBLIC_CLOCK_MEMORY = 4,
};

enum {
    NVAPI_GPU_PERF_PSTATE20_CLOCK_TYPE_SINGLE = 0,
    NVAPI_GPU_PERF_PSTATE20_CLOCK_TYPE_RANGE = 1,
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
#define NVAPI_STRUCT_VERSION(type, ver) (unsigned int)(sizeof(type) | ((ver) << 16U))

#define NVAPI_MAX_GPU_PSTATE20_PSTATES 16
#define NVAPI_MAX_GPU_PSTATE20_CLOCKS 8
#define NVAPI_MAX_GPU_PSTATE20_BASE_VOLTAGES 4

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

typedef struct {
    int value;
    struct {
        int min;
        int max;
    } valueRange;
} nvapiPstates20ParamDelta_t;

typedef struct {
    unsigned int freq_kHz;
} nvapiPstate20SingleClock_t;

typedef struct {
    unsigned int minFreq_kHz;
    unsigned int maxFreq_kHz;
    unsigned int domainId;
    unsigned int minVoltage_uV;
    unsigned int maxVoltage_uV;
} nvapiPstate20RangeClock_t;

typedef union {
    nvapiPstate20SingleClock_t single;
    nvapiPstate20RangeClock_t range;
} nvapiPstate20ClockData_t;

typedef struct {
    unsigned int domainId;
    unsigned int typeId;
    unsigned int bIsEditable:1;
    unsigned int reserved:31;
    nvapiPstates20ParamDelta_t freqDelta_kHz;
    nvapiPstate20ClockData_t data;
} nvapiPstate20ClockEntry_t;

typedef struct {
    unsigned int domainId;
    unsigned int bIsEditable:1;
    unsigned int reserved:31;
    unsigned int volt_uV;
    nvapiPstates20ParamDelta_t voltDelta_uV;
} nvapiPstate20BaseVoltageEntry_t;

typedef struct {
    unsigned int pstateId;
    unsigned int bIsEditable:1;
    unsigned int reserved:31;
    nvapiPstate20ClockEntry_t clocks[NVAPI_MAX_GPU_PSTATE20_CLOCKS];
    nvapiPstate20BaseVoltageEntry_t baseVoltages[NVAPI_MAX_GPU_PSTATE20_BASE_VOLTAGES];
} nvapiPstate20Entry_t;
static_assert(sizeof(nvapiPstate20Entry_t) == 456, "nvapiPstate20Entry_t size mismatch");

typedef struct {
    unsigned int numVoltages;
    nvapiPstate20BaseVoltageEntry_t voltages[NVAPI_MAX_GPU_PSTATE20_BASE_VOLTAGES];
} nvapiPstates20Ov_t;

typedef struct {
    unsigned int version;
    unsigned int bIsEditable:1;
    unsigned int reserved:31;
    unsigned int numPstates;
    unsigned int numClocks;
    unsigned int numBaseVoltages;
    nvapiPstate20Entry_t pstates[NVAPI_MAX_GPU_PSTATE20_PSTATES];
    nvapiPstates20Ov_t ov;
} nvapiPerfPstates20Info_t;
static_assert(sizeof(nvapiPerfPstates20Info_t) == 7416, "nvapiPerfPstates20Info_t size mismatch");

#define NVAPI_PERF_PSTATES20_INFO_VER2 NVAPI_STRUCT_VERSION(nvapiPerfPstates20Info_t, 2)
#define NVAPI_PERF_PSTATES20_INFO_VER3 NVAPI_STRUCT_VERSION(nvapiPerfPstates20Info_t, 3)

typedef enum {
    NV_GPU_ARCHITECTURE_UNKNOWN = 0,
    NV_GPU_ARCHITECTURE_GP100 = 0x00000130,
    NV_GPU_ARCHITECTURE_TU100 = 0x00000160,
    NV_GPU_ARCHITECTURE_GA100 = 0x00000170,
    NV_GPU_ARCHITECTURE_AD100 = 0x00000190,
    NV_GPU_ARCHITECTURE_GB200 = 0x000001B0,
} NvGpuArchitectureId;

typedef struct {
    unsigned int version;
    unsigned int architecture;
    unsigned int implementation;
    unsigned int revision;
} nvapiGpuArchInfo_t;
static_assert(sizeof(nvapiGpuArchInfo_t) == 16, "nvapiGpuArchInfo_t size mismatch");

#define NVAPI_GPU_ARCH_INFO_VER2 NVAPI_STRUCT_VERSION(nvapiGpuArchInfo_t, 2)

typedef enum {
    GPU_FAMILY_UNKNOWN = 0,
    GPU_FAMILY_PASCAL = 1,
    GPU_FAMILY_TURING = 2,
    GPU_FAMILY_AMPERE = 3,
    GPU_FAMILY_LOVELACE = 4,
    GPU_FAMILY_BLACKWELL = 5,
} GpuFamily;

typedef struct {
    const char* name;
    GpuFamily family;
    bool supported;
    bool readSupported;
    bool writeSupported;
    bool bestGuessOnly;
    unsigned int getStatusId;
    unsigned int getInfoId;
    unsigned int getControlId;
    unsigned int setControlId;
    unsigned int statusBufferSize;
    unsigned int statusVersion;
    unsigned int statusMaskOffset;
    unsigned int statusNumClocksOffset;
    unsigned int statusEntriesOffset;
    unsigned int statusEntryStride;
    unsigned int infoBufferSize;
    unsigned int infoVersion;
    unsigned int infoMaskOffset;
    unsigned int infoNumClocksOffset;
    unsigned int controlBufferSize;
    unsigned int controlVersion;
    unsigned int controlMaskOffset;
    unsigned int controlEntryBaseOffset;
    unsigned int controlEntryStride;
    unsigned int controlEntryDeltaOffset;
    unsigned int defaultNumClocks;
} VfBackendSpec;

struct VFCurvePoint {
    unsigned int freq_kHz;
    unsigned int volt_uV;
};

enum {
    FAN_MODE_AUTO = 0,
    FAN_MODE_FIXED = 1,
    FAN_MODE_CURVE = 2,
};

enum {
    TRAY_ICON_STATE_DEFAULT = 0,
    TRAY_ICON_STATE_OC = 1,
    TRAY_ICON_STATE_FAN = 2,
    TRAY_ICON_STATE_OC_FAN = 3,
};

struct FanCurvePoint {
    bool enabled;
    int temperatureC;
    int fanPercent;
};

struct FanCurveConfig {
    FanCurvePoint points[FAN_CURVE_MAX_POINTS];
    int pollIntervalMs;
    int hysteresisC;
};

struct ControlState {
    bool valid;
    bool hasGpuOffset;
    int gpuOffsetMHz;
    bool gpuOffsetExcludeLow70;
    bool hasMemOffset;
    int memOffsetMHz;
    bool hasPowerLimit;
    int powerLimitPct;
    bool hasFan;
    int fanMode;
    int fanFixedPercent;
    int fanCurrentPercent;
    int fanCurrentTemperatureC;
    FanCurveConfig fanCurve;
};

struct AppData {
    HINSTANCE hInst;
    HWND hMainWnd;
    HWND hEditsMhz[VF_NUM_POINTS];
    HWND hEditsMv[VF_NUM_POINTS];
    HWND hLocks[VF_NUM_POINTS];
    HWND hApplyBtn;
    HWND hRefreshBtn;
    HWND hResetBtn;
    HWND hLicenseBtn;
    HWND hGpuOffsetEdit;
    HWND hGpuOffsetExcludeLowCheck;
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
    HWND hAppLaunchLabel;
    HWND hLogonLabel;
    HWND hProfileStatusLabel;
    HWND hStartOnLogonCheck;
    HWND hStartOnLogonLabel;
    HWND hLogonHintLabel;
    HWND hServiceEnableCheck;
    HWND hServiceEnableLabel;
    HWND hServiceStatusLabel;

    HBRUSH hWindowClassBrush;
    HANDLE hStartupSyncThread;
    bool startupSyncInFlight;
    HDC hMemDC;
    HBITMAP hMemBmp;
    HBITMAP hOldBmp;

    HMODULE hNvApi;
    GPU_HANDLE gpuHandle;
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
    int numPopulated;
    bool loaded;

    int visibleMap[VF_NUM_POINTS];
    int numVisible;

    int lockedVi;
    int lockedCi;
    unsigned int lockedFreq;
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
    int guiGpuOffsetMHz;
    bool guiGpuOffsetExcludeLow70;
    int appliedGpuOffsetMHz;
    bool appliedGpuOffsetExcludeLow70;

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

    bool launchedFromLogon;
    bool startHiddenToTray;
    bool isServiceProcess;
    bool usingBackgroundService;
    bool backgroundServiceInstalled;
    bool backgroundServiceRunning;
    bool backgroundServiceAvailable;
    bool backgroundServiceBroken;
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

struct DesiredSettings {
    bool hasCurvePoint[VF_NUM_POINTS];
    unsigned int curvePointMHz[VF_NUM_POINTS];
    bool hasLock;
    int lockCi;
    unsigned int lockMHz;
    bool lockTracksAnchor;
    bool hasGpuOffset;
    int gpuOffsetMHz;
    bool gpuOffsetExcludeLow70;
    bool hasMemOffset;
    int memOffsetMHz;
    bool hasPowerLimit;
    int powerLimitPct;
    bool hasFan;
    bool fanAuto;
    int fanMode;
    int fanPercent;
    FanCurveConfig fanCurve;
};

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
    bool hasConfigPath;
    char configPath[MAX_PATH];
    char probeOutputPath[MAX_PATH];
    char error[256];
    DesiredSettings desired;
};

enum {
    SERVICE_PROTOCOL_MAGIC = 0x47535643u,
    SERVICE_PROTOCOL_VERSION = 2,
};

enum ServiceCommand {
    SERVICE_CMD_NONE = 0,
    SERVICE_CMD_PING = 1,
    SERVICE_CMD_GET_SNAPSHOT = 2,
    SERVICE_CMD_GET_TELEMETRY = 3,
    SERVICE_CMD_APPLY = 4,
    SERVICE_CMD_RESET = 5,
    SERVICE_CMD_GET_ACTIVE_DESIRED = 6,
    SERVICE_CMD_WRITE_LOG_SNAPSHOT = 7,
    SERVICE_CMD_WRITE_JSON_SNAPSHOT = 8,
    SERVICE_CMD_WRITE_PROBE_REPORT = 9,
};

enum ServiceResponseStatus {
    SERVICE_STATUS_OK = 0,
    SERVICE_STATUS_ERROR = 1,
    SERVICE_STATUS_VERSION_MISMATCH = 2,
};

struct ServiceSnapshot {
    bool initialized;
    bool loaded;
    bool fanSupported;
    bool fanRangeKnown;
    bool fanIsAuto;
    bool fanCurveRuntimeActive;
    bool fanFixedRuntimeActive;
    bool gpuOffsetRangeKnown;
    bool memOffsetRangeKnown;
    bool curveOffsetRangeKnown;
    bool gpuTemperatureValid;
    bool vfReadSupported;
    bool vfWriteSupported;
    bool vfBestGuess;
    GpuFamily gpuFamily;
    int numPopulated;
    int gpuClockOffsetkHz;
    int memClockOffsetkHz;
    int gpuClockOffsetMinMHz;
    int gpuClockOffsetMaxMHz;
    int memOffsetMinMHz;
    int memOffsetMaxMHz;
    int curveOffsetMinkHz;
    int curveOffsetMaxkHz;
    int powerLimitPct;
    int powerLimitDefaultmW;
    int powerLimitCurrentmW;
    int powerLimitMinmW;
    int powerLimitMaxmW;
    int appliedGpuOffsetMHz;
    bool appliedGpuOffsetExcludeLow70;
    int activeFanMode;
    int activeFanFixedPercent;
    int gpuTemperatureC;
    unsigned int fanCount;
    unsigned int fanMinPct;
    unsigned int fanMaxPct;
    unsigned int fanPercent[MAX_GPU_FANS];
    unsigned int fanTargetPercent[MAX_GPU_FANS];
    unsigned int fanRpm[MAX_GPU_FANS];
    unsigned int fanPolicy[MAX_GPU_FANS];
    unsigned int fanControlSignal[MAX_GPU_FANS];
    unsigned int fanTargetMask[MAX_GPU_FANS];
    VFCurvePoint curve[VF_NUM_POINTS];
    int freqOffsets[VF_NUM_POINTS];
    FanCurveConfig activeFanCurve;
    char gpuName[256];
    char ownerUser[256];
    DWORD ownerSessionId;
    ULONGLONG ownerUtcMs;
};

struct ServiceRequest {
    DWORD magic;
    DWORD version;
    DWORD command;
    DWORD flags;
    DWORD callerPid;
    DWORD callerSessionId;
    DesiredSettings desired;
    char source[64];
    char path[MAX_PATH];
};

struct ServiceResponse {
    DWORD magic;
    DWORD version;
    DWORD status;
    DWORD reserved;
    ServiceSnapshot snapshot;
    DesiredSettings desired;
    ControlState controlState;
    char message[512];
};

enum {
    GUI_FAN_MODE_UNSET = -1,
};

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
typedef nvmlReturn_t (*nvmlDeviceGetTemperature_t)(nvmlDevice_t, unsigned int, unsigned int*);

enum {
    NVML_CLOCK_ID_CURRENT = 0,
    NVML_CLOCK_ID_APP_CLOCK_TARGET = 1,
    NVML_CLOCK_ID_APP_CLOCK_DEFAULT = 2,
    NVML_CLOCK_ID_CUSTOMER_BOOST_MAX = 3,
};

typedef nvmlReturn_t (*nvmlDeviceGetClock_t)(nvmlDevice_t, unsigned int, unsigned int, unsigned int*);
typedef nvmlReturn_t (*nvmlDeviceGetMaxClock_t)(nvmlDevice_t, unsigned int, unsigned int*);

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
    nvmlDeviceGetTemperature_t getTemperature;
    nvmlDeviceGetClock_t getClock;
    nvmlDeviceGetMaxClock_t getMaxClock;
};

extern AppData g_app;
extern NvmlApi g_nvml_api;
extern HMODULE g_nvml;
extern bool g_debug_logging;

typedef int (*NvApiFunc)(void*, void*);

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

#endif
