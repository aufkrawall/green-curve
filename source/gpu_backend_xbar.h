// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// XBAR clock and XBAR-domain MSVDD offsets through the private Windows NvAPI
// ClkDomains surface.  The interface is old (10+ years) and its .domains
// sub-structure is versioned per generation: the driver reports which schema
// it answered with in the response's version word, and that word — not the
// GPU family — selects the layout used here.  XBAR exists as an independent
// domain since Volta, so availability is decided by the reported version and
// the pinned marker validation, never by an architecture whitelist.
//
// The complete control block is read as a template for every transaction; a
// write changes only the two owned fields and is accepted only after an exact
// readback.  A response whose version word has no validated schema row is
// refused for both reads and writes, and logged with enough detail (decoded
// version/size words plus a bounded head dump) to add the row from a remote
// diagnostic report without guessing at privileged-write offsets.
//
// References: LACT issue 1147, the R572..R610 NvAPI validation matrix, and
// the generation-independence guidance for NvAPI_GPU_ClockClkDomainsSetControl.

#ifndef GREEN_CURVE_GPU_BACKEND_XBAR_H
#define GREEN_CURVE_GPU_BACKEND_XBAR_H

#include <string.h>

// These are Windows NvAPI interface IDs, not Linux RM command IDs.  In
// particular, PropRels controls the GPC->XBAR propagation ratio and uses a
// different structure; it must never be mistaken for ClkDomains.
#define XBAR_NVAPI_CLK_DOMAINS_GET_CONTROL 0xF58938F5u
#define XBAR_NVAPI_CLK_DOMAINS_SET_CONTROL 0xD14B69CFu
#define XBAR_NVAPI_CLK_DOMAINS_VERSION     0x000261A4u
#define XBAR_NVAPI_CLK_MEASURE             0x527FC458u
#define XBAR_NVAPI_CLK_MEASURE_VERSION     0x0001000Cu

// Standard NvAPI status returned when a driver does not implement the struct
// version a caller requested.  Seeing it means the adapter speaks an older
// ClkDomains schema than every row we currently pin.
#define XBAR_NVAPI_STATUS_INCOMPATIBLE_STRUCT_VERSION (-9)

static const unsigned int XBAR_CONTROL_BUF_SIZE = 0x13000;
static const unsigned int XBAR_CONTROL_MASK_OFFSET = 8;
static const unsigned int XBAR_CONTROL_DOMAIN_MASK = 0xff;
static const unsigned int XBAR_DOMAIN_MARKER = 0x0f;

// The only layout validated against the R572..R610 ClkDomains V2 response
// (version word 0x000261A4 = version 2, struct size 0x61A4).  Older
// generations report their own version word with a different .domains layout:
// each becomes one additional table row below once a real response has been
// validated on hardware.  Guessing an unpinned layout is unsafe because
// SET_CONTROL receives the complete block; an unknown version therefore leaves
// the domain unavailable instead of selecting the "most plausible" words.
static const unsigned int XBAR_PINNED_ENTRY_BASE = 0x124;
static const unsigned int XBAR_PINNED_ENTRY_STRIDE = 0x304;
static const unsigned int XBAR_PINNED_DOMAIN_COUNT = 8;
static const unsigned int XBAR_PINNED_XBAR_ENTRY_INDEX = 1;
static const unsigned int XBAR_FREQ_OFFSET_FIELD = 0x114;
static const unsigned int XBAR_MSVDD_OFFSET_FIELD = 0x11c;
static const unsigned int XBAR_MEASURE_DOMAIN_XBAR = 2;
static const unsigned int XBAR_MEASURE_VALUE_OFFSET = 8;

struct XbarClkDomainsSchema {
    // (schema version << 16) | struct size, as carried at buffer offset 0 of
    // both requests and responses.
    unsigned int versionWord;
    // Repeated-domain-entry geometry and the two owned field offsets inside
    // the XBAR entry.
    unsigned int entryBase;
    unsigned int entryStride;
    unsigned int domainCount;
    unsigned int entryIndex;
    unsigned int entryMarker;
    unsigned int freqOffsetField;
    unsigned int msvddOffsetField;
    // Controllable-domain mask sent in the request header.
    unsigned int requestMask;
};

