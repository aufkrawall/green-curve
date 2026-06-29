// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// gpu_core.h — platform-neutral GPU control data model shared by the Windows
// and Linux backends.  Contains ONLY portable declarations: NVAPI entry-point
// IDs + private struct layouts, NVML typedefs/enums/structs, the VF-curve data
// model, the GPU family dispatch (VfBackendSpec), the DesiredSettings request
// model + its IPC trust-boundary validator, and the binary service protocol
// (ServiceRequest/ServiceResponse).
//
// This header pulls in NO OS headers (not <windows.h>, not POSIX): the NVAPI
// IDs and struct layouts are driver-ABI constants identical on every OS, so the
// same definitions drive nvapi64.dll on Windows and libnvidia-api.so.1 on
// Linux.  Win32-typed application state (AppData, CliOptions, UI handles) lives
// in app_shared.h, not here.

#ifndef GREEN_CURVE_GPU_CORE_H
#define GREEN_CURVE_GPU_CORE_H

#include <stddef.h>
#include <stdint.h>

// NVAPI/NVML buffers are parsed as little-endian fixed-width fields.  arm64 is
// LE for our targets, but assert it so a future big-endian target fails loudly
// at compile time rather than silently mis-reading the GPU.
#if defined(__BYTE_ORDER__) && defined(__ORDER_LITTLE_ENDIAN__)
static_assert(__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__,
              "gpu_core.h assumes a little-endian target (NVAPI/NVML buffer parsing)");
#endif

// ---------------------------------------------------------------------------
// Fixed-width aliases for the binary IPC protocol.
//
// The wire structs historically used Win32 DWORD/ULONGLONG and MAX_PATH.  To
// stay byte-identical on Windows (where DWORD == `unsigned long`, 32-bit under
// LLP64) AND correctly sized on Linux (where `unsigned long` is 64-bit), we
// alias to `unsigned long` on Windows — the exact type DWORD already is, so all
// existing %lu format strings and DWORD assignments keep compiling — and to
// uint32_t on Linux.  Both are 32-bit, so the struct layout matches across OSes.
// ---------------------------------------------------------------------------
#if defined(_WIN32)
typedef unsigned long gc_u32;       // identical to Win32 DWORD
#else
typedef uint32_t gc_u32;
#endif
typedef uint8_t gc_u8;
typedef unsigned long long gc_u64;  // identical to Win32 ULONGLONG
typedef gc_u8 gc_bool8;
static_assert(sizeof(gc_bool8) == 1, "gc_bool8 must stay a one-byte wire flag");

static inline gc_bool8 gc_bool8_from_bool(bool value) {
    return value ? (gc_bool8)1u : (gc_bool8)0u;
}

static inline bool gc_bool8_is_canonical(gc_bool8 value) {
    return value == 0u || value == 1u;
}

static inline void canonicalize_gc_bool8(gc_bool8* value) {
    if (value) *value = *value ? (gc_bool8)1u : (gc_bool8)0u;
}

// ServiceRequest.path is a fixed 260-byte field (== MAX_PATH on Windows); keep
// the size constant so the protocol layout is stable across platforms.
#define GC_REQUEST_PATH_MAX 260

// ---------------------------------------------------------------------------
// Core constants
// ---------------------------------------------------------------------------
#define VF_NUM_POINTS       128
#define NVAPI_INIT_ID       0x0150E828u
#define NVAPI_ENUM_GPU_ID   0xE5AC921Fu
#define NVAPI_GET_NAME_ID   0xCEEE8E9Fu
#define NVAPI_GET_INTERFACE_VERSION_STRING_ID 0x01053FA5u
#define NVAPI_GET_ERROR_MESSAGE_ID 0x6C2D048Cu
#define NVAPI_GPU_GET_PCI_IDENTIFIERS_ID 0x2DDFB66Eu
#define NVAPI_GPU_GET_ARCH_INFO_ID 0xD8265D24u

#define FAN_CURVE_MAX_POINTS 8
#define FAN_CURVE_MAX_HYSTERESIS_C 10
#define MAX_GPU_FANS        8
#define MAX_GPU_ADAPTERS    8
#define CONFIG_NUM_SLOTS    5
#define CONFIG_DEFAULT_SLOT 1
#define NVML_PERF_STR_LEN   2048

