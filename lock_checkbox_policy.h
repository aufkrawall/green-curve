// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT

#ifndef GREEN_CURVE_LOCK_CHECKBOX_POLICY_H
#define GREEN_CURVE_LOCK_CHECKBOX_POLICY_H

#include <stdint.h>

#include "gpu_core.h"

// Pure policy for the owner-drawn VF lock control.  Keeping notification and
// gesture decisions outside WndProc makes the safety properties executable in
// the regression harness instead of relying only on source-text guards.
struct LockUiStateStamp {
    int lockedVi;
    int lockedCi;
    unsigned int lockedFreq;
    LockMode lockMode;
};

static inline bool lock_ui_state_stamp_equal(const LockUiStateStamp& a,
                                             const LockUiStateStamp& b) {
    return a.lockedVi == b.lockedVi &&
           a.lockedCi == b.lockedCi &&
           a.lockedFreq == b.lockedFreq &&
           a.lockMode == b.lockMode;
}

static inline LockMode lock_mode_after_activation(bool samePoint, LockMode currentMode) {
    if (!samePoint) return LOCK_MODE_FLATTEN;
    if (currentMode == LOCK_MODE_FLATTEN) return LOCK_MODE_HARD;
    return LOCK_MODE_NONE;
}

enum LockActivationDecision {
    LOCK_ACTIVATION_IGNORE_NOTIFICATION = 0,
    LOCK_ACTIVATION_ACCEPT_UNARMED,
    LOCK_ACTIVATION_ACCEPT_ARMED,
    LOCK_ACTIVATION_REJECT_WRONG_CONTROL,
    LOCK_ACTIVATION_REJECT_ALREADY_CONSUMED,
    LOCK_ACTIVATION_REJECT_STATE_CHANGED,
};

static inline LockActivationDecision decide_lock_activation(
        unsigned int notificationCode,
        unsigned int clickedNotificationCode,
        bool gestureArmed,
        bool gestureConsumed,
        int gestureVi,
        int commandVi,
        const LockUiStateStamp& pressState,
        const LockUiStateStamp& currentState) {
    if (notificationCode != clickedNotificationCode) {
        return LOCK_ACTIVATION_IGNORE_NOTIFICATION;
    }
    // BM_CLICK, accessibility providers, and dialog keyboard activation may
    // legitimately produce BN_CLICKED without a raw mouse/key gesture.
    if (!gestureArmed) return LOCK_ACTIVATION_ACCEPT_UNARMED;
    if (gestureVi != commandVi) return LOCK_ACTIVATION_REJECT_WRONG_CONTROL;
    if (gestureConsumed) return LOCK_ACTIVATION_REJECT_ALREADY_CONSUMED;
    if (!lock_ui_state_stamp_equal(pressState, currentState)) {
        return LOCK_ACTIVATION_REJECT_STATE_CHANGED;
    }
    return LOCK_ACTIVATION_ACCEPT_ARMED;
}

static inline const char* lock_activation_decision_name(LockActivationDecision decision) {
    switch (decision) {
        case LOCK_ACTIVATION_ACCEPT_UNARMED: return "accept-unarmed";
        case LOCK_ACTIVATION_ACCEPT_ARMED: return "accept-armed";
        case LOCK_ACTIVATION_REJECT_WRONG_CONTROL: return "reject-wrong-control";
        case LOCK_ACTIVATION_REJECT_ALREADY_CONSUMED: return "reject-consumed";
        case LOCK_ACTIVATION_REJECT_STATE_CHANGED: return "reject-state-changed";
        default: return "ignore-notification";
    }
}

#endif
