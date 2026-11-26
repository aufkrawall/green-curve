// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// Per-transaction F-APPLY-CEILING state for one Linux apply or reset.
//
// The caller owns one value per hardware transaction and passes it explicitly
// to linux_backend_apply()/linux_backend_reset() and, when a successful write
// cannot be committed, to the linux_backend_restore_snapshot() that undoes it.
// The backend resets it on transaction entry. It deliberately is not stored in
// LinuxGpuState or in any process-wide variable: every reader is handed the
// transaction it belongs to, so two transactions -- or two GPUs -- cannot
// observe each other's clamp. See llm-wiki/clock-transition-audit.md.
#ifndef GREEN_CURVE_LINUX_APPLY_CEILING_TRANSACTION_H
#define GREEN_CURVE_LINUX_APPLY_CEILING_TRANSACTION_H

#include "apply_clock_ceiling_policy.h"

struct LinuxApplyCeilingTransaction {
    // A scoped request (or VF baseline reset) releases the curve ceiling, so
    // the plan treats it like a reset-to-stock transition.
    bool scopedCurveTransition = false;
    // What arming actually did, so the witness and the phase result can both
    // stop guessing (audit CT-07): the plan's `arm` flag is an intention, not
    // evidence of installation.
    ApplyClockCeilingArmResult armResult = APPLY_CEILING_ARM_NOT_NEEDED;
    bool armed = false;
    bool writeAttempted = false;
    // The plan AS ARMED, cached for the rest of the transaction. The reset and
    // curve phases change the outgoing state the plan is derived from, so a
    // recomputed plan would describe a half-done transition and the witness
    // would print a ceiling value that never existed.
    ApplyClockCeilingPlan plan = {};
    // CT-07: whether the state this transaction is leaving was a HARD pin.
    // Rollback cannot derive it -- a HARD profile's VF tail is deliberately raw
    // and high because the pin, not the curve, makes it safe.
    bool outgoingHadHardPin = false;
    // Rollback bound captured before the first clock-affecting write.
    unsigned int outgoingCeilingMHz = 0;
};

#endif  // GREEN_CURVE_LINUX_APPLY_CEILING_TRANSACTION_H
