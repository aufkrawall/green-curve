// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT

#ifndef GREEN_CURVE_MESSAGE_BOX_POLICY_H
#define GREEN_CURVE_MESSAGE_BOX_POLICY_H

#include "platform.h"

// Pure button-set resolution and geometry for the themed message box that
// replaces MessageBox in the GDI GUI.  The stock message box is painted by
// user32 from system colors and ignores the application palette entirely, so a
// confirmation appeared as a bright light-mode window in front of a dark
// window.  Keeping the decisions here means the button sets, the default and
// escape mapping, and the layout can be pinned without creating a window.
//
// MB_* values are Win32 constants; they are restated here so this header can be
// compiled into the pure regression harness, which does not link user32.

enum {
    GC_MB_OK = 0x00000000,
    GC_MB_OKCANCEL = 0x00000001,
    GC_MB_YESNOCANCEL = 0x00000003,
    GC_MB_YESNO = 0x00000004,
    GC_MB_RETRYCANCEL = 0x00000005,
    GC_MB_TYPEMASK = 0x0000000F,

    GC_MB_ICONERROR = 0x00000010,
    GC_MB_ICONQUESTION = 0x00000020,
    GC_MB_ICONWARNING = 0x00000030,
    GC_MB_ICONINFORMATION = 0x00000040,
    GC_MB_ICONMASK = 0x000000F0,

    GC_MB_DEFBUTTON1 = 0x00000000,
    GC_MB_DEFBUTTON2 = 0x00000100,
    GC_MB_DEFBUTTON3 = 0x00000200,
    GC_MB_DEFMASK = 0x00000F00,

    GC_ID_OK = 1,
    GC_ID_CANCEL = 2,
    GC_ID_RETRY = 4,
    GC_ID_YES = 6,
    GC_ID_NO = 7,
};

enum { MESSAGE_BOX_MAX_BUTTONS = 3 };

struct MessageBoxButtonSet {
    int count;
    int id[MESSAGE_BOX_MAX_BUTTONS];
    const char* label[MESSAGE_BOX_MAX_BUTTONS];
    int defaultIndex;
    // What Escape and the title-bar close button resolve to.  Always the
    // safest available answer, never a confirmation: a user who dismisses a
    // "this is a high overclock, continue?" prompt must not have said yes.
    int escapeId;
};

static inline MessageBoxButtonSet message_box_button_set(unsigned int type) {
    MessageBoxButtonSet set = {};
    switch (type & GC_MB_TYPEMASK) {
        case GC_MB_OKCANCEL:
            set.count = 2;
            set.id[0] = GC_ID_OK;    set.label[0] = "OK";
            set.id[1] = GC_ID_CANCEL; set.label[1] = "Cancel";
            set.escapeId = GC_ID_CANCEL;
            break;
        case GC_MB_YESNOCANCEL:
            set.count = 3;
            set.id[0] = GC_ID_YES;    set.label[0] = "Yes";
            set.id[1] = GC_ID_NO;     set.label[1] = "No";
            set.id[2] = GC_ID_CANCEL; set.label[2] = "Cancel";
            set.escapeId = GC_ID_CANCEL;
            break;
        case GC_MB_YESNO:
            set.count = 2;
            set.id[0] = GC_ID_YES; set.label[0] = "Yes";
            set.id[1] = GC_ID_NO;  set.label[1] = "No";
            // The stock MB_YESNO refuses to close on Escape.  Resolving to No
            // is strictly safer than an undismissable modal window.
            set.escapeId = GC_ID_NO;
            break;
        case GC_MB_RETRYCANCEL:
            set.count = 2;
            set.id[0] = GC_ID_RETRY;  set.label[0] = "Retry";
            set.id[1] = GC_ID_CANCEL; set.label[1] = "Cancel";
            set.escapeId = GC_ID_CANCEL;
            break;
        case GC_MB_OK:
        default:
            set.count = 1;
            set.id[0] = GC_ID_OK; set.label[0] = "OK";
            set.escapeId = GC_ID_OK;
            break;
    }
    unsigned int def = type & GC_MB_DEFMASK;
    int requested = def == GC_MB_DEFBUTTON2 ? 1 : (def == GC_MB_DEFBUTTON3 ? 2 : 0);
    set.defaultIndex = requested < set.count ? requested : 0;
    return set;
}

struct MessageBoxLayoutInput {
    int dpi;
    bool hasIcon;
    int iconSize;
    int textWidth;
    int textHeight;
    int buttonCount;
    int buttonWidth;
    int buttonHeight;
};

struct MessageBoxLayoutPlan {
    int clientWidth;
    int clientHeight;
    int iconX;
    int iconY;
    int textX;
    int textY;
    int buttonY;
    int firstButtonX;
    int buttonStride;
};

static inline int message_box_scale_px(int logicalPixels, int dpi) {
    if (dpi <= 0) dpi = 96;
    return (int)(((long long)logicalPixels * dpi) / 96);
}

static inline MessageBoxLayoutPlan message_box_build_layout(
    const MessageBoxLayoutInput* in) {
    MessageBoxLayoutPlan plan = {};
    if (!in) return plan;
    int dpi = in->dpi > 0 ? in->dpi : 96;
    const int margin = message_box_scale_px(16, dpi);
    const int iconGap = message_box_scale_px(14, dpi);
    const int buttonGap = message_box_scale_px(8, dpi);
    const int bodyToButtons = message_box_scale_px(18, dpi);

    int textWidth = in->textWidth > 0 ? in->textWidth : 0;
    int textHeight = in->textHeight > 0 ? in->textHeight : 0;
    int iconSize = in->hasIcon && in->iconSize > 0 ? in->iconSize : 0;
    int buttonCount = in->buttonCount > 0 ? in->buttonCount : 1;
    int buttonWidth = in->buttonWidth > 0 ? in->buttonWidth : message_box_scale_px(88, dpi);
    int buttonHeight = in->buttonHeight > 0 ? in->buttonHeight : message_box_scale_px(28, dpi);

    plan.iconX = margin;
    plan.textX = margin + (iconSize > 0 ? iconSize + iconGap : 0);
    plan.buttonStride = buttonWidth + buttonGap;

    int bodyWidth = (plan.textX - margin) + textWidth;
    int buttonsWidth = buttonCount * buttonWidth + (buttonCount - 1) * buttonGap;
    int contentWidth = bodyWidth > buttonsWidth ? bodyWidth : buttonsWidth;
    plan.clientWidth = margin * 2 + contentWidth;

    // The icon and a short single line look wrong flush-topped against each
    // other; center the shorter of the two against the taller one.
    int bodyHeight = textHeight > iconSize ? textHeight : iconSize;
    plan.textY = margin + (bodyHeight - textHeight) / 2;
    plan.iconY = margin + (bodyHeight - iconSize) / 2;
    plan.buttonY = margin + bodyHeight + bodyToButtons;
    plan.clientHeight = plan.buttonY + buttonHeight + margin;

    // Buttons are right-aligned as a group, matching every other Windows
    // confirmation the user sees.
    plan.firstButtonX = plan.clientWidth - margin - buttonsWidth;
    return plan;
}

#endif // GREEN_CURVE_MESSAGE_BOX_POLICY_H
