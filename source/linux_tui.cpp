// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT

#include "linux_port.h"
#include "linux_tui_layout.h"
#include "linux_daemon.h"

#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <string>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>
#include <vector>

namespace {

struct TerminalGuard;
static TerminalGuard* g_activeTerminalGuard = nullptr;

static void restore_terminal_mode();

struct TerminalGuard {
    termios original;
    bool active;

    TerminalGuard() : active(false) {
        if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) return;
        if (tcgetattr(STDIN_FILENO, &original) != 0) return;
        termios raw = original;
        cfmakeraw(&raw);
        raw.c_oflag |= OPOST;  // keep ONLCR from `original` so '\n' still returns to column 1
        if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) != 0) return;
        active = true;
        g_activeTerminalGuard = this;
        // Alt screen, hide cursor, SGR mouse tracking (press/release, no motion).
        fputs("\x1b[?1049h\x1b[?25l\x1b[?1000h\x1b[?1006h", stdout);
        fflush(stdout);
    }

    ~TerminalGuard() {
        restore_terminal_mode();
        g_activeTerminalGuard = nullptr;
    }
};

static void restore_terminal_mode() {
    if (!g_activeTerminalGuard || !g_activeTerminalGuard->active) return;
    fputs("\x1b[?1006l\x1b[?1000l\x1b[?25h\x1b[?1049l", stdout);
    fflush(stdout);
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &g_activeTerminalGuard->original);
    g_activeTerminalGuard->active = false;
}

struct TuiState {
    DesiredSettings desired;
    int currentSlot;
    int vfPage;
    int focusIndex;  // index into `actions` highlighted for keyboard use; -1 = none
    bool running;
    char configPath[LINUX_PATH_MAX];
    char status[256];
    ProbeSummary probe;
    ServiceSnapshot daemonSnapshot;
    GpuAdapterInfo targetGpu;
    std::vector<ClickAction> actions;
};

static volatile sig_atomic_t g_resizeRequested = 1;
static volatile sig_atomic_t g_stopRequested = 0;

static void on_sigwinch(int) {
    g_resizeRequested = 1;
}

static void on_stop_signal(int) {
    g_stopRequested = 1;
}

static int clamp_value(int value, int minimum, int maximum) {
    if (value < minimum) return minimum;
    if (value > maximum) return maximum;
    return value;
}

static void load_slot(TuiState* state) {
    char err[256] = {};
    if (load_profile_from_config_path(state->configPath, state->currentSlot, &state->desired, err, sizeof(err))) {
        normalize_desired_settings_for_ui(&state->desired);
        snprintf(state->status, sizeof(state->status), "Loaded profile %d from %s", state->currentSlot, state->configPath);
    } else {
        snprintf(state->status, sizeof(state->status), "%s", err[0] ? err : "Profile load failed");
    }
}

static void save_slot(TuiState* state) {
    char err[256] = {};
    normalize_desired_settings_for_ui(&state->desired);
    if (save_profile_to_config_path(state->configPath, state->currentSlot, &state->desired, err, sizeof(err))) {
        snprintf(state->status, sizeof(state->status), "Saved profile %d to %s", state->currentSlot, state->configPath);
    } else {
        snprintf(state->status, sizeof(state->status), "%s", err[0] ? err : "Profile save failed");
    }
}

static void reset_desired(TuiState* state) {
    initialize_desired_settings_defaults(&state->desired);
    normalize_desired_settings_for_ui(&state->desired);
    snprintf(state->status, sizeof(state->status), "Reset in-memory settings to Linux defaults");
}

static void run_probe_from_tui(TuiState* state) {
    char err[256] = {};
    char outputPath[LINUX_PATH_MAX] = {};
    default_probe_output_path(state->configPath, outputPath, sizeof(outputPath));
    if (run_linux_probe(outputPath, &state->probe, err, sizeof(err))) {
        snprintf(state->status, sizeof(state->status), "Linux probe written to %s", outputPath);
    } else {
        snprintf(state->status, sizeof(state->status), "%s", err[0] ? err : "Linux probe failed");
    }
}

