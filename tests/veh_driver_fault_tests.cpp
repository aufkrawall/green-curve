// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// Native fixture for the service's vectored driver-fault flow
// (veh_driver_fault_win32.h) and what follows a claimed fault
// (service_poisoned_runtime_exit_plan), assertion codes 7100-7139.
//
// The claim decision and the log record run against synthesised exception
// records.  The flow itself runs against REAL access violations on real
// threads: in this process, and on x64 in a child process started with user
// shadow stacks and context-IP validation forced on -- the configuration the
// /cetcompat service runs under.  A second child runs the pre-fix handler
// (instruction pointer rewritten to ExitThread) as a negative control: it must
// die with 0xC0000409, which proves the child really enforces the rule the
// production flow has to respect.  Hosts without CET hardware report so and
// skip only the child half.  A third child proves that a thread the handler
// may not end (the service's main/dispatcher threads) reaches the ordinary
// crash path instead of being ended or hanging.
//
// The bounded waits below only turn a regression (a hang) into a failure code;
// no assertion depends on how long anything takes.
//
// The exit-plan policy runs on every host; everything else is Windows only.

#include <stdio.h>
#include <string.h>

#include "service_recovery_policy.h"

// What a poisoned runtime does next (portable policy).
static int veh_exit_plan_tests() {
    const ServiceRestartSnapshotState states[] = {
        SERVICE_RESTART_SNAPSHOT_ABSENT, SERVICE_RESTART_SNAPSHOT_PRESENT,
        SERVICE_RESTART_SNAPSHOT_UNKNOWN };
    // 7130: an uncorroborated poison always stops without replay, latched.
    for (ServiceRestartSnapshotState state : states) {
        if (service_poisoned_runtime_exit_plan(false, state, false) !=
                SERVICE_POISONED_EXIT_LATCH_AND_STOP ||
            service_poisoned_runtime_exit_plan(false, state, true) !=
                SERVICE_POISONED_EXIT_LATCH_AND_STOP) return 7130;
    }
    // 7131: a driver failure with intent on disk restarts through the helper.
    if (service_poisoned_runtime_exit_plan(true, SERVICE_RESTART_SNAPSHOT_PRESENT, false) !=
            SERVICE_POISONED_EXIT_CONTROLLED_RESTART ||
        service_poisoned_runtime_exit_plan(true, SERVICE_RESTART_SNAPSHOT_PRESENT, true) !=
            SERVICE_POISONED_EXIT_CONTROLLED_RESTART) return 7131;
    // 7132: an undeterminable snapshot still tries; preparation re-proves it.
    if (service_poisoned_runtime_exit_plan(true, SERVICE_RESTART_SNAPSHOT_UNKNOWN, false) !=
            SERVICE_POISONED_EXIT_CONTROLLED_RESTART) return 7132;
    // 7133: nothing to restore is a plain restart, NOT a lockout: latching here
    // blocked the next logon's profile after a driver update without intent.
    if (service_poisoned_runtime_exit_plan(true, SERVICE_RESTART_SNAPSHOT_ABSENT, false) !=
            SERVICE_POISONED_EXIT_ORDINARY_RESTART) return 7133;
    // 7134: live intent without a snapshot cannot be restored: stay fail-closed.
    if (service_poisoned_runtime_exit_plan(true, SERVICE_RESTART_SNAPSHOT_ABSENT, true) !=
            SERVICE_POISONED_EXIT_LATCH_AND_STOP) return 7134;

    // The other way a driver restart reaches recovery: the selected GPU's
    // exact-device STARTED notification.
    // 7135: a driver update stops and starts the device without removing it;
    // with applied intent that alone is recovery (it used to be ignored).
    if (service_selected_gpu_arrival_action(false, true, false) != SERVICE_GPU_ARRIVAL_RECOVER)
        return 7135;
    // 7136: not while this fresh process's own recovery write is pending (the
    // arrival is its readiness cue), and not without intent.
    if (service_selected_gpu_arrival_action(false, true, true) != SERVICE_GPU_ARRIVAL_NONE ||
        service_selected_gpu_arrival_action(false, false, false) != SERVICE_GPU_ARRIVAL_NONE)
        return 7136;
    // 7137: a removal cycle keeps its rule.
    if (service_selected_gpu_arrival_action(true, true, false) != SERVICE_GPU_ARRIVAL_RECOVER ||
        service_selected_gpu_arrival_action(true, true, true) != SERVICE_GPU_ARRIVAL_RECOVER ||
        service_selected_gpu_arrival_action(true, false, false) !=
            SERVICE_GPU_ARRIVAL_ABANDON_EVIDENCE) return 7137;
    return 0;
}

