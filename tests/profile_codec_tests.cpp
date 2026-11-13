// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#include "profile_codec_fixture.h"
#include <vector>

static std::string replacing(std::string text, const char* from, const char* to) {
    const auto p = text.find(from);
    if (p == std::string::npos) return "invalid test substitution";
    text.replace(p, strlen(from), to);
    return text;
}

int run_profile_codec_tests() {
    auto source = std::make_unique<ProfileBank>();
    auto decoded = std::make_unique<ProfileBank>();
    source->count = 1; source->documents[0] = profile_codec_fixture();
    std::string canonical;
    ProfileCodecError error;
    if (!profile_bank_encode(*source, canonical, &error)) return 8340;
    if (canonical.find("numerator=249\ndenominator=250") == std::string::npos ||
        canonical.find("offset_hz=-9223372036854775808") == std::string::npos ||
        canonical.find("target_hz=9223372036854775807") == std::string::npos) return 8341;
    if (!profile_bank_decode(canonical, *decoded, &error) ||
        !profile_document_semantically_equal(source->documents[0], decoded->documents[0])) return 8342;
    const auto& d = decoded->documents[0];
    if (strcmp(d.displayName, source->documents[0].displayName) || d.source.deviceId != UINT_MAX ||
        strcmp(d.source.driverVersion, "1.2.3") || !d.source.captureComplete ||
        d.source.omittedDomains != PROFILE_DOMAIN_FAN || d.source.unavailableDomains != PROFILE_DOMAIN_LOCK ||
        !d.points[0].hasObservedOffset || d.points[0].observedOffsetHz != -500 ||
        !d.points[0].hasObservedFrequency || d.points[0].observedFrequencyHz != 1700000500 ||
        !d.power.hasObservedDefaultMw || d.power.observedDefaultMw != 250000 ||
        !d.power.hasObservedCurrentMw || d.power.observedCurrentMw != 249000 ||
        !d.lock.hasRecognitionEvidence || d.lock.recognizedTailCount != 3 ||
        d.lock.recognitionToleranceHz != 1000000 || !d.lock.hasObservedPinRange ||
        d.lock.observedPinMaximumHz != 1800000000 || d.topology[2].sourceWritable != 2 ||
        d.fan.fanCurve.points[7].temperatureC != 98) return 8343;
    std::string again;
    if (!profile_bank_encode(*decoded, again) || canonical != again) return 8344;
    // Section and key order are irrelevant, including child sections before parents.
    std::vector<std::string> sections;
    for (size_t pos = 0; pos < canonical.size();) {
        const size_t end = canonical.find("\n[", pos + 1);
        sections.push_back(canonical.substr(pos, end == std::string::npos ? end : end - pos));
        if (end == std::string::npos) break;
        pos = end + 1;
    }
    std::string reversed;
    for (size_t i = sections.size(); i > 0; --i) {
        const std::string& s = sections[i - 1];
        const size_t firstLine = s.find('\n');
        reversed += s.substr(0, firstLine + 1);
        std::vector<std::string> keys;
        size_t p = firstLine + 1;
        while (p < s.size()) {
            const size_t end = s.find('\n', p);
            keys.push_back(s.substr(p, end == std::string::npos ? end : end - p));
            if (end == std::string::npos) break;
            p = end + 1;
        }
        for (size_t j = keys.size(); j > 0; --j) reversed += keys[j - 1] + "\n";
    }
    if (!profile_bank_decode(reversed, *decoded) || !profile_bank_encode(*decoded, again) || again != canonical) return 8345;
    std::string crlf = "\xef\xbb\xbf; comment\r\n";
    for (char c : canonical) { if (c == '\n') crlf += '\r'; crlf += c; }
    if (!profile_bank_decode(crlf, *decoded)) return 8346;
    const auto reject = [&](const std::string& input) {
        decoded->count = 1; decoded->documents[0] = profile_document_make_empty();
        strcpy(decoded->documents[0].displayName, "unchanged");
        return !profile_bank_decode(input, *decoded, &error) && error.message[0] &&
            decoded->count == 1 && !strcmp(decoded->documents[0].displayName, "unchanged");
    };
    const char* invalid[][2] = {
        {"major_version=1", "major_version=2"}, {"profile_count=1", "profile_count=0"},
        {"document_id=1", "document_id=0"}, {"document_id=1", "document_id=4294967296"},
        {"document_id=1", "document_id=+1"}, {"domains=vf,graphics", "domains=vf,vf,graphics"},
        {"[profile.1]", "[profile.01]"}, {"[profile.1]", "[profile.6]"},
        {"point_count=4", "point_count=3"}, {"topology_count=4", "topology_count=5"},
        {"kind=absent", "kind=absent\noffset_hz=0"}, {"kind=projection", "kind=projection\ntarget_hz=1"},
        {"target_hz=9223372036854775807", "target_hz=9223372036854775808"},
        {"offset_hz=-9223372036854775808", "offset_hz=-9223372036854775809"},
        {"kind=driver_offset", "kind=driver_offset\ntarget_hz=1"},
        {"surface=global", "surface=guessed"}, {"numerator=249", "numerator=0"},
        {"denominator=250", "denominator=2.5"}, {"denominator=250", "denominator=250 # comment"},
        {"recognition_present=1", "recognition_present=0"}, {"pin_range_present=1", "pin_range_present=0"},
        {"curve_present=1", "curve_present=0"}, {"poll_interval_ms=1000", "poll_interval_ms=1001"},
        {"zero_rpm_enabled=0", "zero_rpm_enabled=2"}, {"temperature_c=98", "temperature_c=101"},
        {"exclusion_policy=identities", "exclusion_policy=none"},
        {"excluded_count=4", "excluded_count=4\nexclude_low_count=0"},
        {"display_name=", "unknown_key="}, {"[profile.1.source]", "[profile.1.unknown]"},
        {"document_id=1", "document_id=1\ndocument_id=1"},
        {"[profile.1.source]", "[profile.1.source]\n[profile.1.source]"},
        {"topology_complete=1", "topology_complete=1\nlegacy_indices_unresolved=1"},
        {"source_index=0", "source_index=128"}, {"source_index=0", "source_index=-2"},
        {"topology_id=1001", "topology_id=1000"}, {"anchor_topology_id=1001", "anchor_topology_id=9999"},
        {"voltage_uv=600000", "voltage_uv=0"}, {"track_anchor=1", "track_anchor=true"},
        {"driver_version=\"1.2.3\"", "driver_version=\"bad\\q\""},
        {"backend_family=1", "backend_family=-1"}, {"device_id=4294967295", "device_id=-0"},
        {"[profile.1.fan.point.7]", "[profile.1.fan.point.8]"},
        {"observed_frequency_hz=1700000500", "observed_frequency_hz=0"},
    };
    for (const auto& pair : invalid) if (!reject(replacing(canonical, pair[0], pair[1]))) {
        fprintf(stderr, "codec accepted malformed fixture replacing %s\n", pair[0]);
        return 8347;
    }
    if (!reject(canonical + "\n[profile.2.aux.sys]\noffset_hz=0\n") ||
        !reject(std::string(PROFILE_FILE_MAX_BYTES + 1, ' ')) ||
        !reject(canonical + "\n#" + std::string(PROFILE_FILE_MAX_LINE, 'x')) ||
        !reject(canonical + std::string("\0", 1)) || !reject(canonical + "\r") ||
        !reject(canonical + "\xc0\xaf") || !reject(canonical + "\xed\xa0\x80")) return 8348;
    source->count = PROFILE_BANK_CAPACITY;
    for (int i = 0; i < source->count; ++i) { source->documents[i] = profile_codec_fixture(VF_NUM_POINTS); source->documents[i].documentId = (gc_u32)i + 1; }
    if (!profile_bank_encode(*source, again) || !profile_bank_decode(again, *decoded) || decoded->count != 5) return 8349;
    // Max-width numeric payloads, escaped strings and every indexed record.
    source->count = 1;
    auto& widest = source->documents[0];
    memset(widest.displayName, '\\', PROFILE_DOCUMENT_MAX_DISPLAY_NAME);
    widest.displayName[PROFILE_DOCUMENT_MAX_DISPLAY_NAME] = 0;
    memset(widest.source.driverVersion, '\\', 64); widest.source.driverVersion[64] = 0;
    widest.source.backendFamily = INT_MAX; widest.source.subSystemId = UINT_MAX;
    for (int i = 0; i < VF_NUM_POINTS; ++i) {
        widest.topology[i] = {INT_MAX - i, 127, LLONG_MAX, 2};
        widest.points[i].kind = PROFILE_POINT_CAPTURED_OFFSET;
        widest.points[i].absoluteFrequencyHz = 0;
        widest.points[i].capturedOffsetHz = LLONG_MIN;
        widest.points[i].observedFrequencyHz = LLONG_MAX;
        widest.points[i].observedOffsetHz = LLONG_MIN;
        widest.graphicsOffset.excludedTopologyIds[i] = INT_MAX - i;
    }
    widest.lock.anchorTopologyId = INT_MAX;
    widest.lock.recognizedAnchorVoltageUv = widest.lock.recognitionToleranceHz = LLONG_MAX;
    widest.lock.observedPinMinimumHz = widest.lock.observedPinMaximumHz = LLONG_MAX;
    widest.lock.targetFrequencyHz = LLONG_MAX;
    widest.power.ratio = {LLONG_MAX, LLONG_MAX - 1};
    widest.power.observedCurrentMw = widest.power.observedDefaultMw = LLONG_MAX;
    widest.memoryOffsetHz = LLONG_MIN;
    widest.sysClkOffsetHz = widest.videoClkOffsetHz = LLONG_MIN;
    if (!profile_bank_encode(*source, again) || again.size() >= 65536) return 8350;
    source->documents[0] = profile_document_make_empty();
    source->documents[0].power.hasObservedDefaultMw = true; source->documents[0].power.observedDefaultMw = 250000;
    if (!profile_bank_encode(*source, again) || !profile_bank_decode(again, *decoded) ||
        decoded->documents[0].power.present || !decoded->documents[0].power.hasObservedDefaultMw) return 8351;
    source->count = 0;
    if (!profile_bank_encode(*source, again) || !profile_bank_decode(again, *decoded) || decoded->count) return 8352;
    source->count = 2; source->documents[1] = source->documents[0];
    again = "unchanged";
    if (profile_bank_encode(*source, again, &error) || again != "unchanged") return 8353;
    // Presence survives explicit zero, and legacy evidence never gains destination voltages.
    source->count = 1; auto& legacy = source->documents[0];
    legacy = profile_document_make_empty(); legacy.domains = PROFILE_DOMAIN_VF_CURVE;
    legacy.legacyIndicesUnresolved = true; legacy.topologyCount = 1; legacy.topology[0] = {7, 25, 0, 0};
    legacy.points[0].kind = PROFILE_POINT_CAPTURED_OFFSET;
    if (!profile_bank_encode(*source, again) || !profile_bank_decode(again, *decoded) ||
        decoded->documents[0].points[0].kind != PROFILE_POINT_CAPTURED_OFFSET ||
        decoded->documents[0].topology[0].voltageUv != 0) return 8354;
    const std::string minimal = "[green_curve_profile]\nformat=green_curve_profiles\nmajor_version=0001\nprofile_count=0\n";
    if (!profile_bank_decode(minimal, *decoded)) return 8355;
    std::string atLimit = minimal;
    while (atLimit.size() < PROFILE_FILE_MAX_BYTES) {
        const size_t remaining = PROFILE_FILE_MAX_BYTES - atLimit.size();
        if (remaining == 1) { atLimit += '\n'; break; }
        const size_t take = remaining > PROFILE_FILE_MAX_LINE ? PROFILE_FILE_MAX_LINE : remaining - 1;
        atLimit += '#'; atLimit.append(take - 1, 'x'); atLimit += '\n';
    }
    if (atLimit.size() != PROFILE_FILE_MAX_BYTES || !profile_bank_decode(atLimit, *decoded)) return 8356;
    fprintf(stdout, "portable profile codec: full-bank, exact-value, observation and malformed-input checks passed\n");
    return 0;
}
