// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
// Included after daemon state and backend helpers; no setters are called here.
#ifndef GREEN_CURVE_LINUX_DAEMON_SCOPED_APPLY_H
#define GREEN_CURVE_LINUX_DAEMON_SCOPED_APPLY_H

static bool daemon_resolve_scoped_ownership(const DesiredSettings* previous,
    DesiredSettings* requested, DesiredSettings* owned, char* why, size_t whySize) {
    linux_backend_refresh(&g_gpu);
    vf_floor_binding_update(&g_gpu.vfFloor, g_gpu.curve, g_gpu.freqOffsets, VF_NUM_POINTS, 0);
    ServiceScopedApplyPlan plan{};
    if (!service_build_scoped_apply_plan(previous, *requested, g_gpu.curve, g_gpu.vfFloor.floor,
            false, &plan, why, whySize, true)) return false;
    if (plan.hardware.memOffsetPrecise) {
        LinuxHardwareSnapshot snapshot{};
        if (!linux_backend_capture_snapshot(&g_gpu, &snapshot, why, whySize) ||
            !snapshot.memOffsetExact || !service_bind_scoped_memory_surface(
                &plan, snapshot.memOffsetSurface, why, whySize)) return false;
        // The independently refreshed backend must use the same resolved API
        // as the ownership record prepared before its transaction.
        if (requested->hasMemOffset) desired_memory_intent_copy(requested, &plan.hardware);
    }
    *owned = plan.owned;
    return true;
}

static bool daemon_prepare_apply_ownership(const DesiredSettings* previous,
    DesiredSettings* requested, DesiredSettings* owned, char* why, size_t whySize) {
    service_upgrade_legacy_scalar_scope(previous, requested);
    *owned = service_merge_desired_after_mutation(previous, requested);
    if (requested->applicationScope != DESIRED_SCOPE_LEGACY &&
        !daemon_resolve_scoped_ownership(previous, requested, owned, why, whySize)) return false;
    if (requested->resetOcBeforeApply)
        dlog("daemon apply: reset baseline intent merge previousCurvePoints=%d requestedCurvePoints=%d "
             "committedCurvePoints=%d preservedPower=%d preservedFan=%d\n",
             service_desired_curve_point_count(previous), service_desired_curve_point_count(requested),
             service_desired_curve_point_count(owned), owned->hasPowerLimit ? 1 : 0, owned->hasFan ? 1 : 0);
    return true;
}
#endif
