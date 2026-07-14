// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT

// One serialized client-side mutation worker. The worker owns only immutable
// request copies and IPC; all AppData and HWND changes happen after the result
// is posted back to the window thread.

#include "gui_mutation_queue_policy.h"

struct GuiMutationWork {
    GuiMutationKind kind;
    GuiMutationUiContext context;
    ServiceRequest request;
    DesiredSettings desired;
    ServiceProfileSource profileSource;
    ServiceApplyOrigin origin;
    int profileSlot;
    LONG gpuEpoch;
    DWORD sessionId;
    ULONGLONG queuedTickMs;
    HWND notifyWindow;
};

struct GuiMutationCompletion {
    GuiMutationWork work;
    ServiceResponse response;
    bool success;
    ULONGLONG durationMs;
    char result[512];
};

static CRITICAL_SECTION g_guiMutationLock;
static bool g_guiMutationLockReady = false;
static HANDLE g_guiMutationEvent = nullptr;
static HANDLE g_guiMutationThread = nullptr;
static bool g_guiMutationActive = false;
static bool g_guiMutationPending = false;
static bool g_guiMutationShuttingDown = false;
static GuiMutationWork g_guiMutationActiveWork = {};
static GuiMutationWork g_guiMutationPendingWork = {};
static volatile LONG g_guiMutationGpuEpoch = 1;

static LONG gui_mutation_gpu_epoch() {
    return InterlockedExchangeAdd(&g_guiMutationGpuEpoch, 0);
}

static void gui_mutation_advance_gpu_epoch(const char* reason) {
    LONG epoch = InterlockedIncrement(&g_guiMutationGpuEpoch);
    debug_log("GUI mutation: GPU epoch advanced to %ld (%s)\n", epoch,
        reason && reason[0] ? reason : "state change");
}

static bool gui_mutation_work_context_is_current(const GuiMutationWork* work,
    char* result, size_t resultSize) {
    if (!work) return false;
    DWORD currentSession = 0;
    if (!ProcessIdToSessionId(GetCurrentProcessId(), &currentSession) ||
        currentSession != work->sessionId) {
        set_message(result, resultSize,
            "Queued GPU operation rejected because the client session changed");
        return false;
    }
    LONG epoch = gui_mutation_gpu_epoch();
    if (epoch != work->gpuEpoch) {
        set_message(result, resultSize,
            "Queued GPU operation rejected because the selected GPU changed");
        return false;
    }
    return true;
}

static void gui_mutation_worker_release_after_post_failure() {
    EnterCriticalSection(&g_guiMutationLock);
    g_guiMutationActive = false;
    if (!g_guiMutationShuttingDown && g_guiMutationPending) {
        g_guiMutationActiveWork = g_guiMutationPendingWork;
        g_guiMutationPending = false;
        g_guiMutationActive = true;
        SetEvent(g_guiMutationEvent);
    }
    LeaveCriticalSection(&g_guiMutationLock);
}

static DWORD WINAPI gui_mutation_worker_proc(void*) {
    for (;;) {
        if (WaitForSingleObject(g_guiMutationEvent, INFINITE) != WAIT_OBJECT_0)
            return 1;
        GuiMutationWork work = {};
        EnterCriticalSection(&g_guiMutationLock);
        bool stop = g_guiMutationShuttingDown;
        if (!stop && g_guiMutationActive) work = g_guiMutationActiveWork;
        LeaveCriticalSection(&g_guiMutationLock);
        if (stop) return 0;
        if (!work.request.operationId) continue;

        GuiMutationCompletion* completion =
            (GuiMutationCompletion*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
                sizeof(GuiMutationCompletion));
        if (!completion) {
            gui_mutation_worker_release_after_post_failure();
            continue;
        }
        completion->work = work;
        ULONGLONG started = GetTickCount64();
        if (gui_mutation_work_context_is_current(&work, completion->result,
                sizeof(completion->result))) {
            completion->success = service_client_execute_mutation_request(
                &work.request, &completion->response, completion->result,
                sizeof(completion->result));
        }
        completion->durationMs = GetTickCount64() - started;
        debug_log("GUI mutation: operation=%llu origin=%lu durationMs=%llu final=%lu success=%d\n",
            (unsigned long long)work.request.operationId,
            (unsigned long)work.origin,
            (unsigned long long)completion->durationMs,
            (unsigned long)completion->response.operationState,
            completion->success ? 1 : 0);
        if (!PostMessageW(work.notifyWindow, APP_WM_MUTATION_COMPLETE, 0,
                (LPARAM)completion)) {
            HeapFree(GetProcessHeap(), 0, completion);
            gui_mutation_worker_release_after_post_failure();
        }
        // The GUI thread acknowledges completion after applying the response.
        // Only then may the latest pending request become active.
    }
}

static bool gui_mutation_initialize(char* err, size_t errSize) {
    if (g_guiMutationThread) return true;
    if (!g_guiMutationLockReady) {
        InitializeCriticalSection(&g_guiMutationLock);
        g_guiMutationLockReady = true;
    }
    g_guiMutationEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!g_guiMutationEvent) {
        set_message(err, errSize, "Could not create the GPU worker event (error %lu)",
            GetLastError());
        return false;
    }
    g_guiMutationThread = CreateThread(nullptr, 0, gui_mutation_worker_proc,
        nullptr, 0, nullptr);
    if (!g_guiMutationThread) {
        DWORD error = GetLastError();
        CloseHandle(g_guiMutationEvent);
        g_guiMutationEvent = nullptr;
        set_message(err, errSize, "Could not create the GPU worker thread (error %lu)",
            error);
        return false;
    }
    return true;
}

