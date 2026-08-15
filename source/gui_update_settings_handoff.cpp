// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// Carrying the applied settings across an in-app update, entirely inside the
// user's session.
//
// ## Why setup cannot do this when the updater drives it
//
// Setup normally captures the active settings itself, by running
// `greencurve.exe --export-active-settings` and, at the end,
// `--apply-settings-file`.  Both of those talk to the service, and
// `service_caller_is_authorized()` restricts control to the **active
// interactive session**.
//
// When the user runs setup by hand that is fine -- the helpers inherit the
// user's session.  When the *updater* runs it, setup was launched by the
// LocalSystem service and lives in session 0, so both helpers are refused:
//
//     service auth reject: source=client ping pid=1280 session=0 activeSession=1
//                          user=NT-AUTORITAET\SYSTEM
//     capture: no settings snapshot from either binary.
//              The upgrade will not re-apply settings.
//
// That is measured, not theorised: it is the 2026-08-14 log of the first
// successful in-app update, which installed correctly and restored nothing.
//
// ## Why the GUI does it instead of the service
//
// Three places could own this, and the other two are worse:
//
//   - **Setup**, by launching its helpers into the interactive session with
//     `WTSQueryUserToken`.  Workable, but it moves the settings file into a
//     user-writable location to let a user-token child write it, and it makes
//     the fix untestable without SYSTEM.
//   - **The service**, by exporting its own in-memory intent and re-applying it
//     after the restart.  Tempting -- no IPC, no session -- but it creates a
//     new path on which the service writes to the GPU without a client asking,
//     and auto-restore-policy.md guards exactly that.  A convenience is not a
//     good reason to open it.
//   - **The GUI**, which is already in the authorized session, already holds a
//     service connection, and is already being closed and restarted as part of
//     the update.  It needs no new privilege, crosses no session boundary, and
//     uses the same explicit-client-apply model the policy already blesses.
//
// So: the GUI exports before it asks for the install, and the relaunched GUI
// replays it through the ordinary `--apply-settings-file` CLI verb.  Setup's
// own capture still runs and still fails in session 0, harmlessly -- it is
// already written to tolerate that, and leaving it alone keeps the interactive
// install path byte-for-byte unchanged.
//
// ## Degrading well
//
// Every failure here leaves the update itself untouched:
//   - nothing applied  -> nothing to export, nothing to restore
//   - export fails     -> the update proceeds; settings are simply not carried
//   - relaunch fails   -> the file waits, and the next manual start restores it
//   - apply fails      -> one attempt only, then the file is gone

#define GC_UPDATE_PENDING_RESTORE_NAME "pending-update-restore.ini"
// The file is renamed to this before the replay runs, so a restore is attempted
// exactly once.  A crash mid-apply must not leave a file that re-applies on
// every future start.
#define GC_UPDATE_PENDING_RESTORE_APPLYING "pending-update-restore.applying.ini"

#include "update_restore_policy.h"

static bool gui_update_pending_restore_path(const char* leaf, char* out, size_t outSize) {
    if (out && outSize) out[0] = 0;
    if (!g_userDataDir[0]) return false;
    return SUCCEEDED(StringCchPrintfA(out, outSize, "%s\\%s", g_userDataDir, leaf));
}

// Capture the applied settings just before asking the service to install.
//
// Returns false when there is nothing to carry, which is a perfectly ordinary
// outcome: `settings_transfer_export()` deliberately fails when the service
// holds no active intent, because an upgrade with nothing applied must not
// "restore" a synthesized stock profile afterwards.
static bool gui_update_capture_settings_for_restore() {
    char path[MAX_PATH] = {};
    if (!gui_update_pending_restore_path(GC_UPDATE_PENDING_RESTORE_NAME,
                                         path, sizeof(path))) {
        debug_log("update handoff: cannot resolve the pending-restore path\n");
        return false;
    }
    // A leftover from an abandoned attempt must not be replayed later as if it
    // were this update's settings.
    gc_DeleteFileUtf8(path);

    char result[512] = {};
    if (!settings_transfer_export(path, result, sizeof(result))) {
        debug_log("update handoff: nothing to carry across the update: %s\n",
                  result[0] ? result : "unknown");
        gc_DeleteFileUtf8(path);
        return false;
    }
    ServiceUpdateState updateValue = {};
    const ServiceUpdateState* update =
        gui_update_state(&updateValue) ? &updateValue : nullptr;
    if (!update || !update->availableVersion[0]) {
        debug_log("update handoff: no authenticated available version; discarding capture\n");
        gc_DeleteFileUtf8(path);
        return false;
    }
    if (!set_config_string(path, GC_UPDATE_RESTORE_SECTION,
                           GC_UPDATE_RESTORE_VERSION_KEY,
                           update->availableVersion)) {
        debug_log("update handoff: could not bind capture to the update version; discarding\n");
        gc_DeleteFileUtf8(path);
        return false;
    }
    debug_log("update handoff: captured active settings for restore: %s\n",
              result[0] ? result : "ok");
    return true;
}

static void gui_update_discard_pending_restore() {
    char pending[MAX_PATH] = {};
    if (!gui_update_pending_restore_path(GC_UPDATE_PENDING_RESTORE_NAME,
                                         pending, sizeof(pending))) {
        return;
    }
    if (gc_DeleteFileUtf8(pending)) {
        debug_log("update handoff: discarded the pending restore\n");
    }
}

