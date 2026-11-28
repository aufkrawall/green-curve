# Multi-GPU: request routing, adapter summary and per-GPU persistence

The service (Windows) and the daemon (Linux) still serve ONE GPU at a time,
and only one GPU may hold active settings (the single-owner guard on both
platforms). This page records the shared contract that lets a later change
give every GPU its own runtime without another protocol change.

## Source anchors

- `source/service_gpu_route.h`: the command route table, the serving rule and
  its refusal text.
- `source/gpu_selection_policy.h`: `gpu_adapter_route_index()`, which resolves
  a route to exactly one live adapter (or none).
- `source/service_protocol_validation.h`: the wire rule.
- Linux: `daemon_route_admits()` / `daemon_served_adapter_index()` in
  `source/linux_daemon_snapshot_runtime.cpp`, called from `handle_request()`.
- Windows: `service_route_admits()` in `source/main_service_route.cpp`, ahead
  of the command switch in `source/main_service_pipe_switch.cpp`.
- Tests: `tests/service_gpu_route_tests.cpp` (14600-14629, 14637-14638 and 14641-14643, both hosts) and the
  production daemon handler in `tests/linux_daemon_commit_rollback_tests.cpp`
  (14630-14636, refresh binding 14639-14640).

## One routing field (protocol v40, 2026-11-27)

`ServiceRequest.targetGpu` is the request's only GPU field. It names the GPU
the command is about; absent, it means the GPU the service serves. There is
no second GPU or UUID field that could disagree with it, and no `#ifdef` in
how it is read.

| Route | Commands | Target on the wire |
|---|---|---|
| machine | PING, LOGON_HANDOFF, GET_OPERATION_RESULT, RESUME_RESTORE, the four updater commands | refused ("machine-wide command carries a target GPU") |
| required | APPLY, RESET | mandatory (unchanged) |
| optional | GET_SNAPSHOT, GET_TELEMETRY, GET_ACTIVE_DESIRED, the three file-write commands, CAPTURE_CURRENT | optional |
| policy | GET_STARTUP_POLICY, SET_STARTUP_POLICY, REFRESH_STARTUP_PROFILE | PROFILE names its binding, other modes and GET carry none, a refresh may name the card its binding must be (an attachment must be complete) |

Changes against v39: PING, LOGON_HANDOFF and GET_OPERATION_RESULT refuse a
target (no client sent one). An operation result is recovered by its id alone,
so a GPU switch between a lost response and its recovery query cannot hide the
outcome; the snapshot attached to that answer is still the currently served
GPU's, not the one the operation wrote. The Linux boot policy stays one
daemon-wide choice until every GPU has its own runtime: its PROFILE target is
the stored binding and may name a card the daemon is not serving, as before. A
refresh may name the card it expects; the daemon refuses it (STALE_STATE)
unless the stored binding resolves to that card, and reuses the binding
verbatim, so a refresh never re-binds.

Serving rule while one GPU is served (`service_gpu_route_disposition()`):

- no target, or a target addressing the served GPU: answered;
- GET_SNAPSHOT naming another GPU: the existing guarded switch
  (`select_request_gpu()` on Linux, `service_prepare_requested_gpu()` on
  Windows), still refused while the served GPU holds active settings;
- APPLY/RESET: unchanged; their reconnect preconditions already require the
  target to be the published selected GPU;
- any other routed command naming another GPU: `SERVICE_STATUS_STALE_STATE`,
  "The requested GPU is not the GPU the service is serving; refresh it first",
  before the handler runs. Diagnostics: `daemon route: refused ...` (Linux),
  `service route: refused ...` (Windows).

A route addresses the served GPU only if it resolves to exactly one live
adapter and that adapter is the served one (`service_gpu_route_addresses()`):
base PCI identity plus exact location when both sides carry PCI data,
otherwise the complete adapter record. Identical cards at an unknown location
never resolve. "Served" is the published selected adapter: on Linux
`selectedAdapterIndex` only while `writeIdentityResolved` (the read-only
telemetry fallback of a multi-GPU host is not served); on Windows
`selectedGpuIndex` while `selectedGpuIdentityValid`, read under `g_appLock`.
Passive capture keeps its own binding check (`hardware_capture_target_matches()`)
behind the route gate.

The Windows gate runs before a handler takes the runtime lock, so a logon or
recovery retarget can intervene. Two checks close that: a file-write command
re-runs the route gate under the runtime lock, again after hardware
initialization (which can itself re-enumerate), right before writing anything;
and for every other routed command, after the envelope is stamped,
`service_gpu_route_answer_mismatch()` turns an OK answer whose published
selected adapter is not the routed GPU into STALE_STATE and clears any capture
(`service route: answer for command=... refused`). A file command's answer,
like every response, carries the envelope stamped after its handler, so after
a retarget it can describe the newly served GPU; the file itself is bound to
the routed GPU under the lock. The Linux
daemon handles each request under one lock that also covers every retarget.

GET_SNAPSHOT keeps each platform's refusal convention for a refused switch:
Windows answers OK with the kept GPU and the refusal text (its GUI and CLI
read it that way), Linux answers ERROR (its TUI reads it that way). A cold
Linux daemon still accepts a targeted GET_SNAPSHOT to bring the backend up.

Behavior deltas: Windows telemetry, active-intent and file commands and Linux
active-intent requests used to ignore a target, Linux GET_TELEMETRY used to
switch, and CAPTURE_CURRENT for another GPU now answers STALE_STATE from the
route gate instead of ERROR from its handler. No in-tree client sends a target
on those other than the shown GPU.
