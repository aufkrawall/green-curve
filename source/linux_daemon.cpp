// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// linux_daemon.cpp — root GPU-control daemon + thin-client transport.
// See linux_daemon.h.

#ifndef _GNU_SOURCE
#define _GNU_SOURCE  // struct ucred + SO_PEERCRED
#endif

#include "linux_daemon.h"
#include "linux_backend.h"
#include "platform.h"

#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#ifndef APP_VERSION
#define APP_VERSION "0.0"
#endif
#ifndef APP_BUILD_NUMBER
#define APP_BUILD_NUMBER 0
#endif

// ===========================================================================
// Framed blocking read/write of the fixed-size protocol structs.
// ===========================================================================
#define GC_DAEMON_IO_TIMEOUT_MS 2000

static unsigned long long monotonic_ms() {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return (unsigned long long)ts.tv_sec * 1000ULL + (unsigned long long)(ts.tv_nsec / 1000000ULL);
}

static bool set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return false;
    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0) return false;
    return true;
}

static bool wait_fd_ready(int fd, short events, unsigned long long deadlineMs) {
    for (;;) {
        unsigned long long now = monotonic_ms();
        if (now >= deadlineMs) return false;
        unsigned long long remaining = deadlineMs - now;
        int timeout = remaining > 2147483647ULL ? 2147483647 : (int)remaining;
        struct pollfd pfd;
        memset(&pfd, 0, sizeof(pfd));
        pfd.fd = fd;
        pfd.events = events;
        int r = poll(&pfd, 1, timeout);
        if (r > 0) {
            if (pfd.revents & events) return true;
            if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) return false;
        } else if (r == 0) {
            return false;
        } else if (errno != EINTR) {
            return false;
        }
    }
}

static bool read_full(int fd, void* buf, size_t len) {
    unsigned char* p = (unsigned char*)buf;
    size_t got = 0;
    unsigned long long deadline = monotonic_ms() + GC_DAEMON_IO_TIMEOUT_MS;
    while (got < len) {
        if (!wait_fd_ready(fd, POLLIN, deadline)) return false;
        ssize_t n = read(fd, p + got, len - got);
        if (n > 0) { got += (size_t)n; continue; }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) continue;
        return false;
    }
    return true;
}
static bool write_full(int fd, const void* buf, size_t len) {
    const unsigned char* p = (const unsigned char*)buf;
    size_t put = 0;
    unsigned long long deadline = monotonic_ms() + GC_DAEMON_IO_TIMEOUT_MS;
    while (put < len) {
        if (!wait_fd_ready(fd, POLLOUT, deadline)) return false;
        ssize_t n = write(fd, p + put, len - put);
        if (n > 0) { put += (size_t)n; continue; }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) continue;
        return false;
    }
    return true;
}

static void dlog(const char* fmt, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 1, 2)))
#endif
    ;
static void dlog(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}

// ===========================================================================
// Client side
// ===========================================================================
static int client_connect() {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    gc_strlcpy(addr.sun_path, sizeof(addr.sun_path), GC_DAEMON_SOCKET_PATH);
    if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
        close(fd);
        return -1;
    }
    set_nonblocking(fd);
    return fd;
}

bool linux_daemon_available() {
    int fd = client_connect();
    if (fd < 0) return false;
    close(fd);
    return true;
}

bool linux_daemon_send(const ServiceRequest* req, ServiceResponse* resp,
                       char* err, size_t errSize) {
    if (err && errSize) err[0] = 0;
    int fd = client_connect();
    if (fd < 0) {
        if (err) gc_snprintf(err, errSize, "daemon not reachable at %s (is greencurve.service running?)",
                             GC_DAEMON_SOCKET_PATH);
        return false;
    }
    bool ok = write_full(fd, req, sizeof(*req)) && read_full(fd, resp, sizeof(*resp));
    close(fd);
    if (!ok) { if (err) gc_strlcpy(err, errSize, "daemon I/O error"); return false; }
    if (resp->magic != SERVICE_PROTOCOL_MAGIC) { if (err) gc_strlcpy(err, errSize, "bad daemon response"); return false; }
    validate_service_response_for_ipc(resp);
    if (resp->status == SERVICE_STATUS_VERSION_MISMATCH) {
        if (err) gc_snprintf(err, errSize, "daemon protocol mismatch (client %u, daemon %u)",
                             (unsigned)SERVICE_PROTOCOL_VERSION, (unsigned)resp->version);
        return false;
    }
    return resp->status == SERVICE_STATUS_OK;
}

