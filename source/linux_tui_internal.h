// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT

#ifndef GREEN_CURVE_LINUX_TUI_INTERNAL_H
#define GREEN_CURVE_LINUX_TUI_INTERNAL_H

#include "linux_daemon.h"
#include "linux_port.h"
#include "linux_tui_authority.h"
#include "linux_tui_edit_policy.h"
#include "linux_tui_layout.h"
#include "linux_tui_diagnostic_policy.h"
#include "linux_startup_sync.h"

#include <string>
#include <vector>

struct TuiEditState {
    bool active;
    TuiField field;
    int index;
    char text[32];
    // Whole contents pre-selected, so the next accepted keystroke replaces the
    // value instead of appending to it.  Set on entry, dropped by a second
    // click into the same field.  See linux_tui_edit_policy.h.
    bool selectAll;
};

struct TuiState {
    DesiredSettings desired;
    DesiredSettings acceptedDesired;
    ServiceResponse service;
    bool serviceOnline;
    bool draftAttached;
    LinuxTuiDraftBinding draftBinding;
    bool dirty;
    int currentSlot;
    TuiTab tab;
    int selectedPoint;
    // Selection the auto-reveal last scrolled into view.  -1 forces one reveal
    // on the first frame; after that only a real selection change re-reveals,
    // so scrolling is never undone.
    int revealedPoint;
    int vfScroll;
    int fanScroll;
    int focusIndex;
    bool running;
    bool forceFullRender;
    int renderedWidth;
    int renderedHeight;
    unsigned long long nextTelemetryMs;
    unsigned long long escapePendingSince;
    char configPath[LINUX_PATH_MAX];
    // What the daemon would write at the next boot when the policy is
    // `profile N`, read back with GET_STARTUP_POLICY rather than assumed.  It
    // is refreshed only when something can have moved it (init, a profile
    // write, a policy change), never on the 1 Hz telemetry tick.
    DesiredSettings startupSnapshot;
    bool startupSnapshotKnown;
    StartupSnapshotState startupSnapshotState;
    char status[512];
    ProbeSummary probe;
    GpuAdapterInfo targetGpu;
    TuiEditState edit;
    TuiLayout layout;
    std::vector<std::string> renderedRows;
    std::string inputBuffer;
};

enum TuiInputType {
    TUI_INPUT_NONE = 0,
    TUI_INPUT_CHARACTER,
    TUI_INPUT_ESCAPE,
    TUI_INPUT_ENTER,
    TUI_INPUT_BACKSPACE,
    TUI_INPUT_TAB,
    TUI_INPUT_SHIFT_TAB,
    TUI_INPUT_UP,
    TUI_INPUT_DOWN,
    TUI_INPUT_LEFT,
    TUI_INPUT_RIGHT,
    TUI_INPUT_PAGE_UP,
    TUI_INPUT_PAGE_DOWN,
    TUI_INPUT_CTRL_PAGE_UP,
    TUI_INPUT_CTRL_PAGE_DOWN,
    TUI_INPUT_HOME,
    TUI_INPUT_END,
    TUI_INPUT_F1,
    TUI_INPUT_MOUSE,
};

struct TuiInputEvent {
    TuiInputType type;
    char character;
    int mouseX;
    int mouseY;
    int mouseButton;
    bool mousePress;
};

unsigned long long tui_monotonic_ms();
bool tui_refresh_service(TuiState* state, bool userRequested,
                         const GpuAdapterInfo* requestedTarget = nullptr);
void tui_recompute_dirty(TuiState* state);
// Read the daemon's boot-apply snapshot back and compare it with the profile
// slot the policy names.  Cheap and explicit: one request, only at the points
// where the answer can have changed.
void tui_refresh_startup_snapshot(TuiState* state);
void tui_apply_action(TuiState* state, const ClickAction& action);
void tui_begin_edit(TuiState* state, TuiField field, int index);
void tui_commit_edit(TuiState* state);
void tui_cancel_edit(TuiState* state);
void tui_handle_character(TuiState* state, char character);
void tui_handle_event(TuiState* state, const TuiInputEvent& event);
void tui_render(TuiState* state);
// Bound a candidate VF scroll offset to [0, last page of populated points].
int tui_clamp_vf_scroll(const TuiState& state, int candidate);
bool tui_read_input(TuiState* state);
bool tui_parse_next_event(std::string* buffer, TuiInputEvent* event);

#endif
