// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#ifndef GREEN_CURVE_PROFILE_CONFIG_WINDOWS_H
#define GREEN_CURVE_PROFILE_CONFIG_WINDOWS_H
#include "profile_slot_codec.h"
#include "profile_file_read.h"

// Caller holds ConfigStorageLockGuard. Bypass INI value/section truncation.
static inline bool profile_windows_document_section(const char* path, const char* name,
    std::string& body, bool& found, char* why, size_t whySize) {
    found = false;
    bool relevant = profile_slot::ascii_equal(name, "controls");
    for (int i = 1; i <= PROFILE_BANK_CAPACITY; ++i)
        relevant |= profile_slot::ascii_equal(name, "profile" + std::to_string(i));
    if (!relevant) return true;
    ProfileCodecError error;
    std::string bytes;
    bool section = false;
    auto document = std::make_unique<ProfileDocument>();
    const bool ok = profile_file_read(path, bytes, profile_slot::MAX_CONFIG_BYTES, true, error) &&
        profile_slot::extract_named(bytes, name, body, section, &error) &&
        (!section || profile_slot_decode(body, *document, found, &error));
    if (!ok && why && whySize) snprintf(why, whySize, "%s", error.message);
    return ok;
}
#endif
