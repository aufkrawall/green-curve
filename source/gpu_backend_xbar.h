// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// XBAR clock and XBAR-domain MSVDD offsets through the private NvAPI
// ClkDomains surface.  The interface is old (10+ years) and its .domains
// sub-structure can differ by generation even with the same version word.
// The returned word selects a candidate layout, whose repeated entry wire types
// must validate before any field is interpreted. Domain identities and units
// also need hardware evidence; availability has no architecture whitelist.
//
// The complete control block is read as a template for every transaction; a
// write changes only the two owned fields and is accepted only after an exact
// readback.  A response whose version word has no validated schema row is
// refused for both reads and writes, and logged with enough detail (decoded
// version/size words plus a bounded head dump) to add the row from a remote
// diagnostic report without guessing at privileged-write offsets.
//
// Validated against the R572..R610 NvAPI response family.  The implementation
// remains generation-independent by dispatching on the returned schema word.

#ifndef GREEN_CURVE_GPU_BACKEND_XBAR_H
#define GREEN_CURVE_GPU_BACKEND_XBAR_H

#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include "debug_log_steady_state_policy.h"

// These are NvAPI interface IDs, not Linux RM command IDs.  In
// particular, PropRels controls the GPC->XBAR propagation ratio and uses a
// different structure; it must never be mistaken for ClkDomains.
#define XBAR_NVAPI_CLK_DOMAINS_GET_CONTROL 0xF58938F5u
#define XBAR_NVAPI_CLK_DOMAINS_SET_CONTROL 0xD14B69CFu
#define XBAR_NVAPI_CLK_DOMAINS_VERSION     0x000261A4u
#define XBAR_NVAPI_CLK_MEASURE             0x527FC458u
#define XBAR_NVAPI_CLK_MEASURE_VERSION     0x0001000Cu
#define NVAPI_GPU_CLIENT_VOLT_RAILS_GET_STATUS         0x465F9BCFu
#define NVAPI_GPU_CLIENT_VOLT_RAILS_GET_STATUS_VERSION 0x0001004Cu

struct NvApiVoltRailsStatus {
    unsigned int version;
    unsigned int flags;
    unsigned int reserved1[8];
    unsigned int value_uV;
    unsigned int reserved2[8];
};
static_assert(sizeof(NvApiVoltRailsStatus) == 76, "NvApiVoltRailsStatus size");
static_assert(offsetof(NvApiVoltRailsStatus, value_uV) == 40, "NvApiVoltRailsStatus value_uV offset");

// Standard NvAPI status returned when a driver does not implement the struct
// version a caller requested.  Seeing it means the adapter speaks an older
// ClkDomains schema than every row we currently pin.
#define XBAR_NVAPI_STATUS_INCOMPATIBLE_STRUCT_VERSION (-9)
// NvAPI gate for privileged operations: SET_CONTROL from a non-elevated
// caller is refused before the RM command is even dispatched (verified live:
// GET succeeds unprivileged, SET returns -137).  GUI and service apply paths
// run elevated, so end users only see this when invoking off-path tools.
#define XBAR_NVAPI_STATUS_INVALID_USER_PRIVILEGE (-137)
static_assert(XBAR_NVAPI_STATUS_INVALID_USER_PRIVILEGE == -137,
              "NVAPI_INVALID_USER_PRIVILEGE status code");

static const unsigned int XBAR_CONTROL_BUF_SIZE = 0x13000;
static const unsigned int XBAR_CONTROL_MASK_OFFSET = 8;
static const unsigned int XBAR_CONTROL_DOMAIN_MASK = 0xff;
// This is the pinned response's wire type, not a universal structure marker.
static const unsigned int XBAR_DOMAIN_MARKER = 0x0f;

