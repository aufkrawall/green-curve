// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// Compatible-curve assistance (§11 Operation A & Operation B):
// - Operation A (`PROFILE_ASSIST_PREFIX`): lowers conflicting pre-anchor points
//   in the draft without moving the anchor/plateau, preserving captured-offset
//   intent where exact representation is proven, explicitly previewing any
//   projected->absolute conversion, and refusing to guess over a shaped base.
// - Operation B (`PROFILE_ASSIST_CLOSEST_TARGET` / `PROFILE_ASSIST_COMBINED`):
//   searches analytically + deterministically descending (<= 65536 evaluations)
//   over exact planner quantization grids without assuming binary-search
//   monotonicity or modifying unrelated domains.
// Pure, read-only, GPU-free; never triggers hardware writes or auto-Apply.
#pragma once
#include "curve_prefix_repair_message.h"
#include "profile_capture_curve_plan.h"
#include "profile_runtime_resolution.h"
#include "profile_control_feasibility.h"
#include "profile_conversion_grid.h"
#include <algorithm>
#include <memory>
#include <string>
#include <string_view>

enum ProfileCurveAssistMode {
    PROFILE_ASSIST_PREFIX = 0,
    PROFILE_ASSIST_CLOSEST_TARGET = 1,
    PROFILE_ASSIST_COMBINED = 2,
};

enum ProfileCurveAssistStatus {
    PROFILE_ASSIST_NOT_NEEDED = 0,
    PROFILE_ASSIST_PREPARED = 1,
    PROFILE_ASSIST_ESTIMATE = 2,
    PROFILE_ASSIST_NO_COMPATIBLE_TARGET = 3,
    PROFILE_ASSIST_INDETERMINATE = 4,
    PROFILE_ASSIST_LIMITED = 5,
    PROFILE_ASSIST_BASELINE_REQUIRED = 6,
    PROFILE_ASSIST_UNAVAILABLE = 7,
};

struct ProfileCurveAssistBinding {
    gc_u64 sourceRevision = 0;
    gc_u64 gpuGeneration = 0;
    gc_u64 captureRevision = 0;
    gc_u64 destinationRevision = 0;
    ProfileDocument expectedSourceSnapshot{};
};

struct ProfileCurveAssistance {
    ProfileCurveAssistStatus status = PROFILE_ASSIST_UNAVAILABLE;
    ProfileCurveAssistMode mode = PROFILE_ASSIST_PREFIX;
    bool proven = false;
    bool budgetExhausted = false;
    int prefixChangedCount = 0;
    int conversionCount = 0;
    unsigned int candidatesEvaluated = 0;
    gc_i64 originalLockHz = 0;
    gc_i64 suggestedLockHz = 0;
    CurvePrefixRepair repair{};
    CurvePlanResult planBefore{};
    CurvePlanResult planAfter{};
    ProfileCurveAssistBinding binding{};
    ProfileDocument previewDocument{};
    char summary[256]{};
    char explanation[1536]{};
};

static inline const char* profile_curve_assist_mode_key(ProfileCurveAssistMode m) {
    switch (m) {
        case PROFILE_ASSIST_PREFIX: return "prefix";
        case PROFILE_ASSIST_CLOSEST_TARGET: return "closest-target";
        case PROFILE_ASSIST_COMBINED: return "combined";
    }
    return "unknown";
}

static inline const char* profile_curve_assist_mode_label(ProfileCurveAssistMode m) {
    switch (m) {
        case PROFILE_ASSIST_PREFIX: return "Lower prefix";
        case PROFILE_ASSIST_CLOSEST_TARGET: return "Closest target";
        case PROFILE_ASSIST_COMBINED: return "Prefix + target";
    }
    return "Unknown";
}

static inline bool profile_curve_assist_mode_from_key(std::string_view key, ProfileCurveAssistMode& out) {
    if (key == "prefix") { out = PROFILE_ASSIST_PREFIX; return true; }
    if (key == "closest-target") { out = PROFILE_ASSIST_CLOSEST_TARGET; return true; }
    if (key == "combined") { out = PROFILE_ASSIST_COMBINED; return true; }
    return false;
}

