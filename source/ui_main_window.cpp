// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT

#include "auto_profile.h"   // auto-profile driver API used by the WndProc
#include "profile_save_policy.h"

// ============================================================================
// Main Window
// ============================================================================

// Apply / Refresh / Reset and the high-overclock confirmation.  Also pulls in
// ui_mutation_completion.cpp and ui_main_control_lifecycle.cpp at the points
// they were included from here.
#include "ui_main_apply.cpp"

// WM_CTLCOLOR* palette handling, including the F-PENDING field colouring.
#include "ui_main_ctlcolor.cpp"

// The lock / machine-logon / shared-profile right-click menus, and the elevated
// helper the logon one runs.  Included here because that is where they were
// defined before the split, so the amalgamated ordering is byte-identical.
#include "ui_main_context_menus.cpp"

// ============================================================================
// Main Window
// ============================================================================

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    // Display-driver reconstruction can restore WS_VISIBLE without issuing a
    // WINDOWPOS carrying SWP_SHOWWINDOW.  Check the durable postcondition on
    // every main-window message so the first notification from any such path
    // synchronously restores tray residency.  Explicit user activation clears
    // the intent before ShowWindow and therefore bypasses this invariant.
    enforce_main_window_tray_state_from_message(hwnd, msg);
    switch (msg) {
        case WM_CREATE:
            reset_main_window_dpi_resources(hwnd, main_layout_window_dpi(hwnd));
            create_backbuffer(hwnd);
            if (!g_app.hCachedGridPen) g_app.hCachedGridPen = CreatePen(PS_SOLID, 1, COL_GRID);
            if (!g_app.hCachedAxisPen) g_app.hCachedAxisPen = CreatePen(PS_SOLID, 1, COL_AXIS);
            if (!g_app.hCachedFont) g_app.hCachedFont = CreateFontA(dp(13), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, "Segoe UI");
            if (!g_app.hCachedFontSmall) g_app.hCachedFontSmall = CreateFontA(dp(11), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, "Segoe UI");
            apply_system_titlebar_theme(hwnd);
            allow_dark_mode_for_window(hwnd);
            update_fan_telemetry_timer();
            ensure_main_window_fits_work_area(hwnd);
            layout_bottom_buttons(hwnd);
            auto_profile_init(hwnd);
            return 0;

        default:
            if (g_taskbarCreatedMessage != 0 && msg == g_taskbarCreatedMessage) {
                g_app.trayIconAdded = false;
                g_app.trayUsesNotificationVersion4 = false;
                g_app.trayLastRenderedValid = false;
                // Unconditionally, because startup adds the icon
                // unconditionally (entry.cpp) -- gating this on a hidden window
                // meant an Explorer restart with the window OPEN silently left
                // the app with no tray icon until the user next minimized.
                // That is not cosmetic: the tray menu is the only surface that
                // can offer an available update from outside the window, and
                // the icon is how a tray-resident instance is reached at all.
                ensure_tray_icon();
                return 0;
            }
            break;

        case WM_SIZE: {
            if (wParam == SIZE_MINIMIZED) {
                hide_main_window_to_tray();
                return 0;
            }
            destroy_backbuffer();
            create_backbuffer(hwnd);
            layout_main_window(hwnd);
            if (wParam == SIZE_RESTORED) main_layout_apply_pending_content_growth(hwnd);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }

        case WM_WINDOWPOSCHANGING: {
            WINDOWPOS* position = reinterpret_cast<WINDOWPOS*>(lParam);
            bool showRequested = position &&
                (position->flags & SWP_SHOWWINDOW) != 0;
            if (position && gui_tray_hidden_intent_blocks_show(
                    g_app.trayWindowHiddenIntent, showRequested)) {
                position->flags &= ~SWP_SHOWWINDOW;
                position->flags |= SWP_HIDEWINDOW | SWP_NOACTIVATE;
                debug_log_on_change(
                    "tray window: blocked unsolicited top-level show while hidden intent is active\n");
            }
            break;
        }

        case WM_DPICHANGED: {
            const RECT* suggested = (const RECT*)lParam;
            int newDpi = (int)HIWORD(wParam);
            main_layout_handle_dpi_changed(hwnd, newDpi, suggested);
            return 0;
        }

        case WM_VSCROLL:
            if (main_layout_handle_scroll(hwnd, SB_VERT, LOWORD(wParam))) return 0;
            break;

        case WM_HSCROLL:
            if (main_layout_handle_scroll(hwnd, SB_HORZ, LOWORD(wParam))) return 0;
            break;

        case WM_MOUSEWHEEL:
            if (main_layout_handle_mouse_wheel(hwnd, GET_WHEEL_DELTA_WPARAM(wParam),
                    (GetKeyState(VK_SHIFT) & 0x8000) != 0)) return 0;
            break;

        case WM_MOUSEHWHEEL:
            if (main_layout_handle_mouse_wheel(hwnd, GET_WHEEL_DELTA_WPARAM(wParam), true)) return 0;
            break;

        case APP_WM_ENSURE_LAYOUT_FOCUS:
            main_layout_ensure_child_visible(hwnd, (HWND)wParam);
            return 0;

        case APP_WM_MUTATION_COMPLETE:
            handle_gui_mutation_completion((GuiMutationCompletion*)lParam);
            return 0;

        case APP_WM_SERVICE_IO_COMPLETE:
            handle_gui_service_io_completion((GuiServiceIoCompletion*)lParam);
            return 0;

        case APP_WM_SELECTED_GPU_PNP:
            gui_handle_selected_gpu_pnp_event(
                (GuiSelectedGpuPnpEvent)wParam);
            return 0;

        case APP_WM_ACTIVATE_EXISTING_INSTANCE:
            debug_log("single instance: explicit foreground launch requested resident-window activation\n");
            show_main_window_from_tray();
            return 0;

        case WM_SYSCOMMAND:
            if ((wParam & 0xFFF0) == SC_MINIMIZE) {
                hide_main_window_to_tray();
                return 0;
            }
            break;

        case WM_ERASEBKGND:
            // A coherent projection transaction owns the next frame; erasing
            // here would expose the window background under controls that are
            // still being rewritten.  See gui_window_redraw_policy.h.
            if (gui_top_level_paint_suppressed()) return 1;
            fill_window_background(hwnd, (HDC)wParam);
            return 1;

        case WM_SETTINGCHANGE:
            if (wParam == SPI_SETWORKAREA) ensure_main_window_fits_work_area(hwnd);
            apply_system_titlebar_theme(hwnd);
            allow_dark_mode_for_window(hwnd);
            refresh_menu_theme_cache();
            break;

        case WM_DISPLAYCHANGE:
            reset_gui_gdi_generation("WM_DISPLAYCHANGE");
            enforce_main_window_tray_state("WM_DISPLAYCHANGE");
            ensure_main_window_fits_work_area(hwnd);
            return 0;

        case WM_DWMCOMPOSITIONCHANGED:
            reset_gui_gdi_generation("WM_DWMCOMPOSITIONCHANGED");
            return 0;

        case WM_DEVICECHANGE:
            if (wParam == 0x0007u) { // DBT_DEVNODES_CHANGED
                reset_gui_gdi_generation("display device tree changed");
                enforce_main_window_tray_state("DBT_DEVNODES_CHANGED");
            }
            break;

        case WM_THEMECHANGED:
            apply_system_titlebar_theme(hwnd);
            allow_dark_mode_for_window(hwnd);
            refresh_menu_theme_cache();
            break;

        case APP_WM_SYNC_STARTUP:
        {
            LONG completedGeneration = (LONG)wParam;
            close_startup_sync_thread_handle();
            g_app.startupSyncInFlight = false;
            LONG currentGeneration = current_startup_sync_generation();

            // Always read current config on the UI thread.  A completed worker
            // may have captured an older task/config state, but it may never
            // move the combo back to that older user choice.
            int logonSlot = 0;
            int logonSharedSlot = 0;
            bool selectionReady = load_normalized_logon_selection(
                &logonSlot, &logonSharedSlot);
            if (g_app.hLogonCombo && selectionReady) {
                LRESULT itemData = logon_combo_item_data_from_slots(
                    logonSlot, logonSharedSlot);
                select_logon_combo_item_by_data(g_app.hLogonCombo, itemData);
            }
            update_profile_state_label();
            if (completedGeneration != currentGeneration) {
                debug_log("startup sync: generation %ld completed stale (current=%ld); scheduling current-state reconciliation\n",
                    completedGeneration, currentGeneration);
                schedule_logon_combo_sync();
            }
            return 0;
        }

        case APP_WM_TRAYICON: {
            UINT trayEvent = LOWORD(lParam);
            GuiTrayCallbackKind kind = GUI_TRAY_CALLBACK_UNKNOWN;
            if (trayEvent == WM_CONTEXTMENU)
                kind = GUI_TRAY_CALLBACK_V4_CONTEXT;
            else if (trayEvent == WM_RBUTTONUP)
                kind = GUI_TRAY_CALLBACK_LEGACY_CONTEXT;
            else if (trayEvent == WM_LBUTTONUP)
                kind = GUI_TRAY_CALLBACK_LEGACY_PRIMARY_UP;
            else if (trayEvent == WM_LBUTTONDBLCLK)
                kind = GUI_TRAY_CALLBACK_LEGACY_PRIMARY_DOUBLE;
            else if (trayEvent == NIN_SELECT)
                kind = GUI_TRAY_CALLBACK_V4_SELECT;
            else if (trayEvent == NIN_KEYSELECT)
                kind = GUI_TRAY_CALLBACK_V4_KEY_SELECT;
            if (gui_tray_callback_opens_context_menu(
                    g_app.trayUsesNotificationVersion4, kind)) {
                show_tray_menu(hwnd);
                return 0;
            }
            if (gui_tray_callback_opens_window(
                    g_app.trayUsesNotificationVersion4, kind)) {
                debug_log("tray icon: accepted activation callback event=0x%04X kind=%d hiddenIntent=%d visible=%d\n",
                    trayEvent, (int)kind,
                    g_app.trayWindowHiddenIntent ? 1 : 0,
                    IsWindowVisible(hwnd) ? 1 : 0);
                show_main_window_from_tray();
                return 0;
            }
            debug_log_on_change("tray icon: ignored duplicate/incompatible callback event=0x%04X version4=%d\n",
                trayEvent, g_app.trayUsesNotificationVersion4 ? 1 : 0);
            break;
        }

        case WM_TIMER:
            if (wParam == FAN_CURVE_TIMER_ID) {
                apply_fan_curve_tick();
                return 0;
            }
            if (wParam == FAN_TELEMETRY_TIMER_ID) {
                bool redrawControls = window_should_redraw_fan_controls();
                refresh_live_fan_telemetry(redrawControls);
                update_fan_telemetry_timer();
                // The poll tick is the first moment the update policy is known
                // to this process, and the only one that repeats until the
                // window is visible -- a logon start goes straight to the tray,
                // where a modal question would be ambush rather than disclosure.
                // The function is a no-op on every tick but the one that asks.
                gui_update_maybe_prompt_first_run(hwnd);
                return 0;
            }
            if (wParam == SERVICE_RECONNECT_TIMER_ID) {
                // Auto-reconnect also owns the scheduled-logon tray handoff.
                // It waits for a real initialized snapshot to populate the UI;
                // it never performs or repeats a hardware apply from this timer.
                if (!gui_service_model_ready(&g_app.guiServiceModel) ||
                    g_app.logonServiceReadinessPending) {
                    debug_log_on_change("reconnect timer: enqueueing asynchronous full sync (phase=%s logonReadinessPending=%d)\n",
                        gui_service_phase_name(g_app.guiServiceModel.phase),
                        g_app.logonServiceReadinessPending ? 1 : 0);
                    gui_service_retry_full_sync("reconnect timer");
                }
                return 0;
            }
            if (wParam == APPLY_IN_FLIGHT_TIMER_ID) {
                gui_apply_in_flight_on_timer();
                return 0;
            }
            if (wParam == AUTO_PROFILE_DEBOUNCE_TIMER_ID) {
                auto_profile_on_debounce_timer(hwnd);
                return 0;
            }
            if (wParam == AUTO_PROFILE_BACKSTOP_TIMER_ID) {
                auto_profile_on_backstop_timer(hwnd);
                return 0;
            }
            break;

        case WM_HOTKEY:
            auto_profile_on_hotkey(hwnd, (int)wParam);
            return 0;

        case WM_PAINT: {
            if (gui_top_level_paint_suppressed()) {
                // Validate the region without drawing.  The enclosing
                // transaction repaints the whole tree once it settles, so
                // painting a half-projected frame here is exactly what the
                // suppression exists to prevent -- and skipping BeginPaint
                // entirely would leave the region invalid and spin WM_PAINT.
                PAINTSTRUCT suppressed;
                BeginPaint(hwnd, &suppressed);
                EndPaint(hwnd, &suppressed);
                debug_log_on_change("GUI paint: suppressed during a coherent projection transaction\n");
                return 0;
            }
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);
            RECT rc;
            GetClientRect(hwnd, &rc);
            int surfaceWidth = nvmax(1, rc.right);
            int surfaceHeight = nvmax(1, rc.bottom);

            if (!g_app.hMemDC || !g_app.hMemBmp ||
                g_backbufferWidth != surfaceWidth ||
                g_backbufferHeight != surfaceHeight ||
                g_backbufferGeneration != g_guiGdiGeneration)
                create_backbuffer(hwnd);
            if (!g_app.hMemDC) {
                draw_gui_scene(hdc, &rc);
                EndPaint(hwnd, &ps);
                return 0;
            }

            draw_gui_scene(g_app.hMemDC, &rc);

            if (!BitBlt(hdc, 0, 0, rc.right, rc.bottom,
                    g_app.hMemDC, 0, 0, SRCCOPY)) {
                debug_log_on_change("GUI GDI: final BitBlt failed; retiring backbuffer (error %lu)\n",
                    GetLastError());
                destroy_backbuffer();
                // Complete this paint directly so a persistent display-DC
                // failure cannot leave stale pixels or create a repaint storm.
                draw_gui_scene(hdc, &rc);
            }
            EndPaint(hwnd, &ps);
            return 0;
        }

        case WM_DRAWITEM: {
            const DRAWITEMSTRUCT* dis = (const DRAWITEMSTRUCT*)lParam;
            if (dis && dis->CtlType == ODT_BUTTON) {
                if (dis->CtlID >= LOCK_BASE_ID && dis->CtlID < LOCK_BASE_ID + VF_NUM_POINTS) {
                    draw_lock_checkbox(dis);
                    return TRUE;
                }
                if (is_themed_button_id(dis->CtlID) || is_themed_checkbox_id(dis->CtlID)) {
                    draw_themed_button(dis);
                    return TRUE;
                }
            }
            return FALSE;
        }

        case WM_CTLCOLORBTN:
            return (LRESULT)gui_main_ctlcolor_btn((HDC)wParam);

        case WM_CTLCOLORSTATIC:
            return (LRESULT)gui_main_ctlcolor_static((HDC)wParam, (HWND)lParam);

        case WM_CTLCOLORLISTBOX:
            return (LRESULT)gui_main_ctlcolor_listbox((HDC)wParam);

        case WM_CTLCOLOREDIT:
            return (LRESULT)gui_main_ctlcolor_edit((HDC)wParam, (HWND)lParam);

        case WM_COMMAND:
            if (HIWORD(wParam) == EN_CHANGE && LOWORD(wParam) >= 1000 && LOWORD(wParam) < 1000 + VF_NUM_POINTS) {
                int vi = LOWORD(wParam) - 1000;
                if (!programmatic_edit_update_active() && vi >= 0 && vi < g_app.numVisible) {
                    int ci = g_app.visibleMap[vi];
                    if (ci >= 0 && ci < VF_NUM_POINTS) {
                        bool lockTailPreviewPoint = (g_app.lockedVi >= 0 && vi > g_app.lockedVi);
                        if (!lockTailPreviewPoint) {
                            g_app.guiCurvePointExplicit[ci] = true;
                            g_app.guiHasUserModifiedValues = true;
                            set_gui_state_dirty(true);
                        }
                        char pointBuf[32] = {};
                        get_window_text_safe(g_app.hEditsMhz[vi], pointBuf, sizeof(pointBuf));
                        if (!lockTailPreviewPoint)
                            gui_draft_capture_curve_value(ci, pointBuf);
                        int pointMHz = 0;
                        if (!lockTailPreviewPoint && parse_int_strict(pointBuf, &pointMHz) && pointMHz > 0) {
                            record_ui_action("point %d edited to %d MHz", ci, pointMHz);
                        }
                    }
                }
                if (!programmatic_edit_update_active() && vi == g_app.lockedVi) {
                    sync_locked_tail_preview_from_anchor();
                    return 0;
                }
            }
            if (HIWORD(wParam) == EN_CHANGE && LOWORD(wParam) == GPU_OFFSET_ID && !programmatic_edit_update_active()) {
                char buf[32] = {};
                get_window_text_safe(g_app.hGpuOffsetEdit, buf, sizeof(buf));
                int value = 0;
                g_app.guiHasUserModifiedValues = true;
                g_app.guiGpuOffsetFromProfileLoad = false;  // hand-typed from here on
                set_gui_state_dirty(true);
                gui_draft_capture_text(g_app.guiDraft.gpuOffsetText,
                    ARRAY_COUNT(g_app.guiDraft.gpuOffsetText), buf);
                if (parse_int_strict(buf, &value)) {
                    g_app.guiGpuOffsetMHz = value;
                    record_ui_action("GPU offset edited to %d MHz", value);
                }
            }
            if (HIWORD(wParam) == EN_CHANGE && LOWORD(wParam) == MEM_OFFSET_ID && !programmatic_edit_update_active()) {
                char buf[32] = {};
                get_window_text_safe(g_app.hMemOffsetEdit, buf, sizeof(buf));
                int value = 0;
                g_app.guiHasUserModifiedValues = true;
                g_app.guiMemOffsetFromProfileLoad = false;  // hand-typed from here on
                set_gui_state_dirty(true);
                gui_draft_capture_text(g_app.guiDraft.memOffsetText,
                    ARRAY_COUNT(g_app.guiDraft.memOffsetText), buf);
                if (parse_int_strict(buf, &value)) {
                    g_app.guiMemOffsetMHz = value;
                    record_ui_action("Mem offset edited to %d MHz", value);
                }
            }
            if (HIWORD(wParam) == EN_CHANGE && LOWORD(wParam) == POWER_LIMIT_ID && !programmatic_edit_update_active()) {
                char buf[32] = {};
                get_window_text_safe(g_app.hPowerLimitEdit, buf, sizeof(buf));
                int value = 0;
                g_app.guiHasUserModifiedValues = true;
                set_gui_state_dirty(true);
                gui_draft_capture_text(g_app.guiDraft.powerLimitText,
                    ARRAY_COUNT(g_app.guiDraft.powerLimitText), buf);
                if (parse_int_strict(buf, &value)) {
                    g_app.guiPowerLimitPct = value;
                    record_ui_action("Power limit edited to %d%%", value);
                }
            }
            if (LOWORD(wParam) == APPLY_BTN_ID) {
                apply_changes();
            } else if (LOWORD(wParam) == REFRESH_BTN_ID) {
                refresh_curve();
            } else if (LOWORD(wParam) == RESET_BTN_ID) {
                reset_curve();
            } else if (LOWORD(wParam) == FAN_MODE_COMBO_ID && HIWORD(wParam) == CBN_SELCHANGE) {
                int selection = (int)SendMessageA(g_app.hFanModeCombo, CB_GETCURSEL, 0, 0);
                if (selection >= FAN_MODE_AUTO && selection <= FAN_MODE_CURVE) {
                    g_app.guiFanMode = selection;
                    g_app.guiHasUserModifiedValues = true;
                    set_gui_state_dirty(true);
                    update_fan_controls_enabled_state();
                    gui_pending_changes_refresh();
                }
            } else if (LOWORD(wParam) == GPU_SELECT_COMBO_ID && HIWORD(wParam) == CBN_SELCHANGE) {
                apply_gpu_selection_from_ui();
            } else if (LOWORD(wParam) == GPU_OFFSET_EXCLUDE_LOW_EDIT_ID && HIWORD(wParam) == EN_CHANGE) {
                if (!gui_service_model_ready(&g_app.guiServiceModel) ||
                    !g_app.gpuOffsetRangeKnown) return 0;
                if (!programmatic_edit_update_active()) {
                    char excludeBuf[16] = {};
                    get_window_text_safe(g_app.hGpuOffsetExcludeLowEdit, excludeBuf, sizeof(excludeBuf));
                    int excludeCount = 0;
                    if (excludeBuf[0] && parse_int_strict(excludeBuf, &excludeCount)) {
                        if (excludeCount < 0) excludeCount = 0;
                        g_app.guiGpuOffsetExcludeLowCount = excludeCount;
                    }
                    set_gui_state_dirty(true);
                    gui_draft_capture_text(
                        g_app.guiDraft.gpuOffsetExcludeLowText,
                        ARRAY_COUNT(g_app.guiDraft.gpuOffsetExcludeLowText),
                        excludeBuf);
                    g_app.guiHasUserModifiedValues = true;
                }
            } else if (LOWORD(wParam) == FAN_CONTROL_ID &&
                HIWORD(wParam) == EN_CHANGE &&
                !programmatic_edit_update_active()) {
                char fanBuf[16] = {};
                get_window_text_safe(g_app.hFanEdit, fanBuf,
                    sizeof(fanBuf));
                set_gui_state_dirty(true);
                gui_draft_capture_text(g_app.guiDraft.fanFixedText,
                    ARRAY_COUNT(g_app.guiDraft.fanFixedText), fanBuf);
                int fanPercent = 0;
                if (parse_int_strict(fanBuf, &fanPercent))
                    g_app.guiFanFixedPercent = clamp_percent(fanPercent);
                g_app.guiHasUserModifiedValues = true;
            } else if (LOWORD(wParam) == FAN_CURVE_BTN_ID && HIWORD(wParam) == BN_CLICKED) {
                open_fan_curve_dialog();
            } else if (LOWORD(wParam) == XBAR_ADVANCED_BTN_ID && HIWORD(wParam) == BN_CLICKED) {
                open_xbar_dialog();
            } else if (LOWORD(wParam) == START_ON_LOGON_CHECK_ID && HIWORD(wParam) == BN_CLICKED) {
                bool enabled = !is_start_on_logon_enabled(g_app.configPath);
                bool previous = is_start_on_logon_enabled(g_app.configPath);
                int logonSlot = get_config_int(g_app.configPath, "profiles", "logon_slot", 0);
                if (logonSlot < 0 || logonSlot > CONFIG_NUM_SLOTS) logonSlot = 0;
                char err[256] = {};
                invalidate_startup_sync_generation();
                bool startupConfigSaved = set_start_on_logon_enabled(
                    g_app.configPath, enabled);
                if (!startupConfigSaved) {
                    set_message(err, sizeof(err), "Failed to save start_program_on_logon");
                }
                bool trayRegistrationSaved = startupConfigSaved &&
                    set_tray_autostart_enabled_for_config(g_app.configPath,
                        enabled, err, sizeof(err));
                if (!startupConfigSaved || !trayRegistrationSaved) {
                    if (startupConfigSaved) {
                        set_start_on_logon_enabled(g_app.configPath, previous);
                        char rollbackErr[256] = {};
                        if (!set_tray_autostart_enabled_for_config(g_app.configPath,
                                previous, rollbackErr, sizeof(rollbackErr))) {
                            debug_log("tray autostart: rollback after failed toggle also failed: %s\n",
                                rollbackErr[0] ? rollbackErr : "unknown error");
                        }
                    }
                    // The config is back at `previous`; repaint from that truth.
                    // The tick is derived at paint time, so this is the whole
                    // rollback -- there is no native check state to restore.
                    if (ui_checkbox_state_needs_repaint(&g_app.startOnLogonPainted, previous)) {
                        InvalidateRect(g_app.hStartOnLogonCheck, nullptr, FALSE);
                    }
                    debug_log("start-on-logon: toggle to %d failed and rolled back to %d (err=%s)\n",
                        enabled ? 1 : 0, previous ? 1 : 0, err[0] ? err : "unknown");
                    write_error_report_log_for_user_failure("Logon startup update failed", err[0] ? err : "Failed to update logon startup");
                    gc_message_box(g_app.hMainWnd, err[0] ? err : "Failed to update logon startup", "Green Curve", MB_OK | MB_ICONERROR);
                    break;
                }

                // Tray residency is independent from the handoff task.  A
                // configured profile still needs that task, so validate/repair
                // it separately without rolling back the tray preference.
                if (!set_startup_task_enabled(
                        should_enable_startup_task_from_config(g_app.configPath),
                        err, sizeof(err))) {
                    const char* taskError = err[0] ? err : "Unknown scheduled-task repair error";
                    write_error_report_log_for_user_failure(
                        "Logon startup task synchronization failed", taskError);
                    char warning[640] = {};
                    StringCchPrintfA(warning, ARRAY_COUNT(warning),
                        "Your tray startup preference was saved, but the Windows logon handoff task could not be synchronized. "
                        "Configured profile auto-apply redundancy is degraded until the task is repaired.\r\n\r\n%s",
                        taskError);
                    gc_message_box(g_app.hMainWnd, warning, "Green Curve",
                        MB_OK | MB_ICONWARNING);
                }
                refresh_profile_controls_from_config();
                update_background_service_controls();
                if (enabled) {
                    set_profile_status_text(logonSlot > 0
                        ? (g_app.usingBackgroundService
                            ? "At Windows logon, slot %d will be applied through the background service and the tray client will start hidden."
                            : "At Windows logon, slot %d will be applied and Green Curve will start hidden in the tray.")
                        : (g_app.usingBackgroundService
                            ? "The tray client will start hidden at Windows logon while the background service owns GPU control."
                            : "Green Curve will start hidden in the tray at Windows logon."),
                        logonSlot);
                } else {
                    set_profile_status_text(logonSlot > 0
                        ? (g_app.usingBackgroundService
                            ? "At Windows logon, slot %d will still be applied through the background service even if the tray client does not stay running."
                            : "At Windows logon, slot %d will be applied silently without showing the tray icon.")
                        : "Program start at Windows logon disabled.",
                        logonSlot);
                }
            } else if (LOWORD(wParam) == SHARE_ALL_USERS_CHECK_ID && HIWORD(wParam) == BN_CLICKED) {
                // Lives next to update_share_all_users_check_state() in
                // config_profiles_ui.cpp, which owns the same shared state.
                handle_share_all_users_toggle();
            } else if (LOWORD(wParam) == SHARED_PROFILES_BTN_ID && HIWORD(wParam) == BN_CLICKED) {
                // Any user: open the list of admin-published shared profiles.
                RECT rc = {};
                GetWindowRect(g_app.hSharedProfilesBtn, &rc);
                POINT pt = { rc.left, rc.bottom };
                show_shared_profiles_menu(hwnd, pt);
            } else if (LOWORD(wParam) == SERVICE_ENABLE_CHECK_ID && HIWORD(wParam) == BN_CLICKED) {
                if (g_app.backgroundServiceToggleInFlight) {
                    break;
                }
                bool repair = g_app.backgroundServiceInstalled && g_app.backgroundServiceBroken;
                bool enable = repair || !g_app.backgroundServiceInstalled;
                const char* confirmText = repair
                    ? "Repair and restart the background service using the current service binary?"
                    : enable
                    ? "Install the elevated background service to enable live GPU control?"
                    : "Remove the background service? Live GPU control will be unavailable until it is installed again.";
                int confirm = gc_message_box(g_app.hMainWnd, confirmText, "Confirm Service Change", MB_YESNO | MB_ICONQUESTION);
                if (confirm != IDYES) {
                    break;
                }
                char status[256] = {};
                begin_background_service_toggle(enable);
                update_background_service_controls();
                if (!gui_service_io_queue_admin_toggle(enable, repair,
                        g_app.configPath, status, sizeof(status))) {
                    end_background_service_toggle();
                    update_background_service_controls();
                    gc_message_box(g_app.hMainWnd,
                        status[0] ? status : "Failed queuing the background service change.",
                        "Green Curve", MB_OK | MB_ICONERROR);
                    break;
                }
                set_profile_status_text("%s", status);
                gui_service_begin_full_sync(enable
                    ? "background service installation/repair"
                    : "background service removal");
            } else if (LOWORD(wParam) == PROFILE_COMBO_ID && HIWORD(wParam) == CBN_SELCHANGE) {
                int slot = (int)SendMessageA(g_app.hProfileCombo, CB_GETCURSEL, 0, 0);
                if (slot < 0) slot = CONFIG_DEFAULT_SLOT - 1;
                slot += 1;
                if (!set_config_int(g_app.configPath, "profiles", "selected_slot", slot)) {
                    gc_message_box(g_app.hMainWnd,
                        "Failed to persist the selected profile slot.",
                        "Green Curve", MB_OK | MB_ICONERROR);
                    refresh_profile_controls_from_config();
                    break;
                }
                update_profile_state_label();
                update_profile_action_buttons();
                update_tray_icon();
                set_profile_status_text("Selected slot %d for save/load actions.", slot);
            } else if (LOWORD(wParam) == PROFILE_LOAD_ID) {
                int slot = (int)SendMessageA(g_app.hProfileCombo, CB_GETCURSEL, 0, 0);
                if (slot < 0) slot = CONFIG_DEFAULT_SLOT - 1;
                slot += 1;
                if (!is_profile_slot_saved(g_app.configPath, slot)) {
                    set_profile_status_text("Slot %d is empty. Save a profile first.", slot);
                    break;
                }
                if (!maybe_confirm_profile_load_replace(slot)) break;
                DesiredSettings desired = {};
                char err[256] = {};
                if (!load_profile_from_config(g_app.configPath, slot, &desired, err, sizeof(err))) {
                    write_error_report_log_for_user_failure("Profile load failed", err);
                    gc_message_box(g_app.hMainWnd, err, "Green Curve", MB_OK | MB_ICONERROR);
                    break;
                }
                populate_desired_into_gui(&desired);
                set_gui_state_dirty(true);
                gui_draft_capture_desired(&desired);
                populate_global_controls();
                if (!set_config_int(g_app.configPath, "profiles", "selected_slot", slot)) {
                    write_error_report_log_for_user_failure(
                        "Selected profile persistence failed",
                        "The profile loaded into the editor, but selected_slot failed readback");
                    gc_message_box(g_app.hMainWnd,
                        "The profile was loaded, but the selected slot could not be saved.",
                        "Green Curve", MB_OK | MB_ICONWARNING);
                }
                refresh_profile_controls_from_config();
                set_profile_status_text("Loaded slot %d into the GUI. GPU settings were not applied.", slot);
                invalidate_main_window();
            } else if (LOWORD(wParam) == PROFILE_SAVE_ID) {
                int slot = (int)SendMessageA(g_app.hProfileCombo, CB_GETCURSEL, 0, 0);
                if (slot < 0) slot = CONFIG_DEFAULT_SLOT - 1;
                slot += 1;
                DesiredSettings desired = {};
                char err[256] = {};
                if (!gui_service_model_ready(&g_app.guiServiceModel)) {
                    gui_service_io_queue_full_sync("profile save requested while reconnecting");
                    gc_message_box(g_app.hMainWnd,
                        "Live GPU state is reconnecting. Your draft is preserved; save it after synchronization completes.",
                        "Green Curve", MB_OK | MB_ICONWARNING);
                    break;
                }
                if (!validate_configured_gpu_selection_for_client(
                        err, sizeof(err))) {
                    gc_message_box(g_app.hMainWnd, err[0] ? err :
                        "Select the intended GPU again before saving a profile.",
                        "Green Curve", MB_OK | MB_ICONWARNING);
                    break;
                }
                bool appliedCurveOrLock =
                    (g_app.lockedCi >= 0 && g_app.lockedFreq > 0) ||
                    (g_app.appliedLockMode != LOCK_MODE_NONE &&
                     g_app.appliedLockFreq > 0);
                if (!appliedCurveOrLock) {
                    for (int ci = 0; ci < VF_NUM_POINTS; ++ci) {
                        if (g_app.appliedCurveMHz[ci]) {
                            appliedCurveOrLock = true;
                            break;
                        }
                    }
                }
                if (profile_save_uses_gui_capture(
                        g_app.guiHasUserModifiedValues,
                        gui_has_pending_curve_or_lock_edits(),
                        appliedCurveOrLock)) {
                    if (!capture_gui_config_settings(&desired, err, sizeof(err))) {
                        write_error_report_log_for_user_failure("Profile save capture failed", err);
                        gc_message_box(g_app.hMainWnd, err, "Green Curve", MB_OK | MB_ICONERROR);
                        break;
                    }
                    debug_log("PROFILE_SAVE: saving GUI curve intent (modified=%d curveOrLock=%d appliedCurveOrLock=%d)\n",
                        g_app.guiHasUserModifiedValues ? 1 : 0,
                        gui_has_pending_curve_or_lock_edits() ? 1 : 0,
                        appliedCurveOrLock ? 1 : 0);
                } else {
                    build_full_live_desired_settings(&desired);
                    if (desired.hasFan && g_app.guiFanMode >= FAN_MODE_AUTO && g_app.guiFanMode <= FAN_MODE_CURVE) {
                        int liveCapturedFanMode = desired.fanMode;
                        desired.fanMode = g_app.guiFanMode;
                        desired.fanAuto = desired.fanMode == FAN_MODE_AUTO;
                        desired.fanPercent = desired.fanMode == FAN_MODE_FIXED ? clamp_percent(g_app.guiFanFixedPercent) : 0;
                        copy_fan_curve(&desired.fanCurve, &g_app.guiFanCurve);
                        ensure_valid_fan_curve_config(&desired.fanCurve);
                        if (liveCapturedFanMode != desired.fanMode) {
                            debug_log("PROFILE_SAVE: preserved visible GUI fan intent %d over live captured fan mode %d\n",
                                desired.fanMode,
                                liveCapturedFanMode);
                        }
                    }
                    debug_log("PROFILE_SAVE: no user edits, saving live state\n");
                }
                if (!save_profile_to_config(g_app.configPath, slot, &desired, err, sizeof(err))) {
                    write_error_report_log_for_user_failure("Profile save failed", err);
                    gc_message_box(g_app.hMainWnd, err, "Green Curve", MB_OK | MB_ICONERROR);
                    break;
                }
                populate_desired_into_gui(&desired);
                sync_applied_profile_from_service_metadata();
                refresh_profile_controls_from_config();
                set_profile_status_text("Saved the current GUI values to slot %d.", slot);
                invalidate_main_window();
            } else if (LOWORD(wParam) == PROFILE_CLEAR_ID) {
                int slot = (int)SendMessageA(g_app.hProfileCombo, CB_GETCURSEL, 0, 0);
                if (slot < 0) slot = CONFIG_DEFAULT_SLOT - 1;
                slot += 1;
                if (!is_profile_slot_saved(g_app.configPath, slot)) {
                    set_profile_status_text("Slot %d is already empty.", slot);
                    break;
                }
                char confirm[192];
                StringCchPrintfA(confirm, ARRAY_COUNT(confirm),
                    "Clear profile %d? Any app start or logon assignment for this slot will also be disabled.", slot);
                if (gc_message_box(g_app.hMainWnd, confirm, "Green Curve", MB_YESNO | MB_ICONQUESTION) != IDYES) break;
                char err[256] = {};
                invalidate_startup_sync_generation();
                if (!clear_profile_from_config(g_app.configPath, slot, err, sizeof(err))) {
                    write_error_report_log_for_user_failure("Profile clear failed", err);
                    gc_message_box(g_app.hMainWnd, err, "Green Curve", MB_OK | MB_ICONERROR);
                    break;
                }
                bool taskOk = true;
                taskOk = set_startup_task_enabled(should_enable_startup_task_from_config(g_app.configPath), err, sizeof(err));
                if (!taskOk) {
                    write_error_report_log_for_user_failure("Startup task update failed after profile clear", err[0] ? err : "Unknown error");
                    gc_message_box(g_app.hMainWnd, err[0] ? err : "Failed to update startup task after profile clear", "Green Curve", MB_OK | MB_ICONWARNING);
                }
                refresh_profile_controls_from_config();
                update_background_service_controls();
                set_profile_status_text("Cleared slot %d and disabled any auto-use for it.", slot);
                invalidate_main_window();
            } else if (LOWORD(wParam) == APP_LAUNCH_COMBO_ID || LOWORD(wParam) == LOGON_COMBO_ID) {
                if (HIWORD(wParam) != CBN_SELCHANGE) break;
                if (LOWORD(wParam) == LOGON_COMBO_ID) {
                    // Unified per-account logon selector: decode the selected item's
                    // CB_SETITEMDATA tag (see refresh_profile_controls_from_config):
                    // 0 = no personal choice, 1..N = per-user logon_slot,
                    // LOGON_COMBO_SHARED_FLAG|N = admin shared bank slot N.
                    int sel = (int)SendMessageA(g_app.hLogonCombo, CB_GETCURSEL, 0, 0);
                    LRESULT itemData = (sel < 0) ? 0 : SendMessageA(g_app.hLogonCombo, CB_GETITEMDATA, (WPARAM)sel, 0);
                    if (itemData == CB_ERR || itemData < 0) itemData = 0;
                    int perUserSlot = (itemData & LOGON_COMBO_SHARED_FLAG) ? 0 : (int)itemData;
                    int sharedSlot = (itemData & LOGON_COMBO_SHARED_FLAG) ? (int)(itemData & 0xFF) : 0;

                    if (perUserSlot > 0 && !is_profile_slot_saved(g_app.configPath, perUserSlot)) {
                        gc_message_box(g_app.hMainWnd,
                            "That slot is empty. Save a profile there before using it for automatic actions.",
                            "Green Curve", MB_OK | MB_ICONINFORMATION);
                        refresh_profile_controls_from_config();
                        break;
                    }
                    if (sharedSlot > 0 && !is_machine_profile_slot_saved(sharedSlot)) {
                        set_profile_status_text("Shared profile %d is no longer available.", sharedSlot);
                        refresh_profile_controls_from_config();
                        break;
                    }

                    // logon_slot and logon_shared_slot are one mutually-exclusive
                    // account choice.  Commit them atomically, verify them under the
                    // same lock, then synchronize Task Scheduler as a separate step.
                    char err[256] = {};
                    invalidate_startup_sync_generation();
                    if (!set_logon_profile_selection_atomic(g_app.configPath, perUserSlot,
                            sharedSlot, err, sizeof(err))) {
                        write_error_report_log_for_user_failure("Logon profile selection save failed",
                            err[0] ? err : "Unknown config transaction error");
                        gc_message_box(g_app.hMainWnd,
                            err[0] ? err : "Failed to save the logon profile selection.",
                            "Green Curve", MB_OK | MB_ICONERROR);
                        refresh_profile_controls_from_config();
                        break;
                    }
                    bool taskSynchronized = set_startup_task_enabled(
                        should_enable_startup_task_from_config(g_app.configPath), err, sizeof(err));
                    if (!taskSynchronized) {
                        const char* taskError = err[0] ? err : "Unknown scheduled-task repair error";
                        write_error_report_log_for_user_failure(
                            "Logon startup task synchronization failed", taskError);
                        char warning[640] = {};
                        StringCchPrintfA(warning, ARRAY_COUNT(warning),
                            "Your logon profile choice was saved, but the Windows scheduled task could not be synchronized. "
                            "Logon auto-apply redundancy is degraded until the task is repaired.\r\n\r\n%s",
                            taskError);
                        gc_message_box(g_app.hMainWnd, warning, "Green Curve",
                            MB_OK | MB_ICONWARNING);
                    }
                    bool startProgramAtLogon = is_start_on_logon_enabled(g_app.configPath);
                    if (sharedSlot > 0) {
                        set_profile_status_text(
                            "Shared profile %d will apply at your logon%s - this overrides the all-users default.",
                            sharedSlot, startProgramAtLogon ? " and Green Curve will start hidden in the tray" : "");
                    } else if (perUserSlot > 0) {
                        set_profile_status_text(startProgramAtLogon
                            ? "At Windows logon, slot %d will be applied and Green Curve will start hidden in the tray."
                            : "At Windows logon, slot %d will be applied silently without showing the tray icon.", perUserSlot);
                    } else {
                        int machineDefault = g_app.machineLogonSlotCache;
                        if (machineDefault > 0 && is_machine_profile_slot_saved(machineDefault)) {
                            set_profile_status_text(
                                "No personal logon profile for this account - the admin all-users default (Shared profile %d) applies.",
                                machineDefault);
                        } else {
                            set_profile_status_text("Windows logon auto-apply disabled for this account.");
                        }
                    }
                    update_share_all_users_check_state();
                } else {
                    // App-launch combo: index 0 = Disabled, 1..N = per-user slot.
                    int sel = (int)SendMessageA(g_app.hAppLaunchCombo, CB_GETCURSEL, 0, 0);
                    int slot = (sel < 0) ? 0 : sel;
                    if (slot > 0 && !is_profile_slot_saved(g_app.configPath, slot)) {
                        gc_message_box(g_app.hMainWnd,
                            "That slot is empty. Save a profile there before using it for automatic actions.",
                            "Green Curve", MB_OK | MB_ICONINFORMATION);
                        refresh_profile_controls_from_config();
                        break;
                    }
                    if (!set_config_int(g_app.configPath, "profiles",
                            "app_launch_slot", slot)) {
                        write_error_report_log_for_user_failure(
                            "App-start profile selection save failed",
                            "The app_launch_slot value failed persistence readback");
                        gc_message_box(g_app.hMainWnd,
                            "Failed to save the app-start profile selection.",
                            "Green Curve", MB_OK | MB_ICONERROR);
                        refresh_profile_controls_from_config();
                        break;
                    }
                    set_profile_status_text(slot > 0
                        ? "At app start, slot %d will load into the GUI and apply automatically."
                        : "App start auto-load disabled.", slot);
                }
                refresh_profile_controls_from_config();
                update_background_service_controls();
                invalidate_main_window();
            } else if (LOWORD(wParam) == TRAY_MENU_SHOW_ID) {
                show_main_window_from_tray();
            } else if (LOWORD(wParam) == TRAY_MENU_EXIT_ID) {
                DestroyWindow(hwnd);
            } else if (LOWORD(wParam) == AUTO_PROFILE_BTN_ID && HIWORD(wParam) == BN_CLICKED) {
                show_profiles_popup(hwnd);
            } else if (LOWORD(wParam) == AUTO_PROFILE_MENU_TOGGLE_ID) {
                auto_profile_toggle_enabled(hwnd);
            } else if (LOWORD(wParam) == AUTO_PROFILE_MENU_CONFIGURE_ID) {
                auto_profile_open_config_dialog(hwnd);
            } else if (LOWORD(wParam) > AUTO_PROFILE_MENU_SLOT_BASE &&
                       LOWORD(wParam) <= AUTO_PROFILE_MENU_SLOT_BASE + CONFIG_NUM_SLOTS) {
                auto_profile_pick_slot(hwnd, LOWORD(wParam) - AUTO_PROFILE_MENU_SLOT_BASE);
            } else if (LOWORD(wParam) >= LOCK_BASE_ID && LOWORD(wParam) < LOCK_BASE_ID + VF_NUM_POINTS) {
                int vi = LOWORD(wParam) - LOCK_BASE_ID;
                LockUiStateStamp currentState = current_lock_ui_state_stamp();
                LockActivationDecision decision = decide_lock_activation(
                    (unsigned int)HIWORD(wParam), (unsigned int)BN_CLICKED,
                    g_lockInputGesture.armed, g_lockInputGesture.consumed,
                    g_lockInputGesture.vi, vi,
                    g_lockInputGesture.pressState, currentState);
                debug_log("lock checkbox command: vi=%d notify=%u decision=%s msgTime=%lu gesture=(armed=%d consumed=%d vi=%d source=0x%04x pressTime=%lu) pressState=(vi=%d ci=%d mhz=%u mode=%s) currentState=(vi=%d ci=%d mhz=%u mode=%s)\n",
                          vi, (unsigned)HIWORD(wParam), lock_activation_decision_name(decision),
                          (unsigned long)(DWORD)GetMessageTime(),
                          g_lockInputGesture.armed ? 1 : 0,
                          g_lockInputGesture.consumed ? 1 : 0,
                          g_lockInputGesture.vi,
                          (unsigned)g_lockInputGesture.sourceMessage,
                          (unsigned long)g_lockInputGesture.messageTime,
                          g_lockInputGesture.pressState.lockedVi,
                          g_lockInputGesture.pressState.lockedCi,
                          g_lockInputGesture.pressState.lockedFreq,
                          lock_mode_name(g_lockInputGesture.pressState.lockMode),
                          currentState.lockedVi, currentState.lockedCi,
                          currentState.lockedFreq, lock_mode_name(currentState.lockMode));
                if (decision == LOCK_ACTIVATION_ACCEPT_ARMED) {
                    g_lockInputGesture.consumed = true;
                    activate_lock_checkbox_once(vi);
                } else if (decision == LOCK_ACTIVATION_ACCEPT_UNARMED) {
                    activate_lock_checkbox_once(vi);
                }
            } else if (LOWORD(wParam) == UPDATE_BTN_ID ||
                       LOWORD(wParam) == TRAY_MENU_UPDATE_ID) {
                // Both entry points open the same dialog rather than the tray
                // installing directly: an install stops the service and returns
                // the GPU to stock for a few seconds, which is not something a
                // single click in a context menu should be able to start.
                gui_update_open_dialog(g_app.hMainWnd);
            } else if (LOWORD(wParam) == LICENSE_BTN_ID) {
                show_license_dialog(g_app.hMainWnd);
            }
            return 0;

        case WM_CONTEXTMENU: {
            // Owner-drawn lock checkboxes forward WM_CONTEXTMENU here with
            // wParam = the checkbox HWND. Right-click selects the lock mode.
            int vi = lock_index_from_hwnd((HWND)wParam);
            if (vi >= 0) {
                POINT pt = { (LONG)(short)LOWORD(lParam), (LONG)(short)HIWORD(lParam) };
                if (pt.x == -1 && pt.y == -1) {
                    // Keyboard-invoked (Shift+F10 / menu key): anchor on the control.
                    RECT rc = {};
                    GetWindowRect(g_app.hLocks[vi], &rc);
                    pt.x = rc.left;
                    pt.y = rc.bottom;
                }
                show_lock_context_menu(hwnd, vi, pt);
                return 0;
            }
            // The "Share with all users" checkbox forwards WM_CONTEXTMENU on
            // right-click so the admin can manage the shared profile bank
            // (publish/clear individual slots without changing the default).
            if ((HWND)wParam == g_app.hShareAllUsersCheck) {
                POINT pt = { (LONG)(short)LOWORD(lParam), (LONG)(short)HIWORD(lParam) };
                if (pt.x == -1 && pt.y == -1) {
                    RECT rc = {};
                    GetWindowRect(g_app.hShareAllUsersCheck, &rc);
                    pt.x = rc.left;
                    pt.y = rc.bottom;
                }
                show_machine_logon_context_menu(hwnd, pt);
                return 0;
            }
            break;
        }

        case WM_GETMINMAXINFO: {
            MINMAXINFO* mmi = (MINMAXINFO*)lParam;
            if (mmi) {
                SIZE minSize = main_window_min_track_size(hwnd);
                mmi->ptMinTrackSize.x = minSize.cx;
                mmi->ptMinTrackSize.y = minSize.cy;
            }
            return 0;
        }

        case WM_DESTROY:
            gui_selected_gpu_notification_unregister();
            gui_mutation_shutdown();
            persist_main_window_placement(hwnd);
            KillTimer(hwnd, FAN_TELEMETRY_TIMER_ID);
            auto_profile_shutdown(hwnd);
            if (!g_app.usingBackgroundService || g_app.isServiceProcess) {
                stop_fan_curve_runtime(true);
            }
            if (g_debug_logging) {
                ULONGLONG elapsedMs = g_debugSessionStartTickMs ? (GetTickCount64() - g_debugSessionStartTickMs) : 0;
                char extra[160] = {};
                StringCchPrintfA(extra, ARRAY_COUNT(extra), "gui shutdown uptimeMs=%llu", elapsedMs);
                debug_log_session_marker("END", "gui", extra);
            }
            remove_tray_icon();
            destroy_tray_menu_owner_window();
            close_startup_sync_thread_handle();
            destroy_backbuffer();
            if (g_app.hCachedGridPen) { DeleteObject(g_app.hCachedGridPen); g_app.hCachedGridPen = nullptr; }
            if (g_app.hCachedAxisPen) { DeleteObject(g_app.hCachedAxisPen); g_app.hCachedAxisPen = nullptr; }
            if (g_app.hCachedFont) { DeleteObject(g_app.hCachedFont); g_app.hCachedFont = nullptr; }
            if (g_app.hCachedFontSmall) { DeleteObject(g_app.hCachedFontSmall); g_app.hCachedFontSmall = nullptr; }
            if (g_hBtnBr) { DeleteObject(g_hBtnBr); g_hBtnBr = nullptr; }
            if (g_hInputBr) { DeleteObject(g_hInputBr); g_hInputBr = nullptr; }
            if (g_hStaticBr) { DeleteObject(g_hStaticBr); g_hStaticBr = nullptr; }
            if (g_hListBr) { DeleteObject(g_hListBr); g_hListBr = nullptr; }
            if (g_hEditBr) { DeleteObject(g_hEditBr); g_hEditBr = nullptr; }
            shutdown_gdiplus();
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcA(hwnd, msg, wParam, lParam);
}
