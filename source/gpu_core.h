// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// gpu_core.h — platform-neutral GPU control data model shared by the Windows
// and Linux backends.  Contains ONLY portable declarations: NVAPI entry-point
// IDs + private struct layouts, NVML typedefs/enums/structs, the VF-curve data
// model, the GPU family dispatch (VfBackendSpec), and the DesiredSettings
// request model + its IPC trust-boundary validator.  The binary service
// protocol is kept in service_protocol.h and included after those prerequisites.
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
#define NVAPI_GPU_GET_BUS_ID_ID          0x1BE0B8E5u
#define NVAPI_GPU_GET_BUS_SLOT_ID_ID     0x2A0A350Fu
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
    NVML_DEVICE_ARCH_KEPLER = 2,
    NVML_DEVICE_ARCH_MAXWELL = 3,
    NVML_DEVICE_ARCH_PASCAL = 4,
    NVML_DEVICE_ARCH_VOLTA = 5,
    NVML_DEVICE_ARCH_TURING = 6,
    NVML_DEVICE_ARCH_AMPERE = 7,
    NVML_DEVICE_ARCH_ADA = 8,
    NVML_DEVICE_ARCH_HOPPER = 9,
    NVML_DEVICE_ARCH_BLACKWELL = 10,
    NVML_DEVICE_ARCH_RUBIN = 13,
    NVML_DEVICE_ARCH_UNKNOWN = 0xffffffffu,
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

// TRAY_ICON_STATE_* is Windows tray presentation, not GPU data model; it lives
// in gui_apply_in_flight_policy.h with the rule that selects between the
// themes.  See this header's own note above: UI state belongs in app_shared.h
// and its policy headers, not here.

struct FanCurvePoint {
    gc_bool8 enabled;
    int temperatureC;
    int fanPercent;
};

struct FanCurveConfig {
    FanCurvePoint points[FAN_CURVE_MAX_POINTS];
    int pollIntervalMs;
    int hysteresisC;
    gc_bool8 zeroRpmEnabled, zeroRpmReserved[3];
};

#include "fan_zero_rpm_policy.h"

