// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
// Included after coordinator initialization. Queue ownership uses its lock.
static bool gui_service_io_queue_transfer(std::unique_ptr<GuiProfileTransferWork>& work, char* error, size_t size) {
    if (!work || !work->token || (work->committing && (!work->preview || !work->preview->ready))) {
        set_message(error, size, "Review a profile transfer before committing"); return false;
    }
    if (!gui_mutation_initialize(error, size)) return false;
    EnterCriticalSection(&g_guiMutationLock);
    if (g_guiMutationShuttingDown || g_guiProfileTransferPending || g_guiProfileTransferCompleted || g_guiProfileTransferActive) {
        LeaveCriticalSection(&g_guiMutationLock);
        set_message(error, size, "Wait for the pending file operation before transferring profiles"); return false;
    }
    const auto token = work->token; const bool committing = work->committing;
    g_guiProfileTransferPending = std::move(work);
    gui_worker_signal_if_dispatchable_locked(); LeaveCriticalSection(&g_guiMutationLock);
    debug_log("profile-import: queued token=%llu commit=%d\n", (unsigned long long)token, committing ? 1 : 0);
    return true;
}
static void gui_service_io_cancel_transfer(gc_u64 token) {
    if (!g_guiMutationLockReady || !token) return;
    EnterCriticalSection(&g_guiMutationLock);
    const bool cancelled = gui_profile_transfer_cancel_pending(g_guiProfileTransferPending, token);
    LeaveCriticalSection(&g_guiMutationLock);
    if (cancelled) debug_log("profile-import: cancelled pending token=%llu\n", (unsigned long long)token);
}
static std::unique_ptr<GuiProfileTransferWork> gui_service_io_take_transfer() {
    if (!g_guiMutationLockReady) return {};
    EnterCriticalSection(&g_guiMutationLock);
    auto work = std::move(g_guiProfileTransferCompleted);
    LeaveCriticalSection(&g_guiMutationLock);
    return work;
}
