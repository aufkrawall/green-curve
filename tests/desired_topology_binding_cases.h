// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
static int desired_topology_binding_cases() {
    VFCurvePoint original[VF_NUM_POINTS]{}, shifted[VF_NUM_POINTS]{};
    DesiredSettings d{};
    d.applicationScope = DESIRED_SCOPE_UPDATE_INCLUDED;
    d.authoring.topologyBound = 1; d.authoring.topologyCount = 8;
    for (int i = 0; i < 8; ++i) {
        original[i] = {1000000u + (unsigned)i * 100000u, 600000u + (unsigned)i * 25000u};
        shifted[i + 1] = original[i];
        d.authoring.voltageUv[i] = original[i].volt_uV;
    }
    original[8] = shifted[9] = {5000000, 500000}; // foreign clock domain
    auto floor = vf_floor_topology_compute(shifted, nullptr, VF_NUM_POINTS);
    d.curvePointCaptured[1] = d.curvePointCaptured[4] = 1;
    d.curvePointCapturedOffsetHz[1] = -500;
    d.hasCurvePoint[2] = 1; d.curvePointMHz[2] = 1100;
    d.authoring.pointFrequencyPrecise[2] = 1; d.authoring.pointFrequencyHz[2] = 1100000500;
    d.hasLock = 1; d.lockCi = 5; d.lockMode = LOCK_MODE_FLATTEN; d.lockMHz = 1500;
    d.hasGpuOffset = 1; d.gpuOffsetMHz = -15;
    d.authoring.graphicsPrecise = 1; d.authoring.graphicsOffsetHz = -15000500;
    d.authoring.graphicsSurface = DESIRED_GRAPHICS_VF_COMPONENT;
    d.authoring.identityExclusions = 1; d.authoring.graphicsExcluded[3] = 1;
    DesiredSettings mapped{}; char why[160]{}; int moved = 0;
    if (!desired_rebind_topology(d, shifted, floor, &mapped, why, sizeof(why), &moved) || moved != 5 ||
        mapped.curvePointCaptured[1] || mapped.curvePointCapturedOffsetHz[2] != -500 ||
        !mapped.curvePointCaptured[5] || mapped.curvePointCapturedOffsetHz[5] || mapped.lockCi != 6 ||
        mapped.authoring.pointFrequencyHz[3] != 1100000500 || mapped.authoring.pointFrequencyHz[2] ||
        !mapped.authoring.graphicsExcluded[4] || mapped.authoring.graphicsExcluded[3] ||
        mapped.authoring.voltageUv[0] || mapped.authoring.voltageUv[8] != 775000) return 8680;
    DesiredSettings stable = mapped;
    if (!desired_rebind_topology(mapped, shifted, floor, &mapped, why, sizeof(why), &moved) || moved ||
        !desired_authoring_equal(stable.authoring, mapped.authoring)) return 8681;
    shifted[0] = shifted[2];
    mapped = stable; moved = 77;
    if (desired_rebind_topology(d, shifted, floor, &mapped, why, sizeof(why), &moved) || moved != 77 ||
        !desired_authoring_equal(stable.authoring, mapped.authoring) || !strstr(why, "ambiguous")) return 8682;
    shifted[0] = {}; shifted[2] = {};
    if (desired_rebind_topology(d, shifted, floor, &mapped, why, sizeof(why)) || !strstr(why, "missing")) return 8683;
    shifted[2] = original[1]; floor.floorEnd = 5; floor.floorRun = 4;
    if (desired_rebind_topology(d, shifted, floor, &mapped, why, sizeof(why)) || !strstr(why, "not writable")) return 8684;
    floor = vf_floor_topology_compute(shifted, nullptr, VF_NUM_POINTS);
    DesiredSettings request{}; request.applicationScope = DESIRED_SCOPE_REPLACE_PROFILE;
    request.hasFan = request.fanAuto = 1;
    ServiceScopedApplyPlan plan{};
    if (!service_build_scoped_apply_plan(&d, request, shifted, floor, false, &plan, why, sizeof(why)) ||
        plan.remappedPoints != 5 || plan.owned.hasLock || plan.owned.hasGpuOffset ||
        !plan.hardware.curvePointCaptured[2] || !plan.hardware.curvePointCaptured[8] ||
        plan.hardware.curvePointCaptured[1] == 0 || plan.hardware.curvePointCaptured[4] ||
        !plan.hardware.authoring.topologyBound || plan.hardware.authoring.voltageUv[2] != 625000 ||
        !desired_authoring_valid(&plan.hardware)) return 8685;
    // Exact named scalar surfaces never acquire VF ownership from a curve.
    request = {}; request.hasGpuOffset = 1; request.authoring.graphicsPrecise = 1;
    request.authoring.graphicsSurface = DESIRED_GRAPHICS_P0;
    if (service_scoped_graphics_offset_in_vf(request, true, true)) return 8686;
    request.authoring.graphicsSurface = DESIRED_GRAPHICS_GLOBAL;
    if (service_scoped_graphics_offset_in_vf(request, true, true)) return 8687;
    request.authoring.graphicsSurface = DESIRED_GRAPHICS_VF_COMPONENT;
    if (!service_scoped_graphics_offset_in_vf(request, false, false) ||
        service_scoped_point_owned(d, 3, 3, true) || !service_scoped_point_owned(d, 0, 0, true)) return 8688;
    request = {}; request.applicationScope = DESIRED_SCOPE_UPDATE_INCLUDED;
    request.hasFan = request.fanAuto = 1;
    if (!service_build_scoped_apply_plan(&d, request, nullptr, {}, false, &plan, why, sizeof(why)) ||
        plan.remappedPoints || !desired_authoring_equal(d.authoring, plan.owned.authoring)) return 8689;
    // A complete table proof can preserve distinct identities at equal voltages.
    // Without that proof, equal voltages remain ambiguous.
    original[2].volt_uV = original[1].volt_uV;
    d.authoring.voltageUv[2] = original[2].volt_uV;
    floor = vf_floor_topology_compute(original, nullptr, VF_NUM_POINTS);
    if (!desired_rebind_topology(d, original, floor, &mapped, why, sizeof(why), &moved) || moved ||
        !mapped.curvePointCaptured[1] || mapped.authoring.pointFrequencyHz[2] != 1100000500) return 8690;
    return 0;
}
