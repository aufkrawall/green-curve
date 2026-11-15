// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#pragma once
#include "profile_editor_transfer.h"

// Owned by exactly one of the window thread, coordinator queue, worker or
// completed-result slot. Wake messages own no object. No AppData, window
// controls or service state travels here.
struct GuiProfileTransferWork {
    gc_u64 token = 0;
    bool committing = false, success = false;
    ProfileEditorTransferOptions options{};
    std::string file, config;
    std::unique_ptr<ProfileEditorTransferPreview> preview;
};
template<class IO> static inline void gui_profile_transfer_execute(GuiProfileTransferWork& work, IO& storage) {
    if (!work.preview) work.preview = std::make_unique<ProfileEditorTransferPreview>();
    work.success = work.committing ? profile_editor_transfer_commit(storage, *work.preview) :
        profile_editor_transfer_prepare(work.options, work.file, work.config, storage, *work.preview);
}
static inline bool gui_profile_transfer_cancel_pending(std::unique_ptr<GuiProfileTransferWork>& pending, gc_u64 token) {
    if (!pending || !token || pending->token != token || pending->committing) return false;
    pending.reset(); return true;
}