#if defined(_WIN32)

// The mitigation-policy API and its CET constants are Windows 10 declarations;
// this translation unit includes <windows.h> first, so it may raise the floor.
#undef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#include "veh_driver_fault_win32.h"

#if defined(__SANITIZE_ADDRESS__)
#define GC_VEH_FIXTURE_ASAN 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define GC_VEH_FIXTURE_ASAN 1
#endif
#endif

namespace {

// A hang detector, never a pass condition.
constexpr DWORD kHangMs = 10000;

constexpr const char* kChildFlag = "--veh-driver-fault-child";
// Child exit codes besides 0 (pass) and a 71xx failure code.
constexpr int kChildNoCet = 3;
constexpr int kChildLegacySurvived = 4;
// The declined child's unhandled-exception filter exits with this.
constexpr int kChildDeclinedReachedCrashPath = 0x7108;
// What ntdll's FAST_FAIL_SET_CONTEXT_DENIED ends a process with.
constexpr DWORD kFastFailExitCode = 0xC0000409;

// The address the faulting thread reads.  Volatile, so the load is emitted as
// written rather than folded away as undefined behaviour.
volatile ULONG_PTR g_faultTarget = 0x10;

WCHAR g_selfPath[MAX_PATH] = {};

bool owns_self(const WCHAR* modulePath) {
    return modulePath && _wcsicmp(modulePath, g_selfPath) == 0;
}
bool owns_nothing(const WCHAR*) { return false; }

struct FaultSink {
    volatile LONG calls;
    GcVehDriverFaultRecord first;
    WCHAR firstPath[MAX_PATH];
};
FaultSink g_sink = {};

void record_fault(EXCEPTION_POINTERS*, const WCHAR* modulePath,
                  const GcVehDriverFaultRecord* fault, void* context) {
    FaultSink* sink = (FaultSink*)context;
    if (InterlockedIncrement(&sink->calls) == 1 && fault) {
        sink->first = *fault;
        lstrcpynW(sink->firstPath, modulePath ? modulePath : L"", MAX_PATH);
    }
}

const GcVehDriverFaultHandler kFixtureHandler = { owns_self, record_fault, &g_sink, nullptr };

int g_refuseThreadEndCalls = 0;
void* g_refuseThreadEndContext = nullptr;
bool refuse_thread_end(void* context) {
    ++g_refuseThreadEndCalls;
    g_refuseThreadEndContext = context;
    return false;
}

// The declined child: a handler that owns the fault but may not end the thread.
FaultSink g_declinedSink = {};
const GcVehDriverFaultHandler kDeclinedHandler = {
    owns_self, record_fault, &g_declinedSink, refuse_thread_end };
LONG CALLBACK declined_vectored_handler(EXCEPTION_POINTERS* info) {
    return gc_veh_handle_driver_fault(info, &kDeclinedHandler);
}
LONG WINAPI declined_crash_filter(EXCEPTION_POINTERS*) {
    ExitProcess((UINT)kChildDeclinedReachedCrashPath);
}

// ntdll keeps an entry whose handler ended its thread referenced, so a removed
// handler may still be called; it only acts while armed.
volatile LONG g_armed = 0;

LONG CALLBACK fixture_vectored_handler(EXCEPTION_POINTERS* info) {
    if (InterlockedCompareExchange(&g_armed, 0, 0) == 0) return EXCEPTION_CONTINUE_SEARCH;
    return gc_veh_handle_driver_fault(info, &kFixtureHandler);
}

volatile LONG g_ranPastFault = 0;
volatile DWORD g_faultingThreadId = 0;

DWORD WINAPI faulting_thread(void*) {
    g_faultingThreadId = GetCurrentThreadId();
    ULONG_PTR value = *(volatile ULONG_PTR*)g_faultTarget;
    // Never reached: the flow ends this thread at the faulting load.
    InterlockedExchange(&g_ranPastFault, 1);
    return (DWORD)value;
}

bool resolve_self_path() {
    DWORD n = GetModuleFileNameW(nullptr, g_selfPath, MAX_PATH);
    return n > 0 && n < MAX_PATH;
}

// 7107 body: a synthesised owned fault on a thread the hook refuses to end.
DWORD WINAPI declined_claim_thread(void*) {
    CONTEXT context = {};
    EXCEPTION_RECORD record = {};
    record.ExceptionCode = EXCEPTION_ACCESS_VIOLATION;
    record.ExceptionAddress = (void*)&faulting_thread;
    EXCEPTION_POINTERS info = { &record, &context };
    FaultSink declinedSink = {};
    GcVehDriverFaultHandler declines = {
        owns_self, record_fault, &declinedSink, refuse_thread_end };
    g_refuseThreadEndCalls = 0;
    g_refuseThreadEndContext = nullptr;
    bool ok = gc_veh_handle_driver_fault(&info, &declines) == EXCEPTION_CONTINUE_SEARCH &&
        declinedSink.calls == 0 && g_refuseThreadEndCalls == 1 &&
        g_refuseThreadEndContext == &declinedSink;
    return ok ? 0 : 7107;
}

// Synthesised records: nothing is raised, so a rejection cannot kill us.
int claim_decision_tests() {
    CONTEXT context = {};
    context.ContextFlags = CONTEXT_FULL;
    EXCEPTION_RECORD record = {};
    record.ExceptionCode = EXCEPTION_ACCESS_VIOLATION;
    record.ExceptionAddress = (void*)&faulting_thread;
    record.NumberParameters = 2;
    record.ExceptionInformation[0] = 1;
    record.ExceptionInformation[1] = 0x20;
    EXCEPTION_POINTERS info = { &record, &context };
    GcVehDriverFaultHandler ownsAll = { owns_self, nullptr, nullptr, nullptr };
    GcVehDriverFaultHandler ownsNone = { owns_nothing, nullptr, nullptr, nullptr };
    GcVehDriverFaultHandler noPredicate = { nullptr, nullptr, nullptr, nullptr };
    HMODULE module = nullptr;
    WCHAR path[MAX_PATH] = {};

    // 7100: incomplete input is never claimed.
    EXCEPTION_POINTERS noRecord = { nullptr, &context };
    EXCEPTION_POINTERS noContext = { &record, nullptr };
    if (gc_veh_driver_fault_is_ours(nullptr, &ownsAll, &module, path, MAX_PATH) ||
        gc_veh_driver_fault_is_ours(&info, nullptr, &module, path, MAX_PATH) ||
        gc_veh_driver_fault_is_ours(&info, &noPredicate, &module, path, MAX_PATH) ||
        gc_veh_driver_fault_is_ours(&noRecord, &ownsAll, &module, path, MAX_PATH) ||
        gc_veh_driver_fault_is_ours(&noContext, &ownsAll, &module, path, MAX_PATH) ||
        gc_veh_driver_fault_is_ours(&info, &ownsAll, &module, nullptr, MAX_PATH) ||
        gc_veh_handle_driver_fault(nullptr, &ownsAll) != EXCEPTION_CONTINUE_SEARCH ||
        gc_veh_handle_driver_fault(&info, nullptr) != EXCEPTION_CONTINUE_SEARCH) return 7100;

    // 7101: only access violations; a breakpoint in an owned image is not ours.
    record.ExceptionCode = EXCEPTION_BREAKPOINT;
    if (gc_veh_driver_fault_is_ours(&info, &ownsAll, &module, path, MAX_PATH) ||
        gc_veh_handle_driver_fault(&info, &ownsAll) != EXCEPTION_CONTINUE_SEARCH) return 7101;
    record.ExceptionCode = EXCEPTION_ACCESS_VIOLATION;

    // 7102: a fault outside every image (heap) is not ours.
    void* heap = HeapAlloc(GetProcessHeap(), 0, 64);
    if (!heap) return 7102;
    record.ExceptionAddress = heap;
    bool heapClaimed = gc_veh_driver_fault_is_ours(&info, &ownsAll, &module, path, MAX_PATH);
    HeapFree(GetProcessHeap(), 0, heap);
    if (heapClaimed || module || path[0]) return 7102;

    // 7103: the predicate decides; a claim names the image; `info` is untouched.
    record.ExceptionAddress = (void*)&faulting_thread;
    CONTEXT before = context;
    EXCEPTION_RECORD recordBefore = record;
    if (gc_veh_driver_fault_is_ours(&info, &ownsNone, &module, path, MAX_PATH) ||
        gc_veh_handle_driver_fault(&info, &ownsNone) != EXCEPTION_CONTINUE_SEARCH) return 7103;
    if (!gc_veh_driver_fault_is_ours(&info, &ownsAll, &module, path, MAX_PATH) ||
        module != GetModuleHandleW(nullptr) || _wcsicmp(path, g_selfPath) != 0 ||
        memcmp(&before, &context, sizeof(context)) != 0 ||
        memcmp(&recordBefore, &record, sizeof(record)) != 0) return 7103;

    // 7104: the log record carries access, target, image-relative offset,
    // ASCII base name and the reporting thread.
    GcVehDriverFaultRecord fault;
    gc_veh_driver_fault_describe(&info, module, L"C:\\Windows\\System32\\DriverStore\\nvml.dll", &fault);
    if (fault.threadId != GetCurrentThreadId() || fault.access != 1 || fault.target != 0x20 ||
        fault.moduleOffset != (ULONG_PTR)&faulting_thread - (ULONG_PTR)module ||
        strcmp(fault.moduleName, "nvml.dll") != 0) return 7104;
    // A record without parameters, image or ASCII name degrades, never overruns.
    EXCEPTION_RECORD bare = {};
    bare.ExceptionCode = EXCEPTION_ACCESS_VIOLATION;
    EXCEPTION_POINTERS bareInfo = { &bare, &context };
    gc_veh_driver_fault_describe(&bareInfo, nullptr,
        L"D:/x/\u00e9vil_name_that_is_much_longer_than_forty_characters.dll", &fault);
    if (fault.access != 0 || fault.target != 0 || fault.moduleOffset != 0 ||
        fault.moduleName[0] != '?' || strlen(fault.moduleName) != sizeof(fault.moduleName) - 1)
        return 7104;

    // 7105: one stable line.
    GcVehDriverFaultRecord known = {};
    known.threadId = 4242;
    known.access = 0;
    known.moduleOffset = 0x1a2b;
    known.target = 0x10;
    lstrcpynA(known.moduleName, "nvml.dll", sizeof(known.moduleName));
    char line[224] = {};
    gc_veh_driver_fault_format(&known, 1, line, sizeof(line));
    if (strcmp(line, "VEH driver fault: ended thread tid=4242 at nvml.dll+0x1a2b "
                     "(read of 0x10); crashCount=1") != 0) return 7105;
    // 7106: unknown access and a missing name are spelled out, not guessed.
    known.access = 7;
    known.moduleName[0] = 0;
    gc_veh_driver_fault_format(&known, 2, line, sizeof(line));
    if (strstr(line, "<unknown>+0x1a2b (unknown of 0x10); crashCount=2") == nullptr) return 7106;

    // 7107: a thread the handler may not end is not claimed and nothing is
    // recorded; the hook sees the handler's context.  Run on a worker thread:
    // a regression ends the calling thread, which must not be this one.
    HANDLE worker = CreateThread(nullptr, 0, declined_claim_thread, nullptr, 0, nullptr);
    if (!worker) return 7107;
    DWORD joined = WaitForSingleObject(worker, kHangMs);
    DWORD workerExit = 7107;
    GetExitCodeThread(worker, &workerExit);
    CloseHandle(worker);
    if (joined != WAIT_OBJECT_0 || workerExit != 0) return 7107;
    return 0;
}

// Runs one real fault on a fresh thread and checks what the flow did to it.
int run_one_real_fault(LONG expectedCalls) {
    InterlockedExchange(&g_ranPastFault, 0);
    g_faultingThreadId = 0;
    HANDLE thread = CreateThread(nullptr, 0, faulting_thread, nullptr, 0, nullptr);
    if (!thread) return 7110;
    DWORD joined = WaitForSingleObject(thread, kHangMs);
    DWORD exitCode = 0;
    BOOL gotExit = GetExitCodeThread(thread, &exitCode);
    CloseHandle(thread);
    // 7110: the faulting thread ended, with the flow's exit code.
    if (joined != WAIT_OBJECT_0 || !gotExit ||
        exitCode != GC_VEH_DRIVER_FAULT_THREAD_EXIT_CODE) return 7110;
    // 7111: recorded exactly once per fault.
    if (InterlockedCompareExchange(&g_sink.calls, 0, 0) != expectedCalls) return 7111;
    // 7113: nothing after the faulting load ran.
    if (InterlockedCompareExchange(&g_ranPastFault, 0, 0) != 0) return 7113;
    return 0;
}

int real_fault_tests() {
    memset(&g_sink, 0, sizeof(g_sink));
    PVOID registration = AddVectoredExceptionHandler(1, fixture_vectored_handler);
    if (!registration) return 7110;
    InterlockedExchange(&g_armed, 1);
    int failure = run_one_real_fault(1);
    // 7112: the record names the faulting thread, the read of the target and
    // this image.
    if (!failure) {
        HMODULE self = GetModuleHandleW(nullptr);
        const WCHAR* base = g_selfPath;
        for (const WCHAR* p = g_selfPath; *p; ++p) {
            if (*p == L'\\' || *p == L'/') base = p + 1;
        }
        char expectedName[sizeof(g_sink.first.moduleName)] = {};
        WideCharToMultiByte(CP_ACP, 0, base, -1, expectedName, (int)sizeof(expectedName) - 1,
                            nullptr, nullptr);
        ULONG_PTR functionOffset = (ULONG_PTR)&faulting_thread - (ULONG_PTR)self;
        if (g_sink.first.threadId == 0 || g_sink.first.threadId != g_faultingThreadId ||
            g_sink.first.access != 0 || g_sink.first.target != g_faultTarget ||
            g_sink.first.moduleOffset < functionOffset ||
            g_sink.first.moduleOffset >= functionOffset + 4096 ||
            _stricmp(g_sink.first.moduleName, expectedName) != 0 ||
            _wcsicmp(g_sink.firstPath, g_selfPath) != 0) failure = 7112;
    }
    // 7114: the process carries on, and the next fault is handled the same way.
    if (!failure && run_one_real_fault(2) != 0) failure = 7114;
    InterlockedExchange(&g_armed, 0);
    RemoveVectoredExceptionHandler(registration);
    return failure;
}

bool user_shadow_stack_enforced() {
    PROCESS_MITIGATION_USER_SHADOW_STACK_POLICY policy = {};
    return GetProcessMitigationPolicy(GetCurrentProcess(), ProcessUserShadowStackPolicy,
                                      &policy, sizeof(policy)) &&
        policy.EnableUserShadowStack;
}

// Starts this executable as a child in `mode`; with `forceCet`, user shadow
// stacks in strict mode and context-IP validation are forced on.  False when
// the child could not be started (with `forceCet`: the host refuses the
// policy, i.e. no CET support in the OS).
bool run_child(const char* mode, bool forceCet, DWORD* exitCodeOut) {
    WCHAR commandLine[MAX_PATH + 96] = {};
    if (FAILED(StringCchPrintfW(commandLine, ARRAYSIZE(commandLine), L"\"%ls\" %hs %hs",
            g_selfPath, kChildFlag, mode))) return false;
    SIZE_T attributeSize = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attributeSize);
    BYTE attributeStorage[256] = {};
    if (attributeSize == 0 || attributeSize > sizeof(attributeStorage)) return false;
    LPPROC_THREAD_ATTRIBUTE_LIST attributes = (LPPROC_THREAD_ATTRIBUTE_LIST)attributeStorage;
    if (!InitializeProcThreadAttributeList(attributes, 1, 0, &attributeSize)) return false;
    DWORD64 policy[2] = { 0,
        PROCESS_CREATION_MITIGATION_POLICY2_CET_USER_SHADOW_STACKS_STRICT_MODE |
        PROCESS_CREATION_MITIGATION_POLICY2_USER_CET_SET_CONTEXT_IP_VALIDATION_ALWAYS_ON };
    bool ok = !forceCet ||
        UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_MITIGATION_POLICY,
                                  policy, sizeof(policy), nullptr, nullptr) != FALSE;
    STARTUPINFOEXW startup = {};
    startup.StartupInfo.cb = sizeof(startup);
    startup.lpAttributeList = attributes;
    PROCESS_INFORMATION process = {};
    ok = ok && CreateProcessW(g_selfPath, commandLine, nullptr, nullptr, FALSE,
                              EXTENDED_STARTUPINFO_PRESENT, nullptr, nullptr,
                              &startup.StartupInfo, &process) != FALSE;
    DeleteProcThreadAttributeList(attributes);
    if (!ok) return false;
    CloseHandle(process.hThread);
    DWORD waited = WaitForSingleObject(process.hProcess, kHangMs);
    if (waited != WAIT_OBJECT_0) TerminateProcess(process.hProcess, 0xDEAD);
    DWORD exitCode = 0xDEAD;
    GetExitCodeProcess(process.hProcess, &exitCode);
    CloseHandle(process.hProcess);
    *exitCodeOut = waited == WAIT_OBJECT_0 ? exitCode : 0xDEAD;
    return true;
}

