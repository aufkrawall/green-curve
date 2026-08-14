// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// The Updates dialog: status, Check now, Install, and the automatic-check
// preference.
//
// ## Why a dialog and not more main-window chrome
//
// The main window is already dense, and updates are something a user thinks
// about rarely and deliberately.  One button opens this; everything the feature
// has to say lives here.  That mirrors how the fan curve and auto-profile
// editors are reached, so there is one idiom rather than a new one.
//
// ## Consent, expressed in the UI
//
// The automatic-check setting starts UNSET rather than off-or-on, and that
// distinction is visible here: until the user answers, the dialog says so and
// the service makes no outbound request.  A background check reveals an IP, a
// version and an architecture on a timer, which is a disclosure worth asking
// about once rather than defaulting into silently.
//
// Installing is always a button press.  There is no "install automatically"
// checkbox to add later without revisiting this comment: the service has no
// code path that reaches an install from a timer, by construction.

// ---------------------------------------------------------------------------
// Sending
//
// GUI-only: the service binary compiles the shared cache in
// gui_update_client.cpp but never asks anything of itself, and -Werror turns an
// unused static into a build failure.
// ---------------------------------------------------------------------------

static bool gui_update_is_busy() {
    const ServiceUpdateState* state = gui_update_state();
    if (!state) return false;
    // `workerRunning` first, and it is the authoritative half.  The service sets
    // it before creating the worker thread, so it is already true in the reply
    // to the command that started the job -- whereas `phase` may still say IDLE
    // at that moment.  Polling driven by phase alone stops immediately after
    // Install and the user sees nothing happen.
    if (state->workerRunning) return true;
    switch (state->phase) {
        case SERVICE_UPDATE_PHASE_CHECKING:
        case SERVICE_UPDATE_PHASE_DOWNLOADING:
        case SERVICE_UPDATE_PHASE_VERIFYING:
        case SERVICE_UPDATE_PHASE_INSTALLING:
            return true;
        default:
            return false;
    }
}

// The one line shown on the dialog.
static void gui_update_status_text(char* out, size_t outSize) {
    if (!out || outSize == 0) return;
    out[0] = 0;
    const ServiceUpdateState* state = gui_update_state();
    if (!state) {
        StringCchCopyA(out, outSize, "Update status is not available yet.");
        return;
    }
    switch (state->phase) {
        case SERVICE_UPDATE_PHASE_CHECKING:
            StringCchCopyA(out, outSize, "Checking for updates...");
            return;
        case SERVICE_UPDATE_PHASE_DOWNLOADING:
            StringCchPrintfA(out, outSize, "Downloading %s...",
                             state->availableVersion[0] ? state->availableVersion : "update");
            return;
        case SERVICE_UPDATE_PHASE_VERIFYING:
            StringCchCopyA(out, outSize, "Verifying the downloaded package...");
            return;
        case SERVICE_UPDATE_PHASE_INSTALLING:
            StringCchCopyA(out, outSize, "Installing... Green Curve will restart.");
            return;
        case SERVICE_UPDATE_PHASE_READY:
            StringCchPrintfA(out, outSize,
                             "Version %s is downloaded and verified, ready to install.",
                             state->availableVersion);
            return;
        case SERVICE_UPDATE_PHASE_FAILED:
            // The service's detail is shown verbatim.  It never contains an
            // attacker-chosen string: a refused redirect names the hop, not the
            // URL it was pointed at.
            StringCchPrintfA(out, outSize, "Update failed: %s",
                             state->detail[0] ? state->detail : "unknown error");
            return;
        default:
            break;
    }
    switch (state->decision) {
        case GC_UPDATE_DECISION_AVAILABLE:
            StringCchPrintfA(out, outSize, "Version %s is available (you have %s).",
                             state->availableVersion, state->installedVersion);
            break;
        case GC_UPDATE_DECISION_MANUAL_REQUIRED:
            StringCchPrintfA(out, outSize,
                             "Version %s must be installed manually from the "
                             "releases page.", state->availableVersion);
            break;
        case GC_UPDATE_DECISION_NO_ASSET:
            StringCchCopyA(out, outSize,
                           "The latest release has no build for this machine.");
            break;
        case GC_UPDATE_DECISION_UP_TO_DATE:
            StringCchPrintfA(out, outSize, "Green Curve %s is up to date.",
                             state->installedVersion);
            break;
        default:
            if (state->detail[0]) {
                StringCchCopyA(out, outSize, state->detail);
            } else if (state->lastCheckUnix == 0) {
                StringCchCopyA(out, outSize, "Green Curve has not checked for updates yet.");
            } else {
                StringCchCopyA(out, outSize, "No update information available.");
            }
            break;
    }
}

