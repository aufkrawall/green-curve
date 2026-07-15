// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT

#include "linux_tui_internal.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <unistd.h>

namespace {

const char* style_escape(TuiStyle style, bool focus) {
    static const char* normal[TUI_STYLE_COUNT] = {
        "\x1b[0;38;2;189;201;220;48;2;14;18;32m",
        "\x1b[0;38;2;189;201;220;48;2;17;23;41m",
        "\x1b[0;38;2;85;115;155;48;2;14;18;32m",
        "\x1b[1;38;2;231;237;247;48;2;17;23;41m",
        "\x1b[0;38;2;231;237;247;48;2;14;18;32m",
        "\x1b[0;38;2;129;144;170;48;2;14;18;32m",
        "\x1b[0;38;2;70;83;108;48;2;14;18;32m",
        "\x1b[1;38;2;66;211;146;48;2;14;18;32m",
        "\x1b[1;38;2;98;183;255;48;2;14;18;32m",
        "\x1b[1;38;2;242;184;75;48;2;14;18;32m",
        "\x1b[1;38;2;255;111;125;48;2;14;18;32m",
        "\x1b[1;38;2;183;156;255;48;2;14;18;32m",
        "\x1b[0;38;2;189;201;220;48;2;10;15;27m",
        "\x1b[1;38;2;66;211;146;48;2;24;61;52m",
        "\x1b[1;38;2;231;237;247;48;2;8;13;23m",
        "\x1b[1;38;2;98;183;255;48;2;8;25;40m",
        "\x1b[0;38;2;189;201;220;48;2;14;21;36m",
        "\x1b[1;38;2;231;237;247;48;2;24;51;74m",
        "\x1b[0;38;2;129;144;170;48;2;16;35;31m",
    };
    static const char* focused[TUI_STYLE_COUNT] = {
        "\x1b[0;7;38;2;231;237;247;48;2;20;62;86m",
        "\x1b[0;7;38;2;231;237;247;48;2;20;62;86m",
        "\x1b[0;7;38;2;231;237;247;48;2;20;62;86m",
        "\x1b[1;7;38;2;255;255;255;48;2;20;62;86m",
        "\x1b[0;7;38;2;255;255;255;48;2;20;62;86m",
        "\x1b[0;7;38;2;231;237;247;48;2;20;62;86m",
        "\x1b[0;7;38;2;189;201;220;48;2;20;62;86m",
        "\x1b[1;7;38;2;66;211;146;48;2;20;62;86m",
        "\x1b[1;7;38;2;98;183;255;48;2;20;62;86m",
        "\x1b[1;7;38;2;242;184;75;48;2;20;62;86m",
        "\x1b[1;7;38;2;255;111;125;48;2;20;62;86m",
        "\x1b[1;7;38;2;183;156;255;48;2;20;62;86m",
        "\x1b[0;7;38;2;255;255;255;48;2;20;62;86m",
        "\x1b[1;7;38;2;66;211;146;48;2;20;62;86m",
        "\x1b[1;7;38;2;255;255;255;48;2;20;62;86m",
        "\x1b[1;7;38;2;98;183;255;48;2;20;62;86m",
        "\x1b[0;7;38;2;231;237;247;48;2;20;62;86m",
        "\x1b[1;7;38;2;255;255;255;48;2;20;62;86m",
        "\x1b[0;7;38;2;189;201;220;48;2;20;62;86m",
    };
    int index = (int)style;
    if (index < 0 || index >= TUI_STYLE_COUNT) index = 0;
    return focus ? focused[index] : normal[index];
}

bool cell_has_focus(const TuiState& state, int x, int y) {
    if (state.focusIndex < 0 ||
        state.focusIndex >= (int)state.layout.actions.size()) return false;
    const ClickAction& action = state.layout.actions[state.focusIndex];
    return x >= action.x1 && x <= action.x2 &&
           y >= action.y1 && y <= action.y2;
}

std::string rendered_row(const TuiState& state, int y) {
    std::string row;
    TuiStyle activeStyle = TUI_STYLE_COUNT;
    bool activeFocus = false;
    for (int x = 1; x <= state.layout.width; ++x) {
        const TuiCell& cell = state.layout.cells[
            (size_t)(y - 1) * state.layout.width + (x - 1)];
        TuiStyle style = (TuiStyle)cell.style;
        bool focus = cell_has_focus(state, x, y);
        if (style != activeStyle || focus != activeFocus) {
            row += style_escape(style, focus);
            activeStyle = style;
            activeFocus = focus;
        }
        row += cell.glyph[0] ? cell.glyph : " ";
    }
    row += "\x1b[0m";
    return row;
}

void populate_view_model(const TuiState& state, TuiViewModel* vm,
                         char selectedGpu[192]) {
    *vm = TuiViewModel{};
    vm->desired = &state.desired;
    vm->service = state.serviceOnline ? &state.service : nullptr;
    vm->currentSlot = state.currentSlot;
    vm->tab = state.tab;
    vm->selectedPoint = state.selectedPoint;
    vm->vfScroll = state.vfScroll;
    vm->fanScroll = state.fanScroll;
    vm->focusIndex = state.focusIndex;
    vm->dirty = state.dirty;
    vm->serviceOnline = state.serviceOnline;
    vm->editing = state.edit.active;
    vm->editField = state.edit.field;
    vm->editIndex = state.edit.index;
    vm->editText = state.edit.text;
    vm->configPath = state.configPath;
    vm->status = state.status;
    vm->gpuCount = state.service.snapshot.adapterCount;
    vm->probeCompleted = state.probe.completed;
    vm->probeSummary = state.probe.summary;
    vm->probeReportPath = state.probe.reportPath;
    char bdf[32] = {};
    format_linux_gpu_bdf(&state.targetGpu, bdf, sizeof(bdf));
    snprintf(selectedGpu, 192, "%s  %s", bdf,
             state.targetGpu.name[0] ? state.targetGpu.name : "NVIDIA GPU");
    vm->selectedGpu = selectedGpu;
}

bool selected_is_visible(const TuiState& state) {
    if (state.layout.vfVisibleRows <= 0 || !state.serviceOnline) return true;
    int drawn = 0;
    for (int i = state.vfScroll; i < VF_NUM_POINTS &&
            drawn < state.layout.vfVisibleRows; ++i) {
        if (state.service.snapshot.curve[i].freq_kHz == 0) continue;
        if (i == state.selectedPoint) return true;
        ++drawn;
    }
    return false;
}

void reveal_selected_point(TuiState* state) {
    if (state->selectedPoint < 0 || !state->serviceOnline ||
        state->layout.vfVisibleRows <= 0 || selected_is_visible(*state)) return;
    int first = state->selectedPoint;
    int remaining = state->layout.vfVisibleRows - 1;
    for (int i = state->selectedPoint - 1; i >= 0 && remaining > 0; --i) {
        if (state->service.snapshot.curve[i].freq_kHz == 0) continue;
        first = i;
        --remaining;
    }
    state->vfScroll = first;
}

void focus_linear(TuiState* state, int direction) {
    int count = (int)state->layout.actions.size();
    if (count <= 0) { state->focusIndex = -1; return; }
    if (state->focusIndex < 0) {
        state->focusIndex = direction > 0 ? 0 : count - 1;
        return;
    }
    state->focusIndex = ((state->focusIndex + direction) % count + count) % count;
}

void focus_spatial(TuiState* state, int dx, int dy) {
    int count = (int)state->layout.actions.size();
    if (count <= 0) return;
    if (state->focusIndex < 0 || state->focusIndex >= count) {
        state->focusIndex = 0;
        return;
    }
    const ClickAction& current = state->layout.actions[state->focusIndex];
    int currentX = current.x1 + current.x2;
    int currentY = current.y1 + current.y2;
    int best = -1;
    long bestScore = 0;
    for (int i = 0; i < count; ++i) {
        if (i == state->focusIndex) continue;
        const ClickAction& candidate = state->layout.actions[i];
        int candidateX = candidate.x1 + candidate.x2;
        int candidateY = candidate.y1 + candidate.y2;
        int deltaX = candidateX - currentX;
        int deltaY = candidateY - currentY;
        if ((dx < 0 && deltaX >= 0) || (dx > 0 && deltaX <= 0) ||
            (dy < 0 && deltaY >= 0) || (dy > 0 && deltaY <= 0)) continue;
        long primary = dx ? (deltaX < 0 ? -deltaX : deltaX)
                          : (deltaY < 0 ? -deltaY : deltaY);
        long secondary = dx ? (deltaY < 0 ? -deltaY : deltaY)
                            : (deltaX < 0 ? -deltaX : deltaX);
        long score = primary * 1000L + secondary;
        if (best < 0 || score < bestScore) {
            best = i;
            bestScore = score;
        }
    }
    if (best >= 0) state->focusIndex = best;
}

void activate_focused(TuiState* state) {
    if (state->focusIndex >= 0 &&
        state->focusIndex < (int)state->layout.actions.size())
        tui_apply_action(state, state->layout.actions[state->focusIndex]);
}

void scroll_current_tab(TuiState* state, int direction, bool page) {
    if (state->tab == TUI_TAB_VF) {
        int amount = page ? state->layout.vfVisibleRows : 3;
        if (amount < 1) amount = 1;
        state->vfScroll += direction * amount;
        if (state->vfScroll < 0) state->vfScroll = 0;
        if (state->vfScroll >= VF_NUM_POINTS) state->vfScroll = VF_NUM_POINTS - 1;
    } else if (state->tab == TUI_TAB_FAN) {
        state->fanScroll += direction * (page ? 4 : 1);
        if (state->fanScroll < 0) state->fanScroll = 0;
        if (state->fanScroll >= FAN_CURVE_MAX_POINTS)
            state->fanScroll = FAN_CURVE_MAX_POINTS - 1;
    }
}

void select_curve_end(TuiState* state, bool end) {
    if (!state->serviceOnline) return;
    if (end) {
        for (int i = VF_NUM_POINTS - 1; i >= 0; --i) {
            if (state->service.snapshot.curve[i].freq_kHz == 0) continue;
            state->selectedPoint = i;
            state->vfScroll = i;
            return;
        }
    } else {
        for (int i = 0; i < VF_NUM_POINTS; ++i) {
            if (state->service.snapshot.curve[i].freq_kHz == 0) continue;
            state->selectedPoint = i;
            state->vfScroll = i;
            return;
        }
    }
}

int edit_step(TuiField field) {
    switch (field) {
        case TUI_FIELD_GPU_OFFSET: return 15;
        case TUI_FIELD_MEMORY_OFFSET: return 100;
        case TUI_FIELD_FAN_POLL: return 250;
        default: return 1;
    }
}

bool parse_mouse_sequence(const std::string& sequence, TuiInputEvent* event) {
    int button = 0, x = 0, y = 0;
    char suffix = 0;
    if (sscanf(sequence.c_str(), "\x1b[<%d;%d;%d%c",
               &button, &x, &y, &suffix) != 4) return false;
    event->type = TUI_INPUT_MOUSE;
    event->mouseX = x;
    event->mouseY = y;
    event->mouseButton = button;
    event->mousePress = suffix == 'M';
    return true;
}

}  // namespace

