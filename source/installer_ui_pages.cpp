// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// Setup window layout and painting.
//
// Split from installer_ui.cpp, which keeps the window procedure, navigation,
// and construction.  Everything here answers one of two questions: where does a
// control go at the current DPI, and what does this page draw.  Keeping that
// apart from the message routing is what makes the DPI-change path reviewable:
// a monitor change re-runs exactly these functions and nothing else.
//
// Every measurement goes through gc_dp(), which scales from the 96-DPI design
// numbers using the window's *current* DPI rather than a cached process value.

#include "installer_common.h"
#include "installer_ui_internal.h"

static int gc_dp(int logicalPixels) { return gc_scaled(g_wizard.dpi, logicalPixels); }

// ---------------------------------------------------------------------------
// Page plumbing
// ---------------------------------------------------------------------------

static bool gc_page_is_visible(GcWizardPage page, int controlId) {
    switch (controlId) {
        case GC_ID_LICENSE_EDIT:
        case GC_ID_ACCEPT:
            return page == GC_PAGE_LICENSE;
        case GC_ID_PATH_EDIT:
        case GC_ID_BROWSE:
            return page == GC_PAGE_FOLDER;
        case GC_ID_START_MENU:
        case GC_ID_DESKTOP:
        case GC_ID_LAUNCH:
            return page == GC_PAGE_OPTIONS;
        default:
            return true;
    }
}

static void gc_show_control(HWND control, int controlId, GcWizardPage page) {
    if (!control) return;
    ShowWindow(control, gc_page_is_visible(page, controlId) ? SW_SHOW : SW_HIDE);
}

void gc_layout(GcWizard* wizard) {
    if (!wizard->hwnd) return;
    RECT client = {};
    GetClientRect(wizard->hwnd, &client);
    int margin = gc_dp(GC_MARGIN);
    int contentTop = gc_dp(GC_HEADER_HEIGHT) + margin;
    int contentBottom = client.bottom - gc_dp(GC_FOOTER_HEIGHT);
    int contentWidth = client.right - 2 * margin;
    int rowHeight = gc_dp(26);

    if (wizard->licenseEdit) {
        int top = contentTop + gc_dp(26);
        int bottom = contentBottom - rowHeight - gc_dp(10);
        MoveWindow(wizard->licenseEdit, margin + gc_dp(2), top + gc_dp(2),
                   contentWidth - gc_dp(4), bottom - top - gc_dp(4), TRUE);
    }
    if (wizard->acceptCheck) {
        MoveWindow(wizard->acceptCheck, margin, contentBottom - rowHeight,
                   contentWidth, rowHeight, TRUE);
    }
    if (wizard->pathEdit) {
        int top = contentTop + gc_dp(58);
        int browseWidth = gc_dp(96);
        MoveWindow(wizard->pathEdit, margin + gc_dp(2), top + gc_dp(2),
                   contentWidth - browseWidth - gc_dp(12) - gc_dp(4), gc_dp(28) - gc_dp(4), TRUE);
        MoveWindow(wizard->browseButton, margin + contentWidth - browseWidth, top,
                   browseWidth, gc_dp(28), TRUE);
    }
    if (wizard->startMenuCheck) {
        int top = contentTop + gc_dp(30);
        MoveWindow(wizard->startMenuCheck, margin, top, contentWidth, rowHeight, TRUE);
        MoveWindow(wizard->desktopCheck, margin, top + rowHeight + gc_dp(8), contentWidth, rowHeight, TRUE);
        MoveWindow(wizard->launchCheck, margin, top + 2 * (rowHeight + gc_dp(8)), contentWidth, rowHeight, TRUE);
    }

    int buttonWidth = gc_dp(104);
    int buttonHeight = gc_dp(32);
    int buttonTop = client.bottom - gc_dp(GC_FOOTER_HEIGHT) + (gc_dp(GC_FOOTER_HEIGHT) - buttonHeight) / 2;
    int right = client.right - margin;
    MoveWindow(wizard->cancelButton, right - buttonWidth, buttonTop, buttonWidth, buttonHeight, TRUE);
    MoveWindow(wizard->nextButton, right - 2 * buttonWidth - gc_dp(10), buttonTop, buttonWidth, buttonHeight, TRUE);
    MoveWindow(wizard->backButton, right - 3 * buttonWidth - gc_dp(20), buttonTop, buttonWidth, buttonHeight, TRUE);
}

