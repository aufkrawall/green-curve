// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT

// Windows service runtime aggregation. The active implementation is split by
// responsibility; the immutable session resolver consumes this enum later.

#include "main_service_runtime_identity.cpp"
// Before any shard that acquires the runtime lock for a GPU write.
#include "main_service_update_guard.cpp"
#include "main_service_fan_worker.cpp"
#include "main_service_apply_runtime.cpp"

enum ServiceLogonProfileResolveResult {
    SERVICE_LOGON_PROFILE_RESOLVED = 0,
    SERVICE_LOGON_PROFILE_NONE,
    SERVICE_LOGON_PROFILE_TRANSIENT,
    SERVICE_LOGON_PROFILE_INVALID,
};