void tui_render(TuiState* state) {
    if (!state) return;
    winsize size = {};
    ioctl(STDOUT_FILENO, TIOCGWINSZ, &size);
    int width = size.ws_col ? size.ws_col : 120;
    int height = size.ws_row ? size.ws_row : 40;
    char selectedGpu[192] = {};
    TuiViewModel vm = {};
    populate_view_model(*state, &vm, selectedGpu);
    build_tui_layout(vm, width, height, &state->layout);
    reveal_selected_point(state);
    if (vm.vfScroll != state->vfScroll) {
        populate_view_model(*state, &vm, selectedGpu);
        build_tui_layout(vm, width, height, &state->layout);
    }
    if (!tui_layout_actions_valid(state->layout)) {
        state->layout.actions.clear();
        state->focusIndex = -1;
        snprintf(state->status, sizeof(state->status),
                 "Internal layout collision detected; controls disabled safely");
    }
    int actionCount = (int)state->layout.actions.size();
    if (actionCount == 0) state->focusIndex = -1;
    else if (state->focusIndex < 0) state->focusIndex = 0;
    else if (state->focusIndex >= actionCount) state->focusIndex = actionCount - 1;

    bool full = state->forceFullRender || width != state->renderedWidth ||
                height != state->renderedHeight ||
                state->renderedRows.size() != (size_t)height;
    std::vector<std::string> rows;
    rows.reserve((size_t)height);
    for (int y = 1; y <= height; ++y) rows.push_back(rendered_row(*state, y));

    fputs("\x1b[?2026h", stdout);
    if (full) fputs("\x1b[2J\x1b[H", stdout);
    for (int y = 0; y < height; ++y) {
        if (!full && y < (int)state->renderedRows.size() &&
            rows[y] == state->renderedRows[y]) continue;
        printf("\x1b[%d;1H\x1b[2K%s", y + 1, rows[y].c_str());
    }
    fputs("\x1b[0m\x1b[?2026l", stdout);
    fflush(stdout);
    state->renderedRows.swap(rows);
    state->renderedWidth = width;
    state->renderedHeight = height;
    state->forceFullRender = false;
}