#define MIN_VISIBLE_VOLT_mV 700
#define MIN_VISIBLE_FREQ_MHz 500

// ---------------------------------------------------------------------------
// Lock mode (tri-state: none / flatten-tail / hard NVML pin)
//
// Fixed underlying type so every int bit pattern is a valid enum value.  This
// matters at the IPC trust boundary: validate_desired_settings_for_ipc() reads
// and clamps lockMode from caller-supplied bytes, and reading an out-of-range
// value of an enum *without* a fixed underlying type is itself undefined
// behavior (UBSan flags it), defeating the sanitization.
// ---------------------------------------------------------------------------
enum LockMode : int {
    LOCK_MODE_NONE = 0,
    LOCK_MODE_FLATTEN = 1,
    LOCK_MODE_HARD = 2,
};

inline const char* lock_mode_name(LockMode m) {
    switch (m) {
        case LOCK_MODE_FLATTEN: return "flatten";
        case LOCK_MODE_HARD: return "hard";
        default: return "none";
    }
}

// ---------------------------------------------------------------------------
// NVML public types / enums
// ---------------------------------------------------------------------------
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

// ---------------------------------------------------------------------------
// NVAPI private pstates20 layout
// ---------------------------------------------------------------------------
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

// Field-offset pins for the private NVAPI structs.  The sizeof() asserts above
// catch a total-size change; these offsetof() asserts additionally pin the
// internal layout — in particular the bitfield placement (bIsEditable:1 /
// reserved:31), which is the one spot where the AArch64 (AAPCS64) ABI could
// theoretically pack differently from x86-64.  Because the arm64 binary is
// cross-compiled here, any divergence fails `python build.py` rather than
// silently mis-reading the GPU on real hardware.
static_assert(offsetof(nvapiPstate20ClockEntry_t, freqDelta_kHz) == 12, "nvapi clock-entry layout (arm64?)");
static_assert(offsetof(nvapiPstate20ClockEntry_t, data) == 24, "nvapi clock-entry layout (arm64?)");
static_assert(sizeof(nvapiPstate20ClockEntry_t) == 44, "nvapi clock-entry size (arm64?)");
static_assert(offsetof(nvapiPstate20BaseVoltageEntry_t, volt_uV) == 8, "nvapi base-voltage layout (arm64?)");
static_assert(offsetof(nvapiPstate20BaseVoltageEntry_t, voltDelta_uV) == 12, "nvapi base-voltage layout (arm64?)");
static_assert(sizeof(nvapiPstate20BaseVoltageEntry_t) == 24, "nvapi base-voltage size (arm64?)");
static_assert(offsetof(nvapiPstate20Entry_t, clocks) == 8, "nvapi pstate-entry layout (arm64?)");
static_assert(offsetof(nvapiPstate20Entry_t, baseVoltages) == 360, "nvapi pstate-entry layout (arm64?)");
static_assert(offsetof(nvapiPerfPstates20Info_t, pstates) == 20, "nvapi pstates20 layout (arm64?)");
static_assert(offsetof(nvapiPerfPstates20Info_t, ov) == 7316, "nvapi pstates20 layout (arm64?)");
static_assert(offsetof(nvapiGpuArchInfo_t, architecture) == 4, "nvapi arch-info layout (arm64?)");
static_assert(offsetof(nvmlClockOffset_v1_t, clockOffsetMHz) == 12, "nvml clock-offset layout (arm64?)");
static_assert(sizeof(nvmlClockOffset_v1_t) == 24, "nvml clock-offset size (arm64?)");

typedef enum {
    GPU_FAMILY_UNKNOWN = 0,
    GPU_FAMILY_PASCAL = 1,
    GPU_FAMILY_TURING = 2,
    GPU_FAMILY_AMPERE = 3,
    GPU_FAMILY_LOVELACE = 4,
    GPU_FAMILY_BLACKWELL = 5,
} GpuFamily;

// ---------------------------------------------------------------------------
// VF backend dispatch + curve data model
// ---------------------------------------------------------------------------
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
    gc_bool8 enabled;
    int temperatureC;
    int fanPercent;
};

struct FanCurveConfig {
    FanCurvePoint points[FAN_CURVE_MAX_POINTS];
    int pollIntervalMs;
    int hysteresisC;
};

