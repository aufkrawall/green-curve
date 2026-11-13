// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#ifndef GREEN_CURVE_PROFILE_SLOT_CODEC_H
#define GREEN_CURVE_PROFILE_SLOT_CODEC_H
#include "profile_codec.h"
#include <vector>

// Local INI storage uses flat keys in the existing slot section. Text alone
// is hex-encoded UTF-8 to survive the Windows INI code page and quote handling;
// tuning values remain ordinary inspectable decimal fields. No GPU projection.
namespace profile_slot {
using profile_codec::View;
inline constexpr size_t MAX_CONFIG_BYTES = 8 * 1024 * 1024;
inline constexpr size_t MAX_SECTION_BYTES = 128 * 1024;
inline std::string hex(View v) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string out;
    for (unsigned char c : v) { out += digits[c >> 4]; out += digits[c & 15]; }
    return out;
}
inline bool unhex(View v, std::string& out) {
    if (v.size() % 2 || v.size() > 1024) return false;
    const auto digit = [](char c) {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    for (size_t i = 0; i < v.size(); i += 2) {
        const int a = digit(v[i]), b = digit(v[i + 1]);
        if (a < 0 || b < 0) return false;
        out += (char)((a << 4) | b);
    }
    return true;
}
inline bool next_line(View& input, View& line) {
    if (input.empty()) return false;
    const size_t end = input.find('\n');
    line = input.substr(0, end);
    if (end == View::npos) input = {}; else input.remove_prefix(end + 1);
    if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
    line = profile_codec::trim(line);
    return true;
}
inline bool ascii_equal(View a, View b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        const auto lower = [](char c) { return c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c; };
        if (lower(a[i]) != lower(b[i])) return false;
    }
    return true;
}
// Reads just the named section, preserving opaque unrelated preferences and
// rejecting duplicate slot sections instead of combining file generations.
inline bool extract_named(View file, View wanted, std::string& body, bool& found, ProfileCodecError* error) {
    found = false;
    if (wanted.empty() || file.size() > MAX_CONFIG_BYTES || file.find('\0') != View::npos)
        return profile_codec::fail(error, 0, "invalid local profile bank");
    if (file.substr(0, 3) == "\xef\xbb\xbf") file.remove_prefix(3);
    std::string result;
    bool selected = false;
    View line;
    while (next_line(file, line)) {
        if (!line.empty() && line.front() == '[') {
            selected = line.back() == ']' && ascii_equal(profile_codec::trim(line.substr(1, line.size() - 2)), wanted);
            if (selected) {
                if (found) return profile_codec::fail(error, 0, "duplicate local profile section");
                found = true;
            }
        } else if (selected) {
            if (line.size() + 1 > MAX_SECTION_BYTES - result.size())
                return profile_codec::fail(error, 0, "local profile section exceeds size limit");
            result.append(line.data(), line.size()); result += '\n';
        }
    }
    body.swap(result);
    return true;
}
inline bool extract(View file, int slot, std::string& body, bool& found, ProfileCodecError* error) {
    if (slot < 1 || slot > PROFILE_BANK_CAPACITY) return profile_codec::fail(error, 0, "invalid local profile slot");
    return extract_named(file, "profile" + std::to_string(slot), body, found, error);
}
} // namespace profile_slot

