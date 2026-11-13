// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#pragma once
#include "profile_slot_codec.h"

// Raw configuration snapshots keep their encoding and bytes for conflict
// checks. Only the parsing view is decoded; unrelated text is never repaired.
namespace profile_config {
using profile_codec::View;
inline bool fail(ProfileCodecError* e, const char* reason) { return profile_codec::fail(e, 0, reason); }
inline bool decode(View input, std::string& output, ProfileCodecError* e = nullptr) {
    if (input.size() > profile_slot::MAX_CONFIG_BYTES) return fail(e, "configuration exceeds size limit");
    if (input.substr(0, 2) == "\xfe\xff") return fail(e, "unsupported configuration text encoding");
    if (input.substr(0, 2) != "\xff\xfe") {
        if (input.find('\0') != View::npos) return fail(e, "unsupported configuration text encoding");
        output.assign(input.data(), input.size()); return true;
    }
    if (input.size() % 2) return fail(e, "truncated configuration UTF-16 input");
    const auto unit = [&](size_t at) {
        return (unsigned)(unsigned char)input[at] | ((unsigned)(unsigned char)input[at + 1] << 8);
    };
    std::string result;
    for (size_t at = 2; at < input.size(); at += 2) {
        unsigned value = unit(at);
        if (!value) return fail(e, "NUL in configuration text");
        if (value >= 0xd800 && value <= 0xdbff) {
            if (at + 3 >= input.size()) return fail(e, "truncated configuration UTF-16 pair");
            const unsigned tail = unit(at + 2);
            if (tail < 0xdc00 || tail > 0xdfff) return fail(e, "invalid configuration UTF-16 pair");
            value = 0x10000 + ((value - 0xd800) << 10) + tail - 0xdc00; at += 2;
        } else if (value >= 0xdc00 && value <= 0xdfff) return fail(e, "unpaired configuration UTF-16 unit");
        if (value < 0x80) result += (char)value;
        else if (value < 0x800) {
            result += (char)(0xc0 | (value >> 6)); result += (char)(0x80 | (value & 63));
        } else if (value < 0x10000) {
            result += (char)(0xe0 | (value >> 12)); result += (char)(0x80 | ((value >> 6) & 63)); result += (char)(0x80 | (value & 63));
        } else {
            result += (char)(0xf0 | (value >> 18)); result += (char)(0x80 | ((value >> 12) & 63));
            result += (char)(0x80 | ((value >> 6) & 63)); result += (char)(0x80 | (value & 63));
        }
        if (result.size() > profile_slot::MAX_CONFIG_BYTES) return fail(e, "decoded configuration exceeds size limit");
    }
    output.swap(result); return true;
}

inline bool encode_like(View original, View text, std::string& output, ProfileCodecError* e = nullptr) {
    if (original.size() > profile_slot::MAX_CONFIG_BYTES || text.size() > profile_slot::MAX_CONFIG_BYTES ||
        text.find('\0') != View::npos) return fail(e, "invalid or excessive configuration text");
    if (original.substr(0, 2) != "\xff\xfe") { output.assign(text.data(), text.size()); return true; }
    std::string result("\xff\xfe", 2);
    const auto unit = [&](unsigned value) { result += (char)(value & 255); result += (char)(value >> 8); };
    for (size_t at = 0; at < text.size();) {
        const unsigned first = (unsigned char)text[at++];
        unsigned value = first, minimum = 0; size_t extra = 0;
        if (first >= 0xc2 && first <= 0xdf) { value = first & 31; minimum = 0x80; extra = 1; }
        else if (first >= 0xe0 && first <= 0xef) { value = first & 15; minimum = 0x800; extra = 2; }
        else if (first >= 0xf0 && first <= 0xf4) { value = first & 7; minimum = 0x10000; extra = 3; }
        else if (first >= 0x80) return fail(e, "invalid UTF-8 in configuration edit");
        if (extra > text.size() - at) return fail(e, "truncated UTF-8 in configuration edit");
        for (size_t n = 0; n < extra; ++n) {
            const unsigned next = (unsigned char)text[at++];
            if ((next & 0xc0) != 0x80) return fail(e, "invalid UTF-8 in configuration edit");
            value = (value << 6) | (next & 63);
        }
        if (value < minimum || value > 0x10ffff || (value >= 0xd800 && value <= 0xdfff))
            return fail(e, "invalid Unicode in configuration edit");
        if (value < 0x10000) unit(value);
        else { value -= 0x10000; unit(0xd800 | (value >> 10)); unit(0xdc00 | (value & 1023)); }
        if (result.size() > profile_slot::MAX_CONFIG_BYTES) return fail(e, "encoded configuration exceeds size limit");
    }
    output.swap(result); return true;
}
} // namespace profile_config