struct ControlState {
    gc_bool8 valid, hasGpuOffset;
    gc_bool8 gpuOffsetReadbackValid;
    int gpuOffsetMHz;
    int gpuOffsetExcludeLowCount;
    gc_bool8 hasMemOffset, memOffsetReadbackValid;
    int memOffsetMHz;
    gc_bool8 hasPowerLimit;
    gc_bool8 powerLimitReadbackValid;
    int powerLimitPct;
    gc_bool8 hasFan;
    gc_bool8 fanPolicyReadbackValid;
    gc_bool8 fanTargetReadbackValid;
    int fanMode;
    int fanFixedPercent;
    int fanCurrentPercent;
    int fanCurrentTemperatureC;
    FanCurveConfig fanCurve;
    gc_bool8 hasXbarOffset;
    gc_bool8 xbarOffsetReadbackValid;
    int xbarOffsetKhz;
    gc_bool8 hasXbarMsvddOffset;
    gc_bool8 xbarMsvddOffsetReadbackValid;
    int xbarMsvddOffsetUv;
    gc_bool8 hasSysClkOffset;
    gc_bool8 sysClkOffsetReadbackValid;
    int sysClkOffsetKhz;
    gc_bool8 hasVideoClkOffset;
    gc_bool8 videoClkOffsetReadbackValid;
    int videoClkOffsetKhz;
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

// Durable GPU selection. legacyIndex preserves compatibility with existing
// configs, while a present stable identity prevents adapter enumeration order
// changes from redirecting an automatic profile apply to another GPU.
struct ConfiguredGpuSelection {
    gc_bool8 stableIdentityPresent;
    unsigned int legacyIndex;
    GpuAdapterInfo identity;
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
    gc_bool8 hasXbarOffsetKhz;
    int xbarOffsetKhz;
    gc_bool8 hasXbarMsvddOffsetUv;
    int xbarMsvddOffsetUv;
    gc_bool8 hasSysClkOffsetKhz;
    int sysClkOffsetKhz;
    gc_bool8 hasVideoClkOffsetKhz;
    int videoClkOffsetKhz;
};

static inline void validate_gpu_adapter_info_for_ipc(GpuAdapterInfo* g) {
    if (!g) return;
    canonicalize_gc_bool8(&g->valid);
    canonicalize_gc_bool8(&g->pciInfoValid);
    canonicalize_gc_bool8(&g->vfReadSupported);
    canonicalize_gc_bool8(&g->vfWriteSupported);
    canonicalize_gc_bool8(&g->vfBestGuess);
}

// The one authoritative power-limit range, in percent of the board's default
// limit.  Above 100% is a normal, supported request: an RTX 5070 defaults to
// 250 W with a 300 W (120%) ceiling.  Every clamp — the IPC trust boundary, the
// apply gate, the Win32 spinner and the Linux TUI/normalizer — must use these
// bounds.  A stray 0..100 clamp silently rewrites a valid 105% request to 100%.
static const int POWER_LIMIT_MIN_PCT = 50;
static const int POWER_LIMIT_MAX_PCT = 150;

static inline int clamp_power_limit_pct(int pct) {
    if (pct < POWER_LIMIT_MIN_PCT) return POWER_LIMIT_MIN_PCT;
    if (pct > POWER_LIMIT_MAX_PCT) return POWER_LIMIT_MAX_PCT;
    return pct;
}

static inline void validate_control_state_for_ipc(ControlState* c) {
    if (!c) return;
    canonicalize_gc_bool8(&c->valid);
    canonicalize_gc_bool8(&c->hasGpuOffset);
    canonicalize_gc_bool8(&c->gpuOffsetReadbackValid);
    canonicalize_gc_bool8(&c->hasMemOffset);
    canonicalize_gc_bool8(&c->memOffsetReadbackValid);
    canonicalize_gc_bool8(&c->hasPowerLimit);
    canonicalize_gc_bool8(&c->powerLimitReadbackValid);
    canonicalize_gc_bool8(&c->hasFan);
    canonicalize_gc_bool8(&c->fanPolicyReadbackValid);
    canonicalize_gc_bool8(&c->fanTargetReadbackValid);
    validate_fan_curve_flags_for_ipc(&c->fanCurve);
    canonicalize_gc_bool8(&c->hasXbarOffset);
    canonicalize_gc_bool8(&c->xbarOffsetReadbackValid);
    canonicalize_gc_bool8(&c->hasXbarMsvddOffset);
    canonicalize_gc_bool8(&c->xbarMsvddOffsetReadbackValid);
    canonicalize_gc_bool8(&c->hasSysClkOffset);
    canonicalize_gc_bool8(&c->sysClkOffsetReadbackValid);
    canonicalize_gc_bool8(&c->hasVideoClkOffset);
    canonicalize_gc_bool8(&c->videoClkOffsetReadbackValid);
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
    canonicalize_gc_bool8(&d->hasXbarOffsetKhz);
    canonicalize_gc_bool8(&d->hasXbarMsvddOffsetUv);
    canonicalize_gc_bool8(&d->hasSysClkOffsetKhz);
    canonicalize_gc_bool8(&d->hasVideoClkOffsetKhz);
    if (d->hasXbarOffsetKhz && (d->xbarOffsetKhz < -1000000 || d->xbarOffsetKhz > 1000000)) {
        d->xbarOffsetKhz = d->xbarOffsetKhz < -1000000 ? -1000000 : 1000000;
    }
    if (d->hasXbarMsvddOffsetUv && (d->xbarMsvddOffsetUv < -100000 || d->xbarMsvddOffsetUv > 100000)) {
        d->xbarMsvddOffsetUv = d->xbarMsvddOffsetUv < -100000 ? -100000 : 100000;
    }
    if (d->hasSysClkOffsetKhz && (d->sysClkOffsetKhz < -1000000 || d->sysClkOffsetKhz > 1000000)) {
        d->sysClkOffsetKhz = d->sysClkOffsetKhz < -1000000 ? -1000000 : 1000000;
    }
    if (d->hasVideoClkOffsetKhz && (d->videoClkOffsetKhz < -1000000 || d->videoClkOffsetKhz > 1000000)) {
        d->videoClkOffsetKhz = d->videoClkOffsetKhz < -1000000 ? -1000000 : 1000000;
    }
    validate_fan_curve_flags_for_ipc(&d->fanCurve);
    for (int ci = 0; ci < VF_NUM_POINTS; ci++) {
        if (d->curvePointMHz[ci] > 5000u) d->curvePointMHz[ci] = 5000u;
    }
    if (d->hasPowerLimit) d->powerLimitPct = clamp_power_limit_pct(d->powerLimitPct);
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

#include "service_protocol.h"

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
// Real NVML ABI. `busIdLegacy` comes FIRST and is 16 bytes; the 32-byte
// `busId` is appended at the tail by `nvmlDeviceGetPciInfo_v3`. Declaring a
// leading `busId[32]` instead shifts every integer field by 16 bytes, so
// `pciDeviceId`/`pciSubSystemId` land inside the trailing bus-id *string* and
// read back as ASCII (observed on driver 610.43.03: 0x3a37303a ":07:" and
// 0x302e3030 "00.0"), which fails identity matching closed against NvAPI.
// Byte-exact against the installed driver: legacy@0, domain@16, bus@20,
// device@24, pciDeviceId@28, pciSubSystemId@32, busId@36.
enum {
    NVML_DEVICE_PCI_BUS_ID_BUFFER_V2_SIZE = 16,
    NVML_DEVICE_PCI_BUS_ID_BUFFER_SIZE = 32,
};
struct nvmlPciInfo_t {
    char busIdLegacy[NVML_DEVICE_PCI_BUS_ID_BUFFER_V2_SIZE];
    unsigned int domain;
    unsigned int bus;
    unsigned int device;
    unsigned int pciDeviceId;
    unsigned int pciSubSystemId;
    char busId[NVML_DEVICE_PCI_BUS_ID_BUFFER_SIZE];
};
static_assert(offsetof(nvmlPciInfo_t, domain) == 16, "nvml pci-info layout");
static_assert(offsetof(nvmlPciInfo_t, bus) == 20, "nvml pci-info layout");
static_assert(offsetof(nvmlPciInfo_t, device) == 24, "nvml pci-info layout");
static_assert(offsetof(nvmlPciInfo_t, pciDeviceId) == 28, "nvml pci-info layout");
static_assert(offsetof(nvmlPciInfo_t, pciSubSystemId) == 32, "nvml pci-info layout");
static_assert(offsetof(nvmlPciInfo_t, busId) == 36, "nvml pci-info layout");
static_assert(sizeof(nvmlPciInfo_t) == 68, "nvml pci-info size");

// Only `_v3` fills the trailing `busId`; `_v2` and the v1 entry point stop
// after the integers, so an older resolution must fall back to the legacy
// string instead of parsing a zero buffer.
static inline const char* nvml_pci_bus_id_text(const nvmlPciInfo_t* pci) {
    if (!pci) return "";
    if (pci->busId[0]) return pci->busId;
    return pci->busIdLegacy;
}

// Resolve the PCI-info entry point in descending preference. This ordering is
// not cosmetic: `_v2` stops after pciSubSystemId and the v1 name stops after
// pciDeviceId, so binding the v1 name alone silently discards subsystem
// identity and leaves the bus-id string empty. Each platform passes its own
// symbol resolver; the preference order stays in one place.
typedef bool (*NvmlSymbolResolver)(void** out, const char* name);
static inline void nvml_resolve_pci_info(NvmlSymbolResolver resolve, void** out) {
    if (!resolve || !out) return;
    if (!resolve(out, "nvmlDeviceGetPciInfo_v3") &&
        !resolve(out, "nvmlDeviceGetPciInfo_v2"))
        resolve(out, "nvmlDeviceGetPciInfo");
}

// The driver snaps a clock offset to a per-domain grid and then reports the
// SNAPPED value, while still returning NVML_SUCCESS for the write. Measured on
// an RTX 5070 / driver 610.43.03: graphics offsets round-trip exactly, memory
// offsets land on a 2 MHz grid toward zero (15 -> 14, 1 -> 0, -5 -> -4, -1 -> 0).
// Demanding an exact readback therefore turns an honoured write into a failed
// apply; on Linux that rolled the ENTIRE apply back, so every odd memory offset
// was unusable. Windows already tolerates this and only reports inexactness.
static inline int nvml_clock_offset_grid_step(unsigned int domain) {
    return domain == NVML_CLOCK_MEM ? 2 : 1;
}

// Green Curve exposes one configured offset per clock domain, not one value
// per transient performance state. NVML's modern offset API is P-state scoped,
// so reads, writes, verification, and rollback all target the stable P0 slot.
// Using whichever state happened to be active at idle could successfully write
// P8 while leaving the performance-state P0 value unchanged.
static inline unsigned int nvml_configured_clock_offset_pstate() {
    return NVML_PSTATE_0;
}

// Accept only a snap toward zero of less than one grid step. A readback that
// overshoots the request, flips sign, or moves a full step means the driver did
// not honour the request, and must still fail closed.
static inline bool nvml_clock_offset_readback_matches(int requested,
                                                      int readback,
                                                      int stepMHz) {
    if (readback == requested) return true;
    if (stepMHz < 1) stepMHz = 1;
    long long delta = (long long)requested - (long long)readback;
    if (delta < 0) delta = -delta;
    if (delta >= (long long)stepMHz) return false;
    if (requested >= 0) return readback >= 0 && readback <= requested;
    return readback <= 0 && readback >= requested;
}

typedef nvmlReturn_t (*nvmlDeviceGetPciInfo_t)(nvmlDevice_t, nvmlPciInfo_t*);
typedef nvmlReturn_t (*nvmlDeviceGetPowerManagementLimit_t)(nvmlDevice_t, unsigned int*);
typedef nvmlReturn_t (*nvmlDeviceGetPowerManagementDefaultLimit_t)(nvmlDevice_t, unsigned int*);
typedef nvmlReturn_t (*nvmlDeviceGetPowerManagementLimitConstraints_t)(nvmlDevice_t, unsigned int*, unsigned int*);
typedef nvmlReturn_t (*nvmlDeviceSetPowerManagementLimit_t)(nvmlDevice_t, unsigned int);
typedef nvmlReturn_t (*nvmlDeviceGetClockOffsets_t)(nvmlDevice_t, nvmlClockOffset_t*);
typedef nvmlReturn_t (*nvmlDeviceSetClockOffsets_t)(nvmlDevice_t, nvmlClockOffset_t*);
typedef nvmlReturn_t (*nvmlDeviceGetPerformanceState_t)(nvmlDevice_t, unsigned int*);
typedef nvmlReturn_t (*nvmlDeviceGetArchitecture_t)(nvmlDevice_t, unsigned int*);
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
    nvmlDeviceGetArchitecture_t getArchitecture;
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