// The only layout validated against the R572..R610 ClkDomains V2 response
// (version word 0x000261A4 = version 2, struct size 0x61A4).  Older
// generations can return that same word with a different .domains layout.
// Further layouts need a distinct, verified discriminator before the table can
// select their geometry. Guessing an unpinned layout is unsafe because
// SET_CONTROL receives the complete block; an unknown version therefore leaves
// the domain unavailable instead of selecting the "most plausible" words.
static const unsigned int XBAR_PINNED_ENTRY_BASE = 0x124;
static const unsigned int XBAR_PINNED_ENTRY_STRIDE = 0x304;
static const unsigned int XBAR_PINNED_DOMAIN_COUNT = 8;
static const unsigned int XBAR_PINNED_XBAR_ENTRY_INDEX = 1;
// MSVDD voltage lives in domain-entry 0, NOT in the XBAR-frequency entry 1.
// Observed driver behaviour: the driver validates the voltage field at buffer
// offset 0x23c = entry 0 (0x124) + 0x118.  Writing
// to entry 1 + 0x11c (0x544) was rejected by the RM driver with status
// 0xFFFFFFFF because that offset belongs to the SYS domain's second voltage
// rail, which Blackwell does not support.
static const unsigned int XBAR_PINNED_MSVDD_ENTRY_INDEX = 0;
// Second aux clock entry, identified empirically on RTX 5070 / 610.88:
// writing +50 kHz-MHz here moves physical CLK_MEASURE domain 2 by the same
// amount (reproduced across idle and load states), while entry 1 drives
// measure domain 1.  The community labels these two knobs XBAR and SYS.
static const unsigned int XBAR_PINNED_SYS_ENTRY_INDEX = 3;
// Video clock entry, identified by an exact before/after control-block diff on
// RTX 5070 / 610.88: a +400 MHz video offset changed only the standard field at
// 0xE48 (entry 4 + 0x114).  The video engine clock is not visible in a
// CLK_MEASURE domain, so post-write verification relies on exact readback.
static const unsigned int XBAR_PINNED_VIDEO_ENTRY_INDEX = 4;
static const unsigned int XBAR_FREQ_OFFSET_FIELD = 0x114;
// MSVDD voltage field offset within its entry.  This is +0x118, NOT +0x11c.
// The driver accepts buf[0x23c] = entry 0 + 0x118 within [-500000, 500000] uV.
static const unsigned int XBAR_MSVDD_OFFSET_FIELD = 0x118;
// Physical-clock ids for verification/display, identified empirically
// (differential writes): entry 1 drives CLK_MEASURE domain 1, entry 3
// drives domain 2, entry 4 (video) drives NONE - its engine clock is not
// measurable through this interface.
static const unsigned int XBAR_MEASURE_DOMAIN_XBAR = 1;
static const unsigned int XBAR_MEASURE_DOMAIN_SYS = 2;
static const unsigned int XBAR_MEASURE_VALUE_OFFSET = 8;

