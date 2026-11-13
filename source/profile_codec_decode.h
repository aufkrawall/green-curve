// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#ifndef GREEN_CURVE_PROFILE_CODEC_DECODE_H
#define GREEN_CURVE_PROFILE_CODEC_DECODE_H
#include "profile_codec_fields.h"

namespace profile_codec {
inline bool required(const Seen& s, unsigned long long mask) {
    return s.present && (s.keys & mask) == mask;
}
inline bool finish_document(DecodeState& state, int n, ProfileCodecError* error) {
    auto& d = state.bank.documents[n];
    auto& r = state.records[n];
    const auto reject = [&](const char* message) { return fail(error, r.sections[DOCUMENT].line, message); };
    if (!required(r.sections[DOCUMENT], 255)) return reject("missing required profile fields");
    bool referenced[VF_NUM_POINTS]{};
    for (int i = 0; i < VF_NUM_POINTS; ++i) {
        if (r.topology[i].present != (i < d.topologyCount) ||
            (i < d.topologyCount && !required(r.topology[i], 7)))
            return reject("topology count or required fields do not match records");
        const auto& p = r.points[i];
        if (p.seen.present != (i < r.pointCount)) return reject("point count does not match records");
        if (i < r.pointCount) {
            if (!required(p.seen, 3)) return reject("missing point reference or kind");
            const unsigned long long payload = p.value.kind == PROFILE_POINT_ABSOLUTE_FREQUENCY ? 4 :
                p.value.kind == PROFILE_POINT_CAPTURED_OFFSET ? 8 : 0;
            if ((p.seen.keys & 12) != payload) return reject("missing or inapplicable point payload");
            const int index = profile_topology_index(d, p.reference);
            if (index < 0 || referenced[index]) return reject("dangling or duplicate point reference");
            referenced[index] = true;
            d.points[index] = p.value;
            d.points[index].hasObservedFrequency = (p.seen.keys & 16) != 0;
            d.points[index].hasObservedOffset = (p.seen.keys & 32) != 0;
        }
    }
    const auto section = [&](Section kind, unsigned long long mask) {
        return !r.sections[kind].present || required(r.sections[kind], mask);
    };
    d.graphicsOffset.present = r.sections[GRAPHICS].present;
    if (d.graphicsOffset.present) {
        const auto policy = d.graphicsOffset.exclusionPolicy;
        const unsigned long long mask = policy == PROFILE_EXCLUDE_IDENTITIES ? 7 :
            policy == PROFILE_EXCLUDE_LEGACY_COUNT ? 11 : 3;
        if (r.sections[GRAPHICS].keys != mask) return reject("missing or inapplicable graphics fields");
    }
    const int excluded = d.graphicsOffset.present && d.graphicsOffset.exclusionPolicy == PROFILE_EXCLUDE_IDENTITIES ?
        d.graphicsOffset.excludedCount : 0;
    for (int i = 0; i < VF_NUM_POINTS; ++i)
        if (r.exclusions[i].present != (i < excluded) ||
            (i < excluded && !required(r.exclusions[i], 1))) return reject("graphics exclusion records do not match policy");
    d.hasMemoryOffset = r.sections[MEMORY].present;
    d.graphicsOffset.hasObservedOffset = r.sections[GRAPHICS_OBSERVATION].present;
    d.power.present = r.sections[POWER].present;
    d.lock.present = r.sections[LOCK].present;
    d.fan.present = r.sections[FAN].present;
    if (!section(GRAPHICS_OBSERVATION, 7) || !section(MEMORY, 1) || !section(POWER, 7) || !section(LOCK, 15) || !section(FAN, 7) ||
        !section(AUX_SYS, 1) || !section(AUX_VIDEO, 1)) return reject("missing required control fields");
    const auto powerKeys = r.sections[POWER_OBSERVATION].keys;
    if (r.sections[POWER_OBSERVATION].present && !powerKeys) return reject("empty power observation");
    d.power.hasObservedDefaultMw = (powerKeys & 1) != 0;
    d.power.hasObservedCurrentMw = (powerKeys & 2) != 0;
    if (r.sections[LOCK_OBSERVATION].present) {
        const auto keys = r.sections[LOCK_OBSERVATION].keys;
        if (!required(r.sections[LOCK_OBSERVATION], 34) ||
            (keys & 28) != (d.lock.hasRecognitionEvidence ? 28ull : 0ull) ||
            (keys & 192) != (d.lock.hasObservedPinRange ? 192ull : 0ull))
            return reject("missing or inapplicable lock observations");
    }
    const bool curve = d.fan.present && d.fan.hasCurveConfig;
    if (r.sections[FAN_CURVE].present != curve || (curve && !required(r.sections[FAN_CURVE], 31)))
        return reject("fan curve configuration does not match presence");
    for (int i = 0; i < FAN_CURVE_MAX_POINTS; ++i)
        if (r.fanPoints[i].present != curve || (curve && !required(r.fanPoints[i], 7)))
            return reject("fan curve requires all complete point records");
    d.fan.fanAuto = d.fan.fanMode == FAN_MODE_AUTO;
    const auto xbarKeys = r.sections[AUX_XBAR].keys;
    if (r.sections[AUX_XBAR].present && !xbarKeys) return reject("empty auxiliary control");
    d.hasXbarOffsetHz = (xbarKeys & 1) != 0;
    d.hasXbarMsvddOffsetUv = (xbarKeys & 2) != 0;
    d.hasSysClkOffsetHz = r.sections[AUX_SYS].present;
    d.hasVideoClkOffsetHz = r.sections[AUX_VIDEO].present;
    char why[160]{};
    if (!profile_document_structurally_valid(d, why, sizeof(why))) return reject(why);
    return true;
}
inline bool finish(DecodeState& state, ProfileCodecError* error) {
    if (!required(state.header, 7)) return fail(error, state.header.line, "missing required file header");
    for (int n = 0; n < PROFILE_BANK_CAPACITY; ++n) {
        if (n < state.bank.count) {
            if (!finish_document(state, n, error)) return false;
            for (int j = 0; j < n; ++j)
                if (state.bank.documents[j].documentId == state.bank.documents[n].documentId)
                    return fail(error, state.records[n].sections[DOCUMENT].line, "duplicate document identity");
        } else {
            const auto& r = state.records[n];
            for (const auto& s : r.sections) if (s.present) return fail(error, s.line, "profile exceeds declared count");
            for (int i = 0; i < VF_NUM_POINTS; ++i) {
                const Seen* records[] = {&r.topology[i], &r.exclusions[i], &r.points[i].seen};
                for (const Seen* s : records) if (s->present) return fail(error, s->line, "profile exceeds declared count");
            }
            for (const auto& s : r.fanPoints) if (s.present) return fail(error, s.line, "profile exceeds declared count");
        }
    }
    return true;
}
} // namespace profile_codec