struct ControlState {
    gc_bool8 valid;
    gc_bool8 hasGpuOffset;
    int gpuOffsetMHz;
    int gpuOffsetExcludeLowCount;
    gc_bool8 hasMemOffset;
    int memOffsetMHz;
    gc_bool8 hasPowerLimit;
    int powerLimitPct;
    gc_bool8 hasFan;
    int fanMode;
    int fanFixedPercent;
    int fanCurrentPercent;
    int fanCurrentTemperatureC;
    FanCurveConfig fanCurve;
};

struct GpuAdapterInfo {
    gc_bool8 valid;
    gc_bool8 pciInfoValid;
    gc_bool8 vfReadSupported;
    gc_bool8 vfWriteSupported;
    gc_bool8 vfBestGuess;
    unsigned int nvapiIndex;
    unsigned int nvmlIndex;
    unsigned int deviceId;
    unsigned int subSystemId;
    unsigned int pciRevisionId;
    unsigned int extDeviceId;
    unsigned int pciDomain;
    unsigned int pciBus;
    unsigned int pciDevice;
    unsigned int pciFunction;
    GpuFamily family;
    char name[128];
};

struct DesiredSettings {
    gc_bool8 hasCurvePoint[VF_NUM_POINTS];
    unsigned int curvePointMHz[VF_NUM_POINTS];
    gc_bool8 hasLock;
    int lockCi;
    unsigned int lockMHz;
    LockMode lockMode;
    gc_bool8 lockTracksAnchor;
    gc_bool8 hasGpuOffset;
    int gpuOffsetMHz;
    int gpuOffsetExcludeLowCount;
    gc_bool8 hasMemOffset;
    int memOffsetMHz;
    gc_bool8 hasPowerLimit;
    int powerLimitPct;
    gc_bool8 hasFan;
    gc_bool8 fanAuto;
    int fanMode;
    int fanPercent;
    FanCurveConfig fanCurve;
    gc_bool8 resetOcBeforeApply;
};

static inline void validate_fan_curve_flags_for_ipc(FanCurveConfig* c) {
    if (!c) return;
    for (int i = 0; i < FAN_CURVE_MAX_POINTS; i++) {
        canonicalize_gc_bool8(&c->points[i].enabled);
    }
}

static inline void validate_gpu_adapter_info_for_ipc(GpuAdapterInfo* g) {
    if (!g) return;
    canonicalize_gc_bool8(&g->valid);
    canonicalize_gc_bool8(&g->pciInfoValid);
    canonicalize_gc_bool8(&g->vfReadSupported);
    canonicalize_gc_bool8(&g->vfWriteSupported);
    canonicalize_gc_bool8(&g->vfBestGuess);
}

static inline void validate_control_state_for_ipc(ControlState* c) {
    if (!c) return;
    canonicalize_gc_bool8(&c->valid);
    canonicalize_gc_bool8(&c->hasGpuOffset);
    canonicalize_gc_bool8(&c->hasMemOffset);
    canonicalize_gc_bool8(&c->hasPowerLimit);
    canonicalize_gc_bool8(&c->hasFan);
    validate_fan_curve_flags_for_ipc(&c->fanCurve);
}

