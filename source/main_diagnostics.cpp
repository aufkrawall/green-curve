// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT

static void close_debug_log_file() {
    EnterCriticalSection(&g_debugLogLock);
    if (g_debugLogFile != INVALID_HANDLE_VALUE) {
        FlushFileBuffers(g_debugLogFile);
        CloseHandle(g_debugLogFile);
        g_debugLogFile = INVALID_HANDLE_VALUE;
    }
    g_debugLogOpenPath[0] = 0;
    LeaveCriticalSection(&g_debugLogLock);
}

static DWORD debug_log_file_attributes() {
    return FILE_ATTRIBUTE_NORMAL | (g_app.isServiceProcess ? FILE_FLAG_WRITE_THROUGH : 0);
}

static HANDLE open_debug_log_file_locked(const char* debugPath) {
    if (!debugPath || !debugPath[0]) return INVALID_HANDLE_VALUE;
    char pathErr[256] = {};
    ensure_parent_directory_for_file(debugPath, pathErr, sizeof(pathErr));
    HANDLE h = gc_CreateFileUtf8(debugPath, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
        OPEN_ALWAYS, debug_log_file_attributes(), nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        StringCchCopyA(g_debugLogOpenPath, ARRAY_COUNT(g_debugLogOpenPath), debugPath);
    }
    return h;
}

static const char* debug_log_path() {
    return g_debugLogPath[0] ? g_debugLogPath : APP_DEBUG_LOG_FILE;
}

static const char* service_early_debug_log_path() {
    if (g_debugEarlyLogPath[0]) return g_debugEarlyLogPath;

    char dir[MAX_PATH] = {};
    if (!resolve_service_machine_data_dir(dir, sizeof(dir))) {
        return APP_DEBUG_LOG_FILE; // last-resort relative path
    }
    StringCchPrintfA(g_debugEarlyLogPath, ARRAY_COUNT(g_debugEarlyLogPath),
        "%s\\%s", dir, APP_DEBUG_LOG_FILE);
    return g_debugEarlyLogPath;
}

static const char* json_snapshot_path() {
    return g_jsonPath[0] ? g_jsonPath : APP_JSON_FILE;
}

static const char* error_log_path() {
    return g_errorLogPath[0] ? g_errorLogPath : APP_LOG_FILE;
}

static const char* effective_debug_log_path() {
    if (g_app.isServiceProcess && !g_serviceUserPathsResolved) {
        return service_early_debug_log_path();
    }
    return debug_log_path();
}

static void debug_log(const char* fmt, ...) {
    if (!g_debug_logging || !fmt) return;
    char message[1024] = {};
    va_list ap;
    va_start(ap, fmt);
    StringCchVPrintfA(message, ARRAY_COUNT(message), fmt, ap);
    va_end(ap);
    char buf[1200] = {};
    int prefixLen = format_log_timestamp_prefix(buf, ARRAY_COUNT(buf));
    StringCchCatA(buf + prefixLen, ARRAY_COUNT(buf) - prefixLen, message);
    OutputDebugStringA(buf);

    EnterCriticalSection(&g_debugLogLock);
    const char* debugPath = effective_debug_log_path();

    if (!g_app.isServiceProcess && !g_debugLogPath[0]) {
        char pathErr[256] = {};
        resolve_data_paths(pathErr, sizeof(pathErr));
        debugPath = effective_debug_log_path();
    }

    if (g_debugLogFile != INVALID_HANDLE_VALUE && _stricmp(g_debugLogOpenPath, debugPath) != 0) {
        FlushFileBuffers(g_debugLogFile);
        CloseHandle(g_debugLogFile);
        g_debugLogFile = INVALID_HANDLE_VALUE;
        g_debugLogOpenPath[0] = 0;
    }

    if (g_debugLogFile == INVALID_HANDLE_VALUE) {
        g_debugLogFile = open_debug_log_file_locked(debugPath);
    }

    if (g_debugLogFile != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        if (!WriteFile(g_debugLogFile, buf, (DWORD)strlen(buf), &written, nullptr)) {
            CloseHandle(g_debugLogFile);
            g_debugLogFile = INVALID_HANDLE_VALUE;
            g_debugLogOpenPath[0] = 0;

            g_debugLogFile = open_debug_log_file_locked(debugPath);
            if (g_debugLogFile != INVALID_HANDLE_VALUE) {
                WriteFile(g_debugLogFile, buf, (DWORD)strlen(buf), &written, nullptr);
            }
        }
        if (g_app.isServiceProcess && g_debugLogFile != INVALID_HANDLE_VALUE) {
            FlushFileBuffers(g_debugLogFile);
        }
    }

    LeaveCriticalSection(&g_debugLogLock);
}