// The shared sender.  `autoCheck` and `intervalSeconds` are meaningful only for
// SERVICE_CMD_SET_UPDATE_POLICY and are sent only for it -- the service's
// validator refuses them on the other three, so passing them by accident fails
// loudly rather than being quietly ignored.
static bool gui_update_send(ServiceCommand command, gc_u32 autoCheck,
                            gc_u32 intervalSeconds, char* err, size_t errSize) {
    ServiceRequest request = {};
    request.magic = SERVICE_PROTOCOL_MAGIC;
    request.version = SERVICE_PROTOCOL_VERSION;
    request.command = (gc_u32)command;
    request.callerPid = GetCurrentProcessId();
    ProcessIdToSessionId(request.callerPid, &request.callerSessionId);
    if (command == SERVICE_CMD_SET_UPDATE_POLICY) {
        request.updateAutoCheck = autoCheck;
        request.updateIntervalSeconds = intervalSeconds;
    }
    StringCchCopyA(request.source, ARRAY_COUNT(request.source), "gui update");

    ServiceResponse response = {};
    if (!service_send_request(&request, &response, 5000, err, errSize)) {
        debug_log("gui update: command %u transport failed: %s\n",
                  (unsigned)command, err && err[0] ? err : "unknown");
        return false;
    }
    if (response.status != SERVICE_STATUS_OK) {
        set_message(err, errSize, "%s",
                    response.message[0] ? response.message : "Update request failed");
        return false;
    }
    return true;
}

// Whether a fullscreen or presentation-mode application owns the foreground.
//
// This lives in the GUI because the GUI is the only Green Curve process in the
// user's session.  The service runs in session 0, which has no interactive
// desktop, so the same call there describes nothing the user can see -- and
// acting on it refused the first live install on a completely idle machine.
// See service_update_foreground_app_active() for that story.
//
// Courtesy check, not a security one: installing stops the service, which
// returns the GPU to stock for a few seconds, and doing that under a running
// game is rude.  Being client-side is therefore fine -- a client can only make
// itself more restrictive, and the gate that actually protects the hardware
// (a hardware apply in flight) stays in the service where it is answerable.
static bool gui_update_foreground_app_active() {
    QUERY_USER_NOTIFICATION_STATE state = (QUERY_USER_NOTIFICATION_STATE)0;
    if (FAILED(SHQueryUserNotificationState(&state))) return false;
    return state == QUNS_RUNNING_D3D_FULL_SCREEN ||
           state == QUNS_PRESENTATION_MODE ||
           state == QUNS_BUSY;
}

static bool gui_update_request_state(char* err, size_t errSize) {
    return gui_update_send(SERVICE_CMD_GET_UPDATE_STATE, 0, 0, err, errSize);
}

static bool gui_update_request_check(char* err, size_t errSize) {
    debug_log("gui update: user requested a check\n");
    return gui_update_send(SERVICE_CMD_CHECK_FOR_UPDATE, 0, 0, err, errSize);
}

// The user clicked Install.  This is the only thing in the GUI that can lead to
// an installer running, and it still cannot choose WHAT runs: the service
// installs the package it already downloaded and verified against a signed
// manifest, or it refuses.
static bool gui_update_request_install(char* err, size_t errSize) {
    debug_log("gui update: user requested an install\n");
    return gui_update_send(SERVICE_CMD_INSTALL_UPDATE, 0, 0, err, errSize);
}

static bool gui_update_set_policy(GcUpdateAutoCheck autoCheck, int intervalSeconds,
                                  char* err, size_t errSize) {
    // Clamped here as well as in the service.  The service REFUSES an
    // out-of-range interval rather than clamping it -- a request is
    // machine-written, so a bad one means the client is confused -- which makes
    // it this side's job to send something sane.
    int clamped = gc_update_clamp_interval(intervalSeconds);
    debug_log("gui update: setting policy autoCheck=%d interval=%ds\n",
              (int)autoCheck, clamped);
    return gui_update_send(SERVICE_CMD_SET_UPDATE_POLICY, (gc_u32)autoCheck,
                           (gc_u32)clamped, err, errSize);
}