bool tui_read_input(TuiState* state) {
    if (!state) return false;
    fd_set readSet;
    FD_ZERO(&readSet);
    FD_SET(STDIN_FILENO, &readSet);
    timeval timeout = {0, 100000};
    int ready = select(STDIN_FILENO + 1, &readSet, nullptr, nullptr, &timeout);
    if (ready <= 0) return false;
    char bytes[256] = {};
    ssize_t count = read(STDIN_FILENO, bytes, sizeof(bytes));
    if (count <= 0) return false;
    state->inputBuffer.append(bytes, (size_t)count);
    return true;
}

bool tui_parse_next_event(std::string* buffer, TuiInputEvent* event) {
    if (!buffer || !event || buffer->empty()) return false;
    *event = TuiInputEvent{};
    unsigned char first = (unsigned char)(*buffer)[0];
    if (first != 27) {
        buffer->erase(0, 1);
        if (first == '\r' || first == '\n') event->type = TUI_INPUT_ENTER;
        else if (first == '\t') event->type = TUI_INPUT_TAB;
        else if (first == 8 || first == 127) event->type = TUI_INPUT_BACKSPACE;
        else if (first == 3) event->type = TUI_INPUT_ESCAPE;
        else { event->type = TUI_INPUT_CHARACTER; event->character = (char)first; }
        return true;
    }
    if (buffer->size() == 1) {
        return false;
    }
    if (buffer->size() >= 3 && (*buffer)[1] == '[' && (*buffer)[2] == '<') {
        size_t end = buffer->find_first_of("Mm", 3);
        if (end == std::string::npos) return false;
        std::string sequence = buffer->substr(0, end + 1);
        buffer->erase(0, end + 1);
        return parse_mouse_sequence(sequence, event);
    }
    size_t end = 2;
    while (end < buffer->size()) {
        unsigned char value = (unsigned char)(*buffer)[end];
        if (value >= 0x40 && value <= 0x7E) break;
        ++end;
    }
    if (end >= buffer->size()) return false;
    std::string sequence = buffer->substr(0, end + 1);
    buffer->erase(0, end + 1);
    if (sequence == "\x1b[A" || sequence == "\x1bOA") event->type = TUI_INPUT_UP;
    else if (sequence == "\x1b[B" || sequence == "\x1bOB") event->type = TUI_INPUT_DOWN;
    else if (sequence == "\x1b[C" || sequence == "\x1bOC") event->type = TUI_INPUT_RIGHT;
    else if (sequence == "\x1b[D" || sequence == "\x1bOD") event->type = TUI_INPUT_LEFT;
    else if (sequence == "\x1b[Z") event->type = TUI_INPUT_SHIFT_TAB;
    else if (sequence == "\x1b[5~") event->type = TUI_INPUT_PAGE_UP;
    else if (sequence == "\x1b[6~") event->type = TUI_INPUT_PAGE_DOWN;
    else if (sequence == "\x1b[5;5~") event->type = TUI_INPUT_CTRL_PAGE_UP;
    else if (sequence == "\x1b[6;5~") event->type = TUI_INPUT_CTRL_PAGE_DOWN;
    else if (sequence == "\x1b[H" || sequence == "\x1b[1~") event->type = TUI_INPUT_HOME;
    else if (sequence == "\x1b[F" || sequence == "\x1b[4~") event->type = TUI_INPUT_END;
    else if (sequence == "\x1bOP" || sequence == "\x1b[11~") event->type = TUI_INPUT_F1;
    else event->type = TUI_INPUT_NONE;
    return true;
}