static void debug_log_session_marker(const char* phase, const char* kind, const char* extra) {
    if (!g_debug_logging) return;
    SYSTEMTIME now = {};
    GetLocalTime(&now);
    DWORD pid = GetCurrentProcessId();
    DWORD sessionId = 0;
    ProcessIdToSessionId(pid, &sessionId);
    const char* configPath = g_app.configPath[0] ? g_app.configPath : "<unset>";
    debug_log("\n===== SESSION %s =====\n", phase ? phase : "MARK");
    debug_log("time=%04u-%02u-%02u %02u:%02u:%02u.%03u pid=%lu session=%lu kind=%s version=%s build=%lu protocol=%lu elevated=%d serviceProcess=%d serviceInstalled=%d serviceRunning=%d serviceAvailable=%d config=%s\n",
        now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond, now.wMilliseconds,
        pid,
        sessionId,
        kind ? kind : "unknown",
        APP_VERSION,
        (unsigned long)APP_BUILD_NUMBER,
        (unsigned long)SERVICE_PROTOCOL_VERSION,
        is_elevated() ? 1 : 0,
        g_app.isServiceProcess ? 1 : 0,
        g_app.backgroundServiceInstalled ? 1 : 0,
        g_app.backgroundServiceRunning ? 1 : 0,
        g_app.backgroundServiceAvailable ? 1 : 0,
        configPath);
    debug_log("debug logging is enabled by default. This log may contain GPU identifiers, config paths, and applied settings.\n");
    debug_log("Set GREEN_CURVE_DEBUG=0 or [debug] enabled=0 to disable logging.\n");
    if (extra && extra[0]) {
        debug_log("details=%s\n", extra);
    }
    debug_log("========================\n");
}

static void set_last_apply_phase(const char* phase) {
    // Per-phase apply timing (logging only, no behaviour change): log how long the
    // PREVIOUS phase took so profile-apply latency can be attributed precisely
    // (reset/settle vs each ~1s VF setControl vs correction vs fan) when measuring
    // and comparing profile-switch speedups. Single applier thread → a static tick
    // is safe. Reset to 0 on the terminal empty-phase call so a new apply starts fresh.
    static ULONGLONG s_lastPhaseTickMs = 0;
    ULONGLONG nowMs = GetTickCount64();
    if (!phase || !phase[0]) {
        g_lastApplyPhase[0] = 0;
        s_lastPhaseTickMs = 0;
        return;
    }
    unsigned long long prevPhaseMs = (s_lastPhaseTickMs != 0 && nowMs >= s_lastPhaseTickMs)
        ? (unsigned long long)(nowMs - s_lastPhaseTickMs) : 0ull;
    StringCchCopyA(g_lastApplyPhase, ARRAY_COUNT(g_lastApplyPhase), phase);
    debug_log("apply phase: %s (previous phase took +%llums)\n", g_lastApplyPhase, prevPhaseMs);
    s_lastPhaseTickMs = nowMs;
}

static bool crash_artifact_data_dir(char* out, size_t outSize) {
    if (!out || outSize == 0) return false;
    out[0] = 0;
    if (g_app.isServiceProcess) {
        if (resolve_service_machine_data_dir(out, outSize)) return true;
        return false;
    }
    if (g_userDataDir[0]) {
        return SUCCEEDED(StringCchCopyA(out, outSize, g_userDataDir));
    }
    return SUCCEEDED(StringCchCopyA(out, outSize, "."));
}

