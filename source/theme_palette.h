// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// The Green Curve window palette, in one place.
//
// This used to live inside app_shared.h, which drags in the entire application
// model and therefore cannot be included by the standalone installer.  The
// installer must paint the exact same window as the program it installs, so the
// colours moved here instead of being duplicated (a duplicate would silently
// drift the first time a shade is tweaked).  Nothing in this header depends on
// anything but <windows.h>'s RGB macro.

#ifndef GREEN_CURVE_THEME_PALETTE_H
#define GREEN_CURVE_THEME_PALETTE_H

#define COL_BG              RGB(0x18, 0x18, 0x28)
#define COL_PANEL           RGB(0x18, 0x18, 0x28)
#define COL_INPUT           RGB(0x12, 0x12, 0x1C)
#define COL_GRID            RGB(0x40, 0x40, 0x55)
#define COL_AXIS            RGB(0x80, 0x80, 0x90)
#define COL_CURVE           RGB(0x50, 0xD0, 0x80)
#define COL_POINT           RGB(0xFF, 0x60, 0x60)
#define COL_TEXT            RGB(0xE0, 0xE0, 0xE0)
#define COL_LABEL           RGB(0xA0, 0xA0, 0xB0)
#define COL_BUTTON          RGB(0x2B, 0x42, 0x66)
#define COL_BUTTON_PRESSED  RGB(0x23, 0x36, 0x52)
#define COL_BUTTON_BORDER   RGB(0x78, 0x9A, 0xD8)
#define COL_BUTTON_DISABLED RGB(0x2A, 0x2A, 0x38)
// The greyed border/label shade every disabled owner-drawn control shares.
#define COL_DISABLED_BORDER RGB(0x56, 0x56, 0x64)
// The bright caption colour an enabled owner-drawn button paints its label in.
#define COL_BUTTON_LABEL    RGB(0xF0, 0xF4, 0xFF)
// F-PENDING. Fields, curve markers, and the pending graph line whose value has
// been typed/loaded but not applied to the GPU yet. Deliberately far from both
// COL_CURVE (applied, green) and COL_POINT (marker red) so "not applied yet"
// never reads as either. COL_PENDING_DIM is the same hue at roughly the
// enabled/disabled contrast ratio the rest of the window uses, because the
// locked-tail MHz edits are disabled and must still read as disabled.
#define COL_PENDING         RGB(0xFF, 0xA8, 0x3D)
#define COL_PENDING_DIM     RGB(0xB0, 0x74, 0x2A)
// Hover tooltips. Slightly lighter than COL_BG so a tip reads as a floating
// element rather than a hole in the window, and nothing like the system
// info-tip yellow a non-themed tooltip would use by default.
#define COL_TOOLTIP_BG      RGB(0x24, 0x24, 0x38)
#define COL_TOOLTIP_TEXT    RGB(0xE8, 0xE8, 0xF2)

#endif // GREEN_CURVE_THEME_PALETTE_H
