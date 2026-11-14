// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#ifndef GREEN_CURVE_SERVICE_CONTROL_REVIEW_H
#define GREEN_CURVE_SERVICE_CONTROL_REVIEW_H
#include "hardware_capture_runtime.h"

#include "service_control_review_revision.h"

static inline void service_control_review_stamp(HardwareCaptureObservation& o, const DesiredSettings* owned) {
    o.revision = service_control_review_revision(o.fingerprint, owned);
}

// Read must use an already bound, usable session and the bounded passive
// collector. It must never initialize, refresh recovery, select, or write.
template <class Read>
static inline bool service_control_review_matches(const ServiceRequest& request,
    const DesiredSettings* owned, Read read, char* why, size_t size) {
    if (!request.expectedControlFingerprint && !request.expectedControlDomains) return true;
    const char* reason = service_control_preconditions_reject_reason(request);
    if (reason) { if (why && size) snprintf(why, size, "%s", reason); return false; }
    auto observation = std::make_unique<HardwareCaptureObservation>();
    if (!read(*observation, why, size)) return false;
    const auto& o = *observation;
    const auto binding = hardware_capture_binding(request.targetGpu);
    if (!o.valid || !hardware_capture_observation_valid(o) || !o.fingerprint ||
        o.fingerprint != hardware_capture_fingerprint(o) ||
        o.requestedDomains != request.expectedControlDomains ||
        o.gpuGeneration != request.expectedGpuGeneration ||
        memcmp(&o.binding, &binding, sizeof(binding)) ||
        request.expectedControlFingerprint != service_control_review_revision(o.fingerprint, owned)) {
        if (why && size) snprintf(why, size, "%s",
            "Reviewed GPU controls, ownership, or binding changed; capture and review again before Apply");
        return false;
    }
    return true;
}
#endif
