// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// hotkeys.h — global-hotkey string parsing/formatting + RegisterHotKey wrappers
// for the auto-profile per-slot hotkeys.  Parsing/formatting is separable from
// registration so the "ctrl+alt+f2" <-> {mods,vk} contract is unit-testable.

#ifndef GREEN_CURVE_HOTKEYS_H
#define GREEN_CURVE_HOTKEYS_H

#include <stddef.h>

struct HotkeyBinding {
    unsigned int mods;   // combination of MOD_CONTROL/MOD_ALT/MOD_SHIFT/MOD_WIN
    unsigned int vk;     // Win32 virtual-key code (0 = unbound)
};

// Parse a human string like "ctrl+alt+f2" (case-insensitive, '+'-separated).
// Requires at least one modifier and exactly one non-modifier key.  Returns
// false (and leaves *out zeroed) on malformed input or an unknown key token.
bool hotkey_parse(const char* text, HotkeyBinding* out);

// Format a binding back to canonical lowercase "ctrl+alt+f2".  Returns false if
// the binding is unbound or the buffer is too small.
bool hotkey_format(const HotkeyBinding* b, char* out, size_t outSize);

#if defined(_WIN32)
struct HWND__;                 // forward-declare to avoid pulling windows.h here
bool hotkey_register(struct HWND__* hwnd, int id, const HotkeyBinding* b);
void hotkey_unregister(struct HWND__* hwnd, int id);
#endif

#endif // GREEN_CURVE_HOTKEYS_H
