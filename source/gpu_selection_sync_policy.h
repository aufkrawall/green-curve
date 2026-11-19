// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT

// Windows client <-> service GPU selection agreement.
//
// The client's saved [gpu] selection is the user's choice; the service's
// selected adapter is what every live value the client shows belongs to. They
// used to disagree silently: state reads named no GPU, so a selector choice
// was saved but the next snapshot put the service's GPU back on screen, while
// config-driven actions (Save, auto-profile, app-start, CLI) resolved the
// saved GPU and acted on a GPU that was not shown.
//
// Now a full state read names the GPU the client wants, as on Linux. The
// service switches only when gpu_intent_owner_policy.h allows it. A selector
// choice is saved only after the service shows that GPU, and a config-driven
// action refuses while the saved GPU and the shown GPU differ.

#pragma once

#include "gpu_adapter_identity.h"

// Whether a full state read that asked for `requested` was honoured: the
// service now shows that exact adapter.
static inline bool gpu_selection_sync_accepted(
    const GpuAdapterInfo* requested, const GpuAdapterInfo* shown,
    unsigned int adapterCount) {
    return requested && shown && requested->nvapiIndex == shown->nvapiIndex &&
        gpu_adapter_is_same_write_target(requested, shown, adapterCount);
}

enum GpuSelectorChoiceVerdict : unsigned int {
    // The chosen GPU is shown and already saved: nothing to do.
    GPU_SELECTOR_CHOICE_NO_CHANGE = 0,
    // Ask the service to show the chosen GPU; save only once it does.
    GPU_SELECTOR_CHOICE_REQUEST = 1,
    // The shown GPU holds the active intent: switching would be refused.
    GPU_SELECTOR_CHOICE_REFUSE_INTENT_HELD = 2,
    // Not a listed adapter.
    GPU_SELECTOR_CHOICE_INVALID = 3,
};

// `savedMatchesShown` is false when the saved selection is unresolved or
// names another GPU, so choosing the shown GPU still saves it.
static inline GpuSelectorChoiceVerdict gpu_selector_choice_verdict(
    unsigned int chosen, unsigned int shown, unsigned int adapterCount,
    bool savedMatchesShown, bool serviceHoldsIntent) {
    if (chosen >= adapterCount || adapterCount > MAX_GPU_ADAPTERS)
        return GPU_SELECTOR_CHOICE_INVALID;
    if (chosen == shown)
        return savedMatchesShown ? GPU_SELECTOR_CHOICE_NO_CHANGE
                                 : GPU_SELECTOR_CHOICE_REQUEST;
    return serviceHoldsIntent ? GPU_SELECTOR_CHOICE_REFUSE_INTENT_HELD
                              : GPU_SELECTOR_CHOICE_REQUEST;
}

// What a reconcile attempt of the saved selection was made against. A refused
// attempt is not repeated until one of these changes, so a held intent cannot
// make every full sync ask again (and a refusal cannot loop full syncs).
struct GpuSelectionAttemptKey {
    bool valid;
    gc_u64 serviceInstanceId;
    gc_u64 gpuGeneration;
    bool serviceHoldsIntent;
    unsigned int targetIndex;
};

static inline bool gpu_selection_attempt_key_equal(
    const GpuSelectionAttemptKey* a, const GpuSelectionAttemptKey* b) {
    return a && b && a->valid && b->valid &&
        a->serviceInstanceId == b->serviceInstanceId &&
        a->gpuGeneration == b->gpuGeneration &&
        a->serviceHoldsIntent == b->serviceHoldsIntent &&
        a->targetIndex == b->targetIndex;
}

enum GpuSelectionReconcileAction : unsigned int {
    // Saved and shown agree, or there is nothing to reconcile.
    GPU_SELECTION_RECONCILE_NONE = 0,
    // Ask the service to show the saved GPU.
    GPU_SELECTION_RECONCILE_REQUEST = 1,
    // Saved and shown differ, and the same attempt was already refused.
    GPU_SELECTION_RECONCILE_HELD = 2,
};

// After a READY full sync with no selector choice pending. `savedResolved`
// is true when the saved selection resolves stably to `savedIndex`.
static inline GpuSelectionReconcileAction gpu_selection_reconcile_decide(
    unsigned int adapterCount, bool savedResolved, unsigned int savedIndex,
    unsigned int shownIndex, const GpuSelectionAttemptKey* current,
    const GpuSelectionAttemptKey* lastRefused) {
    if (adapterCount <= 1 || adapterCount > MAX_GPU_ADAPTERS ||
        !savedResolved || savedIndex >= adapterCount ||
        savedIndex == shownIndex)
        return GPU_SELECTION_RECONCILE_NONE;
    return gpu_selection_attempt_key_equal(current, lastRefused)
        ? GPU_SELECTION_RECONCILE_HELD : GPU_SELECTION_RECONCILE_REQUEST;
}

// Whether a config-driven action may act on the saved GPU. On a multi-adapter
// system with authoritative live state, the saved GPU must be the shown one:
// otherwise the action would bind or apply values read from another GPU.
static inline bool gpu_selection_saved_matches_shown(bool shownAuthoritative,
    unsigned int adapterCount, unsigned int savedIndex,
    unsigned int shownIndex) {
    if (!shownAuthoritative || adapterCount <= 1) return true;
    return savedIndex == shownIndex;
}
