// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT

#ifndef GREEN_CURVE_LINUX_GPU_SELECTION_H
#define GREEN_CURVE_LINUX_GPU_SELECTION_H

#include "gpu_core.h"
#include "gpu_selection_policy.h"

static inline bool linux_gpu_bdf_valid(const GpuAdapterInfo* gpu) {
    return gpu && gpu->valid && gpu->pciDomain <= 0xFFFFu &&
           gpu->pciBus <= 0xFFu && gpu->pciDevice <= 0x1Fu &&
           gpu->pciFunction <= 7u &&
           (gpu->pciDomain || gpu->pciBus || gpu->pciDevice || gpu->pciFunction);
}

static inline bool linux_gpu_extract_nvidia_device_id(
    unsigned int encoded, unsigned int* deviceId) {
    return gpu_pci_extract_nvidia_device_id(encoded, deviceId);
}

static inline bool linux_gpu_device_id_matches(unsigned int left,
                                                unsigned int right) {
    if (!left || !right) return true;
    if (left == right) return true;
    unsigned int leftDevice = 0, rightDevice = 0;
    return linux_gpu_extract_nvidia_device_id(left, &leftDevice) &&
        linux_gpu_extract_nvidia_device_id(right, &rightDevice) &&
        leftDevice == rightDevice;
}

static inline bool linux_gpu_subsystem_id_matches(unsigned int left,
                                                   unsigned int right) {
    if (!left || !right) return true;
    if (left == right) return true;
    unsigned int leftLow = left & 0xFFFFu;
    unsigned int leftHigh = (left >> 16) & 0xFFFFu;
    unsigned int rightLow = right & 0xFFFFu;
    unsigned int rightHigh = (right >> 16) & 0xFFFFu;
    if (left > 0xFFFFu && right > 0xFFFFu)
        return leftLow == rightHigh && leftHigh == rightLow;
    if (left <= 0xFFFFu)
        return left == rightLow || left == rightHigh;
    return right == leftLow || right == leftHigh;
}

static inline bool linux_gpu_identity_matches(const GpuAdapterInfo* requested,
                                               const GpuAdapterInfo* candidate) {
    if (!requested || !candidate || !requested->valid || !candidate->valid)
        return false;
    if (linux_gpu_bdf_valid(requested)) {
        if (!linux_gpu_bdf_valid(candidate) ||
            requested->pciDomain != candidate->pciDomain ||
            requested->pciBus != candidate->pciBus ||
            requested->pciDevice != candidate->pciDevice ||
            requested->pciFunction != candidate->pciFunction)
            return false;
    }
    if (requested->pciInfoValid) {
        if (!candidate->pciInfoValid ||
            !linux_gpu_device_id_matches(requested->deviceId, candidate->deviceId) ||
            (requested->subSystemId && candidate->subSystemId &&
             !linux_gpu_subsystem_id_matches(requested->subSystemId,
                                             candidate->subSystemId)))
            return false;
    }
    return linux_gpu_bdf_valid(requested) || requested->pciInfoValid;
}

// Read-only recovery already owns a known PCI GPU and must tolerate a transient
// loss of optional device/subsystem metadata. Exact BDF remains mandatory when
// available, and any identifiers present on both sides must not conflict.
// Mutation attachment intentionally continues using the stricter helper above.
static inline bool linux_gpu_same_pci_nonconflicting(
    const GpuAdapterInfo* known, const GpuAdapterInfo* candidate) {
    if (!known || !candidate || !known->valid || !candidate->valid)
        return false;
    if (linux_gpu_bdf_valid(known) && linux_gpu_bdf_valid(candidate)) {
        if (known->pciDomain != candidate->pciDomain ||
            known->pciBus != candidate->pciBus ||
            known->pciDevice != candidate->pciDevice ||
            known->pciFunction != candidate->pciFunction) return false;
        return !(known->pciInfoValid && candidate->pciInfoValid) ||
            (linux_gpu_device_id_matches(known->deviceId,
                                         candidate->deviceId) &&
             (!known->subSystemId || !candidate->subSystemId ||
              linux_gpu_subsystem_id_matches(known->subSystemId,
                                              candidate->subSystemId)));
    }
    return linux_gpu_identity_matches(known, candidate);
}

