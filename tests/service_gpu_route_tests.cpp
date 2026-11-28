// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
// One meaning of ServiceRequest.targetGpu on both platforms (protocol v40):
// the command route table, the shared wire rule, the step-3 serving decision
// and the matcher both dispatchers use. Codes 14600-14629, 14637-14638, 14641-14643.

#include "gpu_core.h"
#include <cstdio>
#include <cstring>
#include <initializer_list>

namespace {

GpuAdapterInfo route_test_gpu(unsigned int bus) {
    GpuAdapterInfo gpu = {};
    gpu.valid = true;
    gpu.pciInfoValid = true;
    gpu.deviceId = 0x1E07;
    gpu.subSystemId = 0x12FF10DE;
    gpu.pciRevisionId = 0xA1;
    gpu.pciBus = bus;
    gpu.nvapiIndex = bus;
    gpu.nvmlIndex = bus;
    snprintf(gpu.name, sizeof(gpu.name), "%s", "NVIDIA GeForce RTX 2080 Ti");
    return gpu;
}

ServiceRequest route_test_request(gc_u32 command) {
    ServiceRequest request = {};
    request.magic = SERVICE_PROTOCOL_MAGIC;
    request.version = SERVICE_PROTOCOL_VERSION;
    request.command = command;
    request.callerPid = 4242;
    return request;
}

}  // namespace

