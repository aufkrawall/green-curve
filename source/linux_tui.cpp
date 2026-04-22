#include "linux_port.h"

#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <string>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <termios.h>
#include <unistd.h>
#include <vector>

namespace {

enum ActionType {
    ACTION_NONE = 0,
    ACTION_QUIT,
    ACTION_SAVE,
    ACTION_LOAD,
    ACTION_RESET,
    ACTION_PROBE,
    ACTION_WRITE_ASSETS,
    ACTION_SLOT_DELTA,
    ACTION_VF_PAGE_DELTA,
    ACTION_GPU_DELTA,
    ACTION_MEM_DELTA,
    ACTION_POWER_DELTA,
    ACTION_FAN_FIXED_DELTA,
    ACTION_FAN_MODE_SET,
    ACTION_POLL_DELTA,
    ACTION_HYST_DELTA,
    ACTION_FAN_POINT_ENABLE,
    ACTION_FAN_POINT_TEMP_DELTA,
    ACTION_FAN_POINT_PCT_DELTA,
    ACTION_VF_POINT_DELTA,
};

struct ClickAction {
    int x1;
    int y1;
    int x2;
    int y2;
    ActionType type;
    int index;
    int value;
};

struct TerminalGuard;
static TerminalGuard* g_activeTerminalGuard = nullptr;

static void restore_terminal_mode();

static void on_fatal_signal(int) {
    restore_terminal_mode();
    _exit(1);
}

struct TerminalGuard {
    termios original;
    bool active;

    TerminalGuard() : active(false) {
        if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) return;
        if (tcgetattr(STDIN_FILENO, &original) != 0) return;
        termios raw = original;
        cfmakeraw(&raw);
        raw.c_oflag |= OPOST;
        if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) != 0) return;
        active = true;
        g_activeTerminalGuard = this;
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
    bool running;
    char configPath[LINUX_PATH_MAX];
    char status[256];
    ProbeSummary probe;
    std::vector<ClickAction> actions;
};

static volatile sig_atomic_t g_resizeRequested = 1;

static void on_sigwinch(int) {
    g_resizeRequested = 1;
}

static int clamp_value(int value, int minimum, int maximum) {
    if (value < minimum) return minimum;
    if (value > maximum) return maximum;
    return value;
}

static void push_button(TuiState* state, std::string* line, int y, ActionType type, int index, int value, const char* label) {
    int x1 = (int)line->size() + 1;
    line->push_back('[');
    line->append(label);
    line->push_back(']');
    int x2 = (int)line->size();
    state->actions.push_back({ x1, y, x2, y, type, index, value });
}