// The Linux daemon currently owns one selected backend and one active intent.
// Switching that backend while an intent belongs to another GPU would make the
// fan worker and state envelope apply/attribute the old intent to the new GPU.
// Until per-GPU runtime ownership exists, selection therefore fails closed.
static inline bool linux_gpu_switch_preserves_active_intent(
    bool hasActiveDesired, const GpuAdapterInfo* activeTarget,
    const GpuAdapterInfo* requestedTarget) {
    return !hasActiveDesired ||
        linux_gpu_identity_matches(activeTarget, requestedTarget);
}

// The snapshot index also names the adapter used for read-only telemetry when
// no multi-GPU write target has been selected. In that state, the first user
// selection must start at an endpoint instead of treating the telemetry index
// as an implicit choice.
static inline int linux_next_gpu_selection_index(
    bool hasSelectedTarget, unsigned int currentIndex,
    unsigned int adapterCount, int delta) {
    if (adapterCount == 0 || adapterCount > MAX_GPU_ADAPTERS) return -1;
    if (!hasSelectedTarget || currentIndex >= adapterCount)
        return delta < 0 ? (int)adapterCount - 1 : 0;
    int count = (int)adapterCount;
    int normalized = ((int)currentIndex + delta) % count;
    return normalized < 0 ? normalized + count : normalized;
}

// The model half of an identity: device and subsystem as NVML reports them
// (either NvAPI word order tolerated). Revision and extended device are not
// read on Linux, so they are compared only when both sides carry them.
static inline bool linux_gpu_model_known(const GpuAdapterInfo* gpu) {
    return gpu && gpu->valid && gpu->pciInfoValid && gpu->deviceId != 0;
}

static inline bool linux_gpu_model_matches(const GpuAdapterInfo* stored,
                                           const GpuAdapterInfo* live) {
    return linux_gpu_model_known(stored) && linux_gpu_model_known(live) &&
        linux_gpu_device_id_matches(stored->deviceId, live->deviceId) &&
        linux_gpu_subsystem_id_matches(stored->subSystemId,
                                       live->subSystemId) &&
        (!stored->pciRevisionId || !live->pciRevisionId ||
         stored->pciRevisionId == live->pciRevisionId) &&
        (!stored->extDeviceId || !live->extDeviceId ||
         stored->extDeviceId == live->extDeviceId);
}

static inline bool linux_gpu_bdf_equal(const GpuAdapterInfo* left,
                                       const GpuAdapterInfo* right) {
    return linux_gpu_bdf_valid(left) && linux_gpu_bdf_valid(right) &&
        left->pciDomain == right->pciDomain && left->pciBus == right->pciBus &&
        left->pciDevice == right->pciDevice &&
        left->pciFunction == right->pciFunction;
}

// Resolves a STORED target (config.ini, the daemon's committed or startup
// record, a request built from either) against live enumeration. Firmware
// assigns PCI bus numbers at boot, so a stored BDF goes stale when an upstream
// device appears or disappears. The rule is the Windows one
// (resolve_configured_gpu_selection): a known model must always match; one
// adapter at the stored location wins; otherwise the model is followed to its
// one live adapter when `role` allows it (configured_gpu_location_may_move).
// A bare location without a model never moves. Returns the adapter index, -1
// when nothing qualifies and -2 when the choice is ambiguous; *movedOut says
// the stored location no longer holds the GPU.  Live-to-live comparisons keep
// using linux_gpu_identity_matches().
static inline int linux_resolve_stored_gpu_target(
    const GpuAdapterInfo* stored, GpuLocationRole role,
    const GpuAdapterInfo* adapters, unsigned int count, bool* movedOut) {
    if (movedOut) *movedOut = false;
    if (!stored || !stored->valid || !adapters) return -1;
    const bool hasBdf = linux_gpu_bdf_valid(stored);
    const bool hasModel = linux_gpu_model_known(stored);
    if (!hasBdf && !hasModel) return -1;
    unsigned int modelMatches = 0, locationMatches = 0;
    int modelIndex = -1, locationIndex = -1;
    for (unsigned int i = 0; i < count && i < MAX_GPU_ADAPTERS; ++i) {
        const GpuAdapterInfo* live = &adapters[i];
        if (!live->valid) continue;
        if (hasModel && !linux_gpu_model_matches(stored, live)) continue;
        if (hasModel) {
            ++modelMatches;
            modelIndex = (int)i;
        }
        if (hasBdf && linux_gpu_bdf_equal(stored, live)) {
            ++locationMatches;
            locationIndex = (int)i;
        }
    }
    if (locationMatches == 1) return locationIndex;
    if (locationMatches > 1) return -2;
    if (modelMatches == 0) return -1;
    if (hasBdf && !configured_gpu_location_may_move(role, count)) return -1;
    if (modelMatches > 1) return -2;
    if (movedOut) *movedOut = hasBdf;
    return modelIndex;
}