static bool send_simple(unsigned int command, const DesiredSettings* desired,
                        bool interactive, char* result, size_t resultSize) {
    ServiceRequest req;
    memset(&req, 0, sizeof(req));
    req.magic = SERVICE_PROTOCOL_MAGIC;
    req.version = SERVICE_PROTOCOL_VERSION;
    req.command = command;
    req.flags = interactive ? SERVICE_REQUEST_FLAG_INTERACTIVE : 0;
    req.callerPid = (gc_u32)getpid();
    if (desired) req.desired = *desired;
    ServiceResponse resp;
    memset(&resp, 0, sizeof(resp));
    char err[256] = {};
    bool ok = linux_daemon_send(&req, &resp, err, sizeof(err));
    if (result) gc_strlcpy(result, resultSize, resp.message[0] ? resp.message : (ok ? "OK" : err));
    return ok;
}

bool linux_daemon_apply(const DesiredSettings* desired, bool interactive,
                        char* result, size_t resultSize) {
    return send_simple(SERVICE_CMD_APPLY, desired, interactive, result, resultSize);
}
bool linux_daemon_reset(char* result, size_t resultSize) {
    return send_simple(SERVICE_CMD_RESET, nullptr, true, result, resultSize);
}

// ===========================================================================
// Daemon side
// ===========================================================================
static LinuxGpuState g_gpu;
static bool g_gpuReady = false;
static pl_mutex g_lock;
static DesiredSettings g_activeDesired;
static bool g_hasActiveDesired = false;
static volatile int g_running = 1;

static bool persist_active_desired() {
    if (!g_hasActiveDesired) {
        if (unlink(GC_DAEMON_STATE_FILE) != 0 && errno != ENOENT) {
            dlog("daemon: failed deleting stale active state %s: %s\n", GC_DAEMON_STATE_FILE, strerror(errno));
            return false;
        }
        return true;
    }
    if (mkdir(GC_DAEMON_STATE_DIR, 0755) != 0 && errno != EEXIST) {
        dlog("daemon: failed creating state dir %s: %s\n", GC_DAEMON_STATE_DIR, strerror(errno));
        return false;
    }
    FILE* f = fopen(GC_DAEMON_STATE_FILE, "wb");
    if (!f) {
        dlog("daemon: failed opening active state %s: %s\n", GC_DAEMON_STATE_FILE, strerror(errno));
        return false;
    }
    bool ok = fwrite(&g_activeDesired, sizeof(g_activeDesired), 1, f) == 1;
    ok = fflush(f) == 0 && ok;
    ok = fsync(fileno(f)) == 0 && ok;
    ok = fclose(f) == 0 && ok;
    if (!ok) dlog("daemon: failed writing active state %s\n", GC_DAEMON_STATE_FILE);
    return ok;
}
static bool load_active_desired(DesiredSettings* out) {
    FILE* f = fopen(GC_DAEMON_STATE_FILE, "rb");
    if (!f) return false;
    bool ok = fread(out, sizeof(*out), 1, f) == 1;
    fclose(f);
    return ok;
}

