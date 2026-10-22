// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#ifndef GREEN_CURVE_LOG_PATH_REDACTION_H
#define GREEN_CURVE_LOG_PATH_REDACTION_H
#include <stddef.h>
#include <string.h>

static inline bool gc_log_path_boundary(char value) {
    return value == 0 || value == ' ' || value == '\t' || value == '\r' || value == '\n' || value == '=' ||
           value == '\'' || value == '"' || value == '(' || value == ':';
}

static inline bool gc_log_word_equal(const char* word, size_t length, const char* expected) {
    if (length != strlen(expected)) return false;
    for (size_t i = 0; i < length; ++i) {
        const char lower = word[i] >= 'A' && word[i] <= 'Z' ? (char)(word[i] + ('a' - 'A')) : word[i];
        if (lower != expected[i]) return false;
    }
    return true;
}

// Only switches in a recognized command fragment are exempt from path
// redaction. A bare /delete, /S, or /file.ini can also be a Unix root entry.
static inline int gc_log_switch_group(const char* word, size_t length) {
    if (!length || word[0] != '/') return 0;
    ++word;
    --length;
    static const char* const kTaskSwitches[] = { "delete", "query", "create", "tn", "xml", "f" };
    for (const char* taskSwitch : kTaskSwitches) {
        if (gc_log_word_equal(word, length, taskSwitch)) return 1;
    }
    if (gc_log_word_equal(word, length, "s") || gc_log_word_equal(word, length, "d")) return 2;
    return 0;
}

static inline bool gc_log_windows_switch(const char* text, size_t at) {
    if (at && (text[at - 1] == '\'' || text[at - 1] == '"')) return false;
    size_t end = at;
    while (text[end] && !gc_log_path_boundary(text[end]) && text[end] != '\r' &&
           text[end] != '\n' && text[end] != ';') ++end;
    const int group = gc_log_switch_group(text + at, end - at);
    if (!group) return false;
    // Walk only preceding switches and whitespace, stopping at any argument,
    // prose, quote or line boundary. This keeps a later path in the same
    // diagnostic from inheriting an earlier command's switch exemption.
    end = at;
    while (end) {
        while (end && (text[end - 1] == ' ' || text[end - 1] == '\t')) --end;
        size_t begin = end;
        while (begin && !gc_log_path_boundary(text[begin - 1]) && text[begin - 1] != '\r' &&
               text[begin - 1] != '\n' && text[begin - 1] != ';') --begin;
        if (begin == end) return false;
        const char* word = text + begin;
        const size_t length = end - begin;
        if (group == 1 && (gc_log_word_equal(word, length, "schtasks") ||
                           gc_log_word_equal(word, length, "schtasks.exe"))) return true;
        if (group == 2 && (gc_log_word_equal(word, length, "setup") ||
                           gc_log_word_equal(word, length, "setup.exe"))) return true;
        if (gc_log_switch_group(word, length) != group) return false;
        end = begin;
    }
    return false;
}

static inline bool gc_log_path_start(const char* text, size_t at) {
    if (at && !gc_log_path_boundary(text[at - 1])) return false;
    const char* p = text + at;
    const bool letter = (p[0] >= 'a' && p[0] <= 'z') || (p[0] >= 'A' && p[0] <= 'Z');
    if (letter && p[1] == ':' && (p[2] == '\\' || p[2] == '/')) return true;
    if (p[0] == '\\' && p[1] == '\\') return true;
    if (p[0] == '.' && (p[1] == '\\' || p[1] == '/')) return true;
    if (p[0] != '/' || !p[1] || p[1] == '/' || p[1] == ' ' || p[1] == '\n') return false;
    return !gc_log_windows_switch(text, at);
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
