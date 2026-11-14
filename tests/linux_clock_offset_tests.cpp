// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
// Exercise the production NVML offset setters with independently scripted
// P0 and global controls. No driver library or GPU is loaded.
#include "linux_backend.h"

static void lb_log(const char*, ...) {}
#include "linux_backend_clock_offsets.h"
#include "linux_graphics_offset_control.h"
#include "linux_graphics_recovery_policy.h"

namespace {
struct Driver {
    int p0Value, globalValue;
    int p0Writes, globalWrites, p0Reads, globalReads;
    bool snap, failP0Read, failGlobalRead, failP0Set, badMetadata;
    bool layoutValid;
    bool badRange, globalRangeFails;
    bool narrowRange, failReadAfterWrite;
    int capWrites;
};
static nvmlReturn_t scalar_guard_set(nvmlDevice_t device, unsigned int, unsigned int) {
    ++reinterpret_cast<Driver*>(device)->capWrites; return NVML_SUCCESS;
}
static nvmlReturn_t scalar_guard_reset(nvmlDevice_t device) {
    ++reinterpret_cast<Driver*>(device)->capWrites; return NVML_SUCCESS;
}
static Driver* driver(nvmlDevice_t device) { return reinterpret_cast<Driver*>(device); }
static nvmlReturn_t set_p0(nvmlDevice_t device, nvmlClockOffset_t* info) {
    Driver* d = driver(device);
    ++d->p0Writes;
    d->layoutValid &= info->version == nvmlClockOffset_v1 && info->pstate == NVML_PSTATE_0;
    d->p0Value = d->snap ? (info->clockOffsetMHz / 2) * 2 : info->clockOffsetMHz;
    return d->failP0Set ? NVML_ERROR_NOT_SUPPORTED : NVML_SUCCESS;
}
static nvmlReturn_t get_p0(nvmlDevice_t device, nvmlClockOffset_t* info) {
    Driver* d = driver(device);
    ++d->p0Reads;
    d->layoutValid &= info->version == nvmlClockOffset_v1 && info->pstate == NVML_PSTATE_0;
    if (d->failP0Read || (d->failReadAfterWrite && d->p0Writes)) return NVML_ERROR_NOT_SUPPORTED;
    info->clockOffsetMHz = d->p0Value;
    info->minClockOffsetMHz = -3000; info->maxClockOffsetMHz = 3000;
    if (d->narrowRange) { info->minClockOffsetMHz = -100; info->maxClockOffsetMHz = 100; }
    if (d->badRange) info->minClockOffsetMHz = 4000;
    if (d->badMetadata) info->pstate = 1;
    return NVML_SUCCESS;
}
static nvmlReturn_t set_global(nvmlDevice_t device, int value) {
    Driver* d = driver(device);
    ++d->globalWrites;
    d->globalValue = d->snap ? (value / 2) * 2 : value;
    return NVML_SUCCESS;
}
static nvmlReturn_t get_global(nvmlDevice_t device, int* value) {
    Driver* d = driver(device);
    ++d->globalReads;
    *value = d->globalValue;
    return d->failGlobalRead ? NVML_ERROR_NOT_SUPPORTED : NVML_SUCCESS;
}
static nvmlReturn_t get_global_range(nvmlDevice_t device, int* minimum, int* maximum) {
    *minimum = -3000; *maximum = 3000;
    return driver(device)->globalRangeFails ? NVML_ERROR_UNKNOWN : NVML_SUCCESS;
}
static void initialize(Driver* d, LinuxGpuState* g) {
    *d = {}; *g = {}; d->layoutValid = true;
    g->nvmlDevice = reinterpret_cast<nvmlDevice_t>(d);
    g->nvml.setClockOffsets = set_p0; g->nvml.getClockOffsets = get_p0;
    g->nvml.setMemClkVfOffset = set_global; g->nvml.getMemClkVfOffset = get_global;
    g->nvml.setGpcClkVfOffset = set_global; g->nvml.getGpcClkVfOffset = get_global;
    // These setter fixtures start after a successful synthetic journal store.
    // The recovery cases below explicitly exercise denied and fresh permits.
    g->graphicsRecovery.prepared = GRAPHICS_SCALAR_RECOVERY_ALL;
}

static int graphics_recovery_cases() {
    Driver state{}; LinuxGpuState g{}; initialize(&state, &g);
    DesiredSettings d{}; d.hasGpuOffset = 1;
    d.authoring.graphicsPrecise = 1; d.authoring.graphicsSurface = DESIRED_GRAPHICS_GLOBAL;
    ScalarCurveSubmissionReview review{};
    {
        GraphicsScalarOperation operation(g.graphicsRecovery);
        int stores = 0;
        if (graphics_scalar_operation_prepare(g.graphicsRecovery, GRAPHICS_SCALAR_RECOVERY_GLOBAL,
                [&] { ++stores; return false; }) ||
            linux_apply_named_graphics_offset(&g, &d, &review) || !review.refusedWithoutWrite ||
            stores != 1 || state.globalWrites || g.graphicsRecovery.covered) return 9060;
        if (!graphics_scalar_operation_prepare(g.graphicsRecovery, GRAPHICS_SCALAR_RECOVERY_GLOBAL,
                [&] { ++stores; return !g.graphicsRecovery.prepared; }) ||
            !linux_apply_named_graphics_offset(&g, &d, &review) || state.globalWrites ||
            g.graphicsRecovery.covered != GRAPHICS_SCALAR_RECOVERY_GLOBAL ||
            g.graphicsRecovery.ownedSurface != CLOCK_OFFSET_SURFACE_GLOBAL ||
            g.graphicsRecovery.attempted || stores != 2) return 9061;
    }
    if (g.graphicsRecovery.prepared || g.graphicsRecovery.attempted ||
        !g.graphicsRecovery.covered) return 9062;
    initialize(&state, &g);
    state.p0Value = 71; state.globalValue = -243;
    g.graphicsRecovery.covered = GRAPHICS_SCALAR_RECOVERY_ALL;
    g.graphicsRecovery.uncertain = GRAPHICS_SCALAR_RECOVERY_ALL;
    LinuxHardwareSnapshot snapshot{};
    if (!linux_capture_named_graphics(&g, &snapshot, g.graphicsRecovery.covered) ||
        snapshot.graphicsPreimage.p0MHz != 71 || snapshot.graphicsPreimage.globalMHz != -243 ||
        state.p0Writes || state.globalWrites) return 9063;
    state.failP0Set = true;
    if (linux_reset_named_graphics(&g) || state.p0Writes != 1 || state.globalWrites != 1 ||
        g.graphicsRecovery.covered != GRAPHICS_SCALAR_RECOVERY_ALL ||
        g.graphicsRecovery.uncertain != GRAPHICS_SCALAR_RECOVERY_P0 ||
        g.graphicsRecovery.attempted != GRAPHICS_SCALAR_RECOVERY_ALL) return 9064;
    state.failP0Set = false;
    if (!graphics_scalar_restore(g.graphicsRecovery, snapshot.graphicsPreimage,
            [&](gc_u32 surface, int value) { return nvml_set_clock_offset(&g, NVML_CLOCK_GRAPHICS, value, surface); }) ||
        state.p0Value != 71 || state.globalValue != -243 ||
        g.graphicsRecovery.covered != GRAPHICS_SCALAR_RECOVERY_ALL ||
        g.graphicsRecovery.uncertain != GRAPHICS_SCALAR_RECOVERY_ALL) return 9065;
    if (!linux_reset_named_graphics(&g) || state.p0Value || state.globalValue ||
        g.graphicsRecovery.covered || g.graphicsRecovery.uncertain) return 9066;
    state.failP0Set = true;
    if (graphics_scalar_restore(g.graphicsRecovery, snapshot.graphicsPreimage,
            [&](gc_u32 surface, int value) { return nvml_set_clock_offset(&g, NVML_CLOCK_GRAPHICS, value, surface); }) ||
        g.graphicsRecovery.covered != GRAPHICS_SCALAR_RECOVERY_ALL ||
        g.graphicsRecovery.uncertain != GRAPHICS_SCALAR_RECOVERY_ALL) return 9082;
    initialize(&state, &g); state.p0Value = 13; state.globalValue = 99;
    snapshot = {};
    if (!linux_capture_named_graphics(&g, &snapshot, GRAPHICS_SCALAR_RECOVERY_P0)) return 9067;
    state.failP0Set = true;
    if (linux_write_named_graphics(&g, CLOCK_OFFSET_SURFACE_P0, 45) ||
        g.graphicsRecovery.attempted != GRAPHICS_SCALAR_RECOVERY_P0 ||
        g.graphicsRecovery.uncertain != GRAPHICS_SCALAR_RECOVERY_P0) return 9068;
    state.failP0Set = false;
    if (!graphics_scalar_restore(g.graphicsRecovery, snapshot.graphicsPreimage,
            [&](gc_u32 surface, int value) { return nvml_set_clock_offset(&g, NVML_CLOCK_GRAPHICS, value, surface); }) ||
        state.p0Value != 13 || state.globalValue != 99 || state.globalWrites ||
        g.graphicsRecovery.covered || g.graphicsRecovery.uncertain) return 9069;
    g.graphicsRecovery.attempted = GRAPHICS_SCALAR_RECOVERY_GLOBAL;
    const int writes = state.p0Writes;
    if (graphics_scalar_restore(g.graphicsRecovery, snapshot.graphicsPreimage,
            [&](gc_u32 surface, int value) { return nvml_set_clock_offset(&g, NVML_CLOCK_GRAPHICS, value, surface); }) ||
        state.p0Writes != writes || state.globalWrites) return 9070;
    snapshot = {}; state.failGlobalRead = true;
    const auto before = snapshot;
    if (linux_capture_named_graphics(&g, &snapshot, GRAPHICS_SCALAR_RECOVERY_ALL) ||
        memcmp(&before, &snapshot, sizeof(before))) return 9071;
    initialize(&state, &g);
    g.graphicsRecovery.prepared = 0;
    if (linux_reset_named_graphics(&g) || state.p0Writes || state.globalWrites) return 9072;
    GpuAdapterInfo gpu{}; gpu.valid = gpu.pciInfoValid = 1;
    gpu.pciDomain = 2; gpu.pciBus = 3; gpu.pciDevice = 1;
    g.adapters[0] = g.selectedGpu = gpu; g.adapterCount = 1;
    g.writeIdentityResolved = true;
    LinuxDaemonStateRecord record{};
    linux_daemon_record_initialize(&record, LINUX_DAEMON_RECORD_PREPARED, &gpu, &d,
        0, SERVICE_OPERATION_NONE, GPU_LOCATION_ROLE_REQUIRED, nullptr, GRAPHICS_SCALAR_RECOVERY_ALL);
    if (!linux_daemon_record_valid(&record) ||
        !linux_daemon_record_requires_recovery(record, false) || !linux_daemon_record_requires_recovery(record, true) ||
        linux_graphics_adopt_pending_record(&g, &record, false) || g.graphicsRecovery.covered) return 9073;
    g.adapters[0].pciBus = 4;
    if (linux_graphics_adopt_pending_record(&g, &record, true) || g.graphicsRecovery.covered) return 9074;
    g.adapters[0] = gpu;
    if (!linux_graphics_adopt_pending_record(&g, &record, true) ||
        g.graphicsRecovery.covered != GRAPHICS_SCALAR_RECOVERY_ALL ||
        g.graphicsRecovery.uncertain != GRAPHICS_SCALAR_RECOVERY_ALL ||
        g.graphicsRecovery.prepared || g.graphicsRecovery.ownedSurface) return 9075;
    record.state = LINUX_DAEMON_RECORD_ACTIVE;
    if (!linux_daemon_record_requires_recovery(record, false) || linux_daemon_record_requires_recovery(record, true)) return 9076;
    record.targetUuid = linux_gpu_uuid_parse("GPU-11111111-2222-3333-4444-555555555555");
    g.adapterUuids[0] = linux_gpu_uuid_parse("GPU-aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee");
    if (linux_graphics_adopt_pending_record(&g, &record, true)) return 9077;
    record.graphicsControlSurfaces = 4; record.checksum = linux_daemon_record_checksum(&record);
    if (linux_daemon_record_valid(&record)) return 9078;
    initialize(&state, &g); state.failP0Read = true;
    g.graphicsRecovery.covered = GRAPHICS_SCALAR_RECOVERY_P0;
    g.graphicsRecovery.ownedSurface = CLOCK_OFFSET_SURFACE_P0;
    snapshot = {};
    linux_capture_graphics_recovery_snapshot(&g, &snapshot);
    DesiredSettings power{}; power.hasPowerLimit = 1; power.powerLimitPct = 80; char why[160]{};
    if (linux_graphics_recovery_snapshot_complete(&g, &snapshot) ||
        snapshot.graphicsPreimage.covered != GRAPHICS_SCALAR_RECOVERY_P0 ||
        snapshot.graphicsPreimage.validMask || !linux_graphics_apply_precheck(&g, &power, false, why, sizeof(why)) ||
        linux_graphics_apply_precheck(&g, &power, true, why, sizeof(why)) ||
        state.p0Writes || state.globalWrites) return 9083;
    d.authoring.graphicsSurface = DESIRED_GRAPHICS_P0;
    g.graphicsRecovery.prepared = 0;
    if (linux_graphics_apply_precheck(&g, &d, true, why, sizeof(why)) || state.p0Writes || state.globalWrites) return 9084;
    g.selectedGpu = gpu; g.adapterCount = 1;
    g.adapterUuids[0] = linux_gpu_uuid_parse("GPU-11111111-2222-3333-4444-555555555555");
    LinuxGpuState moved = g; moved.selectedGpu.pciBus++;
    if (!linux_backend_recovery_binding_matches(&g, &moved)) return 9085;
    moved.adapterUuids[0] = linux_gpu_uuid_parse("GPU-aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee");
    moved.selectedGpu = g.selectedGpu;
    if (linux_backend_recovery_binding_matches(&g, &moved)) return 9086;
    moved.adapterUuids[0] = {}; moved.selectedGpu.pciBus++;
    if (linux_backend_recovery_binding_matches(&g, &moved)) return 9087;
    moved.selectedGpu = g.selectedGpu;
    if (!linux_backend_recovery_binding_matches(&g, &moved)) return 9088;
    return 0;
}

static int graphics_control_cases() {
    Driver driverState{}; LinuxGpuState g{}; initialize(&driverState, &g);
    auto& state = driverState;
    DesiredSettings d{}; d.hasGpuOffset = 1; d.gpuOffsetMHz = -247;
    d.authoring.graphicsPrecise = 1; d.authoring.graphicsOffsetHz = -247000000;
    d.authoring.graphicsSurface = DESIRED_GRAPHICS_GLOBAL;
    state.p0Value = 91; state.globalValue = 17;
    LinuxHardwareSnapshot snapshot{}; snapshot.gpuOffsetMHz = 91; snapshot.gpuOffsetValid = true;
    char why[160]{};
    if (!linux_bind_graphics_snapshot(&g, &d, &snapshot, why, sizeof(why)) ||
        !snapshot.gpuOffsetExact || snapshot.gpuOffsetSurface != CLOCK_OFFSET_SURFACE_GLOBAL ||
        snapshot.gpuOffsetMHz != 17 || state.p0Reads || state.globalWrites || state.p0Writes) return 8828;
    if (!linux_apply_named_graphics_offset(&g, &d) || state.globalValue != -247 || state.p0Value != 91 ||
        state.globalWrites != 1 || state.p0Writes || state.p0Reads) return 8829;
    if (!linux_apply_named_graphics_offset(&g, &d) || state.globalWrites != 1) return 8830;
    initialize(&state, &g); d.authoring.graphicsSurface = DESIRED_GRAPHICS_P0;
    state.failP0Set = true;
    if (linux_apply_named_graphics_offset(&g, &d) || state.p0Writes != 1 || state.globalWrites ||
        state.globalReads || state.p0Value != -247) return 8831;
    state.failP0Set = false; state.p0Value = 0; state.snap = true;
    if (linux_apply_named_graphics_offset(&g, &d) || state.p0Writes != 2 || state.globalWrites) return 8832;
    initialize(&state, &g); state.failP0Read = true;
    const auto before = snapshot;
    if (linux_bind_graphics_snapshot(&g, &d, &snapshot, why, sizeof(why)) ||
        memcmp(&before, &snapshot, sizeof(before)) || state.p0Writes || state.globalWrites || state.globalReads) return 8833;
    initialize(&state, &g); state.badMetadata = true;
    if (linux_apply_named_graphics_offset(&g, &d) || state.p0Writes || state.globalWrites || state.globalReads) return 8834;
    initialize(&state, &g); d.authoring.graphicsOffsetHz = -247000500;
    if (graphics_offset_control_plan(d, g.nvml, g.nvmlDevice).status != GRAPHICS_OFFSET_OFF_GRID ||
        linux_apply_named_graphics_offset(&g, &d) || state.p0Writes || state.p0Reads || state.globalWrites) return 8835;
    d.authoring.graphicsOffsetHz = -247000000; state.badRange = true;
    auto plan = graphics_offset_control_plan(d, g.nvml, g.nvmlDevice);
    if (plan.status != GRAPHICS_OFFSET_READY || plan.before.rangeValid || !plan.before.valid ||
        !linux_apply_named_graphics_offset(&g, &d) || state.globalReads || state.globalWrites) return 8836;
    initialize(&state, &g); d.authoring.graphicsSurface = DESIRED_GRAPHICS_GLOBAL;
    g.nvml.getGpcClkMinMaxVfOffset = get_global_range; state.globalRangeFails = true;
    if (!linux_apply_named_graphics_offset(&g, &d) || state.globalWrites != 1 || state.p0Writes) return 8837;
    initialize(&state, &g); g.nvml.setClockOffsets = nullptr; d.authoring.graphicsSurface = DESIRED_GRAPHICS_P0;
    if (linux_apply_named_graphics_offset(&g, &d) || state.globalWrites || state.p0Writes || state.globalReads) return 8838;
    d.authoring.graphicsSurface = DESIRED_GRAPHICS_AUTO;
    if (graphics_offset_control_plan(d, g.nvml, g.nvmlDevice).status != GRAPHICS_OFFSET_INVALID) return 8839;
    d.authoring.graphicsSurface = DESIRED_GRAPHICS_VF_COMPONENT; d.gpuOffsetMHz = 0; d.authoring.graphicsOffsetHz = 500;
    if (graphics_offset_control_plan(d, {}, g.nvmlDevice).status != GRAPHICS_OFFSET_VF_COMPONENT) return 8840;
    initialize(&state, &g); d.gpuOffsetMHz = -247; d.authoring.graphicsOffsetHz = -247000000;
    d.authoring.graphicsSurface = DESIRED_GRAPHICS_P0; state.narrowRange = true;
    if (graphics_offset_control_plan(d, g.nvml, g.nvmlDevice).status != GRAPHICS_OFFSET_OUT_OF_RANGE ||
        linux_apply_named_graphics_offset(&g, &d) || state.p0Writes || state.globalWrites) return 8841;
    state.narrowRange = false; state.failReadAfterWrite = true;
    if (linux_apply_named_graphics_offset(&g, &d) || state.p0Writes != 1 || state.p0Value != -247 ||
        state.globalWrites || state.globalReads) return 8842;
    return 0;
}
}