#define GUD_CHECK_BTN_ID     2400
#define GUD_INSTALL_BTN_ID   2401
#define GUD_CLOSE_BTN_ID     2402
#define GUD_AUTO_CHECK_ID    2403
#define GUD_RELEASES_BTN_ID  2404
#define GUD_REFRESH_TIMER_ID 1

// The dialog polls while the service is working.  A poll is a local named-pipe
// round trip to a service that is already answering the main window's polling,
// not a network request, so the rate is a UI-responsiveness choice and nothing
// more.
#define GUD_REFRESH_INTERVAL_MS 700

struct GuiUpdateDialogState {
    HWND hwnd;
    HWND statusLabel;
    HWND detailLabel;
    HWND autoCheck;
    HWND checkButton;
    HWND installButton;
    HWND releasesButton;
    HWND closeButton;
    HBRUSH hStaticBrush;
    HBRUSH hBtnBrush;
    UiCheckboxState autoCheckState;
    bool pollingActive;
};

static GuiUpdateDialogState g_updateDialog = {};

static bool gud_check_get() { return g_updateDialog.autoCheckState.checked; }

static void gud_check_set(bool on) {
    g_updateDialog.autoCheckState.checked = on;
    if (g_updateDialog.autoCheck) InvalidateRect(g_updateDialog.autoCheck, nullptr, TRUE);
}

static void gud_set_text(HWND control, const char* text) {
    if (!control) return;
    char existing[512] = {};
    GetWindowTextA(control, existing, ARRAY_COUNT(existing));
    // Only write when the text actually changed.  This runs on a timer, and
    // SetWindowText invalidates unconditionally -- repainting an unchanged
    // label several times a second is a visible flicker on a themed static.
    if (strcmp(existing, text ? text : "") == 0) return;
    SetWindowTextA(control, text ? text : "");
}

// Project the cached service state onto every control.  Called after each
// command and on every poll tick, so there is exactly one function that decides
// what the dialog looks like for a given state.
static void gud_refresh_controls() {
    if (!g_updateDialog.hwnd) return;
    const ServiceUpdateState* state = gui_update_state();

    char status[512] = {};
    gui_update_status_text(status, sizeof(status));
    gud_set_text(g_updateDialog.statusLabel, status);

    char detail[512] = {};
    if (state && !state->isInstalledCopy) {
        // A portable copy has no installer to upgrade and no recorded install
        // directory.  Saying so is better than greying a button with no reason.
        StringCchCopyA(detail, sizeof(detail),
            "This is a portable copy, so updates cannot be installed "
            "automatically. Use the releases page to download a new version.");
    } else if (state && state->autoCheck == GC_UPDATE_AUTO_CHECK_UNSET) {
        StringCchCopyA(detail, sizeof(detail),
            "Automatic checking is not enabled yet. Checking contacts GitHub "
            "and reveals your IP address, version and architecture.");
    } else if (state && state->consecutiveFailures > 0 &&
               state->phase != SERVICE_UPDATE_PHASE_READY) {
        StringCchPrintfA(detail, sizeof(detail),
            "The last %u check(s) failed; Green Curve will retry less often "
            "until one succeeds.", (unsigned)state->consecutiveFailures);
    } else if (state && state->phase == SERVICE_UPDATE_PHASE_READY) {
        StringCchCopyA(detail, sizeof(detail),
            "Installing stops Green Curve briefly and restores your settings "
            "afterwards. Do not install while gaming.");
    } else if (state && state->decision == GC_UPDATE_DECISION_AVAILABLE &&
               !state->packageStaged) {
        // An update is known but nothing is staged, so Install is greyed.  Say
        // why: the first live run left a user looking at a disabled button with
        // no explanation, which is a worse failure than an error message.
        StringCchCopyA(detail, sizeof(detail),
            "The update has not been downloaded yet. Use Check now to download "
            "it, then Install update.");
    }
    gud_set_text(g_updateDialog.detailLabel, detail);

    bool busy = gui_update_is_busy();
    bool ready = state && state->phase == SERVICE_UPDATE_PHASE_READY &&
                 state->packageVerified && state->isInstalledCopy;
    EnableWindow(g_updateDialog.checkButton, !busy);
    EnableWindow(g_updateDialog.installButton, ready && !busy);

    // Poll only while something is actually happening; an idle dialog costs
    // nothing.
    if (busy && !g_updateDialog.pollingActive) {
        SetTimer(g_updateDialog.hwnd, GUD_REFRESH_TIMER_ID, GUD_REFRESH_INTERVAL_MS, nullptr);
        g_updateDialog.pollingActive = true;
    } else if (!busy && g_updateDialog.pollingActive) {
        KillTimer(g_updateDialog.hwnd, GUD_REFRESH_TIMER_ID);
        g_updateDialog.pollingActive = false;
    }
}

