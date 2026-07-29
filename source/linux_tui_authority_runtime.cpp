// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
// Included inside linux_tui_actions.cpp's anonymous namespace.

bool bind_current_draft(TuiState* state) {
    if (!state || !state->serviceOnline) {
        if (state) {
            state->draftAttached = false;
            state->draftBinding = LinuxTuiDraftBinding{};
        }
        return false;
    }
    state->draftAttached = linux_tui_bind_draft(
        &state->draftBinding, &state->service, &state->desired);
    return state->draftAttached;
}

bool vf_editing_available(const TuiState& state) {
    return state.serviceOnline &&
        (state.service.snapshot.health.availableMutationDomains &
            SERVICE_MUTATION_DOMAIN_VF_CURVE) != 0 &&
        (state.service.state.validSections &
            SERVICE_STATE_SECTION_CURVE_TOPOLOGY) != 0;
}

void set_gpu_health_status(TuiState* state, const char* prefix) {
    if (!state || !state->serviceOnline) return;
    const ServiceGpuHealth& health = state->service.snapshot.health;
    snprintf(state->status, sizeof(state->status),
        "%s%s%s%s%s. %s",
        prefix && prefix[0] ? prefix : "GPU state",
        prefix && prefix[0] ? ": " : "",
        service_gpu_health_reason_name(health.reason),
        health.detail[0] ? " — " : "",
        health.detail[0] ? health.detail : "",
        linux_tui_health_remediation(health.reason));
}

gc_u32 field_mutation_domain(const TuiState& state, TuiField field) {
    switch (field) {
        case TUI_FIELD_EXCLUDED_POINTS:
        case TUI_FIELD_VF_TARGET:
            return SERVICE_MUTATION_DOMAIN_VF_CURVE;
        case TUI_FIELD_GPU_OFFSET: {
            gc_u32 domains = service_desired_mutation_domains(&state.desired);
            return (domains & SERVICE_MUTATION_DOMAIN_VF_CURVE)
                ? SERVICE_MUTATION_DOMAIN_VF_CURVE
                : SERVICE_MUTATION_DOMAIN_GPU_OFFSET;
        }
        case TUI_FIELD_MEMORY_OFFSET:
            return SERVICE_MUTATION_DOMAIN_MEM_OFFSET;
        case TUI_FIELD_POWER_LIMIT:
            return SERVICE_MUTATION_DOMAIN_POWER;
        case TUI_FIELD_FAN_FIXED:
        case TUI_FIELD_FAN_POLL:
        case TUI_FIELD_FAN_HYSTERESIS:
        case TUI_FIELD_FAN_TEMPERATURE:
        case TUI_FIELD_FAN_PERCENT:
            return SERVICE_MUTATION_DOMAIN_FAN;
        default:
            return 0;
    }
}

bool field_edit_available(TuiState* state, TuiField field) {
    if (!state) return false;
    gc_u32 domain = field_mutation_domain(*state, field);
    if ((domain & SERVICE_MUTATION_DOMAIN_VF_CURVE) != 0 &&
        !vf_editing_available(*state)) {
        if (state->serviceOnline) set_gpu_health_status(state,
            "VF editing disabled");
        else snprintf(state->status, sizeof(state->status),
            "VF editing disabled: daemon is offline and no authoritative topology is available");
        return false;
    }
    if (state->serviceOnline && domain != 0 &&
        (state->service.snapshot.health.availableMutationDomains & domain) !=
            domain) {
        snprintf(state->status, sizeof(state->status),
            "Editing blocked: mutation domain 0x%02x is unavailable (%s)",
            domain, state->service.snapshot.health.detail[0]
                ? state->service.snapshot.health.detail
                : service_gpu_health_reason_name(
                    state->service.snapshot.health.reason));
        return false;
    }
    return true;
}
