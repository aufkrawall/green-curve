// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// hotkeys.cpp — see hotkeys.h.  Parsing/formatting use only ctype/string so
// they compile in the test harness; RegisterHotKey wrappers are Win32-gated.

#include "hotkeys.h"
#include <string.h>
#include <ctype.h>
#include <stdio.h>

#if defined(_WIN32)
#include "app_shared.h"   // windows.h (MOD_*/VK_*), StringCch*, ARRAY_COUNT
// MOD_NOREPEAT is a Win7+ RegisterHotKey flag; the project pins _WIN32_WINNT to
// 0x0600, so define it if the headers did not.  The OS honors the bit regardless.
#ifndef MOD_NOREPEAT
#define MOD_NOREPEAT 0x4000
#endif
#else
// Mirror the Win32 MOD_* bits so the pure parser/formatter behave identically
// in a non-Windows unit build.
#ifndef MOD_ALT
#define MOD_ALT     0x0001
#define MOD_CONTROL 0x0002
#define MOD_SHIFT   0x0004
#define MOD_WIN     0x0008
#endif
#endif

// Local VK constants so the parser does not depend on windows.h being present.
enum {
    HK_VK_SPACE = 0x20, HK_VK_TAB = 0x09, HK_VK_RETURN = 0x0D, HK_VK_ESCAPE = 0x1B,
    HK_VK_INSERT = 0x2D, HK_VK_DELETE = 0x2E, HK_VK_HOME = 0x24, HK_VK_END = 0x23,
    HK_VK_PRIOR = 0x21, HK_VK_NEXT = 0x22, HK_VK_UP = 0x26, HK_VK_DOWN = 0x28,
    HK_VK_LEFT = 0x25, HK_VK_RIGHT = 0x27, HK_VK_F1 = 0x70,
};

static char hk_lower(char c) { return (char)tolower((unsigned char)c); }

static void hk_trim(char* s) {
    if (!s) return;
    size_t n = strlen(s);
    size_t start = 0;
    while (start < n && (unsigned char)s[start] <= ' ') start++;
    size_t end = n;
    while (end > start && (unsigned char)s[end - 1] <= ' ') end--;
    if (start > 0) memmove(s, s + start, end - start);
    s[end - start] = 0;
}

struct HkNamed { const char* name; unsigned int vk; };
static const HkNamed HK_NAMED[] = {
    {"space", HK_VK_SPACE}, {"tab", HK_VK_TAB}, {"enter", HK_VK_RETURN},
    {"return", HK_VK_RETURN}, {"esc", HK_VK_ESCAPE}, {"escape", HK_VK_ESCAPE},
    {"insert", HK_VK_INSERT}, {"delete", HK_VK_DELETE}, {"del", HK_VK_DELETE},
    {"home", HK_VK_HOME}, {"end", HK_VK_END}, {"pageup", HK_VK_PRIOR},
    {"pagedown", HK_VK_NEXT}, {"up", HK_VK_UP}, {"down", HK_VK_DOWN},
    {"left", HK_VK_LEFT}, {"right", HK_VK_RIGHT},
};

static unsigned int hk_parse_key_token(const char* tok) {
    if (!tok || !tok[0]) return 0;
    size_t len = strlen(tok);
    if (len == 1) {
        char c = hk_lower(tok[0]);
        if (c >= 'a' && c <= 'z') return (unsigned int)('A' + (c - 'a'));
        if (c >= '0' && c <= '9') return (unsigned int)c;   // VK '0'..'9' == ASCII
        return 0;
    }
    // Function keys f1..f24.
    if (hk_lower(tok[0]) == 'f' && tok[1] >= '0' && tok[1] <= '9') {
        int n = 0;
        for (size_t i = 1; i < len; i++) {
            if (tok[i] < '0' || tok[i] > '9') return 0;
            n = n * 10 + (tok[i] - '0');
        }
        if (n >= 1 && n <= 24) return HK_VK_F1 + (unsigned int)(n - 1);
        return 0;
    }
    for (size_t i = 0; i < sizeof(HK_NAMED) / sizeof(HK_NAMED[0]); i++) {
        const char* a = tok; const char* b = HK_NAMED[i].name;
        bool eq = true;
        while (*a && *b) { if (hk_lower(*a) != *b) { eq = false; break; } ++a; ++b; }
        if (eq && *a == 0 && *b == 0) return HK_NAMED[i].vk;
    }
    return 0;
}

