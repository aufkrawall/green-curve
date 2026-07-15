// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT

// One serialized GUI service-I/O coordinator.  The worker owns SCM/pipe work
// and immutable request/response copies only.  All AppData, config, HWND and
// tray mutations happen after completions are posted to the window thread.

#include "gui_mutation_queue_policy.h"
#include "gui_service_io_queue_policy.h"

enum GuiServiceIoKind {
    GUI_SERVICE_IO_FULL_SYNC = 1,
    GUI_SERVICE_IO_TELEMETRY = 2,
    GUI_SERVICE_IO_ADMIN_TOGGLE = 3,
};

struct GuiMutationWork {
    GuiMutationKind kind;
    GuiMutationUiContext context;
    ServiceRequest request;
    DesiredSettings desired;
    ServiceProfileSource profileSource;
    ServiceApplyOrigin origin;
    int profileSlot;
    LONG gpuEpoch;
    LONG presentationEpoch;
    DWORD sessionId;
    ULONGLONG queuedTickMs;
    HWND notifyWindow;
};

struct GuiMutationCompletion {
    GuiMutationWork work;
    ServiceResponse response;
    bool transportAttempted;
    bool success;
    bool serviceInstalled;
    bool serviceRunning;
    gc_u64 connectionEpoch;
    ULONGLONG durationMs;
    char result[512];
};

struct GuiServiceIoCompletion {
    GuiServiceIoKind kind;
    ServiceResponse response;
    bool transportSuccess;
    bool redrawControls;
    bool serviceInstalled;
    bool serviceRunning;
    gc_u64 connectionEpoch;
    LONG presentationEpoch;
    ULONGLONG durationMs;
    bool adminEnable;
    bool adminRepair;
    char reason[96];
    char error[256];
};

static CRITICAL_SECTION g_guiMutationLock;
static bool g_guiMutationLockReady = false;
static HANDLE g_guiMutationEvent = nullptr;
static HANDLE g_guiMutationThread = nullptr;
static bool g_guiWorkerBusy = false;
static bool g_guiMutationActive = false;
static bool g_guiMutationDispatched = false;
static bool g_guiMutationPending = false;
static bool g_guiFullSyncPending = false;
static bool g_guiTelemetryPending = false;
static bool g_guiAdminTogglePending = false;
static bool g_guiAdminToggleEnable = false;
static bool g_guiAdminToggleRepair = false;
static char g_guiAdminToggleConfigPath[MAX_PATH] = {};
static bool g_guiTelemetryRedrawControls = false;
static bool g_guiMutationShuttingDown = false;
static GuiMutationWork g_guiMutationActiveWork = {};
static GuiMutationWork g_guiMutationPendingWork = {};
static char g_guiFullSyncReason[96] = {};
static volatile LONG g_guiMutationGpuEpoch = 1;
static volatile LONGLONG g_guiServiceConnectionEpoch = 1;
static volatile LONG g_guiServicePresentationEpoch = 1;
static gc_u64 g_guiWorkerServiceInstanceId = 0;
static HWND g_guiServiceNotifyWindow = nullptr;

static LONG gui_mutation_gpu_epoch() {
    return InterlockedExchangeAdd(&g_guiMutationGpuEpoch, 0);
}

static gc_u64 gui_service_io_connection_epoch() {
    return (gc_u64)InterlockedCompareExchange64(
        &g_guiServiceConnectionEpoch, 0, 0);
}

static LONG gui_service_presentation_epoch() {
    return InterlockedExchangeAdd(&g_guiServicePresentationEpoch, 0);
}

static void gui_service_advance_presentation_epoch(const char* reason) {
    LONG epoch = InterlockedIncrement(&g_guiServicePresentationEpoch);
    debug_log("GUI service I/O: presentation epoch advanced to %ld (%s)\n",
        epoch, reason && reason[0] ? reason : "presentation invalidated");
}

