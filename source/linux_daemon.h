// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// linux_daemon.h — root GPU-control daemon + thin-client transport.
//
// Mirrors the Windows elevated-service / named-pipe split (LACT's lactd model):
// a root daemon owns the GPU (NvAPI/NVML via linux_backend) and serves the
// binary ServiceRequest/ServiceResponse protocol (gpu_core.h) over a Unix
// domain socket; unprivileged TUI/CLI clients connect and send requests.

#ifndef GREEN_CURVE_LINUX_DAEMON_H
#define GREEN_CURVE_LINUX_DAEMON_H

#include <stddef.h>

#include "gpu_core.h"

#define GC_DAEMON_SOCKET_DIR  "/run/greencurve"
#define GC_DAEMON_SOCKET_NAME "greencurve.sock"
#define GC_DAEMON_SOCKET_PATH GC_DAEMON_SOCKET_DIR "/" GC_DAEMON_SOCKET_NAME
#define GC_DAEMON_STATE_DIR   "/var/lib/greencurve"
#define GC_DAEMON_STATE_FILE  "/var/lib/greencurve/active.bin"
#define GC_DAEMON_OPERATION_FILE "/var/lib/greencurve/operation.bin"
// Boot-apply policy.  Root-owned and checksummed like the other two records:
// it decides whether an unattended hardware write happens at daemon start.
#define GC_DAEMON_STARTUP_FILE "/var/lib/greencurve/startup.bin"
// Automatic-restore guard: how many unattended start-time writes this boot has
// already spent, and whether automatic restoration is latched off entirely.
// Same protection as the records above for the same reason -- it is the only
// thing standing between a setting that hangs the driver and an endless
// crash / systemd-restart / replay loop.
#define GC_DAEMON_GUARD_FILE "/var/lib/greencurve/restore-guard.bin"

// Pending-connection backlog.  Requests are serviced one at a time under the
// runtime lock, so this only has to absorb bursts while one request is in
// flight; poll() keeps a stalled peer from blocking the loop before accept().
#define GC_DAEMON_LISTEN_BACKLOG 8

// Run the daemon event loop (blocks).  `configPath` is used for the
// startup restart-reapply fallback.  Returns a process exit code.
int linux_daemon_run(const char* configPath);

// Client: connect to the daemon, send `req`, receive `resp`.  Returns false +
// message if the daemon is not reachable or the exchange fails.
bool linux_daemon_send(const ServiceRequest* req, ServiceResponse* resp,
                       char* err, size_t errSize);
bool linux_daemon_snapshot(ServiceSnapshot* snapshot, char* err, size_t errSize);
bool linux_daemon_get_state(const GpuAdapterInfo* target, ServiceResponse* response,
                            char* err, size_t errSize);

// Convenience client helpers used by the CLI/TUI.
bool linux_daemon_apply(const GpuAdapterInfo* target, const DesiredSettings* desired, bool interactive,
                        char* result, size_t resultSize);
bool linux_daemon_reset(const GpuAdapterInfo* target, char* result, size_t resultSize);
bool linux_daemon_apply_checked(const GpuAdapterInfo* target,
                                const DesiredSettings* desired, bool interactive,
                                const ServiceStateEnvelope* expected,
                                ServiceResponse* response,
                                char* result, size_t resultSize);
bool linux_daemon_reset_checked(const GpuAdapterInfo* target,
                                const ServiceStateEnvelope* expected,
                                ServiceResponse* response,
                                char* result, size_t resultSize);

// Tell the daemon the machine came back from suspend, so it writes the intent
// it already holds again.  Sent by greencurve-resume.service; carries no
// settings, no target and no operation id -- the daemon owns all three, exactly
// as the Windows service owns its standby restore.
bool linux_daemon_resume_restore(char* result, size_t resultSize);

// Record this process's daemon-access context (group membership, socket owner/
// mode, whether it can actually open the socket) in the debug log, once, before
// anything can fail. Answers "why can't this user talk to the daemon" even for
// runs where a later request happened to succeed.
void linux_daemon_log_client_environment();

// Ask the daemon which exact GPU identity it would write to, preferring
// `preferred` when it is valid.  Fails closed when no exact identity is
// published yet (multi-GPU without a selection, or a degraded backend).
bool linux_daemon_resolve_write_target(const GpuAdapterInfo* preferred,
                                       GpuAdapterInfo* out,
                                       char* err, size_t errSize);

// Startup-apply policy: what the daemon writes to the GPU when it starts.
// The set path snapshots the resolved settings because the daemon runs with
// ProtectHome=yes and cannot read the caller's config.ini itself.
bool linux_daemon_get_startup_policy(ServiceResponse* response,
                                     char* err, size_t errSize);
bool linux_daemon_set_startup_policy(unsigned int mode, int profileSlot,
                                     const char* profileName,
                                     const GpuAdapterInfo* target,
                                     const DesiredSettings* desired,
                                     char* result, size_t resultSize);
// Replace the settings of an already-bound `profile N` policy after slot N was
// rewritten on disk.  Keeps the stored GPU binding, mode, slot and name, so it
// is not a re-bind; fails when the daemon holds no such policy.
bool linux_daemon_refresh_startup_profile(int profileSlot,
                                          const DesiredSettings* desired,
                                          char* result, size_t resultSize);

// Install / remove the systemd unit (greencurve.service running `--daemon`) and
// the greencurve admin group.  Require root.  Return 0 on success.
int linux_service_install(char* err, size_t errSize,
                          ServiceResponse* verifiedResponse = nullptr);
int linux_service_remove(char* err, size_t errSize);

// Group paragraph for the install summary: confirms access when the invoking
// account is already enrolled, and only prescribes usermod when it is not.
void linux_describe_group_enrollment(char* out, size_t outSize);

#endif // GREEN_CURVE_LINUX_DAEMON_H
