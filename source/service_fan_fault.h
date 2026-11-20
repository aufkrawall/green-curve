// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// Protocol v39 wire record, split out of service_protocol.h (size ratchet);
// included by it right before ServiceSnapshot, which carries it last.
#ifndef GREEN_CURVE_SERVICE_FAN_FAULT_H
#define GREEN_CURVE_SERVICE_FAN_FAULT_H

// v39: the last time the fan runtime gave up a fan it was driving and handed
// it back -- after repeated write/telemetry failures, or because a commanded
// fan did not turn (fan_stall_policy.h).  Before it, the Windows service did
// this silently: the GUI kept showing the curve while the driver controlled
// the fan, and only an error log file said otherwise.  All zero when there is
// none.  In memory only: a restarted service or daemon did not see the fault.
enum ServiceFanFaultKind : gc_u32 {
    SERVICE_FAN_FAULT_NONE = 0,
    SERVICE_FAN_FAULT_REPEATED_FAILURES = 1,
    SERVICE_FAN_FAULT_STALL = 2,
    SERVICE_FAN_FAULT_KIND_MAX = SERVICE_FAN_FAULT_STALL,
};

// What became of the fan.
enum ServiceFanFaultHandback : gc_u8 {
    SERVICE_FAN_HANDBACK_NONE = 0,
    SERVICE_FAN_HANDBACK_DRIVER_AUTO = 1,    // the driver controls it again
    SERVICE_FAN_HANDBACK_EMERGENCY_MAX = 2,  // driver refused; forced to 100%
    SERVICE_FAN_HANDBACK_FAILED = 3,         // neither write was confirmed
    SERVICE_FAN_HANDBACK_MAX = SERVICE_FAN_HANDBACK_FAILED,
};

enum { SERVICE_FAN_FAULT_DETAIL_CHARS = 104 };
struct ServiceFanFault {
    gc_u32 kind;              // ServiceFanFaultKind
    gc_u32 serial;            // 1, 2, ... per fault in this producer instance
    gc_u32 fanIndex;          // the stalled fan (0-based); 0 otherwise
    gc_u32 commandedPercent;  // the duty it did not turn at; 0 otherwise
    gc_u32 stalledForMs;      // how long it read stopped; 0 otherwise
    gc_u8 handback;           // ServiceFanFaultHandback
    gc_u8 reserved[3];
    char detail[SERVICE_FAN_FAULT_DETAIL_CHARS];
};
static_assert(sizeof(ServiceFanFault) == 128, "ServiceFanFault is a fixed wire record");

static inline bool service_fan_fault_wire_valid(const ServiceFanFault* f) {
    if (!f) return false;
    if (f->kind > SERVICE_FAN_FAULT_KIND_MAX || f->handback > SERVICE_FAN_HANDBACK_MAX ||
        f->reserved[0] || f->reserved[1] || f->reserved[2])
        return false;
    bool terminated = false;
    for (unsigned int i = 0; i < sizeof(f->detail); ++i)
        if (f->detail[i] == '\0') { terminated = true; break; }
    if (!terminated) return false;
    if (f->kind == SERVICE_FAN_FAULT_NONE) {
        // Nothing to report means nothing at all.
        return f->serial == 0 && f->fanIndex == 0 && f->commandedPercent == 0 &&
               f->stalledForMs == 0 && f->handback == SERVICE_FAN_HANDBACK_NONE &&
               f->detail[0] == '\0';
    }
    if (f->serial == 0 || f->handback == SERVICE_FAN_HANDBACK_NONE ||
        f->fanIndex >= MAX_GPU_FANS || f->commandedPercent > 100)
        return false;
    // Only a stall names a fan, a duty and a duration.
    if (f->kind != SERVICE_FAN_FAULT_STALL &&
        (f->fanIndex || f->commandedPercent || f->stalledForMs))
        return false;
    return true;
}

// A stateless refusal carries none: every byte zero.
static inline bool service_fan_fault_is_zero(const ServiceFanFault& f) {
    const unsigned char* bytes = reinterpret_cast<const unsigned char*>(&f);
    for (unsigned int i = 0; i < sizeof(f); ++i)
        if (bytes[i]) return false;
    return true;
}

#endif // GREEN_CURVE_SERVICE_FAN_FAULT_H