static void populate_snapshot(ServiceSnapshot* s) {
    memset(s, 0, sizeof(*s));
    if (!g_gpuReady) return;
    s->initialized = true;
    s->loaded = (g_gpu.numPopulated > 0);
    s->gpuFamily = g_gpu.family;
    s->numPopulated = g_gpu.numPopulated;
    s->vfReadSupported = g_gpu.backend && g_gpu.backend->readSupported;
    s->vfWriteSupported = g_gpu.backend && g_gpu.backend->writeSupported;
    s->vfBestGuess = g_gpu.backend && g_gpu.backend->bestGuessOnly;
    s->gpuClockOffsetMinMHz = g_gpu.gpuOffsetMinMHz;
    s->gpuClockOffsetMaxMHz = g_gpu.gpuOffsetMaxMHz;
    s->memOffsetMinMHz = g_gpu.memOffsetMinMHz;
    s->memOffsetMaxMHz = g_gpu.memOffsetMaxMHz;
    s->curveOffsetMinkHz = g_gpu.curveOffsetMinKHz;
    s->curveOffsetMaxkHz = g_gpu.curveOffsetMaxKHz;
    s->curveOffsetRangeKnown = g_gpu.curveOffsetRangeKnown;
    s->powerLimitMinmW = g_gpu.powerLimitMinmW;
    s->powerLimitMaxmW = g_gpu.powerLimitMaxmW;
    s->powerLimitDefaultmW = g_gpu.powerLimitDefaultmW;
    for (int i = 0; i < VF_NUM_POINTS; i++) {
        s->curve[i] = g_gpu.curve[i];
        s->freqOffsets[i] = g_gpu.freqOffsets[i];
    }
    gc_strlcpy(s->gpuName, sizeof(s->gpuName), g_gpu.gpuName[0] ? g_gpu.gpuName : "NVIDIA GPU");
    s->adapterCount = 1;
    s->adapters[0].valid = true;
    s->adapters[0].family = g_gpu.family;
    gc_strlcpy(s->adapters[0].name, sizeof(s->adapters[0].name), s->gpuName);
    if (g_gpu.nvml.getTemperature) {
        unsigned int t = 0;
        if (g_gpu.nvml.getTemperature(g_gpu.nvmlDevice, NVML_TEMPERATURE_GPU, &t) == NVML_SUCCESS) {
            s->gpuTemperatureC = (int)t;
            s->gpuTemperatureValid = true;
        }
    }
    if (g_gpu.nvml.getPowerLimit) {
        unsigned int p = 0;
        if (g_gpu.nvml.getPowerLimit(g_gpu.nvmlDevice, &p) == NVML_SUCCESS) s->powerLimitCurrentmW = (int)p;
    }
    if (g_gpu.nvml.getNumFans) {
        unsigned int fans = 0;
        if (g_gpu.nvml.getNumFans(g_gpu.nvmlDevice, &fans) == NVML_SUCCESS) {
            s->fanCount = fans;
            s->fanSupported = (fans > 0);
            for (unsigned int f = 0; f < fans && f < MAX_GPU_FANS; f++) {
                unsigned int pct = 0;
                if (g_gpu.nvml.getFanSpeed && g_gpu.nvml.getFanSpeed(g_gpu.nvmlDevice, f, &pct) == NVML_SUCCESS)
                    s->fanPercent[f] = pct;
            }
        }
    }
}

// Linear fan-curve interpolation (curve mode reassertion).
static int fan_curve_percent_for_temp(const FanCurveConfig* c, int tempC) {
    int best = -1;
    int lastEnabled = 0;
    for (int i = 0; i < FAN_CURVE_MAX_POINTS; i++) {
        if (!c->points[i].enabled) continue;
        lastEnabled = c->points[i].fanPercent;
        if (tempC <= c->points[i].temperatureC) {
            if (i == 0) return c->points[i].fanPercent;
            // interpolate between previous enabled point and this one
            int pPrev = -1;
            for (int j = i - 1; j >= 0; j--) { if (c->points[j].enabled) { pPrev = j; break; } }
            if (pPrev < 0) return c->points[i].fanPercent;
            int t0 = c->points[pPrev].temperatureC, t1 = c->points[i].temperatureC;
            int p0 = c->points[pPrev].fanPercent, p1 = c->points[i].fanPercent;
            if (t1 <= t0) return p1;
            best = p0 + (p1 - p0) * (tempC - t0) / (t1 - t0);
            return best;
        }
    }
    return lastEnabled;  // above the top point: hold the last percent
}

