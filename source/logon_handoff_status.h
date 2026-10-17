// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// Silent scheduled-task status writer, split from the already oversized CLI
// dispatcher.  The service owns profile selection; this only records that the
// authenticated settings-free handoff was accepted.
#ifndef GREEN_CURVE_LOGON_HANDOFF_STATUS_H
#define GREEN_CURVE_LOGON_HANDOFF_STATUS_H

static void write_logon_handoff_accepted_status(const char* handoffResult) {
    char configToken[32] = {};
    gc_log_path_token(g_app.configPath, configToken, sizeof(configToken));
    // Appended, never a replacement: the previous logon's record (including a
    // FAILED one) must survive this logon's success (cli_log_append_policy.h).
    if (!gc_cli_log_append_linef(
            "Green Curve scheduled logon handoff accepted: automatic profile "
            "application is service-owned; elevated=%d config=%s result=%s\n",
            is_elevated() ? 1 : 0, configToken,
            handoffResult && handoffResult[0] ? handoffResult : "accepted")) {
        debug_log("logon startup: could not append the silent handoff status\n");
    }
}

#endif