static inline bool profile_slot_encode(const ProfileDocument& document, int slot,
    std::string& output, ProfileCodecError* error = nullptr) {
    using namespace profile_codec;
    if (error) *error = {};
    if (slot < 1 || slot > PROFILE_BANK_CAPACITY) return fail(error, 0, "invalid destination slot");
    auto bank = std::make_unique<ProfileBank>();
    bank->count = 1; bank->documents[0] = document;
    std::string portable;
    if (!profile_bank_encode(*bank, portable, error)) return false;
    std::string result = "[profile" + std::to_string(slot) + "]\ndocument.format=green_curve_profile_v1\n";
    View input(portable), line;
    std::string prefix;
    while (profile_slot::next_line(input, line)) {
        if (line.empty()) continue;
        if (line.front() == '[') {
            if (line == "[profile.1]") prefix.clear();
            else if (line.substr(0, 11) == "[profile.1.") prefix = std::string(line.substr(11, line.size() - 12)) + ".";
            else prefix = "!header";
            continue;
        }
        if (prefix == "!header") continue;
        const size_t equals = line.find('=');
        const View key = line.substr(0, equals), value = line.substr(equals + 1);
        result += "document." + prefix;
        result.append(key.data(), key.size());
        if (key == "display_name" || key == "driver_version") result += "_hex=" + profile_slot::hex(value);
        else { result += '='; result.append(value.data(), value.size()); }
        result += '\n';
    }
    if (result.size() > profile_slot::MAX_SECTION_BYTES) return fail(error, 0, "local profile section exceeds size limit");
    output.swap(result);
    return true;
}

// found=false distinguishes a recognized legacy slot from a corrupt new record.
// Once ANY document key occurs, invalid/missing format must never fall back to
// legacy repair. Unrelated legacy keys may coexist but cannot supply authority.
static inline bool profile_slot_decode(std::string_view body, ProfileDocument& output,
    bool& found, ProfileCodecError* error = nullptr) {
    using namespace profile_codec;
    if (error) *error = {};
    found = false;
    if (body.size() > profile_slot::MAX_SECTION_BYTES) return fail(error, 0, "local profile section exceeds size limit");
    struct Group { std::string name, fields; };
    std::vector<Group> groups;
    bool format = false;
    size_t lineNumber = 0, fields = 0;
    View line;
    while (profile_slot::next_line(body, line)) {
        ++lineNumber;
        if (line.empty() || line.front() == '#' || line.front() == ';') continue;
        const size_t equals = line.find('=');
        View key = trim(line.substr(0, equals));
        if (!profile_slot::ascii_equal(key.substr(0, 9), "document.")) continue;
        found = true;
        if (key.substr(0, 9) != "document.") return fail(error, lineNumber, "noncanonical local document key");
        if (equals == View::npos || line.size() > PROFILE_FILE_MAX_LINE || ++fields > 4096)
            return fail(error, lineNumber, "invalid local document field");
        key.remove_prefix(9);
        View value = trim(line.substr(equals + 1));
        if (key == "format") {
            if (format || value != "green_curve_profile_v1") return fail(error, lineNumber, "invalid or duplicate local format");
            format = true; continue;
        }
        const size_t dot = key.rfind('.');
        const View group = dot == View::npos ? View{} : key.substr(0, dot);
        View leaf = dot == View::npos ? key : key.substr(dot + 1);
        std::string decoded;
        if (leaf == "display_name_hex" || leaf == "driver_version_hex") {
            if (!profile_slot::unhex(value, decoded)) return fail(error, lineNumber, "invalid UTF-8 text encoding");
            leaf.remove_suffix(4); value = decoded;
        } else if (leaf == "display_name" || leaf == "driver_version") {
            return fail(error, lineNumber, "local text must use UTF-8 encoding");
        }
        size_t index = 0;
        while (index < groups.size() && groups[index].name != group) ++index;
        if (index == groups.size()) {
            if (groups.size() >= 416) return fail(error, lineNumber, "too many local document groups");
            groups.push_back({std::string(group), {}});
        }
        groups[index].fields.append(leaf.data(), leaf.size()); groups[index].fields += '=';
        groups[index].fields.append(value.data(), value.size()); groups[index].fields += '\n';
    }
    if (!found) return true;
    if (!format) return fail(error, 0, "missing local document format");
    std::string portable = "[green_curve_profile]\nformat=green_curve_profiles\nmajor_version=1\nprofile_count=1\n";
    for (const auto& g : groups) {
        portable += "\n[profile.1";
        if (!g.name.empty()) portable += "." + g.name;
        portable += "]\n" + g.fields;
    }
    auto bank = std::make_unique<ProfileBank>();
    if (!profile_bank_decode(portable, *bank, error)) return false;
    output = bank->documents[0];
    return true;
}
#endif
