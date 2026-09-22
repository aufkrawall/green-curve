// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// Pure policy: WHY did installing or removing the background service fail, and
// what should the user do about it?
//
// THE DEFECT THIS EXISTS FOR (audit 2026-09-22):
//
//   The GUI's service checkbox runs `greencurve.exe --service-install` through
//   ShellExecuteEx("runas") and then reports `GetExitCodeProcess`.  Every
//   failure exits 1, so the only thing the user ever saw was
//
//       Elevated service helper failed (exit code 1)
//
//   The real reason ("Failed updating service configuration (error 1072)")
//   went to the helper's own greencurve_cli_log.txt, which resolve_data_paths()
//   places under the LocalAppData of whichever account approved the UAC prompt.
//   On a standard-user machine that is an administrator's profile the user
//   cannot open, and the GUI never named the file anyway.  A user in that state
//   has literally nothing to report, which is exactly how the field report that
//   prompted this arrived: "cannot get the service installed", no details.
//
// THE RULE: the helper's EXIT CODE carries a classified reason, and the parent
// renders the full sentence.  A number that survives a process boundary needs
// no shared file, no handoff path an elevated process would have to open in a
// location its unelevated caller controls, and no new attack surface.  The
// taxonomy is therefore fine-grained enough to replace the Win32 number it
// cannot carry: "marked for deletion" is actionable, "error 1072" is not.
//
// Host-neutral on purpose, like service_path_chain_policy.h: the regression
// harness pins the whole mapping on every host, including the round trip
// through the exit code, which is the part a process boundary would otherwise
// hide until a user hit it.

#ifndef GREEN_CURVE_SERVICE_ADMIN_REASON_POLICY_H
#define GREEN_CURVE_SERVICE_ADMIN_REASON_POLICY_H

// Win32 codes spelled locally so this header stays free of <windows.h> and can
// be compiled by the cross-platform harness.  Prefixed rather than reusing the
// SDK names, because the header IS included where <windows.h> already defined
// them and a redefinition would be a warning at best.
#define GC_SVC_ERR_ACCESS_DENIED            5ul
#define GC_SVC_ERR_SERVICE_REQUEST_TIMEOUT  1053ul
#define GC_SVC_ERR_SERVICE_ALREADY_RUNNING  1056ul
#define GC_SVC_ERR_SERVICE_DISABLED         1058ul
#define GC_SVC_ERR_SERVICE_DOES_NOT_EXIST   1060ul
#define GC_SVC_ERR_SERVICE_NOT_ACTIVE       1062ul
#define GC_SVC_ERR_SERVICE_MARKED_FOR_DELETE 1072ul
#define GC_SVC_ERR_SERVICE_EXISTS           1073ul
#define GC_SVC_ERR_SERVICE_NO_THREAD        1054ul
#define GC_SVC_ERR_CANCELLED                1223ul
#define GC_SVC_ERR_SHARING_VIOLATION        32ul
#define GC_SVC_ERR_WRITE_PROTECT            19ul

// One reason per thing a user can actually do something about.  Ordering is
// part of the wire format (see gc_service_admin_reason_exit_code): append only.
enum GcServiceAdminReason {
    GC_SVC_ADMIN_OK = 0,
    // The failure did not match anything more specific.  Keeps exit code 1 so
    // an older GUI driving a newer helper, or the reverse, still reports a
    // failure rather than mistaking an unknown code for success.
    GC_SVC_ADMIN_UNKNOWN,
    GC_SVC_ADMIN_NOT_ELEVATED,
    GC_SVC_ADMIN_ELEVATION_DECLINED,
    GC_SVC_ADMIN_SCM_UNAVAILABLE,
    GC_SVC_ADMIN_BINARY_MISSING,
    GC_SVC_ADMIN_LOCATION_REFUSED,
    GC_SVC_ADMIN_DIRECTORY_HARDENING_FAILED,
    GC_SVC_ADMIN_BINARY_IN_USE,
    GC_SVC_ADMIN_BINARY_STAGING_FAILED,
    GC_SVC_ADMIN_MARKED_FOR_DELETE,
    GC_SVC_ADMIN_REGISTRATION_FAILED,
    GC_SVC_ADMIN_DISABLED_BY_POLICY,
    GC_SVC_ADMIN_START_FAILED,
    GC_SVC_ADMIN_START_TIMED_OUT,
    GC_SVC_ADMIN_STOP_TIMED_OUT,
    GC_SVC_ADMIN_REMOVE_FAILED,
    GC_SVC_ADMIN_HELPER_LAUNCH_FAILED,
    GC_SVC_ADMIN_HELPER_TIMED_OUT,
    GC_SVC_ADMIN_SHUTDOWN_ABANDONED,
    GC_SVC_ADMIN_REASON_COUNT
};

