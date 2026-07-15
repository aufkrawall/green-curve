// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT

#ifndef GREEN_CURVE_GUI_SERVICE_MODEL_H
#define GREEN_CURVE_GUI_SERVICE_MODEL_H

enum GuiServicePhase {
    GUI_SERVICE_DISCONNECTED = 0,
    GUI_SERVICE_SYNCING = 1,
    GUI_SERVICE_DEVICE_MISSING = 2,
    GUI_SERVICE_RECOVERING = 3,
    GUI_SERVICE_DEGRADED = 4,
    GUI_SERVICE_READY = 5,
};

enum GuiServiceEnvelopeDecision {
    GUI_SERVICE_ENVELOPE_ACCEPTED = 0,
    GUI_SERVICE_ENVELOPE_REJECTED_CONNECTION = 1,
    GUI_SERVICE_ENVELOPE_REJECTED_INSTANCE = 2,
    GUI_SERVICE_ENVELOPE_REJECTED_GENERATION = 3,
    GUI_SERVICE_ENVELOPE_REJECTED_REVISION = 4,
    GUI_SERVICE_ENVELOPE_REJECTED_INVALID = 5,
};

struct GuiServiceModel {
    GuiServicePhase phase;
    gc_u64 connectionEpoch;
    gc_u64 serviceInstanceId;
    gc_u64 stateRevision;
    gc_u64 gpuGeneration;
    gc_u64 minimumGpuGeneration;
    gc_u64 topologySignature;
    gc_u64 retiredServiceInstanceId;
    gc_u32 validSections;
    bool hasAcceptedEnvelope;
};

static inline GuiServicePhase gui_service_phase_from_envelope(
    const ServiceStateEnvelope* state) {
    if (!state) return GUI_SERVICE_DEGRADED;
    switch ((ServiceGpuPhase)state->gpuPhase) {
        case SERVICE_GPU_PHASE_DEVICE_MISSING:
            return GUI_SERVICE_DEVICE_MISSING;
        case SERVICE_GPU_PHASE_RECOVERING:
        case SERVICE_GPU_PHASE_REAPPLYING:
        case SERVICE_GPU_PHASE_STARTING:
            return GUI_SERVICE_RECOVERING;
        case SERVICE_GPU_PHASE_READY:
            return (state->validSections & SERVICE_STATE_SECTION_READY_REQUIRED) ==
                SERVICE_STATE_SECTION_READY_REQUIRED
                    ? GUI_SERVICE_READY : GUI_SERVICE_DEGRADED;
        case SERVICE_GPU_PHASE_DEGRADED:
        default:
            return GUI_SERVICE_DEGRADED;
    }
}

static inline void gui_service_model_initialize(GuiServiceModel* model) {
    if (!model) return;
    *model = {};
    model->phase = GUI_SERVICE_DISCONNECTED;
    model->connectionEpoch = 1;
}

static inline void gui_service_model_begin_sync(
    GuiServiceModel* model, gc_u64 connectionEpoch) {
    if (!model) return;
    if (connectionEpoch > model->connectionEpoch) {
        model->connectionEpoch = connectionEpoch;
        model->hasAcceptedEnvelope = false;
        model->stateRevision = 0;
        model->gpuGeneration = 0;
        model->topologySignature = 0;
        model->validSections = 0;
    }
    model->phase = GUI_SERVICE_SYNCING;
}

static inline void gui_service_model_disconnect(
    GuiServiceModel* model, gc_u64 connectionEpoch) {
    if (!model) return;
    if (connectionEpoch >= model->connectionEpoch)
        model->connectionEpoch = connectionEpoch;
    model->phase = GUI_SERVICE_DISCONNECTED;
    model->hasAcceptedEnvelope = false;
    model->stateRevision = 0;
    model->gpuGeneration = 0;
    model->topologySignature = 0;
    model->validSections = 0;
}

static inline bool gui_service_model_require_new_gpu_generation(
    GuiServiceModel* model) {
    if (!model || !model->hasAcceptedEnvelope ||
        model->phase != GUI_SERVICE_READY || !model->serviceInstanceId ||
        !model->gpuGeneration)
        return false;
    gc_u64 current = model->gpuGeneration;
    if (current < ~(gc_u64)0) {
        model->minimumGpuGeneration = current + 1;
        return true;
    }
    return false;
}

