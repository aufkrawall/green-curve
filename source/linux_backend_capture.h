// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
// Included after the existing ClkDomains backend, under the daemon runtime lane.
#include "hardware_capture_runtime.h"
#include "hardware_capture_aux.h"

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
        return hardware_capture_collect(c, domains, hardware_capture_aux, out, reason, reasonSize);
    };
    return hardware_capture_consistent(read, *output, why, size);
}