// What a backend init selects. Every target handed to init is a stored one
// (the committed or startup record, or a request built from config.ini).
// Durable restores pass their role; requests use UNRECORDED because the wire
// carries no role. A role-less request may move only with one adapter; the client
// resolves config.ini with its recorded role before it sends anything. An
// untargeted init may write to a lone adapter; a targeted one only to the
// adapter its target resolved to, so a swapped-in different model never
// receives a stored intent. `index` is -1/-2 when nothing is selected for
// writes (the caller still reads telemetry from adapter 0).
struct LinuxGpuInitSelection {
    int index;
    bool writeResolved;
    bool moved;
};

static inline LinuxGpuInitSelection linux_gpu_init_selection(
    const GpuAdapterInfo* target, const GpuAdapterInfo* adapters,
    unsigned int count,
    GpuLocationRole role = GPU_LOCATION_ROLE_UNRECORDED) {
    LinuxGpuInitSelection selection = {-1, false, false};
    if (target && target->valid) {
        selection.index = linux_resolve_stored_gpu_target(target,
            role, adapters, count, &selection.moved);
        selection.writeResolved = selection.index >= 0;
    } else if (count == 1) {
        selection.index = 0;
        selection.writeResolved = true;
    }
    return selection;
}

// The role a selection of the live adapter `target` records (the Linux twin
// of configured_gpu_location_role_for_selection): advisory when no other
// adapter shares its model, required when one does, unrecorded without a
// location or a known model.
static inline GpuLocationRole linux_gpu_location_role_for_selection(
    const GpuAdapterInfo* target, const GpuAdapterInfo* adapters,
    unsigned int count) {
    if (!linux_gpu_bdf_valid(target) || !linux_gpu_model_known(target))
        return GPU_LOCATION_ROLE_UNRECORDED;
    unsigned int models = 0;
    for (unsigned int i = 0; adapters && i < count && i < MAX_GPU_ADAPTERS; ++i)
        if (linux_gpu_model_matches(target, &adapters[i])) ++models;
    return models == 1 ? GPU_LOCATION_ROLE_ADVISORY : GPU_LOCATION_ROLE_REQUIRED;
}

// A PCI location as `--gpu` and config.ini spell it (domain:bus:device.function,
// hex).  All-zero is not a GPU location.
static inline bool linux_gpu_parse_bdf(const char* text, GpuAdapterInfo* target) {
    if (!text || !target) return false;
    unsigned int domain = 0, bus = 0, device = 0, function = 0;
    if (!parse_pci_bdf_string(text, &domain, &bus, &device, &function) ||
        (domain == 0 && bus == 0 && device == 0 && function == 0))
        return false;
    *target = GpuAdapterInfo{};
    target->valid = true;
    target->pciDomain = domain;
    target->pciBus = bus;
    target->pciDevice = device;
    target->pciFunction = function;
    return true;
}

// What config.ini's [gpu] record holds.  A record whose location or role does
// not parse is MALFORMED, never "no record": no record writes untargeted (the
// daemon's lone-adapter rule), so reading a damaged `required` record as
// absent would let a write reach the identical GPU it exists to rule out.
// Windows blocks a malformed selection the same way.
enum LinuxGpuSelectionRecord {
    LINUX_GPU_SELECTION_RECORD_ABSENT = 0,
    LINUX_GPU_SELECTION_RECORD_PRESENT,
    LINUX_GPU_SELECTION_RECORD_MALFORMED,
};

// Classifies the raw `selected_bdf` and `selected_pci_location_role` texts.
// Neither key set is no record; either set makes it a record, which must then
// parse completely.  `target` and `roleOut` are written only for PRESENT.
static inline LinuxGpuSelectionRecord linux_gpu_selection_record_parse(
    const char* bdfText, const char* roleText, GpuAdapterInfo* target,
    GpuLocationRole* roleOut) {
    const bool hasBdf = bdfText && bdfText[0];
    const bool hasRole = roleText && roleText[0];
    if (!hasBdf && !hasRole) return LINUX_GPU_SELECTION_RECORD_ABSENT;
    GpuAdapterInfo parsed = {};
    GpuLocationRole role = GPU_LOCATION_ROLE_UNRECORDED;
    if (!hasBdf || !linux_gpu_parse_bdf(bdfText, &parsed) ||
        !configured_gpu_location_role_parse(hasRole ? roleText : "", &role))
        return LINUX_GPU_SELECTION_RECORD_MALFORMED;
    if (target) *target = parsed;
    if (roleOut) *roleOut = role;
    return LINUX_GPU_SELECTION_RECORD_PRESENT;
}