static inline const char* profile_curve_assist_status_name(ProfileCurveAssistStatus s) {
    switch (s) {
        case PROFILE_ASSIST_NOT_NEEDED: return "not-needed";
        case PROFILE_ASSIST_PREPARED: return "prepared";
        case PROFILE_ASSIST_ESTIMATE: return "estimate";
        case PROFILE_ASSIST_NO_COMPATIBLE_TARGET: return "no-compatible-target";
        case PROFILE_ASSIST_INDETERMINATE: return "indeterminate";
        case PROFILE_ASSIST_LIMITED: return "limited";
        case PROFILE_ASSIST_BASELINE_REQUIRED: return "baseline-required";
        case PROFILE_ASSIST_UNAVAILABLE: return "unavailable";
    }
    return "unknown";
}

namespace profile_assist_detail {

static inline gc_i64 mod_pos(gc_i64 v, gc_i64 m) {
    gc_i64 r = v % m;
    return r < 0 ? r + m : r;
}

static inline bool intersect_congruences(gc_i64 r1, gc_i64 m1, gc_i64 r2, gc_i64 m2,
    gc_i64& outStep, gc_i64& outOrigin) {
    if (m1 <= 0 || m2 <= 0) return false;
    gc_i64 a = m1, b = m2, x0 = 1, x1 = 0;
    while (b) {
        const gc_i64 q = a / b, t = a % b;
        a = b; b = t;
        const gc_i64 nx = x0 - q * x1;
        x0 = x1; x1 = nx;
    }
    const gc_i64 g = a;
    const gc_i64 a1 = mod_pos(r1, m1), a2 = mod_pos(r2, m2);
    const gc_i64 diff = a2 - a1;
    if (diff % g != 0) return false;
    const gc_i64 m1g = m1 / g, m2g = m2 / g;
    if (m1g > LLONG_MAX / m2) return false;
    outStep = m1g * m2;
    const gc_i64 k = mod_pos((diff / g) % m2g * mod_pos(x0, m2g), m2g);
    outOrigin = mod_pos(a1 + m1 * k, outStep);
    return true;
}

static inline void isolate_curve_doc(const ProfileDocument& src, ProfileDocument& out) {
    out = src;
    out.domains &= (PROFILE_DOMAIN_VF_CURVE | PROFILE_DOMAIN_GRAPHICS_OFFSET | PROFILE_DOMAIN_LOCK);
    out.hasMemoryOffset = false; out.memoryOffsetHz = 0; out.memorySurface = 0;
    out.power = {}; out.fan = {};
    out.hasXbarOffsetHz = out.hasXbarMsvddOffsetUv = out.hasSysClkOffsetHz = out.hasVideoClkOffsetHz = false;
    out.xbarOffsetHz = out.xbarMsvddOffsetUv = out.sysClkOffsetHz = out.videoClkOffsetHz = 0;
}

static inline bool apply_assist_to_doc(const ProfileDocument& source, const ProfileMappingReport& mapping,
    const CurvePrefixRepair& repair, gc_i64 newLockHz, ProfileDocument& out) {
    out = source;
    bool mutated = false;
    if (newLockHz > 0 && out.lock.present && out.lock.targetFrequencyHz != newLockHz) {
        out.lock.targetFrequencyHz = newLockHz;
        mutated = true;
    }
    if (repair.changedCount > 0) {
        out.domains |= PROFILE_DOMAIN_VF_CURVE;
        for (int k = 0; k < repair.changedCount; ++k) {
            const auto& pt = repair.points[k];
            int at = -1;
            for (int i = 0; i < out.topologyCount; ++i) {
                if (mapping.destinationByTopology[i] == pt.ci) { at = i; break; }
            }
            if (at < 0) return false;
            if (pt.newKind == CURVE_PLAN_POINT_CAPTURED) {
                out.points[at].kind = PROFILE_POINT_CAPTURED_OFFSET;
                out.points[at].capturedOffsetHz = pt.newOffsetHz;
                out.points[at].absoluteFrequencyHz = 0;
            } else {
                out.points[at].kind = PROFILE_POINT_ABSOLUTE_FREQUENCY;
                out.points[at].absoluteFrequencyHz = pt.newHz;
                out.points[at].capturedOffsetHz = 0;
            }
            mutated = true;
        }
    }
    if (mutated) out.source.captureComplete = false;
    return profile_document_structurally_valid(out, nullptr, 0);
}

static inline void finalize_summary(ProfileCurveAssistance& a, unsigned int evals) {
    a.candidatesEvaluated = evals;
    snprintf(a.summary, sizeof(a.summary),
        "curve-assist: mode=%s status=%s proven=%d lock=%lld->%lld prefix=%d conv=%d evals=%u",
        profile_curve_assist_mode_key(a.mode), profile_curve_assist_status_name(a.status),
        a.proven ? 1 : 0, a.originalLockHz, a.suggestedLockHz,
        a.prefixChangedCount, a.conversionCount, a.candidatesEvaluated);
}

static inline void format_preview_explanation(ProfileCurveAssistance& a, const CurvePrefixRepair& repair) {
    a.explanation[0] = 0;
    if (!a.proven) {
        curve_plan_appendf(a.explanation, sizeof(a.explanation),
            "ESTIMATE ONLY: current GPU curve carries programmed offsets; Apply requires a fresh baseline.\n");
    }
    if (a.suggestedLockHz > 0 && a.suggestedLockHz != a.originalLockHz) {
        curve_plan_appendf(a.explanation, sizeof(a.explanation),
            "Lock target lowered: %lld Hz -> %lld Hz (anchor fixed at %u mV).\n",
            a.originalLockHz, a.suggestedLockHz, a.planAfter.lockVoltUv / 1000u);
    }
    if (repair.changedCount > 0) {
        curve_plan_appendf(a.explanation, sizeof(a.explanation),
            "Lowered %d prefix point%s (%u..%u mV; %d kind conversion%s):\n",
            repair.changedCount, repair.changedCount == 1 ? "" : "s",
            repair.minVoltUv / 1000u, repair.maxVoltUv / 1000u,
            repair.conversionCount, repair.conversionCount == 1 ? "" : "s");
        const int limit = repair.changedCount < 8 ? repair.changedCount : 8;
        for (int k = repair.changedCount - 1, shown = 0; k >= 0 && shown < limit; --k, ++shown) {
            const auto& p = repair.points[k];
            const char* note = p.wasConverted ? " [projection->absolute]"
                : p.wasAbsent ? " [absent->absolute]"
                : p.newKind == CURVE_PLAN_POINT_CAPTURED ? " [kept captured]" : "";
            curve_plan_appendf(a.explanation, sizeof(a.explanation),
                "  %u mV (pt %d): %lld -> %lld Hz%s\n",
                p.voltUv / 1000u, p.ci, p.oldHz, p.newHz, note);
        }
        if (repair.changedCount > limit) {
            curve_plan_appendf(a.explanation, sizeof(a.explanation),
                "  ... and %d more point(s)\n", repair.changedCount - limit);
        }
    }
    curve_plan_appendf(a.explanation, sizeof(a.explanation),
        "Draft-only change: memory, power, fan and auxiliary tuning are unchanged.");
}

} // namespace profile_assist_detail

