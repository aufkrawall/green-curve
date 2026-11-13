// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#include "profile_conversion_batch.h"
#include <memory>

int run_profile_conversion_tests() {
    auto source = std::make_unique<ProfileDocument>(profile_document_make_empty());
    source->domains = PROFILE_DOMAIN_VF_CURVE;
    source->topologyCount = VF_NUM_POINTS; source->sourceTopologyComplete = true;
    source->source.captureComplete = true;
    for (int i = 0; i < VF_NUM_POINTS; ++i) {
        source->topology[i] = {1000 + i, i, 600000 + (long long)i * 6250, 1};
        source->points[i].kind = PROFILE_POINT_CAPTURED_OFFSET;
        source->points[i].capturedOffsetHz = i % 2 ? -15000500 : 15000500;
        source->points[i].hasObservedFrequency = true;
        source->points[i].observedFrequencyHz = 1700000500;
    }
    source->points[17].capturedOffsetHz = -15000001;
    ProfileConversionSurface surface{PROFILE_CONVERT_VF_OFFSETS, 3, 5, 8, {true, 1000, 0}, 0};
    auto batch = std::make_unique<ProfileConversionBatch>();
    auto derived = std::make_unique<ProfileDocument>();
    char why[192]{};
    int plans = 0;
    const auto valid = [&](const ProfileDocument& d, char*, size_t) {
        ++plans;
        for (int i = 0; i < d.topologyCount; ++i)
            if (d.points[i].capturedOffsetHz > source->points[i].capturedOffsetHz) return false;
        return true;
    };
    if (!profile_conversion_prepare(*source, 11, surface, valid, *batch, why, sizeof(why)) ||
        batch->affectedCount != VF_NUM_POINTS || batch->maximumError != 999 || batch->firstTopologyId != 1000 ||
        batch->worstTopologyId != 1017 || plans != 1 || source->points[1].capturedOffsetHz != -15000500) return 8460;
    if (!profile_conversion_accept(*batch, *source, 11, surface, valid, *derived, why, sizeof(why)) ||
        derived->points[0].capturedOffsetHz != 15000000 || derived->points[1].capturedOffsetHz != -15001000 ||
        derived->points[17].capturedOffsetHz != -15001000 || derived->source.captureComplete ||
        !source->source.captureComplete || derived->points[0].observedFrequencyHz != 1700000500 || plans != 2) return 8461;
    const auto originalDerived = *derived;
    if (profile_conversion_accept(*batch, *source, 12, surface, valid, *derived, why, sizeof(why)) ||
        !profile_document_semantically_equal(*derived, originalDerived)) return 8462;
    auto changed = surface; changed.grid.step = 2000;
    if (profile_conversion_accept(*batch, *source, 11, changed, valid, *derived, why, sizeof(why))) return 8463;
    changed = surface; changed.identity++;
    if (profile_conversion_accept(*batch, *source, 11, changed, valid, *derived, why, sizeof(why))) return 8464;
    source->points[0].capturedOffsetHz++;
    if (profile_conversion_accept(*batch, *source, 11, surface, valid, *derived, why, sizeof(why))) return 8465;
    source->points[0].capturedOffsetHz--;
    strcpy(source->displayName, "Renamed after review");
    source->points[0].observedFrequencyHz++;
    if (!profile_conversion_accept(*batch, *source, 11, surface, valid, *derived, why, sizeof(why)) ||
        strcmp(derived->displayName, source->displayName) ||
        derived->points[0].observedFrequencyHz != source->points[0].observedFrequencyHz) return 8466;
    batch->affectedCount = 7;
    const auto refused = [](const ProfileDocument&, char* message, size_t capacity) {
        snprintf(message, capacity, "%s", "converted curve violates destination constraints"); return false;
    };
    if (profile_conversion_prepare(*source, 11, surface, refused, *batch, why, sizeof(why)) ||
        batch->affectedCount != 7 || !strstr(why, "destination constraints")) return 8467;
    changed = surface; changed.grid.known = false;
    if (profile_conversion_prepare(*source, 11, changed, valid, *batch, why, sizeof(why))) return 8468;
    changed = surface; changed.kind = PROFILE_CONVERT_XBAR_VOLTAGE;
    if (profile_conversion_prepare(*source, 11, changed, valid, *batch, why, sizeof(why))) return 8469;
    // Source edits in another included domain invalidate the entire conversion.
    if (!profile_conversion_prepare(*source, 11, surface, valid, *batch, why, sizeof(why))) return 8470;
    source->hasMemoryOffset = true; source->domains |= PROFILE_DOMAIN_MEMORY; source->memoryOffsetHz = 500000;
    if (profile_conversion_accept(*batch, *source, 11, surface, valid, *derived, why, sizeof(why))) return 8471;
    // A fresh default produces a new exact ratio; changing it invalidates review.
    *source = profile_document_make_empty(); source->domains = PROFILE_DOMAIN_POWER;
    source->power.present = true; source->power.ratio = {1001, 1000};
    surface = {PROFILE_CONVERT_POWER, 9, 5, 8, {true, 1000, 0}, 250000};
    const auto noCurve = [](const ProfileDocument&, char*, size_t) { return true; };
    if (!profile_conversion_prepare(*source, 20, surface, noCurve, *batch, why, sizeof(why)) ||
        batch->affectedCount != 1 || batch->maximumError != 250 ||
        !profile_conversion_accept(*batch, *source, 20, surface, noCurve, *derived, why, sizeof(why)) ||
        !tuning_power_ratio_equal(derived->power.ratio, {1, 1}) ||
        !tuning_power_ratio_equal(source->power.ratio, {1001, 1000})) return 8472;
    changed = surface; changed.powerDefaultMw++;
    if (profile_conversion_accept(*batch, *source, 20, changed, noCurve, *derived, why, sizeof(why))) return 8473;
    char summary[256]{};
    profile_conversion_summary(*batch, PROFILE_CONVERSION_ACCEPTED, summary, sizeof(summary));
    if (!strstr(summary, "profile-map: domain=power") || !strstr(summary, "count=1 max_error=250 unit=mW decision=accepted")) return 8474;
    return 0;
}