struct XbarClkDomainsSchema {
    // (schema version << 16) | struct size, as carried at buffer offset 0 of
    // both requests and responses.
    unsigned int versionWord;
    // Repeated-domain-entry geometry.
    unsigned int entryBase;
    unsigned int entryStride;
    unsigned int domainCount;
    // XBAR frequency offset lives in entryIndex (entry 1 on Blackwell).
    unsigned int entryIndex;
    unsigned int entryMarker; // Required wire type for this validated layout.
    unsigned int freqOffsetField;
    // MSVDD voltage offset lives in msvddEntryIndex (entry 0 on Blackwell),
    // which may differ from the XBAR-frequency entry.
    unsigned int msvddEntryIndex;
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
      XBAR_DOMAIN_MARKER, XBAR_FREQ_OFFSET_FIELD,
      XBAR_PINNED_MSVDD_ENTRY_INDEX, XBAR_MSVDD_OFFSET_FIELD,
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

// Which check refused the last read.  xbar_read_control_quiet() records it
// with the rejected GET_CONTROL statuses so the caller decides how often the
// diagnostics are logged (xbar_log_read_outcome()).
#define XBAR_READ_REFUSAL_NONE             0u
#define XBAR_READ_REFUSAL_REQUEST_REJECTED 1u  // every request version failed
#define XBAR_READ_REFUSAL_UNKNOWN_VERSION  2u  // response word has no schema row
#define XBAR_READ_REFUSAL_LAYOUT_MISMATCH  3u  // pinned entry markers absent

struct XbarControlSnapshot {
    unsigned char buf[XBAR_CONTROL_BUF_SIZE];
    bool valid;
    unsigned int versionWord;
    unsigned int schemaStatus;
    unsigned int entryBase;
    unsigned int entryStride;
    unsigned int domainIndex;       // XBAR freq entry index
    unsigned int msvddDomainIndex;  // MSVDD voltage entry index
    unsigned int freqFieldOffset;
    unsigned int msvddFieldOffset;
    int freqOffsetKhz;
    int msvddOffsetUv;
    unsigned int measuredKhz;
    unsigned int refusal;           // XBAR_READ_REFUSAL_*
    unsigned int requestWord;       // request version the driver answered
    unsigned int rejectedCount;     // GET_CONTROL failures before an answer
    int rejectedStatus[XBAR_SCHEMA_COUNT];
    bool versionRejected;
};

struct XbarBufferLayout {
    unsigned int entryBase;
    unsigned int entryStride;
    unsigned int domainIndex;
    unsigned int msvddDomainIndex;
    unsigned int freqFieldOffset;
    unsigned int msvddFieldOffset;
};

// The Windows service supplies debug_log; the Linux daemon overrides this
// before including the shared transaction header so diagnostics reach its
// journal logger without duplicating the schema or write logic.
#ifndef XBAR_LOG
#define XBAR_LOG debug_log
#define GREEN_CURVE_XBAR_LOG_WAS_DEFAULTED
#endif

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
    layout->msvddDomainIndex = schema->msvddEntryIndex;
    layout->freqFieldOffset = schema->entryBase +
        schema->entryIndex * schema->entryStride + schema->freqOffsetField;
    layout->msvddFieldOffset = schema->entryBase +
        schema->msvddEntryIndex * schema->entryStride + schema->msvddOffsetField;
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
        XBAR_LOG("xbar: %s failed status=%d"
                  " (NVAPI_INCOMPATIBLE_STRUCT_VERSION: driver rejected the"
                  " requested struct version)\n", what, status);
    } else if (status == XBAR_NVAPI_STATUS_INVALID_USER_PRIVILEGE) {
        XBAR_LOG("xbar: %s failed status=%d"
                  " (NVAPI_INVALID_USER_PRIVILEGE: this operation requires an"
                  " elevated caller; the app's apply paths run elevated)\n",
                  what, status);
    } else {
        XBAR_LOG("xbar: %s failed status=0x%X\n", what, (unsigned)status);
    }
}

// Unknown-version diagnostics: decode both words and dump a bounded prefix of
// the untouched response.  This is the evidence a Volta..Ada owner can report
// so the matching schema row can be pinned and validated upstream.
static inline void xbar_log_unknown_response_version(
    const unsigned char* buf, unsigned int requestedWord,
    unsigned int responseWord) {
    XBAR_LOG("xbar_read: unvalidated ClkDomains response version word"
              " 0x%08X (version=%u size=%u bytes), requested 0x%08X;"
              " no pinned schema, refusing read/write access\n",
              responseWord, responseWord >> 16, responseWord & 0xFFFFu,
              requestedWord);
    XBAR_LOG("xbar_read: response head:");
    for (unsigned int i = 0; i < 16; ++i) {
        XBAR_LOG(" %08X", xbar_get_u32(buf, i * sizeof(unsigned int)));
    }
    XBAR_LOG("\n");
}

// NvApiFunc is the project-wide two-argument private-NvAPI entry signature.
static inline bool xbar_measure_clock(NvApiFunc measureFunc, void* gpuHandle,
                                      unsigned int measureDomain,
                                      unsigned int* measuredKhz) {
    if (!measureFunc || !gpuHandle || !measuredKhz) return false;
    *measuredKhz = 0;
    unsigned int params[3] = {};
    params[0] = XBAR_NVAPI_CLK_MEASURE_VERSION;
    params[1] = measureDomain;
    if (measureFunc(gpuHandle, params) != 0) return false;
    *measuredKhz = params[2];
    return *measuredKhz != 0;
}

static inline bool xbar_measure_voltage(NvApiFunc voltFunc, void* gpuHandle,
                                        unsigned int* measuredUv) {
    if (!voltFunc || !gpuHandle || !measuredUv) return false;
    *measuredUv = 0;
    NvApiVoltRailsStatus rails = {};
    rails.version = NVAPI_GPU_CLIENT_VOLT_RAILS_GET_STATUS_VERSION;
    if (voltFunc(gpuHandle, &rails) != 0) return false;
    *measuredUv = rails.value_uV;
    return *measuredUv != 0;
}

