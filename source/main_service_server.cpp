static bool service_path_has_reparse_component(const char* absPath, char* err, size_t errSize) {
    if (!absPath || !absPath[0]) return false;
    char probe[MAX_PATH] = {};
    if (FAILED(StringCchCopyA(probe, ARRAY_COUNT(probe), absPath))) {
        set_message(err, errSize, "Path is too long");
        return true;
    }

    size_t len = strlen(probe);
    size_t rootLen = 0;
    if (len >= 3 && probe[1] == ':' && (probe[2] == '\\' || probe[2] == '/')) {
        rootLen = 3;
    } else if (len >= 2 && probe[0] == '\\' && probe[1] == '\\') {
        const char* serverEnd = strpbrk(probe + 2, "\\/");
        if (serverEnd) {
            const char* shareEnd = strpbrk(serverEnd + 1, "\\/");
            if (shareEnd) rootLen = (size_t)(shareEnd - probe + 1);
        }
    }

    if (rootLen == 0 || rootLen >= len) return false;
    for (size_t i = rootLen; i < len; i++) {
        if (probe[i] != '\\' && probe[i] != '/') continue;
        char saved = probe[i];
        probe[i] = 0;
        DWORD attrs = GetFileAttributesA(probe);
        probe[i] = saved;
        if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_REPARSE_POINT)) {
            set_message(err, errSize, "Path crosses a reparse point");
            return true;
        }
    }

    char* slash = strrchr(probe, '\\');
    char* slashAlt = strrchr(probe, '/');
    if (!slash || (slashAlt && slashAlt > slash)) slash = slashAlt;
    if (slash && (size_t)(slash - probe) >= rootLen) {
        *slash = 0;
        DWORD attrs = GetFileAttributesA(probe);
        if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_REPARSE_POINT)) {
            set_message(err, errSize, "Path crosses a reparse point");
            return true;
        }
    }
    return false;
}

static bool service_validate_file_write_path(const char* path, char* err, size_t errSize) {
    if (!path || !path[0]) {
        set_message(err, errSize, "Empty path");
        return false;
    }
    if (strstr(path, "..")) {
        set_message(err, errSize, "Path contains parent directory references");
        return false;
    }
    if (strchr(path, '*') || strchr(path, '?')) {
        set_message(err, errSize, "Path contains wildcard characters");
        return false;
    }
    int colonCount = 0;
    for (const char* p = path; *p; ++p) {
        if (*p == ':') colonCount++;
    }
    if (colonCount > 1) {
        set_message(err, errSize, "Path contains invalid colon characters");
        return false;
    }
    char absPath[MAX_PATH] = {};
    DWORD len = GetFullPathNameA(path, ARRAY_COUNT(absPath), absPath, nullptr);
    if (len == 0 || len >= ARRAY_COUNT(absPath)) {
        set_message(err, errSize, "Invalid path");
        return false;
    }
    if (!g_serviceUserPathsResolved || !g_serviceUserProfileDir[0]) {
        set_message(err, errSize, "User paths not resolved");
        return false;
    }
    size_t profileLen = strlen(g_serviceUserProfileDir);
    if (_strnicmp(absPath, g_serviceUserProfileDir, profileLen) != 0 ||
        (absPath[profileLen] != '\\' && absPath[profileLen] != '\0')) {
        set_message(err, errSize, "Path is outside the caller's profile directory");
        return false;
    }
    if (service_path_has_reparse_component(absPath, err, errSize)) {
        return false;
    }
    return true;
}

static bool service_verify_written_file_path(const char* path, char* err, size_t errSize) {
    if (!path || !path[0]) {
        set_message(err, errSize, "Empty path");
        return false;
    }
    if (!g_serviceUserPathsResolved || !g_serviceUserProfileDir[0]) {
        set_message(err, errSize, "User paths not resolved");
        return false;
    }
    HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        // If we cannot open the file for verification, fail closed.
        set_message(err, errSize, "Cannot verify written file path");
        return false;
    }
    char finalPath[MAX_PATH] = {};
    DWORD len = GetFinalPathNameByHandleA(h, finalPath, ARRAY_COUNT(finalPath), FILE_NAME_NORMALIZED);
    CloseHandle(h);
    if (len == 0 || len >= ARRAY_COUNT(finalPath)) {
        set_message(err, errSize, "Cannot resolve written file path");
        return false;
    }
    // GetFinalPathNameByHandleA may return a \\?\ prefix.
    const char* comparePath = finalPath;
    if (strncmp(finalPath, "\\\\?\\", 4) == 0) comparePath = finalPath + 4;
    size_t profileLen = strlen(g_serviceUserProfileDir);
    if (_strnicmp(comparePath, g_serviceUserProfileDir, profileLen) != 0 ||
        (comparePath[profileLen] != '\\' && comparePath[profileLen] != '\0')) {
        set_message(err, errSize, "Written file resolved outside the caller's profile directory");
        return false;
    }
    return true;
}

static bool service_prepare_requested_gpu(const ServiceRequest* request, char* err, size_t errSize) {
    if (!request || !request->targetGpu.valid) return true;
    if (request->targetGpu.nvapiIndex >= MAX_GPU_ADAPTERS) {
        set_message(err, errSize, "Requested GPU index is invalid");
        return false;
    }

    char initDetail[256] = {};
    if (!hardware_initialize(initDetail, sizeof(initDetail))) {
        set_message(err, errSize, "%s", initDetail[0] ? initDetail : "Hardware initialization failed");
        return false;
    }
    if (request->targetGpu.nvapiIndex >= g_app.adapterCount) {
        set_message(err, errSize, "Requested GPU is no longer available");
        debug_log("service gpu target: rejected nvapi=%u outside live adapter count %u\n",
            request->targetGpu.nvapiIndex, g_app.adapterCount);
        return false;
    }

    GpuAdapterInfo live = g_app.adapters[request->targetGpu.nvapiIndex];
    if (!live.valid) {
        set_message(err, errSize, "Requested GPU is not valid");
        return false;
    }
    bool haveStrongIdentity = request->targetGpu.pciInfoValid && live.pciInfoValid;
    if (haveStrongIdentity && !gpu_adapter_has_same_pci_identity(&live, &request->targetGpu)) {
        set_message(err, errSize, "Requested GPU identity no longer matches");
        debug_log("service gpu target: rejected nvapi=%u because PCI identity changed\n",
            request->targetGpu.nvapiIndex);
        return false;
    }
    if (!haveStrongIdentity && g_app.adapterCount > 1) {
        set_message(err, errSize, "Requested GPU identity is ambiguous");
        debug_log("service gpu target: rejected nvapi=%u without PCI identity on multi-adapter system\n",
            request->targetGpu.nvapiIndex);
        return false;
    }

    bool change = !g_app.selectedGpuIdentityValid ||
        g_app.selectedGpuIndex != live.nvapiIndex ||
        (haveStrongIdentity && !gpu_adapter_has_same_pci_identity(&g_app.selectedGpu, &live));
    if (change) {
        debug_log("service gpu target: selecting validated nvapi=%u nvml=%u identity=%s name=%s\n",
            live.nvapiIndex,
            live.nvmlIndex,
            haveStrongIdentity ? "pci" : "single-adapter-ordinal",
            live.name[0] ? live.name : "<unnamed>");
        reset_gpu_runtime_selection();
        g_app.selectedGpuIndex = live.nvapiIndex;
        g_app.selectedNvmlIndex = live.nvmlIndex;
        g_app.selectedGpuExplicit = true;
        g_app.selectedGpu = live;
        g_app.selectedGpuIdentityValid = live.valid;
        g_app.selectedGpuOrdinalFallback = !haveStrongIdentity;
    }
    return true;
}