// Where the call that failed sat, so one Win32 code can mean different things
// in different places (ACCESS_DENIED opening the SCM is "not elevated";
// ACCESS_DENIED replacing the binary is not).
enum GcServiceAdminStage {
    GC_SVC_STAGE_OPEN_SCM = 0,
    GC_SVC_STAGE_STAGE_BINARY,
    GC_SVC_STAGE_REGISTER,
    GC_SVC_STAGE_START,
    GC_SVC_STAGE_STOP,
    GC_SVC_STAGE_REMOVE
};

// Exit codes start well above the shell's own conventions and above the
// installer's documented 0/1/2/3 so a code can never be confused with one of
// those.  UNKNOWN deliberately stays 1: that is what every build before this
// header returned, and a GUI that cannot classify a code must still call it a
// failure.
#define GC_SVC_ADMIN_EXIT_BASE 40

static inline int gc_service_admin_reason_exit_code(int reason) {
    if (reason == GC_SVC_ADMIN_OK) return 0;
    if (reason <= GC_SVC_ADMIN_UNKNOWN || reason >= GC_SVC_ADMIN_REASON_COUNT) return 1;
    return GC_SVC_ADMIN_EXIT_BASE + (reason - GC_SVC_ADMIN_UNKNOWN - 1);
}

// The inverse.  Any code this build does not recognize is UNKNOWN, never OK:
// a helper from a future build that invents a reason must not read as success.
// The offset is bounded BEFORE the narrowing cast: a helper that could not
// even be launched reports (DWORD)-1, and casting that out-of-range value to
// int is implementation-defined rather than the UNKNOWN this must return.
static inline int gc_service_admin_reason_from_exit_code(unsigned long exitCode) {
    if (exitCode == 0) return GC_SVC_ADMIN_OK;
    if (exitCode < (unsigned long)GC_SVC_ADMIN_EXIT_BASE) return GC_SVC_ADMIN_UNKNOWN;
    unsigned long offset = exitCode - (unsigned long)GC_SVC_ADMIN_EXIT_BASE;
    unsigned long reasonCount = (unsigned long)(GC_SVC_ADMIN_REASON_COUNT -
        GC_SVC_ADMIN_UNKNOWN - 1);
    if (offset >= reasonCount) return GC_SVC_ADMIN_UNKNOWN;
    return (int)offset + GC_SVC_ADMIN_UNKNOWN + 1;
}

// Map a Win32 error at a known call site onto a reason.  Unrecognized codes
// fall back to the stage's own generic reason rather than to UNKNOWN, because
// "the service could not be registered" is still more useful than "something
// failed" and the numeric code accompanies it in the log either way.
static inline int gc_service_admin_classify_win32(int stage, unsigned long err) {
    switch (stage) {
        case GC_SVC_STAGE_OPEN_SCM:
            if (err == GC_SVC_ERR_ACCESS_DENIED) return GC_SVC_ADMIN_NOT_ELEVATED;
            return GC_SVC_ADMIN_SCM_UNAVAILABLE;
        case GC_SVC_STAGE_STAGE_BINARY:
            if (err == GC_SVC_ERR_SHARING_VIOLATION) return GC_SVC_ADMIN_BINARY_IN_USE;
            if (err == GC_SVC_ERR_ACCESS_DENIED) return GC_SVC_ADMIN_BINARY_IN_USE;
            if (err == GC_SVC_ERR_WRITE_PROTECT) return GC_SVC_ADMIN_BINARY_STAGING_FAILED;
            return GC_SVC_ADMIN_BINARY_STAGING_FAILED;
        case GC_SVC_STAGE_REGISTER:
            if (err == GC_SVC_ERR_SERVICE_MARKED_FOR_DELETE) return GC_SVC_ADMIN_MARKED_FOR_DELETE;
            if (err == GC_SVC_ERR_ACCESS_DENIED) return GC_SVC_ADMIN_NOT_ELEVATED;
            return GC_SVC_ADMIN_REGISTRATION_FAILED;
        case GC_SVC_STAGE_START:
            if (err == GC_SVC_ERR_SERVICE_DISABLED) return GC_SVC_ADMIN_DISABLED_BY_POLICY;
            if (err == GC_SVC_ERR_SERVICE_MARKED_FOR_DELETE) return GC_SVC_ADMIN_MARKED_FOR_DELETE;
            if (err == GC_SVC_ERR_SERVICE_REQUEST_TIMEOUT) return GC_SVC_ADMIN_START_TIMED_OUT;
            return GC_SVC_ADMIN_START_FAILED;
        case GC_SVC_STAGE_STOP:
            return GC_SVC_ADMIN_STOP_TIMED_OUT;
        case GC_SVC_STAGE_REMOVE:
            if (err == GC_SVC_ERR_SERVICE_MARKED_FOR_DELETE) return GC_SVC_ADMIN_MARKED_FOR_DELETE;
            if (err == GC_SVC_ERR_ACCESS_DENIED) return GC_SVC_ADMIN_NOT_ELEVATED;
            return GC_SVC_ADMIN_REMOVE_FAILED;
        default:
            break;
    }
    return GC_SVC_ADMIN_UNKNOWN;
}

