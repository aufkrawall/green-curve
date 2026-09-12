// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT

// The Updates dialog's service commands, off the GUI message thread.
//
// ## Why this shard exists
//
// Every update command is a local named-pipe round trip dispatched under the
// service's ONE serialized dispatch lock, so its turnaround carries whatever
// command is already running -- a full GET_SNAPSHOT measured at p90 1140 ms and
// a 1968 ms maximum on real hardware. The dialog used to make those calls
// synchronously from its window procedure (a `WM_TIMER` poll every 700 ms plus
// every button click) with a 5000 ms deadline, on the thread that pumps the
// main window. The whole UI stopped repainting for the duration.
//
// That is a lose-lose the deadline cannot fix: short enough not to hang gives
// spurious failures on a healthy service, long enough to be correct gives a
// visible freeze. The only fix is to not block that thread.
//
// ## The shape
//
// One command at a time (the buttons that start them are disabled while one is
// in flight, and the interlocked claim below makes that an invariant rather
// than a UI convention). The thread is DETACHED on purpose: nothing joins it,
// it owns only the heap it allocated, and it delivers its result by posting to
// the MAIN window -- which outlives every dialog, so a completion can never
// arrive at a destroyed or recycled dialog HWND. Closing the dialog mid-command
// is therefore safe and needs no wait.
//
// The read-only GET_UPDATE_STATE poll this replaced is gone entirely rather
// than moved: the service stamps ServiceUpdateState onto EVERY response, and
// gui_update_note_response() already refreshes the shared cache from inside the
// transport, so the coordinator's once-per-second telemetry read keeps that
// cache current for free. Polling for it was a second code path for data that
// was already arriving.

struct GuiUpdateCommandWork {
    ServiceCommand command;
    gc_u32 autoCheck;
    gc_u32 intervalSeconds;
    // Whether a failure is worth a message box. False for the commands the user
    // did not explicitly ask for (the first-run preference, the cold-cache
    // fetch), where a box would be noise on top of a question they did not ask.
    bool reportFailure;
    // An INSTALL captures the applied settings BEFORE asking, so a refused
    // install has to discard that capture again. Carried here because the
    // discard now happens on the completion, not at the call site.
    bool settingsCaptured;
};

struct GuiUpdateCommandCompletion {
    ServiceCommand command;
    bool reportFailure;
    bool settingsCaptured;
    bool success;
    ULONGLONG durationMs;
    char err[256];
};

// Exactly one command in flight. Claimed before the thread starts and released
// on the message thread when the completion is handled (or on the worker if the
// completion could not be delivered at all).
static volatile LONG g_guiUpdateCommandInFlight = 0;

static bool gui_update_command_active() {
    return InterlockedCompareExchange(&g_guiUpdateCommandInFlight, 0, 0) != 0;
}

static void gui_update_command_release() {
    InterlockedExchange(&g_guiUpdateCommandInFlight, 0);
}

// The shared sender.  `autoCheck` and `intervalSeconds` are meaningful only for
// SERVICE_CMD_SET_UPDATE_POLICY and are sent only for it -- the service's
// validator refuses them on the other three, so passing them by accident fails
// loudly rather than being quietly ignored.
//
// Blocking, and called only from the worker thread below.
static bool gui_update_send(ServiceCommand command, gc_u32 autoCheck,
                            gc_u32 intervalSeconds, char* err, size_t errSize) {
    ServiceRequest request = {};
    request.magic = SERVICE_PROTOCOL_MAGIC;
    request.version = SERVICE_PROTOCOL_VERSION;
    request.command = (gc_u32)command;
    request.callerPid = GetCurrentProcessId();
    ProcessIdToSessionId(request.callerPid, &request.callerSessionId);
    if (command == SERVICE_CMD_SET_UPDATE_POLICY) {
        request.updateAutoCheck = autoCheck;
        request.updateIntervalSeconds = intervalSeconds;
    }
    StringCchCopyA(request.source, ARRAY_COUNT(request.source), "gui update");

    ServiceResponse response = {};
    // Off the message thread, so there is no UI-stall reason to expire while a
    // legitimate APPLY/RESET already owns the service's one dispatch lock. The
    // health-probe budget covers the update command's own queue/framing work;
    // layer the mutation ceiling in front of it so an INSTALL cannot report a
    // transport failure, discard its captured restore state, and then execute
    // later when the service finally reaches the already-accepted request.
    DWORD responseTimeoutMs = SERVICE_APPLY_CLIENT_TIMEOUT_MS +
        (DWORD)service_health_probe_response_timeout_ms();
    if (!service_send_request_split(&request, &response,
            responseTimeoutMs, err, errSize)) {
        debug_log("gui update: command %u transport failed: %s\n",
                  (unsigned)command, err && err[0] ? err : "unknown");
        return false;
    }
    if (response.status != SERVICE_STATUS_OK) {
        set_message(err, errSize, "%s",
                    response.message[0] ? response.message : "Update request failed");
        return false;
    }
    return true;
}

