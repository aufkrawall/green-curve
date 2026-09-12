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

static unsigned char g_debugLogRing[gc_debug_log_queue::kRingBytes];
// Monotonic byte counters, not wrapped indices (debug_log_queue_policy.h).
// `head` is published with an interlocked store AFTER the payload bytes are in
// place, so a crash-path reader that takes no lock still sees only whole,
// fully written records between tail and head.
static volatile LONG64 g_debugLogRingHead = 0;
static volatile LONG64 g_debugLogRingTail = 0;
static volatile LONG64 g_debugLogDroppedLines = 0;

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

static void debug_log_close_file_locked() {
    if (g_debugLogFile != INVALID_HANDLE_VALUE) {
        FlushFileBuffers(g_debugLogFile);
        CloseHandle(g_debugLogFile);
        g_debugLogFile = INVALID_HANDLE_VALUE;
    }
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
};

static unsigned short g_debugLogCurrentRouteGen = 0;
static DebugLogRouteSlot g_debugLogRoutes[gc_debug_log_queue::kMaxRouteSlots] = {};

// Update the current log destination path and advance the route generation.
// Callable from any thread (takes g_debugLogLock, never touches disk I/O).
static void debug_log_set_route_path(const char* path) {
    if (!path || !path[0]) return;
    debug_log_initialize_locks();
    EnterCriticalSection(&g_debugLogLock);
    unsigned int curSlotIdx = (unsigned int)(g_debugLogCurrentRouteGen % gc_debug_log_queue::kMaxRouteSlots);
    if (g_debugLogCurrentRouteGen != 0 &&
        g_debugLogRoutes[curSlotIdx].valid &&
        g_debugLogRoutes[curSlotIdx].generation == g_debugLogCurrentRouteGen &&
        _stricmp(g_debugLogRoutes[curSlotIdx].path, path) == 0) {
        LeaveCriticalSection(&g_debugLogLock);
        return;
    }
    g_debugLogCurrentRouteGen++;
    if (g_debugLogCurrentRouteGen == 0) g_debugLogCurrentRouteGen = 1;
    unsigned int newIdx = (unsigned int)(g_debugLogCurrentRouteGen % gc_debug_log_queue::kMaxRouteSlots);
    g_debugLogRoutes[newIdx].generation = g_debugLogCurrentRouteGen;
    g_debugLogRoutes[newIdx].valid = true;
    StringCchCopyA(g_debugLogRoutes[newIdx].path, ARRAY_COUNT(g_debugLogRoutes[newIdx].path), path);
    LeaveCriticalSection(&g_debugLogLock);
    if (g_debugLogWriterEvent) SetEvent(g_debugLogWriterEvent);
}

static HANDLE debug_log_acquire_handle_for_path_locked(const char* targetPath) {
    if (!targetPath || !targetPath[0]) return INVALID_HANDLE_VALUE;
    if (InterlockedExchange(&g_debugLogCloseRequested, 0) != 0)
        debug_log_close_file_locked();

    if (g_debugLogFile != INVALID_HANDLE_VALUE &&
        _stricmp(g_debugLogOpenPath, targetPath) != 0) {
        debug_log_close_file_locked();
    }
    if (g_debugLogFile == INVALID_HANDLE_VALUE) {
        g_debugLogFile = open_debug_log_file_locked(targetPath);
        if (g_debugLogFile != INVALID_HANDLE_VALUE) {
            StringCchCopyA(g_debugLogOpenPath, ARRAY_COUNT(g_debugLogOpenPath), targetPath);
        }
    }
    if (g_debugLogFile == INVALID_HANDLE_VALUE) return INVALID_HANDLE_VALUE;

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
        g_debugLogFile = open_debug_log_file_locked(targetPath);
        if (g_debugLogFile != INVALID_HANDLE_VALUE) {
            StringCchCopyA(g_debugLogOpenPath, ARRAY_COUNT(g_debugLogOpenPath), targetPath);
        }
        if (g_debugLogFile == INVALID_HANDLE_VALUE) return INVALID_HANDLE_VALUE;
    }
    return g_debugLogFile;
}

// Open (or re-open) the handle for the path that is currently in effect, and
// honour a pending close request. Returns the handle to write through, or
// INVALID_HANDLE_VALUE when the log cannot be opened at all.
static HANDLE debug_log_acquire_handle_locked() {
    if (InterlockedExchange(&g_debugLogCloseRequested, 0) != 0)
        debug_log_close_file_locked();

    const char* debugPath = effective_debug_log_path();
    // Late path resolution for the GUI, preserved from the synchronous writer:
    // lines produced before WinMain resolves the data paths would otherwise
    // land in the relative fallback file forever.
    if (!g_app.isServiceProcess && !g_debugLogPath[0]) {
        char pathErr[256] = {};
        resolve_data_paths(pathErr, sizeof(pathErr));
        debugPath = effective_debug_log_path();
    }
    if (g_debugLogFile != INVALID_HANDLE_VALUE &&
        _stricmp(g_debugLogOpenPath, debugPath) != 0) {
        debug_log_close_file_locked();
    }
    if (g_debugLogFile == INVALID_HANDLE_VALUE)
        g_debugLogFile = open_debug_log_file_locked(debugPath);
    if (g_debugLogFile == INVALID_HANDLE_VALUE) return INVALID_HANDLE_VALUE;

    // Size-cap rotation. The GUI and service can share one user-side log via
    // append-only handles, so truncation (not rename) is what keeps every
    // writer valid: FILE_APPEND_DATA writes always land at EOF, and a
    // cooperative truncate simply moves that EOF for everyone.
    LARGE_INTEGER logSize = {};
    if (GetFileSizeEx(g_debugLogFile, &logSize) &&
        gc_debug_log_rotation::should_rotate(logSize.QuadPart)) {
        debug_log_close_file_locked();
        HANDLE fresh = gc_CreateFileUtf8(debugPath, GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, CREATE_ALWAYS,
            debug_log_file_attributes(), nullptr);
        if (fresh != INVALID_HANDLE_VALUE) {
            const char* marker = gc_debug_log_rotation::marker_line();
            DWORD markerWritten = 0;
            WriteFile(fresh, marker, (DWORD)strlen(marker), &markerWritten,
                      nullptr);
            CloseHandle(fresh);
        }
        g_debugLogFile = open_debug_log_file_locked(debugPath);
        if (g_debugLogFile == INVALID_HANDLE_VALUE) return INVALID_HANDLE_VALUE;
    }
    return g_debugLogFile;
}

