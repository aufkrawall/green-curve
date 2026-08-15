// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// The two read loops that consume an already-opened update response, with the
// transport behind a two-function seam so they can be driven by a fake.
//
// ## Why these loops and not the whole fetch
//
// The connection half of `main_service_update_fetch.cpp` -- session setup, TLS
// pinning, the hand-validated redirect chain -- stays where it is.  Its
// security-critical *predicates* (`gc_update_url_is_acceptable`,
// `gc_update_host_is_allowed`, `gc_update_redirect_is_acceptable`) are already
// pure and already asserted, and refactoring the code that does TLS and
// connection handling to test the loop around them is a poor risk trade on the
// highest-consequence I/O path in the application.
//
// These two loops are different: they carry a property that is BOTH
// security-relevant and completely invisible when it regresses.
//
// ## The property
//
// `gc_update_stream_asset()` compares the running total against the size the
// signature-verified manifest declared **before** it hands a chunk to the sink.
// A hostile or broken server therefore cannot make a LocalSystem service write
// more than the manifest said, no matter how much it sends.
//
// Moving that check after the write, or to the end of the loop, changes nothing
// a user or a test could see: the download is still refused, the digest still
// fails, the update still does not install.  It just writes the attacker's
// entire response to disk first.  There is no crash, no log line and no failing
// assertion -- which is exactly the shape of regression that needs a test
// rather than a comment.  Assertion 4383 pins the ordering by measuring what
// the sink actually received.
//
// ## The seam is a transcription, not a redesign
//
// `available` + `read` map one-to-one onto `WinHttpQueryDataAvailable` and
// `WinHttpReadData`, in that order, with the same treatment of a zero-length
// read and the same chunk clamping.  That is deliberate: a seam that
// "improved" the loops would mean the new tests assert new behaviour, and the
// refactor itself would rest on review alone.  This way the tests describe what
// the shipped code already did.

#ifndef GREEN_CURVE_UPDATE_TRANSPORT_POLICY_H
#define GREEN_CURVE_UPDATE_TRANSPORT_POLICY_H

#include <stddef.h>

#include "update_manifest_policy.h"   // GC_UPDATE_ASSET_MAX_BYTES

// An opened response body.  Both calls return false for a transport failure,
// which is distinct from `*bytes == 0` on `available` -- that means the body is
// finished and is the loops' only successful exit.
struct GcUpdateReader {
    bool (*available)(void* ctx, size_t* bytes);
    bool (*read)(void* ctx, void* buffer, size_t want, size_t* got);
    void* ctx;
};

// Where a streamed asset goes.  Behind a seam only so the loop can be tested
// without a file; the real one is a WriteFile onto the staged handle, and a
// short write is a failure exactly as it is there.
struct GcUpdateSink {
    bool (*write)(void* ctx, const void* buffer, size_t bytes);
    void* ctx;
};

// Every distinguishable outcome, kept separate rather than collapsed into a
// bool because each maps to a different sentence in the debug log and on the
// GUI's status line -- and "the update did nothing" is otherwise
// undiagnosable.  The Win32 caller turns these back into the exact messages it
// always produced, including the GetLastError() values a pure function has no
// way to know.
enum GcUpdateFetchResult {
    GC_UPDATE_FETCH_OK = 0,
    GC_UPDATE_FETCH_QUERY_FAILED = 1,
    GC_UPDATE_FETCH_READ_FAILED = 2,
    // Larger than the caller's own ceiling, which is never the server's
    // Content-Length.
    GC_UPDATE_FETCH_TOO_LARGE = 3,
    GC_UPDATE_FETCH_EMPTY = 4,
    GC_UPDATE_FETCH_WRITE_FAILED = 5,
    GC_UPDATE_FETCH_SIZE_MISMATCH = 6,
    GC_UPDATE_FETCH_BAD_EXPECTED_SIZE = 7,
    GC_UPDATE_FETCH_BAD_ARGUMENTS = 8,
};

