// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
// Atomic Linux VF reads, also executed with a fake driver in tests.
// Included after the backend types, NvAPI call type and logging helpers.

#ifndef GREEN_CURVE_LINUX_BACKEND_VF_READ_H
#define GREEN_CURVE_LINUX_BACKEND_VF_READ_H

static const int LB_NVAPI_FUNCTION_MISSING = -100001;
static const int LB_NVAPI_INVALID_DATA = -100002;
static const int LB_NVAPI_NOT_ATTEMPTED = -100003;

struct LinuxVfReadCandidate {
    VFCurvePoint curve[VF_NUM_POINTS];
    int offsets[VF_NUM_POINTS];
    int populated;
};

// Read current per-point active mask. A cached result is
// never promoted to fresh: every published VF snapshot proves all three driver
// calls (info, status and control) in the same refresh transaction.
static bool nvapi_get_vf_info(LinuxGpuState* g) {
    const VfBackendSpec* b = g->backend;
    g->vfInfoFresh = false;
    if (!b || !g->gpuHandle || !g->nvapiQi) {
        g->vfInfoStatus = LB_NVAPI_FUNCTION_MISSING;
        return false;
    }
    auto getInfo = (nvapi_buf_t)g->nvapiQi(b->getInfoId);
    if (!getInfo) {
        g->vfInfoStatus = LB_NVAPI_FUNCTION_MISSING;
        return false;
    }
    unsigned int infoSize = b->infoBufferSize ? b->infoBufferSize : 0x4000;
    // F-02-001: the shared layout rule, so the --probe path cannot bounds-check
    // this differently than the control path does.
    if (!linux_vf_info_layout_fits(b->infoBufferSize, b->infoMaskOffset,
            (unsigned int)sizeof(g->vfMask), infoSize)) {
        g->vfInfoStatus = LB_NVAPI_INVALID_DATA;
        return false;
    }
    unsigned char* ibuf = (unsigned char*)calloc(1, infoSize);
    if (!ibuf) {
        g->vfInfoStatus = -1;
        return false;
    }
    unsigned int ver = (b->infoVersion << 16) | infoSize;
    memcpy(ibuf, &ver, sizeof(ver));
    memset(ibuf + b->infoMaskOffset, 0xFF, sizeof(g->vfMask));
    int status = getInfo(g->gpuHandle, ibuf);
    g->vfInfoStatus = status;
    if (!nvapi_ok(status)) {
        free(ibuf);
        return false;
    }
    unsigned int returnedVersion = 0;
    memcpy(&returnedVersion, ibuf, sizeof(returnedVersion));
    if (!linux_vf_returned_version_valid(returnedVersion, b->infoVersion,
                                         infoSize)) {
        g->vfInfoStatus = LB_NVAPI_INVALID_DATA;
        free(ibuf);
        return false;
    }
    unsigned char mask[sizeof(g->vfMask)] = {};
    memcpy(mask, ibuf + b->infoMaskOffset, sizeof(mask));
    free(ibuf);
    linux_vf_layout_note_mask(&g->vfLayout, mask, (unsigned int)sizeof(mask));
    if (!linux_vf_info_data_usable(mask, (unsigned int)sizeof(mask))) {
        g->vfInfoStatus = LB_NVAPI_INVALID_DATA;
        return false;
    }
    if (!g->vfInfoCached || memcmp(g->vfMask, mask, sizeof(mask)) != 0) {
        unsigned int graphicsCandidates = 0;
        for (int i = 0; i < VF_NUM_POINTS; ++i)
            if (mask[i / 8] & (1u << (i % 8))) ++graphicsCandidates;
        lb_log("vf_info: validated point mask (graphicsCandidates=%u); "
               "clock counts are not part of VF-info V1\n", graphicsCandidates);
    }
    memcpy(g->vfMask, mask, sizeof(mask));
    g->vfInfoCached = true;
    g->vfInfoFresh = true;
    return true;
}

