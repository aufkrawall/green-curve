// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// The one thread that touches the debug-log file handle.
//
// Split out of main_diagnostics.cpp (2026-09-12) after a volume stall turned a
// single FlushFileBuffers() into 19 seconds of service unavailability -- the
// full incident and the invariant are written up in debug_log_queue_policy.h.
// main_diagnostics.cpp keeps path resolution and formatting; everything below
// is the hand-off, the writer thread, and the crash-path drain.
//
// Locking contract (two locks on purpose):
//   g_debugLogLock      guards ONLY the ring. Held for a memcpy, by producers
//                       and by the writer's copy-out. Never held across I/O.
//   g_debugLogFileLock  guards the handle, the rotation check, the write and
//                       the flush. Held ONLY by the writer thread and by the
//                       synchronous fallback/shutdown paths. A PRODUCER NEVER
//                       TAKES IT, which is the whole point.
// The drain holds the file lock and takes the ring lock inside it, so the order
// is file -> ring and it is never inverted: nothing that holds the ring lock
// asks for the file lock, so a stalled writer blocks nobody but itself.
//
// Neither lock nor the writer's event is destroyed at shutdown. They are
// process-lifetime objects, and a late line -- an atexit path, a thread the
// shutdown did not join -- must be able to log rather than touch a freed
// critical section or a recycled handle. Windows reclaims them at exit.

#include "debug_log_queue_policy.h"

static CRITICAL_SECTION g_debugLogFileLock = {};
static bool g_debugLogLocksReady = false;
static HANDLE g_debugLogFileUserToken = nullptr;
static bool g_debugLogFileUserDestination = false;

static unsigned char g_debugLogRing[gc_debug_log_queue::kRingBytes];
// Monotonic byte counters, not wrapped indices (debug_log_queue_policy.h).
// `head` is published with an interlocked store AFTER the payload bytes are in
// place, so a crash-path reader that takes no lock still sees only whole,
// fully written records between tail and head.
static volatile LONG64 g_debugLogRingHead = 0;
static volatile LONG64 g_debugLogRingTail = 0;
static volatile LONG64 g_debugLogDroppedLines = 0;
// F-04-002: lines whose route generation had already been recycled out of the
// slot table by the time the writer drained them, and which were therefore
// written to the current session's file rather than their own.
static volatile LONG64 g_debugLogReroutedLines = 0;

static HANDLE g_debugLogWriterThread = nullptr;
static HANDLE g_debugLogWriterEvent = nullptr;
static volatile LONG g_debugLogWriterStopping = 0;
static volatile LONG g_debugLogCloseRequested = 0;
static volatile LONG g_debugLogWriterStartGuard = 0;

static void debug_log_initialize_locks() {
    if (g_debugLogLocksReady) return;
    InitializeCriticalSection(&g_debugLogFileLock);
    g_debugLogLocksReady = true;
}

// ---------------------------------------------------------------------------
// The file half. Everything here can block for as long as the volume decides
// to, which is exactly why no producer thread is allowed to reach it.
// ---------------------------------------------------------------------------

static void debug_log_flush_file_locked() {
    GcDebugLogIdentity identity(g_debugLogFileUserToken, g_debugLogFileUserDestination);
    if (identity.valid() && g_debugLogFile != INVALID_HANDLE_VALUE)
        FlushFileBuffers(g_debugLogFile);
}

static void debug_log_close_file_locked() {
    if (g_debugLogFile != INVALID_HANDLE_VALUE) {
        GcDebugLogIdentity identity(g_debugLogFileUserToken, g_debugLogFileUserDestination);
        if (identity.valid()) FlushFileBuffers(g_debugLogFile);
        CloseHandle(g_debugLogFile);
        g_debugLogFile = INVALID_HANDLE_VALUE;
    }
    if (g_debugLogFileUserToken) CloseHandle(g_debugLogFileUserToken);
    g_debugLogFileUserToken = nullptr;
    g_debugLogFileUserDestination = false;
    g_debugLogOpenPath[0] = 0;
}

