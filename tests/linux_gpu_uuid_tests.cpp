// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#include "linux_gpu_uuid.h"
#include <initializer_list>

namespace {
const char* const UUID_A = "GPU-01234567-89ab-cdef-0123-456789abcdef";
const char* const UUID_B = "GPU-11234567-89ab-cdef-0123-456789abcdef";
GpuAdapterInfo adapter(unsigned int bus) {
    GpuAdapterInfo gpu = {};
    gpu.valid = true; gpu.pciInfoValid = true;
    gpu.deviceId = 0x2f0410deu; gpu.subSystemId = 0x123410deu;
    gpu.pciBus = bus;
    return gpu;
}
nvmlReturn_t failed_query(nvmlDevice_t, char* text, unsigned int size) {
    memset(text, 'x', size);
    return NVML_ERROR_NOT_SUPPORTED;
}
nvmlReturn_t unterminated_query(nvmlDevice_t, char* text, unsigned int size) {
    memset(text, 'x', size);
    return NVML_SUCCESS;
}
}

int run_linux_gpu_uuid_tests() {
    const LinuxGpuUuid a = linux_gpu_uuid_parse(UUID_A), b = linux_gpu_uuid_parse(UUID_B);
    const LinuxGpuUuid empty = {};
    if (!linux_gpu_uuid_valid(&a) || !linux_gpu_uuid_valid(&b) ||
        linux_gpu_uuid_valid(&empty)) return 7460;
    const LinuxGpuUuid upper = linux_gpu_uuid_parse("GPU-01234567-89AB-CDEF-0123-456789ABCDEF");
    const LinuxGpuUuid malformed = linux_gpu_uuid_parse("GPU-invalid");
    if (strcmp(a.text, upper.text) || linux_gpu_uuid_valid(&malformed)) return 7461;
    const GpuAdapterInfo selected = adapter(7), moved[] = {adapter(6)};
    const LinuxGpuUuid live[] = {a};
    LinuxGpuUuidVerdict verdict;
    auto result = linux_gpu_restore_selection(&selected, GPU_LOCATION_ROLE_REQUIRED,
        moved, 1, &a, live, &verdict);
    if (!result.writeResolved || !result.moved || result.index != 0 ||
        verdict != LINUX_GPU_UUID_MATCHED) return 7462;
    const LinuxGpuUuid other[] = {b};
    for (auto role : {GPU_LOCATION_ROLE_UNRECORDED, GPU_LOCATION_ROLE_ADVISORY}) {
        result = linux_gpu_restore_selection(&selected, role, moved, 1, &a, other, &verdict);
        if (result.writeResolved || verdict != LINUX_GPU_UUID_MISMATCH) return 7463;
        result = linux_gpu_restore_selection(&selected, role, moved, 1, nullptr, other, &verdict);
        if (!result.writeResolved || verdict != LINUX_GPU_UUID_FALLBACK) return 7464;
        result = linux_gpu_restore_selection(&selected, role, moved, 1, &a, &empty, &verdict);
        if (!result.writeResolved || verdict != LINUX_GPU_UUID_FALLBACK) return 7465;
    }
    const GpuAdapterInfo twins[] = {adapter(7), adapter(8)};
    const LinuxGpuUuid twinsUuid[] = {b, a};
    result = linux_gpu_restore_selection(&selected, GPU_LOCATION_ROLE_REQUIRED,
        twins, 2, &a, twinsUuid, &verdict);
    if (!result.writeResolved || result.index != 1 || !result.moved) return 7466;
    result = linux_gpu_restore_selection(&selected, GPU_LOCATION_ROLE_REQUIRED,
        twins, 1, &a, twinsUuid, &verdict);
    if (result.writeResolved || verdict != LINUX_GPU_UUID_MISMATCH) return 7467;
    const LinuxGpuUuid duplicates[] = {a, a};
    result = linux_gpu_restore_selection(&selected, GPU_LOCATION_ROLE_ADVISORY,
        twins, 2, &a, duplicates, &verdict);
    if (result.writeResolved || verdict != LINUX_GPU_UUID_AMBIGUOUS) return 7468;
    auto read = linux_gpu_read_uuid(nullptr, nullptr);
    auto failed = linux_gpu_read_uuid(failed_query, nullptr);
    auto unterminated = linux_gpu_read_uuid(unterminated_query, nullptr);
    if (linux_gpu_uuid_valid(&read) || linux_gpu_uuid_valid(&failed) ||
        linux_gpu_uuid_valid(&unterminated)) return 7469;
    const auto retained = linux_gpu_uuid_retain(&failed, &a);
    if (strcmp(retained.text, a.text)) return 7470;
    result = linux_gpu_restore_selection(&selected, GPU_LOCATION_ROLE_REQUIRED,
        moved, 1, &a, &empty, &verdict);
    if (result.writeResolved || verdict != LINUX_GPU_UUID_FALLBACK) return 7471;
    GpuAdapterInfo different[] = {adapter(6)};
    different[0].deviceId++;
    result = linux_gpu_restore_selection(&selected, GPU_LOCATION_ROLE_ADVISORY,
        different, 1, &a, live, &verdict);
    if (result.writeResolved || verdict != LINUX_GPU_UUID_MISMATCH) return 7472;
    return 0;
}
