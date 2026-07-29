// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
// Pure status formatting: never replace an actionable first transport error.

#ifndef GREEN_CURVE_LINUX_TUI_DIAGNOSTIC_POLICY_H
#define GREEN_CURVE_LINUX_TUI_DIAGNOSTIC_POLICY_H

#include "platform.h"

static inline void linux_tui_format_failure(
    char* output, size_t outputSize,
    const char* fallback, const char* detail) {
    if (!output || outputSize == 0) return;
    const char* safeFallback = fallback && fallback[0]
        ? fallback : "Operation failed";
    gc_snprintf(output, outputSize, "%s%s%s",
        safeFallback, detail && detail[0] ? ": " : "",
        detail && detail[0] ? detail : "");
}

#endif // GREEN_CURVE_LINUX_TUI_DIAGNOSTIC_POLICY_H
