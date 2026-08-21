// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// Blackwell XBAR clock and XBAR-domain MSVDD offsets through the private
// Windows NvAPI ClkDomains V2 surface.  The complete control block is read as
// a template for every transaction; a write changes only the two owned fields
// and is accepted only after an exact readback.  Layout discovery validates
// the returned version and finds the repeated domain entries before touching
// either offset.
//
// Reference: LACT issue 1147 and the R572..R610 NvAPI validation matrix.

#ifndef GREEN_CURVE_GPU_BACKEND_XBAR_H
#define GREEN_CURVE_GPU_BACKEND_XBAR_H

#include <string.h>

// These are Windows NvAPI interface IDs, not Linux RM command IDs.  In
// particular, PropRels controls the GPC->XBAR propagation ratio and uses a
// different structure; it must never be mistaken for ClkDomains.
#define XBAR_NVAPI_CLK_DOMAINS_GET_CONTROL 0xF58938F5u
#define XBAR_NVAPI_CLK_DOMAINS_SET_CONTROL 0xD14B69CFu
#define XBAR_NVAPI_CLK_DOMAINS_VERSION     0x000261A4u
#define XBAR_NVAPI_CLK_MEASURE             0x527FC458u
#define XBAR_NVAPI_CLK_MEASURE_VERSION     0x0001000Cu

static const unsigned int XBAR_CONTROL_BUF_SIZE = 0x13000;
static const unsigned int XBAR_CONTROL_MASK_OFFSET = 8;
static const unsigned int XBAR_CONTROL_DOMAIN_MASK = 0xff;
static const unsigned int XBAR_DOMAIN_MARKER = 0x0f;
static const unsigned int XBAR_DOMAIN_DEFAULT_INDEX = 1;
static const unsigned int XBAR_FREQ_OFFSET_FIELD = 0x114;
static const unsigned int XBAR_MSVDD_OFFSET_FIELD = 0x11c;
static const unsigned int XBAR_MEASURE_DOMAIN_XBAR = 2;
static const unsigned int XBAR_MEASURE_VALUE_OFFSET = 8;

struct XbarControlSnapshot {
    unsigned char buf[XBAR_CONTROL_BUF_SIZE];
    bool valid;
    unsigned int entryBase;
    unsigned int entryStride;
    unsigned int domainIndex;
    int freqOffsetKhz;
    int msvddOffsetUv;
    unsigned int measuredKhz;
};

struct XbarBufferLayout {
    unsigned int entryBase;
    unsigned int entryStride;
    unsigned int domainIndex;
};

static inline unsigned int xbar_get_u32(const unsigned char* buf,
                                        unsigned int offset) {
    unsigned int value = 0;
    if (buf) memcpy(&value, buf + offset, sizeof(value));
    return value;
}

static inline void xbar_put_u32(unsigned char* buf, unsigned int offset,
                                unsigned int value) {
    if (buf) memcpy(buf + offset, &value, sizeof(value));
}

static inline void xbar_put_i32(unsigned char* buf, unsigned int offset,
                                int value) {
    xbar_put_u32(buf, offset, (unsigned int)value);
}

// Find repeated domain records without assuming one driver branch's base or
// stride.  The marker is the first field observed in every ClkDomains entry.
// Restricting strides to 0x40..0x1000 rejects unrelated repeated words while
// covering all validated layouts.
static inline bool xbar_discover_entry_layout(const unsigned char* buf,
                                              unsigned int bufSize,
                                              XbarBufferLayout* layout) {
    if (!buf || !layout || bufSize < 0x200) return false;
    unsigned int bestCount = 0;
    XbarBufferLayout best{};
    for (unsigned int first = 0x100; first + 4 <= bufSize; first += 4) {
        if (xbar_get_u32(buf, first) != XBAR_DOMAIN_MARKER) continue;
        for (unsigned int next = first + 0x40; next + 4 <= bufSize; next += 4) {
            if (xbar_get_u32(buf, next) != XBAR_DOMAIN_MARKER) continue;
            unsigned int stride = next - first;
            if (stride < 0x40 || stride > 0x1000) continue;
            unsigned int count = 0;
            for (unsigned int off = first; off + 4 <= bufSize; off += stride) {
                if (xbar_get_u32(buf, off) == XBAR_DOMAIN_MARKER) ++count;
            }
            bool better = count > bestCount ||
                (count == bestCount && count > 0 &&
                 (first < best.entryBase ||
                  (first == best.entryBase && stride < best.entryStride)));
            if (better) {
                bestCount = count;
                best.entryBase = first;
                best.entryStride = stride;
            }
        }
    }
    if (bestCount < 2) return false;
    *layout = best;
    return true;
}