static void write_assets_from_tui(TuiState* state) {
    char err[256] = {};
    char outputDir[LINUX_PATH_MAX] = {};
    char exePath[LINUX_PATH_MAX] = {};
    default_assets_output_dir(state->configPath, outputDir, sizeof(outputDir));
    if (!get_executable_path(exePath, sizeof(exePath))) {
        snprintf(state->status, sizeof(state->status), "Failed to resolve /proc/self/exe");
        return;
    }
    if (write_linux_assets(outputDir, exePath, state->configPath, err, sizeof(err))) {
        snprintf(state->status, sizeof(state->status), "Linux assets written to %s", outputDir);
    } else {
        snprintf(state->status, sizeof(state->status), "%s", err[0] ? err : "Asset generation failed");
    }
}

static void set_daemon_status(TuiState* state, bool ok, const char* result, const char* okFallback, const char* failFallback) {
    if (ok) {
        snprintf(state->status, sizeof(state->status), "%s", (result && result[0]) ? result : okFallback);
    } else {
        snprintf(state->status, sizeof(state->status), "%s", (result && result[0]) ? result : failFallback);
    }
}

static void apply_to_daemon(TuiState* state) {
    normalize_desired_settings_for_ui(&state->desired);
    char result[512] = {};
    bool ok = linux_daemon_apply(state->targetGpu.valid ? &state->targetGpu : nullptr,
                                 &state->desired, true, result, sizeof(result));
    set_daemon_status(state, ok, result, "Applied current settings to the GPU via the daemon", "Apply failed");
}

static void reset_gpu_via_daemon(TuiState* state) {
    char result[512] = {};
    bool ok = linux_daemon_reset(state->targetGpu.valid ? &state->targetGpu : nullptr,
                                 result, sizeof(result));
    set_daemon_status(state, ok, result, "GPU reset to driver defaults via the daemon", "GPU reset failed");
}

