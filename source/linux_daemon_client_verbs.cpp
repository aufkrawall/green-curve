// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// linux_daemon_client_verbs.cpp -- the individual client request verbs.
//
// Split out of linux_daemon.cpp for the source-size ratchet, along the seam the
// file already had: everything here runs in an UNPRIVILEGED client process and
// touches no daemon state, while everything after the include site owns the
// GPU as root.  Externally linked definitions live in an included .cpp rather
// than a header, the same rule linux_startup_policy.h follows.
//
// Each verb decides what a request must be attached to before it is sent; the
// shared build/send/recover machinery is in linux_daemon_client.h and the
// deadlines are derived in linux_daemon_deadline_policy.h.
//
// Included by linux_daemon.cpp after linux_daemon_client.h; do not compile
// separately.

bool linux_daemon_apply(const GpuAdapterInfo* target, const DesiredSettings* desired, bool interactive,
                         char* result, size_t resultSize) {
    ServiceResponse current = {};
    char error[256] = {};
    if (!linux_daemon_get_state(target, &current, error, sizeof(error))) {
        if (result) gc_strlcpy(result, resultSize,
            error[0] ? error : "cannot attach Apply to live daemon state");
        return false;
    }
    const GpuAdapterInfo* authoritative =
        daemon_response_selected_gpu(&current);
    if (!authoritative) {
        if (result) gc_strlcpy(result, resultSize,
            "daemon did not publish an exact GPU identity for Apply");
        return false;
    }
    return send_simple(SERVICE_CMD_APPLY, desired, authoritative, interactive,
                       SERVICE_APPLY_ORIGIN_CLI, &current.state,
                       nullptr, result, resultSize);
}
bool linux_daemon_reset(const GpuAdapterInfo* target, char* result, size_t resultSize) {
    ServiceResponse current = {};
    char error[256] = {};
    if (!linux_daemon_get_state(target, &current, error, sizeof(error))) {
        if (result) gc_strlcpy(result, resultSize,
            error[0] ? error : "cannot attach Reset to live daemon state");
        return false;
    }
    const GpuAdapterInfo* authoritative =
        daemon_response_selected_gpu(&current);
    if (!authoritative) {
        if (result) gc_strlcpy(result, resultSize,
            "daemon did not publish an exact GPU identity for Reset");
        return false;
    }
    return send_simple(SERVICE_CMD_RESET, nullptr, authoritative, true,
                       SERVICE_APPLY_ORIGIN_UNSPECIFIED, &current.state,
                       nullptr, result, resultSize);
}

bool linux_daemon_resume_restore(char* result, size_t resultSize) {
    // No live-state attachment on purpose. The reconnect preconditions exist so
    // a client cannot apply settings it computed against a stale envelope, but
    // this request carries no settings and names no GPU: the daemon replays the
    // intent it is holding right now, against the adapter it is holding it for.
    // Reading state first would only widen the window between the resume edge
    // and the write.
    return send_simple(SERVICE_CMD_RESUME_RESTORE, nullptr, nullptr, false,
                       SERVICE_APPLY_ORIGIN_UNSPECIFIED, nullptr,
                       nullptr, result, resultSize);
}

bool linux_daemon_apply_checked(const GpuAdapterInfo* target,
                                const DesiredSettings* desired, bool interactive,
                                const ServiceStateEnvelope* expected,
                                ServiceResponse* response,
                                char* result, size_t resultSize) {
    gc_u32 domains = service_desired_mutation_domains(desired);
    if (!target || !target->valid || !desired || !expected ||
        !expected->serviceInstanceId || !expected->gpuGeneration ||
        (service_mutation_domains_require_vf(domains) &&
         !expected->topologySignature)) {
        if (result) gc_strlcpy(result, resultSize,
            "Apply is not attached to the required daemon/GPU/topology generation");
        return false;
    }
    return send_simple(SERVICE_CMD_APPLY, desired, target, interactive,
                       SERVICE_APPLY_ORIGIN_GUI,
                       expected, response, result, resultSize);
}

bool linux_daemon_reset_checked(const GpuAdapterInfo* target,
                                const ServiceStateEnvelope* expected,
                                ServiceResponse* response,
                                char* result, size_t resultSize) {
    if (!target || !target->valid || !expected ||
        !expected->serviceInstanceId || !expected->gpuGeneration ||
        !expected->topologySignature) {
        if (result) gc_strlcpy(result, resultSize,
            "Reset is not attached to a complete READY daemon/GPU/topology generation");
        return false;
    }
    return send_simple(SERVICE_CMD_RESET, nullptr, target, true,
                       SERVICE_APPLY_ORIGIN_UNSPECIFIED,
                       expected, response, result, resultSize);
}

bool linux_daemon_snapshot(ServiceSnapshot* snapshot, char* err, size_t errSize) {
    ServiceResponse resp = {};
    if (!linux_daemon_get_state(nullptr, &resp, err, errSize)) return false;
    if (snapshot) *snapshot = resp.snapshot;
    return true;
}

bool linux_daemon_get_state_ex(const GpuAdapterInfo* target,
                               ServiceResponse* response,
                               LinuxDaemonReachability* reachability,
                               char* err, size_t errSize) {
    ServiceRequest req = {};
    ServiceResponse resp = {};
    req.magic = SERVICE_PROTOCOL_MAGIC;
    req.version = SERVICE_PROTOCOL_VERSION;
    req.command = SERVICE_CMD_GET_SNAPSHOT;
    req.callerPid = (gc_u32)getpid();
    if (target) req.targetGpu = *target;
    LinuxDaemonSendOutcome outcome = {};
    bool ok = linux_daemon_send_deadline(
        &req, &resp, linux_daemon_command_response_timeout_ms(req.command),
        &outcome, err, errSize);
    if (reachability) *reachability = outcome.reachability;
    if (!ok) return false;
    if (response) *response = resp;
    return true;
}

bool linux_daemon_get_state(const GpuAdapterInfo* target, ServiceResponse* response,
                            char* err, size_t errSize) {
    return linux_daemon_get_state_ex(target, response, nullptr, err, errSize);
}

bool linux_daemon_resolve_write_target(const GpuAdapterInfo* preferred,
                                       GpuAdapterInfo* out,
                                       char* err, size_t errSize) {
    ServiceResponse current = {};
    if (!linux_daemon_get_state(preferred, &current, err, errSize)) return false;
    const GpuAdapterInfo* authoritative = daemon_response_selected_gpu(&current);
    if (!authoritative) {
        gc_strlcpy(err, errSize,
            "daemon has not published an exact GPU identity yet");
        return false;
    }
    if (out) *out = *authoritative;
    return true;
}
