// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#ifndef GREEN_CURVE_LINUX_DAEMON_DEADLINE_POLICY_H
#define GREEN_CURVE_LINUX_DAEMON_DEADLINE_POLICY_H

// The ONE place the Linux client's request deadlines and the daemon's own
// bounds for the same request are written down.  The Linux counterpart of
// service_request_deadline_policy.h, and it exists for the same reason: a
// deadline that is picked rather than derived turns a healthy-but-busy server
// into a lost connection.
//
// Why this exists (2026-09-12 audit, after the Windows F-PIPE-DEADLINE fix):
//
// linux_daemon_transport.cpp carried ONE literal -- GC_DAEMON_IO_TIMEOUT_MS
// 2000 -- used for all four I/O roles: the daemon reading a request, the daemon
// writing a response, the client writing a request, and *the client waiting for
// the daemon to compute and send its answer*.  Only the first two are
// stall-bounding questions that a fixed small number answers correctly.  The
// fourth is a turnaround question, and 2000 ms was below the daemon's own bound
// for a mutation by an order of magnitude:
//
//   - Windows gives the same operation 20000 ms (SERVICE_APPLY_CLIENT_TIMEOUT_MS
//     in main.cpp), for strictly LESS work.
//   - The Linux APPLY handler additionally performs up to four fsync()-ed
//     temp+rename+dirfsync record writes inline (store_daemon_record /
//     restore_committed_record in linux_daemon.cpp), which Windows does not.
//   - On the persistence-failure path it performs a SECOND full hardware pass
//     (linux_backend_restore_snapshot) before it answers.
//
// The second half of the defect was worse than the first.  On a transport
// timeout, send_simple() in linux_daemon_client.h queries the original
// operation ID instead of issuing another hardware write -- but that query was
// sent with the same 2000 ms.  The daemon is strictly single-threaded: the
// accept loop runs handle_request() to completion under g_lock before it
// accepts anything else (linux_daemon_serve.h), so while the apply is still
// running the recovery query cannot be accepted either.  It therefore expired
// too, and the client reported "outcome is pending or unknown" in exactly the
// case the recovery exists to cover.  The TUI presented a SUCCEEDED apply as
// "Apply failed" with the draft still dirty.
//
// Every deadline below is DERIVED from what the daemon is allowed to spend.
// The static_asserts at the bottom enforce that.

#include "gpu_core.h"
#include <errno.h>
// service_phase_remaining_ms() is pure arithmetic with its own suite, and the
// split-phase/total problem is identical on both transports.  Reusing it keeps
// one implementation of "how long may this phase still run" rather than two
// that can drift.
#include "service_request_deadline_policy.h"

// --- Daemon-side bounds ------------------------------------------------------

// fan_reassert_thread() (linux_fan_runtime.h) takes g_lock and performs a
// telemetry read plus a fan write under it, at least every 250 ms.  Every
// request's turnaround carries one such hold before handle_request() can take
// the lock.  The Linux counterpart of SERVICE_STATE_READ_RUNTIME_LOCK_WAIT_MS.
#define GC_DAEMON_FAN_RUNTIME_LOCK_WAIT_MS 250u

// linux_backend_refresh() plus populate_snapshot(), the live hardware read
// every answering handler ends with.  ONE number, unlike Windows: on Linux
// GET_SNAPSHOT and GET_TELEMETRY are literally the same case label in
// handle_request(), so a separate telemetry budget would be a fiction.
#define GC_DAEMON_STATE_REFRESH_BUDGET_MS 2500u

// One durable daemon record: same-directory temp, write, fsync, atomic rename,
// directory fsync (linux_daemon_state.cpp).  Two fsyncs against whatever the
// root filesystem is, which on a busy spinning disk is not fast.
#define GC_DAEMON_DURABLE_RECORD_BUDGET_MS 1000u

// Worst-case record writes inside ONE mutation, counted from linux_daemon.cpp:
// PREPARED, then ACTIVE, and on the persistence-failure path
// restore_committed_record() plus an UNCERTAIN marker.
#define GC_DAEMON_MUTATION_RECORD_WRITES 4u

// One full hardware pass: the VF curve, clock offsets, power limit, fan, and
// the write-verify readback that goes with them.
#define GC_DAEMON_HARDWARE_WRITE_BUDGET_MS 6000u

