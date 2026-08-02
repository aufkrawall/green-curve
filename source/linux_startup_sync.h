// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// linux_startup_sync.h — client half of the boot-apply snapshot invariant.
//
// The daemon boot-applies a *snapshot* of a profile because `ProtectHome=yes`
// hides the user's config.ini from it (see startup_snapshot_policy.h).  Whoever
// writes a profile slot therefore owes the daemon the new content, and whoever
// shows "what boots" owes the user the truth about whether the two still agree.
//
// Both the TUI and the CLI need exactly that, so the config-loading,
// daemon-calling half lives here once instead of twice.  The decisions
// themselves stay pure in startup_snapshot_policy.h.

#ifndef GREEN_CURVE_LINUX_STARTUP_SYNC_H
#define GREEN_CURVE_LINUX_STARTUP_SYNC_H

#include "startup_snapshot_policy.h"

#include <stdio.h>

struct LinuxStartupSyncResult {
    StartupSnapshotSync action;
    // False only when something was owed and could not be delivered, so the
    // boot-apply is knowingly out of date. NONE is always ok.
    bool ok;
    char message[256];
};

// Push profile `slot` to the daemon when the boot-apply policy is bound to it.
// `slotStillHasContent` is false when the slot was cleared, which unbinds the
// policy instead of leaving it applying a deleted profile.
LinuxStartupSyncResult linux_startup_sync_after_profile_write(
    const char* configPath, int slot, bool slotStillHasContent,
    bool daemonReachable, unsigned int policyMode, unsigned int policySlot);

struct LinuxStartupSnapshotReport {
    StartupSnapshotState state;
    unsigned int slot;
    bool snapshotKnown;
    bool profileLoaded;
    DesiredSettings snapshot;  // what the daemon would write at the next boot
    DesiredSettings profile;   // what the named slot holds on disk right now
    char detail[256];
};

// Read the daemon's stored snapshot back and compare it with the profile the
// policy names. Never claims agreement it did not verify.
LinuxStartupSnapshotReport linux_startup_snapshot_report(
    const char* configPath, bool daemonReachable, unsigned int policyMode,
    unsigned int policySlot);

// Human-readable form of the report for `--show-startup`. Prints the two sides
// field by field when they diverge, because "they differ" without saying how is
// exactly the answer that sent this bug to a hex dump of startup.bin.
void print_startup_snapshot_report(FILE* out,
                                   const LinuxStartupSnapshotReport* report,
                                   const char* configPath);

#endif  // GREEN_CURVE_LINUX_STARTUP_SYNC_H
