// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// linux_daemon_client.h -- the request-building half of the client side.
//
// Split out of linux_daemon.cpp purely for the source-size ratchet: the file
// holds a client role and a daemon role in one translation unit (the binary is
// deliberately one multi-role executable), and the client's shared machinery
// is the part with no daemon-side dependencies at all.
//
// What lives here is the two things every client helper needs -- which adapter
// a response is authoritative about, and how a request is built, sent, and its
// outcome recovered after a transport failure.  The individual verbs
// (linux_daemon_apply/reset/resume_restore/...) stay next to their callers.
//
// Included by linux_daemon.cpp after linux_daemon_transport.cpp.

#ifndef GREEN_CURVE_LINUX_DAEMON_CLIENT_H
#define GREEN_CURVE_LINUX_DAEMON_CLIENT_H

static const GpuAdapterInfo* daemon_response_selected_gpu(
    const ServiceResponse* response) {
    if (!response ||
        (response->state.validSections &
            SERVICE_STATE_SECTION_ADAPTER_IDENTITY) == 0 ||
        response->snapshot.selectedAdapterIndex >=
            response->snapshot.adapterCount ||
        response->snapshot.selectedAdapterIndex >= MAX_GPU_ADAPTERS)
        return nullptr;
    const GpuAdapterInfo* gpu = &response->snapshot.adapters[
        response->snapshot.selectedAdapterIndex];
    return gpu->valid &&
        (linux_gpu_bdf_valid(gpu) || gpu->pciInfoValid) ? gpu : nullptr;
}

