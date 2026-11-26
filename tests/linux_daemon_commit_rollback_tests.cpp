// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
// Request-level fixture (assertion codes 14260-14270): the production Linux
// daemon request handler (linux_daemon.cpp, compiled whole) over fake backend,
// persistence and log functions. When the durable commit fails after a
// successful Apply, Reset or automatic resume restore, the daemon must roll
// the hardware back with the very ceiling transaction that mutation used, so
// the rollback is bounded by the clamp and outgoing facts it recorded. No
// driver, socket or GPU is used.
#if !defined(_WIN32)
// Rename the two symbols another fixture (linux_tui_gpu_target_tests.cpp)
// also defines, so both can link into the one regression binary.
#define linux_daemon_get_state_ex commit_fixture_daemon_get_state_ex
#define linux_debug_logf commit_fixture_debug_logf
#include "linux_daemon.cpp"
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
                                        const DesiredSettings*, char* result, size_t resultSize) {
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
    // It is a payload-free command (no caller-supplied target, settings, or
    // preconditions). When its ACTIVE record commit fails after the hardware
    // write, the unattended path rolls back once with that same ceiling
    // transaction and locks out further automatic restores.
    fixture = DaemonFixture{};
    fixture.failActiveStore = true;
    req = ServiceRequest{};
    req.magic = SERVICE_PROTOCOL_MAGIC;
    req.version = SERVICE_PROTOCOL_VERSION;
    req.command = SERVICE_CMD_RESUME_RESTORE;
    req.flags = service_request_flags_for_command(SERVICE_CMD_RESUME_RESTORE, false);
    req.callerPid = (gc_u32)getpid();
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
    return 0;
}
#else
// The Linux daemon is not built for Windows.
int run_linux_daemon_commit_rollback_tests() { return 0; }
#endif