static inline GuiServiceEnvelopeDecision gui_service_model_accept(
    GuiServiceModel* model, gc_u64 connectionEpoch,
    const ServiceStateEnvelope* state) {
    if (!model || !state || !state->serviceInstanceId ||
        !state->stateRevision || !state->gpuGeneration ||
        state->gpuPhase > SERVICE_GPU_PHASE_DEGRADED ||
        (state->validSections & ~SERVICE_STATE_SECTION_ALL) != 0 ||
        ((state->validSections & SERVICE_STATE_SECTION_CURVE_TOPOLOGY) != 0 &&
         !state->topologySignature) || state->activeDesiredValid > 1) {
        return GUI_SERVICE_ENVELOPE_REJECTED_INVALID;
    }
    if (connectionEpoch < model->connectionEpoch)
        return GUI_SERVICE_ENVELOPE_REJECTED_CONNECTION;
    if (connectionEpoch > model->connectionEpoch) {
        gui_service_model_begin_sync(model, connectionEpoch);
    }
    if (state->serviceInstanceId == model->retiredServiceInstanceId)
        return GUI_SERVICE_ENVELOPE_REJECTED_INSTANCE;
    if (!model->hasAcceptedEnvelope && model->serviceInstanceId &&
        state->serviceInstanceId != model->serviceInstanceId) {
        model->retiredServiceInstanceId = model->serviceInstanceId;
        model->minimumGpuGeneration = 0;
    }
    if (state->serviceInstanceId == model->serviceInstanceId &&
        model->minimumGpuGeneration &&
        state->gpuGeneration < model->minimumGpuGeneration)
        return GUI_SERVICE_ENVELOPE_REJECTED_GENERATION;
    if (model->hasAcceptedEnvelope) {
        if (state->serviceInstanceId != model->serviceInstanceId)
            return GUI_SERVICE_ENVELOPE_REJECTED_INSTANCE;
        if (state->gpuGeneration < model->gpuGeneration)
            return GUI_SERVICE_ENVELOPE_REJECTED_GENERATION;
        if (state->stateRevision <= model->stateRevision)
            return GUI_SERVICE_ENVELOPE_REJECTED_REVISION;
    }
    model->hasAcceptedEnvelope = true;
    model->serviceInstanceId = state->serviceInstanceId;
    model->stateRevision = state->stateRevision;
    model->gpuGeneration = state->gpuGeneration;
    model->topologySignature = state->topologySignature;
    model->validSections = state->validSections;
    model->phase = gui_service_phase_from_envelope(state);
    if (state->gpuGeneration >= model->minimumGpuGeneration)
        model->minimumGpuGeneration = 0;
    return GUI_SERVICE_ENVELOPE_ACCEPTED;
}

static inline bool gui_service_model_ready(const GuiServiceModel* model) {
    return model && model->phase == GUI_SERVICE_READY &&
        model->hasAcceptedEnvelope;
}

static inline bool gui_service_phase_actions_enabled(GuiServicePhase phase) {
    return phase == GUI_SERVICE_READY;
}

static inline bool gui_service_completion_context_current(
    gc_u64 workGpuEpoch, gc_u64 currentGpuEpoch,
    gc_u64 workPresentationEpoch, gc_u64 currentPresentationEpoch) {
    return workGpuEpoch == currentGpuEpoch &&
        workPresentationEpoch == currentPresentationEpoch;
}

static inline bool gui_service_mutation_result_can_commit(
    bool transportSuccess, bool contextCurrent, bool sameAuthority,
    GuiServiceEnvelopeDecision previewDecision,
    const GuiServiceModel* previewModel) {
    return transportSuccess && contextCurrent && sameAuthority &&
        previewDecision == GUI_SERVICE_ENVELOPE_ACCEPTED &&
        gui_service_model_ready(previewModel);
}

static inline bool gui_service_model_live_section(
    const GuiServiceModel* model, gc_u32 section) {
    return gui_service_model_ready(model) &&
        (model->validSections & section) == section;
}

static inline bool gui_service_failure_requires_render(
    GuiServicePhase previousPhase, bool hadLiveAuthority,
    bool previousInstalled, bool previousRunning, bool previousBroken,
    bool nextInstalled, bool nextRunning, bool nextBroken,
    bool errorChanged) {
    return previousPhase != GUI_SERVICE_DISCONNECTED || hadLiveAuthority ||
        previousInstalled != nextInstalled ||
        previousRunning != nextRunning ||
        previousBroken != nextBroken || errorChanged;
}

static inline const char* gui_service_phase_tray_text(GuiServicePhase phase) {
    switch (phase) {
        case GUI_SERVICE_SYNCING: return "synchronizing GPU state";
        case GUI_SERVICE_DEVICE_MISSING: return "GPU disconnected";
        case GUI_SERVICE_RECOVERING: return "GPU reconnecting";
        case GUI_SERVICE_DEGRADED: return "GPU state degraded";
        default: return "service unavailable";
    }
}

#endif // GREEN_CURVE_GUI_SERVICE_MODEL_H
