// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// Stall rationing for the single-threaded Linux serve loop (F-LNX-STALL).
//
// The loop answers one connection at a time, so every millisecond it waits on
// a peer that is not completing its request, or not draining its response, is
// a millisecond no other client is served.  The per-account token bucket
// (service_ipc_throttle_policy.h) cannot ration that on its own: a stall costs
// SERVICE_IPC_COST_TRANSPORT_FAULT tokens, the bucket refills more than that
// during one GC_DAEMON_PEER_STALL_BUDGET_MS window, and admission needs only
// one token.  So a stall is repaid in time instead: the account is refused for
// GC_DAEMON_PEER_STALL_HOLD_FACTOR times as long as it made the loop wait,
// which bounds one account's stalls to 1 / (1 + factor) of the loop.
//
// Handler time is never counted.  A long APPLY is the work the account is
// authorized to ask for, and holding it would refuse an honest client's next
// poll.  A fault that ends at once (EOF, reset) is not a stall either: the
// token cost already rations those.
//
// Pure and host-neutral: the serve loop measures, this decides.

#ifndef GREEN_CURVE_LINUX_DAEMON_STALL_POLICY_H
#define GREEN_CURVE_LINUX_DAEMON_STALL_POLICY_H

#include "service_ipc_throttle_policy.h"
#include "linux_daemon_deadline_policy.h"

#define GC_DAEMON_PEER_STALL_HOLD_FACTOR 2ull

// Below this a failed exchange ended on its own (peer closed or reset), so it
// earns no hold and no log line.
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
// `stalledMs` in one phase.  Zero means no hold.
static inline unsigned long long gc_daemon_peer_stall_hold_ms(
        unsigned long long stalledMs) {
    if (stalledMs < GC_DAEMON_PEER_STALL_HOLD_MIN_MS) return 0ull;
    if (stalledMs > GC_DAEMON_PEER_STALL_MAX_MS)
        stalledMs = GC_DAEMON_PEER_STALL_MAX_MS;
    return stalledMs * GC_DAEMON_PEER_STALL_HOLD_FACTOR;
}

#endif // GREEN_CURVE_LINUX_DAEMON_STALL_POLICY_H
