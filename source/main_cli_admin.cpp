// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// Administrator-only machine-wide CLI commands, split out of entry.cpp.
//
// These six switches all do the same shape of work — mutate machine-wide state
// under HKLM/%ProgramData% and report one sentence — and together they were the
// largest block in handle_cli().  Collecting them behind one dispatcher keeps
// the CLI entry point readable and gives every command the same success/failure
// reporting instead of six near-identical copies of it.
//
// The dispatcher answers "did this command line name one of my commands", so
// the caller keeps ownership of logging and the process exit code.

#ifndef GREEN_CURVE_SERVICE_BINARY

// Returns true when `opts` selects one of these commands.  `*okOut` then says
// whether it succeeded and `message` carries the line to print.
static bool cli_handle_machine_admin_command(const CliOptions* opts, bool* okOut,
                                             char* message, size_t messageSize) {
    if (!opts || !okOut || !message || messageSize == 0) return false;
    message[0] = 0;
    *okOut = false;
    char err[256] = {};

    if (opts->setMachineLogonSlot || opts->clearMachineLogonSlot) {
        *okOut = opts->clearMachineLogonSlot
            ? clear_machine_logon_slot(err, sizeof(err))
            : set_machine_logon_slot(opts->machineLogonSlotValue, err, sizeof(err));
        if (!*okOut) {
            set_message(message, messageSize, "%s", err[0] ? err : "Machine logon profile update failed");
        } else if (opts->clearMachineLogonSlot) {
            set_message(message, messageSize, "Cleared the machine-wide default logon profile.");
        } else {
            set_message(message, messageSize, "Set the machine-wide default logon profile to slot %d.",
                opts->machineLogonSlotValue);
        }
        return true;
    }

    if (opts->publishSlotToMachine || opts->clearMachineSlot) {
        *okOut = opts->clearMachineSlot
            ? clear_machine_profile_slot(opts->machineSlotValue, err, sizeof(err))
            : copy_profile_slot_to_machine_config(g_app.configPath, opts->machineSlotValue, err, sizeof(err));
        if (!*okOut) {
            set_message(message, messageSize, "%s", err[0] ? err : "Machine profile bank update failed");
        } else if (opts->clearMachineSlot) {
            set_message(message, messageSize, "Cleared machine-wide profile slot %d.", opts->machineSlotValue);
        } else {
            set_message(message, messageSize, "Published profile slot %d to the machine-wide profile bank.",
                opts->machineSlotValue);
        }
        return true;
    }

    if (opts->setRestrictPolicy) {
        *okOut = set_machine_restrict_policy(opts->restrictPolicyValue != 0, err, sizeof(err));
        if (!*okOut) {
            set_message(message, messageSize, "%s", err[0] ? err : "Shared-only policy update failed");
        } else {
            set_message(message, messageSize, "%s", opts->restrictPolicyValue != 0
                ? "Enabled shared-only policy: standard (non-admin) users may only apply shared profiles."
                : "Disabled shared-only policy: standard users may apply custom settings again.");
        }
        return true;
    }

    // One coherent share/unshare: --share-slot publishes the slot's full profile
    // data into the shared bank AND sets it as the all-users default logon
    // profile (so users without their own logon profile receive it). --unshare
    // reverses both.  This backs the GUI "Share with all users" checkbox.
    if (opts->shareSlot || opts->unshareSlot) {
        *okOut = opts->unshareSlot
            ? unshare_profile_slot_for_all_users(opts->shareSlotValue, err, sizeof(err))
            : share_profile_slot_for_all_users(g_app.configPath, opts->shareSlotValue, err, sizeof(err));
        if (!*okOut) {
            set_message(message, messageSize, "%s", err[0] ? err : "Share update failed");
        } else if (opts->unshareSlot) {
            set_message(message, messageSize,
                "Unshared profile slot %d (removed from the shared bank and the all-users default).",
                opts->shareSlotValue);
        } else {
            set_message(message, messageSize,
                "Shared profile slot %d with all users (published + set as the all-users default logon profile).",
                opts->shareSlotValue);
        }
        return true;
    }

    return false;
}

#endif // GREEN_CURVE_SERVICE_BINARY
