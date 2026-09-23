// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#ifndef GREEN_CURVE_SERVICE_GATE_PROGRESS_H
#define GREEN_CURVE_SERVICE_GATE_PROGRESS_H

// All state the service's wedge watchdog reads.  It lives in its own header
// because main.cpp sits on its size ratchet, and keeping the two heartbeats
// together is the point: they answer different questions and the watchdog was
// wrong for as long as it asked only the first one.

// Heartbeat written by the fan runtime thread at the START of every pulse
// attempt (just before any NVML call) and again on completion.  The main-loop
// watchdog uses it to detect a fan thread WEDGED inside nvml.dll on a dead
// driver (a hang the VEH cannot catch) and request controlled process recovery.
static volatile ULONGLONG g_serviceFanPulseHeartbeatMs = 0;
static volatile LONG g_serviceFanPulseInFlight = 0;

// Last moment the service's hardware gate demonstrably MOVED: the runtime lock
// was taken or released, a fan pulse finished its NVML work, or an apply
// entered its next phase.  The wedge watchdog needs this because the fan
// pulse's own heartbeat is stamped BEFORE it queues on the runtime lock, so a
// pulse waiting behind a legitimately slow apply is indistinguishable from one
// hung inside a dead nvml.dll.  It is not: on 2026-09-17 an under-load apply
// spent ~12 s in its VF correction loop, the queued pulse aged past
// SERVICE_FAN_PULSE_WEDGE_TIMEOUT_MS, and the service tore down a driver that
// was working -- the client's apply died with a broken pipe and automatic
// restore was locked out.  Progress, not waiter age, is the signal: a wedge
// stops everything, so nothing stamps this, while a slow apply keeps stamping.
static volatile ULONGLONG g_serviceHardwareProgressMs = 0;

static void service_note_hardware_progress();

// Hardware work the watchdog must be able to see hang even when no fan pulse
// is queued behind it (service_wedge_watchdog_policy.h).  Nested scopes share
// one depth; the label names the outermost one for the watchdog's log line.
static volatile LONG g_serviceHardwareWorkDepth = 0;
static const char* volatile g_serviceHardwareWorkLabel = nullptr;

struct ServiceHardwareWorkScope {
    explicit ServiceHardwareWorkScope(const char* label) {
        service_note_hardware_progress();
        if (InterlockedIncrement(&g_serviceHardwareWorkDepth) == 1)
            g_serviceHardwareWorkLabel = label;
    }
    ~ServiceHardwareWorkScope() {
        service_note_hardware_progress();
        if (InterlockedDecrement(&g_serviceHardwareWorkDepth) == 0)
            g_serviceHardwareWorkLabel = nullptr;
    }
    ServiceHardwareWorkScope(const ServiceHardwareWorkScope&) = delete;
    ServiceHardwareWorkScope& operator=(const ServiceHardwareWorkScope&) = delete;
};

#endif
