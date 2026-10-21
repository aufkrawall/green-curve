// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#ifndef GREEN_CURVE_VEH_DRIVER_FAULT_WIN32_H
#define GREEN_CURVE_VEH_DRIVER_FAULT_WIN32_H

// The service's vectored "driver fault" flow, kept free of service globals so
// the native regression fixture (tests/veh_driver_fault_tests.cpp) runs this
// exact code against real access violations, including in a child process
// with hardware shadow stacks forced on.
//
// After a driver update/restart a stale NVML/NvAPI handle can access-violate
// inside the driver's user-mode image.  The service survives that by ending
// ONLY the faulting thread and restarting itself through the nonce-bound
// controlled recovery, which re-applies the active profile.
//
// The faulting thread is ended by calling ExitThread from inside the handler.
// It must NOT be ended by rewriting the context's instruction pointer and
// asking the dispatcher to continue there: the x64 images are /cetcompat, and
// with user shadow stacks enforced the continuation's NtContinue refuses a
// target that is neither on the shadow stack nor a registered EH continuation
// (STATUS_SET_CONTEXT_DENIED), and ntdll then fast-fails the whole process
// (0xC0000409, FAST_FAIL_SET_CONTEXT_DENIED).  That turned every recoverable
// driver update into a service crash, whose ordinary restart hands the GPU
// back to stock instead of replaying the profile.
//
// Calling ExitThread here is sound: ntdll invokes vectored handlers with its
// handler-list lock released (the entry is reference counted instead), so the
// only state left behind is whatever the faulting thread itself held -- the
// same as before, and the reason recovery is a process restart.
//
// Not every thread may be ended.  A thread nobody recreates and nobody watches
// (the service's own main thread, which is also the watchdog, or the SCM
// control dispatcher) would leave a live process that never recovers -- worse
// than the crash, because the SCM restarts a dead service but not a hung one.
// `mayEndCurrentThread` says no for those, and the fault then takes the
// ordinary crash path.

#include <windows.h>
#include <strsafe.h>

// Exit code of a thread ended by this flow; nothing reads it for control flow,
// it only makes the thread's end recognisable in a dump or a debugger.
#define GC_VEH_DRIVER_FAULT_THREAD_EXIT_CODE ((DWORD)EXCEPTION_ACCESS_VIOLATION)

// What the service logs about a claimed fault once the process is healthy
// enough to log (the handler itself must not call debug_log).
struct GcVehDriverFaultRecord {
    DWORD threadId;
    DWORD access;            // ExceptionInformation[0]: 0 read, 1 write, 8 execute
    ULONG_PTR moduleOffset;  // fault address relative to the image base
    ULONG_PTR target;        // ExceptionInformation[1]: the inaccessible address
    char moduleName[40];     // image base name, ASCII-folded, truncated
};

struct GcVehDriverFaultHandler {
    // Whether an access violation inside the image at `modulePath` (as the
    // loader reports it, any case) belongs to this handler.
    bool (*ownsModule)(const WCHAR* modulePath);
    // Called once per claimed fault, on the faulting thread, before it ends.
    // Must not take any lock the faulting thread might hold.
    void (*record)(EXCEPTION_POINTERS* info, const WCHAR* modulePath,
                   const GcVehDriverFaultRecord* fault, void* context);
    void* context;
    // Whether the faulting (calling) thread may be ended.  Null means yes.
    // On no the fault is not claimed and continues as an ordinary crash.
    bool (*mayEndCurrentThread)(void* context);
};

// Loader path of the image containing `address`; false when it is in no image.
static inline bool gc_veh_fault_module(const void* address, HMODULE* moduleOut,
                                       WCHAR* out, DWORD outCount) {
    if (moduleOut) *moduleOut = nullptr;
    if (!out || outCount == 0) return false;
    out[0] = 0;
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            (LPCWSTR)address, &module) || !module) {
        return false;
    }
    if (GetModuleFileNameW(module, out, outCount) == 0) {
        out[0] = 0;
        return false;
    }
    out[outCount - 1] = 0;
    if (moduleOut) *moduleOut = module;
    return true;
}