static MINIDUMP_TYPE green_curve_actionable_minidump_type() {
    // Keep dumps bounded (no full process memory), but include the metadata
    // WinDbg/cdb need for useful postmortems: data and referenced stack memory,
    // complete thread records, unloaded modules, handles, process/thread data,
    // and the full virtual-memory map. Matching private PDBs are emitted by the
    // build into dist/symbols and are deliberately excluded from packages.
    return (MINIDUMP_TYPE)(
        MiniDumpWithDataSegs |
        MiniDumpWithHandleData |
        MiniDumpScanMemory |
        MiniDumpWithUnloadedModules |
        MiniDumpWithIndirectlyReferencedMemory |
        MiniDumpWithProcessThreadData |
        MiniDumpWithThreadInfo |
        MiniDumpWithFullMemoryInfo);
}

static void write_crash_breadcrumb_direct(const char* text) {
    if (!text || !text[0]) return;
    OutputDebugStringA(text);

    char path[MAX_PATH] = {};
    char dataDir[MAX_PATH] = {};
    if (!crash_artifact_data_dir(dataDir, sizeof(dataDir))) return;
    StringCchPrintfA(path, ARRAY_COUNT(path), "%s\\%s", dataDir, "greencurve_crash.txt");
    char pathErr[256] = {};
    ensure_parent_directory_for_file(path, pathErr, sizeof(pathErr));

    HANDLE h = gc_CreateFileUtf8(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
        OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    WriteFile(h, text, (DWORD)strlen(text), &written, nullptr);
    FlushFileBuffers(h);
    CloseHandle(h);
}

static LONG WINAPI green_curve_unhandled_exception_filter(EXCEPTION_POINTERS* info) {
    SYSTEMTIME now = {};
    GetLocalTime(&now);
    DWORD code = 0;
    void* address = nullptr;
    void* faultAddr = nullptr;
    bool faultWrite = false;
    if (info && info->ExceptionRecord) {
        code = info->ExceptionRecord->ExceptionCode;
        address = info->ExceptionRecord->ExceptionAddress;
        if (code == 0xC0000005 && info->ExceptionRecord->NumberParameters >= 2) {
            faultWrite = (info->ExceptionRecord->ExceptionInformation[0] != 0);
            faultAddr = (void*)info->ExceptionRecord->ExceptionInformation[1];
        }
    }

    // If the crash is inside a GPU driver DLL (nvml.dll, nvapi64, NvMessageBus)
    // it's caused by a driver update / stale handles — suppress the dump to avoid
    // crash dump spam on every driver update. Only dump for crashes in our code.
    bool isGpuDriverDll = false;
    if (address) {
        HMODULE hMod = nullptr;
        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                (LPCWSTR)address, &hMod) && hMod) {
            WCHAR modPath[MAX_PATH] = {};
            if (GetModuleFileNameW(hMod, modPath, ARRAY_COUNT(modPath))) {
                CharLowerW(modPath);
                isGpuDriverDll = wcsstr(modPath, L"nvml.dll") != nullptr
                    || wcsstr(modPath, L"nvapi64.dll") != nullptr
                    || wcsstr(modPath, L"nvcuda.dll") != nullptr
                    || wcsstr(modPath, L"nvmessagebus") != nullptr
                    || wcsstr(modPath, L"nvwgf2umx") != nullptr;
            }
        }
    }
    if (isGpuDriverDll) {
        // For GPU driver DLL crashes, log the crash info.
        // For the service process, also capture a minidump for crash analysis
        // (driver upgrade crashes happen in GPU driver DLLs — stale handles).
        // For the GUI process, suppress the dump to avoid crash dump spam.
        BOOL captureDump = g_app.isServiceProcess && info && info->ExceptionRecord && info->ContextRecord;

        char text[2048] = {};
        int len = 0;
        len += StringCchPrintfA(text + len, ARRAY_COUNT(text) - len,
            "\r\n%04u-%02u-%02u %02u:%02u:%02u.%03u CRASH pid=%lu tid=%lu exception=0x%08lX address=%p",
            now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond, now.wMilliseconds,
            GetCurrentProcessId(),
            GetCurrentThreadId(),
            code,
            address);
        if (code == 0xC0000005 && faultAddr) {
            len += StringCchPrintfA(text + len, ARRAY_COUNT(text) - len,
                " fault=%s@%p", faultWrite ? "write" : "read", faultAddr);
        }
        len += StringCchPrintfA(text + len, ARRAY_COUNT(text) - len,
            " source=%s phase=%s serviceProcess=%d deviceRemoved=%d gpuDriverDll=1\r\n",
            g_pendingOperationSource[0] ? g_pendingOperationSource : "<none>",
            g_lastApplyPhase[0] ? g_lastApplyPhase : "<none>",
            g_app.isServiceProcess ? 1 : 0,
            g_app.deviceRemoved ? 1 : 0);
        write_crash_breadcrumb_direct(text);

        if (captureDump) {
            // Capture minidump for service crashes in GPU driver DLLs (driver upgrade crashes).
            // Use FILE_FLAG_WRITE_THROUGH for reliability during crashes.
            char dumpPath[MAX_PATH] = {};
            char dataDir[MAX_PATH] = {};
            if (!crash_artifact_data_dir(dataDir, sizeof(dataDir))) return EXCEPTION_EXECUTE_HANDLER;
            StringCchPrintfA(dumpPath, ARRAY_COUNT(dumpPath),
                "%s\\greencurve_crash_%04u%02u%02u_%02u%02u%02u_%03u_pid%lu.dmp",
                dataDir, now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute,
                now.wSecond, now.wMilliseconds, (unsigned long)GetCurrentProcessId());
            char pathErr[256] = {};
            ensure_parent_directory_for_file(dumpPath, pathErr, sizeof(pathErr));

            HANDLE hDump = gc_CreateFileUtf8(dumpPath, GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr);
            if (hDump != INVALID_HANDLE_VALUE) {
                MINIDUMP_EXCEPTION_INFORMATION mei = {};
                mei.ThreadId = GetCurrentThreadId();
                mei.ExceptionPointers = info;
                mei.ClientPointers = FALSE;
                BOOL dumpOk = MiniDumpWriteDump(
                    GetCurrentProcess(),
                    GetCurrentProcessId(),
                    hDump,
                    green_curve_actionable_minidump_type(),
                    &mei,
                    nullptr,
                    nullptr);
                CloseHandle(hDump);
                if (!dumpOk) {
                    gc_DeleteFileUtf8(dumpPath);
                }
            }
        }
        return EXCEPTION_EXECUTE_HANDLER;
    }

    // Non-GPU-driver-DLL crash: write breadcrumb and dump.
    char text[2048] = {};
    int len = 0;
    len += StringCchPrintfA(text + len, ARRAY_COUNT(text) - len,
        "\r\n%04u-%02u-%02u %02u:%02u:%02u.%03u CRASH pid=%lu tid=%lu exception=0x%08lX address=%p",
        now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond, now.wMilliseconds,
        GetCurrentProcessId(),
        GetCurrentThreadId(),
        code,
        address);
    if (code == 0xC0000005 && faultAddr) {
        len += StringCchPrintfA(text + len, ARRAY_COUNT(text) - len,
            " fault=%s@%p", faultWrite ? "write" : "read", faultAddr);
    }
    len += StringCchPrintfA(text + len, ARRAY_COUNT(text) - len,
        " source=%s phase=%s serviceProcess=%d config=%s\r\n",
        g_pendingOperationSource[0] ? g_pendingOperationSource : "<none>",
        g_lastApplyPhase[0] ? g_lastApplyPhase : "<none>",
        g_app.isServiceProcess ? 1 : 0,
        g_app.configPath[0] ? g_app.configPath : "<unset>");
    if (g_app.fanCurveRuntimeActive || g_app.fanFixedRuntimeActive) {
        len += StringCchPrintfA(text + len, ARRAY_COUNT(text) - len,
            "  fanRuntime: mode=%d curveActive=%d fixedActive=%d fixedPct=%d temp=%d consecutiveFailures=%u lastApplyMs=%llu\r\n",
            g_app.activeFanMode,
            g_app.fanCurveRuntimeActive ? 1 : 0,
            g_app.fanFixedRuntimeActive ? 1 : 0,
            g_app.activeFanFixedPercent,
            g_app.gpuTemperatureValid ? g_app.gpuTemperatureC : -1,
            g_app.fanRuntimeConsecutiveFailures,
            g_app.fanRuntimeLastApplyTickMs);
    }
    write_crash_breadcrumb_direct(text);

    // Write a minidump alongside the text breadcrumb for deeper crash analysis.
    // Capture actionable thread/module/handle/memory-map context without the
    // privacy and size cost of a complete process-memory dump.
    if (info && info->ExceptionRecord && info->ContextRecord) {
        char dumpPath[MAX_PATH] = {};
        char dataDir[MAX_PATH] = {};
        if (!crash_artifact_data_dir(dataDir, sizeof(dataDir))) return EXCEPTION_EXECUTE_HANDLER;
        StringCchPrintfA(dumpPath, ARRAY_COUNT(dumpPath),
            "%s\\greencurve_crash_%04u%02u%02u_%02u%02u%02u_%03u_pid%lu.dmp",
            dataDir, now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute,
            now.wSecond, now.wMilliseconds, (unsigned long)GetCurrentProcessId());
        char pathErr[256] = {};
        ensure_parent_directory_for_file(dumpPath, pathErr, sizeof(pathErr));

        HANDLE hDump = gc_CreateFileUtf8(dumpPath, GENERIC_WRITE, FILE_SHARE_READ, nullptr,
            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr);
        if (hDump != INVALID_HANDLE_VALUE) {
            MINIDUMP_EXCEPTION_INFORMATION mei = {};
            mei.ThreadId = GetCurrentThreadId();
            mei.ExceptionPointers = info;
            mei.ClientPointers = FALSE;
            BOOL dumpOk = MiniDumpWriteDump(
                GetCurrentProcess(),
                GetCurrentProcessId(),
                hDump,
                green_curve_actionable_minidump_type(),
                &mei,
                nullptr,
                nullptr);
            (void)dumpOk; // best-effort; log would fail inside a crash handler
            CloseHandle(hDump);
            if (!dumpOk) {
                gc_DeleteFileUtf8(dumpPath);
            }
        }
    }

    return EXCEPTION_EXECUTE_HANDLER;
}

