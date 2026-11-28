// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
// Protocol v40 adapter summaries (service_adapter_summary.h): UUID text and
// binary forms, the shared producer builder, and the envelope validator that
// keeps each entry coherent with itself and with the selected snapshot.
// Codes 14700-14729, both hosts (14727-14729: the Windows NVML matcher).

#include "gpu_core.h"
#include "nvml_adapter_uuid_policy.h"
#include <cstdio>
#include <cstring>

namespace {

ServiceResponse summary_test_response() {
    ServiceResponse r = {};
    r.state.serviceInstanceId = 7;
    r.state.stateRevision = 1;
    r.state.gpuGeneration = 1;
    r.state.gpuPhase = SERVICE_GPU_PHASE_DEGRADED;
    r.snapshot.adapterCount = 3;
    r.snapshot.selectedAdapterIndex = 1;
    for (unsigned int i = 0; i < 3; ++i) {
        r.snapshot.adapters[i].valid = true;
        r.snapshot.adapters[i].pciInfoValid = true;
        r.snapshot.adapters[i].deviceId = 0x1E07;
        r.snapshot.adapters[i].pciBus = 0x21 + i;
        r.snapshot.adapters[i].nvapiIndex = r.snapshot.adapters[i].nvmlIndex = i;
    }
    return r;
}

bool summary_test_envelope_valid(ServiceResponse& r) {
    return validate_service_state_envelope_for_ipc(&r.state, &r.snapshot, &r.desired,
                                                   &r.controlState);
}

}  // namespace

