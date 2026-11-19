// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
// The passive capture's advanced clocks (XBAR, MSVDD, system, video) carry a
// setter flag like every other scalar: set when the ClkDomains setter entry
// point resolves, clear when it does not (12265-12267). Without the flag the
// control planner called every advanced clock unavailable.
#include "app_shared.h"
#include <cstring>

namespace capture_aux_writable_fixture {
static void debug_log(const char*, ...) {}
#define XBAR_LOG debug_log
#include "gpu_backend_xbar.h"
#undef XBAR_LOG
}  // namespace capture_aux_writable_fixture
using namespace capture_aux_writable_fixture;
#include "hardware_capture_aux.h"

namespace {
static unsigned char g_live[XBAR_CONTROL_BUF_SIZE] = {};
static bool g_setterAvailable = true;
static int g_setterCalls = 0;
static int get_control(void*, void* out) {
    std::memcpy(out, g_live, sizeof(g_live));
    return 0;
}
static int set_control(void*, void*) {
    ++g_setterCalls;
    return 0;
}
static void* query(unsigned int id) {
    if (id == XBAR_NVAPI_CLK_DOMAINS_GET_CONTROL) return (void*)get_control;
    if (id == XBAR_NVAPI_CLK_DOMAINS_SET_CONTROL) return g_setterAvailable ? (void*)set_control : nullptr;
    return nullptr;
}
static void pinned_block() {
    std::memset(g_live, 0, sizeof(g_live));
    xbar_put_u32(g_live, 0, XBAR_NVAPI_CLK_DOMAINS_VERSION);
    for (unsigned int i = 0; i < XBAR_PINNED_DOMAIN_COUNT; ++i)
        xbar_put_u32(g_live, XBAR_PINNED_ENTRY_BASE + i * XBAR_PINNED_ENTRY_STRIDE, XBAR_DOMAIN_MARKER);
    xbar_put_i32(g_live, XBAR_PINNED_ENTRY_BASE + XBAR_PINNED_XBAR_ENTRY_INDEX * XBAR_PINNED_ENTRY_STRIDE +
        XBAR_FREQ_OFFSET_FIELD, 50000);
    xbar_put_i32(g_live, XBAR_PINNED_ENTRY_BASE + XBAR_PINNED_SYS_ENTRY_INDEX * XBAR_PINNED_ENTRY_STRIDE +
        XBAR_FREQ_OFFSET_FIELD, 25000);
}
static void capture(HardwareCaptureObservation& out) {
    std::memset(&out, 0, sizeof(out));
    out.requestedDomains = PROFILE_DOMAIN_XBAR | PROFILE_DOMAIN_SYS_CLK | PROFILE_DOMAIN_VIDEO_CLK;
    HardwareCaptureContext c{};
    c.query = query;
    c.gpuHandle = &g_live;
    hardware_capture_aux(c, out);
}
}  // namespace

int run_capture_aux_writable_tests() {
    static HardwareCaptureObservation o;
    pinned_block();
    g_setterAvailable = true;
    capture(o);
    if (!o.xbarClock.valid || o.xbarClock.value != 50000000 || !o.systemClock.valid || o.systemClock.value != 25000000 ||
        !o.videoClock.valid || o.videoClock.value != 0 || !o.xbarVoltage.valid)
        return 12265;
    if (o.xbarClock.writable != 1 || o.xbarVoltage.writable != 1 || o.systemClock.writable != 1 ||
        o.videoClock.writable != 1 || !hardware_capture_scalar_valid(o.xbarClock))
        return 12266;
    // Resolving the setter is the whole check: capture never calls it.
    g_setterAvailable = false;
    capture(o);
    if (!o.xbarClock.valid || o.xbarClock.writable || o.xbarVoltage.writable || o.systemClock.writable ||
        o.videoClock.writable || g_setterCalls != 0)
        return 12267;
    return 0;
}
