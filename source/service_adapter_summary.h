// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// service_adapter_summary.h -- what the adapter list says about each card
// (protocol v40), next to the one full ServiceSnapshot of the selected GPU.
//
// ServiceSnapshot.adapterSummaries[] is index-parallel to adapters[]. It
// carries each card's physical UUID when the driver provides one (so a client
// can key per-card data without loading NVML itself) and a compact summary:
// whether the service runs the card and, if so, its phase, health, restore
// lockout, uncertainty and fan ownership; which card holds Green Curve
// settings; and which card still has a fan to hand back. It is never N full
// snapshots.
//
// While the service runs one GPU, RUNTIME is set on that card only and every
// other entry carries identity alone. Nothing here limits RUNTIME to the
// selected index, so a service that runs every card fills every entry without
// another wire change.

#ifndef GREEN_CURVE_SERVICE_ADAPTER_SUMMARY_H
#define GREEN_CURVE_SERVICE_ADAPTER_SUMMARY_H

// NVML's physical GPU UUID ("GPU-xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx") in
// binary. All zero means absent.
struct ServiceGpuUuid { gc_u8 bytes[16]; };
static_assert(sizeof(ServiceGpuUuid) == 16, "ServiceGpuUuid is a fixed wire record");

enum ServiceAdapterSummaryFlag : gc_u32 {
    SERVICE_ADAPTER_UUID = 1u << 0,         // uuid is the driver's physical GPU UUID
    SERVICE_ADAPTER_RUNTIME = 1u << 1,      // the service runs this card; the fields below are its own
    SERVICE_ADAPTER_ACTIVE = 1u << 2,       // this card holds Green Curve settings
    SERVICE_ADAPTER_UNCERTAIN = 1u << 3,    // its hardware state is unknown until a Reset (RUNTIME)
    SERVICE_ADAPTER_FAN_OWNED = 1u << 4,    // Green Curve drives one of its fans (RUNTIME)
    SERVICE_ADAPTER_FAN_FAULT = 1u << 5,    // a fan was handed back to the driver (RUNTIME)
    // A fan Green Curve left in manual control (a crash, an interrupted
    // handback) still has to be returned to the driver; writes stay blocked
    // until it is. Marked on the card the ownership marker names.
    SERVICE_ADAPTER_FAN_HANDBACK = 1u << 6,
    SERVICE_ADAPTER_KNOWN_FLAGS = 0x7Fu,
};

struct ServiceAdapterSummary {
    ServiceGpuUuid uuid;
    gc_u32 flags;                    // ServiceAdapterSummaryFlag
    gc_u32 gpuPhase;                 // ServiceGpuPhase; RUNTIME only
    gc_u32 healthReason;             // ServiceGpuHealthReason; RUNTIME only
    gc_u32 autoRestoreLockoutReason; // ServiceAutoRestoreLockoutReason; RUNTIME only
    gc_u32 activeProfileSource;      // ServiceProfileSource; ACTIVE only (NONE when unknown)
    gc_u32 activeProfileSlot;        // ACTIVE only
};
static_assert(sizeof(ServiceAdapterSummary) == 40, "ServiceAdapterSummary is a fixed wire record");

static inline bool service_gpu_uuid_present(const ServiceGpuUuid& uuid) {
    for (gc_u8 b : uuid.bytes)
        if (b) return true;
    return false;
}

static inline bool service_gpu_uuid_equal(const ServiceGpuUuid& a, const ServiceGpuUuid& b) {
    for (unsigned int i = 0; i < sizeof(a.bytes); ++i)
        if (a.bytes[i] != b.bytes[i]) return false;
    return true;
}

