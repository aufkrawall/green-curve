// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT

#ifndef GREEN_CURVE_SERVICE_PIPE_ACL_POLICY_H
#define GREEN_CURVE_SERVICE_PIPE_ACL_POLICY_H

#include <stddef.h>

static const size_t SERVICE_PIPE_ACL_SDDL_CHARS = 256;

static inline bool service_pipe_acl_copy(
    wchar_t* output, size_t outputChars, const wchar_t* text) {
    if (!output || outputChars == 0 || !text) return false;
    size_t used = 0;
    while (text[used]) {
        if (used + 1 >= outputChars) {
            output[0] = L'\0';
            return false;
        }
        output[used] = text[used];
        ++used;
    }
    output[used] = L'\0';
    return true;
}

static inline bool service_pipe_acl_sid_is_well_formed(const wchar_t* sid) {
    if (!sid || sid[0] != L'S' || sid[1] != L'-') return false;
    bool digitSeen = false;
    for (size_t i = 2; sid[i]; ++i) {
        const wchar_t value = sid[i];
        if (value == L'-') {
            if (!digitSeen) return false;
            digitSeen = false;
        } else if (value >= L'0' && value <= L'9') {
            digitSeen = true;
        } else {
            return false;
        }
    }
    return digitSeen;
}

// While no interactive session exists, the listener remains reachable only to
// SYSTEM/Administrators.  Once Windows identifies the active session, its exact
// user SID replaces Authenticated Users.  The session-change recycle event then
// rebuilds this descriptor without waiting for service restart, so account
// switching does not recreate the old stale-ACL lockout.
static inline bool service_pipe_acl_sddl_for_session(
    bool activeSessionKnown, const wchar_t* activeSessionSid,
    wchar_t* output, size_t outputChars) {
    if (!output || outputChars < SERVICE_PIPE_ACL_SDDL_CHARS) return false;
    if (!activeSessionKnown) {
        return service_pipe_acl_copy(output, outputChars,
            L"D:(A;;GA;;;SY)(A;;GA;;;BA)(A;;GRGW;;;AU)");
    }
    if (!service_pipe_acl_sid_is_well_formed(activeSessionSid)) return false;

    static const wchar_t prefix[] =
        L"D:(A;;GA;;;SY)(A;;GA;;;BA)(A;;GRGW;;;";
    size_t used = 0;
    for (const wchar_t* part = prefix; *part; ++part) {
        if (used + 1 >= outputChars) { output[0] = L'\0'; return false; }
        output[used++] = *part;
    }
    for (const wchar_t* part = activeSessionSid; *part; ++part) {
        if (used + 2 >= outputChars) { output[0] = L'\0'; return false; }
        output[used++] = *part;
    }
    output[used++] = L')';
    output[used] = L'\0';
    return true;
}

#endif
