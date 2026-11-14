// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#pragma once
#include "profile_bank_edit.h"
#include <memory>

// The configuration is already decoded. Return a canonical document or an
// explicit legacy/missing result; do not change output for those cases.
static inline bool profile_document_slot_decode(std::string_view text, int slot,
    ProfileDocument& output, bool& found, ProfileCodecError& error) {
    found = false;
    std::string body; bool section = false;
    if (!profile_slot::extract(text, slot, body, section, &error)) return false;
    if (!section && slot == 1 && !profile_slot::extract_named(text, "controls", body, section, &error)) return false;
    return profile_slot_decode(body, output, found, &error);
}
template<class IO>
static inline bool profile_document_slot_load(IO& io, const std::string& path, int slot,
    ProfileDocument& output, bool& found, ProfileCodecError& error) {
    found = false;
    std::string bytes, text;
    return io.read(path, bytes, profile_slot::MAX_CONFIG_BYTES, true, error) &&
        profile_config::decode(bytes, text, &error) && profile_document_slot_decode(text, slot, output, found, error);
}
template<class IO>
static inline bool profile_document_slot_save(IO& io, const std::string& path, int slot,
    const ProfileDocument& document, ProfileCodecError& error) {
    std::string previous;
    if (!io.read(path, previous, profile_slot::MAX_CONFIG_BYTES, true, error)) return false;
    auto bank = std::make_unique<ProfileBank>(); bank->count = 1; bank->documents[0] = document;
    ProfileBankEdit edit;
    return profile_bank_prepare_edit(previous, *bank, &slot, edit, &error) && io.commit(path, edit, error);
}
