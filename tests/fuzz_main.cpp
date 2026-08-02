// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// libFuzzer harnesses over the untrusted-input boundaries.
//
// Built and run by `python build.py --fuzz`.  One binary is produced per
// target: build.py compiles this file once per GC_FUZZ_TARGET value so each
// target keeps a focused coverage signal and its own corpus, which is how
// libFuzzer is meant to be driven.
//
// Note on the toolchain: llvm-mingw's clang driver REJECTS -fsanitize=fuzzer
// for the x86_64-w64-windows-gnu triple, but the gate is only on that
// convenience flag.  The instrumentation flag (-fsanitize-coverage=...) and
// the runtime archive (libclang_rt.fuzzer-x86_64.a) both ship and both work,
// so build.py links them directly.  See llm-wiki/testing.md.
//
// Every target does more than "did it crash": each asserts the post-conditions
// its function is supposed to guarantee.  A validator that returns "accepted"
// while leaving a field out of range is a finding even without a memory error.

#include "fan_curve.h"
#include "gpu_core.h"
#include "service_protocol.h"
#include "linux_vf_validation.h"
#include "linux_daemon_transport_policy.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// GC_FUZZ_TARGET values.  Keep in sync with FUZZ_TARGETS in build.py; a
// regression check in build.py enforces that both lists agree.
#define GC_FUZZ_SERVICE_REQUEST 1
#define GC_FUZZ_VF_SNAPSHOT     2
#define GC_FUZZ_TASK_XML        3
#define GC_FUZZ_CONFIG_STRINGS  4
#define GC_FUZZ_WIRE_PREFIX     5

#ifndef GC_FUZZ_TARGET
#error "GC_FUZZ_TARGET must be defined (see FUZZ_TARGETS in build.py)"
#endif

// Stubs for the GUI/service surface that the linked shards reference but no
// fuzz target reaches.  Mirrors tests/regression_main.cpp so both fixtures link
// the same production .cpp files without dragging in the whole application.
bool is_curve_point_visible_in_gui(int) { return true; }
void debug_log(const char*, ...) {}
void invalidate_tray_profile_cache() {}

