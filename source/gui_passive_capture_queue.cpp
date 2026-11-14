// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
// Included after coordinator initialization; all queue state uses its lock.
static bool gui_service_io_queue_capture(const GpuAdapterInfo& target, gc_u32 domains,
    gc_u64 token, char* error, size_t size) {
    GuiPassiveCaptureWork work{};
    if (!gui_passive_capture_prepare(target, domains, token, work)) {
        set_message(error, size, "A destination check requires a named GPU and valid domains"); return false;
    }
    work.request.callerPid = GetCurrentProcessId();
    ProcessIdToSessionId(work.request.callerPid, &work.request.callerSessionId);
    StringCchCopyA(work.request.source, ARRAY_COUNT(work.request.source), "GUI passive profile check");
    if (!gui_mutation_initialize(error, size)) return false;
    EnterCriticalSection(&g_guiMutationLock);
    if (g_guiMutationShuttingDown || g_guiCapturePending || g_guiMutationActive || g_guiAdminTogglePending) {
        LeaveCriticalSection(&g_guiMutationLock);
        set_message(error, size, "Wait for the pending GPU or service operation before checking"); return false;
    }
    g_guiCaptureWork = work; g_guiCapturePending = true;
    g_guiTelemetryPending = false; g_guiTelemetryRedrawControls = false;
    gui_worker_signal_if_dispatchable_locked();
    LeaveCriticalSection(&g_guiMutationLock);
    debug_log("profile-check: queued token=%llu domains=0x%03X\n", (unsigned long long)token, (unsigned)domains);
    return true;
}
