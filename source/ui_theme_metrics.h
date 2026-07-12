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