static bool create_restricted_pipe_security_descriptor(PSECURITY_DESCRIPTOR* outSd) {
    *outSd = nullptr;
    // SYSTEM (full), Administrators (full), Authenticated Users (read+write).
    //
    // F-SEC-3 ("only the active interactive session may drive GPU OC/RESET") is
    // enforced SERVER-SIDE by service_caller_is_authorized(): every request's
    // caller session is resolved from the pipe handle (GetNamedPipeClientProcessId
    // → ProcessIdToSessionId — unspoofable) and rejected unless it equals the
    // active interactive session.  The pipe ACL therefore only needs to admit
    // authenticated LOCAL users (PIPE_REJECT_REMOTE_CLIENTS blocks the network),
    // and must NOT bake a specific console-user SID into the ACL:
    //
    //   Root cause of "service not responding after switching accounts" — a
    //   per-user ACL grants write to whoever was the console user when the
    //   listening instance was created.  After a logoff/login (or fast-user-
    //   switch) the now-active user is a *different* SID, so their GUI is denied
    //   at CreateFile and the server's ConnectNamedPipe never completes — the
    //   stale-ACL instance stays listening and keeps rejecting the new user
    //   until the service is restarted (reboot).  A user-agnostic ACL lets the
    //   new active user connect immediately; the server-side check still rejects
    //   any non-active session with a clear message.
    const WCHAR* sddl = L"D:(A;;GA;;;SY)(A;;GA;;;BA)(A;;GRGW;;;AU)";
    if (ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl, SDDL_REVISION_1, outSd, nullptr) == FALSE) {
        debug_log("pipe_server: failed building pipe security descriptor (error %lu)\n", GetLastError());
        *outSd = nullptr;
        return false;
    }
    return true;
}

// Close a pipe handle owned by the pipe server thread, atomically clearing the
// shared g_servicePipeHandle slot first so the main-loop watchdog cannot also
// close it.  A double-close raises STATUS_INVALID_HANDLE (c0000008) under the
// Strict Handle Check mitigation, which is NOT an access violation so the VEH
// does not catch it — it reaches the unhandled filter and terminates the whole
// service process (GUI then sees ERROR_BROKEN_PIPE / error 109).  Exactly one
// of the pipe thread (here) or the watchdog wins the slot via the atomic CAS
// and performs the single close.  The pipe thread can only be VEH-terminated
// while executing NVML/NVAPI inside a command handler — never inside this
// function — so there is no leak window between the CAS and CloseHandle.
static void service_close_owned_pipe(HANDLE pipe) {
    if (pipe == nullptr || pipe == INVALID_HANDLE_VALUE) return;
    HANDLE prev = (HANDLE)InterlockedCompareExchangePointer(
        (PVOID volatile*)&g_servicePipeHandle, INVALID_HANDLE_VALUE, pipe);
    if (prev == pipe) {
        CloseHandle(pipe);
    }
    // else: the watchdog already reclaimed (and will close) this handle.
}

