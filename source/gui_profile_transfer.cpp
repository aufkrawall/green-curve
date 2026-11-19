// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
// "Export profiles to file" and "Import profiles from file". Both work without
// a GPU. File reads and writes run on the coordinator worker; the dialogs only
// show choices and results. Importing never changes the editor draft.
#include "profile_transfer_picker.h"
#include "profile_summary.h"
#include "profile_ini_slot.h"

enum {
    TRANSFER_DLG_INTRO = 9340, TRANSFER_DLG_FILE, TRANSFER_DLG_STATUS, TRANSFER_DLG_NOTE, TRANSFER_DLG_RUN, TRANSFER_DLG_CANCEL,
    TRANSFER_DLG_ORIGIN_LABEL, TRANSFER_DLG_ORIGIN, TRANSFER_DLG_MEMORY_LABEL, TRANSFER_DLG_MEMORY,
    TRANSFER_DLG_CURVE_LABEL, TRANSFER_DLG_CURVE
};
static constexpr int TRANSFER_DLG_ROW_CHECK = 9360;
static constexpr int TRANSFER_DLG_ROW_TEXT = 9370;
static constexpr int TRANSFER_DLG_ROW_SLOT = 9380;
static constexpr int TRANSFER_ROWS = PROFILE_BANK_CAPACITY;
struct GuiTransferDialog {
    HWND hwnd = nullptr, intro = nullptr, file = nullptr, status = nullptr, note = nullptr, run = nullptr, cancel = nullptr;
    HWND originLabel = nullptr, origin = nullptr, memoryLabel = nullptr, memory = nullptr, curveLabel = nullptr, curve = nullptr;
    HWND rowCheck[TRANSFER_ROWS]{}, rowText[TRANSFER_ROWS]{}, rowSlot[TRANSFER_ROWS]{};
    UiCheckboxState rowState[TRANSFER_ROWS]{};
    GuiProfileDialogBrushes brushes;
    bool importing = false, busy = false, writing = false;
    int rows = 0;
    std::string path;
    std::unique_ptr<ProfileEditorTransferProbe> probe;
};
static GuiTransferDialog g_transferDialog;
static gc_u64 g_profileTransferToken = 0, g_profileTransferPendingToken = 0;

static bool gui_profile_transfer_active() { return g_transferDialog.hwnd != nullptr; }
static void gui_transfer_status(const char* text) { gui_profile_dialog_set_text(g_transferDialog.status, text); }
static std::string gui_transfer_file_name(const std::string& path) {
    const size_t at = path.find_last_of("\\/");
    return at == std::string::npos ? path : path.substr(at + 1);
}
// The slot's profile as a document (portable section or ordinary record).
static bool gui_profile_slot_document(int slot, ProfileDocument& document) {
    ConfigStorageLockGuard lock;
    WindowsProfileSlotIO io;
    ProfileCodecError error{};
    std::string bytes, text;
    bool found = false;
    if (!lock.locked() || !io.read(g_app.configPath, bytes, profile_slot::MAX_CONFIG_BYTES, true, error) ||
        !profile_config::decode(bytes, text, &error) || !profile_document_slot_decode(text, slot, document, found, error))
        return false;
    if (!found && !profile_legacy_native_slot_decode(text, PROFILE_LEGACY_WINDOWS_INI, slot, document, found, &error)) {
        debug_log("profile-transfer: slot %d summary unavailable: %s\n", slot, error.message);
        return false;
    }
    return found;
}
static std::vector<int> gui_transfer_checked_rows() {
    std::vector<int> rows;
    for (int i = 0; i < g_transferDialog.rows; ++i)
        if (IsWindowEnabled(g_transferDialog.rowCheck[i]) && ui_checkbox_state_get(&g_transferDialog.rowState[i])) rows.push_back(i);
    return rows;
}

