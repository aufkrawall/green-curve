// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
// Clock settings changed outside Green Curve: Reset decides from a live
// readback rather than from the last snapshot.
#include "reset_live_readback_policy.h"

namespace {

int reset_live_readback_cases() {
    // Another tool raised the memory offset after the last snapshot: the
    // snapshot says stock, the live read does not, so Reset writes.
    if (!reset_scalar_domain_needs_write(false, true, false, true)) return 12380;
    // The other tool already returned it to stock: no redundant write.
    if (reset_scalar_domain_needs_write(false, true, true, false)) return 12381;
    // The live read failed (a failed power read publishes the default): the
    // snapshot decides, so a raised target is still reset ...
    if (!reset_scalar_domain_needs_write(false, false, true, false)) return 12382;
    // ... and a snapshot at stock is not written on a failed read.
    if (reset_scalar_domain_needs_write(false, false, true, true)) return 12383;
    // An uncertain earlier write is always repeated, whatever the reads say.
    if (!reset_scalar_domain_needs_write(true, true, true, true)) return 12384;
    if (__builtin_strcmp(reset_scalar_decision_source(false, false), "cached") != 0 ||
        __builtin_strcmp(reset_scalar_decision_source(false, true), "live") != 0 ||
        __builtin_strcmp(reset_scalar_decision_source(true, false), "uncertain") != 0) return 12385;
    return 0;
}

}  // namespace

int run_external_oc_change_tests() {
    return reset_live_readback_cases();
}
