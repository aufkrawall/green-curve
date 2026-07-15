// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT

#ifndef GREEN_CURVE_GUI_DRAFT_POLICY_H
#define GREEN_CURVE_GUI_DRAFT_POLICY_H

enum GuiDraftReconcileDecision {
    GUI_DRAFT_REBASE_CLEAN = 0,
    GUI_DRAFT_ATTACH_DIRTY = 1,
    GUI_DRAFT_DETACH_DIRTY = 2,
};

static inline GuiDraftReconcileDecision gui_draft_reconcile_decide(
    bool dirty, bool gpuMatches, bool topologyMatches) {
    if (!dirty) return GUI_DRAFT_REBASE_CLEAN;
    return gpuMatches && topologyMatches
        ? GUI_DRAFT_ATTACH_DIRTY : GUI_DRAFT_DETACH_DIRTY;
}

#endif // GREEN_CURVE_GUI_DRAFT_POLICY_H
