// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#pragma once

static inline void tui_expire_source_review(TuiState& state, const char* reason) {
    if (!state.sourceReview && !state.sourceCheckVisible) return;
    state.sourceReview.reset(); state.sourceReviewContext = {};
    state.sourceCheckVisible = false; state.sourceCheckLines.clear(); state.sourceScroll = 0;
    std::snprintf(state.status, sizeof(state.status), "Destination Check expired: %s; source intent is preserved", reason);
    linux_debug_logf("profile-check: source=tui expired=%s", reason);
}
static inline void tui_reconcile_source_review(TuiState& state) {
    if (!state.sourceReview) return;
    if (!state.document.valid || state.document.revision != state.sourceReview->sourceRevision ||
        !state.serviceOnline || !linux_tui_review_context_matches(state.sourceReviewContext, state.service) ||
        linux_tui_shown_gpu_write(&state.configuredGpu, linux_tui_authoritative_gpu(&state.service)) != LINUX_TUI_SHOWN_GPU_WRITE_ALLOWED)
        tui_expire_source_review(state, "draft or GPU session changed");
}
