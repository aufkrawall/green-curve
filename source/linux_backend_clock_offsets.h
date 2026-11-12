// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
// NVML offset controls and verification, included after backend dependencies.
#ifndef GREEN_CURVE_LINUX_BACKEND_CLOCK_OFFSETS_H
#define GREEN_CURVE_LINUX_BACKEND_CLOCK_OFFSETS_H

// NVML units: physical graphics MHz, effective memory MHz (twice display MHz).
// Shared readback gate: accepts the driver's grid-snapped value (see
// nvml_clock_offset_readback_matches) and logs it, so a snap is diagnosable
// instead of surfacing as an unexplained rolled-back apply.
static bool nvml_clock_offset_verified(unsigned int domain, int requested,
                                       int readback, const char* via) {
    int step = nvml_clock_offset_grid_step(domain);
    if (!nvml_clock_offset_readback_matches(requested, readback, step)) {
        lb_log("offset: %s domain=%u requested=%d readback=%d rejected "
               "(grid step %d)\n", via, domain, requested, readback, step);
        return false;
    }
    if (readback != requested)
        lb_log("offset: %s domain=%u requested=%d snapped by driver to %d "
               "(grid step %d); accepted\n", via, domain, requested, readback,
               step);
    return true;
}

struct NvmlClockOffsetReadback {
    bool offsetValid;
    bool rangeValid;
    bool modernApi;
    int offsetMHz;
    int minMHz;
    int maxMHz;
};

// Prefer the current NVML P-state-scoped API at the configured P0 slot. The
// deprecated domain-global getters remain a compatibility fallback for older
// drivers, but are no longer required for capture/rollback availability.
static NvmlClockOffsetReadback nvml_read_clock_offset(
    LinuxGpuState* g, unsigned int domain) {
    NvmlClockOffsetReadback readback = {};
    if (!g) return readback;
    NvmlApi* a = &g->nvml;
    if (a->getClockOffsets) {
        nvmlClockOffset_t info = {};
        info.version = nvmlClockOffset_v1;
        info.type = domain;
        info.pstate = nvml_configured_clock_offset_pstate();
        if (a->getClockOffsets(g->nvmlDevice, &info) == NVML_SUCCESS &&
            info.version == nvmlClockOffset_v1 && info.type == domain &&
            info.pstate == nvml_configured_clock_offset_pstate()) {
            readback.offsetValid = true;
            readback.rangeValid = true;
            readback.modernApi = true;
            readback.offsetMHz = info.clockOffsetMHz;
            readback.minMHz = info.minClockOffsetMHz;
            readback.maxMHz = info.maxClockOffsetMHz;
            g->offsetReadPstate = (int)info.pstate;
            return readback;
        }
    }

    nvmlReturn_t rangeResult = NVML_ERROR_NOT_SUPPORTED;
    nvmlReturn_t offsetResult = NVML_ERROR_NOT_SUPPORTED;
    if (domain == NVML_CLOCK_GRAPHICS) {
        if (a->getGpcClkMinMaxVfOffset) {
            rangeResult = a->getGpcClkMinMaxVfOffset(
                g->nvmlDevice, &readback.minMHz, &readback.maxMHz);
        }
        if (a->getGpcClkVfOffset) {
            offsetResult = a->getGpcClkVfOffset(
                g->nvmlDevice, &readback.offsetMHz);
        }
    } else if (domain == NVML_CLOCK_MEM) {
        if (a->getMemClkMinMaxVfOffset) {
            rangeResult = a->getMemClkMinMaxVfOffset(
                g->nvmlDevice, &readback.minMHz, &readback.maxMHz);
        }
        if (a->getMemClkVfOffset) {
            offsetResult = a->getMemClkVfOffset(
                g->nvmlDevice, &readback.offsetMHz);
        }
    }
    readback.rangeValid = rangeResult == NVML_SUCCESS;
    readback.offsetValid = offsetResult == NVML_SUCCESS;
    return readback;
}

