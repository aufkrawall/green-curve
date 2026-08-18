// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// The updater's orchestration, inside the LocalSystem service: check, download,
// verify, stage, and -- only on an explicit request that passes every gate --
// launch the installer.
//
// ## The order is the security property
//
//   1. Fetch the manifest and its detached signature from two fixed URLs.
//   2. VERIFY THE SIGNATURE over the manifest's exact bytes, against a key
//      compiled into this binary.  Nothing below happens if this fails, and in
//      particular the manifest is not parsed first: the parser runs on
//      authenticated bytes only.
//   3. Parse, and decide.  A version not strictly newer than the installed one
//      is a refusal, which is the only control that rejects a replayed older
//      release -- that artifact is genuinely signed, genuinely attested and
//      correctly hashed.
//   4. Download the named asset into a staging directory a standard user cannot
//      write, bounded by the size the signed manifest declared.
//   5. Re-open it with a share mode that denies writes and deletes, and verify
//      length and SHA-256 through that handle.
//   6. Keep that handle open across CreateProcess.
//
// Step 6 is what closes the time-of-check/time-of-use window.  Verifying by
// path and then launching by path would leave a moment in which the bytes that
// were measured and the bytes that get executed are not required to be the same
// file, and this process is SYSTEM.
//
// ## Why the service and not the GUI
//
// The GUI runs unelevated (its manifest asks for asInvoker).  Had it done the
// download, the file would have to live somewhere that account can write, and
// anything else running as that user could swap it between the hash check and
// the elevation prompt.  Doing it here means the staged file is protected by
// the directory's DACL for its whole life, and it means the install needs no
// UAC prompt at all, because the service is already SYSTEM.

#define GC_UPDATE_INSTALL_TIMEOUT_MS 600000
// How much longer a setup process that overran its timeout keeps the GPU
// reserved.  The reservation must not be permanent: it disables the fan runtime
// pulse, every auto-restore path and every Apply/Reset, so leaving it set for
// the remaining life of the service process turns one hung installer into fan
// control that stays dead until somebody restarts the service.
#define GC_UPDATE_INSTALL_ABANDON_TIMEOUT_MS 120000
// The exit code forced onto a setup process we had to terminate.  Chosen to sit
// outside setup's own documented contract (0 success, 1 failure, 2 cancelled,
// 3 bad arguments) so a log never confuses "we killed it" with "it decided".
//
// Spelled here rather than shared with installer_main.cpp's GC_EXIT_*, because
// installer.md invariant 11 keeps the setup program and the application model
// from including each other's headers -- the same reason
// GC_UPDATE_UNINSTALL_KEY is written twice.  Nothing reads this value back, so
// the duplication carries no drift risk.
#define GC_UPDATE_INSTALLER_ABANDONED_EXIT_CODE 4

// ---------------------------------------------------------------------------
// Where this copy is installed
// ---------------------------------------------------------------------------