static void gud_report_error(HWND hwnd, const char* action, const char* err) {
    char message[640] = {};
    StringCchPrintfA(message, sizeof(message), "%s failed:\n\n%s",
                     action, err && err[0] ? err : "unknown error");
    debug_log("gui update dialog: %s failed: %s\n", action,
              err && err[0] ? err : "unknown");
    gc_message_box(hwnd, message, "Green Curve", MB_OK | MB_ICONWARNING);
}

static void gud_apply_auto_check(HWND hwnd) {
    const ServiceUpdateState* state = gui_update_state();
    int interval = state && state->intervalSeconds
                       ? (int)state->intervalSeconds
                       : GC_UPDATE_INTERVAL_DEFAULT_SECONDS;
    GcUpdateAutoCheck wanted = gud_check_get() ? GC_UPDATE_AUTO_CHECK_ON
                                               : GC_UPDATE_AUTO_CHECK_OFF;
    char err[256] = {};
    if (!gui_update_set_policy(wanted, interval, err, sizeof(err))) {
        gud_report_error(hwnd, "Saving the update preference", err);
        // Put the checkbox back: showing it ticked when the service did not
        // record it would be a lie the next restart silently corrects.
        gud_check_set(!gud_check_get());
    }
    gud_refresh_controls();
}

static void gud_open_releases_page() {
    // The releases page, built from the same compiled-in owner/repo constants
    // the updater itself uses, so there is one source of truth for where Green
    // Curve lives and no configurable URL to redirect.
    char url[GC_UPDATE_URL_MAX_CHARS] = {};
    if (FAILED(StringCchPrintfA(url, sizeof(url),
                                "https://github.com/%s/%s/releases/latest",
                                GC_UPDATE_REPO_OWNER, GC_UPDATE_REPO_NAME))) {
        return;
    }
    // Re-validated through the same allowlist the downloader uses.  This opens
    // the user's browser rather than fetching anything, but a URL that would be
    // refused for a download has no business being handed to a shell either.
    if (!gc_update_url_is_acceptable(url)) {
        debug_log("gui update dialog: refusing to open a non-allowlisted URL\n");
        return;
    }
    debug_log("gui update dialog: opening the releases page\n");
    ShellExecuteA(nullptr, "open", url, nullptr, nullptr, SW_SHOWNORMAL);
}

static HWND gud_label(HWND hwnd, const char* text, int x, int y, int w, int h) {
    return CreateWindowExA(0, "STATIC", text, WS_CHILD | WS_VISIBLE | SS_LEFT,
        dp(x), dp(y), dp(w), dp(h), hwnd, nullptr, g_app.hInst, nullptr);
}

static void gud_create_controls(HWND hwnd) {
    apply_system_titlebar_theme(hwnd);
    allow_dark_mode_for_window(hwnd);

    g_updateDialog.statusLabel = gud_label(hwnd, "", 16, 16, 448, 36);
    g_updateDialog.detailLabel = gud_label(hwnd, "", 16, 56, 448, 52);

    g_updateDialog.autoCheck = CreateWindowExA(0, "BUTTON",
        "Check for updates automatically",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
        dp(16), dp(116), dp(260), dp(20), hwnd,
        (HMENU)(INT_PTR)GUD_AUTO_CHECK_ID, g_app.hInst, nullptr);

    const int btnTop = 152;
    g_updateDialog.checkButton = CreateWindowExA(0, "BUTTON", "Check now",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
        dp(16), dp(btnTop), dp(104), dp(28), hwnd,
        (HMENU)(INT_PTR)GUD_CHECK_BTN_ID, g_app.hInst, nullptr);
    g_updateDialog.installButton = CreateWindowExA(0, "BUTTON", "Install update",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
        dp(128), dp(btnTop), dp(120), dp(28), hwnd,
        (HMENU)(INT_PTR)GUD_INSTALL_BTN_ID, g_app.hInst, nullptr);
    g_updateDialog.releasesButton = CreateWindowExA(0, "BUTTON", "Releases page",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
        dp(256), dp(btnTop), dp(120), dp(28), hwnd,
        (HMENU)(INT_PTR)GUD_RELEASES_BTN_ID, g_app.hInst, nullptr);
    g_updateDialog.closeButton = CreateWindowExA(0, "BUTTON", "Close",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
        dp(384), dp(btnTop), dp(80), dp(28), hwnd,
        (HMENU)(INT_PTR)GUD_CLOSE_BTN_ID, g_app.hInst, nullptr);

    apply_ui_font_to_children(hwnd);
    fit_themed_checkbox_to_label(g_updateDialog.autoCheck);

    const ServiceUpdateState* state = gui_update_state();
    gud_check_set(state && state->autoCheck == GC_UPDATE_AUTO_CHECK_ON);
    gud_refresh_controls();
}

