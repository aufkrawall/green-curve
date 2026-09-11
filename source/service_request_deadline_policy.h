// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#pragma once

// The ONE place the client's request deadlines and the service's own bounds for
// the same request are written down, so the two halves cannot drift apart.
//
// Why this exists (2026-09-11 live incident):
//
// Every service command is dispatched under one process-wide dispatch lock, and
// each state-read handler may additionally wait for the runtime lock and then
// read live hardware before it answers.  The GUI's asynchronous coordinator,
// however, carried two magic literals -- 500 ms for GET_TELEMETRY and 2000 ms
// for GET_SNAPSHOT -- chosen independently of any of that.  Whenever a healthy
// service legitimately took longer, the client cancelled its overlapped read and
// closed the pipe; the service then failed its response write with
// ERROR_NO_DATA (232) for a request it had completed, and the GUI presented the
// whole thing as a lost connection: connection epoch advanced, GPU epoch
// advanced, live authority discarded, model revision/generation/topology wiped,
// the applied-profile indicator dropped to "Manual settings", and the window
// rebuilt -- twice, because the recovery read advanced both epochs again.
//
// In the incident log that is 29 telemetry reads clipped at exactly 500 ms out
// of ~36.7k (p50 15 ms, p99 47 ms, p99.9 203 ms), each one a visible
// disconnect/reconnect flash on a service that never stopped serving.  The
// identical bug class is already documented for the APPLY path in main.cpp
// (SERVICE_APPLY_CLIENT_TIMEOUT_MS); this header generalizes that reasoning to
// the read lanes instead of leaving it as one commented constant.
//
// A client deadline is therefore DERIVED from what the service is allowed to
// spend, never picked. The static_asserts at the bottom enforce that.

// --- Service-side bounds -----------------------------------------------------

// try_lock_service_runtime() budget in service_handle_snapshot_request() and
// service_handle_telemetry_request(). On expiry they serve cached state, so
// this is a hard bound on the lock wait, not an estimate.
#define SERVICE_STATE_READ_RUNTIME_LOCK_WAIT_MS 250u

// Live NVML/NVAPI work a GET_TELEMETRY handler may do once it holds the runtime
// lock: the fan-runtime cache check, the throttled ClkDomains/XBAR read, and the
// bootstrap pulse on the very first snapshot.
#define SERVICE_TELEMETRY_REFRESH_BUDGET_MS 750u

// Live work a GET_SNAPSHOT handler may do under the same lock: the settled VF
// curve re-read (read_live_curve_snapshot_settled(3, 20)) plus a full
// refresh_global_state(). Measured on the incident host at p90 1140 ms with a
// 1968 ms maximum, i.e. the previous 2000 ms client deadline was clipping this
// lane too.
#define SERVICE_SNAPSHOT_REFRESH_BUDGET_MS 2500u

// Request/response framing once the pipe is connected: two message-mode
// transfers of a fixed-size struct plus the server's post-dispatch snapshot
// copy. Deliberately generous; it is not where the time goes.
#define SERVICE_RESPONSE_FRAMING_BUDGET_MS 250u

// How long the asynchronous coordinator waits for the pipe to EXIST and be
// free. This is an availability question ("is the service up?"), not a
// turnaround question, which is exactly why it is a separate budget: a stopped
// service must still be reported promptly no matter how patient the response
// deadline below is.
#define SERVICE_ASYNC_CONNECT_TIMEOUT_MS 2000u

static constexpr unsigned long service_state_read_handler_budget_ms(bool fullSync) {
    return (unsigned long)SERVICE_STATE_READ_RUNTIME_LOCK_WAIT_MS +
        (unsigned long)(fullSync ? SERVICE_SNAPSHOT_REFRESH_BUDGET_MS
                                 : SERVICE_TELEMETRY_REFRESH_BUDGET_MS);
}

// Any command can be queued behind another client's command on the single
// dispatch lock, so every turnaround carries the slowest read handler ahead of
// it. A concurrent third-party APPLY is longer still and deliberately NOT
// covered here: that case is bounded by the service's own apply/wedge
// watchdogs, and one stale telemetry frame is the correct degradation for it.
static constexpr unsigned long service_dispatch_serialization_budget_ms() {
    unsigned long telemetry = service_state_read_handler_budget_ms(false);
    unsigned long snapshot = service_state_read_handler_budget_ms(true);
    return telemetry > snapshot ? telemetry : snapshot;
}

// What the service may legitimately spend between accepting the request and
// writing its response.
static constexpr unsigned long service_state_read_server_budget_ms(bool fullSync) {
    return service_dispatch_serialization_budget_ms() +
        service_state_read_handler_budget_ms(fullSync);
}

