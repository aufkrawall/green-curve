// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// Pure hand-off policy between a thread that PRODUCES a debug line and the one
// thread that WRITES it to disk.
//
// Why this exists (2026-09-12 live incident):
//
// debug_log() used to do the whole file write inline, on whichever thread
// called it, under g_debugLogLock -- and in the service that write is durable
// by construction: the handle carries FILE_FLAG_WRITE_THROUGH and every single
// line is followed by FlushFileBuffers().  So one log line cost one synchronous
// round trip to stable storage, on the caller's thread.
//
// The service calls debug_log() from inside service_handle_telemetry_request(),
// which runs under BOTH the runtime lock and the process-wide serialized
// dispatch lock.  On 2026-09-12 11:25:48 the C: volume stalled (Windows logged
// Volsnap event 25, "shadow copies deleted because the shadow copy storage
// could not grow in time -- reduce the I/O load on the system") and one such
// flush blocked for 19.078 seconds.  The service could not answer ANY request
// for that whole time: the three requests behind it logged 15375 / 10391 /
// 4735 ms of dispatch-queue wait, the GUI's read deadlines expired, and the GUI
// presented a healthy service as a lost connection.  The proof it was the log
// write and not the GPU is in the file itself: the handler reported
// `lockWaitMs=0` (so no lock contention) and lines timestamped 11:25:48.173 and
// 11:25:49.816 -- from two DIFFERENT service threads, one of them the main loop
// that touches no GPU -- were appended only after an 11:26:07 line, i.e. both
// were queued behind a debug_log() that was stuck inside the write.
//
// The invariant this policy backs: PRODUCING a diagnostic line must never block
// on I/O.  A caller formats into a fixed buffer, copies it into this ring, and
// returns; one writer thread owns the handle, the rotation check, the write and
// the flush.  Disk latency then costs log freshness, never request turnaround.
//
// A bounded ring must be allowed to overflow, because the alternative -- making
// the producer wait for space -- reintroduces exactly the coupling above.  An
// overflow therefore DROPS the incoming line and counts it, and the writer
// emits a marker naming the count, so the log stays honest about its own gap
// instead of silently losing lines.

#ifndef GREEN_CURVE_DEBUG_LOG_QUEUE_POLICY_H
#define GREEN_CURVE_DEBUG_LOG_QUEUE_POLICY_H

namespace gc_debug_log_queue {

enum {
    // Ring capacity. Sized from the worst burst either process actually
    // produces while the writer is stalled: the service emits ~4 lines/s
    // steady-state, and the GUI's full control rebuild emits ~80 lines in
    // ~1.5 s. At ~150 bytes/line, 512 KiB absorbs roughly an hour of service
    // cadence or many minutes of GUI rebuild churn -- far beyond the 19 s
    // stall that motivated this -- while staying a fixed, bounded allocation
    // made once at startup rather than per line.
    kRingBytes = 512 * 1024,
    // Longest single line a producer may hand over. Matches debug_log()'s own
    // formatting buffer, so no caller can be truncated by the queue that was
    // not already truncated by the formatter.
    kMaxRecordBytes = 1200,
    // Length prefix stored ahead of each payload. Four bytes rather than two
    // so the framing cannot become the reason a future longer line is refused.
    kHeaderBytes = 4,
    // How long a shutdown waits to JOIN the writer thread before draining the
    // remainder itself. This is deliberately a bound on process exit, not on
    // logging: the shutdown thread drains whatever is left either way, so the
    // timeout can cost a duplicated line but never a lost one. It exists
    // because the failure mode this whole queue was built for -- a volume that
    // stops answering -- would otherwise be able to hold the process open.
    kWriterJoinMs = 5000,
};

// Bytes currently committed and not yet drained. Positions are MONOTONIC byte
// counters, never wrapped indices: the difference is then always the true
// occupancy with no empty-versus-full ambiguity, and a 64-bit counter cannot
// realistically wrap (at 512 KiB/s it takes over a million years).
inline unsigned long long used_bytes(unsigned long long head,
                                     unsigned long long tail) {
    return head >= tail ? head - tail : 0ull;
}

inline unsigned long long free_bytes(unsigned long long head,
                                     unsigned long long tail,
                                     unsigned int capacityBytes) {
    unsigned long long used = used_bytes(head, tail);
    return used >= (unsigned long long)capacityBytes
        ? 0ull : (unsigned long long)capacityBytes - used;
}

// Total ring bytes one payload occupies, framing included.
inline unsigned int record_bytes(unsigned int payloadBytes) {
    return payloadBytes + (unsigned int)kHeaderBytes;
}

// Whether a payload may be committed right now.
//
// A payload longer than kMaxRecordBytes is refused rather than truncated: the
// producer's buffer is that size, so a longer one means a caller bypassed
// debug_log()'s formatter and the honest answer is the dropped-line marker,
// not a silently clipped line that reads as complete.
inline bool fits(unsigned long long head, unsigned long long tail,
                 unsigned int capacityBytes, unsigned int payloadBytes) {
    if (payloadBytes == 0 || payloadBytes > (unsigned int)kMaxRecordBytes)
        return false;
    if (capacityBytes < record_bytes((unsigned int)kMaxRecordBytes))
        return false;
    return free_bytes(head, tail, capacityBytes) >=
        (unsigned long long)record_bytes(payloadBytes);
}

// Where a monotonic position lands in the backing array.
inline unsigned int offset_of(unsigned long long position,
                              unsigned int capacityBytes) {
    if (capacityBytes == 0) return 0;
    return (unsigned int)(position % (unsigned long long)capacityBytes);
}

// How many of `bytes` can be copied before the array end forces a wrap. The
// remainder (bytes - first_span) continues at offset 0, so a record may span
// the seam rather than needing the ring to reserve padding for it.
inline unsigned int first_span(unsigned int offset, unsigned int capacityBytes,
                               unsigned int bytes) {
    if (capacityBytes == 0 || offset >= capacityBytes) return 0;
    unsigned int untilEnd = capacityBytes - offset;
    return bytes < untilEnd ? bytes : untilEnd;
}

// Whether a committed record's length prefix is usable. A crash-path drain
// reads the ring WITHOUT the producer lock, so it must be able to reject a
// nonsense length instead of walking off the end of the committed region.
inline bool record_length_is_valid(unsigned int payloadBytes,
                                   unsigned long long head,
                                   unsigned long long tail) {
    if (payloadBytes == 0 || payloadBytes > (unsigned int)kMaxRecordBytes)
        return false;
    return used_bytes(head, tail) >=
        (unsigned long long)record_bytes(payloadBytes);
}

// Written by the writer thread when it resumes after an overflow, so a reader
// of the log can tell "nothing happened" from "the writer could not keep up".
// printf-style with one unsigned long long argument.
inline const char* dropped_marker_format() {
    return "debug log: %llu line(s) dropped -- the log writer could not keep up "
           "with producers (disk stall or burst); request handling was NOT "
           "blocked\n";
}

} // namespace gc_debug_log_queue

#endif // GREEN_CURVE_DEBUG_LOG_QUEUE_POLICY_H
