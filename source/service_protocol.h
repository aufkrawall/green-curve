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
    // v13 adds the Linux daemon's startup-apply policy: two commands, the
    // request's startupMode, and the mode/slot pair published in every state
    // envelope. Both structs changed size, so old and new ends cannot talk to
    // each other -- which the header-first prefix reports precisely.
    //
    // v15 adds SERVICE_CMD_REFRESH_STARTUP_PROFILE, which keeps a `profile N`
    // boot-apply snapshot equal to the profile it names after that profile is
    // edited. The command set grew, so the version moves with it even though no
    // struct changed size: a v14 daemon must reject the new command outright
    // rather than fall through its command switch.
    //
    // v16 gives that snapshot its own response field. v15 returned it in
    // `desired`, the member the envelope defines as the ACTIVE intent, so the
    // daemon's unconditional end-of-request stamp overwrote it and every
    // staleness check compared the applied settings against the profile
    // instead. One wire field, one meaning: `desired` is always the active
    // intent, `startupProfile` is always the boot-apply snapshot.
    //
    // v17 adds SERVICE_CMD_RESUME_RESTORE, the Linux counterpart of the Windows
    // standby-resume restore. No struct changed size; as with v15 the command
    // set grew, so a v16 daemon must reject the new command outright instead of
    // falling through its switch and answering "unknown command" to a resume
    // that silently never restored anything.
    //
    // v18 adds ServiceResponse.outcomeSeverity, so a client can tell a CLEAN
    // success from one the service completed with reservations without parsing
    // `message`. ServiceResponse changed size, so the ends genuinely cannot talk
    // to each other and the version has to move: a v17 GUI paired with a v18
    // service would otherwise read every byte from `operationId` onwards -- the
    // whole state envelope and the message -- shifted.
    SERVICE_PROTOCOL_VERSION = 18,
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
    // Linux daemon startup-apply policy: what, if anything, the daemon writes
    // to the GPU when it starts (boot, `systemctl restart`, upgrade). Read is
    // unauthenticated like any other query; the write is bounded by the same
    // socket-group authorization as APPLY, because a caller who can set the
    // boot profile can already apply it directly.
    SERVICE_CMD_GET_STARTUP_POLICY = 12,
    SERVICE_CMD_SET_STARTUP_POLICY = 13,
    // Replace the settings of an EXISTING `profile N` policy after the client
    // rewrote slot N on disk. It carries only the slot and the new settings:
    // the stored GPU binding, mode, slot and display name are the daemon's and
    // stay untouched, so this can never create a policy, move it to another
    // slot, or re-bind the boot write to a different GPU. Without it, editing
    // the profile left the daemon writing the snapshot captured when the policy
    // was first set. See startup_snapshot_policy.h.
    SERVICE_CMD_REFRESH_STARTUP_PROFILE = 14,
    // "The machine just came back from suspend; write the intent you already
    // hold again." Settings-free by construction: like the Windows standby
    // path it restores the daemon's own in-memory active intent, so a caller
    // who can reach the socket cannot smuggle an overclock through it. The
    // Linux resume unit (greencurve-resume.service) is the only sender.
    SERVICE_CMD_RESUME_RESTORE = 15,
};

// What the daemon does with the GPU at startup.  RESTORE_LAST is zero so an
// absent or zero-filled record keeps the behaviour every previous build had:
// replay the committed active state.
enum ServiceStartupPolicyMode {
    SERVICE_STARTUP_POLICY_RESTORE_LAST = 0,
    SERVICE_STARTUP_POLICY_NONE = 1,
    SERVICE_STARTUP_POLICY_PROFILE = 2,
    SERVICE_STARTUP_POLICY_MODE_COUNT = 3,
};

static inline const char* service_startup_policy_mode_name(unsigned int mode) {
    switch (mode) {
        case SERVICE_STARTUP_POLICY_RESTORE_LAST: return "restore-last";
        case SERVICE_STARTUP_POLICY_NONE:         return "none";
        case SERVICE_STARTUP_POLICY_PROFILE:      return "profile";
        default:                                  return "unknown";
    }
}

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
    SERVICE_STATUS_STALE_STATE = 3,
};

