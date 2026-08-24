// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// Pure per-domain control-surface capability classification, shared by the
// Windows GUI/service, the Linux daemon, and the regression harness.
//
// WHY THIS EXISTS
// ---------------
// Family detection (`vf_backend_for_architecture`) answers exactly one
// question: which private NVAPI struct layout does this architecture use.  It
// has never answered the *other* question — which control domains this
// particular board actually exposes — because on every discrete GeForce board
// we have tested, the answer was "all of them".
//
// Integrated Grace/Blackwell SoC parts (NVIDIA RTX Spark / GB10 and the
// Tegra/Jetson lineage before it) break that assumption while still reporting a
// Blackwell architecture: system RAM is unified with the CPU (no dedicated
// memory controller to offset), the power budget is an SoC-wide one rather than
// a board TGP, and the fan is typically owned by the platform EC rather than by
// the NVIDIA driver.  Selecting the Blackwell VF layout for such a part is
// correct; claiming it is a fully validated discrete board is not.
//
// THE CORE INVARIANT
// ------------------
// This layer only ever *subtracts* a domain after the driver has actually
// refused it.  A zero-initialized `GpuCapabilityProbe` (every domain UNPROBED)
// therefore reports every domain available and `gpu_capability_is_complete()`
// true, which reproduces today's behavior byte for byte on x64 and on every
// validated discrete family.  Nothing here may become a new way for a working
// GPU to lose a capability it has.  See `gpu_capability_is_complete()`.

#ifndef GREEN_CURVE_GPU_CAPABILITY_POLICY_H
#define GREEN_CURVE_GPU_CAPABILITY_POLICY_H

#include "gpu_core.h"

// Mutation-domain bits are declared in service_protocol.h, which includes this
// header's prerequisites rather than the other way round.  The bit positions
// are part of the wire protocol and are mirrored here as indices so this header
// stays free of protocol dependencies.
#define GPU_CAP_DOMAIN_COUNT 10

enum GpuDomainCapability : gc_u32 {
    // Never asked.  Treated as available — see the core invariant above.
    GPU_DOMAIN_CAP_UNPROBED = 0,
    // The driver answered a read of this domain's control surface.
    GPU_DOMAIN_CAP_AVAILABLE = 1,
    // The control surface exists but the driver refused it on this part.
    GPU_DOMAIN_CAP_REFUSED = 2,
    // The control surface does not exist on this part at all.
    GPU_DOMAIN_CAP_ABSENT = 3,
};

enum GpuMemoryTopology : gc_u32 {
    GPU_MEMORY_TOPOLOGY_UNKNOWN = 0,
    // Discrete board with its own VRAM and memory controller.
    GPU_MEMORY_TOPOLOGY_DEDICATED = 1,
    // SoC sharing one physical memory pool with the CPU (GB10-class parts).
    GPU_MEMORY_TOPOLOGY_UNIFIED = 2,
};

enum GpuControlSurfaceClass : gc_u32 {
    // Every domain available.  Indistinguishable from a validated discrete board.
    GPU_CONTROL_SURFACE_FULL = 0,
    // Some write domains available, some refused/absent.
    GPU_CONTROL_SURFACE_PARTIAL = 1,
    // No write domain available; reads/telemetry only.
    GPU_CONTROL_SURFACE_MONITOR_ONLY = 2,
};

struct GpuCapabilityProbe {
    // Indexed by mutation-domain bit position (0..GPU_CAP_DOMAIN_COUNT-1).
    gc_u32 domain[GPU_CAP_DOMAIN_COUNT];
    gc_u32 memoryTopology;
    gc_bool8 reserved[8];
};

// Bit position -> domain mask, and back.  Kept as small pure helpers so callers
// can iterate domains without duplicating the protocol bit layout.
// These are constexpr so the protocol-binding static_asserts in vf_backends.cpp
// can evaluate them at compile time.
constexpr gc_u32 gpu_capability_mask_for_index(int index) {
    if (index < 0 || index >= GPU_CAP_DOMAIN_COUNT) return 0;
    return (gc_u32)1u << (unsigned)index;
}