static bool nvapi_read_curve_candidate(LinuxGpuState* g,
                                       LinuxVfReadCandidate* candidate) {
    const VfBackendSpec* b = g->backend;
    g->vfStatusFresh = false;
    if (!b || !b->readSupported || !candidate || !g->gpuHandle) {
        g->vfStatusStatus = LB_NVAPI_FUNCTION_MISSING;
        return false;
    }
    if (!g->vfInfoFresh) {
        g->vfStatusStatus = LB_NVAPI_INVALID_DATA;
        return false;
    }
    auto getStatus = (nvapi_buf_t)g->nvapiQi(b->getStatusId);
    if (!getStatus || b->statusBufferSize == 0 || b->statusBufferSize > 0x4000 ||
        b->statusMaskOffset + sizeof(g->vfMask) > b->statusBufferSize ||
        b->statusEntriesOffset + (VF_NUM_POINTS - 1u) *
            b->statusEntryStride + 8u > b->statusBufferSize) {
        g->vfStatusStatus = LB_NVAPI_FUNCTION_MISSING;
        return false;
    }
    unsigned char* buf = (unsigned char*)calloc(1, b->statusBufferSize);
    if (!buf) {
        g->vfStatusStatus = -1;
        return false;
    }
    unsigned int ver = (b->statusVersion << 16) | b->statusBufferSize;
    memcpy(buf, &ver, sizeof(ver));
    memcpy(buf + b->statusMaskOffset, g->vfMask, sizeof(g->vfMask));
    int status = getStatus(g->gpuHandle, buf);
    g->vfStatusStatus = status;
    bool ok = nvapi_ok(status);
    if (ok) {
        unsigned int returnedVersion = 0;
        unsigned char returnedMask[sizeof(g->vfMask)] = {};
        memcpy(&returnedVersion, buf, sizeof(returnedVersion));
        if (!linux_vf_returned_version_valid(returnedVersion,
                b->statusVersion, b->statusBufferSize)) {
            g->vfStatusStatus = LB_NVAPI_INVALID_DATA;
            ok = false;
        }
        memcpy(returnedMask, buf + b->statusMaskOffset,
               sizeof(returnedMask));
        if (ok && memcmp(returnedMask, g->vfMask, sizeof(returnedMask)) != 0) {
            g->vfStatusStatus = LB_NVAPI_INVALID_DATA;
            ok = false;
        }
    }
    if (ok) {
        // The status table concatenates the VF points of every clock domain,
        // including the graphics curve, and the domains
        // are not padded to a fixed stride. Observed on an RTX 5070 (driver
        // 610.43.03): the graphics domain is 127 points (180 MHz @ 450 mV ..
        // 3157 MHz @ 1240 mV), index 127 already belongs to another domain
        // (405 MHz @ 540 mV), index 128 to a third, and 129..131 are the
        // ~14 GHz GDDR7 memory domain.
        //
        // Reading a fixed VF_NUM_POINTS window therefore drags foreign points
        // into the curve, where they break the ordered-voltage invariant and
        // failed the ENTIRE snapshot closed — no VF read and no writes at all
        // on that GPU. Voltage ascends monotonically within a domain and drops
        // at a domain boundary, so the graphics curve is the leading
        // non-decreasing run. Everything past it is zeroed: unpopulated points
        // are skipped by validation and by apply_curve_offsets_verified(), so
        // a foreign domain can be neither published nor written.
        for (int i = 0; i < VF_NUM_POINTS; i++) {
            unsigned int freq = 0, volt = 0;
            unsigned int off = b->statusEntriesOffset + (unsigned int)i * b->statusEntryStride;
            if (off + 8 <= b->statusBufferSize) {
                memcpy(&freq, buf + off, 4);
                memcpy(&volt, buf + off + 4, 4);
            }
            candidate->curve[i].freq_kHz = freq;
            candidate->curve[i].volt_uV = volt;
        }
        int domainEnd = vf_normalize_graphics_status(b, candidate->curve,
                                                    VF_NUM_POINTS);
        linux_vf_layout_note_status(&g->vfLayout, candidate->curve,
                                    VF_NUM_POINTS, g->vfMask,
                                    (unsigned int)sizeof(g->vfMask));
        candidate->populated = 0;
        for (int i = 0; i < VF_NUM_POINTS; i++) {
            if (i >= domainEnd) {
                candidate->curve[i].freq_kHz = 0;
                candidate->curve[i].volt_uV = 0;
            } else if (candidate->curve[i].freq_kHz > 0) {
                candidate->populated++;
            }
        }
        if (!g->graphicsDomainBoundaryLogged ||
            g->lastLoggedGraphicsDomainEnd != domainEnd) {
            if (domainEnd != VF_NUM_POINTS) {
                lb_log("curve: graphics domain ends at point %d "
                       "(%d MHz @ %u mV); omitted %d trailing read-window "
                       "slots beyond the graphics domain\n",
                       domainEnd - 1,
                       domainEnd > 0
                           ? (int)(candidate->curve[domainEnd - 1].freq_kHz /
                                   1000u) : 0,
                       domainEnd > 0
                           ? candidate->curve[domainEnd - 1].volt_uV / 1000u
                           : 0u,
                       VF_NUM_POINTS - domainEnd);
            } else if (g->graphicsDomainBoundaryLogged) {
                lb_log("curve: graphics domain now spans the full %d-point "
                       "read window\n", VF_NUM_POINTS);
            }
            g->lastLoggedGraphicsDomainEnd = domainEnd;
            g->graphicsDomainBoundaryLogged = true;
        }
        g->vfStatusFresh = true;
    }
    free(buf);
    return ok;
}