// How much of a success a successful answer actually is.
//
// `status` is binary, but a hardware write is not: an apply can verify every
// requested value, or it can commit while the driver quietly declined some of
// what was asked for (a VF point that refuses its selective offset, a flatten
// target the tail will not hold).  Both answer SERVICE_STATUS_OK -- the request
// was carried out and the service now owns that intent -- yet only the first is
// a result the user never needs to see.
//
// The distinction exists ONLY on the wire because only the service can make it:
// the counts behind it live in the apply backend, and the client's alternative
// is scraping `message` for phrases like "3 of 8 boost points matched", which
// is not a contract anything may depend on.
enum ServiceOutcomeSeverity : gc_u32 {
    // Every requested change was carried out and verified.
    SERVICE_OUTCOME_SEVERITY_SUCCESS = 0,
    // The operation succeeded and its result is owned, but something in it is
    // worth telling the user about.  Never used for a failure.
    SERVICE_OUTCOME_SEVERITY_WARNING = 1,
    // The operation did not succeed.  Always paired with a non-OK status.
    SERVICE_OUTCOME_SEVERITY_ERROR = 2,
};

// The single rule that keeps severity and status from ever contradicting each
// other, applied at the one point each producer writes a response to the wire.
// Total by construction: a non-OK status is ERROR regardless of what a handler
// recorded, and an OK status can only be SUCCESS or WARNING.  A handler that
// forgets to record anything therefore still ships a truthful severity, which
// is why this is derived centrally instead of assigned per branch.
static inline gc_u32 service_response_resolve_outcome_severity(
    gc_u32 status, gc_u32 recordedSeverity) {
    if (status != (gc_u32)SERVICE_STATUS_OK)
        return (gc_u32)SERVICE_OUTCOME_SEVERITY_ERROR;
    return recordedSeverity == (gc_u32)SERVICE_OUTCOME_SEVERITY_SUCCESS
        ? (gc_u32)SERVICE_OUTCOME_SEVERITY_SUCCESS
        : (gc_u32)SERVICE_OUTCOME_SEVERITY_WARNING;
}

// Exactly what the resolver above guarantees, restated for the validator and
// for the regression harness: OK carries SUCCESS or WARNING, anything else
// carries ERROR.
static inline bool service_outcome_severity_matches_status(gc_u32 status,
    gc_u32 severity) {
    return severity == service_response_resolve_outcome_severity(status,
        severity);
}

// The producer side of the same distinction, for a hardware apply.
//
// `failCount` is the apply's own verdict and outranks everything: any failed
// step means the request did not succeed.  The two "partial" counts are points
// the write COMMITTED but the readback did not match -- the backend accepts the
// live value for them rather than failing the whole apply, because a single
// stubborn VF point is not a reason to reject a profile.  Accepting them
// silently is what made an apply that did not do what was asked look identical
// to one that did, so they become a warning instead.
static inline gc_u32 service_apply_outcome_severity(int failCount,
    int partialBoostPoints, int partialFlattenPoints) {
    if (failCount > 0) return (gc_u32)SERVICE_OUTCOME_SEVERITY_ERROR;
    if (partialBoostPoints > 0 || partialFlattenPoints > 0)
        return (gc_u32)SERVICE_OUTCOME_SEVERITY_WARNING;
    return (gc_u32)SERVICE_OUTCOME_SEVERITY_SUCCESS;
}

static inline const char* service_outcome_severity_name(gc_u32 severity) {
    switch (severity) {
        case SERVICE_OUTCOME_SEVERITY_SUCCESS: return "success";
        case SERVICE_OUTCOME_SEVERITY_WARNING: return "warning";
        case SERVICE_OUTCOME_SEVERITY_ERROR:   return "error";
    }
    return "unknown";
}

// A response is useful to an interactive client only when its payload can be
// tied to one service process and one selected-GPU lifetime.  These phases and
// section bits make recovery/partial state explicit instead of asking clients
// to infer authority from cached numeric values.
enum ServiceGpuPhase : gc_u32 {
    SERVICE_GPU_PHASE_STARTING = 0,
    SERVICE_GPU_PHASE_DEVICE_MISSING = 1,
    SERVICE_GPU_PHASE_RECOVERING = 2,
    SERVICE_GPU_PHASE_REAPPLYING = 3,
    SERVICE_GPU_PHASE_READY = 4,
    SERVICE_GPU_PHASE_DEGRADED = 5,
};

enum ServiceStateSection : gc_u32 {
    SERVICE_STATE_SECTION_ADAPTER_IDENTITY = 1u << 0,
    SERVICE_STATE_SECTION_CURVE_TOPOLOGY = 1u << 1,
    SERVICE_STATE_SECTION_APPLIED_CONTROLS = 1u << 2,
    SERVICE_STATE_SECTION_FAN_TELEMETRY = 1u << 3,
    SERVICE_STATE_SECTION_ACTIVE_INTENT = 1u << 4,
};

