// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// Message-mode prefix read + bounded drain for the Windows service pipe.
//
// ImpersonateNamedPipeClient() requires the server to have completed at least
// one read on the connected pipe; calling it before the first read fails with
// ERROR_CANNOT_IMPERSONATE (1368). The 2026-08-22 live regression proved that
// ordering is mandatory, so the listener reads exactly the fixed-size request
// header first -- magic+version+command, pinned at offsets 0/4/8 --
// impersonates the verified client, and only then admits and finishes the
// message.
//
// On a message-mode pipe, ReadFile with a buffer smaller than the inbound
// message completes with ERROR_MORE_DATA while GetOverlappedResult reports the
// partial transfer. That partial completion IS the mandatory first read.
//
// service_pipe_read_exact()/service_pipe_write_exact() keep their strict
// whole-buffer contract; this header deliberately does not weaken them.

#ifndef GREEN_CURVE_SERVICE_PIPE_PREFIX_READ_H
#define GREEN_CURVE_SERVICE_PIPE_PREFIX_READ_H

#include "service_ipc_throttle_policy.h"
#include "service_protocol.h"

#include <stddef.h>

// magic(4) + version(4) + command(4). Pinned by static_asserts on the
// ServiceRequest layout in service_protocol.h; re-asserted here so the probe
// can never drift from the wire structure it interprets.
enum { SERVICE_REQUEST_HEADER_BYTES =
           offsetof(ServiceRequest, command) + sizeof(((ServiceRequest*)0)->command) };

struct ServicePipePrefixRead {
    unsigned char bytes[SERVICE_REQUEST_HEADER_BYTES];
    DWORD transferred;
    // True when exactly the header bytes arrived (the expected v20 case: the
    // message continues with the request body).
    bool gotExpectedPrefix;
    // True when the ENTIRE inbound message was only these bytes (no
    // ERROR_MORE_DATA pending). Not an error: the header fields decide what
    // happens next, and there is nothing left to drain.
    bool messageComplete;
};

namespace gc_pipe_prefix_internal {

inline void set_prefix_err(char* err, size_t errSize, const char* text) {
    if (err && errSize) {
        StringCchCopyA(err, errSize, text);
    }
}

inline void set_prefix_err_fmt(char* err, size_t errSize, const char* fmt,
                               DWORD value) {
    if (err && errSize) {
        StringCchPrintfA(err, errSize, fmt, value);
    }
}

} // namespace gc_pipe_prefix_internal

// Read exactly SERVICE_REQUEST_HEADER_BYTES from the front of the inbound
// message-mode message, tolerating the expected ERROR_MORE_DATA partial
// completion. Every timeout/failure path cancels AND JOINS the outstanding I/O
// before returning: CancelIoEx() only requests cancellation, and the stack
// OVERLAPPED/event must stay alive until its completion has been observed.
static bool service_pipe_read_request_header(HANDLE pipe,
        ServicePipePrefixRead* out, DWORD timeoutMs, char* err,
        size_t errSize) {
    if (err && errSize) err[0] = 0;
    if (!pipe || pipe == INVALID_HANDLE_VALUE || !out) {
        gc_pipe_prefix_internal::set_prefix_err(err, errSize,
            "Invalid service pipe handle for header probe");
        return false;
    }
    memset(out, 0, sizeof(*out));

    OVERLAPPED ov = {};
    ov.hEvent = CreateEventA(nullptr, TRUE, FALSE, nullptr);
    if (!ov.hEvent) {
        gc_pipe_prefix_internal::set_prefix_err_fmt(err, errSize,
            "Failed creating header-probe event (error %lu)", GetLastError());
        return false;
    }

    BOOL started = ReadFile(pipe, out->bytes, SERVICE_REQUEST_HEADER_BYTES,
        nullptr, &ov);
    DWORD startErr = started ? ERROR_SUCCESS : GetLastError();
    bool ok = false;
    if (started || startErr == ERROR_IO_PENDING ||
        startErr == ERROR_MORE_DATA) {
        DWORD wait = WaitForSingleObject(ov.hEvent, timeoutMs);
        if (wait == WAIT_OBJECT_0) {
            DWORD transferred = 0;
            BOOL result = GetOverlappedResult(pipe, &ov, &transferred, FALSE);
            DWORD completeErr = result ? ERROR_SUCCESS : GetLastError();
            out->transferred = transferred;
            if (!result && completeErr == ERROR_MORE_DATA &&
                transferred >= (DWORD)SERVICE_REQUEST_HEADER_BYTES) {
                // Expected path for a full v20 request: the header arrived and
                // the body follows in the same message.
                out->gotExpectedPrefix = true;
                ok = true;
            } else if (result &&
                       transferred == (DWORD)SERVICE_REQUEST_HEADER_BYTES) {
                // The entire message was exactly header-sized. Unusual but
                // well-defined: nothing further to read on this message.
                out->gotExpectedPrefix = true;
                out->messageComplete = true;
                ok = true;
            } else if (transferred == 0) {
                gc_pipe_prefix_internal::set_prefix_err(err, errSize,
                    "Client disconnected before sending the request header");
            } else {
                gc_pipe_prefix_internal::set_prefix_err_fmt(err, errSize,
                    "Truncated service request header (error %lu)",
                    completeErr);
            }
        } else if (wait == WAIT_TIMEOUT) {
            CancelIoEx(pipe, &ov);
            DWORD cancelled = 0;
            GetOverlappedResult(pipe, &ov, &cancelled, TRUE);
            gc_pipe_prefix_internal::set_prefix_err(err, errSize,
                "Timed out waiting for the service request header");
        } else {
            DWORD waitError = GetLastError();
            CancelIoEx(pipe, &ov);
            DWORD cancelled = 0;
            GetOverlappedResult(pipe, &ov, &cancelled, TRUE);
            gc_pipe_prefix_internal::set_prefix_err_fmt(err, errSize,
                "Failed waiting for the service request header (error %lu)",
                waitError);
        }
    } else {
        gc_pipe_prefix_internal::set_prefix_err_fmt(err, errSize,
            "Failed starting the header probe (error %lu)", startErr);
    }
    CloseHandle(ov.hEvent);
    return ok;
}