// ---- layout and projection -----------------------------------------------
static void gui_transfer_layout() {
    auto& d = g_transferDialog;
    if (!d.hwnd) return;
    const int w = 640, checkW = 150, slotW = d.importing ? 220 : 0, textX = 16 + checkW + 8;
    const int textW = w - textX - 16 - (d.importing ? slotW + 10 : 0);
    int y = 14;
    gui_profile_dialog_place(d.intro, 16, y, w - 32, 36); y += 42;
    gui_profile_dialog_place(d.file, 16, y, w - 32, 20, d.importing); if (d.importing) y += 28;
    const bool legacy = d.importing && d.probe && d.probe->legacyFile;
    const bool memory = legacy && d.probe->needsMemoryUnit, curve = legacy && d.probe->needsCurveMeaning;
    const auto question = [&](HWND label, HWND combo, bool shown) {
        gui_profile_dialog_place(label, 16, y + 4, 220, 20, shown);
        gui_profile_dialog_place(combo, 246, y, w - 262, 220, shown);
        if (shown) y += 34;
    };
    question(d.originLabel, d.origin, legacy);
    question(d.memoryLabel, d.memory, memory || (legacy && gui_profile_dialog_combo_data(d.memory, 0) > 0));
    question(d.curveLabel, d.curve, curve || (legacy && gui_profile_dialog_combo_data(d.curve, 0) > 0));
    if (legacy) y += 6;
    for (int i = 0; i < TRANSFER_ROWS; ++i) {
        const bool shown = i < d.rows;
        gui_profile_dialog_place(d.rowCheck[i], 16, y, checkW, 24, shown);
        gui_profile_dialog_place(d.rowText[i], textX, y + 3, textW, 20, shown);
        gui_profile_dialog_place(d.rowSlot[i], w - 16 - slotW, y, slotW, 220, shown && d.importing);
        if (shown) { fit_themed_checkbox_to_label(d.rowCheck[i]); y += 30; }
    }
    y += 6;
    gui_profile_dialog_place(d.note, 16, y, w - 32, 36); y += 42;
    gui_profile_dialog_place(d.status, 16, y, w - 32, 36); y += 44;
    gui_profile_dialog_place(d.run, w - 16 - 100 - 10 - 120, y, 120, 28);
    gui_profile_dialog_place(d.cancel, w - 16 - 100, y, 100, 28);
    gui_profile_dialog_fit(d.hwnd, w, y + 42);
}
// Notes the user must read before importing: replaced slots and slot roles,
// records made for a different GPU, duplicate destinations.
static bool gui_transfer_import_note(std::string& note) {
    auto& d = g_transferDialog;
    note.clear();
    if (!d.probe || !d.probe->ok) return false;
    bool used[TRANSFER_ROWS + 1]{}, duplicate = false;
    std::string replaced;
    for (int row : gui_transfer_checked_rows()) {
        const int slot = (int)gui_profile_dialog_combo_data(d.rowSlot[row], row + 1);
        if (slot < 1 || slot > TRANSFER_ROWS) continue;
        if (used[slot]) duplicate = true;
        used[slot] = true;
        if (is_profile_slot_saved(g_app.configPath, slot)) {
            if (!replaced.empty()) replaced += ", ";
            replaced += std::to_string(slot);
        }
    }
    if (duplicate) { note = "Two profiles go to the same slot. Choose a different slot for one of them."; return false; }
    if (!replaced.empty()) note = "Replaces the profile saved in slot " + replaced + ". Logon, app start and hotkeys keep using these slots.";
    auto live = std::make_unique<ProfileDestinationTopology>();
    const bool haveLive = profile_live_destination_topology(*live);
    auto converted = std::make_unique<DesiredSettings>();
    for (int row : gui_transfer_checked_rows()) {
        const auto& document = d.probe->documents[(size_t)row];
        if (!profile_document_requires_destination(document)) continue;
        char why[192]{};
        if (!haveLive) {
            if (!note.empty()) note += " ";
            note += "Profile " + std::to_string(row + 1) + " needs this GPU to be ready before it can be applied.";
        } else if (!profile_slot_document_runtime(document, live.get(), *converted, why, sizeof(why))) {
            if (!note.empty()) note += " ";
            note += "Profile " + std::to_string(row + 1) + ": " + why + ".";
        }
    }
    return true;
}
static void gui_transfer_project() {
    auto& d = g_transferDialog;
    if (!d.hwnd) return;
    std::string note;
    bool canRun = !d.busy && !gui_transfer_checked_rows().empty();
    if (d.importing) canRun = gui_transfer_import_note(note) && canRun;
    else note = "Choose the profiles to save. You pick the file name next.";
    gui_profile_dialog_set_text(d.note, note.c_str());
    EnableWindow(d.run, canRun);
    EnableWindow(d.cancel, !d.writing);
    for (int i = 0; i < d.rows; ++i) InvalidateRect(d.rowCheck[i], nullptr, FALSE);
    for (HWND combo : {d.origin, d.memory, d.curve}) EnableWindow(combo, !d.busy);
    gui_transfer_layout();
}
static void gui_transfer_fill_export_rows() {
    auto& d = g_transferDialog;
    d.rows = TRANSFER_ROWS;
    auto document = std::make_unique<ProfileDocument>();
    for (int i = 0; i < TRANSFER_ROWS; ++i) {
        const int slot = i + 1;
        const bool saved = is_profile_slot_saved(g_app.configPath, slot);
        std::string text = "Empty";
        if (saved) text = gui_profile_slot_document(slot, *document) ? profile_document_summary(*document) : "Saved profile";
        gui_profile_dialog_set_text(d.rowText[i], text.c_str());
        EnableWindow(d.rowCheck[i], saved);
        ui_checkbox_state_set(&d.rowState[i], saved);
    }
}
static void gui_transfer_fill_import_rows() {
    auto& d = g_transferDialog;
    d.rows = d.probe && d.probe->ok ? (int)std::min<size_t>(d.probe->documents.size(), TRANSFER_ROWS) : 0;
    // Records go to empty slots first, then to the slot of the same number.
    bool taken[TRANSFER_ROWS + 1]{};
    for (int i = 0; i < d.rows; ++i) {
        const auto& document = d.probe->documents[(size_t)i];
        std::string caption = "Profile " + std::to_string(i + 1);
        gui_profile_dialog_set_text(d.rowCheck[i], caption.c_str());
        std::string text = document.displayName[0] ? std::string(document.displayName) + ": " : std::string();
        text += profile_document_summary(document);
        gui_profile_dialog_set_text(d.rowText[i], text.c_str());
        EnableWindow(d.rowCheck[i], TRUE);
        ui_checkbox_state_set(&d.rowState[i], true);
        HWND combo = d.rowSlot[i];
        SendMessageW(combo, CB_RESETCONTENT, 0, 0);
        for (int s = 1; s <= TRANSFER_ROWS; ++s) {
            std::string label = "Into slot " + std::to_string(s) + (is_profile_slot_saved(g_app.configPath, s) ? " (replace)" : " (empty)");
            gui_profile_dialog_combo_add(combo, label.c_str(), s);
        }
        int slot = 0;
        for (int s = 1; s <= TRANSFER_ROWS && !slot; ++s) if (!taken[s] && !is_profile_slot_saved(g_app.configPath, s)) slot = s;
        for (int s = i + 1; s <= TRANSFER_ROWS && !slot; ++s) if (!taken[s]) slot = s;
        for (int s = 1; s <= TRANSFER_ROWS && !slot; ++s) if (!taken[s]) slot = s;
        if (slot) taken[slot] = true;
        gui_profile_dialog_combo_select(combo, slot ? slot : i + 1);
    }
}