// Asking for the handle to be dropped is a REQUEST, not a synchronous close:
// the callers are session/config transitions, and making them wait on a handle
// the writer may currently be stalled inside would hand the stall straight back
// to a caller that has nothing to do with logging. The writer honours it at its
// next drain boundary; shutdown closes for real via debug_log_writer_stop().
static void close_debug_log_file() {
    InterlockedExchange(&g_debugLogCloseRequested, 1);
    if (g_debugLogWriterEvent) SetEvent(g_debugLogWriterEvent);
}

struct DebugLogRouteSlot {
    unsigned short generation;
    bool valid;
    char path[MAX_PATH];
    HANDLE userToken;
    bool userDestination;
};

static unsigned short g_debugLogCurrentRouteGen = 0;
static DebugLogRouteSlot g_debugLogRoutes[gc_debug_log_queue::kMaxRouteSlots] = {};

// Update the current log destination path and advance the route generation.
// Callable from any thread (takes g_debugLogLock, never touches disk I/O).
static void debug_log_set_route_path(const char* path, HANDLE userToken) {
    if (!path || !path[0]) return;
    HANDLE tokenCopy = gc_debug_log_user_token(userToken);
    debug_log_initialize_locks();
    EnterCriticalSection(&g_debugLogLock);
    unsigned int curSlotIdx = (unsigned int)(g_debugLogCurrentRouteGen % gc_debug_log_queue::kMaxRouteSlots);
    if (g_debugLogCurrentRouteGen != 0 &&
        g_debugLogRoutes[curSlotIdx].valid &&
        g_debugLogRoutes[curSlotIdx].generation == g_debugLogCurrentRouteGen &&
        !userToken && !g_debugLogRoutes[curSlotIdx].userDestination &&
        _stricmp(g_debugLogRoutes[curSlotIdx].path, path) == 0) {
        LeaveCriticalSection(&g_debugLogLock);
        return;
    }
    g_debugLogCurrentRouteGen++;
    if (g_debugLogCurrentRouteGen == 0) g_debugLogCurrentRouteGen = 1;
    unsigned int newIdx = (unsigned int)(g_debugLogCurrentRouteGen % gc_debug_log_queue::kMaxRouteSlots);
    if (g_debugLogRoutes[newIdx].userToken) CloseHandle(g_debugLogRoutes[newIdx].userToken);
    g_debugLogRoutes[newIdx].userToken = tokenCopy;
    g_debugLogRoutes[newIdx].userDestination = userToken != nullptr;
    g_debugLogRoutes[newIdx].generation = g_debugLogCurrentRouteGen;
    g_debugLogRoutes[newIdx].valid = true;
    StringCchCopyA(g_debugLogRoutes[newIdx].path, ARRAY_COUNT(g_debugLogRoutes[newIdx].path), path);
    LeaveCriticalSection(&g_debugLogLock);
    if (g_debugLogWriterEvent) SetEvent(g_debugLogWriterEvent);
}

static void debug_log_initialize_route_locked() {
    if (g_debugLogCurrentRouteGen == 0) {
        const char* initPath = app_is_service_process() ? service_early_debug_log_path() : effective_debug_log_path();
        if (initPath && initPath[0]) {
            g_debugLogCurrentRouteGen = 1;
            unsigned int idx = (unsigned int)(1 % gc_debug_log_queue::kMaxRouteSlots);
            g_debugLogRoutes[idx].generation = 1;
            g_debugLogRoutes[idx].valid = true;
            StringCchCopyA(g_debugLogRoutes[idx].path, ARRAY_COUNT(g_debugLogRoutes[idx].path), initPath);
        }
    }
}