static DWORD WINAPI service_pipe_server_thread_proc(void*) {
    WCHAR pipeName[128] = {};
    if (!background_service_pipe_name(pipeName, ARRAY_COUNT(pipeName))) return 1;

    while (!g_serviceStopEvent || WaitForSingleObject(g_serviceStopEvent, 0) != WAIT_OBJECT_0) {
        PSECURITY_DESCRIPTOR securityDescriptor = nullptr;
        SECURITY_ATTRIBUTES sa = {};
        sa.nLength = sizeof(sa);
        if (create_restricted_pipe_security_descriptor(&securityDescriptor)) {
            sa.lpSecurityDescriptor = securityDescriptor;
        } else {
            debug_log("pipe_server: cannot create restricted ACL, deferring pipe creation\n");
            if (securityDescriptor) {
                LocalFree(securityDescriptor);
                securityDescriptor = nullptr;
            }
            Sleep(250);
            continue;
        }
        if (!securityDescriptor) {
            debug_log("pipe_server: restricted ACL creation returned no descriptor, deferring pipe creation\n");
            Sleep(250);
            continue;
        }

        HANDLE pipe = CreateNamedPipeW(
            pipeName,
            PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
            PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
            4,
            sizeof(ServiceResponse),
            sizeof(ServiceRequest),
            1000,
            sa.lpSecurityDescriptor ? &sa : nullptr);
        if (securityDescriptor) {
            LocalFree(securityDescriptor);
            securityDescriptor = nullptr;
        }
        if (pipe == INVALID_HANDLE_VALUE) {
            Sleep(250);
            continue;
        }
        // Publish the pipe handle so the main-loop watchdog can reclaim it if
        // the VEH terminates this thread inside a command handler.  The slot
        // is INVALID here (every close path clears it via service_close_owned_pipe
        // / the watchdog), so this is a clean publish — we must NOT close the
        // previous value (it was the just-freed handle from the prior iteration;
        // closing it double-closes and the Strict Handle Check mitigation turns
        // that into a process-killing STATUS_INVALID_HANDLE).
        InterlockedExchangePointer((PVOID volatile*)&g_servicePipeHandle, pipe);

        OVERLAPPED ov = {};
        ov.hEvent = CreateEventA(nullptr, TRUE, FALSE, nullptr);
        if (!ov.hEvent) {
            service_close_owned_pipe(pipe);
            continue;
        }
        BOOL connected = ConnectNamedPipe(pipe, &ov);
        DWORD connectErr = connected ? ERROR_SUCCESS : GetLastError();
        if (!connected && connectErr == ERROR_IO_PENDING) {
            // Build the wait set from the events that actually exist.  Index 0
            // is always the stop event (breaks the loop).  The pipe-wake and
            // pipe-recycle events recycle the current instance (close + create a
            // fresh one).  The recycle event is signaled by
            // service_handle_session_change() on an active-user change so the
            // next connect gets a clean instance (the ACL itself is user-agnostic;
            // active-session enforcement is server-side in
            // service_caller_is_authorized).
            HANDLE waitHandles[4] = {};
            DWORD waitCount = 0;
            DWORD stopIdx = (DWORD)-1, wakeIdx = (DWORD)-1, recycleIdx = (DWORD)-1;
            if (g_serviceStopEvent) { stopIdx = waitCount; waitHandles[waitCount++] = g_serviceStopEvent; }
            if (g_servicePipeWakeEvent) { wakeIdx = waitCount; waitHandles[waitCount++] = g_servicePipeWakeEvent; }
            if (g_servicePipeRecycleEvent) { recycleIdx = waitCount; waitHandles[waitCount++] = g_servicePipeRecycleEvent; }
            waitHandles[waitCount++] = ov.hEvent; // always last
            DWORD ovIdx = waitCount - 1;
            DWORD waitResult = WaitForMultipleObjects(waitCount, waitHandles, FALSE, INFINITE);
            if (stopIdx != (DWORD)-1 && waitResult == WAIT_OBJECT_0 + stopIdx) {
                CancelIoEx(pipe, &ov);
                CloseHandle(ov.hEvent);
                DisconnectNamedPipe(pipe);
                service_close_owned_pipe(pipe);
                break;
            }
            auto recycle_current_pipe = [&]() {
                CancelIoEx(pipe, &ov);
                CloseHandle(ov.hEvent);
                DisconnectNamedPipe(pipe);
                service_close_owned_pipe(pipe);
            };
            if (wakeIdx != (DWORD)-1 && waitResult == WAIT_OBJECT_0 + wakeIdx) {
                recycle_current_pipe();
                continue;
            }
            if (recycleIdx != (DWORD)-1 && waitResult == WAIT_OBJECT_0 + recycleIdx) {
                debug_log("pipe_server: recycling instance for ACL rebuild after session change\n");
                recycle_current_pipe();
                continue;
            }
            connected = waitResult == WAIT_OBJECT_0 + ovIdx;
        } else if (!connected && connectErr == ERROR_PIPE_CONNECTED) {
            connected = TRUE;
        }
        CloseHandle(ov.hEvent);
        if (!connected) {
            DisconnectNamedPipe(pipe);
            service_close_owned_pipe(pipe);
            continue;
        }

        ServiceRequest request = {};
        ServiceResponse response = {};
        response.magic = SERVICE_PROTOCOL_MAGIC;
        response.version = SERVICE_PROTOCOL_VERSION;
        response.serviceBuildNumber = (DWORD)APP_BUILD_NUMBER;
        StringCchCopyA(response.serviceVersion, ARRAY_COUNT(response.serviceVersion), APP_VERSION);
        char callerUser[256] = {};
        DWORD callerSessionId = (DWORD)-1;
        DWORD callerPid = 0;
        bool callerIsAdmin = false;
        char pipeErr[256] = {};
        if (!service_pipe_read_exact(pipe, &request, sizeof(request), SERVICE_PIPE_SERVER_IO_TIMEOUT_MS, "reading service request", pipeErr, sizeof(pipeErr))) {
            debug_log("service_pipe_server: dropping stalled or invalid client read: %s\n", pipeErr[0] ? pipeErr : "unknown");
            DisconnectNamedPipe(pipe);
            service_close_owned_pipe(pipe);
            continue;
        }
        request.source[ARRAY_COUNT(request.source) - 1] = '\0';
        request.path[ARRAY_COUNT(request.path) - 1] = '\0';

        if (request.magic != SERVICE_PROTOCOL_MAGIC || request.version != SERVICE_PROTOCOL_VERSION) {
            response.status = SERVICE_STATUS_VERSION_MISMATCH;
            StringCchCopyA(response.message, ARRAY_COUNT(response.message), "Service protocol mismatch");
        } else {
            validate_gpu_adapter_info_for_ipc(&request.targetGpu);
            validate_desired_settings_for_ipc(&request.desired);
            if (!service_caller_is_authorized(pipe, request.source, response.message, ARRAY_COUNT(response.message), callerUser, sizeof(callerUser), &callerSessionId, &callerPid, &callerIsAdmin)) {
                response.status = SERVICE_STATUS_ERROR;
            } else {
                char userPathErr[256] = {};
                if (resolve_service_user_data_paths(callerSessionId, userPathErr, sizeof(userPathErr))) {
                    if (!g_app.configPath[0]) {
                        set_default_config_path();
                    }
                    refresh_service_debug_logging_from_config();
                } else {
                    debug_log("service_pipe_server: failed to resolve user data paths: %s\n", userPathErr);
                }
            service_set_pending_operation_source(request.source[0] ? request.source : "service request");
            switch (request.command) {
                case SERVICE_CMD_PING:
                    response.status = SERVICE_STATUS_OK;
                    StringCchCopyA(response.message, ARRAY_COUNT(response.message), "pong");
                    break;
                case SERVICE_CMD_GET_SNAPSHOT: {
                    char detail[256] = {};
                    // Use a timed lock so the pipe server does not block
                    // indefinitely when the runtime lock is held by the recovery
                    // reapply thread.  If the lock is busy, serve cached data
                    // immediately so PING and other commands are not starved on
                    // the single-instance pipe.
                    bool lockAcquired = try_lock_service_runtime(250);
                    if (!lockAcquired) {
                        debug_log("service snapshot: runtime lock busy (recovery reapply in progress), serving cached globals\n");
                        response.status = SERVICE_STATUS_OK;
                        StringCchCopyA(response.message, ARRAY_COUNT(response.message), "snapshot cached");
                        populate_service_snapshot(&response.snapshot);
                        if (g_serviceControlStateValid) response.controlState = g_serviceControlState;
                        break;
                    }
                    bool ok = hardware_initialize(detail, sizeof(detail));
                    if (!ok) {
                        debug_log("service snapshot: hardware initialize unavailable: %s\n", detail[0] ? detail : "unknown");
                    } else {
                        bool offsetsOk = false;
                        if (!read_live_curve_snapshot_settled(3, 20, &offsetsOk)) {
                            debug_log("service snapshot: live curve refresh failed, returning cached curve\n");
                        } else if (!offsetsOk) {
                            debug_log("service snapshot: curve refresh completed without offset readback confirmation\n");
                        }
                        // While recovering from a recent nvml.dll crash (GPU
                        // device reconnect / driver restart), skip
                        // refresh_global_state() which issues NVML reads
                        // directly and would access-violate and kill this pipe
                        // server thread, breaking the GUI connection.  Serve
                        // cached globals instead.  We check the recovery WINDOW
                        // (not the consume-once g_nvmlVhCrashed flag) so EVERY
                        // snapshot during the window is safe, and we leave
                        // g_nvmlVhCrashed intact for the fan runtime thread's
                        // nvml_ensure_ready() to drive the actual NVML/NVAPI
                        // re-init.  The fan thread clears the crash state on a
                        // successful reapply, ending the window early.
                        if (nvml_crash_recovery_active()) {
                            debug_log("service snapshot: NVML crash recovery in progress, using cached globals\n");
                        } else if (!refresh_global_state(detail, sizeof(detail))) {
                            debug_log("service snapshot: state refresh failed, returning cached globals%s%s\n",
                                detail[0] ? ": " : "",
                                detail[0] ? detail : "");
                        }
                        populate_control_state(&g_serviceControlState);
                        g_serviceControlStateValid = true;
                    }
                    response.status = SERVICE_STATUS_OK;
                    StringCchCopyA(response.message, ARRAY_COUNT(response.message), ok ? "snapshot ready" : (detail[0] ? detail : "snapshot unavailable"));
                    populate_service_snapshot(&response.snapshot);
                    if (g_serviceControlStateValid) response.controlState = g_serviceControlState;
                    unlock_service_runtime();
                    break;
                }
                case SERVICE_CMD_GET_TELEMETRY: {
                    char detail[256] = {};
                    bool lockAcquired = try_lock_service_runtime(250);
                    if (!lockAcquired) {
                        debug_log("service telemetry: runtime lock busy (recovery reapply in progress), serving cached telemetry\n");
                        response.status = SERVICE_STATUS_OK;
                        StringCchCopyA(response.message, ARRAY_COUNT(response.message), "telemetry cached");
                        populate_service_snapshot(&response.snapshot);
                        if (g_serviceControlStateValid) response.controlState = g_serviceControlState;
                        break;
                    }
                    if (!service_refresh_telemetry_for_request(detail, sizeof(detail))) {
                        debug_log("service telemetry: hardware initialize unavailable: %s\n", detail[0] ? detail : "unknown");
                    }
                    response.status = SERVICE_STATUS_OK;
                    StringCchCopyA(response.message, ARRAY_COUNT(response.message), detail[0] ? detail : "telemetry ready");
                    populate_service_snapshot(&response.snapshot);
                    if (g_serviceControlStateValid) response.controlState = g_serviceControlState;
                    unlock_service_runtime();
                    break;
                }
                case SERVICE_CMD_APPLY: {
                    char result[512] = {};
                    // Shared-only policy: a non-admin caller may only apply an
                    // admin-published shared profile.  The request must name a
                    // shared slot (SERVICE_REQUEST_FLAG_SHARED_SLOT); the service
                    // loads its OWN copy of that slot from the bank and applies
                    // it, ignoring the client-supplied settings — so a restricted
                    // user cannot smuggle custom OC.  Admins and machines without
                    // the policy are unaffected.  Checked before the runtime lock,
                    // so an early reject needs no unlock.
                    {
                        bool restrictShared = false;
                        get_machine_restrict_policy(&restrictShared);
                        if (restrictShared && !callerIsAdmin) {
                            if (!(request.flags & SERVICE_REQUEST_FLAG_SHARED_SLOT)) {
                                debug_log("service APPLY rejected: shared-only policy active; caller %s is not a machine admin and did not request a shared slot\n",
                                    callerUser[0] ? callerUser : "<unknown>");
                                response.status = SERVICE_STATUS_ERROR;
                                StringCchCopyA(response.message, ARRAY_COUNT(response.message),
                                    "Your administrator restricts this PC to shared profiles. Use \"Shared profiles...\" to load and apply one.");
                                break;
                            }
                            int sharedSlot = (int)((request.flags >> SERVICE_REQUEST_SHARED_SLOT_SHIFT) & SERVICE_REQUEST_SHARED_SLOT_MASK);
                            char machinePath[MAX_PATH] = {};
                            DesiredSettings sharedDesired = {};
                            char loadErr[256] = {};
                            if (sharedSlot < 1 || sharedSlot > CONFIG_NUM_SLOTS ||
                                !resolve_machine_config_path(machinePath, sizeof(machinePath)) ||
                                !is_profile_slot_saved(machinePath, sharedSlot) ||
                                !load_profile_from_config(machinePath, sharedSlot, &sharedDesired, loadErr, sizeof(loadErr))) {
                                debug_log("service APPLY rejected: shared slot %d unavailable for restricted caller %s: %s\n",
                                    sharedSlot, callerUser[0] ? callerUser : "<unknown>", loadErr[0] ? loadErr : "unknown");
                                response.status = SERVICE_STATUS_ERROR;
                                StringCchCopyA(response.message, ARRAY_COUNT(response.message),
                                    "That shared profile is no longer available. Ask your administrator.");
                                break;
                            }
                            // Apply the AUTHORITATIVE admin copy: replace the
                            // client-supplied settings and fall through to the
                            // normal validate+apply path.
                            sharedDesired.resetOcBeforeApply = true;
                            request.desired = sharedDesired;
                            request.resetOcBeforeApply = 1u;
                            debug_log("service APPLY: restricted caller %s applying admin shared slot %d (authoritative copy)\n",
                                callerUser[0] ? callerUser : "<unknown>", sharedSlot);
                        }
                    }
                    // Reject apply while recovering from a GPU device reconnect:
                    // the NVML/NVAPI writes would access-violate on the still-
                    // transitional driver and kill this pipe server thread
                    // (GUI sees ERROR_BROKEN_PIPE).  The fan runtime thread
                    // auto-reapplies the active profile once the driver settles.
                    //
                    // Allow apply if GPU data is already loaded (g_app.loaded
                    // is true) — the crash window was restored by recovery as a
                    // safety measure, but the handles are fresh and valid.
                    // RC7: block ALL GUI applies during crash recovery, even if
                    // g_app.loaded is true.  NVML writes (mem offset, fan speed)
                    // access-violate on the transitional driver and kill the pipe
                    // server thread (GUI sees ERROR_BROKEN_PIPE).  The dedicated
                    // reapply thread handles writes during the recovery window and
                    // survives VEH crashes via the health-check monitor.
                    if (nvml_crash_recovery_active()) {
                        debug_log("service APPLY rejected: NVML crash recovery in progress (loaded=%d)\n",
                            g_app.loaded ? 1 : 0);
                        response.status = SERVICE_STATUS_ERROR;
                        StringCchCopyA(response.message, ARRAY_COUNT(response.message),
                            "GPU driver is recovering after a device reconnect; please retry in a few seconds.");
                        populate_service_snapshot(&response.snapshot);
                        if (g_serviceHasActiveDesired) response.desired = g_serviceActiveDesired;
                        if (g_serviceControlStateValid) response.controlState = g_serviceControlState;
                        break;
                    }
                    debug_log("ipc raw: hasMem=%d memRaw=%d hasGpu=%d gpuRaw=%d exclRaw=%d\n",
                        request.desired.hasMemOffset ? 1 : 0,
                        request.desired.memOffsetMHz,
                        request.desired.hasGpuOffset ? 1 : 0,
                        request.desired.gpuOffsetMHz,
                        request.desired.gpuOffsetExcludeLowCount);
                    int rawGpuMHz = request.desired.gpuOffsetMHz;
                    validate_desired_settings_for_ipc(&request.desired);
                    request.desired.resetOcBeforeApply = request.resetOcBeforeApply != 0;
                    if (request.desired.hasGpuOffset && rawGpuMHz != request.desired.gpuOffsetMHz) {
                        debug_log("ipc validated: GPU offset clamped from %d to %d MHz (out of [-1000,1000] IPC range)\n",
                            rawGpuMHz, request.desired.gpuOffsetMHz);
                    }
                    debug_log("ipc validated: hasMem=%d mem=%d\n",
                        request.desired.hasMemOffset ? 1 : 0,
                        request.desired.memOffsetMHz);
                    lock_service_runtime();
                    bool ok = service_prepare_requested_gpu(&request, result, sizeof(result));
                    if (ok) ok = service_apply_desired_settings(&request.desired, (request.flags & 1u) != 0, result, sizeof(result));
                    if (ok) {
                        service_capture_owner_identity(callerUser, callerSessionId);
                        // OC stabilization window: record the user-apply time.  If the
                        // service crash-restarts within the window, startup reapply treats
                        // the just-applied settings as unstable and does NOT auto-reapply.
                        service_record_oc_apply_stamp();
                    }
                    response.status = ok ? SERVICE_STATUS_OK : SERVICE_STATUS_ERROR;
                    StringCchCopyA(response.message, ARRAY_COUNT(response.message), result);
                    populate_service_snapshot(&response.snapshot);
                    if (g_serviceHasActiveDesired) response.desired = g_serviceActiveDesired;
                    if (g_serviceControlStateValid) response.controlState = g_serviceControlState;
                    debug_log("service response APPLY: ok=%d controlValid=%d gpu=%d exclude=%d mem=%d power=%d fanMode=%d fanPct=%d\n",
                        ok ? 1 : 0,
                        response.controlState.valid ? 1 : 0,
                        response.controlState.gpuOffsetMHz,
                        response.controlState.gpuOffsetExcludeLowCount,
                        response.controlState.memOffsetMHz,
                        response.controlState.powerLimitPct,
                        response.controlState.fanMode,
                        response.controlState.fanFixedPercent);
                    unlock_service_runtime();
                    break;
                }
                case SERVICE_CMD_RESET: {
                    char result[512] = {};
                    // Reject reset while recovering from a GPU device reconnect:
                    // service_reset_all() issues NVAPI/NVML writes + refresh
                    // that would access-violate on the transitional driver and
                    // kill this pipe server thread (GUI sees ERROR_BROKEN_PIPE).
                    //
                    // Allow reset if GPU data is already loaded (g_app.loaded is
                    // true) — the crash window was restored by recovery as a
                    // safety measure, but the handles are fresh and valid.
                    // RC7: block ALL resets during crash recovery (same reason
                    // as APPLY — writes access-violate on the transitional driver).
                    if (nvml_crash_recovery_active()) {
                        debug_log("service RESET rejected: NVML crash recovery in progress (loaded=%d)\n",
                            g_app.loaded ? 1 : 0);
                        response.status = SERVICE_STATUS_ERROR;
                        StringCchCopyA(response.message, ARRAY_COUNT(response.message),
                            "GPU driver is recovering after a device reconnect; please retry in a few seconds.");
                        populate_service_snapshot(&response.snapshot);
                        if (g_serviceHasActiveDesired) response.desired = g_serviceActiveDesired;
                        if (g_serviceControlStateValid) response.controlState = g_serviceControlState;
                        break;
                    }
                    lock_service_runtime();
                    bool ok = service_prepare_requested_gpu(&request, result, sizeof(result));
                    if (ok) ok = service_reset_all(result, sizeof(result));
                    if (ok) {
                        service_capture_owner_identity(callerUser, callerSessionId);
                        // OC reset to defaults — clear the stabilization window stamp.
                        service_clear_oc_apply_stamp();
                    }
                    response.status = ok ? SERVICE_STATUS_OK : SERVICE_STATUS_ERROR;
                    StringCchCopyA(response.message, ARRAY_COUNT(response.message), result);
                    populate_service_snapshot(&response.snapshot);
                    if (g_serviceHasActiveDesired) response.desired = g_serviceActiveDesired;
                    if (g_serviceControlStateValid) response.controlState = g_serviceControlState;
                    debug_log("service response RESET: ok=%d gpu=%d exclude=%d fanMode=%d fanPct=%d\n",
                        ok ? 1 : 0,
                        response.controlState.gpuOffsetMHz,
                        response.controlState.gpuOffsetExcludeLowCount,
                        response.controlState.fanMode,
                        response.controlState.fanFixedPercent);
                    unlock_service_runtime();
                    break;
                }
                case SERVICE_CMD_GET_ACTIVE_DESIRED:
                    lock_service_runtime();
                    response.status = SERVICE_STATUS_OK;
                    if (g_serviceHasActiveDesired) response.desired = g_serviceActiveDesired;
                    populate_service_snapshot(&response.snapshot);
                    if (g_serviceControlStateValid) response.controlState = g_serviceControlState;
                    unlock_service_runtime();
                    break;
                case SERVICE_CMD_WRITE_LOG_SNAPSHOT:
                case SERVICE_CMD_WRITE_JSON_SNAPSHOT:
                case SERVICE_CMD_WRITE_PROBE_REPORT: {
                    char pathErr[256] = {};
                    if (!service_validate_file_write_path(request.path, pathErr, sizeof(pathErr))) {
                        response.status = SERVICE_STATUS_ERROR;
                        StringCchCopyA(response.message, ARRAY_COUNT(response.message), pathErr);
                        break;
                    }
                    char detail[256] = {};
                    lock_service_runtime();
                    bool ok = hardware_initialize(detail, sizeof(detail));
                    if (!ok && request.command != SERVICE_CMD_WRITE_PROBE_REPORT) {
                        response.status = SERVICE_STATUS_ERROR;
                        StringCchCopyA(response.message, ARRAY_COUNT(response.message), detail[0] ? detail : "Hardware initialization failed");
                    } else {
                        bool offsetsOk = false;
                        if (ok && !read_live_curve_snapshot_settled(4, 40, &offsetsOk)) {
                            debug_log("service file command: live curve refresh failed before file write\n");
                        }
                        if (ok) {
                            refresh_global_state(detail, sizeof(detail));
                        }
                        char fileErr[256] = {};
                        bool writeOk = false;
                        if (request.command == SERVICE_CMD_WRITE_LOG_SNAPSHOT) {
                            writeOk = write_log_snapshot(request.path, fileErr, sizeof(fileErr));
                        } else if (request.command == SERVICE_CMD_WRITE_JSON_SNAPSHOT) {
                            writeOk = write_json_snapshot(request.path, fileErr, sizeof(fileErr));
                        } else {
                            writeOk = write_probe_report(request.path, fileErr, sizeof(fileErr));
                        }
                        if (writeOk) {
                            char verifyErr[256] = {};
                            if (!service_verify_written_file_path(request.path, verifyErr, sizeof(verifyErr))) {
                                writeOk = false;
                                StringCchCopyA(fileErr, sizeof(fileErr), verifyErr);
                            }
                        }
                        response.status = writeOk ? SERVICE_STATUS_OK : SERVICE_STATUS_ERROR;
                        if (writeOk) {
                            StringCchPrintfA(response.message, ARRAY_COUNT(response.message), "Wrote %s", request.path[0] ? request.path : "requested output file");
                        } else {
                            StringCchCopyA(response.message, ARRAY_COUNT(response.message), fileErr[0] ? fileErr : "Failed writing requested file");
                        }
                        populate_service_snapshot(&response.snapshot);
                        if (g_serviceControlStateValid) response.controlState = g_serviceControlState;
                    }
                    unlock_service_runtime();
                    break;
                }
                default:
                    response.status = SERVICE_STATUS_ERROR;
                    StringCchCopyA(response.message, ARRAY_COUNT(response.message), "Unsupported service command");
                    break;
            }
            }
        }

        response.message[ARRAY_COUNT(response.message) - 1] = '\0';
        pipeErr[0] = 0;
        if (!service_pipe_write_exact(pipe, &response, sizeof(response), SERVICE_PIPE_SERVER_IO_TIMEOUT_MS, "writing service response", pipeErr, sizeof(pipeErr))) {
            debug_log("service_pipe_server: response write failed: %s\n", pipeErr[0] ? pipeErr : "unknown");
        }
        DisconnectNamedPipe(pipe);
        service_close_owned_pipe(pipe);
    }
    return 0;
}

