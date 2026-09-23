// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// Ownership marker and the once-per-crash handback (ownership_handback_policy.h).
//
// The marker is written durably at the same pre-write boundary that already
// invalidates the stability proof, and removed only by a successful reset to
// stock (graceful stop or explicit Reset).  Finding it at startup is the
// evidence that the previous instance of this service died while owning GPU
// state.  The handback returns that state to the driver exactly once:
//   - the lifecycle worker runs it as soon as the GPU is ready;
//   - any other hardware write that arrives first runs it before itself, so
//     nothing is ever layered over a dead instance's half-owned state.

#include "ownership_handback_policy.h"
#include "fan_runtime_policy.h"

static volatile LONG g_serviceOwnershipMarkerCommitted = 0;
static GpuAdapterInfo g_serviceOwnershipMarkerTarget = {};
// Guarded by the runtime lock.
static OwnershipHandbackScope g_serviceHandbackPendingScope =
    OWNERSHIP_HANDBACK_SCOPE_NONE;
static ServiceOwnershipMarker g_serviceHandbackMarker = {};
static volatile LONG g_serviceHandbackPending = 0;
// Set while the handback itself writes, so its own reset does not re-stamp the
// marker (which would clear the in-flight flag the crash-loop guard relies on).
static bool g_serviceHandbackRunning = false;

static bool service_ownership_marker_path(char* out, size_t outSize) {
    if (!out || outSize == 0) return false;
    char dir[MAX_PATH] = {};
    if (!resolve_service_machine_data_dir(dir, sizeof(dir))) return false;
    return SUCCEEDED(StringCchPrintfA(out, outSize,
        "%s\\service_ownership_marker.bin", dir));
}

static bool service_ownership_marker_store(const ServiceOwnershipMarker* marker) {
    if (!service_ownership_marker_valid(marker)) return false;
    char path[MAX_PATH] = {};
    if (!service_ownership_marker_path(path, sizeof(path))) return false;
    char pathErr[256] = {};
    if (!ensure_parent_directory_for_file(path, pathErr, sizeof(pathErr))) {
        debug_log("ownership marker: cannot create directory: %s\n",
            pathErr[0] ? pathErr : "unknown error");
        return false;
    }
    char tempPath[MAX_PATH] = {};
    if (FAILED(StringCchPrintfA(tempPath, ARRAY_COUNT(tempPath), "%s.tmp.%lu.%llu",
            path, (unsigned long)GetCurrentProcessId(),
            (unsigned long long)GetTickCount64()))) return false;
    HANDLE h = gc_CreateFileUtf8(tempPath, GENERIC_WRITE, 0, nullptr, CREATE_NEW,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        debug_log("ownership marker: temp create failed (error=%lu)\n", GetLastError());
        return false;
    }
    DWORD written = 0;
    bool ok = WriteFile(h, marker, sizeof(*marker), &written, nullptr) &&
        written == sizeof(*marker) && FlushFileBuffers(h) != FALSE;
    CloseHandle(h);
    if (ok) {
        ok = gc_MoveFileExUtf8(tempPath, path,
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
    }
    if (!ok) {
        debug_log("ownership marker: commit failed (error=%lu)\n", GetLastError());
        gc_DeleteFileUtf8(tempPath);
    }
    return ok;
}

// 0 = absent, 1 = valid, -1 = present but unreadable/corrupt.
static int service_ownership_marker_load(ServiceOwnershipMarker* out) {
    memset(out, 0, sizeof(*out));
    char path[MAX_PATH] = {};
    if (!service_ownership_marker_path(path, sizeof(path))) return -1;
    HANDLE h = gc_CreateFileUtf8(path, GENERIC_READ, FILE_SHARE_READ, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        DWORD error = GetLastError();
        if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) return 0;
        debug_log("ownership marker: unreadable (error=%lu)\n", error);
        return -1;
    }
    DWORD read = 0;
    BYTE trailing = 0;
    DWORD trailingRead = 0;
    bool ok = ReadFile(h, out, sizeof(*out), &read, nullptr) && read == sizeof(*out) &&
        ReadFile(h, &trailing, sizeof(trailing), &trailingRead, nullptr) &&
        trailingRead == 0;
    CloseHandle(h);
    if (!ok || !service_ownership_marker_valid(out)) {
        memset(out, 0, sizeof(*out));
        return -1;
    }
    return 1;
}

static bool service_ownership_marker_clear(const char* reason) {
    InterlockedExchange(&g_serviceOwnershipMarkerCommitted, 0);
    memset(&g_serviceOwnershipMarkerTarget, 0, sizeof(g_serviceOwnershipMarkerTarget));
    char path[MAX_PATH] = {};
    if (!service_ownership_marker_path(path, sizeof(path))) return false;
    bool ok = gc_DeleteFileUtf8(path) != FALSE;
    DWORD error = ok ? ERROR_SUCCESS : GetLastError();
    if (!ok && (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND)) {
        return true;
    }
    debug_log("ownership marker: %s (%s)\n",
        ok ? "retired" : "could not be retired",
        reason && reason[0] ? reason : "unspecified");
    return ok;
}

