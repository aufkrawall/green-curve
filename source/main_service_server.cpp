// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT

// Windows service host aggregation. Kept as one include surface for the
// amalgamated build while the implementation stays in focused shards.

#include "main_service_request_policy.cpp"
#include "main_service_operation_persist.cpp"
#include "main_service_snapshot_request.cpp"
// The updater, in dependency order: state and settings, then the two things
// that touch untrusted bytes (network, crypto), then the orchestration that
// uses all three.  It sits ahead of the pipe shard because the command handlers
// there call into it.
#include "main_service_update_state.cpp"
#include "main_service_update_fetch.cpp"
#include "main_service_update_verify.cpp"
// Before the worker: the orchestration there calls straight into it.
#include "main_service_update_gui_stop.cpp"
#include "main_service_update_worker.cpp"
// After the worker: the cache re-runs the worker's own verify/parse/decide
// sequence over the documents the last check stored, and re-adopts a staged
// package through the worker's re-verification helper.
#include "main_service_update_cache.cpp"
#include "main_service_update_worker_thread.cpp"
#include "main_service_update_commands.cpp"
#include "main_service_pipe.cpp"
#include "main_service_pipe_listener.cpp"
#include "main_service_host.cpp"
