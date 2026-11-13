// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#ifndef GREEN_CURVE_PROFILE_TEXT_VALIDATION_H
#define GREEN_CURVE_PROFILE_TEXT_VALIDATION_H
#include <stddef.h>

static inline bool profile_utf8_valid(const char* text, size_t size, bool documentWhitespace = false) {
    if (!text && size) return false;
    for (size_t i = 0; i < size;) {
        const unsigned char first = (unsigned char)text[i++];
        if (first < 0x80) {
            if (first == 0 || first == 0x7f || (first < 0x20 &&
                !(documentWhitespace && (first == '\t' || first == '\r' || first == '\n')))) return false;
            continue;
        }
        unsigned int cp = 0, minimum = 0;
        size_t extra = 0;
        if (first >= 0xc2 && first <= 0xdf) { cp = first & 0x1fu; minimum = 0x80; extra = 1; }
        else if (first >= 0xe0 && first <= 0xef) { cp = first & 0x0fu; minimum = 0x800; extra = 2; }
        else if (first >= 0xf0 && first <= 0xf4) { cp = first & 7u; minimum = 0x10000; extra = 3; }
        else return false;
        if (extra > size - i) return false;
        for (size_t n = 0; n < extra; ++n) {
            const unsigned char next = (unsigned char)text[i++];
            if ((next & 0xc0u) != 0x80u) return false;
            cp = (cp << 6) | (next & 0x3fu);
        }
        if (cp < minimum || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff)) return false;
    }
    return true;
}

static inline bool profile_bounded_text_valid(const char* text, size_t capacity, size_t maximum) {
    if (!text) return false;
    size_t count = 0;
    while (count < capacity && text[count]) ++count;
    return count < capacity && count <= maximum && profile_utf8_valid(text, count);
}
#endif
