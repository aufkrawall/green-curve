// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// The setup window.
//
// One window, five pages, no dialog resources: the whole surface is painted
// with the Green Curve palette so setup and program look like one thing.  The
// work itself runs on a worker thread and reports back through PostMessage, so
// the window keeps repainting (and stays responsive to a DPI change or a drag
// to another monitor) while the service is being stopped and files replaced.
//
// The uninstaller is the same window with a different first page and a
// different worker, which is why both live here.

#include "installer_common.h"
#include "installer_ui_internal.h"

GcWizard g_wizard;

// ---------------------------------------------------------------------------
// Dark control theming
// ---------------------------------------------------------------------------

// The client area is always dark, so the edit controls' scrollbars are always
// asked for the dark variant regardless of the system setting.  A light
// scrollbar inside a dark panel is the single most obvious "this was not
// designed together" tell.
static void gc_enable_dark_controls(HWND hwnd) {
    HMODULE uxtheme = LoadLibraryExW(L"uxtheme.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!uxtheme) return;
    typedef bool(WINAPI * AllowDarkModeForWindowFn)(HWND, bool);
    typedef int(WINAPI * SetPreferredAppModeFn)(int);
    auto allowDark = (AllowDarkModeForWindowFn)GetProcAddress(uxtheme, MAKEINTRESOURCEA(133));
    auto setAppMode = (SetPreferredAppModeFn)GetProcAddress(uxtheme, MAKEINTRESOURCEA(135));
    if (setAppMode) setAppMode(1 /* AllowDark */);
    if (allowDark && hwnd) allowDark(hwnd, true);
    if (hwnd) SetWindowTheme(hwnd, L"DarkMode_Explorer", nullptr);
    FreeLibrary(uxtheme);
}

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------

void gc_set_control_font(HWND control, HFONT font) {
    if (control && font) SendMessageW(control, WM_SETFONT, (WPARAM)font, TRUE);
}

void gc_set_text_utf8(HWND control, const char* text) {
    WCHAR wide[1024] = {};
    gc_utf8_to_wide(text ? text : "", wide, (int)GC_ARRAY_COUNT(wide));
    SetWindowTextW(control, wide);
}

static void gc_get_text_utf8(HWND control, char* out, size_t outCount) {
    if (!out || outCount == 0) return;
    out[0] = 0;
    WCHAR wide[GC_INSTALLER_MAX_PATH_CHARS] = {};
    GetWindowTextW(control, wide, (int)GC_ARRAY_COUNT(wide));
    gc_wide_to_utf8(wide, out, (int)outCount);
}

// ---------------------------------------------------------------------------
// Worker
// ---------------------------------------------------------------------------

static void gc_progress_callback(void* context, int percent, const char* status) {
    auto* wizard = (GcWizard*)context;
    if (!wizard) return;
    EnterCriticalSection(&wizard->progressLock);
    wizard->progressPercent = percent;
    StringCchCopyA(wizard->progressStatus, GC_ARRAY_COUNT(wizard->progressStatus), status ? status : "");
    LeaveCriticalSection(&wizard->progressLock);
    PostMessageW(wizard->hwnd, GC_WM_PROGRESS, 0, 0);
}

static DWORD WINAPI gc_worker_thread(LPVOID parameter) {
    auto* wizard = (GcWizard*)parameter;
    // CoInitializeEx applies to the calling THREAD, so WinMain's call does not
    // cover this one.  Without this, shortcut creation failed with
    // CO_E_NOTINITIALIZED (0x800401f0) and every install silently produced no
    // Start menu or desktop icon.
    //
    // Apartment-threaded, matching the shell link handler's own model, so
    // IShellLink is created in-process and every call is a direct one.  An MTA
    // here would work but only by having COM spin up a host apartment and
    // marshal into it, which is a lot of machinery for three outgoing calls.
    HRESULT comStatus = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    if (FAILED(comStatus)) {
        gc_log_step("worker: COM could not be initialized (hr 0x%08lx); shortcuts will be skipped",
                    (unsigned long)comStatus);
    }
    bool ok = false;
    if (wizard->uninstallMode) {
        char error[512] = {};
        gc_progress_callback(wizard, 20, "Removing Green Curve...");
        ok = gc_uninstall_execute(wizard->uninstallDirectory, error, sizeof(error));
        if (ok) {
            StringCchCopyA(wizard->resultMessage, GC_ARRAY_COUNT(wizard->resultMessage),
                           "Green Curve has been removed. Files that were not installed by setup were left in "
                           "place, and your saved profiles are untouched.");
        } else {
            StringCchCopyA(wizard->resultMessage, GC_ARRAY_COUNT(wizard->resultMessage),
                           error[0] ? error : "The installation could not be removed.");
        }
        gc_progress_callback(wizard, 100, ok ? "Removed." : "Failed.");
    } else {
        ok = gc_install_execute(&wizard->install);
        if (ok) {
            // A restore that did not happen is worth saying out loud: the user
            // would otherwise just find their GPU back at stock settings.
            const char* restoreNote = "";
            if (wizard->install.settingsRestoreAttempted) {
                restoreNote = wizard->install.settingsRestored
                    ? "\n\nYour previous overclock, power, and fan settings were applied again."
                    : "\n\nYour previous settings could NOT be applied again - open Green Curve and "
                      "press Apply. greencurve_cli_log.txt in %LOCALAPPDATA%\\Green Curve says why.";
            }
            snprintf(wizard->resultMessage, sizeof(wizard->resultMessage),
                     "Green Curve %s is installed in %s and the background service is running.%s%s",
                     APP_VERSION, wizard->install.plan.targetDirectory, restoreNote,
                     wizard->install.plan.directoryChanged
                         ? "\n\nThe previous installation folder was left in place; you can delete it whenever "
                           "you like."
                         : "");
        } else {
            snprintf(wizard->resultMessage, sizeof(wizard->resultMessage), "%s",
                     wizard->install.error[0] ? wizard->install.error : "Setup could not complete.");
        }
    }
    wizard->workSucceeded = ok;
    if (SUCCEEDED(comStatus)) CoUninitialize();
    PostMessageW(wizard->hwnd, GC_WM_WORK_DONE, 0, 0);
    return ok ? 0u : 1u;
}

static void gc_start_work(GcWizard* wizard) {
    wizard->page = GC_PAGE_PROGRESS;
    wizard->progressPercent = 0;
    StringCchCopyA(wizard->progressStatus, GC_ARRAY_COUNT(wizard->progressStatus), "Starting...");
    gc_update_page_controls(wizard);
    wizard->worker = CreateThread(nullptr, 0, gc_worker_thread, wizard, 0, nullptr);
    if (!wizard->worker) {
        gc_log_fail("ui: could not start the worker thread (error %lu)", GetLastError());
        wizard->workSucceeded = false;
        StringCchCopyA(wizard->resultMessage, GC_ARRAY_COUNT(wizard->resultMessage),
                       "Setup could not start its worker thread.");
        wizard->page = GC_PAGE_DONE;
        gc_update_page_controls(wizard);
    }
}

// ---------------------------------------------------------------------------
// Navigation
// ---------------------------------------------------------------------------

// Fold the folder page's answer back into the options, then re-derive the whole
// plan.  Re-deriving (rather than patching the plan in place) keeps the pure
// policy the single decision-maker even when the user steps backwards.
static bool gc_commit_folder_page(GcWizard* wizard) {
    char chosen[GC_INSTALLER_MAX_PATH_CHARS] = {};
    gc_get_text_utf8(wizard->pathEdit, chosen, sizeof(chosen));
    const char* reason = nullptr;
    if (!gc_install_directory_is_acceptable(chosen, &reason)) {
        gc_show_message(wizard->hwnd, reason ? reason : "That installation folder cannot be used.",
                        "Green Curve Setup", true);
        SetFocus(wizard->pathEdit);
        return false;
    }
    WCHAR chosenWide[GC_INSTALLER_MAX_PATH_CHARS] = {};
    if (!gc_utf8_to_wide(chosen, chosenWide,
            (int)GC_ARRAY_COUNT(chosenWide)) ||
        !gc_install_directory_is_secure_rooted(chosenWide)) {
        gc_show_message(wizard->hwnd,
            "Green Curve must be installed in a direct child folder of Program Files because its background service runs as LocalSystem.",
            "Green Curve Setup", true);
        SetFocus(wizard->pathEdit);
        return false;
    }
    StringCchCopyA(wizard->options.directory, GC_ARRAY_COUNT(wizard->options.directory), chosen);
    wizard->options.hasDirectory = true;
    return true;
}

static void gc_commit_options_page(GcWizard* wizard) {
    wizard->options.startMenuShortcut = wizard->startMenu ? GC_TOGGLE_ON : GC_TOGGLE_OFF;
    wizard->options.desktopShortcut = wizard->desktop ? GC_TOGGLE_ON : GC_TOGGLE_OFF;
    wizard->options.launchAfterInstall = wizard->launch ? GC_TOGGLE_ON : GC_TOGGLE_OFF;
    gc_install_build_plan(&wizard->options, &wizard->prior, wizard->defaultDirectory,
                          &wizard->install.plan);
}

static void gc_advance(GcWizard* wizard) {
    switch (wizard->page) {
        case GC_PAGE_LICENSE:
            if (!wizard->accepted) return;
            wizard->page = GC_PAGE_FOLDER;
            break;
        case GC_PAGE_FOLDER:
            if (!gc_commit_folder_page(wizard)) return;
            wizard->page = GC_PAGE_OPTIONS;
            break;
        case GC_PAGE_OPTIONS:
            gc_commit_options_page(wizard);
            if (!wizard->install.plan.valid) {
                gc_show_message(wizard->hwnd, wizard->install.plan.error, "Green Curve Setup", true);
                wizard->page = GC_PAGE_FOLDER;
                break;
            }
            gc_start_work(wizard);
            return;
        case GC_PAGE_CONFIRM_REMOVE:
            gc_start_work(wizard);
            return;
        case GC_PAGE_PROGRESS:
            return;
        case GC_PAGE_DONE:
            if (!wizard->uninstallMode && wizard->workSucceeded && wizard->install.plan.launchAfterInstall) {
                WCHAR directory[GC_INSTALLER_MAX_PATH_CHARS] = {};
                if (gc_utf8_to_wide(wizard->install.plan.targetDirectory, directory,
                                    (int)GC_ARRAY_COUNT(directory))) {
                    gc_launch_installed_gui(directory);
                }
            }
            wizard->exitCode = wizard->workSucceeded ? 0 : 1;
            DestroyWindow(wizard->hwnd);
            return;
    }
    gc_update_page_controls(wizard);
}

static void gc_go_back(GcWizard* wizard) {
    if (wizard->page == GC_PAGE_FOLDER) wizard->page = GC_PAGE_LICENSE;
    else if (wizard->page == GC_PAGE_OPTIONS) wizard->page = GC_PAGE_FOLDER;
    else return;
    gc_update_page_controls(wizard);
}

// Modern folder picker.  The user selects the PARENT folder; setup appends the
// fixed "Green Curve" name, unless the folder they picked already is one, which
// avoids the "Green Curve\Green Curve" people otherwise create by browsing to
// their existing installation.
static void gc_browse_for_folder(GcWizard* wizard) {
    IFileDialog* dialog = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                IID_IFileDialog, (void**)&dialog)) || !dialog) {
        return;
    }
    DWORD options = 0;
    if (SUCCEEDED(dialog->GetOptions(&options))) {
        dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_PATHMUSTEXIST | FOS_FORCEFILESYSTEM);
    }
    dialog->SetTitle(L"Select the folder that should contain \"Green Curve\"");
    if (SUCCEEDED(dialog->Show(wizard->hwnd))) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dialog->GetResult(&item)) && item) {
            PWSTR selected = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &selected)) && selected) {
                char parent[GC_INSTALLER_MAX_PATH_CHARS] = {};
                if (gc_wide_to_utf8(selected, parent, (int)sizeof(parent))) {
                    char resolved[GC_INSTALLER_MAX_PATH_CHARS] = {};
                    const WCHAR* leaf = wcsrchr(selected, L'\\');
                    bool alreadyNamed = leaf && lstrcmpiW(leaf + 1, GC_SETUP_PRODUCT_NAME_W) == 0;
                    if (alreadyNamed) {
                        StringCchCopyA(resolved, GC_ARRAY_COUNT(resolved), parent);
                    } else if (!gc_install_default_directory(parent, resolved, sizeof(resolved))) {
                        StringCchCopyA(resolved, GC_ARRAY_COUNT(resolved), parent);
                    }
                    gc_set_text_utf8(wizard->pathEdit, resolved);
                }
                CoTaskMemFree(selected);
            }
            item->Release();
        }
    }
    dialog->Release();
}