// Whether this handler claims the exception; on true `moduleOut` and
// `modulePathOut` name the faulting image.  Pure apart from the loader query;
// never changes `info`.
static inline bool gc_veh_driver_fault_is_ours(const EXCEPTION_POINTERS* info,
                                               const GcVehDriverFaultHandler* handler,
                                               HMODULE* moduleOut,
                                               WCHAR* modulePathOut, DWORD modulePathCount) {
    if (moduleOut) *moduleOut = nullptr;
    if (modulePathOut && modulePathCount) modulePathOut[0] = 0;
    if (!handler || !handler->ownsModule || !modulePathOut || modulePathCount == 0)
        return false;
    if (!info || !info->ExceptionRecord || !info->ContextRecord) return false;
    if (info->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION) return false;
    if (!gc_veh_fault_module(info->ExceptionRecord->ExceptionAddress, moduleOut,
            modulePathOut, modulePathCount)) {
        return false;
    }
    return handler->ownsModule(modulePathOut);
}

// Fill `out` from a claimed fault.  Lock- and allocation-free (VEH-safe).
static inline void gc_veh_driver_fault_describe(const EXCEPTION_POINTERS* info,
                                                HMODULE module, const WCHAR* modulePath,
                                                GcVehDriverFaultRecord* out) {
    if (!out) return;
    ZeroMemory(out, sizeof(*out));
    out->threadId = GetCurrentThreadId();
    if (!info || !info->ExceptionRecord) return;
    const EXCEPTION_RECORD* record = info->ExceptionRecord;
    if (record->NumberParameters >= 2) {
        out->access = (DWORD)record->ExceptionInformation[0];
        out->target = record->ExceptionInformation[1];
    }
    if (module && (ULONG_PTR)record->ExceptionAddress >= (ULONG_PTR)module) {
        out->moduleOffset = (ULONG_PTR)record->ExceptionAddress - (ULONG_PTR)module;
    }
    const WCHAR* base = modulePath ? modulePath : L"";
    for (const WCHAR* p = base; *p; ++p) {
        if (*p == L'\\' || *p == L'/') base = p + 1;
    }
    size_t n = 0;
    for (; base[n] && n + 1 < sizeof(out->moduleName); ++n) {
        WCHAR c = base[n];
        out->moduleName[n] = (c >= 0x20 && c < 0x7F) ? (char)c : '?';
    }
    out->moduleName[n] = 0;
}

// One stable single-line description for the debug log.
static inline void gc_veh_driver_fault_format(const GcVehDriverFaultRecord* record,
                                              long crashCount, char* out, size_t outSize) {
    if (!out || outSize == 0) return;
    out[0] = 0;
    if (!record) return;
    const char* access = record->access == 0 ? "read"
        : record->access == 1 ? "write"
        : record->access == 8 ? "execute" : "unknown";
    StringCchPrintfA(out, outSize,
        "VEH driver fault: ended thread tid=%lu at %s+0x%llx (%s of 0x%llx); crashCount=%ld",
        (unsigned long)record->threadId,
        record->moduleName[0] ? record->moduleName : "<unknown>",
        (unsigned long long)record->moduleOffset, access,
        (unsigned long long)record->target, crashCount);
}

// Ends the calling thread.  See the header comment for why this is a direct
// call and never an instruction-pointer rewrite.
[[noreturn]] static inline void gc_veh_exit_faulting_thread() {
    ExitThread(GC_VEH_DRIVER_FAULT_THREAD_EXIT_CODE);
    __builtin_unreachable();
}

// The vectored handler body: EXCEPTION_CONTINUE_SEARCH for anything that is
// not ours or whose thread may not be ended; otherwise record and end the
// faulting thread -- it never returns.
static inline LONG gc_veh_handle_driver_fault(EXCEPTION_POINTERS* info,
                                              const GcVehDriverFaultHandler* handler) {
    HMODULE module = nullptr;
    WCHAR modulePath[MAX_PATH] = {};
    if (!gc_veh_driver_fault_is_ours(info, handler, &module, modulePath, MAX_PATH))
        return EXCEPTION_CONTINUE_SEARCH;
    if (handler->mayEndCurrentThread && !handler->mayEndCurrentThread(handler->context))
        return EXCEPTION_CONTINUE_SEARCH;
    if (handler->record) {
        GcVehDriverFaultRecord fault;
        gc_veh_driver_fault_describe(info, module, modulePath, &fault);
        handler->record(info, modulePath, &fault, handler->context);
    }
    gc_veh_exit_faulting_thread();
}

#endif  // GREEN_CURVE_VEH_DRIVER_FAULT_WIN32_H
