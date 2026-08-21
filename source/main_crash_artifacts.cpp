// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT

#include "log_redaction_policy.h"
//
// main_crash_artifacts.cpp — where a Windows crash leaves evidence.
//
// Split out of main_diagnostics.cpp (which kept the debug log) because the two
// halves answer different questions and the file was outgrowing the size rule.
// Everything here runs while the process is already dying, so it obeys three
// constraints that do not apply anywhere else in the codebase:
//
//   * No lock may be taken.  The crashing thread may already hold g_appLock or
//     g_debugLogLock, so the VEH path in particular must never call debug_log().
//   * No heap allocation.  Stack buffers only.
//   * Path resolution is environment-only — no SHGetKnownFolderPath, which is
//     COM and not safe from an exception filter.
//
// Location policy is crash_artifact_policy.h: artifacts land next to config.ini
// for user-scope processes and in the admin-only machine directory for the
// service, or they are not written at all.  There is no working-directory
// fallback; see the invariants in that header for why.

#include "crash_artifact_policy.h"
#include "fatal_dump_hook.h"

// ---------------------------------------------------------------------------
// Where artifacts go
// ---------------------------------------------------------------------------

// Resolve the directory a crash artifact belongs in, using ONLY environment
// variables and already-cached state.  Callable from the unhandled-exception
// filter and from the vectored handler.
//
// Fail-closed: when nothing resolves this returns false and the caller writes
// nothing.  It deliberately no longer falls back to "." — the current directory
// is %SystemRoot%\System32 for a service, the install directory for a
// shell-launched GUI, and an arbitrary folder for a CLI run, so that fallback
// quietly scattered dumps outside the one place a user is ever told to look
// (and, for the service, outside the admin-only directory that keeps a SYSTEM
// dump from being world-readable).
static bool crash_artifact_data_dir(char* out, size_t outSize) {
    if (!out || outSize == 0) return false;
    out[0] = 0;
    char machineDir[MAX_PATH] = {};
    bool machineResolvable = resolve_service_machine_data_dir(machineDir, sizeof(machineDir)) &&
        gc_crash_dir_is_acceptable(machineDir);

    switch (gc_crash_dir_source(g_app.isServiceProcess, g_userDataDir, machineResolvable)) {
    case GC_CRASH_DIR_MACHINE:
        return SUCCEEDED(StringCchCopyA(out, outSize, machineDir));
    case GC_CRASH_DIR_USER_CONFIG:
        return SUCCEEDED(StringCchCopyA(out, outSize, g_userDataDir)) &&
            gc_crash_dir_is_acceptable(out);
    case GC_CRASH_DIR_USER_ENV: {
        // The cached path was never resolved (a crash before resolve_data_paths,
        // or a resolution failure).  %LOCALAPPDATA% reaches the same directory
        // config.ini uses, without the COM call resolve_data_paths would make.
        char base[MAX_PATH] = {};
        DWORD n = GetEnvironmentVariableA("LOCALAPPDATA", base, ARRAY_COUNT(base));
        if (n == 0 || n >= ARRAY_COUNT(base)) return false;
        if (FAILED(StringCchPrintfA(out, outSize, "%s\\Green Curve", base))) return false;
        return gc_crash_dir_is_acceptable(out);
    }
    case GC_CRASH_DIR_NONE:
    default:
        out[0] = 0;
        return false;
    }
}

// ---------------------------------------------------------------------------
// Rotation (bounded disk usage)
// ---------------------------------------------------------------------------