enum {
    SERVICE_STATE_SECTION_ALL =
        SERVICE_STATE_SECTION_ADAPTER_IDENTITY |
        SERVICE_STATE_SECTION_CURVE_TOPOLOGY |
        SERVICE_STATE_SECTION_APPLIED_CONTROLS |
        SERVICE_STATE_SECTION_FAN_TELEMETRY |
        SERVICE_STATE_SECTION_ACTIVE_INTENT,
    SERVICE_STATE_SECTION_READY_REQUIRED =
        SERVICE_STATE_SECTION_ADAPTER_IDENTITY |
        SERVICE_STATE_SECTION_CURVE_TOPOLOGY |
        SERVICE_STATE_SECTION_APPLIED_CONTROLS |
        SERVICE_STATE_SECTION_ACTIVE_INTENT,
};

// Mutation domains are derived from the canonical desired settings by the
// service.  Clients may use the same pure helper for presentation, but the
// wire never trusts a client-supplied domain mask.
enum ServiceMutationDomain : gc_u32 {
    SERVICE_MUTATION_DOMAIN_RESET_BASELINE = 1u << 0,
    SERVICE_MUTATION_DOMAIN_GPU_OFFSET = 1u << 1,
    SERVICE_MUTATION_DOMAIN_MEM_OFFSET = 1u << 2,
    SERVICE_MUTATION_DOMAIN_POWER = 1u << 3,
    SERVICE_MUTATION_DOMAIN_VF_CURVE = 1u << 4,
    SERVICE_MUTATION_DOMAIN_LOCK = 1u << 5,
    SERVICE_MUTATION_DOMAIN_FAN = 1u << 6,
};

enum {
    SERVICE_MUTATION_DOMAIN_ALL =
        SERVICE_MUTATION_DOMAIN_RESET_BASELINE |
        SERVICE_MUTATION_DOMAIN_GPU_OFFSET |
        SERVICE_MUTATION_DOMAIN_MEM_OFFSET |
        SERVICE_MUTATION_DOMAIN_POWER |
        SERVICE_MUTATION_DOMAIN_VF_CURVE |
        SERVICE_MUTATION_DOMAIN_LOCK |
        SERVICE_MUTATION_DOMAIN_FAN,
};

// NOTE: the per-domain capability policy (gpu_capability_policy.h) indexes these
// same bits by position.  The two representations are pinned against each other
// by static_asserts in vf_backends.cpp — not here, because this header is
// included from inside gpu_core.h and cannot itself pull in a header that
// includes gpu_core.h.

static inline bool service_desired_has_curve_point(
    const DesiredSettings* desired) {
    if (!desired) return false;
    for (int i = 0; i < VF_NUM_POINTS; ++i)
        if (desired->hasCurvePoint[i]) return true;
    return false;
}

static inline gc_u32 service_desired_mutation_domains(
    const DesiredSettings* desired) {
    if (!desired) return 0;
    gc_u32 domains = 0;
    bool hasCurvePoint = service_desired_has_curve_point(desired);
    bool gpuOffsetUsesCurve = desired->hasGpuOffset &&
        (desired->gpuOffsetExcludeLowCount > 0 || desired->hasLock ||
         hasCurvePoint);
    bool hasCurveWrite = hasCurvePoint ||
        (desired->hasLock &&
         (desired->lockMode == LOCK_MODE_FLATTEN ||
          desired->lockMode == LOCK_MODE_HARD)) ||
        (desired->hasGpuOffset && desired->gpuOffsetExcludeLowCount > 0);
    if (desired->resetOcBeforeApply)
        domains |= SERVICE_MUTATION_DOMAIN_RESET_BASELINE;
    if (desired->hasGpuOffset && !gpuOffsetUsesCurve)
        domains |= SERVICE_MUTATION_DOMAIN_GPU_OFFSET;
    if (desired->hasMemOffset)
        domains |= SERVICE_MUTATION_DOMAIN_MEM_OFFSET;
    if (desired->hasPowerLimit)
        domains |= SERVICE_MUTATION_DOMAIN_POWER;
    if (hasCurveWrite)
        domains |= SERVICE_MUTATION_DOMAIN_VF_CURVE;
    if (desired->hasLock)
        domains |= SERVICE_MUTATION_DOMAIN_LOCK;
    if (desired->hasFan)
        domains |= SERVICE_MUTATION_DOMAIN_FAN;
    return domains;
}

