// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#pragma once
#include "profile_document.h"
#include <cstdio>
#include <string>

enum ProfileEditorSourceField { PROFILE_EDIT_POINT, PROFILE_EDIT_GRAPHICS, PROFILE_EDIT_MEMORY };

// Decimal MHz is exact to one Hz. Reject excess precision and overflow rather
// than rounding a document to the destination's display or driver grid.
static inline bool profile_editor_parse_mhz(const char* text, long long& out) {
    if (!text || !*text) return false;
    const bool negative = *text == '-';
    if (*text == '-' || *text == '+') ++text;
    if (*text < '0' || *text > '9') return false;
    const unsigned long long limit = (unsigned long long)LLONG_MAX + (negative ? 1ULL : 0ULL);
    unsigned long long whole = 0, fraction = 0;
    int digits = 0;
    while (*text >= '0' && *text <= '9') {
        const unsigned digit = (unsigned)(*text++ - '0');
        if (whole > (limit / 1000000ULL - digit) / 10) return false;
        whole = whole * 10 + digit;
    }
    if (*text == '.') {
        ++text;
        while (*text >= '0' && *text <= '9') {
            if (++digits > 6) return false;
            fraction = fraction * 10 + (unsigned)(*text++ - '0');
        }
        if (!digits) return false;
    }
    if (*text) return false;
    for (; digits < 6; ++digits) fraction *= 10;
    if (whole > limit / 1000000ULL || fraction > limit - whole * 1000000ULL) return false;
    const auto magnitude = whole * 1000000ULL + fraction;
    out = negative ? (magnitude == (unsigned long long)LLONG_MAX + 1ULL ? LLONG_MIN : -(long long)magnitude)
                   : (long long)magnitude;
    return true;
}
static inline std::string profile_editor_format_mhz(long long hz) {
    const auto magnitude = hz < 0 ? (unsigned long long)(-(hz + 1)) + 1ULL : (unsigned long long)hz;
    char text[40]{};
    std::snprintf(text, sizeof(text), "%s%llu.%06llu", hz < 0 ? "-" : "", magnitude / 1000000ULL, magnitude % 1000000ULL);
    std::string result(text);
    while (result.back() == '0') result.pop_back();
    if (result.back() == '.') result.pop_back();
    return result;
}
static inline bool profile_editor_source_editable(const ProfileDocument& d, ProfileEditorSourceField field, int index) {
    if (d.applicationScope == PROFILE_SCOPE_EXPLICIT_RELEASE) return false;
    switch (field) {
        case PROFILE_EDIT_POINT:
            return (d.domains & PROFILE_DOMAIN_VF_CURVE) && index >= 0 && index < d.topologyCount &&
                index < VF_NUM_POINTS && d.topology[index].sourceWritable != 2;
        case PROFILE_EDIT_GRAPHICS: return d.graphicsOffset.present;
        case PROFILE_EDIT_MEMORY: return d.hasMemoryOffset;
    }
    return false;
}
static inline std::string profile_editor_source_seed(const ProfileDocument& d, ProfileEditorSourceField field, int index) {
    if (!profile_editor_source_editable(d, field, index)) return {};
    if (field == PROFILE_EDIT_GRAPHICS) return profile_editor_format_mhz(d.graphicsOffset.offsetHz);
    if (field == PROFILE_EDIT_MEMORY) return profile_editor_format_mhz(d.memoryOffsetHz);
    const auto& p = d.points[index];
    if (p.kind == PROFILE_POINT_ABSOLUTE_FREQUENCY) return profile_editor_format_mhz(p.absoluteFrequencyHz);
    return p.hasObservedFrequency ? profile_editor_format_mhz(p.observedFrequencyHz) : std::string();
}
static inline bool profile_editor_source_set(ProfileDocument& d, ProfileEditorSourceField field, int index, const char* text) {
    long long hz = 0;
    if (!profile_editor_source_editable(d, field, index) || !profile_editor_parse_mhz(text, hz) ||
        (field == PROFILE_EDIT_POINT && hz <= 0)) return false;
    if (field == PROFILE_EDIT_GRAPHICS) d.graphicsOffset.offsetHz = hz;
    else if (field == PROFILE_EDIT_MEMORY) d.memoryOffsetHz = hz;
    else {
        auto& p = d.points[index];
        p.kind = PROFILE_POINT_ABSOLUTE_FREQUENCY;
        p.absoluteFrequencyHz = hz; p.capturedOffsetHz = 0;
    }
    return true;
}