static void append_text(std::string* line, const char* text) {
    line->append(text ? text : "");
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
        case ACTION_SLOT_DELTA:
            state->currentSlot = clamp_value(state->currentSlot + action.value, 1, CONFIG_NUM_SLOTS);
            snprintf(state->status, sizeof(state->status), "Selected profile slot %d", state->currentSlot);
            break;
        case ACTION_VF_PAGE_DELTA:
            state->vfPage = clamp_value(state->vfPage + action.value, 0, (VF_NUM_POINTS / 16) - 1);
            snprintf(state->status, sizeof(state->status), "VF page %d/8", state->vfPage + 1);
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

static void render_ui(TuiState* state) {
    winsize ws = {};
    ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws);
    int width = ws.ws_col ? ws.ws_col : 120;
    int height = ws.ws_row ? ws.ws_row : 40;

    state->actions.clear();
    fputs("\x1b[H\x1b[2J", stdout);
    if (width < 110 || height < 38) {
        printf("Green Curve Linux TUI\n\nTerminal too small. Need at least 110x38, got %dx%d.\nResize the terminal or use CLI mode.\n", width, height);
        fflush(stdout);
        return;
    }

    int y = 1;
    printf("\x1b[38;5;114mGreen Curve Linux TUI\x1b[0m  backend scaffold + config editor\n");
    y++;
    printf("Config: %s\n", state->configPath);
    y++;
    printf("Status: %s\n", state->status[0] ? state->status : "Ready. Use mouse or hotkeys: q s l p a r [ ] 1-5");
    y++;

    std::string line;
    line = "Profile slot: ";
    push_button(state, &line, y, ACTION_SLOT_DELTA, 0, -1, "<");
    append_text(&line, " ");
    char buffer[64] = {};
    snprintf(buffer, sizeof(buffer), "%d", state->currentSlot);
    append_text(&line, buffer);
    append_text(&line, " ");
    push_button(state, &line, y, ACTION_SLOT_DELTA, 0, 1, ">");
    append_text(&line, "   ");
    push_button(state, &line, y, ACTION_LOAD, 0, 0, "Load");
    append_text(&line, " ");
    push_button(state, &line, y, ACTION_SAVE, 0, 0, "Save");
    append_text(&line, " ");
    push_button(state, &line, y, ACTION_RESET, 0, 0, "Reset");
    y++;
    printf("%s\n", line.c_str());
    y++;

    printf("General controls\n");
    y++;
    line = "  GPU offset: ";
    push_button(state, &line, y, ACTION_GPU_DELTA, 0, -15, "-15");
    append_text(&line, " ");
    push_button(state, &line, y, ACTION_GPU_DELTA, 0, 15, "+15");
    append_text(&line, "   ");
    snprintf(buffer, sizeof(buffer), "%d MHz", state->desired.gpuOffsetMHz);
    append_text(&line, buffer);
    printf("%s\n", line.c_str());
    y++;

    line = "  Memory offset: ";
    push_button(state, &line, y, ACTION_MEM_DELTA, 0, -100, "-100");
    append_text(&line, " ");
    push_button(state, &line, y, ACTION_MEM_DELTA, 0, 100, "+100");
    append_text(&line, "   ");
    snprintf(buffer, sizeof(buffer), "%d MHz", state->desired.memOffsetMHz);
    append_text(&line, buffer);
    printf("%s\n", line.c_str());
    y++;

    line = "  Power limit: ";
    push_button(state, &line, y, ACTION_POWER_DELTA, 0, -5, "-5");
    append_text(&line, " ");
    push_button(state, &line, y, ACTION_POWER_DELTA, 0, 5, "+5");
    append_text(&line, "   ");
    snprintf(buffer, sizeof(buffer), "%d%%", state->desired.powerLimitPct);
    append_text(&line, buffer);
    printf("%s\n", line.c_str());
    y++;

    line = "  Fan mode: ";
    push_button(state, &line, y, ACTION_FAN_MODE_SET, 0, FAN_MODE_AUTO, state->desired.fanMode == FAN_MODE_AUTO ? "*Auto*" : "Auto");
    append_text(&line, " ");
    push_button(state, &line, y, ACTION_FAN_MODE_SET, 0, FAN_MODE_FIXED, state->desired.fanMode == FAN_MODE_FIXED ? "*Fixed*" : "Fixed");
    append_text(&line, " ");
    push_button(state, &line, y, ACTION_FAN_MODE_SET, 0, FAN_MODE_CURVE, state->desired.fanMode == FAN_MODE_CURVE ? "*Curve*" : "Curve");
    printf("%s\n", line.c_str());
    y++;

    line = "  Fan fixed pct: ";
    push_button(state, &line, y, ACTION_FAN_FIXED_DELTA, 0, -5, "-5");
    append_text(&line, " ");
    push_button(state, &line, y, ACTION_FAN_FIXED_DELTA, 0, 5, "+5");
    append_text(&line, "   ");
    snprintf(buffer, sizeof(buffer), "%d%%", state->desired.fanPercent);
    append_text(&line, buffer);
    printf("%s\n", line.c_str());
    y += 2;

    printf("Fan curve\n");
    y++;
    line = "  Poll interval: ";
    push_button(state, &line, y, ACTION_POLL_DELTA, 0, -250, "-250");
    append_text(&line, " ");
    push_button(state, &line, y, ACTION_POLL_DELTA, 0, 250, "+250");
    append_text(&line, "   ");
    snprintf(buffer, sizeof(buffer), "%d ms", state->desired.fanCurve.pollIntervalMs);
    append_text(&line, buffer);
    append_text(&line, "    Hysteresis: ");
    push_button(state, &line, y, ACTION_HYST_DELTA, 0, -1, "-1");
    append_text(&line, " ");
    push_button(state, &line, y, ACTION_HYST_DELTA, 0, 1, "+1");
    append_text(&line, "   ");
    snprintf(buffer, sizeof(buffer), "%d\xC2\xB0""C", state->desired.fanCurve.hysteresisC);
    append_text(&line, buffer);
    printf("%s\n", line.c_str());
    y++;

    for (int i = 0; i < FAN_CURVE_MAX_POINTS; i++) {
        line = "  ";
        snprintf(buffer, sizeof(buffer), "P%d ", i);
        append_text(&line, buffer);
        push_button(state, &line, y, ACTION_FAN_POINT_ENABLE, i, 0, state->desired.fanCurve.points[i].enabled ? "On" : "Off");
        append_text(&line, "  Temp ");
        push_button(state, &line, y, ACTION_FAN_POINT_TEMP_DELTA, i, -1, "-");
        append_text(&line, " ");
        push_button(state, &line, y, ACTION_FAN_POINT_TEMP_DELTA, i, 1, "+");
        append_text(&line, " ");
        snprintf(buffer, sizeof(buffer), "%3d\xC2\xB0""C", state->desired.fanCurve.points[i].temperatureC);
        append_text(&line, buffer);
        append_text(&line, "   Pct ");
        push_button(state, &line, y, ACTION_FAN_POINT_PCT_DELTA, i, -1, "-");
        append_text(&line, " ");
        push_button(state, &line, y, ACTION_FAN_POINT_PCT_DELTA, i, 1, "+");
        append_text(&line, " ");
        snprintf(buffer, sizeof(buffer), "%3d%%", state->desired.fanCurve.points[i].fanPercent);
        append_text(&line, buffer);
        printf("%s\n", line.c_str());
        y++;
    }

    y++;
    printf("VF curve page %d/8\n", state->vfPage + 1);
    y++;
    line = "  Page: ";
    push_button(state, &line, y, ACTION_VF_PAGE_DELTA, 0, -1, "Prev");
    append_text(&line, " ");
    push_button(state, &line, y, ACTION_VF_PAGE_DELTA, 0, 1, "Next");
    printf("%s\n", line.c_str());
    y++;

    int firstPoint = state->vfPage * 16;
    for (int row = 0; row < 16; row++) {
        int pointIndex = firstPoint + row;
        line = "  ";
        snprintf(buffer, sizeof(buffer), "P%03d ", pointIndex);
        append_text(&line, buffer);
        push_button(state, &line, y, ACTION_VF_POINT_DELTA, pointIndex, -100, "-100");
        append_text(&line, " ");
        push_button(state, &line, y, ACTION_VF_POINT_DELTA, pointIndex, -15, "-15");
        append_text(&line, " ");
        push_button(state, &line, y, ACTION_VF_POINT_DELTA, pointIndex, 15, "+15");
        append_text(&line, " ");
        push_button(state, &line, y, ACTION_VF_POINT_DELTA, pointIndex, 100, "+100");
        append_text(&line, "   ");
        snprintf(buffer, sizeof(buffer), "%4u MHz", state->desired.curvePointMHz[pointIndex]);
        append_text(&line, buffer);
        printf("%s\n", line.c_str());
        y++;
    }

    y++;
    line.clear();
    push_button(state, &line, y, ACTION_PROBE, 0, 0, "Probe");
    append_text(&line, " ");
    push_button(state, &line, y, ACTION_WRITE_ASSETS, 0, 0, "Write Assets");
    append_text(&line, " ");
    push_button(state, &line, y, ACTION_SAVE, 0, 0, "Save");
    append_text(&line, " ");
    push_button(state, &line, y, ACTION_LOAD, 0, 0, "Load");
    append_text(&line, " ");
    push_button(state, &line, y, ACTION_QUIT, 0, 0, "Quit");
    printf("%s\n", line.c_str());
    y++;

    if (state->probe.completed) {
        printf("Probe: %s\n", state->probe.summary);
        printf("Report: %s\n", state->probe.reportPath);
    } else {
        printf("Probe: not run in this session\n");
        printf("Backend apply note: VF/power/fan writes still need native Linux backend validation.\n");
    }

    fflush(stdout);
}