void tui_handle_event(TuiState* state, const TuiInputEvent& event) {
    if (!state || event.type == TUI_INPUT_NONE) return;
    if (state->edit.active) {
        if (event.type == TUI_INPUT_ESCAPE) tui_cancel_edit(state);
        else if (event.type == TUI_INPUT_ENTER) tui_commit_edit(state);
        else if (event.type == TUI_INPUT_BACKSPACE) {
            size_t length = strlen(state->edit.text);
            if (length > 0) state->edit.text[length - 1] = 0;
        } else if (event.type == TUI_INPUT_UP || event.type == TUI_INPUT_DOWN) {
            TuiField field = state->edit.field;
            int index = state->edit.index;
            int delta = edit_step(field) *
                        (event.type == TUI_INPUT_UP ? 1 : -1);
            tui_commit_edit(state);
            if (!state->edit.active) {
                ClickAction step = {0,0,0,0,ACTION_FIELD_STEP,
                    (int)field, delta, index};
                tui_apply_action(state, step);
            }
        } else if (event.type == TUI_INPUT_TAB ||
                   event.type == TUI_INPUT_SHIFT_TAB) {
            tui_commit_edit(state);
            if (!state->edit.active)
                focus_linear(state, event.type == TUI_INPUT_TAB ? 1 : -1);
        } else if (event.type == TUI_INPUT_CHARACTER) {
            tui_handle_character(state, event.character);
        } else if (event.type == TUI_INPUT_MOUSE && event.mousePress &&
                   (event.mouseButton & 64) == 0) {
            tui_commit_edit(state);
            if (state->edit.active) return;
            tui_handle_event(state, event);
        }
        return;
    }

    if (event.type == TUI_INPUT_MOUSE) {
        if (!event.mousePress) return;
        if (event.mouseButton & 64) {
            scroll_current_tab(state, (event.mouseButton & 1) ? 1 : -1, false);
            return;
        }
        if ((event.mouseButton & 3) != 0) return;
        for (int i = 0; i < (int)state->layout.actions.size(); ++i) {
            const ClickAction& action = state->layout.actions[i];
            if (event.mouseX < action.x1 || event.mouseX > action.x2 ||
                event.mouseY < action.y1 || event.mouseY > action.y2) continue;
            state->focusIndex = i;
            if (action.type == ACTION_VF_SELECT && action.index < 0) {
                TuiViewModel vm = {};
                char selectedGpu[192] = {};
                populate_view_model(*state, &vm, selectedGpu);
                int point = tui_nearest_graph_point(vm,
                    state->layout.graphRect, event.mouseX);
                if (point >= 0) state->selectedPoint = point;
            } else {
                tui_apply_action(state, action);
            }
            return;
        }
        return;
    }

    switch (event.type) {
        case TUI_INPUT_ESCAPE: state->running = false; break;
        case TUI_INPUT_ENTER: activate_focused(state); break;
        case TUI_INPUT_CHARACTER:
            if (event.character == ' ') activate_focused(state);
            else tui_handle_character(state, event.character);
            break;
        case TUI_INPUT_TAB: focus_linear(state, 1); break;
        case TUI_INPUT_SHIFT_TAB: focus_linear(state, -1); break;
        case TUI_INPUT_UP: focus_spatial(state, 0, -1); break;
        case TUI_INPUT_DOWN: focus_spatial(state, 0, 1); break;
        case TUI_INPUT_LEFT: focus_spatial(state, -1, 0); break;
        case TUI_INPUT_RIGHT: focus_spatial(state, 1, 0); break;
        case TUI_INPUT_PAGE_UP: scroll_current_tab(state, -1, true); break;
        case TUI_INPUT_PAGE_DOWN: scroll_current_tab(state, 1, true); break;
        case TUI_INPUT_CTRL_PAGE_UP:
            state->tab = (TuiTab)((state->tab + 2) % 3); state->focusIndex = -1; break;
        case TUI_INPUT_CTRL_PAGE_DOWN:
            state->tab = (TuiTab)((state->tab + 1) % 3); state->focusIndex = -1; break;
        case TUI_INPUT_HOME: select_curve_end(state, false); break;
        case TUI_INPUT_END: select_curve_end(state, true); break;
        case TUI_INPUT_F1:
            state->tab = TUI_TAB_PROFILES;
            snprintf(state->status, sizeof(state->status),
                     "Help: every pointer action has keyboard parity; use Tab and Enter");
            break;
        default: break;
    }
}
