// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// gpu_backend_xbar.h -- Blackwell XBAR clock and per-domain MSVDD offset
// controls.  Read-only probe, runtime read, write-with-readback, and
// restore-from-snapshot.  Included by the Windows and Linux backends.
//
// Reference: https://github.com/ilya-zlobintsev/LACT/issues/1147

#ifndef GREEN_CURVE_GPU_BACKEND_XBAR_H
#define GREEN_CURVE_GPU_BACKEND_XBAR_H

#define XBAR_RM_CLK_DOMAINS_GET_INFO    0x20809019u
#define XBAR_RM_CLK_DOMAINS_GET_CONTROL 0x2080901bu
#define XBAR_RM_CLK_DOMAINS_SET_CONTROL 0x2080d01cu
#define XBAR_RM_CLK_MEASURE_FREQ        0x20809006u

static const unsigned int XBAR_CONTROL_BUF_SIZE_MIN = 0x83c;
static const unsigned int XBAR_DOMAIN_MASK_OFFSET = 0x04;
static const unsigned int XBAR_DOMAIN_HEADER_SIZE = 0x3c;
static const unsigned int XBAR_DOMAIN_STRIDE = 0x40;
static const unsigned int XBAR_DOMAIN_INDEX = 1;
static const unsigned int XBAR_FREQ_OFFSET_MODE  = 0x08;
static const unsigned int XBAR_FREQ_OFFSET_KHZ   = 0x0c;
static const unsigned int XBAR_MSVDD_OFFSET_BASE = 0x10;
static const unsigned int XBAR_MSVDD_RAIL_INDEX  = 1;
static const unsigned int XBAR_MSVDD_RAIL_STRIDE = 4;
static const unsigned int XBAR_MEASURE_DOMAIN = 2;
static const unsigned int XBAR_BUF_MAX = 0x2000;

struct XbarControlSnapshot {
    unsigned char buf[XBAR_BUF_MAX];
    unsigned int bufSize;
    bool valid;
    int freqOffsetKhz;
    int msvddOffsetUv;
    unsigned int measuredKhz;
};

static inline unsigned int xbar_domain_base(unsigned int bufSize, bool* ok) {
    unsigned int base = XBAR_DOMAIN_HEADER_SIZE + XBAR_DOMAIN_INDEX * XBAR_DOMAIN_STRIDE;
    if (base + XBAR_FREQ_OFFSET_KHZ + 4 > bufSize) { if (ok) *ok = false; return 0; }
    if (ok) *ok = true;
    return base;
}

static inline unsigned int xbar_msvdd_offset(unsigned int domainBase) {
    return domainBase + XBAR_MSVDD_OFFSET_BASE + XBAR_MSVDD_RAIL_INDEX * XBAR_MSVDD_RAIL_STRIDE;
}

static inline int xbar_parse_freq_offset(const unsigned char* buf, unsigned int base) {
    int val = 0; memcpy(&val, buf + base + XBAR_FREQ_OFFSET_KHZ, sizeof(val)); return val;
}

static inline int xbar_parse_msvdd_offset(const unsigned char* buf, unsigned int off) {
    int val = 0; memcpy(&val, buf + off, sizeof(val)); return val;
}

// NvApiFunc is defined as int(*)(void*, void*) in app_shared.h and is the
// standard cast target for nvapi_QueryInterface results.  The RM CLK_MEASURE_FREQ
// function uses the same two-argument calling convention.
static inline bool xbar_measure_clock(NvApiFunc rmControl, void* gpuHandle,
                                       unsigned int* measuredKhz) {
    if (!rmControl || !gpuHandle || !measuredKhz) return false;
    *measuredKhz = 0;
    unsigned int params[2] = {};
    params[0] = XBAR_MEASURE_DOMAIN;
    int status = rmControl(gpuHandle, params);
    if (status != 0) return false;
    *measuredKhz = params[1];
    return true;
}

