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