static inline bool service_mutation_domains_require_vf(gc_u32 domains) {
    return (domains & SERVICE_MUTATION_DOMAIN_VF_CURVE) != 0;
}

static inline gc_u32 service_requested_mutation_domains(
    gc_u32 command, const DesiredSettings* desired) {
    return command == SERVICE_CMD_RESET ? SERVICE_MUTATION_DOMAIN_ALL
        : command == SERVICE_CMD_APPLY
            ? service_desired_mutation_domains(desired) : 0;
}

static inline gc_u32 service_unavailable_mutation_domains(
    gc_u32 command, const DesiredSettings* desired,
    gc_u32 availableDomains) {
    return service_requested_mutation_domains(command, desired) &
        ~availableDomains;
}

#include "service_desired_mutation_policy.h"

enum ServiceGpuHealthReason : gc_u32 {
    SERVICE_GPU_HEALTH_NONE = 0,
    SERVICE_GPU_HEALTH_NVML_UNAVAILABLE = 1,
    SERVICE_GPU_HEALTH_NVAPI_LIBRARY_UNAVAILABLE = 2,
    SERVICE_GPU_HEALTH_NVAPI_INIT_FAILED = 3,
    SERVICE_GPU_HEALTH_NVAPI_ENUM_FAILED = 4,
    SERVICE_GPU_HEALTH_NVAPI_HANDLE_UNRESOLVED = 5,
    SERVICE_GPU_HEALTH_ARCHITECTURE_UNAVAILABLE = 6,
    SERVICE_GPU_HEALTH_VF_INFO_FAILED = 7,
    SERVICE_GPU_HEALTH_VF_STATUS_FAILED = 8,
    SERVICE_GPU_HEALTH_VF_CONTROL_FAILED = 9,
    SERVICE_GPU_HEALTH_VF_STRUCTURE_INVALID = 10,
    SERVICE_GPU_HEALTH_STATE_UNCERTAIN = 11,
};

enum ServiceGpuArchitectureSource : gc_u32 {
    SERVICE_GPU_ARCH_SOURCE_NONE = 0,
    SERVICE_GPU_ARCH_SOURCE_NVAPI = 1,
    SERVICE_GPU_ARCH_SOURCE_NVML = 2,
    SERVICE_GPU_ARCH_SOURCE_CACHED = 3,
    SERVICE_GPU_ARCH_SOURCE_FUTURE_GUESS = 4,
};

struct ServiceGpuHealth {
    gc_u32 reason;
    int driverStatus;
    gc_u32 architectureSource;
    gc_u32 availableMutationDomains;
    gc_bool8 vfSnapshotFresh;
    gc_bool8 recoveryAttempted;
    gc_bool8 recoverySucceeded;
    // Compact GpuCapabilityProbe carriage.  These consume the previous five
    // reserved bytes without changing this struct's layout: one topology byte,
    // followed by seven two-bit domain states in the naturally aligned word.
    // An older producer leaves both zero, which decodes conservatively as
    // UNKNOWN topology and every domain UNPROBED.
    gc_u8 capabilityMemoryTopology;
    gc_u32 capabilityDomainsPacked;
    char detail[192];
};
enum : gc_u32 {
    SERVICE_GPU_CAPABILITY_PACKED_MASK = 0x3FFFu,
    SERVICE_GPU_MEMORY_TOPOLOGY_MAX = 2u,
};
static_assert(sizeof(ServiceGpuHealth) == 216,
              "ServiceGpuHealth wire size changed");
static_assert(offsetof(ServiceGpuHealth, capabilityMemoryTopology) == 19,
              "capability topology must reuse the first reserved byte");
static_assert(offsetof(ServiceGpuHealth, capabilityDomainsPacked) == 20,
              "packed capabilities must reuse the aligned reserved word");

