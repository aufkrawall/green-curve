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

    {
        GcUpdateStateLock guard;
        g_updateState.manifest = manifest;
        g_updateState.manifestValid = true;
        g_updateState.decision = decision;
    }

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

// ---------------------------------------------------------------------------
// Stopping the GUI, across sessions
// ---------------------------------------------------------------------------

// How long a GUI gets to close itself after seeing guiShutdownRequested, and
// how long it then gets to die after being terminated.  Both are bounded
// because this runs before an install that will fail at file replacement if a
// GUI is still holding greencurve.exe.
#define GC_UPDATE_GUI_EXIT_TIMEOUT_MS 10000
#define GC_UPDATE_GUI_KILL_TIMEOUT_MS 5000
#define GC_UPDATE_MAX_GUI_PROCESSES 32

// Open every greencurve.exe running out of `installDir`, in ANY session.
//
// Matched by full image path, not by name: another process called
// greencurve.exe somewhere else on the machine is not ours to terminate, and
// this code runs as SYSTEM where that mistake is unrecoverable.
static bool service_update_collect_gui_processes(const char* installDir,
                                                 HANDLE* handles, int maxCount,
                                                 int* countOut) {
    if (countOut) *countOut = 0;
    auto fail_closed = [&](DWORD error, const char* reason) {
        for (int i = 0; i < maxCount; ++i) {
            if (handles[i]) CloseHandle(handles[i]);
            handles[i] = nullptr;
        }
        if (countOut) *countOut = 0;
        debug_log("update install: GUI enumeration FAILED at %s (error %lu); "
                  "refusing to proceed\n", reason, error);
        return false;
    };

    if (!handles || maxCount <= 0 || !installDir || !installDir[0])
        return fail_closed(ERROR_INVALID_PARAMETER, "argument validation");

    char wanted[MAX_PATH] = {};
    if (FAILED(StringCchPrintfA(wanted, sizeof(wanted), "%s\\%s",
                                installDir, APP_EXE_NAME))) {
        return fail_closed(GetLastError(), "target path construction");
    }

    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE)
        return fail_closed(GetLastError(), "process snapshot");

    int found = 0;
    PROCESSENTRY32W entry = {};
    entry.dwSize = sizeof(entry);
    if (Process32FirstW(snapshot, &entry)) {
        do {
            if (entry.th32ProcessID == GetCurrentProcessId()) continue;
            if (_wcsicmp(entry.szExeFile, APP_EXE_NAME_W) != 0) continue;

            HANDLE process = OpenProcess(
                SYNCHRONIZE | PROCESS_TERMINATE | PROCESS_QUERY_LIMITED_INFORMATION,
                FALSE, entry.th32ProcessID);
            if (!process)
                return fail_closed(GetLastError(), "opening candidate GUI");

            WCHAR imageW[MAX_PATH] = {};
            DWORD imageChars = ARRAY_COUNT(imageW);
            char image[MAX_PATH] = {};
            bool mine = false;
            if (QueryFullProcessImageNameW(process, 0, imageW, &imageChars) &&
                copy_wide_to_utf8(imageW, image, (int)sizeof(image))) {
                mine = _stricmp(image, wanted) == 0;
            }
            if (!mine) {
                CloseHandle(process);
                continue;
            }
            if (found >= maxCount) {
                CloseHandle(process);
                CloseHandle(snapshot);
                return fail_closed(ERROR_TOO_MANY_CMDS,
                                   "matching GUI process count");
            }
            handles[found++] = process;
            debug_log("update install: found GUI process pid=%lu session-agnostic path=%s\n",
                      (unsigned long)entry.th32ProcessID, image);
        } while (Process32NextW(snapshot, &entry));
    } else {
        DWORD enumError = GetLastError();
        CloseHandle(snapshot);
        return fail_closed(enumError, "first process enumeration");
    }
    CloseHandle(snapshot);
    if (countOut) *countOut = found;
    return true;
}