// A non-zero offset identifies XBAR after an out-of-band change.  At stock all
// entries are zero, so fall back to the public NvAPI clock-domain enum value.
static inline unsigned int xbar_select_domain_index(
    const unsigned char* buf, const XbarBufferLayout& layout) {
    unsigned int candidate = XBAR_DOMAIN_DEFAULT_INDEX;
    unsigned int candidates = 0;
    for (unsigned int index = 0; index < 32; ++index) {
        unsigned long long base =
            (unsigned long long)layout.entryBase +
            (unsigned long long)index * layout.entryStride;
        if (base + XBAR_MSVDD_OFFSET_FIELD + 4 > XBAR_CONTROL_BUF_SIZE) break;
        if (xbar_get_u32(buf, (unsigned int)base + XBAR_FREQ_OFFSET_FIELD) != 0 ||
            xbar_get_u32(buf, (unsigned int)base + XBAR_MSVDD_OFFSET_FIELD) != 0) {
            candidate = index;
            ++candidates;
        }
    }
    return candidates == 1 ? candidate : XBAR_DOMAIN_DEFAULT_INDEX;
}

static inline bool xbar_layout_for_buffer(const unsigned char* buf,
                                          XbarBufferLayout* layout) {
    if (!buf || !layout) return false;
    if (!xbar_discover_entry_layout(buf, XBAR_CONTROL_BUF_SIZE, layout))
        return false;
    layout->domainIndex = xbar_select_domain_index(buf, *layout);
    unsigned long long fieldEnd =
        (unsigned long long)layout->entryBase +
        (unsigned long long)layout->domainIndex * layout->entryStride +
        XBAR_MSVDD_OFFSET_FIELD + 4;
    return fieldEnd <= XBAR_CONTROL_BUF_SIZE;
}

static inline unsigned int xbar_domain_field_base(
    const XbarBufferLayout& layout) {
    return layout.entryBase + layout.domainIndex * layout.entryStride;
}

static inline unsigned int xbar_domain_field_base(
    const XbarControlSnapshot& snap) {
    XbarBufferLayout layout{};
    layout.entryBase = snap.entryBase;
    layout.entryStride = snap.entryStride;
    layout.domainIndex = snap.domainIndex;
    return xbar_domain_field_base(layout);
}

// NvApiFunc is the project-wide two-argument private-NvAPI entry signature.
static inline bool xbar_measure_clock(NvApiFunc measureFunc, void* gpuHandle,
                                      unsigned int* measuredKhz) {
    if (!measureFunc || !gpuHandle || !measuredKhz) return false;
    *measuredKhz = 0;
    unsigned int params[3] = {};
    params[0] = XBAR_NVAPI_CLK_MEASURE_VERSION;
    params[1] = XBAR_MEASURE_DOMAIN_XBAR;
    if (measureFunc(gpuHandle, params) != 0) return false;
    *measuredKhz = params[2];
    return *measuredKhz != 0;
}