// Ordered newest-first: the request ladder tries rows from the top until the
// driver accepts one, so a future newer schema naturally prefers itself while
// older drivers fall through to their own row.
static const XbarClkDomainsSchema g_xbarSchemas[] = {
    { XBAR_NVAPI_CLK_DOMAINS_VERSION,
      XBAR_PINNED_ENTRY_BASE, XBAR_PINNED_ENTRY_STRIDE,
      XBAR_PINNED_DOMAIN_COUNT, XBAR_PINNED_XBAR_ENTRY_INDEX,
      XBAR_DOMAIN_MARKER, XBAR_FREQ_OFFSET_FIELD, XBAR_MSVDD_OFFSET_FIELD,
      XBAR_CONTROL_DOMAIN_MASK },
};
static const unsigned int XBAR_SCHEMA_COUNT =
    (unsigned int)(sizeof(g_xbarSchemas) / sizeof(g_xbarSchemas[0]));

// How the last read resolved against the pinned schema table.  This lets
// callers distinguish "our schema table cannot talk to this driver yet" from
// "a known schema was accepted but the domain genuinely refused": the first
// is incomplete tooling knowledge (no positive evidence about the domain),
// the second is real driver refusal.
#define XBAR_SCHEMA_STATUS_OK              0u
#define XBAR_SCHEMA_STATUS_UNKNOWN_VERSION 1u
#define XBAR_SCHEMA_STATUS_UNAVAILABLE     2u

struct XbarControlSnapshot {
    unsigned char buf[XBAR_CONTROL_BUF_SIZE];
    bool valid;
    unsigned int versionWord;
    unsigned int schemaStatus;
    unsigned int entryBase;
    unsigned int entryStride;
    unsigned int domainIndex;
    unsigned int freqFieldOffset;
    unsigned int msvddFieldOffset;
    int freqOffsetKhz;
    int msvddOffsetUv;
    unsigned int measuredKhz;
};

struct XbarBufferLayout {
    unsigned int entryBase;
    unsigned int entryStride;
    unsigned int domainIndex;
    unsigned int freqFieldOffset;
    unsigned int msvddFieldOffset;
};

static inline unsigned int xbar_get_u32(const unsigned char* buf,
                                        unsigned int offset) {
    unsigned int value = 0;
    if (buf) memcpy(&value, buf + offset, sizeof(value));
    return value;
}

static inline void xbar_put_u32(unsigned char* buf, unsigned int offset,
                                unsigned int value) {
    if (buf) memcpy(buf + offset, &value, sizeof(value));
}

static inline void xbar_put_i32(unsigned char* buf, unsigned int offset,
                                int value) {
    xbar_put_u32(buf, offset, (unsigned int)value);
}

// Schema selection by REPORTED version word.  This is the generation
// dispatch: whatever the driver echoes back decides the implementation, per
// the same rule the documented NvAPI versioning scheme implies ((version <<
// 16) | size).  Returns nullptr for unknown words — callers must refuse all
// further access rather than interpret the buffer.
static inline const XbarClkDomainsSchema* xbar_schema_for_version_word(
    unsigned int versionWord) {
    for (unsigned int i = 0; i < XBAR_SCHEMA_COUNT; ++i) {
        if (g_xbarSchemas[i].versionWord == versionWord)
            return &g_xbarSchemas[i];
    }
    return nullptr;
}

// Accept only the exact validated repeated-entry schema of the given row.
// This is deliberately not a "find the most plausible pattern" scanner:
// hostile or malformed driver bytes can otherwise select a decoy stride, and
// SET_CONTROL sends the complete buffer back to privileged hardware state.
static inline bool xbar_layout_for_buffer(const unsigned char* buf,
                                          const XbarClkDomainsSchema* schema,
                                          XbarBufferLayout* layout) {
    if (!buf || !schema || !layout) return false;
    if (!schema->entryStride || !schema->domainCount) return false;
    for (unsigned int index = 0; index < schema->domainCount; ++index) {
        unsigned long long markerOffset =
            (unsigned long long)schema->entryBase +
            (unsigned long long)index * schema->entryStride;
        if (markerOffset + sizeof(unsigned int) > XBAR_CONTROL_BUF_SIZE ||
            xbar_get_u32(buf, (unsigned int)markerOffset) !=
                schema->entryMarker) {
            return false;
        }
    }
    // Field offsets are stored ABSOLUTE (buffer-relative) so reads and writes
    // never depend on recomputing entry geometry.
    layout->entryBase = schema->entryBase;
    layout->entryStride = schema->entryStride;
    layout->domainIndex = schema->entryIndex;
    layout->freqFieldOffset = schema->entryBase +
        schema->entryIndex * schema->entryStride + schema->freqOffsetField;
    layout->msvddFieldOffset = schema->entryBase +
        schema->entryIndex * schema->entryStride + schema->msvddOffsetField;
    unsigned long long freqEnd = layout->freqFieldOffset;
    freqEnd += sizeof(unsigned int);
    unsigned long long msvddEnd = layout->msvddFieldOffset;
    msvddEnd += sizeof(unsigned int);
    return freqEnd <= XBAR_CONTROL_BUF_SIZE &&
           msvddEnd <= XBAR_CONTROL_BUF_SIZE;
}

