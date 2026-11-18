// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#pragma once
#include "profile_slot_codec.h"
#include <string>

// Which of the five slots hold a profile (ordinary or imported). An unreadable
// bank reports every slot as saved so nothing is presented as free to replace.
template<class IO> static inline void tui_profile_slots_saved(IO& storage, const char* config, bool saved[6]) {
    for (int slot = 0; slot <= 5; ++slot) saved[slot] = true;
    std::string bytes, text;
    ProfileCodecError error;
    if (!storage.read(config, bytes, profile_slot::MAX_CONFIG_BYTES, true, error) || !profile_config::decode(bytes, text, &error)) return;
    for (int slot = 1; slot <= 5; ++slot) {
        std::string body;
        bool found = false;
        if (!profile_slot::extract(text, slot, body, found, &error)) return;
        saved[slot] = found && !profile_codec::trim(body).empty();
    }
}
static inline std::string tui_slot_choice_label(int slot, const bool saved[6]) {
    return "Slot " + std::to_string(slot) + (saved[slot] ? " (replace)" : " (empty)");
}
