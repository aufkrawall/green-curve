// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#ifndef GREEN_CURVE_CLK_DOMAIN_DUMP_CLI_POLICY_H
#define GREEN_CURVE_CLK_DOMAIN_DUMP_CLI_POLICY_H

namespace gc_clk_dump {
template<typename Char>
static inline bool arg_equals(const Char* value, const char* flag) {
    if (!value) return false;
    while (*flag && *value == (Char)*flag) { ++flag; ++value; }
    return !*flag && !*value;
}

// A diagnostic verb cannot share a command with an operation that an earlier
// dispatch branch could execute. Config and explicit Linux GPU selection are
// read inputs here; neither may persist settings or change the selected GPU.
template<typename Char>
static inline bool args_read_only(int argc, Char* const* argv, bool allowGpu) {
    if (!argv || argc < 2) return false;
    bool found = false;
    for (int i = 1; i < argc; ++i) {
        if (arg_equals(argv[i], "--clk-domain-dump")) found = true;
        else if (arg_equals(argv[i], "--config") ||
                 (allowGpu && arg_equals(argv[i], "--gpu"))) {
            if (++i >= argc || !argv[i] || !argv[i][0]) return false;
        } else if (!arg_equals(argv[i], "--help") && !arg_equals(argv[i], "-h")) return false;
    }
    return found;
}
}
#endif
