// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// config_text_utils.cpp — the platform-neutral text helpers that used to sit
// inside config_utils.cpp.  That file is a Win32 implementation (named-mutex
// config locking, Unicode INI storage), so linking it was the only way to get
// these functions, which in turn made the regression harness unbuildable on a
// Linux host.  They are pure string/format helpers with no OS dependency, so
// they live here and are compiled on both platforms.
//
// This translation unit is in LINUX_SOURCE_FILES, so the shipping Linux binary
// links these definitions rather than a private second copy.  linux_port.cpp
// used to duplicate trim_ascii, streqi_ascii, parse_int_strict, set_message and
// parse_fan_value verbatim, which meant the Linux daemon ran code that neither
// the regression harness nor the fuzz targets ever exercised.  Adding a helper
// here now covers both platforms; adding one to linux_port.cpp again would
// silently reintroduce that split (a source guard forbids it).
//
// Win32-only string calls are deliberately absent: StringCchCopyA and
// _strnicmp do not exist off Windows and neither is in win32_compat.h, so this
// file uses gc_snprintf and an explicit ASCII fold instead.  Both are exact
// behavioural matches, not approximations -- see the notes at each use.
//
// linux_port.cpp still duplicates the fan-curve math; that is a separate,
// larger follow-up tracked in llm-wiki/testing.md.

#include "app_shared.h"

void trim_ascii(char* s) {
    if (!s) return;

    int len = (int)strlen(s);
    int start = 0;
    while (start < len && (unsigned char)s[start] <= ' ') start++;
    int end = len;
    while (end > start && (unsigned char)s[end - 1] <= ' ') end--;

    if (start > 0 && end > start) {
        memmove(s, s + start, (size_t)(end - start));
    }

    if (end <= start) {
        s[0] = 0;
    } else {
        s[end - start] = 0;
    }
}

bool streqi_ascii(const char* a, const char* b) {
    if (!a || !b) return false;
    while (*a && *b) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return false;
        ++a;
        ++b;
    }
    return *a == 0 && *b == 0;
}

bool parse_int_strict(const char* s, int* out) {
    if (!s || !*s || !out) return false;

    char* end = nullptr;
    errno = 0;
    long v = strtol(s, &end, 10);
    if (errno == ERANGE) return false;
    if (!end || *end != 0) return false;
    if (v < -2147483647L - 1L || v > 2147483647L) return false;

    *out = (int)v;
    return true;
}

// gc_vsnprintf rather than StringCchVPrintfA: both NUL-terminate and truncate,
// but only the former exists off Windows.
void set_message(char* dst, size_t dstSize, const char* fmt, ...) {
    if (!dst || dstSize == 0) return;

    va_list ap;
    va_start(ap, fmt);
    gc_vsnprintf(dst, dstSize, fmt, ap);
    va_end(ap);
    dst[dstSize - 1] = 0;
}

// _strnicmp equivalent, restricted to ASCII and independent of locale.  The
// early return on a shared NUL is what stops the comparison reading past the
// end of a `line` shorter than `section`, which is exactly the case
// config_section_header_matches_ascii below relies on.
static bool ascii_prefix_equal_ci(const char* a, const char* b, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        unsigned char ca = (unsigned char)a[i];
        unsigned char cb = (unsigned char)b[i];
        if (tolower(ca) != tolower(cb)) return false;
        if (ca == 0) return true;
    }
    return true;
}

bool parse_cli_point_arg_w(const WCHAR* arg, int* pointIndexOut) {
    if (pointIndexOut) *pointIndexOut = -1;
    if (!arg || wcsncmp(arg, L"--point", 7) != 0) return false;

    const WCHAR* digits = arg + 7;
    if (!*digits) return false;

    int value = 0;
    for (const WCHAR* p = digits; *p; ++p) {
        if (*p < L'0' || *p > L'9') return false;
        value = value * 10 + (int)(*p - L'0');
        if (value >= VF_NUM_POINTS) return false;
    }

    if (pointIndexOut) *pointIndexOut = value;
    return true;
}

// gc_snprintf("%s") rather than StringCchCopyA: both copy what fits and
// NUL-terminate at the same boundary, so the truncation point is unchanged.
// This is also byte-for-byte what linux_port.cpp's deleted duplicate did, so
// the Linux daemon's behaviour is preserved exactly while it stops running a
// separate copy.
bool parse_fan_value(const char* text, bool* isAuto, int* pct) {
    if (!isAuto || !pct) return false;

    char buf[64] = {};
    if (text) gc_snprintf(buf, sizeof(buf), "%s", text);
    trim_ascii(buf);
    if (buf[0] == 0 || streqi_ascii(buf, "auto")) {
        *isAuto = true;
        *pct = 0;
        return true;
    }

    int value = 0;
    if (!parse_int_strict(buf, &value)) return false;
    if (value < 0 || value > 100) return false;

    *isAuto = false;
    *pct = value;
    return true;
}

// Win32 INI section names are case-insensitive.  The direct whole-file
// rewriter must use the same comparison rule or an existing [Profiles] section
// will survive beside a newly appended [profiles] section; the profile APIs
// then continue reading the stale first copy.
bool config_section_header_matches_ascii(const char* line, const char* section) {
    if (!line || !section || line[0] != '[') return false;
    size_t length = strlen(section);
    if (!ascii_prefix_equal_ci(line + 1, section, length)) return false;
    return line[1 + length] == ']';
}