static inline bool xbar_read_control(NvApiFunc getControl, void* gpuHandle,
                                     XbarControlSnapshot* snap) {
    if (!getControl || !gpuHandle || !snap) return false;
    memset(snap->buf, 0, sizeof(snap->buf));
    xbar_put_u32(snap->buf, 0, XBAR_NVAPI_CLK_DOMAINS_VERSION);
    xbar_put_u32(snap->buf, XBAR_CONTROL_MASK_OFFSET,
                 XBAR_CONTROL_DOMAIN_MASK);
    int getStatus = getControl(gpuHandle, snap->buf);
    if (getStatus != 0) {
        debug_log("xbar_read: GET_CONTROL failed status=0x%X\n",
                  (unsigned)getStatus);
        return false;
    }
    if (xbar_get_u32(snap->buf, 0) != XBAR_NVAPI_CLK_DOMAINS_VERSION ||
        xbar_get_u32(snap->buf, XBAR_CONTROL_MASK_OFFSET) !=
            XBAR_CONTROL_DOMAIN_MASK) {
        debug_log("xbar_read: unsupported response version=0x%08X mask=0x%08X\n",
                  xbar_get_u32(snap->buf, 0),
                  xbar_get_u32(snap->buf, XBAR_CONTROL_MASK_OFFSET));
        return false;
    }
    XbarBufferLayout layout{};
    if (!xbar_layout_for_buffer(snap->buf, &layout)) {
        debug_log("xbar_read: no plausible repeated domain layout\n");
        return false;
    }
    snap->entryBase = layout.entryBase;
    snap->entryStride = layout.entryStride;
    snap->domainIndex = layout.domainIndex;
    unsigned int base = xbar_domain_field_base(layout);
    snap->freqOffsetKhz = (int)xbar_get_u32(
        snap->buf, base + XBAR_FREQ_OFFSET_FIELD);
    snap->msvddOffsetUv = (int)xbar_get_u32(
        snap->buf, base + XBAR_MSVDD_OFFSET_FIELD);
    snap->valid = true;
    return true;
}

static inline bool xbar_probe(NvApiFunc getControl, NvApiFunc measureFunc,
                              void* gpuHandle, XbarControlSnapshot* snap) {
    if (!snap) return false;
    memset(snap, 0, sizeof(*snap));
    if (!xbar_read_control(getControl, gpuHandle, snap)) return false;
    xbar_measure_clock(measureFunc, gpuHandle, &snap->measuredKhz);
    return true;
}

static inline bool xbar_write(NvApiFunc getControl, NvApiFunc setControl,
                              NvApiFunc measureFunc, void* gpuHandle,
                              XbarControlSnapshot* snap,
                              int freqKhz, int msvddUv,
                              bool writeFreq, bool writeMsvdd) {
    if (!snap || !getControl || !setControl || !gpuHandle) return false;
    if (!writeFreq && !writeMsvdd) return false;
    // Always use a fresh response as the full-block preimage.  A stale probe
    // snapshot could overwrite an unrelated clock-domain field changed since
    // startup.
    if (!xbar_read_control(getControl, gpuHandle, snap)) return false;
    unsigned int base = xbar_domain_field_base(*snap);
    if (writeFreq)
        xbar_put_i32(snap->buf, base + XBAR_FREQ_OFFSET_FIELD, freqKhz);
    if (writeMsvdd)
        xbar_put_i32(snap->buf, base + XBAR_MSVDD_OFFSET_FIELD, msvddUv);
    int setStatus = setControl(gpuHandle, snap->buf);
    if (setStatus != 0) {
        debug_log("xbar_write: SET_CONTROL failed status=0x%X\n",
                  (unsigned)setStatus);
        return false;
    }
    if (!xbar_read_control(getControl, gpuHandle, snap)) {
        debug_log("xbar_write: post-set GET_CONTROL failed\n");
        return false;
    }
    base = xbar_domain_field_base(*snap);
    if (writeFreq && snap->freqOffsetKhz != freqKhz) {
        debug_log("xbar_write: frequency mismatch requested=%d readback=%d\n",
                  freqKhz, snap->freqOffsetKhz);
        return false;
    }
    if (writeMsvdd && snap->msvddOffsetUv != msvddUv) {
        debug_log("xbar_write: MSVDD mismatch requested=%d readback=%d\n",
                  msvddUv, snap->msvddOffsetUv);
        return false;
    }
    xbar_measure_clock(measureFunc, gpuHandle, &snap->measuredKhz);
    return true;
}

// Reset means stock for both owned fields.  Reading immediately before the
// SET preserves every other field in the current hardware block.
static inline bool xbar_reset_to_stock(NvApiFunc getControl,
                                       NvApiFunc setControl,
                                       NvApiFunc measureFunc,
                                       void* gpuHandle,
                                       XbarControlSnapshot* snap) {
    return xbar_write(getControl, setControl, measureFunc, gpuHandle, snap,
                      0, 0, true, true);
}

#endif
