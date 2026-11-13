// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#ifndef GREEN_CURVE_PROFILE_BANK_EDIT_H
#define GREEN_CURVE_PROFILE_BANK_EDIT_H
#include "profile_slot_codec.h"
#include "profile_config_encoding.h"

struct ProfileBankEdit {
    std::string previous;
    std::string replacement;
    int destinations[PROFILE_BANK_CAPACITY]{};
    int count = 0;
};

// No writes: retain the exact generation reviewed by the caller. Slots are
// explicit destinations; file-local document identities never select them.
static inline bool profile_bank_prepare_edit(std::string_view previous, const ProfileBank& incoming,
    const int* destinations, ProfileBankEdit& output, ProfileCodecError* error = nullptr) {
    using namespace profile_codec;
    if (error) *error = {};
    if (!bank_valid(incoming, error)) return false;
    std::string decoded;
    if (!profile_config::decode(previous, decoded, error)) return false;
    if (incoming.count && !destinations) return fail(error, 0, "missing explicit destination slots");
    bool replaced[PROFILE_BANK_CAPACITY]{};
    std::string sections;
    for (int i = 0; i < incoming.count; ++i) {
        const int slot = destinations[i];
        if (slot < 1 || slot > PROFILE_BANK_CAPACITY || replaced[slot - 1])
            return fail(error, 0, "invalid or duplicate destination slot");
        replaced[slot - 1] = true;
        std::string section;
        if (!profile_slot_encode(incoming.documents[i], slot, section, error)) return false;
        sections += section + "\n";
        if (slot == 1) {
            const size_t newline = section.find('\n');
            sections += "[controls]" + section.substr(newline) + "\n";
        }
    }
    const auto managed = [&](View name) {
        for (int slot = 1; slot <= PROFILE_BANK_CAPACITY; ++slot) if (replaced[slot - 1]) {
            const std::string root = "profile" + std::to_string(slot);
            if (profile_slot::ascii_equal(name, root) || profile_slot::ascii_equal(name, root + "_curve") ||
                profile_slot::ascii_equal(name, root + "_fan_curve")) return true;
            if (slot == 1 && (profile_slot::ascii_equal(name, "controls") ||
                profile_slot::ascii_equal(name, "curve") || profile_slot::ascii_equal(name, "fan_curve"))) return true;
        }
        return false;
    };
    std::string replacement;
    bool keep = true;
    View input(decoded);
    if (input.substr(0, 3) == "\xef\xbb\xbf") {
        replacement = "\xef\xbb\xbf";
        input.remove_prefix(3);
    }
    while (!input.empty()) {
        const size_t end = input.find('\n');
        const size_t take = end == View::npos ? input.size() : end + 1;
        const View raw = input.substr(0, take);
        View line = trim(raw);
        while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.remove_suffix(1);
        line = trim(line);
        if (!line.empty() && line.front() == '[' && line.back() == ']')
            keep = !managed(trim(line.substr(1, line.size() - 2)));
        if (keep) replacement.append(raw.data(), raw.size());
        input.remove_prefix(take);
    }
    if (!replacement.empty() && replacement.back() != '\n') replacement += '\n';
    replacement += sections;
    if (replacement.size() > profile_slot::MAX_CONFIG_BYTES) return fail(error, 0, "updated configuration exceeds size limit");
    // Validate every new-format slot in the proposed bank. A legacy slot is
    // deliberately left to its recognized adapter, never reinterpreted here.
    for (int slot = 1; slot <= PROFILE_BANK_CAPACITY; ++slot) {
        std::string body;
        bool sectionFound = false, documentFound = false;
        auto document = std::make_unique<ProfileDocument>();
        if (!profile_slot::extract(replacement, slot, body, sectionFound, error) ||
            (sectionFound && !profile_slot_decode(body, *document, documentFound, error))) return false;
    }
    ProfileBankEdit edit;
    edit.previous.assign(previous.data(), previous.size());
    if (!profile_config::encode_like(previous, replacement, edit.replacement, error)) return false;
    edit.count = incoming.count;
    for (int i = 0; i < incoming.count; ++i) edit.destinations[i] = destinations[i];
    output = std::move(edit);
    return true;
}

// The platform holds its existing storage lock around this operation. Read
// and atomic-replace callbacks are deliberately separate deterministic seams.
template <class Read, class AtomicReplace>
static inline bool profile_bank_commit_edit(const ProfileBankEdit& edit, Read read,
    AtomicReplace replace, ProfileCodecError* error = nullptr) {
    if (error) *error = {};
    std::string current;
    if (!read(current)) return profile_codec::fail(error, 0, "could not re-read the destination bank");
    if (current != edit.previous) return profile_codec::fail(error, 0, "destination bank changed; review the replacement again");
    if (!replace(edit.replacement)) return profile_codec::fail(error, 0, "atomic profile-bank replacement failed");
    return true;
}
#endif