// --- Client-side deadlines ---------------------------------------------------

// The asynchronous coordinator's response deadline. Strictly greater than the
// server budget so a deadline expiry means the service really did fail to
// answer within its own contract, rather than that the client got impatient.
static constexpr unsigned long service_state_read_response_timeout_ms(bool fullSync) {
    return service_state_read_server_budget_ms(fullSync) +
        (unsigned long)SERVICE_RESPONSE_FRAMING_BUDGET_MS;
}

static constexpr unsigned long service_state_read_connect_timeout_ms() {
    return (unsigned long)SERVICE_ASYNC_CONNECT_TIMEOUT_MS;
}

// A PING does no hardware work, but it is dispatched under the same serialized
// lock as everything else, so its turnaround still carries whatever command is
// already dispatching. Its 500 ms literal was below that from the moment the
// transport stopped being a single thread, and a ping that expires is reported
// as "the service is installed but not responding" -- i.e. a repair prompt for
// a service that was merely busy.
static constexpr unsigned long service_health_probe_response_timeout_ms() {
    return service_dispatch_serialization_budget_ms() +
        (unsigned long)SERVICE_RESPONSE_FRAMING_BUDGET_MS;
}

// --- Phase arithmetic --------------------------------------------------------

// How long one phase of a request may still run, given how long that phase and
// the whole request have already taken. `totalTimeoutMs == 0` means the phases
// are bounded independently (the asynchronous read lane, which trades a longer
// worst case for never aborting an answer the service is still producing);
// a non-zero total caps their sum, which is what keeps the split from doubling
// the worst-case stall of a synchronous caller blocking the user's thread.
static inline unsigned long service_phase_remaining_ms(
    unsigned long phaseElapsedMs, unsigned long phaseTimeoutMs,
    unsigned long requestElapsedMs, unsigned long totalTimeoutMs) {
    unsigned long phase = phaseElapsedMs >= phaseTimeoutMs
        ? 0ul : phaseTimeoutMs - phaseElapsedMs;
    if (totalTimeoutMs == 0ul) return phase;
    unsigned long total = requestElapsedMs >= totalTimeoutMs
        ? 0ul : totalTimeoutMs - requestElapsedMs;
    return phase < total ? phase : total;
}

// --- Connection identity evidence --------------------------------------------

// Which service instance the client believes it is talking to, after one
// request. A request that was never answered carries NO evidence about the
// identity of the process on the other end, so it must leave the tracked
// instance alone. Synthesizing 0 there made "the request timed out" and "the
// service restarted" the same signal, which is what advanced the connection
// epoch twice per transient miss and forced a full topology re-evaluation on
// each one. A genuine restart is still caught, by the next SUCCESSFUL envelope
// naming a different instance.
static inline gc_u64 service_client_tracked_instance_after_request(
    gc_u64 trackedInstance, bool success, gc_u64 responseInstance) {
    return success ? responseInstance : trackedInstance;
}

// --- Contract enforcement ----------------------------------------------------

static_assert(SERVICE_TELEMETRY_REFRESH_BUDGET_MS > 0u &&
    SERVICE_SNAPSHOT_REFRESH_BUDGET_MS > SERVICE_TELEMETRY_REFRESH_BUDGET_MS,
    "A full snapshot re-reads the curve and every global; it cannot be budgeted "
    "at or below a telemetry refresh");
static_assert(service_state_read_response_timeout_ms(false) >
    service_state_read_server_budget_ms(false),
    "The telemetry response deadline must exceed what the service may spend");
static_assert(service_state_read_response_timeout_ms(true) >
    service_state_read_server_budget_ms(true),
    "The snapshot response deadline must exceed what the service may spend");
static_assert(service_state_read_response_timeout_ms(false) >
    service_state_read_handler_budget_ms(true),
    "Telemetry must survive being queued behind one full snapshot dispatch");
// The literals this header replaced. Asserted so a future edit cannot quietly
// reintroduce a deadline below the service's own bound.
static_assert(service_state_read_response_timeout_ms(false) > 500u &&
    service_state_read_response_timeout_ms(true) > 2000u,
    "Deadlines must stay above the 2026-09-11 magic literals they replaced");
static_assert(service_health_probe_response_timeout_ms() >
    service_dispatch_serialization_budget_ms(),
    "A ping must outlast the command it can be queued behind");
static_assert(service_health_probe_response_timeout_ms() > 500u,
    "The ping deadline must stay above the literal it replaced");
