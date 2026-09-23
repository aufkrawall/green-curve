// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// Install-only setup wizard navigation and construction helpers.
//
// Split out of installer_ui.cpp under the source-size ratchet, and linked ONLY
// into the setup stub: greencurve-uninstall.exe must not carry the folder
// picker, the install plan commits, or the payload LICENSE reader.  The shared
// window machine (installer_ui.cpp) calls into this shard through
// installer_ui_internal.h.

#include "installer_common.h"
#include "service_acl.h"
#include "installer_ui_internal.h"

// Reclassify the folder page's path (see service_path_chain_policy.h).  Runs
// on every edit: the classification is a handful of local security reads, and
// the remote short-circuit in the gatherer keeps even a dead network path from
// stalling typing with round trips.
void gc_refresh_folder_protection(GcWizard* wizard, const WCHAR* pathWide) {
    classify_path_protection(pathWide, &wizard->folderProtection, true);
    gc_update_page_controls(wizard);
    InvalidateRect(wizard->hwnd, nullptr, TRUE);
}

// Fold the folder page's answer back into the options, then re-derive the whole
// plan.  Re-deriving (rather than patching the plan in place) keeps the pure
// policy the single decision-maker even when the user steps backwards.
bool gc_commit_folder_page(GcWizard* wizard) {
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
            (int)GC_ARRAY_COUNT(chosenWide))) {
        gc_show_message(wizard->hwnd, "That installation folder cannot be read.",
                        "Green Curve Setup", true);
        SetFocus(wizard->pathEdit);
        return false;
    }
    // The same folders --service-install refuses, refused HERE, on the page
    // where the user typed them.  Setup's step 5 runs `greencurve.exe
    // --service-install` in the target directory, so without this the answer
    // still arrived -- just after the files had been extracted and as a
    // late, generic registration failure.
    int locationVerdict = gc_service_install_location_verdict(chosenWide);
    if (locationVerdict != GC_SVC_LOCATION_OK) {
        gc_log_step("folder page: rejected verdict=%s",
                    gc_service_location_verdict_name(locationVerdict));
        gc_show_message(wizard->hwnd,
                        "Green Curve needs a folder of its own. Installing here would "
                        "change this folder's permissions so that only administrators "
                        "could write to it. Choose or create a subfolder, for example "
                        "C:\\Program Files\\Green Curve.",
                        "Green Curve Setup", true);
        SetFocus(wizard->pathEdit);
        return false;
    }
    gc_refresh_folder_protection(wizard, chosenWide);
    char chosenLabel[GC_INSTALLER_LOG_PATH_LABEL_CHARS] = {};
    gc_log_path_label(chosenWide, chosenLabel, sizeof(chosenLabel));
    gc_log_step("folder page: chosen=%s protected=%d reason=%d acknowledged=%d",
                chosenLabel, wizard->folderProtection.verdict.chain_protected ? 1 : 0,
                (int)wizard->folderProtection.verdict.reason,
                wizard->riskAccepted ? 1 : 0);
    if (gc_path_protection_requires_acknowledgment(&wizard->folderProtection.verdict) &&
        !wizard->riskAccepted) {
        char message[768] = {};
        snprintf(message, sizeof(message), "%s\n\nTick \"%s\" to install there anyway.",
                 gc_path_protection_headline(&wizard->folderProtection.verdict),
                 GC_PATH_PROTECTION_ACKNOWLEDGMENT_LABEL);
        gc_show_message(wizard->hwnd, message, "Green Curve Setup", true);
        SetFocus(wizard->riskCheck);
        return false;
    }
    // Carry the user's ACTUAL answer, not the fact that the page let them
    // past: `gc_install_execute` re-classifies and re-checks on its own, and
    // it can only do that as a second line of defence if it is told whether a
    // box was ticked rather than handed an unconditional yes.
    wizard->install.requirePathRiskAcknowledgment = true;
    wizard->install.pathRiskAcknowledged = wizard->riskAccepted;
    StringCchCopyA(wizard->options.directory, GC_ARRAY_COUNT(wizard->options.directory), chosen);
    wizard->options.hasDirectory = true;
    return true;
}

void gc_commit_options_page(GcWizard* wizard) {
    wizard->options.startMenuShortcut = wizard->startMenu ? GC_TOGGLE_ON : GC_TOGGLE_OFF;
    wizard->options.desktopShortcut = wizard->desktop ? GC_TOGGLE_ON : GC_TOGGLE_OFF;
    wizard->options.launchAfterInstall = wizard->launch ? GC_TOGGLE_ON : GC_TOGGLE_OFF;
    gc_install_build_plan(&wizard->options, &wizard->prior, wizard->defaultDirectory,
                          &wizard->install.plan);
}

void gc_go_back(GcWizard* wizard) {
    if (wizard->page == GC_PAGE_FOLDER) wizard->page = GC_PAGE_LICENSE;
    else if (wizard->page == GC_PAGE_OPTIONS) wizard->page = GC_PAGE_FOLDER;
    else return;
    gc_update_page_controls(wizard);
}

// Modern folder picker.  The user selects the PARENT folder; setup appends the
// fixed "Green Curve" name, unless the folder they picked already is one, which
// avoids the "Green Curve\Green Curve" people otherwise create by browsing to
// their existing installation.
void gc_browse_for_folder(GcWizard* wizard) {
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

void gc_toggle_checkbox(GcWizard* wizard, HWND control, bool* value) {
    *value = !*value;
    InvalidateRect(control, nullptr, TRUE);
    // Next is gated on the license acceptance and on the path-risk
    // acknowledgment; a toggle of either re-derives its enabled state.
    gc_update_page_controls(wizard);
}

void gc_fill_license_text(GcWizard* wizard) {
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