static bool gui_mutation_enqueue(const GuiMutationWork* work,
    char* status, size_t statusSize) {
    if (!work || !work->notifyWindow) return false;
    if (!gui_mutation_initialize(status, statusSize)) return false;

    bool notifySuperseded = false;
    GuiMutationWork superseded = {};
    EnterCriticalSection(&g_guiMutationLock);
    GuiMutationQueueDecision decision = gui_mutation_queue_decide(
        g_guiMutationActive, g_guiMutationPending,
        g_guiMutationPending ? g_guiMutationPendingWork.kind : GUI_MUTATION_APPLY,
        work->kind);
    if (decision == GUI_MUTATION_QUEUE_START) {
        g_guiMutationActiveWork = *work;
        g_guiMutationActive = true;
        SetEvent(g_guiMutationEvent);
        set_message(status, statusSize, "GPU operation started in the background");
    } else if (decision == GUI_MUTATION_QUEUE_KEEP_PENDING_RESET) {
        set_message(status, statusSize,
            "Reset is already pending and takes precedence over this Apply");
        LeaveCriticalSection(&g_guiMutationLock);
        return false;
    } else {
        if (decision == GUI_MUTATION_QUEUE_REPLACE_PENDING) {
            superseded = g_guiMutationPendingWork;
            notifySuperseded = true;
            debug_log("GUI mutation: superseded pending operation=%llu with operation=%llu kind=%d\n",
                (unsigned long long)g_guiMutationPendingWork.request.operationId,
                (unsigned long long)work->request.operationId, (int)work->kind);
        }
        g_guiMutationPendingWork = *work;
        g_guiMutationPending = true;
        set_message(status, statusSize,
            decision == GUI_MUTATION_QUEUE_REPLACE_PENDING
                ? "Latest GPU operation replaced the previous pending request"
                : "GPU operation queued behind the active hardware write");
    }
    LeaveCriticalSection(&g_guiMutationLock);
    g_app.applyInFlight = true;
    if (notifySuperseded) {
        gui_mutation_pending_was_superseded(superseded.context,
            superseded.profileSlot, superseded.origin,
            superseded.request.operationId);
    }
    return true;
}

static bool gui_mutation_queue_apply(const DesiredSettings* desired,
    bool interactive, ServiceApplyOrigin origin,
    ServiceProfileSource profileSource, int profileSlot,
    GuiMutationUiContext context, const char* source,
    char* status, size_t statusSize) {
    GuiMutationWork work = {};
    work.kind = GUI_MUTATION_APPLY;
    work.context = context;
    work.desired = *desired;
    work.profileSource = profileSource;
    work.origin = origin;
    work.profileSlot = profileSlot;
    work.gpuEpoch = gui_mutation_gpu_epoch();
    work.sessionId = 0;
    ProcessIdToSessionId(GetCurrentProcessId(), &work.sessionId);
    work.queuedTickMs = GetTickCount64();
    work.notifyWindow = g_app.hMainWnd;
    if (!service_client_build_apply_request(desired, source, interactive, origin,
            profileSource, profileSlot, &work.request, status, statusSize))
        return false;
    return gui_mutation_enqueue(&work, status, statusSize);
}

static bool gui_mutation_queue_reset(char* status, size_t statusSize) {
    GuiMutationWork work = {};
    work.kind = GUI_MUTATION_RESET;
    work.context = GUI_MUTATION_CONTEXT_MANUAL_RESET;
    work.origin = SERVICE_APPLY_ORIGIN_GUI;
    work.gpuEpoch = gui_mutation_gpu_epoch();
    ProcessIdToSessionId(GetCurrentProcessId(), &work.sessionId);
    work.queuedTickMs = GetTickCount64();
    work.notifyWindow = g_app.hMainWnd;
    if (!service_client_build_reset_request(&work.request, status, statusSize))
        return false;
    return gui_mutation_enqueue(&work, status, statusSize);
}

static void gui_mutation_acknowledge_and_dispatch_next() {
    bool idle = true;
    EnterCriticalSection(&g_guiMutationLock);
    g_guiMutationActive = false;
    if (!g_guiMutationShuttingDown && g_guiMutationPending) {
        g_guiMutationActiveWork = g_guiMutationPendingWork;
        g_guiMutationPending = false;
        g_guiMutationActive = true;
        idle = false;
        SetEvent(g_guiMutationEvent);
    }
    LeaveCriticalSection(&g_guiMutationLock);
    if (idle) g_app.applyInFlight = false;
}

static void gui_mutation_shutdown() {
    if (!g_guiMutationLockReady) return;
    EnterCriticalSection(&g_guiMutationLock);
    g_guiMutationShuttingDown = true;
    g_guiMutationPending = false;
    LeaveCriticalSection(&g_guiMutationLock);
    if (g_guiMutationEvent) SetEvent(g_guiMutationEvent);
}
