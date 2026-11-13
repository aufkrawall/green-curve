// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#pragma once
#include <stddef.h>

// Only payload staging leaves are installer-owned. Never accept directories,
// arbitrary prefixes or a truncated attempt identifier.
static inline bool gc_install_temporary_name(const wchar_t* name, size_t length,
                                             bool attemptOnly = false) {
    if (!name) return false;
    static const wchar_t* const payloads[] = {
        L"greencurve.exe", L"greencurve-service.exe", L"README.md", L"LICENSE",
        L"greencurve-uninstall.exe", L"uninstall.exe"
    };
    for (const wchar_t* payload : payloads) {
        size_t prefix = 0;
        for (; prefix < length && payload[prefix]; ++prefix) {
            wchar_t a = name[prefix], b = payload[prefix];
            if (a >= L'A' && a <= L'Z') a += L'a' - L'A';
            if (b >= L'A' && b <= L'Z') b += L'a' - L'A';
            if (a != b) break;
        }
        if (payload[prefix]) continue;
        static const wchar_t* const suffixes[] = {L".gcnew", L".gcrestore"};
        for (const wchar_t* suffix : suffixes) {
            size_t i = prefix, j = 0;
            for (; i < length && suffix[j] && name[i] == suffix[j]; ++i, ++j) {}
            if (suffix[j]) continue;
            if (i == length) return !attemptOnly;
            const wchar_t* marker = L".gcattempt-";
            for (j = 0; i < length && marker[j] && name[i] == marker[j]; ++i, ++j) {}
            if (marker[j] || length - i != 32) continue;
            for (; i < length; ++i) {
                const wchar_t ch = name[i];
                if (!((ch >= L'0' && ch <= L'9') || (ch >= L'a' && ch <= L'f') ||
                      (ch >= L'A' && ch <= L'F'))) break;
            }
            if (i == length) return true;
        }
    }
    return false;
}
