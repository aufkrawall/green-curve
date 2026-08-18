// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// Whether the update channel itself can still be trusted to be telling us the
// truth, as pure logic.
//
// ## The gap this closes
//
// Every other control in the updater answers "is this the maintainer's code?".
// None of them answers "am I still being told about new releases at all?", and
// that is the one an attacker with network position can actually exploit
// without a signing key:
//
//   * **Drop the traffic.** Checks fail forever. The user is pinned on a
//     vulnerable release and nothing says so -- the failures are invisible
//     outside a dialog nobody opens.
//   * **Replay an old signed manifest.** It verifies, it parses, and the client
//     cheerfully reports "up to date" while a fixed release exists. Neither the
//     signature, the digest nor the downgrade refusal rejects a genuine older
//     document, because it is genuine.
//
// The second one is the nastier of the two: the user is actively reassured.
//
// ## Why this is not solved in the manifest
//
// The obvious fix is a signed `issued=` timestamp, and it is unavailable.
// `gc_update_manifest_parse()` refuses unknown keys -- deliberately, and in
// 0.23 as well as today -- so **adding any field to the manifest permanently
// breaks the updater in every already-deployed client**. 0.23 is the only
// release the public has. The manifest grammar is frozen for as long as those
// copies exist, and that is a constraint on the whole feature, not a detail of
// this file.
//
// So both signals are derived client-side from things the client already knows.
//
// ## The high-water mark
//
// The highest version ever advertised by a **signature-verified** manifest is
// remembered. A later verified manifest that advertises something strictly
// older means the channel went backwards, which happens for exactly two
// reasons: someone is replaying an old document, or the maintainer withdrew a
// release. Both are worth telling the user about, and the client cannot tell
// them apart -- so the wording says what was observed rather than what it
// means.
//
// This never blocks anything. It is a notice, not a gate: refusing to update
// because the channel looks odd would hand an attacker a denial of service
// through the very mechanism meant to detect one.

#ifndef GREEN_CURVE_UPDATE_CHANNEL_POLICY_H
#define GREEN_CURVE_UPDATE_CHANNEL_POLICY_H

#include "update_schedule_policy.h"
#include "update_version_policy.h"

// Thirty days with automatic checking ON and not one success. The interval is
// a day by default, so this is thirty consecutive failures -- far outside
// "GitHub was down" and well inside "something is between us and it".
#define GC_UPDATE_CHANNEL_STALE_SECONDS (30LL * 24LL * 60LL * 60LL)

enum GcUpdateChannelState {
    GC_UPDATE_CHANNEL_OK = 0,
    // No successful check for a long time, while checking is switched on.
    GC_UPDATE_CHANNEL_STALE = 1,
    // A verified manifest advertised an older release than one we have already
    // seen advertised. Replay, or a withdrawn release.
    GC_UPDATE_CHANNEL_REGRESSED = 2,
};

// Fold a freshly verified advertisement into the high-water mark.
//
// Only ever called with a version that came out of a manifest whose signature
// verified, which is what makes the mark meaningful: an attacker who could
// raise it at will could suppress the regression signal by first advertising
// something enormous.
static inline bool gc_update_channel_note_version(const char* highestSeenText,
                                                  const char* advertisedText,
                                                  char* out, size_t outSize) {
    if (!out || outSize == 0) return false;
    out[0] = '\0';
    GcUpdateVersion highest;
    GcUpdateVersion advertised;
    gc_update_version_parse(highestSeenText, &highest);
    gc_update_version_parse(advertisedText, &advertised);

    const GcUpdateVersion* winner = nullptr;
    if (advertised.valid && highest.valid) {
        winner = gc_update_version_compare(&advertised, &highest) > 0 ? &advertised
                                                                      : &highest;
    } else if (advertised.valid) {
        winner = &advertised;
    } else if (highest.valid) {
        winner = &highest;
    }
    if (!winner) return false;

    size_t length = 0;
    while (winner->text[length]) length++;
    if (length + 1 > outSize) return false;
    for (size_t i = 0; i <= length; ++i) out[i] = winner->text[i];
    return true;
}

// `lastSuccessUnix` is the last check that actually completed, not the last
// attempt: an attempt that failed is precisely the condition being measured.
static inline GcUpdateChannelState gc_update_channel_state(
    GcUpdateAutoCheck setting, long long lastSuccessUnix, long long nowUnix,
    const char* highestSeenText, const char* advertisedText) {
    // Regression outranks staleness. A successful check that came back with a
    // rolled-back answer is a stronger signal than silence, and reporting the
    // weaker one would bury it.
    GcUpdateVersion highest;
    GcUpdateVersion advertised;
    gc_update_version_parse(highestSeenText, &highest);
    gc_update_version_parse(advertisedText, &advertised);
    if (highest.valid && advertised.valid &&
        gc_update_version_compare(&advertised, &highest) < 0) {
        return GC_UPDATE_CHANNEL_REGRESSED;
    }

    // Silence is only suspicious if the machine was supposed to be listening.
    // With checking off, no news is the user's own decision and saying anything
    // about it would be nagging them about a setting they chose.
    if (setting != GC_UPDATE_AUTO_CHECK_ON) return GC_UPDATE_CHANNEL_OK;
    // Never checked is not stale; it is new. The first-run question covers it.
    if (lastSuccessUnix <= 0) return GC_UPDATE_CHANNEL_OK;
    // A clock that moved backwards produces a negative age. That is a broken
    // clock, not an attack, and treating it as one would fire on every machine
    // whose time synchronises after a CMOS reset.
    if (nowUnix < lastSuccessUnix) return GC_UPDATE_CHANNEL_OK;
    if (nowUnix - lastSuccessUnix > GC_UPDATE_CHANNEL_STALE_SECONDS) {
        return GC_UPDATE_CHANNEL_STALE;
    }
    return GC_UPDATE_CHANNEL_OK;
}

// One sentence for the dialog. Describes the observation, never the inference:
// the client genuinely cannot distinguish a withdrawn release from a replay,
// and telling a user they may be under attack when their maintainer simply
// deleted a bad build is how a warning gets ignored the next time.
static inline bool gc_update_channel_warning(GcUpdateChannelState state,
                                             char* out, size_t outSize) {
    if (!out || outSize == 0) return false;
    out[0] = '\0';
    const char* text = nullptr;
    if (state == GC_UPDATE_CHANNEL_REGRESSED) {
        text = "The update channel is offering an older release than it did "
               "before. Check the releases page before installing anything.";
    } else if (state == GC_UPDATE_CHANNEL_STALE) {
        text = "No update check has succeeded in over a month. Green Curve "
               "cannot tell whether a newer release exists.";
    }
    if (!text) return false;
    size_t length = 0;
    while (text[length]) length++;
    if (length + 1 > outSize) return false;
    for (size_t i = 0; i <= length; ++i) out[i] = text[i];
    return true;
}

#endif  // GREEN_CURVE_UPDATE_CHANNEL_POLICY_H