// Defined in gui_update_dialog.cpp, which is compiled after this shard.
static void gui_update_dialog_apply_command_completion(
    const GuiUpdateCommandCompletion* completion);

static DWORD WINAPI gui_update_command_thread_proc(void* param) {
    GuiUpdateCommandWork work = {};
    if (param) {
        work = *(GuiUpdateCommandWork*)param;
        HeapFree(GetProcessHeap(), 0, param);
    }

    GuiUpdateCommandCompletion* completion =
        (GuiUpdateCommandCompletion*)HeapAlloc(GetProcessHeap(),
            HEAP_ZERO_MEMORY, sizeof(GuiUpdateCommandCompletion));
    if (!completion) {
        debug_log("gui update: command %u dropped; completion allocation failed\n",
                  (unsigned int)work.command);
        gui_update_command_release();
        return 1;
    }
    completion->command = work.command;
    completion->reportFailure = work.reportFailure;
    completion->settingsCaptured = work.settingsCaptured;

    ULONGLONG started = GetTickCount64();
    completion->success = gui_update_send(work.command, work.autoCheck,
        work.intervalSeconds, completion->err, sizeof(completion->err));
    completion->durationMs = GetTickCount64() - started;
    debug_log("gui update: command %u finished off-thread durationMs=%llu success=%d error=%s\n",
              (unsigned int)work.command,
              (unsigned long long)completion->durationMs,
              completion->success ? 1 : 0,
              completion->err[0] ? completion->err : "none");

    // The main window, never the dialog: it outlives every dialog, so this
    // cannot land on a destroyed or recycled HWND.
    HWND target = g_app.hMainWnd;
    if (!target || !PostMessageA(target, APP_WM_UPDATE_COMMAND_COMPLETE, 0,
                                 (LPARAM)completion)) {
        debug_log("gui update: command %u completion could not be posted; dropping it\n",
                  (unsigned int)work.command);
        HeapFree(GetProcessHeap(), 0, completion);
        gui_update_command_release();
    }
    return 0;
}

// Called on the message thread. Returns false when nothing was started, in
// which case the caller's optimistic UI state must be put back.
static bool gui_update_command_begin(ServiceCommand command, gc_u32 autoCheck,
                                     gc_u32 intervalSeconds, bool reportFailure,
                                     bool settingsCaptured) {
    if (InterlockedCompareExchange(&g_guiUpdateCommandInFlight, 1, 0) != 0) {
        debug_log("gui update: refused command %u; another is already in flight\n",
                  (unsigned int)command);
        return false;
    }
    GuiUpdateCommandWork* work = (GuiUpdateCommandWork*)HeapAlloc(
        GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(GuiUpdateCommandWork));
    if (!work) {
        gui_update_command_release();
        debug_log("gui update: command %u not started; work allocation failed\n",
                  (unsigned int)command);
        return false;
    }
    work->command = command;
    work->autoCheck = autoCheck;
    work->intervalSeconds = intervalSeconds;
    work->reportFailure = reportFailure;
    work->settingsCaptured = settingsCaptured;

    // Default stack reservation: gui_update_send() puts a whole ServiceRequest
    // and ServiceResponse on the stack, and this thread is rare and short-lived,
    // so there is nothing to gain from trimming it.
    HANDLE thread = CreateThread(nullptr, 0, gui_update_command_thread_proc,
                                 work, 0, nullptr);
    if (!thread) {
        DWORD error = GetLastError();
        HeapFree(GetProcessHeap(), 0, work);
        gui_update_command_release();
        debug_log("gui update: could not start the command thread for %u (error %lu)\n",
                  (unsigned int)command, (unsigned long)error);
        return false;
    }
    CloseHandle(thread);
    debug_log("gui update: command %u dispatched off the message thread\n",
              (unsigned int)command);
    return true;
}

// The main window's APP_WM_UPDATE_COMMAND_COMPLETE handler. Owns and frees the
// posted completion, and releases the in-flight claim exactly once.
void gui_update_notify_command_complete(void* completionPtr) {
    GuiUpdateCommandCompletion* completion =
        (GuiUpdateCommandCompletion*)completionPtr;
    if (!completion) return;
    // Released BEFORE the presentation runs, so the refresh below sees the
    // buttons as idle again.
    gui_update_command_release();
    gui_update_dialog_apply_command_completion(completion);
    HeapFree(GetProcessHeap(), 0, completion);
}