void gc_update_page_controls(GcWizard* wizard) {
    gc_show_control(wizard->licenseEdit, GC_ID_LICENSE_EDIT, wizard->page);
    gc_show_control(wizard->acceptCheck, GC_ID_ACCEPT, wizard->page);
    gc_show_control(wizard->pathEdit, GC_ID_PATH_EDIT, wizard->page);
    gc_show_control(wizard->browseButton, GC_ID_BROWSE, wizard->page);
    gc_show_control(wizard->startMenuCheck, GC_ID_START_MENU, wizard->page);
    gc_show_control(wizard->desktopCheck, GC_ID_DESKTOP, wizard->page);
    gc_show_control(wizard->launchCheck, GC_ID_LAUNCH, wizard->page);

    bool working = wizard->page == GC_PAGE_PROGRESS;
    bool done = wizard->page == GC_PAGE_DONE;
    bool firstPage = wizard->page == GC_PAGE_LICENSE || wizard->page == GC_PAGE_CONFIRM_REMOVE;

    EnableWindow(wizard->backButton, !working && !done && !firstPage);
    ShowWindow(wizard->backButton, (working || done) ? SW_HIDE : SW_SHOW);
    ShowWindow(wizard->cancelButton, done ? SW_HIDE : SW_SHOW);
    // Cancelling mid-install would leave a half-replaced installation, so the
    // button is disabled rather than hidden: it stays where the user expects it
    // and visibly explains that this phase runs to completion.
    EnableWindow(wizard->cancelButton, !working);
    ShowWindow(wizard->nextButton, working ? SW_HIDE : SW_SHOW);

    const WCHAR* nextLabel = L"Next";
    if (wizard->page == GC_PAGE_OPTIONS) nextLabel = L"Install";
    if (wizard->page == GC_PAGE_CONFIRM_REMOVE) nextLabel = L"Uninstall";
    if (done) nextLabel = L"Finish";
    SetWindowTextW(wizard->nextButton, nextLabel);
    EnableWindow(wizard->nextButton, wizard->page != GC_PAGE_LICENSE || wizard->accepted);

    gc_layout(wizard);
    InvalidateRect(wizard->hwnd, nullptr, TRUE);
}

// ---------------------------------------------------------------------------
// Painting
// ---------------------------------------------------------------------------

static void gc_draw_text(HDC dc, HFONT font, COLORREF colour, const char* text,
                         int left, int top, int width, int height, UINT format) {
    WCHAR wide[1024] = {};
    gc_utf8_to_wide(text ? text : "", wide, (int)GC_ARRAY_COUNT(wide));
    RECT rect = {left, top, left + width, top + height};
    HFONT oldFont = (HFONT)SelectObject(dc, font);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, colour);
    DrawTextW(dc, wide, -1, &rect, format);
    SelectObject(dc, oldFont);
}

static void gc_draw_field_frame(HDC dc, HWND control) {
    if (!control) return;
    RECT rect = {};
    GetWindowRect(control, &rect);
    MapWindowPoints(nullptr, GetParent(control), (POINT*)&rect, 2);
    InflateRect(&rect, gc_dp(2), gc_dp(2));
    HBRUSH fill = CreateSolidBrush(COL_INPUT);
    FillRect(dc, &rect, fill);
    DeleteObject(fill);
    HPEN border = CreatePen(PS_SOLID, 1, COL_GRID);
    HPEN oldPen = (HPEN)SelectObject(dc, border);
    HBRUSH oldBrush = (HBRUSH)SelectObject(dc, (HBRUSH)GetStockObject(HOLLOW_BRUSH));
    Rectangle(dc, rect.left, rect.top, rect.right, rect.bottom);
    SelectObject(dc, oldBrush);
    DeleteObject(SelectObject(dc, oldPen));
}

