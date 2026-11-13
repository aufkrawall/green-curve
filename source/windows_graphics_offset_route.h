// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#pragma once
#include "curve_point_offset_policy.h"

// A resolved authoring route is explicit authority, including a zero whose
// control must be retained. Never let its integer MHz preview choose a setter.
static inline CurveOffsetRoutingPlan windows_graphics_offset_route(const DesiredSettings& desired,
    const CurveOffsetRoutingPlan& legacy) {
    if (!desired.authoring.graphicsPrecise) return legacy;
    CurveOffsetRoutingPlan route{};
    route.policyChangeRequested = true;
    route.gpuPolicyViaCurveBatch = desired.authoring.graphicsSurface == DESIRED_GRAPHICS_VF_COMPONENT;
    route.shouldApplyGpuOffset = !route.gpuPolicyViaCurveBatch;
    route.curveBatchGpuOffsetMHz = route.gpuPolicyViaCurveBatch ? desired.gpuOffsetMHz : 0;
    route.lastApplyUsedGpuOffset = desired_graphics_hz(desired) != 0;
    return route;
}