int run_linux_clock_offset_tests() {
    if (int failure = graphics_recovery_cases()) return failure;
    {
        Driver state{}; LinuxGpuState g{}; initialize(&state, &g);
        g.vfOffsetsHzValid = true;
        DesiredSettings request{}; request.hasGpuOffset = 1; request.gpuOffsetMHz = 100;
        request.authoring.graphicsPrecise = request.authoring.topologyBound = 1;
        request.authoring.topologyCount = 1; request.authoring.voltageUv[0] = 650000;
        request.authoring.graphicsSurface = DESIRED_GRAPHICS_P0; request.authoring.graphicsOffsetHz = 100000000;
        request.curvePointCaptured[0] = 1; request.curvePointCapturedOffsetHz[0] = -500;
        LinuxHardwareSnapshot snapshot{}; snapshot.gpuOffsetValid = snapshot.gpuOffsetExact = true;
        snapshot.gpuOffsetSurface = CLOCK_OFFSET_SURFACE_P0;
        ScalarCurveSubmissionReview review{}; review.installedGuardRequired = true; char why[256]{};
        if (linux_authoring_scalar_transition_precheck(&g, &request, snapshot, review, why, sizeof(why)) || review.installedGuardRequired ||
            state.p0Writes || state.globalWrites || state.capWrites) return 8970;
        request.hasLock = 1; request.lockMode = LOCK_MODE_FLATTEN; request.lockMHz = 1000;
        if (linux_authoring_scalar_transition_precheck(&g, &request, snapshot, review, why, sizeof(why)) || review.installedGuardRequired ||
            state.p0Writes || state.globalWrites || state.capWrites) return 8971;
        g.nvml.setGpuLockedClocks = scalar_guard_set; g.nvml.resetGpuLockedClocks = scalar_guard_reset;
        if (!linux_authoring_scalar_transition_precheck(&g, &request, snapshot, review, why, sizeof(why)) || !review.installedGuardRequired ||
            state.p0Writes || state.globalWrites || state.capWrites) return 8972;
        snapshot.gpuOffsetExact = false;
        if (linux_authoring_scalar_transition_precheck(&g, &request, snapshot, review, why, sizeof(why)) || review.installedGuardRequired ||
            state.p0Writes || state.globalWrites || state.capWrites) return 8973;
        snapshot.gpuOffsetExact = true;
        if (!linux_authoring_scalar_transition_precheck(&g, &request, snapshot, review, why, sizeof(why))) return 8977;
        state.p0Value = -100;
        if (linux_apply_named_graphics_offset(&g, &request, &review) || !review.refusedWithoutWrite ||
            state.p0Writes || state.globalWrites || state.capWrites) return 8978;
        struct Context { LinuxGpuState* gpu; DesiredSettings* desired; ScalarCurveSubmissionReview* review; int rollbacks; };
        Context context{&g, &request, &review, 0};
        const auto step = [](void* raw, unsigned phase) {
            auto& c = *static_cast<Context*>(raw);
            return phase == LINUX_MUTATION_LOCK_CEILING
                ? c.gpu->nvml.setGpuLockedClocks(c.gpu->nvmlDevice, 0, 1000) == NVML_SUCCESS
                : linux_apply_named_graphics_offset(c.gpu, c.desired, c.review);
        };
        const auto rollback = [](void* raw, unsigned phases) {
            auto& c = *static_cast<Context*>(raw);
            phases = scalar_curve_rollback_phases(*c.review, phases, LINUX_MUTATION_GPU_OFFSET);
            if (!phases) return true;
            ++c.rollbacks;
            return phases == LINUX_MUTATION_LOCK_CEILING &&
                c.gpu->nvml.resetGpuLockedClocks(c.gpu->nvmlDevice) == NVML_SUCCESS;
        };
        auto mutation = linux_execute_transaction(LINUX_MUTATION_GPU_OFFSET, step, rollback, &context);
        scalar_curve_finalize_write_evidence(review, LINUX_MUTATION_GPU_OFFSET, mutation);
        if (mutation.success || mutation.anyWrite || mutation.rollbackAttempted || context.rollbacks ||
            state.p0Writes || state.globalWrites || state.capWrites) return 8979;
        mutation = linux_execute_transaction(LINUX_MUTATION_LOCK_CEILING | LINUX_MUTATION_GPU_OFFSET, step, rollback, &context);
        scalar_curve_finalize_write_evidence(review, LINUX_MUTATION_GPU_OFFSET, mutation);
        if (mutation.success || !mutation.anyWrite || !mutation.rollbackSucceeded || context.rollbacks != 1 ||
            state.p0Writes || state.globalWrites || state.capWrites != 2) return 8980;
        snapshot.gpuOffsetSurface = CLOCK_OFFSET_SURFACE_GLOBAL;
        if (linux_authoring_scalar_transition_precheck(&g, &request, snapshot, review, why, sizeof(why)) ||
            review.installedGuardRequired || review.beforeValid || state.p0Writes || state.globalWrites || state.capWrites != 2) return 8982;
    }
    if (int failure = graphics_control_cases()) return failure;
    Driver d{}; LinuxGpuState g{};
    initialize(&d, &g); d.snap = true;
    if (!nvml_set_clock_offset(&g, NVML_CLOCK_MEM, 247) || d.p0Value != 246) return 8150;
    initialize(&d, &g); d.snap = true;
    if (nvml_set_clock_offset(&g, NVML_CLOCK_MEM, 247, CLOCK_OFFSET_SURFACE_P0) ||
        d.p0Writes != 1 || d.globalWrites || d.p0Value != 246) return 8151;
    initialize(&d, &g);
    if (!nvml_set_clock_offset(&g, NVML_CLOCK_MEM, 247, CLOCK_OFFSET_SURFACE_P0) ||
        d.p0Value != 247 || !d.layoutValid || d.globalReads || d.globalWrites) return 8152;
    initialize(&d, &g); d.failP0Read = true; d.globalValue = 247;
    if (nvml_set_clock_offset(&g, NVML_CLOCK_MEM, 247, CLOCK_OFFSET_SURFACE_P0) ||
        d.p0Writes != 1 || d.p0Reads != 1 || d.globalReads || d.globalWrites) return 8153;
    initialize(&d, &g); d.failP0Set = true;
    if (nvml_set_clock_offset(&g, NVML_CLOCK_MEM, 247, CLOCK_OFFSET_SURFACE_P0) ||
        d.p0Writes != 1 || d.globalWrites || d.p0Value != 247) return 8154;
    initialize(&d, &g); d.p0Value = 999;
    if (!nvml_set_clock_offset(&g, NVML_CLOCK_MEM, -247, CLOCK_OFFSET_SURFACE_GLOBAL) ||
        d.globalValue != -247 || d.p0Value != 999 || d.p0Writes || d.p0Reads) return 8155;
    initialize(&d, &g); d.snap = true;
    if (nvml_set_clock_offset(&g, NVML_CLOCK_MEM, -247, CLOCK_OFFSET_SURFACE_GLOBAL) ||
        d.globalWrites != 1 || d.globalValue != -246) return 8156;
    initialize(&d, &g); d.failGlobalRead = true;
    if (nvml_set_clock_offset(&g, NVML_CLOCK_MEM, 0, CLOCK_OFFSET_SURFACE_GLOBAL) ||
        d.globalWrites != 1) return 8157;
    initialize(&d, &g); g.nvml.getMemClkVfOffset = nullptr;
    if (nvml_set_clock_offset(&g, NVML_CLOCK_MEM, 247, CLOCK_OFFSET_SURFACE_GLOBAL) ||
        d.p0Writes || d.globalWrites) return 8158;
    initialize(&d, &g); g.nvml.setClockOffsets = nullptr;
    if (nvml_set_clock_offset(&g, NVML_CLOCK_MEM, 247, CLOCK_OFFSET_SURFACE_P0) ||
        d.p0Writes || d.globalWrites) return 8159;
    initialize(&d, &g); d.badMetadata = true;
    if (nvml_set_clock_offset(&g, NVML_CLOCK_MEM, 247, CLOCK_OFFSET_SURFACE_P0) ||
        d.p0Writes != 1 || d.globalReads) return 8160;
    d.globalValue = 77;
    const NvmlClockOffsetReadback fallback = nvml_read_clock_offset(&g, NVML_CLOCK_MEM);
    if (!fallback.offsetValid || fallback.modernApi || fallback.offsetMHz != 77 || d.globalReads != 1) return 8161;
    initialize(&d, &g);
    if (nvml_set_clock_offset(&g, NVML_CLOCK_MEM, 247, 99) ||
        d.p0Writes || d.globalWrites) return 8162;
    if (!nvml_set_clock_offset(&g, NVML_CLOCK_MEM, 0, CLOCK_OFFSET_SURFACE_P0) ||
        d.p0Value || d.p0Writes != 1) return 8163;
    initialize(&d, &g); d.p0Value = 247;
    auto read = nvml_clock_observe(g.nvml, g.nvmlDevice, NVML_CLOCK_MEM);
    if (!read.control.valid || !read.control.rangeValid || read.control.value != 247 ||
        read.surface != CLOCK_OFFSET_SURFACE_P0 || d.globalReads) return 8265;
    initialize(&d, &g); d.failP0Read = true; d.globalValue = -247;
    read = nvml_clock_observe(g.nvml, g.nvmlDevice, NVML_CLOCK_MEM);
    if (!read.control.valid || read.control.value != -247 || read.control.rangeValid ||
        read.control.rangeStatus != NVML_ERROR_NOT_SUPPORTED || read.surface != CLOCK_OFFSET_SURFACE_GLOBAL) return 8266;
    const NvmlClockOffsetReadback controlOnly = nvml_read_clock_offset(&g, NVML_CLOCK_MEM);
    if (!controlOnly.offsetValid || controlOnly.rangeValid || controlOnly.offsetMHz != -247) return 8267;
    initialize(&d, &g); d.failP0Read = d.failGlobalRead = true;
    g.nvml.getMemClkMinMaxVfOffset = get_global_range;
    read = nvml_clock_observe(g.nvml, g.nvmlDevice, NVML_CLOCK_MEM);
    if (read.control.valid || !read.control.rangeValid || read.control.rangeStatus != NVML_SUCCESS ||
        read.control.status != NVML_ERROR_NOT_SUPPORTED) return 8268;
    initialize(&d, &g); d.badRange = true; d.p0Value = 247; d.globalValue = 88;
    g.nvml.getMemClkMinMaxVfOffset = get_global_range;
    read = nvml_clock_observe(g.nvml, g.nvmlDevice, NVML_CLOCK_MEM);
    if (!read.control.valid || read.control.rangeValid || read.control.value != 247 ||
        d.globalReads || read.control.rangeStatus != NVML_SUCCESS) return 8269;
    read = nvml_clock_observe(g.nvml, g.nvmlDevice, NVML_CLOCK_MEM, true);
    if (!read.control.valid || !read.control.rangeValid || read.control.value != 88 ||
        read.surface != CLOCK_OFFSET_SURFACE_GLOBAL) return 8270;
    initialize(&d, &g); d.failP0Read = d.globalRangeFails = true; d.globalValue = 247;
    g.nvml.getMemClkMinMaxVfOffset = get_global_range;
    read = nvml_clock_observe(g.nvml, g.nvmlDevice, NVML_CLOCK_MEM);
    if (!read.control.valid || read.control.rangeValid || read.control.value != 247 ||
        read.control.rangeStatus != NVML_ERROR_UNKNOWN || read.control.status != NVML_SUCCESS) return 8271;
    return 0;
}
