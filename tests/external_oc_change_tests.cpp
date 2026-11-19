// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
// Clock settings changed outside Green Curve: Reset decides from a live
// readback, and the GUI re-reads on activation without ever polling.
#include "gui_activation_resync_policy.h"
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

GuiActivationResyncInput ready_input() {
    GuiActivationResyncInput in;
    in.serviceClient = true;
    in.windowShown = true;
    in.modelReady = true;
    in.liveAuthority = true;
    in.nowMs = 100000;
    return in;
}

int activation_resync_cases() {
    GuiActivationResyncInput in = ready_input();
    if (gui_activation_resync_decide(in) != GUI_ACTIVATION_RESYNC_QUEUE) return 12386;

    in = ready_input(); in.serviceClient = false;
    if (gui_activation_resync_decide(in) != GUI_ACTIVATION_RESYNC_SKIP_NO_SERVICE) return 12387;
    // Tray-resident or minimized: never a read, so no background cost.
    in = ready_input(); in.windowShown = false;
    if (gui_activation_resync_decide(in) != GUI_ACTIVATION_RESYNC_SKIP_NOT_SHOWN) return 12388;
    in = ready_input(); in.modalActive = true;
    if (gui_activation_resync_decide(in) != GUI_ACTIVATION_RESYNC_SKIP_MODAL) return 12397;
    // Not READY: a preserve-presentation read is impossible, and the visible
    // transition must not be triggered by focus alone.
    in = ready_input(); in.modelReady = false;
    if (gui_activation_resync_decide(in) != GUI_ACTIVATION_RESYNC_SKIP_NOT_READY) return 12389;
    in = ready_input(); in.liveAuthority = false;
    if (gui_activation_resync_decide(in) != GUI_ACTIVATION_RESYNC_SKIP_NOT_READY) return 12390;
    // A queued or running write (or sync) owns the next fresh state.
    in = ready_input(); in.serviceWorkOutstanding = true;
    if (gui_activation_resync_decide(in) != GUI_ACTIVATION_RESYNC_SKIP_BUSY) return 12391;
    in = ready_input(); in.draftDetached = true;
    if (gui_activation_resync_decide(in) != GUI_ACTIVATION_RESYNC_SKIP_DETACHED_DRAFT) return 12392;

    // Fast switching reads once per interval; the boundary reads again.
    in = ready_input(); in.lastQueuedMs = in.nowMs - 1;
    if (gui_activation_resync_decide(in) != GUI_ACTIVATION_RESYNC_SKIP_RECENT) return 12393;
    in.lastQueuedMs = in.nowMs - GUI_ACTIVATION_RESYNC_MIN_INTERVAL_MS;
    if (gui_activation_resync_decide(in) != GUI_ACTIVATION_RESYNC_QUEUE) return 12394;
    // A tick source that went backwards cannot prove recency.
    in.lastQueuedMs = in.nowMs + 5;
    if (gui_activation_resync_decide(in) != GUI_ACTIVATION_RESYNC_QUEUE) return 12395;

    for (int d = GUI_ACTIVATION_RESYNC_QUEUE; d <= GUI_ACTIVATION_RESYNC_SKIP_RECENT; ++d)
        if (__builtin_strcmp(gui_activation_resync_decision_name(
                (GuiActivationResyncDecision)d), "unknown") == 0) return 12396;
    return 0;
}

}  // namespace

int run_external_oc_change_tests() {
    if (int failure = reset_live_readback_cases()) return failure;
    return activation_resync_cases();
}
