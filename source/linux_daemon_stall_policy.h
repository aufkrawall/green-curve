// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// Stall rationing for the single-threaded Linux serve loop (F-LNX-STALL).
//
// The loop answers one connection at a time, so every millisecond it waits on
// a peer that is not completing its request, or not draining its response, is
// a millisecond no other client is served.  The per-account token bucket
// (service_ipc_throttle_policy.h) cannot ration that on its own: a stall costs
// at most SERVICE_IPC_COST_TRANSPORT_FAULT tokens, the bucket refills more than
// that during one GC_DAEMON_PEER_STALL_BUDGET_MS window, and admission needs
// only one token.  So a stall is repaid in time instead: the account is refused
// for GC_DAEMON_PEER_STALL_HOLD_FACTOR times as long as it made the loop wait,
// which bounds one account's transport time to 1 / (1 + factor) of the loop.
//
// The wait is the request read PLUS the response write, whatever the outcome.
// A request that completes 100 ms before its deadline held the loop as long as
// one that missed it, and a response that times out after a slow request held
// it for both.  Counting only failed phases let a peer that always finished
// just in time keep the loop busy all the time.
//
// Handler time is never counted.  A long APPLY is the work the account is
// authorized to ask for, and holding it would refuse an honest client's next
// poll.  Transport under GC_DAEMON_PEER_STALL_HOLD_MIN_MS is not a stall
// either: a local client's exchange takes well under that, and a fault that
// ends at once (EOF, reset) is already rationed by its token cost.
//
// Pure and host-neutral: the serve loop measures, this decides.

#ifndef GREEN_CURVE_LINUX_DAEMON_STALL_POLICY_H
#define GREEN_CURVE_LINUX_DAEMON_STALL_POLICY_H

#include "service_ipc_throttle_policy.h"
#include "linux_daemon_deadline_policy.h"

#define GC_DAEMON_PEER_STALL_HOLD_FACTOR 2ull

// Below this an exchange's transport time is not a stall (a prompt local
// client, or a peer that closed or reset), so it earns no hold and no log line.
#define GC_DAEMON_PEER_STALL_HOLD_MIN_MS 100ull

// One connection can stall twice: reading the request, then draining the
// response.  Each phase has its own GC_DAEMON_PEER_STALL_BUDGET_MS deadline.
#define GC_DAEMON_PEER_STALL_MAX_MS (2ull * GC_DAEMON_PEER_STALL_BUDGET_MS)
#define GC_DAEMON_PEER_STALL_HOLD_MAX_MS \
    (GC_DAEMON_PEER_STALL_HOLD_FACTOR * GC_DAEMON_PEER_STALL_MAX_MS)

static_assert(GC_DAEMON_PEER_STALL_HOLD_FACTOR >= 1ull,
    "A hold shorter than the stall leaves one account more than half the loop");
static_assert(GC_DAEMON_PEER_STALL_HOLD_MIN_MS < GC_DAEMON_PEER_STALL_BUDGET_MS,
    "A peer that stalls to the deadline must always earn a hold");
static_assert(GC_DAEMON_PEER_STALL_HOLD_MAX_MS <
    (unsigned long long)SERVICE_IPC_IDLE_EXPIRY_MS,
    "A hold must end before its identity can idle out of the admission table, "
    "or the expiry would lift it early");

// How long to refuse an account whose connection made the loop wait
// `stalledMs` in transport.  Zero means no hold.
static inline unsigned long long gc_daemon_peer_stall_hold_ms(
        unsigned long long stalledMs) {
    if (stalledMs < GC_DAEMON_PEER_STALL_HOLD_MIN_MS) return 0ull;
    if (stalledMs > GC_DAEMON_PEER_STALL_MAX_MS)
        stalledMs = GC_DAEMON_PEER_STALL_MAX_MS;
    return stalledMs * GC_DAEMON_PEER_STALL_HOLD_FACTOR;
}

// How an ADMITTED exchange ended.  A refused peer is never read or written,
// so it never reaches the settlement.
enum GcDaemonExchangeOutcome {
    GC_DAEMON_EXCHANGE_COMPLETED = 0,
    GC_DAEMON_EXCHANGE_READ_FAILED = 1,
    GC_DAEMON_EXCHANGE_WRITE_FAILED = 2,
};

static inline const char* gc_daemon_exchange_outcome_name(
        GcDaemonExchangeOutcome outcome) {
    switch (outcome) {
        case GC_DAEMON_EXCHANGE_COMPLETED: return "completed";
        case GC_DAEMON_EXCHANGE_READ_FAILED: return "request read failed";
        case GC_DAEMON_EXCHANGE_WRITE_FAILED: return "response write failed";
    }
    return "unknown";
}

struct GcDaemonExchangeSettlement {
    unsigned int costTokens;         // what the outcome costs in the bucket
    unsigned long long transportMs;  // request read + response write
    unsigned long long holdMs;       // 0 = no hold
};

// The outcome prices the bucket charge; the transport time alone decides the
// hold.  Handler time is not a parameter, so no caller can count it.
static inline GcDaemonExchangeSettlement gc_daemon_exchange_settle(
        GcDaemonExchangeOutcome outcome, unsigned long long readMs,
        unsigned long long writeMs) {
    GcDaemonExchangeSettlement settled = {};
    settled.costTokens = outcome == GC_DAEMON_EXCHANGE_COMPLETED
        ? (unsigned int)SERVICE_IPC_COST_SUCCESS
        : (unsigned int)SERVICE_IPC_COST_TRANSPORT_FAULT;
    // A failed read never reached the write.
    if (outcome == GC_DAEMON_EXCHANGE_READ_FAILED) writeMs = 0ull;
    settled.transportMs = readMs > ~0ull - writeMs ? ~0ull : readMs + writeMs;
    settled.holdMs = gc_daemon_peer_stall_hold_ms(settled.transportMs);
    return settled;
}

// Charges and holds the account exactly once for one admitted exchange.  The
// serve loop's only route into the table after admission, so the tests drive
// the same branch the daemon runs.
static inline GcDaemonExchangeSettlement gc_daemon_exchange_account(
        ServiceIpcAdmissionTable* table, const ServiceIpcThrottleKey& key,
        GcDaemonExchangeOutcome outcome, unsigned long long readMs,
        unsigned long long writeMs, unsigned long long nowMs) {
    const GcDaemonExchangeSettlement settled =
        gc_daemon_exchange_settle(outcome, readMs, writeMs);
    if (!table || !key.valid) return settled;
    service_ipc_charge(table, key, SERVICE_IPC_CLASS_NORMAL, settled.costTokens,
                       nowMs);
    if (settled.holdMs)
        service_ipc_hold(table, key, nowMs + settled.holdMs, nowMs);
    return settled;
}

#endif // GREEN_CURVE_LINUX_DAEMON_STALL_POLICY_H
