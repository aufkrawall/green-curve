// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#ifndef GREEN_CURVE_LOG_PATH_REDACTION_H
#define GREEN_CURVE_LOG_PATH_REDACTION_H
#include <stddef.h>
#include <string.h>

static inline bool gc_log_path_boundary(char value) {
    return value == 0 || value == ' ' || value == '\t' || value == '=' ||
           value == '\'' || value == '"' || value == '(' || value == ':';
}

static inline bool gc_log_path_start(const char* text, size_t at) {
    if (at && !gc_log_path_boundary(text[at - 1])) return false;
    const char* p = text + at;
    const bool letter = (p[0] >= 'a' && p[0] <= 'z') || (p[0] >= 'A' && p[0] <= 'Z');
    if (letter && p[1] == ':' && (p[2] == '\\' || p[2] == '/')) return true;
    if (p[0] == '\\' && p[1] == '\\') return true;
    if (p[0] == '.' && (p[1] == '\\' || p[1] == '/')) return true;
    if (p[0] != '/' || !p[1] || p[1] == '/' || p[1] == ' ' || p[1] == '\n') return false;
    // A Unix path names a folder AND an entry in it ("/home/<name>", "/tmp/x"):
    // its first component is followed by another '/'.  A lone "/word" is a
    // command switch ("schtasks /delete", "/tn", "/S"); taking it for a path
    // tokenized the rest of the line, exit codes included.
    for (size_t i = 1; p[i] && !gc_log_path_boundary(p[i]) && p[i] != '\r' && p[i] != '\n'; ++i) {
        if (p[i] == '/') return true;
    }
    return false;
}

// Paths in formatted error messages are not always quoted. Conservatively
// tokenize the remainder of that line, retaining a conventional error suffix.
// Structured call sites may still use individual path tokens for correlation.
static inline size_t gc_log_redact_paths(const char* input, char* output, size_t capacity) {
    if (!output || !capacity) return 0;
    output[0] = 0;
    if (!input) return 0;
    size_t written = 0;
    for (size_t at = 0; input[at] && written + 1 < capacity;) {
        if (!gc_log_path_start(input, at)) {
            output[written++] = input[at++];
            continue;
        }
        const char quote = at && (input[at - 1] == '\'' || input[at - 1] == '"')
            ? input[at - 1] : 0;
        size_t end = at;
        unsigned long long hash = 1469598103934665603ULL;
        while (input[end] && input[end] != '\r' && input[end] != '\n') {
            if (quote && input[end] == quote) break;
            if (!quote && (!strncmp(input + end, " (error ", 8) ||
                           !strncmp(input + end, " (hr ", 5))) break;
            hash ^= (unsigned char)input[end++];
            hash *= 1099511628211ULL;
        }
        char token[25] = "[path #0000000000000000]";
        for (int shift = 60, i = 7; shift >= 0; shift -= 4, ++i)
            token[i] = "0123456789abcdef"[(hash >> shift) & 15U];
        for (size_t i = 0; token[i] && written + 1 < capacity; ++i)
            output[written++] = token[i];
        at = end;
    }
    output[written] = 0;
    return written;
}

#endif
