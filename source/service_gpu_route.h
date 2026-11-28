// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// service_gpu_route.h -- what ServiceRequest.targetGpu means, for every command
// and on both platforms.
//
// targetGpu is the request's one GPU field. It names the GPU the command is
// about; absent, it means the GPU the service currently serves. A command is
// either about a GPU (routed) or about the machine (a target is refused), and
// that classification lives in the table below, which the shared validator
// and both dispatchers read. The daemon-wide boot policy is its own class: its
// target is the stored binding (or a precondition on it), never a route. There is no second GPU field that could disagree
// with it, and no platform split in how it is read.
//
// The service still serves one GPU at a time. Only a full state read may move
// it to another GPU (under the single-owner guard each platform already
// enforces); Apply and Reset bind their target through their reconnect
// preconditions; every other routed command answers only for the GPU being
// served and refuses a route to any other GPU instead of silently answering
// for the wrong one.

#ifndef GREEN_CURVE_SERVICE_GPU_ROUTE_H
#define GREEN_CURVE_SERVICE_GPU_ROUTE_H

#include "gpu_selection_policy.h"

enum ServiceGpuRoute {
    SERVICE_GPU_ROUTE_MACHINE = 0,   // about the machine; a target is refused
    SERVICE_GPU_ROUTE_OPTIONAL = 1,  // about a GPU; absent means the served GPU
    SERVICE_GPU_ROUTE_REQUIRED = 2,  // about a GPU; the target is mandatory
    // The daemon-wide boot policy. One choice for the machine until each GPU
    // has its own runtime; its PROFILE mode names the card it writes as data
    // (the binding), and a refresh may name the card that stored binding must
    // resolve to. Neither is a route, and a refresh never re-binds.
    SERVICE_GPU_ROUTE_POLICY = 3,
};

// One row per command.
static inline ServiceGpuRoute service_command_gpu_route(gc_u32 command) {
    switch (command) {
        case SERVICE_CMD_APPLY:
        case SERVICE_CMD_RESET:
            return SERVICE_GPU_ROUTE_REQUIRED;
        case SERVICE_CMD_GET_STARTUP_POLICY:
        case SERVICE_CMD_SET_STARTUP_POLICY:
        case SERVICE_CMD_REFRESH_STARTUP_PROFILE:
            return SERVICE_GPU_ROUTE_POLICY;
        case SERVICE_CMD_GET_SNAPSHOT:
        case SERVICE_CMD_GET_TELEMETRY:
        case SERVICE_CMD_GET_ACTIVE_DESIRED:
        case SERVICE_CMD_WRITE_LOG_SNAPSHOT:
        case SERVICE_CMD_WRITE_JSON_SNAPSHOT:
        case SERVICE_CMD_WRITE_PROBE_REPORT:
        case SERVICE_CMD_CAPTURE_CURRENT:
            return SERVICE_GPU_ROUTE_OPTIONAL;
        default:
            // PING, LOGON_HANDOFF, RESUME_RESTORE and the updater are machine-
            // wide events and triggers. GET_OPERATION_RESULT recovers a lost
            // response by its operation id alone, whichever GPU it wrote, so
            // a switch in between cannot hide or misattribute the outcome.
            // Unknown commands are refused before this.
            return SERVICE_GPU_ROUTE_MACHINE;
    }
}

// May a request of this command carry a target at all? Machine-wide commands
// never do; of the policy commands only SET (PROFILE binding) and REFRESH
// (the expected binding) do, under their own rules in the validator.
static inline bool service_command_accepts_target(gc_u32 command) {
    const ServiceGpuRoute route = service_command_gpu_route(command);
    return route != SERVICE_GPU_ROUTE_MACHINE && command != SERVICE_CMD_GET_STARTUP_POLICY;
}

static inline const char* service_gpu_route_name(ServiceGpuRoute route) {
    switch (route) {
        case SERVICE_GPU_ROUTE_OPTIONAL: return "optional";
        case SERVICE_GPU_ROUTE_REQUIRED: return "required";
        case SERVICE_GPU_ROUTE_POLICY:   return "policy";
        default:                         return "machine";
    }
}

