// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
// Request-level fixture (assertion codes 14260-14270, 14400-14415): the
// production Linux daemon request handler (linux_daemon.cpp, compiled whole)
// over fake backend, persistence and log functions. Covers commit/rollback
// ceiling transaction fidelity (14260-14270) and SERVICE_CMD_RESUME_RESTORE
// standby fan stash replay, uncertainty waiver, explicit Apply/Reset window
// closing and stall exclusion (14400-14415). No driver, socket or GPU is used.
#if !defined(_WIN32)
#include <ctime>
#include <new>
#include <time.h>
#include <type_traits>
// Rename the two symbols another fixture (linux_tui_gpu_target_tests.cpp)
// also defines, so both can link into the one regression binary.
#define linux_daemon_get_state_ex commit_fixture_daemon_get_state_ex
#define linux_debug_logf commit_fixture_debug_logf
static unsigned long long commit_fixture_monotonic_ns = 5000000000ull;
static unsigned long long commit_fixture_suspended_ns = 0;
static int commit_fixture_clock_gettime(clockid_t clockId, struct timespec* ts) {
    if (!ts) return -1;
    unsigned long long ns = commit_fixture_monotonic_ns;
    if (clockId == CLOCK_BOOTTIME) ns += commit_fixture_suspended_ns;
    ts->tv_sec = (time_t)(ns / 1000000000ull);
    ts->tv_nsec = (long)(ns % 1000000000ull);
    commit_fixture_monotonic_ns += 1000ull;
    return 0;
}
#define clock_gettime commit_fixture_clock_gettime
#include "linux_daemon.cpp"
#undef clock_gettime
#include <cstdio>
#include <cstring>

namespace {
struct DaemonFixture {
    // The transaction each backend mutation received, and what it recorded.
    const LinuxApplyCeilingTransaction* mutated;
    const LinuxApplyCeilingTransaction* restored;
    // Every field linux_backend_restore_snapshot() reads, as it received them.
    LinuxApplyCeilingTransaction restoredValues;
    int restoreCalls;
    int applyCalls;
    DesiredSettings lastApplied;
    // Persistence: which commit fails.
    bool failActiveStore;
    bool failStateRemove;
    int stateStores;
};
static DaemonFixture fixture;
// Distinct values for every transaction field the rollback reads, so a
// rollback that receives another (or a cleared) transaction is visible.
static void fixture_record(LinuxApplyCeilingTransaction& t, unsigned int planMHz,
                           unsigned int outgoingMHz, bool hardPin) {
    t.armed = true;
    t.armResult = APPLY_CEILING_ARM_INSTALLED;
    t.plan.ceilingMHz = planMHz;
    t.outgoingCeilingMHz = outgoingMHz;
    t.outgoingHadHardPin = hardPin;
}
static bool fixture_restored_matches(unsigned int planMHz, unsigned int outgoingMHz, bool hardPin) {
    const LinuxApplyCeilingTransaction& r = fixture.restoredValues;
    return r.armed && r.armResult == APPLY_CEILING_ARM_INSTALLED &&
           r.plan.ceilingMHz == planMHz && r.outgoingCeilingMHz == outgoingMHz &&
           r.outgoingHadHardPin == hardPin;
}
}

