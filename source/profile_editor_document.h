// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#pragma once
#include "profile_document.h"
#include "profile_codec.h"
#include <climits>

struct ProfileEditorDocument {
    bool valid = false;
    bool unsaved = false;
    gc_u64 revision = 1; // local draft revision, never serialized or authorization
    ProfileDocument value{}, saved{};
};
struct ProfileEditorCandidate {
    DesiredSettings settings{};
    ProfileEditorDocument source{};
};
static inline void profile_editor_document_reset(ProfileEditorDocument& doc) {
    doc.valid = false;
    doc.unsaved = false;
    doc.revision = 1;
    profile_document_reset(doc.value);
    profile_document_reset(doc.saved);
}
static inline void profile_editor_candidate_reset(ProfileEditorCandidate& c) {
    memset(&c.settings, 0, sizeof(c.settings));
    profile_editor_document_reset(c.source);
}
static inline bool profile_editor_document_dirty(const ProfileEditorDocument& source) {
    if (!source.valid) return false;
    if (source.unsaved) return true;
    auto bank = std::make_unique<ProfileBank>(); bank->count = 1;
    std::string saved, current;
    bank->documents[0] = source.saved;
    if (!profile_bank_encode(*bank, saved)) return true;
    bank->documents[0] = source.value;
    return !profile_bank_encode(*bank, current) || current != saved;
}
static inline bool profile_editor_mhz_preview(long long hz, int& value) {
    const long long mhz = hz / 1000000;
    if (mhz < INT_MIN || mhz > INT_MAX) return false;
    value = (int)mhz; return true;
}
// Display scalars only. Point/lock source identities are not destination indices.
// This projection grants no Apply authority, and saving uses the document alone.
static inline void profile_editor_source_display(const ProfileDocument& source, DesiredSettings& d) {
    memset(&d, 0, sizeof(d));
    d.hasGpuOffset = source.graphicsOffset.present && profile_editor_mhz_preview(source.graphicsOffset.offsetHz, d.gpuOffsetMHz);
    d.hasMemOffset = source.hasMemoryOffset && profile_editor_mhz_preview(source.memoryOffsetHz, d.memOffsetMHz);
    if (source.hasMemoryOffset) (void)desired_memory_set_precise(&d, source.memoryOffsetHz, source.memorySurface);
    if (source.power.present) (void)desired_power_set_ratio(&d, source.power.ratio);
    if (source.fan.present) {
        d.hasFan = true; d.fanMode = source.fan.fanMode; d.fanAuto = source.fan.fanAuto;
        d.fanPercent = source.fan.fanPercent; d.fanCurve = source.fan.fanCurve;
    }
}
static inline void profile_editor_source_candidate(const ProfileDocument& source, ProfileEditorCandidate& out) {
    profile_editor_candidate_reset(out);
    out.source.valid = true; out.source.value = out.source.saved = source;
    profile_editor_source_display(source, out.settings);
}