int run_service_adapter_summary_tests() {
    if (sizeof(ServiceAdapterSummary) != 40 ||
        offsetof(ServiceSnapshot, adapterSummaries) % 4 != 0 ||
        offsetof(ServiceSnapshot, adapterSummaries) + sizeof(ServiceAdapterSummary) * MAX_GPU_ADAPTERS !=
            sizeof(ServiceSnapshot)) return 14700;

    // UUID text: exactly NVML's GPU form, either case, canonical lower-case out.
    ServiceGpuUuid uuid = {};
    if (!service_gpu_uuid_parse("GPU-0123abcd-4567-89AB-cdef-0123456789ab", &uuid)) return 14701;
    if (uuid.bytes[0] != 0x01 || uuid.bytes[3] != 0xcd || uuid.bytes[6] != 0x89 ||
        uuid.bytes[15] != 0xab) return 14702;
    char text[41] = {};
    if (!service_gpu_uuid_format(uuid, text, sizeof(text)) ||
        strcmp(text, "GPU-0123abcd-4567-89ab-cdef-0123456789ab")) return 14703;
    char tooSmall[40] = {};
    if (service_gpu_uuid_format(uuid, tooSmall, sizeof(tooSmall))) return 14704;
    const char* refused[] = {
        nullptr, "", "MIG-0123abcd-4567-89ab-cdef-0123456789ab",
        "GPU-0123abcd-4567-89ab-cdef-0123456789a",    // short
        "GPU-0123abcd-4567-89ab-cdef-0123456789abc",  // long
        "GPU-0123abcd_4567-89ab-cdef-0123456789ab",   // separator
        "GPU-0123abcg-4567-89ab-cdef-0123456789ab",   // digit
        "GPU-00000000-0000-0000-0000-000000000000",   // all zero is absent
    };
    for (const char* bad : refused) {
        ServiceGpuUuid out = uuid;
        if (service_gpu_uuid_parse(bad, &out) || service_gpu_uuid_present(out)) return 14705;
    }

    // The builder: UUIDs per adapter, a shared UUID withheld from both, the
    // owner marked ACTIVE with its profile, the served card's runtime summary.
    ServiceGpuUuid uuids[3] = {};
    service_gpu_uuid_parse("GPU-11111111-1111-1111-1111-111111111111", &uuids[0]);
    service_gpu_uuid_parse("GPU-22222222-2222-2222-2222-222222222222", &uuids[1]);
    ServiceAdapterSummaryInputs in = {};
    in.uuids = uuids;
    in.count = 3;
    in.ownerIndex = 1;
    in.servedIndex = 1;
    in.fanHandbackIndex = 0;
    in.uncertain = true;
    in.fanOwned = true;
    in.gpuPhase = SERVICE_GPU_PHASE_DEGRADED;
    in.healthReason = SERVICE_GPU_HEALTH_STATE_UNCERTAIN;
    in.autoRestoreLockoutReason = SERVICE_AUTO_RESTORE_LOCKOUT_AUTOMATIC_APPLY_FAILED;
    in.activeProfileSource = SERVICE_PROFILE_SOURCE_USER_SLOT;
    in.activeProfileSlot = 3;
    ServiceResponse r = summary_test_response();
    r.snapshot.health.reason = SERVICE_GPU_HEALTH_STATE_UNCERTAIN;
    if (service_adapter_summaries_build(in, r.snapshot.adapterSummaries) != 0) return 14706;
    const ServiceAdapterSummary* s = r.snapshot.adapterSummaries;
    if (s[0].flags != (SERVICE_ADAPTER_UUID | SERVICE_ADAPTER_FAN_HANDBACK) || s[2].flags != 0 ||
        s[1].flags != (SERVICE_ADAPTER_UUID | SERVICE_ADAPTER_RUNTIME | SERVICE_ADAPTER_ACTIVE |
                       SERVICE_ADAPTER_UNCERTAIN | SERVICE_ADAPTER_FAN_OWNED) ||
        s[1].activeProfileSlot != 3 || s[1].gpuPhase != SERVICE_GPU_PHASE_DEGRADED) return 14707;
    if (!summary_test_envelope_valid(r)) return 14708;
    ServiceGpuUuid twins[3] = {uuids[0], uuids[0], uuids[1]};
    in.uuids = twins;
    ServiceAdapterSummary shared[MAX_GPU_ADAPTERS] = {};
    in.fanHandbackIndex = -1;
    if (service_adapter_summaries_build(in, shared) != 2 ||
        (shared[0].flags & SERVICE_ADAPTER_UUID) || (shared[1].flags & SERVICE_ADAPTER_UUID) ||
        !(shared[2].flags & SERVICE_ADAPTER_UUID)) return 14709;
    // Nothing known: identity alone is coherent, and a zero list is zero.
    ServiceResponse idle = summary_test_response();
    if (!summary_test_envelope_valid(idle)) return 14710;
    if (!service_adapter_summaries_are_zero(idle.snapshot.adapterSummaries) ||
        service_adapter_summaries_are_zero(r.snapshot.adapterSummaries)) return 14711;
    // A runtime on a card other than the selected one is allowed (a service
    // that runs every card needs no new wire rule), with its own phase.
    ServiceResponse other = r;
    other.snapshot.adapterSummaries[2].flags = SERVICE_ADAPTER_RUNTIME;
    other.snapshot.adapterSummaries[2].gpuPhase = SERVICE_GPU_PHASE_READY;
    if (!summary_test_envelope_valid(other)) return 14712;

    // Windows: NVML devices are matched to NvAPI adapters by PCI identity, one
    // to one, never by ordinal (14727-14729).
    GpuAdapterInfo nvapi[3] = {};
    for (unsigned int i = 0; i < 3; ++i) {
        nvapi[i].valid = nvapi[i].pciInfoValid = true;
        nvapi[i].deviceId = 0x1E07;
        nvapi[i].pciBus = 0x21 + i;
    }
    NvmlUuidCandidate nvml[3] = {};
    for (unsigned int i = 0; i < 3; ++i) {
        nvml[i].pciValid = true;
        nvml[i].deviceId = 0x1E0710DE;  // NVML packs the vendor word
        nvml[i].bus = 0x23 - i;         // enumerated in the opposite order
        nvml[i].uuid = uuids[0];
        nvml[i].uuid.bytes[15] = (gc_u8)(0x30 + i);
    }
    ServiceGpuUuid assigned[3] = {};
    if (nvml_adapter_uuid_assign(nvapi, 3, nvml, 3, assigned) != 3 ||
        assigned[0].bytes[15] != 0x32 || assigned[2].bytes[15] != 0x30) return 14727;
    // A second NVML device at an adapter's location, or a device without a
    // UUID or PCI identity, assigns nothing to that adapter.
    nvml[1].bus = 0x21;
    nvml[0].uuid = ServiceGpuUuid{};
    if (nvml_adapter_uuid_assign(nvapi, 3, nvml, 3, assigned) != 0 ||
        service_gpu_uuid_present(assigned[0]) || service_gpu_uuid_present(assigned[1])) return 14728;
    // A published entry needs the adapter in that slot to still be that card.
    GpuAdapterInfo moved = nvapi[0];
    moved.pciBus = 0x40;
    if (!nvml_adapter_uuid_identity_equal(nvapi[0], nvapi[0]) ||
        nvml_adapter_uuid_identity_equal(nvapi[0], moved) ||
        nvml_adapter_uuid_identity_equal(GpuAdapterInfo{}, GpuAdapterInfo{})) return 14729;

    // The validator, one broken rule at a time against the coherent response.
    struct Case { int code; void (*breakIt)(ServiceResponse&); };
    const Case cases[] = {
        {14713, [](ServiceResponse& x) { x.snapshot.adapterSummaries[3].flags = SERVICE_ADAPTER_RUNTIME; }},
        {14714, [](ServiceResponse& x) { x.snapshot.adapterSummaries[4].uuid.bytes[0] = 1; }},
        {14715, [](ServiceResponse& x) { x.snapshot.adapterSummaries[0].flags |= 0x80u; }},
        {14716, [](ServiceResponse& x) { x.snapshot.adapterSummaries[2].flags = SERVICE_ADAPTER_UUID; }},
        {14717, [](ServiceResponse& x) { x.snapshot.adapterSummaries[0].flags &= ~(gc_u32)SERVICE_ADAPTER_UUID; }},
        {14718, [](ServiceResponse& x) {
             x.snapshot.adapterSummaries[2].uuid = x.snapshot.adapterSummaries[0].uuid;
             x.snapshot.adapterSummaries[2].flags = SERVICE_ADAPTER_UUID; }},
        {14719, [](ServiceResponse& x) { x.snapshot.adapterSummaries[0].gpuPhase = SERVICE_GPU_PHASE_READY; }},
        {14720, [](ServiceResponse& x) { x.snapshot.adapterSummaries[0].flags |= SERVICE_ADAPTER_FAN_FAULT; }},
        {14721, [](ServiceResponse& x) { x.snapshot.adapterSummaries[1].gpuPhase = SERVICE_GPU_PHASE_DEGRADED + 1; }},
        {14722, [](ServiceResponse& x) { x.snapshot.adapterSummaries[1].autoRestoreLockoutReason = SERVICE_AUTO_RESTORE_LOCKOUT_MAX + 1; }},
        {14723, [](ServiceResponse& x) { x.snapshot.adapterSummaries[0].activeProfileSlot = 2; }},
        {14724, [](ServiceResponse& x) { x.snapshot.adapterSummaries[1].activeProfileSource = SERVICE_PROFILE_SOURCE_AD_HOC + 1; }},
        // The selected card's runtime entry must agree with its envelope and snapshot.
        {14725, [](ServiceResponse& x) { x.state.gpuPhase = SERVICE_GPU_PHASE_STARTING; }},
        {14726, [](ServiceResponse& x) { x.snapshot.health.reason = SERVICE_GPU_HEALTH_NONE; }},
    };
    for (const Case& c : cases) {
        ServiceResponse broken = r;
        c.breakIt(broken);
        if (summary_test_envelope_valid(broken)) return c.code;
    }
    return 0;
}

#ifdef GC_ADAPTER_SUMMARY_TEST_MAIN
int main() {
    const int failure = run_service_adapter_summary_tests();
    printf("adapter summary regression: %d\n", failure);
    return failure;
}
#endif
