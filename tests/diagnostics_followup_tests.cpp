// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// 0.28 field-log diagnostics follow-ups (assertion codes 6700-6799): the
// steady-state summarizer behind the per-tick log lines
// (debug_log_steady_state_policy.h).  Pure header; no GPU, no service.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "debug_log_steady_state_policy.h"

namespace {

using namespace gc_log_steady;

int run_steady_state_tests() {
    Tracker t = {};
    // First sight of a key is a change, with nothing pending.
    Decision d = observe(&t, "phase=ready", 1000, 5, false);
    if (d.action != LOG_CHANGE || d.pending.count != 0) return 6700;
    if (!wants_full_line(d) || wants_summary(d)) return 6701;
    // Repeats inside the window are counted, not logged.
    d = observe(&t, "phase=ready", 3000, 9, false);
    if (d.action != SUPPRESS || wants_full_line(d) || wants_summary(d)) return 6702;
    d = observe(&t, "phase=ready", 5000, 2, false);
    if (d.action != SUPPRESS) return 6703;
    // The window closes after the interval and reports count/min/max/last.
    d = observe(&t, "phase=ready", 1000 + kSummaryIntervalMs, 7, false);
    if (d.action != LOG_SUMMARY || !wants_summary(d) || wants_full_line(d)) return 6704;
    if (d.pending.count != 3 || d.pending.minValue != 2 || d.pending.maxValue != 9 ||
        d.pending.lastValue != 7 || d.pending.elapsedMs != kSummaryIntervalMs) return 6705;
    // ...and a new window starts empty.
    d = observe(&t, "phase=ready", 2000 + kSummaryIntervalMs, 1, false);
    if (d.action != SUPPRESS || t.window.count != 1 || t.window.minValue != 1) return 6706;
    // A key change logs at once and hands back the repeats of the old key.
    d = observe(&t, "phase=degraded", 3000 + kSummaryIntervalMs, 1, false);
    if (d.action != LOG_CHANGE || d.pending.count != 1 || !wants_summary(d) ||
        !wants_full_line(d)) return 6707;
    // Verbose logs every repeat in full; the first sight is still a change.
    Tracker v = {};
    if (observe(&v, "k", 0, 0, true).action != LOG_CHANGE) return 6708;
    d = observe(&v, "k", 1, 0, true);
    if (d.action != LOG_VERBOSE || !wants_full_line(d) || wants_summary(d)) return 6709;
    // invalidate(): the next healthy line is a change, so a recovery shows.
    Tracker a = {};
    observe(&a, "k", 0, 0, false);
    observe(&a, "k", 10, 0, false);
    invalidate(&a);
    d = observe(&a, "k", 20, 0, false);
    if (d.action != LOG_CHANGE) return 6710;
    // A null tracker degrades to logging, never to silence.
    if (observe(nullptr, "k", 0, 0, false).action != LOG_CHANGE) return 6711;

    // THE 0.28 FIELD-LOG BUG: a key that contains a per-read revision never
    // repeats, so on-change suppression suppressed nothing.  With the revision
    // as the tracked value instead, an hour of 2 s ticks costs one change line
    // plus one summary per 10 minutes.
    Tracker hour = {};
    int lines = 0;
    for (unsigned long long tick = 0; tick < 1800; ++tick) {
        Decision h = observe(&hour, "instance=1 phase=4 valid=0x1F", tick * 2000ull,
                             5000 + tick, false);
        if (wants_summary(h)) lines++;
        if (wants_full_line(h)) lines++;
    }
    if (lines > 7) return 6712;
    Tracker revisionKey = {};
    int naive = 0;
    for (unsigned long long tick = 0; tick < 1800; ++tick) {
        char key[64] = {};
        snprintf(key, sizeof(key), "revision=%llu phase=4", tick);
        if (wants_full_line(observe(&revisionKey, key, tick * 2000ull, 0, false))) naive++;
    }
    if (naive != 1800) return 6713;  // the failure mode the call sites must avoid

    // The shared summary tail.
    Window w = {};
    w.count = 300; w.minValue = 0; w.maxValue = 3; w.lastValue = 1; w.elapsedMs = 600999;
    char buf[128] = {};
    format_window(buf, sizeof(buf), w, "durationMs");
    if (strcmp(buf, "n=300 windowS=600 durationMs=0..3 last=1") != 0) return 6714;
    format_window(buf, sizeof(buf), w, nullptr);
    if (strncmp(buf, "n=300 windowS=600 value=0..3", 28) != 0) return 6715;
    char tiny[8] = {};
    format_window(tiny, sizeof(tiny), w, "x");
    if (strlen(tiny) != 7) return 6716;

    // Keys longer than the stored key compare on their stored prefix only.
    char longA[300], longB[300];
    memset(longA, 'a', sizeof(longA)); longA[299] = '\0';
    memcpy(longB, longA, sizeof(longB)); longB[250] = 'b';
    Tracker l = {};
    observe(&l, longA, 0, 0, false);
    if (observe(&l, longB, 1, 0, false).action != SUPPRESS) return 6717;

    // GREEN_CURVE_DEBUG values that request per-tick detail.
    if (!env_value_requests_verbose("verbose") ||
        !env_value_requests_verbose("VERBOSE") ||
        !env_value_requests_verbose("  Verbose") ||
        !env_value_requests_verbose("2")) return 6718;
    if (env_value_requests_verbose("1") || env_value_requests_verbose("0") ||
        env_value_requests_verbose("") || env_value_requests_verbose(nullptr) ||
        env_value_requests_verbose("verbosely") ||
        env_value_requests_verbose("22") ||
        env_value_requests_verbose("verb")) return 6719;
    return 0;
}

}  // namespace

int run_diagnostics_followup_tests() {
    if (int r = run_steady_state_tests()) return r;
    return 0;
}