static inline const char* linux_gpu_selection_record_name(
    LinuxGpuSelectionRecord record) {
    switch (record) {
    case LINUX_GPU_SELECTION_RECORD_ABSENT: return "absent";
    case LINUX_GPU_SELECTION_RECORD_PRESENT: return "present";
    case LINUX_GPU_SELECTION_RECORD_MALFORMED: return "malformed";
    }
    return "invalid";
}

// config.ini's GPU in the client, from loading the record to the requests that
// name it.  NOT_PRESENT, AMBIGUOUS and MALFORMED are final for the process:
// only an explicit selection (`--gpu`, the TUI selector) replaces them, as on
// Windows.
enum LinuxConfiguredGpuStatus {
    LINUX_CONFIGURED_GPU_NONE = 0,     // no record: requests go untargeted
    LINUX_CONFIGURED_GPU_EXPLICIT,     // `--gpu` or a TUI selection
    LINUX_CONFIGURED_GPU_PENDING,      // loaded, not resolved yet
    LINUX_CONFIGURED_GPU_RESOLVED,     // `target` is the live adapter
    LINUX_CONFIGURED_GPU_UNVALIDATED,  // no live adapter list to resolve against
    LINUX_CONFIGURED_GPU_NOT_PRESENT,
    LINUX_CONFIGURED_GPU_AMBIGUOUS,
    LINUX_CONFIGURED_GPU_MALFORMED,    // the record is damaged or unreadable
};

// The status a freshly loaded selection starts in; an explicit `--gpu`
// replaces whatever config.ini holds.
static inline LinuxConfiguredGpuStatus linux_configured_gpu_status_for_record(
    LinuxGpuSelectionRecord record, bool explicitTarget) {
    if (explicitTarget) return LINUX_CONFIGURED_GPU_EXPLICIT;
    switch (record) {
    case LINUX_GPU_SELECTION_RECORD_PRESENT: return LINUX_CONFIGURED_GPU_PENDING;
    case LINUX_GPU_SELECTION_RECORD_ABSENT: return LINUX_CONFIGURED_GPU_NONE;
    case LINUX_GPU_SELECTION_RECORD_MALFORMED: break;
    }
    return LINUX_CONFIGURED_GPU_MALFORMED;
}

struct LinuxConfiguredGpu {
    GpuAdapterInfo target;  // the record until it resolves, then the live adapter
    GpuLocationRole role;
    LinuxConfiguredGpuStatus status;
};

static inline const char* linux_configured_gpu_status_name(
    LinuxConfiguredGpuStatus status) {
    switch (status) {
    case LINUX_CONFIGURED_GPU_NONE: return "none";
    case LINUX_CONFIGURED_GPU_EXPLICIT: return "explicit";
    case LINUX_CONFIGURED_GPU_PENDING: return "pending";
    case LINUX_CONFIGURED_GPU_RESOLVED: return "resolved";
    case LINUX_CONFIGURED_GPU_UNVALIDATED: return "unvalidated";
    case LINUX_CONFIGURED_GPU_NOT_PRESENT: return "not-present";
    case LINUX_CONFIGURED_GPU_AMBIGUOUS: return "ambiguous";
    case LINUX_CONFIGURED_GPU_MALFORMED: return "malformed";
    }
    return "invalid";
}

// Resolves a PENDING or UNVALIDATED selection against one live adapter list
// with the recorded role.  Returns the adapter index once RESOLVED (`target`
// then holds that adapter), otherwise -1; any other status is left alone.
static inline int linux_configured_gpu_resolve(LinuxConfiguredGpu* gpu,
    const GpuAdapterInfo* adapters, unsigned int count, bool* movedOut) {
    if (movedOut) *movedOut = false;
    if (!gpu || (gpu->status != LINUX_CONFIGURED_GPU_PENDING &&
                 gpu->status != LINUX_CONFIGURED_GPU_UNVALIDATED)) return -1;
    if (!adapters || count == 0) {
        gpu->status = LINUX_CONFIGURED_GPU_UNVALIDATED;
        return -1;
    }
    int index = linux_resolve_stored_gpu_target(&gpu->target, gpu->role,
                                                adapters, count, movedOut);
    if (index < 0) {
        gpu->status = index == -2 ? LINUX_CONFIGURED_GPU_AMBIGUOUS
                                  : LINUX_CONFIGURED_GPU_NOT_PRESENT;
        return -1;
    }
    gpu->target = adapters[index];
    gpu->status = LINUX_CONFIGURED_GPU_RESOLVED;
    return index;
}

