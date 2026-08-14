// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// Release version parsing and ordering for the in-app updater, as pure logic.
//
// The updater's single most important refusal is a DOWNGRADE.  Neither the
// GitHub build-provenance attestation nor the published `.sha256` files can
// catch one: an attacker who replays an older release replays a genuinely
// signed, genuinely attested, correctly hashed artifact.  The only thing that
// rejects it is comparing versions and refusing anything that is not strictly
// newer than what is installed, which makes this comparator a security
// boundary rather than a display helper.
//
// Accepted grammar is exactly the one `.github/workflows/release.yml` enforces
// on the `VERSION` file before it will cut a tag:
//
//     MAJOR "." MINOR [ "." PATCH ]
//
// with two deliberate tightenings over that regex:
//
//   - **No leading zeros** in a multi-digit component.  The workflow regex
//     would accept `0.022`, which orders identically to `0.22` here but is a
//     different string, and the release *filenames* are built from the raw
//     text.  Two spellings of one version is exactly the ambiguity a
//     mix-and-match attack wants; there is no upside to tolerating it.
//   - **A component ceiling.**  Six digits is far past anything this project
//     will ship and keeps the parse away from `int` overflow, which would
//     otherwise be a way to make a huge version compare as negative and turn
//     the downgrade check inside out.
//
// A missing PATCH is zero, so `0.22` and `0.22.0` compare equal -- but the raw
// text is preserved separately, because the release asset names embed the
// version exactly as it was written and the updater has to reproduce them
// byte-for-byte.

#ifndef GREEN_CURVE_UPDATE_VERSION_POLICY_H
#define GREEN_CURVE_UPDATE_VERSION_POLICY_H

#include <stddef.h>

// Longest accepted version text, e.g. "999999.999999.999999" plus terminator.
#define GC_UPDATE_VERSION_MAX_CHARS 24
#define GC_UPDATE_VERSION_MAX_COMPONENT 999999
#define GC_UPDATE_VERSION_MAX_COMPONENT_DIGITS 6

struct GcUpdateVersion {
    int major;
    int minor;
    int patch;
    // The version exactly as it appeared in its source, which is what the
    // release asset filenames are built from.  Never reconstruct this from the
    // three integers: "0.22" and "0.22.0" are the same version and different
    // filenames.
    char text[GC_UPDATE_VERSION_MAX_CHARS];
    bool valid;
};

// Parse one component starting at `*cursor`.  Advances the cursor past the
// digits it consumed.  Returns false on an empty component, a leading zero in
// a multi-digit run, more digits than the ceiling allows, or a non-digit.
static inline bool gc_update_parse_component(const char** cursor, int* valueOut) {
    if (!cursor || !*cursor || !valueOut) return false;
    const char* p = *cursor;
    size_t digits = 0;
    int value = 0;
    while (p[digits] >= '0' && p[digits] <= '9') {
        if (digits >= GC_UPDATE_VERSION_MAX_COMPONENT_DIGITS) return false;
        value = value * 10 + (p[digits] - '0');
        digits++;
    }
    if (digits == 0) return false;
    // "0" is fine; "01" is not.  See the header comment.
    if (digits > 1 && p[0] == '0') return false;
    if (value > GC_UPDATE_VERSION_MAX_COMPONENT) return false;
    *cursor = p + digits;
    *valueOut = value;
    return true;
}

// Parse `MAJOR.MINOR[.PATCH]`.  Anything else -- a leading `v`, surrounding
// whitespace, a fourth component, a trailing dot, an empty string, a
// pre-release suffix -- leaves `valid` false.  The caller must never treat an
// invalid version as "assume it is old"; every consumer here refuses instead.
static inline void gc_update_version_parse(const char* text, GcUpdateVersion* out) {
    if (!out) return;
    GcUpdateVersion blank = {};
    *out = blank;
    if (!text || !text[0]) return;

    size_t length = 0;
    while (text[length]) {
        if (length + 1 >= GC_UPDATE_VERSION_MAX_CHARS) return;
        length++;
    }

    const char* cursor = text;
    int major = 0, minor = 0, patch = 0;
    if (!gc_update_parse_component(&cursor, &major)) return;
    if (*cursor != '.') return;
    cursor++;
    if (!gc_update_parse_component(&cursor, &minor)) return;
    if (*cursor == '.') {
        cursor++;
        if (!gc_update_parse_component(&cursor, &patch)) return;
    }
    // Trailing garbage is a rejection, not something to ignore.  "0.23-beta"
    // must not silently become "0.23".
    if (*cursor != 0) return;

    out->major = major;
    out->minor = minor;
    out->patch = patch;
    for (size_t i = 0; i < length; ++i) out->text[i] = text[i];
    out->text[length] = 0;
    out->valid = true;
}

// -1 / 0 / +1 ordering.  Both sides must be valid; the caller is expected to
// have checked that, and a comparison involving an invalid version returns 0
// so that it can never *look* newer than something real.
static inline int gc_update_version_compare(const GcUpdateVersion* a,
                                            const GcUpdateVersion* b) {
    if (!a || !b || !a->valid || !b->valid) return 0;
    if (a->major != b->major) return a->major < b->major ? -1 : 1;
    if (a->minor != b->minor) return a->minor < b->minor ? -1 : 1;
    if (a->patch != b->patch) return a->patch < b->patch ? -1 : 1;
    return 0;
}

// The downgrade gate.  A candidate is offered only when both versions parsed
// and the candidate is STRICTLY newer.  Equal is not newer: re-installing the
// running version unattended would stop the service, reset the GPU and re-apply
// for no reason at all.
static inline bool gc_update_is_newer(const GcUpdateVersion* installed,
                                      const GcUpdateVersion* candidate) {
    if (!installed || !candidate) return false;
    if (!installed->valid || !candidate->valid) return false;
    return gc_update_version_compare(candidate, installed) > 0;
}

// Whether the installed version is new enough to take an unattended jump to
// the candidate.  A release can declare a floor (`min_from` in the manifest)
// when its upgrade path needs something the older build cannot do -- the
// settings-export verb that only exists from 0.21 on is the standing example.
// Below the floor the updater must send the user to a manual install rather
// than run a silent upgrade that would quietly lose their settings.
static inline bool gc_update_meets_minimum_from(const GcUpdateVersion* installed,
                                                const GcUpdateVersion* minimumFrom) {
    if (!installed || !installed->valid) return false;
    // No declared floor is not an error: most releases do not need one.
    if (!minimumFrom || !minimumFrom->valid) return true;
    return gc_update_version_compare(installed, minimumFrom) >= 0;
}

#endif // GREEN_CURVE_UPDATE_VERSION_POLICY_H
