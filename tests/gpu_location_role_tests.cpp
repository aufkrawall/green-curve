// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// Durable GPU selection across PCI bus renumbering (gpu_selection_policy.h and
// the [gpu] record in config_utils.cpp), assertion codes 7300-7339.  Firmware
// renumbers PCI buses when an upstream device appears or disappears, so a GPU
// whose model was unique when selected must survive a location change, while a
// location that told identical adapters apart must keep failing closed.

#include <stdio.h>
#include <string.h>

#include "app_shared.h"
#include "gpu_selection_policy.h"

namespace {

const unsigned int kRtx5070 = 0x2F0410DEu;
const unsigned int kOtherModel = 0x2C0510DEu;

GpuAdapterInfo adapter_at(unsigned int deviceId, unsigned int bus) {
    GpuAdapterInfo adapter = {};
    adapter.valid = true;
    adapter.pciInfoValid = true;
    adapter.deviceId = deviceId;
    adapter.subSystemId = 0x89E71043u;
    adapter.pciRevisionId = 0xA1u;
    adapter.extDeviceId = deviceId >> 16;
    adapter.pciBus = bus;
    return adapter;
}

ConfiguredGpuSelection selection_of(const GpuAdapterInfo& adapter,
                                    GpuLocationRole role) {
    ConfiguredGpuSelection selection = {};
    selection.stableIdentityPresent = true;
    selection.identity = adapter;
    selection.locationRole = role;
    return selection;
}

ConfiguredGpuResolveResult resolve(const ConfiguredGpuSelection& selection,
                                   const GpuAdapterInfo* adapters,
                                   unsigned int count, unsigned int* index) {
    *index = 99;
    return resolve_configured_gpu_selection(&selection, adapters, count, index);
}

int resolver_tests() {
    unsigned int index = 0;

    // 7300: the reported case.  A record written before the role existed,
    // one GPU, an on-board NIC disabled in the BIOS: bus 07 became bus 06.
    {
        GpuAdapterInfo live[] = {adapter_at(kRtx5070, 6)};
        ConfiguredGpuSelection stored = selection_of(adapter_at(kRtx5070, 7),
            GPU_LOCATION_ROLE_UNRECORDED);
        if (resolve(stored, live, 1, &index) != CONFIGURED_GPU_RESOLVE_STABLE ||
            index != 0) return 7300;
        if (!configured_gpu_selection_location_moved(&stored, &live[0]))
            return 7301;
    }

    // 7302: two different models; the unique one moved and is followed.
    {
        GpuAdapterInfo live[] = {adapter_at(kOtherModel, 1),
                                 adapter_at(kRtx5070, 6)};
        ConfiguredGpuSelection stored = selection_of(adapter_at(kRtx5070, 7),
            GPU_LOCATION_ROLE_ADVISORY);
        if (resolve(stored, live, 2, &index) != CONFIGURED_GPU_RESOLVE_STABLE ||
            index != 1) return 7302;
        // 7303: the same move without a recorded role is not trusted with
        // more than one adapter present.
        stored.locationRole = GPU_LOCATION_ROLE_UNRECORDED;
        if (resolve(stored, live, 2, &index) != CONFIGURED_GPU_RESOLVE_NOT_FOUND)
            return 7303;
    }

    // 7304: identical twins when selected.  The location IS the identity:
    // the selected twin vanished and the other one is elsewhere.
    {
        GpuAdapterInfo live[] = {adapter_at(kRtx5070, 6)};
        ConfiguredGpuSelection stored = selection_of(adapter_at(kRtx5070, 7),
            GPU_LOCATION_ROLE_REQUIRED);
        if (resolve(stored, live, 1, &index) != CONFIGURED_GPU_RESOLVE_NOT_FOUND)
            return 7304;
        // 7305: both twins present, the stored location picks one.
        GpuAdapterInfo twins[] = {adapter_at(kRtx5070, 6),
                                  adapter_at(kRtx5070, 7)};
        if (resolve(stored, twins, 2, &index) != CONFIGURED_GPU_RESOLVE_STABLE ||
            index != 1) return 7305;
        if (configured_gpu_selection_location_moved(&stored, &twins[1]))
            return 7306;
    }

    // 7307: unique when selected, an identical card added later.  The stored
    // location still holds the original: that one is chosen.
    {
        GpuAdapterInfo live[] = {adapter_at(kRtx5070, 3),
                                 adapter_at(kRtx5070, 7)};
        ConfiguredGpuSelection stored = selection_of(adapter_at(kRtx5070, 7),
            GPU_LOCATION_ROLE_ADVISORY);
        if (resolve(stored, live, 2, &index) != CONFIGURED_GPU_RESOLVE_STABLE ||
            index != 1) return 7307;
        // 7308: neither identical card sits at the stored location: which one
        // moved is unknowable, so nothing is chosen.
        live[1].pciBus = 8;
        if (resolve(stored, live, 2, &index) != CONFIGURED_GPU_RESOLVE_AMBIGUOUS)
            return 7308;
    }

    // 7309: a different model never resolves, whatever the role says.
    {
        GpuAdapterInfo live[] = {adapter_at(kOtherModel, 7)};
        const GpuLocationRole roles[] = {GPU_LOCATION_ROLE_UNRECORDED,
            GPU_LOCATION_ROLE_ADVISORY, GPU_LOCATION_ROLE_REQUIRED};
        for (GpuLocationRole role : roles) {
            ConfiguredGpuSelection stored = selection_of(
                adapter_at(kRtx5070, 7), role);
            if (resolve(stored, live, 1, &index) !=
                    CONFIGURED_GPU_RESOLVE_NOT_FOUND) return 7309;
        }
        // 7310: same device ID, different board (subsystem) is another model.
        live[0] = adapter_at(kRtx5070, 6);
        live[0].subSystemId = 0x12345678u;
        ConfiguredGpuSelection stored = selection_of(adapter_at(kRtx5070, 7),
            GPU_LOCATION_ROLE_ADVISORY);
        if (resolve(stored, live, 1, &index) != CONFIGURED_GPU_RESOLVE_NOT_FOUND)
            return 7310;
    }

    // 7311: a live adapter whose location could not be read is followed when
    // the role allows it, and is reported as moved.
    {
        GpuAdapterInfo live[] = {adapter_at(kRtx5070, 0)};
        ConfiguredGpuSelection stored = selection_of(adapter_at(kRtx5070, 7),
            GPU_LOCATION_ROLE_ADVISORY);
        if (resolve(stored, live, 1, &index) != CONFIGURED_GPU_RESOLVE_STABLE ||
            index != 0 ||
            !configured_gpu_selection_location_moved(&stored, &live[0]))
            return 7311;
    }

    // 7312: two live adapters reporting the same model at the same location
    // are never trusted, even though the location matches.
    {
        GpuAdapterInfo live[] = {adapter_at(kRtx5070, 7),
                                 adapter_at(kRtx5070, 7)};
        ConfiguredGpuSelection stored = selection_of(adapter_at(kRtx5070, 7),
            GPU_LOCATION_ROLE_ADVISORY);
        if (resolve(stored, live, 2, &index) != CONFIGURED_GPU_RESOLVE_AMBIGUOUS)
            return 7312;
    }

    // 7313: the role a selection records.
    {
        GpuAdapterInfo distinct[] = {adapter_at(kOtherModel, 1),
                                     adapter_at(kRtx5070, 7)};
        GpuAdapterInfo twins[] = {adapter_at(kRtx5070, 6),
                                  adapter_at(kRtx5070, 7)};
        GpuAdapterInfo unlocated = adapter_at(kRtx5070, 0);
        if (configured_gpu_location_role_for_selection(&distinct[1], distinct,
                2) != GPU_LOCATION_ROLE_ADVISORY ||
            configured_gpu_location_role_for_selection(&twins[1], twins, 2) !=
                GPU_LOCATION_ROLE_REQUIRED ||
            configured_gpu_location_role_for_selection(&unlocated, &unlocated,
                1) != GPU_LOCATION_ROLE_UNRECORDED) return 7313;
    }

    // 7314: which roles may move, by adapter count.
    if (!configured_gpu_location_may_move(GPU_LOCATION_ROLE_ADVISORY, 3) ||
        !configured_gpu_location_may_move(GPU_LOCATION_ROLE_UNRECORDED, 1) ||
        configured_gpu_location_may_move(GPU_LOCATION_ROLE_UNRECORDED, 2) ||
        configured_gpu_location_may_move(GPU_LOCATION_ROLE_REQUIRED, 1))
        return 7314;

    // 7315: equality covers the role, so a readback that lost it fails.
    {
        ConfiguredGpuSelection a = selection_of(adapter_at(kRtx5070, 7),
            GPU_LOCATION_ROLE_ADVISORY);
        ConfiguredGpuSelection b = a;
        if (!configured_gpu_selection_equal(&a, &b)) return 7315;
        b.locationRole = GPU_LOCATION_ROLE_UNRECORDED;
        if (configured_gpu_selection_equal(&a, &b)) return 7316;
    }
    return 0;
}

int detail_tests() {
    // 7317: the diagnostic names both locations and the role.
    GpuAdapterInfo live[6] = {};
    live[0] = adapter_at(kRtx5070, 6);
    ConfiguredGpuSelection stored = selection_of(adapter_at(kRtx5070, 7),
        GPU_LOCATION_ROLE_UNRECORDED);
    char text[256] = {};
    format_configured_gpu_resolution_detail(&stored, live, 1, text,
        sizeof(text));
    if (!strstr(text, "@0000:07:00.0") || !strstr(text, "role=unrecorded") ||
        !strstr(text, "0:model@0000:06:00.0]")) return 7317;

    // 7318: six adapters list four, then an ellipsis.
    for (unsigned int i = 1; i < 6; ++i) live[i] = adapter_at(kOtherModel, i);
    format_configured_gpu_resolution_detail(&stored, live, 6, text,
        sizeof(text));
    if (!strstr(text, "3:other@") || strstr(text, "4:other@") ||
        !strstr(text, " ...]")) return 7318;

    // 7319: a short buffer truncates but stays terminated.
    char tiny[24];
    memset(tiny, 'x', sizeof(tiny));
    format_configured_gpu_resolution_detail(&stored, live, 6, tiny,
        sizeof(tiny));
    if (memchr(tiny, 0, sizeof(tiny)) == nullptr) return 7319;
    return 0;
}

#if defined(_WIN32)
bool write_record(const char* path, int bus, const char* role) {
    DeleteFileA(path);
    return set_config_int(path, "gpu", "selected_index", 0) &&
        set_config_int(path, "gpu", "selected_identity_version", 1) &&
        set_config_string(path, "gpu", "selected_device_id", "2F0410DE") &&
        set_config_string(path, "gpu", "selected_subsystem_id", "89E71043") &&
        set_config_string(path, "gpu", "selected_revision_id", "000000A1") &&
        set_config_string(path, "gpu", "selected_ext_device_id", "00002F04") &&
        set_config_int(path, "gpu", "selected_bdf_valid", 1) &&
        set_config_int(path, "gpu", "selected_pci_domain", 0) &&
        set_config_int(path, "gpu", "selected_pci_bus", bus) &&
        set_config_int(path, "gpu", "selected_pci_device", 0) &&
        set_config_int(path, "gpu", "selected_pci_function", 0) &&
        (!role || set_config_string(path, "gpu",
            "selected_pci_location_role", role));
}

int persistence_tests(const char* path) {
    if (!path || !path[0]) return 7320;
    ConfiguredGpuSelection loaded = {};
    char err[128] = {};

    // 7321: the user's existing record (no role key) loads as unrecorded and
    // then resolves on the renumbered bus.
    if (!write_record(path, 7, nullptr) ||
        !load_configured_gpu_selection(path, &loaded, err, sizeof(err)) ||
        loaded.locationRole != GPU_LOCATION_ROLE_UNRECORDED ||
        loaded.identity.pciBus != 7) return 7321;
    GpuAdapterInfo live[] = {adapter_at(kRtx5070, 6)};
    unsigned int index = 99;
    if (resolve(loaded, live, 1, &index) != CONFIGURED_GPU_RESOLVE_STABLE ||
        index != 0) return 7322;

    // 7323: both written roles load, case-insensitively.
    if (!write_record(path, 7, "advisory") ||
        !load_configured_gpu_selection(path, &loaded, err, sizeof(err)) ||
        loaded.locationRole != GPU_LOCATION_ROLE_ADVISORY) return 7323;
    if (!write_record(path, 7, "Required") ||
        !load_configured_gpu_selection(path, &loaded, err, sizeof(err)) ||
        loaded.locationRole != GPU_LOCATION_ROLE_REQUIRED) return 7324;

    // 7325: an unknown role is malformed, never silently weakened.
    if (!write_record(path, 7, "sometimes") ||
        load_configured_gpu_selection(path, &loaded, err, sizeof(err)))
        return 7325;
    DeleteFileA(path);

    // 7326: the serializer writes the role only beside a location.
    ConfiguredGpuSelection selection = selection_of(adapter_at(kRtx5070, 6),
        GPU_LOCATION_ROLE_ADVISORY);
    char section[1024] = {};
    if (!format_configured_gpu_selection_section("gpu", &selection, section,
            sizeof(section), err, sizeof(err)) ||
        !strstr(section, "selected_pci_bus=6\r\n") ||
        !strstr(section, "selected_pci_location_role=advisory\r\n"))
        return 7326;
    selection.locationRole = GPU_LOCATION_ROLE_UNRECORDED;
    if (!format_configured_gpu_selection_section("gpu", &selection, section,
            sizeof(section), err, sizeof(err)) ||
        strstr(section, "selected_pci_location_role")) return 7327;
    selection = selection_of(adapter_at(kRtx5070, 0),
        GPU_LOCATION_ROLE_REQUIRED);
    if (!format_configured_gpu_selection_section("gpu", &selection, section,
            sizeof(section), err, sizeof(err)) ||
        strstr(section, "selected_pci_location_role")) return 7328;
    // 7329: an out-of-range role is refused rather than written.
    selection = selection_of(adapter_at(kRtx5070, 6),
        static_cast<GpuLocationRole>(7));
    if (format_configured_gpu_selection_section("gpu", &selection, section,
            sizeof(section), err, sizeof(err))) return 7329;
    return 0;
}
#endif

}  // namespace

int run_gpu_location_role_tests(const char* scratchConfigPath) {
    if (int r = resolver_tests()) return r;
    if (int r = detail_tests()) return r;
#if defined(_WIN32)
    if (int r = persistence_tests(scratchConfigPath)) return r;
#else
    (void)scratchConfigPath;
#endif
    return 0;
}
