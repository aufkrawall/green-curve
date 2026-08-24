// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT

#include "linux_tui_layout_internal.h"

#include <stdio.h>
#include <string.h>

namespace {

void add_fan_stepper(TuiCanvas* c, int x, int y, const char* label,
                     TuiField field, int context, int value, int delta,
                     const char* unit) {
    c->text(x, y, 18, label, TUI_STYLE_TEXT);
    int minus = c->button(TuiRect{x + 18, y, 3, 1}, "−",
                          ACTION_FIELD_STEP, (int)field, -delta);
    c->layout()->actions[minus].context = context;
    char formatted[24] = {};
    tui_format_int(formatted, sizeof(formatted), value,
                   field == TUI_FIELD_MEMORY_OFFSET ||
                   field == TUI_FIELD_GPU_OFFSET);
    c->field(TuiRect{x + 22, y, 9, 1}, formatted, field, context);
    int plus = c->button(TuiRect{x + 32, y, 3, 1}, "+",
                         ACTION_FIELD_STEP, (int)field, delta);
    c->layout()->actions[plus].context = context;
    c->text(x + 36, y, 8, unit, TUI_STYLE_MUTED);
}

void draw_fan_graph(TuiCanvas* c, const TuiRect& panel) {
    const TuiViewModel& vm = c->view();
    c->box(panel, "CUSTOM FAN CURVE", "click fields to edit");
    if (panel.width < 20 || panel.height < 8) return;
    TuiRect plot{panel.x + 4, panel.y + 2,
                 panel.width - 7, panel.height - 4};
    c->fill(plot.x, plot.y, plot.width, plot.height, TUI_STYLE_DEFAULT);
    for (int row = 0; row < plot.height; row += 3)
        c->hline(plot.x, plot.y + row, plot.width, TUI_STYLE_DIM);
    int lastX = -1, lastY = -1;
    for (int i = 0; i < FAN_CURVE_MAX_POINTS; ++i) {
        const FanCurvePoint& point = vm.desired->fanCurve.points[i];
        if (!point.enabled) continue;
        int x = plot.x + (point.temperatureC * (plot.width - 1)) / 100;
        int y = plot.y + plot.height - 1 -
                (point.fanPercent * (plot.height - 1)) / 100;
        if (lastX >= 0) {
            int dx = x - lastX;
            int steps = dx > 0 ? dx : 1;
            for (int s = 1; s < steps; ++s) {
                int lineY = lastY + ((y - lastY) * s) / steps;
                c->cell(lastX + s, lineY,
                        y < lastY ? "╱" : y > lastY ? "╲" : "─",
                        TUI_STYLE_GREEN);
            }
        }
        c->cell(x, y, "●", TUI_STYLE_CYAN);
        lastX = x;
        lastY = y;
    }
    char telemetry[96] = {};
    if (vm.service && vm.service->snapshot.gpuTemperatureValid) {
        snprintf(telemetry, sizeof(telemetry), "Live %d°C • fan %d%%",
                 vm.service->snapshot.gpuTemperatureC,
                 vm.service->controlState.fanCurrentPercent);
    } else {
        snprintf(telemetry, sizeof(telemetry), "Temperature telemetry unavailable");
    }
    c->text(plot.x, panel.y + panel.height - 2, plot.width,
            telemetry, TUI_STYLE_CYAN);
}

void draw_fan_settings(TuiCanvas* c, const TuiRect& panel) {
    const TuiViewModel& vm = c->view();
    const DesiredSettings& desired = *vm.desired;
    char actual[96] = {};
    if (vm.serviceOnline && vm.service &&
        vm.service->snapshot.fanSupported &&
        vm.service->snapshot.fanCount > 0 &&
        vm.service->controlState.fanPolicyReadbackValid) {
        char target[16] = "?";
        if (vm.service->controlState.fanTargetReadbackValid)
            snprintf(target, sizeof(target), "%u",
                     vm.service->snapshot.fanTargetPercent[0]);
        snprintf(actual, sizeof(actual), "actual %s • target %s%% • measured %d%%",
                 vm.service->snapshot.fanPolicy[0] ==
                    NVML_FAN_POLICY_TEMPERATURE_CONTINOUS_SW
                    ? "auto" : "manual",
                 target,
                 vm.service->controlState.fanCurrentPercent);
    } else {
        snprintf(actual, sizeof(actual), "hardware readback unavailable");
    }
    c->box(panel, "FAN CONTROL — CONFIGURED INTENT", actual);
    int x = panel.x + 2;
    int y = panel.y + 2;
    c->text(x, y, 10, "Mode", TUI_STYLE_TEXT);
    c->button(TuiRect{x + 11, y, 9, 1}, "AUTO", ACTION_FAN_MODE_SET,
              0, FAN_MODE_AUTO, desired.fanMode == FAN_MODE_AUTO);
    c->button(TuiRect{x + 21, y, 10, 1}, "FIXED", ACTION_FAN_MODE_SET,
              0, FAN_MODE_FIXED, desired.fanMode == FAN_MODE_FIXED);
    c->button(TuiRect{x + 32, y, 10, 1}, "CURVE", ACTION_FAN_MODE_SET,
              0, FAN_MODE_CURVE, desired.fanMode == FAN_MODE_CURVE);
    if (panel.width >= 48)
        c->text(x + 43, y, panel.width - 47,
                desired.fanMode == FAN_MODE_CURVE ? "custom" :
                desired.fanMode == FAN_MODE_FIXED ? "manual" : "driver",
                TUI_STYLE_MUTED);
    if (panel.height >= 7) {
        add_fan_stepper(c, x, y + 2, "Fixed fan", TUI_FIELD_FAN_FIXED,
                        0, desired.fanPercent, 1, "%");
        add_fan_stepper(c, x, y + 4, "Poll interval", TUI_FIELD_FAN_POLL,
                        0, desired.fanCurve.pollIntervalMs, 250, "ms");
        add_fan_stepper(c, x, y + 6, "Hysteresis", TUI_FIELD_FAN_HYSTERESIS,
                        0, desired.fanCurve.hysteresisC, 1, "°C");
        if (panel.width >= 68) {
            int zeroX = panel.x + panel.width - 24;
            c->checkbox(zeroX, y + 6,
                desired.fanCurve.zeroRpmEnabled ? 1 : 0,
                desired.fanCurve.zeroRpmEnabled
                    ? TUI_STYLE_GREEN : TUI_STYLE_BORDER);
            c->text(zeroX + 4, y + 6, 18, "Native zero-RPM",
                TUI_STYLE_TEXT);
            c->register_action(TuiRect{zeroX, y + 6, 22, 1},
                ACTION_FAN_ZERO_RPM_TOGGLE);
        }
    }
}

void draw_fan_table(TuiCanvas* c, const TuiRect& panel) {
    const TuiViewModel& vm = c->view();
    char rule[96] = "strict temp ↑ • speed nondecreasing";
    if (vm.desired->fanCurve.zeroRpmEnabled) {
        int startC = fan_curve_first_enabled_temperature(
            &vm.desired->fanCurve);
        int stopC = startC - fan_curve_zero_rpm_hysteresis(
            &vm.desired->fanCurve);
        if (stopC < 0) stopC = 0;
        snprintf(rule, sizeof(rule),
            "zero-RPM: OFF <=%d°C • ON >=%d°C • speed >= firmware min",
            stopC, startC);
    }
    c->box(panel, "FAN CURVE POINTS", rule);
    int rows = panel.height - 3;
    if (rows <= 0) return;
    c->layout()->fanVisibleRows = rows;
    int x = panel.x + 2;
    int tempX = panel.width >= 75 ? x + 18 : x + 12;
    int pctX = tempX + 18;
    c->text(x, panel.y + 1, 8, "POINT", TUI_STYLE_MUTED);
    c->text(x + 8, panel.y + 1, 8, "ENABLED", TUI_STYLE_MUTED);
    c->text(tempX, panel.y + 1, 14, "TEMP °C", TUI_STYLE_MUTED);
    c->text(pctX, panel.y + 1, 14, "FAN %", TUI_STYLE_MUTED);
    if (panel.width >= 75)
        c->text(pctX + 18, panel.y + 1,
                panel.x + panel.width - pctX - 20,
                "BEHAVIOR", TUI_STYLE_MUTED);
    int first = vm.fanScroll;
    if (first < 0) first = 0;
    if (first >= FAN_CURVE_MAX_POINTS) first = FAN_CURVE_MAX_POINTS - 1;
    int visible = rows < FAN_CURVE_MAX_POINTS - first
        ? rows : FAN_CURVE_MAX_POINTS - first;
    for (int row = 0; row < visible; ++row) {
        int i = first + row;
        int y = panel.y + 2 + row;
        const FanCurvePoint& point = vm.desired->fanCurve.points[i];
        c->fill(panel.x + 1, y, panel.width - 2, 1,
                (row & 1) ? TUI_STYLE_ROW_ALT : TUI_STYLE_PANEL);
        char number[16] = {}, temp[16] = {}, pct[16] = {};
        snprintf(number, sizeof(number), "P%d", i + 1);
        snprintf(temp, sizeof(temp), "%d", point.temperatureC);
        snprintf(pct, sizeof(pct), "%d", point.fanPercent);
        c->text(x, y, 6, number, TUI_STYLE_TEXT);
        c->checkbox(x + 9, y, point.enabled ? 1 : 0,
                    point.enabled ? TUI_STYLE_GREEN : TUI_STYLE_BORDER);
        c->register_action(TuiRect{x + 9, y, 3, 1},
                           ACTION_FAN_POINT_ENABLE, i, 0);
        c->field(TuiRect{tempX, y, 10, 1}, temp,
                 TUI_FIELD_FAN_TEMPERATURE, i);
        c->field(TuiRect{pctX, y, 10, 1}, pct,
                 TUI_FIELD_FAN_PERCENT, i);
        if (panel.width >= 75) {
            char behavior[64] = {};
            snprintf(behavior, sizeof(behavior), "%s at %d°C",
                     point.enabled ? "target" : "disabled",
                     point.temperatureC);
            c->text(pctX + 18, y,
                    panel.x + panel.width - pctX - 20,
                    behavior, point.enabled ? TUI_STYLE_MUTED : TUI_STYLE_DIM);
        }
    }
}

void draw_profile_panel(TuiCanvas* c, const TuiRect& panel) {
    const TuiViewModel& vm = c->view();
    c->box(panel, "PROFILE", "explicit Load stages saved values");
    int x = panel.x + 3;
    int y = panel.y + 2;
    c->text(x, y, 14, "Profile slot", TUI_STYLE_TEXT);
    c->button(TuiRect{x + 15, y, 4, 1}, "<", ACTION_SLOT_DELTA, 0, -1);
    char slot[24] = {};
    snprintf(slot, sizeof(slot), "SLOT %d", vm.currentSlot);
    c->fill(x + 20, y, 12, 1, TUI_STYLE_FIELD);
    c->text(x + 20, y, 12, slot, TUI_STYLE_FIELD);
    c->button(TuiRect{x + 33, y, 4, 1}, ">", ACTION_SLOT_DELTA, 0, 1);
    c->button(TuiRect{x, y + 2, 11, 1}, "LOAD", ACTION_LOAD);
    c->button(TuiRect{x + 12, y + 2, 11, 1}, "SAVE", ACTION_SAVE);
    c->button(TuiRect{x + 24, y + 2, 11, 1}, "CLEAR", ACTION_CLEAR_PROFILE);
    c->button(TuiRect{x + 36, y + 2, 15, 1}, "RESET DRAFT",
              ACTION_RESET_DRAFT);
    c->text(x, y + 4, panel.width - 6,
            vm.dirty ? "● staged values differ from accepted intent"
                     : vm.intentReadback.divergedDomains
                        ? "● accepted intent differs from hardware readback"
                        : "● editor matches accepted intent and hardware",
            vm.dirty ? TUI_STYLE_ORANGE
                     : vm.intentReadback.divergedDomains
                        ? TUI_STYLE_RED : TUI_STYLE_GREEN);
    if (panel.height >= 9) {
        c->text(x, y + 6, panel.width - 6, "Config", TUI_STYLE_MUTED);
        c->text(x + 8, y + 6, panel.width - 14,
                vm.configPath ? vm.configPath : "", TUI_STYLE_TEXT);
    }
}

void draw_tools_panel(TuiCanvas* c, const TuiRect& panel) {
    const TuiViewModel& vm = c->view();
    // Not "all 128 points": the exports cover every populated point of the
    // graphics clock domain, which is 127 on an RTX 5070. The read window is
    // VF_NUM_POINTS, but the domain length comes from the driver's table.
    c->box(panel, "TOOLS", "read-only exports include every populated point");
    int x = panel.x + 3;
    int y = panel.y + 2;
    c->button(TuiRect{x, y, 12, 1}, "REFRESH", ACTION_REFRESH);
    c->button(TuiRect{x + 13, y, 10, 1}, "PROBE", ACTION_PROBE);
    c->button(TuiRect{x + 24, y, 15, 1}, "WRITE ASSETS",
              ACTION_WRITE_ASSETS);
    c->button(TuiRect{x, y + 2, 18, 1}, "EXPORT LIVE TXT",
              ACTION_EXPORT_LIVE_TEXT);
    c->button(TuiRect{x + 19, y + 2, 19, 1}, "EXPORT LIVE JSON",
              ACTION_EXPORT_LIVE_JSON);
    if (panel.height >= 8) {
        c->text(x, y + 4, panel.width - 6,
                "CLI: greencurve --dump-live", TUI_STYLE_CYAN);
        c->text(x, y + 5, panel.width - 6,
                "CLI: greencurve --json-live", TUI_STYLE_CYAN);
    }
    if (vm.probeCompleted && panel.height >= 10) {
        c->text(x, y + 7, panel.width - 6,
                vm.probeSummary ? vm.probeSummary : "Probe complete",
                TUI_STYLE_MUTED);
    }
}

void draw_service_panel(TuiCanvas* c, const TuiRect& panel) {
    const TuiViewModel& vm = c->view();
    c->box(panel, "DAEMON / INPUT", "no control is mouse-only");
    int x = panel.x + 3;
    int y = panel.y + 2;
    char state[192] = {};
    if (vm.serviceOnline && vm.service) {
        snprintf(state, sizeof(state),
                 "Service %s build %u • instance %llu • generation %llu",
                 vm.service->serviceVersion,
                 (unsigned int)vm.service->serviceBuildNumber,
                 (unsigned long long)vm.service->state.serviceInstanceId,
                 (unsigned long long)vm.service->state.gpuGeneration);
    } else {
        snprintf(state, sizeof(state), "Daemon unavailable — edits remain local");
    }
    c->text(x, y, panel.width - 6, state,
            vm.serviceOnline ? TUI_STYLE_GREEN : TUI_STYLE_RED);
    c->text(x, y + 2, panel.width - 6,
            "Mouse: click fields, checkboxes, tabs, graph points and buttons",
            TUI_STYLE_TEXT);
    c->text(x, y + 3, panel.width - 6,
            "Keyboard: Tab/Shift+Tab • arrows • Enter/Space • PgUp/PgDn • Home/End",
            TUI_STYLE_TEXT);
    c->text(x, y + 4, panel.width - 6,
            "Resize: layout and hitboxes are rebuilt atomically from terminal cells",
            TUI_STYLE_MUTED);
    // The boot-apply policy lives here rather than on the PROFILE panel because
    // it is daemon state, not editor state: it survives this process and is what
    // the GPU gets at the next boot.
    if (panel.height >= 9) {
        c->text(x, y + 6, 16, "Startup apply", TUI_STYLE_TEXT);
        // The daemon boot-applies a snapshot it took of the profile, not the
        // profile itself.  When the two have drifted apart, "PROFILE N" alone
        // would claim something untrue, so the label says so.
        bool snapshotStale =
            startup_snapshot_state_is_stale(vm.startupSnapshotState);
        char label[24] = {};
        if (vm.startupPolicyMode == SERVICE_STARTUP_POLICY_PROFILE)
            snprintf(label, sizeof(label), "PROFILE %u%s", vm.startupPolicySlot,
                     snapshotStale ? " STALE" : "");
        else
            snprintf(label, sizeof(label), "%s",
                     vm.startupPolicyMode == SERVICE_STARTUP_POLICY_NONE
                         ? "NOTHING" : "LAST APPLIED");
        c->button(TuiRect{x + 17, y + 6, 16, 1}, label,
                  ACTION_STARTUP_POLICY_CYCLE);
        const char* hint =
            vm.startupPolicyMode == SERVICE_STARTUP_POLICY_PROFILE
                ? (vm.startupSnapshotState ==
                       STARTUP_SNAPSHOT_STATE_PROFILE_MISSING
                       ? "the bound slot no longer loads; re-bind or disable it"
                       : (snapshotStale
                              ? "what boots differs from the slot; Save it again"
                              : "saved profile is applied when the daemon starts"))
                : (vm.startupPolicyMode == SERVICE_STARTUP_POLICY_NONE
                       ? "the GPU is left untouched when the daemon starts"
                       : "the last applied settings are restored at daemon start");
        if (panel.width > 42)
            c->text(x + 34, y + 6, panel.width - 40, hint,
                    snapshotStale ? TUI_STYLE_RED : TUI_STYLE_MUTED);
    }
}

void draw_advanced_stepper(TuiCanvas* c, int x, int y, const char* label,
                           TuiField field, int value, int delta,
                           const char* unit, bool editable,
                           const char* live) {
    c->text(x, y, 18, label,
            editable ? TUI_STYLE_TEXT : TUI_STYLE_DIM);
    char formatted[24] = {};
    tui_format_int(formatted, sizeof(formatted), value, true);
    if (!editable) {
        c->text(x + 22, y, 9, formatted, TUI_STYLE_DIM, true);
        c->text(x + 36, y, 5, unit, TUI_STYLE_DIM);
        if (live && c->layout()->width >= 100)
            c->text(x + 43, y, c->layout()->width - x - 47, live,
                    TUI_STYLE_DIM);
        return;
    }
    int minus = c->button(TuiRect{x + 18, y, 3, 1}, "−",
                          ACTION_FIELD_STEP, (int)field, -delta);
    c->layout()->actions[minus].context = 0;
    c->field(TuiRect{x + 22, y, 9, 1}, formatted, field, 0);
    int plus = c->button(TuiRect{x + 32, y, 3, 1}, "+",
                         ACTION_FIELD_STEP, (int)field, delta);
    c->layout()->actions[plus].context = 0;
    c->text(x + 36, y, 5, unit, TUI_STYLE_MUTED);
    if (live && c->layout()->width >= 100)
        c->text(x + 43, y, c->layout()->width - x - 47, live,
                TUI_STYLE_CYAN);
}

void draw_advanced_panel(TuiCanvas* c, const TuiRect& panel) {
    const TuiViewModel& vm = c->view();
    const DesiredSettings& desired = *vm.desired;
    c->box(panel, "ADVANCED CLOCKS",
           "version-checked full-block writes with exact offset readback");
    int x = panel.x + 3;
    int y = panel.y + 2;
    gc_u32 availableDomains = vm.serviceOnline && vm.service
        ? vm.service->snapshot.health.availableMutationDomains : 0;
    const ServiceSnapshot* snap = vm.serviceOnline && vm.service
        ? &vm.service->snapshot : nullptr;
    char xbarLive[64] = "offline / saved intent only";
    char voltageLive[64] = "offline / saved intent only";
    char sysLive[64] = "offline / saved intent only";
    char videoLive[64] = "offline / saved intent only";
    if (snap) {
        snprintf(xbarLive, sizeof(xbarLive), "%s",
                 "unavailable for this GPU");
        snprintf(voltageLive, sizeof(voltageLive), "%s",
                 "unavailable for this GPU");
        snprintf(sysLive, sizeof(sysLive), "%s",
                 "unavailable for this GPU");
        snprintf(videoLive, sizeof(videoLive), "%s",
                 "unavailable for this GPU");
        char xbarNow[16] = "---";
        char sysNow[16] = "---";
        char videoNow[16] = "---";
        if (snap->xbarMeasuredClockKhz)
            snprintf(xbarNow, sizeof(xbarNow), "%u",
                     snap->xbarMeasuredClockKhz / 1000);
        if (snap->sysClkMeasuredClockKhz)
            snprintf(sysNow, sizeof(sysNow), "%u",
                     snap->sysClkMeasuredClockKhz / 1000);
        if (snap->videoClkMeasuredClockKhz)
            snprintf(videoNow, sizeof(videoNow), "%u",
                     snap->videoClkMeasuredClockKhz / 1000);
        if (snap->xbarOffsetReadbackValid)
            snprintf(xbarLive, sizeof(xbarLive),
                     "readback %+d MHz • now %s MHz",
                     snap->xbarOffsetKhz / 1000, xbarNow);
        if (snap->xbarMsvddOffsetReadbackValid)
            snprintf(voltageLive, sizeof(voltageLive), "readback %+d mV",
                     snap->xbarMsvddOffsetUv / 1000);
        if (snap->sysClkOffsetReadbackValid)
            snprintf(sysLive, sizeof(sysLive),
                     "readback %+d MHz • now %s MHz",
                     snap->sysClkOffsetKhz / 1000, sysNow);
        if (snap->videoClkOffsetReadbackValid)
            snprintf(videoLive, sizeof(videoLive),
                     "readback %+d MHz • now %s MHz",
                     snap->videoClkOffsetKhz / 1000, videoNow);
    }
    bool xbar = (availableDomains & SERVICE_MUTATION_DOMAIN_XBAR) != 0;
    bool sys = (availableDomains & SERVICE_MUTATION_DOMAIN_SYS_CLK) != 0;
    bool video = (availableDomains & SERVICE_MUTATION_DOMAIN_VIDEO_CLK) != 0;
    bool offline = !vm.serviceOnline;
    draw_advanced_stepper(c, x, y, "XBAR clock", TUI_FIELD_XBAR_OFFSET,
                          desired.xbarOffsetKhz / 1000, 10, "MHz",
                          offline || xbar,
                          xbarLive);
    draw_advanced_stepper(c, x, y + 2, "XBAR MSVDD", TUI_FIELD_XBAR_MSVDD,
                          desired.xbarMsvddOffsetUv / 1000, 1, "mV",
                          offline || xbar,
                          voltageLive);
    draw_advanced_stepper(c, x, y + 4, "SYS clock", TUI_FIELD_SYS_CLK_OFFSET,
                          desired.sysClkOffsetKhz / 1000, 10, "MHz",
                          offline || sys,
                          sysLive);
    draw_advanced_stepper(c, x, y + 6, "Video clock",
                          TUI_FIELD_VIDEO_CLK_OFFSET,
                          desired.videoClkOffsetKhz / 1000, 10, "MHz",
                          offline || video,
                          videoLive);
    if (panel.height >= 13) {
        c->text(x, y + 9, panel.width - 6,
                vm.serviceOnline
                    ? "Unavailable rows are retained in portable profiles but cannot be staged for this GPU."
                    : "Daemon offline: values can be saved locally; Apply remains detached.",
                TUI_STYLE_MUTED);
    }
}

}  // namespace