// The sentence the user reads.  Each one names the cause AND the next action,
// because a reason without a remedy is the same dead end in nicer words.
// Present tense, no error numbers: the number is in the log, and a user who
// can act on "close Services.msc and restart Windows" does not need 1072.
static inline const char* gc_service_admin_reason_text(int reason) {
    switch (reason) {
        case GC_SVC_ADMIN_OK:
            return "";
        case GC_SVC_ADMIN_NOT_ELEVATED:
            return "This needs administrator rights. Approve the Windows "
                   "elevation prompt, or run greencurve.exe --service-install "
                   "from an elevated PowerShell or Command Prompt.";
        case GC_SVC_ADMIN_ELEVATION_DECLINED:
            return "The Windows elevation prompt was declined. Registering a "
                   "background service always requires administrator rights.";
        case GC_SVC_ADMIN_SCM_UNAVAILABLE:
            return "The Windows Service Control Manager could not be opened. "
                   "This is usually a security product or a group policy "
                   "blocking service management on this PC.";
        case GC_SVC_ADMIN_BINARY_MISSING:
            return "greencurve-service.exe is missing from the folder "
                   "greencurve.exe was started from. Extract the whole archive, "
                   "keep both programs together, and check whether antivirus "
                   "quarantined the service binary.";
        case GC_SVC_ADMIN_LOCATION_REFUSED:
            return "Green Curve will not register the background service from "
                   "this folder, because securing it would take write access to "
                   "the folder away from you. Move greencurve.exe and "
                   "greencurve-service.exe into a folder of their own - for "
                   "example C:\\Program Files\\Green Curve - and try again.";
        case GC_SVC_ADMIN_DIRECTORY_HARDENING_FAILED:
            return "The program folder's permissions could not be secured, so "
                   "the service was not registered. Install into a local "
                   "NTFS folder such as C:\\Program Files\\Green Curve.";
        case GC_SVC_ADMIN_BINARY_IN_USE:
            return "greencurve-service.exe is in use and could not be replaced. "
                   "Close Green Curve everywhere, stop the Green Curve service "
                   "in Services, and try again.";
        case GC_SVC_ADMIN_BINARY_STAGING_FAILED:
            return "The service binary could not be written into the program "
                   "folder. Check that the drive is not full or write-protected "
                   "and that antivirus is not blocking the file.";
        case GC_SVC_ADMIN_MARKED_FOR_DELETE:
            return "Windows still has the previous Green Curve service marked "
                   "for deletion and will not accept a new one until every "
                   "handle to it is gone. Close the Services window and Task "
                   "Manager's Services tab, then restart Windows and try again.";
        case GC_SVC_ADMIN_REGISTRATION_FAILED:
            return "The service could not be registered with Windows. A "
                   "security product blocking service creation is the usual "
                   "cause.";
        case GC_SVC_ADMIN_DISABLED_BY_POLICY:
            return "The Green Curve service is disabled on this PC and Windows "
                   "refused to start it. Re-enable it in Services, or check "
                   "whether a group policy or security product disabled it.";
        case GC_SVC_ADMIN_START_FAILED:
            return "The service was registered but Windows refused to start it.";
        case GC_SVC_ADMIN_START_TIMED_OUT:
            return "The service was registered but did not finish starting in "
                   "time. This is usually antivirus scanning the new binary on "
                   "first run; it often succeeds on a second try or after a "
                   "restart.";
        case GC_SVC_ADMIN_STOP_TIMED_OUT:
            return "The running Green Curve service did not stop in time, so "
                   "its binary could not be replaced. Restart Windows and try "
                   "again.";
        case GC_SVC_ADMIN_REMOVE_FAILED:
            return "The service could not be removed.";
        case GC_SVC_ADMIN_HELPER_LAUNCH_FAILED:
            return "The elevated helper could not be started.";
        case GC_SVC_ADMIN_HELPER_TIMED_OUT:
            return "The elevated helper did not finish in time and was stopped. "
                   "Running the install or repair again completes whatever steps "
                   "were left over.";
        case GC_SVC_ADMIN_SHUTDOWN_ABANDONED:
            return "Green Curve closed while the elevated helper was still "
                   "working. The helper finishes on its own; reopen Green Curve "
                   "and check whether the background service is installed.";
        case GC_SVC_ADMIN_UNKNOWN:
        default:
            return "The background service could not be updated.";
    }
}

