// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#ifndef GREEN_CURVE_PROFILE_INI_SLOT_H
#define GREEN_CURVE_PROFILE_INI_SLOT_H
// Ordinary INI slot sections -- this machine's own (export, slot summaries) or
// another installation's settings file (import) -- as portable documents. An
// ordinary slot means exactly the runtime record the slot loader builds from
// it, so it is decoded to that record and described with the same adapter
// every reader reverses (profile_slot_runtime.h):
//
// * base keys go through the historical decoder (profile_legacy_ini.h). For
//   this machine's own slots a curve saved before the curve marker is read
//   with the slot loader's rule, since nobody can be asked how their own older
//   build saved it; a file being imported still asks;
// * the keys newer slots add (application scope, precise values, captured
//   points, voltage binding) are applied after the base keys by the loader's
//   own extension reader, as the loader does.
//
// No live GPU state and no repair pass is consulted, so the same file always
// decodes the same way.
#include "profile_legacy_ini.h"
#include "profile_legacy_document.h"
#include "profile_slot_runtime.h"
#include "profile_desired_ini_ext.h"

namespace profile_ini_slot {
using profile_legacy::Fields;
inline bool control_extension(const std::string& key) {
    static const char* const keys[] = {"application_scope", "mem_offset_precise", "mem_offset_surface", "mem_offset_hz",
        "power_limit_ratio", "power_ratio_num", "power_ratio_den", "graphics_precise", "graphics_surface",
        "graphics_offset_hz", "graphics_identity_exclusions", "lock_precise", "lock_frequency_hz", "topology_bound",
        "topology_count"};
    for (const char* known : keys) if (key == known) return true;
    return false;
}
inline bool curve_extension(const std::string& key) {
    if (key.compare(0, 5, "point") != 0) return false;
    size_t at = 5;
    while (at < key.size() && key[at] >= '0' && key[at] <= '9') ++at;
    if (at == 5 || at >= key.size() || key[at] != '_') return false;
    const std::string suffix = key.substr(at + 1);
    static const char* const suffixes[] = {"captured", "captured_offset_hz", "freq_precise", "freq_hz",
        "graphics_excluded", "topology_uv"};
    for (const char* known : suffixes) if (suffix == known) return true;
    return false;
}
inline int split(Fields& fields, bool curve, Fields& extension) {
    int moved = 0;
    for (auto it = fields.begin(); it != fields.end();) {
        if (curve ? curve_extension(it->first) : control_extension(it->first)) {
            extension.insert(*it); it = fields.erase(it); ++moved;
        } else {
            ++it;
        }
    }
    return moved;
}
inline auto reader(const Fields& fields) {
    return [&fields](const char* key, char* out, size_t size) {
        const auto at = fields.find(key);
        if (at == fields.end()) return false;
        snprintf(out, size, "%s", at->second.c_str());
        return true;
    };
}
} // namespace profile_ini_slot

// One slot's sections as a document. A slot without extension keys keeps the
// historical decoder's document unchanged.
static inline bool profile_ini_slot_document(profile_legacy::Fields controls, profile_legacy::Fields curve,
    const profile_legacy::Fields& fan, const profile_legacy::Fields& meta, ProfileLegacyOptions options, int slot,
    ProfileDocument& output, ProfileCodecError* error) {
    profile_legacy::Fields controlExtension, curveExtension;
    const int extensions = profile_ini_slot::split(controls, false, controlExtension) +
        profile_ini_slot::split(curve, true, curveExtension);
    auto base = std::make_unique<ProfileDocument>();
    if (!profile_legacy::document(controls, curve, fan, meta, options, slot, *base, error)) return false;
    if (!extensions) { output = *base; return true; }
    auto record = std::make_unique<DesiredSettings>();
    char why[192]{};
    if (!profile_desired_from_indexed_document(*base, *record, why, sizeof(why)) ||
        !desired_ini_load_controls_ext(profile_ini_slot::reader(controlExtension), record.get(), why, sizeof(why)) ||
        !desired_ini_load_curve_ext(profile_ini_slot::reader(curveExtension), record.get(), why, sizeof(why)) ||
        !desired_ini_validate_loaded(record.get(), why, sizeof(why)))
        return profile_legacy::fail(error, why[0] ? why : "saved profile settings contradict each other");
    auto document = std::make_unique<ProfileDocument>();
    if (!profile_document_from_legacy_desired(*record, *document, why, sizeof(why)))
        return profile_legacy::fail(error, why[0] ? why : "saved profile cannot be described");
    document->documentId = base->documentId;
    document->source = base->source;
    snprintf(document->displayName, sizeof(document->displayName), "%s", base->displayName);
    output = *document;
    return true;
}