static inline int service_gpu_uuid_hex(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// Parses exactly "GPU-" + 8-4-4-4-12 hex digits (any case), terminated.
// Anything else -- MIG identifiers, truncation, an all-zero UUID -- is absent.
static inline bool service_gpu_uuid_parse(const char* text, ServiceGpuUuid* out) {
    if (!out) return false;
    *out = ServiceGpuUuid{};
    if (!text || text[0] != 'G' || text[1] != 'P' || text[2] != 'U' || text[3] != '-')
        return false;
    ServiceGpuUuid parsed{};
    unsigned int nibble = 0;
    unsigned int i = 4;
    for (; i < 40; ++i) {
        const char c = text[i];
        if (i == 12 || i == 17 || i == 22 || i == 27) {
            if (c != '-') return false;
            continue;
        }
        const int value = service_gpu_uuid_hex(c);
        if (value < 0) return false;
        parsed.bytes[nibble / 2] = (gc_u8)(parsed.bytes[nibble / 2] | (value << ((nibble % 2) ? 0 : 4)));
        ++nibble;
    }
    if (text[i] != 0 || nibble != 32 || !service_gpu_uuid_present(parsed)) return false;
    *out = parsed;
    return true;
}

// Lower-case canonical text; `size` must hold 41 bytes.
static inline bool service_gpu_uuid_format(const ServiceGpuUuid& uuid, char* out, size_t size) {
    static const char digits[] = "0123456789abcdef";
    if (!out || size < 41 || !service_gpu_uuid_present(uuid)) return false;
    out[0] = 'G'; out[1] = 'P'; out[2] = 'U'; out[3] = '-';
    unsigned int at = 4;
    for (unsigned int b = 0; b < 16; ++b) {
        if (b == 4 || b == 6 || b == 8 || b == 10) out[at++] = '-';
        out[at++] = digits[uuid.bytes[b] >> 4];
        out[at++] = digits[uuid.bytes[b] & 0xF];
    }
    out[at] = 0;
    return true;
}

static inline bool service_adapter_summaries_are_zero(const ServiceAdapterSummary* summaries) {
    const gc_u8* bytes = (const gc_u8*)summaries;
    for (size_t i = 0; i < sizeof(ServiceAdapterSummary) * MAX_GPU_ADAPTERS; ++i)
        if (bytes[i]) return false;
    return true;
}

// Why an adapter summary list is incoherent, or nullptr. `count` is the
// snapshot's adapterCount (already bounded by the caller). The selected
// card's entry, when the service runs it, must agree with the envelope phase
// and the full snapshot's health.
static inline const char* service_adapter_summaries_reject_reason(
    const ServiceAdapterSummary* summaries, unsigned int count,
    unsigned int selectedIndex, gc_u32 selectedPhase, gc_u32 selectedHealth) {
    if (selectedIndex < count && (summaries[selectedIndex].flags & SERVICE_ADAPTER_RUNTIME) &&
        (summaries[selectedIndex].gpuPhase != selectedPhase ||
         summaries[selectedIndex].healthReason != selectedHealth))
        return "selected adapter summary disagrees with its snapshot";
    for (unsigned int i = 0; i < MAX_GPU_ADAPTERS; ++i) {
        const ServiceAdapterSummary& s = summaries[i];
        const bool uuidPresent = service_gpu_uuid_present(s.uuid);
        if (i >= count) {
            if (s.flags || uuidPresent || s.gpuPhase || s.healthReason ||
                s.autoRestoreLockoutReason || s.activeProfileSource || s.activeProfileSlot)
                return "adapter summary beyond the adapter count";
            continue;
        }
        if (s.flags & ~(gc_u32)SERVICE_ADAPTER_KNOWN_FLAGS) return "unknown adapter summary flag";
        if (((s.flags & SERVICE_ADAPTER_UUID) != 0) != uuidPresent)
            return "adapter UUID flag disagrees with its UUID";
        if (uuidPresent) {
            for (unsigned int j = 0; j < i; ++j)
                if ((summaries[j].flags & SERVICE_ADAPTER_UUID) &&
                    service_gpu_uuid_equal(summaries[j].uuid, s.uuid))
                    return "two adapters publish the same UUID";
        }
        if (!(s.flags & SERVICE_ADAPTER_RUNTIME)) {
            if (s.gpuPhase || s.healthReason || s.autoRestoreLockoutReason ||
                (s.flags & (SERVICE_ADAPTER_UNCERTAIN | SERVICE_ADAPTER_FAN_OWNED |
                            SERVICE_ADAPTER_FAN_FAULT)))
                return "runtime state on an adapter the service does not run";
        } else if (s.gpuPhase > SERVICE_GPU_PHASE_DEGRADED ||
                   s.healthReason > SERVICE_GPU_HEALTH_STATE_UNCERTAIN ||
                   s.autoRestoreLockoutReason > SERVICE_AUTO_RESTORE_LOCKOUT_MAX) {
            return "adapter runtime state out of range";
        }
        if (!(s.flags & SERVICE_ADAPTER_ACTIVE)) {
            if (s.activeProfileSource || s.activeProfileSlot)
                return "profile identity on an adapter without settings";
        } else if (s.activeProfileSource > SERVICE_PROFILE_SOURCE_AD_HOC ||
                   s.activeProfileSlot > 255u) {
            return "adapter profile identity out of range";
        }
    }
    return nullptr;
}

// What a producer knows about its cards. uuids[] is index-parallel to the
// adapter list (absent = all zero); ownerIndex is the card holding Green Curve
// settings and servedIndex the card the service runs (-1: none). The runtime
// fields describe servedIndex.
struct ServiceAdapterSummaryInputs {
    const ServiceGpuUuid* uuids;
    unsigned int count;
    int ownerIndex;
    int servedIndex;
    int fanHandbackIndex;  // card whose fan handback is pending, -1 none
    bool uncertain, fanOwned, fanFault;
    gc_u32 gpuPhase, healthReason, autoRestoreLockoutReason;
    gc_u32 activeProfileSource, activeProfileSlot;
};

// Fills out[MAX_GPU_ADAPTERS] coherently. A UUID two adapters report is
// withheld from both (it cannot key either card); returns how many were.
static inline unsigned int service_adapter_summaries_build(
    const ServiceAdapterSummaryInputs& in, ServiceAdapterSummary* out) {
    for (unsigned int i = 0; i < MAX_GPU_ADAPTERS; ++i) out[i] = ServiceAdapterSummary{};
    const unsigned int count = in.count < MAX_GPU_ADAPTERS ? in.count : MAX_GPU_ADAPTERS;
    unsigned int withheld = 0;
    for (unsigned int i = 0; in.uuids && i < count; ++i) {
        if (!service_gpu_uuid_present(in.uuids[i])) continue;
        bool shared = false;
        for (unsigned int j = 0; j < count; ++j)
            if (j != i && service_gpu_uuid_equal(in.uuids[i], in.uuids[j])) shared = true;
        if (shared) { ++withheld; continue; }
        out[i].uuid = in.uuids[i];
        out[i].flags |= SERVICE_ADAPTER_UUID;
    }
    if (in.ownerIndex >= 0 && (unsigned int)in.ownerIndex < count) {
        ServiceAdapterSummary& owner = out[in.ownerIndex];
        owner.flags |= SERVICE_ADAPTER_ACTIVE;
        if (in.activeProfileSource <= SERVICE_PROFILE_SOURCE_AD_HOC && in.activeProfileSlot <= 255u) {
            owner.activeProfileSource = in.activeProfileSource;
            owner.activeProfileSlot = in.activeProfileSlot;
        }
    }
    if (in.fanHandbackIndex >= 0 && (unsigned int)in.fanHandbackIndex < count)
        out[in.fanHandbackIndex].flags |= SERVICE_ADAPTER_FAN_HANDBACK;
    if (in.servedIndex >= 0 && (unsigned int)in.servedIndex < count) {
        ServiceAdapterSummary& served = out[in.servedIndex];
        served.flags |= SERVICE_ADAPTER_RUNTIME;
        if (in.uncertain) served.flags |= SERVICE_ADAPTER_UNCERTAIN;
        if (in.fanOwned) served.flags |= SERVICE_ADAPTER_FAN_OWNED;
        if (in.fanFault) served.flags |= SERVICE_ADAPTER_FAN_FAULT;
        served.gpuPhase = in.gpuPhase;
        served.healthReason = in.healthReason;
        served.autoRestoreLockoutReason = in.autoRestoreLockoutReason;
    }
    return withheld;
}

#endif  // GREEN_CURVE_SERVICE_ADAPTER_SUMMARY_H
