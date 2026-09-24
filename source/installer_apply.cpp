// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// Everything the setup program actually does to the machine.
//
// Ordering is the whole design here.  An upgrade must ask the running build for
// its live settings BEFORE anything is stopped (afterwards there is nothing
// left to ask), stop the GUI and the service BEFORE their files are replaced,
// and re-point the service registration BEFORE the new service is started, so
// the SCM never launches a binary from a directory the user just moved away
// from.  Each step reports into the failure log, so a support log shows exactly
// which of those boundaries was crossed.

#include "installer_common.h"
#include "service_acl.h"
#include "installer_transaction_policy.h"

// Bounded waits.  These are not race workarounds: each one waits on a real
// kernel object (process exit) or on an external state machine (the SCM) that
// has no other completion signal, and the bound exists only so a wedged
// third-party state cannot hang an unattended silent update forever.
// The service-install child may legitimately spend a stop wait PLUS a start
// wait plus its staging and DACL work inside the SCM, and
// service_admin_reason_policy.h derives exactly that budget -- so this bound
// IS the shared helper bound, not a second opinion.  A tighter one would
// terminate a slow-but-healthy install and report failure for work that then
// completes anyway.
#define GC_APP_CLI_TIMEOUT_MS GC_SVC_ADMIN_HELPER_TIMEOUT_MS
// The settings export is a read: it talks to a service that is already running
// and writes one small file.  It gets a tighter bound than a general CLI call
// because it is also the one place setup runs a binary whose vocabulary it had
// to infer -- and a build that does not know the verb opens its window instead
// of exiting.  Terminating that after seconds rather than a minute is the
// difference between a hiccup and an unattended update that looks hung.
#define GC_APP_EXPORT_TIMEOUT_MS 20000
// The restore additionally waits for the just-started service to bring the GPU
// up, so it needs more headroom than a plain CLI call.
#define GC_APP_REAPPLY_TIMEOUT_MS 180000

// Defined below with the extraction step; the settings capture unpacks the new
// greencurve.exe through the same verified path before anything is stopped.
static bool gc_write_payload_file(const WCHAR* directory, const GcPayloadFile* file,
                                  GcInstallContext* context);

// Stopping what is running lives in installer_stop.cpp, a real translation
// unit in both binaries (the uninstaller needs gc_stop_gui_processes and must
// not carry this shard).  gc_stop_background_service() is the setup-only half.

// ---------------------------------------------------------------------------
// Settings capture and explicit re-apply
// ---------------------------------------------------------------------------

// Run one `--export-active-settings` attempt. Only a successful exit with a
// readable file counts as a capture; the distinct no-intent exit is trustworthy
// only from a client that completed the service read.
static GcSettingsCaptureResult gc_try_export_active_settings(
    const WCHAR* exePath, const WCHAR* snapshot, const char* which) {
    if (!gc_file_exists(exePath)) {
        gc_log_step("capture: no %hs binary at the expected path", which);
        return GC_SETTINGS_CAPTURE_FAILED;
    }
    WCHAR commandLine[2048] = {};
    if (FAILED(StringCchPrintfW(commandLine, GC_ARRAY_COUNT(commandLine),
                                L"\"%ls\" --export-active-settings \"%ls\"", exePath, snapshot))) {
        return GC_SETTINGS_CAPTURE_FAILED;
    }
    DWORD exitCode = (DWORD)-1;
    bool ran = gc_run_and_wait(exePath, commandLine, GC_APP_EXPORT_TIMEOUT_MS, &exitCode);
    bool readable = gc_file_exists(snapshot);
    GcSettingsCaptureResult outcome = gc_settings_capture_attempt_result(
        ran, exitCode, readable);
    gc_log_step("capture: %hs binary export ran=%d exit=%lu snapshot=%d result=%d",
                 which, ran ? 1 : 0, exitCode, readable ? 1 : 0,
                 (int)outcome);
    if (outcome != GC_SETTINGS_CAPTURE_SAVED) DeleteFileW(snapshot);
    return outcome;
}

