// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#ifndef GREEN_CURVE_HARDWARE_CAPTURE_AUX_H
#define GREEN_CURVE_HARDWARE_CAPTURE_AUX_H
// Include after the backend's gpu_backend_xbar.h logging binding.
#include "hardware_capture_getters.h"

static inline void hardware_capture_aux(const HardwareCaptureContext& c, HardwareCaptureObservation& out) {
    auto get = c.query ? (NvApiFunc)c.query(XBAR_NVAPI_CLK_DOMAINS_GET_CONTROL) : nullptr;
    auto snap = std::make_unique<XbarControlSnapshot>();
    const bool ok = get && xbar_read_control_quiet(get, c.gpuHandle, snap.get());
    const int status = ok ? 0 : !get ? -100001 : snap->rejectedCount ? snap->rejectedStatus[0] : -100002;
    const auto scalar = [](HardwareCaptureScalar& s, gc_i64 value, gc_i64 grid) {
        s.valid = 1; s.value = value; s.grid = grid;
    };
    if (ok) {
        if (out.requestedDomains & PROFILE_DOMAIN_XBAR) {
            scalar(out.xbarClock, (gc_i64)snap->freqOffsetKhz * 1000, 1000);
            scalar(out.xbarVoltage, snap->msvddOffsetUv, 1);
        }
        int value = 0;
        if ((out.requestedDomains & PROFILE_DOMAIN_SYS_CLK) &&
            xbar_read_entry_freq(snap.get(), XBAR_PINNED_SYS_ENTRY_INDEX, &value)) scalar(out.systemClock, (gc_i64)value * 1000, 1000);
        if ((out.requestedDomains & PROFILE_DOMAIN_VIDEO_CLK) &&
            xbar_read_entry_freq(snap.get(), XBAR_PINNED_VIDEO_ENTRY_INDEX, &value)) scalar(out.videoClock, (gc_i64)value * 1000, 1000);
    }
    const gc_u32 domains[] = {PROFILE_DOMAIN_XBAR, PROFILE_DOMAIN_SYS_CLK, PROFILE_DOMAIN_VIDEO_CLK};
    const bool valid[] = {out.xbarClock.valid && out.xbarVoltage.valid, out.systemClock.valid != 0, out.videoClock.valid != 0};
    for (int i = 0; i < 3; ++i) hardware_capture_domain(out, (int)domains[i], valid[i] ? CAPTURE_COMPLETE :
        get ? CAPTURE_READ_FAILED : CAPTURE_UNSUPPORTED, valid[i] ? 0 : status);
}
#endif