// Fetch a small document (the manifest or its signature) into a caller buffer.
//
// The ceiling is the CALLER's buffer, never anything the server said.  The
// refusal is raised from `available` before a single byte of an oversized
// document is copied, which is why the check is `total + available + 1` rather
// than a test on what was actually read.
static inline GcUpdateFetchResult gc_update_read_document(
    const GcUpdateReader* reader, char* out, size_t outSize, size_t* outLen) {
    if (outLen) *outLen = 0;
    if (!reader || !reader->available || !reader->read || !out || outSize == 0) {
        return GC_UPDATE_FETCH_BAD_ARGUMENTS;
    }
    out[0] = '\0';

    size_t total = 0;
    for (;;) {
        size_t available = 0;
        if (!reader->available(reader->ctx, &available)) {
            return GC_UPDATE_FETCH_QUERY_FAILED;
        }
        if (available == 0) break;
        // Leave room for the terminator; anything that does not fit means the
        // document is larger than the format permits, which is a refusal and
        // never a truncation -- a truncated manifest could parse as a valid
        // but different one.
        if (total + available + 1 > outSize) return GC_UPDATE_FETCH_TOO_LARGE;
        size_t got = 0;
        if (!reader->read(reader->ctx, out + total, available, &got) || got == 0) {
            return GC_UPDATE_FETCH_READ_FAILED;
        }
        total += got;
    }

    out[total] = '\0';
    if (outLen) *outLen = total;
    // A server that answers 200 with nothing is a failure, not an empty
    // manifest: the parse would refuse it anyway, with a worse message.
    if (total == 0) return GC_UPDATE_FETCH_EMPTY;
    return GC_UPDATE_FETCH_OK;
}

// Stream an asset into the sink, bounded by the size the signed manifest
// declared.
//
// `chunk` is the caller's scratch buffer; passing it in rather than allocating
// keeps this allocation-free and lets a test drive the chunking boundary
// directly.
static inline GcUpdateFetchResult gc_update_stream_asset(
    const GcUpdateReader* reader, const GcUpdateSink* sink,
    unsigned long long expectedBytes, void* chunk, size_t chunkSize,
    unsigned long long* totalOut) {
    if (totalOut) *totalOut = 0;
    if (!reader || !reader->available || !reader->read || !sink ||
        !sink->write || !chunk || chunkSize == 0) {
        return GC_UPDATE_FETCH_BAD_ARGUMENTS;
    }
    // The manifest is signature-verified, so an out-of-range size here means
    // the release tooling produced something this build will not stream --
    // refused before a connection's worth of data is touched.
    if (expectedBytes == 0 || expectedBytes > GC_UPDATE_ASSET_MAX_BYTES) {
        return GC_UPDATE_FETCH_BAD_EXPECTED_SIZE;
    }

    // `totalOut` is kept current at every exit, not just the successful one:
    // it is how many bytes actually reached the sink, and the caller logs it on
    // the oversize path to say where the abort happened.  Reporting zero there
    // would make the one diagnostic that proves the abort worked look like it
    // never ran.
    unsigned long long total = 0;
    for (;;) {
        size_t available = 0;
        if (!reader->available(reader->ctx, &available)) {
            if (totalOut) *totalOut = total;
            return GC_UPDATE_FETCH_QUERY_FAILED;
        }
        if (available == 0) break;
        size_t want = available > chunkSize ? chunkSize : available;
        size_t got = 0;
        if (!reader->read(reader->ctx, chunk, want, &got) || got == 0) {
            if (totalOut) *totalOut = total;
            return GC_UPDATE_FETCH_READ_FAILED;
        }
        // BEFORE the write, and this order is the whole point of the file.
        // Checking after would still refuse the update and would still fail the
        // digest -- after writing everything the server chose to send into a
        // directory a SYSTEM process launches executables from.
        if (total + (unsigned long long)got > expectedBytes) {
            if (totalOut) *totalOut = total;
            return GC_UPDATE_FETCH_TOO_LARGE;
        }
        if (!sink->write(sink->ctx, chunk, got)) {
            if (totalOut) *totalOut = total;
            return GC_UPDATE_FETCH_WRITE_FAILED;
        }
        total += (unsigned long long)got;
    }

    if (totalOut) *totalOut = total;
    // A SHORT file is caught here rather than being left to the digest, so the
    // log says "the server sent less than the manifest declared" instead of
    // "the hash did not match", which points at the wrong thing entirely.
    if (total != expectedBytes) return GC_UPDATE_FETCH_SIZE_MISMATCH;
    return GC_UPDATE_FETCH_OK;
}

#endif // GREEN_CURVE_UPDATE_TRANSPORT_POLICY_H
