// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#pragma once

#include <stddef.h>
#include <stdint.h>

#define SERVICE_OC_APPLY_STAMP_MAGIC   0x47434153u /* 'GCAS' */
#define SERVICE_OC_APPLY_STAMP_VERSION 3u

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
};

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
