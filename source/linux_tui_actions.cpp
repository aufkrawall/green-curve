// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT

#include "linux_tui_internal.h"

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

bool desired_equal(const DesiredSettings& left, const DesiredSettings& right) {
    for (int i = 0; i < VF_NUM_POINTS; ++i) {
        if (left.hasCurvePoint[i] != right.hasCurvePoint[i] ||
            left.curvePointMHz[i] != right.curvePointMHz[i]) return false;
    }
    if (left.hasLock != right.hasLock || left.lockCi != right.lockCi ||
        left.lockMHz != right.lockMHz || left.lockMode != right.lockMode ||
        left.lockTracksAnchor != right.lockTracksAnchor ||
        left.hasGpuOffset != right.hasGpuOffset ||
        left.gpuOffsetMHz != right.gpuOffsetMHz ||
        left.gpuOffsetExcludeLowCount != right.gpuOffsetExcludeLowCount ||
        left.hasMemOffset != right.hasMemOffset ||
        left.memOffsetMHz != right.memOffsetMHz ||
        left.hasPowerLimit != right.hasPowerLimit ||
        left.powerLimitPct != right.powerLimitPct ||
        left.hasFan != right.hasFan || left.fanAuto != right.fanAuto ||
        left.fanMode != right.fanMode ||
        left.fanPercent != right.fanPercent ||
        left.fanCurve.pollIntervalMs != right.fanCurve.pollIntervalMs ||
        left.fanCurve.hysteresisC != right.fanCurve.hysteresisC ||
        left.resetOcBeforeApply != right.resetOcBeforeApply) return false;
    for (int i = 0; i < FAN_CURVE_MAX_POINTS; ++i) {
        const FanCurvePoint& a = left.fanCurve.points[i];
        const FanCurvePoint& b = right.fanCurve.points[i];
        if (a.enabled != b.enabled || a.temperatureC != b.temperatureC ||
            a.fanPercent != b.fanPercent) return false;
    }
    return true;
}

const GpuAdapterInfo* selected_adapter(const ServiceResponse& response) {
    if ((response.state.validSections &
            SERVICE_STATE_SECTION_ADAPTER_IDENTITY) == 0 ||
        response.snapshot.selectedAdapterIndex >= response.snapshot.adapterCount ||
        response.snapshot.selectedAdapterIndex >= MAX_GPU_ADAPTERS) return nullptr;
    const GpuAdapterInfo* selected =
        &response.snapshot.adapters[response.snapshot.selectedAdapterIndex];
    return selected->valid ? selected : nullptr;
}

void desired_from_live_response(const ServiceResponse& response,
                                DesiredSettings* desired) {
    if (response.state.activeDesiredValid) {
        *desired = response.desired;
        normalize_desired_settings_for_ui(desired);
        return;
    }
    initialize_desired_settings_defaults(desired);
    const ControlState& controls = response.controlState;
    if (controls.valid) {
        desired->hasGpuOffset = controls.hasGpuOffset;
        desired->gpuOffsetMHz = controls.gpuOffsetMHz;
        desired->gpuOffsetExcludeLowCount = controls.gpuOffsetExcludeLowCount;
        desired->hasMemOffset = controls.hasMemOffset;
        desired->memOffsetMHz = controls.memOffsetMHz;
        desired->hasPowerLimit = controls.hasPowerLimit;
        desired->powerLimitPct = controls.powerLimitPct;
        desired->hasFan = controls.hasFan;
        desired->fanMode = controls.fanMode;
        desired->fanAuto = controls.fanMode == FAN_MODE_AUTO;
        desired->fanPercent = controls.fanFixedPercent;
        desired->fanCurve = controls.fanCurve;
    }
    normalize_desired_settings_for_ui(desired);
}

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

