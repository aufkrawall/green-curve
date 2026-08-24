// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT

#include "linux_tui_internal.h"

#include "linux_debug_log.h"
#include "linux_gpu_selection.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

namespace {

int clamp_int(int value, int minimum, int maximum) {
    if (value < minimum) return minimum;
    if (value > maximum) return maximum;
    return value;
}

const GpuAdapterInfo* selected_adapter(const ServiceResponse& response) {
    return linux_tui_authoritative_gpu(&response);
}

#include "linux_tui_desired_runtime.cpp"

int useful_initial_point(const ServiceResponse& response,
                         const DesiredSettings& desired) {
    if (desired.hasLock && desired.lockCi >= 0 && desired.lockCi < VF_NUM_POINTS &&
        response.snapshot.curve[desired.lockCi].freq_kHz != 0) return desired.lockCi;
    int best = -1;
    unsigned int bestDistance = 0;
    for (int i = 0; i < VF_NUM_POINTS; ++i) {
        if (response.snapshot.curve[i].freq_kHz == 0) continue;
        unsigned int mv = response.snapshot.curve[i].volt_uV / 1000u;
        unsigned int distance = mv > 925 ? mv - 925 : 925 - mv;
        if (best < 0 || distance < bestDistance) {
            best = i;
            bestDistance = distance;
        }
    }
    return best >= 0 ? best : 0;
}

#include "linux_tui_authority_runtime.cpp"

TuiViewModel view_for_state(const TuiState& state) {
    TuiViewModel vm = {};
    vm.desired = &state.desired;
    vm.service = state.serviceOnline ? &state.service : nullptr;
    vm.currentSlot = state.currentSlot;
    vm.tab = state.tab;
    vm.selectedPoint = state.selectedPoint;
    vm.vfScroll = state.vfScroll;
    vm.fanScroll = state.fanScroll;
    vm.serviceOnline = state.serviceOnline;
    vm.draftAttached = state.draftAttached;
    if (state.serviceOnline)
        vm.intentReadback = compare_intent_to_readback(&state.service);
    return vm;
}

int current_field_value(const TuiState& state, TuiField field, int index) {
    switch (field) {
        case TUI_FIELD_GPU_OFFSET: return state.desired.gpuOffsetMHz;
        case TUI_FIELD_EXCLUDED_POINTS:
            return state.desired.gpuOffsetExcludeLowCount;
        case TUI_FIELD_MEMORY_OFFSET: return state.desired.memOffsetMHz;
        case TUI_FIELD_POWER_LIMIT: return state.desired.powerLimitPct;
        case TUI_FIELD_FAN_FIXED: return state.desired.fanPercent;
        case TUI_FIELD_FAN_POLL: return state.desired.fanCurve.pollIntervalMs;
        case TUI_FIELD_FAN_HYSTERESIS:
            return state.desired.fanCurve.hysteresisC;
        case TUI_FIELD_FAN_ZERO_RPM_HYSTERESIS:
            return state.desired.fanCurve.zeroRpmHysteresisC;
        case TUI_FIELD_FAN_TEMPERATURE:
            return index >= 0 && index < FAN_CURVE_MAX_POINTS
                ? state.desired.fanCurve.points[index].temperatureC : 0;
        case TUI_FIELD_FAN_PERCENT:
            return index >= 0 && index < FAN_CURVE_MAX_POINTS
                ? state.desired.fanCurve.points[index].fanPercent : 0;
        case TUI_FIELD_XBAR_OFFSET:
            return state.desired.xbarOffsetKhz / 1000;
        case TUI_FIELD_XBAR_MSVDD:
            return state.desired.xbarMsvddOffsetUv / 1000;
        case TUI_FIELD_SYS_CLK_OFFSET:
            return state.desired.sysClkOffsetKhz / 1000;
        case TUI_FIELD_VIDEO_CLK_OFFSET:
            return state.desired.videoClkOffsetKhz / 1000;
        case TUI_FIELD_VF_TARGET: {
            TuiViewModel vm = view_for_state(state);
            return tui_point_values(vm, index).targetMHz;
        }
        default: return 0;
    }
}

int previous_enabled_fan_point(const DesiredSettings& desired, int index) {
    for (int i = index - 1; i >= 0; --i)
        if (desired.fanCurve.points[i].enabled) return i;
    return -1;
}

int next_enabled_fan_point(const DesiredSettings& desired, int index) {
    for (int i = index + 1; i < FAN_CURVE_MAX_POINTS; ++i)
        if (desired.fanCurve.points[i].enabled) return i;
    return -1;
}

void set_field_value(TuiState* state, TuiField field, int index, int value) {
    if (!field_edit_available(state, field)) return;
    DesiredSettings& desired = state->desired;
    DesiredSettings previousSettings = desired;
    switch (field) {
        case TUI_FIELD_GPU_OFFSET:
            desired.hasGpuOffset = true;
            desired.gpuOffsetMHz = clamp_int(value, -1000, 1000);
            break;
        case TUI_FIELD_EXCLUDED_POINTS:
            desired.hasGpuOffset = true;
            desired.gpuOffsetExcludeLowCount = clamp_int(value, 0,
                state->serviceOnline ? state->service.snapshot.numPopulated
                                     : VF_NUM_POINTS);
            break;
        case TUI_FIELD_MEMORY_OFFSET:
            desired.hasMemOffset = true;
            desired.memOffsetMHz = clamp_int(value, -5000, 5000);
            break;
        case TUI_FIELD_POWER_LIMIT:
            desired.hasPowerLimit = true;
            desired.powerLimitPct = clamp_power_limit_pct(value);
            break;
        case TUI_FIELD_XBAR_OFFSET:
            desired.hasXbarOffsetKhz = true;
            desired.xbarOffsetKhz = clamp_int(value, -1000, 1000) * 1000;
            break;
        case TUI_FIELD_XBAR_MSVDD:
            desired.hasXbarMsvddOffsetUv = true;
            desired.xbarMsvddOffsetUv = clamp_int(value, -100, 100) * 1000;
            break;
        case TUI_FIELD_SYS_CLK_OFFSET:
            desired.hasSysClkOffsetKhz = true;
            desired.sysClkOffsetKhz = clamp_int(value, -1000, 1000) * 1000;
            break;
        case TUI_FIELD_VIDEO_CLK_OFFSET:
            desired.hasVideoClkOffsetKhz = true;
            desired.videoClkOffsetKhz = clamp_int(value, -1000, 1000) * 1000;
            break;
        case TUI_FIELD_VF_TARGET: {
            if (index < 0 || index >= VF_NUM_POINTS) return;
            int target = clamp_int(value, 1, 5000);
            TuiViewModel vm = view_for_state(*state);
            for (int i = index - 1; i >= 0; --i) {
                TuiPointValues previous = tui_point_values(vm, i);
                if (!previous.populated) continue;
                if (target < previous.targetMHz) target = previous.targetMHz;
                break;
            }
            for (int i = index + 1; i < VF_NUM_POINTS; ++i) {
                TuiPointValues next = tui_point_values(vm, i);
                if (!next.populated) continue;
                if (target > next.targetMHz &&
                    !(desired.hasLock && index >= desired.lockCi))
                    target = next.targetMHz;
                break;
            }
            if (desired.hasLock && index >= desired.lockCi) {
                desired.lockMHz = (unsigned int)target;
            } else {
                desired.hasCurvePoint[index] = true;
                desired.curvePointMHz[index] = (unsigned int)target;
            }
            state->selectedPoint = index;
            break;
        }
        case TUI_FIELD_FAN_FIXED:
            desired.hasFan = true;
            desired.fanMode = FAN_MODE_FIXED;
            desired.fanAuto = false;
            desired.fanPercent = clamp_int(value, 0, 100);
            break;
        case TUI_FIELD_FAN_POLL:
            desired.hasFan = true;
            desired.fanMode = FAN_MODE_CURVE;
            desired.fanCurve.pollIntervalMs =
                clamp_int(((value + 125) / 250) * 250, 250, 5000);
            break;
        case TUI_FIELD_FAN_HYSTERESIS:
            desired.hasFan = true;
            desired.fanMode = FAN_MODE_CURVE;
            desired.fanCurve.hysteresisC = clamp_int(
                value, 0, FAN_CURVE_MAX_HYSTERESIS_C);
            break;
        case TUI_FIELD_FAN_ZERO_RPM_HYSTERESIS:
            desired.hasFan = true;
            desired.fanMode = FAN_MODE_CURVE;
            desired.fanCurve.zeroRpmHysteresisC = (gc_u8)clamp_int(
                value, FAN_ZERO_RPM_MIN_HYSTERESIS_C,
                FAN_ZERO_RPM_MAX_HYSTERESIS_C);
            break;
        case TUI_FIELD_FAN_TEMPERATURE: {
            if (index < 0 || index >= FAN_CURVE_MAX_POINTS) return;
            desired.hasFan = true;
            desired.fanMode = FAN_MODE_CURVE;
            desired.fanCurve.points[index].enabled = true;
            int minimum = 0, maximum = 100;
            int previous = previous_enabled_fan_point(desired, index);
            int next = next_enabled_fan_point(desired, index);
            if (previous >= 0)
                minimum = desired.fanCurve.points[previous].temperatureC + 1;
            if (next >= 0)
                maximum = desired.fanCurve.points[next].temperatureC - 1;
            desired.fanCurve.points[index].temperatureC =
                clamp_int(value, minimum, maximum);
            break;
        }
        case TUI_FIELD_FAN_PERCENT: {
            if (index < 0 || index >= FAN_CURVE_MAX_POINTS) return;
            desired.hasFan = true;
            desired.fanMode = FAN_MODE_CURVE;
            desired.fanCurve.points[index].enabled = true;
            int minimum = 0, maximum = 100;
            int previous = previous_enabled_fan_point(desired, index);
            int next = next_enabled_fan_point(desired, index);
            if (previous >= 0)
                minimum = desired.fanCurve.points[previous].fanPercent;
            if (next >= 0)
                maximum = desired.fanCurve.points[next].fanPercent;
            desired.fanCurve.points[index].fanPercent =
                clamp_int(value, minimum, maximum);
            break;
        }
        default:
            return;
    }
    if (state->serviceOnline) {
        gc_u32 domains = service_desired_mutation_domains(&desired);
        gc_u32 missing = domains &
            ~state->service.snapshot.health.availableMutationDomains;
        if (missing != 0) {
            desired = previousSettings;
            snprintf(state->status, sizeof(state->status),
                "Edit would create a mixed unavailable request (missing domains=0x%02x); no value was staged",
                missing);
            return;
        }
    }
    tui_recompute_dirty(state);
    bind_current_draft(state);
    snprintf(state->status, sizeof(state->status),
             "Staged %s value: %d",
             field == TUI_FIELD_VF_TARGET ? "absolute VF target" : "control",
             current_field_value(*state, field, index));
}

void apply_lock_action(TuiState* state, int index, int requestedMode) {
    if (!state || !vf_editing_available(*state)) {
        if (state && state->serviceOnline)
            set_gpu_health_status(state, "VF tail editing disabled");
        else if (state)
            snprintf(state->status, sizeof(state->status),
                "VF tail editing disabled: daemon/topology unavailable");
        return;
    }
    if (index < 0 || index >= VF_NUM_POINTS) index = state->selectedPoint;
    TuiViewModel vm = view_for_state(*state);
    TuiPointValues values = tui_point_values(vm, index);
    if (!values.populated) {
        snprintf(state->status, sizeof(state->status),
                 "Select a populated VF point first");
        return;
    }
    int mode = requestedMode;
    if (requestedMode == 3) {
        if (!state->desired.hasLock || state->desired.lockCi != index)
            mode = LOCK_MODE_FLATTEN;
        else if (state->desired.lockMode == LOCK_MODE_FLATTEN)
            mode = LOCK_MODE_HARD;
        else
            mode = LOCK_MODE_NONE;
    }
    if (mode == LOCK_MODE_NONE) {
        state->desired.hasLock = false;
        state->desired.lockCi = -1;
        state->desired.lockMHz = 0;
        state->desired.lockMode = LOCK_MODE_NONE;
    } else {
        state->desired.hasLock = true;
        state->desired.lockCi = index;
        state->desired.lockMHz = (unsigned int)values.targetMHz;
        state->desired.lockMode = (LockMode)mode;
        state->desired.lockTracksAnchor = true;
        state->selectedPoint = index;
    }
    tui_recompute_dirty(state);
    bind_current_draft(state);
    snprintf(state->status, sizeof(state->status), "VF point #%d tail mode: %s",
             index, mode == LOCK_MODE_FLATTEN ? "flatten" :
             mode == LOCK_MODE_HARD ? "hard pin" : "off");
}

void set_daemon_failure(TuiState* state, const char* fallback,
                        const char* detail) {
    linux_tui_format_failure(state->status, sizeof(state->status),
                             fallback, detail);
}

void apply_to_gpu(TuiState* state) {
    if (!state->serviceOnline) {
        snprintf(state->status, sizeof(state->status),
                 "Apply blocked: daemon is offline");
        return;
    }
    if (!state->draftAttached ||
        !linux_tui_draft_binding_matches(&state->draftBinding,
                                         &state->service,
                                         &state->desired)) {
        state->draftAttached = false;
        snprintf(state->status, sizeof(state->status),
                 "Apply blocked: this draft is detached from the current daemon/GPU generation; reload or restage it after review");
        return;
    }
    DesiredSettings normalized = state->desired;
    if (normalized.hasFan && normalized.fanMode == FAN_MODE_CURVE)
        fan_curve_normalize(&normalized.fanCurve);
    gc_u32 requestedDomains = service_desired_mutation_domains(&normalized);
    gc_u32 missingDomains = requestedDomains &
        ~state->service.snapshot.health.availableMutationDomains;
    if (requestedDomains == 0 || missingDomains != 0) {
        snprintf(state->status, sizeof(state->status),
            requestedDomains == 0
                ? "Apply blocked: the draft contains no GPU mutation"
                : "Apply blocked before write: requested domains=0x%02x available=0x%02x missing=0x%02x",
            requestedDomains,
            state->service.snapshot.health.availableMutationDomains,
            missingDomains);
        return;
    }
    char fanError[160] = {};
    if (normalized.hasFan && normalized.fanMode == FAN_MODE_CURVE &&
        !fan_curve_validate(&normalized.fanCurve, fanError, sizeof(fanError))) {
        set_daemon_failure(state, "Fan curve is invalid", fanError);
        return;
    }
    snprintf(state->status, sizeof(state->status), "Applying staged settings...");
    tui_render(state);
    ServiceResponse response = {};
    char result[512] = {};
    bool ok = linux_daemon_apply_checked(
        state->targetGpu.valid ? &state->targetGpu : nullptr,
        &normalized, true, &state->service.state, &response,
        result, sizeof(result));
    if (!ok) {
        set_daemon_failure(state, "Apply failed", result);
        if (response.status == SERVICE_STATUS_STALE_STATE)
            tui_refresh_service(state, false);
        return;
    }
    state->service = response;
    state->serviceOnline = true;
    desired_from_live_response(response, &state->desired);
    state->acceptedDesired = state->desired;
    bind_current_draft(state);
    state->dirty = false;
    snprintf(state->status, sizeof(state->status), "%s",
             result[0] ? result : "Applied staged settings");
}

void reset_gpu(TuiState* state) {
    if (!state->serviceOnline ||
        state->service.state.gpuPhase != SERVICE_GPU_PHASE_READY ||
        (state->service.snapshot.health.availableMutationDomains &
            SERVICE_MUTATION_DOMAIN_ALL) != SERVICE_MUTATION_DOMAIN_ALL) {
        snprintf(state->status, sizeof(state->status),
                 "Reset blocked: full atomic Reset requires READY and every mutation domain (available=0x%02x)",
                 state->service.snapshot.health.availableMutationDomains);
        return;
    }
    snprintf(state->status, sizeof(state->status), "Resetting GPU controls...");
    tui_render(state);
    ServiceResponse response = {};
    char result[512] = {};
    bool ok = linux_daemon_reset_checked(
        state->targetGpu.valid ? &state->targetGpu : nullptr,
        &state->service.state, &response, result, sizeof(result));
    if (!ok) {
        set_daemon_failure(state, "GPU reset failed", result);
        if (response.status == SERVICE_STATUS_STALE_STATE)
            tui_refresh_service(state, false);
        return;
    }
    state->service = response;
    desired_from_live_response(response, &state->desired);
    state->acceptedDesired = state->desired;
    bind_current_draft(state);
    state->dirty = false;
    snprintf(state->status, sizeof(state->status), "%s",
             result[0] ? result : "GPU reset to driver defaults");
}

// Cycle the daemon's boot-apply policy.  Choosing "profile N" snapshots the
// *saved* profile, never the unsaved draft: what gets written at the next boot
// has to be something the user can read back out of config.ini.
void apply_startup_policy_cycle(TuiState* state) {
    if (!state->serviceOnline) {
        snprintf(state->status, sizeof(state->status),
                 "Startup policy requires a reachable daemon");
        return;
    }
    unsigned int current = state->service.state.startupPolicyMode;
    unsigned int next =
        current == SERVICE_STARTUP_POLICY_RESTORE_LAST ? SERVICE_STARTUP_POLICY_NONE
        : current == SERVICE_STARTUP_POLICY_NONE ? SERVICE_STARTUP_POLICY_PROFILE
        : SERVICE_STARTUP_POLICY_RESTORE_LAST;

    char result[512] = {};
    bool ok = false;
    if (next == SERVICE_STARTUP_POLICY_PROFILE) {
        if (!state->targetGpu.valid) {
            snprintf(state->status, sizeof(state->status),
                     "Select a GPU before binding a startup profile to it");
            return;
        }
        DesiredSettings saved = {};
        char error[256] = {};
        if (!load_profile_from_config_path(state->configPath,
                                           state->currentSlot, &saved,
                                           error, sizeof(error))) {
            set_daemon_failure(state, "Startup profile load failed", error);
            return;
        }
        normalize_desired_settings_for_ui(&saved);
        char label[64] = {};
        snprintf(label, sizeof(label), "profile %d", state->currentSlot);
        ok = linux_daemon_set_startup_policy(next, state->currentSlot, label,
                                             &state->targetGpu, &saved,
                                             result, sizeof(result));
    } else {
        ok = linux_daemon_set_startup_policy(next, 0, nullptr, nullptr, nullptr,
                                             result, sizeof(result));
    }
    if (!ok) {
        set_daemon_failure(state, "Startup policy update failed", result);
        return;
    }
    // Re-read rather than assume: the envelope the daemon publishes is the only
    // authority for what the control now shows.
    tui_refresh_service(state, false);
    tui_refresh_startup_snapshot(state);
    snprintf(state->status, sizeof(state->status), "%s",
             result[0] ? result : "Startup policy updated");
}

// A profile write is only half the job while the daemon boot-applies that same
// slot: the daemon holds a snapshot of it and cannot read config.ini itself, so
// an un-pushed edit silently reverts at the next boot.  Called after every path
// that changes the *content* of a slot; it never re-binds the policy.
void sync_startup_snapshot_after_profile_write(TuiState* state, int slot,
                                               bool slotStillHasContent) {
    if (!state) return;
    LinuxStartupSyncResult sync = linux_startup_sync_after_profile_write(
        state->configPath, slot, slotStillHasContent, state->serviceOnline,
        state->serviceOnline ? state->service.state.startupPolicyMode
                             : (unsigned int)SERVICE_STARTUP_POLICY_RESTORE_LAST,
        state->serviceOnline ? state->service.state.startupPolicySlot : 0u);
    if (sync.action == STARTUP_SNAPSHOT_SYNC_NONE) return;
    size_t used = strlen(state->status);
    if (used + 1 < sizeof(state->status)) {
        snprintf(state->status + used, sizeof(state->status) - used, " • %s",
                 sync.message);
    }
    if (sync.action != STARTUP_SNAPSHOT_SYNC_UNREACHABLE) {
        tui_refresh_service(state, false);
        tui_refresh_startup_snapshot(state);
    }
}

void export_live(TuiState* state, bool json) {
    if (!state->serviceOnline) {
        snprintf(state->status, sizeof(state->status),
                 "Live export requires a daemon snapshot");
        return;
    }
    char path[LINUX_PATH_MAX] = {};
    const char* slash = strrchr(state->configPath, '/');
    int directoryLength = slash ? (int)(slash - state->configPath) : 0;
    if (directoryLength > 0) {
        snprintf(path, sizeof(path), "%.*s/%s", directoryLength,
                 state->configPath,
                 json ? "greencurve-live.json" : "greencurve-live.txt");
    } else {
        snprintf(path, sizeof(path), "%s",
                 json ? "greencurve-live.json" : "greencurve-live.txt");
    }
    FILE* file = fopen(path, "wb");
    if (!file) {
        snprintf(state->status, sizeof(state->status),
                 "Cannot create live export: %s", path);
        return;
    }
    if (json) print_linux_live_state_json(file, &state->service);
    else print_linux_live_state_text(file, &state->service);
    bool flushed = fflush(file) == 0;
    bool closed = fclose(file) == 0;
    bool ok = flushed && closed;
    snprintf(state->status, sizeof(state->status), "%s: %s",
             ok ? "Live VF export written" : "Live VF export failed", path);
}

}  // namespace

