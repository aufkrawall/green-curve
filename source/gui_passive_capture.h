// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#pragma once
#include "service_protocol.h"

struct GuiPassiveCaptureWork {
    ServiceRequest request{};
    gc_u64 token = 0;
};
// The coordinator lock protects pending work. Only this fixed getter verb can
// enter the passive queue; no caller-provided request is accepted.
static inline bool gui_passive_capture_prepare(const GpuAdapterInfo& target, gc_u32 domains,
    gc_u64 token, GuiPassiveCaptureWork& out) {
    if (!target.valid || !token || !domains || (domains & ~511u)) return false;
    out = {}; out.token = token;
    out.request.magic = SERVICE_PROTOCOL_MAGIC; out.request.version = SERVICE_PROTOCOL_VERSION;
    out.request.command = SERVICE_CMD_CAPTURE_CURRENT; out.request.targetGpu = target; out.request.captureDomains = domains;
    return true;
}
