// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#pragma once
#include "profile_bank_edit.h"
#include "profile_ini_slot.h"
#include "profile_file_read.h"
#include "linux_port.h"
#include "linux_config_lock.h"
#include "linux_debug_log.h"
#include <stdlib.h>

struct LinuxProfileSlotIO {
    bool read(const std::string& path, std::string& bytes, size_t maximum, bool missing, ProfileCodecError& error) {
        const size_t bound = maximum > (size_t)LINUX_INI_MAX_BYTES ? (size_t)LINUX_INI_MAX_BYTES : maximum;
        return profile_file_read(path.c_str(), bytes, bound, missing, error);
    }
    bool write(const std::string& path, const std::string& bytes, ProfileCodecError& error) {
        return write_text_file_atomic(path.c_str(), bytes, error.message, sizeof(error.message));
    }
    bool writeNew(const std::string& path, const std::string& bytes, bool& exists, ProfileCodecError& error) {
        return write_text_file_atomic(path.c_str(), bytes, error.message, sizeof(error.message), &exists);
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
    bool distinct(const std::string& config, const std::string& file, ProfileCodecError& error) {
        const auto resolved = [](const std::string& path) {
            char* full = realpath(path.c_str(), nullptr);
            if (full) { std::string value(full); free(full); return value; }
            const size_t slash = path.find_last_of('/');
            const std::string parent = slash == std::string::npos ? "." : path.substr(0, slash + 1);
            full = realpath(parent.c_str(), nullptr);
            if (!full) return std::string{};
            std::string value(full); free(full);
            return value + "/" + path.substr(slash == std::string::npos ? 0 : slash + 1);
        };
        const std::string a = resolved(config), b = resolved(file);
        if ((!a.empty() && a == b) || (a.empty() && b.empty()))
            return profile_codec::fail(&error, 0, "profile file must have a distinct valid destination path");
        return true;
    }
    bool legacy(const std::string& input, int slot, ProfileDocument& document, bool& found, ProfileCodecError& error) {
        const bool ok = profile_legacy_native_slot_decode(input, PROFILE_LEGACY_LINUX_INI, slot, document, found, &error);
        linux_debug_logf("profile legacy export: platform=linux slot=%d decoded=%d found=%d", slot, ok ? 1 : 0, found ? 1 : 0);
        return ok;
    }
};