unsigned long long tui_monotonic_ms() {
    struct timespec now = {};
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return 0;
    return (unsigned long long)now.tv_sec * 1000ULL +
           (unsigned long long)now.tv_nsec / 1000000ULL;
}

void tui_recompute_dirty(TuiState* state) {
    if (!state) return;
    state->dirty = !desired_settings_equal(
        &state->desired, &state->acceptedDesired);
}

#include "linux_tui_refresh.cpp"

void tui_begin_edit(TuiState* state, TuiField field, int index) {
    if (!state || field == TUI_FIELD_NONE) return;
    if (!field_edit_available(state, field)) return;
    state->edit.active = true;
    state->edit.field = field;
    state->edit.index = index;
    snprintf(state->edit.text, sizeof(state->edit.text), "%d",
             current_field_value(*state, field, index));
    // Opening a field selects its contents, so the first digit typed replaces
    // the value rather than being appended to it.  A second click into the same
    // field drops the selection; see linux_tui_edit_policy.h.
    state->edit.selectAll = true;
    snprintf(state->status, sizeof(state->status),
             "Editing numeric value • typing replaces • click again to append • Enter commits • Esc cancels");
}

void tui_commit_edit(TuiState* state) {
    if (!state || !state->edit.active) return;
    int value = 0;
    if (!parse_int_strict(state->edit.text, &value)) {
        snprintf(state->status, sizeof(state->status),
                 "Invalid numeric value; edit remains active");
        return;
    }
    TuiField field = state->edit.field;
    int index = state->edit.index;
    state->edit = TuiEditState{};
    set_field_value(state, field, index, value);
}