static void apply_action(TuiState* state, const ClickAction& action) {
    switch (action.type) {
        case ACTION_QUIT:
            state->running = false;
            break;
        case ACTION_SAVE:
            save_slot(state);
            break;
        case ACTION_LOAD:
            load_slot(state);
            break;
        case ACTION_RESET:
            reset_desired(state);
            break;
        case ACTION_PROBE:
            run_probe_from_tui(state);
            break;
        case ACTION_WRITE_ASSETS:
            write_assets_from_tui(state);
            break;
        case ACTION_APPLY:
            apply_to_daemon(state);
            break;
        case ACTION_APPLY_RESET:
            reset_gpu_via_daemon(state);
            break;
        case ACTION_SLOT_DELTA:
            state->currentSlot = clamp_value(state->currentSlot + action.value, 1, CONFIG_NUM_SLOTS);
            snprintf(state->status, sizeof(state->status), "Selected profile slot %d", state->currentSlot);
            break;
        case ACTION_GPU_SELECT_DELTA: {
            unsigned int count = state->daemonSnapshot.adapterCount;
            if (count == 0) {
                snprintf(state->status, sizeof(state->status), "Daemon reported no selectable GPUs");
                break;
            }
            int current = -1;
            for (unsigned int i = 0; i < count; ++i) {
                const GpuAdapterInfo& adapter = state->daemonSnapshot.adapters[i];
                if (adapter.valid && state->targetGpu.valid &&
                    adapter.pciDomain == state->targetGpu.pciDomain &&
                    adapter.pciBus == state->targetGpu.pciBus &&
                    adapter.pciDevice == state->targetGpu.pciDevice &&
                    adapter.pciFunction == state->targetGpu.pciFunction) {
                    current = (int)i;
                    break;
                }
            }
            int next = current < 0 ? 0 : (current + action.value + (int)count) % (int)count;
            state->targetGpu = state->daemonSnapshot.adapters[next];
            char persistErr[256] = {};
            char bdf[32] = {};
            format_linux_gpu_bdf(&state->targetGpu, bdf, sizeof(bdf));
            if (save_linux_gpu_selection(state->configPath, &state->targetGpu,
                                         persistErr, sizeof(persistErr)))
                snprintf(state->status, sizeof(state->status), "Selected GPU %s (%s)",
                         bdf, state->targetGpu.name);
            else
                snprintf(state->status, sizeof(state->status), "GPU selection save failed: %s", persistErr);
            break;
        }
        case ACTION_VF_PAGE_DELTA:
            state->vfPage = clamp_value(state->vfPage + action.value, 0, (VF_NUM_POINTS / 16) - 1);
            snprintf(state->status, sizeof(state->status), "VF page %d/%d", state->vfPage + 1, VF_NUM_POINTS / 16);
            break;
        case ACTION_GPU_DELTA:
            state->desired.hasGpuOffset = true;
            state->desired.gpuOffsetMHz += action.value;
            snprintf(state->status, sizeof(state->status), "GPU offset now %d MHz", state->desired.gpuOffsetMHz);
            break;
        case ACTION_MEM_DELTA:
            state->desired.hasMemOffset = true;
            state->desired.memOffsetMHz += action.value;
            snprintf(state->status, sizeof(state->status), "Memory offset now %d MHz", state->desired.memOffsetMHz);
            break;
        case ACTION_POWER_DELTA:
            state->desired.hasPowerLimit = true;
            state->desired.powerLimitPct = clamp_value(state->desired.powerLimitPct + action.value, 50, 150);
            snprintf(state->status, sizeof(state->status), "Power limit now %d%%", state->desired.powerLimitPct);
            break;
        case ACTION_FAN_FIXED_DELTA:
            state->desired.hasFan = true;
            if (state->desired.fanMode == FAN_MODE_AUTO) state->desired.fanMode = FAN_MODE_FIXED;
            state->desired.fanAuto = false;
            state->desired.fanPercent = clamp_value(state->desired.fanPercent + action.value, 0, 100);
            snprintf(state->status, sizeof(state->status), "Fixed fan now %d%%", state->desired.fanPercent);
            break;
        case ACTION_FAN_MODE_SET:
            state->desired.hasFan = true;
            state->desired.fanMode = action.value;
            state->desired.fanAuto = action.value == FAN_MODE_AUTO;
            snprintf(state->status, sizeof(state->status), "Fan mode set to %s", fan_mode_label(action.value));
            break;
        case ACTION_POLL_DELTA:
            state->desired.hasFan = true;
            state->desired.fanMode = FAN_MODE_CURVE;
            state->desired.fanCurve.pollIntervalMs = clamp_value(state->desired.fanCurve.pollIntervalMs + action.value, 250, 5000);
            state->desired.fanCurve.pollIntervalMs = ((state->desired.fanCurve.pollIntervalMs + 125) / 250) * 250;
            snprintf(state->status, sizeof(state->status), "Fan poll interval now %d ms", state->desired.fanCurve.pollIntervalMs);
            break;
        case ACTION_HYST_DELTA:
            state->desired.hasFan = true;
            state->desired.fanMode = FAN_MODE_CURVE;
            state->desired.fanCurve.hysteresisC = clamp_value(state->desired.fanCurve.hysteresisC + action.value, 0, FAN_CURVE_MAX_HYSTERESIS_C);
            snprintf(state->status, sizeof(state->status), "Fan hysteresis now %d\xC2\xB0""C", state->desired.fanCurve.hysteresisC);
            break;
        case ACTION_FAN_POINT_ENABLE:
            state->desired.hasFan = true;
            state->desired.fanMode = FAN_MODE_CURVE;
            state->desired.fanCurve.points[action.index].enabled = !state->desired.fanCurve.points[action.index].enabled;
            snprintf(state->status, sizeof(state->status), "Fan point %d %s", action.index, state->desired.fanCurve.points[action.index].enabled ? "enabled" : "disabled");
            break;
        case ACTION_FAN_POINT_TEMP_DELTA:
            state->desired.hasFan = true;
            state->desired.fanMode = FAN_MODE_CURVE;
            state->desired.fanCurve.points[action.index].enabled = true;
            state->desired.fanCurve.points[action.index].temperatureC = clamp_value(state->desired.fanCurve.points[action.index].temperatureC + action.value, 0, 100);
            snprintf(state->status, sizeof(state->status), "Fan point %d temp now %d\xC2\xB0""C", action.index, state->desired.fanCurve.points[action.index].temperatureC);
            break;
        case ACTION_FAN_POINT_PCT_DELTA:
            state->desired.hasFan = true;
            state->desired.fanMode = FAN_MODE_CURVE;
            state->desired.fanCurve.points[action.index].enabled = true;
            state->desired.fanCurve.points[action.index].fanPercent = clamp_value(state->desired.fanCurve.points[action.index].fanPercent + action.value, 0, 100);
            snprintf(state->status, sizeof(state->status), "Fan point %d pct now %d%%", action.index, state->desired.fanCurve.points[action.index].fanPercent);
            break;
        case ACTION_VF_POINT_DELTA: {
            int pointIndex = action.index;
            state->desired.hasCurvePoint[pointIndex] = true;
            int nextValue = (int)state->desired.curvePointMHz[pointIndex] + action.value;
            if (nextValue < 0) nextValue = 0;
            state->desired.curvePointMHz[pointIndex] = (unsigned int)nextValue;
            snprintf(state->status, sizeof(state->status), "VF point %d now %u MHz", pointIndex, state->desired.curvePointMHz[pointIndex]);
            break;
        }
        default:
            break;
    }

    normalize_desired_settings_for_ui(&state->desired);
}

