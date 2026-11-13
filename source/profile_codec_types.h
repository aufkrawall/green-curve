// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#ifndef GREEN_CURVE_PROFILE_CODEC_TYPES_H
#define GREEN_CURVE_PROFILE_CODEC_TYPES_H
#include <string>
#include <string_view>
#include <memory>
#include <new>
#include <initializer_list>
#include <limits.h>
#include "profile_document.h"

static constexpr size_t PROFILE_FILE_MAX_BYTES = 1024 * 1024;
static constexpr size_t PROFILE_FILE_MAX_LINE = 4096;
static constexpr int PROFILE_BANK_CAPACITY = 5;

struct ProfileBank {
    int count = 0;
    ProfileDocument documents[PROFILE_BANK_CAPACITY]{};
};

struct ProfileCodecError {
    size_t line = 0;
    char message[192]{};
};

namespace profile_codec {
using View = std::string_view;
enum Section {
    HEADER, DOCUMENT, SOURCE, TOPOLOGY, POINT, GRAPHICS, EXCLUSION, MEMORY,
    POWER, POWER_OBSERVATION, LOCK, LOCK_OBSERVATION, FAN, FAN_CURVE,
    FAN_POINT, AUX_XBAR, AUX_SYS, AUX_VIDEO, GRAPHICS_OBSERVATION, SECTION_COUNT
};
struct Seen {
    bool present = false;
    unsigned long long keys = 0;
    size_t line = 0;
};
struct ParsedPoint {
    Seen seen;
    int reference = -1;
    ProfilePointIntent value{};
};
struct ParsedDocument {
    Seen sections[SECTION_COUNT];
    Seen topology[VF_NUM_POINTS], exclusions[VF_NUM_POINTS], fanPoints[FAN_CURVE_MAX_POINTS];
    ParsedPoint points[VF_NUM_POINTS];
    int pointCount = 0;
    int fanPointCount = 0;
};
struct DecodeState {
    ProfileBank bank;
    Seen header;
    ParsedDocument records[PROFILE_BANK_CAPACITY];
    DecodeState() {
        for (auto& d : bank.documents) d = profile_document_make_empty();
    }
};
struct Location { Section section = HEADER; int document = -1, index = -1; };

inline bool fail(ProfileCodecError* error, size_t line, const char* message) {
    if (error) {
        error->line = line;
        snprintf(error->message, sizeof(error->message), "%s", message);
    }
    return false;
}
inline View trim(View v) {
    while (!v.empty() && (v.front() == ' ' || v.front() == '\t')) v.remove_prefix(1);
    while (!v.empty() && (v.back() == ' ' || v.back() == '\t')) v.remove_suffix(1);
    return v;
}
inline bool integer(View v, long long& result) {
    if (v.empty()) return false;
    const bool negative = v.front() == '-';
    if (negative) v.remove_prefix(1);
    if (v.empty()) return false;
    const unsigned long long limit = (unsigned long long)LLONG_MAX + (negative ? 1ull : 0ull);
    unsigned long long value = 0;
    for (const char c : v) {
        if (c < '0' || c > '9' || value > (limit - (unsigned int)(c - '0')) / 10) return false;
        value = value * 10 + (unsigned int)(c - '0');
    }
    result = negative ? (value == (1ull << 63) ? LLONG_MIN : -(long long)value) : (long long)value;
    return true;
}
template <class T> inline bool number(View v, T& out, long long low, long long high) {
    long long value = 0;
    if (!integer(v, value) || value < low || value > high || (low >= 0 && v.front() == '-')) return false;
    out = (T)value;
    return true;
}
template <class T> inline bool boolean(View v, T& out) {
    if (v != "0" && v != "1") return false;
    out = (T)(v == "1"); return true;
}
template <class T> inline bool choice(View v, T& out, std::initializer_list<View> names) {
    int i = 0;
    for (View name : names) { if (v == name) { out = (T)i; return true; } ++i; }
    return false;
}
inline bool text(View v, char* out, size_t capacity) {
    if (v.size() < 2 || v.front() != '"' || v.back() != '"') return false;
    v.remove_prefix(1); v.remove_suffix(1);
    size_t length = 0;
    for (size_t i = 0; i < v.size(); ++i) {
        char c = v[i];
        if (c == '\\') {
            if (++i == v.size() || (v[i] != '\\' && v[i] != '"')) return false;
            c = v[i];
        } else if (c == '"') return false;
        if (length + 1 >= capacity) return false;
        out[length++] = c;
    }
    out[length] = 0;
    return profile_utf8_valid(out, length);
}
inline constexpr View domainNames[] = {"vf", "graphics", "memory", "power", "lock", "fan", "xbar", "sys_clock", "video_clock"};
template <class T> inline bool domains(View v, T& out) {
    int mask = 0;
    if (v == "none") { out = (T)0; return true; }
    for (;;) {
        const size_t comma = v.find(',');
        const View token = trim(v.substr(0, comma));
        int bit = 0;
        for (int i = 0; i < 9; ++i) if (token == domainNames[i]) bit = 1 << i;
        if (!bit || (mask & bit)) return false;
        mask |= bit;
        if (comma == View::npos) break;
        v.remove_prefix(comma + 1);
    }
    out = (T)mask; return true;
}
inline bool ordinal(View v, int& out, int low, int high) {
    return !v.empty() && !(v.size() > 1 && v.front() == '0') && number(v, out, low, high);
}
inline bool location(View name, Location& out) {
    if (name == "green_curve_profile") { out = {}; return true; }
    if (name.substr(0, 8) != "profile.") return false;
    name.remove_prefix(8);
    const size_t dot = name.find('.');
    int n = 0;
    if (!ordinal(name.substr(0, dot), n, 1, PROFILE_BANK_CAPACITY)) return false;
    out.document = n - 1; out.index = -1;
    if (dot == View::npos) { out.section = DOCUMENT; return true; }
    name.remove_prefix(dot + 1);
    const View plain[] = {"source", "graphics", "memory", "power", "power_observation", "lock", "lock_observation", "fan", "fan.curve", "aux.xbar", "aux.sys", "aux.video", "graphics_observation"};
    const Section kinds[] = {SOURCE, GRAPHICS, MEMORY, POWER, POWER_OBSERVATION, LOCK, LOCK_OBSERVATION, FAN, FAN_CURVE, AUX_XBAR, AUX_SYS, AUX_VIDEO, GRAPHICS_OBSERVATION};
    for (int i = 0; i < 13; ++i) if (name == plain[i]) { out.section = kinds[i]; return true; }
    const View prefixes[] = {"topology.", "point.", "exclusion.", "fan.point."};
    const Section indexed[] = {TOPOLOGY, POINT, EXCLUSION, FAN_POINT};
    for (int i = 0; i < 4; ++i) {
        if (name.substr(0, prefixes[i].size()) != prefixes[i]) continue;
        out.section = indexed[i];
        return ordinal(name.substr(prefixes[i].size()), out.index, 0,
            i == 3 ? FAN_CURVE_MAX_POINTS - 1 : VF_NUM_POINTS - 1);
    }
    return false;
}
inline Seen& seen(DecodeState& s, const Location& at) {
    if (at.section == HEADER) return s.header;
    auto& r = s.records[at.document];
    switch (at.section) {
        case TOPOLOGY: return r.topology[at.index];
        case POINT: return r.points[at.index].seen;
        case EXCLUSION: return r.exclusions[at.index];
        case FAN_POINT: return r.fanPoints[at.index];
        default: return r.sections[at.section];
    }
}
} // namespace profile_codec
#endif