// --- backend -------------------------------------------------------------
LinuxMutationResult linux_backend_apply(LinuxGpuState*, LinuxApplyCeilingTransaction& ceiling,
                                        const DesiredSettings*, const DesiredSettings*,
                                        const DesiredSettings* committed, char* result, size_t resultSize) {
    ++fixture.applyCalls;
    fixture.lastApplied = committed ? *committed : DesiredSettings{};
    fixture.mutated = &ceiling;
    fixture_record(ceiling, 1777, 1888, true);
    gc_strlcpy(result, resultSize, "fixture apply");
    LinuxMutationResult r = {};
    r.success = r.anyWrite = true;
    r.attemptedPhases = r.completedPhases = 1u;
    return r;
}
LinuxMutationResult linux_backend_reset(LinuxGpuState*, LinuxApplyCeilingTransaction& ceiling,
                                        const DesiredSettings*, char* result, size_t resultSize) {
    fixture.mutated = &ceiling;
    // Production Reset bounds its rollback by outgoingCeilingMHz; the state it
    // leaves was a HARD pin, which the rollback must still know.
    fixture_record(ceiling, 1666, 1555, true);
    gc_strlcpy(result, resultSize, "fixture reset");
    LinuxMutationResult r = {};
    r.success = r.anyWrite = true;
    r.attemptedPhases = r.completedPhases = 1u;
    return r;
}
bool linux_backend_restore_snapshot(LinuxGpuState*, LinuxApplyCeilingTransaction& ceiling,
                                    const LinuxHardwareSnapshot*, unsigned int, char*, size_t) {
    ++fixture.restoreCalls;
    fixture.restored = &ceiling;
    fixture.restoredValues = ceiling;
    return true;
}
bool linux_backend_capture_apply_snapshot(LinuxGpuState*, const DesiredSettings*,
    LinuxHardwareSnapshot* snapshot, char*, size_t, const DesiredSettings*, const DesiredSettings*) {
    *snapshot = LinuxHardwareSnapshot{};
    return true;
}
bool linux_backend_capture_reset_snapshot(LinuxGpuState*, const DesiredSettings*,
    LinuxHardwareSnapshot* snapshot, char*, size_t) {
    *snapshot = LinuxHardwareSnapshot{};
    return true;
}
bool linux_backend_capture_snapshot(LinuxGpuState*, LinuxHardwareSnapshot* snapshot, char*, size_t) {
    *snapshot = LinuxHardwareSnapshot{};
    return true;
}
bool linux_backend_capture_current(const LinuxGpuState*, gc_u64, gc_u32, const DesiredSettings*,
    HardwareCaptureObservation*, char* why, size_t size) {
    gc_strlcpy(why, size, "fixture");
    return false;
}
bool linux_backend_init(LinuxGpuState*, LinuxDriverContext*, const GpuAdapterInfo*, char*, size_t,
                        GpuLocationRole, const LinuxGpuUuid*) { return false; }
void linux_backend_shutdown(LinuxGpuState*) {}
bool linux_backend_select_target(LinuxGpuState* g, const GpuAdapterInfo*, char*, size_t,
                                 GpuLocationRole, const LinuxGpuUuid*, LinuxGpuUuidVerdict* verdict) {
    if (verdict) *verdict = LINUX_GPU_UUID_FALLBACK;
    return g->writeIdentityResolved;
}
LinuxBackendRefreshResult linux_backend_refresh(LinuxGpuState* g) {
    LinuxBackendRefreshResult r = {};
    r.nvmlReady = g->nvmlReady;
    r.health = g->health;
    return r;
}
bool linux_backend_set_curve_fan_percent(LinuxGpuState*, unsigned int) { return true; }
bool linux_backend_set_fan_auto(LinuxGpuState*) { return true; }
bool linux_backend_fans_are_auto(LinuxGpuState*) { return true; }
FanFixedMaintenanceDecision linux_backend_fixed_fan_check(LinuxGpuState*, int) {
    return {false, false, "fixture"};
}
bool linux_backend_fan_stall_sample(LinuxGpuState*, FanStallFanSample*, unsigned int* count) {
    *count = 0;
    return true;
}
void linux_driver_close(LinuxDriverContext*) {}