static bool service_ownership_current_target(GpuAdapterInfo* out) {
    memset(out, 0, sizeof(*out));
    if (g_app.selectedGpu.valid) {
        *out = g_app.selectedGpu;
    } else if (g_app.selectedGpuIndex < g_app.adapterCount &&
               g_app.adapters[g_app.selectedGpuIndex].valid) {
        *out = g_app.adapters[g_app.selectedGpuIndex];
    }
    return out->valid != 0;
}

// The pre-write boundary.  Fail closed like the proof invalidation beside it:
// a write whose ownership cannot be recorded is a write a crash could strand.
static bool service_ownership_marker_ensure_before_write() {
    if (!g_app.isServiceProcess || g_serviceHandbackRunning) return true;
    if (InterlockedExchangeAdd(&g_serviceHandbackPending, 0) != 0) {
        // Reached only when the pre-write hook found the GPU not ready for the
        // handback but this write is going ahead anyway: from here on the new
        // write owns the GPU, and a later handback would undo it.
        debug_log("ownership handback: superseded by a newer hardware write before it could run\n");
        g_serviceHandbackPendingScope = OWNERSHIP_HANDBACK_SCOPE_NONE;
        memset(&g_serviceHandbackMarker, 0, sizeof(g_serviceHandbackMarker));
        InterlockedExchange(&g_serviceHandbackPending, 0);
    }
    GpuAdapterInfo target = {};
    service_ownership_current_target(&target);
    if (InterlockedExchangeAdd(&g_serviceOwnershipMarkerCommitted, 0) != 0 &&
        (!target.valid || !g_serviceOwnershipMarkerTarget.valid ||
         gpu_adapter_has_same_pci_identity(&target, &g_serviceOwnershipMarkerTarget))) {
        return true;
    }
    ServiceBootIdentity boot = {};
    if (!service_query_boot_identity(&boot)) {
        debug_log("ownership marker: boot identity unavailable; refusing the hardware write\n");
        return false;
    }
    ServiceOwnershipMarker marker = {};
    service_ownership_marker_initialize(&marker, boot, target.valid ? &target : nullptr);
    if (!service_ownership_marker_store(&marker)) {
        debug_log("ownership marker: could not be committed; refusing the hardware write"
                  " (a crash after it could not be handed back)\n");
        return false;
    }
    g_serviceOwnershipMarkerTarget = target;
    InterlockedExchange(&g_serviceOwnershipMarkerCommitted, 1);
    debug_log("ownership marker: committed before the first hardware write (target=%s)\n",
        target.valid && target.name[0] ? target.name : "<current selection>");
    return true;
}

// Startup, file I/O only, before RUNNING.  Decides; never writes hardware.
static void service_ownership_handback_prepare_at_startup(bool controlledRecoveryValidated) {
    ServiceOwnershipMarker marker = {};
    int loaded = service_ownership_marker_load(&marker);
    ServiceBootIdentity currentBoot = {};
    bool bootKnown = service_query_boot_identity(&currentBoot);
    OwnershipHandbackInputs in = {};
    in.markerPresent = loaded != 0;
    in.markerValid = loaded == 1;
    in.currentBootKnown = bootKnown;
    in.sameBoot = loaded == 1 && bootKnown &&
        service_boot_identity_equal(marker.bootIdentity, currentBoot);
    in.previousHandbackInFlight = loaded == 1 && marker.handbackInFlight != 0;
    OwnershipHandbackPlan plan = ownership_handback_plan(in,
        ownership_handback_windows_start_scope(controlledRecoveryValidated));
    if (plan.verdict == OWNERSHIP_HANDBACK_NO_MARKER) {
        debug_log("ownership handback: no marker; the previous instance returned what it owned\n");
        return;
    }
    debug_log("ownership handback: previous instance left GPU state it owned;"
              " verdict=%s scope=%s markerValid=%d bootKnown=%d sameBoot=%d"
              " previousHandbackInFlight=%d controlledRecovery=%d\n",
        ownership_handback_verdict_name(plan.verdict),
        ownership_handback_scope_name(plan.scope), in.markerValid ? 1 : 0,
        in.currentBootKnown ? 1 : 0, in.sameBoot ? 1 : 0,
        in.previousHandbackInFlight ? 1 : 0, controlledRecoveryValidated ? 1 : 0);
    if (plan.verdict == OWNERSHIP_HANDBACK_GIVE_UP_PREVIOUS_ATTEMPT_DIED) {
        service_latch_auto_restore_lockout(
            SERVICE_AUTO_RESTORE_LOCKOUT_AUTOMATIC_APPLY_FAILED,
            "the previous instance died while handing its GPU state back");
        char logErr[256] = {};
        write_error_report_log("Returning GPU settings to stock was abandoned",
            "The background service stopped unexpectedly while returning GPU"
            " settings to stock after an earlier unexpected stop. It did not try"
            " again, to avoid a restart loop. Use Reset in Green Curve to return"
            " the GPU to stock.", logErr, sizeof(logErr));
    }
    if (plan.deleteMarker) {
        service_ownership_marker_clear(ownership_handback_verdict_name(plan.verdict));
        return;
    }
    g_serviceHandbackMarker = marker;
    g_serviceHandbackPendingScope = plan.scope;
    InterlockedExchange(&g_serviceHandbackPending, 1);
}