void tui_cancel_edit(TuiState* state) {
    if (!state) return;
    state->edit = TuiEditState{};
    snprintf(state->status, sizeof(state->status), "Edit cancelled");
}

void tui_handle_character(TuiState* state, char character) {
    if (!state) return;
    if (state->edit.active) {
        tui_edit_insert_character(state->edit.text, sizeof(state->edit.text),
                                  &state->edit.selectAll, character);
        return;
    }
    switch (character) {
        case 'q': case 'Q': state->running = false; break;
        case 'g': case 'G': apply_to_gpu(state); break;
        case 's': case 'S':
            tui_apply_action(state, ClickAction{0,0,0,0,ACTION_SAVE,0,0,0});
            break;
        case 'l': case 'L':
            tui_apply_action(state, ClickAction{0,0,0,0,ACTION_LOAD,0,0,0});
            break;
        case 'r': case 'R':
            tui_apply_action(state, ClickAction{0,0,0,0,ACTION_REFRESH,0,0,0});
            break;
        case '1': case '2': case '3': case '4': case '5':
            state->currentSlot = character - '0';
            snprintf(state->status, sizeof(state->status),
                     "Selected profile slot %d; press Load to stage it",
                     state->currentSlot);
            break;
        default: break;
    }
}

void tui_apply_action(TuiState* state, const ClickAction& action) {
    if (!state) return;
    switch (action.type) {
        case ACTION_QUIT:
            state->running = false;
            break;
        case ACTION_TAB_SET:
            state->tab = (TuiTab)clamp_int(action.value,
                                           TUI_TAB_VF, TUI_TAB_ADVANCED);
            state->focusIndex = -1;
            break;
        case ACTION_GPU_SELECT_DELTA: {
            if (state->dirty) {
                snprintf(state->status, sizeof(state->status),
                         "Reset or save staged changes before switching GPUs");
                break;
            }
            unsigned int count = state->service.snapshot.adapterCount;
            if (!state->serviceOnline || count == 0) {
                snprintf(state->status, sizeof(state->status),
                         "No daemon GPU list is available");
                break;
            }
            bool hasSelectedTarget =
                (state->service.state.validSections &
                    SERVICE_STATE_SECTION_ADAPTER_IDENTITY) != 0;
            int next = linux_next_gpu_selection_index(
                hasSelectedTarget,
                state->service.snapshot.selectedAdapterIndex,
                count, action.value);
            if (next < 0) {
                snprintf(state->status, sizeof(state->status),
                         "GPU selection state is invalid; refresh the daemon");
                break;
            }
            tui_refresh_service(state, true,
                &state->service.snapshot.adapters[next]);
            break;
        }
        case ACTION_REFRESH:
            tui_refresh_service(state, true);
            break;
        case ACTION_APPLY:
            apply_to_gpu(state);
            break;
        case ACTION_APPLY_RESET:
            reset_gpu(state);
            break;
        case ACTION_FIELD_EDIT:
            tui_begin_edit(state, (TuiField)action.index, action.value);
            break;
        case ACTION_FIELD_STEP:
            set_field_value(state, (TuiField)action.index, action.context,
                current_field_value(*state, (TuiField)action.index,
                                    action.context) + action.value);
            break;
        case ACTION_LOCK_CYCLE:
            apply_lock_action(state, action.index, action.value);
            break;
        case ACTION_VF_SELECT:
            if (action.index >= 0) state->selectedPoint = action.index;
            break;
        case ACTION_VF_SCROLL:
            state->vfScroll = tui_clamp_vf_scroll(*state,
                state->vfScroll + action.value);
            break;
        case ACTION_FAN_MODE_SET:
            if (state->serviceOnline &&
                (state->service.snapshot.health.availableMutationDomains &
                    SERVICE_MUTATION_DOMAIN_FAN) == 0) {
                set_gpu_health_status(state, "Fan editing disabled");
                break;
            }
            state->desired.hasFan = true;
            state->desired.fanMode = action.value;
            state->desired.fanAuto = action.value == FAN_MODE_AUTO;
            tui_recompute_dirty(state);
            bind_current_draft(state);
            snprintf(state->status, sizeof(state->status), "Fan mode: %s",
                     fan_mode_label(action.value));
            break;
        case ACTION_FAN_POINT_ENABLE:
            if (action.index >= 0 && action.index < FAN_CURVE_MAX_POINTS) {
                if (state->serviceOnline &&
                    (state->service.snapshot.health.availableMutationDomains &
                        SERVICE_MUTATION_DOMAIN_FAN) == 0) {
                    set_gpu_health_status(state, "Fan editing disabled");
                    break;
                }
                state->desired.hasFan = true;
                state->desired.fanMode = FAN_MODE_CURVE;
                state->desired.fanCurve.points[action.index].enabled =
                    !state->desired.fanCurve.points[action.index].enabled;
                tui_recompute_dirty(state);
                bind_current_draft(state);
            }
            break;
        case ACTION_FAN_ZERO_RPM_TOGGLE:
            toggle_fan_zero_rpm(state);
            break;
        case ACTION_SLOT_DELTA:
            state->currentSlot = clamp_int(state->currentSlot + action.value,
                                           1, CONFIG_NUM_SLOTS);
            snprintf(state->status, sizeof(state->status),
                     "Selected profile slot %d; press Load to stage it",
                     state->currentSlot);
            break;
        case ACTION_LOAD: {
            DesiredSettings loaded = {};
            char error[256] = {};
            if (!load_profile_from_config_path(state->configPath,
                    state->currentSlot, &loaded, error, sizeof(error))) {
                set_daemon_failure(state, "Profile load failed", error);
                break;
            }
            state->desired = loaded;
            bind_current_draft(state);
            tui_recompute_dirty(state);
            if (state->draftAttached) {
                snprintf(state->status, sizeof(state->status),
                         "Loaded slot %d into the staged editor",
                         state->currentSlot);
            } else if (state->serviceOnline) {
                gc_u32 requested = service_desired_mutation_domains(
                    &state->desired);
                gc_u32 missing = requested &
                    ~state->service.snapshot.health.availableMutationDomains;
                snprintf(state->status, sizeof(state->status),
                    "Loaded slot %d, but its draft is detached (requested=0x%02x missing=0x%02x); unavailable/mixed Apply remains blocked",
                    state->currentSlot, requested, missing);
            } else {
                snprintf(state->status, sizeof(state->status),
                    "Loaded slot %d locally; daemon is offline, so Apply remains detached",
                    state->currentSlot);
            }
            break;
        }
        case ACTION_SAVE: {
            char error[256] = {};
            if (!save_profile_to_config_path(state->configPath,
                    state->currentSlot, &state->desired,
                    error, sizeof(error))) {
                set_daemon_failure(state, "Profile save failed", error);
                break;
            }
            snprintf(state->status, sizeof(state->status),
                     "Saved staged values to profile slot %d",
                     state->currentSlot);
            sync_startup_snapshot_after_profile_write(state,
                state->currentSlot, true);
            break;
        }
        case ACTION_CLEAR_PROFILE: {
            char error[256] = {};
            if (!clear_profile_from_config_path(state->configPath,
                    state->currentSlot, error, sizeof(error))) {
                set_daemon_failure(state, "Profile clear failed", error);
                break;
            }
            snprintf(state->status, sizeof(state->status),
                     "Cleared profile slot %d", state->currentSlot);
            sync_startup_snapshot_after_profile_write(state,
                state->currentSlot, false);
            break;
        }
        case ACTION_RESET_DRAFT:
            state->desired = state->acceptedDesired;
            state->dirty = false;
            bind_current_draft(state);
            snprintf(state->status, sizeof(state->status),
                     "Discarded staged changes and restored accepted live intent");
            break;
        case ACTION_PROBE: {
            char error[256] = {}, output[LINUX_PATH_MAX] = {};
            default_probe_output_path(state->configPath, output, sizeof(output));
            if (run_linux_probe(output, &state->probe,
                    error, sizeof(error)))
                snprintf(state->status, sizeof(state->status),
                         "Probe written to %s", output);
            else
                set_daemon_failure(state, "Probe failed", error);
            break;
        }
        case ACTION_WRITE_ASSETS: {
            char error[256] = {}, output[LINUX_PATH_MAX] = {};
            char executable[LINUX_PATH_MAX] = {};
            default_assets_output_dir(state->configPath, output, sizeof(output));
            if (!get_executable_path(executable, sizeof(executable)) ||
                !write_linux_assets(output, executable, state->configPath,
                                    error, sizeof(error)))
                set_daemon_failure(state, "Asset generation failed", error);
            else
                snprintf(state->status, sizeof(state->status),
                         "Linux assets written to %s", output);
            break;
        }
        case ACTION_EXPORT_LIVE_TEXT:
            export_live(state, false);
            break;
        case ACTION_EXPORT_LIVE_JSON:
            export_live(state, true);
            break;
        case ACTION_STARTUP_POLICY_CYCLE:
            apply_startup_policy_cycle(state);
            break;
        default:
            break;
    }
}
