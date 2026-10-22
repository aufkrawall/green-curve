// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// Linux GPU selection across PCI bus renumbering (linux_gpu_selection.h),
// assertion codes 7340-7379.  Stored targets (config.ini, the daemon's
// committed and startup records, requests built from them) carry a PCI
// location that firmware may renumber at boot.  A known model must always
// match; the location only picks between adapters that share it, and a moved
// model is followed only where the location role allows it.

#include <stdio.h>
#include <string.h>

#include "gpu_core.h"
#include "linux_gpu_selection.h"

namespace {

const unsigned int kModel = 0x2F0410DEu;       // NVML: device << 16 | vendor
const unsigned int kModelSwapped = 0x10DE2F04u; // NvAPI word order
const unsigned int kOtherModel = 0x2C0510DEu;
const unsigned int kSubsystem = 0x89E71043u;

GpuAdapterInfo gpu_at(unsigned int deviceId, unsigned int bus) {
    GpuAdapterInfo gpu = {};
    gpu.valid = true;
    gpu.pciInfoValid = deviceId != 0;
    gpu.deviceId = deviceId;
    gpu.subSystemId = deviceId ? kSubsystem : 0;
    gpu.pciBus = bus;
    return gpu;
}

int resolve(const GpuAdapterInfo& stored, GpuLocationRole role,
            const GpuAdapterInfo* live, unsigned int count, bool* moved) {
    return linux_resolve_stored_gpu_target(&stored, role, live, count, moved);
}

int resolver_tests() {
    bool moved = false;

    // 7340: one GPU, firmware renumbered bus 07 to bus 06, record without a
    // role (every daemon record and older config.ini).  The removed exact
    // resolver returned -1 here.
    {
        GpuAdapterInfo live[] = {gpu_at(kModel, 6)};
        if (resolve(gpu_at(kModel, 7), GPU_LOCATION_ROLE_UNRECORDED, live, 1,
                    &moved) != 0 || !moved) return 7340;
        // 7341: an unmoved target is not reported as moved.
        if (resolve(gpu_at(kModel, 6), GPU_LOCATION_ROLE_UNRECORDED, live, 1,
                    &moved) != 0 || moved) return 7341;
    }

    // 7342: a different model in the only slot is never the stored GPU, at
    // its old location or anywhere else, whatever the role.
    {
        GpuAdapterInfo live[] = {gpu_at(kOtherModel, 7)};
        const GpuLocationRole roles[] = {GPU_LOCATION_ROLE_UNRECORDED,
            GPU_LOCATION_ROLE_ADVISORY, GPU_LOCATION_ROLE_REQUIRED};
        for (GpuLocationRole role : roles)
            if (resolve(gpu_at(kModel, 7), role, live, 1, &moved) != -1)
                return 7342;
        // 7343: same device, different board (subsystem) is another model.
        live[0] = gpu_at(kModel, 6);
        live[0].subSystemId = 0x12345678u;
        if (resolve(gpu_at(kModel, 7), GPU_LOCATION_ROLE_ADVISORY, live, 1,
                    &moved) != -1) return 7343;
    }

    // 7344: two different models; the unique one moved.  Only a recorded
    // advisory role may follow it with more than one adapter present.
    {
        GpuAdapterInfo live[] = {gpu_at(kOtherModel, 1), gpu_at(kModel, 6)};
        if (resolve(gpu_at(kModel, 7), GPU_LOCATION_ROLE_UNRECORDED, live, 2,
                    &moved) != -1) return 7344;
        if (resolve(gpu_at(kModel, 7), GPU_LOCATION_ROLE_ADVISORY, live, 2,
                    &moved) != 1 || !moved) return 7345;
    }

    // 7346: identical twins when selected: the location IS the identity.
    {
        GpuAdapterInfo one[] = {gpu_at(kModel, 6)};
        if (resolve(gpu_at(kModel, 7), GPU_LOCATION_ROLE_REQUIRED, one, 1,
                    &moved) != -1) return 7346;
        GpuAdapterInfo twins[] = {gpu_at(kModel, 6), gpu_at(kModel, 7)};
        if (resolve(gpu_at(kModel, 7), GPU_LOCATION_ROLE_REQUIRED, twins, 2,
                    &moved) != 1 || moved) return 7347;
        // 7348: unique when selected, a twin added later and neither at the
        // stored location: which one moved is unknowable.
        twins[1].pciBus = 8;
        if (resolve(gpu_at(kModel, 7), GPU_LOCATION_ROLE_ADVISORY, twins, 2,
                    &moved) != -2) return 7348;
    }

    // 7349: a bare location (no model) is exact and never moves.
    {
        GpuAdapterInfo live[] = {gpu_at(kModel, 6)};
        if (resolve(gpu_at(0, 7), GPU_LOCATION_ROLE_ADVISORY, live, 1,
                    &moved) != -1) return 7349;
        if (resolve(gpu_at(0, 6), GPU_LOCATION_ROLE_REQUIRED, live, 1,
                    &moved) != 0) return 7350;
    }

    // 7351: a model without a location resolves only when unique.
    {
        GpuAdapterInfo one[] = {gpu_at(kModel, 6)};
        GpuAdapterInfo twins[] = {gpu_at(kModel, 6), gpu_at(kModel, 7)};
        if (resolve(gpu_at(kModel, 0), GPU_LOCATION_ROLE_UNRECORDED, one, 1,
                    &moved) != 0 || moved) return 7351;
        if (resolve(gpu_at(kModel, 0), GPU_LOCATION_ROLE_UNRECORDED, twins, 2,
                    &moved) != -2) return 7352;
    }

    // 7353: two live adapters claiming the stored location are never trusted.
    {
        GpuAdapterInfo live[] = {gpu_at(kModel, 7), gpu_at(kModel, 7)};
        if (resolve(gpu_at(kModel, 7), GPU_LOCATION_ROLE_ADVISORY, live, 2,
                    &moved) != -2) return 7353;
    }

    // 7354: the NvAPI word order of the device ID is the same model.
    {
        GpuAdapterInfo live[] = {gpu_at(kModel, 6)};
        if (resolve(gpu_at(kModelSwapped, 7), GPU_LOCATION_ROLE_UNRECORDED,
                    live, 1, &moved) != 0) return 7354;
    }

    // 7355: a live adapter whose model could not be read is not a match for a
    // stored model, even at the stored location.
    {
        GpuAdapterInfo live[] = {gpu_at(0, 7)};
        if (resolve(gpu_at(kModel, 7), GPU_LOCATION_ROLE_UNRECORDED, live, 1,
                    &moved) != -1) return 7355;
    }
    return 0;
}

int init_selection_tests() {
    // 7356: backend init with a moved stored target on one GPU selects it for
    // writes; the daemon's boot restore and resume restore depend on this.
    GpuAdapterInfo live[] = {gpu_at(kModel, 6)};
    GpuAdapterInfo stored = gpu_at(kModel, 7);
    LinuxGpuInitSelection selection = linux_gpu_init_selection(&stored, live, 1);
    if (selection.index != 0 || !selection.writeResolved || !selection.moved)
        return 7356;
    // 7357: a different model on a one-GPU system no longer receives the
    // stored intent (the previous rule resolved writes for any lone adapter).
    GpuAdapterInfo other[] = {gpu_at(kOtherModel, 7)};
    selection = linux_gpu_init_selection(&stored, other, 1);
    if (selection.index >= 0 || selection.writeResolved) return 7357;
    // 7358: untargeted: a lone adapter is implicit, several are not.
    selection = linux_gpu_init_selection(nullptr, live, 1);
    if (selection.index != 0 || !selection.writeResolved) return 7358;
    GpuAdapterInfo two[] = {gpu_at(kModel, 6), gpu_at(kOtherModel, 7)};
    selection = linux_gpu_init_selection(nullptr, two, 2);
    if (selection.index != -1 || selection.writeResolved) return 7359;
    // 7360: a moved target among several adapters is not followed by init,
    // which never knows the role; the client resolves config.ini first.
    selection = linux_gpu_init_selection(&stored, two, 2);
    if (selection.index != -1 || selection.writeResolved) return 7360;
    return 0;
}

int daemon_request_tests() {
    // 7361: the daemon resolves a stale request target before the active-
    // intent check, so the active GPU is not mistaken for "another GPU".
    GpuAdapterInfo live[] = {gpu_at(kModel, 6)};
    GpuAdapterInfo active = live[0];
    GpuAdapterInfo staleRequest = gpu_at(kModel, 7);
    if (linux_gpu_switch_preserves_active_intent(true, &active, &staleRequest))
        return 7361; // the raw comparison alone refuses it
    int index = linux_resolve_stored_gpu_target(&staleRequest,
        GPU_LOCATION_ROLE_UNRECORDED, live, 1, nullptr);
    if (index != 0 ||
        !linux_gpu_switch_preserves_active_intent(true, &active, &live[index]))
        return 7362;
    return 0;
}

int role_tests() {
    // 7363: the role a selection records.
    GpuAdapterInfo distinct[] = {gpu_at(kOtherModel, 1), gpu_at(kModel, 6)};
    GpuAdapterInfo twins[] = {gpu_at(kModel, 6), gpu_at(kModel, 7)};
    GpuAdapterInfo unlocated = gpu_at(kModel, 0);
    if (linux_gpu_location_role_for_selection(&distinct[1], distinct, 2) !=
            GPU_LOCATION_ROLE_ADVISORY ||
        linux_gpu_location_role_for_selection(&twins[0], twins, 2) !=
            GPU_LOCATION_ROLE_REQUIRED ||
        linux_gpu_location_role_for_selection(&unlocated, &unlocated, 1) !=
            GPU_LOCATION_ROLE_UNRECORDED) return 7363;

    // 7364: the persisted spelling (shared with the Windows [gpu] reader).
    GpuLocationRole role = GPU_LOCATION_ROLE_REQUIRED;
    if (!configured_gpu_location_role_parse("", &role) ||
        role != GPU_LOCATION_ROLE_UNRECORDED) return 7364;
    if (!configured_gpu_location_role_parse("advisory", &role) ||
        role != GPU_LOCATION_ROLE_ADVISORY) return 7365;
    if (!configured_gpu_location_role_parse("Required", &role) ||
        role != GPU_LOCATION_ROLE_REQUIRED) return 7366;
    const char* malformed[] = {"sometimes", "advis", "advisoryx",
                               "unrecorded", " advisory"};
    for (const char* text : malformed)
        if (configured_gpu_location_role_parse(text, &role)) return 7367;
    return 0;
}

int detail_tests() {
    // 7368: the diagnostic names both locations, the role and the model tag.
    GpuAdapterInfo live[6] = {};
    live[0] = gpu_at(kModel, 6);
    GpuAdapterInfo stored = gpu_at(kModel, 7);
    char text[256] = {};
    linux_format_stored_gpu_detail(&stored, GPU_LOCATION_ROLE_UNRECORDED, live,
                                   1, text, sizeof(text));
    if (!strstr(text, "@0000:07:00.0") || !strstr(text, "role=unrecorded") ||
        !strstr(text, "0:model@0000:06:00.0]")) return 7368;
    // 7369: six adapters list four, then an ellipsis.
    for (unsigned int i = 1; i < 6; ++i) live[i] = gpu_at(kOtherModel, i);
    linux_format_stored_gpu_detail(&stored, GPU_LOCATION_ROLE_ADVISORY, live,
                                   6, text, sizeof(text));
    if (!strstr(text, "3:other@") || strstr(text, "4:other@") ||
        !strstr(text, " ...]")) return 7369;
    // 7370: a short buffer truncates but stays terminated.
    char tiny[20];
    memset(tiny, 'x', sizeof(tiny));
    linux_format_stored_gpu_detail(&stored, GPU_LOCATION_ROLE_ADVISORY, live,
                                   6, tiny, sizeof(tiny));
    if (!memchr(tiny, 0, sizeof(tiny))) return 7370;
    return 0;
}

}  // namespace

int run_linux_gpu_location_tests() {
    if (int r = resolver_tests()) return r;
    if (int r = init_selection_tests()) return r;
    if (int r = daemon_request_tests()) return r;
    if (int r = role_tests()) return r;
    if (int r = detail_tests()) return r;
    return 0;
}