static inline bool xbar_read_control(NvApiFunc rmControl, void* gpuHandle,
                                      XbarControlSnapshot* snap) {
    if (!snap || !rmControl || !gpuHandle) return false;
    memset(snap->buf, 0, XBAR_BUF_MAX);
    unsigned int mask = 0x000000ffu;
    memcpy(snap->buf + XBAR_DOMAIN_MASK_OFFSET, &mask, sizeof(mask));
    int status = rmControl(gpuHandle, snap->buf);
    if (status != 0) return false;
    bool ok = false;
    unsigned int base = xbar_domain_base(XBAR_BUF_MAX, &ok);
    if (!ok) return false;
    snap->bufSize = XBAR_BUF_MAX;
    snap->valid = true;
    snap->freqOffsetKhz = xbar_parse_freq_offset(snap->buf, base);
    snap->msvddOffsetUv = xbar_parse_msvdd_offset(snap->buf, xbar_msvdd_offset(base));
    return true;
}

static inline bool xbar_probe(NvApiFunc rmGetCtrl, void* gpuHandle,
                               XbarControlSnapshot* snap) {
    if (!snap) return false;
    memset(snap, 0, sizeof(*snap));
    if (!rmGetCtrl || !gpuHandle) return false;
    if (!xbar_read_control(rmGetCtrl, gpuHandle, snap)) return false;
    xbar_measure_clock(rmGetCtrl, gpuHandle, &snap->measuredKhz);
    return true;
}

static inline bool xbar_read(NvApiFunc rmGetCtrl, void* gpuHandle,
                              XbarControlSnapshot* snap) {
    if (!snap || !snap->valid) return false;
    if (!rmGetCtrl || !gpuHandle) return false;
    if (!xbar_read_control(rmGetCtrl, gpuHandle, snap)) return false;
    xbar_measure_clock(rmGetCtrl, gpuHandle, &snap->measuredKhz);
    return true;
}

static inline bool xbar_write(NvApiFunc rmGetCtrl, NvApiFunc rmSetCtrl,
                               void* gpuHandle, XbarControlSnapshot* snap,
                               int freqKhz, int msvddUv) {
    if (!snap || !snap->valid || !rmGetCtrl || !rmSetCtrl || !gpuHandle) return false;
    // Read current control block as template.
    if (!xbar_read_control(rmGetCtrl, gpuHandle, snap)) return false;
    bool ok = false;
    unsigned int base = xbar_domain_base(XBAR_BUF_MAX, &ok);
    if (!ok) return false;
    unsigned int freqMode = 0;
    memcpy(snap->buf + base + XBAR_FREQ_OFFSET_MODE, &freqMode, sizeof(freqMode));
    memcpy(snap->buf + base + XBAR_FREQ_OFFSET_KHZ, &freqKhz, sizeof(freqKhz));
    unsigned int msvddOff = xbar_msvdd_offset(base);
    memcpy(snap->buf + msvddOff, &msvddUv, sizeof(msvddUv));
    int setStatus = rmSetCtrl(gpuHandle, snap->buf);
    if (setStatus != 0) return false;
    // Read back and verify.
    if (!xbar_read_control(rmGetCtrl, gpuHandle, snap)) return false;
    int readbackFreq = xbar_parse_freq_offset(snap->buf, base);
    int readbackMsvdd = xbar_parse_msvdd_offset(snap->buf, xbar_msvdd_offset(base));
    if (readbackFreq != freqKhz || readbackMsvdd != msvddUv) {
        debug_log("xbar_write: mismatch requested=(%d kHz, %d uV) readback=(%d kHz, %d uV)\n",
                  freqKhz, msvddUv, readbackFreq, readbackMsvdd);
        return false;
    }
    snap->freqOffsetKhz = readbackFreq;
    snap->msvddOffsetUv = readbackMsvdd;
    xbar_measure_clock(rmGetCtrl, gpuHandle, &snap->measuredKhz);
    return true;
}

static inline bool xbar_restore(NvApiFunc rmGetCtrl, NvApiFunc rmSetCtrl,
                                 void* gpuHandle, XbarControlSnapshot* snap) {
    if (!snap || !snap->valid || !rmGetCtrl || !rmSetCtrl || !gpuHandle) return false;
    int setStatus = rmSetCtrl(gpuHandle, snap->buf);
    if (setStatus != 0) return false;
    if (!xbar_read_control(rmGetCtrl, gpuHandle, snap)) return false;
    xbar_measure_clock(rmGetCtrl, gpuHandle, &snap->measuredKhz);
    return true;
}

#endif // GREEN_CURVE_GPU_BACKEND_XBAR_H