// Ask the running service for its live settings before anything is stopped.
//
// Two binaries can ask, and neither is right on its own:
//
//   - The INSTALLED one is the only client guaranteed to speak the protocol the
//     running service speaks, because they were built together.  It is asked
//     first, but only when the recorded version is new enough to understand the
//     verb -- an older build treats it as an unknown argument and falls through
//     to opening its window, which is what once made setup sit in front of an
//     unwanted GUI until the timeout expired.
//   - The PAYLOAD's one always understands the verb regardless of what is
//     installed, and is the fallback.  It cannot be the only attempt: across a
//     protocol bump it refuses the old service's responses, reports that no
//     settings are active, and the upgrade then restores nothing at all while
//     looking like a clean run.
//
// Both attempts are bounded and the helper is terminated on timeout, so neither
// can leave a process behind holding the files about to be replaced.
static void gc_capture_active_settings(GcInstallContext* context) {
    if (!context || !context->plan.captureActiveSettings) return;
    context->settingsCaptureResult = GC_SETTINGS_CAPTURE_FAILED;
    const GcPayloadFile* gui = gc_payload_find(&context->payload, GC_SETUP_GUI_EXE);
    if (!gui) {
        gc_log_fail("capture: the payload carries no %s; cannot confirm active settings",
                    GC_SETUP_GUI_EXE);
        return;
    }

    WCHAR scratch[GC_INSTALLER_MAX_PATH_CHARS] = {};
    if (!gc_create_private_temp_directory(
            scratch, GC_ARRAY_COUNT(scratch))) {
        gc_log_fail("capture: could not create an administrator-only scratch folder; "
                    "cannot confirm active settings");
        return;
    }
    WCHAR snapshot[GC_INSTALLER_MAX_PATH_CHARS] = {};
    WCHAR exePath[GC_INSTALLER_MAX_PATH_CHARS] = {};
    bool ok = gc_join_path(scratch, L"greencurve-upgrade-settings.ini", snapshot, GC_ARRAY_COUNT(snapshot)) &&
              gc_join_path(scratch, GC_SETUP_GUI_EXE_W, exePath, GC_ARRAY_COUNT(exePath));
    if (ok) {
        gc_report(context, 5, "Reading the current settings...");
        // Written through the same verified-payload path as a real install, so a
        // damaged download cannot be executed here either.
        ok = gc_write_payload_file(scratch, gui, nullptr);
    }
    GcSettingsCaptureResult capture = GC_SETTINGS_CAPTURE_FAILED;
    if (ok && context->plan.captureFromInstalledBinary) {
        WCHAR installedDirectory[GC_INSTALLER_MAX_PATH_CHARS] = {};
        WCHAR installedExe[GC_INSTALLER_MAX_PATH_CHARS] = {};
        if (gc_utf8_to_wide(context->plan.captureBinaryDirectory, installedDirectory,
                            (int)GC_ARRAY_COUNT(installedDirectory)) &&
            gc_join_path(installedDirectory, GC_SETUP_GUI_EXE_W, installedExe,
                         GC_ARRAY_COUNT(installedExe))) {
            capture = gc_try_export_active_settings(installedExe, snapshot, "installed");
        }
    }
    if (ok && capture == GC_SETTINGS_CAPTURE_FAILED)
        capture = gc_try_export_active_settings(exePath, snapshot, "payload");
    context->settingsCaptureResult = capture;
    if (capture == GC_SETTINGS_CAPTURE_SAVED) {
        // Keep the settings in the same administrator-only directory until the
        // new build consumes them. Moving them back to the ordinary user TEMP
        // root would re-open a replacement window after the executable race was
        // closed.
        StringCchCopyW(context->capturedSettingsPath,
            GC_ARRAY_COUNT(context->capturedSettingsPath), snapshot);
        context->haveCapturedSettings = true;
        gc_log_step("capture: active settings snapshot written to protected path");
    } else if (capture == GC_SETTINGS_CAPTURE_NONE_ACTIVE) {
        gc_log_step("capture: service confirmed no active intent; no restore needed");
    } else {
        // An old helper cannot distinguish no intent from a failed service
        // read. State the uncertainty instead of reporting a clean upgrade.
        gc_log_fail("capture: could not confirm or save the previous active "
                    "settings; the upgrade will not re-apply them");
    }
    DeleteFileW(exePath);
    if (!context->haveCapturedSettings) {
        DeleteFileW(snapshot);
        RemoveDirectoryW(scratch);
    }
}

