// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#pragma once

#include <stddef.h>
#include <stdint.h>

#define SERVICE_OC_APPLY_STAMP_MAGIC   0x47434153u /* 'GCAS' */
// v4 (2026-10-21) appends the driver version the settings were applied under.
// A v3 stamp fails closed like any other legacy format: the next successful
// apply writes v4.
#define SERVICE_OC_APPLY_STAMP_VERSION 4u

// SystemBootEnvironmentInformation supplies a fresh 128-bit BootIdentifier
// for each Windows boot. Keep both halves: unlike the wall-clock-derived
// SystemTimeOfDayInformation.BootTime, this identity cannot move when Windows
// corrects the system clock while the service is running.
struct ServiceBootIdentity {
    uint64_t high;
    uint64_t low;
};

static inline bool service_boot_identity_valid(const ServiceBootIdentity& identity) {
    return identity.high != 0 || identity.low != 0;
}

static inline bool service_boot_identity_equal(const ServiceBootIdentity& a,
    const ServiceBootIdentity& b) {
    return a.high == b.high && a.low == b.low;
}

struct ServiceOcApplyProofStamp {
    uint32_t magic;
    uint32_t version;
    uint32_t size;
    uint32_t reserved;
    ServiceBootIdentity bootIdentity;
    uint64_t awakeTime100ns;
    // File version (MS, LS) of the NvAPI image the apply ran under; 0/0 when
    // unknown.  Compared at driver recovery: a different version on disk means
    // the driver was updated since the settings were proven applied.
    uint32_t driverVersionMs;
    uint32_t driverVersionLs;
};

// Both versions known and different.  Unknown on either side is "not proven".
static inline bool service_driver_version_changed(uint32_t previousMs, uint32_t previousLs,
                                                  uint32_t currentMs, uint32_t currentLs) {
    const bool previousKnown = previousMs != 0 || previousLs != 0;
    const bool currentKnown = currentMs != 0 || currentLs != 0;
    return previousKnown && currentKnown &&
        (previousMs != currentMs || previousLs != currentLs);
}

// The awake-time proof exists to catch settings that make the driver fail
// soon after they are applied.  A verified driver update is not that
// evidence -- its device restart says nothing about the settings -- so an
// immature proof does not block the replay then.  The proof itself must still
// be valid (same boot, a successful apply), and the recovery ledger's spam
// threshold still applies.  Before, a driver update inside the window latched
// the sticky UNSTABLE_APPLY lockout and the profile was silently not restored.
static inline bool service_driver_recovery_proof_accepts(uint64_t proofAgeMs,
                                                         uint64_t windowMs,
                                                         bool verifiedDriverUpdate) {
    return proofAgeMs >= windowMs || verifiedDriverUpdate;
}

// Pure boundary calculation. Querying the real clocks and reading the protected
// stamp are deliberately outside this helper so tests need no sleeps or Windows.
static inline bool service_compute_proof_age_ms(const ServiceOcApplyProofStamp* stamp,
    const ServiceBootIdentity& currentBootIdentity, uint64_t currentAwakeTime100ns,
    uint64_t* ageMsOut) {
    if (ageMsOut) *ageMsOut = 0;
    if (!stamp || stamp->magic != SERVICE_OC_APPLY_STAMP_MAGIC ||
        stamp->version != SERVICE_OC_APPLY_STAMP_VERSION ||
        stamp->size != sizeof(ServiceOcApplyProofStamp) ||
        stamp->reserved != 0 || !service_boot_identity_valid(stamp->bootIdentity) ||
        stamp->awakeTime100ns == 0 ||
        !service_boot_identity_valid(currentBootIdentity) ||
        !service_boot_identity_equal(stamp->bootIdentity, currentBootIdentity) ||
        currentAwakeTime100ns < stamp->awakeTime100ns) {
        return false;
    }
    uint64_t ageMs = (currentAwakeTime100ns - stamp->awakeTime100ns) / 10000ULL;
    if (ageMsOut) *ageMsOut = ageMs;
    return true;
}

static inline bool service_should_preserve_proof_after_standby(
    bool proofValid, uint64_t proofAgeMs, uint64_t requiredAgeMs) {
    return proofValid && requiredAgeMs != 0 && proofAgeMs >= requiredAgeMs;
}