// Reads the complete control block through the request ladder (newest pinned
// schema first) and validates the RESPONSE's version word against the schema
// table before interpreting anything.  On any refusal the snapshot is left
// marked invalid so stale values can never masquerade as fresh proof.
// Logs nothing: the refusal is recorded in the snapshot for the caller.
static inline bool xbar_read_control_quiet(NvApiFunc getControl,
                                           void* gpuHandle,
                                           XbarControlSnapshot* snap) {
    if (!snap) return false;
    snap->refusal = XBAR_READ_REFUSAL_NONE;
    snap->requestWord = 0;
    snap->rejectedCount = 0;
    snap->versionRejected = false;
    if (!getControl || !gpuHandle) return false;
    snap->valid = false;
    snap->schemaStatus = XBAR_SCHEMA_STATUS_UNAVAILABLE;
    const XbarClkDomainsSchema* matchedRequest = nullptr;
    for (unsigned int i = 0; i < XBAR_SCHEMA_COUNT; ++i) {
        memset(snap->buf, 0, sizeof(snap->buf));
        xbar_put_u32(snap->buf, 0, g_xbarSchemas[i].versionWord);
        xbar_put_u32(snap->buf, XBAR_CONTROL_MASK_OFFSET,
                     g_xbarSchemas[i].requestMask);
        int getStatus = getControl(gpuHandle, snap->buf);
        if (getStatus == 0) {
            matchedRequest = &g_xbarSchemas[i];
            break;
        }
        if (getStatus == XBAR_NVAPI_STATUS_INCOMPATIBLE_STRUCT_VERSION)
            snap->versionRejected = true;
        snap->rejectedStatus[snap->rejectedCount++] = getStatus;
    }
    if (!matchedRequest) {
        // A driver rejecting every requested struct version is telling us our
        // request vocabulary is wrong for it — incomplete tooling knowledge,
        // not a refusal of the domain itself.
        snap->schemaStatus = snap->versionRejected
            ? XBAR_SCHEMA_STATUS_UNKNOWN_VERSION
            : XBAR_SCHEMA_STATUS_UNAVAILABLE;
        snap->refusal = XBAR_READ_REFUSAL_REQUEST_REJECTED;
        return false;
    }
    snap->requestWord = matchedRequest->versionWord;
    unsigned int responseWord = xbar_get_u32(snap->buf, 0);
    const XbarClkDomainsSchema* responseSchema =
        xbar_schema_for_version_word(responseWord);
    if (!responseSchema) {
        snap->schemaStatus = XBAR_SCHEMA_STATUS_UNKNOWN_VERSION;
        snap->refusal = XBAR_READ_REFUSAL_UNKNOWN_VERSION;
        return false;
    }
    XbarBufferLayout layout{};
    if (!xbar_layout_for_buffer(snap->buf, responseSchema, &layout)) {
        snap->schemaStatus = XBAR_SCHEMA_STATUS_UNAVAILABLE;
        snap->refusal = XBAR_READ_REFUSAL_LAYOUT_MISMATCH;
        return false;
    }
    snap->versionWord = responseWord;
    snap->schemaStatus = XBAR_SCHEMA_STATUS_OK;
    snap->entryBase = layout.entryBase;
    snap->entryStride = layout.entryStride;
    snap->domainIndex = layout.domainIndex;
    snap->msvddDomainIndex = layout.msvddDomainIndex;
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

// Every diagnostic line of the read recorded in `snap`, in ladder order.
static inline void xbar_log_read_outcome(const XbarControlSnapshot* snap) {
    if (!snap) return;
    for (unsigned int i = 0; i < snap->rejectedCount; ++i)
        xbar_log_control_status("GET_CONTROL", snap->rejectedStatus[i]);
    const unsigned int responseWord = xbar_get_u32(snap->buf, 0);
    if (snap->refusal == XBAR_READ_REFUSAL_REQUEST_REJECTED) {
        XBAR_LOG("xbar_read: every pinned ClkDomains request version was"
                  " rejected (%u schemas%s)\n", XBAR_SCHEMA_COUNT,
                  snap->versionRejected ? ", incompatible struct version" : "");
    } else if (snap->refusal == XBAR_READ_REFUSAL_UNKNOWN_VERSION) {
        xbar_log_unknown_response_version(snap->buf, snap->requestWord,
                                          responseWord);
    } else if (snap->refusal == XBAR_READ_REFUSAL_LAYOUT_MISMATCH) {
        XBAR_LOG("xbar_read: pinned schema for version word 0x%08X did not"
                  " match the response\n", responseWord);
    }
}

// One-shot reads (probes, write preimages, self-test) log every refusal.
static inline bool xbar_read_control(NvApiFunc getControl, void* gpuHandle,
                                     XbarControlSnapshot* snap) {
    const bool ok = xbar_read_control_quiet(getControl, gpuHandle, snap);
    xbar_log_read_outcome(snap);
    return ok;
}

// The refusal identity a timer-driven reader compares between reads.  The
// response head of an unknown version is not part of it.
static inline void xbar_read_outcome_key(const XbarControlSnapshot* snap,
                                         char* out, size_t outSize) {
    if (!out || outSize == 0) return;
    out[0] = '\0';
    if (!snap) return;
    const unsigned int responseWord =
        snap->refusal == XBAR_READ_REFUSAL_UNKNOWN_VERSION ||
        snap->refusal == XBAR_READ_REFUSAL_LAYOUT_MISMATCH
            ? xbar_get_u32(snap->buf, 0) : 0u;
    int used = snprintf(out, outSize, "refusal=%u request=0x%08X"
        " response=0x%08X statuses=", snap->refusal, snap->requestWord,
        responseWord);
    for (unsigned int i = 0; i < snap->rejectedCount && used > 0 &&
         (size_t)used < outSize; ++i) {
        int n = snprintf(out + used, outSize - (size_t)used, "%s%d",
                         i ? "," : "", snap->rejectedStatus[i]);
        if (n < 0) break;
        used += n;
    }
}

// A timer-driven reader sees the same refusal on every tick: a GPU whose
// ClkDomains layout this build has not pinned (Pascal answers the V2 request
// without the pinned entry markers) answers identically until its binding
// changes.  The first refusal and every change log in full, identical repeats
// are counted and summarized per interval (debug_log_steady_state_policy.h),
// and the first clean read after a refusal reports the unlogged count.  The
// caller serializes `tracker` and resets it when the GPU binding changes.
static inline void xbar_log_polled_read_outcome(gc_log_steady::Tracker* tracker,
                                                const XbarControlSnapshot* snap,
                                                bool ok,
                                                unsigned long long nowMs,
                                                bool verbose) {
    if (!snap) return;
    if (!tracker) {
        xbar_log_read_outcome(snap);
        return;
    }
    if (ok && snap->rejectedCount == 0) {
        if (tracker->haveKey || tracker->window.count) {
            XBAR_LOG("xbar_read: ClkDomains read accepted after a refusal"
                      " (%llu identical repeats not logged)\n",
                      tracker->window.count);
            memset(tracker, 0, sizeof(*tracker));
        }
        return;
    }
    char key[gc_log_steady::kKeyBytes] = {};
    xbar_read_outcome_key(snap, key, sizeof(key));
    gc_log_steady::Decision d =
        gc_log_steady::observe(tracker, key, nowMs, snap->refusal, verbose);
    if (gc_log_steady::wants_summary(d)) {
        XBAR_LOG("xbar_read: %s refusal repeated n=%llu windowS=%llu\n",
                  d.action == gc_log_steady::LOG_CHANGE ? "previous"
                                                        : "unchanged",
                  d.pending.count, d.pending.elapsedMs / 1000ull);
    }
    if (gc_log_steady::wants_full_line(d)) xbar_log_read_outcome(snap);
}

static inline bool xbar_probe(NvApiFunc getControl, NvApiFunc measureFunc,
                              void* gpuHandle, XbarControlSnapshot* snap) {
    if (!snap) return false;
    memset(snap, 0, sizeof(*snap));
    if (!xbar_read_control(getControl, gpuHandle, snap)) return false;
    xbar_measure_clock(measureFunc, gpuHandle, XBAR_MEASURE_DOMAIN_XBAR,
                       &snap->measuredKhz);
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
        XBAR_LOG("xbar_write: post-set GET_CONTROL failed\n");
        return false;
    }
    if (writeFreq && snap->freqOffsetKhz != freqKhz) {
        XBAR_LOG("xbar_write: frequency mismatch requested=%d readback=%d\n",
                  freqKhz, snap->freqOffsetKhz);
        return false;
    }
    if (writeMsvdd && snap->msvddOffsetUv != msvddUv) {
        XBAR_LOG("xbar_write: MSVDD mismatch requested=%d readback=%d\n",
                  msvddUv, snap->msvddOffsetUv);
        return false;
    }
    xbar_measure_clock(measureFunc, gpuHandle, XBAR_MEASURE_DOMAIN_XBAR,
                       &snap->measuredKhz);
    XBAR_LOG("xbar_write: ok schema=0x%08X base=0x%03X stride=0x%03X"
              " freqEntry=%u freqOff=0x%03X msvddEntry=%u msvddOff=0x%03X"
              " freq=%d kHz msvdd=%d uV measured=%u kHz\n",
              snap->versionWord, snap->entryBase, snap->entryStride,
              snap->domainIndex, snap->freqFieldOffset,
              snap->msvddDomainIndex, snap->msvddFieldOffset,
              snap->freqOffsetKhz, snap->msvddOffsetUv,
              snap->measuredKhz);
    return true;
}

