// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT

#pragma once

static inline int ui_theme_scale_px(int logicalPixels, int dpi) {
    if (dpi <= 0) dpi = 96;
    return (int)(((long long)logicalPixels * dpi) / 96);
}

// Every ordinary checkbox uses the same 14-logical-pixel square regardless of
// whether its label lives inside the BUTTON or in a neighboring STATIC.
static inline int ui_theme_checkbox_box_size(
    int controlWidth, int controlHeight, int dpi) {
    int size = ui_theme_scale_px(14, dpi);
    if (size < 1) size = 1;
    if (controlWidth > 0 && size > controlWidth) size = controlWidth;
    if (controlHeight > 0 && size > controlHeight) size = controlHeight;
    return size;
}

// A labeled checkbox insets its box from the control's left edge and starts the
// label one gap further right.  The renderer and the width fit share these two
// metrics so the pixels and the clickable rectangle can never drift apart.
static inline int ui_theme_checkbox_box_inset(int dpi) {
    return ui_theme_scale_px(2, dpi);
}

static inline int ui_theme_checkbox_label_gap(int dpi) {
    return ui_theme_scale_px(8, dpi);
}

// Offset of the label's first pixel from the control's left edge.
static inline int ui_theme_labeled_checkbox_label_offset(
    int controlHeight, int dpi) {
    return ui_theme_checkbox_box_inset(dpi) +
        ui_theme_checkbox_box_size(0, controlHeight, dpi) +
        ui_theme_checkbox_label_gap(dpi);
}

// F-CHECKBOX-HIT  Width a labeled checkbox needs so its BUTTON covers exactly
// the box, the gap, and the label -- and nothing past the label.
//
// Both bounds are deliberate.  Demanding pixel-accurate aim at a 14-logical-
// pixel square is bad targeting, so the descriptive text and the gap in between
// must toggle the box as well; that is what makes the label part of the
// control rather than decoration next to it.  But a BUTTON sized by eye to
// "comfortably wide" turns the empty background to the right of the text into a
// silent toggle, which is worse than the small target: nothing there looks
// clickable.  The trailing inset only balances the leading one and keeps
// DT_END_ELLIPSIS from clipping a glyph whose advance width the measurement
// rounded down.
static inline int ui_theme_labeled_checkbox_width(
    int textWidth, int controlHeight, int dpi) {
    if (textWidth < 0) textWidth = 0;
    int width = ui_theme_labeled_checkbox_label_offset(controlHeight, dpi) +
        textWidth + ui_theme_checkbox_box_inset(dpi);
    if (width < 1) width = 1;
    return width;
}