bool hotkey_parse(const char* text, HotkeyBinding* out) {
    if (out) { out->mods = 0; out->vk = 0; }
    if (!text || !out) return false;

    char buf[128] = {};
    size_t ti = 0;
    for (const char* p = text; *p && ti + 1 < sizeof(buf); ++p) buf[ti++] = *p;
    buf[ti] = 0;

    HotkeyBinding b = {0, 0};
    // Manual '+' tokenizer (avoids strtok's shared static state).
    char token[64];
    size_t start = 0;
    size_t len = strlen(buf);
    for (size_t i = 0; i <= len; i++) {
        if (buf[i] != '+' && buf[i] != 0) continue;
        size_t tlen = i - start;
        if (tlen >= sizeof(token)) return false;
        memcpy(token, buf + start, tlen);
        token[tlen] = 0;
        hk_trim(token);
        start = i + 1;
        if (token[0] == 0) continue;
        for (size_t j = 0; token[j]; j++) token[j] = hk_lower(token[j]);   // canonical lowercase
        if (strcmp(token, "ctrl") == 0 || strcmp(token, "control") == 0) b.mods |= MOD_CONTROL;
        else if (strcmp(token, "alt") == 0) b.mods |= MOD_ALT;
        else if (strcmp(token, "shift") == 0) b.mods |= MOD_SHIFT;
        else if (strcmp(token, "win") == 0 || strcmp(token, "super") == 0 || strcmp(token, "meta") == 0) b.mods |= MOD_WIN;
        else {
            unsigned int vk = hk_parse_key_token(token);
            if (vk == 0) return false;
            if (b.vk != 0) return false;   // two non-modifier keys
            b.vk = vk;
        }
    }
    if (b.vk == 0) return false;
    *out = b;
    return true;
}

bool hotkey_format(const HotkeyBinding* b, char* out, size_t outSize) {
    if (!b || !out || outSize == 0) return false;
    out[0] = 0;
    if (b->vk == 0) return false;

    char tmp[64] = {};
    size_t pos = 0;
    auto append = [&](const char* s) {
        while (*s && pos + 1 < sizeof(tmp)) tmp[pos++] = *s++;
        tmp[pos] = 0;
    };
    if (b->mods & MOD_CONTROL) append("ctrl+");
    if (b->mods & MOD_ALT) append("alt+");
    if (b->mods & MOD_SHIFT) append("shift+");
    if (b->mods & MOD_WIN) append("win+");

    char key[16] = {};
    unsigned int vk = b->vk;
    if (vk >= 'A' && vk <= 'Z') { key[0] = (char)hk_lower((char)vk); key[1] = 0; }
    else if (vk >= '0' && vk <= '9') { key[0] = (char)vk; key[1] = 0; }
    else if (vk >= HK_VK_F1 && vk <= HK_VK_F1 + 23) {
        snprintf(key, sizeof(key), "f%u", vk - HK_VK_F1 + 1);
    } else {
        const char* named = nullptr;
        for (size_t i = 0; i < sizeof(HK_NAMED) / sizeof(HK_NAMED[0]); i++) {
            if (HK_NAMED[i].vk == vk) { named = HK_NAMED[i].name; break; }
        }
        if (!named) return false;
        snprintf(key, sizeof(key), "%s", named);
    }
    append(key);

    if (strlen(tmp) >= outSize) return false;
    memcpy(out, tmp, strlen(tmp) + 1);
    return true;
}

#if defined(_WIN32)
bool hotkey_register(HWND hwnd, int id, const HotkeyBinding* b) {
    if (!hwnd || !b || b->vk == 0) return false;
    // MOD_NOREPEAT: a held hotkey fires once, not a stream, so a pinned-profile
    // toggle cannot flap.
    return RegisterHotKey(hwnd, id, b->mods | MOD_NOREPEAT, b->vk) != FALSE;
}

void hotkey_unregister(HWND hwnd, int id) {
    if (hwnd) UnregisterHotKey(hwnd, id);
}
#endif