static bool gui_worker_dispatchable_locked() {
    if (g_guiMutationShuttingDown || g_guiWorkerBusy) return false;
    if (g_guiMutationActive) return !g_guiMutationDispatched;
    return g_guiAdminTogglePending || g_guiFullSyncPending ||
        g_guiTelemetryPending;
}

static void gui_worker_signal_if_dispatchable_locked() {
    if (gui_worker_dispatchable_locked() && g_guiMutationEvent)
        SetEvent(g_guiMutationEvent);
}

static void gui_mutation_advance_gpu_epoch(const char* reason) {
    LONG epoch = InterlockedIncrement(&g_guiMutationGpuEpoch);
    bool notifySuperseded = false;
    GuiMutationWork superseded = {};
    if (g_guiMutationLockReady) {
        EnterCriticalSection(&g_guiMutationLock);
        if (g_guiMutationPending) {
            superseded = g_guiMutationPendingWork;
            g_guiMutationPending = false;
            notifySuperseded = true;
        }
        g_guiFullSyncPending = true;
        g_guiTelemetryPending = false;
        g_guiTelemetryRedrawControls = false;
        StringCchCopyA(g_guiFullSyncReason,
            ARRAY_COUNT(g_guiFullSyncReason),
            reason && reason[0] ? reason : "GPU generation changed");
        gui_worker_signal_if_dispatchable_locked();
        LeaveCriticalSection(&g_guiMutationLock);
    }
    debug_log("GUI service I/O: GPU epoch advanced to %ld; pendingMutationDropped=%d (%s)\n",
        epoch, notifySuperseded ? 1 : 0,
        reason && reason[0] ? reason : "state change");
    if (notifySuperseded) {
        gui_mutation_pending_was_superseded(superseded.context,
            superseded.profileSlot, superseded.origin,
            superseded.request.operationId);
    }
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
            "Queued GPU operation rejected because the GPU generation changed");
        return false;
    }
    return true;
}

static gc_u64 gui_worker_record_connection_result(
    bool success, const ServiceResponse* response) {
    gc_u64 nextInstance = success && response
        ? response->state.serviceInstanceId : 0;
    if (nextInstance != g_guiWorkerServiceInstanceId) {
        gc_u64 previous = g_guiWorkerServiceInstanceId;
        g_guiWorkerServiceInstanceId = nextInstance;
        InterlockedIncrement64(&g_guiServiceConnectionEpoch);
        debug_log("GUI service I/O: connection epoch advanced to %llu instance=%llu (previous=%llu success=%d)\n",
            (unsigned long long)gui_service_io_connection_epoch(),
            (unsigned long long)nextInstance,
            (unsigned long long)previous, success ? 1 : 0);
    }
    return gui_service_io_connection_epoch();
}

static bool gui_worker_send_state_request(GuiServiceIoKind kind,
    ServiceResponse* response, char* err, size_t errSize) {
    ServiceRequest request = {};
    request.magic = SERVICE_PROTOCOL_MAGIC;
    request.version = SERVICE_PROTOCOL_VERSION;
    request.command = kind == GUI_SERVICE_IO_FULL_SYNC
        ? SERVICE_CMD_GET_SNAPSHOT : SERVICE_CMD_GET_TELEMETRY;
    request.callerPid = GetCurrentProcessId();
    ProcessIdToSessionId(request.callerPid, &request.callerSessionId);
    StringCchCopyA(request.source, ARRAY_COUNT(request.source),
        kind == GUI_SERVICE_IO_FULL_SYNC
            ? "GUI async full sync" : "GUI async telemetry");
    return service_send_request(&request, response,
        kind == GUI_SERVICE_IO_FULL_SYNC ? 2000 : 500, err, errSize);
}

