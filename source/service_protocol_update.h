// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// service_protocol_update.h — the in-app updater's wire types (protocol v19).
//
// Split out of service_protocol.h to keep that file inside the source-size
// ratchet, the same reason service_protocol_validation.h was extracted; it is
// included from service_protocol.h and is not meant to be included directly.
//
// ## The trust boundary these types exist to express
//
// The four update commands are TRIGGERS, never payloads.  None of them lets the
// caller name a URL, a version, a digest, a filename or a path: the service
// resolves every one of those itself, from constants compiled into the binary
// and from a manifest whose ECDSA P-256 signature it verified against a public
// key whose private half has never been in GitHub Actions.
//
// That distinction is the whole security model here.  The service runs as
// LocalSystem and ends a successful update by launching an installer with
// SYSTEM rights.  A command that accepted "which file" from an unprivileged GUI
// would be a local privilege escalation with extra steps, no matter how
// carefully the file it named were checked afterwards.  So the entire
// client-supplied surface is two integers on one command, and
// `service_request_reject_reason()` refuses anything else at the boundary
// rather than trusting four handlers to each remember not to look.
//
// ## Why the state rides on every response
//
// ServiceUpdateState is stamped onto every response by the same single place
// that stamps `outcomeSeverity`.  The GUI and tray therefore learn that an
// update exists through the polling they already do, with no extra round trip
// and no second code path that a new command branch could forget to feed.
//
// It is deliberately NOT gated on the caller passing the state-envelope
// authorization checks: it carries no hardware state, no session state and no
// settings -- only "a newer public release exists", which is a fact anyone can
// read off the releases page.

#ifndef GREEN_CURVE_SERVICE_PROTOCOL_UPDATE_H
#define GREEN_CURVE_SERVICE_PROTOCOL_UPDATE_H

// Where the service is in the check -> download -> verify -> install pipeline.
enum ServiceUpdatePhase {
    SERVICE_UPDATE_PHASE_IDLE = 0,
    SERVICE_UPDATE_PHASE_CHECKING = 1,
    SERVICE_UPDATE_PHASE_DOWNLOADING = 2,
    SERVICE_UPDATE_PHASE_VERIFYING = 3,
    // Verified and staged: the package on disk matched the digest and the exact
    // byte length in a manifest signed by a trusted key.  This is the only
    // phase from which an install may proceed.
    SERVICE_UPDATE_PHASE_READY = 4,
    SERVICE_UPDATE_PHASE_INSTALLING = 5,
    SERVICE_UPDATE_PHASE_FAILED = 6,
};

// Longest version text on the wire ("999999.999999.999999" plus terminator).
// Spelled as a literal rather than pulled from update_version_policy.h so the
// wire layout stays readable in one place; main_service_update_state.cpp
// static_asserts that the two agree.
#define SERVICE_UPDATE_VERSION_CHARS 24

struct ServiceUpdateState {
    gc_u32 phase;                 // ServiceUpdatePhase
    gc_u32 decision;              // GcUpdateDecision from the last check
    gc_u32 autoCheck;             // GcUpdateAutoCheck
    gc_u32 intervalSeconds;
    gc_u32 consecutiveFailures;
    gc_u32 lastRefusal;           // GcUpdateInstallRefusal
    gc_u64 lastCheckUnix;
    gc_u64 availableBytes;
    char availableVersion[SERVICE_UPDATE_VERSION_CHARS];
    char installedVersion[SERVICE_UPDATE_VERSION_CHARS];
    gc_bool8 packageStaged;
    gc_bool8 packageVerified;
    gc_bool8 installRunning;
    // False for a portable .7z copy, which has no installer to upgrade and no
    // Add/Remove Programs entry to read the install directory from.  Those
    // users are pointed at the release page rather than silently upgraded into
    // a layout they did not choose.
    gc_bool8 isInstalledCopy;
    // A check/download/install job is running.  Set by the service BEFORE it
    // creates the worker thread, and every response is stamped after that, so
    // the very first response following a command already reports it.
    //
    // This exists because the GUI cannot infer it from `phase`: the reply to
    // CHECK/INSTALL is sent while the worker may not have run yet, so a client
    // deciding "should I keep polling?" from the phase alone sees IDLE, stops
    // refreshing, and shows the user nothing at all -- whatever the service
    // goes on to do.  That was the first live install: the request succeeded,
    // the worker refused, and the dialog never displayed the refusal.
    //
    // Takes one of the reserved bytes rather than growing the struct, which is
    // what they are for; the size is unchanged, and a build that predates this
    // reads it as the zero it always was.
    gc_bool8 workerRunning;
    // "Close yourself now, an install is starting."
    //
    // The service launches the setup program, so setup runs in session 0 --
    // and window enumeration is session-scoped, so it cannot see, close, or
    // even detect a GUI running in the user's session.  The first real install
    // logged "no running Green Curve window found" and then failed with
    // ERROR_ACCESS_DENIED replacing greencurve.exe, which the GUI still had
    // mapped.
    //
    // A service cannot post a window message across sessions either, so the
    // shutdown request travels the one channel that already crosses them: the
    // state every GUI polls.  Each GUI sees this and runs its own ordinary Exit
    // path, releasing the tray icon, the single-instance mutex and the service
    // connection.  The service then waits for the processes to actually go and
    // escalates if they do not.
    gc_bool8 guiShutdownRequested;
    gc_bool8 updateReserved[2];
    // Why the last operation failed, in the words the GUI shows.  Never carries
    // an attacker-chosen string: a refused redirect names the hop, not the URL,
    // because echoing an attacker-supplied URL into a log and a status line is
    // a small gift with no upside.
    char detail[256];
};

#endif // GREEN_CURVE_SERVICE_PROTOCOL_UPDATE_H