static inline const char* service_gpu_health_reason_name(gc_u32 reason) {
    switch (reason) {
        case SERVICE_GPU_HEALTH_NONE: return "healthy";
        case SERVICE_GPU_HEALTH_NVML_UNAVAILABLE: return "NVML unavailable";
        case SERVICE_GPU_HEALTH_NVAPI_LIBRARY_UNAVAILABLE: return "NvAPI library unavailable";
        case SERVICE_GPU_HEALTH_NVAPI_INIT_FAILED: return "NvAPI initialization failed";
        case SERVICE_GPU_HEALTH_NVAPI_ENUM_FAILED: return "NvAPI enumeration failed";
        case SERVICE_GPU_HEALTH_NVAPI_HANDLE_UNRESOLVED: return "NvAPI GPU handle unresolved";
        case SERVICE_GPU_HEALTH_ARCHITECTURE_UNAVAILABLE: return "GPU architecture unavailable";
        case SERVICE_GPU_HEALTH_VF_INFO_FAILED: return "VF info read failed";
        case SERVICE_GPU_HEALTH_VF_STATUS_FAILED: return "VF status read failed";
        case SERVICE_GPU_HEALTH_VF_CONTROL_FAILED: return "VF control read failed";
        case SERVICE_GPU_HEALTH_VF_STRUCTURE_INVALID: return "VF data failed structural validation";
        case SERVICE_GPU_HEALTH_STATE_UNCERTAIN: return "daemon state is uncertain";
        default: return "unknown GPU health failure";
    }
}

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
    ServiceGpuHealth health;
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
    // Checked GUI mutation preconditions. Zero preserves CLI/bootstrap
    // compatibility. Instance + generation + target are always required for a
    // checked request; topology is additionally required for a VF domain.
    gc_u64 expectedServiceInstanceId;
    gc_u64 expectedGpuGeneration;
    gc_u64 expectedTopologySignature;
    // ServiceStartupPolicyMode for SERVICE_CMD_SET_STARTUP_POLICY; zero on every
    // other command, which the validator enforces. When the mode is PROFILE the
    // command carries the resolved settings in `desired`, the slot in
    // `profileSlot`, and the display name in `source` -- the daemon cannot read
    // the user's config.ini itself (ProtectHome=yes).
    gc_u32 startupMode;
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

static inline bool service_wire_string_is_terminated(
    const char* value, unsigned int count) {
    if (!value || count == 0) return false;
    for (unsigned int i = 0; i < count; ++i)
        if (value[i] == '\0') return true;
    return false;
}

static inline bool service_desired_bool_fields_valid(
    const DesiredSettings* desired) {
    if (!desired) return false;
    for (int i = 0; i < VF_NUM_POINTS; ++i)
        if (desired->hasCurvePoint[i] > 1) return false;
    const gc_bool8* flags[] = {
        &desired->hasLock, &desired->lockTracksAnchor,
        &desired->hasGpuOffset, &desired->hasMemOffset,
        &desired->hasPowerLimit, &desired->hasFan, &desired->fanAuto,
        &desired->resetOcBeforeApply,
    };
    for (const gc_bool8* flag : flags)
        if (*flag > 1) return false;
    for (int i = 0; i < FAN_CURVE_MAX_POINTS; ++i)
        if (desired->fanCurve.points[i].enabled > 1) return false;
    return true;
}

static inline bool service_gpu_bool_fields_valid(const GpuAdapterInfo* gpu) {
    return gpu && gpu->valid <= 1 && gpu->pciInfoValid <= 1 &&
        gpu->vfReadSupported <= 1 && gpu->vfWriteSupported <= 1 &&
        gpu->vfBestGuess <= 1;
}

// Flags a client may put on a request. RESET carries none by contract:
// validate_service_request_for_ipc() rejects any flag on RESET, and the
// interactive bit is only ever consumed on the APPLY path. Building the flag
// word here keeps both clients on that contract -- the Linux client used to set
// the interactive bit unconditionally, which made the daemon reject EVERY
// Reset with "invalid protocol fields".
// RESUME_RESTORE is on the same contract for the same reason: it is a
// machine-generated edge with no payload, and the daemon rejects any flag on it.
static inline gc_u32 service_request_flags_for_command(unsigned int command,
                                                       bool interactive) {
    if (command == SERVICE_CMD_RESET ||
        command == SERVICE_CMD_RESUME_RESTORE) return 0u;
    return interactive ? (gc_u32)SERVICE_REQUEST_FLAG_INTERACTIVE : 0u;
}

// service_request_reject_reason() and validate_service_request_for_ipc() live
// in service_protocol_validation.h, included at the bottom of this file, next
// to the response-side rules they mirror.