static inline void profile_curve_assistance_prepare(
    const ProfileDocument& source,
    const HardwareCaptureObservation& capture,
    const ProfileDestinationTopology& destination,
    gc_u64 sourceRevision,
    ProfileRuntimeRoutes routes,
    ProfileCurveAssistMode mode,
    ProfileCurveAssistance& result,
    unsigned int maxCandidates = 65536u) {
    auto out = std::make_unique<ProfileCurveAssistance>();
    out->mode = mode;
    if (source.lock.present) out->originalLockHz = source.lock.targetFrequencyHz;
    maxCandidates = std::clamp(maxCandidates, 1u, 65536u);
    const auto fail = [&](ProfileCurveAssistStatus st, const char* msg) {
        out->status = st;
        snprintf(out->explanation, sizeof(out->explanation), "%s", msg);
        profile_assist_detail::finalize_summary(*out, out->candidatesEvaluated);
        result = *out;
    };
    #define ASSIST_FAIL(st, msg) do { fail((st), (msg)); return; } while (0)

    char why[256]{};
    if (!profile_document_structurally_valid(source, why, sizeof(why)))
        ASSIST_FAIL(PROFILE_ASSIST_UNAVAILABLE, why[0] ? why : "source document is invalid");
    if (source.legacyIndicesUnresolved)
        ASSIST_FAIL(PROFILE_ASSIST_UNAVAILABLE, "resolve legacy point indices before curve assistance");
    if (!source.lock.present || source.lock.mode == LOCK_MODE_NONE)
        ASSIST_FAIL(PROFILE_ASSIST_UNAVAILABLE, "curve assistance requires a lock intent");
    if (!sourceRevision || !capture.valid || !(capture.capturedDomains & PROFILE_DOMAIN_VF_CURVE) ||
        !capture.topologyComplete || !capture.vfLimits.valid || !capture.vfLimits.gridValid ||
        capture.vfLimits.gridHz <= 0 || !capture.vfLimits.backendWritable) {
        ASSIST_FAIL(PROFILE_ASSIST_UNAVAILABLE, "destination VF topology, limits or grid are unavailable");
    }
    if (source.graphicsOffset.present && !source.graphicsOffset.controlSurface &&
        routes.graphics == DESIRED_GRAPHICS_AUTO)
        ASSIST_FAIL(PROFILE_ASSIST_BASELINE_REQUIRED, "resolve destination graphics control composition before preparing curve");

    auto curveDoc = std::make_unique<ProfileDocument>();
    profile_assist_detail::isolate_curve_doc(source, *curveDoc);
    auto baseRuntime = std::make_unique<DesiredSettings>();
    auto mappingPtr = std::make_unique<ProfileMappingReport>();
    ProfileMappingReport& mapping = *mappingPtr;
    if (!profile_resolve_runtime(*curveDoc, destination, sourceRevision, routes, *baseRuntime, mapping, why, sizeof(why)))
        ASSIST_FAIL(PROFILE_ASSIST_UNAVAILABLE, why[0] ? why : "destination mapping requires review");

    auto planStorage = std::make_unique<ProfileCaptureCurvePlan>();
    if (!profile_capture_curve_plan_input(capture, *baseRuntime, *planStorage))
        ASSIST_FAIL(PROFILE_ASSIST_UNAVAILABLE, "destination curve baseline input is unavailable");

    const auto& in = planStorage->input;
    const int anchorCi = baseRuntime->lockCi;
    if (vf_point_target_status(in.floor, in.curve, anchorCi) != VF_POINT_TARGET_OK)
        ASSIST_FAIL(PROFILE_ASSIST_UNAVAILABLE, "lock anchor is on a read-only floor or unavailable point");
    if (in.lockMode == LOCK_MODE_FLATTEN && !vf_offset_range_supports_flatten({in.range.minKHz, in.range.maxKHz, true}))
        ASSIST_FAIL(PROFILE_ASSIST_UNAVAILABLE, "flatten lock is unsupported by destination VF offset range");

    out->binding.sourceRevision = sourceRevision;
    out->binding.gpuGeneration = capture.gpuGeneration;
    out->binding.captureRevision = capture.revision;
    out->binding.destinationRevision = destination.revision;
    out->binding.expectedSourceSnapshot = source;
    out->planBefore = curve_plan_evaluate(in);
    const bool baselineProves = curve_plan_baseline_proves(in.evidence);
    const auto acceptReady = [&](const CurvePlanResult& res) {
        return baselineProves ? res.disposition == CURVE_PLAN_REPRESENTABLE
            : (res.disposition == CURVE_PLAN_REPRESENTABLE || res.disposition == CURVE_PLAN_NEEDS_FRESH_BASELINE);
    };

    if (out->planBefore.disposition == CURVE_PLAN_REPRESENTABLE) {
        out->suggestedLockHz = out->originalLockHz;
        out->planAfter = out->planBefore;
        out->proven = baselineProves;
        ASSIST_FAIL(PROFILE_ASSIST_NOT_NEEDED, "curve already passes destination planner without modification");
    }

    if (mode == PROFILE_ASSIST_PREFIX) {
        auto repPtr = std::make_unique<CurvePrefixRepair>(curve_prefix_repair_prepare(in, true));
        const auto& rep = *repPtr;
        out->repair = rep;
        if (rep.status == CURVE_PREFIX_REPAIR_NOT_NEEDED || rep.status == CURVE_PREFIX_REPAIR_NO_LOCK) {
            ASSIST_FAIL(out->planBefore.disposition == CURVE_PLAN_REFUSED ? PROFILE_ASSIST_NO_COMPATIBLE_TARGET : PROFILE_ASSIST_UNAVAILABLE,
                "lowering earlier points does not resolve this lock's anchor/tail constraint; try Closest target");
        }
        if (rep.status == CURVE_PREFIX_REPAIR_LIMITED) {
            char msg[768]{};
            curve_prefix_repair_format_refusal(rep, msg, sizeof(msg));
            ASSIST_FAIL(PROFILE_ASSIST_LIMITED, msg[0] ? msg : "prefix point cannot be lowered far enough");
        }
        if (rep.status == CURVE_PREFIX_REPAIR_BASELINE_REQUIRED || rep.status == CURVE_PREFIX_REPAIR_CONVERSION_REQUIRED) {
            char msg[768]{};
            curve_prefix_repair_format_refusal(rep, msg, sizeof(msg));
            ASSIST_FAIL(PROFILE_ASSIST_BASELINE_REQUIRED, msg[0] ? msg : "trusted stock baseline required before adjusting captured/projected prefix");
        }
        if (rep.status != CURVE_PREFIX_REPAIR_PREPARED) {
            char msg[768]{};
            curve_prefix_repair_format_refusal(rep, msg, sizeof(msg));
            ASSIST_FAIL(PROFILE_ASSIST_NO_COMPATIBLE_TARGET, msg[0] ? msg : "lowering prefix cannot make this curve feasible");
        }
        if (!profile_assist_detail::apply_assist_to_doc(source, mapping, rep, 0, out->previewDocument))
            ASSIST_FAIL(PROFILE_ASSIST_UNAVAILABLE, "could not construct valid repaired document");
        out->prefixChangedCount = rep.changedCount;
        out->conversionCount = rep.conversionCount;
        out->suggestedLockHz = out->originalLockHz;
        out->planAfter = rep.after;
        out->proven = baselineProves && rep.after.disposition == CURVE_PLAN_REPRESENTABLE;
        out->status = out->proven ? PROFILE_ASSIST_PREPARED : PROFILE_ASSIST_ESTIMATE;
        profile_assist_detail::format_preview_explanation(*out, rep);
        profile_assist_detail::finalize_summary(*out, 1);
        result = *out;
        return;
    }

    // Operation B: bounded deterministic descending search over exact target grid.
    const bool allowPrefix = (mode == PROFILE_ASSIST_COMBINED);
    const gc_i64 vfGrid = capture.vfLimits.gridHz;
    const gc_i64 anchorBaseHz = curve_plan_base_hz(in, anchorCi);
    gc_i64 step = vfGrid, origin = profile_assist_detail::mod_pos(anchorBaseHz, vfGrid);
    if (in.lockMode == LOCK_MODE_HARD) {
        if (!profile_assist_detail::intersect_congruences(0, 1000000LL, anchorBaseHz, vfGrid, step, origin)) {
            ASSIST_FAIL(PROFILE_ASSIST_NO_COMPATIBLE_TARGET,
                "hard pin 1 MHz grid conflicts with anchor baseline grid; no compatible target exists");
        }
    }

    const gc_i64 minHz = (gc_i64)in.range.minKHz * 1000LL;
    const gc_i64 maxHz = (gc_i64)in.range.maxKHz * 1000LL;
    const gc_i64 requestedHz = source.lock.targetFrequencyHz;
    gc_i64 hi = requestedHz, lo = 1LL;
    const bool checkPrefix = in.lockMode != LOCK_MODE_HARD || in.prefixAppliesToHardLock;

    if (baselineProves) {
        hi = std::min(hi, anchorBaseHz + maxHz);
        lo = std::max(lo, anchorBaseHz + minHz);
        for (int ci = anchorCi + 1; ci < in.floor.graphicsEnd && ci < VF_NUM_POINTS; ++ci) {
            if (!in.curve[ci].freq_kHz) continue;
            const gc_i64 tb = curve_plan_base_hz(in, ci);
            lo = std::max(lo, tb + minHz);
            if (in.lockMode == LOCK_MODE_HARD) hi = std::min(hi, tb + maxHz);
        }
        if (checkPrefix) {
            for (int ci = 0; ci < anchorCi && ci < in.floor.graphicsEnd; ++ci) {
                if (!in.curve[ci].freq_kHz) continue;
                if (!allowPrefix) {
                    const auto t = curve_plan_prefix_point_target(in, curve_plan_base_hz(in, ci) / 1000LL, ci, true);
                    lo = std::max(lo, t.targetHz);
                } else {
                    if (vf_point_in_flat_floor(in.floor, in.curve, ci) || !vf_point_listed(in.floor, in.curve, ci))
                        lo = std::max(lo, (gc_i64)in.floor.floorMHz * 1000LL);
                    else lo = std::max(lo, curve_plan_base_hz(in, ci) + minHz);
                }
            }
        }
    } else if (checkPrefix && !allowPrefix) {
        for (int ci = 0; ci < anchorCi && ci < in.floor.graphicsEnd; ++ci) {
            if (!in.curve[ci].freq_kHz) continue;
            const auto t = curve_plan_prefix_point_target(in, curve_plan_base_hz(in, ci) / 1000LL, ci, false);
            if (t.kind == CURVE_PLAN_POINT_ABSOLUTE) lo = std::max(lo, t.targetHz);
        }
    }

    const ProfileGridProjection hiProj = profile_grid_lower(hi, {true, step, origin});
    const gc_i64 loRem = profile_assist_detail::mod_pos(lo - origin, step);
    const gc_i64 loGrid = loRem ? lo + (step - loRem) : lo;
    if (!hiProj.valid || lo > hi || loGrid > hiProj.lower) {
        if (baselineProves || lo > requestedHz) {
            snprintf(why, sizeof(why),
                "No compatible plateau <= %lld Hz exists under current bounds (required >= %lld Hz, anchor cap %lld Hz).",
                requestedHz, lo, baselineProves ? anchorBaseHz + maxHz : requestedHz);
            ASSIST_FAIL(PROFILE_ASSIST_NO_COMPATIBLE_TARGET, why);
        }
    }

    auto workDesired = std::make_unique<DesiredSettings>(*baseRuntime);
    auto candDoc = std::make_unique<ProfileDocument>();
    auto checkDoc = std::make_unique<ProfileDocument>();
    auto fullRt = std::make_unique<DesiredSettings>();
    auto fullStorage = std::make_unique<ProfileCaptureCurvePlan>();
    auto fullMap = std::make_unique<ProfileMappingReport>();
    CurvePlanInput workIn = in;
    workIn.desired = workDesired.get();

    unsigned int evals = 0;
    bool budgetExhausted = false, sawBaselineRequired = false;
    gc_i64 foundHz = 0;
    auto foundRepair = std::make_unique<CurvePrefixRepair>();
    CurvePlanResult foundPlan{};
    for (gc_i64 candHz = hiProj.lower; candHz >= loGrid;) {
        if (evals >= maxCandidates) { budgetExhausted = true; break; }
        ++evals;
        workDesired->authoring.lockPrecise = 1;
        workDesired->authoring.lockFrequencyHz = candHz;
        workDesired->lockMHz = curve_plan_display_hz(candHz);
        bool ok = false;
        auto candRep = std::make_unique<CurvePrefixRepair>();
        CurvePlanResult candPlan{};
        if (!allowPrefix) {
            candPlan = curve_plan_evaluate(workIn);
            ok = acceptReady(candPlan);
        } else {
            *candRep = curve_prefix_repair_prepare(workIn, true);
            if (candRep->status == CURVE_PREFIX_REPAIR_NOT_NEEDED) {
                candPlan = curve_plan_evaluate(workIn);
                ok = acceptReady(candPlan);
            } else if (candRep->status == CURVE_PREFIX_REPAIR_PREPARED) {
                candPlan = candRep->after;
                ok = acceptReady(candPlan);
            } else if (candRep->status == CURVE_PREFIX_REPAIR_BASELINE_REQUIRED ||
                       candRep->status == CURVE_PREFIX_REPAIR_CONVERSION_REQUIRED) {
                sawBaselineRequired = true;
            }
        }
        if (ok && profile_assist_detail::apply_assist_to_doc(source, mapping, *candRep, candHz, *candDoc)) {
            profile_assist_detail::isolate_curve_doc(*candDoc, *checkDoc);
            if (profile_resolve_runtime(*checkDoc, destination, sourceRevision + 1, routes, *fullRt, *fullMap, nullptr, 0) &&
                profile_capture_curve_plan_input(capture, *fullRt, *fullStorage)) {
                const CurvePlanResult fullRes = curve_plan_evaluate(fullStorage->input);
                if (acceptReady(fullRes)) {
                    foundHz = candHz;
                    *foundRepair = *candRep;
                    foundPlan = fullRes;
                    out->previewDocument = *candDoc;
                    break;
                }
            }
        }
        if (candHz - step > candHz || candHz < loGrid + step) break; // prevent signed underflow
        candHz -= step;
    }

    out->candidatesEvaluated = evals;
    out->budgetExhausted = budgetExhausted;
    if (foundHz > 0) {
        out->suggestedLockHz = foundHz;
        out->repair = *foundRepair;
        out->prefixChangedCount = foundRepair->changedCount;
        out->conversionCount = foundRepair->conversionCount;
        out->planAfter = foundPlan;
        out->proven = baselineProves && foundPlan.disposition == CURVE_PLAN_REPRESENTABLE;
        out->status = out->proven ? PROFILE_ASSIST_PREPARED : PROFILE_ASSIST_ESTIMATE;
        profile_assist_detail::format_preview_explanation(*out, *foundRepair);
        profile_assist_detail::finalize_summary(*out, evals);
        result = *out;
        return;
    }

    if (budgetExhausted) {
        snprintf(why, sizeof(why),
            "Search budget of %u descending candidates exhausted without proving an empty feasible set.", evals);
        ASSIST_FAIL(PROFILE_ASSIST_INDETERMINATE, why);
    }
    if (sawBaselineRequired && !baselineProves)
        ASSIST_FAIL(PROFILE_ASSIST_BASELINE_REQUIRED,
            "Cannot adjust captured/projected prefix points over a shaped live curve; reset to stock and retry.");
    if (!baselineProves)
        ASSIST_FAIL(PROFILE_ASSIST_INDETERMINATE,
            "No compatible estimate established under shaped live baseline; reset to stock to prove feasibility.");
    snprintf(why, sizeof(why),
        "No compatible target <= %lld Hz exists across all %u evaluated grid candidates under current restrictions.",
        requestedHz, evals);
    fail(PROFILE_ASSIST_NO_COMPATIBLE_TARGET, why);
    #undef ASSIST_FAIL
}