// NDEBUG is set for these builds, so assert() is compiled out.  Invariant
// violations must still stop the run, and they must stop it in a way libFuzzer
// reports and reproduces, so abort() rather than a return.
#define GC_FUZZ_CHECK(cond, msg)                                              \
    do {                                                                      \
        if (!(cond)) {                                                        \
            fprintf(stderr,                                                   \
                    "\n*** fuzz invariant violated: %s\n"                     \
                    "    check: %s\n    at: %s:%d\n",                         \
                    (msg), #cond, __FILE__, __LINE__);                        \
            fflush(stderr);                                                   \
            abort();                                                          \
        }                                                                     \
    } while (0)

namespace {

// Deterministic sequential reader.  Past the end it yields zeroes rather than
// reading out of bounds, so a target's control flow stays reproducible for a
// given input regardless of how short that input is.
class FuzzInput {
public:
    FuzzInput(const uint8_t* data, size_t size)
        : data_(data), size_(size), pos_(0) {}

    uint8_t byte() { return pos_ < size_ ? data_[pos_++] : (uint8_t)0; }

    unsigned int u32() {
        unsigned int v = 0;
        for (int i = 0; i < 4; ++i) v = (v << 8) | byte();
        return v;
    }

    int i32() { return (int)u32(); }

    // Fill dst with the next n bytes, zero-padding once input is exhausted.
    void bytes(void* dst, size_t n) {
        uint8_t* out = (uint8_t*)dst;
        for (size_t i = 0; i < n; ++i) out[i] = byte();
    }

    size_t remaining() const { return pos_ < size_ ? size_ - pos_ : 0; }

private:
    const uint8_t* data_;
    size_t size_;
    size_t pos_;
};

// A NUL-terminated ASCII string built from fuzz bytes.  Bounded so the target
// exercises the parser rather than the allocator.
template <size_t N>
void fuzz_cstring(FuzzInput& in, char (&out)[N]) {
    size_t take = (size_t)in.byte() % N;
    for (size_t i = 0; i < take; ++i) out[i] = (char)in.byte();
    out[take] = '\0';
}

}  // namespace

// ---------------------------------------------------------------------------
// Target 1: the privileged IPC trust boundary.
//
// validate_service_request_for_ipc() is the single gate between an
// unprivileged caller and the service.  Anything it accepts reaches hardware
// writes and array indexing, so "accepted" must imply every documented clamp.
// ---------------------------------------------------------------------------
#if GC_FUZZ_TARGET == GC_FUZZ_SERVICE_REQUEST

static void check_desired_postconditions(const DesiredSettings& d) {
    for (int i = 0; i < VF_NUM_POINTS; ++i) {
        GC_FUZZ_CHECK(d.hasCurvePoint[i] <= 1,
                      "hasCurvePoint[] left non-canonical past the IPC boundary");
        GC_FUZZ_CHECK(d.curvePointMHz[i] <= 5000u,
                      "curvePointMHz[] exceeds the 5000 MHz clamp");
    }
    GC_FUZZ_CHECK(d.hasLock <= 1 && d.lockTracksAnchor <= 1 &&
                  d.hasGpuOffset <= 1 && d.hasMemOffset <= 1 &&
                  d.hasPowerLimit <= 1 && d.hasFan <= 1 && d.fanAuto <= 1 &&
                  d.resetOcBeforeApply <= 1,
                  "a gc_bool8 field left the IPC boundary non-canonical");

    // lockCi indexes VF_NUM_POINTS-sized arrays downstream; -1 is the
    // documented "no explicit lock" sentinel.
    GC_FUZZ_CHECK(d.lockCi >= -1 && d.lockCi < VF_NUM_POINTS,
                  "lockCi can index outside the VF arrays");
    GC_FUZZ_CHECK(d.lockMHz <= 5000u, "lockMHz exceeds the 5000 MHz clamp");
    GC_FUZZ_CHECK(d.lockMode >= LOCK_MODE_NONE && d.lockMode <= LOCK_MODE_HARD,
                  "lockMode is outside the enum range");
    GC_FUZZ_CHECK(d.gpuOffsetExcludeLowCount >= 0 &&
                  d.gpuOffsetExcludeLowCount <= VF_NUM_POINTS,
                  "gpuOffsetExcludeLowCount is out of range");

    if (d.hasPowerLimit)
        GC_FUZZ_CHECK(d.powerLimitPct >= 50 && d.powerLimitPct <= 150,
                      "powerLimitPct escaped the 50..150 clamp");
    if (d.hasGpuOffset)
        GC_FUZZ_CHECK(d.gpuOffsetMHz >= -1000 && d.gpuOffsetMHz <= 1000,
                      "gpuOffsetMHz escaped the +/-1000 clamp");
    if (d.hasMemOffset)
        GC_FUZZ_CHECK(d.memOffsetMHz >= -5000 && d.memOffsetMHz <= 5000,
                      "memOffsetMHz escaped the +/-5000 clamp");
    if (d.hasFan) {
        GC_FUZZ_CHECK(d.fanPercent >= 0 && d.fanPercent <= 100,
                      "fanPercent escaped the 0..100 clamp");
        GC_FUZZ_CHECK(d.fanMode >= FAN_MODE_AUTO && d.fanMode <= FAN_MODE_CURVE,
                      "fanMode is outside the enum range");
        for (int i = 0; i < FAN_CURVE_MAX_POINTS; ++i) {
            GC_FUZZ_CHECK(d.fanCurve.points[i].fanPercent >= 0 &&
                          d.fanCurve.points[i].fanPercent <= 100,
                          "fan curve point percent escaped the 0..100 clamp");
            GC_FUZZ_CHECK(d.fanCurve.points[i].temperatureC >= 0 &&
                          d.fanCurve.points[i].temperatureC <= 150,
                          "fan curve point temperature escaped the 0..150 clamp");
        }
        GC_FUZZ_CHECK(d.fanCurve.hysteresisC >= 0 &&
                      d.fanCurve.hysteresisC <= FAN_CURVE_MAX_HYSTERESIS_C,
                      "fan curve hysteresis escaped its clamp");
        GC_FUZZ_CHECK(d.fanCurve.pollIntervalMs >= 1,
                      "fan curve poll interval would spin a runtime loop");
    }
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    ServiceRequest request;
    memset(&request, 0, sizeof(request));

    FuzzInput in(data, size);
    const uint8_t mode = in.byte();

    // Raw wire image.  Short inputs leave the tail zeroed.
    uint8_t* raw = (uint8_t*)&request;
    size_t copy = in.remaining();
    if (copy > sizeof(request)) copy = sizeof(request);
    in.bytes(raw, copy);

    // Without help the fuzzer spends nearly all its budget failing the magic
    // and version words, so most of the validator stays unreached.  These bits
    // let it steer into the deep, security-relevant paths while still leaving
    // the fully-random shape reachable.
    if (mode & 0x01) request.magic = SERVICE_PROTOCOL_MAGIC;
    if (mode & 0x02) request.version = SERVICE_PROTOCOL_VERSION;
    if (mode & 0x04) request.callerPid = 4242;
    if (mode & 0x08) {
        request.source[sizeof(request.source) - 1] = '\0';
        request.path[sizeof(request.path) - 1] = '\0';
        request.targetGpu.name[sizeof(request.targetGpu.name) - 1] = '\0';
    }

    const bool accepted = validate_service_request_for_ipc(&request);
    if (!accepted) return 0;

    // Accepted requests are handed to privileged code, so the guarantees the
    // boundary advertises must actually hold.
    GC_FUZZ_CHECK(request.magic == SERVICE_PROTOCOL_MAGIC,
                  "accepted a request with the wrong protocol magic");
    GC_FUZZ_CHECK(request.version == SERVICE_PROTOCOL_VERSION,
                  "accepted a request with the wrong protocol version");
    GC_FUZZ_CHECK(request.callerPid != 0,
                  "accepted a request with no caller pid");
    GC_FUZZ_CHECK(request.profileSlot <= 255u,
                  "accepted an out-of-range profile slot");
    GC_FUZZ_CHECK(service_wire_string_is_terminated(
                      request.source, (unsigned int)sizeof(request.source)),
                  "accepted an unterminated source string");
    GC_FUZZ_CHECK(service_wire_string_is_terminated(
                      request.path, (unsigned int)sizeof(request.path)),
                  "accepted an unterminated path string");
    GC_FUZZ_CHECK(service_wire_string_is_terminated(
                      request.targetGpu.name,
                      (unsigned int)sizeof(request.targetGpu.name)),
                  "accepted an unterminated GPU name string");

    // Reading them is what a real consumer does; ASan catches any overrun the
    // termination checks failed to prevent.
    volatile size_t sink = strlen(request.source) + strlen(request.path) +
                           strlen(request.targetGpu.name);
    (void)sink;

    check_desired_postconditions(request.desired);

    // Re-validating an accepted request must be a no-op: the boundary has to
    // be idempotent or a re-queued request could change meaning in flight.
    ServiceRequest again = request;
    const bool acceptedAgain = validate_service_request_for_ipc(&again);
    GC_FUZZ_CHECK(acceptedAgain, "validation is not idempotent (second pass rejected)");
    GC_FUZZ_CHECK(memcmp(&again, &request, sizeof(request)) == 0,
                  "validation is not idempotent (second pass mutated the request)");
    return 0;
}

#endif  // GC_FUZZ_SERVICE_REQUEST

// ---------------------------------------------------------------------------
// Target 2: Linux VF snapshot structural validation.
//
// The candidate curve comes from driver ioctls.  A malformed or hostile
// snapshot must be rejected rather than indexed.
// ---------------------------------------------------------------------------
#if GC_FUZZ_TARGET == GC_FUZZ_VF_SNAPSHOT

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    FuzzInput in(data, size);

    const uint8_t flags = in.byte();
    const bool infoFresh = (flags & 0x01) != 0;
    const bool statusFresh = (flags & 0x02) != 0;
    const bool controlFresh = (flags & 0x04) != 0;
    const bool nullMask = (flags & 0x08) != 0;
    const bool shortMask = (flags & 0x10) != 0;
    const bool nullCurve = (flags & 0x20) != 0;
    const bool nullOffsets = (flags & 0x40) != 0;

    unsigned char editableMask[(VF_NUM_POINTS + 7u) / 8u];
    VFCurvePoint curve[VF_NUM_POINTS];
    int offsets[VF_NUM_POINTS];

    in.bytes(editableMask, sizeof(editableMask));
    for (int i = 0; i < VF_NUM_POINTS; ++i) {
        curve[i].freq_kHz = in.u32();
        curve[i].volt_uV = in.u32();
        offsets[i] = in.i32();
    }

    const unsigned int numClocks = in.u32();
    const int reportedPopulated = in.i32();

    // A deliberately awkward buffer size: the callee must respect whySize.
    char why[96];
    memset(why, 0x7e, sizeof(why));
    const size_t whySize = (size_t)(in.byte() % (sizeof(why) + 1));

    const size_t maskSize = shortMask
        ? (size_t)((VF_NUM_POINTS + 7u) / 8u) - 1u
        : sizeof(editableMask);

    const bool valid = linux_vf_snapshot_structurally_valid(
        infoFresh, statusFresh, controlFresh,
        nullMask ? nullptr : editableMask, maskSize,
        numClocks, nullCurve ? nullptr : curve,
        nullOffsets ? nullptr : offsets, reportedPopulated,
        whySize ? why : nullptr, whySize);

    if (whySize) {
        // The reason buffer must always come back NUL-terminated inside the
        // size it was given, and must never be written past that size.
        bool terminated = false;
        for (size_t i = 0; i < whySize; ++i) {
            if (why[i] == '\0') { terminated = true; break; }
        }
        GC_FUZZ_CHECK(terminated, "reason buffer was not NUL-terminated within whySize");
        for (size_t i = whySize; i < sizeof(why); ++i)
            GC_FUZZ_CHECK((unsigned char)why[i] == 0x7eu,
                          "reason buffer was written past whySize");
    }

    if (valid) {
        GC_FUZZ_CHECK(infoFresh && statusFresh && controlFresh,
                      "accepted a snapshot with a stale domain");
        GC_FUZZ_CHECK(!nullMask && !nullCurve && !nullOffsets,
                      "accepted a snapshot with a null buffer");
        GC_FUZZ_CHECK(numClocks >= 1u && numClocks <= 64u,
                      "accepted an out-of-range clock count");
        GC_FUZZ_CHECK(reportedPopulated >= 8 && reportedPopulated <= VF_NUM_POINTS,
                      "accepted an out-of-range populated count");

        // Recompute independently: an accepted snapshot must really carry the
        // point count it reported, or downstream loops read uninitialized points.
        int actual = 0;
        for (int i = 0; i < VF_NUM_POINTS; ++i)
            if (curve[i].freq_kHz != 0) ++actual;
        GC_FUZZ_CHECK(actual == reportedPopulated,
                      "accepted a snapshot whose populated count does not match the curve");
    }
    return 0;
}