static HANDLE g_servicePipeThread = nullptr;
static HANDLE g_serviceDeviceNotifyHandle = nullptr;

static DWORD WINAPI service_resume_reapply_thread_proc(void*) {
    lock_service_runtime();
    debug_log("resume reapply thread: checking OC persistence\n");
    service_check_oc_persistence(false);
    unlock_service_runtime();
    return 0;
}

// service_handle_session_change is defined in main_service_sessions.cpp
// (included before this shard).  The SCM SESSIONCHANGE handler routes through it.

static DWORD WINAPI service_control_handler_ex(DWORD dwControl, DWORD dwEventType, LPVOID, LPVOID lpEventData) {
    if (dwControl == SERVICE_CONTROL_POWEREVENT && dwEventType == PBT_APMRESUMEAUTOMATIC) {
        if (g_serviceHasActiveDesired) {
            debug_log("power event: resume from standby, re-applying settings\n");
            HANDLE hReapply = CreateThread(nullptr, 0, service_resume_reapply_thread_proc, nullptr, 0, nullptr);
            if (hReapply) CloseHandle(hReapply);
            else debug_log("power event: resume reapply CreateThread FAILED (error=%lu) — OC persistence check skipped\n", GetLastError());
        }
        return NO_ERROR;
    }

    // Device event handling — only fires for GUID_DEVINTERFACE_DISPLAY_ADAPTER
    if (dwControl == SERVICE_CONTROL_DEVICEEVENT) {
        PDEV_BROADCAST_DEVICEINTERFACEW db_dev = (PDEV_BROADCAST_DEVICEINTERFACEW)lpEventData;
        if (!db_dev) return NO_ERROR;

        switch (dwEventType) {
            case DBT_DEVICEREMOVEPENDING:
            case DBT_DEVICEREMOVECOMPLETE: {
                // This handler runs on the SCM control thread — it must NOT
                // block or call into nvml.dll (which can hang on a dead driver).
                // Just set the removed flag (read lock-free elsewhere) and the
                // crash-tick so nvml_crash_recovery_active() / the fan pulse
                // treat NVML as unsafe.  Snapshot the active settings for the
                // recovery reapply fallback.  Do NOT stop the fan thread or
                // close NVML here (both can block); the arrival handler or
                // main-loop monitor will run in-process recovery.
                debug_log("device event: GPU device removal detected (type=%lu)\n", dwEventType);
                g_app.deviceRemoved = true;
                g_app.deviceRemoveTimeMs = GetTickCount64();
                g_app.pendingDeviceRecovery = g_serviceHasActiveDesired;
                service_write_restart_reapply_snapshot();
                debug_log("device event: marked removed, pendingRecovery=%d\n", g_app.pendingDeviceRecovery ? 1 : 0);
                return NO_ERROR;
            }

            case DBT_DEVICEARRIVAL: {
                // Device came back after a removal.  Perform in-process recovery:
                // close stale NVML/NvAPI handles, re-init with fresh module
                // instances, and reapply the saved OC/fan settings.  No process
                // restart.  Must not block the SCM control thread — launch a
                // dedicated recovery thread instead.
                debug_log("device event: GPU device arrived (removal flag was %d)\n", g_app.deviceRemoved);
                bool deviceWasRemoved = g_app.deviceRemoved;
                g_app.deviceRemoved = false;
                // The on-disk driver version may have changed during the
                // device-removed window (driver upgrade).  Log it here so
                // post-mortem analysis can correlate with the Phase B log
                // line in the recovery thread.  The actual reload happens
                // inside the recovery thread (Phase B/C) where the lock is
                // held and the pipe server is blocked.
                if (deviceWasRemoved) {
                    service_check_disk_version_on_device_arrival();
                }
                if (deviceWasRemoved && (g_serviceHasActiveDesired ||
                        g_app.fanCurveRuntimeActive || g_app.fanFixedRuntimeActive)) {
                    debug_log("device event: GPU arrived with active settings — requesting service restart for clean driver re-init\n");
                    // F-REL-2: a genuine device arrival / driver (re)install is a
                    // strong signal the environment changed.  Clear the restart-loop
                    // history so the fresh process re-arms and reapplies the retained
                    // snapshot even if a recent loop had tripped the dormant breaker.
                    service_clear_restart_history();
                    launch_recovery_thread();
                } else {
                    debug_log("device event: arrival with nothing to restore; no recovery needed\n");
                }
                return NO_ERROR;
            }

            case DBT_DEVNODES_CHANGED: {
                // Device node state changed (e.g. GPU enabled/disabled in Device
                // Manager).  This does NOT fire REMOVEPENDING/REMOVECOMPLETE or
                // ARRIVAL for the device interface — it's a separate notification
                // for the device node itself.  Check whether OC settings survived
                // the driver reload and reapply if needed.  Must not block the
                // SCM control thread, so dispatch to a worker.
                debug_log("device event: devnode changed — checking OC persistence\n");
                HANDLE hDevnode = CreateThread(nullptr, 0, [](LPVOID) -> DWORD {
                    lock_service_runtime();
                    service_check_oc_persistence(true);
                    unlock_service_runtime();
                    return 0;
                }, nullptr, 0, nullptr);
                if (hDevnode) CloseHandle(hDevnode);
                else debug_log("device event: devnode persistence check CreateThread FAILED (error=%lu)\n", GetLastError());
                return NO_ERROR;
            }

            default:
                debug_log("device event: unhandled type=%lu\n", dwEventType);
                break;
        }
        return NO_ERROR;
    }

    if (dwControl == SERVICE_CONTROL_SESSIONCHANGE) {
        // Route every session-change notification through the active-user
        // policy router.  The router debounces (only re-applies when the
        // resolved active session identity changes) and handles LOGON, LOGOFF,
        // and CONSOLE_CONNECT/DISCONNECT so Fast-User-Switching restores the
        // now-active user's profile.  WTSSESSION_NOTIFICATION carries the
        // specific session, but the router re-resolves the *active* session
        // (console-first, RDP fallback) rather than trusting the event session
        // alone — a logon for a non-active session must not win GPU control.
        WTSSESSION_NOTIFICATION* sn = (WTSSESSION_NOTIFICATION*)lpEventData;
        if (sn) {
            DWORD sessionId = sn->dwSessionId;
            debug_log("session change: event type=%lu for session %lu\n",
                (unsigned long)dwEventType, (unsigned long)sessionId);

            // The SCM control handler must return quickly, so dispatch the
            // (driver-waiting, lock-taking) apply work to a worker thread.
            struct SessionChangePayload {
                DWORD eventType;
                DWORD sessionId;
            };
            SessionChangePayload* payload = (SessionChangePayload*)HeapAlloc(GetProcessHeap(), 0, sizeof(SessionChangePayload));
            if (payload) {
                payload->eventType = dwEventType;
                payload->sessionId = sessionId;
                HANDLE hThread = CreateThread(nullptr, 0, [](LPVOID p) -> DWORD {
                    SessionChangePayload* pl = (SessionChangePayload*)p;
                    if (!pl) return 1;
                    DWORD et = pl->eventType;
                    DWORD sid = pl->sessionId;
                    HeapFree(GetProcessHeap(), 0, pl);
                    service_handle_session_change(et, sid);
                    return 0;
                }, payload, 0, nullptr);
                if (hThread) {
                    CloseHandle(hThread);
                } else {
                    debug_log("session change: failed to create worker thread for session %lu (error=%lu)\n",
                        (unsigned long)sessionId, GetLastError());
                    HeapFree(GetProcessHeap(), 0, payload);
                }
            }
        }
        return NO_ERROR;
    }

    if (dwControl != SERVICE_CONTROL_STOP && dwControl != SERVICE_CONTROL_SHUTDOWN) return NO_ERROR;
    g_serviceStatus.dwCurrentState = SERVICE_STOP_PENDING;
    SetServiceStatus(g_serviceStatusHandle, &g_serviceStatus);
    if (g_serviceStopEvent) SetEvent(g_serviceStopEvent);
    if (g_servicePipeWakeEvent) SetEvent(g_servicePipeWakeEvent);
    return NO_ERROR;
}

