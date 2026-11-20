// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
// Included after the existing ClkDomains backend, under the daemon runtime lane.
#include "hardware_capture_runtime.h"
#include "hardware_capture_aux.h"

// Called on the daemon runtime lane by every passive capture read.
static void linux_capture_log_client_locks(const NvapiClientLocks& locks) {
    static NvapiClientLocksLogGate gate{};
    if (!nvapi_client_locks_log_changed(gate, locks)) return;
    char line[192];
    nvapi_client_locks_describe(locks, line, sizeof(line));
    lb_log("%s\n", line);
}

// Called on the daemon runtime lane by every passive capture read that includes
// memory: logs getter vs running-clock memory evidence once per changed reading.
static void linux_capture_log_memory_clock(const MemoryClockReading& reading, gc_i64 getterHz,
                                           const HardwareCaptureObservation& out) {
    static char last[256]{};
    char line[256];
    snprintf(line, sizeof(line),
        "capture memory: getter=%lldHz (valid=%d surface=%s) running=%u MHz (pstates=%d) board=%u MHz (nvml=%d)"
        " -> %lldHz evidence=%s",
        (long long)getterHz, out.memory.valid ? 1 : 0, hardware_capture_clock_surface_name(out.memory.surface),
        reading.runningMaxMHz, reading.pstatesStatus, reading.boardMaxMHz, reading.boardStatus,
        (long long)out.memory.value, hardware_capture_memory_evidence_name(out.memoryEvidence));
    if (!strcmp(line, last)) return;
    snprintf(last, sizeof(last), "%s", line);
    lb_log("%s\n", line);
}

bool linux_backend_capture_current(const LinuxGpuState* gpu, gc_u64 generation, gc_u32 domains,
    const DesiredSettings* ownedFan, HardwareCaptureObservation* output, char* why, size_t size) {
    if (!gpu || !output) return false;
    const auto read = [&](HardwareCaptureObservation& out, char* reason, size_t reasonSize) {
        HardwareCaptureContext c{};
        c.usable = gpu->nvapiInitialized || gpu->nvmlReady;
        c.gpuHandle = gpu->gpuHandle; c.query = gpu->nvapiQi; c.backend = gpu->backend;
        c.nvmlUsable = gpu->nvmlReady; c.nvml = &gpu->nvml; c.nvmlDevice = gpu->nvmlDevice;
        c.binding = hardware_capture_binding(gpu->selectedGpu); c.gpuGeneration = generation;
        c.osFamily = PROFILE_SOURCE_OS_LINUX; c.backendFamily = (gc_u32)gpu->family;
        char version[65]{};
        auto getVersion = gpu->nvmlLib ? sym<nvmlReturn_t (*)(char*, unsigned int)>(gpu->nvmlLib, "nvmlSystemGetDriverVersion") : nullptr;
        if (getVersion && getVersion(version, sizeof(version)) == NVML_SUCCESS)
            memcpy(c.driverVersion, version, sizeof(version));
        c.ownedFanIntent = ownedFan;
        c.lockObserved = linux_capture_log_client_locks;
        c.memoryClockReader = hardware_capture_memory_clock;
        c.memoryObserved = linux_capture_log_memory_clock;
        return hardware_capture_collect(c, domains, hardware_capture_aux, out, reason, reasonSize);
    };
    return hardware_capture_consistent(read, *output, why, size);
}