// What a request may name for config.ini's GPU; false when it must not go
// out.  Only a live adapter is ever sent.  The daemon resolves a request
// target without the location role (UNRECORDED follows a moved model to the
// one adapter left), and a request without a target reaches a lone adapter,
// so a stale record sent as it is, or dropped, can land on the identical GPU
// that a `required` role exists to tell apart.  A write therefore stops on
// every outcome but NONE, EXPLICIT and RESOLVED (MALFORMED included: a damaged
// record is not "no record"); a read goes untargeted and shows the daemon's
// current GPU without selecting another.
static inline bool linux_configured_gpu_request_target(
    const LinuxConfiguredGpu* gpu, bool write,
    const GpuAdapterInfo** targetOut) {
    if (targetOut) *targetOut = nullptr;
    if (!gpu || gpu->status == LINUX_CONFIGURED_GPU_NONE) return true;
    if ((gpu->status == LINUX_CONFIGURED_GPU_EXPLICIT ||
         gpu->status == LINUX_CONFIGURED_GPU_RESOLVED) && gpu->target.valid) {
        if (targetOut) *targetOut = &gpu->target;
        return true;
    }
    return !write;
}

// Why a write stopped, for the CLI error and the TUI status line.
static inline const char* linux_configured_gpu_problem(
    LinuxConfiguredGpuStatus status) {
    switch (status) {
    case LINUX_CONFIGURED_GPU_NOT_PRESENT:
        return "the GPU saved in config.ini is not present";
    case LINUX_CONFIGURED_GPU_AMBIGUOUS:
        return "the GPU saved in config.ini is ambiguous";
    case LINUX_CONFIGURED_GPU_MALFORMED:
        return "the GPU saved in config.ini is malformed or unreadable";
    case LINUX_CONFIGURED_GPU_UNVALIDATED:
        return "the GPU saved in config.ini could not be checked against the "
               "daemon's GPUs";
    case LINUX_CONFIGURED_GPU_PENDING:
        return "the GPU saved in config.ini has not been checked yet";
    default:
        return "the GPU saved in config.ini is unavailable";
    }
}

// One bounded diagnostic line for a stored-target resolution: the stored
// model, location and role, then the first four live adapters.
static inline void linux_format_stored_gpu_detail(
    const GpuAdapterInfo* stored, GpuLocationRole role,
    const GpuAdapterInfo* adapters, unsigned int count,
    char* out, size_t outSize) {
    if (!out || outSize == 0) return;
    out[0] = 0;
    if (!stored) return;
    char location[24] = "unknown";
    if (linux_gpu_bdf_valid(stored))
        snprintf(location, sizeof(location), "%04x:%02x:%02x.%u",
                 stored->pciDomain, stored->pciBus, stored->pciDevice,
                 stored->pciFunction);
    int written = snprintf(out, outSize,
        "stored=%08X/%08X@%s role=%s adapters=%u live=[",
        linux_gpu_model_known(stored) ? stored->deviceId : 0u,
        linux_gpu_model_known(stored) ? stored->subSystemId : 0u, location,
        configured_gpu_location_role_name(role), count);
    if (written < 0 || (size_t)written >= outSize) return;
    size_t used = (size_t)written;
    const unsigned int shown = count < 4u ? count : 4u;
    for (unsigned int i = 0; adapters && i < shown; ++i) {
        const GpuAdapterInfo* live = &adapters[i];
        const char* kind = linux_gpu_model_matches(stored, live)
            ? "model" : "other";
        written = linux_gpu_bdf_valid(live)
            ? snprintf(out + used, outSize - used, "%s%u:%s@%04x:%02x:%02x.%u",
                i ? " " : "", i, kind, live->pciDomain, live->pciBus,
                live->pciDevice, live->pciFunction)
            : snprintf(out + used, outSize - used, "%s%u:%s@unknown",
                i ? " " : "", i, kind);
        if (written < 0 || (size_t)written >= outSize - used) return;
        used += (size_t)written;
    }
    snprintf(out + used, outSize - used, "%s]", count > shown ? " ..." : "");
}

#endif