// Strong output guarantee: failure never publishes a partial document or bank.
static inline bool profile_bank_decode(std::string_view input, ProfileBank& output,
    ProfileCodecError* error = nullptr) {
    using namespace profile_codec;
    if (error) *error = {};
    if (input.size() > PROFILE_FILE_MAX_BYTES) return fail(error, 0, "profile file exceeds size limit");
    if (!profile_utf8_valid(input.data(), input.size(), true)) return fail(error, 0, "invalid UTF-8 or control character");
    if (input.substr(0, 3) == "\xef\xbb\xbf") input.remove_prefix(3);
    auto state = std::unique_ptr<DecodeState>(new (std::nothrow) DecodeState{});
    if (!state) return fail(error, 0, "profile allocation failed");
    Location at;
    bool haveSection = false;
    size_t lineNumber = 0;
    while (!input.empty()) {
        ++lineNumber;
        const size_t end = input.find('\n');
        View line = input.substr(0, end);
        if (end != View::npos && !line.empty() && line.back() == '\r') line.remove_suffix(1);
        if (line.size() > PROFILE_FILE_MAX_LINE || line.find('\r') != View::npos)
            return fail(error, lineNumber, "overlong line or isolated carriage return");
        if (end == View::npos) input = {}; else input.remove_prefix(end + 1);
        line = trim(line);
        if (line.empty() || line.front() == '#' || line.front() == ';') continue;
        if (line.front() == '[') {
            if (line.back() != ']' || !location(trim(line.substr(1, line.size() - 2)), at))
                return fail(error, lineNumber, "unknown or invalid section");
            Seen& s = seen(*state, at);
            if (s.present) return fail(error, lineNumber, "duplicate section");
            s.present = true; s.line = lineNumber; haveSection = true;
        } else {
            const size_t equals = line.find('=');
            if (!haveSection || equals == View::npos ||
                !field(*state, at, trim(line.substr(0, equals)), trim(line.substr(equals + 1))))
                return fail(error, lineNumber, "unknown, duplicate or invalid field");
        }
    }
    if (!finish(*state, error)) return false;
    output = state->bank;
    return true;
}
#endif