// ---------------------------------------------------------------------------
// Window procedure
// ---------------------------------------------------------------------------

static void gc_toggle_checkbox(GcWizard* wizard, HWND control, bool* value) {
    *value = !*value;
    InvalidateRect(control, nullptr, TRUE);
    if (control == wizard->acceptCheck) {
        EnableWindow(wizard->nextButton, wizard->accepted);
        InvalidateRect(wizard->nextButton, nullptr, TRUE);
    }
}

// Title-bar / Alt-Tab / taskbar icon.
//
// The .rc embeds GC_SETUP_ICON_ID, which is what Explorer shows for the setup
// file, but a window gets its icon from its class (or WM_SETICON) -- and that
// was LoadIconW(nullptr, IDI_APPLICATION), the stock Windows executable icon.
// So the file looked right and the running window did not.
//
// Sized per DPI rather than per primary monitor: the window is
// per-monitor-v2 aware, so GetSystemMetrics() alone would pick the wrong size
// on a secondary display.  GetSystemMetricsForDpi is resolved dynamically for
// the same reason gc_dpi_for_window() resolves GetDpiForWindow that way -- the
// two toolchains disagree about which headers declare it.
//
// LR_SHARED: the returned handle belongs to the module's resource, must not be
// destroyed, and repeated loads of the same size return the same handle.
static HICON gc_load_setup_icon(HINSTANCE instance, UINT dpi, int metric) {
    int size = GetSystemMetrics(metric);
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (user32) {
        typedef int(WINAPI * GetSystemMetricsForDpiFn)(int, UINT);
        auto forDpi = (GetSystemMetricsForDpiFn)GetProcAddress(user32, "GetSystemMetricsForDpi");
        if (forDpi) {
            int scaled = forDpi(metric, dpi);
            if (scaled > 0) size = scaled;
        }
    }
    HICON icon = (HICON)LoadImageW(instance, MAKEINTRESOURCEW(GC_SETUP_ICON_ID),
                                   IMAGE_ICON, size, size, LR_SHARED);
    if (!icon) {
        gc_log_step("ui: icon %d missing at %dpx (error %lu); falling back to the system icon",
                    GC_SETUP_ICON_ID, size, GetLastError());
        icon = LoadIconW(nullptr, IDI_APPLICATION);
    }
    return icon;
}

