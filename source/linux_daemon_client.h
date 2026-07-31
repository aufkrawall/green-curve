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
    bool ok = linux_daemon_send(&req, &resp, err, sizeof(err));
    bool receivedServiceError = !ok &&
        resp.magic == SERVICE_PROTOCOL_MAGIC &&
        resp.version == SERVICE_PROTOCOL_VERSION &&
        resp.status != SERVICE_STATUS_OK;
    if (!ok && req.operationId != 0 && !receivedServiceError) {
        ServiceRequest query = {};
        query.magic = SERVICE_PROTOCOL_MAGIC;
        query.version = SERVICE_PROTOCOL_VERSION;
        query.command = SERVICE_CMD_GET_OPERATION_RESULT;
        query.callerPid = (gc_u32)getpid();
        query.operationId = req.operationId;
        char queryErr[256] = {};
        if (linux_daemon_send(&query, &resp, queryErr, sizeof(queryErr)) &&
            resp.operationState != SERVICE_OPERATION_IN_PROGRESS &&
            resp.operationState != SERVICE_OPERATION_OUTCOME_UNKNOWN) {
            ok = resp.status == SERVICE_STATUS_OK;
            dlog("daemon client: operation=%llu recovered state=%u after transport error\n",
                (unsigned long long)req.operationId,
                (unsigned int)resp.operationState);
        } else {
            if (result) gc_snprintf(result, resultSize,
                "operation %llu outcome is pending or unknown after transport timeout; do not retry with a new operation ID",
                (unsigned long long)req.operationId);
            return false;
        }
    }
    if (response) *response = resp;
    if (result) gc_strlcpy(result, resultSize, resp.message[0] ? resp.message : (ok ? "OK" : err));
    return ok;
}

#endif // GREEN_CURVE_LINUX_DAEMON_CLIENT_H