static void gui_worker_release_after_mutation_post_failure() {
    EnterCriticalSection(&g_guiMutationLock);
    g_guiWorkerBusy = false;
    g_guiMutationActive = false;
    g_guiMutationDispatched = false;
    if (!g_guiMutationShuttingDown && g_guiMutationPending) {
        g_guiMutationActiveWork = g_guiMutationPendingWork;
        g_guiMutationPending = false;
        g_guiMutationActive = true;
    }
    g_guiFullSyncPending = true;
    StringCchCopyA(g_guiFullSyncReason, ARRAY_COUNT(g_guiFullSyncReason),
        "mutation completion post failure");
    gui_worker_signal_if_dispatchable_locked();
    LeaveCriticalSection(&g_guiMutationLock);
}

static DWORD WINAPI gui_mutation_worker_proc(void*) {
    for (;;) {
        if (WaitForSingleObject(g_guiMutationEvent, INFINITE) != WAIT_OBJECT_0)
            return 1;

        GuiMutationWork mutation = {};
        GuiServiceIoKind ioKind = GUI_SERVICE_IO_FULL_SYNC;
        bool executeMutation = false;
        bool executeRead = false;
        bool redrawControls = false;
        bool adminEnable = false;
        bool adminRepair = false;
        LONG presentationEpoch = 0;
        char reason[96] = {};
        char adminConfigPath[MAX_PATH] = {};

        EnterCriticalSection(&g_guiMutationLock);
        if (g_guiMutationShuttingDown) {
            LeaveCriticalSection(&g_guiMutationLock);
            return 0;
        }
        if (!g_guiWorkerBusy && g_guiMutationActive &&
            !g_guiMutationDispatched) {
            mutation = g_guiMutationActiveWork;
            g_guiMutationDispatched = true;
            g_guiWorkerBusy = true;
            executeMutation = true;
        } else if (!g_guiWorkerBusy && !g_guiMutationActive &&
                   g_guiAdminTogglePending) {
            g_guiAdminTogglePending = false;
            adminEnable = g_guiAdminToggleEnable;
            adminRepair = g_guiAdminToggleRepair;
            StringCchCopyA(adminConfigPath, ARRAY_COUNT(adminConfigPath),
                g_guiAdminToggleConfigPath);
            StringCchCopyA(reason, ARRAY_COUNT(reason),
                adminEnable ? "service install/repair" : "service removal");
            g_guiWorkerBusy = true;
            ioKind = GUI_SERVICE_IO_ADMIN_TOGGLE;
            executeRead = true;
            presentationEpoch = gui_service_presentation_epoch();
        } else if (!g_guiWorkerBusy && !g_guiMutationActive &&
                   g_guiFullSyncPending) {
            g_guiFullSyncPending = false;
            StringCchCopyA(reason, ARRAY_COUNT(reason),
                g_guiFullSyncReason[0] ? g_guiFullSyncReason : "full sync");
            g_guiFullSyncReason[0] = '\0';
            g_guiWorkerBusy = true;
            ioKind = GUI_SERVICE_IO_FULL_SYNC;
            executeRead = true;
            presentationEpoch = gui_service_presentation_epoch();
        } else if (!g_guiWorkerBusy && !g_guiMutationActive &&
                   g_guiTelemetryPending) {
            g_guiTelemetryPending = false;
            redrawControls = g_guiTelemetryRedrawControls;
            g_guiTelemetryRedrawControls = false;
            StringCchCopyA(reason, ARRAY_COUNT(reason), "telemetry cadence");
            g_guiWorkerBusy = true;
            ioKind = GUI_SERVICE_IO_TELEMETRY;
            executeRead = true;
            presentationEpoch = gui_service_presentation_epoch();
        }
        LeaveCriticalSection(&g_guiMutationLock);
        if (!executeMutation && !executeRead) continue;

        if (executeMutation) {
            GuiMutationCompletion* completion =
                (GuiMutationCompletion*)HeapAlloc(GetProcessHeap(),
                    HEAP_ZERO_MEMORY, sizeof(GuiMutationCompletion));
            if (!completion) {
                gui_worker_release_after_mutation_post_failure();
                continue;
            }
            completion->work = mutation;
            query_background_service_state(&completion->serviceInstalled,
                &completion->serviceRunning);
            ULONGLONG started = GetTickCount64();
            if (gui_mutation_work_context_is_current(&mutation,
                    completion->result, sizeof(completion->result))) {
                completion->transportAttempted = true;
                completion->success = service_client_execute_mutation_request(
                    &mutation.request, &completion->response,
                    completion->result, sizeof(completion->result));
            }
            bool attemptedTransport = completion->transportAttempted;
            bool haveEnvelope = completion->response.state.serviceInstanceId != 0;
            completion->connectionEpoch = attemptedTransport
                ? gui_worker_record_connection_result(haveEnvelope,
                    haveEnvelope ? &completion->response : nullptr)
                : gui_service_io_connection_epoch();
            completion->durationMs = GetTickCount64() - started;
            debug_log("GUI service I/O: mutation operation=%llu origin=%lu durationMs=%llu final=%lu success=%d epoch=%llu\n",
                (unsigned long long)mutation.request.operationId,
                (unsigned long)mutation.origin,
                (unsigned long long)completion->durationMs,
                (unsigned long)completion->response.operationState,
                completion->success ? 1 : 0,
                (unsigned long long)completion->connectionEpoch);
            EnterCriticalSection(&g_guiMutationLock);
            g_guiWorkerBusy = false;
            LeaveCriticalSection(&g_guiMutationLock);
            if (!PostMessageW(mutation.notifyWindow,
                    APP_WM_MUTATION_COMPLETE, 0, (LPARAM)completion)) {
                HeapFree(GetProcessHeap(), 0, completion);
                gui_worker_release_after_mutation_post_failure();
            }
            continue;
        }

        GuiServiceIoCompletion* completion =
            (GuiServiceIoCompletion*)HeapAlloc(GetProcessHeap(),
                HEAP_ZERO_MEMORY, sizeof(GuiServiceIoCompletion));
        if (!completion) {
            EnterCriticalSection(&g_guiMutationLock);
            g_guiWorkerBusy = false;
            gui_worker_signal_if_dispatchable_locked();
            LeaveCriticalSection(&g_guiMutationLock);
            continue;
        }
        completion->kind = ioKind;
        completion->redrawControls = redrawControls;
        completion->presentationEpoch = presentationEpoch;
        completion->adminEnable = adminEnable;
        completion->adminRepair = adminRepair;
        StringCchCopyA(completion->reason, ARRAY_COUNT(completion->reason), reason);
        ULONGLONG started = GetTickCount64();
        if (ioKind == GUI_SERVICE_IO_ADMIN_TOGGLE) {
            completion->transportSuccess = is_elevated()
                ? service_install_or_remove(adminEnable, completion->error,
                    sizeof(completion->error))
                : launch_service_admin_helper(adminEnable, adminConfigPath,
                    completion->error, sizeof(completion->error));
            query_background_service_state(&completion->serviceInstalled,
                &completion->serviceRunning);
            completion->connectionEpoch =
                gui_worker_record_connection_result(false, nullptr);
        } else {
            query_background_service_state(&completion->serviceInstalled,
                &completion->serviceRunning);
            completion->transportSuccess = gui_worker_send_state_request(ioKind,
                &completion->response, completion->error,
                sizeof(completion->error));
            completion->connectionEpoch = gui_worker_record_connection_result(
                completion->transportSuccess,
                completion->transportSuccess ? &completion->response : nullptr);
        }
        completion->durationMs = GetTickCount64() - started;
        debug_log("GUI service I/O: read kind=%d reason=%s durationMs=%llu success=%d epoch=%llu phase=%u revision=%llu error=%s\n",
            (int)ioKind, completion->reason,
            (unsigned long long)completion->durationMs,
            completion->transportSuccess ? 1 : 0,
            (unsigned long long)completion->connectionEpoch,
            completion->response.state.gpuPhase,
            (unsigned long long)completion->response.state.stateRevision,
            completion->error[0] ? completion->error : "none");

        HWND notifyWindow = g_guiServiceNotifyWindow;
        // The notify handle is captured on the main thread during coordinator
        // initialization.  The window owns and frees the completion.
        bool posted = notifyWindow && PostMessageW(notifyWindow,
            APP_WM_SERVICE_IO_COMPLETE, 0, (LPARAM)completion);
        if (!posted) HeapFree(GetProcessHeap(), 0, completion);
        EnterCriticalSection(&g_guiMutationLock);
        g_guiWorkerBusy = false;
        gui_worker_signal_if_dispatchable_locked();
        LeaveCriticalSection(&g_guiMutationLock);
    }
}