constexpr int gpu_capability_index_for_mask(gc_u32 mask) {
    for (int i = 0; i < GPU_CAP_DOMAIN_COUNT; ++i) {
        if (gpu_capability_mask_for_index(i) == mask) return i;
    }
    return -1;
}

static inline void gpu_capability_set(GpuCapabilityProbe* probe, gc_u32 mask,
                                      gc_u32 capability) {
    if (!probe) return;
    int index = gpu_capability_index_for_mask(mask);
    if (index < 0) return;
    if (capability > GPU_DOMAIN_CAP_ABSENT) return;
    probe->domain[index] = capability;
}

constexpr gc_u32 gpu_capability_get(const GpuCapabilityProbe* probe,
                                    gc_u32 mask) {
    if (!probe) return GPU_DOMAIN_CAP_UNPROBED;
    int index = gpu_capability_index_for_mask(mask);
    if (index < 0) return GPU_DOMAIN_CAP_UNPROBED;
    return probe->domain[index];
}

// Compact wire representation carried in ServiceGpuHealth.  Two bits are
// sufficient for each of the ten domain capability values, so the complete
// probe fits in the protocol's existing reserved bytes without changing the
// ServiceResponse size or forcing an otherwise unnecessary protocol bump.
constexpr gc_u32 gpu_capability_packed_domain_mask() {
    return ((gc_u32)1u << (GPU_CAP_DOMAIN_COUNT * 2u)) - 1u;
}

constexpr gc_u32 gpu_capability_pack_domains(
    const GpuCapabilityProbe* probe) {
    if (!probe) return 0;
    gc_u32 packed = 0;
    for (int i = 0; i < GPU_CAP_DOMAIN_COUNT; ++i) {
        gc_u32 capability = probe->domain[i];
        if (capability > GPU_DOMAIN_CAP_ABSENT)
            capability = GPU_DOMAIN_CAP_UNPROBED;
        packed |= capability << ((unsigned)i * 2u);
    }
    return packed;
}

static inline void gpu_capability_unpack_domains(
    GpuCapabilityProbe* probe, gc_u32 packed, gc_u32 memoryTopology) {
    if (!probe) return;
    for (int i = 0; i < GPU_CAP_DOMAIN_COUNT; ++i)
        probe->domain[i] = (packed >> ((unsigned)i * 2u)) & 0x3u;
    probe->memoryTopology = memoryTopology <= GPU_MEMORY_TOPOLOGY_UNIFIED
        ? memoryTopology : GPU_MEMORY_TOPOLOGY_UNKNOWN;
    for (size_t i = 0; i < sizeof(probe->reserved); ++i)
        probe->reserved[i] = 0;
}

static_assert(gpu_capability_packed_domain_mask() ==
                  SERVICE_GPU_CAPABILITY_PACKED_MASK,
              "capability wire mask must match service protocol storage");

// A domain is "known missing" only when the driver actually said so.
constexpr bool gpu_capability_domain_is_missing(gc_u32 capability) {
    return capability == GPU_DOMAIN_CAP_REFUSED ||
           capability == GPU_DOMAIN_CAP_ABSENT;
}

constexpr gc_u32 gpu_capability_missing_domains(
    const GpuCapabilityProbe* probe) {
    if (!probe) return 0;
    gc_u32 missing = 0;
    for (int i = 0; i < GPU_CAP_DOMAIN_COUNT; ++i) {
        if (gpu_capability_domain_is_missing(probe->domain[i]))
            missing |= gpu_capability_mask_for_index(i);
    }
    return missing;
}

