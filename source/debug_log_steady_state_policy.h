// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// Pure steady-state summarizer for per-tick debug log lines.
//
// The 0.28 field log measured eight call sites (GUI telemetry reads, the
// service state envelope, the GUI's adoption of it, the XBAR telemetry
// refresh, and the fan worker's watchdog/pulse/wait lines) at about 97% of
// greencurve_debug.txt.  Each fired every 2-6 s, so the 32 MiB cap held only
// about two days and evidence older than that (a service fast-fail during a
// driver reinstall) was gone before anyone looked.  Two of those sites already
// used debug_log_on_change, but their key included a revision counter that
// grows every tick, so nothing was ever suppressed.
//
// The contract each call site follows with this tracker:
//   - the line's STATE KEY (everything that is not a per-tick counter or
//     measurement) changing logs immediately, in full (LOG_CHANGE);
//   - repeats of an unchanged key are counted, and one summary line with the
//     count and min/max/last of one tracked value is logged per interval
//     (LOG_SUMMARY, default 10 minutes);
//   - with verbose logging on, every repeat logs in full (LOG_VERBOSE), which
//     is the old per-tick behavior;
//   - anomalies (failed reads, slow reads, errors) are never routed through a
//     tracker: the caller logs them immediately and calls invalidate(), so the
//     next healthy line is logged as a change and the recovery is visible;
//   - the one exception is a deterministic refusal that a timer repeats
//     verbatim (an unpinned ClkDomains layout, gpu_backend_xbar.h): its full
//     detail is the key, so the first instance and every change still log at
//     once, and the first clean read logs the suppressed count.
//
// The tracker holds no lock.  Every call site either runs on one thread or
// already serializes the call under an existing lock.
#ifndef GREEN_CURVE_DEBUG_LOG_STEADY_STATE_POLICY_H
#define GREEN_CURVE_DEBUG_LOG_STEADY_STATE_POLICY_H

#include <stddef.h>
#include <stdio.h>
#include <string.h>

namespace gc_log_steady {

enum : unsigned long long {
    kSummaryIntervalMs = 10ull * 60ull * 1000ull,
};

enum {
    kKeyBytes = 192,
};

enum Action {
    SUPPRESS = 0,
    LOG_CHANGE = 1,
    LOG_VERBOSE = 2,
    LOG_SUMMARY = 3,
};

struct Window {
    unsigned long long count;
    unsigned long long minValue;
    unsigned long long maxValue;
    unsigned long long lastValue;
    unsigned long long elapsedMs;
};

struct Tracker {
    char key[kKeyBytes];
    bool haveKey;
    unsigned long long windowStartMs;
    Window window;
};

// `pending` is the unsummarized window of the PREVIOUS key on LOG_CHANGE (so a
// caller can flush it first when its count is nonzero) and the window being
// reported on LOG_SUMMARY.  Zero otherwise.
struct Decision {
    Action action;
    Window pending;
};

inline void invalidate(Tracker* t) {
    if (t) t->haveKey = false;
}

inline Decision observe(Tracker* t, const char* key, unsigned long long nowMs,
                        unsigned long long value, bool verbose,
                        unsigned long long intervalMs = kSummaryIntervalMs) {
    Decision d = {};
    if (!t) {
        d.action = LOG_CHANGE;
        return d;
    }
    if (!key) key = "";
    if (!t->haveKey || strncmp(t->key, key, sizeof(t->key) - 1) != 0) {
        d.action = LOG_CHANGE;
        d.pending = t->window;
        d.pending.elapsedMs = t->haveKey || t->window.count
            ? nowMs - t->windowStartMs : 0;
        size_t n = strlen(key);
        if (n > sizeof(t->key) - 1) n = sizeof(t->key) - 1;
        memcpy(t->key, key, n);
        t->key[n] = '\0';
        t->haveKey = true;
        t->windowStartMs = nowMs;
        memset(&t->window, 0, sizeof(t->window));
        return d;
    }
    if (verbose) {
        d.action = LOG_VERBOSE;
        return d;
    }
    if (t->window.count == 0) {
        t->window.minValue = value;
        t->window.maxValue = value;
    } else {
        if (value < t->window.minValue) t->window.minValue = value;
        if (value > t->window.maxValue) t->window.maxValue = value;
    }
    t->window.lastValue = value;
    t->window.count++;
    // Unsigned difference: a clock that moved backwards reads as elapsed, which
    // costs one early summary rather than a window that never closes.
    unsigned long long elapsed = nowMs - t->windowStartMs;
    if (elapsed < intervalMs) {
        d.action = SUPPRESS;
        return d;
    }
    d.action = LOG_SUMMARY;
    d.pending = t->window;
    d.pending.elapsedMs = elapsed;
    t->windowStartMs = nowMs;
    memset(&t->window, 0, sizeof(t->window));
    return d;
}

// What a call site emits for a decision, in this order: the summary line
// first (a closed window, or the repeats of the key that just changed), then
// the full per-tick line (the change itself, or verbose detail).
inline bool wants_summary(const Decision& d) {
    return d.action == LOG_SUMMARY ||
        (d.action == LOG_CHANGE && d.pending.count > 0);
}

inline bool wants_full_line(const Decision& d) {
    return d.action == LOG_CHANGE || d.action == LOG_VERBOSE;
}

// "n=<count> windowS=<s> <valueName>=<min>..<max> last=<last>", the shared
// tail of every summary line so one grep reads all of them.
inline int format_window(char* out, size_t outSize, const Window& w,
                         const char* valueName) {
    if (!out || outSize == 0) return 0;
    int n = snprintf(out, outSize, "n=%llu windowS=%llu %s=%llu..%llu last=%llu",
        w.count, w.elapsedMs / 1000ull,
        valueName && valueName[0] ? valueName : "value",
        w.minValue, w.maxValue, w.lastValue);
    if (n < 0) {
        out[0] = '\0';
        return 0;
    }
    return n;
}

// GREEN_CURVE_DEBUG=verbose (or =2) turns on debug logging AND per-tick
// detail; any other non-"0"/"false" value only turns logging on.
inline bool env_value_requests_verbose(const char* value) {
    if (!value) return false;
    while (*value == ' ' || *value == '\t') value++;
    if (value[0] == '2' && (value[1] == '\0' || value[1] == ' ')) return true;
    const char* word = "verbose";
    size_t i = 0;
    for (; word[i]; ++i) {
        char c = value[i];
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        if (c != word[i]) return false;
    }
    return value[i] == '\0' || value[i] == ' ' || value[i] == '\t';
}

} // namespace gc_log_steady

#endif // GREEN_CURVE_DEBUG_LOG_STEADY_STATE_POLICY_H