static LRESULT CALLBACK GuiUpdateDialogProc(HWND hwnd, UINT msg,
                                            WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_CREATE:
            gud_create_controls(hwnd);
            return 0;

        case WM_THEMECHANGED:
        case WM_SETTINGCHANGE:
            apply_system_titlebar_theme(hwnd);
            allow_dark_mode_for_window(hwnd);
            break;

        case WM_TIMER:
            if (wParam == GUD_REFRESH_TIMER_ID) {
                // A poll failure is not reported: the service may legitimately
                // be stopping (an install does exactly that), and a dialog that
                // threw a message box every 700 ms during its own update would
                // be worse than one that just stops refreshing.
                char err[256] = {};
                (void)gui_update_request_state(err, sizeof(err));
                gud_refresh_controls();
                return 0;
            }
            break;

        case WM_DRAWITEM: {
            const DRAWITEMSTRUCT* dis = (const DRAWITEMSTRUCT*)lParam;
            if (dis && dis->CtlType == ODT_BUTTON) {
                if ((int)dis->CtlID == GUD_AUTO_CHECK_ID) {
                    draw_themed_checkbox_control(dis, gud_check_get(), true);
                    return TRUE;
                }
                draw_themed_button(dis);
                return TRUE;
            }
            return FALSE;
        }

        case WM_COMMAND: {
            int id = LOWORD(wParam);
            int note = HIWORD(wParam);
            if (note != BN_CLICKED) break;
            if (id == GUD_AUTO_CHECK_ID) {
                gud_check_set(!gud_check_get());
                gud_apply_auto_check(hwnd);
                return 0;
            }
            if (id == GUD_CHECK_BTN_ID) {
                char err[256] = {};
                if (!gui_update_request_check(err, sizeof(err))) {
                    gud_report_error(hwnd, "Checking for updates", err);
                }
                gud_refresh_controls();
                return 0;
            }
            if (id == GUD_INSTALL_BTN_ID) {
                // The confirmation states the two consequences that actually
                // surprise people: the app disappears for a moment, and the GPU
                // returns to stock while the service is stopped.
                const char* prompt =
                    "Install the update now?\n\n"
                    "Green Curve will close, the background service will "
                    "restart, and your settings will be restored afterwards. "
                    "Your GPU returns to stock settings for a few seconds "
                    "during the update.";
                // The service cannot see the user's session, so this check
                // lives here.  It warns rather than refuses: the user asked,
                // and a courtesy check is not something to hard-block on.
                if (gui_update_foreground_app_active()) {
                    prompt =
                        "A fullscreen application appears to be running.\n\n"
                        "Installing now stops the background service, which "
                        "returns your GPU to stock settings for a few seconds. "
                        "Install anyway?";
                }
                if (gc_message_box(hwnd, prompt, "Green Curve",
                                   MB_YESNO | MB_ICONQUESTION) != IDYES) {
                    return 0;
                }
                // Capture the applied settings BEFORE the install is
                // requested.  Setup's own capture cannot work here: the
                // updater launches it in session 0, where the service refuses
                // its helpers.  This process is in the authorized session and
                // already connected, so it does the job itself.  Failure is
                // ordinary -- it means nothing is applied -- and never blocks
                // the update.
                (void)gui_update_capture_settings_for_restore();

                char err[256] = {};
                if (!gui_update_request_install(err, sizeof(err))) {
                    gud_report_error(hwnd, "Installing the update", err);
                }
                gud_refresh_controls();
                return 0;
            }
            if (id == GUD_RELEASES_BTN_ID) {
                gud_open_releases_page();
                return 0;
            }
            if (id == GUD_CLOSE_BTN_ID) {
                DestroyWindow(hwnd);
                return 0;
            }
            break;
        }

        case WM_ERASEBKGND: {
            RECT rc = {};
            GetClientRect(hwnd, &rc);
            HBRUSH bg = CreateSolidBrush(COL_BG);
            FillRect((HDC)wParam, &rc, bg);
            DeleteObject(bg);
            return 1;
        }

        case WM_CTLCOLORBTN: {
            SetTextColor((HDC)wParam, COL_TEXT);
            SetBkColor((HDC)wParam, COL_BG);
            if (!g_updateDialog.hBtnBrush) g_updateDialog.hBtnBrush = CreateSolidBrush(COL_BG);
            return (LRESULT)g_updateDialog.hBtnBrush;
        }

        case WM_CTLCOLORSTATIC: {
            SetTextColor((HDC)wParam, COL_LABEL);
            SetBkColor((HDC)wParam, COL_BG);
            if (!g_updateDialog.hStaticBrush) {
                g_updateDialog.hStaticBrush = CreateSolidBrush(COL_BG);
            }
            return (LRESULT)g_updateDialog.hStaticBrush;
        }

        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;

        case WM_DESTROY:
            if (g_updateDialog.pollingActive) {
                KillTimer(hwnd, GUD_REFRESH_TIMER_ID);
            }
            if (g_updateDialog.hStaticBrush) {
                DeleteObject(g_updateDialog.hStaticBrush);
                g_updateDialog.hStaticBrush = nullptr;
            }
            if (g_updateDialog.hBtnBrush) {
                DeleteObject(g_updateDialog.hBtnBrush);
                g_updateDialog.hBtnBrush = nullptr;
            }
            if (g_app.hMainWnd) {
                EnableWindow(g_app.hMainWnd, TRUE);
                SetForegroundWindow(g_app.hMainWnd);
            }
            memset(&g_updateDialog, 0, sizeof(g_updateDialog));
            return 0;

        default:
            // WM_* is an unbounded UINT, not an enum; everything this dialog
            // does not handle falls through to DefWindowProc below.
            break;
    }
    return DefWindowProcA(hwnd, msg, wParam, lParam);
}

