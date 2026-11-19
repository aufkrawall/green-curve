// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// Which scalar domains an explicit Reset returns to stock.
//
// Reset promises "all GPU settings to their default values", but it used to
// decide memory offset and power target from the service's last snapshot.
// Another tool can change either after that snapshot: a memory offset raised
// elsewhere was then left in place by a Reset that reported success.  The
// Reset now re-reads those controls first and decides from the live value;
// the snapshot only answers when the live read fails (a failed power read
// publishes the board default, which must not be mistaken for "already at
// stock").  A write the service already marked uncertain is always repeated.

#ifndef GREEN_CURVE_RESET_LIVE_READBACK_POLICY_H
#define GREEN_CURVE_RESET_LIVE_READBACK_POLICY_H

static inline bool reset_scalar_domain_needs_write(bool writeUncertain,
    bool liveReadOk, bool liveAtStock, bool cachedAtStock) {
    if (writeUncertain) return true;
    return liveReadOk ? !liveAtStock : !cachedAtStock;
}

// For the one-line diagnostic: where the decision came from.
static inline const char* reset_scalar_decision_source(bool writeUncertain,
    bool liveReadOk) {
    if (writeUncertain) return "uncertain";
    return liveReadOk ? "live" : "cached";
}

#endif  // GREEN_CURVE_RESET_LIVE_READBACK_POLICY_H
