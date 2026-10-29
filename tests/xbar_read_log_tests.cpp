// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
// Execute the ClkDomains read logging with a fake driver: one-shot reads log
// every refusal, a polled reader logs a repeated refusal once.
#include "app_shared.h"
#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace xbar_read_log_fixture {
static char g_log[4096] = {};
static int g_lines = 0;
static void capture_log(const char* fmt, ...) {
    const size_t used = std::strlen(g_log);
    va_list ap;
    va_start(ap, fmt);
    if (used < sizeof(g_log)) std::vsnprintf(g_log + used, sizeof(g_log) - used, fmt, ap);
    va_end(ap);
    for (const char* p = fmt; *p; ++p) if (*p == '\n') ++g_lines;
}
#define XBAR_LOG capture_log
#include "gpu_backend_xbar.h"
#undef XBAR_LOG

static unsigned char g_response[XBAR_CONTROL_BUF_SIZE] = {};
static int g_status = 0;
static int get_control(void*, void* out) {
    if (g_status == 0) std::memcpy(out, g_response, sizeof(g_response));
    return g_status;
}
static void clear_log() {
    g_log[0] = '\0';
    g_lines = 0;
}
// The reported Pascal answer: the V2 request is accepted and echoed, but the
// pinned per-domain markers are absent.
static void answer_pascal_layout() {
    std::memset(g_response, 0, sizeof(g_response));
    xbar_put_u32(g_response, 0, XBAR_NVAPI_CLK_DOMAINS_VERSION);
    g_status = 0;
}
static void answer_pinned_layout() {
    answer_pascal_layout();
    for (unsigned int i = 0; i < XBAR_PINNED_DOMAIN_COUNT; ++i)
        xbar_put_u32(g_response, XBAR_PINNED_ENTRY_BASE + i * XBAR_PINNED_ENTRY_STRIDE,
                     XBAR_DOMAIN_MARKER);
}
static bool polled(gc_log_steady::Tracker* tracker, XbarControlSnapshot* snap,
                   unsigned long long nowMs) {
    void* gpu = &g_status;
    const bool ok = xbar_read_control_quiet((NvApiFunc)get_control, gpu, snap);
    xbar_log_polled_read_outcome(tracker, snap, ok, nowMs, false);
    return ok;
}
static const char* kMismatch =
    "xbar_read: pinned schema for version word 0x000261A4 did not match the response\n";
}  // namespace xbar_read_log_fixture

int run_xbar_read_log_tests() {
    using namespace xbar_read_log_fixture;
    static XbarControlSnapshot snap;
    void* gpu = &g_status;
    const unsigned long long interval = gc_log_steady::kSummaryIntervalMs;

    // One-shot readers keep logging every refusal, with the refusal recorded.
    answer_pascal_layout();
    clear_log();
    std::memset(&snap, 0, sizeof(snap));
    if (xbar_read_control((NvApiFunc)get_control, gpu, &snap) ||
        xbar_read_control((NvApiFunc)get_control, gpu, &snap) || snap.valid ||
        snap.schemaStatus != XBAR_SCHEMA_STATUS_UNAVAILABLE ||
        snap.refusal != XBAR_READ_REFUSAL_LAYOUT_MISMATCH ||
        snap.requestWord != XBAR_NVAPI_CLK_DOMAINS_VERSION || g_lines != 2 ||
        std::strncmp(g_log, kMismatch, std::strlen(kMismatch)) != 0) return 7730;

    // A polled reader logs the first refusal in full, then stays quiet.
    gc_log_steady::Tracker tracker = {};
    clear_log();
    if (polled(&tracker, &snap, 1000) || g_lines != 1 || std::strcmp(g_log, kMismatch))
        return 7731;
    clear_log();
    if (polled(&tracker, &snap, 2000) || polled(&tracker, &snap, 3000) || g_lines)
        return 7732;
    // The interval closes with one summary and no repeated detail.
    if (polled(&tracker, &snap, 1000 + interval) || g_lines != 1 ||
        !std::strstr(g_log, "unchanged refusal repeated n=3 windowS=600") ||
        std::strstr(g_log, "pinned schema")) return 7733;

    // A different refusal flushes the previous count and logs in full.
    clear_log();
    polled(&tracker, &snap, 2000 + interval);
    g_status = XBAR_NVAPI_STATUS_INCOMPATIBLE_STRUCT_VERSION;
    if (polled(&tracker, &snap, 3000 + interval) ||
        snap.refusal != XBAR_READ_REFUSAL_REQUEST_REJECTED || g_lines != 3 ||
        !std::strstr(g_log, "previous refusal repeated n=1") ||
        !std::strstr(g_log, "GET_CONTROL failed status=-9") ||
        !std::strstr(g_log, "every pinned ClkDomains request version was rejected"))
        return 7734;

    // Recovery reports the suppressed repeats once; clean reads stay silent.
    clear_log();
    polled(&tracker, &snap, 4000 + interval);
    answer_pinned_layout();
    if (!polled(&tracker, &snap, 5000 + interval) || !snap.valid || g_lines != 1 ||
        !std::strstr(g_log, "accepted after a refusal (1 identical repeats not logged)"))
        return 7735;
    clear_log();
    if (!polled(&tracker, &snap, 6000 + interval) || g_lines || tracker.haveKey)
        return 7736;

    // A rebind invalidates the tracker: the same refusal is new evidence.
    answer_pascal_layout();
    polled(&tracker, &snap, 7000 + interval);
    gc_log_steady::invalidate(&tracker);
    clear_log();
    if (polled(&tracker, &snap, 8000 + interval) || g_lines != 1 ||
        std::strcmp(g_log, kMismatch)) return 7737;

    // Without a tracker a polled caller keeps the one-shot contract.
    clear_log();
    if (polled(nullptr, &snap, 9000 + interval) ||
        polled(nullptr, &snap, 9001 + interval) || g_lines != 2) return 7738;

    // The comparison key separates refusals and carries rejected statuses.
    char key[gc_log_steady::kKeyBytes] = {};
    xbar_read_outcome_key(&snap, key, sizeof(key));
    if (std::strcmp(key, "refusal=3 request=0x000261A4 response=0x000261A4 statuses="))
        return 7739;
    g_status = -5;
    xbar_read_control_quiet((NvApiFunc)get_control, gpu, &snap);
    xbar_read_outcome_key(&snap, key, sizeof(key));
    if (std::strcmp(key, "refusal=1 request=0x00000000 response=0x00000000 statuses=-5"))
        return 7739;
    return 0;
}
