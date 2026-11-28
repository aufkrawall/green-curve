// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
// Request-level fixture (assertion codes 14260-14270, 14400-14415): the
// production Linux daemon request handler (linux_daemon.cpp, compiled whole)
// over fake backend, persistence and log functions. Covers commit/rollback
// ceiling transaction fidelity (14260-14270) and SERVICE_CMD_RESUME_RESTORE
// standby fan stash replay, uncertainty waiver, explicit Apply/Reset window
// closing and stall exclusion (14400-14415). No driver, socket or GPU is used.
// Codes 14630-14636 and 14639-14640 drive the same handler
// with routed requests (service_gpu_route.h): only the full state read may
// move the daemon, and any other command naming a GPU it is not serving is
// refused before its handler runs. Codes 14730-14733 check the adapter
// summaries the stamp publishes, 14930-14939, 14942-14946, 14948, 14950-14954, 14959, 14974 and 14977-14987 the saved-journal
// interlock (including automatic restores).
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
    int startupStores;
    int selectCalls;
    int captureCalls;
    bool failApply;  // the hardware write fails without a rollback
    bool loseUuidAfterWrite;  // the write's own refresh loses the UUID and write identity
    bool selectLosesUuid;     // the selection's rediscovery cannot read the selected card's UUID
    bool failRestore;         // the hardware rollback fails
    // The journal on disk (14930-14939, 14950-14954) and the last record stored over it.
    bool journalPresent;
    LinuxDaemonStateRecord journal;
    LinuxDaemonStateRecord lastStored;
    int activeStoreFailures;  // fail only this many ACTIVE stores (0: failActiveStore rules)
    bool journalUnreadable;   // the loader refuses and removes the journal
    bool removeUnlinksThenFails;  // unlink happens, the directory sync fails
    bool failAllStores;           // every journal store fails
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
LinuxMutationResult linux_backend_apply(LinuxGpuState* g, LinuxApplyCeilingTransaction& ceiling,
                                        const DesiredSettings*, const DesiredSettings*,
                                        const DesiredSettings* committed, char* result, size_t resultSize) {
    ++fixture.applyCalls;
    fixture.lastApplied = committed ? *committed : DesiredSettings{};
    fixture.mutated = &ceiling;
    if (fixture.loseUuidAfterWrite) {  // the post-write refresh's recovery could not re-read it
        g->adapterUuids[g->selectedAdapterIndex] = LinuxGpuUuid{};
        g->writeIdentityResolved = false;
    }
    fixture_record(ceiling, 1777, 1888, true);
    gc_strlcpy(result, resultSize, "fixture apply");
    LinuxMutationResult r = {};
    r.anyWrite = true;
    r.success = !fixture.failApply;
    r.attemptedPhases = r.completedPhases = 1u;
    return r;
}
LinuxMutationResult linux_backend_reset(LinuxGpuState* g, LinuxApplyCeilingTransaction& ceiling,
                                        const DesiredSettings*, char* result, size_t resultSize) {
    fixture.mutated = &ceiling;
    if (fixture.loseUuidAfterWrite) g->writeIdentityResolved = false;  // the post-write refresh revoked it
    // Production Reset bounds its rollback by outgoingCeilingMHz; the state it
    // leaves was a HARD pin, which the rollback must still know.
    fixture_record(ceiling, 1666, 1555, true);
    gc_strlcpy(result, resultSize, "fixture reset");
    LinuxMutationResult r = {};
    r.success = r.anyWrite = true;
    r.attemptedPhases = r.completedPhases = 1u;
    return r;
}
bool linux_backend_restore_snapshot(LinuxGpuState* g, LinuxApplyCeilingTransaction& ceiling,
                                    const LinuxHardwareSnapshot*, unsigned int, char*, size_t) {
    ++fixture.restoreCalls;
    fixture.restored = &ceiling;
    fixture.restoredValues = ceiling;
    return !fixture.failRestore && g->writeIdentityResolved;  // the real rollback refuses a revoked binding
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
    ++fixture.captureCalls;
    gc_strlcpy(why, size, "fixture");
    return false;
}
bool linux_backend_init(LinuxGpuState*, LinuxDriverContext*, const GpuAdapterInfo*, char*, size_t,
                        GpuLocationRole, const LinuxGpuUuid*) { return false; }