// Status decoding keeps remote diagnostics actionable: a pre-Blackwell owner
// seeing INCOMPATIBLE_STRUCT_VERSION knows immediately that the fix is a new
// pinned schema row for their reported version, not a broken install.
static inline void xbar_log_control_status(const char* what, int status) {
    if (status == XBAR_NVAPI_STATUS_INCOMPATIBLE_STRUCT_VERSION) {
        debug_log("xbar: %s failed status=%d"
                  " (NVAPI_INCOMPATIBLE_STRUCT_VERSION: driver rejected the"
                  " requested struct version)\n", what, status);
    } else {
        debug_log("xbar: %s failed status=0x%X\n", what, (unsigned)status);
    }
}

// Unknown-version diagnostics: decode both words and dump a bounded prefix of
// the untouched response.  This is the evidence a Volta..Ada owner can report
// so the matching schema row can be pinned and validated upstream.
static inline void xbar_log_unknown_response_version(
    const unsigned char* buf, unsigned int requestedWord,
    unsigned int responseWord) {
    debug_log("xbar_read: unvalidated ClkDomains response version word"
              " 0x%08X (version=%u size=%u bytes), requested 0x%08X;"
              " no pinned schema, refusing read/write access\n",
              responseWord, responseWord >> 16, responseWord & 0xFFFFu,
              requestedWord);
    debug_log("xbar_read: response head:");
    for (unsigned int i = 0; i < 16; ++i) {
        debug_log(" %08X", xbar_get_u32(buf, i * sizeof(unsigned int)));
    }
    debug_log("\n");
}

// NvApiFunc is the project-wide two-argument private-NvAPI entry signature.
static inline bool xbar_measure_clock(NvApiFunc measureFunc, void* gpuHandle,
                                      unsigned int* measuredKhz) {
    if (!measureFunc || !gpuHandle || !measuredKhz) return false;
    *measuredKhz = 0;
    unsigned int params[3] = {};
    params[0] = XBAR_NVAPI_CLK_MEASURE_VERSION;
    params[1] = XBAR_MEASURE_DOMAIN_XBAR;
    if (measureFunc(gpuHandle, params) != 0) return false;
    *measuredKhz = params[2];
    return *measuredKhz != 0;
}

// Reads the complete control block through the request ladder (newest pinned
// schema first) and validates the RESPONSE's version word against the schema
// table before interpreting anything.  On any refusal the snapshot is left
// marked invalid so stale values can never masquerade as fresh proof.
static inline bool xbar_read_control(NvApiFunc getControl, void* gpuHandle,
                                     XbarControlSnapshot* snap) {
    if (!getControl || !gpuHandle || !snap) return false;
    snap->valid = false;
    snap->schemaStatus = XBAR_SCHEMA_STATUS_UNAVAILABLE;
    const XbarClkDomainsSchema* matchedRequest = nullptr;
    int getStatus = 0;
    bool versionRejected = false;
    for (unsigned int i = 0; i < XBAR_SCHEMA_COUNT; ++i) {
        memset(snap->buf, 0, sizeof(snap->buf));
        xbar_put_u32(snap->buf, 0, g_xbarSchemas[i].versionWord);
        xbar_put_u32(snap->buf, XBAR_CONTROL_MASK_OFFSET,
                     g_xbarSchemas[i].requestMask);
        getStatus = getControl(gpuHandle, snap->buf);
        if (getStatus == 0) {
            matchedRequest = &g_xbarSchemas[i];
            break;
        }
        if (getStatus == XBAR_NVAPI_STATUS_INCOMPATIBLE_STRUCT_VERSION)
            versionRejected = true;
        xbar_log_control_status("GET_CONTROL", getStatus);
    }
    if (!matchedRequest) {
        // A driver rejecting every requested struct version is telling us our
        // request vocabulary is wrong for it — incomplete tooling knowledge,
        // not a refusal of the domain itself.
        snap->schemaStatus = versionRejected
            ? XBAR_SCHEMA_STATUS_UNKNOWN_VERSION
            : XBAR_SCHEMA_STATUS_UNAVAILABLE;
        debug_log("xbar_read: every pinned ClkDomains request version was"
                  " rejected (%u schemas%s)\n", XBAR_SCHEMA_COUNT,
                  versionRejected ? ", incompatible struct version" : "");
        return false;
    }
    unsigned int responseWord = xbar_get_u32(snap->buf, 0);
    const XbarClkDomainsSchema* responseSchema =
        xbar_schema_for_version_word(responseWord);
    if (!responseSchema) {
        snap->schemaStatus = XBAR_SCHEMA_STATUS_UNKNOWN_VERSION;
        xbar_log_unknown_response_version(snap->buf,
                                          matchedRequest->versionWord,
                                          responseWord);
        return false;
    }
    XbarBufferLayout layout{};
    if (!xbar_layout_for_buffer(snap->buf, responseSchema, &layout)) {
        snap->schemaStatus = XBAR_SCHEMA_STATUS_UNAVAILABLE;
        debug_log("xbar_read: pinned schema for version word 0x%08X did not"
                  " match the response\n", responseWord);
        return false;
    }
    snap->versionWord = responseWord;
    snap->schemaStatus = XBAR_SCHEMA_STATUS_OK;
    snap->entryBase = layout.entryBase;
    snap->entryStride = layout.entryStride;
    snap->domainIndex = layout.domainIndex;
    snap->freqFieldOffset = layout.freqFieldOffset;
    snap->msvddFieldOffset = layout.msvddFieldOffset;
    snap->freqOffsetKhz = (int)xbar_get_u32(snap->buf, layout.freqFieldOffset);
    snap->msvddOffsetUv =
        (int)xbar_get_u32(snap->buf, layout.msvddFieldOffset);
    snap->valid = true;
    // No per-read success log here: telemetry refreshes call this repeatedly.
    // The capability probe and the write path log the full schema details.
    return true;
}