static bool gui_mutation_initialize(char* err, size_t errSize) {
    if (g_guiMutationThread) return true;
    g_guiServiceNotifyWindow = g_app.hMainWnd;
    if (!g_guiMutationLockReady) {
        InitializeCriticalSection(&g_guiMutationLock);
        g_guiMutationLockReady = true;
    }
    g_guiMutationEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!g_guiMutationEvent) {
        set_message(err, errSize,
            "Could not create the GPU worker event (error %lu)", GetLastError());
        return false;
    }
    g_guiMutationThread = CreateThread(nullptr, 0,
        gui_mutation_worker_proc, nullptr, 0, nullptr);
    if (!g_guiMutationThread) {
        DWORD error = GetLastError();
        CloseHandle(g_guiMutationEvent);
        g_guiMutationEvent = nullptr;
        set_message(err, errSize,
            "Could not create the GPU worker thread (error %lu)", error);
        return false;
    }
    return true;
}

static bool gui_service_io_queue_full_sync(const char* reason) {
    char err[192] = {};
    if (!gui_mutation_initialize(err, sizeof(err))) {
        debug_log("GUI service I/O: full sync queue failed: %s\n", err);
        return false;
    }
    EnterCriticalSection(&g_guiMutationLock);
    GuiServiceReadQueueDecision decision =
        gui_full_sync_queue_decide(g_guiFullSyncPending);
    g_guiFullSyncPending = true;
    g_guiTelemetryPending = false;
    g_guiTelemetryRedrawControls = false;
    StringCchCopyA(g_guiFullSyncReason, ARRAY_COUNT(g_guiFullSyncReason),
        reason && reason[0] ? reason : "full sync");
    gui_worker_signal_if_dispatchable_locked();
    LeaveCriticalSection(&g_guiMutationLock);
    debug_log_on_change("GUI service I/O: full sync %s (%s)\n",
        decision == GUI_SERVICE_READ_COALESCE ? "coalesced" : "pending",
        reason && reason[0] ? reason : "full sync");
    return true;
}

