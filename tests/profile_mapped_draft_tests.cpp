// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
// Mapped-draft acceptance and the shared review engines the CLI uses:
// domain conversion, captured-tail restore and curve assistance. No UI state.
// Cases 9630-9680 and 9870-9874.
#include "profile_review_fixture.h"
#include "profile_mapped_draft.h"
#include "service_control_review.h"
#include "gui_service_actionability_policy.h"

int run_profile_mapped_draft_tests() {
    auto live = std::make_unique<ServiceResponse>();
    profile_review_fixture::live(*live);
    auto savedSource = profile_review_fixture::source();
    auto sourceHolder = std::make_unique<ProfileDocument>(savedSource);
    auto &source = *sourceHolder;
    profile_review_fixture::capture(*live, source);
    auto reviewHolder = std::make_unique<ProfileDestinationReview>();
    auto &review = *reviewHolder;
    std::vector<std::string> lines;
    if (profile_editor_check(
            source, 0, 0,
            [&](gc_u32 domains, auto &out, auto &) {
                if (domains != 511u)
                    return false;
                out = live->capture;
                return true;
            },
            lines, &review, 29) != PROFILE_COMMAND_DECISION ||
        !profile_mapped_draft_ready(review, source, 29))
        return 9630;
    DesiredSettings desired{};
    desired.hasGpuOffset = 1;
    gc_u64 token = 123;
    if (profile_mapped_draft_prepare(review, source, 29, desired, token) || !desired.hasGpuOffset || token != 123)
        return 9631; // The mapped draft must be explicitly accepted first.
    if (!profile_mapped_draft_accept(review, source, 29) || !review.accepted ||
        !profile_mapped_draft_prepare(review, source, 29, desired, token) || desired.hasGpuOffset ||
        desired.memOffsetHz != -123500000 || desired.memOffsetSurface != CLOCK_OFFSET_SURFACE_P0 ||
        desired.applicationScope != DESIRED_SCOPE_UPDATE_INCLUDED || token != live->capture.revision)
        return 9632;
    // Acceptance is neither a rounded live projection nor a local save/ownership acknowledgment.
    if (source.memoryOffsetHz != -123500000 || savedSource.memoryOffsetHz != source.memoryOffsetHz)
        return 9633;
    ServiceRequest request{};
    request.magic = SERVICE_PROTOCOL_MAGIC;
    request.version = SERVICE_PROTOCOL_VERSION;
    request.command = SERVICE_CMD_APPLY;
    request.flags = SERVICE_REQUEST_FLAG_INTERACTIVE;
    request.applyOrigin = SERVICE_APPLY_ORIGIN_GUI;
    request.callerPid = 1;
    request.operationId = 5;
    request.targetGpu = live->snapshot.adapters[0];
    request.desired = desired;
    request.expectedServiceInstanceId = live->state.serviceInstanceId;
    request.expectedGpuGeneration = live->state.gpuGeneration;
    request.expectedTopologySignature = live->state.topologySignature;
    request.expectedControlFingerprint = token;
    request.expectedControlDomains = 511u;
    char why[256]{};
    int reads = 0;
    const auto read = [&](HardwareCaptureObservation &out, char *, size_t) {
        ++reads;
        out = live->capture;
        return true;
    };
    if (service_request_reject_reason(&request) ||
        !service_control_review_matches(request, nullptr, read, why, sizeof(why)) || reads != 1)
        return 9647;
    live->capture.memory.value += 500000;
    live->capture.fingerprint = hardware_capture_fingerprint(live->capture);
    if (service_control_review_matches(request, nullptr, read, why, sizeof(why)) || reads != 2)
        return 9648;
    live->capture = review.capture;
    if (profile_mapped_draft_prepare(review, source, 30, desired, token))
        return 9634; // A different source revision cannot reuse the review.
    source.source.driverVersion[0] = 'x';
    if (profile_mapped_draft_ready(review, source, 29))
        return 9635; // Metadata edits are source changes too.
    source = savedSource;
    const auto capture = review.capture;
    ++review.capture.memory.value;
    if (profile_mapped_draft_prepare(review, source, 29, desired, token))
        return 9636;
    review.capture = capture;
    review.capture.requestedDomains = PROFILE_DOMAIN_MEMORY;
    if (profile_mapped_draft_ready(review, source, 29))
        return 9637;
    review.capture = capture;
    review.controls.disposition = PROFILE_CONTROL_REFUSED;
    if (profile_mapped_draft_accept(review, source, 29) || review.accepted)
        return 9638;
    review.controls.disposition = PROFILE_CONTROL_UNAVAILABLE;
    if (profile_mapped_draft_accept(review, source, 29))
        return 9639;
    review.controls.disposition = PROFILE_CONTROL_CONDITIONAL;
    if (!profile_mapped_draft_accept(review, source, 29))
        return 9640;
    review.mapping.structurallyValid = false;
    if (profile_mapped_draft_accept(review, source, 29))
        return 9646;

    // Batch conversion produces a derived document while the reviewed source stays exact.
    auto convDoc = profile_review_fixture::source();
    convDoc.memoryOffsetHz = -500;
    profile_review_fixture::capture(*live, convDoc);
    auto convReview = std::make_unique<ProfileDestinationReview>();
    const auto liveCapture = [&](gc_u32 domains, auto &out, auto &) {
        if (domains != 511u)
            return false;
        out = live->capture;
        return true;
    };
    if (profile_editor_check(convDoc, 0, 0, liveCapture, lines, convReview.get(), 40) != PROFILE_COMMAND_DECISION ||
        !convReview->conversions.anyConvertible())
        return 9678;
    int convertible = -1;
    for (int i = 0; i < convReview->conversions.count && convertible < 0; ++i)
        if (convReview->conversions.items[i].convertible)
            convertible = i;
    ProfileDestinationTopology dest{};
    char summary[256]{};
    auto derived = std::make_unique<ProfileDocument>();
    if (convertible < 0 || !profile_destination_from_capture(convReview->capture, dest, why, sizeof(why)) ||
        !profile_conversion_accept_item(convReview->conversions.items[convertible], convDoc, 40, convReview->capture,
            dest, convReview->routes, *derived, summary, sizeof(summary), why, sizeof(why)) ||
        !strstr(summary, "profile-map: domain=memory") || !strstr(summary, "decision=accepted") ||
        derived->memoryOffsetHz != -500000 || convDoc.memoryOffsetHz != -500)
        return 9679;

    // Captured-tail restore works on the document alone, without a recapture.
    auto flat = profile_document_make_empty();
    flat.domains = PROFILE_DOMAIN_VF_CURVE | PROFILE_DOMAIN_LOCK;
    flat.topologyCount = 2;
    flat.sourceTopologyComplete = true;
    flat.topology[0] = {10, 22, 662000, 1};
    flat.topology[1] = {11, 23, 668250, 1};
    flat.points[0] = {PROFILE_POINT_CAPTURED_OFFSET, 0, 15000500, false, 0, true, 15000500};
    flat.points[1] = {PROFILE_POINT_ABSENT, 0, 0, false, 0, true, -25000500};
    flat.lock.present = true;
    flat.lock.mode = LOCK_MODE_FLATTEN;
    flat.lock.anchorTopologyId = 11;
    flat.lock.targetFrequencyHz = 1000000000;
    auto restored = std::make_unique<ProfileDocument>();
    if (!profile_document_restore_captured_tail(flat, *restored, why, sizeof(why)) || restored->lock.present ||
        !flat.lock.present || restored->points[1].kind != PROFILE_POINT_CAPTURED_OFFSET ||
        restored->points[1].capturedOffsetHz != -25000500)
        return 9680;

    // Curve assistance: a two-step preview that leaves the document untouched until accepted,
    // and a stale source revision is refused.
    auto assistDoc = profile_document_make_empty();
    assistDoc.domains = PROFILE_DOMAIN_VF_CURVE | PROFILE_DOMAIN_LOCK;
    assistDoc.topologyCount = 3;
    assistDoc.sourceTopologyComplete = true;
    for (int i = 0; i < 3; ++i)
        assistDoc.topology[i] = {10 + i, 22 + i, 662000 + (gc_i64)i * 6250, 1};
    assistDoc.points[0].kind = PROFILE_POINT_ABSOLUTE_FREQUENCY;
    assistDoc.points[0].absoluteFrequencyHz = 1200000000LL;
    assistDoc.points[1].kind = PROFILE_POINT_ABSOLUTE_FREQUENCY;
    assistDoc.points[1].absoluteFrequencyHz = 1000000000LL;
    assistDoc.lock.present = true;
    assistDoc.lock.mode = LOCK_MODE_FLATTEN;
    assistDoc.lock.anchorTopologyId = 11;
    assistDoc.lock.targetFrequencyHz = 1050000250LL;
    profile_review_fixture::capture(*live, assistDoc);
    {
        auto &o = live->capture;
        o.pointCount = 3;
        for (gc_u32 i = 0; i < 3; ++i) {
            o.points[i].index = 22 + i;
            o.points[i].voltageUv = 662000 + (gc_i64)i * 6250;
            o.points[i].writableState = o.points[i].frequencyValid = o.points[i].offsetValid = 1;
            o.points[i].offsetHz = 0;
            o.points[i].frequencyHz = 1000000000LL + (gc_i64)i * 50000000LL;
        }
        o.vfLimits = {1, 1, 1, 1, VF_OFFSET_RANGE_SOURCE_NVML_GRAPHICS, -500000000, 500000000, 500};
        o.fingerprint = hardware_capture_fingerprint(o);
        o.revision = service_control_review_revision(o.fingerprint, nullptr);
    }
    auto assistReview = std::make_unique<ProfileDestinationReview>();
    if (profile_editor_check(assistDoc, 0, 0, liveCapture, lines, assistReview.get(), 80) != PROFILE_COMMAND_DECISION)
        return 9870;
    ProfileDestinationTopology assistDest{};
    if (!profile_destination_from_capture(assistReview->capture, assistDest, why, sizeof(why)))
        return 9871;
    auto assist = std::make_unique<ProfileCurveAssistance>();
    profile_curve_assistance_prepare(assistDoc, assistReview->capture, assistDest, 80, assistReview->routes,
        PROFILE_ASSIST_COMBINED, *assist);
    if ((assist->status != PROFILE_ASSIST_PREPARED && assist->status != PROFILE_ASSIST_ESTIMATE) ||
        assistDoc.lock.targetFrequencyHz != 1050000250LL)
        return 9872;
    auto accepted = std::make_unique<ProfileDocument>();
    if (profile_curve_assistance_accept(*assist, assistDoc, 81, assistReview->capture, assistDest,
            assistReview->routes, *accepted, why, sizeof(why)) ||
        assistDoc.lock.targetFrequencyHz != 1050000250LL)
        return 9873;
    if (!profile_curve_assistance_accept(*assist, assistDoc, 80, assistReview->capture, assistDest,
            assistReview->routes, *accepted, why, sizeof(why)) ||
        accepted->lock.targetFrequencyHz != 1050000000LL || accepted->points[0].absoluteFrequencyHz != 1050000000LL ||
        assistDoc.lock.targetFrequencyHz != 1050000250LL)
        return 9874;
    return 0;
}