static inline bool xbar_probe(NvApiFunc getControl, NvApiFunc measureFunc,
                              void* gpuHandle, XbarControlSnapshot* snap) {
    if (!snap) return false;
    memset(snap, 0, sizeof(*snap));
    if (!xbar_read_control(getControl, gpuHandle, snap)) return false;
    xbar_measure_clock(measureFunc, gpuHandle, &snap->measuredKhz);
    return true;
}

static inline bool xbar_write(NvApiFunc getControl, NvApiFunc setControl,
                              NvApiFunc measureFunc, void* gpuHandle,
                              XbarControlSnapshot* snap,
                              int freqKhz, int msvddUv,
                              bool writeFreq, bool writeMsvdd) {
    if (!snap || !getControl || !setControl || !gpuHandle) return false;
    if (!writeFreq && !writeMsvdd) return false;
    // Always use a fresh response as the full-block preimage.  A stale probe
    // snapshot could overwrite an unrelated clock-domain field changed since
    // startup.  The fresh read also re-validates the schema: if the driver or
    // adapter changed underneath us, this refuses instead of writing blind.
    if (!xbar_read_control(getControl, gpuHandle, snap)) return false;
    if (writeFreq)
        xbar_put_i32(snap->buf, snap->freqFieldOffset, freqKhz);
    if (writeMsvdd)
        xbar_put_i32(snap->buf, snap->msvddFieldOffset, msvddUv);
    int setStatus = setControl(gpuHandle, snap->buf);
    if (setStatus != 0) {
        xbar_log_control_status("SET_CONTROL", setStatus);
        return false;
    }
    if (!xbar_read_control(getControl, gpuHandle, snap)) {
        debug_log("xbar_write: post-set GET_CONTROL failed\n");
        return false;
    }
    if (writeFreq && snap->freqOffsetKhz != freqKhz) {
        debug_log("xbar_write: frequency mismatch requested=%d readback=%d\n",
                  freqKhz, snap->freqOffsetKhz);
        return false;
    }
    if (writeMsvdd && snap->msvddOffsetUv != msvddUv) {
        debug_log("xbar_write: MSVDD mismatch requested=%d readback=%d\n",
                  msvddUv, snap->msvddOffsetUv);
        return false;
    }
    xbar_measure_clock(measureFunc, gpuHandle, &snap->measuredKhz);
    debug_log("xbar_write: ok schema=0x%08X base=0x%03X stride=0x%03X"
              " domain=%u freq=%d kHz msvdd=%d uV measured=%u kHz\n",
              snap->versionWord, snap->entryBase, snap->entryStride,
              snap->domainIndex, snap->freqOffsetKhz, snap->msvddOffsetUv,
              snap->measuredKhz);
    return true;
}

// Reset means stock for both owned fields.  Reading immediately before the
// SET preserves every other field in the current hardware block.
static inline bool xbar_reset_to_stock(NvApiFunc getControl,
                                       NvApiFunc setControl,
                                       NvApiFunc measureFunc,
                                       void* gpuHandle,
                                       XbarControlSnapshot* snap) {
    return xbar_write(getControl, setControl, measureFunc, gpuHandle, snap,
                      0, 0, true, true);
}

#endif