// ---- worker jobs ----------------------------------------------------------
static bool gui_transfer_queue(std::unique_ptr<GuiProfileTransferWork> work) {
    if (g_profileTransferToken == ~(gc_u64)0) { gui_transfer_status("Restart Green Curve before transferring again."); return false; }
    work->token = ++g_profileTransferToken;
    work->config = g_app.configPath;
    const gc_u64 token = work->token;
    char error[256]{};
    if (!gui_service_io_queue_transfer(work, error, sizeof(error))) { gui_transfer_status(error); return false; }
    g_profileTransferPendingToken = token;
    g_transferDialog.busy = true;
    return true;
}
static void gui_transfer_probe() {
    auto& d = g_transferDialog;
    auto work = std::make_unique<GuiProfileTransferWork>();
    work->kind = GUI_PROFILE_TRANSFER_PROBE;
    work->file = d.path;
    const LPARAM origin = gui_profile_dialog_combo_data(d.origin, PROFILE_LEGACY_NONE);
    if (origin != PROFILE_LEGACY_NONE) {
        work->options.legacy.format = (ProfileLegacyFormat)origin;
        work->options.legacy.memory = (ProfileLegacyMemoryUnit)std::max<LPARAM>(0, gui_profile_dialog_combo_data(d.memory, 0));
        work->options.legacy.curve = (ProfileLegacyCurveKind)std::max<LPARAM>(0, gui_profile_dialog_combo_data(d.curve, 0));
    }
    d.rows = 0;
    gui_transfer_status("Reading the file...");
    if (gui_transfer_queue(std::move(work))) gui_transfer_project();
}
static void gui_transfer_run() {
    auto& d = g_transferDialog;
    const auto rows = gui_transfer_checked_rows();
    if (d.busy || rows.empty()) return;
    auto work = std::make_unique<GuiProfileTransferWork>();
    work->kind = GUI_PROFILE_TRANSFER_RUN;
    work->options.importing = d.importing;
    if (d.importing) {
        if (!d.probe || !d.probe->ok) return;
        std::string note;
        if (!gui_transfer_import_note(note)) return;
        work->options.legacy = d.probe->legacy;
        for (int row : rows) {
            work->options.sources.push_back(d.probe->sources[(size_t)row]);
            work->options.slots.push_back((int)gui_profile_dialog_combo_data(d.rowSlot[row], row + 1));
        }
        work->file = d.path;
    } else {
        std::string path; char error[256]{};
        if (!profile_transfer_choose_file(d.hwnd, false, path, error, sizeof(error))) { if (error[0]) gui_transfer_status(error); return; }
        d.path = path;
        for (int row : rows) work->options.slots.push_back(row + 1);
        work->file = path;
    }
    const size_t count = rows.size();
    if (!gui_transfer_queue(std::move(work))) return;
    d.writing = true;
    gui_transfer_status(d.importing ? "Importing..." : "Writing the file...");
    gui_transfer_project();
    debug_log("profile-transfer: run queued import=%d records=%u\n", d.importing ? 1 : 0, (unsigned)count);
}
static void gui_transfer_completed(std::unique_ptr<GuiProfileTransferWork> work) {
    auto& d = g_transferDialog;
    if (!work) return;
    if (!d.hwnd || work->token != g_profileTransferPendingToken) {
        debug_log("profile-transfer: stale result discarded token=%llu\n", (unsigned long long)work->token); return;
    }
    g_profileTransferPendingToken = 0;
    d.busy = d.writing = false;
    if (work->kind == GUI_PROFILE_TRANSFER_PROBE) {
        d.probe = std::move(work->probe);
        if (!d.probe) d.probe = std::make_unique<ProfileEditorTransferProbe>();
        if (d.probe->legacyFile && !gui_profile_dialog_combo_data(d.origin, PROFILE_LEGACY_NONE))
            gui_profile_dialog_combo_select(d.origin, d.probe->legacy.format);
        gui_transfer_fill_import_rows();
        if (d.probe->ok) gui_transfer_status(d.probe->legacyFile ? "Older Green Curve settings file. Choose where each profile goes." :
            "Choose where each profile goes.");
        else if (d.probe->needsMemoryUnit || d.probe->needsCurveMeaning)
            gui_transfer_status("This older file does not say how some values were saved. Answer the question above.");
        else gui_transfer_status((std::string("Cannot import this file: ") + d.probe->error + ".").c_str());
        debug_log("profile-transfer: probe ok=%d legacy=%d records=%u askMemory=%d askCurve=%d\n", d.probe->ok ? 1 : 0,
            d.probe->legacyFile ? 1 : 0, (unsigned)d.probe->documents.size(), d.probe->needsMemoryUnit ? 1 : 0,
            d.probe->needsCurveMeaning ? 1 : 0);
        gui_transfer_project();
        return;
    }
    if (!work->success) {
        const char* reason = work->preview && work->preview->error.message[0] ? work->preview->error.message : "the file could not be processed";
        gui_transfer_status((std::string(d.importing ? "Import failed: " : "Export failed: ") + reason + ". Nothing was changed.").c_str());
        gui_transfer_project();
        return;
    }
    const bool importing = d.importing;
    const std::string file = gui_transfer_file_name(d.path);
    std::string slots;
    for (int slot : work->options.slots) { if (!slots.empty()) slots += ", "; slots += std::to_string(slot); }
    const size_t count = work->options.slots.size();
    DestroyWindow(d.hwnd);
    if (importing) {
        const int selected = selected_profile_slot_from_combo();
        invalidate_tray_profile_cache(); refresh_profile_controls_from_config();
        if (g_app.hProfileCombo) SendMessageW(g_app.hProfileCombo, CB_SETCURSEL, (WPARAM)(selected - 1), 0);
        set_profile_status_text("Imported %u profile%s into slot%s %s. Load a slot to edit it.", (unsigned)count,
            count == 1 ? "" : "s", count == 1 ? "" : "s", slots.c_str());
    } else {
        set_profile_status_text("Exported %u profile%s to %s.", (unsigned)count, count == 1 ? "" : "s", file.c_str());
    }
}
static void gui_profile_transfer_drain() {
    auto work = gui_service_io_take_transfer();
    if (work) gui_transfer_completed(std::move(work));
}