// Ask every GUI to close, then make sure it did.
//
// Fails closed: if a GUI is still alive afterwards the install must NOT
// proceed, because setup would get to the file-replacement step and stop there
// with ERROR_ACCESS_DENIED -- which is precisely the failure this exists to
// prevent, and it leaves a half-copied install directory behind.
static bool service_update_stop_gui_processes(const char* installDir,
                                              int* closedCountOut,
                                              char* err, size_t errSize) {
    if (closedCountOut) *closedCountOut = 0;
    HANDLE handles[GC_UPDATE_MAX_GUI_PROCESSES] = {};
    int count = 0;
    if (!service_update_collect_gui_processes(
            installDir, handles, GC_UPDATE_MAX_GUI_PROCESSES, &count)) {
        set_message(err, errSize,
                    "Could not prove every Green Curve window was closed; "
                    "the update was not installed");
        return false;
    }
    if (count == 0) {
        debug_log("update install: no GUI processes are running\n");
        return true;
    }

    // Publish the request and let each GUI run its own Exit path.  Graceful
    // matters: that path releases the tray icon, the single-instance mutex and
    // the service connection, none of which TerminateProcess would.
    {
        GcUpdateStateLock guard;
        g_updateState.guiShutdownRequested = true;
    }
    debug_log("update install: asked %d GUI process(es) to close\n", count);

    DWORD waited = WaitForMultipleObjects((DWORD)count, handles, TRUE,
                                          GC_UPDATE_GUI_EXIT_TIMEOUT_MS);
    bool allGone = waited >= WAIT_OBJECT_0 && waited < WAIT_OBJECT_0 + (DWORD)count;

    if (!allGone) {
        // A GUI that ignored the request (wedged, or not polling) is terminated.
        // The installer's own stop step does the same escalation for the same
        // reason; a hung window must not block an update forever.
        debug_log("update install: GUI did not exit within %d ms; terminating\n",
                  GC_UPDATE_GUI_EXIT_TIMEOUT_MS);
        for (int i = 0; i < count; ++i) {
            if (WaitForSingleObject(handles[i], 0) == WAIT_TIMEOUT) {
                TerminateProcess(handles[i], 0);
            }
        }
        waited = WaitForMultipleObjects((DWORD)count, handles, TRUE,
                                        GC_UPDATE_GUI_KILL_TIMEOUT_MS);
        allGone = waited >= WAIT_OBJECT_0 && waited < WAIT_OBJECT_0 + (DWORD)count;
    }

    for (int i = 0; i < count; ++i) CloseHandle(handles[i]);
    {
        GcUpdateStateLock guard;
        g_updateState.guiShutdownRequested = false;
    }

    if (!allGone) {
        set_message(err, errSize,
                    "A Green Curve window is still running and could not be "
                    "closed; the update was not installed");
        debug_log("update install: ABORTED, GUI processes still alive\n");
        return false;
    }
    // Report how many were closed.  The caller turns this into --launch vs
    // --no-launch, so leaving it at the zero set on entry silently means "no
    // GUI was running" and the user's window never comes back -- which is
    // exactly what shipped in 0.27.
    if (closedCountOut) *closedCountOut = count;
    debug_log("update install: all %d GUI process(es) exited\n", count);
    return true;
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

    // Close every GUI BEFORE setup starts.  Setup runs in session 0 (the
    // service launched it) and cannot see, let alone close, a window in the
    // user's session -- so left to itself it reports "no running Green Curve
    // window found" and then fails replacing greencurve.exe with error 5.
    //
    // This also has to happen before the command line is built, because how
    // many GUIs were closed decides whether setup should start one again.
    int closedGuiCount = 0;
    if (!service_update_stop_gui_processes(installDir, &closedGuiCount,
                                           err, errSize)) {
        CloseHandle(pinned);
        service_update_set_install_reserved(false);
        GcUpdateStateLock guard;
        g_updateState.installRunning = false;
        return false;
    }

    // Built by update_install_policy.h, which exists because this was four
    // lines here and shipped an unquoted `/D=<path with spaces>` that setup
    // correctly refused -- exit 3, before any step, for every default
    // installation.  The builder is pure so the exact string is asserted
    // against the real installer parser (4260-4269).
    //
    // Relaunch the GUI only if one was actually running: the stop above counted
    // them, so this is measured rather than assumed.
    char commandLine[GC_UPDATE_COMMAND_LINE_MAX_CHARS] = {};
    char launchSessionId[16] = {};
    StringCchPrintfA(launchSessionId, sizeof(launchSessionId), "%lu",
                     (unsigned long)requestingSessionId);
    if (!gc_update_build_installer_command_line(stagedPath, installDir,
                                                closedGuiCount > 0,
                                                launchSessionId,
                                                commandLine, sizeof(commandLine))) {
        CloseHandle(pinned);
        service_update_set_install_reserved(false);
        set_message(err, errSize,
                    "Cannot build a usable installer command line for %s",
                    installDir);
        GcUpdateStateLock guard;
        g_updateState.installRunning = false;
        return false;
    }
    debug_log("update install: command line is %s\n", commandLine);

    Win32Utf8Path wideCommand(commandLine);
    if (!wideCommand.valid_for(commandLine)) {
        CloseHandle(pinned);
        service_update_set_install_reserved(false);
        GcUpdateStateLock guard;
        g_updateState.installRunning = false;
        set_message(err, errSize, "Cannot encode the installer command line");
        return false;
    }

    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi = {};
    BOOL created = CreateProcessW(nullptr, wideCommand.value, nullptr, nullptr,
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
    bool processExited = false;
    if (waited == WAIT_OBJECT_0 && GetExitCodeProcess(pi.hProcess, &exitCode)) {
        processExited = true;
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
        set_message(err, errSize, "The installer did not finish in time");
    }
    CloseHandle(pi.hProcess);

    if (!ok && processExited) service_update_set_install_reserved(false);

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
