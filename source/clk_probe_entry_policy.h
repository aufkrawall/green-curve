// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#pragma once

namespace gc_clk_probe_entry {

inline bool is_ascii_space(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

// Parse the deliberately opt-in clock-domain selector. Invalid input must not
// become entry zero: the probe temporarily writes to the selected domain.
inline bool parse(const char* text, unsigned int domainCount,
                  unsigned int* entryOut) {
    if (!text || !entryOut || domainCount == 0) return false;
    while (is_ascii_space(*text)) ++text;
    if (*text < '0' || *text > '9') return false;

    unsigned int value = 0;
    const unsigned int maximum = domainCount - 1;
    do {
        unsigned int digit = (unsigned int)(*text - '0');
        if (digit > maximum || value > (maximum - digit) / 10) return false;
        value = value * 10 + digit;
        ++text;
    } while (*text >= '0' && *text <= '9');

    while (is_ascii_space(*text)) ++text;
    if (*text != '\0') return false;
    *entryOut = value;
    return true;
}

}  // namespace gc_clk_probe_entry