// --- persistence -----------------------------------------------------------
int linux_daemon_lock_instance(const char*, char*, size_t) { return -1; }
LinuxDaemonStateLoadResult linux_daemon_state_load(const char*, LinuxDaemonStateRecord*,
                                                   char*, size_t, bool*) {
    return LinuxDaemonStateLoadResult{};
}
bool linux_daemon_state_store(const char*, const LinuxDaemonStateRecord* record,
                              char* err, size_t errSize) {
    ++fixture.stateStores;
    if (fixture.failActiveStore && record->state == LINUX_DAEMON_RECORD_ACTIVE) {
        gc_strlcpy(err, errSize, "fixture: active record not stored");
        return false;
    }
    return true;
}
bool linux_daemon_state_remove(const char*, char* err, size_t errSize) {
    if (fixture.failStateRemove) {
        gc_strlcpy(err, errSize, "fixture: state not removed");
        return false;
    }
    return true;
}
bool linux_daemon_operation_store(const char*, const LinuxDaemonOperationRecord*, char*, size_t) { return true; }
bool linux_daemon_operation_load(const char*, LinuxDaemonOperationRecord*, char*, size_t) { return false; }
bool linux_daemon_startup_load(const char*, LinuxDaemonStartupRecord*, bool*, char*, size_t, bool*) { return false; }
bool linux_daemon_startup_store(const char*, const LinuxDaemonStartupRecord*, char*, size_t) { return true; }
bool linux_daemon_guard_load(const char*, LinuxAutoRestoreGuard*, bool*, char*, size_t) { return false; }
bool linux_daemon_guard_store(const char*, const LinuxAutoRestoreGuard*, char*, size_t) { return true; }
bool linux_read_boot_id(char* out, size_t size) { gc_strlcpy(out, size, "fixture-boot"); return true; }
bool linux_fan_ownership_marker_store(const char*, const LinuxFanOwnershipMarker*, char*, size_t) { return true; }
int linux_fan_ownership_marker_load(const char*, LinuxFanOwnershipMarker*, char*, size_t) { return 0; }

// --- debug log -------------------------------------------------------------
bool linux_debug_log_is_enabled() { return false; }
const char* linux_debug_log_path() { return ""; }
int linux_debug_log_raw_fd() { return -1; }
void linux_debug_logf(const char*, ...) {}

namespace {
// One bound GPU with write authority, as after a successful daemon start.
static void fixture_controller(LinuxGpuController& ctl) {
    linux_gpu_controller_init(&ctl);
    LinuxGpuState& gpu = ctl.gpu;
    gpu.adapterCount = 1;
    GpuAdapterInfo& adapter = gpu.adapters[0];
    adapter.valid = adapter.pciInfoValid = true;
    adapter.deviceId = 0x1e0410deu;
    adapter.pciBus = 1;
    gpu.selectedGpu = adapter;
    gpu.selectedAdapterIndex = 0;
    gpu.writeIdentityResolved = true;
    gpu.nvmlReady = true;
    // A fresh VF read of a small curve, as after a successful binding.
    gpu.vfSnapshotFresh = true;
    gpu.numPopulated = 4;
    for (int i = 0; i < 4; ++i) {
        gpu.curve[i].freq_kHz = 1200000u + 15000u * (unsigned int)i;
        gpu.curve[i].volt_uV = 700000u + 25000u * (unsigned int)i;
    }
    // A healthy binding publishes every control surface it can mutate.
    gpu.health.reason = SERVICE_GPU_HEALTH_NONE;
    gpu.health.availableMutationDomains = 0x1FFu;
    ctl.gpuReady = true;
}
static void fixture_request(ServiceRequest& req, const LinuxGpuController& ctl,
                            gc_u32 command, gc_u64 operationId,
                            const ServiceStateEnvelope& observed) {
    // Built like the CLI client's send_simple() (linux_daemon_client.h).
    req = ServiceRequest{};
    req.magic = SERVICE_PROTOCOL_MAGIC;
    req.version = SERVICE_PROTOCOL_VERSION;
    req.command = command;
    req.flags = service_request_flags_for_command(command, true);
    req.callerPid = (gc_u32)getpid();
    if (command == SERVICE_CMD_APPLY) req.applyOrigin = SERVICE_APPLY_ORIGIN_CLI;
    req.operationId = operationId;
    req.targetGpu = ctl.gpu.selectedGpu;
    req.desired.hasGpuOffset = 1;
    req.desired.gpuOffsetMHz = 50;
    // The freshness preconditions a client copies from its last state read.
    req.expectedServiceInstanceId = observed.serviceInstanceId;
    req.expectedGpuGeneration = observed.gpuGeneration;
    req.expectedTopologySignature = observed.topologySignature;
}
// Reads state the way a client does before it mutates.
static ServiceStateEnvelope fixture_observe(LinuxGpuController& ctl) {
    static ServiceRequest read;
    static ServiceResponse state;
    read = ServiceRequest{};
    read.magic = SERVICE_PROTOCOL_MAGIC;
    read.version = SERVICE_PROTOCOL_VERSION;
    read.command = SERVICE_CMD_GET_SNAPSHOT;
    read.flags = service_request_flags_for_command(SERVICE_CMD_GET_SNAPSHOT, true);
    read.callerPid = (gc_u32)getpid();
    read.targetGpu = ctl.gpu.selectedGpu;
    handle_request(ctl, &read, &state);
    return state.state;
}
static ServiceRequest fixture_curve_apply_req(const LinuxGpuController& ctl,
                                              gc_u64 operationId,
                                              const ServiceStateEnvelope& observed) {
    ServiceRequest r = {};
    fixture_request(r, ctl, SERVICE_CMD_APPLY, operationId, observed);
    r.desired.hasFan = 1;
    r.desired.fanAuto = 0;
    r.desired.fanMode = FAN_MODE_CURVE;
    fan_curve_set_default(&r.desired.fanCurve);
    return r;
}
static ServiceRequest fixture_resume_req() {
    ServiceRequest r = {};
    r.magic = SERVICE_PROTOCOL_MAGIC;
    r.version = SERVICE_PROTOCOL_VERSION;
    r.command = SERVICE_CMD_RESUME_RESTORE;
    r.flags = service_request_flags_for_command(SERVICE_CMD_RESUME_RESTORE, false);
    r.callerPid = (gc_u32)getpid();
    return r;
}
}

