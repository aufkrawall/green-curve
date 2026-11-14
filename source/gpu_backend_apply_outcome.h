// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#pragma once
// Keep user-visible outcome reporting separate from clock submission ordering.
static bool apply_report_outcome(LockMode lockMode, int failCount, int successCount,
    int userBoostFailed, int flattenFailed, ULONGLONG applyStartTickMs, bool partialApplyRisk,
    const char* failureDetails, const char* curveVerifySummary, char* result, size_t resultSize,
    gc_u32* outcomeSeverityOut) {
    // The verify counters are the only place the difference between "did what
    // was asked" and "committed something close to it" exists; the summary text
    // below already reports it to a human, and this reports it to the client.
    gc_u32 outcomeSeverity = service_apply_outcome_severity_for_lock_mode(
        lockMode == LOCK_MODE_HARD, failCount, userBoostFailed, flattenFailed);
    if (outcomeSeverityOut) *outcomeSeverityOut = outcomeSeverity;
    const ULONGLONG applyElapsedMs = GetTickCount64() - applyStartTickMs;
    debug_log("apply outcome: severity=%s failCount=%d successCount=%d boostUnmatched=%d flattenUnmatched=%d elapsedMs=%llu budgetMs=%lu%s\n",
        service_outcome_severity_name(outcomeSeverity), failCount, successCount,
        userBoostFailed, flattenFailed, (unsigned long long)applyElapsedMs,
        (unsigned long)SERVICE_APPLY_HANDLER_BUDGET_MS,
        applyElapsedMs > SERVICE_APPLY_HANDLER_BUDGET_MS
            ? " OVER-BUDGET (the client has already timed out)" : "");
    if (successCount == 0 && failCount == 0) {
        set_message(result, resultSize, "No setting changes needed.");
    } else if (failCount == 0) {
        set_message(result, resultSize, "Applied %d setting changes successfully.%s", successCount, curveVerifySummary);
    } else {
        char logErr[256] = {};
        bool logWritten = write_error_report_log("Setting apply reported one or more failures", failureDetails, logErr, sizeof(logErr));
        if (failureDetails[0]) {
            set_message(result, resultSize, "%sApplied %d OK, %d failed: %s%s%s",
                partialApplyRisk ? "Live state may now be a mixed partial apply. " : "",
                successCount,
                failCount,
                failureDetails,
                logWritten ? " See " : "",
                logWritten ? error_log_path() : "");
        } else {
            set_message(result, resultSize, "%sApplied %d OK, %d failed.%s%s",
                partialApplyRisk ? "Live state may now be a mixed partial apply. " : "",
                successCount,
                failCount,
                logWritten ? " See " : "",
                logWritten ? error_log_path() : "");
        }
        if (!logWritten && logErr[0]) {
            debug_log("failed to write error report: %s\n", logErr);
        }
    }
    return failCount == 0;
}