// Sanitize a DesiredSettings struct received over IPC.  This is the single
// trust boundary between an unprivileged caller and the privileged service:
// every numeric field that can reach an array index, a hardware write, or a
// runtime loop MUST be range-checked here.  Downstream code also guards the
// index fields, but completing the clamps at the boundary is defense in depth
// (CWE-20) so a malformed or hostile request can never drive out-of-range
// behavior even if a future downstream guard is dropped.
static inline void validate_desired_settings_for_ipc(DesiredSettings* d) {
    if (!d) return;
    for (int ci = 0; ci < VF_NUM_POINTS; ci++) {
        canonicalize_gc_bool8(&d->hasCurvePoint[ci]);
    }
    canonicalize_gc_bool8(&d->hasLock);
    canonicalize_gc_bool8(&d->lockTracksAnchor);
    canonicalize_gc_bool8(&d->hasGpuOffset);
    canonicalize_gc_bool8(&d->hasMemOffset);
    canonicalize_gc_bool8(&d->hasPowerLimit);
    canonicalize_gc_bool8(&d->hasFan);
    canonicalize_gc_bool8(&d->fanAuto);
    canonicalize_gc_bool8(&d->resetOcBeforeApply);
    validate_fan_curve_flags_for_ipc(&d->fanCurve);
    for (int ci = 0; ci < VF_NUM_POINTS; ci++) {
        if (d->curvePointMHz[ci] > 5000u) d->curvePointMHz[ci] = 5000u;
    }
    if (d->hasPowerLimit && (d->powerLimitPct < 50 || d->powerLimitPct > 150)) {
        d->powerLimitPct = d->powerLimitPct < 50 ? 50 : 150;
    }
    if (d->hasGpuOffset && (d->gpuOffsetMHz < -1000 || d->gpuOffsetMHz > 1000)) {
        d->gpuOffsetMHz = d->gpuOffsetMHz < -1000 ? -1000 : 1000;
    }
    if (d->hasMemOffset && (d->memOffsetMHz < -5000 || d->memOffsetMHz > 5000)) {
        d->memOffsetMHz = d->memOffsetMHz < -5000 ? -5000 : 5000;
    }
    // lockCi indexes VF_NUM_POINTS-sized arrays downstream.  Preserve the -1
    // "no explicit lock" sentinel but neutralize any out-of-bounds index.
    if (d->lockCi < -1) d->lockCi = -1;
    if (d->lockCi >= VF_NUM_POINTS) d->lockCi = VF_NUM_POINTS - 1;
    if (d->lockMode < LOCK_MODE_NONE) d->lockMode = LOCK_MODE_NONE;
    if (d->lockMode > LOCK_MODE_HARD) d->lockMode = LOCK_MODE_HARD;
    // lockMHz feeds NVML locked-clocks and flatten-tail targets; cap it like
    // the curve points (0 stays 0 = "no target").
    if (d->lockMHz > 5000u) d->lockMHz = 5000u;
    // Selective-offset exclude count gates per-point GPU offset application.
    if (d->gpuOffsetExcludeLowCount < 0) d->gpuOffsetExcludeLowCount = 0;
    if (d->gpuOffsetExcludeLowCount > VF_NUM_POINTS) d->gpuOffsetExcludeLowCount = VF_NUM_POINTS;
    if (d->hasFan) {
        if (d->fanPercent < 0) d->fanPercent = 0;
        if (d->fanPercent > 100) d->fanPercent = 100;
        // fanMode selects the runtime policy (auto/fixed/curve); an unknown
        // value would fall through every branch with undefined effect.
        if (d->fanMode < FAN_MODE_AUTO) d->fanMode = FAN_MODE_AUTO;
        if (d->fanMode > FAN_MODE_CURVE) d->fanMode = FAN_MODE_CURVE;
        // The embedded fan curve feeds fan-speed writes and interpolation.
        for (int i = 0; i < FAN_CURVE_MAX_POINTS; i++) {
            if (d->fanCurve.points[i].fanPercent < 0) d->fanCurve.points[i].fanPercent = 0;
            if (d->fanCurve.points[i].fanPercent > 100) d->fanCurve.points[i].fanPercent = 100;
            if (d->fanCurve.points[i].temperatureC < 0) d->fanCurve.points[i].temperatureC = 0;
            if (d->fanCurve.points[i].temperatureC > 150) d->fanCurve.points[i].temperatureC = 150;
        }
        if (d->fanCurve.hysteresisC < 0) d->fanCurve.hysteresisC = 0;
        if (d->fanCurve.hysteresisC > FAN_CURVE_MAX_HYSTERESIS_C) d->fanCurve.hysteresisC = FAN_CURVE_MAX_HYSTERESIS_C;
        if (d->fanCurve.pollIntervalMs < 1) d->fanCurve.pollIntervalMs = 1;
    }
}

// Decide whether the GUI may adopt the service snapshot's lock MODE for an
// already-matching lock point.  Invariant: `lockMode != appliedLockMode`
// means the user holds pending, not-yet-applied lock intent (checkbox click
// FLATTEN->HARD, right-click menu switch, or a loaded profile) — the snapshot
// still carries the PREVIOUSLY applied mode and must not clobber that intent,
// or the change becomes un-appliable ("No changes to apply") and gets saved
// wrong.  Adoption is allowed only when the GUI is clean and holds no
// divergent intent (e.g. curve-detected FLATTEN while the service
// authoritatively reports HARD at the same point).
static inline bool lock_mode_sync_allowed(int guiLockMode, int guiAppliedLockMode, bool guiDirty) {
    return !guiDirty && guiLockMode == guiAppliedLockMode;
}

