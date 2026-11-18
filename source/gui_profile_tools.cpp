// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
// Profile tools in the main window's Profiles menu: import the GPU's current
// settings, import profiles from a file, export profiles to a file. Each is a
// small dialog that ends in ordinary slots every apply path understands.
#include "profile_slot_io_windows.h"
#include "profile_document_slot.h"
#include "profile_legacy_ini.h"
#include "gui_profile_dialog_kit.h"

static bool gui_profile_transfer_active();
#include "gui_profile_capture.cpp"
#include "gui_profile_transfer.cpp"

static void gui_profile_tools_service_changed() { gui_profile_capture_service_changed(); }
static bool gui_profile_tools_route_message(MSG& message) {
    gui_profile_transfer_drain();
    return gui_profile_dialog_route(g_transferDialog.hwnd, message) || gui_profile_dialog_route(g_captureDialog.hwnd, message);
}
// Closing the app closes the tool dialogs; only a file write in progress waits.
static bool gui_profile_tools_can_exit() {
    if (g_transferDialog.writing) return false;
    if (g_transferDialog.hwnd) DestroyWindow(g_transferDialog.hwnd);
    if (g_captureDialog.hwnd) DestroyWindow(g_captureDialog.hwnd);
    return true;
}
static void gui_profile_tools_append_menu(HMENU menu) {
    if (!menu) return;
    const bool busy = g_app.applyInFlight || gui_profile_capture_active() || gui_profile_transfer_active();
    bool anySaved = false;
    for (int slot = 1; slot <= CONFIG_NUM_SLOTS && !anySaved; ++slot) anySaved = is_profile_slot_saved(g_app.configPath, slot);
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING | (gui_service_model_ready(&g_app.guiServiceModel) && !busy ? 0 : MF_GRAYED),
        PROFILE_CAPTURE_ID, L"Import current GPU settings...");
    AppendMenuW(menu, MF_STRING | (busy ? MF_GRAYED : 0), PROFILE_IMPORT_ID, L"Import profiles from file...");
    AppendMenuW(menu, MF_STRING | (busy || !anySaved ? MF_GRAYED : 0), PROFILE_EXPORT_ID, L"Export profiles to file...");
}