#endif  // GC_FUZZ_VF_SNAPSHOT

// ---------------------------------------------------------------------------
// Target 3: the startup-task XML classifier.
//
// A hand-rolled UTF-16 tag scanner over text that Task Scheduler (or a user
// with an editor) controls.  Exactly where an off-by-one read hides.
// ---------------------------------------------------------------------------
#if GC_FUZZ_TARGET == GC_FUZZ_TASK_XML

// The classifier lives in an amalgamated Windows shard.  Supply only the
// surrounding declarations that shard needs, mirroring tests/regression_main.cpp;
// this fixture calls the pure classifier and never touches Task Scheduler.
#include "app_shared.h"
#include "startup_task_definition_policy.h"

static char g_userDataDir[MAX_PATH] = {};
static WCHAR g_forcedStartupUserSam[512] = {};
bool utf8_to_wide(const char*, WCHAR*, int);
static bool get_current_user_sam_name(WCHAR*, DWORD) { return false; }
#include "main_startup_task_definition.cpp"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    // Interpret the input as UTF-16 code units; cap so the fuzzer explores
    // structure rather than length.
    enum { MAX_UNITS = 4096 };
    static WCHAR xml[MAX_UNITS + 1];
    size_t units = size / sizeof(WCHAR);
    if (units > MAX_UNITS) units = MAX_UNITS;
    memcpy(xml, data, units * sizeof(WCHAR));
    xml[units] = L'\0';

    // An embedded NUL just ends the string early, which is itself worth
    // exercising, so it is deliberately not filtered out.
    char detail[512];
    memset(detail, 0x5a, sizeof(detail));

    const StartupTaskDefinitionClass klass = startup_task_definition_classify_xml(
        xml, L"TESTDOMAIN\\tester", L"C:\\Program Files\\greencurve\\greencurve.exe",
        L"C:\\Users\\tester\\AppData\\Roaming\\greencurve\\greencurve.ini",
        L"C:\\Program Files\\greencurve", detail, sizeof(detail));

    GC_FUZZ_CHECK(klass == STARTUP_TASK_DEFINITION_BROKEN ||
                  klass == STARTUP_TASK_DEFINITION_COMPATIBLE_LEGACY ||
                  klass == STARTUP_TASK_DEFINITION_CANONICAL,
                  "classifier returned a value outside the enum");

    bool terminated = false;
    for (size_t i = 0; i < sizeof(detail); ++i)
        if (detail[i] == '\0') { terminated = true; break; }
    GC_FUZZ_CHECK(terminated, "detail buffer was not NUL-terminated");

    // Classification must be a pure function of its inputs.
    char detail2[512];
    memset(detail2, 0x5a, sizeof(detail2));
    const StartupTaskDefinitionClass again = startup_task_definition_classify_xml(
        xml, L"TESTDOMAIN\\tester", L"C:\\Program Files\\greencurve\\greencurve.exe",
        L"C:\\Users\\tester\\AppData\\Roaming\\greencurve\\greencurve.ini",
        L"C:\\Program Files\\greencurve", detail2, sizeof(detail2));
    GC_FUZZ_CHECK(again == klass, "classifier is not deterministic");
    return 0;
}

