// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// The three client-requested file writes (log snapshot, JSON snapshot, probe
// report).  Split out of main_service_pipe.cpp, which had reached its size
// ratchet, and included from the position that case body occupied so the
// amalgamated ordering -- and therefore every symbol it depends on -- is
// unchanged.
//
// The whole body runs under the caller's own token.  The service runs as
// SYSTEM and would happily write anywhere; impersonating first means the path
// is resolved and the file created with exactly the rights the requesting user
// has, so a client cannot use the service as a privileged file writer.  The
// path is validated under that impersonation, and the written file is verified
// afterwards, because validation and creation are two separate moments.

static void service_handle_file_write_command(const ServiceRequest& request,
    ServiceResponse& response, HANDLE callerToken, DWORD callerPid) {
    char detail[256] = {};
    lock_service_runtime();
    bool ok = hardware_initialize(detail, sizeof(detail));
    // A probe report is the diagnostic for a machine where hardware init is
    // exactly what fails, so it is the one command still written without it.
    if (!ok && request.command != SERVICE_CMD_WRITE_PROBE_REPORT) {
        response.status = SERVICE_STATUS_ERROR;
        StringCchCopyA(response.message, ARRAY_COUNT(response.message), detail[0] ? detail : "Hardware initialization failed");
        unlock_service_runtime();
        return;
    }
    bool offsetsOk = false;
    if (ok && !read_live_curve_snapshot_settled(4, 40, &offsetsOk)) {
        debug_log("service file command: live curve refresh failed before file write\n");
    }
    if (ok) {
        refresh_global_state(detail, sizeof(detail));
    }
    char fileErr[256] = {};
    bool writeOk = false;
    {
        ScopedServiceClientImpersonation impersonation(callerToken);
        if (!impersonation.active()) {
            set_message(fileErr, sizeof(fileErr),
                "Failed impersonating the authenticated client for output write (error %lu)",
                GetLastError());
        } else if (!service_validate_file_write_path(request.path,
                       fileErr, sizeof(fileErr))) {
            debug_log("service file command: caller-scoped path validation failed command=%u pid=%lu: %s\n",
                (unsigned int)request.command,
                (unsigned long)callerPid,
                fileErr[0] ? fileErr : "unknown");
        } else if (request.command == SERVICE_CMD_WRITE_LOG_SNAPSHOT) {
            writeOk = write_log_snapshot(request.path, fileErr,
                sizeof(fileErr));
        } else if (request.command == SERVICE_CMD_WRITE_JSON_SNAPSHOT) {
            writeOk = write_json_snapshot(request.path, fileErr,
                sizeof(fileErr));
        } else {
            writeOk = write_probe_report(request.path, fileErr, sizeof(fileErr));
        }
        if (writeOk) {
            char verifyErr[256] = {};
            if (!service_verify_written_file_path(request.path, verifyErr, sizeof(verifyErr))) {
                writeOk = false;
                StringCchCopyA(fileErr, sizeof(fileErr), verifyErr);
            }
        }
    }
    response.status = writeOk ? SERVICE_STATUS_OK : SERVICE_STATUS_ERROR;
    if (writeOk) {
        StringCchPrintfA(response.message, ARRAY_COUNT(response.message), "Wrote %s", request.path[0] ? request.path : "requested output file");
    } else {
        StringCchCopyA(response.message, ARRAY_COUNT(response.message), fileErr[0] ? fileErr : "Failed writing requested file");
    }
    populate_service_snapshot(&response.snapshot);
    if (g_serviceControlStateValid) response.controlState = g_serviceControlState;
    unlock_service_runtime();
}
