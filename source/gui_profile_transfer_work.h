// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#pragma once
#include "profile_editor_transfer.h"

// Owned by exactly one of the window thread, coordinator queue, worker or
// completed-result slot. Wake messages own no object. No AppData, window
// controls or service state travels here.
//
// PROBE reads an import file and lists its records. RUN prepares the chosen
// import or export against the current bank and, when that succeeds, writes
// it in the same job: the dialog already showed every choice, and the bank
// generation check still refuses a bank that changed in between.
enum GuiProfileTransferKind { GUI_PROFILE_TRANSFER_PROBE, GUI_PROFILE_TRANSFER_RUN };
struct GuiProfileTransferWork {
    gc_u64 token = 0;
    GuiProfileTransferKind kind = GUI_PROFILE_TRANSFER_RUN;
    bool committing = false, success = false;
    ProfileEditorTransferOptions options{};
    std::string file, config;
    std::unique_ptr<ProfileEditorTransferPreview> preview;
    std::unique_ptr<ProfileEditorTransferProbe> probe;
};
template<class IO> static inline void gui_profile_transfer_execute(GuiProfileTransferWork& work, IO& storage) {
    if (work.kind == GUI_PROFILE_TRANSFER_PROBE) {
        if (!work.probe) work.probe = std::make_unique<ProfileEditorTransferProbe>();
        work.success = profile_editor_transfer_probe(work.file, work.options.legacy, storage, *work.probe);
        return;
    }
    if (!work.preview) work.preview = std::make_unique<ProfileEditorTransferPreview>();
    work.success = profile_editor_transfer_prepare(work.options, work.file, work.config, storage, *work.preview) &&
        profile_editor_transfer_commit(storage, *work.preview);
    work.committing = true;
}
// Only a job that has not started may be cancelled; a started RUN finishes.
static inline bool gui_profile_transfer_cancel_pending(std::unique_ptr<GuiProfileTransferWork>& pending, gc_u64 token) {
    if (!pending || !token || pending->token != token) return false;
    pending.reset(); return true;
}
