// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// When the updater may check, and when it may install.  Clock-free and pure:
// every time value is a parameter, so the whole schedule is testable without
// waiting for anything and without a sleep anywhere in the tests.
//
// ## Checking and installing are different decisions
//
// A background check is a network request that reveals an IP, a version and an
// architecture on a timer.  That is a disclosure the user should be told about
// once, which is why the auto-check setting starts UNSET rather than ON: the
// first run has to ask (or at minimum state) before the first request goes out,
// and "unset" is distinguishable from "the user said yes".
//
// Installing is a different thing entirely.  It stops the GUI, stops the
// service -- which resets the GPU to stock on the way down when it holds owned
// intent -- replaces binaries, re-registers the service and re-applies the
// captured settings.  Doing that unattended while somebody is mid-game is not
// an update, it is a crash with extra steps.  So an install requires an
// explicit click, always, and is additionally deferred while the machine is
// busy in ways that would make it visible.
//
// ## Backoff exists so failure is quiet, not so it is fast
//
// A failed check must not turn into a retry loop against GitHub.  Consecutive
// failures back off geometrically from a short base up to the normal interval,
// so a transient network drop recovers within minutes while a genuinely
// unreachable endpoint settles at the same rate a successful check would use
// and never exceeds it.

#ifndef GREEN_CURVE_UPDATE_SCHEDULE_POLICY_H
#define GREEN_CURVE_UPDATE_SCHEDULE_POLICY_H

#include <stddef.h>

// Seconds.  The default is daily; the floor exists so a hand-edited config
// cannot turn the feature into a request flood, and the ceiling so that
// "enabled" cannot quietly mean "never".
#define GC_UPDATE_INTERVAL_DEFAULT_SECONDS 86400
#define GC_UPDATE_INTERVAL_MIN_SECONDS 3600
#define GC_UPDATE_INTERVAL_MAX_SECONDS 2592000   // 30 days
#define GC_UPDATE_RETRY_BASE_SECONDS 900         // 15 minutes
#define GC_UPDATE_RETRY_MAX_DOUBLINGS 8

enum GcUpdateAutoCheck {
    // No answer recorded yet.  Distinct from OFF so the first run can disclose
    // the outbound request before making one, and so an upgrade from a build
    // without this feature does not silently start phoning home.
    GC_UPDATE_AUTO_CHECK_UNSET = 0,
    GC_UPDATE_AUTO_CHECK_OFF = 1,
    GC_UPDATE_AUTO_CHECK_ON = 2,
};

static inline int gc_update_clamp_interval(int seconds) {
    if (seconds < GC_UPDATE_INTERVAL_MIN_SECONDS) return GC_UPDATE_INTERVAL_MIN_SECONDS;
    if (seconds > GC_UPDATE_INTERVAL_MAX_SECONDS) return GC_UPDATE_INTERVAL_MAX_SECONDS;
    return seconds;
}

// Geometric backoff from the retry base, capped at the normal interval.  It is
// capped there rather than at some larger number on purpose: backing off
// *further* than the steady-state rate would mean a machine that failed once
// checks less often than one that never tried.
static inline int gc_update_next_check_delay(int intervalSeconds,
                                             int consecutiveFailures) {
    int interval = gc_update_clamp_interval(intervalSeconds);
    if (consecutiveFailures <= 0) return interval;
    int doublings = consecutiveFailures - 1;
    if (doublings > GC_UPDATE_RETRY_MAX_DOUBLINGS) doublings = GC_UPDATE_RETRY_MAX_DOUBLINGS;
    long long delay = GC_UPDATE_RETRY_BASE_SECONDS;
    for (int i = 0; i < doublings; ++i) {
        delay *= 2;
        if (delay >= interval) return interval;
    }
    if (delay > interval) return interval;
    return (int)delay;
}

// Whether a check is due.  `lastCheckUnix` of 0 means "never checked", which is
// always due.
//
// A clock that moved BACKWARDS is also treated as due rather than as a very
// long wait: a machine that had its time corrected, or that restored from a
// snapshot, would otherwise sit with a future timestamp and never check again.
// The failure mode of being wrong here is one extra request; the failure mode
// of the alternative is a permanently silent updater.
static inline bool gc_update_check_is_due(long long lastCheckUnix,
                                          long long nowUnix,
                                          int delaySeconds) {
    if (lastCheckUnix <= 0) return true;
    if (nowUnix < lastCheckUnix) return true;
    if (delaySeconds <= 0) return true;
    return (nowUnix - lastCheckUnix) >= (long long)delaySeconds;
}