// Print one screen row, clipping to the terminal width (so a long info line can
// never wrap and push every hitbox below it down by a row) and splicing in a
// reverse-video highlight for the keyboard-focused button.
static void print_row(const std::string& lineIn, int width, int rowIndex,
                      const TuiState* state) {
    std::string line = lineIn;
    if (tui_display_columns(line) > width) {
        int cut = tui_column_to_byte_offset(line, width + 1);
        line.erase(cut);
    }

    if (rowIndex == 0) {
        printf("\x1b[38;5;114m%s\x1b[0m\n", line.c_str());
        return;
    }

    if (state->focusIndex >= 0 && state->focusIndex < (int)state->actions.size()) {
        const ClickAction& f = state->actions[state->focusIndex];
        int be = f.byteStart + f.byteLen;
        if (f.y1 - 1 == rowIndex && f.byteStart >= 0 && be <= (int)line.size()) {
            printf("%s\x1b[7m%s\x1b[27m%s\n",
                   line.substr(0, f.byteStart).c_str(),
                   line.substr(f.byteStart, f.byteLen).c_str(),
                   line.substr(be).c_str());
            return;
        }
    }

    printf("%s\n", line.c_str());
}

static void render_ui(TuiState* state) {
    winsize ws = {};
    ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws);
    int width = ws.ws_col ? ws.ws_col : 120;
    int height = ws.ws_row ? ws.ws_row : 40;

    TuiViewModel vm = {};
    vm.desired = &state->desired;
    vm.currentSlot = state->currentSlot;
    vm.vfPage = state->vfPage;
    char selectedGpu[160] = {};
    char selectedBdf[32] = {};
    format_linux_gpu_bdf(&state->targetGpu, selectedBdf, sizeof(selectedBdf));
    snprintf(selectedGpu, sizeof(selectedGpu), "%s %s", selectedBdf,
             state->targetGpu.name[0] ? state->targetGpu.name : "");
    vm.selectedGpu = selectedGpu;
    vm.gpuCount = state->daemonSnapshot.adapterCount;
    vm.configPath = state->configPath;
    vm.status = state->status;
    vm.probeCompleted = state->probe.completed;
    vm.probeSummary = state->probe.summary;
    vm.probeReportPath = state->probe.reportPath;

    TuiLayout layout;
    build_tui_layout(vm, &layout);

    fputs("\x1b[H\x1b[2J", stdout);

    if (width < layout.requiredCols || height < layout.requiredRows) {
        // Content is not drawn, so no hitbox is valid: drop them so a stray
        // click / focus key cannot act on an off-screen button.
        state->actions.clear();
        state->focusIndex = -1;
        printf("Green Curve Linux TUI\n\nTerminal too small. Need at least %dx%d, got %dx%d.\n"
               "Resize the terminal or use CLI mode (greencurve --help).\n",
               layout.requiredCols, layout.requiredRows, width, height);
        fflush(stdout);
        return;
    }

    state->actions = layout.actions;
    int actionCount = (int)state->actions.size();
    if (actionCount == 0) {
        state->focusIndex = -1;
    } else {
        if (state->focusIndex < 0) state->focusIndex = 0;
        if (state->focusIndex >= actionCount) state->focusIndex = actionCount - 1;
    }

    for (int i = 0; i < (int)layout.lines.size(); i++) {
        print_row(layout.lines[i], width, i, state);
    }
    fflush(stdout);
}

