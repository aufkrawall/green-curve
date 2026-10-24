// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// The Linux TUI's write target, assertion codes 7400-7409 and 7430-7439.  The
// TUI sends the GPU it shows (TuiState::targetGpu), so a write is allowed only
// while the selected GPU -- `--gpu`, config.ini's resolved record, the
// selector -- IS the one shown.  7400-7402, 7409 and 7430 drive the pure policy
// (linux_tui_authority.h); 7403-7408 and 7431-7436 drive the real
// tui_refresh_service() and tui_configured_gpu_allows_write()
// (linux_tui_refresh.cpp) against a scripted daemon that keeps the rule the
// real one enforces: while it holds active settings it refuses to select
// another GPU (linux_gpu_switch_preserves_active_intent).

#include "linux_tui_internal.h"

#include "linux_debug_log.h"
#include "linux_auto_restore_policy.h"
#include "auto_apply_failure_notice_policy.h"

#include <stdio.h>
#include <string.h>

namespace {

struct ScriptedDaemon {
    ServiceResponse published;
    bool busyTargeted;    // a targeted read is accepted, then not answered
    bool refuseTargeted;  // a targeted read to another GPU is answered "no"
    bool offline;         // connect() fails
    int reads;
    int targetedReads;
    int switchRequests;   // targeted reads naming a GPU it does not show
};

ScriptedDaemon g_daemon;

GpuAdapterInfo twin_at(unsigned int bus) {
    GpuAdapterInfo gpu = {};
    gpu.valid = true;
    gpu.pciInfoValid = true;
    gpu.deviceId = 0x2F0410DEu;
    gpu.subSystemId = 0x89E71043u;
    gpu.pciBus = bus;
    return gpu;
}

// Two identical cards, A on bus 6 (the daemon's selection) and B on bus 7.
void daemon_reset(bool holdsActiveSettings) {
    g_daemon = ScriptedDaemon{};
    ServiceResponse& r = g_daemon.published;
    r.snapshot.adapterCount = 2;
    r.snapshot.adapters[0] = twin_at(6);
    r.snapshot.adapters[1] = twin_at(7);
    r.snapshot.selectedAdapterIndex = 0;
    r.snapshot.health.availableMutationDomains = SERVICE_MUTATION_DOMAIN_ALL;
    r.state.validSections = SERVICE_STATE_SECTION_ADAPTER_IDENTITY |
        SERVICE_STATE_SECTION_CURVE_TOPOLOGY;
    r.state.serviceInstanceId = 1;
    r.state.gpuGeneration = 2;
    r.state.topologySignature = 3;
    r.state.gpuPhase = SERVICE_GPU_PHASE_READY;
    r.state.activeDesiredValid = holdsActiveSettings;
}

TuiState* fresh_state(LinuxConfiguredGpuStatus status, unsigned int bus) {
    static TuiState state;
    state = TuiState{};
    state.configuredGpu.target = twin_at(bus);
    state.configuredGpu.role = GPU_LOCATION_ROLE_REQUIRED;
    state.configuredGpu.status = status;
    return &state;
}

unsigned int shown_bus(const TuiState* state) {
    return state->targetGpu.valid ? state->targetGpu.pciBus : 0u;
}

}  // namespace

