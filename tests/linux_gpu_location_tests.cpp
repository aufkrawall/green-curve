// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// Linux GPU selection across PCI bus renumbering (linux_gpu_selection.h),
// assertion codes 7340-7379 and 7410-7419 (config.ini's [gpu] record).  Stored targets (config.ini, the daemon's
// committed and startup records, requests built from them) carry a PCI
// location that firmware may renumber at boot.  A known model must always
// match; the location only picks between adapters that share it, and a moved
// model is followed only where the location role allows it.  config.ini's
// selection reaches the daemon only as the live adapter it resolved to.

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

LinuxConfiguredGpu configured_at(unsigned int bus, GpuLocationRole role) {
    LinuxConfiguredGpu gpu = {};
    gpu.target = gpu_at(kModel, bus);
    gpu.role = role;
    gpu.status = LINUX_CONFIGURED_GPU_PENDING;
    return gpu;
}

int configured_gpu_tests() {
    bool moved = false;
    const GpuAdapterInfo* target = nullptr;

    // 7371: one of two identical GPUs was selected (role required) and the
    // selected card is gone.  config.ini's GPU does not resolve, and the
    // record keeps naming its old location.
    {
        GpuAdapterInfo remaining[] = {gpu_at(kModel, 6)};
        LinuxConfiguredGpu gpu = configured_at(7, GPU_LOCATION_ROLE_REQUIRED);
        if (linux_configured_gpu_resolve(&gpu, remaining, 1, &moved) != -1 ||
            gpu.status != LINUX_CONFIGURED_GPU_NOT_PRESENT ||
            gpu.target.pciBus != 7 || moved) return 7371;
        // 7372: why the client must stop the write itself.  The daemon
        // resolves a request target without the role and follows the stale
        // record to the remaining card; an untargeted write reaches that lone
        // adapter too.
        if (linux_resolve_stored_gpu_target(&gpu.target,
                GPU_LOCATION_ROLE_UNRECORDED, remaining, 1, nullptr) != 0 ||
            !linux_gpu_init_selection(nullptr, remaining, 1).writeResolved)
            return 7372;
        // 7373: so neither goes out: a write stops, a read goes untargeted.
        target = &remaining[0];
        if (linux_configured_gpu_request_target(&gpu, true, &target) ||
            target != nullptr) return 7373;
        target = &remaining[0];
        if (!linux_configured_gpu_request_target(&gpu, false, &target) ||
            target != nullptr) return 7373;
        // Every other unresolved outcome stops a write the same way.
        const LinuxConfiguredGpuStatus unresolved[] = {
            LINUX_CONFIGURED_GPU_PENDING, LINUX_CONFIGURED_GPU_UNVALIDATED,
            LINUX_CONFIGURED_GPU_AMBIGUOUS};
        for (LinuxConfiguredGpuStatus status : unresolved) {
            gpu.status = status;
            if (linux_configured_gpu_request_target(&gpu, true, &target) ||
                target != nullptr) return 7373;
        }
    }

    // 7374: a resolved selection names the live adapter, never the record.
    {
        GpuAdapterInfo live[] = {gpu_at(kOtherModel, 1), gpu_at(kModel, 6)};
        LinuxConfiguredGpu gpu = configured_at(7, GPU_LOCATION_ROLE_ADVISORY);
        if (linux_configured_gpu_resolve(&gpu, live, 2, &moved) != 1 ||
            !moved || gpu.status != LINUX_CONFIGURED_GPU_RESOLVED ||
            gpu.target.pciBus != 6) return 7374;
        if (!linux_configured_gpu_request_target(&gpu, true, &target) ||
            target != &gpu.target) return 7374;
    }

    // 7375: no record writes untargeted (the daemon's lone-adapter rule); an
    // explicit choice is sent as it is, but never without a target.
    {
        LinuxConfiguredGpu none = {};
        target = &none.target;
        if (!linux_configured_gpu_request_target(&none, true, &target) ||
            target != nullptr) return 7375;
        if (!linux_configured_gpu_request_target(nullptr, true, &target) ||
            target != nullptr) return 7375;
        LinuxConfiguredGpu chosen = configured_at(6, GPU_LOCATION_ROLE_UNRECORDED);
        chosen.status = LINUX_CONFIGURED_GPU_EXPLICIT;
        if (!linux_configured_gpu_request_target(&chosen, true, &target) ||
            target != &chosen.target) return 7375;
        chosen.target.valid = false;
        if (linux_configured_gpu_request_target(&chosen, true, &target) ||
            target != nullptr) return 7375;
    }

    // 7376: a TUI started without the daemon (or with no GPU listed) keeps
    // the record unvalidated and resolves it once an adapter list arrives.
    {
        GpuAdapterInfo live[] = {gpu_at(kModel, 7)};
        LinuxConfiguredGpu gpu = configured_at(7, GPU_LOCATION_ROLE_REQUIRED);
        if (linux_configured_gpu_resolve(&gpu, nullptr, 0, &moved) != -1 ||
            gpu.status != LINUX_CONFIGURED_GPU_UNVALIDATED) return 7376;
        if (linux_configured_gpu_resolve(&gpu, live, 0, &moved) != -1 ||
            gpu.status != LINUX_CONFIGURED_GPU_UNVALIDATED) return 7376;
        if (linux_configured_gpu_resolve(&gpu, live, 1, &moved) != 0 ||
            gpu.status != LINUX_CONFIGURED_GPU_RESOLVED || moved) return 7376;
    }

    // 7377: not present and ambiguous are final; only an explicit selection
    // replaces them, so a later adapter list does not quietly re-target.
    {
        GpuAdapterInfo live[] = {gpu_at(kModel, 7)};
        LinuxConfiguredGpu gpu = configured_at(7, GPU_LOCATION_ROLE_REQUIRED);
        gpu.status = LINUX_CONFIGURED_GPU_NOT_PRESENT;
        if (linux_configured_gpu_resolve(&gpu, live, 1, &moved) != -1 ||
            gpu.status != LINUX_CONFIGURED_GPU_NOT_PRESENT) return 7377;
        gpu.status = LINUX_CONFIGURED_GPU_EXPLICIT;
        if (linux_configured_gpu_resolve(&gpu, live, 1, &moved) != -1 ||
            gpu.status != LINUX_CONFIGURED_GPU_EXPLICIT) return 7377;
    }

    // 7378: a unique GPU joined by a twin, neither at the stored location.
    {
        GpuAdapterInfo twins[] = {gpu_at(kModel, 6), gpu_at(kModel, 8)};
        LinuxConfiguredGpu gpu = configured_at(7, GPU_LOCATION_ROLE_ADVISORY);
        if (linux_configured_gpu_resolve(&gpu, twins, 2, &moved) != -1 ||
            gpu.status != LINUX_CONFIGURED_GPU_AMBIGUOUS) return 7378;
    }

    // 7379: every status has a stable log name and a user-facing reason.
    {
        const LinuxConfiguredGpuStatus all[] = {
            LINUX_CONFIGURED_GPU_NONE, LINUX_CONFIGURED_GPU_EXPLICIT,
            LINUX_CONFIGURED_GPU_PENDING, LINUX_CONFIGURED_GPU_RESOLVED,
            LINUX_CONFIGURED_GPU_UNVALIDATED, LINUX_CONFIGURED_GPU_NOT_PRESENT,
            LINUX_CONFIGURED_GPU_AMBIGUOUS, LINUX_CONFIGURED_GPU_MALFORMED};
        for (LinuxConfiguredGpuStatus status : all) {
            const char* name = linux_configured_gpu_status_name(status);
            if (!name[0] || strcmp(name, "invalid") == 0 ||
                !strstr(linux_configured_gpu_problem(status), "config.ini"))
                return 7379;
        }
        if (!strstr(linux_configured_gpu_problem(
                        LINUX_CONFIGURED_GPU_NOT_PRESENT), "not present") ||
            !strstr(linux_configured_gpu_problem(
                        LINUX_CONFIGURED_GPU_AMBIGUOUS), "ambiguous"))
            return 7379;
    }
    return 0;
}

