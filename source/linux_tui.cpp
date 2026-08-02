// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT

#include "linux_tui_internal.h"

#include "linux_debug_log.h"
#include "linux_gpu_selection.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

namespace {

struct TerminalGuard;
TerminalGuard* g_activeTerminalGuard = nullptr;
volatile sig_atomic_t g_resizeRequested = 1;
volatile sig_atomic_t g_stopRequested = 0;

void restore_terminal_mode();

bool terminal_modes_equal(const termios& left, const termios& right) {
    if (left.c_iflag != right.c_iflag || left.c_oflag != right.c_oflag ||
        left.c_cflag != right.c_cflag || left.c_lflag != right.c_lflag ||
        cfgetispeed(&left) != cfgetispeed(&right) ||
        cfgetospeed(&left) != cfgetospeed(&right)) return false;
    for (unsigned int i = 0; i < NCCS; ++i)
        if (left.c_cc[i] != right.c_cc[i]) return false;
    return true;
}

struct TerminalGuard {
    termios original;
    bool active;

    TerminalGuard() : original{}, active(false) {
        if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) return;
        if (tcgetattr(STDIN_FILENO, &original) != 0) return;
        termios raw = original;
        cfmakeraw(&raw);
        if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) != 0) return;
        active = true;
        g_activeTerminalGuard = this;
        // Alternate screen, hidden cursor and SGR coordinates.  Mode 1000
        // includes button presses and wheel events without noisy pointer-motion
        // traffic; keyboard navigation remains fully equivalent.
        fputs("\x1b[?1049h\x1b[?25l\x1b[?1000h\x1b[?1006h", stdout);
        fflush(stdout);
    }

    ~TerminalGuard() {
        restore_terminal_mode();
        g_activeTerminalGuard = nullptr;
    }
};

// Leave the terminal exactly as it was found, then put the cursor on a fresh
// line.  The trailing newline is not cosmetic: on Konsole a write that ends
// precisely at the alternate-screen exit leaves the *next* write unpainted
// until more output arrives, which is why quitting appeared to need one extra
// keypress before the shell prompt showed up.
#define TUI_PRESENTATION_RESTORE \
    "\x1b[?2026l\x1b[?1006l\x1b[?1000l\x1b[?25h\x1b[?1049l\r\n"

void restore_terminal_mode() {
    if (!g_activeTerminalGuard || !g_activeTerminalGuard->active) return;
    fputs(TUI_PRESENTATION_RESTORE, stdout);
    fflush(stdout);
    tcsetattr(STDIN_FILENO, TCSAFLUSH,
              &g_activeTerminalGuard->original);
    g_activeTerminalGuard->active = false;
}

void on_sigwinch(int) {
    g_resizeRequested = 1;
}

void on_stop_signal(int) {
    g_stopRequested = 1;
}

void initialize_state(TuiState* state, const char* configPath,
                      int initialSlot, DesiredSettings* initialDesired,
                      const GpuAdapterInfo* initialTarget) {
    *state = TuiState{};
    if (configPath && configPath[0])
        snprintf(state->configPath, sizeof(state->configPath), "%s", configPath);
    else
        default_linux_config_path(state->configPath, sizeof(state->configPath));
    state->currentSlot = initialSlot >= 1 && initialSlot <= CONFIG_NUM_SLOTS
        ? initialSlot : CONFIG_DEFAULT_SLOT;
    state->tab = TUI_TAB_VF;
    state->selectedPoint = 0;
    state->revealedPoint = -1;
    state->vfScroll = 0;
    state->fanScroll = 0;
    state->focusIndex = -1;
    state->running = true;
    state->forceFullRender = true;
    state->draftAttached = false;
    initialize_desired_settings_defaults(&state->desired);
    normalize_desired_settings_for_ui(&state->desired);
    state->acceptedDesired = state->desired;
    bool serviceLoaded = tui_refresh_service(state, false);
    if (serviceLoaded && initialTarget && initialTarget->valid) {
        bool sameTarget = state->targetGpu.valid &&
            linux_gpu_identity_matches(initialTarget, &state->targetGpu);
        if (!sameTarget && state->service.state.activeDesiredValid) {
            snprintf(state->status, sizeof(state->status),
                     "Another GPU has active settings; Reset it before selecting the saved GPU");
        } else if (!sameTarget) {
            serviceLoaded = tui_refresh_service(state, false, initialTarget);
        }
    }
    // One read-back at start, before any status line is composed: a boot-apply
    // snapshot that no longer matches its profile is a silent revert at the
    // next boot, so it has to be visible without the user doing anything.
    tui_refresh_startup_snapshot(state);
    if (!serviceLoaded) {
        if (initialTarget) state->targetGpu = *initialTarget;
        if (initialDesired) {
            state->desired = *initialDesired;
            normalize_desired_settings_for_ui(&state->desired);
            state->acceptedDesired = state->desired;
        }
        if (!state->status[0])
            snprintf(state->status, sizeof(state->status),
                     "Daemon unavailable; editing is local and Apply is blocked");
    } else if ((state->service.state.validSections &
                    SERVICE_STATE_SECTION_ADAPTER_IDENTITY) == 0) {
        snprintf(state->status, sizeof(state->status),
                 "Select a GPU before editing or applying settings");
    } else if (!state->status[0]) {
        snprintf(state->status, sizeof(state->status),
                 "Live GPU state loaded • click a field or use Tab/Enter");
    }
    // Outranks the informational lines above: what boots is about to differ
    // from what the profile says, and the user cannot see that anywhere else.
    if (startup_snapshot_state_is_stale(state->startupSnapshotState)) {
        snprintf(state->status, sizeof(state->status),
            state->startupSnapshotState == STARTUP_SNAPSHOT_STATE_PROFILE_MISSING
                ? "Startup apply boots profile %u, but that slot no longer loads; re-bind or disable it"
                : "Startup apply boots a STALE copy of profile %u; Save that slot again to refresh what boots",
            (unsigned int)state->service.state.startupPolicySlot);
    }
}