// Parse an SGR mouse report and accept it only as a left-button *press*.
// `?1000h` reports wheel scroll as buttons 64/65 (still an 'M' suffix), which
// must not be treated as a click on whatever button is under the cursor.
static bool parse_mouse_click(const std::string& sequence, int* x, int* y) {
    if (sequence.size() < 6) return false;
    if (sequence[0] != '\x1b' || sequence[1] != '[' || sequence[2] != '<') return false;
    int button = 0;
    int parsedX = 0;
    int parsedY = 0;
    char suffix = 0;
    if (sscanf(sequence.c_str(), "\x1b[<%d;%d;%d%c", &button, &parsedX, &parsedY, &suffix) != 4) return false;
    if (suffix != 'M') return false;      // 'M' = press, 'm' = release
    if (button & 64) return false;        // wheel scroll, not a click
    if ((button & 3) != 0) return false;  // only the left button activates
    if (x) *x = parsedX;
    if (y) *y = parsedY;
    return true;
}

static bool read_input(std::string* input) {
    input->clear();
    fd_set readSet;
    FD_ZERO(&readSet);
    FD_SET(STDIN_FILENO, &readSet);
    timeval timeout = { 0, 200000 };
    int ready = select(STDIN_FILENO + 1, &readSet, nullptr, nullptr, &timeout);
    if (ready <= 0) return false;

    char buffer[64] = {};
    ssize_t readCount = read(STDIN_FILENO, buffer, sizeof(buffer));
    if (readCount <= 0) return false;
    input->assign(buffer, buffer + readCount);
    return true;
}

// Move keyboard focus to the previous/next button in natural order.
static void focus_step_linear(TuiState* state, int dir) {
    int n = (int)state->actions.size();
    if (n == 0) return;
    if (state->focusIndex < 0) {
        state->focusIndex = dir > 0 ? 0 : n - 1;
        return;
    }
    state->focusIndex = ((state->focusIndex + dir) % n + n) % n;
}

// Move focus to the nearest button on the closest row above/below, preferring
// the button whose start column is nearest the current one.
static void focus_step_vertical(TuiState* state, int dir) {
    int n = (int)state->actions.size();
    if (n == 0) return;
    if (state->focusIndex < 0) {
        state->focusIndex = 0;
        return;
    }
    const ClickAction& cur = state->actions[state->focusIndex];
    int best = -1;
    int bestRow = 0;
    int bestColDist = 0;
    for (int i = 0; i < n; i++) {
        const ClickAction& a = state->actions[i];
        if (dir > 0 ? (a.y1 <= cur.y1) : (a.y1 >= cur.y1)) continue;
        int colDist = a.x1 > cur.x1 ? a.x1 - cur.x1 : cur.x1 - a.x1;
        bool nearerRow = dir > 0 ? (a.y1 < bestRow) : (a.y1 > bestRow);
        if (best < 0 || nearerRow || (a.y1 == bestRow && colDist < bestColDist)) {
            best = i;
            bestRow = a.y1;
            bestColDist = colDist;
        }
    }
    if (best >= 0) state->focusIndex = best;
}

static void activate_focus(TuiState* state) {
    if (state->focusIndex >= 0 && state->focusIndex < (int)state->actions.size()) {
        apply_action(state, state->actions[state->focusIndex]);
    }
}

