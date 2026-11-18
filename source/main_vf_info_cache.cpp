// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// The cached VF-info point mask, split out of main_runtime_nvml.cpp (at its
// size ratchet).  Included there, at the definition's original position in
// the amalgamated translation unit.
//
// The cache holds the driver's FULL advertised mask: VF control and the
// write-path editability checks use it as is, while every VF status request is
// bounded to the 128-point table (vf_point_mask_policy.h).  A failed or missing
// VF-info call leaves the 0..127 default in place; the one-line log below says
// which of the two the session is running on.

#include "vf_point_mask_policy.h"

static bool nvapi_get_vf_info_cached(unsigned char* maskOut) {
    const VfBackendSpec* backend = g_app.vfBackend;
    if (!backend) return false;

    if (!g_app.vfInfoCached) {
        memset(g_app.vfMask, 0, sizeof(g_app.vfMask));
        memset(g_app.vfMask, 0xFF, 16);

        auto getInfo = (NvApiFunc)nvapi_qi(backend->getInfoId);
        // Sentinel for "entry point missing", distinct from any NvAPI status.
        int infoRet = -999998;
        if (getInfo) {
            size_t infoSize = backend->infoBufferSize > 0 ? backend->infoBufferSize : 0x4000;
            if (infoSize > 0x4000) infoSize = 0x4000;
            HeapBuffer ibuf(infoSize);
            if (!ibuf) return false;
            memset(ibuf, 0, infoSize);
            if (backend->infoBufferSize > (unsigned int)infoSize) return false;
            const unsigned int version = (backend->infoVersion << 16) | (unsigned int)infoSize;
            memcpy(&ibuf[0], &version, sizeof(version));
            if (backend->infoMaskOffset + sizeof(g_app.vfMask) <= (unsigned int)infoSize) {
                memset(&ibuf[backend->infoMaskOffset], 0xFF, sizeof(g_app.vfMask));
            }
            infoRet = getInfo(g_app.gpuHandle, ibuf);
            if (infoRet == 0) {
                if (backend->infoMaskOffset + sizeof(g_app.vfMask) <= (unsigned int)infoSize) {
                    memcpy(g_app.vfMask, &ibuf[backend->infoMaskOffset], sizeof(g_app.vfMask));
                }
            }
        }
        // Once per cache fill: whether the mask is the driver's or the 0..127
        // default, and how far it reaches past the point table.
        const unsigned int maskBytes = (unsigned int)sizeof(g_app.vfMask);
        debug_log("vf_info: GetInfo(0x%08X) ret=%d source=%s advertised=%u highest=%d "
                  "pastTable=%u (status requests stop at point %d)\n",
            backend->getInfoId, infoRet,
            infoRet == 0 ? "driver" : (infoRet == -999998 ? "default (entry point missing)"
                                                            : "default (GetInfo refused)"),
            vf_mask_point_count(g_app.vfMask, maskBytes),
            vf_mask_highest_point(g_app.vfMask, maskBytes),
            vf_mask_points_past_table(g_app.vfMask, maskBytes, VF_NUM_POINTS),
            VF_NUM_POINTS - 1);
        g_app.vfInfoCached = true;
    }

    if (maskOut) memcpy(maskOut, g_app.vfMask, sizeof(g_app.vfMask));
    return true;
}