static bool gui_service_io_queue_admin_toggle(bool enable, bool repair,
    const char* configPath, char* status, size_t statusSize) {
    if (!configPath || !configPath[0]) {
        set_message(status, statusSize,
            "Cannot change the background service without a configuration path");
        return false;
    }
    if (!gui_mutation_initialize(status, statusSize)) return false;
    service_admin_reset_wait_cancel();
    EnterCriticalSection(&g_guiMutationLock);
    if (g_guiMutationActive || g_guiAdminTogglePending) {
        LeaveCriticalSection(&g_guiMutationLock);
        set_message(status, statusSize, g_guiMutationActive
            ? "Wait for the active GPU write before changing the service"
            : "A background service change is already pending");
        return false;
    }
    g_guiAdminTogglePending = true;
    g_guiAdminToggleEnable = enable;
    g_guiAdminToggleRepair = repair;
    StringCchCopyA(g_guiAdminToggleConfigPath,
        ARRAY_COUNT(g_guiAdminToggleConfigPath), configPath);
    g_guiTelemetryPending = false;
    g_guiTelemetryRedrawControls = false;
    gui_worker_signal_if_dispatchable_locked();
    LeaveCriticalSection(&g_guiMutationLock);
    set_message(status, statusSize, enable
        ? (repair ? "Repairing the background service..."
                  : "Installing the background service...")
        : "Removing the background service...");
    debug_log("GUI service I/O: admin change queued enable=%d repair=%d\n",
        enable ? 1 : 0, repair ? 1 : 0);
    return true;
}

