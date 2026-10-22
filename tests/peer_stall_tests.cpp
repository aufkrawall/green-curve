// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// Stall rationing for the single-threaded Linux serve loop
// (linux_daemon_stall_policy.h plus service_ipc_hold()), assertion codes
// 7240-7279.  The hold arithmetic, the table primitive, the exchange
// settlement, and simulated peers that make the loop wait on every connection:
// with the bucket alone they keep it busy almost all the time, with the hold at
// most a third of it -- whether the slow exchange fails or completes.

#include <stdio.h>
#include <string.h>

#include "linux_daemon_stall_policy.h"

namespace {

// How one simulated peer behaves on every admitted connection.
struct PeerShape {
    GcDaemonExchangeOutcome outcome;
    unsigned long long readMs;     // until the request read ends
    unsigned long long handlerMs;  // the daemon's own work, never transport
    unsigned long long writeMs;    // until the response write ends
};

struct PeerRun {
    unsigned long long transportPerMille;  // loop share spent on its transport
    unsigned long long refusals;
};

// Runs `shape` for `spanMs` of simulated time, retrying every 10 ms while
// refused.  `rationed` settles each exchange through the daemon's own
// gc_daemon_exchange_account(); otherwise only the outcome's token cost is
// charged, which is the bucket on its own.
PeerRun simulate_peer(const PeerShape& shape, bool rationed,
                      unsigned long long spanMs) {
    static ServiceIpcAdmissionTable table;
    table.reset(0);
    ServiceIpcThrottleKey key;
    key.fill(0, 0, "uid:1000");
    const unsigned long long writeMs =
        shape.outcome == GC_DAEMON_EXCHANGE_READ_FAILED ? 0ull : shape.writeMs;
    const unsigned long long handlerMs =
        shape.outcome == GC_DAEMON_EXCHANGE_READ_FAILED ? 0ull : shape.handlerMs;
    PeerRun run = {};
    unsigned long long now = 0, transport = 0;
    while (now < spanMs) {
        if (service_ipc_decide_admission(&table, key, SERVICE_IPC_CLASS_NORMAL,
                                         now) != SERVICE_IPC_ADMITTED) {
            run.refusals++;
            now += 10;
            continue;
        }
        now += shape.readMs + handlerMs + writeMs;
        transport += shape.readMs + writeMs;
        if (rationed) {
            gc_daemon_exchange_account(&table, key, shape.outcome, shape.readMs,
                                       writeMs, now);
        } else {
            service_ipc_charge(&table, key, SERVICE_IPC_CLASS_NORMAL,
                gc_daemon_exchange_settle(shape.outcome, shape.readMs,
                                          writeMs).costTokens, now);
        }
    }
    run.transportPerMille = transport * 1000ull / now;
    return run;
}

// One account's transport may take at most 1 / (1 + factor) of the loop; the
// slack covers the 10 ms retry granularity.  An hour, so the last exchange's
// unserved hold cannot skew the share toward the bound.
const unsigned long long kRationedBoundPerMille =
    1000ull / (1ull + GC_DAEMON_PEER_STALL_HOLD_FACTOR) + 5ull;
const unsigned long long kSpanMs = 60ull * 60ull * 1000ull;

int stall_hold_arithmetic_tests() {
    // Faults that end on their own are not stalls.
    if (gc_daemon_peer_stall_hold_ms(0) != 0) return 7240;
    if (gc_daemon_peer_stall_hold_ms(GC_DAEMON_PEER_STALL_HOLD_MIN_MS - 1) != 0)
        return 7241;
    if (gc_daemon_peer_stall_hold_ms(GC_DAEMON_PEER_STALL_HOLD_MIN_MS) !=
        GC_DAEMON_PEER_STALL_HOLD_MIN_MS * GC_DAEMON_PEER_STALL_HOLD_FACTOR)
        return 7242;
    // A stall to the deadline is held twice as long; an over-long measurement
    // (scheduling noise, both phases) is capped below the idle expiry.
    if (gc_daemon_peer_stall_hold_ms(GC_DAEMON_PEER_STALL_BUDGET_MS) !=
        2ull * GC_DAEMON_PEER_STALL_BUDGET_MS) return 7243;
    if (gc_daemon_peer_stall_hold_ms(~0ull) != GC_DAEMON_PEER_STALL_HOLD_MAX_MS)
        return 7244;
    return 0;
}

int stall_hold_table_tests() {
    static ServiceIpcAdmissionTable table;
    ServiceIpcThrottleKey peer, other, invalid;
    peer.fill(0, 0, "uid:1000");
    other.fill(0, 0, "uid:1001");
    invalid.clear();

    // A hold refuses although the bucket is nearly full, ends exactly on
    // time, and leaves the bucket alone.
    table.reset(1000);
    service_ipc_charge(&table, peer, SERVICE_IPC_CLASS_NORMAL,
                       SERVICE_IPC_COST_SUCCESS, 1000);
    service_ipc_hold(&table, peer, 5000, 1000);
    if (service_ipc_decide_admission(&table, peer, SERVICE_IPC_CLASS_NORMAL,
                                     1001) != SERVICE_IPC_REJECTED_RATE) return 7245;
    if (service_ipc_decide_admission(&table, peer, SERVICE_IPC_CLASS_NORMAL,
                                     4999) != SERVICE_IPC_REJECTED_RATE) return 7246;
    if (service_ipc_decide_admission(&table, peer, SERVICE_IPC_CLASS_NORMAL,
                                     5000) != SERVICE_IPC_ADMITTED) return 7247;
    // Expired holds are cleared, not re-evaluated against the old deadline.
    ServiceIpcIdentitySlot* slot = table.find(peer, service_ipc_key_hash(peer));
    if (!slot || slot->holdActive) return 7248;

    // Another account is never held by this one's stall.
    service_ipc_hold(&table, peer, 9000, 5000);
    if (service_ipc_decide_admission(&table, other, SERVICE_IPC_CLASS_NORMAL,
                                     5001) != SERVICE_IPC_ADMITTED) return 7249;

    // A shorter hold never shortens a longer one; a longer one extends it.
    service_ipc_hold(&table, peer, 6000, 5000);
    if (service_ipc_decide_admission(&table, peer, SERVICE_IPC_CLASS_NORMAL,
                                     8000) != SERVICE_IPC_REJECTED_RATE) return 7250;
    service_ipc_hold(&table, peer, 12000, 8000);
    if (service_ipc_decide_admission(&table, peer, SERVICE_IPC_CLASS_NORMAL,
                                     11000) != SERVICE_IPC_REJECTED_RATE) return 7251;
    if (service_ipc_decide_admission(&table, peer, SERVICE_IPC_CLASS_NORMAL,
                                     12000) != SERVICE_IPC_ADMITTED) return 7252;

    // No hold for an invalid key, a past deadline, or a null table.
    table.reset(0);
    service_ipc_hold(&table, invalid, 1000, 0);
    service_ipc_hold(&table, peer, 0, 0);
    service_ipc_hold(&table, peer, 100, 500);
    service_ipc_hold(nullptr, peer, 1000, 0);
    if (table.find(peer, service_ipc_key_hash(peer))) return 7253;

    // Without any hold the Windows-shaped decisions are unchanged: a fresh
    // identity is admitted and a drained bucket is refused by rate.
    service_ipc_charge(&table, peer, SERVICE_IPC_CLASS_NORMAL,
                       SERVICE_IPC_BUCKET_BURST_MILLI / 1000, 0);
    slot = table.find(peer, service_ipc_key_hash(peer));
    if (!slot || slot->holdActive) return 7254;
    if (service_ipc_decide_admission(&table, peer, SERVICE_IPC_CLASS_NORMAL, 0) !=
        SERVICE_IPC_REJECTED_RATE) return 7255;
    return 0;
}

int exchange_settlement_tests() {
    const unsigned long long budget = GC_DAEMON_PEER_STALL_BUDGET_MS;
    // A prompt completed exchange costs one token and earns no hold.
    GcDaemonExchangeSettlement s =
        gc_daemon_exchange_settle(GC_DAEMON_EXCHANGE_COMPLETED, 0, 0);
    if (s.costTokens != SERVICE_IPC_COST_SUCCESS || s.holdMs != 0) return 7258;
    // A request that completes just inside its deadline held the loop as long
    // as one that missed it: success prices the tokens, not the hold.
    s = gc_daemon_exchange_settle(GC_DAEMON_EXCHANGE_COMPLETED, budget - 100, 0);
    if (s.costTokens != SERVICE_IPC_COST_SUCCESS || s.transportMs != budget - 100 ||
        s.holdMs != gc_daemon_peer_stall_hold_ms(budget - 100)) return 7259;
    // A response that times out after a slow request is held for BOTH phases.
    s = gc_daemon_exchange_settle(GC_DAEMON_EXCHANGE_WRITE_FAILED, budget - 100,
                                  budget);
    if (s.costTokens != SERVICE_IPC_COST_TRANSPORT_FAULT ||
        s.transportMs != 2ull * budget - 100 ||
        s.holdMs != gc_daemon_peer_stall_hold_ms(2ull * budget - 100)) return 7260;
    // Two phases each under the threshold still add up to a stall.
    const unsigned long long half = GC_DAEMON_PEER_STALL_HOLD_MIN_MS / 2 + 10;
    if (gc_daemon_exchange_settle(GC_DAEMON_EXCHANGE_COMPLETED, half, half).holdMs !=
        gc_daemon_peer_stall_hold_ms(2 * half)) return 7261;
    // A failed read never reached the write; a stray write figure is ignored.
    s = gc_daemon_exchange_settle(GC_DAEMON_EXCHANGE_READ_FAILED, budget, 5000);
    if (s.costTokens != SERVICE_IPC_COST_TRANSPORT_FAULT || s.transportMs != budget)
        return 7262;
    // The sum saturates rather than wrapping into a short hold.
    if (gc_daemon_exchange_settle(GC_DAEMON_EXCHANGE_COMPLETED, ~0ull, 5).holdMs !=
        GC_DAEMON_PEER_STALL_HOLD_MAX_MS) return 7263;

    // The table half: a slow SUCCESSFUL exchange holds the account (the
    // defect: it used to be charged one token and admitted at once).
    static ServiceIpcAdmissionTable table;
    ServiceIpcThrottleKey peer, invalid;
    peer.fill(0, 0, "uid:1000");
    invalid.clear();
    table.reset(1000);
    s = gc_daemon_exchange_account(&table, peer, GC_DAEMON_EXCHANGE_COMPLETED,
                                   budget - 100, 0, 1000);
    if (service_ipc_decide_admission(&table, peer, SERVICE_IPC_CLASS_NORMAL,
                                     1001) != SERVICE_IPC_REJECTED_RATE) return 7264;
    if (service_ipc_decide_admission(&table, peer, SERVICE_IPC_CLASS_NORMAL,
                                     1000 + s.holdMs) != SERVICE_IPC_ADMITTED) return 7265;
    // An invalid identity and a null table touch nothing.
    table.reset(0);
    gc_daemon_exchange_account(&table, invalid, GC_DAEMON_EXCHANGE_COMPLETED,
                               budget, 0, 0);
    gc_daemon_exchange_account(nullptr, peer, GC_DAEMON_EXCHANGE_COMPLETED,
                               budget, 0, 0);
    if (table.find(peer, service_ipc_key_hash(peer))) return 7266;
    return 0;
}

int stall_occupancy_tests() {
    const unsigned long long budget = GC_DAEMON_PEER_STALL_BUDGET_MS;
    // The defect this closes: the bucket refills faster than a stall costs,
    // so a peer stalling every connection held the loop almost permanently.
    const PeerShape readTimesOut = {GC_DAEMON_EXCHANGE_READ_FAILED, budget, 0, 0};
    if (simulate_peer(readTimesOut, false, kSpanMs).transportPerMille < 900ull)
        return 7256;
    if (simulate_peer(readTimesOut, true, kSpanMs).transportPerMille >
        kRationedBoundPerMille) return 7257;

    // A request that always completes 100 ms before its deadline, answered
    // promptly: 100% of the loop when only failures were held.
    const PeerShape slowSuccess = {GC_DAEMON_EXCHANGE_COMPLETED, budget - 100, 0, 0};
    if (simulate_peer(slowSuccess, false, kSpanMs).transportPerMille < 900ull)
        return 7267;
    if (simulate_peer(slowSuccess, true, kSpanMs).transportPerMille >
        kRationedBoundPerMille) return 7268;
    // The same slow request followed by a response write that times out: about
    // half the loop when the hold covered only the write.
    const PeerShape slowThenWriteFails = {GC_DAEMON_EXCHANGE_WRITE_FAILED,
                                          budget - 100, 0, budget};
    if (simulate_peer(slowThenWriteFails, true, kSpanMs).transportPerMille >
        kRationedBoundPerMille) return 7269;

    // An honest client whose exchanges are prompt is never held, however long
    // the daemon's own handler takes (an APPLY can take seconds).
    const PeerShape honestLongHandler = {GC_DAEMON_EXCHANGE_COMPLETED, 1, 5000, 1};
    if (simulate_peer(honestLongHandler, true, kSpanMs).refusals != 0) return 7270;
    return 0;
}

}  // namespace

int run_peer_stall_tests() {
    if (int failure = stall_hold_arithmetic_tests()) return failure;
    if (int failure = stall_hold_table_tests()) return failure;
    if (int failure = exchange_settlement_tests()) return failure;
    if (int failure = stall_occupancy_tests()) return failure;
    return 0;
}
