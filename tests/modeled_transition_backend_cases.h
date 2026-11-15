// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
// Included by the scripted-driver fixture after both production guard helpers.
static int modeled_transition_backend_cases() {
    struct Restore {
        App app = g_app; Api api = g_nvml_api;
        unsigned int oldCap = cap, oldResets = resets, oldCalls = armCalls;
        bool oldRefuse = refuseArm, oldReset = refuseReset; int oldCode = armRefusalCode;
        ~Restore() {
            g_app = app; g_nvml_api = api; cap = oldCap; resets = oldResets; armCalls = oldCalls;
            refuseArm = oldRefuse; refuseReset = oldReset; armRefusalCode = oldCode;
            linux_apply_ceiling_reset_state();
        }
    } restore;
    DesiredSettings request{}; request.hasGpuOffset = true; request.gpuOffsetMHz = 100;
    ApplyClockCeilingPlan modeled{}; modeled.required = modeled.arm = true;
    modeled.ceilingMHz = 1800; modeled.reason = APPLY_CEILING_REASON_MODELED_CURVE_TRANSITION;
    g_app = {}; g_app.gpuFamily = GPU_FAMILY_TURING;
    g_nvml_api = {}; g_nvml_api.setGpuLockedClocks = setCap; g_nvml_api.resetGpuLockedClocks = resetCap;
    cap = 0; refuseArm = refuseReset = false;
    unsigned int before = armCalls;
    {
        ApplyClockCeilingGuard ordinary(&request);
        if (ordinary.arm() != APPLY_CEILING_ARM_NOT_NEEDED || ordinary.must_refuse_transition() || armCalls != before) return 9583;
    }
    {
        ApplyClockCeilingGuard guarded(&request, false, true, &modeled);
        if (guarded.arm() != APPLY_CEILING_ARM_INSTALLED || guarded.must_refuse_transition() || cap != 1800 ||
            guarded.plan.symmetricFallbackAllowed || armCalls != before + 1) return 9584;
    }
    g_app = {}; cap = 0; refuseArm = true; armRefusalCode = NVML_ERROR_NOT_SUPPORTED; before = armCalls;
    {
        ApplyClockCeilingGuard guarded(&request, false, false, &modeled);
        if (guarded.arm() != APPLY_CEILING_ARM_UNSUPPORTED || !guarded.must_refuse_transition() ||
            cap || armCalls != before + 1) return 9585;
    }
    g_nvml_api.setGpuLockedClocks = nullptr; before = armCalls;
    {
        ApplyClockCeilingGuard guarded(&request, false, true, &modeled);
        if (guarded.arm() != APPLY_CEILING_ARM_UNSUPPORTED || !guarded.must_refuse_transition() || armCalls != before) return 9586;
    }
    LinuxGpuState gpu{}; gpu.family = GPU_FAMILY_TURING;
    gpu.nvml.setGpuLockedClocks = setCap; gpu.nvml.resetGpuLockedClocks = resetCap;
    refuseArm = false; linux_apply_ceiling_reset_state(); before = armCalls;
    if (!linux_apply_arm_transition_ceiling(&gpu, &request, nullptr, false, &modeled) ||
        cap != 1800 || armCalls != before + 1 || g_linuxCeilingPlan.symmetricFallbackAllowed ||
        !linux_apply_write_final_lock(&gpu, &request) || cap) return 9587;
    refuseArm = true; before = armCalls; linux_apply_ceiling_reset_state();
    if (linux_apply_arm_transition_ceiling(&gpu, &request, nullptr, false, &modeled) ||
        g_linuxCeilingArmResult != APPLY_CEILING_ARM_UNSUPPORTED || g_linuxCeilingArmed || armCalls != before + 1) return 9588;
    gpu.nvml.setGpuLockedClocks = nullptr; before = armCalls; linux_apply_ceiling_reset_state();
    if (linux_apply_arm_transition_ceiling(&gpu, &request, nullptr, true, &modeled) || armCalls != before) return 9589;
    gpu.nvml.setGpuLockedClocks = setCap; refuseArm = false;
    DesiredSettings outgoing{}; outgoing.hasLock = true; outgoing.lockMode = LOCK_MODE_HARD; outgoing.lockMHz = 1200;
    request.resetOcBeforeApply = true; linux_apply_ceiling_reset_state(); linux_apply_ceiling_note_outgoing(&outgoing);
    g_linuxOutgoingCeilingMHz = 1200;
    if (!linux_apply_arm_transition_ceiling(&gpu, &request, &outgoing, true, &modeled) || cap != 1200 ||
        g_linuxCeilingPlan.ceilingMHz != 1200 || g_linuxCeilingPlan.symmetricFallbackAllowed) return 9590;
    g_app = {}; g_app.lockMode = LOCK_MODE_HARD; g_app.appliedLockFreq = 1200;
    g_nvml_api.setGpuLockedClocks = setCap; before = armCalls;
    {
        ApplyClockCeilingGuard guarded(&request, false, true, &modeled);
        if (guarded.arm() != APPLY_CEILING_ARM_INSTALLED || guarded.plan.ceilingMHz != 1200 || cap != 1200 ||
            armCalls != before + 1 || guarded.plan.symmetricFallbackAllowed) return 9591;
    }
    before = armCalls;
    if (linux_apply_arm_transition_ceiling(nullptr, &request, nullptr, false, &modeled) || armCalls != before) return 9594;
    if (linux_apply_arm_transition_ceiling(&gpu, nullptr, nullptr, false, &modeled) || armCalls != before) return 9595;
    {
        ApplyClockCeilingGuard guarded(nullptr, false, false, &modeled);
        guarded.arm(); if (!guarded.must_refuse_transition() || armCalls != before) return 9596;
    }
    return 0;
}