static bool gui_service_io_queue_telemetry(bool redrawControls) {
    char err[192] = {};
    if (!gui_mutation_initialize(err, sizeof(err))) {
        debug_log("GUI service I/O: telemetry queue failed: %s\n", err);
        return false;
    }
    EnterCriticalSection(&g_guiMutationLock);
    GuiServiceReadQueueDecision decision = gui_telemetry_queue_decide(
        g_guiFullSyncPending, g_guiMutationActive, g_guiAdminTogglePending,
        g_guiWorkerBusy,
        g_guiTelemetryPending);
    if (decision == GUI_SERVICE_READ_DROP_BEHIND_PRIORITY_WORK) {
        debug_log_on_change("GUI service I/O: telemetry dropped behind write/full sync (activeMutation=%d fullSync=%d busy=%d)\n",
            g_guiMutationActive ? 1 : 0,
            g_guiFullSyncPending ? 1 : 0,
            g_guiWorkerBusy ? 1 : 0);
        LeaveCriticalSection(&g_guiMutationLock);
        return true;
    }
    g_guiTelemetryPending = true;
    g_guiTelemetryRedrawControls =
        g_guiTelemetryRedrawControls || redrawControls;
    gui_worker_signal_if_dispatchable_locked();
    LeaveCriticalSection(&g_guiMutationLock);
    if (decision == GUI_SERVICE_READ_COALESCE)
        debug_log_on_change("GUI service I/O: telemetry request coalesced\n");
    return true;
}