void gc_paint(GcWizard* wizard, HDC dc, const RECT* client) {
    int margin = gc_dp(GC_MARGIN);
    int headerHeight = gc_dp(GC_HEADER_HEIGHT);
    int contentTop = headerHeight + margin;
    int contentWidth = client->right - 2 * margin;

    // Header band, slightly lifted off the page so the product name and the
    // version being installed read as a title rather than as body text.
    RECT header = {0, 0, client->right, headerHeight};
    HBRUSH headerBrush = CreateSolidBrush(COL_TOOLTIP_BG);
    FillRect(dc, &header, headerBrush);
    DeleteObject(headerBrush);
    RECT rule = {0, headerHeight - 1, client->right, headerHeight};
    HBRUSH ruleBrush = CreateSolidBrush(COL_GRID);
    FillRect(dc, &rule, ruleBrush);
    DeleteObject(ruleBrush);

    gc_draw_text(dc, wizard->fonts.heading, COL_TEXT, GC_SETUP_PRODUCT_NAME,
                 margin, gc_dp(14), contentWidth, gc_dp(28), DT_LEFT | DT_SINGLELINE | DT_NOPREFIX);
    char versionLine[128] = {};
    snprintf(versionLine, sizeof(versionLine),
             wizard->uninstallMode ? "Uninstall version %s" : "Version %s", APP_VERSION);
    gc_draw_text(dc, wizard->fonts.body, COL_CURVE, versionLine,
                 margin, gc_dp(44), contentWidth, gc_dp(20), DT_LEFT | DT_SINGLELINE | DT_NOPREFIX);

    switch (wizard->page) {
        case GC_PAGE_LICENSE:
            gc_draw_text(dc, wizard->fonts.body, COL_LABEL,
                         "This program is released under the MIT license. Read it, then continue.",
                         margin, contentTop, contentWidth, gc_dp(22),
                         DT_LEFT | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
            gc_draw_field_frame(dc, wizard->licenseEdit);
            break;
        case GC_PAGE_FOLDER: {
            gc_draw_text(dc, wizard->fonts.body, COL_LABEL,
                         "Choose a folder directly under Program Files. This protects the "
                         "LocalSystem background service from user-writable parent folders.",
                         margin, contentTop, contentWidth, gc_dp(40),
                         DT_LEFT | DT_WORDBREAK | DT_NOPREFIX);
            if (wizard->prior.present) {
                char note[512] = {};
                snprintf(note, sizeof(note),
                         "Green Curve %s is already installed in %s. Setup can upgrade it in place only when "
                         "that folder is directly under Program Files; otherwise choose a supported folder to "
                         "move the installation and leave the old files for you to delete.",
                         wizard->prior.version[0] ? wizard->prior.version : "(unknown version)",
                         wizard->prior.directory);
                gc_draw_text(dc, wizard->fonts.small_text, COL_PENDING, note,
                             margin, contentTop + gc_dp(100), contentWidth, gc_dp(72),
                             DT_LEFT | DT_WORDBREAK | DT_NOPREFIX);
            }
            gc_draw_field_frame(dc, wizard->pathEdit);
            break;
        }
        case GC_PAGE_OPTIONS:
            gc_draw_text(dc, wizard->fonts.body, COL_LABEL, "Choose what setup should do:",
                         margin, contentTop, contentWidth, gc_dp(22),
                         DT_LEFT | DT_SINGLELINE | DT_NOPREFIX);
            if (wizard->prior.present) {
                gc_draw_text(dc, wizard->fonts.small_text, COL_LABEL,
                             "Your current overclock, power, and fan settings are read before the update and "
                             "applied again once the new version is running.",
                             margin, contentTop + gc_dp(140), contentWidth, gc_dp(56),
                             DT_LEFT | DT_WORDBREAK | DT_NOPREFIX);
            }
            break;
        case GC_PAGE_CONFIRM_REMOVE: {
            char note[600] = {};
            char directory[GC_INSTALLER_MAX_PATH_CHARS] = {};
            gc_wide_to_utf8(wizard->uninstallDirectory, directory, (int)sizeof(directory));
            snprintf(note, sizeof(note),
                     "Green Curve will be removed from %s.\n\n"
                     "The background service is stopped and unregistered, and your GPU returns to its "
                     "default settings. Your saved profiles in %%LOCALAPPDATA%%\\Green Curve are left alone.",
                     directory);
            gc_draw_text(dc, wizard->fonts.body, COL_TEXT, note,
                         margin, contentTop, contentWidth, gc_dp(160),
                         DT_LEFT | DT_WORDBREAK | DT_NOPREFIX);
            break;
        }
        case GC_PAGE_PROGRESS: {
            int percent = 0;
            char status[160] = {};
            EnterCriticalSection(&wizard->progressLock);
            percent = wizard->progressPercent;
            StringCchCopyA(status, GC_ARRAY_COUNT(status), wizard->progressStatus);
            LeaveCriticalSection(&wizard->progressLock);
            gc_draw_text(dc, wizard->fonts.body, COL_TEXT, status,
                         margin, contentTop + gc_dp(30), contentWidth, gc_dp(22),
                         DT_LEFT | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
            RECT bar = {margin, contentTop + gc_dp(64), margin + contentWidth, contentTop + gc_dp(64) + gc_dp(16)};
            gc_draw_progress_bar(dc, &bar, percent);
            char percentText[32] = {};
            snprintf(percentText, sizeof(percentText), "%d%%", percent);
            gc_draw_text(dc, wizard->fonts.small_text, COL_LABEL, percentText,
                         margin, contentTop + gc_dp(88), contentWidth, gc_dp(18),
                         DT_LEFT | DT_SINGLELINE | DT_NOPREFIX);
            break;
        }
        case GC_PAGE_DONE:
            gc_draw_text(dc, wizard->fonts.heading,
                         wizard->workSucceeded ? COL_CURVE : COL_POINT,
                         wizard->workSucceeded
                             ? (wizard->uninstallMode ? "Green Curve was removed" : "Green Curve is installed")
                             : (wizard->uninstallMode ? "Uninstall did not finish" : "Setup did not finish"),
                         margin, contentTop, contentWidth, gc_dp(30),
                         DT_LEFT | DT_SINGLELINE | DT_NOPREFIX);
            gc_draw_text(dc, wizard->fonts.body, COL_TEXT, wizard->resultMessage,
                         margin, contentTop + gc_dp(42), contentWidth, gc_dp(180),
                         DT_LEFT | DT_WORDBREAK | DT_NOPREFIX);
            break;
    }
}