int selection_record_tests() {
    GpuAdapterInfo target = {};
    GpuLocationRole role = GPU_LOCATION_ROLE_ADVISORY;

    // 7410: no `selected_bdf` and no role is no record.
    if (linux_gpu_selection_record_parse("", "", &target, &role) !=
            LINUX_GPU_SELECTION_RECORD_ABSENT ||
        linux_gpu_selection_record_parse(nullptr, nullptr, &target, &role) !=
            LINUX_GPU_SELECTION_RECORD_ABSENT ||
        target.valid || role != GPU_LOCATION_ROLE_ADVISORY) return 7410;

    // 7411: a complete record parses; a missing role predates the role.
    if (linux_gpu_selection_record_parse("0000:07:00.0", "required", &target,
                                         &role) !=
            LINUX_GPU_SELECTION_RECORD_PRESENT ||
        !target.valid || target.pciBus != 7 ||
        role != GPU_LOCATION_ROLE_REQUIRED) return 7411;
    if (linux_gpu_selection_record_parse("0000:06:00.0", "", &target, &role) !=
            LINUX_GPU_SELECTION_RECORD_PRESENT ||
        target.pciBus != 6 || role != GPU_LOCATION_ROLE_UNRECORDED)
        return 7411;

    // 7412: a damaged location or role is MALFORMED, never "no record":
    // before 2026-10-24 each of these loaded as no selection, which writes
    // untargeted and reaches a lone remaining twin.
    {
        const char* cases[][2] = {
            {"0000:07:00", "required"},   // truncated location
            {"0000:07:00.0", "requird"},  // misspelled role
            {"0000:00:00.0", ""},         // all-zero is not a location
            {"", "required"},             // role without a location
            {"0000:07:20.0", "advisory"}, // device out of range
        };
        for (const auto& c : cases) {
            GpuAdapterInfo untouched = gpu_at(kModel, 9);
            GpuLocationRole kept = GPU_LOCATION_ROLE_ADVISORY;
            if (linux_gpu_selection_record_parse(c[0], c[1], &untouched,
                                                 &kept) !=
                    LINUX_GPU_SELECTION_RECORD_MALFORMED ||
                untouched.pciBus != 9 || kept != GPU_LOCATION_ROLE_ADVISORY)
                return 7412;
        }
    }

    // 7413: a malformed record starts MALFORMED, stops a write, sends no
    // target on a read, is final for the resolver, and `--gpu` replaces it.
    {
        if (linux_configured_gpu_status_for_record(
                LINUX_GPU_SELECTION_RECORD_MALFORMED, false) !=
                LINUX_CONFIGURED_GPU_MALFORMED ||
            linux_configured_gpu_status_for_record(
                LINUX_GPU_SELECTION_RECORD_PRESENT, false) !=
                LINUX_CONFIGURED_GPU_PENDING ||
            linux_configured_gpu_status_for_record(
                LINUX_GPU_SELECTION_RECORD_ABSENT, false) !=
                LINUX_CONFIGURED_GPU_NONE) return 7413;
        const LinuxGpuSelectionRecord records[] = {
            LINUX_GPU_SELECTION_RECORD_ABSENT,
            LINUX_GPU_SELECTION_RECORD_PRESENT,
            LINUX_GPU_SELECTION_RECORD_MALFORMED};
        for (LinuxGpuSelectionRecord record : records)
            if (linux_configured_gpu_status_for_record(record, true) !=
                    LINUX_CONFIGURED_GPU_EXPLICIT ||
                strcmp(linux_gpu_selection_record_name(record), "invalid") == 0)
                return 7413;
        GpuAdapterInfo remaining[] = {gpu_at(kModel, 6)};
        LinuxConfiguredGpu gpu = {};
        gpu.status = LINUX_CONFIGURED_GPU_MALFORMED;
        const GpuAdapterInfo* sent = &remaining[0];
        if (linux_configured_gpu_request_target(&gpu, true, &sent) ||
            sent != nullptr) return 7413;
        sent = &remaining[0];
        if (!linux_configured_gpu_request_target(&gpu, false, &sent) ||
            sent != nullptr) return 7413;
        bool moved = false;
        if (linux_configured_gpu_resolve(&gpu, remaining, 1, &moved) != -1 ||
            gpu.status != LINUX_CONFIGURED_GPU_MALFORMED) return 7413;
        if (!strstr(linux_configured_gpu_problem(gpu.status), "malformed"))
            return 7413;
    }

    // 7414: the shared BDF parser keeps the `--gpu` spelling and bounds.
    if (!linux_gpu_parse_bdf(" 0001:0a:1f.7 ", &target) ||
        target.pciDomain != 1 || target.pciBus != 0x0a ||
        target.pciDevice != 0x1f || target.pciFunction != 7 ||
        linux_gpu_parse_bdf("0000:100:00.0", &target) ||
        linux_gpu_parse_bdf("0000:07:00.8", &target) ||
        linux_gpu_parse_bdf(nullptr, &target)) return 7414;
    return 0;
}

}  // namespace

int run_linux_gpu_location_tests() {
    if (int r = resolver_tests()) return r;
    if (int r = init_selection_tests()) return r;
    if (int r = daemon_request_tests()) return r;
    if (int r = role_tests()) return r;
    if (int r = detail_tests()) return r;
    if (int r = configured_gpu_tests()) return r;
    if (int r = selection_record_tests()) return r;
    return 0;
}
