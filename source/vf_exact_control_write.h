// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#ifndef GREEN_CURVE_VF_EXACT_CONTROL_WRITE_H
#define GREEN_CURVE_VF_EXACT_CONTROL_WRITE_H
#include "gpu_core.h"
#include "vf_curve_units.h"
#include <memory>
#include <string.h>

enum VfExactWriteStatus { VF_EXACT_VERIFIED, VF_EXACT_INVALID, VF_EXACT_READ_FAILED,
    VF_EXACT_WRITE_FAILED, VF_EXACT_MISMATCH, VF_EXACT_BUDGET_EXHAUSTED };
struct VfExactWriteResult {
    VfExactWriteStatus status = VF_EXACT_INVALID;
    int points = 0, writes = 0, batchWrites = 0, pointWrites = 0, firstCi = -1;
    gc_i64 targetHz = 0, actualHz = 0;
};

// Exact control replay: no frequency samples, unit rounding, clamping or benign
// placeholder acceptance. Every write starts from a fresh full control template
// and verifies the selected raw fields. Caller owns authorization, topology,
// range planning, transition protection and rollback.
template <class IO>
static inline VfExactWriteResult vf_exact_control_write(const VfBackendSpec* backend,
    const unsigned char* membership, const bool* mask, const gc_i64* targetHz,
    int batchPasses, IO& io) {
    VfExactWriteResult r;
    if (!backend || !backend->writeSupported || !membership || !mask || !targetHz ||
        backend->controlBufferSize < 36 || backend->controlBufferSize > 0x4000 ||
        backend->controlMaskOffset > backend->controlBufferSize - 32 || backend->controlEntryStride < 4) return r;
    const unsigned int size = backend->controlBufferSize;
    const unsigned int base = backend->controlEntryBaseOffset, stride = backend->controlEntryStride;
    const unsigned int field = backend->controlEntryDeltaOffset;
    if (base > size || field > size - base || 4 > size - base - field ||
        stride > (size - base - field - 4) / 127) return r;
    int rawTargets[VF_NUM_POINTS]{};
    bool pending[VF_NUM_POINTS]{};
    for (int i = 0; i < VF_NUM_POINTS; ++i) if (mask[i]) {
        r.firstCi = i; r.targetHz = targetHz[i];
        if (!(membership[i / 8] & (1u << (i % 8))) ||
            !vf_graphics_offset_driver_units_hz(backend, targetHz[i], &rawTargets[i])) return r;
        pending[i] = true; ++r.points;
    }
    if (!r.points) { r.status = VF_EXACT_VERIFIED; return r; }
    auto buffer = std::make_unique<unsigned char[]>(size);
    const unsigned int version = (backend->controlVersion << 16) | size;
    const auto read = [&]() {
        memset(buffer.get(), 0, size); memcpy(buffer.get(), &version, 4);
        memcpy(buffer.get() + backend->controlMaskOffset, membership, 32);
        if (!io.read(buffer.get(), size)) { r.status = VF_EXACT_READ_FAILED; return false; }
        unsigned int returned = 0; memcpy(&returned, buffer.get(), 4);
        if (returned != version || memcmp(buffer.get() + backend->controlMaskOffset, membership, 32)) {
            r.status = VF_EXACT_READ_FAILED; return false;
        }
        bool first = true;
        for (int i = 0; i < VF_NUM_POINTS; ++i) if (mask[i]) {
            int raw = 0; memcpy(&raw, buffer.get() + base + (unsigned int)i * stride + field, 4);
            pending[i] = raw != rawTargets[i];
            if (pending[i] && first) {
                r.firstCi = i; r.targetHz = targetHz[i];
                if (!vf_graphics_offset_hz(backend, raw, &r.actualHz)) { r.status = VF_EXACT_READ_FAILED; return false; }
                first = false;
            }
        }
        if (first) { r.firstCi = -1; r.targetHz = r.actualHz = 0; }
        return true;
    };
    const auto write = [&](int only) {
        unsigned char selected[32]{};
        for (int i = 0; i < VF_NUM_POINTS; ++i) if (pending[i] && (only < 0 || only == i)) {
            selected[i / 8] |= (unsigned char)(1u << (i % 8));
            memcpy(buffer.get() + base + (unsigned int)i * stride + field, &rawTargets[i], 4);
        }
        memcpy(buffer.get() + backend->controlMaskOffset, selected, 32);
        ++r.writes;
        if (only < 0) ++r.batchWrites; else ++r.pointWrites;
        if (!io.write(buffer.get(), size)) { r.status = VF_EXACT_WRITE_FAILED; return false; }
        return true;
    };
    if (!read()) return r;
    if (batchPasses < 1) batchPasses = 1;
    if (batchPasses > 3) batchPasses = 3;
    for (int pass = 0; pass < batchPasses && r.firstCi >= 0; ++pass) {
        if (!io.mayWrite()) { r.status = VF_EXACT_BUDGET_EXHAUSTED; return r; }
        if (!write(-1) || !read()) return r;
    }
    // Some drivers honor one changed point per control call. The fallback is
    // bounded by the same caller deadline, with immediate exact verification.
    for (int i = 0; i < VF_NUM_POINTS; ++i) if (pending[i]) {
        if (!io.mayWrite()) { r.status = VF_EXACT_BUDGET_EXHAUSTED; return r; }
        if (!read()) return r;
        if (!pending[i]) continue;
        if (!write(i) || !read()) return r;
        if (pending[i]) { r.status = VF_EXACT_MISMATCH; return r; }
    }
    r.status = r.firstCi < 0 ? VF_EXACT_VERIFIED : VF_EXACT_MISMATCH;
    return r;
}
#endif
