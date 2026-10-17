// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// The GUI worker's one line per service read, split out of
// gui_mutation_worker.cpp (included after GuiServiceIoCompletion).
//
// The telemetry cadence reads every ~2 s, and logging each one was the single
// largest line in the 0.28 field log (23% of its bytes).  A healthy telemetry
// read is now summarized (debug_log_steady_state_policy.h); everything that
// can explain a stall or a disconnect is still logged at once and in full: a
// failed or deadline-expired read, an error, a read slower than
// kGuiTelemetryReadSlowMs, any change of epoch/phase/reachability, and every
// non-telemetry read (full sync, admin toggle).
#ifndef GREEN_CURVE_GUI_SERVICE_IO_LOG_H
#define GREEN_CURVE_GUI_SERVICE_IO_LOG_H

// Healthy reads measure 0-3 ms; a quarter second is already worth a line.
static const ULONGLONG kGuiTelemetryReadSlowMs = 250;

static void gui_worker_log_read_line(GuiServiceIoKind ioKind,
    const GuiServiceIoCompletion* completion) {
    // deadlineMs makes a timeout self-describing: without it a failed read
    // could not be told apart from a read that was merely slower than a
    // deadline nobody recorded.  reachability/deadlineExpired finish the
    // job: without them a missed answer from a BUSY service and a genuinely
    // absent one are the same log line, which is the conflation that made a
    // 19 s volume stall present as a lost connection (2026-09-12).
    debug_log("GUI service I/O: read kind=%d reason=%s durationMs=%llu deadlineMs=%lu success=%d epoch=%llu phase=%u revision=%llu reachability=%d deadlineExpired=%d error=%s\n",
        (int)ioKind, completion->reason,
        (unsigned long long)completion->durationMs,
        ioKind == GUI_SERVICE_IO_ADMIN_TOGGLE ? 0ul
            : service_state_read_response_timeout_ms(
                ioKind == GUI_SERVICE_IO_FULL_SYNC),
        completion->transportSuccess ? 1 : 0,
        (unsigned long long)completion->connectionEpoch,
        // gc_u32 is `unsigned long`, which is 64-bit under the MinGW/LP64
        // release toolchain and 32-bit under clang-cl/LLP64.  The value is a
        // small enum, so pin the width explicitly rather than letting %u
        // read a 64-bit argument on one toolchain and a 32-bit one on the
        // other.  (Found by the printf attribute added to debug_log.)
        (unsigned int)completion->response.state.gpuPhase,
        (unsigned long long)completion->response.state.stateRevision,
        (int)completion->sendOutcome.reachability,
        completion->sendOutcome.deadlineExpired ? 1 : 0,
        completion->error[0] ? completion->error : "none");
}

// Worker thread only, so the tracker needs no lock.
static void gui_worker_log_read_completion(GuiServiceIoKind ioKind,
    const GuiServiceIoCompletion* completion) {
    static gc_log_steady::Tracker s_telemetry = {};
    bool steady = ioKind == GUI_SERVICE_IO_TELEMETRY &&
        completion->transportSuccess &&
        !completion->sendOutcome.deadlineExpired &&
        !completion->error[0] &&
        completion->durationMs < kGuiTelemetryReadSlowMs;
    if (!steady) {
        if (ioKind == GUI_SERVICE_IO_TELEMETRY) {
            gc_log_steady::invalidate(&s_telemetry);
        }
        gui_worker_log_read_line(ioKind, completion);
        return;
    }
    char key[160] = {};
    StringCchPrintfA(key, ARRAY_COUNT(key),
        "kind=%d reason=%s epoch=%llu phase=%u reachability=%d",
        (int)ioKind, completion->reason,
        (unsigned long long)completion->connectionEpoch,
        (unsigned int)completion->response.state.gpuPhase,
        (int)completion->sendOutcome.reachability);
    gc_log_steady::Decision d = gc_log_steady::observe(&s_telemetry, key,
        GetTickCount64(), (unsigned long long)completion->durationMs,
        g_debug_log_verbose);
    if (gc_log_steady::wants_summary(d)) {
        char window[128] = {};
        gc_log_steady::format_window(window, sizeof(window), d.pending,
            "durationMs");
        debug_log("GUI service I/O: read steady %s %s revision=%llu\n",
            d.action == gc_log_steady::LOG_CHANGE ? "(previous state)" : key,
            window,
            (unsigned long long)completion->response.state.stateRevision);
    }
    if (gc_log_steady::wants_full_line(d)) {
        gui_worker_log_read_line(ioKind, completion);
    }
}

// The window thread's adoption of a READY/non-READY envelope (called from
// gui_service_state.cpp, window thread only).  Same rule as the service's own
// envelope line: the revision bumps on every read, so it is the tracked value
// and never part of the key.  A phase, epoch, instance, generation or section
// change still logs at once.
static void gui_log_service_state_adoption(const char* phaseName,
    gc_u64 connectionEpoch, const ServiceResponse* response) {
    static gc_log_steady::Tracker s_adoption = {};
    char key[160] = {};
    StringCchPrintfA(key, ARRAY_COUNT(key),
        "phase=%s epoch=%llu instance=%llu gpuGeneration=%llu valid=0x%02X",
        phaseName ? phaseName : "?",
        (unsigned long long)connectionEpoch,
        (unsigned long long)response->state.serviceInstanceId,
        (unsigned long long)response->state.gpuGeneration,
        (unsigned int)response->state.validSections);
    gc_log_steady::Decision d = gc_log_steady::observe(&s_adoption, key,
        GetTickCount64(), (unsigned long long)response->state.stateRevision,
        g_debug_log_verbose);
    if (gc_log_steady::wants_summary(d)) {
        char window[128] = {};
        gc_log_steady::format_window(window, sizeof(window), d.pending,
            "revision");
        debug_log("GUI service state: steady %s %s\n",
            d.action == gc_log_steady::LOG_CHANGE ? "(previous state)" : key,
            window);
    }
    if (gc_log_steady::wants_full_line(d)) {
        debug_log("GUI service state: %s revision=%llu\n", key,
            (unsigned long long)response->state.stateRevision);
    }
}

#endif // GREEN_CURVE_GUI_SERVICE_IO_LOG_H
