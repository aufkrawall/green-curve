// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// The four v19 update commands, as pipe handlers.
//
// Split out of main_service_pipe.cpp, which is at its size ratchet.  Keeping
// them together also keeps one property visible in one place: every handler
// here reads the request for AT MOST two integers, and never for a path, a
// version, a digest or a URL.  See service_protocol_update.h for why.

// The single entry point the pipe's switch calls for all four commands.  Kept
// as one grouped case there because main_service_pipe.cpp is at its size
// ratchet, and because the four share exactly one property worth stating once:
// none of them reads a path, a version, a digest or a URL from the request.
static void service_handle_update_command(const ServiceRequest& request,
                                          ServiceResponse& response,
                                          DWORD callerPid, DWORD callerSessionId,
                                          const char* callerUser);

// Answers SERVICE_CMD_GET_UPDATE_STATE.  The state itself is stamped onto every
// response by the pipe loop, so this command has no work to do beyond saying
// that the read succeeded -- it exists so a client can ask for the state
// without provoking a check.
static void service_handle_update_state_request(ServiceResponse& response) {
    response.status = SERVICE_STATUS_OK;
    StringCchCopyA(response.message, ARRAY_COUNT(response.message), "Update state");
}

// SERVICE_CMD_CHECK_FOR_UPDATE.  An explicit check bypasses the schedule and
// its failure backoff, because a user who clicked a button has already
// expressed the intent the schedule exists to infer.  It bypasses none of the
// verification: the worker it starts is the same one the automatic tick uses.
static void service_handle_update_check_request(ServiceResponse& response,
                                                DWORD callerPid,
                                                DWORD callerSessionId) {
    char err[256] = {};
    if (service_update_start_worker(GC_UPDATE_WORK_CHECK_AND_DOWNLOAD,
                                    err, sizeof(err))) {
        response.status = SERVICE_STATUS_OK;
        StringCchCopyA(response.message, ARRAY_COUNT(response.message),
                       "Checking for updates");
        debug_log("update: manual check requested by pid=%lu session=%lu\n",
                  (unsigned long)callerPid, (unsigned long)callerSessionId);
    } else {
        response.status = SERVICE_STATUS_ERROR;
        StringCchCopyA(response.message, ARRAY_COUNT(response.message), err);
    }
}

// SERVICE_CMD_INSTALL_UPDATE.
//
// This is the ONLY path that reaches service_update_run_install(), and it is
// reachable only from a client request.  That is what makes the install gate's
// `userConsented` truthful rather than decorative: there is no timer, no
// watchdog and no retry that can arrive here.
static void service_handle_update_install_request(ServiceResponse& response,
                                                  DWORD callerPid,
                                                  DWORD callerSessionId,
                                                  const char* callerUser) {
    char err[256] = {};
    if (service_update_start_worker(GC_UPDATE_WORK_INSTALL, err, sizeof(err))) {
        response.status = SERVICE_STATUS_OK;
        StringCchCopyA(response.message, ARRAY_COUNT(response.message),
                       "Installing update");
        debug_log("update: install requested by pid=%lu session=%lu user=%s\n",
                  (unsigned long)callerPid, (unsigned long)callerSessionId,
                  callerUser && callerUser[0] ? callerUser : "<unknown>");
    } else {
        response.status = SERVICE_STATUS_ERROR;
        StringCchCopyA(response.message, ARRAY_COUNT(response.message), err);
    }
}

// SERVICE_CMD_SET_UPDATE_POLICY.  The validator has already bounded both
// fields, so this stores and persists them.
//
// Turning auto-check ON deliberately does NOT trigger a check here: the
// periodic tick will pick it up on its own, and making a preference change also
// make an immediate network request would surprise someone who only meant to
// tick a checkbox.
static void service_handle_update_policy_request(const ServiceRequest& request,
                                                 ServiceResponse& response,
                                                 DWORD callerPid) {
    {
        GcUpdateStateLock guard;
        g_updateState.autoCheck = (GcUpdateAutoCheck)request.updateAutoCheck;
        g_updateState.intervalSeconds =
            gc_update_clamp_interval((int)request.updateIntervalSeconds);
    }
    service_update_save_settings();
    response.status = SERVICE_STATUS_OK;
    StringCchCopyA(response.message, ARRAY_COUNT(response.message),
                   "Update policy saved");
    debug_log("update: policy set autoCheck=%u interval=%us by pid=%lu\n",
              (unsigned)request.updateAutoCheck,
              (unsigned)request.updateIntervalSeconds,
              (unsigned long)callerPid);
}

static void service_handle_update_command(const ServiceRequest& request,
                                          ServiceResponse& response,
                                          DWORD callerPid, DWORD callerSessionId,
                                          const char* callerUser) {
    switch (request.command) {
        case SERVICE_CMD_GET_UPDATE_STATE:
            service_handle_update_state_request(response);
            break;
        case SERVICE_CMD_CHECK_FOR_UPDATE:
            service_handle_update_check_request(response, callerPid, callerSessionId);
            break;
        case SERVICE_CMD_INSTALL_UPDATE:
            service_handle_update_install_request(response, callerPid,
                                                  callerSessionId, callerUser);
            break;
        case SERVICE_CMD_SET_UPDATE_POLICY:
            service_handle_update_policy_request(request, response, callerPid);
            break;
        default:
            // Unreachable: the pipe only routes the four commands above here,
            // and the validator has already refused anything else. Answering
            // rather than falling through silently keeps that assumption
            // visible if the routing ever changes.
            response.status = SERVICE_STATUS_ERROR;
            StringCchCopyA(response.message, ARRAY_COUNT(response.message),
                           "Unsupported update command");
            break;
    }
}