static inline bool profile_curve_assistance_accept(
    const ProfileCurveAssistance& assist,
    const ProfileDocument& current,
    gc_u64 sourceRevision,
    const HardwareCaptureObservation& freshCapture,
    const ProfileDestinationTopology& freshDestination,
    ProfileRuntimeRoutes routes,
    ProfileDocument& derived,
    char* why, size_t whySize) {
    const auto fail = [&](const char* msg) {
        if (why && whySize) snprintf(why, whySize, "%s", msg);
        return false;
    };
    if (assist.status != PROFILE_ASSIST_PREPARED && assist.status != PROFILE_ASSIST_ESTIMATE)
        return fail("assistance result does not hold an acceptable candidate");
    if (!sourceRevision || !assist.binding.sourceRevision || sourceRevision != assist.binding.sourceRevision ||
        !freshCapture.valid || freshCapture.gpuGeneration != assist.binding.gpuGeneration ||
        freshCapture.revision != assist.binding.captureRevision ||
        freshDestination.revision != assist.binding.destinationRevision ||
        !profile_document_semantically_equal(current, assist.binding.expectedSourceSnapshot)) {
        return fail("source draft or destination state changed since preparation; review again");
    }
    auto candidate = std::make_unique<ProfileDocument>(assist.previewDocument);
    snprintf(candidate->displayName, sizeof(candidate->displayName), "%s", current.displayName);
    auto checkDoc = std::make_unique<ProfileDocument>();
    profile_assist_detail::isolate_curve_doc(*candidate, *checkDoc);
    auto rt = std::make_unique<DesiredSettings>();
    auto map = std::make_unique<ProfileMappingReport>();
    if (!profile_resolve_runtime(*checkDoc, freshDestination, sourceRevision + 1, routes, *rt, *map, why, whySize))
        return false;
    auto storage = std::make_unique<ProfileCaptureCurvePlan>();
    if (!profile_capture_curve_plan_input(freshCapture, *rt, *storage))
        return fail("fresh destination baseline cannot validate prepared curve");
    const CurvePlanResult res = curve_plan_evaluate(storage->input);
    if (res.disposition == CURVE_PLAN_REFUSED || res.disposition == CURVE_PLAN_UNAVAILABLE)
        return fail("prepared curve is refused by fresh destination planner");
    derived = *candidate;
    return true;
}
