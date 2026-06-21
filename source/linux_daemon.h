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
#define GC_DAEMON_SOCKET_PATH "/run/greencurve/greencurve.sock"
#define GC_DAEMON_STATE_DIR   "/var/lib/greencurve"
#define GC_DAEMON_STATE_FILE  "/var/lib/greencurve/active.bin"

// Run the daemon event loop (blocks).  `configPath` is used for the
// startup restart-reapply fallback.  Returns a process exit code.
int linux_daemon_run(const char* configPath);

// Client: connect to the daemon, send `req`, receive `resp`.  Returns false +
// message if the daemon is not reachable or the exchange fails.
bool linux_daemon_send(const ServiceRequest* req, ServiceResponse* resp,
                       char* err, size_t errSize);

// Convenience client helpers used by the CLI/TUI.
bool linux_daemon_apply(const DesiredSettings* desired, bool interactive,
                        char* result, size_t resultSize);
bool linux_daemon_reset(char* result, size_t resultSize);
bool linux_daemon_available();

// Install / remove the systemd unit (greencurve.service running `--daemon`) and
// the greencurve admin group.  Require root.  Return 0 on success.
int linux_service_install(char* err, size_t errSize);
int linux_service_remove(char* err, size_t errSize);

#endif // GREEN_CURVE_LINUX_DAEMON_H