struct ServiceRecoveryClockEntry {
    ServiceBootIdentity bootIdentity;
    uint64_t awakeTime100ns;
};

struct ServiceRecoveryEvidenceKey {
    uint64_t high;
    uint64_t low;
};

// Pure startup authorization gate. Disk parsing, nonce comparison, process
// identity/freshness checks, and SCM querying remain in the Windows shard, but
// every result must converge here before a startup may carry intent into the
// lifecycle reducer. A zero-initialized/ordinary start is therefore inert.
struct ServiceControlledRecoveryStartGate {
    bool argumentsValid;
    bool explicitlyRequested;
    bool scmStartReasonKnown;
    bool scmDemandStart;
    bool authorizationValid;
    bool helperValidated;
    bool snapshotValid;
};

enum ServiceControlledRecoveryScmStopDisposition {
    SERVICE_CONTROLLED_RECOVERY_SCM_REJECT = 0,
    SERVICE_CONTROLLED_RECOVERY_SCM_STOPPED = 1,
    SERVICE_CONTROLLED_RECOVERY_SCM_WAIT_FOR_STOPPED = 2,
};

// QueryServiceStatusEx explicitly does not guarantee a valid process ID while
// a service is STOP_PENDING. The helper has already verified the old process's
// dedicated exit through a pinned process handle, so STOP_PENDING remains the
// one legitimate transitional state even when SCM reports pid=0 or a stale
// value. Other non-stopped states must still identify the exact old generation.
static inline ServiceControlledRecoveryScmStopDisposition
service_classify_controlled_recovery_scm_stop_state(
    bool stopped, bool stopPending, uint32_t reportedProcessId,
    uint32_t expectedPreviousProcessId) {
    if (stopped) return SERVICE_CONTROLLED_RECOVERY_SCM_STOPPED;
    if (expectedPreviousProcessId == 0) {
        return SERVICE_CONTROLLED_RECOVERY_SCM_REJECT;
    }
    if (stopPending) {
        return SERVICE_CONTROLLED_RECOVERY_SCM_WAIT_FOR_STOPPED;
    }
    return reportedProcessId == expectedPreviousProcessId
        ? SERVICE_CONTROLLED_RECOVERY_SCM_WAIT_FOR_STOPPED
        : SERVICE_CONTROLLED_RECOVERY_SCM_REJECT;
}

// What the service does once its runtime mutex is poisoned (a thread died
// holding it, or hardware work wedged inside the driver).  The process must go
// either way; the question is how, and whether automatic restore stays armed.
enum ServiceRestartSnapshotState {
    SERVICE_RESTART_SNAPSHOT_ABSENT = 0,  // no settings intent was persisted
    SERVICE_RESTART_SNAPSHOT_PRESENT,
    SERVICE_RESTART_SNAPSHOT_UNKNOWN,     // could not be determined
};

enum ServicePoisonedRuntimeExit {
    // Corroborated driver failure with intent on disk: the nonce-bound helper
    // restarts the service, which replays that intent after the proof gates.
    SERVICE_POISONED_EXIT_CONTROLLED_RESTART = 0,
    // Corroborated driver failure with nothing to restore: an ordinary restart
    // replays nothing by construction, so there is nothing to lock out.
    // Latching here used to block the NEXT logon's configured profile because
    // a driver update happened while no profile was applied.
    SERVICE_POISONED_EXIT_ORDINARY_RESTART,
    // Anything else (uncorroborated poison, intent that cannot be proven
    // absent): stop without replay and latch the lockout.
    SERVICE_POISONED_EXIT_LATCH_AND_STOP,
};