#endif  // GC_FUZZ_TASK_XML

// ---------------------------------------------------------------------------
// Target 4: config/INI scalar parsers.
//
// These read attacker-influenceable INI text and CLI arguments.
// ---------------------------------------------------------------------------
#if GC_FUZZ_TARGET == GC_FUZZ_CONFIG_STRINGS

#include "app_shared.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    FuzzInput in(data, size);

    // trim_ascii mutates in place; a canary tail catches an overrun.
    char line[256 + 16];
    memset(line, 0x33, sizeof(line));
    char text[256];
    memset(text, 0, sizeof(text));
    fuzz_cstring(in, text);
    memcpy(line, text, sizeof(text));
    const size_t beforeLen = strlen(line);

    trim_ascii(line);
    const size_t afterLen = strlen(line);
    GC_FUZZ_CHECK(afterLen <= beforeLen, "trim_ascii grew the string");
    for (size_t i = sizeof(text); i < sizeof(line); ++i)
        GC_FUZZ_CHECK((unsigned char)line[i] == 0x33u,
                      "trim_ascii wrote past the buffer");
    // Trimming is idempotent and leaves no leading/trailing blanks.
    if (afterLen) {
        GC_FUZZ_CHECK((unsigned char)line[0] > ' ',
                      "trim_ascii left a leading blank");
        GC_FUZZ_CHECK((unsigned char)line[afterLen - 1] > ' ',
                      "trim_ascii left a trailing blank");
    }
    char again[sizeof(line)];
    memcpy(again, line, sizeof(again));
    trim_ascii(again);
    GC_FUZZ_CHECK(strcmp(again, line) == 0, "trim_ascii is not idempotent");

    int parsed = 0;
    if (parse_int_strict(text, &parsed)) {
        // A strict parse must round-trip, or config values silently change.
        char rendered[32];
        snprintf(rendered, sizeof(rendered), "%d", parsed);
        int reparsed = 0;
        GC_FUZZ_CHECK(parse_int_strict(rendered, &reparsed),
                      "parse_int_strict rejected its own rendering");
        GC_FUZZ_CHECK(reparsed == parsed, "parse_int_strict does not round-trip");
    }

    bool isAuto = false;
    int pct = -12345;
    if (parse_fan_value(text, &isAuto, &pct)) {
        GC_FUZZ_CHECK(isAuto || (pct >= 0 && pct <= 100),
                      "parse_fan_value accepted an out-of-range percentage");
    }

    char section[64];
    memset(section, 0, sizeof(section));
    fuzz_cstring(in, section);
    (void)config_section_header_matches_ascii(text, section);
    (void)streqi_ascii(text, section);
    GC_FUZZ_CHECK(streqi_ascii(text, text), "streqi_ascii is not reflexive");

    // Wide CLI argument path.
    WCHAR warg[128];
    size_t wtake = (size_t)in.byte() % (sizeof(warg) / sizeof(warg[0]));
    for (size_t i = 0; i < wtake; ++i) warg[i] = (WCHAR)(in.byte() | (in.byte() << 8));
    warg[wtake] = L'\0';
    int pointIndex = -1;
    if (parse_cli_point_arg_w(warg, &pointIndex))
        GC_FUZZ_CHECK(pointIndex >= 0 && pointIndex < VF_NUM_POINTS,
                      "parse_cli_point_arg_w accepted an out-of-range point index");
    return 0;
}