static bool nvml_set_clock_offset_legacy(LinuxGpuState* g, unsigned int domain, int offsetMHz) {
    NvmlApi* a = &g->nvml;
    if (a->setClockOffsets) {
        nvmlClockOffset_t info = {};
        info.version = nvmlClockOffset_v1;
        info.type = domain;
        info.pstate = nvml_configured_clock_offset_pstate();
        info.clockOffsetMHz = offsetMHz;
        nvmlReturn_t result = a->setClockOffsets(g->nvmlDevice, &info);
        // Wire units are effective MHz for MEM; also log display MHz so a bug
        // report reads the same number the TUI shows.
        int displayMHz = domain == NVML_CLOCK_MEM
            ? nvml_mem_display_mhz_from_effective_mhz(offsetMHz) : offsetMHz;
        lb_log("offset: write domain=%u pstate=P%u requested=%d display=%d "
               "via=setClockOffsets nvml=%d\n",
               domain, info.pstate, offsetMHz, displayMHz, (int)result);
        if (result == NVML_SUCCESS) {
            NvmlClockOffsetReadback verify =
                nvml_read_clock_offset(g, domain);
            if (!verify.offsetValid) {
                lb_log("offset: verify domain=%u pstate=P%u requested=%d "
                       "readback=unavailable\n",
                       domain, info.pstate, offsetMHz);
                return false;
            }
            int verifyDisplayMHz = domain == NVML_CLOCK_MEM
                ? nvml_mem_display_mhz_from_effective_mhz(verify.offsetMHz)
                : verify.offsetMHz;
            lb_log("offset: verify domain=%u pstate=P%u requested=%d "
                   "readback=%d display=%d via=%s\n",
                   domain, info.pstate, offsetMHz, verify.offsetMHz,
                   verifyDisplayMHz,
                   verify.modernApi ? "getClockOffsets" : "legacy");
            return nvml_clock_offset_verified(
                domain, offsetMHz, verify.offsetMHz, "setClockOffsets");
        }
    }
    if (domain == NVML_CLOCK_GRAPHICS && a->setGpcClkVfOffset) {
        if (a->setGpcClkVfOffset(g->nvmlDevice, offsetMHz) != NVML_SUCCESS) return false;
        NvmlClockOffsetReadback verify = nvml_read_clock_offset(g, domain);
        lb_log("offset: write domain=%u pstate=global requested=%d "
               "readback=%s%d via=setGpcClkVfOffset\n",
               domain, offsetMHz, verify.offsetValid ? "" : "unavailable:",
               verify.offsetMHz);
        return !verify.offsetValid ||
               nvml_clock_offset_verified(domain, offsetMHz, verify.offsetMHz,
                                          "setGpcClkVfOffset");
    }
    if (domain == NVML_CLOCK_MEM && a->setMemClkVfOffset) {
        if (a->setMemClkVfOffset(g->nvmlDevice, offsetMHz) != NVML_SUCCESS) return false;
        NvmlClockOffsetReadback verify = nvml_read_clock_offset(g, domain);
        lb_log("offset: write domain=%u pstate=global requested=%d display=%d "
               "readback=%s%d via=setMemClkVfOffset\n",
               domain, offsetMHz,
               nvml_mem_display_mhz_from_effective_mhz(offsetMHz),
               verify.offsetValid ? "" : "unavailable:",
               verify.offsetMHz);
        return !verify.offsetValid ||
               nvml_clock_offset_verified(domain, offsetMHz, verify.offsetMHz,
                                          "setMemClkVfOffset");
    }
    return false;
}

// Exact replay/recovery uses the same control surface that supplied the
// observation. A failed modern write cannot fall through to a second setter,
// and a different getter or a tolerated grid snap cannot prove restoration.
static bool nvml_set_clock_offset(LinuxGpuState* g, unsigned int domain, int offsetMHz,
    gc_u32 exactSurface = CLOCK_OFFSET_SURFACE_UNSPECIFIED) {
    if (!g) return false;
    if (!exactSurface) return nvml_set_clock_offset_legacy(g, domain, offsetMHz);
    if (domain != NVML_CLOCK_GRAPHICS && domain != NVML_CLOCK_MEM) return false;
    NvmlApi* a = &g->nvml;
    const bool modern = exactSurface == CLOCK_OFFSET_SURFACE_P0;
    if (!modern && exactSurface != CLOCK_OFFSET_SURFACE_GLOBAL) return false;
    auto legacySet = domain == NVML_CLOCK_MEM ? a->setMemClkVfOffset : a->setGpcClkVfOffset;
    auto legacyGet = domain == NVML_CLOCK_MEM ? a->getMemClkVfOffset : a->getGpcClkVfOffset;
    if (modern ? (!a->setClockOffsets || !a->getClockOffsets) : (!legacySet || !legacyGet)) {
        lb_log("offset: precise control unavailable domain=%u surface=%u\n", domain, (unsigned)exactSurface);
        return false;
    }
    nvmlClockOffset_t info{};
    info.version = nvmlClockOffset_v1;
    info.type = domain;
    info.pstate = nvml_configured_clock_offset_pstate();
    info.clockOffsetMHz = offsetMHz;
    const nvmlReturn_t written = modern
        ? a->setClockOffsets(g->nvmlDevice, &info) : legacySet(g->nvmlDevice, offsetMHz);
    lb_log("offset: precise write domain=%u surface=%u requested=%d nvml=%d\n",
        domain, (unsigned)exactSurface, offsetMHz, (int)written);
    if (written != NVML_SUCCESS) return false;
    int actual = 0;
    nvmlReturn_t read;
    if (modern) {
        info = {};
        info.version = nvmlClockOffset_v1;
        info.type = domain;
        info.pstate = nvml_configured_clock_offset_pstate();
        read = a->getClockOffsets(g->nvmlDevice, &info);
        actual = info.clockOffsetMHz;
        if (info.version != nvmlClockOffset_v1 || info.type != domain ||
            info.pstate != nvml_configured_clock_offset_pstate()) {
            lb_log("offset: precise verify refused control metadata domain=%u version=%u type=%u pstate=%u\n",
                domain, info.version, info.type, info.pstate);
            return false;
        }
    } else read = legacyGet(g->nvmlDevice, &actual);
    lb_log("offset: precise verify domain=%u surface=%u requested=%d readback=%d nvml=%d\n",
        domain, (unsigned)exactSurface, offsetMHz, actual, (int)read);
    return read == NVML_SUCCESS && actual == offsetMHz;
}

#endif