// A failed mutation rolls the hardware back before it answers, so the worst
// case is the write pass plus the restore pass.
#define GC_DAEMON_MUTATION_WRITE_PASSES 2u

// Framing once the socket is connected: two fixed-size struct transfers.
// Deliberately generous; it is not where the time goes.
#define GC_DAEMON_RESPONSE_FRAMING_BUDGET_MS 250u

// How long the DAEMON waits on a peer that has connected but is not completing
// its fixed-size request, and how long it waits for a peer to drain its
// response.  This is the one role the old literal answered correctly: no honest
// client pauses between connect() and one fixed-size write, so a small bound is
// exactly right and a large one is a local denial-of-service surface (requests
// are serviced one at a time).  Kept at the old value deliberately.
#define GC_DAEMON_PEER_STALL_BUDGET_MS 2000u

// How long a CLIENT waits for the socket to accept its connection.  This is an
// availability question ("is the daemon up?"), not a turnaround question, which
// is why it is a separate budget: a stopped daemon must still be reported
// promptly no matter how patient the response deadlines below are.  It is also
// the reason client_connect() must connect non-blockingly -- a blocking
// connect() against a full listen backlog has no bound at all.
#define GC_DAEMON_CONNECT_BUDGET_MS 2000u

// --- Command classes ---------------------------------------------------------

enum LinuxDaemonDeadlineClass {
    // Answers from memory plus one live refresh.
    LINUX_DAEMON_DEADLINE_STATE_READ = 0,
    // Writes one durable record, then refreshes.  No hardware write.
    LINUX_DAEMON_DEADLINE_RECORD_WRITE,
    // Writes the GPU, with durable records around it and a rollback pass on
    // the failure path.
    LINUX_DAEMON_DEADLINE_MUTATION,
    // The post-timeout operation query.  Trivial to answer, but it is queued
    // behind the very mutation it is asking about, so its deadline is a
    // mutation deadline and not a read one.  This distinction is the fix.
    LINUX_DAEMON_DEADLINE_OPERATION_RECOVERY,
};

static constexpr LinuxDaemonDeadlineClass linux_daemon_deadline_class(
    unsigned int command) {
    switch (command) {
        case SERVICE_CMD_APPLY:
        case SERVICE_CMD_RESET:
        case SERVICE_CMD_RESUME_RESTORE:
            return LINUX_DAEMON_DEADLINE_MUTATION;
        case SERVICE_CMD_SET_STARTUP_POLICY:
        case SERVICE_CMD_REFRESH_STARTUP_PROFILE:
            return LINUX_DAEMON_DEADLINE_RECORD_WRITE;
        case SERVICE_CMD_GET_OPERATION_RESULT:
            return LINUX_DAEMON_DEADLINE_OPERATION_RECOVERY;
        default:
            return LINUX_DAEMON_DEADLINE_STATE_READ;
    }
}

// --- Handler budgets ---------------------------------------------------------

static constexpr unsigned long linux_daemon_state_read_handler_budget_ms() {
    return (unsigned long)GC_DAEMON_FAN_RUNTIME_LOCK_WAIT_MS +
        (unsigned long)GC_DAEMON_STATE_REFRESH_BUDGET_MS;
}

static constexpr unsigned long linux_daemon_record_write_handler_budget_ms() {
    return linux_daemon_state_read_handler_budget_ms() +
        (unsigned long)GC_DAEMON_DURABLE_RECORD_BUDGET_MS;
}

static constexpr unsigned long linux_daemon_mutation_handler_budget_ms() {
    return linux_daemon_state_read_handler_budget_ms() +
        (unsigned long)GC_DAEMON_MUTATION_WRITE_PASSES *
            (unsigned long)GC_DAEMON_HARDWARE_WRITE_BUDGET_MS +
        (unsigned long)GC_DAEMON_MUTATION_RECORD_WRITES *
            (unsigned long)GC_DAEMON_DURABLE_RECORD_BUDGET_MS;
}

static constexpr unsigned long linux_daemon_handler_budget_ms(
    LinuxDaemonDeadlineClass commandClass) {
    return commandClass == LINUX_DAEMON_DEADLINE_MUTATION
        ? linux_daemon_mutation_handler_budget_ms()
        : commandClass == LINUX_DAEMON_DEADLINE_RECORD_WRITE
            ? linux_daemon_record_write_handler_budget_ms()
            : linux_daemon_state_read_handler_budget_ms();
}

