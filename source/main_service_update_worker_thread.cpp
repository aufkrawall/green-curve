// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// The updater's worker thread, start gate, and automatic schedule tick.
// Split out of main_service_update_worker.cpp to keep both files inside the
// source-size ratchet; the orchestration functions it calls are statics from
// that shard and are visible because the amalgamation includes this after it.

enum GcUpdateWorkKind {
    // The periodic tick.  Auto-check must be ON and the interval elapsed.
    GC_UPDATE_WORK_CHECK_AUTOMATIC = 0,
    // Somebody pressed Check now.  That click is consent to this traffic, so a
    // found update is downloaded regardless of the automatic-check setting --
    // otherwise the manual path finds an update and does nothing with it, and
    // Install stays greyed out with no explanation.
    GC_UPDATE_WORK_CHECK_MANUAL = 1,
    GC_UPDATE_WORK_INSTALL = 2,
};

static void service_update_record_check_result(bool succeeded) {
    {
        GcUpdateStateLock guard;
        g_updateState.lastCheckUnix = service_update_now_unix();
        if (succeeded) {
            g_updateState.consecutiveFailures = 0;
        } else if (g_updateState.consecutiveFailures < 1000) {
            g_updateState.consecutiveFailures++;
        }
    }
    service_update_save_settings();
}

static DWORD WINAPI service_update_worker_thread(LPVOID param) {
    GcUpdateWorkKind kind = (GcUpdateWorkKind)(uintptr_t)param;
    char err[256] = {};

    if (kind == GC_UPDATE_WORK_INSTALL) {
        if (!service_update_run_install(err, sizeof(err))) {
            service_update_set_phase(SERVICE_UPDATE_PHASE_FAILED, err);
        }
    } else {
        bool checked = service_update_run_check(err, sizeof(err));
        service_update_record_check_result(checked);
        if (!checked) {
            // A transport failure must not erase proof that an already staged
            // package matched the trusted manifest.  The install gate re-verifies
            // again, so preserving READY here is fail-closed, not an assumption.
            bool stagedStillVerified = false;
            GcUpdateManifest currentManifest;
            bool currentManifestValid;
            {
                GcUpdateStateLock guard;
                stagedStillVerified = g_updateState.packageStaged &&
                                      g_updateState.decision ==
                                          GC_UPDATE_DECISION_AVAILABLE;
                currentManifest = g_updateState.manifest;
                currentManifestValid = g_updateState.manifestValid;
            }
            if (stagedStillVerified && currentManifestValid &&
                service_update_staged_package_matches_manifest(
                    &currentManifest, err, sizeof(err))) {
                service_update_set_phase(
                    SERVICE_UPDATE_PHASE_READY,
                    "The update check failed, but the verified package is still ready.");
            } else {
                service_update_set_phase(SERVICE_UPDATE_PHASE_FAILED, err);
            }
        } else {
            GcUpdateDecision decision;
            GcUpdateAutoCheck autoCheck;
            bool staged;
            GcUpdateManifest currentManifest;
            bool currentManifestValid;
            {
                GcUpdateStateLock guard;
                decision = g_updateState.decision;
                autoCheck = g_updateState.autoCheck;
                staged = g_updateState.packageStaged;
                currentManifest = g_updateState.manifest;
                currentManifestValid = g_updateState.manifestValid;
            }
            if (staged) {
                char verifyErr[256] = {};
                bool stagedStillVerified = currentManifestValid &&
                    service_update_staged_package_matches_manifest(
                        &currentManifest, verifyErr, sizeof(verifyErr));
                if (stagedStillVerified) {
                    service_update_set_phase(SERVICE_UPDATE_PHASE_READY, nullptr);
                    staged = true;
                } else {
                    debug_log("update check: staged package no longer matches the latest manifest; discarding it\n");
                    service_update_clear_staging();
                    staged = false;
                }
            }
            // Downloading changes nothing about the running system -- the file
            // lands in a directory a standard user cannot write.  INSTALLING is
            // still a separate, explicitly-consented step.
            if (gc_update_download_allowed(
                    autoCheck, kind == GC_UPDATE_WORK_CHECK_MANUAL,
                    decision == GC_UPDATE_DECISION_AVAILABLE, staged)) {
                if (!service_update_run_download(err, sizeof(err))) {
                    service_update_set_phase(SERVICE_UPDATE_PHASE_FAILED, err);
                }
            }
        }
    }

    {
        GcUpdateStateLock guard;
        g_updateState.workerRunning = false;
    }
    return 0;
}

// Start the worker if one is not already running.  Returns false when a job is
// already in flight, which the pipe handler reports rather than queueing: two
// concurrent downloads into one staging directory is not a situation worth
// supporting, and the GUI's button is disabled while a job runs anyway.
static bool service_update_start_worker(GcUpdateWorkKind kind, char* err, size_t errSize) {
    {
        GcUpdateStateLock guard;
        if (g_updateState.workerRunning) {
            set_message(err, errSize, "An update operation is already running");
            return false;
        }
        g_updateState.workerRunning = true;
    }
    HANDLE thread = CreateThread(nullptr, 0, service_update_worker_thread,
                                 (LPVOID)(uintptr_t)kind, 0, nullptr);
    if (!thread) {
        GcUpdateStateLock guard;
        g_updateState.workerRunning = false;
        set_message(err, errSize, "Cannot start the update worker (error %lu)",
                    GetLastError());
        return false;
    }
    CloseHandle(thread);
    return true;
}

// Called from the service's periodic lifecycle tick.  Does nothing at all
// unless the user turned automatic checking ON AND the interval (or its backoff)
// has elapsed.
static void service_update_maybe_auto_check() {
    GcUpdateAutoCheck setting;
    long long lastCheck;
    int interval, failures;
    bool busy;
    {
        GcUpdateStateLock guard;
        setting = g_updateState.autoCheck;
        lastCheck = g_updateState.lastCheckUnix;
        interval = g_updateState.intervalSeconds;
        failures = g_updateState.consecutiveFailures;
        busy = g_updateState.workerRunning || g_updateState.installRunning;
    }
    if (busy) return;
    if (!gc_update_auto_check_allowed(setting, lastCheck, service_update_now_unix(),
                                      interval, failures)) {
        return;
    }
    char err[256] = {};
    if (!service_update_start_worker(GC_UPDATE_WORK_CHECK_AUTOMATIC, err, sizeof(err))) {
        debug_log("update auto-check: not started: %s\n", err);
    } else {
        debug_log("update auto-check: started (interval=%ds failures=%d)\n",
                  interval, failures);
    }
}