static pl_thread_ret fan_reassert_thread(void*) {
    while (g_running) {
        pl_sleep_ms(5000);
        pl_mutex_lock(&g_lock);
        bool active = g_gpuReady && g_hasActiveDesired && g_activeDesired.hasFan &&
                      g_activeDesired.fanMode == FAN_MODE_CURVE;
        if (active && g_gpu.nvml.getTemperature && g_gpu.nvml.setFanSpeed) {
            unsigned int t = 0;
            if (g_gpu.nvml.getTemperature(g_gpu.nvmlDevice, NVML_TEMPERATURE_GPU, &t) == NVML_SUCCESS) {
                int pct = fan_curve_percent_for_temp(&g_activeDesired.fanCurve, (int)t);
                if (pct < 0) pct = 0; if (pct > 100) pct = 100;
                unsigned int fans = 0;
                if (g_gpu.nvml.getNumFans) g_gpu.nvml.getNumFans(g_gpu.nvmlDevice, &fans);
                if (fans == 0) fans = 1;
                for (unsigned int f = 0; f < fans; f++)
                    g_gpu.nvml.setFanSpeed(g_gpu.nvmlDevice, f, (unsigned int)pct);
            }
        }
        pl_mutex_unlock(&g_lock);
    }
    return PL_THREAD_RET_OK;
}

static void handle_request(const ServiceRequest* req, ServiceResponse* resp) {
    memset(resp, 0, sizeof(*resp));
    resp->magic = SERVICE_PROTOCOL_MAGIC;
    resp->version = SERVICE_PROTOCOL_VERSION;
    resp->serviceBuildNumber = APP_BUILD_NUMBER;
    gc_strlcpy(resp->serviceVersion, sizeof(resp->serviceVersion), APP_VERSION);

    if (req->magic != SERVICE_PROTOCOL_MAGIC || req->version != SERVICE_PROTOCOL_VERSION) {
        resp->status = SERVICE_STATUS_VERSION_MISMATCH;
        gc_strlcpy(resp->message, sizeof(resp->message), "protocol version mismatch");
        return;
    }

    pl_mutex_lock(&g_lock);
    switch (req->command) {
        case SERVICE_CMD_PING:
            resp->status = SERVICE_STATUS_OK;
            gc_strlcpy(resp->message, sizeof(resp->message), "pong");
            break;
        case SERVICE_CMD_GET_SNAPSHOT:
        case SERVICE_CMD_GET_TELEMETRY:
            if (g_gpuReady) linux_backend_refresh(&g_gpu);
            populate_snapshot(&resp->snapshot);
            if (g_hasActiveDesired) resp->desired = g_activeDesired;
            resp->status = SERVICE_STATUS_OK;
            break;
        case SERVICE_CMD_GET_ACTIVE_DESIRED:
            if (g_hasActiveDesired) resp->desired = g_activeDesired;
            resp->status = SERVICE_STATUS_OK;
            break;
        case SERVICE_CMD_APPLY: {
            if (!g_gpuReady) {
                resp->status = SERVICE_STATUS_ERROR;
                gc_strlcpy(resp->message, sizeof(resp->message), "GPU backend not initialized");
                break;
            }
            DesiredSettings d = req->desired;
            validate_desired_settings_for_ipc(&d);
            bool ok = linux_backend_apply(&g_gpu, &d, resp->message, sizeof(resp->message));
            g_activeDesired = d;
            g_hasActiveDesired = true;
            if (!persist_active_desired()) {
                ok = false;
                gc_strlcpy(resp->message, sizeof(resp->message), "active state persistence failed");
            }
            populate_snapshot(&resp->snapshot);
            resp->desired = g_activeDesired;
            resp->status = ok ? SERVICE_STATUS_OK : SERVICE_STATUS_ERROR;
            break;
        }
        case SERVICE_CMD_RESET: {
            if (!g_gpuReady) {
                resp->status = SERVICE_STATUS_ERROR;
                gc_strlcpy(resp->message, sizeof(resp->message), "GPU backend not initialized");
                break;
            }
            bool ok = linux_backend_reset(&g_gpu, resp->message, sizeof(resp->message));
            g_hasActiveDesired = false;
            if (!persist_active_desired()) {
                ok = false;
                gc_strlcpy(resp->message, sizeof(resp->message), "failed deleting stale active state");
            }
            populate_snapshot(&resp->snapshot);
            resp->status = ok ? SERVICE_STATUS_OK : SERVICE_STATUS_ERROR;
            break;
        }
        default:
            resp->status = SERVICE_STATUS_ERROR;
            gc_strlcpy(resp->message, sizeof(resp->message), "unknown command");
            break;
    }
    pl_mutex_unlock(&g_lock);
}