int run_tui_child(const char* configPath, int initialSlot,
                  DesiredSettings* initialDesired,
                  const GpuAdapterInfo* initialTarget) {
    TuiState state;
    initialize_state(&state, configPath, initialSlot,
                     initialDesired, initialTarget);

    struct sigaction resizeAction = {};
    resizeAction.sa_handler = on_sigwinch;
    sigemptyset(&resizeAction.sa_mask);
    sigaction(SIGWINCH, &resizeAction, nullptr);
    struct sigaction stopAction = {};
    stopAction.sa_handler = on_stop_signal;
    sigemptyset(&stopAction.sa_mask);
    sigaction(SIGINT, &stopAction, nullptr);
    sigaction(SIGTERM, &stopAction, nullptr);

    TerminalGuard terminal;
    if (!terminal.active) {
        fprintf(stderr, "Linux TUI requires an interactive terminal.\n");
        return 1;
    }

    tui_render(&state);
    while (state.running && !g_stopRequested) {
        if (g_resizeRequested) {
            g_resizeRequested = 0;
            state.forceFullRender = true;
            tui_render(&state);
        }

        bool inputArrived = tui_read_input(&state);
        bool handled = false;
        TuiInputEvent event = {};
        while (tui_parse_next_event(&state.inputBuffer, &event)) {
            tui_handle_event(&state, event);
            handled = true;
            if (!state.running) break;
        }
        unsigned long long inputNow = tui_monotonic_ms();
        if (state.inputBuffer.size() == 1 && state.inputBuffer[0] == '\x1b') {
            if (!state.escapePendingSince)
                state.escapePendingSince = inputNow;
            else if (!inputArrived &&
                     inputNow - state.escapePendingSince >= 40) {
                state.inputBuffer.clear();
                state.escapePendingSince = 0;
                TuiInputEvent escape = {};
                escape.type = TUI_INPUT_ESCAPE;
                tui_handle_event(&state, escape);
                handled = true;
            }
        } else {
            state.escapePendingSince = 0;
        }
        // Quitting is not a reason to paint another frame; the guard is about to
        // tear the alternate screen down. The old code rendered here regardless,
        // so every exit ended with one or two dead frames on the wire.
        if (handled && state.running) tui_render(&state);
        if (!state.running) break;

        unsigned long long now = tui_monotonic_ms();
        if (!state.edit.active && now >= state.nextTelemetryMs) {
            tui_refresh_service(&state, false);
            tui_render(&state);
        } else if (!inputArrived && !handled) {
            // select() already supplied the bounded idle wait; no sleep or
            // timing-dependent input workaround is needed here.
        }
    }
    return 0;
}

}  // namespace

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
        int status = run_tui_child(configPath, initialSlot,
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

    // The supervisor never enters raw mode and can restore presentation with
    // normal APIs even when the child terminates through SIGSEGV or SIGABRT.
    //
    // It only does so when the child actually left the terminal modified: the
    // child is already reaped, so a termios read here races with nobody, and
    // repeating a restore the child performed correctly would duplicate the
    // whole escape sequence on the wire for no benefit.
    termios afterChild = {};
    bool childRestored = tcgetattr(STDIN_FILENO, &afterChild) == 0 &&
        terminal_modes_equal(afterChild, original);
    if (!childRestored) {
        fputs(TUI_PRESENTATION_RESTORE, stdout);
        fflush(stdout);
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &original);
    }
    linux_debug_logf("tui supervisor: child=%ld childRestoredTerminal=%d "
                     "signaled=%d status=%d",
                     (long)child, childRestored ? 1 : 0,
                     WIFSIGNALED(childStatus) ? 1 : 0,
                     WIFEXITED(childStatus) ? WEXITSTATUS(childStatus) : -1);
    if (WIFSIGNALED(childStatus)) {
        int signalNumber = WTERMSIG(childStatus);
        fprintf(stderr,
                "Green Curve TUI terminated by signal %d; terminal state restored.\n",
                signalNumber);
        return 128 + signalNumber;
    }
    return WIFEXITED(childStatus) ? WEXITSTATUS(childStatus) : 1;
}