static void service_ownership_handback_finish_locked() {
    g_serviceHandbackPendingScope = OWNERSHIP_HANDBACK_SCOPE_NONE;
    memset(&g_serviceHandbackMarker, 0, sizeof(g_serviceHandbackMarker));
    InterlockedExchange(&g_serviceHandbackPending, 0);
}

// An explicit Reset returns everything a pending handback would; it takes the
// handback over whatever its own outcome (a failed Reset is reported to the
// user who asked for it).
static void service_ownership_handback_superseded_by_reset_locked() {
    if (InterlockedExchangeAdd(&g_serviceHandbackPending, 0) == 0 ||
        g_serviceHandbackRunning) return;
    debug_log("ownership handback: superseded by a Reset to stock\n");
    service_ownership_handback_finish_locked();
}

// Runtime lock held.  Returns true when nothing is pending or the handback
// completed.  `*writeAttemptedOut` distinguishes "not ready yet" (retry on the
// next readiness cue) from a terminal outcome.
static bool service_ownership_handback_run_locked(const char* origin,
    char* detail, size_t detailSize, bool* writeAttemptedOut) {
    if (writeAttemptedOut) *writeAttemptedOut = false;
    if (detail && detailSize) detail[0] = 0;
    if (InterlockedExchangeAdd(&g_serviceHandbackPending, 0) == 0) return true;
    const OwnershipHandbackScope scope = g_serviceHandbackPendingScope;

    if (!hardware_initialize(detail, detailSize) || g_app.deviceRemoved) {
        debug_log("ownership handback (%s): GPU not ready; stays pending: %s\n",
            origin, detail && detail[0] ? detail : "device removed");
        return false;
    }
    if (g_serviceHandbackMarker.targetGpu.valid) {
        char selectErr[256] = {};
        if (!service_select_restart_reapply_gpu(&g_serviceHandbackMarker.targetGpu,
                selectErr, sizeof(selectErr))) {
            // The adapter that was written is gone.  Its re-arrival
            // re-initializes it, so there is nothing left to hand back here.
            debug_log("ownership handback (%s): owned GPU is not present (%s);"
                      " retiring the marker without a write\n", origin, selectErr);
            service_ownership_marker_clear("owned GPU not present");
            service_ownership_handback_finish_locked();
            return true;
        }
    }
    ServiceSelectedGpuWriteEpoch gpuEpoch = service_selected_gpu_capture_write_epoch();

    // Write-before-act: if this handback kills the process, the next start
    // must see that and stop, not repeat it forever.
    ServiceOwnershipMarker inFlight = g_serviceHandbackMarker;
    inFlight.handbackInFlight = 1u;
    if (!service_ownership_marker_store(&inFlight)) {
        set_message(detail, detailSize,
            "The handback could not record itself durably, so it was not attempted");
        debug_log("ownership handback (%s): in-flight record failed; not writing"
                  " (an unguarded handback could crash-loop)\n", origin);
        service_latch_auto_restore_lockout(
            SERVICE_AUTO_RESTORE_LOCKOUT_AUTOMATIC_APPLY_FAILED,
            "ownership handback could not be recorded before writing");
        service_ownership_handback_finish_locked();
        return false;
    }
    if (!service_selected_gpu_write_epoch_is_current(gpuEpoch)) {
        inFlight.handbackInFlight = 0u;
        service_ownership_marker_store(&inFlight);
        debug_log("ownership handback (%s): selected GPU changed before the write; stays pending\n",
            origin);
        return false;
    }
    if (writeAttemptedOut) *writeAttemptedOut = true;
    g_serviceHandbackRunning = true;
    debug_log("ownership handback (%s): returning %s GPU state to the driver\n",
        origin, ownership_handback_scope_name(scope));

    // Fan first: it is the only owned state that is unsafe without a
    // controller.  Idempotent when the driver already owns the fan.
    bool fanOk = true;
    char fanDetail[128] = {};
    if (g_app.fanSupported) {
        fanOk = nvml_set_fan_auto(fanDetail, sizeof(fanDetail));
        if (fanOk) {
            g_app.fanIsAuto = true;
            g_app.activeFanMode = FAN_MODE_AUTO;
            g_app.activeFanFixedPercent = 0;
        } else if (fan_runtime_escalation_after_auto_restore(false) ==
                   FAN_RUNTIME_ESCALATION_EMERGENCY_MAX) {
            char emergencyDetail[128] = {};
            bool emergencyOk = nvml_set_fan_manual(FAN_RUNTIME_EMERGENCY_PERCENT,
                nullptr, emergencyDetail, sizeof(emergencyDetail));
            debug_log("ownership handback (%s): driver auto fan refused (%s);"
                      " forced %d%% ok=%d\n", origin,
                fanDetail[0] ? fanDetail : "unknown", FAN_RUNTIME_EMERGENCY_PERCENT,
                emergencyOk ? 1 : 0);
        }
    }
    debug_log("ownership handback (%s): fan -> driver auto ok=%d%s%s\n", origin,
        fanOk ? 1 : 0, fanDetail[0] ? " detail=" : "", fanDetail);

    bool resetOk = true;
    char resetDetail[512] = {};
    if (scope == OWNERSHIP_HANDBACK_SCOPE_FULL) {
        bool resetAttempted = false;
        resetOk = service_reset_all(resetDetail, sizeof(resetDetail), &resetAttempted);
        debug_log("ownership handback (%s): reset to stock ok=%d attempted=%d detail=%s\n",
            origin, resetOk ? 1 : 0, resetAttempted ? 1 : 0,
            resetDetail[0] ? resetDetail : "none");
    }
    g_serviceHandbackRunning = false;

    const bool success = fanOk && resetOk;
    if (success && ownership_handback_retires_marker(scope, false)) {
        // service_reset_all() already retired it; make that unconditional.
        service_ownership_marker_clear("handback returned the owned state");
    } else {
        // Survived: the in-flight flag must not read as a crash next time.  A
        // FAN_ONLY handback leaves a controlled recovery that still owns intent.
        inFlight.handbackInFlight = 0u;
        if (service_ownership_marker_store(&inFlight)) {
            g_serviceOwnershipMarkerTarget = inFlight.targetGpu;
            InterlockedExchange(&g_serviceOwnershipMarkerCommitted, 1);
        }
    }
    if (!success) {
        set_message(detail, detailSize, "%s%s%s",
            !fanOk ? "The fan could not be returned to driver control. " : "",
            !resetOk ? "The GPU could not be fully returned to stock: " : "",
            !resetOk ? resetDetail : "");
        service_latch_auto_restore_lockout(
            SERVICE_AUTO_RESTORE_LOCKOUT_AUTOMATIC_APPLY_FAILED,
            "ownership handback after an unexpected stop did not complete");
        char logErr[256] = {};
        write_error_report_log(
            "Returning GPU settings to stock after an unexpected stop failed",
            detail, logErr, sizeof(logErr));
    }
    debug_log("ownership handback (%s): %s scope=%s\n", origin,
        success ? "complete" : "FAILED", ownership_handback_scope_name(scope));
    service_ownership_handback_finish_locked();
    return success;
}