// ports nvapi_read_control_table()
static bool nvapi_read_control_table(LinuxGpuState* g, unsigned char* buf, unsigned int bufSize) {
    const VfBackendSpec* b = g->backend;
    g->vfControlFresh = false;
    if (!b || !g->gpuHandle || !buf || bufSize < b->controlBufferSize ||
        b->controlBufferSize == 0 || b->controlBufferSize > 0x4000 ||
        b->controlMaskOffset + sizeof(g->vfMask) > b->controlBufferSize ||
        b->controlEntryBaseOffset + (VF_NUM_POINTS - 1u) *
            b->controlEntryStride + b->controlEntryDeltaOffset +
            sizeof(int) > b->controlBufferSize) {
        g->vfControlStatus = LB_NVAPI_FUNCTION_MISSING;
        return false;
    }
    auto getFunc = (nvapi_buf_t)g->nvapiQi(b->getControlId);
    if (!getFunc) {
        g->vfControlStatus = LB_NVAPI_FUNCTION_MISSING;
        return false;
    }
    if (!g->vfInfoFresh) {
        g->vfControlStatus = LB_NVAPI_INVALID_DATA;
        return false;
    }
    memset(buf, 0, b->controlBufferSize);
    unsigned int ver = (b->controlVersion << 16) | b->controlBufferSize;
    memcpy(buf, &ver, sizeof(ver));
    memcpy(buf + b->controlMaskOffset, g->vfMask, sizeof(g->vfMask));
    int status = getFunc(g->gpuHandle, buf);
    g->vfControlStatus = status;
    g->vfControlFresh = nvapi_ok(status);
    if (g->vfControlFresh) {
        unsigned int returnedVersion = 0;
        unsigned char returnedMask[sizeof(g->vfMask)] = {};
        memcpy(&returnedVersion, buf, sizeof(returnedVersion));
        if (!linux_vf_returned_version_valid(returnedVersion,
                b->controlVersion, b->controlBufferSize)) {
            g->vfControlStatus = LB_NVAPI_INVALID_DATA;
            g->vfControlFresh = false;
        }
        memcpy(returnedMask, buf + b->controlMaskOffset,
               sizeof(returnedMask));
        if (g->vfControlFresh &&
            memcmp(returnedMask, g->vfMask, sizeof(returnedMask)) != 0) {
            g->vfControlStatus = LB_NVAPI_INVALID_DATA;
            g->vfControlFresh = false;
        }
    }
    return g->vfControlFresh;
}

