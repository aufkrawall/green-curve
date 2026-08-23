// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// CLI usage text.  Split out of entry.cpp, which sits at its size ratchet, when
// --self-test was added.  Pure output; the caller owns the log handle and the
// early-return.
//
// Included by main_shell.cpp ahead of entry.cpp.

static void cli_print_help(FILE* out) {
    if (!out) return;
    // Same timestamped shape as entry.cpp's CLI_LOG, kept local so this shard
    // does not depend on a macro defined around another file's log handle.
    #define HELP_LOG(...) do { \
        char _gc_ts[64] = {}; \
        format_log_timestamp_prefix(_gc_ts, sizeof(_gc_ts)); \
        fprintf(out, "%s", _gc_ts); \
        fprintf(out, __VA_ARGS__); \
    } while (0)

    HELP_LOG(APP_NAME " v" APP_VERSION " - NVIDIA VF Curve Editor\n");
    HELP_LOG("Usage:\n");
    HELP_LOG("  greencurve.exe              Launch GUI\n");
    HELP_LOG("  greencurve.exe --dump       Write VF curve to greencurve_cli_log.txt\n");
    HELP_LOG("  greencurve.exe --json       Write VF curve to greencurve_curve.json\n");
    HELP_LOG("  greencurve.exe --probe [--probe-output <path>]  Probe NvAPI/NVML/VF support and write a report\n");
    HELP_LOG("  greencurve.exe --self-test  Read-only driver/arch pre-flight, no service needed (run elevated)\n");
    HELP_LOG("  greencurve.exe --clk-domain-probe  Identify ClkDomains entries by briefly writing +50 MHz offsets, then restoring (run elevated)\n");
    HELP_LOG("  greencurve.exe --gpu-offset <mhz> --mem-offset <mhz> --power-limit <pct>\n");
    HELP_LOG("  greencurve.exe --fan <auto|0-100> --point49 <mhz> ... --point127 <mhz>\n");
    HELP_LOG("  greencurve.exe --apply-config [--config <path>]  Apply logon profile slot\n");
    HELP_LOG("  greencurve.exe --service-install           Install and start background service\n");
    HELP_LOG("  greencurve.exe --service-remove            Stop and remove background service\n");
    HELP_LOG("  greencurve.exe --export-active-settings <path>  Write the currently applied settings to a file (used by setup)\n");
    HELP_LOG("  greencurve.exe --apply-settings-file <path>     Apply settings written by --export-active-settings\n");
    HELP_LOG("  greencurve.exe --set-machine-logon-slot <slot>  Set machine-wide default logon profile (admin only)\n");
    HELP_LOG("  greencurve.exe --clear-machine-logon-slot       Clear machine-wide default logon profile (admin only)\n");
    HELP_LOG("  greencurve.exe --share-slot <slot>              Share slot with all users: publish data + set as all-users default (admin only)\n");
    HELP_LOG("  greencurve.exe --unshare-slot <slot>            Stop sharing slot with all users (admin only)\n");
    HELP_LOG("  greencurve.exe --set-restrict-shared <0|1>      Restrict standard users to shared profiles only (admin only)\n");
    HELP_LOG("  greencurve.exe --publish-slot-to-machine <slot> [advanced] Copy profile slot to shared bank without changing the default (admin only)\n");
    HELP_LOG("  greencurve.exe --clear-machine-slot <slot>      [advanced] Clear a slot from the shared bank (admin only)\n");
    HELP_LOG("  greencurve.exe --save-config [--config <path>]  Save to selected profile slot\n");
    HELP_LOG("  greencurve.exe --reset      Reset curve/global controls to defaults\n");
    HELP_LOG("  greencurve.exe --help       This help\n");
    fflush(out);

    #undef HELP_LOG
}
