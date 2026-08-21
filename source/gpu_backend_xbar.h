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
// The only layout validated against the R572..R610 ClkDomains V2 response.
// A future driver may move it, but guessing a new private layout is unsafe:
// SET_CONTROL receives the complete block.  An unknown layout therefore leaves
// the domain unavailable instead of selecting the "most plausible" words.
static const unsigned int XBAR_PINNED_ENTRY_BASE = 0x124;
static const unsigned int XBAR_PINNED_ENTRY_STRIDE = 0x304;
static const unsigned int XBAR_PINNED_DOMAIN_COUNT = 8;
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

// Accept only the exact validated repeated-entry schema.  This is deliberately
// not a "find the most plausible pattern" scanner: hostile or malformed driver
// bytes can otherwise select a decoy stride, and SET_CONTROL sends the complete
// buffer back to privileged hardware state.
static inline bool xbar_layout_for_buffer(const unsigned char* buf,
                                          XbarBufferLayout* layout) {
    if (!buf || !layout) return false;
    for (unsigned int index = 0; index < XBAR_PINNED_DOMAIN_COUNT; ++index) {
        unsigned long long markerOffset =
            (unsigned long long)XBAR_PINNED_ENTRY_BASE +
            (unsigned long long)index * XBAR_PINNED_ENTRY_STRIDE;
        if (markerOffset + sizeof(unsigned int) > XBAR_CONTROL_BUF_SIZE ||
            xbar_get_u32(buf, (unsigned int)markerOffset) !=
                XBAR_DOMAIN_MARKER) {
            return false;
        }
    }
    layout->entryBase = XBAR_PINNED_ENTRY_BASE;
    layout->entryStride = XBAR_PINNED_ENTRY_STRIDE;
    layout->domainIndex = XBAR_DOMAIN_DEFAULT_INDEX;
    unsigned long long fieldEnd =
        (unsigned long long)layout->entryBase +
        (unsigned long long)layout->domainIndex * layout->entryStride +
        XBAR_MSVDD_OFFSET_FIELD + sizeof(unsigned int);
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
        debug_log("xbar_read: pinned ClkDomains V2 schema did not match\n");
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
