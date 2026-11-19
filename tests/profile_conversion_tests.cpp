// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#include "profile_conversion_batch.h"
#include "profile_domain_conversions.h"
#include "profile_destination_review.h"
#include <cstring>
#include <memory>

static int run_profile_reset_equivalence_tests() {
    auto zeroDoc = std::make_unique<ProfileDocument>(profile_document_make_empty());
    auto dirtyDoc = std::make_unique<ProfileDocument>();
    memset(dirtyDoc.get(), 0xA5, sizeof(*dirtyDoc));
    profile_document_reset(*dirtyDoc);
    if (dirtyDoc->documentVersion != 1 || dirtyDoc->applicationScope != PROFILE_SCOPE_UPDATE_INCLUDED ||
        dirtyDoc->documentId != 1 || dirtyDoc->domains != 0 || dirtyDoc->topologyCount != 0 ||
        dirtyDoc->topology[0].topologyId != -1 || dirtyDoc->topology[0].sourceIndex != -1 ||
        dirtyDoc->topology[VF_NUM_POINTS - 1].topologyId != -1 ||
        dirtyDoc->lock.anchorTopologyId != -1 || dirtyDoc->displayName[0] != '\0' ||
        dirtyDoc->lock.present || dirtyDoc->fan.present ||
        !profile_document_semantically_equal(*dirtyDoc, *zeroDoc)) return 8485;

    auto plan = std::make_unique<ProfileControlPlan>();
    memset(plan.get(), 0xA5, sizeof(*plan));
    profile_control_plan_reset(*plan);
    if (plan->disposition != PROFILE_CONTROL_READY || plan->curveChecked ||
        plan->issueCount != 0 || plan->omittedIssues != 0 ||
        plan->curve.issueCount != 0) return 8486;

    auto batch = std::make_unique<ProfileConversionBatch>();
    memset(batch.get(), 0xA5, sizeof(*batch));
    profile_conversion_batch_reset(*batch);
    if (batch->surface.identity != 0 || batch->affectedCount != 0 ||
        batch->firstTopologyId != -1 || batch->worstTopologyId != -1 ||
        batch->source.documentVersion != 1 || batch->source.lock.anchorTopologyId != -1) return 8487;

    auto convPlan = std::make_unique<ProfileConversionPlan>();
    memset(convPlan.get(), 0xA5, sizeof(*convPlan));
    profile_conversion_plan_reset(*convPlan);
    if (convPlan->count != 0 || convPlan->items[0].convertible || convPlan->items[0].why[0] != '\0' ||
        convPlan->items[PROFILE_CONVERSION_MAX_CANDIDATES - 1].batch.source.documentVersion != 1) return 8488;

    auto review = std::make_unique<ProfileDestinationReview>();
    memset(review.get(), 0xA5, sizeof(*review));
    profile_destination_review_reset(*review);
    if (review->accepted || review->stage != PROFILE_REVIEW_EMPTY || review->sourceRevision != 0 ||
        review->conversions.count != 0 || review->runtime.hasLock ||
        review->mapping.issues[0].topologyId != -1 ||
        review->controls.disposition != PROFILE_CONTROL_READY) return 8489;
    return 0;
}

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

    // UNIT-07 shared domain conversion planner: half-kHz VF, power, voltage refusal, unknown grid, and whole-batch constraint failure.
    auto capture = std::make_unique<HardwareCaptureObservation>();
    capture->valid = 1; capture->schemaVersion = HARDWARE_CAPTURE_SCHEMA_CURRENT;
    capture->gpuGeneration = 9; capture->revision = capture->fingerprint = 77;
    capture->binding.valid = 1; capture->requestedDomains = 511u;
    for (int i = 0; i < 9; ++i) hardware_capture_domain(*capture, 1 << i, CAPTURE_UNSUPPORTED, -100001);
    capture->topologyComplete = 1; capture->pointCount = 128;
    for (int i = 0; i < 128; ++i) {
        auto& pt = capture->points[i];
        pt.index = (gc_u32)i; pt.voltageUv = 600000 + (gc_i64)i * 6250;
        pt.writableState = 1; pt.offsetValid = pt.frequencyValid = 1;
        pt.frequencyHz = 1700000000LL + (gc_i64)i * 1000000LL;
    }
    hardware_capture_domain(*capture, PROFILE_DOMAIN_VF_CURVE, CAPTURE_COMPLETE);
    capture->vfLimits = {1, 1, 1, 1, VF_OFFSET_RANGE_SOURCE_NVML_GRAPHICS, -200000000, 200000000, 1000};
    capture->power = {1, 1, 1, 1, 0, 0, 0, 250000, 250000, 100000, 300000, 1000};
    hardware_capture_domain(*capture, PROFILE_DOMAIN_POWER, CAPTURE_COMPLETE);
    capture->xbarVoltage = {1, 1, 1, 0, 0, 0, 0, 0, -50000, 50000, 1000};
    hardware_capture_domain(*capture, PROFILE_DOMAIN_XBAR, CAPTURE_COMPLETE);
    ProfileDestinationTopology dest{};
    if (!profile_destination_from_capture(*capture, dest, why, sizeof(why))) return 8475;

    *source = profile_document_make_empty();
    source->domains = PROFILE_DOMAIN_VF_CURVE | PROFILE_DOMAIN_POWER | PROFILE_DOMAIN_XBAR;
    source->topologyCount = 128; source->sourceTopologyComplete = true; source->source.captureComplete = true;
    for (int i = 0; i < 128; ++i) {
        source->topology[i] = {1000 + i, i, 600000 + (gc_i64)i * 6250, 1};
        source->points[i].kind = PROFILE_POINT_CAPTURED_OFFSET;
        source->points[i].capturedOffsetHz = i % 2 ? -15000500 : 15000500;
        source->points[i].hasObservedOffset = true;
        source->points[i].observedOffsetHz = source->points[i].capturedOffsetHz;
    }
    source->points[17].capturedOffsetHz = -15000001;
    source->power.present = true; source->power.ratio = {1001, 1000};
    source->hasXbarMsvddOffsetUv = true; source->xbarMsvddOffsetUv = -500;

    auto convPlan = std::make_unique<ProfileConversionPlan>();
    profile_conversion_plan_build(*source, *capture, dest, 11, {}, *convPlan);
    const auto* vfCand = convPlan->find(PROFILE_CONVERT_VF_OFFSETS);
    const auto* pwrCand = convPlan->find(PROFILE_CONVERT_POWER);
    const auto* voltCand = convPlan->find(PROFILE_CONVERT_XBAR_VOLTAGE);
    if (!vfCand || !vfCand->convertible || vfCand->batch.affectedCount != 128 || vfCand->batch.maximumError != 999 ||
        !pwrCand || !pwrCand->convertible || pwrCand->batch.maximumError != 250 ||
        !voltCand || voltCand->convertible || !voltCand->unresolved) return 8476;

    // Accepting VF then Power keeps source exact while producing representable derived values.
    if (!profile_conversion_accept_item(*vfCand, *source, 11, *capture, dest, {}, *derived, summary, sizeof(summary), why, sizeof(why)) ||
        !strstr(summary, "profile-map: domain=vf-offsets") || !strstr(summary, "decision=accepted") ||
        derived->points[1].capturedOffsetHz != -15001000 || derived->points[17].capturedOffsetHz != -15001000 ||
        source->points[1].capturedOffsetHz != -15000500) return 8477;

    // Stale capture revision must refuse acceptance.
    auto staleCapture = *capture; ++staleCapture.revision;
    if (profile_conversion_accept_item(*vfCand, *source, 11, staleCapture, dest, {}, *derived, summary, sizeof(summary), why, sizeof(why)) ||
        !strstr(summary, "decision=rejected")) return 8478;

    // Voltage adjustment never accepts batch conversion.
    if (profile_conversion_accept_item(*voltCand, *source, 11, *capture, dest, {}, *derived, summary, sizeof(summary), why, sizeof(why)) ||
        !strstr(summary, "decision=rejected") || !strstr(why, "voltage")) return 8479;

    // Re-plan on derived VF copy and accept power conversion cleanly.
    auto derived2 = std::make_unique<ProfileDocument>();
    profile_conversion_plan_build(*derived, *capture, dest, 12, {}, *convPlan);
    pwrCand = convPlan->find(PROFILE_CONVERT_POWER);
    if (!pwrCand || !pwrCand->convertible ||
        !profile_conversion_accept_item(*pwrCand, *derived, 12, *capture, dest, {}, *derived2, summary, sizeof(summary), why, sizeof(why)) ||
        !tuning_power_ratio_equal(derived2->power.ratio, {1, 1}) || source->power.ratio.numerator != 1001) return 8480;

    // Unknown granularity is reported as unresolved instead of inventing a grid.
    auto unknownGrid = *capture; unknownGrid.vfLimits.gridValid = 0; unknownGrid.vfLimits.gridHz = 0;
    profile_conversion_plan_build(*source, unknownGrid, dest, 11, {}, *convPlan);
    vfCand = convPlan->find(PROFILE_CONVERT_VF_OFFSETS);
    if (!vfCand || vfCand->convertible || !vfCand->unresolved || !strstr(vfCand->why, "unknown")) return 8481;

    // Whole-batch planner refusal when downward rounding crosses VF minimum bound.
    auto tightRange = *capture; tightRange.vfLimits.minimumHz = -15000000;
    profile_conversion_plan_build(*source, tightRange, dest, 11, {}, *convPlan);
    vfCand = convPlan->find(PROFILE_CONVERT_VF_OFFSETS);
    if (!vfCand || vfCand->convertible || !vfCand->unresolved || vfCand->batch.affectedCount != 0) return 8482;

    // CAP-10: Restoring exact captured tail removes lock and restores observed tail offsets without recapture.
    auto flat = std::make_unique<ProfileDocument>(profile_document_make_empty());
    flat->domains = PROFILE_DOMAIN_VF_CURVE | PROFILE_DOMAIN_LOCK;
    flat->topologyCount = 4; flat->sourceTopologyComplete = true;
    for (int i = 0; i < 4; ++i) {
        flat->topology[i] = {200 + i, 20 + i, 650000 + (gc_i64)i * 6250, 1};
        flat->points[i].hasObservedOffset = true;
        flat->points[i].observedOffsetHz = i < 2 ? -15000500 : -25000500;
        if (i < 2) {
            flat->points[i].kind = PROFILE_POINT_CAPTURED_OFFSET;
            flat->points[i].capturedOffsetHz = -15000500;
        } else {
            flat->points[i].kind = PROFILE_POINT_ABSENT;
        }
    }
    flat->lock.present = true; flat->lock.mode = LOCK_MODE_FLATTEN;
    flat->lock.anchorTopologyId = 202; flat->lock.targetFrequencyHz = 1000000000;
    auto restored = std::make_unique<ProfileDocument>();
    if (!profile_document_can_restore_captured_tail(*flat, why, sizeof(why)) ||
        !profile_document_restore_captured_tail(*flat, *restored, why, sizeof(why)) ||
        restored->lock.present || (restored->domains & PROFILE_DOMAIN_LOCK) || !(restored->domains & PROFILE_DOMAIN_VF_CURVE) ||
        restored->points[1].capturedOffsetHz != -15000500 ||
        restored->points[2].kind != PROFILE_POINT_CAPTURED_OFFSET || restored->points[2].capturedOffsetHz != -25000500 ||
        restored->points[3].kind != PROFILE_POINT_CAPTURED_OFFSET || restored->points[3].capturedOffsetHz != -25000500 ||
        !flat->lock.present) return 8483;

    // Missing captured tail observation refuses restore cleanly and preserves original draft.
    flat->points[3].hasObservedOffset = false;
    const auto frozenFlat = *flat;
    if (profile_document_can_restore_captured_tail(*flat, why, sizeof(why)) || !strstr(why, "missing") ||
        profile_document_restore_captured_tail(*flat, *restored, why, sizeof(why)) ||
        !profile_document_semantically_equal(*flat, frozenFlat)) return 8484;
    if (const int resetFailure = run_profile_reset_equivalence_tests()) return resetFailure;
    return 0;
}
