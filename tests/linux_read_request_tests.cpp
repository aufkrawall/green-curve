// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
// Exercise the client flag builder against the daemon's actual wire validator.

#include "gpu_core.h"
#include <cstdio>
#include <cstring>
#include <initializer_list>

int run_linux_read_request_tests() {
    for (unsigned int command : {SERVICE_CMD_GET_SNAPSHOT, SERVICE_CMD_GET_TELEMETRY}) {
        ServiceRequest request = {};
        request.magic = SERVICE_PROTOCOL_MAGIC;
        request.version = SERVICE_PROTOCOL_VERSION;
        request.callerPid = 1;
        request.command = command;
        request.flags = service_request_flags_for_command(command, true);
        if (request.flags != SERVICE_REQUEST_FLAG_INTERACTIVE ||
            !validate_service_request_for_ipc(&request)) return 7710;
        request.flags = service_request_flags_for_command(command, false);
        if (request.flags || !validate_service_request_for_ipc(&request)) return 7711;
        request.flags = SERVICE_REQUEST_FLAG_INTERACTIVE;
        request.resetOcBeforeApply = 1;
        const char* reason = service_request_reject_reason(&request);
        if (!reason || strcmp(reason, "read-only command carries mutation fields"))
            return 7712;
        request.resetOcBeforeApply = 0;
        request.operationId = 1;
        if (validate_service_request_for_ipc(&request)) return 7713;
        request.operationId = 0;
        for (gc_u32 flags : {SERVICE_REQUEST_FLAG_SHARED_SLOT,
                1u << SERVICE_REQUEST_SHARED_SLOT_SHIFT, 0x80000000u}) {
            request.flags = SERVICE_REQUEST_FLAG_INTERACTIVE | flags;
            if (validate_service_request_for_ipc(&request)) return 7714;
        }
        request.flags = SERVICE_REQUEST_FLAG_INTERACTIVE;
        request.command = SERVICE_CMD_PING;
        if (validate_service_request_for_ipc(&request)) return 7715;
    }
    for (unsigned int command = SERVICE_CMD_PING;
         command <= SERVICE_CMD_SET_UPDATE_POLICY; ++command) {
        const bool consumesFlag = command == SERVICE_CMD_APPLY ||
            command == SERVICE_CMD_GET_SNAPSHOT || command == SERVICE_CMD_GET_TELEMETRY;
        if ((service_request_flags_for_command(command, true) != 0) != consumesFlag ||
            service_request_flags_for_command(command, false)) return 7716;
    }
    return 0;
}

#ifdef GC_READ_REQUEST_TEST_MAIN
int main() {
    const int failure = run_linux_read_request_tests();
    printf("Linux read request regression: %d\n", failure);
    return failure;
}
#endif