// Caller holds the ring lock while copying both destination and identity.
static bool debug_log_capture_route_locked(unsigned short generation, GcDebugLogRoute* out) {
    out->reset();
    unsigned int index = generation % gc_debug_log_queue::kMaxRouteSlots;
    if (!generation || !g_debugLogRoutes[index].valid ||
        g_debugLogRoutes[index].generation != generation) {
        index = g_debugLogCurrentRouteGen % gc_debug_log_queue::kMaxRouteSlots;
        if (generation && generation != g_debugLogCurrentRouteGen)
            InterlockedIncrement64(&g_debugLogReroutedLines);
    }
    const DebugLogRouteSlot& slot = g_debugLogRoutes[index];
    if (!slot.valid) return false;
    out->userDestination = slot.userDestination;
    out->generation = slot.generation;
    if (slot.userDestination && (!slot.userToken || !DuplicateHandle(
            GetCurrentProcess(), slot.userToken, GetCurrentProcess(), &out->userToken,
            0, FALSE, DUPLICATE_SAME_ACCESS))) {
        InterlockedIncrement64(&g_debugLogDroppedLines);
        return false;
    }
    if (slot.userDestination) {
        TOKEN_STATISTICS statistics = {{}, {}, {}, TokenImpersonation,
            SecurityImpersonation, 0, 0, 0, 0, {}};
        DWORD bytes = 0;
        if (!GetTokenInformation(out->userToken, TokenStatistics, &statistics,
                                  sizeof(statistics), &bytes)) {
            InterlockedIncrement64(&g_debugLogDroppedLines);
            return false;
        }
        out->tokenId = statistics.TokenId;
    }
    StringCchCopyA(out->path, ARRAY_COUNT(out->path), slot.path);
    return true;
}

static unsigned short g_debugLogOpenRouteGen = 0;
static LUID g_debugLogOpenTokenId = {};

static HANDLE debug_log_open_route_file_locked(const GcDebugLogRoute& route) {
    HANDLE token = nullptr;
    if (route.userDestination && !DuplicateHandle(GetCurrentProcess(), route.userToken,
            GetCurrentProcess(), &token, 0, FALSE, DUPLICATE_SAME_ACCESS))
        return INVALID_HANDLE_VALUE;
    HANDLE file = open_debug_log_file_locked(route.path, route.userDestination);
    if (file == INVALID_HANDLE_VALUE) {
        if (token) CloseHandle(token);
        return INVALID_HANDLE_VALUE;
    }
    g_debugLogFileUserToken = token;
    g_debugLogFileUserDestination = route.userDestination;
    g_debugLogOpenTokenId = route.tokenId;
    return file;
}

static HANDLE debug_log_acquire_handle_for_path_locked(const GcDebugLogRoute& route) {
    const char* targetPath = route.path;
    if (!targetPath || !targetPath[0]) return INVALID_HANDLE_VALUE;
    if (InterlockedExchange(&g_debugLogCloseRequested, 0) != 0)
        debug_log_close_file_locked();

    if (g_debugLogFile != INVALID_HANDLE_VALUE &&
        (_stricmp(g_debugLogOpenPath, targetPath) != 0 ||
         g_debugLogOpenRouteGen != route.generation ||
         g_debugLogFileUserDestination != route.userDestination ||
         g_debugLogOpenTokenId.LowPart != route.tokenId.LowPart ||
         g_debugLogOpenTokenId.HighPart != route.tokenId.HighPart)) {
        debug_log_close_file_locked();
    }
    if (g_debugLogFile == INVALID_HANDLE_VALUE) {
        g_debugLogFile = debug_log_open_route_file_locked(route);
        if (g_debugLogFile != INVALID_HANDLE_VALUE) {
            StringCchCopyA(g_debugLogOpenPath, ARRAY_COUNT(g_debugLogOpenPath), targetPath);
        }
    }
    if (g_debugLogFile == INVALID_HANDLE_VALUE) return INVALID_HANDLE_VALUE;

    g_debugLogOpenRouteGen = route.generation;
    LARGE_INTEGER logSize = {};
    if (GetFileSizeEx(g_debugLogFile, &logSize) &&
        gc_debug_log_rotation::should_rotate(logSize.QuadPart)) {
        debug_log_close_file_locked();
        HANDLE fresh = gc_CreateFileUtf8(targetPath, GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, CREATE_ALWAYS,
            debug_log_file_attributes(), nullptr);
        if (fresh != INVALID_HANDLE_VALUE) {
            const char* marker = gc_debug_log_rotation::marker_line();
            DWORD markerWritten = 0;
            WriteFile(fresh, marker, (DWORD)strlen(marker), &markerWritten,
                      nullptr);
            CloseHandle(fresh);
        }
        g_debugLogFile = debug_log_open_route_file_locked(route);
        if (g_debugLogFile != INVALID_HANDLE_VALUE) {
            StringCchCopyA(g_debugLogOpenPath, ARRAY_COUNT(g_debugLogOpenPath), targetPath);
        }
        if (g_debugLogFile == INVALID_HANDLE_VALUE) return INVALID_HANDLE_VALUE;
    }
    return g_debugLogFile;
}