// Re-derive both icons for `wizard->dpi` and hand them to the window.  Called
// once at creation and again on WM_DPICHANGED, so dragging setup to a monitor
// with a different scale does not leave a stretched caption icon behind.
static void gc_apply_window_icons(GcWizard* wizard) {
    if (!wizard || !wizard->hwnd) return;
    HICON large = gc_load_setup_icon(wizard->instance, wizard->dpi, SM_CXICON);
    HICON small = gc_load_setup_icon(wizard->instance, wizard->dpi, SM_CXSMICON);
    SendMessageW(wizard->hwnd, WM_SETICON, ICON_BIG, (LPARAM)large);
    SendMessageW(wizard->hwnd, WM_SETICON, ICON_SMALL, (LPARAM)small);
}

static LRESULT CALLBACK gc_wizard_proc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    GcWizard* wizard = &g_wizard;
    switch (message) {
        case WM_ERASEBKGND: {
            RECT client = {};
            GetClientRect(hwnd, &client);
            FillRect((HDC)wParam, &client, wizard->backgroundBrush);
            return 1;
        }
        case WM_PAINT: {
            PAINTSTRUCT paint = {};
            HDC dc = BeginPaint(hwnd, &paint);
            RECT client = {};
            GetClientRect(hwnd, &client);
            gc_paint(wizard, dc, &client);
            EndPaint(hwnd, &paint);
            return 0;
        }
        case WM_CTLCOLOREDIT:
        case WM_CTLCOLORSTATIC: {
            HDC dc = (HDC)wParam;
            SetTextColor(dc, COL_TEXT);
            SetBkColor(dc, COL_INPUT);
            return (LRESULT)wizard->inputBrush;
        }
        case WM_DRAWITEM: {
            const auto* item = (const DRAWITEMSTRUCT*)lParam;
            if (!item || item->CtlType != ODT_BUTTON) break;
            switch (item->CtlID) {
                case GC_ID_ACCEPT:
                    gc_draw_themed_checkbox(item, wizard->fonts.body, wizard->dpi, wizard->accepted);
                    return TRUE;
                case GC_ID_START_MENU:
                    gc_draw_themed_checkbox(item, wizard->fonts.body, wizard->dpi, wizard->startMenu);
                    return TRUE;
                case GC_ID_DESKTOP:
                    gc_draw_themed_checkbox(item, wizard->fonts.body, wizard->dpi, wizard->desktop);
                    return TRUE;
                case GC_ID_LAUNCH:
                    gc_draw_themed_checkbox(item, wizard->fonts.body, wizard->dpi, wizard->launch);
                    return TRUE;
                default:
                    gc_draw_themed_button(item, wizard->fonts.body);
                    return TRUE;
            }
        }
        case WM_COMMAND: {
            if (HIWORD(wParam) != BN_CLICKED) break;
            switch (LOWORD(wParam)) {
                case GC_ID_ACCEPT:      gc_toggle_checkbox(wizard, wizard->acceptCheck, &wizard->accepted); return 0;
                case GC_ID_START_MENU:  gc_toggle_checkbox(wizard, wizard->startMenuCheck, &wizard->startMenu); return 0;
                case GC_ID_DESKTOP:     gc_toggle_checkbox(wizard, wizard->desktopCheck, &wizard->desktop); return 0;
                case GC_ID_LAUNCH:      gc_toggle_checkbox(wizard, wizard->launchCheck, &wizard->launch); return 0;
                case GC_ID_BROWSE:      gc_browse_for_folder(wizard); return 0;
                case GC_ID_BACK:        gc_go_back(wizard); return 0;
                case GC_ID_NEXT:        gc_advance(wizard); return 0;
                case GC_ID_CANCEL:      SendMessageW(hwnd, WM_CLOSE, 0, 0); return 0;
                default: break;
            }
            break;
        }
        case GC_WM_PROGRESS:
            InvalidateRect(hwnd, nullptr, TRUE);
            return 0;
        case GC_WM_WORK_DONE:
            if (wizard->worker) {
                WaitForSingleObject(wizard->worker, INFINITE);
                CloseHandle(wizard->worker);
                wizard->worker = nullptr;
            }
            wizard->page = GC_PAGE_DONE;
            gc_update_page_controls(wizard);
            return 0;
        case WM_DPICHANGED: {
            // Rebuild the fonts for the new monitor before taking the size
            // Windows suggests, so the relayout that follows measures with the
            // metrics the window will actually paint with.
            wizard->dpi = HIWORD(wParam);
            gc_create_theme_fonts(&wizard->fonts, wizard->dpi);
            gc_apply_window_icons(wizard);
            gc_set_control_font(wizard->licenseEdit, wizard->fonts.monospace);
            gc_set_control_font(wizard->pathEdit, wizard->fonts.body);
            const RECT* suggested = (const RECT*)lParam;
            if (suggested) {
                SetWindowPos(hwnd, nullptr, suggested->left, suggested->top,
                             suggested->right - suggested->left, suggested->bottom - suggested->top,
                             SWP_NOZORDER | SWP_NOACTIVATE);
            }
            gc_update_page_controls(wizard);
            return 0;
        }
        case WM_SETTINGCHANGE:
            // Covers the user flipping Windows between light and dark while
            // setup is open.
            gc_apply_titlebar_theme(hwnd);
            return 0;
        case WM_SIZE:
            gc_layout(wizard);
            return 0;
        case WM_CLOSE:
            if (wizard->page == GC_PAGE_PROGRESS) return 0;
            wizard->exitCode = (wizard->page == GC_PAGE_DONE && wizard->workSucceeded) ? 0 : 2;
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        default:
            break;
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

static HWND gc_create_button(GcWizard* wizard, const WCHAR* text, int id) {
    HWND button = CreateWindowExW(0, L"BUTTON", text,
                                  WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                                  0, 0, 10, 10, wizard->hwnd, (HMENU)(INT_PTR)id,
                                  wizard->instance, nullptr);
    gc_set_control_font(button, wizard->fonts.body);
    return button;
}

static void gc_fill_license_text(GcWizard* wizard) {
    const GcPayloadFile* license = gc_payload_find(&wizard->install.payload, "LICENSE");
    if (!license || license->size == 0 || license->size > 512 * 1024) {
        gc_set_text_utf8(wizard->licenseEdit,
                         "MIT License\r\n\r\nThe LICENSE file could not be read from this setup file.");
        return;
    }
    // The edit control needs CRLF; the shipped file uses LF.
    size_t capacity = (size_t)license->size * 2 + 2;
    char* text = (char*)HeapAlloc(GetProcessHeap(), 0, capacity);
    if (!text) return;
    size_t out = 0;
    for (uint64_t i = 0; i < license->size; i++) {
        char c = (char)license->data[i];
        if (c == '\n' && (i == 0 || license->data[i - 1] != '\r')) text[out++] = '\r';
        text[out++] = c;
    }
    text[out] = 0;
    WCHAR* wide = (WCHAR*)HeapAlloc(GetProcessHeap(), 0, (out + 1) * sizeof(WCHAR));
    if (wide) {
        int written = MultiByteToWideChar(CP_UTF8, 0, text, (int)out, wide, (int)out);
        if (written > 0) {
            wide[written] = 0;
            SetWindowTextW(wizard->licenseEdit, wide);
        }
        HeapFree(GetProcessHeap(), 0, wide);
    }
    HeapFree(GetProcessHeap(), 0, text);
}

static bool gc_create_wizard_window(GcWizard* wizard, HINSTANCE instance, const WCHAR* title) {
    wizard->instance = instance;
    wizard->dpi = gc_dpi_for_window(nullptr);
    gc_create_theme_fonts(&wizard->fonts, wizard->dpi);
    wizard->backgroundBrush = CreateSolidBrush(COL_BG);
    wizard->inputBrush = CreateSolidBrush(COL_INPUT);
    InitializeCriticalSection(&wizard->progressLock);

    WNDCLASSEXW windowClass = {};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.lpfnWndProc = gc_wizard_proc;
    windowClass.hInstance = instance;
    windowClass.lpszClassName = GC_SETUP_WINDOW_CLASS;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hbrBackground = wizard->backgroundBrush;
    // Both slots: hIcon feeds Alt-Tab and the taskbar, hIconSm the caption bar.
    // Leaving hIconSm null makes Windows down-scale hIcon, which looks muddy at
    // 16px next to every other caption icon on the desktop.
    windowClass.hIcon = gc_load_setup_icon(instance, wizard->dpi, SM_CXICON);
    windowClass.hIconSm = gc_load_setup_icon(instance, wizard->dpi, SM_CXSMICON);
    if (!RegisterClassExW(&windowClass)) {
        gc_log_fail("ui: RegisterClassEx failed (error %lu)", GetLastError());
        return false;
    }

    RECT desired = {0, 0, gc_scaled(wizard->dpi, GC_WINDOW_WIDTH), gc_scaled(wizard->dpi, GC_WINDOW_HEIGHT)};
    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    AdjustWindowRectEx(&desired, style, FALSE, 0);
    int width = desired.right - desired.left;
    int height = desired.bottom - desired.top;
    int left = (GetSystemMetrics(SM_CXSCREEN) - width) / 2;
    int top = (GetSystemMetrics(SM_CYSCREEN) - height) / 2;

    wizard->hwnd = CreateWindowExW(0, GC_SETUP_WINDOW_CLASS, title, style,
                                   left, top, width, height, nullptr, nullptr, instance, nullptr);
    if (!wizard->hwnd) {
        gc_log_fail("ui: CreateWindowEx failed (error %lu)", GetLastError());
        return false;
    }
    // The DPI of the monitor the window actually landed on can differ from the
    // primary monitor's; re-derive before anything is measured.
    UINT actualDpi = gc_dpi_for_window(wizard->hwnd);
    if (actualDpi != wizard->dpi) {
        wizard->dpi = actualDpi;
        gc_create_theme_fonts(&wizard->fonts, wizard->dpi);
    }
    // After the DPI is final, so the caption icon is sized for the monitor the
    // window actually landed on rather than the primary one.
    gc_apply_window_icons(wizard);
    gc_apply_titlebar_theme(wizard->hwnd);
    gc_enable_dark_controls(wizard->hwnd);

    wizard->backButton = gc_create_button(wizard, L"Back", GC_ID_BACK);
    wizard->nextButton = gc_create_button(wizard, L"Next", GC_ID_NEXT);
    wizard->cancelButton = gc_create_button(wizard, L"Cancel", GC_ID_CANCEL);
    return true;
}

static void gc_destroy_wizard(GcWizard* wizard) {
    gc_destroy_theme_fonts(&wizard->fonts);
    if (wizard->backgroundBrush) DeleteObject(wizard->backgroundBrush);
    if (wizard->inputBrush) DeleteObject(wizard->inputBrush);
    DeleteCriticalSection(&wizard->progressLock);
}

static int gc_message_loop(GcWizard* wizard) {
    ShowWindow(wizard->hwnd, SW_SHOW);
    UpdateWindow(wizard->hwnd);
    MSG message = {};
    while (GetMessageW(&message, nullptr, 0, 0)) {
        if (!IsDialogMessageW(wizard->hwnd, &message)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }
    return wizard->exitCode;
}

int gc_run_setup_wizard(HINSTANCE instance, const GcInstallerOptions* options,
                        const GcPriorInstall* prior, const char* defaultDirectory) {
    GcWizard* wizard = &g_wizard;
    wizard->options = *options;
    wizard->prior = *prior;
    StringCchCopyA(wizard->defaultDirectory, GC_ARRAY_COUNT(wizard->defaultDirectory), defaultDirectory);
    wizard->exitCode = 2;

    WCHAR title[128] = {};
    StringCchPrintfW(title, GC_ARRAY_COUNT(title), L"%ls %hs Setup", GC_SETUP_PRODUCT_NAME_W, APP_VERSION);
    if (!gc_create_wizard_window(wizard, instance, title)) return 1;

    WCHAR modulePath[GC_INSTALLER_MAX_PATH_CHARS] = {};
    if (!gc_module_path(modulePath, GC_ARRAY_COUNT(modulePath)) ||
        !gc_payload_load(modulePath, &wizard->install.payload)) {
        gc_show_message(wizard->hwnd,
                        "This setup file does not contain the Green Curve program files. "
                        "Download it again.", "Green Curve Setup", true);
        gc_destroy_wizard(wizard);
        return 1;
    }
    wizard->install.progress = gc_progress_callback;
    wizard->install.progressContext = wizard;

    // Seed the plan so the folder page opens on the directory that would be
    // used if the user pressed Install immediately.
    gc_install_build_plan(&wizard->options, &wizard->prior, wizard->defaultDirectory,
                          &wizard->install.plan);

    wizard->licenseEdit = CreateWindowExW(0, L"EDIT", L"",
                                          WS_CHILD | WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
                                          0, 0, 10, 10, wizard->hwnd, (HMENU)(INT_PTR)GC_ID_LICENSE_EDIT,
                                          instance, nullptr);
    gc_set_control_font(wizard->licenseEdit, wizard->fonts.monospace);
    gc_enable_dark_controls(wizard->licenseEdit);
    gc_fill_license_text(wizard);

    wizard->acceptCheck = CreateWindowExW(0, L"BUTTON", L"I accept the terms of the MIT license",
                                          WS_CHILD | WS_TABSTOP | BS_OWNERDRAW,
                                          0, 0, 10, 10, wizard->hwnd, (HMENU)(INT_PTR)GC_ID_ACCEPT,
                                          instance, nullptr);
    gc_set_control_font(wizard->acceptCheck, wizard->fonts.body);

    wizard->pathEdit = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_TABSTOP | ES_AUTOHSCROLL,
                                       0, 0, 10, 10, wizard->hwnd, (HMENU)(INT_PTR)GC_ID_PATH_EDIT,
                                       instance, nullptr);
    gc_set_control_font(wizard->pathEdit, wizard->fonts.body);
    gc_set_text_utf8(wizard->pathEdit, wizard->install.plan.targetDirectory);
    wizard->browseButton = gc_create_button(wizard, L"Browse...", GC_ID_BROWSE);

    wizard->startMenuCheck = CreateWindowExW(0, L"BUTTON", L"Create a Start menu shortcut",
                                             WS_CHILD | WS_TABSTOP | BS_OWNERDRAW,
                                             0, 0, 10, 10, wizard->hwnd, (HMENU)(INT_PTR)GC_ID_START_MENU,
                                             instance, nullptr);
    wizard->desktopCheck = CreateWindowExW(0, L"BUTTON", L"Create a desktop shortcut",
                                           WS_CHILD | WS_TABSTOP | BS_OWNERDRAW,
                                           0, 0, 10, 10, wizard->hwnd, (HMENU)(INT_PTR)GC_ID_DESKTOP,
                                           instance, nullptr);
    wizard->launchCheck = CreateWindowExW(0, L"BUTTON", L"Start Green Curve when setup finishes",
                                          WS_CHILD | WS_TABSTOP | BS_OWNERDRAW,
                                          0, 0, 10, 10, wizard->hwnd, (HMENU)(INT_PTR)GC_ID_LAUNCH,
                                          instance, nullptr);
    gc_set_control_font(wizard->startMenuCheck, wizard->fonts.body);
    gc_set_control_font(wizard->desktopCheck, wizard->fonts.body);
    gc_set_control_font(wizard->launchCheck, wizard->fonts.body);
    wizard->startMenu = wizard->install.plan.createStartMenuShortcut;
    wizard->desktop = wizard->install.plan.createDesktopShortcut;
    wizard->launch = wizard->install.plan.launchAfterInstall;

    wizard->page = GC_PAGE_LICENSE;
    gc_update_page_controls(wizard);
    int exitCode = gc_message_loop(wizard);
    gc_payload_release(&wizard->install.payload);
    gc_destroy_wizard(wizard);
    return exitCode;
}

int gc_run_uninstall_window(HINSTANCE instance, const WCHAR* installDirectory) {
    GcWizard* wizard = &g_wizard;
    wizard->uninstallMode = true;
    wizard->exitCode = 2;
    StringCchCopyW(wizard->uninstallDirectory, GC_ARRAY_COUNT(wizard->uninstallDirectory), installDirectory);

    WCHAR title[128] = {};
    StringCchPrintfW(title, GC_ARRAY_COUNT(title), L"Uninstall %ls", GC_SETUP_PRODUCT_NAME_W);
    if (!gc_create_wizard_window(wizard, instance, title)) return 1;
    wizard->page = GC_PAGE_CONFIRM_REMOVE;
    gc_update_page_controls(wizard);
    int exitCode = gc_message_loop(wizard);
    gc_destroy_wizard(wizard);
    return exitCode;
}
