// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT

#ifndef GREEN_CURVE_GUI_MUTATION_QUEUE_POLICY_H
#define GREEN_CURVE_GUI_MUTATION_QUEUE_POLICY_H

enum GuiMutationKind {
    GUI_MUTATION_APPLY = 1,
    GUI_MUTATION_RESET = 2,
};

enum GuiMutationUiContext {
    GUI_MUTATION_CONTEXT_MANUAL_APPLY = 1,
    GUI_MUTATION_CONTEXT_MANUAL_RESET = 2,
    GUI_MUTATION_CONTEXT_AUTO_PROFILE = 3,
    GUI_MUTATION_CONTEXT_APP_LAUNCH = 4,
};

enum GuiMutationQueueDecision {
    GUI_MUTATION_QUEUE_START = 1,
    GUI_MUTATION_QUEUE_PENDING = 2,
    GUI_MUTATION_QUEUE_REPLACE_PENDING = 3,
    GUI_MUTATION_QUEUE_KEEP_PENDING_RESET = 4,
};

inline GuiMutationQueueDecision gui_mutation_queue_decide(bool active,
    bool hasPending, GuiMutationKind pendingKind, GuiMutationKind incomingKind) {
    if (!active) return GUI_MUTATION_QUEUE_START;
    if (!hasPending) return GUI_MUTATION_QUEUE_PENDING;
    // Reset is a safety action. Once it is waiting behind an active hardware
    // write, a later Apply cannot overtake or discard it.
    if (pendingKind == GUI_MUTATION_RESET && incomingKind == GUI_MUTATION_APPLY)
        return GUI_MUTATION_QUEUE_KEEP_PENDING_RESET;
    return GUI_MUTATION_QUEUE_REPLACE_PENDING;
}

#endif