// Every domain that is not known-missing.  UNPROBED counts as available so a
// zero-initialized probe subtracts nothing.
constexpr gc_u32 gpu_capability_available_domains(
    const GpuCapabilityProbe* probe) {
    gc_u32 all = 0;
    for (int i = 0; i < GPU_CAP_DOMAIN_COUNT; ++i)
        all |= gpu_capability_mask_for_index(i);
    if (!probe) return all;
    return all & ~gpu_capability_missing_domains(probe);
}

// THE inertness predicate.  While this returns true, every caller added for
// integrated-SoC support must behave exactly as it did before this header
// existed.  Regression tests pin both directions.
constexpr bool gpu_capability_is_complete(const GpuCapabilityProbe* probe) {
    return gpu_capability_missing_domains(probe) == 0;
}

// Domains that represent an actual hardware write (RESET_BASELINE is a write
// but is derived from the others, so it does not classify the surface on its
// own).
constexpr gc_u32 gpu_capability_write_domains() {
    return gpu_capability_mask_for_index(1) |   // GPU_OFFSET
           gpu_capability_mask_for_index(2) |   // MEM_OFFSET
           gpu_capability_mask_for_index(3) |   // POWER
           gpu_capability_mask_for_index(4) |   // VF_CURVE
           gpu_capability_mask_for_index(5) |   // LOCK
           gpu_capability_mask_for_index(6) |   // FAN
           gpu_capability_mask_for_index(7) |   // XBAR
           gpu_capability_mask_for_index(8) |   // SYS_CLK
           gpu_capability_mask_for_index(9);    // VIDEO_CLK
}

constexpr gc_u32 gpu_capability_surface_class(
    const GpuCapabilityProbe* probe) {
    gc_u32 writes = gpu_capability_write_domains();
    gc_u32 missing = gpu_capability_missing_domains(probe) & writes;
    if (missing == 0) return GPU_CONTROL_SURFACE_FULL;
    if (missing == writes) return GPU_CONTROL_SURFACE_MONITOR_ONLY;
    return GPU_CONTROL_SURFACE_PARTIAL;
}

// A memory-clock write on a unified pool is not merely likely to be refused —
// it targets the same physical DRAM the CPU is executing from.  That is a
// different risk class from a discrete board rejecting an out-of-range offset,
// and is the one place integrated topology earns an explicit confirmation
// rather than a warning.  Unknown topology is NOT risky: it must not start
// prompting on the discrete boards that report nothing.
constexpr bool gpu_capability_memory_write_is_risky(
    const GpuCapabilityProbe* probe) {
    return probe && probe->memoryTopology == GPU_MEMORY_TOPOLOGY_UNIFIED;
}

// What a single read-only probe of one domain observed.  Platform code fills
// this in from NVML/NvAPI; the classification below stays pure and testable.
struct GpuDomainObservation {
    // The NVML/NvAPI entry point for this domain resolved in the loaded driver.
    gc_bool8 entryPointPresent;
    // A read-only call against it returned success.
    gc_bool8 readSucceeded;
    // The driver answered and positively reported no such unit (e.g. zero fans).
    gc_bool8 hardwareAbsent;
    gc_bool8 reserved[5];
};

// The conservatism here is deliberate and load-bearing.  An unresolved entry
// point means an OLDER DRIVER, not absent hardware — classifying it as missing
// would start raising the limited-surface warning on existing x64 installs
// whose driver predates an optional API they never use.  Only positive
// evidence (the driver answered, and answered "none" or "no") may downgrade a
// domain.
constexpr gc_u32 gpu_capability_classify(const GpuDomainObservation* obs) {
    if (!obs) return GPU_DOMAIN_CAP_UNPROBED;
    if (!obs->entryPointPresent) return GPU_DOMAIN_CAP_UNPROBED;
    if (obs->hardwareAbsent) return GPU_DOMAIN_CAP_ABSENT;
    if (!obs->readSucceeded) return GPU_DOMAIN_CAP_REFUSED;
    return GPU_DOMAIN_CAP_AVAILABLE;
}