#endif  // GC_FUZZ_CONFIG_STRINGS

// ---------------------------------------------------------------------------
// Target 5: daemon transport prefix/errno classification and the permission
// diagnostic formatter (which builds a message out of untrusted path and
// group strings).
// ---------------------------------------------------------------------------
#if GC_FUZZ_TARGET == GC_FUZZ_WIRE_PREFIX

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    FuzzInput in(data, size);

    ServiceWirePrefix prefix;
    prefix.magic = in.u32();
    prefix.version = in.u32();
    const uint8_t steer = in.byte();
    if (steer & 0x01) prefix.magic = SERVICE_PROTOCOL_MAGIC;
    if (steer & 0x02) prefix.version = SERVICE_PROTOCOL_VERSION;

    const ServiceWirePrefixDisposition disposition =
        service_wire_prefix_disposition(&prefix);
    GC_FUZZ_CHECK(disposition == SERVICE_WIRE_PREFIX_CURRENT ||
                  disposition == SERVICE_WIRE_PREFIX_BAD_MAGIC ||
                  disposition == SERVICE_WIRE_PREFIX_VERSION_MISMATCH,
                  "wire prefix disposition is outside the enum");
    if (disposition == SERVICE_WIRE_PREFIX_CURRENT) {
        GC_FUZZ_CHECK(prefix.magic == SERVICE_PROTOCOL_MAGIC &&
                      prefix.version == SERVICE_PROTOCOL_VERSION,
                      "accepted a mismatched wire prefix as current");
    }
    GC_FUZZ_CHECK(service_wire_prefix_disposition(nullptr) ==
                  SERVICE_WIRE_PREFIX_BAD_MAGIC,
                  "null wire prefix was not rejected");

    const int errorNumber = in.i32();
    const DaemonAcceptDisposition accept = daemon_accept_disposition(errorNumber);
    GC_FUZZ_CHECK(accept == DAEMON_ACCEPT_RETRY ||
                  accept == DAEMON_ACCEPT_RECLAIM_FD ||
                  accept == DAEMON_ACCEPT_FATAL,
                  "accept disposition is outside the enum");
    GC_FUZZ_CHECK(daemon_accept_error_is_fatal(errorNumber) ==
                  (accept == DAEMON_ACCEPT_FATAL),
                  "fatal helper disagrees with the disposition it wraps");

    const DaemonIoFailure failure = (DaemonIoFailure)(in.byte() % 4u);
    const char* classification =
        daemon_io_failure_classification(failure, (size_t)in.u32());
    GC_FUZZ_CHECK(classification != nullptr && classification[0] != '\0',
                  "I/O failure classification returned an empty string");

    // The formatter concatenates untrusted path/group strings into a bounded
    // buffer; a canary tail catches an overrun and a short size catches a
    // missing truncation.
    char pathBuf[128];
    char connectBuf[64];
    char metaBuf[64];
    char groupBuf[64];
    memset(pathBuf, 0, sizeof(pathBuf));
    memset(connectBuf, 0, sizeof(connectBuf));
    memset(metaBuf, 0, sizeof(metaBuf));
    memset(groupBuf, 0, sizeof(groupBuf));
    fuzz_cstring(in, pathBuf);
    fuzz_cstring(in, connectBuf);
    fuzz_cstring(in, metaBuf);
    fuzz_cstring(in, groupBuf);

    const uint8_t nulls = in.byte();
    LinuxDaemonPermissionFacts facts;
    facts.socketMetadataAvailable = (nulls & 0x01) != 0;
    facts.socketPath = (nulls & 0x02) ? nullptr : pathBuf;
    facts.connectError = (nulls & 0x04) ? nullptr : connectBuf;
    facts.metadataError = (nulls & 0x08) ? nullptr : metaBuf;
    facts.socketGroupName = (nulls & 0x10) ? nullptr : groupBuf;
    facts.socketOwnerUid = in.u32();
    facts.socketGroupId = in.u32();
    facts.socketMode = in.u32();
    facts.processEuid = in.u32();
    facts.processPrimaryGid = in.u32();
    facts.supplementaryGreencurve = (int)(in.byte() % 3u) - 1;

    char out[192];
    memset(out, 0x11, sizeof(out));
    const size_t outSize = (size_t)(in.byte() % (sizeof(out) + 1));
    linux_daemon_format_permission_facts(
        (nulls & 0x20) ? nullptr : &facts, outSize ? out : nullptr, outSize);
    if (outSize) {
        bool terminated = false;
        for (size_t i = 0; i < outSize; ++i)
            if (out[i] == '\0') { terminated = true; break; }
        GC_FUZZ_CHECK(terminated,
                      "permission diagnostic was not NUL-terminated within its size");
        for (size_t i = outSize; i < sizeof(out); ++i)
            GC_FUZZ_CHECK((unsigned char)out[i] == 0x11u,
                          "permission diagnostic was written past its size");
    }
    return 0;
}

#endif  // GC_FUZZ_WIRE_PREFIX