static void debug_log_write_line_to_path_locked(const char* text, const GcDebugLogRoute& route) {
    if (!text || !text[0] || !route.path[0]) return;
    GcDebugLogIdentity identity(route.userToken, route.userDestination);
    if (!identity.valid()) {
        InterlockedIncrement64(&g_debugLogDroppedLines);
        return;
    }
    HANDLE file = debug_log_acquire_handle_for_path_locked(route);
    if (file == INVALID_HANDLE_VALUE) {
        InterlockedIncrement64(&g_debugLogDroppedLines);
        return;
    }
    DWORD written = 0;
    if (!WriteFile(file, text, (DWORD)strlen(text), &written, nullptr)) {
        debug_log_close_file_locked();
        file = debug_log_acquire_handle_for_path_locked(route);
        if (file != INVALID_HANDLE_VALUE)
            WriteFile(file, text, (DWORD)strlen(text), &written, nullptr);
    }
}

// Append to the current route using the same identity rules as queued records.
static void debug_log_write_line_locked(const char* text) {
    GcDebugLogRoute route;
    EnterCriticalSection(&g_debugLogLock);
    debug_log_initialize_route_locked();
    debug_log_capture_route_locked(g_debugLogCurrentRouteGen, &route);
    LeaveCriticalSection(&g_debugLogLock);
    debug_log_write_line_to_path_locked(text, route);
}

// ---------------------------------------------------------------------------
// The ring half. Producers only ever run this.
// ---------------------------------------------------------------------------

static void debug_log_ring_copy_in(unsigned long long position,
                                   const void* data, unsigned int bytes) {
    unsigned int offset = gc_debug_log_queue::offset_of(position,
        (unsigned int)gc_debug_log_queue::kRingBytes);
    unsigned int firstSpan = gc_debug_log_queue::first_span(offset,
        (unsigned int)gc_debug_log_queue::kRingBytes, bytes);
    memcpy(g_debugLogRing + offset, data, firstSpan);
    if (firstSpan < bytes)
        memcpy(g_debugLogRing, (const unsigned char*)data + firstSpan,
               bytes - firstSpan);
}

static void debug_log_ring_copy_out(unsigned long long position,
                                    void* data, unsigned int bytes) {
    unsigned int offset = gc_debug_log_queue::offset_of(position,
        (unsigned int)gc_debug_log_queue::kRingBytes);
    unsigned int firstSpan = gc_debug_log_queue::first_span(offset,
        (unsigned int)gc_debug_log_queue::kRingBytes, bytes);
    memcpy(data, g_debugLogRing + offset, firstSpan);
    if (firstSpan < bytes)
        memcpy((unsigned char*)data + firstSpan, g_debugLogRing,
               bytes - firstSpan);
}

static void debug_log_writer_ensure_started();

