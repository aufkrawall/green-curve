// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// Windows single-owner GPU retargeting (gpu_intent_owner_policy.h) and the
// PCI identity predicates it rests on (gpu_adapter_identity.h), assertion
// codes 7060-7079. The service holds one active intent bound to one GPU; a
// retarget away from that GPU used to orphan its clocks and manual fan duty
// and lend its remaining fields to the new GPU's intent.

#include <stdio.h>
#include <string.h>

#include "app_shared.h"
#include "gpu_intent_owner_policy.h"

namespace {

const unsigned int kModelA = 0x2F0410DEu;
const unsigned int kModelB = 0x2C0510DEu;

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

GpuIntentOwnerVerdict verdict(bool hasIntent, const GpuAdapterInfo& owner,
    const GpuAdapterInfo& target, const GpuAdapterInfo* adapters,
    unsigned int count, bool retargets, bool releases) {
    return gpu_intent_owner_verdict(hasIntent, &owner, &target, adapters,
        count, retargets, releases);
}

int identity_tests() {
    const GpuAdapterInfo a = gpu(0, kModelA, 1);
    GpuAdapterInfo aNoBdf = a;
    aNoBdf.pciBus = 0;
    const GpuAdapterInfo twin = gpu(1, kModelA, 2);
    const GpuAdapterInfo b = gpu(1, kModelB, 2);
    // 7060: BDF strengthens identity only when both sides carry it.
    if (!gpu_adapter_has_same_pci_identity(&a, &aNoBdf)) return 7060;
    if (gpu_adapter_has_same_pci_identity(&a, &twin)) return 7060;
    if (gpu_adapter_has_same_pci_identity(&a, &b)) return 7060;
    if (gpu_adapter_has_valid_pci_location(&aNoBdf)) return 7060;
    // 7061: base identity is unique only without a twin in the list.
    const GpuAdapterInfo pair[] = {a, b};
    const GpuAdapterInfo twins[] = {a, twin};
    if (!gpu_adapter_pci_base_identity_is_unique(&a, pair, 2)) return 7061;
    if (gpu_adapter_pci_base_identity_is_unique(&a, twins, 2)) return 7061;
    // 7062: a weak ordinal write target is accepted only on one adapter.
    GpuAdapterInfo weak0 = a;
    weak0.pciInfoValid = false;
    if (!gpu_adapter_is_same_write_target(&weak0, &weak0, 1)) return 7062;
    if (gpu_adapter_is_same_write_target(&weak0, &weak0, 2)) return 7062;
    GpuAdapterInfo invalid = a;
    invalid.valid = false;
    if (gpu_adapter_is_same_write_target(&a, &invalid, 1)) return 7062;
    if (gpu_adapter_is_same_write_target(nullptr, &a, 1)) return 7062;
    return 0;
}

int retarget_tests() {
    const GpuAdapterInfo a = gpu(0, kModelA, 1);
    const GpuAdapterInfo b = gpu(1, kModelB, 2);
    const GpuAdapterInfo twin = gpu(1, kModelA, 2);
    const GpuAdapterInfo pair[] = {a, b};
    const GpuAdapterInfo twins[] = {a, twin};
    const GpuAdapterInfo onlyB[] = {b};

    // 7063: without an active intent any validated target may be selected.
    if (verdict(false, a, b, pair, 2, true, false) !=
        GPU_INTENT_OWNER_ALLOW_NO_INTENT) return 7063;
    // 7064: the defect. Apply or Reset aimed at GPU 1 while GPU 0 owns the
    // intent and is present is refused instead of retargeting.
    if (verdict(true, a, b, pair, 2, true, false) !=
        GPU_INTENT_OWNER_REFUSE_OTHER_GPU) return 7064;
    if (verdict(true, a, b, pair, 2, true, true) !=
        GPU_INTENT_OWNER_REFUSE_OTHER_GPU) return 7064;
    // 7065: identical cards are told apart by location.
    if (verdict(true, a, twin, twins, 2, true, false) !=
        GPU_INTENT_OWNER_REFUSE_OTHER_GPU) return 7065;
    GpuAdapterInfo unlocated = a;
    unlocated.pciDomain = unlocated.pciBus = unlocated.pciDevice = unlocated.pciFunction = 0;
    if (verdict(true, unlocated, twin, twins, 2, true, false) != GPU_INTENT_OWNER_REFUSE_OTHER_GPU ||
        verdict(true, a, unlocated, twins, 2, true, true) != GPU_INTENT_OWNER_REFUSE_OTHER_GPU ||
        verdict(true, unlocated, a, pair, 2, true, false) != GPU_INTENT_OWNER_ALLOW_OWNER)
        return 12705;
    // 7066: retargeting back to the owner is allowed.
    if (verdict(true, a, a, pair, 2, true, false) !=
        GPU_INTENT_OWNER_ALLOW_OWNER) return 7066;
    // 7067: an owner that left the adapter list: only a Reset releases it;
    // an Apply would inherit its fields.
    if (verdict(true, a, b, onlyB, 1, true, false) !=
        GPU_INTENT_OWNER_REFUSE_ABSENT_OWNER) return 7067;
    if (verdict(true, a, b, onlyB, 1, true, true) !=
        GPU_INTENT_OWNER_ALLOW_RELEASE_ABSENT_OWNER) return 7067;
    // 7068: an adapter that cannot be compared (no PCI identity on a
    // multi-adapter list) may be the owner, so the owner stays present.
    GpuAdapterInfo opaque = gpu(0, 0, 0);
    opaque.pciInfoValid = false;
    const GpuAdapterInfo undecidable[] = {opaque, b};
    if (verdict(true, a, b, undecidable, 2, true, true) !=
        GPU_INTENT_OWNER_REFUSE_OTHER_GPU) return 7068;
    if (verdict(true, a, b, nullptr, 2, true, true) !=
        GPU_INTENT_OWNER_REFUSE_OTHER_GPU) return 7068;
    // 7069: an intent without a recorded owner may not be retargeted at all;
    // a Reset of the current GPU (not a retarget) still releases it.
    GpuAdapterInfo unknown = {};
    if (verdict(true, unknown, b, pair, 2, true, false) !=
        GPU_INTENT_OWNER_REFUSE_UNKNOWN_OWNER) return 7069;
    if (verdict(true, unknown, b, pair, 2, true, true) !=
        GPU_INTENT_OWNER_REFUSE_UNKNOWN_OWNER) return 7069;
    if (verdict(true, unknown, b, pair, 2, false, true) !=
        GPU_INTENT_OWNER_ALLOW_CURRENT_TARGET) return 7069;
    // 7070: requests that keep the current target are never refused; a
    // current target that is not the owner is reported, not refused.
    if (verdict(true, a, a, pair, 2, false, false) !=
        GPU_INTENT_OWNER_ALLOW_CURRENT_TARGET) return 7070;
    if (verdict(true, a, b, pair, 2, false, false) !=
        GPU_INTENT_OWNER_ALLOW_CURRENT_NOT_OWNER) return 7070;
    // 7071: single-GPU systems are unaffected, with or without PCI identity.
    GpuAdapterInfo weak = a;
    weak.pciInfoValid = false;
    const GpuAdapterInfo one[] = {weak};
    if (verdict(true, weak, weak, one, 1, true, false) !=
        GPU_INTENT_OWNER_ALLOW_OWNER) return 7071;
    const GpuAdapterInfo oneStrong[] = {a};
    if (verdict(true, a, a, oneStrong, 1, true, true) !=
        GPU_INTENT_OWNER_ALLOW_OWNER) return 7071;
    return 0;
}

int verdict_table_tests() {
    // 7072: every verdict has a name, and exactly the ALLOW_* ones allow.
    for (int v = GPU_INTENT_OWNER_ALLOW_NO_INTENT;
         v <= GPU_INTENT_OWNER_REFUSE_UNKNOWN_OWNER; ++v) {
        const GpuIntentOwnerVerdict verdictValue =
            static_cast<GpuIntentOwnerVerdict>(v);
        const char* name = gpu_intent_owner_verdict_name(verdictValue);
        if (!name || !strcmp(name, "invalid")) return 7072;
        const bool refuses = !strncmp(name, "refuse-", 7);
        if (gpu_intent_owner_verdict_allows(verdictValue) == refuses)
            return 7072;
    }
    if (strcmp(gpu_intent_owner_verdict_name(
            static_cast<GpuIntentOwnerVerdict>(99)), "invalid")) return 7072;
    if (gpu_intent_owner_verdict_allows(
            static_cast<GpuIntentOwnerVerdict>(99))) return 7072;
    return 0;
}

}  // namespace

int run_gpu_intent_owner_tests() {
    if (int r = identity_tests()) return r;
    if (int r = retarget_tests()) return r;
    if (int r = verdict_table_tests()) return r;
    return 0;
}