int run_service_gpu_route_tests() {
    // The table: every command has exactly one class, independent of platform.
    struct Row { gc_u32 command; ServiceGpuRoute route; };
    const Row rows[] = {
        {SERVICE_CMD_PING, SERVICE_GPU_ROUTE_MACHINE},
        {SERVICE_CMD_GET_SNAPSHOT, SERVICE_GPU_ROUTE_OPTIONAL},
        {SERVICE_CMD_GET_TELEMETRY, SERVICE_GPU_ROUTE_OPTIONAL},
        {SERVICE_CMD_APPLY, SERVICE_GPU_ROUTE_REQUIRED},
        {SERVICE_CMD_RESET, SERVICE_GPU_ROUTE_REQUIRED},
        {SERVICE_CMD_GET_ACTIVE_DESIRED, SERVICE_GPU_ROUTE_OPTIONAL},
        {SERVICE_CMD_WRITE_LOG_SNAPSHOT, SERVICE_GPU_ROUTE_OPTIONAL},
        {SERVICE_CMD_WRITE_JSON_SNAPSHOT, SERVICE_GPU_ROUTE_OPTIONAL},
        {SERVICE_CMD_WRITE_PROBE_REPORT, SERVICE_GPU_ROUTE_OPTIONAL},
        {SERVICE_CMD_LOGON_HANDOFF, SERVICE_GPU_ROUTE_MACHINE},
        {SERVICE_CMD_GET_OPERATION_RESULT, SERVICE_GPU_ROUTE_MACHINE},
        {SERVICE_CMD_GET_STARTUP_POLICY, SERVICE_GPU_ROUTE_POLICY},
        {SERVICE_CMD_SET_STARTUP_POLICY, SERVICE_GPU_ROUTE_POLICY},
        {SERVICE_CMD_REFRESH_STARTUP_PROFILE, SERVICE_GPU_ROUTE_POLICY},
        {SERVICE_CMD_RESUME_RESTORE, SERVICE_GPU_ROUTE_MACHINE},
        {SERVICE_CMD_GET_UPDATE_STATE, SERVICE_GPU_ROUTE_MACHINE},
        {SERVICE_CMD_CHECK_FOR_UPDATE, SERVICE_GPU_ROUTE_MACHINE},
        {SERVICE_CMD_INSTALL_UPDATE, SERVICE_GPU_ROUTE_MACHINE},
        {SERVICE_CMD_SET_UPDATE_POLICY, SERVICE_GPU_ROUTE_MACHINE},
        {SERVICE_CMD_CAPTURE_CURRENT, SERVICE_GPU_ROUTE_OPTIONAL},
    };
    unsigned int covered = 0;
    for (const Row& row : rows) {
        if (service_command_gpu_route(row.command) != row.route) return 14600;
        ++covered;
    }
    // A new command must be classified here, not fall through unnoticed.
    if (covered != SERVICE_CMD_CAPTURE_CURRENT - SERVICE_CMD_PING + 1) return 14601;

    // The wire rule: a machine-wide command refuses a target; every routed
    // command accepts one. Built from otherwise-valid requests.
    const GpuAdapterInfo served = route_test_gpu(0x21);
    const GpuAdapterInfo other = route_test_gpu(0x4B);
    for (gc_u32 command : {SERVICE_CMD_PING, SERVICE_CMD_GET_UPDATE_STATE,
                           SERVICE_CMD_CHECK_FOR_UPDATE, SERVICE_CMD_RESUME_RESTORE}) {
        ServiceRequest request = route_test_request(command);
        if (!validate_service_request_for_ipc(&request)) return 14602;
        request.targetGpu = served;
        if (validate_service_request_for_ipc(&request)) return 14603;
    }
    ServiceRequest logon = route_test_request(SERVICE_CMD_LOGON_HANDOFF);
    logon.applyOrigin = SERVICE_APPLY_ORIGIN_LOGON;
    if (!validate_service_request_for_ipc(&logon)) return 14604;
    logon.targetGpu = served;
    const char* logonReason = service_request_reject_reason(&logon);
    if (!logonReason || strcmp(logonReason, "machine-wide command carries a target GPU"))
        return 14605;
    for (gc_u32 command : {SERVICE_CMD_GET_SNAPSHOT, SERVICE_CMD_GET_TELEMETRY,
                           SERVICE_CMD_GET_ACTIVE_DESIRED, SERVICE_CMD_WRITE_LOG_SNAPSHOT,
                           SERVICE_CMD_WRITE_JSON_SNAPSHOT, SERVICE_CMD_WRITE_PROBE_REPORT}) {
        ServiceRequest request = route_test_request(command);
        request.targetGpu = other;
        if (!validate_service_request_for_ipc(&request)) return 14606;
    }
    ServiceRequest capture = route_test_request(SERVICE_CMD_CAPTURE_CURRENT);
    capture.captureDomains = 1;
    capture.targetGpu = other;
    if (!validate_service_request_for_ipc(&capture)) return 14607;
    // An operation result is looked up by its id alone, whichever GPU it wrote.
    ServiceRequest operation = route_test_request(SERVICE_CMD_GET_OPERATION_RESULT);
    operation.operationId = 77;
    if (!validate_service_request_for_ipc(&operation)) return 14608;
    operation.targetGpu = other;
    if (validate_service_request_for_ipc(&operation)) return 14638;
    // The daemon-wide boot policy keeps its own target rules: PROFILE names
    // its binding, the other modes carry none, and a refresh may name the
    // card its stored binding must be; an attachment must be complete.
    ServiceRequest none = route_test_request(SERVICE_CMD_SET_STARTUP_POLICY);
    none.startupMode = SERVICE_STARTUP_POLICY_NONE;
    if (!validate_service_request_for_ipc(&none)) return 14609;
    none.targetGpu = served;
    if (validate_service_request_for_ipc(&none)) return 14610;
    ServiceRequest refresh = route_test_request(SERVICE_CMD_REFRESH_STARTUP_PROFILE);
    refresh.profileSlot = 1;
    refresh.targetGpu = served;
    if (!validate_service_request_for_ipc(&refresh)) return 14611;
    refresh.expectedServiceInstanceId = 3;
    refresh.expectedGpuGeneration = 4;
    if (validate_service_request_for_ipc(&refresh)) return 14612;
    refresh.expectedTopologySignature = 5;
    if (!validate_service_request_for_ipc(&refresh)) return 14613;
    ServiceRequest getPolicy = route_test_request(SERVICE_CMD_GET_STARTUP_POLICY);
    getPolicy.targetGpu = served;
    if (validate_service_request_for_ipc(&getPolicy)) return 14637;

    // The matcher: a route must resolve to exactly one live adapter, the
    // served one. An absent route addresses the served GPU; nothing is
    // addressed when nothing is served; another location is another GPU even
    // for the identical model; identical cards at an unknown location are
    // ambiguous and never resolved by guessing.
    const GpuAdapterInfo none_gpu = {};
    GpuAdapterInfo live[3] = {served, other, route_test_gpu(0x65)};
    if (!service_gpu_route_addresses(none_gpu, live, 2, 0)) return 14614;
    if (service_gpu_route_addresses(served, live, 2, -1) ||
        service_gpu_route_addresses(none_gpu, live, 2, -1)) return 14615;
    if (service_gpu_route_addresses(served, live, 2, 2)) return 14616;
    if (!service_gpu_route_addresses(served, live, 2, 0)) return 14617;
    if (service_gpu_route_addresses(other, live, 2, 0) ||
        !service_gpu_route_addresses(other, live, 2, 1)) return 14618;
    GpuAdapterInfo renamed = served;
    snprintf(renamed.name, sizeof(renamed.name), "%s", "renamed by the driver");
    renamed.nvapiIndex = 7;
    if (!service_gpu_route_addresses(renamed, live, 2, 0)) return 14619;
    GpuAdapterInfo noPci = served;
    noPci.pciInfoValid = false;
    if (service_gpu_route_addresses(noPci, live, 2, 0)) return 14620;
    if (gpu_adapter_route_index(&live[2], live, 2) != -1) return 14621;
    GpuAdapterInfo twins[2] = {served, served};
    twins[0].pciBus = twins[1].pciBus = 0;
    twins[1].nvapiIndex = twins[1].nvmlIndex = 9;
    GpuAdapterInfo twinRoute = twins[0];
    if (gpu_adapter_route_index(&twinRoute, twins, 2) != -1 ||
        service_gpu_route_addresses(twinRoute, twins, 2, 0)) return 14629;

    // The step-3 serving decision.
    if (service_gpu_route_disposition(SERVICE_CMD_GET_SNAPSHOT, true, false) !=
            SERVICE_GPU_ROUTE_SELECT) return 14622;
    if (service_gpu_route_disposition(SERVICE_CMD_GET_SNAPSHOT, false, false) !=
            SERVICE_GPU_ROUTE_SERVE) return 14623;
    for (gc_u32 command : {SERVICE_CMD_APPLY, SERVICE_CMD_RESET})
        if (service_gpu_route_disposition(command, true, false) !=
                SERVICE_GPU_ROUTE_BOUND) return 14624;
    for (gc_u32 command : {SERVICE_CMD_GET_TELEMETRY, SERVICE_CMD_GET_ACTIVE_DESIRED,
                           SERVICE_CMD_WRITE_LOG_SNAPSHOT, SERVICE_CMD_WRITE_JSON_SNAPSHOT,
                           SERVICE_CMD_WRITE_PROBE_REPORT, SERVICE_CMD_CAPTURE_CURRENT}) {
        if (service_gpu_route_disposition(command, true, false) !=
                SERVICE_GPU_ROUTE_NOT_SERVED) return 14625;
        if (service_gpu_route_disposition(command, true, true) !=
                SERVICE_GPU_ROUTE_SERVE) return 14626;
        if (service_gpu_route_disposition(command, false, false) !=
                SERVICE_GPU_ROUTE_SERVE) return 14627;
    }
    // A routed answer must describe the GPU it was routed to (14641-14643):
    // a retarget between the route gate and the handler leaves an OK answer
    // published for another card, which the dispatcher then refuses.
    ServiceRequest routedTelemetry = route_test_request(SERVICE_CMD_GET_TELEMETRY);
    routedTelemetry.targetGpu = served;
    static ServiceResponse answer;
    answer = ServiceResponse{};
    answer.status = SERVICE_STATUS_OK;
    answer.state.validSections = SERVICE_STATE_SECTION_ADAPTER_IDENTITY;
    answer.snapshot.adapterCount = 2;
    answer.snapshot.adapters[0] = served;
    answer.snapshot.adapters[1] = other;
    answer.snapshot.selectedAdapterIndex = 0;
    if (service_gpu_route_answer_mismatch(routedTelemetry, answer)) return 14641;
    answer.snapshot.selectedAdapterIndex = 1;
    if (!service_gpu_route_answer_mismatch(routedTelemetry, answer)) return 14642;
    // Not routed, not OK, or a command with its own binding: nothing to check.
    ServiceRequest unrouted = route_test_request(SERVICE_CMD_GET_TELEMETRY);
    ServiceRequest stateRead = routedTelemetry;
    stateRead.command = SERVICE_CMD_GET_SNAPSHOT;
    ServiceResponse failed = answer;
    failed.status = SERVICE_STATUS_ERROR;
    if (service_gpu_route_answer_mismatch(unrouted, answer) ||
        service_gpu_route_answer_mismatch(stateRead, answer) ||
        service_gpu_route_answer_mismatch(routedTelemetry, failed)) return 14643;

    // Machine-wide commands and the daemon-wide policy are never route-refused:
    // the validator already refused a stray target, and a policy binding is
    // checked by its own handler against live adapters.
    for (gc_u32 command : {SERVICE_CMD_PING, SERVICE_CMD_GET_OPERATION_RESULT,
                           SERVICE_CMD_SET_STARTUP_POLICY, SERVICE_CMD_REFRESH_STARTUP_PROFILE})
        if (service_gpu_route_disposition(command, true, false) !=
                SERVICE_GPU_ROUTE_SERVE) return 14628;
    return 0;
}

#ifdef GC_GPU_ROUTE_TEST_MAIN
int main() {
    const int failure = run_service_gpu_route_tests();
    printf("service GPU route regression: %d\n", failure);
    return failure;
}
#endif
