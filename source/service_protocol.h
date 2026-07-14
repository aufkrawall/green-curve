// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// Binary service protocol shared by the Windows named-pipe and Linux Unix-
// socket transports.  gpu_core.h includes this file after defining the fixed-
// width aliases, GPU/settings data model, and IPC trust-boundary helpers used
// below.  Keep the wire declarations free of OS-specific types.

#ifndef GREEN_CURVE_SERVICE_PROTOCOL_H
#define GREEN_CURVE_SERVICE_PROTOCOL_H

enum {
    SERVICE_PROTOCOL_MAGIC = 0x47535643u,
    SERVICE_PROTOCOL_VERSION = 10,
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
    // Settings-free authenticated notification from the per-user scheduled
    // task.  The service derives the active session, account and configured
    // profile; no client-supplied profile data is trusted for this command.
    SERVICE_CMD_LOGON_HANDOFF = 10,
    SERVICE_CMD_GET_OPERATION_RESULT = 11,
};

enum ServiceOperationState {
    SERVICE_OPERATION_NONE = 0,
    SERVICE_OPERATION_IN_PROGRESS = 1,
    SERVICE_OPERATION_SUCCEEDED = 2,
    SERVICE_OPERATION_FAILED = 3,
    SERVICE_OPERATION_OUTCOME_UNKNOWN = 4,
};

// Every write request carries an origin.  Only the deliberately explicit
// user actions below may acknowledge and clear a sticky automatic-restore
// lockout after a successful write.  All automatic origins must honor the
// lockout and can never clear it.
enum ServiceApplyOrigin {
    SERVICE_APPLY_ORIGIN_UNSPECIFIED = 0,
    SERVICE_APPLY_ORIGIN_GUI = 1,
    SERVICE_APPLY_ORIGIN_CLI = 2,
    SERVICE_APPLY_ORIGIN_HOTKEY = 3,
    SERVICE_APPLY_ORIGIN_TRAY = 4,
    SERVICE_APPLY_ORIGIN_APP_LAUNCH = 5,
    SERVICE_APPLY_ORIGIN_FOREGROUND = 6,
    SERVICE_APPLY_ORIGIN_LOGON = 7,
    SERVICE_APPLY_ORIGIN_STANDBY = 8,
    SERVICE_APPLY_ORIGIN_DRIVER_RECOVERY = 9,
};

static inline bool service_apply_origin_is_explicit(ServiceApplyOrigin origin) {
    return origin == SERVICE_APPLY_ORIGIN_GUI ||
           origin == SERVICE_APPLY_ORIGIN_CLI ||
           origin == SERVICE_APPLY_ORIGIN_HOTKEY ||
           origin == SERVICE_APPLY_ORIGIN_TRAY;
}

static inline bool service_apply_origin_is_automatic(ServiceApplyOrigin origin) {
    return origin == SERVICE_APPLY_ORIGIN_APP_LAUNCH ||
           origin == SERVICE_APPLY_ORIGIN_FOREGROUND ||
           origin == SERVICE_APPLY_ORIGIN_LOGON ||
           origin == SERVICE_APPLY_ORIGIN_STANDBY ||
           origin == SERVICE_APPLY_ORIGIN_DRIVER_RECOVERY;
}

// Only these origins are valid on a client-supplied SERVICE_CMD_APPLY. Logon
// has its own settings-free handoff command, while standby and driver recovery
// are authorized solely inside the service lifecycle worker. Keeping this
// whitelist separate prevents a buggy or hostile client from smuggling a
// settings payload through one of those service-owned lifecycle origins.
static inline bool service_apply_origin_is_client_apply(ServiceApplyOrigin origin) {
    return origin == SERVICE_APPLY_ORIGIN_GUI ||
           origin == SERVICE_APPLY_ORIGIN_CLI ||
           origin == SERVICE_APPLY_ORIGIN_HOTKEY ||
           origin == SERVICE_APPLY_ORIGIN_TRAY ||
           origin == SERVICE_APPLY_ORIGIN_APP_LAUNCH ||
           origin == SERVICE_APPLY_ORIGIN_FOREGROUND;
}

enum ServiceProfileSource {
    SERVICE_PROFILE_SOURCE_NONE = 0,
    SERVICE_PROFILE_SOURCE_USER_SLOT = 1,
    SERVICE_PROFILE_SOURCE_SHARED_SLOT = 2,
    SERVICE_PROFILE_SOURCE_MACHINE_SLOT = 3,
    SERVICE_PROFILE_SOURCE_AD_HOC = 4,
};

enum ServiceLifecycleTrigger {
    SERVICE_LIFECYCLE_TRIGGER_NONE = 0,
    SERVICE_LIFECYCLE_TRIGGER_WTS_LOGON = 1,
    SERVICE_LIFECYCLE_TRIGGER_TASK_HANDOFF = 2,
    SERVICE_LIFECYCLE_TRIGGER_STANDBY_RESUME = 3,
    SERVICE_LIFECYCLE_TRIGGER_DRIVER_RECOVERY = 4,
};