static bool gui_update_pending_restore_age_seconds(const char* path,
                                                   long long* ageOut) {
    if (ageOut) *ageOut = -1;
    if (!path || !path[0]) return false;
    WIN32_FILE_ATTRIBUTE_DATA info = {};
    if (!gc_GetFileAttributesExUtf8(path, GetFileExInfoStandard, &info))
        return false;
    ULARGE_INTEGER written = {};
    written.LowPart = info.ftLastWriteTime.dwLowDateTime;
    written.HighPart = info.ftLastWriteTime.dwHighDateTime;
    FILETIME nowFile = {};
    GetSystemTimeAsFileTime(&nowFile);
    ULARGE_INTEGER now = {};
    now.LowPart = nowFile.dwLowDateTime;
    now.HighPart = nowFile.dwHighDateTime;
    if (now.QuadPart < written.QuadPart) return false;
    *ageOut = (long long)((now.QuadPart - written.QuadPart) / 10000000ULL);
    return true;
}

// On GUI start, replay a capture left by an update.
//
// The replay runs as a CHILD PROCESS on the existing `--apply-settings-file`
// verb rather than inline.  That verb already waits for a READY service against
// its own deadline, already stamps the request preconditions, and is the path
// the installer has always used -- so this adds no new apply logic and cannot
// block the window's startup while the service comes up.
void gui_update_replay_pending_restore() {
    // One attempt per process, so this stays safe if it is ever called from a
    // second place (a later service-ready transition, say) without turning a
    // failed apply into a retry loop.
    static bool s_replayConsidered = false;
    if (s_replayConsidered) return;
    s_replayConsidered = true;

    char pending[MAX_PATH] = {};
    char applying[MAX_PATH] = {};
    if (!gui_update_pending_restore_path(GC_UPDATE_PENDING_RESTORE_NAME,
                                         pending, sizeof(pending)) ||
        !gui_update_pending_restore_path(GC_UPDATE_PENDING_RESTORE_APPLYING,
                                         applying, sizeof(applying))) {
        // Silence here is what made the first failure undiagnosable: "no log
        // line" was indistinguishable from "never called".  Every exit from
        // this function now says something.
        debug_log("update handoff: no restore attempted; user data dir is "
                  "unresolved (g_userDataDir=%s)\n",
                  g_userDataDir[0] ? g_userDataDir : "<empty>");
        return;
    }
    DWORD pendingAttrs = gc_GetFileAttributesUtf8(pending);
    if (pendingAttrs == INVALID_FILE_ATTRIBUTES) {
        debug_log("update handoff: no pending restore at %s (error %lu)\n",
                  pending, GetLastError());
        return;
    }
    debug_log("update handoff: found a pending restore at %s\n", pending);

    char expectedVersion[GC_UPDATE_VERSION_MAX_CHARS] = {};
    long long ageSeconds = -1;
    if (!get_config_string(pending, GC_UPDATE_RESTORE_SECTION,
                           GC_UPDATE_RESTORE_VERSION_KEY, "",
                           expectedVersion, sizeof(expectedVersion)) ||
        !gui_update_pending_restore_age_seconds(pending, &ageSeconds) ||
        gc_update_restore_decide(expectedVersion, APP_VERSION, ageSeconds) !=
            GC_UPDATE_RESTORE_APPLY) {
        // Named in full, because this line is the answer to "the update ran and
        // my settings did not come back".  The common cause is not corruption:
        // it is an update that did NOT complete, leaving a capture bound to a
        // version this build is not, which is exactly what the gate is for.
        debug_log("update handoff: discarding the pending restore "
                  "(captured for version '%s', running %s, age %llds)\n",
                  expectedVersion[0] ? expectedVersion : "<none>", APP_VERSION,
                  ageSeconds);
        gc_DeleteFileUtf8(pending);
        return;
    }

    // Rename first: the capture is consumed whether or not the replay works, so
    // a failing apply cannot turn into a restore that runs on every start.
    if (!gc_MoveFileExUtf8(pending, applying,
                           MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        debug_log("update handoff: could not claim the pending restore (error %lu)\n",
                  GetLastError());
        gc_DeleteFileUtf8(pending);
        return;
    }

    char exePath[MAX_PATH] = {};
    if (!gc_GetModuleFileNameUtf8(nullptr, exePath, ARRAY_COUNT(exePath)) ||
        !exePath[0]) {
        debug_log("update handoff: cannot resolve our own path; restore skipped\n");
        return;
    }
    char commandLine[MAX_PATH * 2 + 64] = {};
    if (FAILED(StringCchPrintfA(commandLine, sizeof(commandLine),
                                "\"%s\" --apply-settings-file \"%s\"",
                                exePath, applying))) {
        debug_log("update handoff: restore command line is too long\n");
        return;
    }

    Win32Utf8Path wideCommand(commandLine);
    if (!wideCommand.valid_for(commandLine)) return;
    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi = {};
    if (!CreateProcessW(nullptr, wideCommand.value, nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        debug_log("update handoff: could not start the restore helper (error %lu)\n",
                  GetLastError());
        return;
    }
    // Detached on purpose: the helper waits for the service to reach READY on
    // its own deadline, and the window must not sit blank while it does.  Its
    // outcome lands in greencurve_cli_log.txt.
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    debug_log("update handoff: replaying settings after an update (pid=%lu)\n",
              (unsigned long)pi.dwProcessId);
}