void linux_backend_shutdown(LinuxGpuState*) {}
bool linux_backend_select_target(LinuxGpuState* g, const GpuAdapterInfo* target, char*, size_t,
                                 GpuLocationRole, const LinuxGpuUuid*, LinuxGpuUuidVerdict* verdict) {
    if (verdict) *verdict = LINUX_GPU_UUID_FALLBACK;
    ++fixture.selectCalls;
    // Binds the adapter it was asked for, as the real backend does.
    for (unsigned int i = 0; target && g->writeIdentityResolved && i < g->adapterCount; ++i)
        if (linux_gpu_identity_matches(target, &g->adapters[i])) {
            g->selectedAdapterIndex = i;
            g->selectedGpu = g->adapters[i];
            if (fixture.selectLosesUuid) g->adapterUuids[i] = LinuxGpuUuid{};
        }
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
LinuxDaemonStateLoadResult linux_daemon_state_load(const char*, LinuxDaemonStateRecord* out,
                                                   char*, size_t, bool*) {
    if (fixture.journalUnreadable) return LINUX_DAEMON_STATE_INVALID_REMOVED;
    if (!fixture.journalPresent) return LinuxDaemonStateLoadResult{};
    if (out) *out = fixture.journal;
    return LINUX_DAEMON_STATE_LOADED;
}
bool linux_daemon_state_store(const char*, const LinuxDaemonStateRecord* record,
                              char* err, size_t errSize) {
    ++fixture.stateStores;
    if (fixture.failAllStores) {
        gc_strlcpy(err, errSize, "fixture: storage unavailable");
        return false;
    }
    if (record->state == LINUX_DAEMON_RECORD_ACTIVE &&
        (fixture.activeStoreFailures > 0 ? fixture.activeStoreFailures-- > 0 : fixture.failActiveStore)) {
        gc_strlcpy(err, errSize, "fixture: active record not stored");
        return false;
    }
    fixture.lastStored = *record;
    fixture.journalPresent = true;
    fixture.journal = *record;
    return true;
}
bool linux_daemon_state_remove(const char*, char* err, size_t errSize) {
    if (fixture.removeUnlinksThenFails) {
        fixture.journalPresent = false;
        gc_strlcpy(err, errSize, "fixture: directory sync failed");
        return false;
    }
    if (fixture.failStateRemove) {
        gc_strlcpy(err, errSize, "fixture: state not removed");
        return false;
    }
    fixture.journalPresent = false;
    return true;
}
bool linux_daemon_operation_store(const char*, const LinuxDaemonOperationRecord*, char*, size_t) { return true; }
bool linux_daemon_operation_load(const char*, LinuxDaemonOperationRecord*, char*, size_t) { return false; }
bool linux_daemon_startup_load(const char*, LinuxDaemonStartupRecord*, bool*, char*, size_t, bool*) { return false; }
bool linux_daemon_startup_store(const char*, const LinuxDaemonStartupRecord*, char*, size_t) {
    ++fixture.startupStores;
    return true;
}
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
    gpu.adapterReportedCount = 1;
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

// 14930-14935: the saved journal on disk decides which card may be written
// (linux_daemon_journal.h). The served card is adapter 0 of two.
static LinuxDaemonStateRecord fixture_journal(const GpuAdapterInfo& target, gc_u32 state) {
    DesiredSettings desired = {};
    desired.hasGpuOffset = 1;
    desired.gpuOffsetMHz = 35;
    LinuxDaemonStateRecord record = {};
    linux_daemon_record_initialize(&record, (LinuxDaemonRecordState)state, &target, &desired,
        0, SERVICE_OPERATION_NONE, GPU_LOCATION_ROLE_UNRECORDED, nullptr, 0);
    return record;
}
static int run_linux_daemon_journal_cases(LinuxGpuController& ctl, ServiceRequest& req,
                                          ServiceResponse& resp) {
    const auto setup = [&](const LinuxDaemonStateRecord* journal) {
        fixture = DaemonFixture{};
        fixture_controller(ctl);
        ctl.gpu.adapterCount = ctl.gpu.adapterReportedCount = 2;
        ctl.gpu.adapters[1] = ctl.gpu.adapters[0];
        ctl.gpu.adapters[1].pciBus = 2;
        ctl.gpu.adapters[1].deviceId = 0x220410deu;
        if (journal) { fixture.journalPresent = true; fixture.journal = *journal; }
    };
    const auto send = [&](gc_u32 command, gc_u64 operationId) {
        fixture_request(req, ctl, command, operationId, fixture_observe(ctl));
        resp = ServiceResponse{};
        handle_request(ctl, &req, &resp);
    };
    // Another present card's journal: neither Apply nor Reset writes anything.
    setup(nullptr);
    const LinuxDaemonStateRecord otherCard = fixture_journal(ctl.gpu.adapters[1], LINUX_DAEMON_RECORD_ACTIVE);
    for (gc_u32 command : {SERVICE_CMD_APPLY, SERVICE_CMD_RESET}) {
        setup(&otherCard);
        send(command, 40 + command);
        if (resp.status == SERVICE_STATUS_OK || fixture.mutated || fixture.stateStores ||
            !std::strstr(resp.message, "select it and Reset it")) {
            std::fprintf(stderr, "daemon journal other card command=%u: %s\n",
                         (unsigned int)command, resp.message);
            return 14930;
        }
        if (memcmp(&fixture.journal, &otherCard, sizeof(otherCard))) return 14931;
    }
    // A card that is not present: Apply refuses; an explicit Reset of the
    // served card releases its journal.
    GpuAdapterInfo absent = ctl.gpu.adapters[0];
    absent.pciBus = 9;
    absent.deviceId = 0x268410deu;
    const LinuxDaemonStateRecord gone = fixture_journal(absent, LINUX_DAEMON_RECORD_ACTIVE);
    setup(&gone);
    send(SERVICE_CMD_APPLY, 50);
    if (resp.status == SERVICE_STATUS_OK || fixture.mutated ||
        !std::strstr(resp.message, "Reset this GPU to release it")) return 14932;
    setup(&gone);
    send(SERVICE_CMD_RESET, 51);
    // Released as its own step before this card's Reset, which then runs and
    // journals normally (its own PREPARED, removed on commit).
    if (resp.status != SERVICE_STATUS_OK || !fixture.mutated || fixture.journalPresent ||
        fixture.stateStores != 1 || fixture.lastStored.targetGpu.pciBus != ctl.gpu.selectedGpu.pciBus) {
        std::fprintf(stderr, "daemon journal release: status=%u %s\n",
                     (unsigned int)resp.status, resp.message);
        return 14933;
    }
    // ...and a release that cannot reach disk stops before any write, with
    // the journal exactly as it was.
    setup(&gone);
    fixture.failStateRemove = true;
    send(SERVICE_CMD_RESET, 52);
    if (resp.status == SERVICE_STATUS_OK || fixture.mutated || !fixture.journalPresent ||
        memcmp(&fixture.journal, &gone, sizeof(gone))) return 14934;
    // A release whose unlink happened but could be neither synced nor undone
    // holds every write until that exact journal is stored again (14959).
    setup(&gone);
    fixture.removeUnlinksThenFails = true;
    fixture.failAllStores = true;
    send(SERVICE_CMD_RESET, 57);
    if (resp.status == SERVICE_STATUS_OK || fixture.mutated || fixture.journalPresent ||
        !ctl.journalHoldActive || !std::strstr(resp.message, "may already be gone")) return 14959;
    fixture.removeUnlinksThenFails = false;
    send(SERVICE_CMD_APPLY, 58);
    if (resp.status == SERVICE_STATUS_OK || fixture.mutated ||
        !std::strstr(resp.message, "could not be made durable")) return 14959;
    fixture.failAllStores = false;
    send(SERVICE_CMD_APPLY, 59);
    if (fixture.mutated || ctl.journalHoldActive || !fixture.journalPresent ||
        memcmp(&fixture.journal, &gone, sizeof(gone)) ||
        !std::strstr(resp.message, "Reset this GPU to release it")) {
        std::fprintf(stderr, "daemon journal hold: %s\n", resp.message);
        return 14959;
    }
    // Unfinished recovery of a card that is not present is not released by
    // another card's Reset while another card exists for it to belong to.
    const LinuxDaemonStateRecord goneUncertain = fixture_journal(absent, LINUX_DAEMON_RECORD_UNCERTAIN);
    setup(&goneUncertain);
    send(SERVICE_CMD_RESET, 54);
    if (resp.status == SERVICE_STATUS_OK || fixture.mutated || fixture.stateStores ||
        !std::strstr(resp.message, "reconnect it and Reset it")) return 14937;
    // ...not even when the served card is the only one (14952).
    setup(&goneUncertain);
    ctl.gpu.adapterCount = ctl.gpu.adapterReportedCount = 1;
    send(SERVICE_CMD_RESET, 56);
    if (resp.status == SERVICE_STATUS_OK || fixture.mutated || fixture.stateStores) return 14952;
    // A transient UUID read failure does not strip this card's learned UUID
    // from its journal (a single GPU, which keeps the location rule).
    LinuxDaemonStateRecord bound = fixture_journal(ctl.gpu.adapters[0], LINUX_DAEMON_RECORD_ACTIVE);
    bound.targetUuid = linux_gpu_uuid_parse("GPU-cccccccc-1111-2222-3333-444444444444");
    bound.checksum = linux_daemon_record_checksum(&bound);
    setup(&bound);
    ctl.gpu.adapterCount = ctl.gpu.adapterReportedCount = 1;
    send(SERVICE_CMD_APPLY, 55);
    if (resp.status != SERVICE_STATUS_OK ||
        std::strcmp(fixture.lastStored.targetUuid.text, bound.targetUuid.text)) {
        std::fprintf(stderr, "daemon journal learned uuid: %s\n", resp.message);
        return 14938;
    }
    // An automatic (resume) restore is admitted through the same journal
    // check: another present card's journal stops it before any write (14939).
    const LinuxDaemonStateRecord own = fixture_journal(ctl.gpu.adapters[0], LINUX_DAEMON_RECORD_ACTIVE);
    const auto resume = [&]() {
        req = ServiceRequest{};
        req.magic = SERVICE_PROTOCOL_MAGIC;
        req.version = SERVICE_PROTOCOL_VERSION;
        req.command = SERVICE_CMD_RESUME_RESTORE;
        req.callerPid = (gc_u32)getpid();
        resp = ServiceResponse{};
        handle_request(ctl, &req, &resp);
    };
    const auto holdIntent = [&](const GpuAdapterInfo& target) {
        ctl.hasActiveDesired = true;
        ctl.activeTarget = target;
        ctl.activeDesired = DesiredSettings{};
        ctl.activeDesired.hasGpuOffset = 1;
        ctl.activeDesired.gpuOffsetMHz = 35;
    };
    setup(&otherCard);
    holdIntent(ctl.gpu.adapters[0]);
    resume();
    if (fixture.mutated || fixture.stateStores ||
        !std::strstr(resp.message, "automatic restore refused")) {
        std::fprintf(stderr, "daemon journal resume: %s\n", resp.message);
        return 14939;
    }
    // A failed unattended write journals the card the backend resolved and
    // wrote, not the stored target's older location (14950).
    setup(&own);
    // A lone card follows its model after a renumbering, so the old location
    // still resolves to it (and is proven: discovery listed every device).
    ctl.gpu.adapterCount = ctl.gpu.adapterReportedCount = 1;
    GpuAdapterInfo oldLocation = ctl.gpu.adapters[0];
    oldLocation.pciBus = 7;
    holdIntent(oldLocation);
    ctl.activeTargetRole = GPU_LOCATION_ROLE_ADVISORY;
    fixture.failApply = true;
    resume();
    if (!fixture.mutated || fixture.lastStored.state != LINUX_DAEMON_RECORD_UNCERTAIN ||
        fixture.lastStored.targetGpu.pciBus != ctl.gpu.selectedGpu.pciBus) {
        std::fprintf(stderr, "daemon journal resume failure: %s\n", resp.message);
        return 14950;
    }
    // An unattended write proves its own target too, not only the journal:
    // with no journal on disk and one of two reported devices not opened, an
    // intent bound to another location cannot follow its model here (14954).
    setup(nullptr);
    ctl.gpu.adapterCount = 1;
    ctl.gpu.adapterReportedCount = 2;
    GpuAdapterInfo elsewhere = ctl.gpu.adapters[0];
    elsewhere.pciBus = 5;
    holdIntent(elsewhere);
    ctl.activeTargetRole = GPU_LOCATION_ROLE_ADVISORY;  // would follow its model on one card
    resume();
    if (fixture.mutated || !std::strstr(resp.message, "proven on the bound card (unproven)")) {
        std::fprintf(stderr, "daemon journal target proof: %s\n", resp.message);
        return 14954;
    }
    // 14942: with two cards, the served card's own journal naming UUID A is
    // proven only by reading A now: with the current read missing, Apply is
    // refused (no write by location); with every card read, it proceeds.
    LinuxDaemonStateRecord boundA = fixture_journal(ctl.gpu.adapters[0], LINUX_DAEMON_RECORD_ACTIVE);
    boundA.targetUuid = linux_gpu_uuid_parse("GPU-aaaaaaaa-1111-2222-3333-444444444444");
    boundA.checksum = linux_daemon_record_checksum(&boundA);
    setup(&boundA);
    send(SERVICE_CMD_APPLY, 60);
    if (resp.status == SERVICE_STATUS_OK || fixture.mutated ||
        !std::strstr(resp.message, "cannot be identified")) {
        std::fprintf(stderr, "daemon journal unread UUID: %s\n", resp.message);
        return 14942;
    }
    setup(&boundA);
    ctl.gpu.adapterUuids[0] = boundA.targetUuid;
    ctl.gpu.adapterUuids[1] = linux_gpu_uuid_parse("GPU-bbbbbbbb-1111-2222-3333-444444444444");
    send(SERVICE_CMD_APPLY, 61);
    if (resp.status != SERVICE_STATUS_OK || !fixture.mutated) {
        std::fprintf(stderr, "daemon journal read UUID: %s\n", resp.message);
        return 14942;
    }
    // 14943: a single GPU keeps the location rule as before, with or without
    // a readable UUID.
    setup(&boundA);
    ctl.gpu.adapterCount = ctl.gpu.adapterReportedCount = 1;
    send(SERVICE_CMD_APPLY, 62);
    if (resp.status != SERVICE_STATUS_OK || !fixture.mutated) {
        std::fprintf(stderr, "daemon journal single GPU: %s\n", resp.message);
        return 14943;
    }
    // 14946: UUIDs the binding read in an NVML session since replaced are not
    // a current read: every request drops them first, so A read in the old
    // session does not prove the journal.
    static LinuxDriverContext sessionDriver;
    sessionDriver = LinuxDriverContext{};
    setup(&boundA);
    ctl.gpu.driver = &sessionDriver;
    sessionDriver.nvmlSessions = 2;
    ctl.gpu.uuidSession = 1;
    ctl.gpu.adapterUuids[0] = boundA.targetUuid;
    send(SERVICE_CMD_APPLY, 63);
    ctl.gpu.driver = nullptr;
    if (resp.status == SERVICE_STATUS_OK || fixture.mutated || linux_gpu_uuid_valid(&ctl.gpu.adapterUuids[0])) {
        std::fprintf(stderr, "daemon journal stale session: %s\n", resp.message);
        return 14946;
    }
    // The same for an unattended write outside any request (boot restore).
    setup(&boundA);
    ctl.gpu.driver = &sessionDriver;
    ctl.gpu.uuidSession = 1;
    ctl.gpu.adapterUuids[0] = boundA.targetUuid;
    const LinuxAutoRestoreOutcome staleBoot = daemon_automatic_restore_write(ctl,
        LINUX_AUTO_RESTORE_TRIGGER_BOOT_RESTORE_LAST, &boundA.targetGpu, &boundA.desired,
        (GpuLocationRole)boundA.targetLocationRole, &boundA.targetUuid, &boundA);
    ctl.gpu.driver = nullptr;
    if (staleBoot.success || fixture.mutated) {
        std::fprintf(stderr, "daemon journal stale session boot: %s\n", staleBoot.message);
        return 14946;
    }
    // 14948: nor does a startup policy store such a UUID as its binding.
    setup(nullptr);
    ctl.gpu.driver = &sessionDriver;
    ctl.gpu.uuidSession = 1;
    ctl.gpu.adapterUuids[0] = boundA.targetUuid;
    req = ServiceRequest{};
    req.magic = SERVICE_PROTOCOL_MAGIC;
    req.version = SERVICE_PROTOCOL_VERSION;
    req.command = SERVICE_CMD_SET_STARTUP_POLICY;
    req.callerPid = (gc_u32)getpid();
    req.startupMode = SERVICE_STARTUP_POLICY_PROFILE;
    req.profileSlot = 1;
    req.targetGpu = ctl.gpu.adapters[0];
    resp = ServiceResponse{};
    handle_request(ctl, &req, &resp);
    ctl.gpu.driver = nullptr;
    if (resp.status != SERVICE_STATUS_OK || fixture.startupStores != 1 ||
        linux_gpu_uuid_valid(&ctl.startupPolicy.targetUuid)) {
        std::fprintf(stderr, "daemon startup policy stale session: %s\n", resp.message);
        return 14948;
    }
    // 14944: the startup binding is proven by the same rule: a refresh naming
    // the bound card is refused (and the startup topology is not published as
    // bound) until its UUID is read now; both hold once it is.
    bool topologyBound = false;
    const auto refreshBound = [&](const LinuxGpuUuid& current) {
        ctl.gpu.adapterUuids[0] = current;
        ctl.gpu.adapterUuids[1] = linux_gpu_uuid_parse("GPU-bbbbbbbb-1111-2222-3333-444444444444");
        req = ServiceRequest{};
        req.magic = SERVICE_PROTOCOL_MAGIC;
        req.version = SERVICE_PROTOCOL_VERSION;
        req.command = SERVICE_CMD_REFRESH_STARTUP_PROFILE;
        req.callerPid = (gc_u32)getpid();
        req.profileSlot = 1;
        req.targetGpu = ctl.gpu.adapters[0];
        resp = ServiceResponse{};
        handle_request(ctl, &req, &resp);
        topologyBound = daemon_selected_startup_topology(ctl);
        return resp.status;
    };
    setup(nullptr);
    ctl.startupPolicy.mode = SERVICE_STARTUP_POLICY_PROFILE;
    ctl.startupPolicy.profileSlot = 1;
    ctl.startupPolicy.targetGpu = ctl.gpu.adapters[0];
    ctl.startupPolicy.targetLocationRole = GPU_LOCATION_ROLE_UNRECORDED;
    ctl.startupPolicy.targetUuid = boundA.targetUuid;
    if (refreshBound(LinuxGpuUuid{}) != SERVICE_STATUS_STALE_STATE || fixture.startupStores || topologyBound ||
        refreshBound(boundA.targetUuid) != SERVICE_STATUS_OK || fixture.startupStores != 1 || !topologyBound) {
        std::fprintf(stderr, "daemon startup binding proof: %s\n", resp.message);
        return 14944;
    }
    // 14974: the intent in memory drives the fan worker and the snapshot's fan
    // state only while it is proven on the served card by the journal rule: a
    // different card read at its location (both cards read) is not the card
    // the intent was written for, whatever its PCI identity.
    setup(nullptr);
    holdIntent(ctl.gpu.adapters[0]);
    ctl.activeDesired.hasFan = 1;
    ctl.activeDesired.fanMode = FAN_MODE_FIXED;
    ctl.activeDesired.fanPercent = 60;
    ctl.activeTargetUuid = boundA.targetUuid;
    ctl.gpu.adapterUuids[0] = boundA.targetUuid;
    ctl.gpu.adapterUuids[1] = linux_gpu_uuid_parse("GPU-cccccccc-1111-2222-3333-444444444444");
    ServiceSnapshot ownedSnapshot{};
    populate_snapshot(ctl, &ownedSnapshot, nullptr);
    const bool ownedBefore = daemon_active_intent_on_served(ctl) && ownedSnapshot.fanFixedRuntimeActive;
    ctl.gpu.adapterUuids[0] = linux_gpu_uuid_parse("GPU-bbbbbbbb-1111-2222-3333-444444444444");
    ServiceSnapshot otherSnapshot{};
    populate_snapshot(ctl, &otherSnapshot, nullptr);
    ServiceResponse otherSummary{};
    populate_snapshot(ctl, &otherSummary.snapshot, nullptr);
    daemon_publish_adapter_summaries(ctl, &otherSummary);
    if (!ownedBefore || daemon_active_intent_on_served(ctl) || otherSnapshot.fanFixedRuntimeActive ||
        otherSnapshot.activeFanFixedPercent ||
        (otherSummary.snapshot.adapterSummaries[0].flags & SERVICE_ADAPTER_ACTIVE)) return 14974;
    // Another card's unread UUID is no reason to stop: the served card's own
    // read is what names it, so its fan stays maintained (not left pinned).
    ctl.gpu.adapterUuids[0] = boundA.targetUuid;
    ctl.gpu.adapterUuids[1] = LinuxGpuUuid{};
    ServiceResponse ownedSummary{};
    populate_snapshot(ctl, &ownedSummary.snapshot, nullptr);
    daemon_publish_adapter_summaries(ctl, &ownedSummary);
    if (!daemon_active_intent_on_served(ctl) || !ownedSummary.snapshot.fanFixedRuntimeActive ||
        !(ownedSummary.snapshot.adapterSummaries[0].flags & SERVICE_ADAPTER_ACTIVE)) return 14974;
    // Another card reading the same UUID (a duplicate, however it was found)
    // proves neither: the intent is not owned and its fan is not maintained.
    ctl.gpu.adapterUuids[1] = boundA.targetUuid;
    ServiceSnapshot duplicateSnapshot{};
    populate_snapshot(ctl, &duplicateSnapshot, nullptr);
    if (daemon_active_intent_on_served(ctl) || duplicateSnapshot.fanFixedRuntimeActive) return 14974;
    // 14978: an Apply whose own post-write refresh lost the binding's identity
    // (and the UUID read) is not committed as a success: its rollback is
    // refused, and the durable record is UNCERTAIN, keeping the UUID verified
    // before the write.
    setup(nullptr);
    ctl.gpu.adapterUuids[0] = boundA.targetUuid;
    ctl.gpu.adapterUuids[1] = linux_gpu_uuid_parse("GPU-cccccccc-1111-2222-3333-444444444444");
    fixture.loseUuidAfterWrite = true;
    send(SERVICE_CMD_APPLY, 64);
    fixture.loseUuidAfterWrite = false;
    if (resp.status == SERVICE_STATUS_OK || fixture.lastStored.state != LINUX_DAEMON_RECORD_UNCERTAIN ||
        !ctl.stateUncertain || std::strcmp(fixture.lastStored.targetUuid.text, boundA.targetUuid.text)) {
        std::fprintf(stderr, "daemon apply pinned uuid: %s\n", resp.message);
        return 14978;
    }
    // 14979: after a renumbering, the card that reads the intent's UUID may be
    // selected to Reset it, whatever another card's read; once selected the
    // intent follows it, so the fan worker and Reset (which compare PCI
    // identity) see it as this card's. An unrelated card, an intent with no
    // UUID (a model match is no proof), and a UUID two cards read are refused;
    // a selection whose new binding no longer reads the UUID reports an owned
    // manual fan instead of leaving it silently untracked.
    const auto selectGpu = [&](const GpuAdapterInfo& target) {
        ServiceRequest select = {};
        select.targetGpu = target;
        char selectErr[256] = {};
        return select_request_gpu(ctl, &select, selectErr, sizeof(selectErr));
    };
    const auto movedIntent = [&](const LinuxGpuUuid& atNewLocation, const LinuxGpuUuid& sibling) {
        setup(nullptr);
        holdIntent(ctl.gpu.adapters[0]);
        ctl.activeDesired.hasFan = 1;
        ctl.activeDesired.fanMode = FAN_MODE_FIXED;
        ctl.activeDesired.fanPercent = 60;
        ctl.activeTarget.pciBus = 7;  // the intent's location before the renumbering
        ctl.activeTargetUuid = boundA.targetUuid;
        ctl.gpu.adapterUuids[0] = atNewLocation;
        ctl.gpu.adapterUuids[1] = sibling;
        ctl.gpu.selectedAdapterIndex = 1;
        ctl.gpu.selectedGpu = ctl.gpu.adapters[1];
    };
    movedIntent(boundA.targetUuid, LinuxGpuUuid{});
    if (!selectGpu(ctl.gpu.adapters[0]) ||
        !linux_gpu_identity_matches(&ctl.activeTarget, &ctl.gpu.selectedGpu) ||
        !linux_gpu_identity_matches(&ctl.activeTarget, &ctl.gpu.adapters[0]) ||
        !daemon_active_intent_on_served(ctl) || ctl.stateUncertain) return 14979;
    movedIntent(linux_gpu_uuid_parse("GPU-dddddddd-1111-2222-3333-444444444444"), LinuxGpuUuid{});
    if (selectGpu(ctl.gpu.adapters[0])) return 14979;
    movedIntent(boundA.targetUuid, LinuxGpuUuid{});
    ctl.activeTargetUuid = LinuxGpuUuid{};
    if (selectGpu(ctl.gpu.adapters[0])) return 14979;
    // Two cards reading the same UUID: neither proves the intent, so it does
    // not move to the other one (its fan would be driven instead).
    movedIntent(boundA.targetUuid, boundA.targetUuid);
    if (selectGpu(ctl.gpu.adapters[0])) return 14979;
    movedIntent(boundA.targetUuid, LinuxGpuUuid{});
    fixture.selectLosesUuid = true;
    const bool lostSelected = selectGpu(ctl.gpu.adapters[0]);
    fixture.selectLosesUuid = false;
    if (!lostSelected || linux_gpu_identity_matches(&ctl.activeTarget, &ctl.gpu.selectedGpu) ||
        !ctl.stateUncertain || ctl.fanFault.kind != SERVICE_FAN_FAULT_REPEATED_FAILURES) return 14979;
    // 14980: a Reset whose hardware write succeeded but whose journal removal
    // and rollback both failed leaves a durable UNCERTAIN record bound to the
    // pinned UUID, not the old ACTIVE one a restart could replay.
    setup(&boundA);
    ctl.gpu.adapterUuids[0] = boundA.targetUuid;
    ctl.gpu.adapterUuids[1] = linux_gpu_uuid_parse("GPU-cccccccc-1111-2222-3333-444444444444");
    fixture.failStateRemove = true;
    fixture.failRestore = true;
    send(SERVICE_CMD_RESET, 65);
    fixture.failStateRemove = fixture.failRestore = false;
    // Only PREPARED then UNCERTAIN: the old ACTIVE record is never written back
    // after the failed rollback, not even for a moment.
    if (fixture.lastStored.state != LINUX_DAEMON_RECORD_UNCERTAIN || !ctl.stateUncertain ||
        fixture.stateStores != 2 ||
        std::strcmp(fixture.lastStored.targetUuid.text, boundA.targetUuid.text)) {
        std::fprintf(stderr, "daemon reset rollback failure: %s state=%u\n", resp.message,
                     (unsigned int)fixture.lastStored.state);
        return 14980;
    }
    // 14982: the same for a Reset and an automatic restore: a write after which
    // the binding lost its identity is never committed (no ACTIVE record is
    // stored for an unproven card); the record is UNCERTAIN and the daemon
    // uncertain.
    setup(&boundA);
    ctl.gpu.adapterUuids[0] = boundA.targetUuid;
    ctl.gpu.adapterUuids[1] = linux_gpu_uuid_parse("GPU-cccccccc-1111-2222-3333-444444444444");
    fixture.loseUuidAfterWrite = true;
    send(SERVICE_CMD_RESET, 66);
    fixture.loseUuidAfterWrite = false;
    if (resp.status == SERVICE_STATUS_OK || fixture.lastStored.state != LINUX_DAEMON_RECORD_UNCERTAIN ||
        !ctl.stateUncertain) return 14982;
    setup(&boundA);
    ctl.gpu.adapterUuids[0] = boundA.targetUuid;
    ctl.gpu.adapterUuids[1] = linux_gpu_uuid_parse("GPU-cccccccc-1111-2222-3333-444444444444");
    fixture.loseUuidAfterWrite = true;
    const LinuxAutoRestoreOutcome lostRestore = daemon_automatic_restore_write(ctl,
        LINUX_AUTO_RESTORE_TRIGGER_BOOT_RESTORE_LAST, &boundA.targetGpu, &boundA.desired,
        (GpuLocationRole)boundA.targetLocationRole, &boundA.targetUuid, &boundA);
    fixture.loseUuidAfterWrite = false;
    if (lostRestore.success || fixture.lastStored.state != LINUX_DAEMON_RECORD_UNCERTAIN || !ctl.stateUncertain) {
        std::fprintf(stderr, "daemon restore lost identity: %s\n", lostRestore.message);
        return 14982;
    }
    // 14984: a first Apply writes a UUID-bound journal only where this card
    // could clear it again: with another card's UUID unread, Reset could not
    // prove it, so the Apply is refused before any write; with every card read
    // it proceeds.
    setup(nullptr);
    ctl.gpu.adapterUuids[0] = boundA.targetUuid;
    send(SERVICE_CMD_APPLY, 67);
    if (resp.status == SERVICE_STATUS_OK || fixture.mutated || !std::strstr(resp.message, "save its settings"))
        return 14984;
    // The same for replacing this card's journal that had no UUID with one
    // that binds the UUID now read.
    const LinuxDaemonStateRecord uuidLess = fixture_journal(ctl.gpu.adapters[0], LINUX_DAEMON_RECORD_ACTIVE);
    setup(&uuidLess);
    ctl.gpu.adapterUuids[0] = boundA.targetUuid;
    send(SERVICE_CMD_APPLY, 69);
    if (resp.status == SERVICE_STATUS_OK || fixture.mutated) return 14984;
    setup(nullptr);
    ctl.gpu.adapterUuids[0] = boundA.targetUuid;
    ctl.gpu.adapterUuids[1] = linux_gpu_uuid_parse("GPU-cccccccc-1111-2222-3333-444444444444");
    send(SERVICE_CMD_APPLY, 68);
    if (resp.status != SERVICE_STATUS_OK || !fixture.mutated) return 14984;
    // 14977: a record stored for an explicit UUID keeps it: a different UUID
    // read from the card now at that location never re-attributes the journal.
    setup(nullptr);
    ctl.gpu.adapterUuids[0] = linux_gpu_uuid_parse("GPU-bbbbbbbb-1111-2222-3333-444444444444");
    char explicitErr[128] = {};
    if (!store_daemon_record(ctl, LINUX_DAEMON_RECORD_UNCERTAIN, &ctl.gpu.selectedGpu, &boundA.desired,
            explicitErr, sizeof(explicitErr), 0, SERVICE_OPERATION_NONE, GPU_LOCATION_ROLE_REQUIRED,
            &boundA.targetUuid) ||
        std::strcmp(fixture.lastStored.targetUuid.text, boundA.targetUuid.text)) return 14977;
    // 14983: nor does a record stored without an explicit UUID take one from a
    // binding that lost its write identity: that read may be another card's.
    setup(nullptr);
    ctl.gpu.adapterUuids[0] = linux_gpu_uuid_parse("GPU-bbbbbbbb-1111-2222-3333-444444444444");
    ctl.gpu.writeIdentityResolved = false;
    if (!store_daemon_record(ctl, LINUX_DAEMON_RECORD_UNCERTAIN, &ctl.gpu.selectedGpu, &boundA.desired,
            explicitErr, sizeof(explicitErr)) ||
        linux_gpu_uuid_valid(&fixture.lastStored.targetUuid)) return 14983;
    // 14987: a failed operation that keeps an uncertain record with no
    // previous intent never pairs this card with a stale binding's UUID.
    setup(nullptr);
    ctl.activeTargetUuid = boundA.targetUuid;
    ctl.activeTargetRole = GPU_LOCATION_ROLE_REQUIRED;
    const DesiredSettings noPrevious = {};
    restore_committed_record(ctl, true, false, &ctl.gpu.selectedGpu, &noPrevious, nullptr, nullptr);
    if (fixture.lastStored.state != LINUX_DAEMON_RECORD_UNCERTAIN ||
        linux_gpu_uuid_valid(&fixture.lastStored.targetUuid)) return 14987;
    // 14986: a binding pinned before a write is kept as given, even without a
    // UUID: a UUID first read during the write never enters the record, so the
    // record stays one this card's Reset can prove (location rule).
    setup(nullptr);
    ctl.gpu.adapterUuids[0] = boundA.targetUuid;
    const LinuxGpuUuid pinnedNone = {};
    if (!store_daemon_record(ctl, LINUX_DAEMON_RECORD_ACTIVE, &ctl.gpu.selectedGpu, &boundA.desired,
            explicitErr, sizeof(explicitErr), 0, SERVICE_OPERATION_NONE, GPU_LOCATION_ROLE_REQUIRED, &pinnedNone) ||
        linux_gpu_uuid_valid(&fixture.lastStored.targetUuid)) return 14986;
    // 14945: a journal's named graphics coverage joins the card's recovery only
    // after the journal and the target are proven to be this card's: a refused
    // unattended write leaves no coverage behind.
    LinuxDaemonStateRecord covered = boundA;
    covered.desired.authoring.graphicsPrecise = 1;
    covered.desired.authoring.graphicsSurface = DESIRED_GRAPHICS_P0;
    covered.graphicsControlSurfaces = GRAPHICS_SCALAR_RECOVERY_P0;
    covered.checksum = linux_daemon_record_checksum(&covered);
    setup(&covered);
    const LinuxAutoRestoreOutcome coveredOutcome = daemon_automatic_restore_write(ctl,
        LINUX_AUTO_RESTORE_TRIGGER_BOOT_RESTORE_LAST, &covered.targetGpu, &covered.desired,
        (GpuLocationRole)covered.targetLocationRole, &covered.targetUuid, &covered);
    if (coveredOutcome.success || fixture.mutated || ctl.gpu.graphicsRecovery.covered) {
        std::fprintf(stderr, "daemon journal coverage: %s\n", coveredOutcome.message);
        return 14945;
    }
    // An unreadable journal stops a resume and holds that refusal: the
    // loader removed the evidence, so the next resume must not find nothing
    // to refuse on (14953).
    setup(nullptr);
    holdIntent(ctl.gpu.adapters[0]);
    fixture.journalUnreadable = true;
    resume();
    if (fixture.mutated || !ctl.stateUncertain) return 14953;
    fixture.journalUnreadable = false;
    resume();
    if (fixture.mutated) return 14953;
    // The served card's own journal that this daemon did not replay (policy
    // "none"; no intent in memory): a failed Apply restores it, not deletes it.
    setup(&own);
    fixture.activeStoreFailures = 1;
    send(SERVICE_CMD_APPLY, 53);
    if (!fixture.mutated || fixture.restoreCalls != 1 || !fixture.journalPresent ||
        memcmp(&fixture.lastStored, &own, sizeof(own))) {
        std::fprintf(stderr, "daemon journal preimage: %s\n", resp.message);
        return 14935;
    }
    return 0;
}

// 14630-14639: one served GPU and an identical second card at another
// location. Every routed command except the full state read is refused for
// the second card without reaching its handler or the backend.
static int run_linux_daemon_route_cases(LinuxGpuController& ctl, ServiceRequest& req,
                                        ServiceResponse& resp) {
    fixture = DaemonFixture{};
    fixture_controller(ctl);
    ctl.gpu.adapterCount = ctl.gpu.adapterReportedCount = 2;
    ctl.gpu.adapters[1] = ctl.gpu.adapters[0];
    ctl.gpu.adapters[1].pciBus = 2;
    ctl.gpu.adapters[1].nvapiIndex = ctl.gpu.adapters[1].nvmlIndex = 1;
    const GpuAdapterInfo served = ctl.gpu.selectedGpu;
    const GpuAdapterInfo other = ctl.gpu.adapters[1];
    const gc_u64 generation = ctl.gpuGeneration;
    const auto routed = [&](gc_u32 command, const GpuAdapterInfo& target) {
        req = ServiceRequest{};
        req.magic = SERVICE_PROTOCOL_MAGIC;
        req.version = SERVICE_PROTOCOL_VERSION;
        req.command = command;
        req.callerPid = (gc_u32)getpid();
        req.targetGpu = target;
        if (command == SERVICE_CMD_CAPTURE_CURRENT) req.captureDomains = 1;
        if (command == SERVICE_CMD_SET_STARTUP_POLICY) {
            req.startupMode = SERVICE_STARTUP_POLICY_PROFILE;
            req.profileSlot = 1;
        }
        resp = ServiceResponse{};
        handle_request(ctl, &req, &resp);
        return resp.status;
    };
    const gc_u32 refused[] = {SERVICE_CMD_GET_TELEMETRY, SERVICE_CMD_GET_ACTIVE_DESIRED,
        SERVICE_CMD_CAPTURE_CURRENT};
    for (gc_u32 command : refused) {
        if (routed(command, other) != SERVICE_STATUS_STALE_STATE ||
            std::strcmp(resp.message, SERVICE_GPU_ROUTE_NOT_SERVED_MESSAGE)) {
            std::fprintf(stderr, "daemon route command=%u: status=%u %s\n",
                         (unsigned int)command, (unsigned int)resp.status, resp.message);
            return 14630;
        }
        if (fixture.selectCalls || fixture.startupStores ||
            !linux_gpu_identity_matches(&ctl.gpu.selectedGpu, &served) ||
            ctl.gpuGeneration != generation) return 14631;
    }
    // The same commands routed to the served GPU, or not routed, are answered.
    if (routed(SERVICE_CMD_GET_ACTIVE_DESIRED, served) != SERVICE_STATUS_OK) return 14632;
    // Routed to the served card, capture reaches its own handler.
    if (routed(SERVICE_CMD_CAPTURE_CURRENT, served) == SERVICE_STATUS_STALE_STATE ||
        fixture.captureCalls != 1) return 14633;
    // The daemon-wide boot policy may bind a card the daemon is not serving,
    // as before: its target is the stored binding, not a route.
    if (routed(SERVICE_CMD_SET_STARTUP_POLICY, other) != SERVICE_STATUS_OK ||
        fixture.startupStores != 1 ||
        !linux_gpu_identity_matches(&ctl.startupPolicy.targetGpu, &other)) return 14634;
    // A refresh may name the card it expects; one naming another card than the
    // binding is refused, and the binding never moves (14639-14640).
    const auto refresh = [&](const GpuAdapterInfo& expected) {
        req = ServiceRequest{};
        req.magic = SERVICE_PROTOCOL_MAGIC;
        req.version = SERVICE_PROTOCOL_VERSION;
        req.command = SERVICE_CMD_REFRESH_STARTUP_PROFILE;
        req.callerPid = (gc_u32)getpid();
        req.profileSlot = 1;
        req.targetGpu = expected;
        resp = ServiceResponse{};
        handle_request(ctl, &req, &resp);
        return resp.status;
    };
    if (refresh(served) != SERVICE_STATUS_STALE_STATE || fixture.startupStores != 1 ||
        !linux_gpu_identity_matches(&ctl.startupPolicy.targetGpu, &other)) return 14639;
    if (refresh(other) != SERVICE_STATUS_OK || fixture.startupStores != 2 ||
        !linux_gpu_identity_matches(&ctl.startupPolicy.targetGpu, &other)) return 14640;
    if (routed(SERVICE_CMD_GET_TELEMETRY, GpuAdapterInfo{}) != SERVICE_STATUS_OK ||
        fixture.selectCalls) return 14635;
    // Only the full state read moves the daemon to another GPU.
    if (routed(SERVICE_CMD_GET_SNAPSHOT, other) != SERVICE_STATUS_OK ||
        fixture.selectCalls != 1) return 14636;

    // 14730-14733: the v40 adapter summaries the production stamp publishes.
    // Both UUIDs, the runtime summary on the served card only, agreeing with
    // the envelope; the card holding the intent marked ACTIVE.
    fixture = DaemonFixture{};
    fixture_controller(ctl);
    ctl.gpu.adapterCount = ctl.gpu.adapterReportedCount = 2;
    ctl.gpu.adapters[1] = other;
    ctl.gpu.adapterUuids[0] = linux_gpu_uuid_parse("GPU-aaaaaaaa-1111-2222-3333-444444444444");
    ctl.gpu.adapterUuids[1] = linux_gpu_uuid_parse("GPU-bbbbbbbb-1111-2222-3333-444444444444");
    ctl.hasActiveDesired = true;
    ctl.activeTarget = served;
    if (routed(SERVICE_CMD_GET_SNAPSHOT, served) != SERVICE_STATUS_OK) return 14730;
    const ServiceAdapterSummary* summary = resp.snapshot.adapterSummaries;
    if (!(summary[0].flags & SERVICE_ADAPTER_UUID) || !(summary[1].flags & SERVICE_ADAPTER_UUID) ||
        summary[0].uuid.bytes[0] != 0xaa || summary[1].uuid.bytes[0] != 0xbb) return 14731;
    if (!(summary[0].flags & SERVICE_ADAPTER_RUNTIME) || !(summary[0].flags & SERVICE_ADAPTER_ACTIVE) ||
        summary[1].flags != SERVICE_ADAPTER_UUID ||
        summary[0].gpuPhase != resp.state.gpuPhase) return 14732;
    if (!validate_service_state_envelope_for_ipc(&resp.state, &resp.snapshot, &resp.desired,
                                                 &resp.controlState)) return 14733;
    return run_linux_daemon_journal_cases(ctl, req, resp);
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
    return run_linux_daemon_route_cases(ctl, req, resp);
}
#else
// The Linux daemon is not built for Windows.
int run_linux_daemon_commit_rollback_tests() { return 0; }
#endif