// Consume any further inbound bytes in fixed-size chunks until nothing is
// queued, the message stream ends, or the deadline expires. Used when the
// header already decided the response (protocol mismatch / refused admission /
// unknown command) and the declared body length therefore cannot be trusted.
//
// A PeekNamedPipe gate keeps this O(0) for honest clients that sent exactly
// one message -- they get their refusal immediately instead of after a full
// deadline -- while a flooding client is drained up to the fixed chunk cap and
// wall-clock bound. Cancels and joins any still-outstanding chunk read before
// its OVERLAPPED/event storage goes out of scope.
static bool service_pipe_drain_inbound_message(HANDLE pipe, DWORD timeoutMs,
        char* err, size_t errSize) {
    if (err && errSize) err[0] = 0;
    if (!pipe || pipe == INVALID_HANDLE_VALUE) {
        gc_pipe_prefix_internal::set_prefix_err(err, errSize,
            "Invalid service pipe handle for message drain");
        return false;
    }
    enum {
        DRAIN_CHUNK_BYTES = 256,
        DRAIN_MAX_CHUNKS = 64, // 16 KiB >> any legitimate protocol-v20 message
    };
    unsigned char scratch[DRAIN_CHUNK_BYTES];
    ULONGLONG startMs = GetTickCount64();

    for (int chunk = 0; chunk < DRAIN_MAX_CHUNKS; ++chunk) {
        // How much inbound data is queued right now? Zero means the client
        // sent everything it is going to (or went silent): done instantly.
        DWORD available = 0;
        if (!PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr)) {
            // Peer disconnected or pipe broken: nothing left to drain.
            return true;
        }
        if (available == 0) {
            // Nothing is queued: the client sent everything it is going to
            // (or went silent). Responding now is both correct and fast;
            // anything that arrives later is discarded at disconnect.
            return true;
        }
        ULONGLONG nowMs = GetTickCount64();
        DWORD remainingMs;
        if (service_ipc_time_at_or_after(startMs + timeoutMs, nowMs)) {
            remainingMs = 1;
        } else {
            remainingMs = timeoutMs - (DWORD)(nowMs - startMs);
            if (remainingMs == 0) remainingMs = 1;
        }

        OVERLAPPED ov = {};
        ov.hEvent = CreateEventA(nullptr, TRUE, FALSE, nullptr);
        if (!ov.hEvent) {
            gc_pipe_prefix_internal::set_prefix_err_fmt(err, errSize,
                "Failed creating drain event (error %lu)", GetLastError());
            return false;
        }
        BOOL started = ReadFile(pipe, scratch, DRAIN_CHUNK_BYTES, nullptr,
                                &ov);
        DWORD startErr = started ? ERROR_SUCCESS : GetLastError();
        bool drained = false;
        bool fault = false;
        if (started || startErr == ERROR_IO_PENDING ||
            startErr == ERROR_MORE_DATA) {
            DWORD wait = WaitForSingleObject(ov.hEvent, remainingMs);
            if (wait == WAIT_OBJECT_0) {
                DWORD transferred = 0;
                BOOL result =
                    GetOverlappedResult(pipe, &ov, &transferred, FALSE);
                DWORD completeErr = result ? ERROR_SUCCESS : GetLastError();
                if ((result || completeErr == ERROR_MORE_DATA) &&
                    transferred > 0) {
                    drained = transferred < (DWORD)DRAIN_CHUNK_BYTES ||
                              available <= (DWORD)DRAIN_CHUNK_BYTES;
                    // A short chunk means the message ended inside this
                    // read; a fully-consumed queue means nothing follows.
                } else if (completeErr == ERROR_BROKEN_PIPE ||
                           transferred == 0) {
                    drained = true;
                } else {
                    fault = true;
                    gc_pipe_prefix_internal::set_prefix_err_fmt(err, errSize,
                        "Failed draining service message (error %lu)",
                        completeErr);
                }
            } else {
                DWORD waitError = wait == WAIT_TIMEOUT ? ERROR_SUCCESS
                                                       : GetLastError();
                CancelIoEx(pipe, &ov);
                DWORD cancelled = 0;
                GetOverlappedResult(pipe, &ov, &cancelled, TRUE);
                if (wait == WAIT_TIMEOUT) {
                    gc_pipe_prefix_internal::set_prefix_err(err, errSize,
                        "Timed out draining an oversized or stalled service message");
                } else {
                    gc_pipe_prefix_internal::set_prefix_err_fmt(err, errSize,
                        "Failed waiting while draining the service message (error %lu)",
                        waitError);
                }
                fault = true;
            }
        } else if (startErr == ERROR_BROKEN_PIPE) {
            drained = true;
        } else {
            fault = true;
            gc_pipe_prefix_internal::set_prefix_err_fmt(err, errSize,
                "Failed starting drain read (error %lu)", startErr);
        }
        CloseHandle(ov.hEvent);
        if (fault) return false;
        if (drained) return true;
    }
    // Iteration cap hit without seeing the message end: treat as a transport
    // fault rather than reading forever.
    gc_pipe_prefix_internal::set_prefix_err(err, errSize,
        "Service message exceeded the drain budget");
    return false;
}

#endif // GREEN_CURVE_SERVICE_PIPE_PREFIX_READ_H
