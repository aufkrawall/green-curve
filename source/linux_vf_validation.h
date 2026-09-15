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

// ---------------------------------------------------------------------------
// The shared VF-info rule (F-02-001)
// ---------------------------------------------------------------------------
//
// THE DEFECT THIS EXISTS FOR (audit 2026-09-15, 0.25.2):
//
//   The private-NvAPI getInfo read -- "give me the per-point editable mask and
//   the active clock count" -- was implemented TWICE, and the two copies
//   disagreed about what a valid answer is:
//
//     linux_backend.cpp (the live control path) rejected an all-zero mask or a
//     zero/over-64 clock count as LB_NVAPI_INVALID_DATA.
//     linux_gpu.cpp (the --probe diagnostic) pre-seeded the mask to 0xFF and
//     substituted defaultNumClocks for a zero count, so the same driver answer
//     silently became "128 editable points".
//
//   On a GPU where getInfo succeeds but returns zeros -- exactly the
//   unrecognized-future-family case --probe exists to qualify -- `--probe`
//   reported a readable VF curve while the TUI/daemon refused. The diagnostic
//   could not predict the control path, which is its only job.
//
//   This is the same shape as the fan-curve duplication recorded in
//   llm-wiki/testing.md, where two copies had silently diverged into a real
//   shipped defect. One rule, one place, asserted on both hosts.

enum LinuxVfInfoStatus {
    LINUX_VF_INFO_OK = 0,
    // The spec's own offsets do not fit the buffer it asks for. A compiled-in
    // table bug, not a driver answer.
    LINUX_VF_INFO_LAYOUT_INVALID,
    // The driver answered, but with a mask or clock count that cannot describe
    // a real curve.
    LINUX_VF_INFO_DATA_INVALID,
};

// Does the spec's declared layout fit the buffer it declares?
//
// Checked ONCE, up front, rather than re-checked at each memcpy: the live
// backend already did it this way, the probe copy re-checked inline and each
// check could drift on its own, and a single answer is what lets the extract
// below be an unconditional read.
static inline bool linux_vf_info_layout_fits(
    unsigned int infoBufferSize, unsigned int infoMaskOffset,
    unsigned int infoNumClocksOffset, unsigned int maskBytes,
    unsigned int infoSize) {
    if (infoSize == 0 || infoSize > 0x4000u) return false;
    if (infoBufferSize > infoSize) return false;
    if (infoMaskOffset > infoSize - maskBytes) return false;
    if (infoNumClocksOffset > infoSize - (unsigned int)sizeof(unsigned int))
        return false;
    return true;
}

// Is the answer the driver put in the buffer usable?
//
// The live backend's rule, now the only rule: a mask with no editable bit at
// all, or a clock count of zero or above 64, describes no curve this code can
// write, and treating it as "128 editable points" is what made the probe lie.
static inline bool linux_vf_info_data_usable(const unsigned char* mask,
                                             unsigned int maskBytes,
                                             unsigned int numClocks) {
    if (!mask || maskBytes == 0) return false;
    if (numClocks == 0 || numClocks > 64) return false;
    for (unsigned int index = 0; index < maskBytes; ++index) {
        if (mask[index] != 0) return true;
    }
    return false;
}

// NvAPI EnumPhysicalGPUs writes at most NVAPI_MAX_PHYSICAL_GPUS (64) handles,
// and the caller's array is sized for exactly that (F-04-001).
//
// linux_backend_discovery.cpp clamped `count > 64`; the probe copy in
// linux_gpu.cpp checked only `count < 1` and then used the count as a loop
// bound over the same fixed 64-entry stack array. The clamp existing at one of
// two identical call sites is what made it a defect rather than a style
// difference, so it is stated once here and both call sites ask this.
static inline bool linux_nvapi_enum_count_is_usable(int count, int capacity) {
    if (capacity <= 0) return false;
    return count >= 1 && count <= capacity;
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