// The full automatic-check gate.  Automatic means automatic: an explicit "Check
// now" from the GUI bypasses this entirely and is never rate limited by it,
// because a user who clicked a button has already expressed the intent this
// function exists to infer.
static inline bool gc_update_auto_check_allowed(GcUpdateAutoCheck setting,
                                                long long lastCheckUnix,
                                                long long nowUnix,
                                                int intervalSeconds,
                                                int consecutiveFailures) {
    if (setting != GC_UPDATE_AUTO_CHECK_ON) return false;
    int delay = gc_update_next_check_delay(intervalSeconds, consecutiveFailures);
    return gc_update_check_is_due(lastCheckUnix, nowUnix, delay);
}

// ---------------------------------------------------------------------------
// Installing
// ---------------------------------------------------------------------------

// Why an install was refused.  Separated rather than collapsed into a bool
// because every one of these ends up in the debug log and on the GUI's status
// line, and "the update did nothing" is otherwise undiagnosable.
enum GcUpdateInstallRefusal {
    GC_UPDATE_INSTALL_ALLOWED = 0,
    GC_UPDATE_INSTALL_NO_CONSENT = 1,
    GC_UPDATE_INSTALL_NOT_VERIFIED = 2,
    GC_UPDATE_INSTALL_NO_PACKAGE = 3,
    GC_UPDATE_INSTALL_BUSY_APPLYING = 4,
    GC_UPDATE_INSTALL_BUSY_FOREGROUND = 5,
    GC_UPDATE_INSTALL_NOT_INSTALLED_COPY = 6,
    GC_UPDATE_INSTALL_ALREADY_RUNNING = 7,
};

struct GcUpdateInstallGate {
    // The user clicked Install.  There is no path that sets this from a timer.
    bool userConsented;
    // The manifest signature verified against an embedded key AND the staged
    // file's SHA-256 and byte length matched the manifest.  Both, not either.
    bool packageVerified;
    bool packageStaged;
    // A hardware write is in flight; stopping the service through one is how
    // you get a half-applied curve.
    bool applyInFlight;
    // A fullscreen or auto-profile-matched application is in the foreground.
    // Stopping the service resets the GPU to stock, so this would yank the
    // overclock out from under a running game.
    bool foregroundAppActive;
    // A portable .7z copy has no installer to upgrade and no ARP entry to read
    // the install directory from; those users get pointed at the release page.
    bool isInstalledCopy;
    bool installAlreadyRunning;
};

static inline GcUpdateInstallRefusal gc_update_install_decision(
    const GcUpdateInstallGate* gate) {
    if (!gate) return GC_UPDATE_INSTALL_NO_CONSENT;
    // Consent is checked first so that the log's first line about a refused
    // install is never a technical detail when the real answer is "nobody
    // asked for this".
    if (!gate->userConsented) return GC_UPDATE_INSTALL_NO_CONSENT;
    if (gate->installAlreadyRunning) return GC_UPDATE_INSTALL_ALREADY_RUNNING;
    if (!gate->packageStaged) return GC_UPDATE_INSTALL_NO_PACKAGE;
    if (!gate->packageVerified) return GC_UPDATE_INSTALL_NOT_VERIFIED;
    if (!gate->isInstalledCopy) return GC_UPDATE_INSTALL_NOT_INSTALLED_COPY;
    if (gate->applyInFlight) return GC_UPDATE_INSTALL_BUSY_APPLYING;
    if (gate->foregroundAppActive) return GC_UPDATE_INSTALL_BUSY_FOREGROUND;
    return GC_UPDATE_INSTALL_ALLOWED;
}

static inline const char* gc_update_install_refusal_text(
    GcUpdateInstallRefusal refusal) {
    switch (refusal) {
        case GC_UPDATE_INSTALL_ALLOWED: return "allowed";
        case GC_UPDATE_INSTALL_NO_CONSENT: return "no explicit user consent";
        case GC_UPDATE_INSTALL_NOT_VERIFIED: return "package failed verification";
        case GC_UPDATE_INSTALL_NO_PACKAGE: return "no staged package";
        case GC_UPDATE_INSTALL_BUSY_APPLYING: return "a hardware apply is in flight";
        case GC_UPDATE_INSTALL_BUSY_FOREGROUND: return "a fullscreen application is active";
        case GC_UPDATE_INSTALL_NOT_INSTALLED_COPY: return "this is not an installed copy";
        case GC_UPDATE_INSTALL_ALREADY_RUNNING: return "an install is already running";
    }
    return "unknown";
}

// Downloading is deliberately a weaker gate than installing: it writes a file
// into a protected staging directory and changes nothing about the running
// system, so it is allowed to happen on the automatic path once an update is
// known.  It still requires the auto-check setting to be ON, because a user who
// turned checking off has not agreed to background traffic either.
static inline bool gc_update_auto_download_allowed(GcUpdateAutoCheck setting,
                                                   bool updateAvailable,
                                                   bool alreadyStaged) {
    if (setting != GC_UPDATE_AUTO_CHECK_ON) return false;
    if (!updateAvailable) return false;
    return !alreadyStaged;
}

#endif // GREEN_CURVE_UPDATE_SCHEDULE_POLICY_H