static void log_peer(int connFd) {
    struct ucred cred;
    socklen_t len = sizeof(cred);
    if (getsockopt(connFd, SOL_SOCKET, SO_PEERCRED, &cred, &len) == 0) {
        dlog("daemon: connection from pid=%d uid=%d gid=%d\n", (int)cred.pid, (int)cred.uid, (int)cred.gid);
    }
}

int linux_daemon_run(const char* configPath) {
    (void)configPath;
    pl_mutex_init(&g_lock);

    char err[256] = {};
    if (linux_backend_init(&g_gpu, 0, err, sizeof(err))) {
        g_gpuReady = true;
        dlog("daemon: GPU backend ready (%s, family=%d)\n", g_gpu.gpuName, (int)g_gpu.family);
    } else {
        dlog("daemon: GPU backend init failed: %s (serving telemetry-less)\n", err);
    }

    // Startup restart-reapply: re-apply the last active settings.
    DesiredSettings saved;
    if (g_gpuReady && load_active_desired(&saved)) {
        validate_desired_settings_for_ipc(&saved);
        char msg[256] = {};
        linux_backend_apply(&g_gpu, &saved, msg, sizeof(msg));
        g_activeDesired = saved;
        g_hasActiveDesired = true;
        dlog("daemon: startup reapply -> %s\n", msg);
    }

    pl_thread fanThread;
    bool fanThreadOk = pl_thread_start(&fanThread, fan_reassert_thread, nullptr);

    // Socket: root-owned, group-accessible (the systemd unit chowns to the
    // greencurve admin group; mode 0660).  Anyone who can connect is authorized;
    // every request is still clamped by validate_desired_settings_for_ipc.
    if (mkdir(GC_DAEMON_SOCKET_DIR, 0755) != 0 && errno != EEXIST) { /* best effort */ }
    unlink(GC_DAEMON_SOCKET_PATH);
    int srv = socket(AF_UNIX, SOCK_STREAM, 0);
    if (srv < 0) { dlog("daemon: socket() failed\n"); return 1; }
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    gc_strlcpy(addr.sun_path, sizeof(addr.sun_path), GC_DAEMON_SOCKET_PATH);
    if (bind(srv, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
        dlog("daemon: bind(%s) failed: %s\n", GC_DAEMON_SOCKET_PATH, strerror(errno));
        close(srv);
        return 1;
    }
    // Grant the greencurve admin group access (created by --service-install);
    // fall back to root-only if the group does not exist.
    struct group* gr = getgrnam("greencurve");
    if (gr && chown(GC_DAEMON_SOCKET_PATH, 0, gr->gr_gid) != 0)
        dlog("daemon: chown socket to greencurve group failed (non-fatal)\n");
    if (chmod(GC_DAEMON_SOCKET_PATH, 0660) != 0)
        dlog("daemon: chmod socket failed (non-fatal)\n");
    if (listen(srv, 8) != 0) { dlog("daemon: listen() failed\n"); close(srv); return 1; }
    dlog("daemon: listening on %s\n", GC_DAEMON_SOCKET_PATH);

    while (g_running) {
        int conn = accept(srv, nullptr, nullptr);
        if (conn < 0) { if (errno == EINTR) continue; break; }
        set_nonblocking(conn);
        log_peer(conn);
        ServiceRequest req;
        if (read_full(conn, &req, sizeof(req))) {
            ServiceResponse resp;
            handle_request(&req, &resp);
            write_full(conn, &resp, sizeof(resp));
        }
        close(conn);
    }

    g_running = 0;
    if (fanThreadOk) pl_thread_join(fanThread);
    close(srv);
    unlink(GC_DAEMON_SOCKET_PATH);
    if (g_gpuReady) linux_backend_shutdown(&g_gpu);
    return 0;
}

// ===========================================================================
// systemd install / remove  (requires root)
// ===========================================================================
#define GC_UNIT_PATH "/etc/systemd/system/greencurve.service"
#define GC_INSTALL_DIR "/usr/local/libexec/greencurve"
#define GC_INSTALL_BIN GC_INSTALL_DIR "/greencurve"

static bool root_owned_nonwritable_path(const char* path, bool wantDir, char* err, size_t errSize) {
    struct stat st;
    if (lstat(path, &st) != 0) {
        gc_snprintf(err, errSize, "cannot inspect %s: %s", path, strerror(errno));
        return false;
    }
    if (S_ISLNK(st.st_mode)) {
        gc_snprintf(err, errSize, "%s is a symlink", path);
        return false;
    }
    if (wantDir && !S_ISDIR(st.st_mode)) {
        gc_snprintf(err, errSize, "%s is not a directory", path);
        return false;
    }
    if (!wantDir && !S_ISREG(st.st_mode)) {
        gc_snprintf(err, errSize, "%s is not a regular file", path);
        return false;
    }
    if (st.st_uid != 0) {
        gc_snprintf(err, errSize, "%s is not root-owned", path);
        return false;
    }
    if ((st.st_mode & 0022) != 0) {
        gc_snprintf(err, errSize, "%s is writable by group/other", path);
        return false;
    }
    return true;
}

static bool ensure_root_owned_dir(const char* path, mode_t mode, char* err, size_t errSize) {
    if (mkdir(path, mode) != 0) {
        if (errno == EEXIST) {
            return root_owned_nonwritable_path(path, true, err, errSize);
        }
        gc_snprintf(err, errSize, "cannot create %s: %s", path, strerror(errno));
        return false;
    }
    if (chown(path, 0, 0) != 0) {
        gc_snprintf(err, errSize, "cannot chown %s: %s", path, strerror(errno));
        return false;
    }
    if (chmod(path, mode) != 0) {
        gc_snprintf(err, errSize, "cannot chmod %s: %s", path, strerror(errno));
        return false;
    }
    return root_owned_nonwritable_path(path, true, err, errSize);
}

static bool validate_install_parent_chain(char* err, size_t errSize) {
    if (!root_owned_nonwritable_path("/usr", true, err, errSize)) return false;
    if (!root_owned_nonwritable_path("/usr/local", true, err, errSize)) return false;
    if (!ensure_root_owned_dir("/usr/local/libexec", 0755, err, errSize)) return false;
    if (!ensure_root_owned_dir(GC_INSTALL_DIR, 0755, err, errSize)) return false;
    return true;
}

static bool write_all_file(int fd, const void* buf, size_t len) {
    const unsigned char* p = (const unsigned char*)buf;
    size_t done = 0;
    while (done < len) {
        ssize_t n = write(fd, p + done, len - done);
        if (n > 0) {
            done += (size_t)n;
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        return false;
    }
    return true;
}

static bool stage_service_binary(const char* sourceExe, char* err, size_t errSize) {
    if (!validate_install_parent_chain(err, errSize)) return false;

    char tempPath[4096] = {};
    gc_snprintf(tempPath, sizeof(tempPath), "%s.tmp.%ld", GC_INSTALL_BIN, (long)getpid());
    unlink(tempPath);

    int in = open(sourceExe, O_RDONLY | O_CLOEXEC);
    if (in < 0) {
        gc_snprintf(err, errSize, "cannot open source executable %s: %s", sourceExe, strerror(errno));
        return false;
    }
    int out = open(tempPath, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0755);
    if (out < 0) {
        gc_snprintf(err, errSize, "cannot create staged executable %s: %s", tempPath, strerror(errno));
        close(in);
        return false;
    }

    bool ok = true;
    unsigned char buf[65536];
    for (;;) {
        ssize_t n = read(in, buf, sizeof(buf));
        if (n > 0) {
            if (!write_all_file(out, buf, (size_t)n)) { ok = false; break; }
            continue;
        }
        if (n == 0) break;
        if (errno == EINTR) continue;
        ok = false;
        break;
    }
    if (fsync(out) != 0) ok = false;
    if (fchown(out, 0, 0) != 0) ok = false;
    if (fchmod(out, 0755) != 0) ok = false;
    if (close(out) != 0) ok = false;
    close(in);
    if (!ok) {
        gc_snprintf(err, errSize, "failed staging executable %s: %s", tempPath, strerror(errno));
        unlink(tempPath);
        return false;
    }
    if (rename(tempPath, GC_INSTALL_BIN) != 0) {
        gc_snprintf(err, errSize, "cannot install %s: %s", GC_INSTALL_BIN, strerror(errno));
        unlink(tempPath);
        return false;
    }
    if (!root_owned_nonwritable_path(GC_INSTALL_BIN, false, err, errSize)) return false;
    int dirfd = open(GC_INSTALL_DIR, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dirfd >= 0) {
        fsync(dirfd);
        close(dirfd);
    }
    return validate_install_parent_chain(err, errSize);
}

int linux_service_install(char* err, size_t errSize) {
    if (err && errSize) err[0] = 0;
    if (geteuid() != 0) {
        gc_strlcpy(err, errSize, "--service-install requires root (use sudo)");
        return 1;
    }
    char exe[4096] = {};
    ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (n <= 0) { gc_strlcpy(err, errSize, "cannot resolve /proc/self/exe"); return 1; }
    exe[n] = 0;
    if (!stage_service_binary(exe, err, errSize)) return 1;

    // Admin group for socket access (best-effort; ignore "already exists").
    if (system("groupadd -f greencurve >/dev/null 2>&1") != 0)
        dlog("service-install: groupadd greencurve failed (non-fatal)\n");

    FILE* f = fopen(GC_UNIT_PATH, "w");
    if (!f) { gc_snprintf(err, errSize, "cannot write %s: %s", GC_UNIT_PATH, strerror(errno)); return 1; }
    fprintf(f,
        "[Unit]\n"
        "Description=Green Curve NVIDIA GPU control daemon\n"
        "After=multi-user.target\n\n"
        "[Service]\n"
        "Type=simple\n"
        "ExecStart=%s --daemon\n"
        "Restart=on-failure\n"
        "RestartSec=2\n"
        "StateDirectory=greencurve\n"
        "RuntimeDirectory=greencurve\n\n"
        "[Install]\n"
        "WantedBy=multi-user.target\n",
        GC_INSTALL_BIN);
    fclose(f);

    if (system("systemctl daemon-reload") != 0)
        dlog("service-install: systemctl daemon-reload failed (non-fatal)\n");
    if (system("systemctl enable --now greencurve.service") != 0) {
        gc_strlcpy(err, errSize,
                   "systemctl enable --now greencurve.service failed "
                   "(check: journalctl -u greencurve)");
        return 1;
    }
    return 0;
}

int linux_service_remove(char* err, size_t errSize) {
    if (err && errSize) err[0] = 0;
    if (geteuid() != 0) {
        gc_strlcpy(err, errSize, "--service-remove requires root (use sudo)");
        return 1;
    }
    if (system("systemctl disable --now greencurve.service >/dev/null 2>&1") != 0)
        dlog("service-remove: disable failed (non-fatal)\n");
    unlink(GC_UNIT_PATH);
    if (system("systemctl daemon-reload") != 0)
        dlog("service-remove: daemon-reload failed (non-fatal)\n");
    return 0;
}
