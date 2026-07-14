// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#pragma once

// The Windows service currently serializes connected pipe requests. A timeout
// from telemetry/ping while this GUI owns a long mutation therefore means
// "expected busy", not "service unavailable". SCM STOPPED still wins.
static inline bool service_health_probe_should_defer(bool serviceProcess,
    bool ownMutationActive, bool installed, bool running) {
    return !serviceProcess && ownMutationActive && installed && running;
}