static void handle_hotkey(TuiState* state, char key) {
    switch (key) {
        case 'q':
        case 'Q':
            state->running = false;
            break;
        case 's':
        case 'S':
            save_slot(state);
            break;
        case 'l':
        case 'L':
            load_slot(state);
            break;
        case 'p':
        case 'P':
            run_probe_from_tui(state);
            break;
        case 'a':
        case 'A':
            write_assets_from_tui(state);
            break;
        case 'r':
        case 'R':
            reset_desired(state);
            break;
        case 'g':
        case 'G':
            apply_to_daemon(state);
            break;
        case '[':
            apply_action(state, ClickAction{ 0, 0, 0, 0, 0, 0, ACTION_VF_PAGE_DELTA, 0, -1 });
            break;
        case ']':
            apply_action(state, ClickAction{ 0, 0, 0, 0, 0, 0, ACTION_VF_PAGE_DELTA, 0, 1 });
            break;
        case '1':
        case '2':
        case '3':
        case '4':
        case '5':
            state->currentSlot = key - '0';
            snprintf(state->status, sizeof(state->status), "Selected profile slot %d", state->currentSlot);
            break;
        default:
            break;
    }
}

static void handle_mouse(TuiState* state, int mouseX, int mouseY) {
    for (int i = 0; i < (int)state->actions.size(); i++) {
        const ClickAction& action = state->actions[i];
        if (mouseX >= action.x1 && mouseX <= action.x2 && mouseY >= action.y1 && mouseY <= action.y2) {
            state->focusIndex = i;  // let keyboard focus follow the click
            apply_action(state, action);
            return;
        }
    }
}

}  // namespace

static int linux_run_tui_child(const char* configPath, int initialSlot,
                  DesiredSettings* initialDesired,
                  const GpuAdapterInfo* initialTarget) {
    TuiState state = {};
    if (configPath && *configPath) snprintf(state.configPath, sizeof(state.configPath), "%s", configPath);
    else default_linux_config_path(state.configPath, sizeof(state.configPath));

    if (initialDesired) state.desired = *initialDesired;
    else initialize_desired_settings_defaults(&state.desired);
    normalize_desired_settings_for_ui(&state.desired);

    state.currentSlot = initialSlot >= 1 && initialSlot <= CONFIG_NUM_SLOTS ? initialSlot : CONFIG_DEFAULT_SLOT;
    state.vfPage = 0;
    state.focusIndex = 0;
    state.running = true;
    if (initialTarget) state.targetGpu = *initialTarget;
    char daemonErr[256] = {};
    if (linux_daemon_snapshot(&state.daemonSnapshot, daemonErr, sizeof(daemonErr))) {
        if (!state.targetGpu.valid && state.daemonSnapshot.adapterCount == 1)
            state.targetGpu = state.daemonSnapshot.adapters[0];
    } else {
        snprintf(state.status, sizeof(state.status), "%s", daemonErr);
    }
    if (!state.status[0])
        snprintf(state.status, sizeof(state.status), "Ready. Click a button, or use the keyboard (arrows/Tab/Enter).");

    struct sigaction resizeAction = {};
    resizeAction.sa_handler = on_sigwinch;
    sigemptyset(&resizeAction.sa_mask);
    sigaction(SIGWINCH, &resizeAction, nullptr);
    struct sigaction stopAction = {};
    stopAction.sa_handler = on_stop_signal;
    sigemptyset(&stopAction.sa_mask);
    sigaction(SIGINT, &stopAction, nullptr);
    sigaction(SIGTERM, &stopAction, nullptr);
    TerminalGuard guard;
    if (!guard.active) {
        fprintf(stderr, "Linux TUI requires an interactive terminal.\n");
        return 1;
    }

    while (state.running && !g_stopRequested) {
        if (g_resizeRequested) {
            g_resizeRequested = 0;
            render_ui(&state);
        }

        std::string input;
        if (!read_input(&input)) continue;

        // Mouse (SGR) reports.
        if (input.size() >= 6 && input[0] == '\x1b' && input[1] == '[' && input[2] == '<') {
            int mouseX = 0;
            int mouseY = 0;
            if (parse_mouse_click(input, &mouseX, &mouseY)) {
                handle_mouse(&state, mouseX, mouseY);
            }
            render_ui(&state);
            continue;
        }

        // Arrow keys / navigation escape sequences (CSI or SS3 form).
        if (input.size() >= 3 && input[0] == '\x1b' && (input[1] == '[' || input[1] == 'O')) {
            switch (input[2]) {
                case 'A': focus_step_vertical(&state, -1); break;  // up
                case 'B': focus_step_vertical(&state, +1); break;  // down
                case 'C': focus_step_linear(&state, +1); break;    // right
                case 'D': focus_step_linear(&state, -1); break;    // left
                case 'Z': focus_step_linear(&state, -1); break;    // shift-tab
                default: break;
            }
            render_ui(&state);
            continue;
        }

        // Lone ESC quits.
        if (input.size() == 1 && (unsigned char)input[0] == 27) {
            state.running = false;
            render_ui(&state);
            continue;
        }

        for (char ch : input) {
            if (ch == 3) {  // Ctrl-C
                state.running = false;
                break;
            }
            if (ch == '\t') {
                focus_step_linear(&state, +1);
                continue;
            }
            if (ch == '\r' || ch == '\n' || ch == ' ') {
                activate_focus(&state);
                continue;
            }
            handle_hotkey(&state, ch);
        }
        render_ui(&state);
    }

    return 0;
}