// Does a route address the GPU being served? It must resolve to exactly one
// live adapter (gpu_adapter_route_index), and that adapter must be the one
// served. An absent route addresses the served GPU; when nothing is served
// (servedIndex < 0), nothing is addressed.
static inline bool service_gpu_route_addresses(const GpuAdapterInfo& requested,
                                               const GpuAdapterInfo* adapters,
                                               unsigned int count, int servedIndex) {
    if (servedIndex < 0 || (unsigned int)servedIndex >= count) return false;
    if (!requested.valid) return true;
    return gpu_adapter_route_index(&requested, adapters, count) == servedIndex;
}

enum ServiceGpuRouteDisposition {
    SERVICE_GPU_ROUTE_SERVE = 0,    // answer for the served GPU
    SERVICE_GPU_ROUTE_SELECT = 1,   // state read: move to the target, guarded
    SERVICE_GPU_ROUTE_BOUND = 2,    // Apply/Reset: the preconditions bind it
    SERVICE_GPU_ROUTE_NOT_SERVED = 3,
};

#define SERVICE_GPU_ROUTE_NOT_SERVED_MESSAGE \
    "The requested GPU is not the GPU the service is serving; refresh it first"

// The step-3 serving rule. `addressesServed` is service_gpu_route_addresses()
// evaluated against the platform's live adapters under its own lock.
static inline ServiceGpuRouteDisposition service_gpu_route_disposition(
    gc_u32 command, bool targetValid, bool addressesServed) {
    if (command == SERVICE_CMD_APPLY || command == SERVICE_CMD_RESET)
        return SERVICE_GPU_ROUTE_BOUND;
    const ServiceGpuRoute route = service_command_gpu_route(command);
    if (!targetValid || route == SERVICE_GPU_ROUTE_MACHINE ||
        route == SERVICE_GPU_ROUTE_POLICY)
        return SERVICE_GPU_ROUTE_SERVE;
    if (command == SERVICE_CMD_GET_SNAPSHOT) return SERVICE_GPU_ROUTE_SELECT;
    return addressesServed ? SERVICE_GPU_ROUTE_SERVE
                           : SERVICE_GPU_ROUTE_NOT_SERVED;
}

static inline const char* service_gpu_route_disposition_name(
    ServiceGpuRouteDisposition disposition) {
    switch (disposition) {
        case SERVICE_GPU_ROUTE_SELECT:     return "select";
        case SERVICE_GPU_ROUTE_BOUND:      return "bound";
        case SERVICE_GPU_ROUTE_NOT_SERVED: return "not-served";
        default:                           return "serve";
    }
}

// Does an answered (OK) routed request describe a GPU other than the one it
// was routed to? The envelope's published selected adapter is the GPU the
// answer is about; a dispatcher whose handler can be preceded by a retarget
// refuses such an answer instead of returning it. Only optional routes apply:
// the state read, Apply/Reset and the boot policy carry their own binding.
static inline bool service_gpu_route_answer_mismatch(const ServiceRequest& request,
                                                     const ServiceResponse& response) {
    if (response.status != SERVICE_STATUS_OK || !request.targetGpu.valid ||
        service_gpu_route_disposition(request.command, true, false) != SERVICE_GPU_ROUTE_NOT_SERVED)
        return false;
    const ServiceSnapshot& published = response.snapshot;
    const unsigned int count = published.adapterCount < MAX_GPU_ADAPTERS
        ? published.adapterCount : MAX_GPU_ADAPTERS;
    const bool identity = (response.state.validSections & SERVICE_STATE_SECTION_ADAPTER_IDENTITY) != 0;
    const int served = identity && published.selectedAdapterIndex < count
        ? (int)published.selectedAdapterIndex : -1;
    return !service_gpu_route_addresses(request.targetGpu, published.adapters, count, served);
}

#endif  // GREEN_CURVE_SERVICE_GPU_ROUTE_H