// The pre-write hook for every other hardware write: a dead instance's state
// is returned before anything is layered over it.  Runtime lock held.
static bool service_ownership_handback_before_write_locked(const char* origin,
    char* result, size_t resultSize) {
    if (InterlockedExchangeAdd(&g_serviceHandbackPending, 0) == 0 ||
        g_serviceHandbackRunning) return true;
    char detail[512] = {};
    bool attempted = false;
    bool ok = service_ownership_handback_run_locked(origin, detail, sizeof(detail),
        &attempted);
    if (ok || !attempted) return true;  // not ready: the write's own init decides
    set_message(result, resultSize,
        "Returning the GPU to stock after the service's unexpected stop failed,"
        " so nothing else was written: %s", detail[0] ? detail : "unknown error");
    return false;
}

// Lifecycle worker entry: run as soon as the GPU is ready.
static void service_lifecycle_attempt_ownership_handback() {
    if (InterlockedExchangeAdd(&g_serviceHandbackPending, 0) == 0) return;
    lock_service_runtime();
    if (service_update_install_release_and_block()) return;
    char detail[512] = {};
    bool attempted = false;
    bool ok = service_ownership_handback_run_locked("startup", detail,
        sizeof(detail), &attempted);
    if (ok || attempted) {
        populate_control_state(&g_serviceControlState);
        g_serviceControlStateValid = true;
        mark_service_telemetry_cache_updated("ownership handback");
    }
    unlock_service_runtime();
    if (!ok && !attempted) {
        debug_log("ownership handback: waiting for a real GPU readiness signal (%s)\n",
            detail[0] ? detail : "not ready");
    }
}