bool response_matches_draft_gpu(const ServiceResponse& before,
                                const ServiceResponse& after) {
    const GpuAdapterInfo* oldGpu = selected_adapter(before);
    const GpuAdapterInfo* newGpu = selected_adapter(after);
    return oldGpu && newGpu && linux_gpu_identity_matches(oldGpu, newGpu) &&
        before.state.topologySignature != 0 &&
        before.state.topologySignature == after.state.topologySignature;
}

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
        case TUI_FIELD_FAN_TEMPERATURE:
            return index >= 0 && index < FAN_CURVE_MAX_POINTS
                ? state.desired.fanCurve.points[index].temperatureC : 0;
        case TUI_FIELD_FAN_PERCENT:
            return index >= 0 && index < FAN_CURVE_MAX_POINTS
                ? state.desired.fanCurve.points[index].fanPercent : 0;
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
    DesiredSettings& desired = state->desired;
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
            desired.powerLimitPct = clamp_int(value, 50, 150);
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
            desired.fanCurve.hysteresisC = clamp_int(value, 0,
                FAN_CURVE_MAX_HYSTERESIS_C);
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
    tui_recompute_dirty(state);
    snprintf(state->status, sizeof(state->status),
             "Staged %s value: %d",
             field == TUI_FIELD_VF_TARGET ? "absolute VF target" : "control",
             current_field_value(*state, field, index));
}

void apply_lock_action(TuiState* state, int index, int requestedMode) {
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
    snprintf(state->status, sizeof(state->status), "VF point #%d tail mode: %s",
             index, mode == LOCK_MODE_FLATTEN ? "flatten" :
             mode == LOCK_MODE_HARD ? "hard pin" : "off");
}

void set_daemon_failure(TuiState* state, const char* fallback,
                        const char* detail) {
    snprintf(state->status, sizeof(state->status), "%s%s%s",
             fallback, detail && detail[0] ? ": " : "",
             detail && detail[0] ? detail : "");
}

void apply_to_gpu(TuiState* state) {
    if (!state->serviceOnline ||
        state->service.state.gpuPhase != SERVICE_GPU_PHASE_READY ||
        !state->draftAttached) {
        snprintf(state->status, sizeof(state->status),
                 "Apply blocked: refresh and attach the draft to a READY GPU state");
        return;
    }
    DesiredSettings normalized = state->desired;
    normalize_desired_settings_for_ui(&normalized);
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
    state->draftAttached = true;
    state->dirty = false;
    snprintf(state->status, sizeof(state->status), "%s",
             result[0] ? result : "Applied staged settings");
}