// ---- window ---------------------------------------------------------------
static LRESULT CALLBACK gui_transfer_dialog_proc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto& d = g_transferDialog;
    switch (msg) {
        case WM_COMMAND: {
            const int id = LOWORD(wParam), note = HIWORD(wParam);
            if (note == BN_CLICKED && id >= TRANSFER_DLG_ROW_CHECK && id < TRANSFER_DLG_ROW_CHECK + TRANSFER_ROWS) {
                if (!d.busy) ui_checkbox_state_toggle(&d.rowState[id - TRANSFER_DLG_ROW_CHECK]);
                gui_transfer_project(); return 0;
            }
            if (note == CBN_SELCHANGE && id >= TRANSFER_DLG_ROW_SLOT && id < TRANSFER_DLG_ROW_SLOT + TRANSFER_ROWS) {
                gui_transfer_project(); return 0;
            }
            if (note == CBN_SELCHANGE && (id == TRANSFER_DLG_ORIGIN || id == TRANSFER_DLG_MEMORY || id == TRANSFER_DLG_CURVE)) {
                if (!d.busy) gui_transfer_probe();
                return 0;
            }
            if (note != BN_CLICKED) break;
            if (id == TRANSFER_DLG_RUN) gui_transfer_run();
            else if (id == TRANSFER_DLG_CANCEL) SendMessageW(hwnd, WM_CLOSE, 0, 0);
            return 0;
        }
        case WM_DRAWITEM: {
            const auto* dis = (const DRAWITEMSTRUCT*)lParam;
            if (dis && dis->CtlID >= TRANSFER_DLG_ROW_CHECK && dis->CtlID < TRANSFER_DLG_ROW_CHECK + TRANSFER_ROWS) {
                draw_themed_checkbox_control(dis, ui_checkbox_state_get(&d.rowState[dis->CtlID - TRANSFER_DLG_ROW_CHECK]), true);
                return TRUE;
            }
            if (dis) draw_themed_button(dis);
            return TRUE;
        }
        case WM_CTLCOLORSTATIC: {
            const int id = GetDlgCtrlID((HWND)lParam);
            const bool strong = id == TRANSFER_DLG_STATUS || id == TRANSFER_DLG_INTRO || id == TRANSFER_DLG_FILE ||
                (id >= TRANSFER_DLG_ROW_TEXT && id < TRANSFER_DLG_ROW_TEXT + TRANSFER_ROWS);
            return gui_profile_dialog_color(d.brushes, msg, (HDC)wParam, (HWND)lParam, strong, id == TRANSFER_DLG_NOTE && d.importing);
        }
        case WM_CTLCOLOREDIT: case WM_CTLCOLORLISTBOX: case WM_CTLCOLORBTN:
            return gui_profile_dialog_color(d.brushes, msg, (HDC)wParam, (HWND)lParam, true);
        case WM_ERASEBKGND: gui_profile_dialog_paint_background(hwnd, (HDC)wParam); return 1;
        case WM_THEMECHANGED: case WM_SETTINGCHANGE: apply_system_titlebar_theme(hwnd); allow_dark_mode_for_window(hwnd); break;
        case WM_CLOSE:
            if (d.writing) { gui_transfer_status("Wait until the file operation finishes."); return 0; }
            DestroyWindow(hwnd); return 0;
        case WM_DESTROY:
            gui_service_io_cancel_transfer(g_profileTransferPendingToken);
            g_profileTransferPendingToken = 0;
            gui_profile_dialog_release(d.brushes);
            d = GuiTransferDialog{};
            gui_profile_dialog_restore_owner();
            update_profile_action_buttons();
            debug_log("profile-transfer: closed\n");
            return 0;
        default: break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}
