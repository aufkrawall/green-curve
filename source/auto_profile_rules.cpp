// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// auto_profile_rules.cpp — pure resolver + INI persistence for auto-profiles.
// The resolver and its helpers use NO OS APIs (only <string.h>/<ctype.h>) so
// they stay unit-testable and reusable; only the config load/save at the bottom
// is Windows-gated (it uses the get/set_config_* INI helpers).

#include "auto_profile_rules.h"
#include <string.h>
#include <ctype.h>

// ---- Pure helpers ----------------------------------------------------------

static char ap_lower(char c) {
    return (char)tolower((unsigned char)c);
}

// ASCII case-insensitive equality (self-contained; avoids depending on the
// Win32-TU streqi_ascii so this file can compile in the pure test harness and a
// future Linux port).
static bool ap_streqi(const char* a, const char* b) {
    if (!a || !b) return false;
    while (*a && *b) {
        if (ap_lower(*a) != ap_lower(*b)) return false;
        ++a; ++b;
    }
    return *a == 0 && *b == 0;
}

bool auto_profile_text_contains_ci(const char* haystack, const char* needle) {
    if (!haystack || !needle) return false;
    if (needle[0] == 0) return false;          // empty pattern is not a wildcard
    if (haystack[0] == 0) return false;
    size_t nlen = strlen(needle);
    for (const char* h = haystack; *h; ++h) {
        size_t i = 0;
        while (i < nlen && h[i] && ap_lower(h[i]) == ap_lower(needle[i])) ++i;
        if (i == nlen) return true;
    }
    return false;
}

const char* auto_profile_match_type_name(AutoProfileMatchType t) {
    switch (t) {
        case AUTO_MATCH_EXE: return "exe";
        case AUTO_MATCH_TITLE: return "title";
        case AUTO_MATCH_CLASS: return "class";
        case AUTO_MATCH_FULLSCREEN: return "fullscreen";
        default: return "none";
    }
}

AutoProfileMatchType auto_profile_match_type_from_name(const char* s) {
    if (!s) return AUTO_MATCH_NONE;
    if (ap_streqi(s, "exe")) return AUTO_MATCH_EXE;
    if (ap_streqi(s, "title")) return AUTO_MATCH_TITLE;
    if (ap_streqi(s, "class")) return AUTO_MATCH_CLASS;
    if (ap_streqi(s, "fullscreen")) return AUTO_MATCH_FULLSCREEN;
    return AUTO_MATCH_NONE;
}

static bool ap_slot_in_range(int slot) {
    return slot >= 1 && slot <= CONFIG_NUM_SLOTS;
}

bool auto_profile_rule_active(const AutoProfileRule* rule, int ruleIndex,
                              const ForegroundInfo* fg, const ProcessPresence* presence) {
    if (!rule) return false;
    if (!ap_slot_in_range(rule->slot)) return false;
    switch (rule->matchType) {
        case AUTO_MATCH_EXE:
            if (rule->pattern[0] == 0) return false;
            if (rule->requireFocus) {
                return fg && fg->valid && fg->exeName[0] != 0 &&
                       ap_streqi(fg->exeName, rule->pattern);
            }
            // Focus-optional: rely on the detection layer's presence scan.
            if (!presence || ruleIndex < 0 || ruleIndex >= AUTO_PROFILE_MAX_RULES) return false;
            return presence->rulePresent[ruleIndex];
        case AUTO_MATCH_TITLE:
            return fg && fg->valid && auto_profile_text_contains_ci(fg->title, rule->pattern);
        case AUTO_MATCH_CLASS:
            return fg && fg->valid && auto_profile_text_contains_ci(fg->className, rule->pattern);
        case AUTO_MATCH_FULLSCREEN:
            return fg && fg->valid && fg->isFullscreen;
        default:
            return false;
    }
}

int resolve_auto_profile_slot(const AutoProfileConfig* cfg,
                              const ForegroundInfo* fg, const ProcessPresence* presence) {
    if (!cfg) return CONFIG_DEFAULT_SLOT;
    int n = cfg->ruleCount;
    if (n < 0) n = 0;
    if (n > AUTO_PROFILE_MAX_RULES) n = AUTO_PROFILE_MAX_RULES;
    for (int i = 0; i < n; i++) {
        if (auto_profile_rule_active(&cfg->rules[i], i, fg, presence)) {
            return ap_slot_in_range(cfg->rules[i].slot) ? cfg->rules[i].slot : CONFIG_DEFAULT_SLOT;
        }
    }
    return ap_slot_in_range(cfg->defaultSlot) ? cfg->defaultSlot : CONFIG_DEFAULT_SLOT;
}