// `liveIntentHint` is the unlocked in-memory flag: it may only ever make the
// outcome stricter, because the runtime it belongs to is no longer trusted.
// An undeterminable snapshot still attempts the controlled restart: its
// preparation re-proves the file by fingerprint and falls back to the latch.
static inline ServicePoisonedRuntimeExit service_poisoned_runtime_exit_plan(
    bool corroboratedDriverFailure, ServiceRestartSnapshotState snapshot,
    bool liveIntentHint) {
    if (!corroboratedDriverFailure) return SERVICE_POISONED_EXIT_LATCH_AND_STOP;
    if (snapshot != SERVICE_RESTART_SNAPSHOT_ABSENT)
        return SERVICE_POISONED_EXIT_CONTROLLED_RESTART;
    return liveIntentHint ? SERVICE_POISONED_EXIT_LATCH_AND_STOP
                          : SERVICE_POISONED_EXIT_ORDINARY_RESTART;
}

// What an exact-device arrival (CM STARTED) of the selected GPU means.
enum ServiceSelectedGpuArrivalAction {
    SERVICE_GPU_ARRIVAL_RECOVER = 0,        // controlled restart, then replay
    SERVICE_GPU_ARRIVAL_ABANDON_EVIDENCE,   // removal cycle with nothing to restore
    SERVICE_GPU_ARRIVAL_NONE,               // diagnostic / readiness only
};

// The selected GPU was already running when its notification was registered,
// so a STARTED without a preceding REMOVED means its driver was stopped and
// started again.  A driver update does exactly that (observed 2026-10-21: one
// STARTED, no REMOVED), and the stop discards every setting -- so with intent
// applied by this process it is recovery evidence on its own.  It used to be
// "diagnostic only", which left the OC wiped until the next logon whenever no
// NVML call happened to fault (no fan curve running, GUI closed).
// Not while a validated recovery write is still pending: in that fresh process
// the arrival is the readiness cue the write is waiting for, and the write
// re-checks the device generation itself.  A removal cycle keeps its rule.
static inline ServiceSelectedGpuArrivalAction service_selected_gpu_arrival_action(
    bool wasRemoved, bool hasIntent, bool recoveryWritePending) {
    if (wasRemoved)
        return hasIntent ? SERVICE_GPU_ARRIVAL_RECOVER : SERVICE_GPU_ARRIVAL_ABANDON_EVIDENCE;
    return (hasIntent && !recoveryWritePending) ? SERVICE_GPU_ARRIVAL_RECOVER
                                                : SERVICE_GPU_ARRIVAL_NONE;
}

static inline bool service_controlled_recovery_start_is_authorized(
    const ServiceControlledRecoveryStartGate& gate) {
    return gate.argumentsValid && gate.explicitlyRequested &&
        gate.scmStartReasonKnown && gate.scmDemandStart &&
        gate.authorizationValid && gate.helperValidated &&
        gate.snapshotValid;
}

static inline bool service_recovery_evidence_key_valid(const ServiceRecoveryEvidenceKey& key) {
    return key.high != 0 || key.low != 0;
}

static inline bool service_recovery_evidence_key_equal(const ServiceRecoveryEvidenceKey& a,
    const ServiceRecoveryEvidenceKey& b) {
    return a.high == b.high && a.low == b.low;
}

static inline bool service_recovery_evidence_already_recorded(
    const ServiceRecoveryEvidenceKey* keys, size_t count,
    const ServiceRecoveryEvidenceKey& candidate) {
    if (!keys || !service_recovery_evidence_key_valid(candidate)) return false;
    for (size_t i = 0; i < count; ++i) {
        if (service_recovery_evidence_key_equal(keys[i], candidate)) return true;
    }
    return false;
}

static inline unsigned int service_count_recent_recovery_clock_entries(
    const ServiceRecoveryClockEntry* entries, size_t count,
    const ServiceBootIdentity& currentBootIdentity, uint64_t currentAwakeTime100ns,
    uint64_t windowMs) {
    if (!entries || !service_boot_identity_valid(currentBootIdentity) ||
        currentAwakeTime100ns == 0) return 0;
    unsigned int recent = 0;
    for (size_t i = 0; i < count; ++i) {
        if (service_boot_identity_equal(entries[i].bootIdentity, currentBootIdentity) &&
            entries[i].awakeTime100ns != 0 &&
            entries[i].awakeTime100ns <= currentAwakeTime100ns &&
            ((currentAwakeTime100ns - entries[i].awakeTime100ns) / 10000ULL) <= windowMs) {
            ++recent;
        }
    }
    return recent;
}