static inline bool profile_legacy_ini_decode(std::string_view input, ProfileLegacyOptions options,
    const ProfileSourceSelection& selection, ProfileBank& output, ProfileCodecError* error = nullptr) {
    if (error) *error = {};
    if (options.format != PROFILE_LEGACY_WINDOWS_INI && options.format != PROFILE_LEGACY_LINUX_INI)
        return profile_legacy::fail(error, "select a supported legacy source format");
    if (options.memory < PROFILE_LEGACY_MEMORY_UNKNOWN || options.memory > PROFILE_LEGACY_MEMORY_EFFECTIVE ||
        options.curve < PROFILE_LEGACY_CURVE_UNKNOWN || options.curve > PROFILE_LEGACY_CURVE_BASE)
        return profile_legacy::fail(error, "invalid legacy source interpretation");
    if (selection.count < 1 || selection.count > 5) return profile_legacy::fail(error, "invalid legacy source selection");
    if (selection.all && (selection.count != 5 || selection.slots[0] != 1 || selection.slots[1] != 2 ||
        selection.slots[2] != 3 || selection.slots[3] != 4 || selection.slots[4] != 5))
        return profile_legacy::fail(error, "invalid all-profiles selection");
    profile_legacy::Sections sections;
    if (!profile_legacy::sections(input, sections, error)) return false;
    const auto meta = sections.find("meta");
    const profile_legacy::Fields empty;
    const auto& metadata = meta != sections.end() ? meta->second : empty;
    const auto version = metadata.find("format_version");
    if (version != metadata.end() && version->second != "1" && version->second != "2")
        return profile_legacy::fail(error, "unsupported legacy format version");
    auto bank = std::make_unique<ProfileBank>(); bool seen[5]{};
    for (int n = 0; n < selection.count; ++n) {
        const int slot = selection.slots[n];
        if (slot < 1 || slot > 5 || seen[slot - 1]) return profile_legacy::fail(error, "invalid or duplicate legacy source slot");
        seen[slot - 1] = true;
        std::string name = "profile" + std::to_string(slot);
        const auto hasFields = [&](const std::string& root) {
            const auto at = sections.find(root); return at != sections.end() && !at->second.empty();
        };
        const auto exists = [&](const std::string& root) {
            return hasFields(root) || hasFields(root + "_curve") || hasFields(root + "_fan_curve");
        };
        const bool useFallback = slot == 1 && !exists(name) &&
            (hasFields("controls") || hasFields("curve") || hasFields("fan_curve"));
        const auto get = [&](const std::string& suffix) -> profile_legacy::Fields {
            const auto key = useFallback ? (suffix.empty() ? "controls" : suffix == "_curve" ? "curve" : "fan_curve") : name + suffix;
            const auto at = sections.find(key); return at == sections.end() ? profile_legacy::Fields{} : at->second;
        };
        if (!exists(name) && !useFallback) {
            if (selection.all) continue;
            return profile_legacy::fail(error, "selected legacy source slot is empty");
        }
        if (!profile_ini_slot_document(get(""), get("_curve"), get("_fan_curve"), metadata, options,
            slot, bank->documents[bank->count], error)) return false;
        ++bank->count;
    }
    if (!bank->count) return profile_legacy::fail(error, "no legacy profiles were found");
    output = *bank; return true;
}

// Decode one of this machine's own slots (ordinary INI sections; a document
// slot is read by profile_document_slot_decode). `found` is false for an
// empty slot.
static inline bool profile_legacy_native_slot_decode(std::string_view input, ProfileLegacyFormat format, int slot,
    ProfileDocument& output, bool& found, ProfileCodecError* error) {
    found = false;
    if (error) *error = {};
    if (slot < 1 || slot > 5) return profile_legacy::fail(error, "invalid native legacy slot");
    if (format != PROFILE_LEGACY_WINDOWS_INI && format != PROFILE_LEGACY_LINUX_INI)
        return profile_legacy::fail(error, "select a supported legacy source format");
    profile_legacy::Sections sections;
    if (!profile_legacy::sections(input, sections, error)) return false;
    const auto section = [&](const std::string& name) {
        const auto at = sections.find(name);
        return at == sections.end() ? profile_legacy::Fields{} : at->second;
    };
    const std::string root = "profile" + std::to_string(slot);
    const bool own = !section(root).empty() || !section(root + "_curve").empty() || !section(root + "_fan_curve").empty();
    const bool fallback = !own && slot == 1 &&
        (!section("controls").empty() || !section("curve").empty() || !section("fan_curve").empty());
    if (!own && !fallback) return true;
    const auto meta = section("meta");
    const auto version = meta.find("format_version");
    if (version != meta.end() && version->second != "1" && version->second != "2")
        return profile_legacy::fail(error, "unsupported legacy format version");
    ProfileLegacyOptions options;
    options.format = format; options.nativeSlot = true;
    auto document = std::make_unique<ProfileDocument>();
    if (!profile_ini_slot_document(section(fallback ? "controls" : root), section(fallback ? "curve" : root + "_curve"),
            section(fallback ? "fan_curve" : root + "_fan_curve"), meta, options, slot, *document, error)) return false;
    output = *document; found = true; return true;
}
#endif
