// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
// Included inside linux_tui_actions.cpp's anonymous namespace.

void desired_from_live_response(const ServiceResponse& response,
                                DesiredSettings* desired) {
    if (response.state.activeDesiredValid) {
        *desired = response.desired;
    } else {
        initialize_desired_settings_defaults(desired);
        const ControlState& controls = response.controlState;
        if (controls.valid) {
            desired->hasGpuOffset = controls.hasGpuOffset;
            desired->gpuOffsetMHz = controls.gpuOffsetMHz;
            desired->gpuOffsetExcludeLowCount =
                controls.gpuOffsetExcludeLowCount;
            desired->hasMemOffset = controls.hasMemOffset;
            desired->memOffsetMHz = controls.memOffsetMHz;
            desired->hasPowerLimit = controls.hasPowerLimit;
            desired->powerLimitPct = controls.powerLimitPct;
            desired->hasFan = controls.hasFan;
            desired->fanMode = controls.fanMode;
            desired->fanAuto = controls.fanMode == FAN_MODE_AUTO;
            desired->fanPercent = controls.fanFixedPercent;
            desired->fanCurve = controls.fanCurve;
            desired->hasXbarOffsetKhz = controls.hasXbarOffset;
            desired->xbarOffsetKhz = controls.xbarOffsetKhz;
            desired->hasXbarMsvddOffsetUv = controls.hasXbarMsvddOffset;
            desired->xbarMsvddOffsetUv = controls.xbarMsvddOffsetUv;
            desired->hasSysClkOffsetKhz = controls.hasSysClkOffset;
            desired->sysClkOffsetKhz = controls.sysClkOffsetKhz;
            desired->hasVideoClkOffsetKhz = controls.hasVideoClkOffset;
            desired->videoClkOffsetKhz = controls.videoClkOffsetKhz;
        }
    }
    normalize_desired_settings_for_ui(desired);
    service_project_desired_to_available_domains(desired,
        response.snapshot.health.availableMutationDomains);
}