int linux_run_tui(const char* configPath, int initialSlot,
                  DesiredSettings* initialDesired,
                  const GpuAdapterInfo* initialTarget) {
    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) {
        fprintf(stderr, "Linux TUI requires an interactive terminal.\n");
        return 1;
    }
    termios original = {};
    if (tcgetattr(STDIN_FILENO, &original) != 0) {
        fprintf(stderr, "Cannot capture terminal mode: %s\n", strerror(errno));
        return 1;
    }
    pid_t child = fork();
    if (child < 0) {
        fprintf(stderr, "Cannot start TUI supervisor: %s\n", strerror(errno));
        return 1;
    }
    if (child == 0) {
        struct sigaction defaultAction = {};
        defaultAction.sa_handler = SIG_DFL;
        sigemptyset(&defaultAction.sa_mask);
        sigaction(SIGINT, &defaultAction, nullptr);
        sigaction(SIGTERM, &defaultAction, nullptr);
        sigaction(SIGSEGV, &defaultAction, nullptr);
        int status = linux_run_tui_child(configPath, initialSlot,
            initialDesired, initialTarget);
        _exit(status);
    }

    struct sigaction ignoreAction = {};
    ignoreAction.sa_handler = SIG_IGN;
    sigemptyset(&ignoreAction.sa_mask);
    struct sigaction oldInt = {}, oldTerm = {};
    sigaction(SIGINT, &ignoreAction, &oldInt);
    sigaction(SIGTERM, &ignoreAction, &oldTerm);
    int childStatus = 0;
    while (waitpid(child, &childStatus, 0) < 0) {
        if (errno == EINTR) continue;
        childStatus = 1 << 8;
        break;
    }
    sigaction(SIGINT, &oldInt, nullptr);
    sigaction(SIGTERM, &oldTerm, nullptr);

    // The supervisor never enters raw mode and can use ordinary terminal APIs
    // even when the child terminates through SIGSEGV or SIGABRT.
    fputs("\x1b[?1006l\x1b[?1000l\x1b[?25h\x1b[?1049l", stdout);
    fflush(stdout);
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &original);
    if (WIFSIGNALED(childStatus)) {
        int signalNumber = WTERMSIG(childStatus);
        fprintf(stderr,
            "Green Curve TUI terminated by signal %d; terminal state restored.\n",
            signalNumber);
        return 128 + signalNumber;
    }
    return WIFEXITED(childStatus) ? WEXITSTATUS(childStatus) : 1;
}
