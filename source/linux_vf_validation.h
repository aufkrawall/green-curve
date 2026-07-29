// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
// Pure structural validation for an atomic Linux VF snapshot candidate.

#ifndef GREEN_CURVE_LINUX_VF_VALIDATION_H
#define GREEN_CURVE_LINUX_VF_VALIDATION_H

#include "gpu_core.h"

static inline bool linux_vf_returned_version_valid(
    unsigned int returnedVersion, unsigned int expectedStructVersion,
    unsigned int expectedBufferSize) {
    return (returnedVersion & 0xFFFFu) == expectedBufferSize &&
        (returnedVersion >> 16) == expectedStructVersion;
}

// The NvAPI status table concatenates the VF points of EVERY clock domain
// (`numClocks` of them) without padding domains to a fixed stride, so a fixed
// VF_NUM_POINTS read window can run past the graphics curve into another
// domain.  Observed on an RTX 5070 (driver 610.43.03): the graphics domain is
// 127 points (180 MHz @ 450 mV .. 3157 MHz @ 1240 mV), index 127 belongs to a
// second domain (405 MHz @ 540 mV), index 128 to a third, and 129..131 are the
// ~14 GHz GDDR7 memory domain.
//
// Voltage ascends monotonically within one domain and drops at a boundary, so
// the graphics curve is the leading non-decreasing run.  Returns that length;
// callers drop everything past it so a foreign domain can be neither published
// nor written.  Unpopulated (zero-frequency) points do not end the domain --
// only a populated point whose voltage falls below the previous one does.
static inline int linux_vf_graphics_domain_length(
    const VFCurvePoint* curve, int count) {
    if (!curve || count <= 0) return 0;
    if (count > VF_NUM_POINTS) count = VF_NUM_POINTS;
    unsigned int previousVoltage = 0;
    for (int index = 0; index < count; ++index) {
        if (curve[index].freq_kHz == 0) continue;
        if (curve[index].volt_uV == 0) continue;
        if (previousVoltage != 0 && curve[index].volt_uV < previousVoltage)
            return index;
        previousVoltage = curve[index].volt_uV;
    }
    return count;
}

static inline bool linux_vf_snapshot_authoritative(
    bool infoFresh, bool statusFresh, bool controlFresh,
    bool structureValid, bool snapshotFresh) {
    return infoFresh && statusFresh && controlFresh &&
        structureValid && snapshotFresh;
}

static inline bool linux_vf_snapshot_structurally_valid(
    bool infoFresh, bool statusFresh, bool controlFresh,
    const unsigned char* editableMask, size_t editableMaskSize,
    unsigned int numClocks, const VFCurvePoint* curve,
    const int* offsets, int reportedPopulated,
    char* why, size_t whySize) {
    if (why && whySize) why[0] = 0;
    if (!infoFresh || !statusFresh || !controlFresh || !editableMask ||
        editableMaskSize < (VF_NUM_POINTS + 7u) / 8u || !curve || !offsets) {
        if (why) gc_strlcpy(why, whySize,
            "VF info/status/control snapshot is incomplete");
        return false;
    }
    if (numClocks == 0 || numClocks > 64) {
        if (why) gc_snprintf(why, whySize,
            "VF info clock count is invalid (%u)", numClocks);
        return false;
    }
    if (reportedPopulated < 8 || reportedPopulated > VF_NUM_POINTS) {
        if (why) gc_snprintf(why, whySize,
            "only %d populated VF points", reportedPopulated);
        return false;
    }

    unsigned int previousVoltage = 0;
    int actualPopulated = 0;
    int editablePopulated = 0;
    for (int index = 0; index < VF_NUM_POINTS; ++index) {
        const VFCurvePoint& point = curve[index];
        if (point.freq_kHz == 0) {
            if (point.volt_uV != 0) {
                if (why) gc_snprintf(why, whySize,
                    "point %d has voltage without frequency", index);
                return false;
            }
            continue;
        }
        ++actualPopulated;
        if (point.freq_kHz < 100000u || point.freq_kHz > 6000000u ||
            point.volt_uV < 400000u || point.volt_uV > 1600000u) {
            if (why) gc_snprintf(why, whySize,
                "point %d out of range (%u kHz @ %u uV)", index,
                point.freq_kHz, point.volt_uV);
            return false;
        }
        // Live frequency can legitimately fall after an applied VF edit. Only
        // voltage defines the topology and must remain ordered.
        if (previousVoltage && point.volt_uV < previousVoltage) {
            if (why) gc_snprintf(why, whySize,
                "point %d voltage topology is not ordered", index);
            return false;
        }
        previousVoltage = point.volt_uV;
        long long offset = offsets[index];
        if (offset < -5000000LL || offset > 5000000LL) {
            if (why) gc_snprintf(why, whySize,
                "point %d control offset is implausible (%lld kHz)",
                index, offset);
            return false;
        }
        if ((editableMask[index / 8] &
             (unsigned char)(1u << (index % 8))) != 0)
            ++editablePopulated;
    }
    if (actualPopulated != reportedPopulated) {
        if (why) gc_snprintf(why, whySize,
            "VF populated count mismatch (reported=%d actual=%d)",
            reportedPopulated, actualPopulated);
        return false;
    }
    if (editablePopulated == 0) {
        if (why) gc_strlcpy(why, whySize,
            "VF info mask has no populated editable point");
        return false;
    }
    return true;
}

#endif // GREEN_CURVE_LINUX_VF_VALIDATION_H