static void gc_discard_captured_settings(GcInstallContext* context) {
    if (!context || !context->haveCapturedSettings) return;
    WCHAR scratch[GC_INSTALLER_MAX_PATH_CHARS] = {};
    gc_directory_of(context->capturedSettingsPath, scratch,
                    GC_ARRAY_COUNT(scratch));
    DeleteFileW(context->capturedSettingsPath);
    context->capturedSettingsPath[0] = 0;
    context->haveCapturedSettings = false;
    if (scratch[0]) RemoveDirectoryW(scratch);
}

struct GcCapturedSettingsGuard {
    GcInstallContext* context;
    ~GcCapturedSettingsGuard() { gc_discard_captured_settings(context); }
};

// Re-apply the captured settings through the application's explicit CLI apply
// path.  This is a normal, user-typed-equivalent Apply — not a silent replay of
// a persisted snapshot — which is why it is allowed to write to the GPU at all
// under the service's event-only restore policy.
static void gc_reapply_captured_settings(GcInstallContext* context,
                                         const WCHAR* directoryOverride = nullptr) {
    if (!context || !context->haveCapturedSettings) return;
    WCHAR targetDirectory[GC_INSTALLER_MAX_PATH_CHARS] = {};
    WCHAR exePath[GC_INSTALLER_MAX_PATH_CHARS] = {};
    if ((directoryOverride
            ? FAILED(StringCchCopyW(targetDirectory, GC_ARRAY_COUNT(targetDirectory),
                                     directoryOverride))
            : !gc_utf8_to_wide(context->plan.targetDirectory, targetDirectory,
                               (int)GC_ARRAY_COUNT(targetDirectory))) ||
        !gc_join_path(targetDirectory, GC_SETUP_GUI_EXE_W, exePath, GC_ARRAY_COUNT(exePath))) {
        return;
    }
    WCHAR commandLine[2048] = {};
    if (FAILED(StringCchPrintfW(commandLine, GC_ARRAY_COUNT(commandLine),
                                L"\"%ls\" --apply-settings-file \"%ls\"",
                                exePath, context->capturedSettingsPath))) {
        return;
    }
    gc_report(context, 90, "Restoring your previous settings...");
    context->settingsRestoreAttempted = true;
    DWORD exitCode = (DWORD)-1;
    bool ran = gc_run_and_wait(exePath, commandLine, GC_APP_REAPPLY_TIMEOUT_MS, &exitCode);
    context->settingsRestored = ran && exitCode == 0;
    if (context->settingsRestored) {
        gc_log_step("re-apply: previous settings restored");
    } else {
        // The install itself succeeded; only the convenience restore did not.
        // It is reported on the final page as well as here, because a user who
        // is not told will simply find their GPU back at stock.
        gc_log_step("re-apply: previous settings were NOT restored (ran=%d exit=%lu); "
                    "see greencurve_cli_log.txt in %%LOCALAPPDATA%%\\Green Curve",
                    ran ? 1 : 0, exitCode);
    }
    gc_discard_captured_settings(context);
}

// ---------------------------------------------------------------------------
// Writing the payload
// ---------------------------------------------------------------------------