// What one request can be queued behind before it is even ACCEPTED.
//
// This is where Linux is structurally worse than Windows and the budget has to
// say so.  Windows serializes dispatch but still accepts the connection and
// reads the request; the Linux accept loop is blocked outright, so a queued
// request sees the full handler ahead of it with nothing overlapped.
//
// A concurrent MUTATION is deliberately excluded from the read and record-write
// lanes, for the same reason the Windows header excludes a concurrent APPLY: a
// mutation is bounded by the daemon's own watchdog, and one stale telemetry
// frame is the correct degradation for it -- provided the client does not
// mistake the expiry for "the daemon is gone", which is what
// linux_daemon_failure_means_offline() below is for.
static constexpr unsigned long linux_daemon_serialization_budget_ms() {
    return linux_daemon_record_write_handler_budget_ms();
}

// --- Client response deadlines -----------------------------------------------

// Strictly greater than what the daemon may spend, so an expiry means the
// daemon really did fail its own contract rather than that the client got
// impatient.
static constexpr unsigned long linux_daemon_response_timeout_ms(
    LinuxDaemonDeadlineClass commandClass) {
    return (commandClass == LINUX_DAEMON_DEADLINE_OPERATION_RECOVERY
                // It must outlast the mutation it is queued behind; answering
                // it costs nothing once the lock is free.
                ? linux_daemon_mutation_handler_budget_ms()
                : linux_daemon_serialization_budget_ms() +
                    linux_daemon_handler_budget_ms(commandClass)) +
        (unsigned long)GC_DAEMON_RESPONSE_FRAMING_BUDGET_MS;
}

static constexpr unsigned long linux_daemon_command_response_timeout_ms(
    unsigned int command) {
    return linux_daemon_response_timeout_ms(linux_daemon_deadline_class(command));
}

// The wall clock a client may spend on ONE mutation including its outcome
// recovery.  Reaching it means the daemon accepted the request, blew its own
// mutation budget, and then blew a trivial query's budget too -- i.e. it is
// wedged, and OUTCOME UNKNOWN is the truthful answer rather than a guess.
// systemd's WatchdogSec= is what resolves that state, not this client.
static constexpr unsigned long linux_daemon_mutation_total_budget_ms() {
    return linux_daemon_response_timeout_ms(LINUX_DAEMON_DEADLINE_MUTATION) +
        linux_daemon_response_timeout_ms(
            LINUX_DAEMON_DEADLINE_OPERATION_RECOVERY);
}

// How long the next operation-result query may still wait, given how long the
// mutation and any earlier recovery attempts already spent.  Zero means the
// total budget is exhausted and the outcome must be reported as unknown.
//
// Expressed through the shared phase arithmetic so recovery can retry without
// the total wait doubling per attempt, and without a sleep or a poll interval:
// each attempt blocks in poll() on a real descriptor for its own slice.
static constexpr unsigned long linux_daemon_recovery_remaining_ms(
    unsigned long mutationElapsedMs) {
    return service_phase_remaining_ms(
        0ul,
        linux_daemon_response_timeout_ms(
            LINUX_DAEMON_DEADLINE_OPERATION_RECOVERY),
        mutationElapsedMs,
        linux_daemon_mutation_total_budget_ms());
}

// --- Reachability evidence ---------------------------------------------------

// What one failed exchange proves about the daemon's existence.  The Linux
// counterpart of service_client_tracked_instance_after_request(): a request
// that was accepted and then not answered in time is evidence that the daemon
// is BUSY, and none at all that it is gone.  Collapsing the two is what made a
// slow apply tear down the TUI's whole live presentation.
enum LinuxDaemonReachability {
    // Nothing was attempted, or the exchange succeeded.
    LINUX_DAEMON_REACHABILITY_UNKNOWN = 0,
    // connect() itself failed: no such socket, refused, or forbidden.  This is
    // a definite answer and the only one that means "offline".
    LINUX_DAEMON_REACHABILITY_UNREACHABLE,
    // The socket accepted the connection, so a daemon exists and owns it.  A
    // later timeout, EOF or protocol failure says something about this
    // exchange, not about the daemon's presence.
    LINUX_DAEMON_REACHABILITY_CONNECTED,
};