// The producer side of the whole mechanism: a bounded memcpy and a SetEvent.
// Nothing here can wait on I/O, which is the invariant the 2026-09-12 incident
// was caused by not having.
static void debug_log_enqueue(const char* line) {
    if (!line || !line[0]) return;
    size_t length = strlen(line);
    if (length > (size_t)gc_debug_log_queue::kMaxRecordBytes)
        length = (size_t)gc_debug_log_queue::kMaxRecordBytes;
    debug_log_initialize_locks();
    debug_log_writer_ensure_started();

    if (!g_debugLogWriterThread) {
        // No writer yet (very early startup, or thread creation failed). The
        // synchronous path is then the honest fallback: these callers are
        // process bring-up, not request handlers.
        EnterCriticalSection(&g_debugLogFileLock);
        OutputDebugStringA(line);
        debug_log_write_line_locked(line);
        if (app_is_service_process() && g_debugLogFile != INVALID_HANDLE_VALUE)
            debug_log_flush_file_locked();
        LeaveCriticalSection(&g_debugLogFileLock);
        return;
    }

    bool committed = false;
    EnterCriticalSection(&g_debugLogLock);
    debug_log_initialize_route_locked();
    unsigned short routeGen = g_debugLogCurrentRouteGen;
    unsigned long long head = (unsigned long long)g_debugLogRingHead;
    unsigned long long tail = (unsigned long long)g_debugLogRingTail;
    if (gc_debug_log_queue::fits(head, tail,
            (unsigned int)gc_debug_log_queue::kRingBytes,
            (unsigned int)length)) {
        unsigned int payloadBytes = (unsigned int)length;
        unsigned int header = gc_debug_log_queue::pack_header(payloadBytes, routeGen);
        debug_log_ring_copy_in(head, &header,
            (unsigned int)gc_debug_log_queue::kHeaderBytes);
        debug_log_ring_copy_in(head + gc_debug_log_queue::kHeaderBytes, line,
            payloadBytes);
        // Publish only after both copies land, so the lock-free crash drain
        // never reads a half-written record.
        InterlockedExchange64(&g_debugLogRingHead,
            (LONG64)(head + gc_debug_log_queue::record_bytes(payloadBytes)));
        committed = true;
    } else {
        InterlockedIncrement64(&g_debugLogDroppedLines);
    }
    LeaveCriticalSection(&g_debugLogLock);
    if (committed && g_debugLogWriterEvent) SetEvent(g_debugLogWriterEvent);
}

// Move one record out of the ring. Returns false when the ring is empty.
static bool debug_log_dequeue(char* out, size_t outSize, GcDebugLogRoute* route) {
    if (!out || outSize == 0) return false;
    bool dequeued = false;
    route->reset();
    EnterCriticalSection(&g_debugLogLock);
    unsigned long long head = (unsigned long long)g_debugLogRingHead;
    unsigned long long tail = (unsigned long long)g_debugLogRingTail;
    if (gc_debug_log_queue::used_bytes(head, tail) >
            (unsigned long long)gc_debug_log_queue::kHeaderBytes) {
        unsigned int header = 0;
        debug_log_ring_copy_out(tail, &header,
            (unsigned int)gc_debug_log_queue::kHeaderBytes);
        unsigned int payloadBytes = gc_debug_log_queue::unpack_payload_bytes(header);
        unsigned int routeGen = gc_debug_log_queue::unpack_route_generation(header);
        if (gc_debug_log_queue::record_length_is_valid(payloadBytes, head,
                tail)) {
            unsigned int copyBytes = (payloadBytes < outSize)
                ? payloadBytes : (unsigned int)(outSize - 1);
            debug_log_ring_copy_out(
                tail + gc_debug_log_queue::kHeaderBytes, out, copyBytes);
            out[copyBytes] = 0;
            debug_log_capture_route_locked((unsigned short)routeGen, route);
            InterlockedExchange64(&g_debugLogRingTail,
                (LONG64)(tail +
                    gc_debug_log_queue::record_bytes(payloadBytes)));
            dequeued = true;
        } else {
            // An unusable length means the ring's framing is no longer
            // trustworthy; discarding the whole backlog is preferable to
            // walking it. Counted, so the log says so.
            InterlockedExchange64(&g_debugLogRingTail, (LONG64)head);
            InterlockedIncrement64(&g_debugLogDroppedLines);
        }
    }
    LeaveCriticalSection(&g_debugLogLock);
    return dequeued;
}

