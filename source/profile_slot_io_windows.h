// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#pragma once
#include "profile_bank_edit.h"
#include "profile_file_read.h"

// ConfigStorageLockGuard and the bounded atomic writer belong to the client
// storage layer. No live GPU data or selected-device substitution is involved.
struct WindowsProfileSlotIO {
    bool read(const std::string& path, std::string& bytes, size_t maximum, bool missing, ProfileCodecError& error) {
        return profile_file_read(path.c_str(), bytes, maximum, missing, error);
    }
    bool write(const std::string& path, const std::string& bytes, ProfileCodecError& error) {
        return write_text_file_atomic(path.c_str(), bytes.data(), bytes.size(), error.message, sizeof(error.message));
    }
    bool commit(const std::string& path, const ProfileBankEdit& edit, ProfileCodecError& error) {
        ConfigStorageLockGuard lock;
        if (!lock.locked()) return profile_codec::fail(&error, 0, "could not acquire profile storage lock");
        const bool ok = profile_bank_commit_edit(edit,
            [&](std::string& bytes) { return read(path, bytes, profile_slot::MAX_CONFIG_BYTES, true, error); },
            [&](const std::string& bytes) { return write(path, bytes, error); }, &error);
        if (ok) (void)gc_WritePrivateProfileStringUtf8(nullptr, nullptr, nullptr, path.c_str());
        debug_log("profile-bank: committed=%d records=%d utf16=%d\n", ok ? 1 : 0, edit.count,
            edit.replacement.compare(0, 2, "\xff\xfe") == 0 ? 1 : 0);
        return ok;
    }
};