// VEH-safe logging: does NOT acquire g_debugLogLock (the crashing thread may
// hold it).  Uses OutputDebugStringA only, with a short fixed buffer.
static void debug_log_veh(const char* msg) {
    if (!msg) return;
    OutputDebugStringA(msg);
}

// Write a focused minidump from within the VEH handler.  Must NOT acquire any
// lock that the crashing thread might hold (g_appLock, g_debugLogLock).
// Uses direct file I/O with no heap allocation (stack buffers only).
static void write_veh_minidump(EXCEPTION_POINTERS* info, const WCHAR* modPath) {
    if (!info || !info->ExceptionRecord || !info->ContextRecord) return;

    SYSTEMTIME now = {};
    GetLocalTime(&now);
    char dumpPath[MAX_PATH] = {};
    char dataDir[MAX_PATH] = {};
    if (!resolve_service_machine_data_dir(dataDir, sizeof(dataDir))) return;
    StringCchPrintfA(dumpPath, ARRAY_COUNT(dumpPath),
        "%s\\greencurve_veh_%04u%02u%02u_%02u%02u%02u_%03u_pid%lu.dmp",
        dataDir,
        now.wYear, now.wMonth, now.wDay,
        now.wHour, now.wMinute, now.wSecond, now.wMilliseconds,
        (unsigned long)GetCurrentProcessId());
    ensure_parent_directory_for_file(dumpPath, nullptr, 0);

    HANDLE hDump = gc_CreateFileUtf8(dumpPath, GENERIC_WRITE, FILE_SHARE_READ, nullptr,
        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr);
    if (hDump == INVALID_HANDLE_VALUE) return;

    MINIDUMP_EXCEPTION_INFORMATION mei = {};
    mei.ThreadId = GetCurrentThreadId();
    mei.ExceptionPointers = info;
    mei.ClientPointers = FALSE;
    BOOL dumpOk = MiniDumpWriteDump(
        GetCurrentProcess(),
        GetCurrentProcessId(),
        hDump,
        green_curve_actionable_minidump_type(),
        &mei,
        nullptr,
        nullptr);
    CloseHandle(hDump);
    if (!dumpOk) {
        gc_DeleteFileUtf8(dumpPath);
    }

    // Write companion breadcrumb with crash context
    {
        char text[512] = {};
        StringCchPrintfA(text, ARRAY_COUNT(text),
            "\r\n%04u-%02u-%02u %02u:%02u:%02u VEH MINIDUMP pid=%lu tid=%lu addr=%p module=%ws\r\n",
            now.wYear, now.wMonth, now.wDay,
            now.wHour, now.wMinute, now.wSecond,
            GetCurrentProcessId(), GetCurrentThreadId(),
            info->ExceptionRecord->ExceptionAddress,
            modPath ? modPath : L"<unknown>");
        char breadPath[MAX_PATH] = {};
        StringCchPrintfA(breadPath, ARRAY_COUNT(breadPath),
            "%s\\greencurve_crash.txt", dataDir);
        HANDLE hBread = gc_CreateFileUtf8(breadPath, FILE_APPEND_DATA,
            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
            OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr);
        if (hBread != INVALID_HANDLE_VALUE) {
            DWORD written = 0;
            WriteFile(hBread, text, (DWORD)strlen(text), &written, nullptr);
            FlushFileBuffers(hBread);
            CloseHandle(hBread);
        }
    }
}

