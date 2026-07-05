// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// auto_profile_rules.h — pure, platform-neutral data model + resolver for the
// auto-profile feature (automatically select a GPU tuning profile based on the
// foreground process / window).  This header pulls in NO OS headers so the
// resolver stays unit-testable and reusable by a future Linux port; the Win32
// foreground/process probing lives in auto_profile_detect.cpp and only feeds
// the neutral ForegroundInfo / ProcessPresence structs to resolve_*().

#ifndef GREEN_CURVE_AUTO_PROFILE_RULES_H
#define GREEN_CURVE_AUTO_PROFILE_RULES_H

#include "gpu_core.h"   // CONFIG_NUM_SLOTS, CONFIG_DEFAULT_SLOT

// A modest cap keeps the config bounded, the ordered first-match scan cheap, and
// the rule-editor dialog a single fixed grid (no scrolling / data loss).
#define AUTO_PROFILE_MAX_RULES     8
#define AUTO_PROFILE_PATTERN_MAX   128
#define AUTO_PROFILE_TITLE_MAX     256
#define AUTO_PROFILE_CLASS_MAX     128

// Default hysteresis knobs.  The debounce coalesces alt-tab bursts; the minimum
// switch interval enforces a cooldown >= the ~3s single-apply floor so rapid
// foreground churn can never queue up serialized 3s applies on the service pipe.
#define AUTO_PROFILE_DEFAULT_DEBOUNCE_MS      800
#define AUTO_PROFILE_DEFAULT_MIN_INTERVAL_MS  4000
#define AUTO_PROFILE_MIN_DEBOUNCE_MS          100
#define AUTO_PROFILE_MAX_DEBOUNCE_MS          10000
#define AUTO_PROFILE_MIN_INTERVAL_FLOOR_MS    1000
#define AUTO_PROFILE_MAX_INTERVAL_MS          60000

enum AutoProfileMatchType {
    AUTO_MATCH_NONE = 0,
    AUTO_MATCH_EXE = 1,        // pattern = exe base name (e.g. "game.exe"), case-insensitive equality
    AUTO_MATCH_TITLE = 2,      // pattern = case-insensitive substring of the foreground window title
    AUTO_MATCH_CLASS = 3,      // pattern = case-insensitive substring of the foreground window class
    AUTO_MATCH_FULLSCREEN = 4, // no pattern; matches when the foreground window is fullscreen
};

struct AutoProfileRule {
    AutoProfileMatchType matchType;
    char pattern[AUTO_PROFILE_PATTERN_MAX];
    // requireFocus is meaningful only for EXE rules: when true the process must
    // be the FOREGROUND app; when false the rule is active whenever the process
    // is running anywhere.  Title/class/fullscreen matchers are foreground-by-
    // nature, so requireFocus is ignored for them (treated as always-focus).
    bool requireFocus;
    int slot;                  // target profile slot, 1..CONFIG_NUM_SLOTS
};

struct AutoProfileConfig {
    bool enabled;
    int defaultSlot;           // revert-to slot when nothing matches (1..CONFIG_NUM_SLOTS)
    int switchDebounceMs;
    int minSwitchIntervalMs;
    bool suppressWhenWindowOpen; // do not auto-switch while the main GUI window is visible/being edited
    int ruleCount;
    AutoProfileRule rules[AUTO_PROFILE_MAX_RULES];
};

// Neutral description of the current foreground window, filled by the Win32
// detection layer.  exeName is the base file name only (e.g. "game.exe"); it may
// be empty when the process image could not be resolved (e.g. an elevated game
// from an unelevated GUI) — in that case only title/class/fullscreen rules can
// match the foreground.
struct ForegroundInfo {
    bool valid;
    char exeName[AUTO_PROFILE_PATTERN_MAX];
    char title[AUTO_PROFILE_TITLE_MAX];
    char className[AUTO_PROFILE_CLASS_MAX];
    bool isFullscreen;
};

// Presence of focus-optional EXE rules' processes.  The detection layer sets
// rulePresent[i] = true when rule i is a requireFocus==false EXE rule whose
// executable is currently running anywhere (foreground or not).  Indexed by rule
// so the resolver never enumerates processes itself.
struct ProcessPresence {
    bool rulePresent[AUTO_PROFILE_MAX_RULES];
};

// ---- Pure helpers (no OS deps) --------------------------------------------

// ASCII case-insensitive substring test.  Empty needle never matches (an empty
// pattern is a misconfiguration, not a wildcard).
bool auto_profile_text_contains_ci(const char* haystack, const char* needle);

const char* auto_profile_match_type_name(AutoProfileMatchType t);
AutoProfileMatchType auto_profile_match_type_from_name(const char* s);

// Is a single rule "active" given the foreground state?  Exposed for testing.
bool auto_profile_rule_active(const AutoProfileRule* rule, int ruleIndex,
                              const ForegroundInfo* fg, const ProcessPresence* presence);

// Resolve the target slot: first active rule wins (ordered scan); else the
// configured defaultSlot.  Never returns a slot outside 1..CONFIG_NUM_SLOTS.
int resolve_auto_profile_slot(const AutoProfileConfig* cfg,
                              const ForegroundInfo* fg, const ProcessPresence* presence);

// Clamp/normalize a loaded config to valid ranges (slots, debounce, interval,
// rule count, per-rule slots and match types).  Idempotent.
void auto_profile_config_normalize(AutoProfileConfig* cfg);

void auto_profile_config_set_defaults(AutoProfileConfig* cfg);

#if defined(_WIN32)
// INI persistence (Windows only; uses the get/set_config_* helpers).
void auto_profile_config_load(const char* path, AutoProfileConfig* cfg);
bool auto_profile_config_save(const char* path, const AutoProfileConfig* cfg);
#endif

#endif // GREEN_CURVE_AUTO_PROFILE_RULES_H