static bool parse_mouse_sequence(const std::string& sequence, int* x, int* y) {
    if (sequence.size() < 6) return false;
    if (sequence[0] != '\x1b' || sequence[1] != '[' || sequence[2] != '<') return false;
    int button = 0;
    int parsedX = 0;
    int parsedY = 0;
    char suffix = 0;
    if (sscanf(sequence.c_str(), "\x1b[<%d;%d;%d%c", &button, &parsedX, &parsedY, &suffix) != 4) return false;
    if (suffix != 'M') return false;
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
        case '[':
            apply_action(state, { 0, 0, 0, 0, ACTION_VF_PAGE_DELTA, 0, -1 });
            break;
        case ']':
            apply_action(state, { 0, 0, 0, 0, ACTION_VF_PAGE_DELTA, 0, 1 });
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
    for (const ClickAction& action : state->actions) {
        if (mouseX >= action.x1 && mouseX <= action.x2 && mouseY >= action.y1 && mouseY <= action.y2) {
            apply_action(state, action);
            return;
        }
    }
}

}  // namespace

int linux_run_tui(const char* configPath, int initialSlot, DesiredSettings* initialDesired) {
    TuiState state = {};
    if (configPath && *configPath) snprintf(state.configPath, sizeof(state.configPath), "%s", configPath);
    else default_linux_config_path(state.configPath, sizeof(state.configPath));

    if (initialDesired) state.desired = *initialDesired;
    else initialize_desired_settings_defaults(&state.desired);
    normalize_desired_settings_for_ui(&state.desired);

    state.currentSlot = initialSlot >= 1 && initialSlot <= CONFIG_NUM_SLOTS ? initialSlot : CONFIG_DEFAULT_SLOT;
    state.vfPage = 0;
    state.running = true;
    snprintf(state.status, sizeof(state.status), "Ready. Left click buttons or use hotkeys.");

    signal(SIGWINCH, on_sigwinch);
    signal(SIGINT, on_fatal_signal);
    signal(SIGTERM, on_fatal_signal);
    signal(SIGSEGV, on_fatal_signal);
    atexit(restore_terminal_mode);
    TerminalGuard guard;
    if (!guard.active) {
        fprintf(stderr, "Linux TUI requires an interactive terminal.\n");
        return 1;
    }

    while (state.running) {
        if (g_resizeRequested) {
            g_resizeRequested = 0;
            render_ui(&state);
        }

        std::string input;
        if (!read_input(&input)) continue;

        if (input.size() >= 6 && input[0] == '\x1b' && input[1] == '[' && input[2] == '<') {
            int mouseX = 0;
            int mouseY = 0;
            if (parse_mouse_sequence(input, &mouseX, &mouseY)) {
                handle_mouse(&state, mouseX, mouseY);
                render_ui(&state);
            }
            continue;
        }

        if (input.size() == 1 && input[0] == 27) {
            state.running = false;
        } else {
            for (char ch : input) {
                if (ch == 3) {
                    state.running = false;
                    break;
                }
                handle_hotkey(&state, ch);
            }
        }
        render_ui(&state);
    }

    return 0;
}