#if defined(_M_X64) || defined(__x86_64__)
// The handler as it was before the fix: rewrite the instruction pointer to
// ExitThread and continue.  Only ever run in the negative-control child.
LONG CALLBACK legacy_redirect_handler(EXCEPTION_POINTERS* info) {
    if (InterlockedCompareExchange(&g_armed, 0, 0) == 0 || !info || !info->ExceptionRecord ||
        info->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION)
        return EXCEPTION_CONTINUE_SEARCH;
    FARPROC exitThread = GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "ExitThread");
    if (!exitThread) return EXCEPTION_CONTINUE_SEARCH;
    info->ContextRecord->Rip = (ULONG_PTR)exitThread;
    info->ContextRecord->Rcx = 0;
    info->ContextRecord->Rsp = (info->ContextRecord->Rsp & ~(ULONG_PTR)15) - 8;
    return EXCEPTION_CONTINUE_EXECUTION;
}

int cet_child_tests() {
    // 7120: the production flow survives a real fault under enforced shadow
    // stacks with context-IP validation.
    DWORD flowExit = 0;
    if (!run_child("flow", true, &flowExit)) {
        printf("veh driver fault: CET child policy refused by this host; shadow-stack half skipped\n");
        return 0;
    }
    if (flowExit == (DWORD)kChildNoCet) {
        printf("veh driver fault: no user shadow stacks on this host; shadow-stack half skipped\n");
        return 0;
    }
    if (flowExit != 0) {
        fprintf(stderr, "veh driver fault: flow child exit 0x%08lX\n", (unsigned long)flowExit);
        return 7120;
    }
    // 7121: negative control.  The pre-fix instruction-pointer rewrite must
    // fast-fail the same child; if it survives, 7120 proved nothing.
    DWORD legacyExit = 0;
    if (!run_child("legacy", true, &legacyExit) || legacyExit != kFastFailExitCode) {
        fprintf(stderr, "veh driver fault: legacy child exit 0x%08lX (expected 0xC0000409)\n",
                (unsigned long)legacyExit);
        return 7121;
    }
    return 0;
}
#endif