void gui_update_open_dialog(HWND parent) {
    if (g_updateDialog.hwnd) {
        ShowWindow(g_updateDialog.hwnd, SW_SHOW);
        SetForegroundWindow(g_updateDialog.hwnd);
        return;
    }

    // Fetch current state before the window exists, so it opens already
    // populated rather than blank-then-filled.  A failure here is not fatal:
    // the dialog says the status is unavailable and Check now still works.
    char err[256] = {};
    if (!gui_update_request_state(err, sizeof(err))) {
        debug_log("gui update dialog: initial state fetch failed: %s\n",
                  err[0] ? err : "unknown");
    }

    WNDCLASSEXA wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = GuiUpdateDialogProc;
    wc.hInstance = g_app.hInst;
    wc.lpszClassName = "GreenCurveUpdateDialog";
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = g_app.hWindowClassBrush;
    wc.hIcon = (HICON)SendMessageA(parent ? parent : g_app.hMainWnd,
                                   WM_GETICON, ICON_SMALL, 0);
    WNDCLASSEXA existing = {};
    if (!GetClassInfoExA(g_app.hInst, wc.lpszClassName, &existing)) {
        RegisterClassExA(&wc);
    }

    SIZE size = adjusted_window_size_for_client(dp(480), dp(196),
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, WS_EX_DLGMODALFRAME);

    HWND owner = parent ? parent : g_app.hMainWnd;
    RECT ownerRect = {};
    if (owner) GetWindowRect(owner, &ownerRect);
    RECT work = owner ? main_window_work_area(owner)
                      : main_window_work_area_for_rect(nullptr);
    MainLayoutRect anchor = main_layout_rect_from_win32(owner ? ownerRect : work);
    RECT centered = main_layout_rect_to_win32(
        main_layout_center_rect(anchor, size.cx, size.cy));
    RECT target = clamp_window_rect_to_work_area(centered, work);

    g_updateDialog.hwnd = CreateWindowExA(WS_EX_DLGMODALFRAME, wc.lpszClassName,
        "Green Curve Updates",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
        target.left, target.top, target.right - target.left,
        target.bottom - target.top, owner, nullptr, g_app.hInst, nullptr);
    if (!g_updateDialog.hwnd) return;

    if (owner) EnableWindow(owner, FALSE);
    ShowWindow(g_updateDialog.hwnd, SW_SHOW);
    UpdateWindow(g_updateDialog.hwnd);
}