// ---------------------------------------------------------------------------
// The writer thread.
// ---------------------------------------------------------------------------

// Drain everything currently queued, then flush ONCE. Batching the flush is
// what turns a per-line durability round trip into a per-burst one; the
// durability that matters (the tail of the log at a crash) is preserved by the
// crash drain below and by debug_log_writer_stop().
static void debug_log_writer_drain() {
    char line[gc_debug_log_queue::kMaxRecordBytes + 1] = {};
    GcDebugLogRoute route;
    bool wroteAny = false;
    if (!g_debugLogLocksReady) return;
    EnterCriticalSection(&g_debugLogFileLock);
    LONG64 dropped = InterlockedExchange64(&g_debugLogDroppedLines, 0);
    if (dropped > 0) {
        char marker[256] = {};
        char stamped[gc_debug_log_queue::kMaxRecordBytes] = {};
        StringCchPrintfA(marker, ARRAY_COUNT(marker),
            gc_debug_log_queue::dropped_marker_format(),
            (unsigned long long)dropped);
        int prefixLen = format_log_timestamp_prefix(stamped,
            ARRAY_COUNT(stamped));
        StringCchCatA(stamped + prefixLen, ARRAY_COUNT(stamped) - prefixLen,
            marker);
        OutputDebugStringA(stamped);
        debug_log_write_line_locked(stamped);
        wroteAny = true;
    }
    // F-04-002: same shape as the dropped-line marker, and for the same reason
    // -- the log must be honest about its own gaps. A rerouted line is one that
    // outlived its route slot and was written to the CURRENT session's file
    // instead of the one it was produced for.
    LONG64 rerouted = InterlockedExchange64(&g_debugLogReroutedLines, 0);
    if (rerouted > 0) {
        char marker[256] = {};
        char stamped[gc_debug_log_queue::kMaxRecordBytes] = {};
        StringCchPrintfA(marker, ARRAY_COUNT(marker),
            gc_debug_log_queue::rerouted_marker_format(),
            (unsigned long long)rerouted);
        int prefixLen = format_log_timestamp_prefix(stamped,
            ARRAY_COUNT(stamped));
        StringCchCatA(stamped + prefixLen, ARRAY_COUNT(stamped) - prefixLen,
            marker);
        OutputDebugStringA(stamped);
        debug_log_write_line_locked(stamped);
        wroteAny = true;
    }
    while (debug_log_dequeue(line, sizeof(line), &route)) {
        // OutputDebugStringA serializes on a system-wide mutex whenever a
        // debugger or DebugView is listening, so it belongs on this thread for
        // the same reason the file write does.
        OutputDebugStringA(line);
        if (route.path[0]) {
            debug_log_write_line_to_path_locked(line, route);
            wroteAny = true;
        }
    }
    if (wroteAny && app_is_service_process() &&
        g_debugLogFile != INVALID_HANDLE_VALUE) {
        debug_log_flush_file_locked();
    }
    LeaveCriticalSection(&g_debugLogFileLock);
}

static DWORD WINAPI debug_log_writer_thread_proc(void*) {
    for (;;) {
        DWORD wait = WaitForSingleObject(g_debugLogWriterEvent, INFINITE);
        if (wait != WAIT_OBJECT_0) return 1;
        debug_log_writer_drain();
        if (InterlockedCompareExchange(&g_debugLogWriterStopping, 1, 1) == 1) {
            debug_log_writer_drain();
            return 0;
        }
    }
}