// Vectored exception handler — catches nvml.dll access violations at first chance
// so the fan runtime thread survives stale-handle crashes (driver restart without
// device removal notification).
//
// When nvmlDeviceGetTemperature reads from invalid memory inside nvml.dll (after
// a WDDM driver restart, e.g. via restart64.exe), this handler writes a minidump,
// invalidates NVML state, and cleanly terminates the crashing thread via
// ExitThread(0).  It records the crash; the main service loop observes
// crashCount>0 and requests a controlled service-process restart for clean
// driver re-init (see request_service_restart / launch_recovery_thread).
//
// This approach replaces the earlier stack-scanning recovery attempt which was
// unreliable (false positives from data values on the stack that happened to
// look like code addresses).
//
// NOTE: llvm-mingw does not support __try/__except SEH blocks, so a VEH is the
// standard Windows API for exception catching.
static LONG CALLBACK green_curve_vectored_handler(EXCEPTION_POINTERS* info) {
    // Only intercept access violations
    if (!info || !info->ExceptionRecord || !info->ContextRecord)
        return EXCEPTION_CONTINUE_SEARCH;
    if (info->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION)
        return EXCEPTION_CONTINUE_SEARCH;

    // Check if the crash is inside nvml.dll or nvapi64.dll (stale handle after
    // driver restart / driver upgrade / device reconnect)
    void* address = info->ExceptionRecord->ExceptionAddress;
    HMODULE hMod = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            (LPCWSTR)address, &hMod) || !hMod)
        return EXCEPTION_CONTINUE_SEARCH;

    WCHAR modPath[MAX_PATH] = {};
    if (!GetModuleFileNameW(hMod, modPath, MAX_PATH))
        return EXCEPTION_CONTINUE_SEARCH;
    CharLowerW(modPath);
    if (!wcsstr(modPath, L"nvml.dll") && !wcsstr(modPath, L"nvapi64.dll"))
        return EXCEPTION_CONTINUE_SEARCH;

    // Write a focused minidump BEFORE making any state changes — this captures
    // the exact crash context.  Must NOT hold any locks.
    if (g_app.isServiceProcess) {
        write_veh_minidump(info, modPath);
    }

    // Mark NVML as invalid.  Do not call nvmlShutdown() from this crash path:
    // after a driver restart it can hang inside the dead driver instance while
    // the service is trying to recover.
    if (g_app.isServiceProcess) {
        service_close_nvml_without_shutdown();
    } else {
        g_app.nvmlReady = false;
        g_app.nvmlDevice = nullptr;
        if (g_nvml) {
            FreeLibrary(g_nvml);
            g_nvml = nullptr;
        }
        memset(&g_nvml_api, 0, sizeof(g_nvml_api));
    }
    // NOTE: Do NOT FreeLibrary(g_app.hNvApi) here — nvapi_qi() caches function
    // pointers from the DLL in a static local.  Other threads (e.g. the pipe
    // server thread processing a snapshot) may hold references to those cached
    // pointers.  Unloading the DLL from within the VEH causes those pointers
    // to dangle, crashing other threads with access violations in unmapped
    // memory.  NvAPI invalidation happens in the recovery thread (which stops
    // all other threads first).

    // Set the NVML-crashed flag so the snapshot handler knows to skip
    // refresh_global_state() and use cached data instead.  This prevents
    // the pipe server thread from crashing when the next SNAPSHOT request
    // arrives while NVML handles are stale.
    InterlockedExchange(&g_nvmlVhCrashed, 1);
    // Update crash back-off state.  The service main loop observes crashCount>0
    // and requests a controlled process restart for clean driver re-init.
    g_nvmlCrashTickMs = GetTickCount64();
    LONG crashCountNow = InterlockedIncrement(&g_nvmlCrashCount);

    {
        char vehMsg[180] = {};
        StringCchPrintfA(vehMsg, sizeof(vehMsg),
            "VEH: nvml/nvapi access violation at %p handled (crashCount=%ld) — minidump captured; main loop will request service restart\n",
            address, (long)crashCountNow);
        debug_log_veh(vehMsg);
    }

    // The VEH only marks the crash and kills the faulting thread. The main
    // service loop observes crashCount>0 and prepares the nonce-bound process
    // restart. There is no per-event reapply thread to clean up or retry.

    // Terminate the crashing thread via ExitThread(0).  Rsp is adjusted to
    // simulate the return address a CALL would have pushed, maintaining
    // 16-byte alignment through ExitThread's prologue.
    //
    // For the fan runtime thread: the main-loop watchdog recreates it after
    // the recovery thread completes.
    //
    // For the pipe server thread: the pipe watchdog recreates it.  The
    // g_nvmlVhCrashed flag causes the NEW pipe server thread to skip NVML
    // calls in the snapshot handler, so it won't crash a second time.
    HMODULE hK32 = GetModuleHandleA("kernel32");
    if (hK32) {
        FARPROC exitThread = GetProcAddress(hK32, "ExitThread");
        if (exitThread) {
#if defined(__aarch64__) || defined(_M_ARM64)
            // ARM64: PC = target, first arg in X0, SP stays 16-byte aligned
            // (the link register holds the "return" address but ExitThread
            // never returns).
            info->ContextRecord->Pc = (ULONG_PTR)exitThread;
            info->ContextRecord->X[0] = 0; // exit code
            info->ContextRecord->Sp = info->ContextRecord->Sp & ~(ULONG_PTR)15;
#else
            info->ContextRecord->Rip = (ULONG_PTR)exitThread;
            info->ContextRecord->Rcx = 0; // exit code
            info->ContextRecord->Rsp = (info->ContextRecord->Rsp & ~(ULONG_PTR)15) - 8;
#endif
            return EXCEPTION_CONTINUE_EXECUTION;
        }
    }
    return EXCEPTION_CONTINUE_EXECUTION;
}

static void write_error_report_log_for_user_failure(const char* summary, const char* details) {
    char logErr[256] = {};
    if (!write_error_report_log(summary, details, logErr, sizeof(logErr)) && logErr[0]) {
        debug_log("error report log failed: %s\n", logErr);
    }
}