static constexpr bool linux_daemon_failure_means_offline(
    LinuxDaemonReachability reachability) {
    return reachability == LINUX_DAEMON_REACHABILITY_UNREACHABLE;
}

// What a failed connect() proves.  EAGAIN is the interesting one: the kernel
// returns it from a non-blocking AF_UNIX connect() only when the pathname IS a
// listening socket whose backlog is full, so it is positive evidence that a
// daemon exists -- it is saturated, not absent.  Reporting that as "daemon
// offline" would be the same conflation the reachability rule exists to stop.
static constexpr LinuxDaemonReachability linux_daemon_connect_reachability(
    int connectErrorNumber) {
    switch (connectErrorNumber) {
        case EAGAIN:
#if defined(EWOULDBLOCK) && EWOULDBLOCK != EAGAIN
        case EWOULDBLOCK:
#endif
        case EINPROGRESS:
        case EINTR:
        case ETIMEDOUT:
            return LINUX_DAEMON_REACHABILITY_CONNECTED;
        default:
            // ENOENT, ECONNREFUSED, EACCES, EPERM, ENOTDIR: either nothing is
            // listening or this process may not use it.  Both are definite.
            return LINUX_DAEMON_REACHABILITY_UNREACHABLE;
    }
}

// Whether a connect failure should be explained as a saturated daemon rather
// than an absent one.  Split from the reachability answer because the two
// drive different things: presentation (stay online) and wording.
static constexpr bool linux_daemon_connect_is_saturated(
    int connectErrorNumber) {
    return connectErrorNumber == EAGAIN
#if defined(EWOULDBLOCK) && EWOULDBLOCK != EAGAIN
        || connectErrorNumber == EWOULDBLOCK
#endif
        || connectErrorNumber == ETIMEDOUT;
}

// What one attempted exchange reported back, beyond success or failure.  Two
// facts, because they answer different questions and collapsing them is what
// the Linux client did wrong: whether a daemon exists (presentation), and
// whether the failure was the daemon running out of time (retry).
struct LinuxDaemonSendOutcome {
    LinuxDaemonReachability reachability;
    // The daemon accepted the request and did not answer inside the budget.
    // The ONLY failure that is evidence of ongoing work rather than of a
    // settled result.
    bool deadlineExpired;
};

// Whether the operation-result query may be attempted again.
//
// Only a deadline expiry against a daemon that owns the socket justifies it:
// that is the one failure meaning "still working on the mutation we asked
// about", and the attempt that follows blocks on a real descriptor for its
// whole slice.  Every other failure -- unreachable, backlog saturated,
// protocol mismatch, EOF -- is returned promptly, so retrying it would burn the
// remaining budget in a spin instead of waiting.  Retrying those is exactly the
// kind of timing bandaid this transport must not grow.
static constexpr bool linux_daemon_recovery_may_retry(
    bool answered, const LinuxDaemonSendOutcome& outcome) {
    return !answered && outcome.deadlineExpired &&
        outcome.reachability == LINUX_DAEMON_REACHABILITY_CONNECTED;
}

// Whether an ANSWERED operation query settled the outcome.  IN_PROGRESS and
// OUTCOME_UNKNOWN are both non-answers about the hardware, but they are final
// answers about this daemon: it is responsive, and that is what its record
// says.  Asking a responsive daemon the same question again is a spin.
static constexpr bool linux_daemon_recovery_state_is_settled(
    unsigned int operationState) {
    return operationState != SERVICE_OPERATION_IN_PROGRESS &&
        operationState != SERVICE_OPERATION_OUTCOME_UNKNOWN;
}

// Whether a failed mutation left the client genuinely unable to say what the
// hardware did.  Only true when the outcome was never recovered: a daemon that
// answered with a refusal, or whose operation record was read back, is a known
// outcome no matter how the first attempt failed.
static constexpr bool linux_daemon_outcome_is_unknown(bool recovered,
                                                   bool receivedDaemonAnswer) {
    return !recovered && !receivedDaemonAnswer;
}

// --- Contract enforcement ----------------------------------------------------

static_assert(linux_daemon_mutation_handler_budget_ms() >
    linux_daemon_record_write_handler_budget_ms(),
    "A mutation writes the GPU twice on the rollback path and fsyncs four "
    "records; it cannot be budgeted at or below a single record write");
