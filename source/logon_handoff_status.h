// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// Silent scheduled-task status writer, split from the already oversized CLI
// dispatcher.  The service owns profile selection; this only records that the
// authenticated settings-free handoff was accepted.
#ifndef GREEN_CURVE_LOGON_HANDOFF_STATUS_H
#define GREEN_CURVE_LOGON_HANDOFF_STATUS_H

static void write_logon_handoff_accepted_status(const char* handoffResult) {
    char taskLog[768] = {};
    char timestamp[64] = {};
    char configToken[32] = {};
    gc_log_path_token(g_app.configPath, configToken, sizeof(configToken));
    format_log_timestamp_prefix(timestamp, sizeof(timestamp));
    StringCchPrintfA(taskLog, ARRAY_COUNT(taskLog),
        "%sGreen Curve scheduled logon handoff accepted: automatic profile "
        "application is service-owned; elevated=%d config=%s result=%s\n",
        timestamp,
        is_elevated() ? 1 : 0,
        configToken,
        handoffResult && handoffResult[0] ? handoffResult : "accepted");
    char pathErr[256] = {};
    char writeErr[256] = {};
    if (!resolve_data_paths(pathErr, sizeof(pathErr))) {
        debug_log("logon startup: could not resolve silent handoff log path: %s\n",
                  pathErr[0] ? pathErr : "unknown error");
        return;
    }
    if (!write_text_file_atomic(cli_log_path(), taskLog, strlen(taskLog),
                                writeErr, sizeof(writeErr))) {
        debug_log("logon startup: could not write silent handoff status: %s\n",
                  writeErr[0] ? writeErr : "unknown error");
    }
}

#endif