static bool nvapi_read_offsets_candidate(LinuxGpuState* g,
                                         LinuxVfReadCandidate* candidate) {
    const VfBackendSpec* b = g->backend;
    if (!b || !candidate || !b->readSupported ||
        b->controlBufferSize == 0 || b->controlBufferSize > 0x4000) {
        g->vfControlStatus = LB_NVAPI_FUNCTION_MISSING;
        g->vfControlFresh = false;
        return false;
    }
    unsigned char* buf = (unsigned char*)calloc(1, b->controlBufferSize ? b->controlBufferSize : 0x4000);
    if (!buf) return false;
    bool ok = nvapi_read_control_table(g, buf, b->controlBufferSize);
    if (ok) {
        for (int i = 0; i < VF_NUM_POINTS; i++) {
            int delta = 0;
            unsigned int off = b->controlEntryBaseOffset + (unsigned int)i * b->controlEntryStride + b->controlEntryDeltaOffset;
            if (off + sizeof(delta) <= b->controlBufferSize) memcpy(&delta, buf + off, sizeof(delta));
            else delta = 0;
            candidate->offsets[i] = vf_graphics_offset_khz(b, delta);
        }
    }
    free(buf);
    return ok;
}

static bool linux_vf_candidate_structurally_valid(
    const LinuxGpuState* g, const LinuxVfReadCandidate* candidate,
    char* why, size_t whySize) {
    return g && candidate && linux_vf_snapshot_structurally_valid(
        g->vfInfoFresh, g->vfStatusFresh, g->vfControlFresh,
        g->vfMask, sizeof(g->vfMask),
        candidate->curve, candidate->offsets, candidate->populated,
        why, whySize);
}

static bool nvapi_read_vf_snapshot(LinuxGpuState* g, char* why,
                                   size_t whySize) {
    if (why && whySize) why[0] = 0;
    g->vfInfoFresh = false;
    g->vfStatusFresh = false;
    g->vfControlFresh = false;
    g->vfSnapshotFresh = false;
    g->vfStructureValid = false;
    g->vfInfoStatus = LB_NVAPI_FUNCTION_MISSING;
    g->vfStatusStatus = LB_NVAPI_NOT_ATTEMPTED;
    g->vfControlStatus = LB_NVAPI_NOT_ATTEMPTED;
    linux_vf_layout_reset(&g->vfLayout);
    LinuxVfReadCandidate candidate = {};
    if (!nvapi_get_vf_info(g)) {
        if (why) gc_snprintf(why, whySize, "getInfo status=%d (%s)",
            g->vfInfoStatus, nvapi_status_name(g->vfInfoStatus));
        return false;
    }
    if (!nvapi_read_curve_candidate(g, &candidate)) {
        if (why) gc_snprintf(why, whySize, "getStatus status=%d (%s)",
            g->vfStatusStatus, nvapi_status_name(g->vfStatusStatus));
        return false;
    }
    if (!nvapi_read_offsets_candidate(g, &candidate)) {
        if (why) gc_snprintf(why, whySize, "getControl status=%d (%s)",
            g->vfControlStatus, nvapi_status_name(g->vfControlStatus));
        return false;
    }
    if (!linux_vf_candidate_structurally_valid(g, &candidate, why, whySize))
        return false;
    memcpy(g->curve, candidate.curve, sizeof(g->curve));
    memcpy(g->freqOffsets, candidate.offsets, sizeof(g->freqOffsets));
    g->numPopulated = candidate.populated;
    g->vfStructureValid = true;
    g->vfSnapshotFresh = true;
    return true;
}

#endif // GREEN_CURVE_LINUX_BACKEND_VF_READ_H