// Cap the number of minidumps of one kind kept in one directory.  A
// driver-recovery restart loop writes one greencurve_veh_*.dmp per crash cycle
// and a crash loop writes one greencurve_crash_*.dmp per dead process; without
// a cap either could write hundreds and fill the disk.
//
// Ordering compares the EMBEDDED TIMESTAMP (gc_crash_artifact_is_older), not the
// whole filename.  A whole-name comparison is wrong across the two prefixes:
// "greencurve_crash_" sorts before "greencurve_veh_" for every possible date,
// so it would delete the newest terminal crash dump while keeping stale VEH
// dumps forever.  Each prefix is swept against its own budget, so a driver
// upgrade's run of recovered crashes can never evict the terminal dump that
// explains why the process actually died.
//
// Only names this project formats are deleted (gc_crash_artifact_stamp).
static void rotate_crash_dumps_in_dir(const char* dir, const char* prefix, unsigned int maxKeep) {
    if (!dir || !dir[0] || !prefix || !prefix[0]) return;
    char pattern[MAX_PATH] = {};
    if (FAILED(StringCchPrintfA(pattern, ARRAY_COUNT(pattern), "%s\\%s*%s",
            dir, prefix, GC_CRASH_DUMP_SUFFIX))) {
        return;
    }
    for (;;) {
        WIN32_FIND_DATAA fd = {};
        HANDLE h = gc_FindFirstFileUtf8(pattern, &fd);
        if (h == INVALID_HANDLE_VALUE) return;
        unsigned int count = 0;
        char oldest[MAX_PATH] = {};
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            // The wildcard already restricts this to our own prefix, but
            // re-attributing the name keeps the delete honest if the pattern is
            // ever widened, and rejects a hand-renamed file whose ordering
            // cannot be trusted.
            char stamp[GC_CRASH_STAMP_LENGTH + 1] = {};
            if (!gc_crash_artifact_stamp(fd.cFileName, stamp)) continue;
            count++;
            if (oldest[0] == 0 || gc_crash_artifact_is_older(fd.cFileName, oldest)) {
                StringCchCopyA(oldest, ARRAY_COUNT(oldest), fd.cFileName);
            }
        } while (gc_FindNextFileUtf8(h, &fd));
        FindClose(h);
        if (!gc_crash_rotation_needed(count, maxKeep) || oldest[0] == 0) return;
        char victim[MAX_PATH] = {};
        if (FAILED(StringCchPrintfA(victim, ARRAY_COUNT(victim), "%s\\%s", dir, oldest))) return;
        if (!gc_DeleteFileUtf8(victim)) return; // stop on failure to avoid an infinite loop
        debug_log("crash artifacts: rotated out %s (%u present, keeping %u)\n",
            oldest, count, maxKeep);
    }
}

// Truncate the append-only crash breadcrumb once it exceeds its cap.  It is one
// short line per crash, so this only trips on a sustained restart loop; the
// dumps beside it carry the detail, which is why this truncates instead of
// keeping a second generation.
static void rotate_crash_breadcrumb_in_dir(const char* dir) {
    if (!dir || !dir[0]) return;
    char path[MAX_PATH] = {};
    if (FAILED(StringCchPrintfA(path, ARRAY_COUNT(path), "%s\\%s", dir, GC_CRASH_BREADCRUMB_NAME))) return;
    WIN32_FILE_ATTRIBUTE_DATA info = {};
    if (!gc_GetFileAttributesExUtf8(path, GetFileExInfoStandard, &info)) return;
    unsigned long long size =
        ((unsigned long long)info.nFileSizeHigh << 32) | (unsigned long long)info.nFileSizeLow;
    if (!gc_crash_breadcrumb_needs_reset(size)) return;
    HANDLE h = gc_CreateFileUtf8(path, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
        TRUNCATE_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    CloseHandle(h);
    debug_log("crash artifacts: truncated %s at %llu bytes\n", GC_CRASH_BREADCRUMB_NAME, size);
}

// One rotation pass over whichever directory THIS process writes crash
// artifacts to.  Called once per fresh process, which for the service is once
// per restart cycle, so it bounds an entire recovery loop.  The GUI needs it
// just as much: its dumps land in the user data directory and nothing capped
// them before.
static void rotate_crash_artifacts_for_process() {
    char dir[MAX_PATH] = {};
    if (!crash_artifact_data_dir(dir, sizeof(dir))) {
        debug_log("crash artifacts: no writable artifact directory resolved; "
            "dumps are disabled for this process (serviceProcess=%d userDataDir=%s)\n",
            g_app.isServiceProcess ? 1 : 0,
            g_userDataDir[0] ? g_userDataDir : "<unset>");
        return;
    }
    debug_log("crash artifacts: directory=%s keepPerKind=%d\n", dir, GC_CRASH_ARTIFACT_MAX_KEEP);
    rotate_crash_dumps_in_dir(dir, GC_CRASH_DUMP_PREFIX, GC_CRASH_ARTIFACT_MAX_KEEP);
    rotate_crash_dumps_in_dir(dir, GC_VEH_DUMP_PREFIX, GC_CRASH_ARTIFACT_MAX_KEEP);
    rotate_crash_breadcrumb_in_dir(dir);
}

// ---------------------------------------------------------------------------
// Writers
// ---------------------------------------------------------------------------

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
    StringCchPrintfA(path, ARRAY_COUNT(path), "%s\\%s", dataDir, GC_CRASH_BREADCRUMB_NAME);
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

// Write one minidump for `info` into the process's artifact directory.
//
// Single implementation for all four crash paths (unhandled exception, GPU
// driver DLL, VEH-recovered, fast-fail).  They had three near-identical copies
// of this block, which is how the VEH copy ended up on a different directory
// rule from the others; one writer means one rule.
//
// `prefix` is GC_CRASH_DUMP_PREFIX or GC_VEH_DUMP_PREFIX.  Returns true when a
// dump file was left on disk; a failed MiniDumpWriteDump deletes its own
// zero-length file rather than leaving a decoy that cdb cannot open.
static bool write_crash_minidump(EXCEPTION_POINTERS* info, const char* prefix,
                                 const SYSTEMTIME* now, char* pathOut, size_t pathOutSize) {
    if (pathOut && pathOutSize) pathOut[0] = 0;
    if (!info || !info->ExceptionRecord || !info->ContextRecord || !now) return false;

    char dataDir[MAX_PATH] = {};
    if (!crash_artifact_data_dir(dataDir, sizeof(dataDir))) return false;

    char dumpPath[MAX_PATH] = {};
    if (FAILED(StringCchPrintfA(dumpPath, ARRAY_COUNT(dumpPath),
            "%s\\%s%04u%02u%02u_%02u%02u%02u_%03u_pid%lu%s",
            dataDir, prefix,
            now->wYear, now->wMonth, now->wDay, now->wHour, now->wMinute,
            now->wSecond, now->wMilliseconds, (unsigned long)GetCurrentProcessId(),
            GC_CRASH_DUMP_SUFFIX))) {
        return false;
    }
    char pathErr[256] = {};
    ensure_parent_directory_for_file(dumpPath, pathErr, sizeof(pathErr));

    HANDLE hDump = gc_CreateFileUtf8(dumpPath, GENERIC_WRITE, FILE_SHARE_READ, nullptr,
        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr);
    if (hDump == INVALID_HANDLE_VALUE) return false;

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
        return false;
    }
    if (pathOut && pathOutSize) StringCchCopyA(pathOut, pathOutSize, dumpPath);
    return true;
}