int run_linux_daemon_commit_rollback_tests() {
    // daemon_run_owned() initializes the request lock; this fixture is the caller.
    static bool lockReady = false;
    if (!lockReady) { pl_mutex_init(&g_lock); lockReady = true; }
    static LinuxGpuController ctl;
    static ServiceRequest req;
    static ServiceResponse resp;

    // 14260-14263: Apply succeeds on the hardware but its active record cannot
    // be stored. The daemon rolls back once, with the same transaction the
    // apply recorded its clamp in, and reports the persistence failure.
    fixture = DaemonFixture{};
    fixture.failActiveStore = true;
    fixture_controller(ctl);
    fixture_request(req, ctl, SERVICE_CMD_APPLY, 1, fixture_observe(ctl));
    resp = ServiceResponse{};
    handle_request(ctl, &req, &resp);
    if (!fixture.mutated) {
        std::fprintf(stderr, "daemon fixture apply: %s\n", resp.message);
        return 14260;
    }
    if (fixture.restoreCalls != 1) return 14261;
    if (fixture.restored != fixture.mutated || !fixture_restored_matches(1777, 1888, true))
        return 14262;
    if (!std::strstr(resp.message, "Apply persistence failed; hardware rollback succeeded") ||
        ctl.hasActiveDesired) return 14263;

    // 14264-14266: Reset succeeds on the hardware but the state file cannot be
    // removed: the same rule, with the Reset's own transaction.
    fixture = DaemonFixture{};
    fixture.failStateRemove = true;
    fixture_controller(ctl);
    fixture_request(req, ctl, SERVICE_CMD_RESET, 2, fixture_observe(ctl));
    resp = ServiceResponse{};
    handle_request(ctl, &req, &resp);
    if (!fixture.mutated) {
        std::fprintf(stderr, "daemon fixture reset: %s\n", resp.message);
        return 14264;
    }
    if (fixture.restoreCalls != 1 || fixture.restored != fixture.mutated ||
        !fixture_restored_matches(1666, 1555, true)) return 14265;
    if (!std::strstr(resp.message, "Reset persistence failed; hardware rollback succeeded"))
        return 14266;

    // 14267: a commit that succeeds never rolls back.
    fixture = DaemonFixture{};
    fixture_controller(ctl);
    fixture_request(req, ctl, SERVICE_CMD_APPLY, 3, fixture_observe(ctl));
    resp = ServiceResponse{};
    handle_request(ctl, &req, &resp);
    if (!fixture.mutated || fixture.restoreCalls != 0 || resp.status != SERVICE_STATUS_OK) {
        std::fprintf(stderr, "daemon fixture committed apply: status=%u %s\n",
                     (unsigned int)resp.status, resp.message);
        return 14267;
    }

    // 14268-14270: SERVICE_CMD_RESUME_RESTORE drives daemon_automatic_restore_write().
    // Self-contained setup: start from a fresh controller and commit one Apply
    // so activeDesired is populated independently of earlier cases. Resume is
    // a payload-free command (no caller-supplied target, settings, or
    // preconditions). When its ACTIVE record commit fails after the hardware
    // write, the unattended path rolls back once with that same ceiling
    // transaction and locks out further automatic restores.
    fixture = DaemonFixture{};
    fixture_controller(ctl);
    fixture_request(req, ctl, SERVICE_CMD_APPLY, 4, fixture_observe(ctl));
    resp = ServiceResponse{};
    handle_request(ctl, &req, &resp);
    if (resp.status != SERVICE_STATUS_OK || !ctl.hasActiveDesired) return 14268;
    fixture = DaemonFixture{};
    fixture.failActiveStore = true;
    req = fixture_resume_req();
    resp = ServiceResponse{};
    handle_request(ctl, &req, &resp);
    if (!fixture.mutated) {
        std::fprintf(stderr, "daemon fixture resume: %s\n", resp.message);
        return 14268;
    }
    if (fixture.restoreCalls != 1 || fixture.restored != fixture.mutated ||
        !fixture_restored_matches(1777, 1888, true)) return 14269;
    if (resp.status != SERVICE_STATUS_ERROR ||
        !std::strstr(resp.message, "automatic restore persistence failed after hardware write; hardware rollback succeeded") ||
        !ctl.stateUncertain || !ctl.autoRestoreGuard.lockedOut) return 14270;

    // 14400-14402: Standby fan stash replay through SERVICE_CMD_RESUME_RESTORE
    // succeeds, replays the replaced fan curve, and clears stateUncertain,
    // ctl.fanFault, and ctl.standbyFanStash.
    commit_fixture_monotonic_ns = 5000000000ull;
    commit_fixture_suspended_ns = 0;
    fixture = DaemonFixture{};
    fixture_controller(ctl);
    daemon_standby_window_close(ctl, "daemon start");
    req = fixture_curve_apply_req(ctl, 5, fixture_observe(ctl));
    resp = ServiceResponse{};
    handle_request(ctl, &req, &resp);
    if (resp.status != SERVICE_STATUS_OK || !ctl.hasActiveDesired ||
        ctl.activeDesired.fanMode != FAN_MODE_CURVE) return 14400;
    commit_fixture_suspended_ns += 30ull * 1000000000ull;
    pl_mutex_lock(&g_lock);
    daemon_fan_runtime_lockout(ctl, SERVICE_FAN_FAULT_REPEATED_FAILURES,
        "wake nvml failure", 0, 0, 0);
    pl_mutex_unlock(&g_lock);
    if (!ctl.stateUncertain || ctl.activeDesired.hasFan ||
        ctl.fanFault.kind != SERVICE_FAN_FAULT_REPEATED_FAILURES ||
        !ctl.standbyFanStash.valid ||
        ctl.standbyFanStash.replaced.fanMode != FAN_MODE_CURVE) return 14401;
    fixture = DaemonFixture{};
    req = fixture_resume_req();
    resp = ServiceResponse{};
    handle_request(ctl, &req, &resp);
    if (resp.status != SERVICE_STATUS_OK || fixture.applyCalls != 1 ||
        !fixture.lastApplied.hasFan || fixture.lastApplied.fanMode != FAN_MODE_CURVE ||
        ctl.stateUncertain || ctl.fanFault.kind != SERVICE_FAN_FAULT_NONE ||
        ctl.standbyFanStash.valid || !ctl.hasActiveDesired ||
        !ctl.activeDesired.hasFan || ctl.activeDesired.fanMode != FAN_MODE_CURVE)
        return 14402;

    // 14403-14404: Pre-write backend prepare failure preserves the stash so
    // the next SERVICE_CMD_RESUME_RESTORE replays and clears it once ready.
    fixture = DaemonFixture{};
    fixture_controller(ctl);
    daemon_standby_window_close(ctl, "daemon start");
    req = fixture_curve_apply_req(ctl, 6, fixture_observe(ctl));
    resp = ServiceResponse{};
    handle_request(ctl, &req, &resp);
    if (resp.status != SERVICE_STATUS_OK) return 14403;
    commit_fixture_suspended_ns += 30ull * 1000000000ull;
    pl_mutex_lock(&g_lock);
    daemon_fan_runtime_lockout(ctl, SERVICE_FAN_FAULT_REPEATED_FAILURES,
        "wake nvml failure", 0, 0, 0);
    pl_mutex_unlock(&g_lock);
    fixture = DaemonFixture{};
    ctl.gpu.writeIdentityResolved = false;
    req = fixture_resume_req();
    resp = ServiceResponse{};
    handle_request(ctl, &req, &resp);
    if (resp.status != SERVICE_STATUS_ERROR || fixture.applyCalls != 0 ||
        !ctl.standbyFanStash.valid || !ctl.stateUncertain) return 14403;
    ctl.gpu.writeIdentityResolved = true;
    req = fixture_resume_req();
    resp = ServiceResponse{};
    handle_request(ctl, &req, &resp);
    if (resp.status != SERVICE_STATUS_OK || fixture.applyCalls != 1 ||
        ctl.standbyFanStash.valid || ctl.stateUncertain ||
        ctl.fanFault.kind != SERVICE_FAN_FAULT_NONE) return 14404;

    // 14405-14406: A bumped uncertainSerial drops the stash and refuses the
    // resume restore under the uncertain gate.
    fixture = DaemonFixture{};
    fixture_controller(ctl);
    daemon_standby_window_close(ctl, "daemon start");
    req = fixture_curve_apply_req(ctl, 7, fixture_observe(ctl));
    resp = ServiceResponse{};
    handle_request(ctl, &req, &resp);
    if (resp.status != SERVICE_STATUS_OK) return 14405;
    commit_fixture_suspended_ns += 30ull * 1000000000ull;
    pl_mutex_lock(&g_lock);
    daemon_fan_runtime_lockout(ctl, SERVICE_FAN_FAULT_REPEATED_FAILURES,
        "wake nvml failure", 0, 0, 0);
    pl_mutex_unlock(&g_lock);
    ++ctl.uncertainSerial;
    fixture = DaemonFixture{};
    req = fixture_resume_req();
    resp = ServiceResponse{};
    handle_request(ctl, &req, &resp);
    if (resp.status != SERVICE_STATUS_ERROR || fixture.applyCalls != 0 ||
        !ctl.stateUncertain || ctl.standbyFanStash.valid) return 14406;

    // 14407-14409: Explicit Apply drops any pending stash AND closes the
    // suspend window even when no SERVICE_CMD_RESUME_RESTORE ran after wake.
    fixture = DaemonFixture{};
    fixture_controller(ctl);
    daemon_standby_window_close(ctl, "daemon start");
    req = fixture_curve_apply_req(ctl, 8, fixture_observe(ctl));
    resp = ServiceResponse{};
    handle_request(ctl, &req, &resp);
    if (resp.status != SERVICE_STATUS_OK) return 14407;
    commit_fixture_suspended_ns += 30ull * 1000000000ull;
    pl_mutex_lock(&g_lock);
    daemon_fan_runtime_lockout(ctl, SERVICE_FAN_FAULT_REPEATED_FAILURES,
        "wake nvml failure", 0, 0, 0);
    pl_mutex_unlock(&g_lock);
    if (!ctl.standbyFanStash.valid) return 14407;
    req = fixture_curve_apply_req(ctl, 9, fixture_observe(ctl));
    resp = ServiceResponse{};
    handle_request(ctl, &req, &resp);
    if (resp.status != SERVICE_STATUS_OK || ctl.standbyFanStash.valid ||
        ctl.stateUncertain) return 14408;
    // Suspend without a resume command, then explicitly Apply while awake:
    // a later awake lockout must NOT capture a stash.
    commit_fixture_suspended_ns += 30ull * 1000000000ull;
    req = fixture_curve_apply_req(ctl, 10, fixture_observe(ctl));
    resp = ServiceResponse{};
    handle_request(ctl, &req, &resp);
    if (resp.status != SERVICE_STATUS_OK) return 14408;
    pl_mutex_lock(&g_lock);
    daemon_fan_runtime_lockout(ctl, SERVICE_FAN_FAULT_REPEATED_FAILURES,
        "awake nvml failure", 0, 0, 0);
    pl_mutex_unlock(&g_lock);
    fixture = DaemonFixture{};
    req = fixture_resume_req();
    resp = ServiceResponse{};
    handle_request(ctl, &req, &resp);
    if (ctl.standbyFanStash.valid || resp.status != SERVICE_STATUS_ERROR ||
        fixture.applyCalls != 0) return 14409;

    // 14410-14412: Explicit Reset drops any pending stash AND closes the
    // suspend window.
    fixture = DaemonFixture{};
    fixture_controller(ctl);
    daemon_standby_window_close(ctl, "daemon start");
    req = fixture_curve_apply_req(ctl, 11, fixture_observe(ctl));
    resp = ServiceResponse{};
    handle_request(ctl, &req, &resp);
    if (resp.status != SERVICE_STATUS_OK) return 14410;
    commit_fixture_suspended_ns += 30ull * 1000000000ull;
    pl_mutex_lock(&g_lock);
    daemon_fan_runtime_lockout(ctl, SERVICE_FAN_FAULT_REPEATED_FAILURES,
        "wake nvml failure", 0, 0, 0);
    pl_mutex_unlock(&g_lock);
    fixture_request(req, ctl, SERVICE_CMD_RESET, 12, fixture_observe(ctl));
    resp = ServiceResponse{};
    handle_request(ctl, &req, &resp);
    if (resp.status != SERVICE_STATUS_OK || ctl.standbyFanStash.valid ||
        ctl.stateUncertain) return 14410;
    // Suspend without resume, Reset, re-Apply curve, then awake lockout:
    // window was closed by Reset, so awake lockout must not capture a stash.
    commit_fixture_suspended_ns += 30ull * 1000000000ull;
    fixture_request(req, ctl, SERVICE_CMD_RESET, 13, fixture_observe(ctl));
    resp = ServiceResponse{};
    handle_request(ctl, &req, &resp);
    if (resp.status != SERVICE_STATUS_OK) return 14411;
    req = fixture_curve_apply_req(ctl, 14, fixture_observe(ctl));
    resp = ServiceResponse{};
    handle_request(ctl, &req, &resp);
    if (resp.status != SERVICE_STATUS_OK) return 14411;
    pl_mutex_lock(&g_lock);
    daemon_fan_runtime_lockout(ctl, SERVICE_FAN_FAULT_REPEATED_FAILURES,
        "awake nvml failure", 0, 0, 0);
    pl_mutex_unlock(&g_lock);
    fixture = DaemonFixture{};
    req = fixture_resume_req();
    resp = ServiceResponse{};
    handle_request(ctl, &req, &resp);
    if (ctl.standbyFanStash.valid || resp.status != SERVICE_STATUS_ERROR ||
        fixture.applyCalls != 0) return 14412;

    // 14413-14415: A confirmed fan stall inside the suspend window captures no
    // stash and SERVICE_CMD_RESUME_RESTORE refuses to replay onto it.
    fixture = DaemonFixture{};
    fixture_controller(ctl);
    daemon_standby_window_close(ctl, "daemon start");
    req = fixture_curve_apply_req(ctl, 15, fixture_observe(ctl));
    resp = ServiceResponse{};
    handle_request(ctl, &req, &resp);
    if (resp.status != SERVICE_STATUS_OK) return 14413;
    commit_fixture_suspended_ns += 30ull * 1000000000ull;
    pl_mutex_lock(&g_lock);
    daemon_fan_runtime_lockout(ctl, SERVICE_FAN_FAULT_STALL,
        "Fan 1 did not turn at 40% for 20 s", 0, 40, 20000);
    pl_mutex_unlock(&g_lock);
    if (!ctl.stateUncertain || ctl.standbyFanStash.valid ||
        ctl.fanFault.kind != SERVICE_FAN_FAULT_STALL) return 14414;
    fixture = DaemonFixture{};
    req = fixture_resume_req();
    resp = ServiceResponse{};
    handle_request(ctl, &req, &resp);
    if (resp.status != SERVICE_STATUS_ERROR || fixture.applyCalls != 0 ||
        !ctl.stateUncertain || ctl.standbyFanStash.valid) return 14415;
    return 0;
}
#else
// The Linux daemon is not built for Windows.
int run_linux_daemon_commit_rollback_tests() { return 0; }
#endif
