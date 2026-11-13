// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
static int hardware_capture_constraints_cases() {
    Driver driver{}; NvmlApi api{}; api.getClockOffsets = clock;
    auto c = context(driver, api); char why[192]{};
    auto observation = std::make_unique<HardwareCaptureObservation>();
    const auto read = [&](auto& out, char* reason, size_t size) {
        return collect(c, PROFILE_DOMAIN_VF_CURVE, out, reason, size);
    };
    if (!hardware_capture_consistent(read, *observation, why, sizeof(why))) return 8720;
    const auto& vf = observation->vfLimits;
    if (!vf.valid || !vf.gridValid || vf.gridHz != 500 || !vf.backendWritable || !vf.rangeReported ||
        vf.rangeSource != VF_OFFSET_RANGE_SOURCE_NVML_GRAPHICS || vf.minimumHz != -3000000000LL ||
        vf.maximumHz != 2000000000LL) return 8721;
    char summary[512]{}; hardware_capture_summary(*observation, summary, sizeof(summary));
    if (!strstr(summary, "grid=500 Hz") || !strstr(summary, "reported graphics bounds")) return 8736;
    const auto fingerprint = hardware_capture_fingerprint(*observation);
    auto modified = std::make_unique<HardwareCaptureObservation>(*observation);
    modified->vfLimits.gridHz = 1000;
    if (!hardware_capture_observation_valid(*modified) || hardware_capture_fingerprint(*modified) == fingerprint) return 8722;
    for (int kind = 0; kind < 5; ++kind) {
        *modified = *observation;
        if (kind == 0) modified->vfLimits.valid = 2;
        if (kind == 1) modified->vfLimits.gridHz = 0;
        if (kind == 2) modified->vfLimits.rangeReported = 0;
        if (kind == 3) modified->vfLimits.valid = 0;
        if (kind == 4) modified->schemaVersion = HARDWARE_CAPTURE_SCHEMA_CURRENT - 1;
        if (hardware_capture_observation_valid(*modified)) return 8723 + kind;
    }
    driver.rangeFails = true;
    if (!hardware_capture_consistent(read, *observation, why, sizeof(why)) || !vf.valid || vf.rangeReported ||
        vf.rangeSource != VF_OFFSET_RANGE_SOURCE_FALLBACK || vf.minimumHz != -1000000000LL ||
        vf.maximumHz != 1000000000LL || vf.gridHz != 500) return 8728;
    hardware_capture_summary(*observation, summary, sizeof(summary));
    if (!strstr(summary, "application policy")) return 8737;
    VfBackendSpec readOnly = g_vfBackendPascal; readOnly.writeSupported = false; c.backend = &readOnly;
    if (!hardware_capture_consistent(read, *observation, why, sizeof(why)) || vf.backendWritable) return 8729;
    for (gc_u32 i = 0; i < observation->pointCount; ++i)
        if (observation->points[i].writableState != 2) return 8730;
    auto document = std::make_unique<ProfileDocument>();
    if (!hardware_capture_to_document(*observation, *document, why, sizeof(why)) ||
        (document->domains & PROFILE_DOMAIN_VF_CURVE) || driver.unexpectedQueries) return 8731;
    const auto memoryRead = [&](auto& out, char* reason, size_t size) {
        return collect(c, PROFILE_DOMAIN_VF_CURVE | PROFILE_DOMAIN_MEMORY, out, reason, size);
    };
    if (!hardware_capture_consistent(memoryRead, *observation, why, sizeof(why)) ||
        !hardware_capture_to_document(*observation, *document, why, sizeof(why)) ||
        document->domains != PROFILE_DOMAIN_MEMORY || !document->hasMemoryOffset) return 8735;
    return 0;
}
