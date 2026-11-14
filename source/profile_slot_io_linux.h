// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#pragma once
#include "profile_bank_edit.h"
#include "profile_file_read.h"
#include "linux_port.h"
#include "linux_config_lock.h"
#include "linux_debug_log.h"

struct LinuxProfileSlotIO {
    bool read(const std::string& path, std::string& bytes, size_t maximum, bool missing, ProfileCodecError& error) {
        const size_t bound = maximum > (size_t)LINUX_INI_MAX_BYTES ? (size_t)LINUX_INI_MAX_BYTES : maximum;
        return profile_file_read(path.c_str(), bytes, bound, missing, error);
    }
    bool write(const std::string& path, const std::string& bytes, ProfileCodecError& error) {
        return write_text_file_atomic(path.c_str(), bytes, error.message, sizeof(error.message));
    }
    bool commit(const std::string& path, const ProfileBankEdit& edit, ProfileCodecError& error) {
        if (edit.replacement.size() > (size_t)LINUX_INI_MAX_BYTES)
            return profile_codec::fail(&error, 0, "updated profile bank exceeds configuration size limit");
        LinuxConfigLock lock(path.c_str(), error.message, sizeof(error.message));
        if (!lock.locked()) return false;
        const bool ok = profile_bank_commit_edit(edit,
            [&](std::string& bytes) { return read(path, bytes, profile_slot::MAX_CONFIG_BYTES, true, error); },
            [&](const std::string& bytes) { return write(path, bytes, error); }, &error);
        linux_debug_logf("profile import: committed=%d records=%d utf16=%d", ok ? 1 : 0, edit.count,
            edit.replacement.compare(0, 2, "\xff\xfe") == 0 ? 1 : 0);
        return ok;
    }
};
