// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// One route rule for every command (service_gpu_route.h), the Windows half. A
// routed command answers for the GPU the service serves; the full state read
// may move the service (guarded by service_prepare_requested_gpu); Apply and
// Reset bind their target through their reconnect preconditions. Any other
// command naming another GPU is refused instead of answered for the wrong one.
// Included by main_service_server.cpp ahead of the pipe shard.

static bool service_route_admits(const ServiceRequest* request, ServiceResponse* response) {
    GpuAdapterInfo adapters[MAX_GPU_ADAPTERS] = {};
    EnterCriticalSection(&g_appLock);
    const unsigned int count = g_app.adapterCount < MAX_GPU_ADAPTERS
        ? g_app.adapterCount : MAX_GPU_ADAPTERS;
    memcpy(adapters, g_app.adapters, sizeof(adapters[0]) * count);
    const int served = g_app.selectedGpuIdentityValid && g_app.selectedGpuIndex < count
        ? (int)g_app.selectedGpuIndex : -1;
    LeaveCriticalSection(&g_appLock);
    const ServiceGpuRouteDisposition disposition = service_gpu_route_disposition(
        request->command, request->targetGpu.valid != 0,
        service_gpu_route_addresses(request->targetGpu, adapters, count, served));
    if (disposition != SERVICE_GPU_ROUTE_NOT_SERVED) return true;
    response->status = SERVICE_STATUS_STALE_STATE;
    StringCchCopyA(response->message, ARRAY_COUNT(response->message),
        SERVICE_GPU_ROUTE_NOT_SERVED_MESSAGE);
    debug_log("service route: refused command=%u route=%s target=nvapi%u bus=%u device=%u "
              "resolved=%d served=%d adapters=%u\n", (unsigned int)request->command,
              service_gpu_route_name(service_command_gpu_route(request->command)),
              request->targetGpu.nvapiIndex, request->targetGpu.pciBus,
              request->targetGpu.pciDevice,
              gpu_adapter_route_index(&request->targetGpu, adapters, count), served, count);
    return false;
}

// The same rule once the envelope is stamped: a routed answer must describe
// the GPU it was routed to. The route gate runs before the handler takes the
// runtime lock, so a logon or recovery retarget in between could otherwise
// answer for another card with OK. Only optional routes are checked here; the
// state read, Apply/Reset and the boot policy carry their own binding.
static void service_route_check_published(const ServiceRequest* request, ServiceResponse* response) {
    // A file command checked its route under the runtime lock right before it
    // wrote; a later retarget must not call a correct report stale.
    if (request->command == SERVICE_CMD_WRITE_LOG_SNAPSHOT ||
        request->command == SERVICE_CMD_WRITE_JSON_SNAPSHOT ||
        request->command == SERVICE_CMD_WRITE_PROBE_REPORT ||
        !service_gpu_route_answer_mismatch(*request, *response)) return;
    memset(&response->capture, 0, sizeof(response->capture));
    const int served = (response->state.validSections & SERVICE_STATE_SECTION_ADAPTER_IDENTITY)
        ? (int)response->snapshot.selectedAdapterIndex : -1;
    response->status = SERVICE_STATUS_STALE_STATE;
    response->outcomeSeverity = service_response_resolve_outcome_severity(response->status, 0);
    StringCchCopyA(response->message, ARRAY_COUNT(response->message), SERVICE_GPU_ROUTE_NOT_SERVED_MESSAGE);
    debug_log("service route: answer for command=%u no longer describes its routed GPU (served=%d); refused\n",
              (unsigned int)request->command, served);
}