// 7108: a real fault on a thread the handler may not end reaches the ordinary
// crash path (the child's unhandled-exception filter) -- not ended, not hung.
int declined_child_test() {
    DWORD exitCode = 0;
    if (!run_child("declined", false, &exitCode) ||
        exitCode != (DWORD)kChildDeclinedReachedCrashPath) {
        fprintf(stderr, "veh driver fault: declined child exit 0x%08lX (expected 0x%08X)\n",
                (unsigned long)exitCode, (unsigned)kChildDeclinedReachedCrashPath);
        return 7108;
    }
    return 0;
}

}  // namespace

// Child entry: the harness's main() dispatches here before any suite runs.
// Returns the process exit code.
int run_veh_driver_fault_child(int argc, char** argv, bool* isChild) {
    *isChild = argc >= 3 && strcmp(argv[1], kChildFlag) == 0;
    if (!*isChild) return 0;
    // A fast-fail must end the child, not open an error dialog that waits.
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    if (!resolve_self_path()) return 7122;
    if (strcmp(argv[2], "declined") == 0) {
        SetUnhandledExceptionFilter(declined_crash_filter);
        if (!AddVectoredExceptionHandler(1, declined_vectored_handler)) return 7122;
        HANDLE thread = CreateThread(nullptr, 0, faulting_thread, nullptr, 0, nullptr);
        if (!thread) return 7122;
        WaitForSingleObject(thread, kHangMs);
        CloseHandle(thread);
        // Reaching here means the thread was ended or the wait timed out.
        return 7108;
    }
    if (!user_shadow_stack_enforced()) return kChildNoCet;
    if (strcmp(argv[2], "flow") == 0) return real_fault_tests();
#if defined(_M_X64) || defined(__x86_64__)
    if (strcmp(argv[2], "legacy") == 0) {
        if (!AddVectoredExceptionHandler(1, legacy_redirect_handler)) return 7122;
        InterlockedExchange(&g_armed, 1);
        HANDLE thread = CreateThread(nullptr, 0, faulting_thread, nullptr, 0, nullptr);
        if (!thread) return 7122;
        WaitForSingleObject(thread, kHangMs);
        CloseHandle(thread);
        return kChildLegacySurvived;
    }
#endif
    return 7122;
}

int run_veh_driver_fault_tests() {
    if (int failure = veh_exit_plan_tests()) return failure;
    if (!resolve_self_path()) return 7100;
    if (int failure = claim_decision_tests()) return failure;
#if defined(GC_VEH_FIXTURE_ASAN)
    // ASan commits shadow memory from its own fault handler; a first-chance
    // handler claiming this image's access violations would steal those.
    printf("veh driver fault: real-fault half skipped under AddressSanitizer\n");
    return 0;
#else
    if (int failure = real_fault_tests()) return failure;
    if (int failure = declined_child_test()) return failure;
#if defined(_M_X64) || defined(__x86_64__)
    if (int failure = cet_child_tests()) return failure;
#endif
    return 0;
#endif
}

#else

int run_veh_driver_fault_child(int, char**, bool* isChild) {
    *isChild = false;
    return 0;
}
int run_veh_driver_fault_tests() { return veh_exit_plan_tests(); }

#endif  // _WIN32
