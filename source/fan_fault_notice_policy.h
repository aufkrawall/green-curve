// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// What a client says when the fan runtime handed a fan back (protocol v39
// ServiceFanFault).  Shared by the Windows GUI, the GUI-process fan runtime,
// the Linux TUI and the Linux live/status output, so every surface names the
// same fan, duty and outcome in the same words.
//
// Each recorded fault is announced once per client: the producer numbers its
// faults, and a client announces a serial (or kind) it has not seen.  A record
// that returns to NONE re-arms the observer, so a restarted producer's first
// fault is news again.
#ifndef GREEN_CURVE_FAN_FAULT_NOTICE_POLICY_H
#define GREEN_CURVE_FAN_FAULT_NOTICE_POLICY_H

#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "service_protocol.h"

struct GcFanFaultObserver {
    gc_u32 lastSerial;
    gc_u32 lastKind;
};

// True when `fault` is one this client has not announced yet.
static inline bool gc_fan_fault_observe(GcFanFaultObserver* observer,
                                        const ServiceFanFault* fault) {
    if (!observer || !fault) return false;
    if (fault->kind == SERVICE_FAN_FAULT_NONE) {
        observer->lastSerial = 0;
        observer->lastKind = SERVICE_FAN_FAULT_NONE;
        return false;
    }
    const bool fresh = fault->serial != observer->lastSerial ||
                       fault->kind != observer->lastKind;
    observer->lastSerial = fault->serial;
    observer->lastKind = fault->kind;
    return fresh;
}

static inline const char* gc_fan_fault_kind_name(gc_u32 kind) {
    switch (kind) {
        case SERVICE_FAN_FAULT_NONE: return "none";
        case SERVICE_FAN_FAULT_REPEATED_FAILURES: return "repeated-failures";
        case SERVICE_FAN_FAULT_STALL: return "stall";
    }
    return "unknown";
}

static inline const char* gc_fan_fault_handback_name(gc_u8 handback) {
    switch (handback) {
        case SERVICE_FAN_HANDBACK_NONE: return "none";
        case SERVICE_FAN_HANDBACK_DRIVER_AUTO: return "driver-auto";
        case SERVICE_FAN_HANDBACK_EMERGENCY_MAX: return "emergency-max";
        case SERVICE_FAN_HANDBACK_FAILED: return "failed";
    }
    return "unknown";
}

// The outcome as the end of a sentence.
static inline const char* gc_fan_fault_handback_phrase(gc_u8 handback) {
    switch (handback) {
        case SERVICE_FAN_HANDBACK_DRIVER_AUTO:
            return "the driver controls the fan again";
        case SERVICE_FAN_HANDBACK_EMERGENCY_MAX:
            return "the driver refused it back, so the fan was forced to 100%";
        case SERVICE_FAN_HANDBACK_FAILED:
            return "neither driver control nor 100% could be confirmed";
    }
    return "its state is unknown";
}

// Status line, notification title and body for one fault.  Returns false (and
// writes empty strings) when there is nothing to say.  Fans are numbered from
// 1 for people.
static inline bool gc_fan_fault_compose(const ServiceFanFault* fault,
                                        char* status, size_t statusSize,
                                        char* title, size_t titleSize,
                                        char* body, size_t bodySize) {
    if (status && statusSize) status[0] = '\0';
    if (title && titleSize) title[0] = '\0';
    if (body && bodySize) body[0] = '\0';
    if (!fault || !service_fan_fault_wire_valid(fault) ||
        fault->kind == SERVICE_FAN_FAULT_NONE)
        return false;
    const char* outcome = gc_fan_fault_handback_phrase(fault->handback);
    const bool unconfirmed = fault->handback == SERVICE_FAN_HANDBACK_FAILED;
    if (fault->kind == SERVICE_FAN_FAULT_STALL) {
        const unsigned int fan = (unsigned int)fault->fanIndex + 1u;
        const unsigned int pct = (unsigned int)fault->commandedPercent;
        const unsigned int seconds = (unsigned int)((fault->stalledForMs + 500u) / 1000u);
        if (status && statusSize)
            snprintf(status, statusSize,
                     "Fan %u stopped turning at %u%%: custom fan control is off and %s. %s",
                     fan, pct, outcome,
                     unconfirmed ? "Check GPU cooling now." : "Check the fan.");
        if (title && titleSize) snprintf(title, titleSize, "GPU fan stopped turning");
        if (body && bodySize)
            snprintf(body, bodySize,
                     "Fan %u read stopped for %u s at %u%%. Custom fan control is off and %s. "
                     "Check the fan before applying a custom fan setting again.",
                     fan, seconds, pct, outcome);
        return true;
    }
    if (status && statusSize)
        snprintf(status, statusSize,
                 "Custom fan control stopped after repeated fan errors: %s.%s",
                 outcome, unconfirmed ? " Check GPU cooling now." : "");
    if (title && titleSize) snprintf(title, titleSize, "Custom fan control stopped");
    if (body && bodySize)
        snprintf(body, bodySize,
                 "Fan writes or temperature reads kept failing (%s). Custom fan control is off and %s.",
                 fault->detail[0] ? fault->detail : "no detail recorded", outcome);
    return true;
}

#endif // GREEN_CURVE_FAN_FAULT_NOTICE_POLICY_H