static_assert(linux_daemon_record_write_handler_budget_ms() >
    linux_daemon_state_read_handler_budget_ms(),
    "A record write is a state read plus an fsync-ed record");
static_assert(linux_daemon_state_read_handler_budget_ms() >
    (unsigned long)GC_DAEMON_FAN_RUNTIME_LOCK_WAIT_MS,
    "The handler budget must contain the fan-thread lock hold the handler "
    "actually waits out, not just the refresh after it");

// The literal this header replaced, asserted per lane so a future edit cannot
// quietly reintroduce a client deadline below the daemon's own bound.
static_assert(linux_daemon_response_timeout_ms(
    LINUX_DAEMON_DEADLINE_STATE_READ) > 2000ul &&
    linux_daemon_response_timeout_ms(
        LINUX_DAEMON_DEADLINE_RECORD_WRITE) > 2000ul &&
    linux_daemon_response_timeout_ms(
        LINUX_DAEMON_DEADLINE_MUTATION) > 2000ul &&
    linux_daemon_response_timeout_ms(
        LINUX_DAEMON_DEADLINE_OPERATION_RECOVERY) > 2000ul,
    "Every client response deadline must stay above the 2026-09-12 "
    "GC_DAEMON_IO_TIMEOUT_MS literal it replaced");

static_assert(linux_daemon_response_timeout_ms(
    LINUX_DAEMON_DEADLINE_MUTATION) >
    linux_daemon_mutation_handler_budget_ms(),
    "The mutation deadline must exceed what the daemon may spend mutating");
static_assert(linux_daemon_response_timeout_ms(
    LINUX_DAEMON_DEADLINE_STATE_READ) >
    linux_daemon_state_read_handler_budget_ms() +
        linux_daemon_serialization_budget_ms(),
    "A read must survive being queued behind one record-write dispatch");

// The defect this header was written for: the recovery query has to outlast the
// mutation it is asking about, or it can only ever recover the rare case where
// the daemon answered fast and the response was lost.
static_assert(linux_daemon_response_timeout_ms(
    LINUX_DAEMON_DEADLINE_OPERATION_RECOVERY) >
    linux_daemon_mutation_handler_budget_ms(),
    "The operation-result query is queued behind the mutation it recovers, so "
    "its deadline must exceed the mutation handler budget");
static_assert(linux_daemon_response_timeout_ms(
    LINUX_DAEMON_DEADLINE_OPERATION_RECOVERY) >
    linux_daemon_response_timeout_ms(LINUX_DAEMON_DEADLINE_STATE_READ),
    "Recovery is a mutation-class wait, not a read-class one");

// Availability and turnaround stay separate budgets, in both directions:
// a stopped daemon must be reported promptly, and a peer that stalls mid-frame
// must not be granted a mutation-sized window against a single-threaded server.
static_assert((unsigned long)GC_DAEMON_CONNECT_BUDGET_MS <
    linux_daemon_response_timeout_ms(LINUX_DAEMON_DEADLINE_STATE_READ),
    "Finding the daemon must be cheaper than waiting for its answer");
static_assert((unsigned long)GC_DAEMON_PEER_STALL_BUDGET_MS <
    linux_daemon_response_timeout_ms(LINUX_DAEMON_DEADLINE_MUTATION),
    "The daemon's per-peer stall bound is not a turnaround budget and must "
    "stay far below one");

static_assert(linux_daemon_mutation_total_budget_ms() >
    linux_daemon_response_timeout_ms(LINUX_DAEMON_DEADLINE_MUTATION),
    "The total mutation budget must leave room for at least one recovery");
static_assert(linux_daemon_recovery_remaining_ms(0ul) ==
    linux_daemon_response_timeout_ms(
        LINUX_DAEMON_DEADLINE_OPERATION_RECOVERY),
    "A recovery that starts with nothing spent gets its whole slice");
static_assert(linux_daemon_recovery_remaining_ms(
    linux_daemon_mutation_total_budget_ms()) == 0ul,
    "An exhausted total budget must stop the recovery loop rather than "
    "granting another full slice");

#endif // GREEN_CURVE_LINUX_DAEMON_DEADLINE_POLICY_H
