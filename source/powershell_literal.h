// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// PowerShell single-quoted string literals for the commands Green Curve SHOWS
// a user to paste into an elevated PowerShell (the crash-dump copy command,
// the install-folder permission remedy).  Those commands embed folder names
// the user or their profile controls, and inside double quotes PowerShell
// expands `$name`, `$(...)` and backtick escapes: "D:\Apps$Old" turns into
// "D:\Apps", and the pasted elevated command acts on another folder.  Inside
// single quotes nothing expands.  Pure, so either host can test it.

#ifndef GREEN_CURVE_POWERSHELL_LITERAL_H
#define GREEN_CURVE_POWERSHELL_LITERAL_H

#include <stddef.h>
#include <string.h>

// Appends `text` (UTF-8) as a single-quoted literal at out[used].  The only
// special characters are the single quotes: ASCII ' and the typographic
// U+2018..U+201B that PowerShell also accepts; each is doubled.  Sets
// *overflow (never clears it) when `out` cannot hold the whole literal; the
// text is then cut (never inside a doubled quote), so the caller must discard
// it.  Returns the new length; out stays NUL-terminated.
static inline size_t gc_powershell_append_literal(char* out, size_t outSize, size_t used,
                                                  const char* text, bool* overflow) {
    // Appends `bytes` `copies` times, or nothing when that does not fit.
    auto put = [&](const char* bytes, size_t n, size_t copies) {
        if (!out || used >= outSize || n * copies > outSize - used - 1) {
            if (overflow) *overflow = true;
            return false;
        }
        for (size_t i = 0; i < copies; ++i, used += n) memcpy(out + used, bytes, n);
        out[used] = 0;
        return true;
    };
    if (!put("'", 1, 1)) return used;
    for (const char* p = text ? text : ""; *p;) {
        const unsigned char* u = (const unsigned char*)p;
        const bool typographic = u[0] == 0xE2 && u[1] == 0x80 && u[2] >= 0x98 && u[2] <= 0x9B;
        const size_t n = typographic ? 3 : 1;
        if (!put(p, n, typographic || *p == '\'' ? 2 : 1)) return used;
        p += n;
    }
    put("'", 1, 1);
    return used;
}

#endif  // GREEN_CURVE_POWERSHELL_LITERAL_H
