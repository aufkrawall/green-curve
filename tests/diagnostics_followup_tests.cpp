// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// 0.28 field-log diagnostics follow-ups (assertion codes 6700-6799): the
// steady-state summarizer behind the per-tick log lines
// (debug_log_steady_state_policy.h) and the bounded CLI-log append the silent
// logon paths now use (cli_log_append_policy.h), and the GUI notice for a
// failed automatic apply plus its protocol-v29 wire record
// (auto_apply_failure_notice_policy.h).  Pure headers plus one temp file; no
// GPU, no service.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
#else
#include <unistd.h>
#endif

#include <stddef.h>

#include "gpu_core.h"
#include "service_protocol.h"
#include "auto_apply_failure_notice_policy.h"
#include "cli_log_append_policy.h"
#include "debug_log_steady_state_policy.h"

namespace {

using namespace gc_log_steady;

int run_steady_state_tests() {
    Tracker t = {};
    // First sight of a key is a change, with nothing pending.
    Decision d = observe(&t, "phase=ready", 1000, 5, false);
    if (d.action != LOG_CHANGE || d.pending.count != 0) return 6700;
    if (!wants_full_line(d) || wants_summary(d)) return 6701;
    // Repeats inside the window are counted, not logged.
    d = observe(&t, "phase=ready", 3000, 9, false);
    if (d.action != SUPPRESS || wants_full_line(d) || wants_summary(d)) return 6702;
    d = observe(&t, "phase=ready", 5000, 2, false);
    if (d.action != SUPPRESS) return 6703;
    // The window closes after the interval and reports count/min/max/last.
    d = observe(&t, "phase=ready", 1000 + kSummaryIntervalMs, 7, false);
    if (d.action != LOG_SUMMARY || !wants_summary(d) || wants_full_line(d)) return 6704;
    if (d.pending.count != 3 || d.pending.minValue != 2 || d.pending.maxValue != 9 ||
        d.pending.lastValue != 7 || d.pending.elapsedMs != kSummaryIntervalMs) return 6705;
    // ...and a new window starts empty.
    d = observe(&t, "phase=ready", 2000 + kSummaryIntervalMs, 1, false);
    if (d.action != SUPPRESS || t.window.count != 1 || t.window.minValue != 1) return 6706;
    // A key change logs at once and hands back the repeats of the old key.
    d = observe(&t, "phase=degraded", 3000 + kSummaryIntervalMs, 1, false);
    if (d.action != LOG_CHANGE || d.pending.count != 1 || !wants_summary(d) ||
        !wants_full_line(d)) return 6707;
    // Verbose logs every repeat in full; the first sight is still a change.
    Tracker v = {};
    if (observe(&v, "k", 0, 0, true).action != LOG_CHANGE) return 6708;
    d = observe(&v, "k", 1, 0, true);
    if (d.action != LOG_VERBOSE || !wants_full_line(d) || wants_summary(d)) return 6709;
    // invalidate(): the next healthy line is a change, so a recovery shows.
    Tracker a = {};
    observe(&a, "k", 0, 0, false);
    observe(&a, "k", 10, 0, false);
    invalidate(&a);
    d = observe(&a, "k", 20, 0, false);
    if (d.action != LOG_CHANGE) return 6710;
    // A null tracker degrades to logging, never to silence.
    if (observe(nullptr, "k", 0, 0, false).action != LOG_CHANGE) return 6711;

    // THE 0.28 FIELD-LOG BUG: a key that contains a per-read revision never
    // repeats, so on-change suppression suppressed nothing.  With the revision
    // as the tracked value instead, an hour of 2 s ticks costs one change line
    // plus one summary per 10 minutes.
    Tracker hour = {};
    int lines = 0;
    for (unsigned long long tick = 0; tick < 1800; ++tick) {
        Decision h = observe(&hour, "instance=1 phase=4 valid=0x1F", tick * 2000ull,
                             5000 + tick, false);
        if (wants_summary(h)) lines++;
        if (wants_full_line(h)) lines++;
    }
    if (lines > 7) return 6712;
    Tracker revisionKey = {};
    int naive = 0;
    for (unsigned long long tick = 0; tick < 1800; ++tick) {
        char key[64] = {};
        snprintf(key, sizeof(key), "revision=%llu phase=4", tick);
        if (wants_full_line(observe(&revisionKey, key, tick * 2000ull, 0, false))) naive++;
    }
    if (naive != 1800) return 6713;  // the failure mode the call sites must avoid

    // The shared summary tail.
    Window w = {};
    w.count = 300; w.minValue = 0; w.maxValue = 3; w.lastValue = 1; w.elapsedMs = 600999;
    char buf[128] = {};
    format_window(buf, sizeof(buf), w, "durationMs");
    if (strcmp(buf, "n=300 windowS=600 durationMs=0..3 last=1") != 0) return 6714;
    format_window(buf, sizeof(buf), w, nullptr);
    if (strncmp(buf, "n=300 windowS=600 value=0..3", 28) != 0) return 6715;
    char tiny[8] = {};
    format_window(tiny, sizeof(tiny), w, "x");
    if (strlen(tiny) != 7) return 6716;

    // Keys longer than the stored key compare on their stored prefix only.
    char longA[300], longB[300];
    memset(longA, 'a', sizeof(longA)); longA[299] = '\0';
    memcpy(longB, longA, sizeof(longB)); longB[250] = 'b';
    Tracker l = {};
    observe(&l, longA, 0, 0, false);
    if (observe(&l, longB, 1, 0, false).action != SUPPRESS) return 6717;

    // GREEN_CURVE_DEBUG values that request per-tick detail.
    if (!env_value_requests_verbose("verbose") ||
        !env_value_requests_verbose("VERBOSE") ||
        !env_value_requests_verbose("  Verbose") ||
        !env_value_requests_verbose("2")) return 6718;
    if (env_value_requests_verbose("1") || env_value_requests_verbose("0") ||
        env_value_requests_verbose("") || env_value_requests_verbose(nullptr) ||
        env_value_requests_verbose("verbosely") ||
        env_value_requests_verbose("22") ||
        env_value_requests_verbose("verb")) return 6719;
    return 0;
}

bool temp_log_path(char* out, size_t outSize, const char* tag) {
#if defined(_WIN32)
    char dir[MAX_PATH] = {};
    if (GetTempPathA(MAX_PATH, dir) == 0) return false;
    int n = snprintf(out, outSize, "%sgc_cli_log_%s_%lu.txt", dir, tag,
                     (unsigned long)GetCurrentProcessId());
#else
    int n = snprintf(out, outSize, "/tmp/gc_cli_log_%s_%ld.txt", tag, (long)getpid());
#endif
    return n > 0 && (size_t)n < outSize;
}

bool read_all(const char* path, char* out, size_t outSize) {
    FILE* f = fopen(path, "r");
    if (!f) return false;
    size_t n = fread(out, 1, outSize - 1, f);
    out[n] = '\0';
    fclose(f);
    return true;
}

FILE* refuse_open(const char*, const char*) { return nullptr; }

int run_cli_log_append_tests() {
    char path[512] = {};
    if (!temp_log_path(path, sizeof(path), "append")) return 6730;
    remove(path);
    // THE BUG: a logon record replaced the file.  Two appends keep both.
    if (gc_cli_log_append_record(path, "first FAILED\n", 1 << 20, "ts ", fopen) !=
        GC_CLI_LOG_APPENDED) return 6731;
    if (gc_cli_log_append_record(path, "second accepted\n", 1 << 20, "ts ", fopen) !=
        GC_CLI_LOG_APPENDED) return 6732;
    char content[512] = {};
    if (!read_all(path, content, sizeof(content))) return 6733;
    if (strcmp(content, "first FAILED\nsecond accepted\n") != 0) return 6734;
    // At the cap the file is truncated in place, marker first, then the record.
    if (gc_cli_log_append_record(path, "third\n", 4, "ts ", fopen) !=
        GC_CLI_LOG_APPENDED_AFTER_ROTATION) return 6735;
    if (!read_all(path, content, sizeof(content))) return 6736;
    char expected[256] = {};
    snprintf(expected, sizeof(expected), "ts %sthird\n",
             gc_debug_log_rotation::cli_marker_line());
    if (strcmp(content, expected) != 0) return 6737;
    // Under the cap, an existing file is appended to, never rotated.
    if (gc_cli_log_append_record(path, "fourth\n", 1 << 20, nullptr, fopen) !=
        GC_CLI_LOG_APPENDED) return 6738;
    if (!read_all(path, content, sizeof(content))) return 6739;
    if (strstr(content, "third\nfourth\n") == nullptr) return 6740;
    // A non-positive cap never rotates (rotation policy contract).
    if (gc_cli_log_append_record(path, "fifth\n", 0, nullptr, fopen) !=
        GC_CLI_LOG_APPENDED) return 6741;
    remove(path);
    // Refusals.
    if (gc_cli_log_append_record(nullptr, "x\n", 10, nullptr, fopen) !=
        GC_CLI_LOG_APPEND_FAILED) return 6742;
    if (gc_cli_log_append_record("", "x\n", 10, nullptr, fopen) !=
        GC_CLI_LOG_APPEND_FAILED) return 6743;
    if (gc_cli_log_append_record(path, nullptr, 10, nullptr, fopen) !=
        GC_CLI_LOG_APPEND_FAILED) return 6744;
    if (gc_cli_log_append_record(path, "x\n", 10, nullptr, nullptr) !=
        GC_CLI_LOG_APPEND_FAILED) return 6745;
    if (gc_cli_log_append_record(path, "x\n", 10, nullptr, refuse_open) !=
        GC_CLI_LOG_APPEND_FAILED) return 6746;
    return 0;
}

ServiceAutoApplyFailure failure_record(gc_u32 trigger, bool recovered, const char* detail) {
    ServiceAutoApplyFailure f = {};
    f.trigger = trigger;
    f.coreRecovered = recovered ? 1 : 0;
    snprintf(f.detail, sizeof(f.detail), "%s", detail);
    return f;
}

int run_auto_apply_notice_tests() {
    // Which lockout transitions are announced.
    const gc_u32 NONE = SERVICE_AUTO_RESTORE_LOCKOUT_NONE;
    const gc_u32 FAILED = SERVICE_AUTO_RESTORE_LOCKOUT_AUTOMATIC_APPLY_FAILED;
    const gc_u32 HANDBACK = SERVICE_AUTO_RESTORE_LOCKOUT_HANDBACK_INCOMPLETE;
    // THE 0.28 GAP: a failed automatic apply latched FAILED and nothing was said.
    if (gc_auto_restore_notice_for_transition(NONE, FAILED) !=
        GC_AUTO_RESTORE_NOTICE_AUTO_APPLY_FAILED) return 6750;
    if (gc_auto_restore_notice_for_transition(NONE, HANDBACK) !=
        GC_AUTO_RESTORE_NOTICE_HANDBACK_INCOMPLETE ||
        gc_auto_restore_notice_for_transition(FAILED, HANDBACK) !=
        GC_AUTO_RESTORE_NOTICE_HANDBACK_INCOMPLETE) return 6751;
    // The user's Reset downgrading HANDBACK to FAILED is not news; nor is a
    // lockout that stays, clears, or has a reason without a notice.
    if (gc_auto_restore_notice_for_transition(HANDBACK, FAILED) != GC_AUTO_RESTORE_NOTICE_NONE ||
        gc_auto_restore_notice_for_transition(FAILED, FAILED) != GC_AUTO_RESTORE_NOTICE_NONE ||
        gc_auto_restore_notice_for_transition(FAILED, NONE) != GC_AUTO_RESTORE_NOTICE_NONE ||
        gc_auto_restore_notice_for_transition(NONE, SERVICE_AUTO_RESTORE_LOCKOUT_TDR_SPAM) !=
            GC_AUTO_RESTORE_NOTICE_NONE) return 6752;

    // The verified-recovery sentence is what licenses "returned to stock".
    if (!gc_apply_result_reports_core_recovery(
            "VF point 45 offset verified at 22000 kHz above requested 0 kHz. "
            GC_APPLY_RECOVERY_VERIFIED_TEXT) ||
        gc_apply_result_reports_core_recovery(
            "x. Recovery could not verify core defaults; clock protection retained") ||
        gc_apply_result_reports_core_recovery(nullptr)) return 6753;

    char status[256], title[64], body[256];
    ServiceAutoApplyFailure f = failure_record(SERVICE_LIFECYCLE_TRIGGER_TASK_HANDOFF,
                                               true, "VF point 45 offset mismatch");
    if (!gc_auto_apply_failure_compose(&f, status, sizeof(status), title, sizeof(title),
                                       body, sizeof(body))) return 6754;
    if (strcmp(status, "Automatic profile apply at logon failed; GPU clocks were "
                       "returned to stock. Press Apply to retry. Cause: VF point 45 "
                       "offset mismatch") != 0) return 6755;
    if (strcmp(title, "Green Curve: automatic apply failed") != 0 ||
        strcmp(body, status) != 0) return 6756;
    // Unverified recovery never claims stock and offers Reset.
    f = failure_record(SERVICE_LIFECYCLE_TRIGGER_STANDBY_RESUME, false, "fan write failed");
    gc_auto_apply_failure_compose(&f, status, sizeof(status), title, sizeof(title),
                                  body, sizeof(body));
    if (strstr(status, "after standby failed. Press Apply to retry, or Reset") != status + 24 ||
        strstr(status, "returned to stock") != nullptr) return 6757;
    f = failure_record(SERVICE_LIFECYCLE_TRIGGER_DRIVER_RECOVERY, true, "x");
    gc_auto_apply_failure_compose(&f, status, sizeof(status), title, sizeof(title),
                                  body, sizeof(body));
    if (strstr(status, "after a driver restart failed") == nullptr) return 6758;
    f = failure_record(SERVICE_LIFECYCLE_TRIGGER_DAEMON_START, false, "rollback ok");
    gc_auto_apply_failure_compose(&f, status, sizeof(status), title, sizeof(title),
                                  body, sizeof(body));
    if (strstr(status, "at daemon start failed. Press Apply to retry, or Reset") == nullptr)
        return 6765;
    // No record (lockout from an earlier service instance): no invented cause.
    ServiceAutoApplyFailure none = {};
    snprintf(none.detail, sizeof(none.detail), "stale text must not show");
    gc_auto_apply_failure_compose(&none, status, sizeof(status), title, sizeof(title),
                                  body, sizeof(body));
    if (strstr(status, "paused") == nullptr || strstr(status, "Press Apply") == nullptr ||
        strstr(status, "stale") != nullptr || strstr(body, "Cause") != nullptr) return 6759;
    if (!gc_auto_apply_failure_compose(nullptr, status, sizeof(status), title,
                                       sizeof(title), body, sizeof(body)) ||
        strstr(status, "paused") == nullptr) return 6760;
    // A full-length cause is cut with "..." in the 256-char balloon, never overflowed.
    char longDetail[SERVICE_AUTO_APPLY_FAILURE_DETAIL_CHARS];
    memset(longDetail, 'd', sizeof(longDetail) - 1);
    longDetail[sizeof(longDetail) - 1] = '\0';
    f = failure_record(SERVICE_LIFECYCLE_TRIGGER_WTS_LOGON, false, longDetail);
    gc_auto_apply_failure_compose(&f, status, sizeof(status), title, sizeof(title),
                                  body, sizeof(body));
    size_t bodyLen = strlen(body);
    if (bodyLen != sizeof(body) - 1 || strcmp(body + bodyLen - 3, "...") != 0) return 6761;
    if (strlen(status) != sizeof(status) - 1) return 6762;
    // An unterminated wire detail is never read past its field.
    memset(f.detail, 'z', sizeof(f.detail));
    gc_auto_apply_failure_compose(&f, status, sizeof(status), title, sizeof(title),
                                  body, sizeof(body));
    if (strlen(status) >= sizeof(status)) return 6763;
    if (gc_auto_apply_failure_compose(&f, nullptr, 1, title, sizeof(title), body,
                                      sizeof(body))) return 6764;
    return 0;
}

int run_auto_apply_wire_tests() {
    // v29 carries the record; every fixed-size peer must refuse a v28 header.
    if (SERVICE_PROTOCOL_VERSION != 29) return 6770;
    if (sizeof(ServiceAutoApplyFailure) != 192 ||
        offsetof(ServiceSnapshot, autoApplyFailure) + sizeof(ServiceAutoApplyFailure) >
            sizeof(ServiceSnapshot)) return 6771;
    ServiceStateEnvelope envelope = {};
    ServiceSnapshot snapshot = {};
    DesiredSettings desired = {};
    ControlState controls = {};
    envelope.serviceInstanceId = 11;
    envelope.stateRevision = 4;
    envelope.gpuGeneration = 1;
    if (!validate_service_state_envelope_for_ipc(&envelope, &snapshot, &desired,
                                                 &controls)) return 6772;
    snapshot.autoApplyFailure = failure_record(SERVICE_LIFECYCLE_TRIGGER_WTS_LOGON,
                                               true, "cause");
    if (!validate_service_state_envelope_for_ipc(&envelope, &snapshot, &desired,
                                                 &controls)) return 6773;
    // The Linux start-time trigger is valid in the record, and only there.
    ServiceSnapshot linuxStart = snapshot;
    linuxStart.autoApplyFailure.trigger = SERVICE_LIFECYCLE_TRIGGER_DAEMON_START;
    if (!validate_service_state_envelope_for_ipc(&envelope, &linuxStart, &desired,
                                                 &controls)) return 6782;
    linuxStart.lastLifecycleTrigger = SERVICE_LIFECYCLE_TRIGGER_DAEMON_START;
    if (validate_service_state_envelope_for_ipc(&envelope, &linuxStart, &desired,
                                                &controls)) return 6783;
    ServiceSnapshot bad = snapshot;
    bad.autoApplyFailure.trigger = SERVICE_LIFECYCLE_TRIGGER_DAEMON_START + 1;
    if (validate_service_state_envelope_for_ipc(&envelope, &bad, &desired, &controls))
        return 6774;
    bad = snapshot;
    bad.autoApplyFailure.coreRecovered = 2;
    if (validate_service_state_envelope_for_ipc(&envelope, &bad, &desired, &controls))
        return 6775;
    bad = snapshot;
    bad.autoApplyFailure.reserved[1] = 1;
    if (validate_service_state_envelope_for_ipc(&envelope, &bad, &desired, &controls))
        return 6776;
    bad = snapshot;
    memset(bad.autoApplyFailure.detail, 'x', sizeof(bad.autoApplyFailure.detail));
    if (validate_service_state_envelope_for_ipc(&envelope, &bad, &desired, &controls))
        return 6777;
    // The canonicalizer makes the same record safe rather than trusting it.
    validate_service_snapshot_for_ipc(&bad);
    if (bad.autoApplyFailure.detail[sizeof(bad.autoApplyFailure.detail) - 1] != '\0')
        return 6778;
    bad.autoApplyFailure.trigger = 99;
    validate_service_snapshot_for_ipc(&bad);
    if (bad.autoApplyFailure.trigger != 0 || bad.autoApplyFailure.detail[0] != '\0')
        return 6779;
    // A snapshot that must be empty may not smuggle a cause.
    ServiceSnapshot empty = {};
    if (!service_snapshot_is_zero(empty)) return 6780;
    empty.autoApplyFailure.detail[0] = 'x';
    if (service_snapshot_is_zero(empty)) return 6781;
    return 0;
}

}  // namespace

int run_diagnostics_followup_tests() {
    if (int r = run_steady_state_tests()) return r;
    if (int r = run_cli_log_append_tests()) return r;
    if (int r = run_auto_apply_notice_tests()) return r;
    if (int r = run_auto_apply_wire_tests()) return r;
    return 0;
}