// Collaborators of linux_tui_refresh.cpp from the rest of the client.
void linux_debug_logf(const char*, ...) {}
unsigned long long tui_monotonic_ms() { return 1; }
int tui_clamp_vf_scroll(const TuiState&, int candidate) { return candidate; }
void tui_recompute_dirty(TuiState*) {}
int useful_initial_point(const ServiceResponse&, const DesiredSettings&) {
    return 0;
}
const GpuAdapterInfo* selected_adapter(const ServiceResponse& response) {
    return linux_tui_authoritative_gpu(&response);
}
void set_daemon_failure(TuiState* state, const char* fallback,
                        const char* detail) {
    snprintf(state->status, sizeof(state->status), "%s: %s", fallback,
             detail ? detail : "");
}
void set_gpu_health_status(TuiState*, const char*) {}
void format_linux_gpu_bdf(const GpuAdapterInfo* target, char* text,
                          size_t textSize) {
    snprintf(text, textSize, "bus%u", target && target->valid
                                          ? target->pciBus : 0u);
}
void desired_from_live_response(const ServiceResponse& response,
                                DesiredSettings* desired) {
    *desired = response.state.activeDesiredValid ? response.desired
                                                 : DesiredSettings{};
}
bool bind_current_draft(TuiState* state) {
    state->draftAttached = linux_tui_bind_draft(
        &state->draftBinding, &state->service, &state->desired);
    return state->draftAttached;
}
void linux_configured_gpu_resolve_against(const char*, LinuxConfiguredGpu* gpu,
                                          const GpuAdapterInfo* adapters,
                                          unsigned int count) {
    linux_configured_gpu_resolve(gpu, adapters, count, nullptr);
}
LinuxStartupSnapshotReport linux_startup_snapshot_report(
    const char*, bool, unsigned int, unsigned int) {
    return LinuxStartupSnapshotReport{};
}
bool linux_daemon_get_state_ex(const GpuAdapterInfo* target,
                               ServiceResponse* response,
                               LinuxDaemonSendOutcome* outcome,
                               char* error, size_t errorSize) {
    ++g_daemon.reads;
    *outcome = LinuxDaemonSendOutcome{};
    if (g_daemon.offline) {
        outcome->reachability = LINUX_DAEMON_REACHABILITY_UNREACHABLE;
        snprintf(error, errorSize, "%s", "connect: no such file or directory");
        return false;
    }
    outcome->reachability = LINUX_DAEMON_REACHABILITY_CONNECTED;
    outcome->requestSubmitted = true;
    ServiceSnapshot& snapshot = g_daemon.published.snapshot;
    if (target) {
        ++g_daemon.targetedReads;
        int index = -1;
        for (unsigned int i = 0; i < snapshot.adapterCount; ++i)
            if (linux_gpu_identity_matches(target, &snapshot.adapters[i]))
                index = (int)i;
        const bool other = index < 0 ||
            (unsigned int)index != snapshot.selectedAdapterIndex;
        if (other) ++g_daemon.switchRequests;
        if (g_daemon.busyTargeted) {
            outcome->deadlineExpired = true;
            snprintf(error, errorSize, "%s",
                     "request accepted but not answered in time");
            return false;
        }
        if (index < 0 || (other && (g_daemon.refuseTargeted ||
                                    g_daemon.published.state.activeDesiredValid))) {
            outcome->answered = true;
            snprintf(error, errorSize, "%s", g_daemon.refuseTargeted
                ? "cannot initialize the requested GPU"
                : "GPU has active Green Curve settings; Reset it first");
            return false;
        }
        snapshot.selectedAdapterIndex = (unsigned int)index;
    }
    outcome->answered = true;
    *response = g_daemon.published;
    return true;
}

#include "linux_tui_refresh.cpp"