static void debug_log_writer_ensure_started() {
    if (g_debugLogWriterThread) return;
    if (InterlockedCompareExchange(&g_debugLogWriterStopping, 0, 0) != 0)
        return;
    // One winner creates the thread; every other producer takes the
    // synchronous fallback for this line rather than waiting on the creation.
    if (InterlockedCompareExchange(&g_debugLogWriterStartGuard, 1, 0) != 0)
        return;
    if (!g_debugLogWriterEvent)
        g_debugLogWriterEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (g_debugLogWriterEvent) {
        g_debugLogWriterThread = CreateThread(nullptr, (SIZE_T)64 * 1024,
            debug_log_writer_thread_proc, nullptr,
            STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
    }
    if (!g_debugLogWriterThread) {
        InterlockedExchange(&g_debugLogWriterStartGuard, 0);
    }
}

// Called once per process, beside the other lock initialization, so the very
// first logged line already goes through the queue rather than through the
// synchronous fallback.
static void debug_log_writer_start() {
    debug_log_initialize_locks();
    EnterCriticalSection(&g_debugLogLock);
    debug_log_initialize_route_locked();
    LeaveCriticalSection(&g_debugLogLock);
    debug_log_writer_ensure_started();
}

// Drain, join, and close for real. Every teardown path calls this BEFORE it
// deletes the critical sections, so the last lines of a session are on disk.
static void debug_log_writer_stop() {
    InterlockedExchange(&g_debugLogWriterStopping, 1);
    HANDLE thread = g_debugLogWriterThread;
    bool joined = true;
    if (thread) {
        if (g_debugLogWriterEvent) SetEvent(g_debugLogWriterEvent);
        // Bounded: a writer wedged in a volume stall must not stop the process
        // from exiting.
        joined = WaitForSingleObject(thread,
            (DWORD)gc_debug_log_queue::kWriterJoinMs) == WAIT_OBJECT_0;
        if (!joined) {
            OutputDebugStringA("green curve: debug log writer did not finish "
                               "within its join bound; leaving the remaining "
                               "queued lines to it\n");
        }
        CloseHandle(thread);
        g_debugLogWriterThread = nullptr;
    }
    // Drain and close ONLY when the writer is known to be finished. A writer
    // that did not come back still owns the file lock, so waiting for it here
    // would put the unbounded stall straight back into process exit -- exactly
    // what the join bound above exists to prevent. Losing the tail of the log
    // to a wedged volume is the correct trade; the crash paths have their own
    // lock-free drain for the case that actually matters.
    if (!joined || !g_debugLogLocksReady) return;
    debug_log_writer_drain();
    // The event handle is deliberately NOT closed. A producer reads it without
    // holding anything (SetEvent after committing a record), so closing it here
    // would be a use-after-close against whatever handle value Windows recycles
    // next -- a far worse failure than leaking one event at process exit.
    EnterCriticalSection(&g_debugLogFileLock);
    debug_log_close_file_locked();
    LeaveCriticalSection(&g_debugLogFileLock);
}

// Best-effort crash drain. Takes NO lock, because the crashing thread may hold
// either of them -- the same reason debug_log_veh() exists. Safe to walk
// because `head` is only published after a record's bytes are fully in place,
// so tail..head contains whole records; a length that still fails validation
// stops the walk instead of running past the committed region.
static void debug_log_drain_pending_for_crash() {
    unsigned long long head = (unsigned long long)g_debugLogRingHead;
    unsigned long long tail = (unsigned long long)g_debugLogRingTail;
    if (gc_debug_log_queue::used_bytes(head, tail) == 0) return;
    while (gc_debug_log_queue::used_bytes(head, tail) >
            (unsigned long long)gc_debug_log_queue::kHeaderBytes) {
        unsigned int header = 0;
        debug_log_ring_copy_out(tail, &header,
            (unsigned int)gc_debug_log_queue::kHeaderBytes);
        unsigned int payloadBytes = gc_debug_log_queue::unpack_payload_bytes(header);
        if (!gc_debug_log_queue::record_length_is_valid(payloadBytes, head,
                tail))
            break;
        char staging[gc_debug_log_queue::kMaxRecordBytes + 1] = {};
        debug_log_ring_copy_out(tail + gc_debug_log_queue::kHeaderBytes,
            staging, payloadBytes);
        staging[payloadBytes] = 0;
        write_crash_breadcrumb_direct(staging);
        tail += gc_debug_log_queue::record_bytes(payloadBytes);
    }
}