static bool gui_mutation_enqueue(const GuiMutationWork* work,
    char* status, size_t statusSize) {
    if (!work || !work->notifyWindow) return false;
    if (!gui_mutation_initialize(status, statusSize)) return false;

    bool notifySuperseded = false;
    GuiMutationWork superseded = {};
    EnterCriticalSection(&g_guiMutationLock);
    g_guiTelemetryPending = false;
    g_guiTelemetryRedrawControls = false;
    GuiMutationQueueDecision decision = gui_mutation_queue_decide(
        g_guiMutationActive, g_guiMutationPending,
        g_guiMutationPending ? g_guiMutationPendingWork.kind :
            GUI_MUTATION_APPLY,
        work->kind);
    if (decision == GUI_MUTATION_QUEUE_START) {
        g_guiMutationActiveWork = *work;
        g_guiMutationActive = true;
        g_guiMutationDispatched = false;
        gui_worker_signal_if_dispatchable_locked();
        set_message(status, statusSize,
            "GPU operation started in the background");
    } else if (decision == GUI_MUTATION_QUEUE_KEEP_PENDING_RESET) {
        set_message(status, statusSize,
            "Reset is already pending and takes precedence over this Apply");
        LeaveCriticalSection(&g_guiMutationLock);
        return false;
    } else {
        if (decision == GUI_MUTATION_QUEUE_REPLACE_PENDING) {
            superseded = g_guiMutationPendingWork;
            notifySuperseded = true;
            debug_log("GUI service I/O: superseded pending operation=%llu with operation=%llu kind=%d\n",
                (unsigned long long)g_guiMutationPendingWork.request.operationId,
                (unsigned long long)work->request.operationId,
                (int)work->kind);
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

static bool gui_mutation_stamp_request(ServiceRequest* request,
    char* status, size_t statusSize) {
    const GuiServiceModel& model = g_app.guiServiceModel;
    if (!gui_service_model_ready(&model) || !model.serviceInstanceId ||
        !model.gpuGeneration || !model.topologySignature ||
        !g_app.guiDraft.attached || g_app.guiDraft.detached) {
        set_message(status, statusSize,
            "GPU state is reconnecting or the preserved draft no longer matches this GPU; refresh before applying");
        return false;
    }
    request->expectedServiceInstanceId = model.serviceInstanceId;
    request->expectedGpuGeneration = model.gpuGeneration;
    request->expectedTopologySignature = model.topologySignature;
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
    work.presentationEpoch = gui_service_presentation_epoch();
    ProcessIdToSessionId(GetCurrentProcessId(), &work.sessionId);
    work.queuedTickMs = GetTickCount64();
    work.notifyWindow = g_app.hMainWnd;
    if (!service_client_build_apply_request(desired, source, interactive,
            origin, profileSource, profileSlot, &work.request,
            status, statusSize) ||
        !gui_mutation_stamp_request(&work.request, status, statusSize))
        return false;
    return gui_mutation_enqueue(&work, status, statusSize);
}

static bool gui_mutation_queue_reset(char* status, size_t statusSize) {
    GuiMutationWork work = {};
    work.kind = GUI_MUTATION_RESET;
    work.context = GUI_MUTATION_CONTEXT_MANUAL_RESET;
    work.origin = SERVICE_APPLY_ORIGIN_GUI;
    work.gpuEpoch = gui_mutation_gpu_epoch();
    work.presentationEpoch = gui_service_presentation_epoch();
    ProcessIdToSessionId(GetCurrentProcessId(), &work.sessionId);
    work.queuedTickMs = GetTickCount64();
    work.notifyWindow = g_app.hMainWnd;
    if (!service_client_build_reset_request(&work.request,
            status, statusSize) ||
        !gui_mutation_stamp_request(&work.request, status, statusSize))
        return false;
    return gui_mutation_enqueue(&work, status, statusSize);
}

static void gui_mutation_acknowledge_and_dispatch_next() {
    bool idle = true;
    EnterCriticalSection(&g_guiMutationLock);
    g_guiMutationActive = false;
    g_guiMutationDispatched = false;
    if (!g_guiMutationShuttingDown && g_guiMutationPending) {
        g_guiMutationActiveWork = g_guiMutationPendingWork;
        g_guiMutationPending = false;
        g_guiMutationActive = true;
        idle = false;
    }
    // A mutation result is followed by one full snapshot even when the result
    // itself was an error; this reconciles service-owned recovery/profile state.
    g_guiFullSyncPending = true;
    StringCchCopyA(g_guiFullSyncReason, ARRAY_COUNT(g_guiFullSyncReason),
        "post-mutation reconciliation");
    gui_worker_signal_if_dispatchable_locked();
    LeaveCriticalSection(&g_guiMutationLock);
    if (idle) g_app.applyInFlight = false;
}

static bool gui_mutation_shutdown() {
    if (!g_guiMutationLockReady) return true;
    EnterCriticalSection(&g_guiMutationLock);
    g_guiMutationShuttingDown = true;
    g_guiServiceNotifyWindow = nullptr;
    g_guiMutationPending = false;
    g_guiAdminTogglePending = false;
    g_guiFullSyncPending = false;
    g_guiTelemetryPending = false;
    LeaveCriticalSection(&g_guiMutationLock);
    service_admin_request_wait_cancel();
    if (g_guiMutationEvent) SetEvent(g_guiMutationEvent);
    if (!g_guiMutationThread) return true;
    CancelSynchronousIo(g_guiMutationThread);
    DWORD waitResult = WaitForSingleObject(g_guiMutationThread, 2500);
    if (waitResult != WAIT_OBJECT_0) {
        debug_log("GUI service I/O: coordinator did not stop within terminal shutdown grace (wait=%lu); process-lifetime synchronization objects will remain valid\n",
            (unsigned long)waitResult);
        return false;
    }
    CloseHandle(g_guiMutationThread);
    g_guiMutationThread = nullptr;
    if (g_guiMutationEvent) {
        CloseHandle(g_guiMutationEvent);
        g_guiMutationEvent = nullptr;
    }
    debug_log("GUI service I/O: coordinator stopped cleanly\n");
    return true;
}