void tui_draw_fan_tab(TuiCanvas* canvas, const TuiRect& content) {
    TuiLayout* out = canvas->layout();
    if (out->breakpoint == TUI_BREAKPOINT_WIDE) {
        int topHeight = content.height >= 28 ? 13 : 10;
        int leftWidth = content.width / 2;
        draw_fan_graph(canvas,
            TuiRect{2, content.y, leftWidth - 1, topHeight});
        draw_fan_settings(canvas,
            TuiRect{leftWidth + 1, content.y,
                    content.width - leftWidth, topHeight});
        draw_fan_table(canvas,
            TuiRect{2, content.y + topHeight,
                    content.width - 2, content.height - topHeight});
    } else {
        int settingsHeight = 10;
        draw_fan_settings(canvas,
            TuiRect{2, content.y, content.width - 2, settingsHeight});
        draw_fan_table(canvas,
            TuiRect{2, content.y + settingsHeight, content.width - 2,
                    content.height - settingsHeight});
    }
}

void tui_draw_profiles_tab(TuiCanvas* canvas, const TuiRect& content) {
    TuiLayout* out = canvas->layout();
    if (out->breakpoint == TUI_BREAKPOINT_WIDE) {
        int half = content.width / 2;
        int top = content.height / 2;
        draw_profile_panel(canvas, TuiRect{2, content.y, half - 1, top});
        draw_tools_panel(canvas,
            TuiRect{half + 1, content.y, content.width - half, top});
        draw_service_panel(canvas,
            TuiRect{2, content.y + top, content.width - 2,
                    content.height - top});
    } else {
        int profileHeight = content.height >= 26 ? 9 : 7;
        int toolsHeight = content.height >= 26 ? 9 : 7;
        draw_profile_panel(canvas,
            TuiRect{2, content.y, content.width - 2, profileHeight});
        draw_tools_panel(canvas,
            TuiRect{2, content.y + profileHeight,
                    content.width - 2, toolsHeight});
        int serviceHeight = content.height - profileHeight - toolsHeight;
        if (serviceHeight >= 5)
            draw_service_panel(canvas,
                TuiRect{2, content.y + profileHeight + toolsHeight,
                        content.width - 2, serviceHeight});
    }
}

void tui_draw_advanced_tab(TuiCanvas* canvas, const TuiRect& content) {
    draw_advanced_panel(canvas,
        TuiRect{2, content.y, content.width - 2, content.height});
}