enum ServiceLifecycleResult {
    SERVICE_LIFECYCLE_RESULT_NONE = 0,
    SERVICE_LIFECYCLE_RESULT_PENDING = 1,
    SERVICE_LIFECYCLE_RESULT_APPLIED = 2,
    SERVICE_LIFECYCLE_RESULT_NO_PROFILE = 3,
    SERVICE_LIFECYCLE_RESULT_LOCKED_OUT = 4,
    SERVICE_LIFECYCLE_RESULT_TRANSIENT_NOT_READY = 5,
    SERVICE_LIFECYCLE_RESULT_SUPERSEDED = 6,
    SERVICE_LIFECYCLE_RESULT_CANCELLED_LOGOFF = 7,
    SERVICE_LIFECYCLE_RESULT_FAILED = 8,
};

enum ServiceAutoRestoreLockoutReason {
    SERVICE_AUTO_RESTORE_LOCKOUT_NONE = 0,
    SERVICE_AUTO_RESTORE_LOCKOUT_UNSTABLE_APPLY = 1,
    SERVICE_AUTO_RESTORE_LOCKOUT_TDR_SPAM = 2,
    SERVICE_AUTO_RESTORE_LOCKOUT_AUTOMATIC_APPLY_FAILED = 3,
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
    // Authoritative intent ownership.  This is metadata about the last
    // successful write, not a comparison with temperature-sensitive live VF
    // MHz values.
    gc_u32 activeProfileSource;
    gc_u32 activeProfileSlot;
    gc_u32 lastLifecycleTrigger;
    gc_u32 lastLifecycleResult;
    gc_u32 autoRestoreLockoutReason;
};

struct ServiceRequest {
    gc_u32 magic;
    gc_u32 version;
    gc_u32 command;
    gc_u32 flags;
    gc_u32 callerPid;
    gc_u32 callerSessionId;
    gc_u32 resetOcBeforeApply;
    gc_u32 applyOrigin;
    gc_u32 profileSource;
    gc_u32 profileSlot;
    gc_u64 operationId;
    GpuAdapterInfo targetGpu;
    DesiredSettings desired;
    char source[64];
    char path[GC_REQUEST_PATH_MAX];
};
// Static field-offset assertions: these catch accidental ABI breaks when
// struct fields are reordered, resized, or moved between versions.
// The magic field must always be at offset 0 for protocol identification.
static_assert(offsetof(ServiceRequest, magic) == 0, "ServiceRequest.magic must be at offset 0");
static_assert(offsetof(ServiceRequest, version) == 4, "ServiceRequest.version offset changed");
static_assert(offsetof(ServiceRequest, command) == 8, "ServiceRequest.command offset changed");
static_assert(sizeof(ServiceRequest) < 65536, "ServiceRequest size sanity check");

struct ServiceResponse {
    gc_u32 magic;
    gc_u32 version;
    gc_u32 status;
    gc_u32 reserved;
    gc_u32 serviceBuildNumber;
    gc_u32 operationState;
    gc_u64 operationId;
    char serviceVersion[32];
    ServiceSnapshot snapshot;
    DesiredSettings desired;
    ControlState controlState;
    char message[512];
};
static_assert(offsetof(ServiceResponse, magic) == 0, "ServiceResponse.magic must be at offset 0");
static_assert(offsetof(ServiceResponse, version) == 4, "ServiceResponse.version offset changed");
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
    if (s->activeProfileSource > SERVICE_PROFILE_SOURCE_AD_HOC) {
        s->activeProfileSource = SERVICE_PROFILE_SOURCE_NONE;
        s->activeProfileSlot = 0;
    }
    if (s->activeProfileSlot > 255u) s->activeProfileSlot = 0;
    if (s->lastLifecycleTrigger > SERVICE_LIFECYCLE_TRIGGER_DRIVER_RECOVERY) {
        s->lastLifecycleTrigger = SERVICE_LIFECYCLE_TRIGGER_NONE;
    }
    if (s->lastLifecycleResult > SERVICE_LIFECYCLE_RESULT_FAILED) {
        s->lastLifecycleResult = SERVICE_LIFECYCLE_RESULT_NONE;
    }
    if (s->autoRestoreLockoutReason > SERVICE_AUTO_RESTORE_LOCKOUT_AUTOMATIC_APPLY_FAILED) {
        s->autoRestoreLockoutReason = SERVICE_AUTO_RESTORE_LOCKOUT_AUTOMATIC_APPLY_FAILED;
    }
    if (s->adapterCount > MAX_GPU_ADAPTERS) s->adapterCount = MAX_GPU_ADAPTERS;
    if (s->selectedAdapterIndex >= MAX_GPU_ADAPTERS) s->selectedAdapterIndex = 0;
    for (unsigned int i = 0; i < s->adapterCount && i < MAX_GPU_ADAPTERS; i++) {
        validate_gpu_adapter_info_for_ipc(&s->adapters[i]);
    }
    validate_fan_curve_flags_for_ipc(&s->activeFanCurve);
}

static inline void validate_service_response_for_ipc(ServiceResponse* r) {
    if (!r) return;
    if (r->operationState > SERVICE_OPERATION_OUTCOME_UNKNOWN) {
        r->operationState = SERVICE_OPERATION_OUTCOME_UNKNOWN;
    }
    validate_service_snapshot_for_ipc(&r->snapshot);
    validate_desired_settings_for_ipc(&r->desired);
    validate_control_state_for_ipc(&r->controlState);
}

#endif // GREEN_CURVE_SERVICE_PROTOCOL_H
