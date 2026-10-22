// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// Stall rationing for the single-threaded Linux serve loop
// (linux_daemon_stall_policy.h plus service_ipc_hold()), assertion codes
// 7240-7259.  The hold arithmetic, the table primitive, and a simulated peer
// that stalls every connection to the deadline: with the bucket alone it keeps
// the loop busy almost all the time, with the hold at most a third of it.

#include <stdio.h>
#include <string.h>

#include "linux_daemon_stall_policy.h"

namespace {

// A peer that stalls each admitted connection to the full read deadline and
// retries every 10 ms while refused, for `spanMs` of simulated time.  Returns
// the share of that time the loop spent waiting on it, in per mille.
unsigned long long simulate_stalling_peer(bool withHold,
                                          unsigned long long spanMs) {
    static ServiceIpcAdmissionTable table;
    table.reset(0);
    ServiceIpcThrottleKey key;
    key.fill(0, 0, "uid:1000");
    unsigned long long now = 0, occupied = 0;
    while (now < spanMs) {
        if (service_ipc_decide_admission(&table, key, SERVICE_IPC_CLASS_NORMAL,
                                         now) != SERVICE_IPC_ADMITTED) {
            now += 10;
            continue;
        }
        const unsigned long long stalled = GC_DAEMON_PEER_STALL_BUDGET_MS;
        now += stalled;
        occupied += stalled;
        service_ipc_charge(&table, key, SERVICE_IPC_CLASS_NORMAL,
                           SERVICE_IPC_COST_TRANSPORT_FAULT, now);
        if (withHold) {
            const unsigned long long hold = gc_daemon_peer_stall_hold_ms(stalled);
            service_ipc_hold(&table, key, now + hold, now);
        }
    }
    return occupied * 1000ull / now;
}

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

int stall_occupancy_tests() {
    // The defect this closes: the bucket refills faster than a stall costs,
    // so a peer stalling every connection held the loop almost permanently.
    const unsigned long long spanMs = 10ull * 60ull * 1000ull;
    if (simulate_stalling_peer(false, spanMs) < 900ull) return 7256;
    // With the hold one account gets at most 1 / (1 + factor) of the loop.
    const unsigned long long bound =
        1000ull / (1ull + GC_DAEMON_PEER_STALL_HOLD_FACTOR) + 5ull;
    if (simulate_stalling_peer(true, spanMs) > bound) return 7257;
    return 0;
}

}  // namespace

int run_peer_stall_tests() {
    if (int failure = stall_hold_arithmetic_tests()) return failure;
    if (int failure = stall_hold_table_tests()) return failure;
    if (int failure = stall_occupancy_tests()) return failure;
    return 0;
}
