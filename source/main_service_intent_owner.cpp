// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT

// Service binding of gpu_intent_owner_policy.h: every path that may retarget
// the single GPU runtime asks here first, under the runtime lock, before it
// touches g_app.selectedGpu. Included by main_shell.cpp ahead of the
// lifecycle and request-policy shards that call it.

static GpuAdapterInfo service_current_runtime_gpu() {
    GpuAdapterInfo current = g_app.selectedGpu;
    if (!current.valid && g_app.selectedGpuIndex < g_app.adapterCount &&
        g_app.selectedGpuIndex < MAX_GPU_ADAPTERS) {
        current = g_app.adapters[g_app.selectedGpuIndex];
    }
    return current;
}

static const char* service_intent_owner_gpu_name(const GpuAdapterInfo* gpu) {
    return gpu && gpu->valid && gpu->name[0] ? gpu->name : "NVIDIA GPU";
}

// `origin` names the caller in logs ("apply", "reset", "logon", ...).
// Returns false with a user-facing message in `err` when the request must not
// retarget the runtime. Never writes hardware.
static bool service_intent_owner_permits_target(const GpuAdapterInfo* target,
    bool retargets, bool releasesIntent, const char* origin,
    char* err, size_t errSize) {
    const GpuAdapterInfo owner = g_serviceActiveDesiredGpu;
    const GpuIntentOwnerVerdict verdict = gpu_intent_owner_verdict(
        g_serviceHasActiveDesired, &owner, target, g_app.adapters,
        g_app.adapterCount, retargets, releasesIntent);
    const char* who = origin && origin[0] ? origin : "request";
    const unsigned int targetIndex =
        target && target->valid ? target->nvapiIndex : 0u;
    switch (verdict) {
    case GPU_INTENT_OWNER_ALLOW_NO_INTENT:
    case GPU_INTENT_OWNER_ALLOW_CURRENT_TARGET:
        return true;
    case GPU_INTENT_OWNER_ALLOW_OWNER:
        if (retargets) {
            debug_log("service gpu owner: %s retargets back to the owner nvapi=%u name=%s adapters=%u\n",
                who, owner.nvapiIndex, service_intent_owner_gpu_name(&owner),
                g_app.adapterCount);
        }
        return true;
    case GPU_INTENT_OWNER_ALLOW_CURRENT_NOT_OWNER:
        // Not a retarget, so not refused here; the runtime already points
        // away from the owner. Recorded once per distinct picture.
        debug_log_on_change("service gpu owner: %s keeps current target nvapi=%u pci=%d, which is not the intent owner nvapi=%u pci=%d (adapters=%u)\n",
            who, targetIndex, target && target->pciInfoValid ? 1 : 0,
            owner.nvapiIndex, owner.pciInfoValid ? 1 : 0, g_app.adapterCount);
        return true;
    case GPU_INTENT_OWNER_ALLOW_RELEASE_ABSENT_OWNER:
        debug_log("service gpu owner: %s releases the intent of absent owner nvapi=%u name=%s; target nvapi=%u adapters=%u\n",
            who, owner.nvapiIndex, service_intent_owner_gpu_name(&owner),
            targetIndex, g_app.adapterCount);
        return true;
    case GPU_INTENT_OWNER_REFUSE_OTHER_GPU:
        set_message(err, errSize,
            "GPU %u (%s) still holds Green Curve settings. Reset that GPU before applying to or resetting GPU %u.",
            owner.nvapiIndex, service_intent_owner_gpu_name(&owner),
            targetIndex);
        break;
    case GPU_INTENT_OWNER_REFUSE_ABSENT_OWNER:
        set_message(err, errSize,
            "GPU %u (%s), which holds the active Green Curve settings, is not present. Reset to release them before applying to GPU %u.",
            owner.nvapiIndex, service_intent_owner_gpu_name(&owner),
            targetIndex);
        break;
    case GPU_INTENT_OWNER_REFUSE_UNKNOWN_OWNER:
        set_message(err, errSize,
            "Active Green Curve settings have no recorded GPU. Reset the current GPU before selecting GPU %u.",
            targetIndex);
        break;
    }
    debug_log("service gpu owner: %s refused verdict=%s owner=nvapi%u pci=%d bdf=%d target=nvapi%u pci=%d bdf=%d release=%d adapters=%u\n",
        who, gpu_intent_owner_verdict_name(verdict),
        owner.nvapiIndex, owner.pciInfoValid ? 1 : 0,
        gpu_adapter_has_valid_pci_location(&owner) ? 1 : 0,
        targetIndex, target && target->pciInfoValid ? 1 : 0,
        gpu_adapter_has_valid_pci_location(target) ? 1 : 0,
        releasesIntent ? 1 : 0, g_app.adapterCount);
    return false;
}