static void WINAPI service_main(DWORD argc, LPWSTR* argv) {
    g_app.isServiceProcess = true;
    // A GUI/CLI-initiated start (install / repair / restart) passes --manual via
    // StartService so the no-snapshot startup coordinator stays non-mutating: the
    // interactive client drives its own explicit apply.  A boot auto-start by the
    // SCM passes no args, leaving this false so the coordinator may reconcile the
    // already-active session's logon profile once (Fast Startup / autologon).
    for (DWORD i = 1; argv && i < argc; i++) {
        if (argv[i] && _wcsicmp(argv[i], L"--manual") == 0) {
            g_serviceManualStart = true;
        }
    }
    SetUnhandledExceptionFilter(green_curve_unhandled_exception_filter);
    // Vectored handler catches nvml.dll access violations (driver restart without
    // device removal notification) and lets the fan runtime thread survive.
    AddVectoredExceptionHandler(1, green_curve_vectored_handler);

    // Suppress all debug logging until the user's config is read.
    // This guarantees zero file I/O when the user has opted out.
    bool envExplicitlyEnabled = false;
    {
        char debugEnvBuf[16] = {};
        DWORD debugEnvLen = GetEnvironmentVariableA(APP_DEBUG_ENV, debugEnvBuf, ARRAY_COUNT(debugEnvBuf));
        if (debugEnvLen > 0 && !(debugEnvBuf[0] == '0' && debugEnvBuf[1] == '\0')) {
            envExplicitlyEnabled = true;
        }
    }
    g_debug_logging = envExplicitlyEnabled;

    g_serviceStatusHandle = RegisterServiceCtrlHandlerExW(L"GreenCurveService", service_control_handler_ex, nullptr);
    if (!g_serviceStatusHandle) return;

    g_serviceStatus.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    // SERVICE_ACCEPT_DEVICE_EVENTS required for DBT_DEVICEREMOVEPENDING /
    // DBT_DEVICEARRIVAL notifications to reach service_control_handler_ex.
    g_serviceStatus.dwControlsAccepted = SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN | SERVICE_ACCEPT_POWEREVENT | SERVICE_ACCEPT_DEVICE_EVENTS | SERVICE_ACCEPT_SESSIONCHANGE;
    g_serviceStatus.dwCurrentState = SERVICE_START_PENDING;
    SetServiceStatus(g_serviceStatusHandle, &g_serviceStatus);

    service_resolve_active_user_paths_for_startup("service_main startup");
    if (!envExplicitlyEnabled) {
        refresh_service_debug_logging_from_config();
    }

    if (g_debug_logging) {
        g_debugSessionStartTickMs = GetTickCount64();
        debug_log_session_marker("BEGIN", "service", "service_main bootstrap");
        debug_log_session_marker("BEGIN", "service", "service_main startup");
    }

    // F-SEC-5 / policy: SENSITIVE program artifacts (crash dumps, service logs,
    // restart/reapply state) live only under SYSTEM %LOCALAPPDATA%\Green Curve.
    // Clear any such legacy artifacts older builds left world-readable in
    // %ProgramData%\Green Curve (the deliberately-shared shared-profiles.ini is
    // preserved — see service_cleanup_legacy_programdata).
    service_cleanup_legacy_programdata();
    // One-time migration of the shared profile bank from the legacy
    // machine.ini-next-to-binary location to %ProgramData%\Green Curve.  Runs as
    // LocalSystem so it can write %ProgramData% and apply the protected DACL.
    migrate_legacy_machine_config();
    // Harden the %ProgramData% shared bank at boot (before any interactive login)
    // so a standard user cannot pre-create and squat the directory/file.
    secure_shared_bank_at_startup();
    // F-REL-2: bound the on-disk VEH minidumps so a restart loop cannot fill the
    // disk (runs once per fresh process = once per restart cycle).
    service_rotate_minidumps(10);

    ensure_service_runtime_lock();
    g_serviceStopEvent = CreateEventA(nullptr, TRUE, FALSE, nullptr);
    g_servicePipeWakeEvent = CreateEventA(nullptr, TRUE, FALSE, nullptr);
    // Auto-reset: each SetEvent forces exactly one pipe-instance recycle so the
    // ACL is rebuilt for the new active user (see service_handle_session_change).
    g_servicePipeRecycleEvent = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    if (!g_serviceStopEvent) {
        debug_log("service_main: FATAL failed to create stop event\n");
        g_serviceStatus.dwCurrentState = SERVICE_STOPPED;
        SetServiceStatus(g_serviceStatusHandle, &g_serviceStatus);
        return;
    }
    if (!g_servicePipeWakeEvent) {
        debug_log("service_main: WARNING failed to create pipe wake event, pipe thread will use 2-handle wait\n");
    }
    if (g_app.fanCurveRuntimeActive || g_app.fanFixedRuntimeActive) {
        ensure_service_fan_runtime_thread();
    }

    DWORD threadId = 0;
    g_servicePipeThread = CreateThread(nullptr, 1024 * 1024, service_pipe_server_thread_proc, nullptr, STACK_SIZE_PARAM_IS_A_RESERVATION, &threadId);
    if (!g_servicePipeThread) {
        debug_log("service_main: FATAL failed to create pipe server thread (error %lu)\n", GetLastError());
        stop_service_fan_runtime_thread();
        if (g_servicePipeWakeEvent) {
            CloseHandle(g_servicePipeWakeEvent);
            g_servicePipeWakeEvent = nullptr;
        }
        if (g_servicePipeRecycleEvent) {
            CloseHandle(g_servicePipeRecycleEvent);
            g_servicePipeRecycleEvent = nullptr;
        }
        if (g_serviceFanStopEvent) {
            CloseHandle(g_serviceFanStopEvent);
            g_serviceFanStopEvent = nullptr;
        }
        if (g_serviceStopEvent) {
            CloseHandle(g_serviceStopEvent);
            g_serviceStopEvent = nullptr;
        }
        if (g_serviceRuntimeLock) {
            CloseHandle(g_serviceRuntimeLock);
            g_serviceRuntimeLock = nullptr;
        }
        g_serviceStatus.dwCurrentState = SERVICE_STOPPED;
        SetServiceStatus(g_serviceStatusHandle, &g_serviceStatus);
        return;
    }
    g_serviceStatus.dwCurrentState = SERVICE_RUNNING;
    SetServiceStatus(g_serviceStatusHandle, &g_serviceStatus);

    // Ensure SCM auto-restart failure actions are present (driver-recovery
    // restart relies on the SCM relaunching us after a non-zero exit).  Done at
    // every start so installs predating this code are repaired in place.
    service_ensure_failure_actions_configured();
    // F-REL-1: log whether the SCM auto-restart net is actually armed, so a
    // "service never came back after a driver event" report is diagnosable.
    service_verify_restart_safety_net();

    // Register for GUID_DEVINTERFACE_DISPLAY_ADAPTER — fires ONLY for GPU/display
    // devices, not for USB, network, or other device changes.
    {
        DEV_BROADCAST_DEVICEINTERFACEW db_dev = {};
        db_dev.dbcc_size = sizeof(db_dev);
        db_dev.dbcc_devicetype = DBT_DEVTYP_DEVICEINTERFACE;
        db_dev.dbcc_reserved = 0;
        db_dev.dbcc_classGuid = GUID_DISPLAY_ADAPTER_DEVINTERFACE;
        g_serviceDeviceNotifyHandle = RegisterDeviceNotificationW(
            g_serviceStatusHandle,
            &db_dev,
            DEVICE_NOTIFY_SERVICE_HANDLE
        );
        if (g_serviceDeviceNotifyHandle) {
            debug_log("device notify: registered for GUID_DEVINTERFACE_DISPLAY_ADAPTER\n");
        } else {
            debug_log("device notify: failed to register (error %lu), device events will be missed\n",
                GetLastError());
        }
    }

    debug_log("service_main: running; hardware writes only on explicit client request, session-change logon/switch, or restart-snapshot recovery\n");

    // One startup worker owns restart-snapshot recovery.  No-snapshot starts are
    // intentionally non-mutating so service install/repair/manual start does
    // not unexpectedly apply a saved logon profile or reset the GPU.
    service_launch_startup_coordinator();

    // Main service loop: wait for stop event, but periodically check if the
    // fan runtime thread or pipe server thread needs restarting (e.g. after a
    // driver-upgrade crash handled by the VEH which calls ExitThread).
    if (g_serviceStopEvent) {
        while (true) {
            DWORD wr = WaitForSingleObject(g_serviceStopEvent, SERVICE_FAN_WATCHDOG_INTERVAL_MS);
            if (wr == WAIT_OBJECT_0) break; // stop event signaled

            // Wedge watchdog: if a fan pulse has been in-flight far longer than
            // any healthy pulse (a driver restart can HANG a read inside nvml.dll
            // — a hang the VEH cannot catch), the fan thread is stuck inside the
            // stale nvml.dll module.  Recover by restarting the process: a fresh
            // process maps clean driver DLLs, and ExitProcess tears down the
            // wedged thread.  Do NOT TerminateThread / close NVML here — racy and
            // unnecessary right before the process exits.
            if (g_serviceFanPulseInFlight && g_serviceFanPulseHeartbeatMs != 0) {
                ULONGLONG stuckMs = GetTickCount64() - g_serviceFanPulseHeartbeatMs;
                if (stuckMs > SERVICE_FAN_PULSE_WEDGE_TIMEOUT_MS) {
                    debug_log("service_main: fan pulse wedged for %llu ms — requesting service restart\n", stuckMs);
                    request_service_restart("fan pulse wedged inside nvml.dll");
                }
            }

            // If a pipe-server request was the first thread to touch stale
            // NVML/NvAPI after a driver restart, the VEH kills that pipe
            // thread rather than the fan thread.  The main loop must still
            // launch recovery so reset/apply works again even when no fan
            // runtime is active.
            service_maybe_launch_recovery_from_main_loop("main loop");

            // Check and launch the recovery reapply on a dedicated thread.
            // If VEH kills the reapply thread, it does NOT take down the
            // main loop — the next iteration retries.
            service_check_reapply_thread_health();

            // Check whether the active desired VF curve drifted at runtime.
            // This only queues the existing reapply worker after confirmed
            // drift; it does not write to hardware from the watchdog path.
            service_check_active_vf_drift_monitor("main loop");

            // Check fan runtime thread health
            if (g_app.fanCurveRuntimeActive || g_app.fanFixedRuntimeActive) {
                ensure_service_fan_runtime_thread();
            }

            // Check pipe server thread health
            if (g_servicePipeThread && WaitForSingleObject(g_servicePipeThread, 0) == WAIT_OBJECT_0) {
                debug_log("service_main: pipe server thread died, recreating\n");
                // Reclaim the orphaned pipe handle atomically: take the slot to
                // INVALID and close only if we won the real handle, so we never
                // double-close one the dead thread already released (which would
                // hard-crash the process under Strict Handle Checks).
                HANDLE orphanPipe = (HANDLE)InterlockedExchangePointer(
                    (PVOID volatile*)&g_servicePipeHandle, INVALID_HANDLE_VALUE);
                if (orphanPipe != INVALID_HANDLE_VALUE) {
                    CloseHandle(orphanPipe);
                }
                CloseHandle(g_servicePipeThread);
                g_servicePipeThread = nullptr;
                DWORD pipeThreadId = 0;
                g_servicePipeThread = CreateThread(nullptr, 1024 * 1024,
                    service_pipe_server_thread_proc, nullptr,
                    STACK_SIZE_PARAM_IS_A_RESERVATION, &pipeThreadId);
                debug_log("service_main: pipe server thread recreated=%d\n",
                    g_servicePipeThread ? 1 : 0);
            }
        }
    }

    // Driver-recovery restart: a recovery trigger requested a controlled process
    // restart (device reconnect / driver upgrade / TDR / fan-pulse wedge).
    //
    // CRITICAL (build 230): terminate WITHOUT reporting SERVICE_STOPPED.  The SCM
    // queues the configured SC_ACTION_RESTART failure action by DEFAULT only when
    // a service process dies *without* reporting SERVICE_STOPPED.  An earlier
    // design reported SERVICE_STOPPED with a non-zero dwWin32ExitCode and relied
    // on the SCM relaunching us — but that requires the
    // SERVICE_CONFIG_FAILURE_ACTIONS_FLAG / fFailureActionsOnNonCrashFailures
    // opt-in, which the LocalSystem service token CANNOT set at runtime
    // (ChangeServiceConfig2 returns ERROR_ACCESS_DENIED because LocalSystem lacks
    // SERVICE_CHANGE_CONFIG on its own service).  Result in the field: the SCM saw
    // a reported "graceful" stop and never relaunched, so the service stayed dead
    // after a GPU reconnect.  Exiting without reporting SERVICE_STOPPED takes the
    // SCM's default crash-recovery path and needs no flag and no admin rights —
    // only the SC_ACTION_RESTART actions (set at install).
    //
    // Also skip the normal NVML teardown below (nvml_set_fan_auto / close_nvml can
    // HANG on a dead/transitional driver and wedge the stop).  The fresh process
    // maps clean driver DLLs and the startup coordinator restores the saved
    // profile.
    if (InterlockedExchangeAdd(&g_serviceRestartRequested, 0) != 0) {
        debug_log("service_main: restart requested — terminating WITHOUT reporting SERVICE_STOPPED so the SCM SC_ACTION_RESTART failure action fires (default crash-recovery path); SCM will relaunch\n");
        if (g_serviceDeviceNotifyHandle) {
            UnregisterDeviceNotification(g_serviceDeviceNotifyHandle);
            g_serviceDeviceNotifyHandle = nullptr;
        }
        if (g_debug_logging) {
            debug_log_session_marker("END", "service", "restart for GPU driver recovery (unexpected-termination path)");
        }
        ExitProcess(1);
    }

    // Shutdown — clean up reapply thread first
    if (g_serviceReapplyThread) {
        // The reapply thread does not hold any resources that need orderly
        // shutdown beyond handle cleanup.  Just close the handle; if the
        // thread is still running, the process exit will clean it up.
        HANDLE reapplyHandle = (HANDLE)InterlockedExchangePointer(
            (PVOID volatile*)&g_serviceReapplyThread, nullptr);
        if (reapplyHandle) {
            WaitForSingleObject(reapplyHandle, 5000);
            CloseHandle(reapplyHandle);
        }
        InterlockedExchange((volatile LONG*)&g_serviceReapplyThreadId, 0);
    }
    stop_service_fan_runtime_thread();
    if (g_servicePipeThread) {
        if (g_servicePipeWakeEvent) SetEvent(g_servicePipeWakeEvent);
        if (g_servicePipeRecycleEvent) SetEvent(g_servicePipeRecycleEvent);
        WaitForSingleObject(g_servicePipeThread, INFINITE);
        CloseHandle(g_servicePipeThread);
        g_servicePipeThread = nullptr;
    }
    if (g_servicePipeWakeEvent) {
        CloseHandle(g_servicePipeWakeEvent);
        g_servicePipeWakeEvent = nullptr;
    }
    if (g_servicePipeRecycleEvent) {
        CloseHandle(g_servicePipeRecycleEvent);
        g_servicePipeRecycleEvent = nullptr;
    }
    if (g_serviceFanStopEvent) {
        CloseHandle(g_serviceFanStopEvent);
        g_serviceFanStopEvent = nullptr;
    }
    if (g_serviceStopEvent) {
        CloseHandle(g_serviceStopEvent);
        g_serviceStopEvent = nullptr;
    }
    if (g_serviceRuntimeLock) {
        CloseHandle(g_serviceRuntimeLock);
        g_serviceRuntimeLock = nullptr;
    }
    // Cleanup device notification handle
    if (g_serviceDeviceNotifyHandle) {
        UnregisterDeviceNotification(g_serviceDeviceNotifyHandle);
        g_serviceDeviceNotifyHandle = nullptr;
    }
    // Reset GPU to driver defaults on graceful shutdown (GPU offset, memory
    // offset, power limit, locked clocks, and fan all restored to stock).
    char resetDetail[256] = {};
    service_reset_all(resetDetail, sizeof(resetDetail));
    close_nvml();
    if (g_app.hNvApi) {
        FreeLibrary(g_app.hNvApi);
        g_app.hNvApi = nullptr;
    }
    // Graceful (user-requested) stop: report a zero exit code explicitly.  With
    // fFailureActionsOnNonCrashFailures enabled, a non-zero dwWin32ExitCode on a
    // reported SERVICE_STOPPED would auto-restart us; a clean stop must not.
    g_serviceStatus.dwWin32ExitCode = NO_ERROR;
    g_serviceStatus.dwServiceSpecificExitCode = 0;
    g_serviceStatus.dwCurrentState = SERVICE_STOPPED;
    SetServiceStatus(g_serviceStatusHandle, &g_serviceStatus);
    if (g_debug_logging) {
        ULONGLONG elapsedMs = g_debugSessionStartTickMs ? (GetTickCount64() - g_debugSessionStartTickMs) : 0;
        char extra[128] = {};
        StringCchPrintfA(extra, ARRAY_COUNT(extra), "service_main shutdown uptimeMs=%llu", elapsedMs);
        debug_log_session_marker("END", "service", extra);
    }
    close_debug_log_file();
    DeleteCriticalSection(&g_debugLogLock);
}

static bool should_suppress_startup_ui() {
    return g_app.launchedFromLogon || g_app.startHiddenToTray;
}

static const char* nvml_err_name(nvmlReturn_t r) {
