// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#pragma once
#include <string>
#include <vector>

static inline void profile_editor_report_append(std::vector<std::string>& lines, bool& truncated,
    const std::string& text, const char* omission, bool draftHeading = false) {
    size_t at = 0;
    while (at < text.size()) {
        if (lines.size() >= 192) {
            if (!truncated) { lines.emplace_back(omission); truncated = true; }
            return;
        }
        const size_t end = text.find('\n', at), length = (end == std::string::npos ? text.size() : end) - at;
        std::string line = text.substr(at, length < 512 ? length : 512);
        if (draftHeading && lines.empty() && line.compare(0, 11, "Profile 1: ") == 0) line = "Draft: " + line.substr(11);
        lines.emplace_back(line);
        if (end == std::string::npos) break;
        at = end + 1;
    }
}
