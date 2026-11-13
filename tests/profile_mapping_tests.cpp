// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#include "profile_mapping.h"
#include "profile_conversion_grid.h"

static ProfileDocument mapping_source() {
    auto d = profile_document_make_empty();
    d.domains = PROFILE_DOMAIN_VF_CURVE | PROFILE_DOMAIN_LOCK;
    d.topologyCount = 2;
    d.topology[0] = {17, 99, 600001, 0}; d.topology[1] = {18, 100, 700001, 0};
    d.points[0].kind = PROFILE_POINT_CAPTURED_OFFSET; d.points[0].capturedOffsetHz = 0;
    d.points[1].kind = PROFILE_POINT_ABSOLUTE_FREQUENCY; d.points[1].absoluteFrequencyHz = 1700000500;
    d.lock.present = true; d.lock.mode = LOCK_MODE_FLATTEN;
    d.lock.anchorTopologyId = 18; d.lock.targetFrequencyHz = 1700000500;
    return d;
}
static ProfileDestinationTopology mapping_destination() {
    ProfileDestinationTopology d;
    d.available = d.complete = true; d.gpuGeneration = 5; d.revision = 8;
    d.count = 3;
    d.points[0] = {2, 600001, true, true};
    d.points[1] = {5, 700001, true, true};
    d.points[2] = {9, 800001, true, true};
    return d;
}
static bool has_issue(const ProfileMappingReport& report, ProfileMappingIssueKind kind) {
    for (int i = 0; i < report.issueCount; ++i) if (report.issues[i].kind == kind) return true;
    return false;
}

int run_profile_mapping_tests() {
    const auto original = mapping_source();
    auto source = original;
    auto destination = mapping_destination();
    auto report = profile_map_topology(source, destination, 11);
    if (!report.ready() || report.destinationByTopology[0] != 2 || report.destinationByTopology[1] != 5 ||
        report.completeTopologyMatch || !report.needsCompleteCurvePlan || report.sourceRevision != 11) return 8420;
    source.source.deviceId = UINT_MAX; source.source.osFamily = PROFILE_SOURCE_OS_LINUX;
    if (!profile_map_topology(source, destination).ready()) return 8421; // metadata cannot select the destination
    destination.points[0].voltageUv = 600000;
    report = profile_map_topology(source, destination);
    if (!has_issue(report, PROFILE_MAP_MISSING_VOLTAGE) || report.destinationByTopology[0] != -1) return 8422;
    destination = mapping_destination(); destination.points[2].voltageUv = 600001;
    if (!has_issue(profile_map_topology(source, destination), PROFILE_MAP_AMBIGUOUS_VOLTAGE)) return 8423;
    destination = mapping_destination(); destination.points[0].graphics = false;
    if (!has_issue(profile_map_topology(source, destination), PROFILE_MAP_MISSING_VOLTAGE)) return 8424;
    destination = mapping_destination(); destination.points[1].writable = false;
    if (!has_issue(profile_map_topology(source, destination), PROFILE_MAP_READ_ONLY_POINT)) return 8425;
    destination = mapping_destination(); destination.complete = false;
    if (!has_issue(profile_map_topology(source, destination), PROFILE_MAP_INCOMPLETE_TOPOLOGY)) return 8426;
    destination.available = false;
    if (!has_issue(profile_map_topology(source, destination), PROFILE_MAP_OFFLINE)) return 8427;
    destination = mapping_destination(); destination.points[1].index = 2;
    if (!has_issue(profile_map_topology(source, destination), PROFILE_MAP_INVALID_TOPOLOGY)) return 8428;
    destination = mapping_destination(); destination.knownUnavailableDomains = PROFILE_DOMAIN_LOCK;
    if (!has_issue(profile_map_topology(source, destination), PROFILE_MAP_UNAVAILABLE_DOMAIN)) return 8429;
    destination = mapping_destination(); destination.count = 2; destination.points[1].voltageUv = 600001;
    source = original; source.topology[1].voltageUv = 600001;
    source.topology[0].sourceIndex = 2; source.topology[1].sourceIndex = 5; source.sourceTopologyComplete = true;
    report = profile_map_topology(source, destination);
    if (!report.ready() || !report.completeTopologyMatch || report.destinationByTopology[1] != 5) return 8430;
    source.sourceTopologyComplete = false;
    if (!has_issue(profile_map_topology(source, destination), PROFILE_MAP_AMBIGUOUS_VOLTAGE)) return 8431;
    destination = mapping_destination(); source = original;
    source.topology[0].sourceIndex = 2; source.topology[1].sourceIndex = 5;
    source.legacyIndicesUnresolved = true; source.topology[0].voltageUv = source.topology[1].voltageUv = 0;
    if (!has_issue(profile_map_topology(source, destination), PROFILE_MAP_LEGACY_INDEX) || source.topology[0].voltageUv) return 8432;
    ProfileDocument derived{}; char why[160]{};
    ProfileReviewBinding review{11, 5, 8};
    if (!profile_mapping_accept_legacy_indices(source, destination, 11, review, derived, why, sizeof(why)) ||
        derived.legacyIndicesUnresolved || derived.sourceTopologyComplete || derived.topology[0].voltageUv != 600001 ||
        source.topology[0].voltageUv || !profile_map_topology(derived, destination).ready()) return 8433;
    if (profile_mapping_accept_legacy_indices(source, destination, 12, review, derived, why, sizeof(why))) return 8434;
    destination.revision++;
    if (profile_mapping_accept_legacy_indices(source, destination, 11, review, derived, why, sizeof(why))) return 8435;
    source = profile_document_make_empty(); source.domains = PROFILE_DOMAIN_POWER; source.power.present = true; source.power.ratio = {1, 1};
    destination = mapping_destination(); destination.knownUnavailableDomains = PROFILE_DOMAIN_POWER;
    report = profile_map_topology(source, destination);
    if (!report.ready() || !report.defaultPowerWithoutSurface) return 8436;
    source.power.ratio = {249, 250};
    if (!has_issue(profile_map_topology(source, destination), PROFILE_MAP_UNAVAILABLE_DOMAIN)) return 8437;
    const ProfileConversionGrid grid{true, 1000, 0};
    auto lower = profile_grid_lower(-15000500, grid);
    if (!lower.valid || lower.exact || lower.lower != -15001000 || lower.error != 500) return 8438;
    lower = profile_grid_lower(15000500, grid);
    if (!lower.valid || lower.lower != 15000000 || lower.error != 500) return 8439;
    if (profile_grid_lower(LLONG_MIN, grid).valid || profile_grid_lower(10, {}).valid ||
        !profile_grid_lower(LLONG_MIN, {true, 1, LLONG_MAX}).exact) return 8440;
    lower = profile_grid_lower(-5, {true, 4, 2});
    if (!lower.valid || lower.lower != -6 || lower.error != 1) return 8441;
    const auto power = profile_power_lower({1001, 1000}, 250000, {true, 1000, 0});
    if (!power.valid || power.exact || power.lowerMw != 250000 || power.maximumErrorMw != 250 ||
        !tuning_power_ratio_equal(power.derivedRatio, {1, 1})) return 8442;
    const auto fractional = profile_power_lower({1, 3}, 1000, {true, 10, 0});
    if (!fractional.valid || fractional.exact || fractional.lowerMw != 330 || fractional.maximumErrorMw != 4 ||
        !tuning_power_ratio_equal(fractional.derivedRatio, {33, 100})) return 8443;
    if (profile_power_lower({LLONG_MAX, 1}, LLONG_MAX, grid).valid ||
        profile_power_lower({1, 1}, 1000, {}).valid) return 8444;
    return 0;
}