static void debug_log_write_line_to_path_locked(const char* text, const char* targetPath) {
    if (!text || !text[0] || !targetPath || !targetPath[0]) return;
    HANDLE file = debug_log_acquire_handle_for_path_locked(targetPath);
    if (file == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    if (!WriteFile(file, text, (DWORD)strlen(text), &written, nullptr)) {
        debug_log_close_file_locked();
        file = debug_log_acquire_handle_for_path_locked(targetPath);
        if (file != INVALID_HANDLE_VALUE)
            WriteFile(file, text, (DWORD)strlen(text), &written, nullptr);
    }
}

// Append one already-formatted line. Caller holds g_debugLogFileLock.
static void debug_log_write_line_locked(const char* text) {
    if (!text || !text[0]) return;
    HANDLE file = debug_log_acquire_handle_locked();
    if (file == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    if (!WriteFile(file, text, (DWORD)strlen(text), &written, nullptr)) {
        // One re-open retry: a handle invalidated by a path/session change
        // under us is recoverable, and losing the line to it is not.
        debug_log_close_file_locked();
        file = debug_log_acquire_handle_locked();
        if (file != INVALID_HANDLE_VALUE)
            WriteFile(file, text, (DWORD)strlen(text), &written, nullptr);
    }
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
        if (g_app.isServiceProcess && g_debugLogFile != INVALID_HANDLE_VALUE)
            FlushFileBuffers(g_debugLogFile);
        LeaveCriticalSection(&g_debugLogFileLock);
        return;
    }

    bool committed = false;
    EnterCriticalSection(&g_debugLogLock);
    if (g_debugLogCurrentRouteGen == 0) {
        const char* initPath = effective_debug_log_path();
        if (initPath && initPath[0]) {
            g_debugLogCurrentRouteGen = 1;
            unsigned int idx = (unsigned int)(1 % gc_debug_log_queue::kMaxRouteSlots);
            g_debugLogRoutes[idx].generation = 1;
            g_debugLogRoutes[idx].valid = true;
            StringCchCopyA(g_debugLogRoutes[idx].path, ARRAY_COUNT(g_debugLogRoutes[idx].path), initPath);
        }
    }
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
static bool debug_log_dequeue(char* out, size_t outSize, char* outPath, size_t outPathSize) {
    if (!out || outSize == 0) return false;
    bool dequeued = false;
    if (outPath && outPathSize > 0) outPath[0] = 0;
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
            if (outPath && outPathSize > 0) {
                unsigned int idx = (unsigned int)(routeGen % gc_debug_log_queue::kMaxRouteSlots);
                if (routeGen != 0 && g_debugLogRoutes[idx].valid &&
                    g_debugLogRoutes[idx].generation == (unsigned short)routeGen) {
                    StringCchCopyA(outPath, outPathSize, g_debugLogRoutes[idx].path);
                } else {
                    unsigned int curIdx = (unsigned int)(g_debugLogCurrentRouteGen % gc_debug_log_queue::kMaxRouteSlots);
                    if (g_debugLogRoutes[curIdx].valid) {
                        StringCchCopyA(outPath, outPathSize, g_debugLogRoutes[curIdx].path);
                    }
                }
            }
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

static bool debug_log_dequeue(char* out, size_t outSize) {
    char targetPath[MAX_PATH] = {};
    return debug_log_dequeue(out, outSize, targetPath, sizeof(targetPath));
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
    char targetPath[MAX_PATH] = {};
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
    while (debug_log_dequeue(line, sizeof(line), targetPath, sizeof(targetPath))) {
        // OutputDebugStringA serializes on a system-wide mutex whenever a
        // debugger or DebugView is listening, so it belongs on this thread for
        // the same reason the file write does.
        OutputDebugStringA(line);
        if (targetPath[0]) {
            debug_log_write_line_to_path_locked(line, targetPath);
            wroteAny = true;
        }
    }
    if (wroteAny && g_app.isServiceProcess &&
        g_debugLogFile != INVALID_HANDLE_VALUE) {
        FlushFileBuffers(g_debugLogFile);
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
    const char* initPath = effective_debug_log_path();
    if (initPath && initPath[0]) {
        debug_log_set_route_path(initPath);
    }
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