// Memory-topology classification from the adapter's reported memory split.
//
// A discrete board reports GBs of dedicated video memory; an integrated/unified
// part reports approximately none and leans on shared system memory.  The
// threshold is deliberately low (256 MiB): every discrete GeForce Green Curve
// supports is far above it, so a working discrete GPU can never be
// misclassified as unified and start prompting.
//
// Both-zero (or a failed query) is UNKNOWN, not UNIFIED — absence of evidence
// must not manufacture a topology fact.
#define GPU_DEDICATED_VRAM_FLOOR_BYTES (256ull * 1024ull * 1024ull)

constexpr gc_u32 gpu_memory_topology_from_sizes(unsigned long long dedicatedBytes,
                                                unsigned long long sharedBytes) {
    if (dedicatedBytes >= GPU_DEDICATED_VRAM_FLOOR_BYTES)
        return GPU_MEMORY_TOPOLOGY_DEDICATED;
    if (sharedBytes >= GPU_DEDICATED_VRAM_FLOOR_BYTES)
        return GPU_MEMORY_TOPOLOGY_UNIFIED;
    return GPU_MEMORY_TOPOLOGY_UNKNOWN;
}

// A discrete board and a unified memory pool are mutually exclusive: every
// GeForce card Green Curve has validated owns its VRAM, so a part that reports a
// *recognized discrete* family AND a unified pool is positively an integrated
// SoC wearing that family's architecture id — a GB10-class Grace Blackwell part
// reporting Blackwell is exactly this.
//
// This is what closes the case where such a part answers every domain: the
// per-domain probe alone finds nothing missing, so nothing would flag hardware
// nobody has validated.  The contradiction is the evidence.
//
// Still positive-evidence-only, so it cannot reach existing hardware: a discrete
// GPU reports DEDICATED, and a failed or absent topology query reports UNKNOWN.
// Both return false here.
constexpr bool gpu_capability_topology_contradicts_discrete(
    const GpuCapabilityProbe* probe) {
    return probe && probe->memoryTopology == GPU_MEMORY_TOPOLOGY_UNIFIED;
}

static inline const char* gpu_capability_name(gc_u32 capability) {
    switch (capability) {
        case GPU_DOMAIN_CAP_UNPROBED: return "unprobed";
        case GPU_DOMAIN_CAP_AVAILABLE: return "available";
        case GPU_DOMAIN_CAP_REFUSED: return "refused by driver";
        case GPU_DOMAIN_CAP_ABSENT: return "absent on this part";
        default: return "unknown";
    }
}

static inline const char* gpu_capability_domain_name(int index) {
    switch (index) {
        case 0: return "reset-baseline";
        case 1: return "gpu-offset";
        case 2: return "memory-offset";
        case 3: return "power-limit";
        case 4: return "vf-curve";
        case 5: return "clock-lock";
        case 6: return "fan";
        case 7: return "xbar";
        case 8: return "sys-clk";
        case 9: return "video-clk";
        default: return "unknown-domain";
    }
}

static inline const char* gpu_memory_topology_name(gc_u32 topology) {
    switch (topology) {
        case GPU_MEMORY_TOPOLOGY_DEDICATED: return "dedicated";
        case GPU_MEMORY_TOPOLOGY_UNIFIED: return "unified with CPU";
        default: return "unknown";
    }
}

static inline const char* gpu_control_surface_class_name(gc_u32 surfaceClass) {
    switch (surfaceClass) {
        case GPU_CONTROL_SURFACE_FULL: return "full";
        case GPU_CONTROL_SURFACE_PARTIAL: return "partial";
        case GPU_CONTROL_SURFACE_MONITOR_ONLY: return "monitor-only";
        default: return "unknown";
    }
}

#endif // GREEN_CURVE_GPU_CAPABILITY_POLICY_H