// ---------------------------------------------------------------------------
// Binary service protocol (named pipe on Windows, Unix socket on Linux)
// ---------------------------------------------------------------------------
enum {
    SERVICE_PROTOCOL_MAGIC = 0x47535643u,
    SERVICE_PROTOCOL_VERSION = 8,
};

// ServiceRequest.flags bits. Bit 0 = interactive apply. Bit 30 marks an
// "apply shared slot N" request (N in bits 8..15): under the shared-only policy
// the service ignores the client-supplied settings and applies its OWN copy of
// shared bank slot N, so a restricted user cannot smuggle custom OC.
#define SERVICE_REQUEST_FLAG_INTERACTIVE   0x00000001u
#define SERVICE_REQUEST_FLAG_SHARED_SLOT   0x40000000u
#define SERVICE_REQUEST_SHARED_SLOT_SHIFT  8
#define SERVICE_REQUEST_SHARED_SLOT_MASK   0xFFu

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
    gc_bool8 initialized;
    gc_bool8 loaded;
    gc_bool8 fanSupported;
    gc_bool8 fanRangeKnown;
    gc_bool8 fanIsAuto;
    gc_bool8 fanCurveRuntimeActive;
    gc_bool8 fanFixedRuntimeActive;
    gc_bool8 gpuOffsetRangeKnown;
    gc_bool8 memOffsetRangeKnown;
    gc_bool8 curveOffsetRangeKnown;
    gc_bool8 gpuTemperatureValid;
    gc_bool8 vfReadSupported;
    gc_bool8 vfWriteSupported;
    gc_bool8 vfBestGuess;
    gc_bool8 hasLock;
    int lockCi;
    unsigned int lockMHz;
    int lockMode;
    gc_bool8 lockTracksAnchor;
    unsigned int adapterCount;
    unsigned int selectedAdapterIndex;
    gc_bool8 selectedAdapterOrdinalFallback;
    GpuAdapterInfo adapters[MAX_GPU_ADAPTERS];
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
    int appliedGpuOffsetExcludeLowCount;
    gc_bool8 lastApplyUsedGpuOffset;
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
    gc_u32 ownerSessionId;
    gc_u64 ownerUtcMs;
    // GPU recovery status — populated when the service is recovering from
    // a device reconnect / driver upgrade.  The GUI uses these to show
    // "GPU reconnecting..." instead of "service not responding".
    gc_bool8 serviceInRecovery;
    gc_u64 lastRecoveryTickMs;
    // True when the service has re-applied settings after recovery but not yet
    // confirmed they stuck — the GUI shows "reapplying..." instead of a
    // misleading "settings active".
    gc_bool8 serviceReapplyInProgress;
};

struct ServiceRequest {
    gc_u32 magic;
    gc_u32 version;
    gc_u32 command;
    gc_u32 flags;
    gc_u32 callerPid;
    gc_u32 callerSessionId;
    gc_u32 resetOcBeforeApply;
    GpuAdapterInfo targetGpu;
    DesiredSettings desired;
    char source[64];
    char path[GC_REQUEST_PATH_MAX];
};
static_assert(sizeof(ServiceRequest) < 65536, "ServiceRequest size sanity check");

struct ServiceResponse {
    gc_u32 magic;
    gc_u32 version;
    gc_u32 status;
    gc_u32 reserved;
    gc_u32 serviceBuildNumber;
    char serviceVersion[32];
    ServiceSnapshot snapshot;
    DesiredSettings desired;
    ControlState controlState;
    char message[512];
};
static_assert(sizeof(ServiceResponse) < 262144, "ServiceResponse size sanity check");