// Where the full technical detail is.  Appended to every failure the GUI shows
// that cannot carry the detail itself (the checkbox path runs the helper under
// a possibly DIFFERENT account, and the user has no other way to learn that).
// Both log names are named because both exist: the helper writes
// greencurve_cli_log.txt, an already-elevated GUI that skips the helper writes
// greencurve_debug.txt -- and in both cases the account that approved the
// elevation prompt is the one whose profile holds them.
#define GC_SVC_ADMIN_LOG_POINTER \
    "Details are in greencurve_cli_log.txt or greencurve_debug.txt under " \
    "%LOCALAPPDATA%\\Green Curve of the account that approved the elevation prompt."

// How long the admin path may wait, in one place so the three bounds cannot
// contradict each other.
//
// GC_SVC_SCM_STATE_WAIT_MS is matched to the SCM's own default
// ServicesPipeTimeout of 30 s. It was 10 s, which reported "did not start" for
// a service Windows itself was still willing to wait for -- antivirus scanning
// a freshly written binary on its first run routinely costs more than that,
// and the install then failed for something that succeeded moments later.
//
// GC_SVC_ADMIN_HELPER_TIMEOUT_MS must exceed a stop wait PLUS a start wait
// plus the staging and DACL work between them, or the unelevated parent can
// terminate a helper that was doing exactly what it was asked -- reporting a
// failure for an install that then completes anyway, which is the worst of
// both answers. The harness asserts that relationship (regression_main.cpp,
// the admin-reason block), because it is the kind of thing a later tuning
// change breaks silently.  Setup and the uninstaller wait on the same helper
// work and therefore wait on THIS constant, not on a private one.
#define GC_SVC_SCM_STATE_WAIT_MS 30000ul
#define GC_SVC_ADMIN_HELPER_TIMEOUT_MS 150000ul

// True when the reason is the user's own decision rather than a fault, so the
// GUI can report it without an error icon or a "something went wrong" framing.
static inline bool gc_service_admin_reason_is_user_cancel(int reason) {
    return reason == GC_SVC_ADMIN_ELEVATION_DECLINED;
}

// True when the outcome is nobody's fault and nothing is broken: the framing
// stays informational (no error icon).  Distinct from is_user_cancel because
// the sentence still needs saying -- an abandoned-at-shutdown result must not
// read as either a failure or a click.
static inline bool gc_service_admin_reason_is_informational(int reason) {
    return gc_service_admin_reason_is_user_cancel(reason) ||
        reason == GC_SVC_ADMIN_SHUTDOWN_ABANDONED;
}

// True when retrying without changing anything cannot help.  Used to decide
// whether the message should suggest trying again.
static inline bool gc_service_admin_reason_needs_user_action(int reason) {
    switch (reason) {
        case GC_SVC_ADMIN_NOT_ELEVATED:
        case GC_SVC_ADMIN_ELEVATION_DECLINED:
        case GC_SVC_ADMIN_BINARY_MISSING:
        case GC_SVC_ADMIN_LOCATION_REFUSED:
        case GC_SVC_ADMIN_MARKED_FOR_DELETE:
        case GC_SVC_ADMIN_DISABLED_BY_POLICY:
            return true;
        default:
            return false;
    }
}

#endif