namespace {

int policy_tests() {
    const GpuAdapterInfo a = twin_at(6), b = twin_at(7);
    // 7400: a selection that names a GPU allows a write only while exactly
    // that GPU is shown; otherwise the TUI switches to it, or holds while the
    // daemon keeps the shown GPU for its active settings.
    {
        LinuxConfiguredGpu chosen = {b, GPU_LOCATION_ROLE_REQUIRED,
                                     LINUX_CONFIGURED_GPU_EXPLICIT};
        LinuxConfiguredGpu resolved = chosen;
        resolved.status = LINUX_CONFIGURED_GPU_RESOLVED;
        for (const LinuxConfiguredGpu* gpu : {&chosen, &resolved}) {
            if (linux_tui_shown_gpu_write(gpu, &a) !=
                    LINUX_TUI_SHOWN_GPU_WRITE_NOT_SHOWN ||
                linux_tui_shown_gpu_write(gpu, nullptr) !=
                    LINUX_TUI_SHOWN_GPU_WRITE_NOT_SHOWN ||
                linux_tui_shown_gpu_write(gpu, &b) !=
                    LINUX_TUI_SHOWN_GPU_WRITE_ALLOWED) return 7400;
            if (linux_tui_gpu_adoption(gpu, &a, false, false, false) !=
                    LINUX_TUI_GPU_ADOPTION_SWITCH ||
                linux_tui_gpu_adoption(gpu, &a, true, false, false) !=
                    LINUX_TUI_GPU_ADOPTION_HELD ||
                linux_tui_gpu_adoption(gpu, &b, true, true, true) !=
                    LINUX_TUI_GPU_ADOPTION_NONE) return 7400;
        }
    }
    // 7401: without a selection the daemon's own rule decides (a lone adapter,
    // or its current selection); there is nothing to switch to.
    {
        LinuxConfiguredGpu none = {};
        if (linux_tui_shown_gpu_write(&none, &a) !=
                LINUX_TUI_SHOWN_GPU_WRITE_ALLOWED ||
            linux_tui_shown_gpu_write(&none, nullptr) !=
                LINUX_TUI_SHOWN_GPU_WRITE_ALLOWED ||
            linux_tui_shown_gpu_write(nullptr, &a) !=
                LINUX_TUI_SHOWN_GPU_WRITE_ALLOWED ||
            linux_tui_gpu_adoption(&none, &a, true, true, true) !=
                LINUX_TUI_GPU_ADOPTION_NONE) return 7401;
    }
    // 7402: an unresolved selection stops a write whatever is shown and never
    // switches; it is BLOCKED, so it says why once.  Every verdict and step
    // has a stable log name.
    {
        const LinuxConfiguredGpuStatus unresolved[] = {
            LINUX_CONFIGURED_GPU_PENDING, LINUX_CONFIGURED_GPU_UNVALIDATED,
            LINUX_CONFIGURED_GPU_NOT_PRESENT, LINUX_CONFIGURED_GPU_AMBIGUOUS,
            LINUX_CONFIGURED_GPU_MALFORMED};
        for (LinuxConfiguredGpuStatus status : unresolved) {
            LinuxConfiguredGpu gpu = {b, GPU_LOCATION_ROLE_REQUIRED, status};
            if (linux_tui_shown_gpu_write(&gpu, &b) !=
                    LINUX_TUI_SHOWN_GPU_WRITE_UNRESOLVED ||
                linux_tui_gpu_adoption(&gpu, &a, false, false, false) !=
                    LINUX_TUI_GPU_ADOPTION_BLOCKED) return 7402;
        }
        const LinuxTuiShownGpuWrite verdicts[] = {
            LINUX_TUI_SHOWN_GPU_WRITE_ALLOWED,
            LINUX_TUI_SHOWN_GPU_WRITE_UNRESOLVED,
            LINUX_TUI_SHOWN_GPU_WRITE_NOT_SHOWN};
        for (LinuxTuiShownGpuWrite verdict : verdicts)
            if (strcmp(linux_tui_shown_gpu_write_name(verdict), "invalid") == 0)
                return 7402;
        const LinuxTuiGpuAdoption steps[] = {LINUX_TUI_GPU_ADOPTION_NONE,
            LINUX_TUI_GPU_ADOPTION_SWITCH, LINUX_TUI_GPU_ADOPTION_HELD,
            LINUX_TUI_GPU_ADOPTION_DEFERRED, LINUX_TUI_GPU_ADOPTION_REFUSED,
            LINUX_TUI_GPU_ADOPTION_BLOCKED};
        for (LinuxTuiGpuAdoption step : steps)
            if (strcmp(linux_tui_gpu_adoption_name(step), "invalid") == 0)
                return 7402;
    }
    // 7409: what keeps a selection that is not shown from being switched to,
    // in order: the daemon's active settings (it would refuse), a staged
    // draft (switching would detach it), an unchanged refusal (asking again
    // gets the same answer).  A refusal holds only while the target and every
    // daemon fact it was refused under stay the same.
    {
        LinuxConfiguredGpu chosen = {b, GPU_LOCATION_ROLE_REQUIRED,
                                     LINUX_CONFIGURED_GPU_RESOLVED};
        if (linux_tui_gpu_adoption(&chosen, &a, true, true, true) !=
                LINUX_TUI_GPU_ADOPTION_HELD ||
            linux_tui_gpu_adoption(&chosen, &a, false, true, true) !=
                LINUX_TUI_GPU_ADOPTION_DEFERRED ||
            linux_tui_gpu_adoption(&chosen, &a, false, false, true) !=
                LINUX_TUI_GPU_ADOPTION_REFUSED ||
            linux_tui_gpu_adoption(&chosen, nullptr, false, false, false) !=
                LINUX_TUI_GPU_ADOPTION_SWITCH) return 7409;
        ServiceResponse refusedIn = {};
        refusedIn.state.serviceInstanceId = 1;
        refusedIn.state.gpuGeneration = 2;
        refusedIn.state.topologySignature = 3;
        refusedIn.snapshot.adapterCount = 2;
        const LinuxTuiSwitchRefusal refusal =
            linux_tui_switch_refusal(&b, &refusedIn);
        if (!refusal.valid ||
            !linux_tui_switch_refusal_matches(&refusal, &b, &refusedIn) ||
            linux_tui_switch_refusal_matches(&refusal, &a, &refusedIn) ||
            linux_tui_switch_refusal_matches(nullptr, &b, &refusedIn) ||
            linux_tui_switch_refusal(nullptr, &refusedIn).valid) return 7409;
        for (int change = 0; change < 5; ++change) {
            ServiceResponse later = refusedIn;
            if (change == 0) later.state.serviceInstanceId = 9;
            if (change == 1) later.state.gpuGeneration = 9;
            if (change == 2) later.state.topologySignature = 9;
            if (change == 3) later.snapshot.adapterCount = 3;
            if (change == 4) later.state.activeDesiredValid = true;
            if (linux_tui_switch_refusal_matches(&refusal, &b, &later))
                return 7409;
        }
    }
    // 7430: the selector steps from the selected GPU when it names a listed
    // adapter.  With B selected and A shown, one step either way reaches A:
    // stepping from the shown GPU never could, so a GPU the daemon kept for
    // its active settings could be neither written nor selected.
    {
        const GpuAdapterInfo adapters[] = {a, b};
        LinuxConfiguredGpu chosen = {b, GPU_LOCATION_ROLE_REQUIRED,
                                     LINUX_CONFIGURED_GPU_RESOLVED};
        unsigned int origin = 0;
        if (!linux_tui_selector_origin(&chosen, adapters, 2, &origin) ||
            origin != 1 ||
            linux_next_gpu_selection_index(true, origin, 2, 1) != 0 ||
            linux_next_gpu_selection_index(true, origin, 2, -1) != 0)
            return 7430;
        // No selection, an unresolved one, or one not listed: the daemon's
        // own selection stays the origin.
        LinuxConfiguredGpu none = {};
        LinuxConfiguredGpu missing = chosen;
        missing.status = LINUX_CONFIGURED_GPU_NOT_PRESENT;
        LinuxConfiguredGpu unlisted = chosen;
        unlisted.target = twin_at(9);
        for (const LinuxConfiguredGpu* gpu : {&none, &missing, &unlisted}) {
            origin = 5;
            if (linux_tui_selector_origin(gpu, adapters, 2, &origin) ||
                origin != 5) return 7430;
        }
        if (linux_tui_selector_origin(&chosen, nullptr, 2, &origin) ||
            linux_tui_selector_origin(&chosen, adapters, 2, nullptr))
            return 7430;
    }
    return 0;
}

int refresh_tests() {
    // 7403: `greencurve --gpu <B> --tui` while the daemon shows A.  The TUI
    // used to leave an explicit selection alone, so it showed, edited and
    // wrote A.  It now switches to B before its first frame.
    {
        daemon_reset(false);
        TuiState* state = fresh_state(LINUX_CONFIGURED_GPU_EXPLICIT, 7);
        if (!tui_refresh_service(state, false) || shown_bus(state) != 7 ||
            g_daemon.targetedReads != 1 ||
            !tui_configured_gpu_allows_write(state, "Apply")) return 7403;
    }
    // 7404: config.ini names B, but A holds active settings, so the daemon
    // refuses to select B.  B used to count as resolved and A received the
    // write; now every write stops while A is shown, no refused switch is
    // sent, and the switch goes ahead once A's settings were reset elsewhere.
    {
        daemon_reset(true);
        TuiState* state = fresh_state(LINUX_CONFIGURED_GPU_PENDING, 7);
        if (!tui_refresh_service(state, false) || shown_bus(state) != 6 ||
            state->configuredGpu.status != LINUX_CONFIGURED_GPU_RESOLVED ||
            g_daemon.targetedReads != 0 ||
            state->gpuAdoption != LINUX_TUI_GPU_ADOPTION_HELD ||
            !strstr(state->status, "active settings")) return 7404;
        const char* actions[] = {"Apply", "Reset", "Startup profile bind"};
        for (const char* action : actions)
            if (tui_configured_gpu_allows_write(state, action) ||
                !strstr(state->status, "blocked")) return 7404;
        g_daemon.published.state.activeDesiredValid = false;
        if (!tui_refresh_service(state, false) || shown_bus(state) != 7 ||
            !tui_configured_gpu_allows_write(state, "Apply")) return 7404;
    }
    // 7405: the switch to B fails as busy.  The TUI used to keep showing A
    // with B marked resolved and never retried; now writes stop and the next
    // answered refresh retries the switch.
    {
        daemon_reset(false);
        g_daemon.busyTargeted = true;
        TuiState* state = fresh_state(LINUX_CONFIGURED_GPU_PENDING, 7);
        if (!tui_refresh_service(state, false) || shown_bus(state) != 6 ||
            g_daemon.targetedReads != 1 ||
            tui_configured_gpu_allows_write(state, "Apply")) return 7405;
        g_daemon.busyTargeted = false;
        if (!tui_refresh_service(state, false) || shown_bus(state) != 7 ||
            !tui_configured_gpu_allows_write(state, "Reset")) return 7405;
    }
    // 7406: a hold announces itself once: its status line is not rewritten
    // on every telemetry tick.
    {
        daemon_reset(true);
        TuiState* state = fresh_state(LINUX_CONFIGURED_GPU_PENDING, 7);
        tui_refresh_service(state, false);
        state->status[0] = 0;
        if (!tui_refresh_service(state, false) || state->status[0] ||
            state->gpuAdoption != LINUX_TUI_GPU_ADOPTION_HELD) return 7406;
    }
    // 7407: an explicit targeted refresh (the selector) is not undone by the
    // adoption within the same call, and it sends exactly one request.
    {
        daemon_reset(false);
        TuiState* state = fresh_state(LINUX_CONFIGURED_GPU_RESOLVED, 6);
        state->configuredGpu.target = g_daemon.published.snapshot.adapters[0];
        if (!tui_refresh_service(state, false) || shown_bus(state) != 6)
            return 7407;
        const int readsBefore = g_daemon.reads;
        const GpuAdapterInfo chosen = g_daemon.published.snapshot.adapters[1];
        if (!tui_refresh_service(state, true, &chosen) ||
            shown_bus(state) != 7 || g_daemon.reads != readsBefore + 1)
            return 7407;
    }
    // 7408: an unresolved record never switches and stops writes, and
    // without a record the daemon's selection is written as before.
    {
        daemon_reset(false);
        TuiState* state = fresh_state(LINUX_CONFIGURED_GPU_PENDING, 9);
        if (!tui_refresh_service(state, false) || shown_bus(state) != 6 ||
            state->configuredGpu.status != LINUX_CONFIGURED_GPU_NOT_PRESENT ||
            g_daemon.targetedReads != 0 ||
            tui_configured_gpu_allows_write(state, "Apply")) return 7408;
        daemon_reset(false);
        state = fresh_state(LINUX_CONFIGURED_GPU_NONE, 0);
        state->configuredGpu.target = GpuAdapterInfo{};
        if (!tui_refresh_service(state, false) || shown_bus(state) != 6 ||
            !tui_configured_gpu_allows_write(state, "Apply")) return 7408;
    }
    return 0;
}

// The way out of a hold, and the steps that keep a switch from being asked
// for once per telemetry tick.
int recovery_tests() {
    // 7431: config.ini names B; A holds active settings, so the daemon keeps
    // A and refuses B.  The TUI used to block every write to A, Reset
    // included, while its selector could only step to B: the hold the
    // daemon says to Reset was unrecoverable from the TUI.  Now one selector
    // step reaches A, Reset is allowed for it, and the next step reaches B.
    {
        daemon_reset(true);
        TuiState* state = fresh_state(LINUX_CONFIGURED_GPU_PENDING, 7);
        if (!tui_refresh_service(state, false) ||
            state->gpuAdoption != LINUX_TUI_GPU_ADOPTION_HELD ||
            !strstr(state->status, "select bus6") ||
            tui_configured_gpu_allows_write(state, "Reset")) return 7431;
        if (!tui_select_gpu_step(state, 1) || shown_bus(state) != 6 ||
            state->configuredGpu.status != LINUX_CONFIGURED_GPU_EXPLICIT ||
            state->configuredGpu.target.pciBus != 6 ||
            !tui_configured_gpu_allows_write(state, "Reset")) return 7431;
        // The Reset the daemon asked for; the next answered refresh ends the
        // hold and says that writes reach the shown GPU again.
        g_daemon.published.state.activeDesiredValid = false;
        if (!tui_refresh_service(state, false) ||
            state->gpuAdoption != LINUX_TUI_GPU_ADOPTION_NONE ||
            !strstr(state->status, "writes reach it again")) return 7431;
        if (!tui_select_gpu_step(state, 1) || shown_bus(state) != 7 ||
            state->configuredGpu.target.pciBus != 7 ||
            !tui_configured_gpu_allows_write(state, "Apply")) return 7431;
    }
    // 7432: a switch the daemon ANSWERS with a refusal (not active settings:
    // here it cannot initialize B) is not asked again on every tick.  It used
    // to cost one more request per second, and its reason was replaced by
    // "Daemon is busy".  It is asked again once the daemon's state changes,
    // or when the user refreshes.
    {
        daemon_reset(false);
        g_daemon.refuseTargeted = true;
        TuiState* state = fresh_state(LINUX_CONFIGURED_GPU_PENDING, 7);
        if (!tui_refresh_service(state, false) || shown_bus(state) != 6 ||
            g_daemon.switchRequests != 1 ||
            state->gpuAdoption != LINUX_TUI_GPU_ADOPTION_REFUSED ||
            !strstr(state->status, "refused") ||
            !strstr(state->status, "cannot initialize") ||
            strstr(state->status, "busy") ||
            tui_configured_gpu_allows_write(state, "Apply")) return 7432;
        state->status[0] = 0;
        if (!tui_refresh_service(state, false) || g_daemon.switchRequests != 1 ||
            state->status[0]) return 7432;
        g_daemon.published.state.gpuGeneration = 5;
        if (!tui_refresh_service(state, false) || g_daemon.switchRequests != 2 ||
            state->gpuAdoption != LINUX_TUI_GPU_ADOPTION_REFUSED) return 7432;
        if (!tui_refresh_service(state, true) || g_daemon.switchRequests != 3)
            return 7432;
        g_daemon.refuseTargeted = false;
        if (!tui_refresh_service(state, true) || shown_bus(state) != 7 ||
            g_daemon.switchRequests != 4 ||
            !tui_configured_gpu_allows_write(state, "Apply")) return 7432;
    }
    // 7433: a refusal says it is one; only an unanswered read is "busy".
    {
        daemon_reset(true);
        TuiState* state = fresh_state(LINUX_CONFIGURED_GPU_NONE, 0);
        state->configuredGpu.target = GpuAdapterInfo{};
        if (!tui_refresh_service(state, false)) return 7433;
        const GpuAdapterInfo other = g_daemon.published.snapshot.adapters[1];
        if (tui_refresh_service(state, true, &other) ||
            !strstr(state->status, "refused") ||
            strstr(state->status, "busy") || !state->serviceOnline) return 7433;
        g_daemon.busyTargeted = true;
        if (tui_refresh_service(state, true, &other) ||
            !strstr(state->status, "busy") || !state->serviceOnline)
            return 7433;
    }
    // 7434: a staged draft defers the automatic switch the selector would
    // refuse too; switching under it detached the draft without a word.
    {
        daemon_reset(false);
        TuiState* state = fresh_state(LINUX_CONFIGURED_GPU_PENDING, 7);
        state->dirty = true;
        if (!tui_refresh_service(state, false) || shown_bus(state) != 6 ||
            g_daemon.switchRequests != 0 ||
            state->gpuAdoption != LINUX_TUI_GPU_ADOPTION_DEFERRED ||
            !strstr(state->status, "staged changes") ||
            tui_configured_gpu_allows_write(state, "Apply") ||
            tui_select_gpu_step(state, 1)) return 7434;
        state->dirty = false;
        if (!tui_refresh_service(state, false) || shown_bus(state) != 7 ||
            !tui_configured_gpu_allows_write(state, "Apply")) return 7434;
    }
    // 7435: a malformed config.ini record says why writes are blocked when
    // the daemon first answers -- also after a start without the daemon,
    // which used to say only "Daemon reconnected" -- and again after every
    // reconnect.
    {
        daemon_reset(false);
        g_daemon.offline = true;
        TuiState* state = fresh_state(LINUX_CONFIGURED_GPU_MALFORMED, 7);
        if (tui_refresh_service(state, false) || state->serviceOnline)
            return 7435;
        g_daemon.offline = false;
        if (!tui_refresh_service(state, false) ||
            state->gpuAdoption != LINUX_TUI_GPU_ADOPTION_BLOCKED ||
            !strstr(state->status, "malformed") ||
            g_daemon.switchRequests != 0) return 7435;
        g_daemon.offline = true;
        tui_refresh_service(state, false);
        g_daemon.offline = false;
        if (!tui_refresh_service(state, false) ||
            !strstr(state->status, "malformed")) return 7435;
        // Selecting a GPU lifts the block.
        if (!tui_select_gpu_step(state, 1) ||
            !tui_configured_gpu_allows_write(state, "Apply")) return 7435;
    }
    // 7436: a record that stops resolving differently (unchecked, then not
    // present) says so again although the step stays BLOCKED.
    {
        daemon_reset(false);
        g_daemon.published.snapshot.adapterCount = 0;
        TuiState* state = fresh_state(LINUX_CONFIGURED_GPU_PENDING, 9);
        if (!tui_refresh_service(state, false) ||
            state->configuredGpu.status != LINUX_CONFIGURED_GPU_UNVALIDATED ||
            !strstr(state->status, "could not be checked")) return 7436;
        g_daemon.published.snapshot.adapterCount = 2;
        if (!tui_refresh_service(state, false) ||
            state->configuredGpu.status != LINUX_CONFIGURED_GPU_NOT_PRESENT ||
            !strstr(state->status, "not present")) return 7436;
    }
    return 0;
}

}  // namespace

int run_linux_tui_gpu_target_tests() {
    if (int r = policy_tests()) return r;
    if (int r = refresh_tests()) return r;
    if (int r = recovery_tests()) return r;
    return 0;
}