static GC_NOINLINE void gui_profile_transfer_open(bool importing) {
    if (gui_profile_transfer_active()) { SetForegroundWindow(g_transferDialog.hwnd); return; }
    if (gui_profile_capture_active() || g_app.applyInFlight) {
        set_profile_status_text("Finish the open profile dialog or GPU operation first."); return;
    }
    std::string path;
    if (importing) {
        char error[256]{};
        if (!profile_transfer_choose_file(g_app.hMainWnd, true, path, error, sizeof(error))) {
            if (error[0]) set_profile_status_text("%s", error);
            return;
        }
    }
    auto& d = g_transferDialog;
    d = GuiTransferDialog{};
    d.importing = importing;
    d.path = path;
    d.hwnd = gui_profile_dialog_create(importing ? L"GreenCurveImportDialog" : L"GreenCurveExportDialog",
        importing ? "Import profiles from file" : "Export profiles to file", gui_transfer_dialog_proc, 640, 320);
    if (!d.hwnd) { set_profile_status_text("Could not open the profile dialog."); return; }
    d.intro = gui_profile_dialog_label(d.hwnd, importing
        ? "Adds profiles from a file to your slots. Your GPU and the editor are not changed."
        : "Saves profiles to a file you can import on another PC or after reinstalling. No GPU is needed.", TRANSFER_DLG_INTRO);
    d.file = gui_profile_dialog_label(d.hwnd, ("File: " + path).c_str(), TRANSFER_DLG_FILE, SS_PATHELLIPSIS);
    d.originLabel = gui_profile_dialog_label(d.hwnd, "Saved by Green Curve on", TRANSFER_DLG_ORIGIN_LABEL);
    d.origin = gui_profile_dialog_combo(d.hwnd, TRANSFER_DLG_ORIGIN);
    gui_profile_dialog_combo_add(d.origin, "Windows", PROFILE_LEGACY_WINDOWS_INI);
    gui_profile_dialog_combo_add(d.origin, "Linux", PROFILE_LEGACY_LINUX_INI);
    SendMessageW(d.origin, CB_SETCURSEL, (WPARAM)-1, 0);
    d.memoryLabel = gui_profile_dialog_label(d.hwnd, "Memory clock values are", TRANSFER_DLG_MEMORY_LABEL);
    d.memory = gui_profile_dialog_combo(d.hwnd, TRANSFER_DLG_MEMORY);
    gui_profile_dialog_combo_add(d.memory, "Choose...", 0);
    gui_profile_dialog_combo_add(d.memory, "As shown in Green Curve (MHz)", PROFILE_LEGACY_MEMORY_DISPLAY);
    gui_profile_dialog_combo_add(d.memory, "Doubled, as some tools show them (effective MHz)", PROFILE_LEGACY_MEMORY_EFFECTIVE);
    SendMessageW(d.memory, CB_SETCURSEL, 0, 0);
    d.curveLabel = gui_profile_dialog_label(d.hwnd, "Curve point values are", TRANSFER_DLG_CURVE_LABEL);
    d.curve = gui_profile_dialog_combo(d.hwnd, TRANSFER_DLG_CURVE);
    gui_profile_dialog_combo_add(d.curve, "Choose...", 0);
    gui_profile_dialog_combo_add(d.curve, "Final clocks", PROFILE_LEGACY_CURVE_ABSOLUTE);
    gui_profile_dialog_combo_add(d.curve, "Final clocks, some following the core offset", PROFILE_LEGACY_CURVE_ORIGIN);
    gui_profile_dialog_combo_add(d.curve, "Offsets from the stock curve", PROFILE_LEGACY_CURVE_BASE);
    SendMessageW(d.curve, CB_SETCURSEL, 0, 0);
    for (int i = 0; i < TRANSFER_ROWS; ++i) {
        d.rowCheck[i] = gui_profile_dialog_button(d.hwnd, ("Slot " + std::to_string(i + 1)).c_str(), TRANSFER_DLG_ROW_CHECK + i);
        d.rowText[i] = gui_profile_dialog_label(d.hwnd, "", TRANSFER_DLG_ROW_TEXT + i, SS_ENDELLIPSIS);
        d.rowSlot[i] = gui_profile_dialog_combo(d.hwnd, TRANSFER_DLG_ROW_SLOT + i);
    }
    d.note = gui_profile_dialog_label(d.hwnd, "", TRANSFER_DLG_NOTE);
    d.status = gui_profile_dialog_label(d.hwnd, "", TRANSFER_DLG_STATUS);
    d.run = gui_profile_dialog_button(d.hwnd, importing ? "Import" : "Export...", TRANSFER_DLG_RUN);
    d.cancel = gui_profile_dialog_button(d.hwnd, "Cancel", TRANSFER_DLG_CANCEL);
    if (!importing) gui_transfer_fill_export_rows();
    gui_profile_dialog_show(d.hwnd, importing ? d.cancel : d.run);
    update_profile_action_buttons();
    debug_log("profile-transfer: opened import=%d\n", importing ? 1 : 0);
    if (importing) gui_transfer_probe();
    else gui_transfer_project();
}