static inline bool xbar_read_entry_freq(const XbarControlSnapshot* snap,
                                        unsigned int entryIndex,
                                        int* freqKhz);

// Generic single-field transaction for any pinned entry of the validated
// schema: fresh full-block preimage, one dword change, exact readback.  Used
// for the SYS domain (entry 3); the XBAR path additionally owns an MSVDD
// field and lives in xbar_write below.
static inline bool xbar_write_entry_freq(NvApiFunc getControl,
                                         NvApiFunc setControl, void* gpuHandle,
                                         XbarControlSnapshot* snap,
                                         unsigned int entryIndex,
                                         int freqKhz) {
    if (!snap || !getControl || !setControl || !gpuHandle) return false;
    if (!xbar_read_control(getControl, gpuHandle, snap)) return false;
    const XbarClkDomainsSchema* schema =
        xbar_schema_for_version_word(snap->versionWord);
    if (!schema || entryIndex >= schema->domainCount) return false;
    unsigned long long field =
        (unsigned long long)snap->entryBase + entryIndex * snap->entryStride +
        schema->freqOffsetField;
    if (field + sizeof(unsigned int) > XBAR_CONTROL_BUF_SIZE) return false;
    xbar_put_i32(snap->buf, (unsigned int)field, freqKhz);
    int setStatus = setControl(gpuHandle, snap->buf);
    if (setStatus != 0) {
        xbar_log_control_status("SET_CONTROL", setStatus);
        return false;
    }
    if (!xbar_read_control(getControl, gpuHandle, snap)) {
        XBAR_LOG("clk entry write: post-set GET_CONTROL failed\n");
        return false;
    }
    int readback = 0;
    if (!xbar_read_entry_freq(snap, entryIndex, &readback) ||
        readback != freqKhz) {
        XBAR_LOG("clk entry %u write: readback mismatch requested=%d"
                  " got=%d\n", entryIndex, freqKhz, readback);
        return false;
    }
    XBAR_LOG("clk entry %u write: ok offset=%d kHz\n", entryIndex, freqKhz);
    return true;
}

// Read one pinned entry from an already validated snapshot.  Resolve the
// field geometry from the RESPONSE schema, never from the first request-ladder
// row: future schema rows may move the field while retaining the same entry
// meaning.
static inline bool xbar_read_entry_freq(const XbarControlSnapshot* snap,
                                        unsigned int entryIndex,
                                        int* freqKhz) {
    if (!snap || !snap->valid || !freqKhz) return false;
    const XbarClkDomainsSchema* schema =
        xbar_schema_for_version_word(snap->versionWord);
    if (!schema || entryIndex >= schema->domainCount) return false;
    unsigned long long field =
        (unsigned long long)snap->entryBase + entryIndex * snap->entryStride +
        schema->freqOffsetField;
    if (field + sizeof(unsigned int) > XBAR_CONTROL_BUF_SIZE) return false;
    *freqKhz = (int)xbar_get_u32(snap->buf, (unsigned int)field);
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

#ifdef GREEN_CURVE_XBAR_LOG_WAS_DEFAULTED
#undef GREEN_CURVE_XBAR_LOG_WAS_DEFAULTED
#undef XBAR_LOG
#endif

#endif