static bool send_simple(unsigned int command, const DesiredSettings* desired,
                         const GpuAdapterInfo* target, bool interactive,
                         ServiceApplyOrigin applyOrigin,
                         const ServiceStateEnvelope* expected,
                         ServiceResponse* response,
                         char* result, size_t resultSize) {
    ServiceRequest req;
    memset(&req, 0, sizeof(req));
    req.magic = SERVICE_PROTOCOL_MAGIC;
    req.version = SERVICE_PROTOCOL_VERSION;
    req.command = command;
    req.flags = service_request_flags_for_command(command, interactive);
    req.callerPid = (gc_u32)getpid();
    if (command == SERVICE_CMD_APPLY) req.applyOrigin = applyOrigin;
    if (command == SERVICE_CMD_APPLY || command == SERVICE_CMD_RESET) {
        ssize_t randomBytes = -1;
        do {
            randomBytes = getrandom(&req.operationId,
                sizeof(req.operationId), 0);
        } while (randomBytes < 0 && errno == EINTR);
        if (randomBytes != (ssize_t)sizeof(req.operationId) ||
            req.operationId == 0) {
            if (result) gc_strlcpy(result, resultSize,
                "failed generating a secure operation ID");
            return false;
        }
    }
    if (desired) req.desired = *desired;
    if (target) req.targetGpu = *target;
    if (expected) {
        req.expectedServiceInstanceId = expected->serviceInstanceId;
        req.expectedGpuGeneration = expected->gpuGeneration;
        req.expectedTopologySignature = expected->topologySignature;
    }
    ServiceResponse resp;
    memset(&resp, 0, sizeof(resp));
    char err[256] = {};
    // The mutation itself gets the mutation lane; see
    // linux_daemon_deadline_policy.h for why the old shared 2000 ms was an
    // order of magnitude below what this daemon may legitimately spend.
    const unsigned long requestBudget =
        linux_daemon_command_response_timeout_ms(command);
    const unsigned long long requestStart = monotonic_ms();
    LinuxDaemonSendOutcome sendOutcome = {};
    bool ok = linux_daemon_send_deadline(&req, &resp, requestBudget,
                                         &sendOutcome, err, sizeof(err));
    bool receivedServiceError = !ok &&
        resp.magic == SERVICE_PROTOCOL_MAGIC &&
        resp.version == SERVICE_PROTOCOL_VERSION &&
        resp.status != SERVICE_STATUS_OK;
    if (!ok && req.operationId != 0 && !receivedServiceError) {
        // Recover the outcome instead of issuing a second hardware write.
        //
        // This used to be one query carrying the SAME 2000 ms the mutation had
        // just exceeded -- and the daemon is single-threaded, so while the
        // mutation is still running the query cannot be accepted either.  It
        // therefore expired too, and reported "pending or unknown" in exactly
        // the case it exists to cover: the TUI showed a committed Apply as
        // "Apply failed" with the draft still dirty.
        //
        // The loop is deadline-bounded, not interval-bounded: each attempt
        // blocks in poll() on a real descriptor for the rest of the budget, and
        // linux_daemon_recovery_may_retry() stops any failure that returns
        // promptly (unreachable, saturated backlog, protocol) from spinning.
        bool recovered = false;
        unsigned int lastState = SERVICE_OPERATION_OUTCOME_UNKNOWN;
        int attempts = 0;
        for (;;) {
            unsigned long remaining = linux_daemon_recovery_remaining_ms(
                (unsigned long)(monotonic_ms() - requestStart));
            if (remaining == 0ul) break;
            ServiceRequest query = {};
            query.magic = SERVICE_PROTOCOL_MAGIC;
            query.version = SERVICE_PROTOCOL_VERSION;
            query.command = SERVICE_CMD_GET_OPERATION_RESULT;
            query.callerPid = (gc_u32)getpid();
            query.operationId = req.operationId;
            char queryErr[256] = {};
            LinuxDaemonSendOutcome queryOutcome = {};
            ++attempts;
            bool answered = linux_daemon_send_deadline(
                &query, &resp, remaining, &queryOutcome,
                queryErr, sizeof(queryErr));
            if (answered) {
                lastState = (unsigned int)resp.operationState;
                // A responsive daemon has given its final word either way;
                // asking it again would be a spin, not patience.
                if (linux_daemon_recovery_state_is_settled(lastState)) {
                    ok = resp.status == SERVICE_STATUS_OK;
                    recovered = true;
                }
                break;
            }
            dlog("daemon client: operation=%llu recovery attempt %d did not "
                 "complete within %lums (reachability=%d deadlineExpired=%d): %s\n",
                 (unsigned long long)req.operationId, attempts, remaining,
                 (int)queryOutcome.reachability,
                 queryOutcome.deadlineExpired ? 1 : 0,
                 queryErr[0] ? queryErr : "no detail");
            if (!linux_daemon_recovery_may_retry(answered, queryOutcome)) break;
        }
        if (recovered) {
            dlog("daemon client: operation=%llu recovered state=%u status=%u "
                 "after %d quer%s and %llums total\n",
                 (unsigned long long)req.operationId, lastState,
                 (unsigned int)resp.status, attempts,
                 attempts == 1 ? "y" : "ies",
                 monotonic_ms() - requestStart);
        } else {
            // Report the outcome as UNKNOWN rather than as a failure: the
            // hardware write may well have committed, and telling the user it
            // failed is what invited the duplicate Apply the message warns
            // against.  The stamped fields let the TUI/CLI say so precisely.
            memset(&resp, 0, sizeof(resp));
            resp.magic = SERVICE_PROTOCOL_MAGIC;
            resp.version = SERVICE_PROTOCOL_VERSION;
            resp.status = SERVICE_STATUS_ERROR;
            resp.operationId = req.operationId;
            resp.operationState = SERVICE_OPERATION_OUTCOME_UNKNOWN;
            dlog("daemon client: operation=%llu outcome UNKNOWN after %d "
                 "quer%s and %llums (budget %lums); NOT retried\n",
                 (unsigned long long)req.operationId, attempts,
                 attempts == 1 ? "y" : "ies",
                 monotonic_ms() - requestStart,
                 linux_daemon_mutation_total_budget_ms());
            if (response) *response = resp;
            if (result) gc_snprintf(result, resultSize,
                "operation %llu was sent but its outcome could not be read back "
                "(the daemon accepted it and stopped answering); the GPU write "
                "may have committed -- refresh before applying again",
                (unsigned long long)req.operationId);
            return false;
        }
    }
    if (response) *response = resp;
    if (result) gc_strlcpy(result, resultSize, resp.message[0] ? resp.message : (ok ? "OK" : err));
    return ok;
}

#endif // GREEN_CURVE_LINUX_DAEMON_CLIENT_H