static int ap_clampi(int v, int lo, int hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

void auto_profile_config_normalize(AutoProfileConfig* cfg) {
    if (!cfg) return;
    if (!ap_slot_in_range(cfg->defaultSlot)) cfg->defaultSlot = CONFIG_DEFAULT_SLOT;
    cfg->switchDebounceMs = ap_clampi(cfg->switchDebounceMs,
        AUTO_PROFILE_MIN_DEBOUNCE_MS, AUTO_PROFILE_MAX_DEBOUNCE_MS);
    cfg->minSwitchIntervalMs = ap_clampi(cfg->minSwitchIntervalMs,
        AUTO_PROFILE_MIN_INTERVAL_FLOOR_MS, AUTO_PROFILE_MAX_INTERVAL_MS);
    if (cfg->ruleCount < 0) cfg->ruleCount = 0;
    if (cfg->ruleCount > AUTO_PROFILE_MAX_RULES) cfg->ruleCount = AUTO_PROFILE_MAX_RULES;
    for (int i = 0; i < AUTO_PROFILE_MAX_RULES; i++) {
        AutoProfileRule* r = &cfg->rules[i];
        r->pattern[AUTO_PROFILE_PATTERN_MAX - 1] = 0;
        if (r->matchType < AUTO_MATCH_NONE || r->matchType > AUTO_MATCH_FULLSCREEN) {
            r->matchType = AUTO_MATCH_NONE;
        }
        if (!ap_slot_in_range(r->slot)) r->slot = CONFIG_DEFAULT_SLOT;
    }
}

void auto_profile_config_set_defaults(AutoProfileConfig* cfg) {
    if (!cfg) return;
    memset(cfg, 0, sizeof(*cfg));
    cfg->enabled = false;
    cfg->defaultSlot = CONFIG_DEFAULT_SLOT;
    cfg->switchDebounceMs = AUTO_PROFILE_DEFAULT_DEBOUNCE_MS;
    cfg->minSwitchIntervalMs = AUTO_PROFILE_DEFAULT_MIN_INTERVAL_MS;
    cfg->suppressWhenWindowOpen = true;
    cfg->ruleCount = 0;
    for (int i = 0; i < AUTO_PROFILE_MAX_RULES; i++) {
        cfg->rules[i].matchType = AUTO_MATCH_NONE;
        cfg->rules[i].slot = CONFIG_DEFAULT_SLOT;
        cfg->rules[i].requireFocus = true;
    }
}

// ---- Windows INI persistence ----------------------------------------------

#if defined(_WIN32)
#include "app_shared.h"   // get/set_config_*, StringCch*, ARRAY_COUNT

#define AUTO_PROFILE_SECTION "auto_profiles"

static void ap_rule_section(char* out, size_t outSize, int oneBasedIndex) {
    StringCchPrintfA(out, outSize, "auto_rule%d", oneBasedIndex);
}

void auto_profile_config_load(const char* path, AutoProfileConfig* cfg) {
    if (!cfg) return;
    auto_profile_config_set_defaults(cfg);
    if (!path) return;
    HANDLE configMutex = nullptr;
    if (!enter_config_storage_lock(&configMutex)) return;

    cfg->enabled = get_config_int(path, AUTO_PROFILE_SECTION, "enabled", 0) != 0;
    cfg->defaultSlot = get_config_int(path, AUTO_PROFILE_SECTION, "default_slot", CONFIG_DEFAULT_SLOT);
    cfg->switchDebounceMs = get_config_int(path, AUTO_PROFILE_SECTION, "switch_debounce_ms",
        AUTO_PROFILE_DEFAULT_DEBOUNCE_MS);
    cfg->minSwitchIntervalMs = get_config_int(path, AUTO_PROFILE_SECTION, "min_switch_interval_ms",
        AUTO_PROFILE_DEFAULT_MIN_INTERVAL_MS);
    cfg->suppressWhenWindowOpen = get_config_int(path, AUTO_PROFILE_SECTION, "suppress_when_window_open", 1) != 0;

    int ruleCount = get_config_int(path, AUTO_PROFILE_SECTION, "rule_count", 0);
    if (ruleCount < 0) ruleCount = 0;
    if (ruleCount > AUTO_PROFILE_MAX_RULES) ruleCount = AUTO_PROFILE_MAX_RULES;
    cfg->ruleCount = ruleCount;

    for (int i = 0; i < ruleCount; i++) {
        char section[32] = {};
        ap_rule_section(section, ARRAY_COUNT(section), i + 1);
        AutoProfileRule* r = &cfg->rules[i];
        char typeName[32] = {};
        get_config_string(path, section, "match_type", "none", typeName, ARRAY_COUNT(typeName));
        r->matchType = auto_profile_match_type_from_name(typeName);
        get_config_string(path, section, "pattern", "", r->pattern, ARRAY_COUNT(r->pattern));
        r->requireFocus = get_config_int(path, section, "require_focus", 1) != 0;
        r->slot = get_config_int(path, section, "slot", CONFIG_DEFAULT_SLOT);
    }

    auto_profile_config_normalize(cfg);
    leave_config_storage_lock(configMutex);
}

bool auto_profile_config_save(const char* path, const AutoProfileConfig* cfg,
    const char (*hotkeys)[64]) {
    if (!path || !cfg) return false;
    AutoProfileConfig norm = *cfg;
    auto_profile_config_normalize(&norm);

    const size_t sectionsCapacity = 65536;
    char* sections = (char*)calloc(sectionsCapacity, 1);
    if (!sections) return false;
    size_t used = 0;
    bool built = true;
    auto append = [&](const char* fmt, ...) {
        if (!built || used >= sectionsCapacity - 1) {
            built = false;
            return;
        }
        va_list args;
        va_start(args, fmt);
        int count = _vsnprintf_s(sections + used, sectionsCapacity - used,
            _TRUNCATE, fmt, args);
        va_end(args);
        if (count < 0) built = false;
        else used += (size_t)count;
    };
    auto safeValue = [](const char* value) {
        return value && !strchr(value, '\r') && !strchr(value, '\n');
    };

    append("[%s]\r\n", AUTO_PROFILE_SECTION);
    append("enabled=%d\r\n", norm.enabled ? 1 : 0);
    append("default_slot=%d\r\n", norm.defaultSlot);
    append("switch_debounce_ms=%d\r\n", norm.switchDebounceMs);
    append("min_switch_interval_ms=%d\r\n", norm.minSwitchIntervalMs);
    append("suppress_when_window_open=%d\r\n",
        norm.suppressWhenWindowOpen ? 1 : 0);
    append("rule_count=%d\r\n\r\n", norm.ruleCount);

    char ruleSectionNames[AUTO_PROFILE_MAX_RULES][32] = {};
    const char* replaced[AUTO_PROFILE_MAX_RULES + 2] = {};
    int replaceCount = 0;
    replaced[replaceCount++] = AUTO_PROFILE_SECTION;
    for (int i = 0; i < AUTO_PROFILE_MAX_RULES; ++i) {
        ap_rule_section(ruleSectionNames[i], ARRAY_COUNT(ruleSectionNames[i]), i + 1);
        replaced[replaceCount++] = ruleSectionNames[i];
        if (i >= norm.ruleCount) continue;
        const AutoProfileRule* rule = &norm.rules[i];
        if (!safeValue(rule->pattern)) {
            free(sections);
            return false;
        }
        append("[%s]\r\nmatch_type=%s\r\npattern=%s\r\nrequire_focus=%d\r\nslot=%d\r\n\r\n",
            ruleSectionNames[i], auto_profile_match_type_name(rule->matchType),
            rule->pattern, rule->requireFocus ? 1 : 0, rule->slot);
    }
    if (hotkeys) {
        replaced[replaceCount++] = "hotkeys";
        append("[hotkeys]\r\n");
        for (int slot = 1; slot <= CONFIG_NUM_SLOTS; ++slot) {
            if (!safeValue(hotkeys[slot])) {
                free(sections);
                return false;
            }
            if (hotkeys[slot][0]) append("slot%d=%s\r\n", slot, hotkeys[slot]);
        }
        append("\r\n");
    }
    if (!built) {
        free(sections);
        return false;
    }

    HANDLE configMutex = nullptr;
    if (!enter_config_storage_lock(&configMutex)) {
        free(sections);
        return false;
    }
    char err[256] = {};
    bool ok = write_config_sections_atomic(path, sections, replaced,
        replaceCount, err, sizeof(err));
    if (ok) {
        (void)gc_WritePrivateProfileStringUtf8(nullptr, nullptr, nullptr, path);
        invalidate_tray_profile_cache();
    }
    leave_config_storage_lock(configMutex);
    free(sections);
    return ok;
}
#endif // _WIN32
