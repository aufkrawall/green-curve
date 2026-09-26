// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// CLI usage text.  Split out of entry.cpp, which sits at its size ratchet, when
// --self-test was added.  Pure output; the caller owns the log handle and the
// early-return.
//
// Included by main_shell.cpp ahead of entry.cpp.

static void cli_print_help(FILE* out) {
    // `out` may now legitimately be null: the console sink stands on its own, so
    // help still reaches a terminal when the log file could not be opened.
    // Same two-sink shape as entry.cpp's CLI_LOG, kept local so this shard does
    // not depend on a macro defined around another file's log handle.
    #define HELP_LOG(...) do { \
        char _gc_line[512] = {}; \
        StringCchPrintfA(_gc_line, ARRAY_COUNT(_gc_line), __VA_ARGS__); \
        gc_cli_emit(out, _gc_line); \
    } while (0)

    HELP_LOG(APP_NAME " v" APP_VERSION " - NVIDIA VF Curve Editor\n");
    HELP_LOG("Usage:\n");
    HELP_LOG("  greencurve.exe              Launch GUI\n");
    HELP_LOG("  greencurve.exe --dump       Write VF curve to greencurve_cli_log.txt\n");
    HELP_LOG("  greencurve.exe --json       Write VF curve to greencurve_curve.json\n");
    HELP_LOG("  greencurve.exe --probe [--probe-output <path>]  Probe NvAPI/NVML/VF support and write a report\n");
    HELP_LOG("  greencurve.exe --self-test  Read-only driver/arch pre-flight, no service needed (run elevated)\n");
    HELP_LOG("  greencurve.exe --clk-domain-probe  Identify ClkDomains entries by briefly writing +50 MHz offsets, then restoring (run elevated)\n");
    HELP_LOG("  greencurve.exe --clk-domain-dump  Write the full ClkDomains control block to a text file (read-only)\n");
    HELP_LOG("  greencurve.exe --gpu-offset <mhz> --mem-offset <mhz> --power-limit <pct>\n");
    HELP_LOG("  greencurve.exe --fan <auto|0-100> --point49 <mhz> ... --point127 <mhz>\n");
    HELP_LOG("  greencurve.exe --apply-config [--config <path>]  Apply logon profile slot\n");
    // "run elevated" is stated here for the same reason it is stated on
    // --self-test: without it the only feedback was "Failed opening service
    // manager (error 5)", and README documented this verb as the archive
    // install step without ever saying which kind of shell it needs.
    HELP_LOG("  greencurve.exe --service-install           Install and start background service (run elevated)\n");
    HELP_LOG("  greencurve.exe --service-remove            Stop and remove background service (run elevated)\n");
    HELP_LOG("  greencurve.exe --export-active-settings <path>  Write the currently applied settings to a file (used by setup)\n");
    HELP_LOG("  greencurve.exe --apply-settings-file <path>     Apply settings written by --export-active-settings\n");
    HELP_LOG("  greencurve.exe --set-machine-logon-slot <slot>  Set machine-wide default logon profile (admin only)\n");
    HELP_LOG("  greencurve.exe --clear-machine-logon-slot       Clear machine-wide default logon profile (admin only)\n");
    HELP_LOG("  greencurve.exe --share-slot <slot>              Share slot with all users: publish data + set as all-users default (admin only)\n");
    HELP_LOG("  greencurve.exe --unshare-slot <slot>            Stop sharing slot with all users (admin only)\n");
    HELP_LOG("  greencurve.exe --set-restrict-shared <0|1>      Restrict standard users to shared profiles only (admin only)\n");
    HELP_LOG("  greencurve.exe --publish-slot-to-machine <slot> [advanced] Copy profile slot for all users without changing the default (admin only)\n");
    HELP_LOG("  greencurve.exe --clear-machine-slot <slot>      [advanced] Clear a shared profile slot (admin only)\n");
    HELP_LOG("  greencurve.exe --save-config [--config <path>]  Save to selected profile slot\n");
    HELP_LOG("  greencurve.exe --reset      Reset curve/global controls to defaults\n");
    HELP_LOG("  greencurve.exe --help       Show this help text\n");
    // Where the same text is also kept, so the file sink is discoverable from
    // the console one. gc_cli_emit() already flushed every line above.
    HELP_LOG("A copy of this output is written to %s\n", cli_log_path());

    #undef HELP_LOG
}