// Read InstallLocation from the Add/Remove Programs entry.  Its presence is
// what distinguishes a real installation from a portable .7z extraction: the
// latter has no installer to run and no recorded directory, so it must never be
// silently "upgraded" into a layout the user did not choose.
static bool service_update_is_installed_copy(char* installDirOut, size_t installDirSize) {
    if (installDirOut && installDirSize) installDirOut[0] = 0;
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, GC_UPDATE_UNINSTALL_KEY, 0,
                      KEY_QUERY_VALUE | KEY_WOW64_64KEY, &key) != ERROR_SUCCESS) {
        return false;
    }
    WCHAR value[MAX_PATH] = {};
    DWORD bytes = sizeof(value) - sizeof(WCHAR);
    DWORD type = 0;
    LSTATUS status = RegQueryValueExW(key, L"InstallLocation", nullptr, &type,
                                      (LPBYTE)value, &bytes);
    RegCloseKey(key);
    if (status != ERROR_SUCCESS || type != REG_SZ || !value[0]) return false;

    DWORD attrs = GetFileAttributesW(value);
    // A recorded directory that no longer exists means the installation was
    // removed by hand; treat that as portable rather than trying to install
    // into a path nothing is at.
    if (attrs == INVALID_FILE_ATTRIBUTES || (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0) {
        return false;
    }
    if (installDirOut && installDirSize) {
        if (!copy_wide_to_utf8(value, installDirOut, (int)installDirSize)) {
            installDirOut[0] = 0;
            return false;
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// The check
// ---------------------------------------------------------------------------

// Persist the two documents a successful check authenticated, so a restart can
// re-derive what this one found instead of going quiet until the next interval
// elapses.  Defined in main_service_update_cache.cpp, which is compiled after
// this shard because it calls back into the staged-package re-verification
// below.
static void service_update_cache_store(const char* manifestText, size_t manifestLength,
                                       const char* signatureText, size_t signatureLength);

// Fetch + verify + decide.  Returns true when the check COMPLETED, regardless of
// whether it found an update; `false` means the check itself failed and the
// caller should count a failure for backoff purposes.
static bool service_update_run_check(char* err, size_t errSize) {
    if (err && errSize) err[0] = 0;
    service_update_set_phase(SERVICE_UPDATE_PHASE_CHECKING, nullptr);

    char manifestUrl[GC_UPDATE_URL_MAX_CHARS] = {};
    char signatureUrl[GC_UPDATE_URL_MAX_CHARS] = {};
    if (!gc_update_build_latest_url(GC_UPDATE_MANIFEST_ASSET, manifestUrl,
                                    sizeof(manifestUrl)) ||
        !gc_update_build_latest_url(GC_UPDATE_SIGNATURE_ASSET, signatureUrl,
                                    sizeof(signatureUrl))) {
        set_message(err, errSize, "Could not build the update URLs");
        return false;
    }

    // Both documents are tiny and both are fetched before anything is trusted.
    char manifestText[GC_UPDATE_MANIFEST_MAX_BYTES + 1] = {};
    size_t manifestLength = 0;
    if (!gc_update_http_get_small(manifestUrl, manifestText, sizeof(manifestText),
                                  &manifestLength, err, errSize)) {
        return false;
    }
    char signatureText[GC_UPDATE_SIGNATURE_MAX_BYTES + 1] = {};
    size_t signatureLength = 0;
    if (!gc_update_http_get_small(signatureUrl, signatureText, sizeof(signatureText),
                                  &signatureLength, err, errSize)) {
        return false;
    }

    // THE GATE.  Everything after this line operates on authenticated bytes.
    if (!gc_update_verify_manifest_signature(manifestText, manifestLength,
                                             signatureText, signatureLength,
                                             err, errSize)) {
        return false;
    }

    GcUpdateManifest manifest;
    gc_update_manifest_parse(manifestText, manifestLength, &manifest);
    if (!manifest.valid) {
        // A correctly signed manifest that does not parse means the release
        // tooling produced something this build cannot read.  That is not a
        // network failure and must not be retried into a loop.
        set_message(err, errSize, "Signed manifest is not usable: %s",
                    manifest.error[0] ? manifest.error : "unknown");
        return false;
    }

    GcUpdateVersion installed;
    gc_update_version_parse(APP_VERSION, &installed);
    if (!installed.valid) {
        // A development build whose APP_VERSION is not a release number ("dev")
        // has nothing to compare against.  Refusing is correct: silently
        // "upgrading" a local build to the last public release would replace
        // work in progress with something older.
        set_message(err, errSize,
                    "This build's version (%s) is not a release version, so "
                    "updates cannot be compared", APP_VERSION);
        return false;
    }

    GcUpdateArch arch = service_update_host_arch();
    GcUpdateDecision decision = gc_update_decide(&manifest, &installed, arch);

    bool channelRegressed = false;
    {
        GcUpdateStateLock guard;
        g_updateState.manifest = manifest;
        g_updateState.manifestValid = true;
        g_updateState.decision = decision;
        // Folded in AFTER the signature verified and the manifest parsed, so
        // the mark can only ever be moved by the maintainer.  An attacker who
        // could raise it would be able to suppress the regression signal by
        // advertising something enormous once.
        channelRegressed =
            gc_update_channel_state((GcUpdateAutoCheck)g_updateState.autoCheck,
                                    (long long)g_updateState.lastCheckUnix,
                                    service_update_now_unix(),
                                    g_updateState.highestSeenVersion,
                                    manifest.version.text) ==
            GC_UPDATE_CHANNEL_REGRESSED;
        char raised[GC_UPDATE_VERSION_MAX_CHARS] = {};
        if (gc_update_channel_note_version(g_updateState.highestSeenVersion,
                                           manifest.version.text, raised,
                                           sizeof(raised))) {
            StringCchCopyA(g_updateState.highestSeenVersion,
                           sizeof(g_updateState.highestSeenVersion), raised);
        }
    }
    if (channelRegressed) {
        debug_log("update channel: REGRESSED -- advertised %s is older than the "
                  "highest previously advertised release; replay or a withdrawn "
                  "release
", manifest.version.text);
    }
    service_update_save_settings();
    // Cached only once the documents have both verified AND parsed, so a
    // restart never spends its startup re-discovering that a signed manifest
    // this build cannot read is still unreadable.  The decision itself is NOT
    // stored: the restore recomputes it against whatever version is running by
    // then, which is what lets the same cache answer UP_TO_DATE after the
    // update it advertised has been installed.
    service_update_cache_store(manifestText, manifestLength,
                               signatureText, signatureLength);

    const char* detail = nullptr;
    switch (decision) {
        case GC_UPDATE_DECISION_AVAILABLE:
            detail = nullptr;
            break;
        case GC_UPDATE_DECISION_UP_TO_DATE:
            detail = "Green Curve is up to date.";
            break;
        case GC_UPDATE_DECISION_MANUAL_REQUIRED:
            detail = "This release must be installed manually from the "
                     "Green Curve releases page.";
            break;
        case GC_UPDATE_DECISION_NO_ASSET:
            detail = "The latest release has no build for this machine's "
                     "architecture.";
            break;
        case GC_UPDATE_DECISION_REJECTED:
        default:
            // Reached when the published release is OLDER than what is
            // installed.  Logged loudly: on a healthy channel it never happens,
            // so it is either a rollback by the maintainer or a replay.
            detail = "The published release is not newer than the installed "
                     "version.";
            debug_log("update check: REFUSED a release that is not newer "
                      "(published=%s installed=%s)\n",
                      manifest.version.text, installed.text);
            break;
    }
    service_update_set_phase(SERVICE_UPDATE_PHASE_IDLE, detail);
    debug_log("update check: decision=%d published=%s installed=%s arch=%s\n",
              (int)decision, manifest.version.text, installed.text,
              gc_update_arch_name(arch));
    return true;
}

// ---------------------------------------------------------------------------
// The download
// ---------------------------------------------------------------------------

static bool service_update_run_download(char* err, size_t errSize) {
    if (err && errSize) err[0] = 0;

    GcUpdateManifest manifest;
    {
        GcUpdateStateLock guard;
        if (!g_updateState.manifestValid ||
            g_updateState.decision != GC_UPDATE_DECISION_AVAILABLE) {
            set_message(err, errSize, "No update is available to download");
            return false;
        }
        manifest = g_updateState.manifest;
    }

    GcUpdateArch arch = service_update_host_arch();
    const GcUpdateAsset* asset = gc_update_select_asset(&manifest, arch);
    if (!asset) {
        set_message(err, errSize, "No build for this machine's architecture");
        return false;
    }

    char stagingDir[MAX_PATH] = {};
    if (!service_update_staging_dir(stagingDir, sizeof(stagingDir), err, errSize)) {
        return false;
    }
    // A previous attempt's file must not survive into this one: it could be a
    // verified installer for a different version, sitting under a name this run
    // is about to trust.
    service_update_clear_staging();

    char assetUrl[GC_UPDATE_URL_MAX_CHARS] = {};
    if (!gc_update_build_asset_url(manifest.version.text, asset->file,
                                   assetUrl, sizeof(assetUrl))) {
        set_message(err, errSize, "Could not build the download URL");
        return false;
    }
    char stagedPath[MAX_PATH] = {};
    if (FAILED(StringCchPrintfA(stagedPath, sizeof(stagedPath), "%s\\%s",
                                stagingDir, asset->file))) {
        set_message(err, errSize, "Staged file path is too long");
        return false;
    }

    service_update_set_phase(SERVICE_UPDATE_PHASE_DOWNLOADING, nullptr);

    // CREATE_NEW plus FILE_FLAG_OPEN_REPARSE_POINT: never follow a link, never
    // truncate something already there.  The staging directory was just swept,
    // so an existing name here means something is racing us and the right
    // answer is to stop.
    HANDLE dest = gc_CreateFileUtf8(stagedPath, GENERIC_WRITE, 0, nullptr,
                                    CREATE_NEW,
                                    FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT,
                                    nullptr);
    if (dest == INVALID_HANDLE_VALUE) {
        set_message(err, errSize, "Cannot create the staged file (error %lu)",
                    GetLastError());
        return false;
    }
    bool ok = gc_update_http_download_to_handle(assetUrl, dest, asset->size,
                                                err, errSize);
    CloseHandle(dest);
    if (!ok) {
        gc_DeleteFileUtf8(stagedPath);
        return false;
    }

    // Re-open for verification with the share mode the launch will need, and
    // verify through THAT handle.  See the file header: this is the TOCTOU
    // closure, and it only works because the same handle is kept open until the
    // installer process has been created.
    service_update_set_phase(SERVICE_UPDATE_PHASE_VERIFYING, nullptr);
    HANDLE verify = gc_CreateFileUtf8(stagedPath, GENERIC_READ, FILE_SHARE_READ,
                                      nullptr, OPEN_EXISTING,
                                      FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT,
                                      nullptr);
    if (verify == INVALID_HANDLE_VALUE) {
        set_message(err, errSize, "Cannot re-open the staged file (error %lu)",
                    GetLastError());
        gc_DeleteFileUtf8(stagedPath);
        return false;
    }
    ok = gc_update_staged_file_matches(verify, asset, err, errSize);
    CloseHandle(verify);
    if (!ok) {
        // A file that failed verification is deleted immediately rather than
        // kept for diagnosis.  Leaving an unverified executable in a directory
        // the service launches from is not a trade worth making for a better
        // bug report.
        gc_DeleteFileUtf8(stagedPath);
        debug_log("update download: verification FAILED, staged file deleted\n");
        return false;
    }

    {
        GcUpdateStateLock guard;
        StringCchCopyA(g_updateState.stagedPath, sizeof(g_updateState.stagedPath),
                       stagedPath);
        g_updateState.packageStaged = true;
        g_updateState.packageVerified = true;
    }
    service_update_set_phase(SERVICE_UPDATE_PHASE_READY, nullptr);
    debug_log("update download: staged and verified %s\n", asset->file);
    return true;
}

static bool service_update_staged_package_matches_manifest(
    const GcUpdateManifest* manifest, char* err, size_t errSize) {
    if (err && errSize) err[0] = 0;
    if (!manifest || !manifest->valid) return false;
    const GcUpdateAsset* asset =
        gc_update_select_asset(manifest, service_update_host_arch());
    if (!asset) return false;

    char stagedPath[MAX_PATH] = {};
    {
        GcUpdateStateLock guard;
        StringCchCopyA(stagedPath, sizeof(stagedPath), g_updateState.stagedPath);
    }
    if (!stagedPath[0]) return false;

    HANDLE verify = gc_CreateFileUtf8(stagedPath, GENERIC_READ, FILE_SHARE_READ,
                                      nullptr, OPEN_EXISTING,
                                      FILE_ATTRIBUTE_NORMAL |
                                          FILE_FLAG_OPEN_REPARSE_POINT,
                                      nullptr);
    if (verify == INVALID_HANDLE_VALUE) return false;
    bool matches = gc_update_staged_file_matches(verify, asset, err, errSize);
    CloseHandle(verify);
    return matches;
}

// ---------------------------------------------------------------------------
// The install
// ---------------------------------------------------------------------------

// Whether a fullscreen application owns the foreground.  Stopping the service
// resets the GPU to stock on the way down, so installing here would yank the
// overclock out from under a running game.
// The service CANNOT answer this, and pretending otherwise cost the first live
// install.
//
// SHQueryUserNotificationState reports the state of the caller's own window
// station.  A LocalSystem service lives in session 0, which has no interactive
// desktop, so what comes back describes nothing the user can see -- and the
// original code refused an install on QUNS_BUSY, which is exactly what an
// answer from a non-interactive session can look like.  The result was an
// install that was silently refused with the machine completely idle.
//
// The check itself is worth keeping: stopping the service returns the GPU to
// stock for a few seconds, and doing that under a running game is rude.  But
// the only process that can see the user's session is the GUI, which lives in
// it, so the check moved there (gui_update_foreground_app_active) and warns
// before the request is sent.
//
// This is a COURTESY gate, not a security one, which is what makes moving it
// client-side acceptable: a client can only make itself more restrictive.  The
// gate that actually protects the hardware -- applyInFlight, taken from the
// runtime mutex -- stays here, where it is answerable.
static bool service_update_foreground_app_active() {
    QUERY_USER_NOTIFICATION_STATE state = (QUERY_USER_NOTIFICATION_STATE)0;
    HRESULT hr = SHQueryUserNotificationState(&state);
    // Logged, never acted on: recorded so that "why was my install refused" is
    // answerable from the log, and so the day this moves into a session-aware
    // helper there is data about what session 0 actually returns.
    debug_log("update install: session-0 notification state hr=0x%08lX value=%d "
              "(not used as a gate; the GUI owns this check)\n",
              (unsigned long)hr, (int)state);
    return false;
}

static bool service_update_run_install(char* err, size_t errSize) {
    if (err && errSize) err[0] = 0;

    char stagedPath[MAX_PATH] = {};
    char installDir[MAX_PATH] = {};
    DWORD requestingSessionId = (DWORD)-1;
    GcUpdateInstallGate gate = {};
    // The one and only place userConsented is set, and it is set because this
    // function is reached exclusively from SERVICE_CMD_INSTALL_UPDATE, which a
    // human triggers. No timer path reaches here.
    gate.userConsented = true;
    {
        GcUpdateStateLock guard;
        gate.packageStaged = g_updateState.packageStaged;
        gate.packageVerified = g_updateState.packageVerified;
        gate.installAlreadyRunning = g_updateState.installRunning;
        requestingSessionId = g_updateState.requestingSessionId;
        StringCchCopyA(stagedPath, sizeof(stagedPath), g_updateState.stagedPath);
    }
    if (service_update_install_reserved()) gate.installAlreadyRunning = true;
    gate.isInstalledCopy = service_update_is_installed_copy(installDir, sizeof(installDir));
    gate.foregroundAppActive = service_update_foreground_app_active();
    // The reservation below is acquired while holding the runtime lock.  This
    // worker blocks behind an already-running apply instead of sampling that
    // lock at one instant and then racing a queued mutation before launch.
    gate.applyInFlight = false;

    GcUpdateInstallRefusal refusal = gc_update_install_decision(&gate);
    {
        GcUpdateStateLock guard;
        g_updateState.lastRefusal = refusal;
    }
    if (refusal != GC_UPDATE_INSTALL_ALLOWED) {
        set_message(err, errSize, "Update not installed: %s",
                    gc_update_install_refusal_text(refusal));
        debug_log("update install: REFUSED (%s)\n",
                  gc_update_install_refusal_text(refusal));
        return false;
    }
    if (!stagedPath[0] || !installDir[0]) {
        set_message(err, errSize, "Update not installed: staging state is incomplete");
        return false;
    }

    // Open the verified file with writes and deletes denied, re-verify through
    // that handle, and HOLD IT until the process exists.  Re-verifying is not
    // paranoia theatre: the download-time check ran against a handle that has
    // since been closed, so without this the window between staging and
    // installing would be unguarded.
    HANDLE pinned = gc_CreateFileUtf8(stagedPath, GENERIC_READ, FILE_SHARE_READ,
                                      nullptr, OPEN_EXISTING,
                                      FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT,
                                      nullptr);
    if (pinned == INVALID_HANDLE_VALUE) {
        set_message(err, errSize, "Cannot open the staged installer (error %lu)",
                    GetLastError());
        return false;
    }

    GcUpdateManifest manifest;
    {
        GcUpdateStateLock guard;
        manifest = g_updateState.manifest;
    }
    const GcUpdateAsset* asset = gc_update_select_asset(&manifest, service_update_host_arch());
    if (!gc_update_staged_file_matches(pinned, asset, err, errSize)) {
        CloseHandle(pinned);
        gc_DeleteFileUtf8(stagedPath);
        {
            GcUpdateStateLock guard;
            g_updateState.packageStaged = false;
            g_updateState.packageVerified = false;
        }
        debug_log("update install: staged file failed re-verification; deleted\n");
        return false;
    }

    lock_service_runtime();
    if (service_update_install_reserved()) {
        unlock_service_runtime();
        CloseHandle(pinned);
        set_message(err, errSize, "Update not installed: an install is already reserved");
        return false;
    }
    service_update_set_install_reserved(true);
    unlock_service_runtime();
    debug_log("update install: reserved GPU writes for setup launch\n");

    service_update_set_phase(SERVICE_UPDATE_PHASE_INSTALLING, nullptr);
    {
        GcUpdateStateLock guard;
        g_updateState.installRunning = true;
    }

    // BOTH command lines are built and encoded HERE, before a single window is
    // closed, and the measured count below only picks between them.
    //
    // Nothing that can fail may happen after the GUIs are gone.  Closing them
    // is the one destructive step the service takes on the user's behalf, and
    // it is not undoable from here: the service does not launch processes into
    // interactive sessions, so a failure after this point used to leave the
    // user with no window, no tray icon and no way to learn why.  Building the
    // string and encoding it to UTF-16 are the two fallible steps that used to
    // sit on the far side of that line; now they cannot.
    //
    // The builder is pure (update_install_policy.h) and its output is asserted
    // against the real installer parser (4260-4269), which is why building an
    // extra candidate costs nothing but a stack buffer.
    char launchSessionId[16] = {};
    StringCchPrintfA(launchSessionId, sizeof(launchSessionId), "%lu",
                     (unsigned long)requestingSessionId);
    char relaunchCommand[GC_UPDATE_COMMAND_LINE_MAX_CHARS] = {};
    char noLaunchCommand[GC_UPDATE_COMMAND_LINE_MAX_CHARS] = {};
    if (!gc_update_build_installer_command_line(stagedPath, installDir, true,
                                                launchSessionId, relaunchCommand,
                                                sizeof(relaunchCommand)) ||
        !gc_update_build_installer_command_line(stagedPath, installDir, false,
                                                nullptr, noLaunchCommand,
                                                sizeof(noLaunchCommand))) {
        CloseHandle(pinned);
        service_update_set_install_reserved(false);
        set_message(err, errSize,
                    "Cannot build a usable installer command line for %s",
                    installDir);
        GcUpdateStateLock guard;
        g_updateState.installRunning = false;
        return false;
    }
    Win32Utf8Path wideRelaunch(relaunchCommand);
    Win32Utf8Path wideNoLaunch(noLaunchCommand);
    if (!wideRelaunch.valid_for(relaunchCommand) ||
        !wideNoLaunch.valid_for(noLaunchCommand)) {
        CloseHandle(pinned);
        service_update_set_install_reserved(false);
        GcUpdateStateLock guard;
        g_updateState.installRunning = false;
        set_message(err, errSize, "Cannot encode the installer command line");
        return false;
    }

    // Close every GUI BEFORE setup starts.  Setup runs in session 0 (the
    // service launched it) and cannot see, let alone close, a window in the
    // user's session -- so left to itself it reports "no running Green Curve
    // window found" and then fails replacing greencurve.exe with error 5.
    int closedGuiCount = 0;
    if (!service_update_stop_gui_processes(installDir, &closedGuiCount,
                                           err, errSize)) {
        CloseHandle(pinned);
        service_update_set_install_reserved(false);
        GcUpdateStateLock guard;
        g_updateState.installRunning = false;
        return false;
    }

    // Relaunch the GUI only if one was actually running.  This SELECTION is
    // what has to follow the stop -- the count is measured, not assumed -- and
    // selecting between two already-validated strings cannot fail.
    LPWSTR wideCommand = closedGuiCount > 0 ? wideRelaunch.value : wideNoLaunch.value;
    debug_log("update install: command line is %s\n",
              closedGuiCount > 0 ? relaunchCommand : noLaunchCommand);

    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi = {};
    BOOL created = CreateProcessW(nullptr, wideCommand, nullptr, nullptr,
                                  FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    DWORD createError = created ? 0 : GetLastError();
    // The pin has done its job the moment the image is mapped: from here the
    // running installer is what matters, not the file it came from.
    CloseHandle(pinned);

    if (!created) {
        service_update_set_install_reserved(false);
        GcUpdateStateLock guard;
        g_updateState.installRunning = false;
        set_message(err, errSize, "Cannot start the installer (error %lu)", createError);
        return false;
    }

    debug_log("update install: launched %s (pid=%lu)\n", stagedPath,
              (unsigned long)pi.dwProcessId);
    CloseHandle(pi.hThread);

    // The installer stops this very service as part of its work, so this wait
    // usually ends with the process being torn down underneath it.  It is here
    // for the failure case: an installer that exits with a non-zero code has to
    // be reported rather than leaving the GUI on "installing" forever.
    DWORD waited = WaitForSingleObject(pi.hProcess, GC_UPDATE_INSTALL_TIMEOUT_MS);
    DWORD exitCode = 0;
    bool ok = false;
    // Set only when the process could NOT be proven dead, which is the sole
    // remaining reason to keep GPU writes blocked past this function.
    bool reservationHeld = false;
    if (waited == WAIT_OBJECT_0 && GetExitCodeProcess(pi.hProcess, &exitCode)) {
        // Exit codes are the installer's documented contract: 0 success,
        // 1 failure, 2 cancelled, 3 bad arguments.
        ok = exitCode == 0;
        if (!ok) {
            set_message(err, errSize,
                        exitCode == 3
                            ? "The installer rejected the updater's arguments (exit 3)"
                            : "The installer reported failure (exit %lu)", exitCode);
        }
    } else {
        set_message(err, errSize,
                    "The installer did not finish in time; Green Curve may have "
                    "to be started by hand");
        // The reservation's only justification is "setup might be part way
        // through replacing files", and that is exactly co-extensive with
        // "the setup process is alive".  So rather than guess a grace period
        // -- every value of which is wrong in one direction or the other --
        // make the condition true by ending the process, then wait for it to
        // actually die before releasing.
        //
        // Terminating is the right choice here because setup is OUR binary run
        // with /S: a silent install that has not returned in ten minutes is not
        // making progress, and leaving it alive forever is strictly worse.
        //
        // It is also very unlikely to be mid-file-replacement: setup copies
        // files only after gc_stop_service(), and that call is what tears this
        // process down, so a service thread still executing here has almost
        // certainly not reached the copy. "Almost" and not "certainly" because
        // a process can briefly outlive its own service stop -- which is why
        // the reservation is still held right up to the moment the kill is
        // confirmed, rather than dropped on the assumption.
        //
        // Previously this returned with the reservation still set, which gates
        // the fan runtime pulse, all three auto-restore paths, Apply, Reset and
        // the controlled restart: one hung installer disabled GPU and fan
        // control for the remaining life of the service process, with the
        // watchdog seeing a healthy heartbeat and never recovering it.
        if (!TerminateProcess(pi.hProcess,
                              GC_UPDATE_INSTALLER_ABANDONED_EXIT_CODE)) {
            debug_log("update install: could not terminate the overrunning "
                      "setup process (error %lu)\n", GetLastError());
        }
        DWORD settled = WaitForSingleObject(pi.hProcess,
                                            GC_UPDATE_INSTALL_ABANDON_TIMEOUT_MS);
        // Only WAIT_OBJECT_0 proves it is gone.  Anything else means we could
        // not establish the precondition, and the reservation is held rather
        // than released on an assumption -- the one case where the old
        // behaviour was right, now reached only when the kernel refuses to
        // kill a process rather than whenever setup is merely slow.
        if (settled == WAIT_OBJECT_0) {
            debug_log("update install: overrunning setup terminated; releasing "
                      "the GPU reservation\n");
        } else {
            reservationHeld = true;
            debug_log("update install: setup could not be terminated (wait=%lu); "
                      "GPU writes stay reserved because file replacement cannot "
                      "be ruled out\n", (unsigned long)settled);
        }
    }
    CloseHandle(pi.hProcess);

    // Released on every path except (a) a SUCCESSFUL install, where setup has
    // stopped this service and this process must not touch the GPU on its way
    // out, and (b) a setup process the kernel would not let us kill, where file
    // replacement genuinely cannot be ruled out.  This used to additionally
    // require having SEEN the process exit, which is how the timeout path
    // leaked the reservation for the life of the service.
    if (!ok && !reservationHeld) service_update_set_install_reserved(false);

    {
        GcUpdateStateLock guard;
        g_updateState.installRunning = false;
    }
    if (ok) {
        service_update_clear_staging();
        service_update_set_phase(SERVICE_UPDATE_PHASE_IDLE, "Update installed.");
        debug_log("update install: completed successfully\n");
    } else {
        service_update_set_phase(SERVICE_UPDATE_PHASE_FAILED, err);
        debug_log("update install: FAILED exit=%lu waited=%lu\n",
                  (unsigned long)exitCode, (unsigned long)waited);
    }
    return ok;
}