void reset_gpu(TuiState* state) {
    if (!state->serviceOnline ||
        state->service.state.gpuPhase != SERVICE_GPU_PHASE_READY) {
        snprintf(state->status, sizeof(state->status),
                 "Reset blocked: daemon does not publish a READY GPU state");
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
    state->draftAttached = true;
    state->dirty = false;
    snprintf(state->status, sizeof(state->status), "%s",
             result[0] ? result : "GPU reset to driver defaults");
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
    state->dirty = !desired_equal(state->desired, state->acceptedDesired);
}

bool tui_refresh_service(TuiState* state, bool userRequested,
                         const GpuAdapterInfo* requestedTarget) {
    if (!state) return false;
    ServiceResponse next = {};
    char error[256] = {};
    const GpuAdapterInfo* target = requestedTarget;
    if (!target && state->targetGpu.valid) target = &state->targetGpu;
    bool wasOnline = state->serviceOnline;
    ServiceResponse previous = state->service;
    if (!linux_daemon_get_state(target, &next, error, sizeof(error))) {
        state->serviceOnline = false;
        state->draftAttached = false;
        if (userRequested || wasOnline)
            set_daemon_failure(state, "Daemon refresh failed", error);
        state->nextTelemetryMs = tui_monotonic_ms() + 1500;
        return false;
    }

    bool hadDirtyDraft = state->dirty;
    bool canReattach = response_matches_draft_gpu(previous, next);
    state->service = next;
    state->serviceOnline = true;
    const GpuAdapterInfo* active = selected_adapter(next);
    if (active) state->targetGpu = *active;

    if (!hadDirtyDraft) {
        desired_from_live_response(next, &state->desired);
        state->acceptedDesired = state->desired;
        state->dirty = false;
        state->draftAttached = next.state.gpuPhase == SERVICE_GPU_PHASE_READY;
        if (!wasOnline || state->selectedPoint < 0 ||
            state->selectedPoint >= VF_NUM_POINTS ||
            next.snapshot.curve[state->selectedPoint].freq_kHz == 0) {
            state->selectedPoint = useful_initial_point(next, state->desired);
        }
        if (state->vfScroll > state->selectedPoint)
            state->vfScroll = state->selectedPoint;
    } else if (canReattach && next.state.gpuPhase == SERVICE_GPU_PHASE_READY) {
        DesiredSettings accepted = {};
        desired_from_live_response(next, &accepted);
        state->acceptedDesired = accepted;
        state->draftAttached = true;
        tui_recompute_dirty(state);
    } else {
        state->draftAttached = false;
    }

    if (userRequested) {
        snprintf(state->status, sizeof(state->status),
                 state->draftAttached
                    ? "Live GPU state refreshed"
                    : "Live state changed topology; staged draft is detached and Apply is blocked");
    } else if (!wasOnline) {
        snprintf(state->status, sizeof(state->status),
                 state->draftAttached ? "Daemon reconnected"
                                      : "Daemon reconnected; draft requires review");
    }
    state->nextTelemetryMs = tui_monotonic_ms() + 1000;
    return true;
}

void tui_begin_edit(TuiState* state, TuiField field, int index) {
    if (!state || field == TUI_FIELD_NONE) return;
    state->edit.active = true;
    state->edit.field = field;
    state->edit.index = index;
    snprintf(state->edit.text, sizeof(state->edit.text), "%d",
             current_field_value(*state, field, index));
    snprintf(state->status, sizeof(state->status),
             "Editing numeric value • Enter commits • Esc cancels");
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
        size_t length = strlen(state->edit.text);
        if ((character >= '0' && character <= '9') ||
            ((character == '-' || character == '+') && length == 0)) {
            if (length + 1 < sizeof(state->edit.text)) {
                state->edit.text[length] = character;
                state->edit.text[length + 1] = 0;
            }
        }
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
                                           TUI_TAB_VF, TUI_TAB_PROFILES);
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
            state->vfScroll = clamp_int(state->vfScroll + action.value,
                                        0, VF_NUM_POINTS - 1);
            break;
        case ACTION_FAN_MODE_SET:
            state->desired.hasFan = true;
            state->desired.fanMode = action.value;
            state->desired.fanAuto = action.value == FAN_MODE_AUTO;
            tui_recompute_dirty(state);
            snprintf(state->status, sizeof(state->status), "Fan mode: %s",
                     fan_mode_label(action.value));
            break;
        case ACTION_FAN_POINT_ENABLE:
            if (action.index >= 0 && action.index < FAN_CURVE_MAX_POINTS) {
                state->desired.hasFan = true;
                state->desired.fanMode = FAN_MODE_CURVE;
                state->desired.fanCurve.points[action.index].enabled =
                    !state->desired.fanCurve.points[action.index].enabled;
                tui_recompute_dirty(state);
            }
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
            state->draftAttached = state->serviceOnline &&
                state->service.state.gpuPhase == SERVICE_GPU_PHASE_READY;
            tui_recompute_dirty(state);
            snprintf(state->status, sizeof(state->status),
                     "Loaded slot %d into the staged editor",
                     state->currentSlot);
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
            break;
        }
        case ACTION_RESET_DRAFT:
            state->desired = state->acceptedDesired;
            state->dirty = false;
            state->draftAttached = state->serviceOnline &&
                state->service.state.gpuPhase == SERVICE_GPU_PHASE_READY;
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
        default:
            break;
    }
}