static inline void validate_service_snapshot_for_ipc(ServiceSnapshot* s) {
    if (!s) return;
    canonicalize_gc_bool8(&s->initialized);
    canonicalize_gc_bool8(&s->loaded);
    canonicalize_gc_bool8(&s->fanSupported);
    canonicalize_gc_bool8(&s->fanRangeKnown);
    canonicalize_gc_bool8(&s->fanIsAuto);
    canonicalize_gc_bool8(&s->fanCurveRuntimeActive);
    canonicalize_gc_bool8(&s->fanFixedRuntimeActive);
    canonicalize_gc_bool8(&s->gpuOffsetRangeKnown);
    canonicalize_gc_bool8(&s->memOffsetRangeKnown);
    canonicalize_gc_bool8(&s->curveOffsetRangeKnown);
    canonicalize_gc_bool8(&s->gpuTemperatureValid);
    canonicalize_gc_bool8(&s->vfReadSupported);
    canonicalize_gc_bool8(&s->vfWriteSupported);
    canonicalize_gc_bool8(&s->vfBestGuess);
    canonicalize_gc_bool8(&s->hasLock);
    canonicalize_gc_bool8(&s->lockTracksAnchor);
    canonicalize_gc_bool8(&s->selectedAdapterOrdinalFallback);
    canonicalize_gc_bool8(&s->lastApplyUsedGpuOffset);
    canonicalize_gc_bool8(&s->serviceInRecovery);
    canonicalize_gc_bool8(&s->serviceReapplyInProgress);
    if (s->adapterCount > MAX_GPU_ADAPTERS) s->adapterCount = MAX_GPU_ADAPTERS;
    if (s->selectedAdapterIndex >= MAX_GPU_ADAPTERS) s->selectedAdapterIndex = 0;
    for (unsigned int i = 0; i < s->adapterCount && i < MAX_GPU_ADAPTERS; i++) {
        validate_gpu_adapter_info_for_ipc(&s->adapters[i]);
    }
    validate_fan_curve_flags_for_ipc(&s->activeFanCurve);
}

static inline void validate_service_response_for_ipc(ServiceResponse* r) {
    if (!r) return;
    validate_service_snapshot_for_ipc(&r->snapshot);
    validate_desired_settings_for_ipc(&r->desired);
    validate_control_state_for_ipc(&r->controlState);
}

enum {
    GUI_FAN_MODE_UNSET = -1,
};

// ---------------------------------------------------------------------------
// NVML function-pointer table (resolved from nvml.dll / libnvidia-ml.so.1)
// ---------------------------------------------------------------------------
typedef nvmlReturn_t (*nvmlInit_v2_t)();
typedef nvmlReturn_t (*nvmlShutdown_t)();
typedef nvmlReturn_t (*nvmlDeviceGetHandleByIndex_v2_t)(unsigned int, nvmlDevice_t*);
typedef nvmlReturn_t (*nvmlDeviceGetCount_v2_t)(unsigned int*);
struct nvmlPciInfo_t {
    char busId[32];
    unsigned int domain;
    unsigned int bus;
    unsigned int device;
    unsigned int pciDeviceId;
    unsigned int pciSubSystemId;
    unsigned int reserved0;
    unsigned int reserved1;
    unsigned int reserved2;
    unsigned int reserved3;
};
typedef nvmlReturn_t (*nvmlDeviceGetPciInfo_t)(nvmlDevice_t, nvmlPciInfo_t*);
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
typedef nvmlReturn_t (*nvmlDeviceSetGpuLockedClocks_t)(nvmlDevice_t, unsigned int, unsigned int);
typedef nvmlReturn_t (*nvmlDeviceResetGpuLockedClocks_t)(nvmlDevice_t);
typedef nvmlReturn_t (*nvmlDeviceSetMemoryLockedClocks_t)(nvmlDevice_t, unsigned int, unsigned int);
typedef nvmlReturn_t (*nvmlDeviceResetMemoryLockedClocks_t)(nvmlDevice_t);

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
    nvmlDeviceGetCount_v2_t getCount;
    nvmlDeviceGetPciInfo_t getPciInfo;
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
    nvmlDeviceSetGpuLockedClocks_t setGpuLockedClocks;
    nvmlDeviceResetGpuLockedClocks_t resetGpuLockedClocks;
    nvmlDeviceSetMemoryLockedClocks_t setMemoryLockedClocks;
    nvmlDeviceResetMemoryLockedClocks_t resetMemoryLockedClocks;
};

#endif // GREEN_CURVE_GPU_CORE_H
