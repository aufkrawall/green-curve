// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// Windows client/service GPU selection agreement (gpu_selection_sync_policy.h),
// assertion codes 7090-7099. A selector choice used to be saved while the next
// snapshot put the service's GPU back on screen, and config-driven actions
// then acted on a GPU that was not shown.

#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include "gpu_adapter_identity.h"
#include "gpu_selection_policy.h"

#include "app_shared.h"
#include "gpu_selection_sync_policy.h"

namespace {

GpuAdapterInfo gpu(unsigned int index, unsigned int deviceId,
                   unsigned int bus) {
    GpuAdapterInfo adapter = {};
    adapter.valid = true;
    adapter.pciInfoValid = true;
    adapter.nvapiIndex = index;
    adapter.nvmlIndex = index;
    adapter.deviceId = deviceId;
    adapter.subSystemId = 0x89E71043u;
    adapter.pciRevisionId = 0xA1u;
    adapter.extDeviceId = deviceId >> 16;
    adapter.pciBus = bus;
    return adapter;
}

GpuSelectionAttemptKey key(gc_u64 instance, gc_u64 generation, bool intent,
                           unsigned int target) {
    GpuSelectionAttemptKey k = {};
    k.valid = true;
    k.serviceInstanceId = instance;
    k.gpuGeneration = generation;
    k.serviceHoldsIntent = intent;
    k.targetIndex = target;
    return k;
}

int accepted_tests() {
    const GpuAdapterInfo a = gpu(0, 0x2F0410DEu, 1);
    const GpuAdapterInfo twin = gpu(1, 0x2F0410DEu, 2);
    GpuAdapterInfo aMovedIndex = a;
    aMovedIndex.nvapiIndex = 1;
    // 7090: honoured only when the service shows the exact requested adapter.
    if (!gpu_selection_sync_accepted(&a, &a, 2)) return 7090;
    if (gpu_selection_sync_accepted(&a, &twin, 2)) return 7090;
    if (gpu_selection_sync_accepted(&a, &aMovedIndex, 2)) return 7090;
    if (gpu_selection_sync_accepted(&a, nullptr, 2)) return 7090;
    if (gpu_selection_sync_accepted(nullptr, &a, 2)) return 7090;
    return 0;
}

int selector_tests() {
    // 7091: choosing the shown, already saved GPU does nothing.
    if (gpu_selector_choice_verdict(0, 0, 2, true, false) !=
        GPU_SELECTOR_CHOICE_NO_CHANGE) return 7091;
    // 7092: choosing the shown GPU while the saved one differs (or is
    // unresolved) still asks, so the choice gets saved.
    if (gpu_selector_choice_verdict(0, 0, 2, false, false) !=
        GPU_SELECTOR_CHOICE_REQUEST) return 7092;
    if (gpu_selector_choice_verdict(0, 0, 2, false, true) !=
        GPU_SELECTOR_CHOICE_REQUEST) return 7092;
    // 7093: the defect. Another GPU is requested instead of saved blindly;
    // while the shown GPU holds the intent the choice is refused up front.
    if (gpu_selector_choice_verdict(1, 0, 2, true, false) !=
        GPU_SELECTOR_CHOICE_REQUEST) return 7093;
    if (gpu_selector_choice_verdict(1, 0, 2, true, true) !=
        GPU_SELECTOR_CHOICE_REFUSE_INTENT_HELD) return 7093;
    // 7094: an index outside the adapter list is invalid.
    if (gpu_selector_choice_verdict(2, 0, 2, true, false) !=
        GPU_SELECTOR_CHOICE_INVALID) return 7094;
    if (gpu_selector_choice_verdict(0, 0, MAX_GPU_ADAPTERS + 1, true, false) !=
        GPU_SELECTOR_CHOICE_INVALID) return 7094;
    return 0;
}

int reconcile_tests() {
    const GpuSelectionAttemptKey current = key(7, 3, true, 1);
    const GpuSelectionAttemptKey none = {};
    // 7095: nothing to reconcile on one adapter, an unresolved saved GPU, or
    // when saved and shown agree.
    if (gpu_selection_reconcile_decide(1, true, 0, 0, &current, &none) !=
        GPU_SELECTION_RECONCILE_NONE) return 7095;
    if (gpu_selection_reconcile_decide(2, false, 1, 0, &current, &none) !=
        GPU_SELECTION_RECONCILE_NONE) return 7095;
    if (gpu_selection_reconcile_decide(2, true, 1, 1, &current, &none) !=
        GPU_SELECTION_RECONCILE_NONE) return 7095;
    if (gpu_selection_reconcile_decide(2, true, 5, 0, &current, &none) !=
        GPU_SELECTION_RECONCILE_NONE) return 7095;
    // 7096: a saved GPU that is not shown is requested once...
    if (gpu_selection_reconcile_decide(2, true, 1, 0, &current, &none) !=
        GPU_SELECTION_RECONCILE_REQUEST) return 7096;
    // ...and held after a refusal of the same attempt, so syncs cannot loop.
    if (gpu_selection_reconcile_decide(2, true, 1, 0, &current, &current) !=
        GPU_SELECTION_RECONCILE_HELD) return 7096;
    // 7097: a changed service, GPU generation, intent or target retries.
    const GpuSelectionAttemptKey changed[] = {
        key(8, 3, true, 1), key(7, 4, true, 1), key(7, 3, false, 1),
        key(7, 3, true, 2)};
    for (const GpuSelectionAttemptKey& refused : changed) {
        if (gpu_selection_reconcile_decide(2, true, 1, 0, &current, &refused) !=
            GPU_SELECTION_RECONCILE_REQUEST) return 7097;
    }
    if (gpu_selection_attempt_key_equal(&none, &none)) return 7097;
    return 0;
}

int saved_matches_shown_tests() {
    // 7098: config-driven actions refuse a saved GPU that is not shown on a
    // multi-adapter system with authoritative state.
    if (gpu_selection_saved_matches_shown(true, 2, 1, 0)) return 7098;
    if (!gpu_selection_saved_matches_shown(true, 2, 1, 1)) return 7098;
    // 7099: single-GPU systems and non-authoritative state keep the old rule.
    if (!gpu_selection_saved_matches_shown(true, 1, 0, 0)) return 7099;
    if (!gpu_selection_saved_matches_shown(false, 2, 1, 0)) return 7099;
    // 7100: transient busy/update deferrals are distinguished from real
    // policy refusals so reconciliation does not latch them permanently.
    if (!gpu_selection_refusal_is_transient(GPU_SELECTION_BUSY_MESSAGE)) return 7100;
    if (!gpu_selection_refusal_is_transient(GPU_SELECTION_TRANSITION_MESSAGE)) return 7100;
    if (gpu_selection_refusal_is_transient("GPU 0 still holds Green Curve settings.")) return 7100;
    if (gpu_selection_refusal_is_transient(nullptr) ||
        gpu_selection_refusal_is_transient("")) return 7100;
    // 7101: GPU selector stays enabled when the service is reachable in
    // DEGRADED/DEVICE_MISSING so selecting a broken GPU never traps the user.
    if (!gpu_selector_combo_enabled(true, true, false, 2, false)) return 7101;
    if (!gpu_selector_combo_enabled(false, true, true, 2, false)) return 7101;
    if (!gpu_selector_combo_enabled(false, true, true, 1, true)) return 7101;
    if (gpu_selector_combo_enabled(false, false, true, 2, false)) return 7101;
    if (gpu_selector_combo_enabled(false, true, false, 2, false)) return 7101;
    if (gpu_selector_combo_enabled(true, true, false, 1, false)) return 7101;
    return 0;
}

#if defined(_WIN32)
AppData g_app{};
NvmlApi g_nvml_api{};
unsigned int nvmlAdapterCount;
nvmlPciInfo_t pciInfo[2]{};

void debug_log(const char*, ...) {}
const char* nvml_err_name(nvmlReturn_t) { return "scripted response"; }
void set_message(char* out, size_t size, const char* format, ...) {
    va_list args;
    va_start(args, format);
    vsnprintf(out, size, format, args);
    va_end(args);
}
#include "main_runtime_nvml_selection.h"

int nvml_binding_tests() {
    constexpr unsigned int model = 0x2F0410DEu;
    g_app = {};
    g_app.adapterCount = 2;
    g_app.selectedGpu = gpu(0, model, 0);
    g_app.adapters[0] = g_app.selectedGpu;
    g_app.adapters[1] = gpu(1, model, 2);
    nvmlAdapterCount = 2;
    pciInfo[0].pciDeviceId = pciInfo[1].pciDeviceId = model;
    pciInfo[0].bus = 1;
    pciInfo[1].bus = 2;
    g_nvml_api.getCount = [](unsigned int* count) -> nvmlReturn_t {
        *count = nvmlAdapterCount;
        return NVML_SUCCESS;
    };
    g_nvml_api.getHandleByIndex = [](unsigned int index, nvmlDevice_t* device) -> nvmlReturn_t {
        *device = reinterpret_cast<nvmlDevice_t>(&pciInfo[index]);
        return NVML_SUCCESS;
    };
    g_nvml_api.getPciInfo = [](nvmlDevice_t device, nvmlPciInfo_t* pci) -> nvmlReturn_t {
        *pci = *reinterpret_cast<const nvmlPciInfo_t*>(device);
        return NVML_SUCCESS;
    };
    char detail[192]{};
    if (nvml_select_device_for_selected_gpu(detail, sizeof(detail)) ||
        g_app.nvmlDevice || g_app.selectedGpu.pciBus || g_app.adapters[0].pciBus) return 7102;
    nvmlAdapterCount = 1;
    if (nvml_select_device_for_selected_gpu(detail, sizeof(detail)) || g_app.nvmlDevice) return 7106;
    pciInfo[0].pciDeviceId = 0x278210DEu;
    if (nvml_select_device_for_selected_gpu(detail, sizeof(detail)) || g_app.nvmlDevice) return 7103;
    // A valid location makes identical model IDs distinguishable.
    nvmlAdapterCount = 2;
    pciInfo[0].pciDeviceId = model;
    g_app.selectedGpu = g_app.adapters[1];
    g_app.selectedGpuIndex = 1;
    if (!nvml_select_device_for_selected_gpu(detail, sizeof(detail)) ||
        g_app.selectedNvmlIndex != 1 || g_app.selectedGpuOrdinalFallback) return 7104;
    // Keep the compatibility path for a single device with no PCI query.
    g_nvml_api.getPciInfo = nullptr;
    g_app.adapterCount = nvmlAdapterCount = 1;
    g_app.selectedGpu = g_app.adapters[0];
    g_app.selectedGpuIndex = 0;
    if (!nvml_select_device_for_selected_gpu(detail, sizeof(detail)) ||
        g_app.selectedNvmlIndex != 0 || g_app.selectedGpuOrdinalFallback) return 7105;
    return 0;
}
#endif

}  // namespace

int run_gpu_selection_sync_tests() {
#if defined(_WIN32)
    if (int r = nvml_binding_tests()) return r;
#endif
    if (int r = accepted_tests()) return r;
    if (int r = selector_tests()) return r;
    if (int r = reconcile_tests()) return r;
    if (int r = saved_matches_shown_tests()) return r;
    return 0;
}
