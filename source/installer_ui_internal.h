// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// Declarations shared between the setup window (installer_ui.cpp) and the
// theme/owner-draw shard (installer_theme.cpp).  Kept out of
// installer_common.h so the non-GUI shards do not pull in painting details.

#ifndef GREEN_CURVE_INSTALLER_UI_INTERNAL_H
#define GREEN_CURVE_INSTALLER_UI_INTERNAL_H

#include "installer_common.h"

struct GcThemeFonts {
    HFONT body;
    HFONT heading;
    HFONT small_text;  // not `small`: MSVC's rpcndr.h #defines it as `char`
    HFONT monospace;
    UINT dpi;
};

UINT gc_dpi_for_window(HWND hwnd);
int gc_scaled(UINT dpi, int logicalPixels);
void gc_create_theme_fonts(GcThemeFonts* fonts, UINT dpi);
void gc_destroy_theme_fonts(GcThemeFonts* fonts);
void gc_draw_themed_button(const DRAWITEMSTRUCT* item, HFONT font);
void gc_draw_themed_checkbox(const DRAWITEMSTRUCT* item, HFONT font, UINT dpi, bool checked);
void gc_draw_progress_bar(HDC dc, const RECT* bounds, int percent);

// ---------------------------------------------------------------------------
// The setup window's own state, shared between the window/navigation shard
// (installer_ui.cpp) and the layout/painting shard (installer_ui_pages.cpp).
// ---------------------------------------------------------------------------

enum GcWizardPage {
    GC_PAGE_LICENSE = 0,
    GC_PAGE_FOLDER,
    GC_PAGE_OPTIONS,
    GC_PAGE_CONFIRM_REMOVE,
    GC_PAGE_PROGRESS,
    GC_PAGE_DONE,
};

enum {
    GC_ID_ACCEPT = 1001,
    GC_ID_PATH_EDIT,
    GC_ID_BROWSE,
    GC_ID_START_MENU,
    GC_ID_DESKTOP,
    GC_ID_LAUNCH,
    GC_ID_BACK,
    GC_ID_NEXT,
    GC_ID_CANCEL,
    GC_ID_LICENSE_EDIT,
};

#define GC_WM_PROGRESS (WM_APP + 1)
#define GC_WM_WORK_DONE (WM_APP + 2)

// Logical (96-DPI) window size.  Wide enough for a full Program Files path on
// one line, tall enough for a readable amount of licence text.
#define GC_WINDOW_WIDTH 620
#define GC_WINDOW_HEIGHT 470
#define GC_MARGIN 22
#define GC_HEADER_HEIGHT 74
#define GC_FOOTER_HEIGHT 62

struct GcWizard {
    HWND hwnd;
    HINSTANCE instance;
    GcThemeFonts fonts;
    UINT dpi;
    GcWizardPage page;
    bool uninstallMode;

    HWND licenseEdit;
    HWND acceptCheck;
    HWND pathEdit;
    HWND browseButton;
    HWND startMenuCheck;
    HWND desktopCheck;
    HWND launchCheck;
    HWND backButton;
    HWND nextButton;
    HWND cancelButton;

    bool accepted;
    bool startMenu;
    bool desktop;
    bool launch;

    GcInstallerOptions options;
    GcPriorInstall prior;
    char defaultDirectory[GC_INSTALLER_MAX_PATH_CHARS];
    WCHAR uninstallDirectory[GC_INSTALLER_MAX_PATH_CHARS];
    GcInstallContext install;

    HANDLE worker;
    CRITICAL_SECTION progressLock;
    int progressPercent;
    char progressStatus[160];
    bool workFinished;
    bool workSucceeded;
    char resultMessage[512];

    HBRUSH backgroundBrush;
    HBRUSH inputBrush;
    int exitCode;
};

// One window per process; the worker thread reaches it through the GcWizard*
// it was handed, and the painting shard scales against its current DPI.
extern GcWizard g_wizard;

// installer_ui_pages.cpp
void gc_layout(GcWizard* wizard);
void gc_update_page_controls(GcWizard* wizard);
void gc_paint(GcWizard* wizard, HDC dc, const RECT* client);

// installer_ui.cpp
void gc_set_control_font(HWND control, HFONT font);
void gc_set_text_utf8(HWND control, const char* text);

#endif // GREEN_CURVE_INSTALLER_UI_INTERNAL_H
