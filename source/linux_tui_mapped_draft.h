// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#pragma once
#include "linux_tui_internal.h"
#include "profile_mapped_draft.h"

static inline bool tui_mapped_draft_available(const TuiState &state, bool accepted = true) {
    return state.document.valid && state.sourceReview && state.sourceReview->accepted == accepted &&
           !state.edit.active && !state.capture.active && !state.transfer.active && !state.profileLoadPending &&
           state.serviceOnline && state.service.state.gpuPhase == SERVICE_GPU_PHASE_READY &&
           (state.service.state.validSections & SERVICE_STATE_SECTION_READY_REQUIRED) ==
               SERVICE_STATE_SECTION_READY_REQUIRED &&
           state.service.state.topologySignature &&
           linux_tui_review_context_matches(state.sourceReviewContext, state.service) &&
           linux_tui_shown_gpu_write(&state.configuredGpu, linux_tui_authoritative_gpu(&state.service)) ==
               LINUX_TUI_SHOWN_GPU_WRITE_ALLOWED &&
           profile_mapped_draft_ready(*state.sourceReview, state.document.value, state.document.revision);
}
static inline bool tui_use_mapped_draft(TuiState &state) {
    if (!state.sourceCheckVisible || !tui_mapped_draft_available(state, false) ||
        !profile_mapped_draft_accept(*state.sourceReview, state.document.value, state.document.revision)) {
        snprintf(state.status, sizeof(state.status),
                 "Check the current source and destination before using a mapped draft");
        return false;
    }
    snprintf(state.status, sizeof(state.status),
             "Mapped draft staged. Apply submits exact settings for fresh daemon validation.");
    return true;
}
static inline const char* tui_conversion_short_name(ProfileConversionKind kind) {
    switch (kind) {
        case PROFILE_CONVERT_VF_OFFSETS: return "vf-off";
        case PROFILE_CONVERT_VF_FREQUENCIES: return "vf-mhz";
        case PROFILE_CONVERT_GRAPHICS: return "graphics";
        case PROFILE_CONVERT_MEMORY: return "memory";
        case PROFILE_CONVERT_XBAR_CLOCK: return "xbar-clk";
        case PROFILE_CONVERT_SYSTEM_CLOCK: return "sys-clk";
        case PROFILE_CONVERT_VIDEO_CLOCK: return "vid-clk";
        case PROFILE_CONVERT_POWER: return "power";
        case PROFILE_CONVERT_XBAR_VOLTAGE: return "voltage";
        case PROFILE_CONVERT_LOCK_FREQUENCY: return "lock-mhz";
    }
    return "domain";
}
static inline bool tui_source_conversion_context_ready(const TuiState& state) {
    return state.document.valid && state.sourceCheckVisible && state.sourceReview &&
           state.sourceReview->sourceRevision == state.document.revision &&
           state.sourceReview->stage >= PROFILE_REVIEW_CAPTURE &&
           !state.edit.active && !state.capture.active && !state.transfer.active && !state.profileLoadPending &&
           state.serviceOnline && state.service.state.gpuPhase == SERVICE_GPU_PHASE_READY &&
           (state.service.state.validSections & SERVICE_STATE_SECTION_READY_REQUIRED) ==
               SERVICE_STATE_SECTION_READY_REQUIRED &&
           state.service.state.topologySignature &&
           linux_tui_review_context_matches(state.sourceReviewContext, state.service) &&
           linux_tui_shown_gpu_write(&state.configuredGpu, linux_tui_authoritative_gpu(&state.service)) ==
               LINUX_TUI_SHOWN_GPU_WRITE_ALLOWED;
}
static inline bool tui_cycle_source_conversion(TuiState& state) {
    if (!tui_source_conversion_context_ready(state) || !state.sourceReview->conversions.count) {
        snprintf(state.status, sizeof(state.status), "Check the source and destination to review domain conversions");
        return false;
    }
    const int n = state.sourceReview->conversions.count;
    state.sourceConvertIndex = ((state.sourceConvertIndex + 1) % n + n) % n;
    const auto& item = state.sourceReview->conversions.items[state.sourceConvertIndex];
    if (item.convertible) {
        char summary[256]{};
        profile_conversion_summary(item.batch, PROFILE_CONVERSION_PREVIEW, summary, sizeof(summary));
        snprintf(state.status, sizeof(state.status), "%s", summary);
    } else {
        snprintf(state.status, sizeof(state.status), "Conversion %s unresolved: %s",
            profile_conversion_kind_key(item.kind), item.why);
    }
    return true;
}
static inline bool tui_accept_source_conversion(TuiState& state, char* logLine = nullptr, size_t logSize = 0) {
    if (logLine && logSize) logLine[0] = 0;
    if (!tui_source_conversion_context_ready(state) || !state.sourceReview->conversions.count) {
        snprintf(state.status, sizeof(state.status), "Check the current source and destination before converting a domain");
        return false;
    }
    const int n = state.sourceReview->conversions.count;
    const int idx = state.sourceConvertIndex >= 0 && state.sourceConvertIndex < n ? state.sourceConvertIndex : 0;
    const auto& item = state.sourceReview->conversions.items[idx];
    const ProfileConversionKind kind = item.kind;
    ProfileDestinationTopology dest{}; char why[256]{}, summary[256]{};
    if (!profile_destination_from_capture(state.sourceReview->capture, dest, why, sizeof(why))) {
        snprintf(state.status, sizeof(state.status), "%s", why[0] ? why : "Capture invalid; Check again");
        return false;
    }
    auto derived = std::make_unique<ProfileDocument>();
    if (!profile_conversion_accept_item(item, state.document.value, state.document.revision,
            state.sourceReview->capture, dest, state.sourceReview->routes, *derived,
            summary, sizeof(summary), why, sizeof(why))) {
        if (logLine && logSize) snprintf(logLine, logSize, "%s", summary);
        snprintf(state.status, sizeof(state.status), "%s", why[0] ? why : "Domain conversion refused");
        return false;
    }
    if (logLine && logSize) snprintf(logLine, logSize, "%s", summary);
    state.document.value = *derived;
    ++state.document.revision;
    state.sourceReview.reset(); state.sourceCheckVisible = false; state.sourceCheckLines.clear();
    state.sourceConvertIndex = 0; state.sourceScroll = 0;
    snprintf(state.status, sizeof(state.status),
             "Converted %s in source draft (saved slot unchanged); Check again before Apply",
             profile_conversion_kind_key(kind));
    return true;
}
static inline bool tui_restore_captured_tail(TuiState& state, char* logLine = nullptr, size_t logSize = 0) {
    if (logLine && logSize) logLine[0] = 0;
    if (!state.document.valid || state.edit.active || state.capture.active || state.transfer.active) {
        snprintf(state.status, sizeof(state.status), "Finish current source operation before restoring captured tail");
        return false;
    }
    char why[256]{};
    auto restored = std::make_unique<ProfileDocument>();
    if (!profile_document_restore_captured_tail(state.document.value, *restored, why, sizeof(why))) {
        if (logLine && logSize) snprintf(logLine, logSize, "profile-source: restore tail refused reason=%s", why);
        snprintf(state.status, sizeof(state.status), "%s", why[0] ? why : "Cannot restore captured tail");
        return false;
    }
    state.document.value = *restored;
    ++state.document.revision;
    state.sourceReview.reset(); state.sourceCheckVisible = false; state.sourceCheckLines.clear();
    state.sourceConvertIndex = 0; state.sourceScroll = 0;
    if (logLine && logSize) {
        snprintf(logLine, logSize, "profile-source: restored exact captured tail revision=%llu",
            (unsigned long long)state.document.revision);
    }
    snprintf(state.status, sizeof(state.status),
             "Restored exact captured VF tail offsets without recapture (saved slot unchanged)");
    return true;
}
static inline void tui_cycle_assist_mode(TuiState& state) {
    state.sourceAssistMode = (state.sourceAssistMode + 1) % 3;
    state.sourceAssistPending.reset();
    snprintf(state.status, sizeof(state.status), "Curve assistance mode: %s",
        profile_curve_assist_mode_label((ProfileCurveAssistMode)state.sourceAssistMode));
}
static inline bool tui_prepare_curve_assist(TuiState& state, char* logLine = nullptr, size_t logSize = 0) {
    if (logLine && logSize) logLine[0] = 0;
    state.sourceAssistPending.reset();
    if (!tui_source_conversion_context_ready(state) || !state.document.value.lock.present) {
        snprintf(state.status, sizeof(state.status),
            "Check a lock-containing source draft on the shown GPU before preparing curve");
        return false;
    }
    ProfileDestinationTopology dest{}; char why[256]{};
    if (!profile_destination_from_capture(state.sourceReview->capture, dest, why, sizeof(why))) {
        snprintf(state.status, sizeof(state.status), "%s", why[0] ? why : "Capture invalid; Check again");
        return false;
    }
    auto assist = std::make_unique<ProfileCurveAssistance>();
    profile_curve_assistance_prepare(state.document.value, state.sourceReview->capture, dest,
        state.document.revision, state.sourceReview->routes, (ProfileCurveAssistMode)state.sourceAssistMode, *assist);
    if (logLine && logSize) snprintf(logLine, logSize, "%s", assist->summary);
    state.sourceCheckLines.push_back(assist->summary);
    {
        const char* p = assist->explanation;
        while (*p) {
            const char* nl = strchr(p, '\n');
            if (nl) { state.sourceCheckLines.emplace_back(p, (size_t)(nl - p)); p = nl + 1; }
            else { state.sourceCheckLines.emplace_back(p); break; }
        }
    }
    state.sourceCheckVisible = true;
    state.sourceScroll = 0;
    if (assist->status == PROFILE_ASSIST_PREPARED || assist->status == PROFILE_ASSIST_ESTIMATE) {
        const bool proven = assist->proven;
        state.sourceAssistPending = std::move(assist);
        snprintf(state.status, sizeof(state.status), "%s",
            proven ? "Prepared curve preview ready; use ACCEPT ASSIST to update draft only"
                   : "Estimated curve preview ready (reset baseline required); use ACCEPT ASSIST to stage");
        return true;
    }
    snprintf(state.status, sizeof(state.status), "%s", assist->explanation);
    return false;
}
static inline bool tui_accept_curve_assist(TuiState& state, char* logLine = nullptr, size_t logSize = 0) {
    if (logLine && logSize) logLine[0] = 0;
    if (!tui_source_conversion_context_ready(state) || !state.sourceAssistPending) {
        snprintf(state.status, sizeof(state.status), "Prepare a compatible curve preview before accepting");
        return false;
    }
    ProfileDestinationTopology dest{}; char why[256]{};
    if (!profile_destination_from_capture(state.sourceReview->capture, dest, why, sizeof(why))) {
        snprintf(state.status, sizeof(state.status), "%s", why[0] ? why : "Capture invalid; Check again");
        return false;
    }
    auto derived = std::make_unique<ProfileDocument>();
    if (!profile_curve_assistance_accept(*state.sourceAssistPending, state.document.value, state.document.revision,
            state.sourceReview->capture, dest, state.sourceReview->routes, *derived, why, sizeof(why))) {
        snprintf(state.status, sizeof(state.status), "%s", why[0] ? why : "Prepared curve stale; Check again");
        return false;
    }
    const bool proven = state.sourceAssistPending->proven;
    if (logLine && logSize) snprintf(logLine, logSize, "accepted %s", state.sourceAssistPending->summary);
    state.document.value = *derived;
    ++state.document.revision;
    state.sourceReview.reset(); state.sourceAssistPending.reset();
    state.sourceCheckVisible = false; state.sourceCheckLines.clear();
    state.sourceConvertIndex = 0; state.sourceScroll = 0;
    snprintf(state.status, sizeof(state.status), "%s",
        proven ? "Accepted prepared curve in draft (saved slot unchanged); Check before Apply"
               : "Accepted curve estimate in draft (reset baseline required at Apply); saved slot unchanged");
    return true;
}
