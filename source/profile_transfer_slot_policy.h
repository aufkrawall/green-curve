// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#ifndef GREEN_CURVE_PROFILE_TRANSFER_SLOT_POLICY_H
#define GREEN_CURVE_PROFILE_TRANSFER_SLOT_POLICY_H
#include <cstdlib>

// Choose default destination slots (1..5) for `count` imported profile records
// (0..5), given each record's optional natural slot `preferred[i]` (e.g. file
// ordinal for a portable bank or original slot number for a legacy INI) and
// `saved[1..5]` indicating which destination slots currently hold a profile.
//
// Invariants:
// - Every assigned slot is in 1..5 and distinct.
// - When all `count` records fit in empty slots (`count <= emptyCount`), no
//   saved slot is overwritten: if every record's distinct preferred slot is
//   empty, keep it; otherwise assign empty slots in ascending slot order.
// - When `count > emptyCount`, some saved slots must be overwritten. Never let
//   an earlier record steal another record's empty preferred slot (e.g. a
//   5-profile import with only slot 5 empty assigns 1->1..5->5 instead of
//   swapping Profile 1 into slot 5 and Profile 5 into slot 1). Remaining
//   unclaimed empty slots are matched monotonically to the closest unassigned
//   records to minimize overwrites.
static inline void profile_transfer_default_slots(int count, const int* preferred,
    const bool saved[6], int outSlots[5]) {
    if (!outSlots) return;
    for (int i = 0; i < 5; ++i) outSlots[i] = i + 1;
    if (count <= 0) return;
    if (count > 5) count = 5;

    int pref[5]{};
    bool prefSeen[6]{};
    bool allPrefDistinct = true;
    for (int i = 0; i < count; ++i) {
        const int p = preferred ? preferred[i] : (i + 1);
        if (p >= 1 && p <= 5 && !prefSeen[p]) {
            pref[i] = p;
            prefSeen[p] = true;
        } else {
            allPrefDistinct = false;
            pref[i] = 0;
        }
    }
    if (!allPrefDistinct) {
        bool usedFallback[6]{};
        for (int i = 0; i < count; ++i) if (pref[i]) usedFallback[pref[i]] = true;
        for (int i = 0; i < count; ++i) {
            if (pref[i]) continue;
            for (int s = 1; s <= 5; ++s) {
                if (!usedFallback[s]) { pref[i] = s; usedFallback[s] = true; break; }
            }
        }
    }

    int emptySlots[5]{};
    int emptyCount = 0;
    for (int s = 1; s <= 5; ++s) {
        if (!saved || !saved[s]) emptySlots[emptyCount++] = s;
    }

    if (count <= emptyCount) {
        bool allPrefEmpty = true;
        for (int i = 0; i < count; ++i) {
            if (saved && saved[pref[i]]) { allPrefEmpty = false; break; }
        }
        if (allPrefEmpty) {
            for (int i = 0; i < count; ++i) outSlots[i] = pref[i];
        } else {
            for (int i = 0; i < count; ++i) outSlots[i] = emptySlots[i];
        }
        return;
    }

    int assigned[5]{};
    bool taken[6]{};
    // Pass 1: claim every record's own preferred slot when that slot is empty.
    for (int i = 0; i < count; ++i) {
        const int p = pref[i];
        if ((!saved || !saved[p]) && !taken[p]) {
            assigned[i] = p;
            taken[p] = true;
        }
    }

    // Pass 2: assign any remaining unclaimed empty slots in ascending order to
    // the closest unassigned rows (preserving relative order).
    int remEmpty[5]{};
    int remEmptyCount = 0;
    for (int i = 0; i < emptyCount; ++i) {
        if (!taken[emptySlots[i]]) remEmpty[remEmptyCount++] = emptySlots[i];
    }
    if (remEmptyCount > 0) {
        bool useEmpty[5]{};
        for (int e = 0; e < remEmptyCount; ++e) {
            int bestRow = -1;
            int bestDist = 100;
            for (int i = 0; i < count; ++i) {
                if (assigned[i] || useEmpty[i]) continue;
                const int dist = std::abs(pref[i] - remEmpty[e]);
                if (dist < bestDist) { bestDist = dist; bestRow = i; }
            }
            if (bestRow >= 0) useEmpty[bestRow] = true;
        }
        int nextEmpty = 0;
        for (int i = 0; i < count && nextEmpty < remEmptyCount; ++i) {
            if (!useEmpty[i]) continue;
            assigned[i] = remEmpty[nextEmpty++];
            taken[assigned[i]] = true;
        }
    }

    // Pass 3: keep remaining unassigned rows on their own preferred slots.
    for (int i = 0; i < count; ++i) {
        if (assigned[i]) continue;
        const int p = pref[i];
        if (!taken[p]) { assigned[i] = p; taken[p] = true; }
    }

    // Pass 4: fallback to any remaining untaken slot.
    for (int i = 0; i < count; ++i) {
        if (assigned[i]) continue;
        for (int s = 1; s <= 5; ++s) {
            if (!taken[s]) { assigned[i] = s; taken[s] = true; break; }
        }
    }

    for (int i = 0; i < count; ++i) outSlots[i] = assigned[i] ? assigned[i] : (i + 1);
}
#endif