static bool gc_write_payload_file(const WCHAR* directory, const GcPayloadFile* file,
                                  GcInstallContext* context) {
    WCHAR name[GC_ARCHIVE_MAX_NAME + 1] = {};
    if (!gc_utf8_to_wide(file->name, name, (int)GC_ARRAY_COUNT(name))) {
        gc_set_error(context, "Payload file name \"%hs\" could not be decoded.", file->name);
        return false;
    }
    WCHAR finalPath[GC_INSTALLER_MAX_PATH_CHARS] = {};
    WCHAR tempPath[GC_INSTALLER_MAX_PATH_CHARS] = {};
    if (!gc_join_path(directory, name, finalPath, GC_ARRAY_COUNT(finalPath)) ||
        FAILED(StringCchPrintfW(tempPath, GC_ARRAY_COUNT(tempPath), L"%ls.gcnew", finalPath))) {
        gc_set_error(context, "The installation path for %hs is too long.", file->name);
        return false;
    }

    DeleteFileW(tempPath);
    GcScopedHandle handle(CreateFileW(tempPath, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                      FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!handle.valid()) {
        gc_set_error(context, "Could not create %ls (error %lu).", tempPath, GetLastError());
        return false;
    }
    const uint8_t* data = file->data;
    uint64_t remaining = file->size;
    while (remaining > 0) {
        DWORD chunk = (DWORD)(remaining > 0x10000000ull ? 0x10000000ull : remaining);
        DWORD written = 0;
        if (!WriteFile(handle.get(), data, chunk, &written, nullptr) || written == 0) {
            gc_set_error(context, "Could not write %ls (error %lu).", tempPath, GetLastError());
            handle.reset();
            DeleteFileW(tempPath);
            return false;
        }
        data += written;
        remaining -= written;
    }
    if (!FlushFileBuffers(handle.get())) {
        gc_log_step("extract: FlushFileBuffers failed for %ls (error %lu)", tempPath, GetLastError());
    }
    handle.reset();

    // Replace atomically so a failure part-way through never leaves a truncated
    // executable where a working one used to be.
    if (!MoveFileExW(tempPath, finalPath, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DWORD error = GetLastError();
        DeleteFileW(tempPath);
        gc_set_error(context,
                     "Could not replace %ls (error %lu). Close any running Green Curve program and retry.",
                     finalPath, error);
        return false;
    }
    gc_log_step("extract: wrote %ls (%llu bytes)", finalPath, (unsigned long long)file->size);
    return true;
}

#include "installer_transaction.cpp"

// ---------------------------------------------------------------------------
// Service registration
// ---------------------------------------------------------------------------

// Let the newly installed binary register itself.  It owns the SCM
// registration, the failure-action policy, and the hardened DACLs on its own
// directory; duplicating any of that here would be a second implementation to
// keep in sync with the program's security model.
static bool gc_register_service(GcInstallContext* context) {
    WCHAR targetDirectory[GC_INSTALLER_MAX_PATH_CHARS] = {};
    WCHAR exePath[GC_INSTALLER_MAX_PATH_CHARS] = {};
    if (!gc_utf8_to_wide(context->plan.targetDirectory, targetDirectory,
                         (int)GC_ARRAY_COUNT(targetDirectory)) ||
        !gc_join_path(targetDirectory, GC_SETUP_GUI_EXE_W, exePath, GC_ARRAY_COUNT(exePath))) {
        gc_set_error(context, "The installation path is not usable.");
        return false;
    }
    WCHAR commandLine[2048] = {};
    if (FAILED(StringCchPrintfW(commandLine, GC_ARRAY_COUNT(commandLine),
                                L"\"%ls\" --service-install", exePath))) {
        gc_set_error(context, "The service install command line is too long.");
        return false;
    }
    gc_report(context, 70, context->plan.repointService
                               ? "Updating the background service registration..."
                               : "Installing the background service...");
    DWORD exitCode = (DWORD)-1;
    if (!gc_run_and_wait(exePath, commandLine, GC_APP_CLI_TIMEOUT_MS, &exitCode) || exitCode != 0) {
        // The helper's exit code IS the classified reason (it is the only
        // thing that crosses back), so setup renders the same sentence the
        // GUI shows instead of a bare number the user cannot act on.
        int reason = gc_service_admin_reason_from_exit_code(exitCode);
        gc_set_error(context, "%s (exit code %lu)",
                     gc_service_admin_reason_text(reason), exitCode);
        return false;
    }
    // Confirm the SCM now points at the new directory.  Without this an upgrade
    // that moved the installation could silently keep launching the old binary.
    WCHAR registered[GC_INSTALLER_MAX_PATH_CHARS] = {};
    if (gc_service_image_directory(registered, GC_ARRAY_COUNT(registered))) {
        char registeredUtf8[GC_INSTALLER_MAX_PATH_CHARS] = {};
        gc_wide_to_utf8(registered, registeredUtf8, (int)sizeof(registeredUtf8));
        if (!gc_install_paths_equal(registeredUtf8, context->plan.targetDirectory)) {
            gc_set_error(context,
                         "The background service is still registered at %ls instead of %hs.",
                         registered, context->plan.targetDirectory);
            return false;
        }
        gc_log_step("service: registration verified at %ls", registered);
    } else {
        gc_set_error(context, "The background service registration could not be read back.");
        return false;
    }
    return true;
}

#include "installer_launch.cpp"

// ---------------------------------------------------------------------------
// The install itself
// ---------------------------------------------------------------------------

bool gc_install_execute(GcInstallContext* context) {
    if (!context) return false;
    context->error[0] = 0;
    context->previousDirectoryRemoved = false;
    if (!context->plan.valid) {
        gc_set_error(context, "%s", context->plan.error[0] ? context->plan.error : "The install plan is invalid.");
        return false;
    }
    if (context->payload.fileCount == 0) {
        gc_set_error(context, "This setup file carries no program files.");
        return false;
    }
    gc_log_step("install: target=%s upgrade=%d moved=%d repointService=%d startMenu=%d desktop=%d launch=%d",
                context->plan.targetDirectory, context->plan.isUpgrade ? 1 : 0,
                context->plan.directoryChanged ? 1 : 0, context->plan.repointService ? 1 : 0,
                context->plan.createStartMenuShortcut ? 1 : 0, context->plan.createDesktopShortcut ? 1 : 0,
                context->plan.launchAfterInstall ? 1 : 0);

    // Classify the service location before capturing settings, closing the
    // GUI, or stopping the existing service.  A preflight failure must leave a
    // working installation completely untouched.  The classification never
    // rejects a chosen folder - the administrator decides - but an interactive
    // run must not proceed past a non-protected one without its explicit
    // acknowledgment, and every run logs the verdict for the record.
    WCHAR targetDirectory[GC_INSTALLER_MAX_PATH_CHARS] = {};
    if (!gc_utf8_to_wide(context->plan.targetDirectory, targetDirectory,
                         (int)GC_ARRAY_COUNT(targetDirectory))) {
        gc_set_error(context, "The installation folder could not be decoded.");
        return false;
    }
    // Before the protection classification, the question that precedes it: may
    // this folder's permissions be rewritten at all?  Step 5 runs
    // `greencurve.exe --service-install`, which hardens the install directory,
    // so a folder that is not Green Curve's own must be refused here -- before
    // the capture, the shutdown and the extraction -- and not by the register
    // step after the old installation has already been replaced.  This is also
    // the ONLY check on the silent path, where there is no folder page:
    // `/S /D=%USERPROFILE%\Downloads` reaches exactly this line.
    int locationVerdict = gc_service_install_location_verdict(targetDirectory);
    if (locationVerdict != GC_SVC_LOCATION_OK) {
        gc_log_step("path location: refused verdict=%s", gc_service_location_verdict_name(locationVerdict));
        gc_set_error(context,
                     "Green Curve needs a folder of its own. Installing into this folder would "
                     "change its permissions so that only administrators could write to it. "
                     "Choose a subfolder, for example C:\\Program Files\\Green Curve.");
        return false;
    }
    GcPathProtectionReport preProtection = {};
    classify_path_protection(targetDirectory, &preProtection, true);
    gc_log_step("path protection: protected=%d standardWritable=%d profile=%d remote=%d "
                "noFilesystemPermissions=%d reason=%d acknowledgmentRequired=%d acknowledged=%d",
                preProtection.verdict.chain_protected ? 1 : 0,
                preProtection.verdict.standard_writable ? 1 : 0,
                preProtection.verdict.user_profile ? 1 : 0,
                preProtection.verdict.remote ? 1 : 0,
                preProtection.verdict.no_filesystem_permissions ? 1 : 0,
                (int)preProtection.verdict.reason,
                gc_path_protection_requires_acknowledgment(&preProtection.verdict) ? 1 : 0,
                context->pathRiskAcknowledged ? 1 : 0);
    if (gc_path_protection_requires_acknowledgment(&preProtection.verdict) &&
        context->requirePathRiskAcknowledgment && !context->pathRiskAcknowledged) {
        gc_set_error(context, "%s Tick \"%s\" to install there anyway.",
                     GC_PATH_PROTECTION_SUMMARY_UNPROTECTED,
                     GC_PATH_PROTECTION_ACKNOWLEDGMENT_LABEL);
        return false;
    }

    // 1. The folder itself: created, PINNED, judged again and hardened before
    //    the running installation is disturbed at all.  Changing a folder's
    //    DACL needs nothing stopped, so a refusal or a failure here still
    //    leaves the old installation running with its settings intact.
    //
    //    The handle is opened without FILE_SHARE_DELETE and with
    //    FILE_FLAG_OPEN_REPARSE_POINT and is held until setup returns: neither
    //    the folder nor any ancestor can be renamed away underneath it, and
    //    the location verdict, the DACL write and its read-back all name the
    //    object that handle holds.  Hardening by NAME (SetNamedSecurityInfoW
    //    follows junctions) let an account that can rename a parent swap the
    //    folder for a junction between the check and the write, so the
    //    elevated setup rewrote the DACL of whatever the junction named.
    gc_report(context, 10, "Preparing the installation folder...");
    // Folders this run creates are removed again if the install then fails,
    // so a refused or rolled-back fresh install leaves no empty,
    // administrator-only folder behind.  Declared before targetHandle: the
    // guard runs after the pinning handle (no FILE_SHARE_DELETE) is closed.
    struct GcCreatedTargetGuard {
        const WCHAR* path;
        int created;
        bool armed;
        ~GcCreatedTargetGuard() {
            if (!armed || created <= 0) return;
            int removed = gc_remove_created_directory_chain(path, created);
            gc_log_step("install failed: removed %d of %d folder(s) this run created at %ls",
                        removed, created, path);
        }
    } createdTarget = {targetDirectory,
                       gc_count_missing_directory_components(targetDirectory), true};
    gc_log_step("install: target folder components to create=%d", createdTarget.created);
    if (!gc_create_directory_tree(targetDirectory)) {
        gc_set_error(context, "Could not create %ls. Choose a different folder or run setup as an administrator.",
                     targetDirectory);
        return false;
    }
    const bool hardenTarget = !preProtection.verdict.no_filesystem_permissions;
    GcScopedHandle targetHandle(CreateFileW(targetDirectory,
        FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES | READ_CONTROL |
            (hardenTarget ? (WRITE_DAC | WRITE_OWNER) : 0),
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    if (!targetHandle.valid()) {
        gc_set_error(context, "The installation folder could not be opened (error %lu).",
                     GetLastError());
        return false;
    }
    int pinnedVerdict = gc_service_install_location_verdict_for_handle(targetDirectory,
                                                                       targetHandle.get());
    if (pinnedVerdict != GC_SVC_LOCATION_OK) {
        gc_log_step("path location: pinned folder refused verdict=%s",
                    gc_service_location_verdict_name(pinnedVerdict));
        gc_set_error(context,
                     "Green Curve needs a folder of its own. Installing into this folder would "
                     "change its permissions so that only administrators could write to it. "
                     "Choose a subfolder, for example C:\\Program Files\\Green Curve.");
        return false;
    }
    char directoryAclError[256] = {};
    if (!hardenTarget) {
        // A capability gap, not a failure: FAT/exFAT cannot express a DACL at
        // all.  The classification already forced the strongest acknowledgment
        // for exactly this case, so skip loudly instead of failing on a
        // security write that can never succeed here.
        gc_log_step("install: volume has no file permissions; skipping directory DACL "
                    "hardening (nothing placed here can be protected)");
    } else if (!apply_protected_service_dacl_to_handle(targetHandle.get(), GC_SERVICE_ACL_DIRECTORY,
                                                       true, directoryAclError,
                                                       sizeof(directoryAclError))) {
        gc_set_error(context, "The installation folder could not be secured: %s",
            directoryAclError[0] ? directoryAclError : "DACL verification failed");
        return false;
    }
    // The folder now exists and carries its hardened DACL: what preflight
    // vouched for must still hold.  A directory planted between the two
    // checks, or a parent swapped out from under them, shows up here as a
    // downgrade and fails the install closed instead of registering a
    // LocalSystem service from a location less protected than the user was
    // told about.
    GcPathProtectionReport postProtection = {};
    classify_path_protection(targetDirectory, &postProtection, false);
    if (!postProtection.verdict.chain_protected) {
        gc_log_step("path protection after hardening: protected=0 reason=%d",
                    (int)postProtection.verdict.reason);
    }
    if (preProtection.verdict.chain_protected && !postProtection.verdict.chain_protected) {
        gc_set_error(context,
            "The installation folder is no longer as protected as it was when setup "
            "started (reason %d). Nothing was registered; run setup again.",
            (int)postProtection.verdict.reason);
        return false;
    }

    GcCapturedSettingsGuard capturedSettingsGuard = {context};

    // 2. Capture: after the next step there is nothing left to ask.
    if (!context->settingsCaptureHandledByGui) {
        gc_capture_active_settings(context);
    } else {
        gc_log_step("capture: handled by the authorized update GUI before setup launched");
    }

    // Preserve a complete, runnable old image and the machine-wide pointers
    // before the first process is stopped. Staging also exposes disk and path
    // failures while the previous installation is still available.
    GcInstallTransaction transaction(context);
    if (!transaction.prepare(targetDirectory)) return false;

    // 3. Nothing may hold the files open once extraction starts.
    if (!gc_stop_gui_processes(context)) {
        gc_set_error(context, "A running Green Curve program could not be closed. Close it and run setup again.");
        transaction.cleanup();
        return false;
    }
    // 4-5. Every failure edge after shutdown reaches the same recovery path.
    // The uninstall record is saved and written before the service helper:
    // there is no fallible setup work after that helper commits and releases
    // the previous location's hardening.
    bool serviceWasRunning = false;
    if (!gc_run_install_transaction(context->payload.fileCount,
        [&]() {
            bool stopped = gc_stop_background_service(context, &serviceWasRunning);
            transaction.serviceWasRunning |= serviceWasRunning;
            return stopped;
        },
        [&]() {
            // Nothing was replaced or registered yet: the previous installation
            // is intact, so this is a refused upgrade, never a failed recovery.
            char original[sizeof(context->error)] = {};
            StringCchCopyA(original, GC_ARRAY_COUNT(original), context->error);
            bool restarted = false;
            GcStopFailureRecovery action = transaction.recover_after_stop_failure(&restarted);
            const char* serviceNote = "";
            if (action == GC_STOP_FAILURE_RESTART_PREVIOUS)
                serviceNote = !restarted
                    ? " The previous background service stopped and could not be restarted; start Green Curve to restart it."
                    : (context->settingsRestored
                        ? " The previous background service was restarted and its GPU settings reapplied."
                        : " The previous background service was restarted; reapply GPU settings if needed.");
            else if (action == GC_STOP_FAILURE_STATE_UNKNOWN)
                serviceNote = " The background service state could not be read.";
            gc_set_error(context, "%s%s Nothing was changed.%s", original,
                         original[0] ? "" : "Setup could not stop the background service.",
                         serviceNote);
        },
        [&](uint32_t i) {
            if (i == 0) gc_report(context, 30, "Copying program files...");
            if (!transaction.replace(i)) return false;
            int percent = 30 + (int)((40 * (uint64_t)(i + 1)) / context->payload.fileCount);
            gc_report(context, percent, "Copying program files...");
            return true;
        },
        [&]() {
            gc_report(context, 75, "Updating the uninstall record...");
            return gc_write_uninstall_registration(context);
        },
        [&]() { return gc_register_service(context); },
        [&](bool recordMayHaveChanged) {
            char original[sizeof(context->error)] = {};
            StringCchCopyA(original, GC_ARRAY_COUNT(original), context->error);
            bool restored = transaction.rollback(recordMayHaveChanged);
            // Retained backups may still reference this folder's files.
            if (!restored) createdTarget.armed = false;
            gc_set_error(context, "%s%s%s", original, original[0] ? "" : "Setup failed.",
                restored ? (transaction.serviceWasRunning
                    ? (context->settingsRestored
                        ? " Setup changes were rolled back and previous GPU settings reapplied."
                        : " Setup changes were rolled back; reapply GPU settings if needed.")
                    : " Setup changes were rolled back.") :
                           " Recovery failed; keep the protected backup and see the failure log.");
        })) return false;
    transaction.cleanup();
    createdTarget.armed = false;

    // 6a. Reconcile the directory to THIS payload.  An in-place upgrade
    // replaces every file the payload contains and leaves every file it does
    // not; the pre-rename uninstaller is exactly such a leave, and a stale
    // generic `uninstall.exe` is what antivirus engines keep flagging.  Only
    // after commit: a failed install must leave the previous uninstaller
    // working.
    gc_remove_stale_payload_leaves(targetDirectory, &context->payload);

    // 6. Shortcuts are best effort and cannot invalidate a running service.
    gc_report(context, 80, "Creating shortcuts...");
    gc_update_shortcuts(context);

    // 7. Put the user's settings back exactly as an explicit Apply would.
    gc_reapply_captured_settings(context);

    // Only after the new registration, shortcuts, and ARP entry succeed is the
    // previous setup-managed directory unused. Never remove a portable copy.
    gc_retire_previous_directory(context);

    gc_report(context, 100, "Installation complete.");
    return true;
}