struct ServiceStateEnvelope {
    gc_u64 serviceInstanceId;
    gc_u64 stateRevision;
    gc_u64 gpuGeneration;
    gc_u64 topologySignature;
    gc_u32 gpuPhase;
    gc_u32 validSections;
    // Linux daemon startup-apply policy, published with every envelope so a
    // client never needs an extra round trip to render it.  Always
    // RESTORE_LAST/0 on Windows, which has its own logon coordinator.
    gc_u32 startupPolicyMode;
    gc_u32 startupPolicySlot;
    gc_bool8 activeDesiredValid;
    gc_bool8 reservedBool[7];
};

struct ServiceResponse {
    gc_u32 magic;
    gc_u32 version;
    gc_u32 status;
    // Daemon identity diagnostics. This occupied the reserved v11 word, so the
    // fixed magic/version/status prefix remains stable across the v12 bump.
    gc_u32 servicePid;
    gc_u32 serviceBuildNumber;
    gc_u32 operationState;
    // ServiceOutcomeSeverity. Stamped by the one place each producer writes a
    // response out, never by an individual command handler, so it cannot
    // contradict `status` and cannot be left behind by a new branch.
    gc_u32 outcomeSeverity;
    gc_u32 outcomeSeverityReserved;
    gc_u64 operationId;
    char serviceVersion[32];
    ServiceStateEnvelope state;
    ServiceSnapshot snapshot;
    // The service's ACTIVE intent, gated by state.activeDesiredValid. Never
    // anything else: the daemon stamps it onto every response it sends, so a
    // handler that borrowed this member for a second meaning had that meaning
    // silently overwritten on the way out.
    DesiredSettings desired;
    ControlState controlState;
    // The Linux daemon's boot-apply snapshot -- what it would write to the GPU
    // at the next start, which is NOT the same thing as what is applied now.
    // Populated only by the startup-policy commands; startupProfileValid says
    // whether this response carries it, and it may only be set while the
    // published policy mode is PROFILE. Always zero on Windows, which has its
    // own logon coordinator.
    DesiredSettings startupProfile;
    gc_bool8 startupProfileValid;
    gc_bool8 startupProfileReserved[7];
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
    canonicalize_gc_bool8(&s->health.vfSnapshotFresh);
    canonicalize_gc_bool8(&s->health.recoveryAttempted);
    canonicalize_gc_bool8(&s->health.recoverySucceeded);
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

static inline gc_u64 service_state_hash_u32(gc_u64 hash, gc_u32 value) {
    for (unsigned int i = 0; i < 4; ++i) {
        hash ^= (gc_u64)((value >> (i * 8)) & 0xFFu);
        hash *= 1099511628211ULL;
    }
    return hash;
}

static inline gc_u64 service_snapshot_topology_signature(
    const ServiceSnapshot* snapshot) {
    if (!snapshot) return 0;
    gc_u64 hash = 1469598103934665603ULL;
    if (snapshot->selectedAdapterIndex < snapshot->adapterCount &&
        snapshot->selectedAdapterIndex < MAX_GPU_ADAPTERS) {
        const GpuAdapterInfo* gpu =
            &snapshot->adapters[snapshot->selectedAdapterIndex];
        hash = service_state_hash_u32(hash, gpu->deviceId);
        hash = service_state_hash_u32(hash, gpu->subSystemId);
        hash = service_state_hash_u32(hash, gpu->pciRevisionId);
        hash = service_state_hash_u32(hash, gpu->extDeviceId);
        hash = service_state_hash_u32(hash, gpu->pciDomain);
        hash = service_state_hash_u32(hash, gpu->pciBus);
        hash = service_state_hash_u32(hash, gpu->pciDevice);
        hash = service_state_hash_u32(hash, gpu->pciFunction);
    }
    hash = service_state_hash_u32(hash, (gc_u32)snapshot->numPopulated);
    for (int i = 0; i < VF_NUM_POINTS; ++i) {
        // Topology is the complete populated-point map, not live frequency
        // telemetry (which legitimately changes after Apply/boost). Including
        // every index catches same-count/different-visibleMap transitions.
        bool populated = snapshot->curve[i].freq_kHz != 0;
        hash = service_state_hash_u32(hash, populated ? 1u : 0u);
        if (populated) {
            hash = service_state_hash_u32(hash, (gc_u32)i);
            hash = service_state_hash_u32(hash,
                snapshot->curve[i].volt_uV);
        }
    }
    return hash ? hash : 1;
}

// Trust-boundary validation for the envelope/response half of the protocol
// lives in its own header purely to keep this one inside the source-size
// ratchet; it is included below, so every consumer still gets one header.
#include "service_protocol_validation.h"

#endif // GREEN_CURVE_SERVICE_PROTOCOL_H