// ---------------------------------------------------------------------------
// Unhandled exception filter
// ---------------------------------------------------------------------------

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

    // Record whether the crash is inside a GPU driver DLL (nvml.dll, nvapi64,
    // NvMessageBus).  Those are caused by driver updates / stale handles rather
    // than by our own code, so the breadcrumb says so — but the dump is still
    // written.  It used to be suppressed for the GUI to avoid filling the disk
    // on every driver update; rotation now bounds that structurally
    // (GC_CRASH_ARTIFACT_MAX_KEEP per kind), and suppressing the dump removed
    // exactly the evidence needed to tell "the driver died under us" apart from
    // "we passed the driver a handle we had already invalidated".
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

    // The dump is written FIRST so that a breadcrumb-write failure (full disk,
    // revoked ACL) cannot cost the dump, and so the breadcrumb can name the file
    // that was actually produced.
    char dumpPath[MAX_PATH] = {};
    bool dumped = write_crash_minidump(info, GC_CRASH_DUMP_PREFIX, &now, dumpPath, sizeof(dumpPath));

    char text[2048] = {};
    size_t len = 0;
    len = gc_appendf(text, ARRAY_COUNT(text), len,
        "\r\n%04u-%02u-%02u %02u:%02u:%02u.%03u CRASH pid=%lu tid=%lu exception=0x%08lX address=%p",
        now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond, now.wMilliseconds,
        GetCurrentProcessId(),
        GetCurrentThreadId(),
        code,
        address);
    if (code == 0xC0000005 && faultAddr) {
        len = gc_appendf(text, ARRAY_COUNT(text), len,
            " fault=%s@%p", faultWrite ? "write" : "read", faultAddr);
    }
    char crashConfigToken[32] = {};
    gc_log_path_token(g_app.configPath, crashConfigToken,
                      sizeof(crashConfigToken));
    len = gc_appendf(text, ARRAY_COUNT(text), len,
        " source=%s phase=%s serviceProcess=%d deviceRemoved=%d gpuDriverDll=%d config=%s",
        g_pendingOperationSource[0] ? g_pendingOperationSource : "<none>",
        g_lastApplyPhase[0] ? g_lastApplyPhase : "<none>",
        g_app.isServiceProcess ? 1 : 0,
        g_app.deviceRemoved ? 1 : 0,
        isGpuDriverDll ? 1 : 0,
        crashConfigToken);
    len = gc_appendf(text, ARRAY_COUNT(text), len,
        " version=%s build=%lu dump=%s\r\n",
        APP_VERSION,
        (unsigned long)APP_BUILD_NUMBER,
        dumped ? dumpPath : "<none>");
    if (g_app.fanCurveRuntimeActive || g_app.fanFixedRuntimeActive) {
        // Terminal append: the cursor is deliberately dropped rather than
        // stored, so adding another append below is a compile-time reminder to
        // thread `len` through it instead of silently overwriting this text.
        (void)gc_appendf(text, ARRAY_COUNT(text), len,
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

    return EXCEPTION_EXECUTE_HANDLER;
}

// ---------------------------------------------------------------------------
// Fast-fail / stack-smash reporter (see fatal_dump_hook.h)
// ---------------------------------------------------------------------------

// Report a failure that never produced an EXCEPTION_RECORD and is about to
// terminate the process uncatchably.  Synthesises the record from the live
// register context, so the dump opens in cdb/WinDbg exactly like any other and
// `.ecxr` lands on the frame that detected the failure.
//
// Called with the one-shot guard in cfg_glue.cpp already taken, so this runs at
// most once per process and never re-enters itself.
static void green_curve_report_fatal_dump(unsigned long reason, const char* label) {
    SYSTEMTIME now = {};
    GetLocalTime(&now);

    CONTEXT context = {};
    RtlCaptureContext(&context);

    EXCEPTION_RECORD record = {};
    record.ExceptionCode = (DWORD)reason;
    record.ExceptionFlags = EXCEPTION_NONCONTINUABLE;
    record.ExceptionAddress = __builtin_return_address(0);

    EXCEPTION_POINTERS pointers = {};
    pointers.ExceptionRecord = &record;
    pointers.ContextRecord = &context;

    char dumpPath[MAX_PATH] = {};
    bool dumped = write_crash_minidump(&pointers, GC_CRASH_DUMP_PREFIX, &now, dumpPath, sizeof(dumpPath));

    char text[1024] = {};
    size_t len = 0;
    len = gc_appendf(text, ARRAY_COUNT(text), len,
        "\r\n%04u-%02u-%02u %02u:%02u:%02u.%03u FATAL pid=%lu tid=%lu reason=0x%08lX address=%p",
        now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond, now.wMilliseconds,
        GetCurrentProcessId(),
        GetCurrentThreadId(),
        reason,
        record.ExceptionAddress);
    // Terminal append; see the note in the unhandled-exception filter above.
    (void)gc_appendf(text, ARRAY_COUNT(text), len,
        " detail=\"%s\" source=%s phase=%s serviceProcess=%d version=%s build=%lu dump=%s\r\n",
        label ? label : "<none>",
        g_pendingOperationSource[0] ? g_pendingOperationSource : "<none>",
        g_lastApplyPhase[0] ? g_lastApplyPhase : "<none>",
        g_app.isServiceProcess ? 1 : 0,
        APP_VERSION,
        (unsigned long)APP_BUILD_NUMBER,
        dumped ? dumpPath : "<none>");
    write_crash_breadcrumb_direct(text);
}

// Install every crash reporter this process needs.  One entry point so the GUI,
// the service and the restart helper cannot drift apart on which handlers they
// have — they did, and the helper spent a release writing nothing at all.
//
// `installVectoredNvmlRecovery` is true only for the service: the vectored
// handler RESUMES execution after an nvml/nvapi access violation, which is a
// recovery the service is built for (it restarts itself cleanly afterwards) and
// the GUI is not.
static void install_crash_handlers(bool installVectoredNvmlRecovery) {
    SetUnhandledExceptionFilter(green_curve_unhandled_exception_filter);
    gc_set_fatal_dump_hook(green_curve_report_fatal_dump);
    if (installVectoredNvmlRecovery) {
        AddVectoredExceptionHandler(1, green_curve_vectored_handler);
    }
}

// ---------------------------------------------------------------------------
// Vectored handler (service only): survive an nvml/nvapi stale-handle fault
// ---------------------------------------------------------------------------

// VEH-safe logging: does NOT acquire g_debugLogLock (the crashing thread may
// hold it).  Uses OutputDebugStringA only, with a short fixed buffer.
static void debug_log_veh(const char* msg) {
    if (!msg) return;
    OutputDebugStringA(msg);
}

// Write the VEH minidump plus its companion breadcrumb.  Must NOT acquire any
// lock that the crashing thread might hold (g_appLock, g_debugLogLock).
static void write_veh_minidump(EXCEPTION_POINTERS* info, const WCHAR* modPath) {
    SYSTEMTIME now = {};
    GetLocalTime(&now);
    char dumpPath[MAX_PATH] = {};
    bool dumped = write_crash_minidump(info, GC_VEH_DUMP_PREFIX, &now, dumpPath, sizeof(dumpPath));

    char text[768] = {};
    gc_appendf(text, ARRAY_COUNT(text), 0,
        "\r\n%04u-%02u-%02u %02u:%02u:%02u.%03u VEH MINIDUMP pid=%lu tid=%lu addr=%p module=%ws dump=%s\r\n",
        now.wYear, now.wMonth, now.wDay,
        now.wHour, now.wMinute, now.wSecond, now.wMilliseconds,
        GetCurrentProcessId(), GetCurrentThreadId(),
        info && info->ExceptionRecord ? info->ExceptionRecord->ExceptionAddress : nullptr,
        modPath ? modPath : L"<unknown>",
        dumped ? dumpPath : "<none>");
    write_crash_breadcrumb_direct(text);
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
