// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// How an available update is SHOWN, as pure functions.
//
// ## Why this exists as its own policy
//
// Until this header the only surface that mentioned an update at all was the
// tray context menu, which requires a right-click to see.  The main window's
// Updates button, the tray icon and the tray tooltip were all identical whether
// the machine was up to date or had a verified installer staged and waiting.  A
// user who never right-clicks the tray -- which is most of them -- was never
// told anything.
//
// Three surfaces now answer to the same decision, so they cannot disagree:
//
//   * the Updates button in the main window, outlined in F-PENDING orange, the
//     same colour the fan-curve button already uses for "there is something
//     here you have not dealt with";
//   * the tray tooltip, which gains a short suffix;
//   * the tray context menu entry, which was already conditional.
//
// Keeping the decision here rather than in each of the three means adding a
// fourth surface later cannot introduce a disagreement, and it means the rules
// are testable without a window, a tray or a service.
//
// ## What is worth alerting about, and what is not
//
// Only decisions the user can ACT on raise an alert:
//
// | Decision | Alert | Why |
// |---|---|---|
// | `AVAILABLE` | yes | Install update is one click away. |
// | `MANUAL_REQUIRED` | yes | Newer, but below the release's `min_from` floor, so it must be installed by hand. The user is stranded until they do, which makes this the case they most need telling about -- and it was previously the quietest, mentioned only inside the dialog. |
// | `NO_ASSET` | **no** | "The latest release has no build for this machine's architecture" is a packaging fault on our side. There is no action the user can take, and a daily unactionable badge is nagging, not informing. The dialog still says it. |
// | `UP_TO_DATE`, `REJECTED` | no | Nothing happened. |
//
// A portable `.7z` copy is deliberately NOT excluded.  It cannot install in
// place, but its user still wants to know a new version exists -- the tray
// entry and the button open the dialog, which explains the portable case and
// offers the releases page.  Suppressing the alert there would mean portable
// users are the only ones who never learn about updates at all.

#ifndef GREEN_CURVE_UPDATE_PRESENTATION_POLICY_H
#define GREEN_CURVE_UPDATE_PRESENTATION_POLICY_H

#include <stddef.h>

#include "update_manifest_policy.h"
#include "update_schedule_policy.h"

enum GcUpdateAlert {
    GC_UPDATE_ALERT_NONE = 0,
    // Installable from the dialog: orange button, tray entry, tooltip suffix.
    GC_UPDATE_ALERT_AVAILABLE = 1,
    // Newer, but the installed build is below the release's declared floor, so
    // the user has to fetch it themselves.  Same three surfaces, different
    // wording -- offering "Update to 0.30..." for something the updater will
    // refuse to install would be a worse lie than saying nothing.
    GC_UPDATE_ALERT_MANUAL = 2,
};

// `hasVersion` is passed rather than the string itself so this stays free of
// string handling: every surface has to render the version anyway, and a
// decision that claimed AVAILABLE with no version to show would produce
// "Update to ..." with a hole in it.
static inline GcUpdateAlert gc_update_alert_kind(GcUpdateDecision decision,
                                                 bool hasVersion) {
    if (!hasVersion) return GC_UPDATE_ALERT_NONE;
    if (decision == GC_UPDATE_DECISION_AVAILABLE) return GC_UPDATE_ALERT_AVAILABLE;
    if (decision == GC_UPDATE_DECISION_MANUAL_REQUIRED) return GC_UPDATE_ALERT_MANUAL;
    return GC_UPDATE_ALERT_NONE;
}

// The tray menu entry's caption.  Returns false (and leaves `out` empty) when
// there is nothing to show, which is what keeps the menu from growing a
// permanently present item -- the rule the entry was built under.
static inline bool gc_update_tray_menu_label(GcUpdateAlert alert,
                                             const char* version,
                                             char* out, size_t outSize) {
    if (!out || outSize == 0) return false;
    out[0] = '\0';
    if (alert == GC_UPDATE_ALERT_NONE || !version || !version[0]) return false;

    const char* prefix = (alert == GC_UPDATE_ALERT_MANUAL)
                             ? "Get update "
                             : "Update to ";
    const char* suffix = (alert == GC_UPDATE_ALERT_MANUAL)
                             ? " (manual install)..."
                             : "...";
    size_t prefixLen = strlen(prefix);
    size_t versionLen = strlen(version);
    size_t suffixLen = strlen(suffix);
    if (prefixLen + versionLen + suffixLen + 1 > outSize) {
        // A caption that does not fit is dropped rather than truncated: a
        // half-written version number in a menu is worse than the button and
        // the tooltip carrying the same news on their own.
        return false;
    }
    memcpy(out, prefix, prefixLen);
    memcpy(out + prefixLen, version, versionLen);
    memcpy(out + prefixLen + versionLen, suffix, suffixLen + 1);
    return true;
}

// The tooltip suffix.  Short on purpose: NOTIFYICONDATA's szTip is 128
// characters and the profile name already in there is user-chosen and can be
// long, so this has to be the cheap half of the pair.
static inline bool gc_update_tray_tooltip_suffix(GcUpdateAlert alert,
                                                 const char* version,
                                                 char* out, size_t outSize) {
    if (!out || outSize == 0) return false;
    out[0] = '\0';
    if (alert == GC_UPDATE_ALERT_NONE || !version || !version[0]) return false;

    const char* prefix = " | Update ";
    const char* suffix = (alert == GC_UPDATE_ALERT_MANUAL) ? " (manual)" : "";
    size_t prefixLen = strlen(prefix);
    size_t versionLen = strlen(version);
    size_t suffixLen = strlen(suffix);
    if (prefixLen + versionLen + suffixLen + 1 > outSize) return false;
    memcpy(out, prefix, prefixLen);
    memcpy(out + prefixLen, version, versionLen);
    memcpy(out + prefixLen + versionLen, suffix, suffixLen + 1);
    return true;
}

// Join the tooltip's ordinary text with the update suffix.
//
// The SUFFIX wins when the two do not both fit.  That inversion is the whole
// point: the base text describes a state the user can also read off the window,
// while the suffix is the only passive notice that an update exists, so letting
// a long profile name push it out would reintroduce exactly the silence this
// header exists to end.  The base is truncated at a character boundary and the
// result is always terminated.
static inline void gc_update_compose_tray_tooltip(const char* base,
                                                  const char* suffix,
                                                  char* out, size_t outSize) {
    if (!out || outSize == 0) return;
    out[0] = '\0';
    size_t baseLen = base ? strlen(base) : 0;
    size_t suffixLen = suffix ? strlen(suffix) : 0;

    // No room for the suffix at all: the base alone is what fits.
    if (suffixLen + 1 > outSize) {
        size_t copy = baseLen;
        if (copy + 1 > outSize) copy = outSize - 1;
        if (copy && base) memcpy(out, base, copy);
        out[copy] = '\0';
        return;
    }

    size_t roomForBase = outSize - 1 - suffixLen;
    size_t baseCopy = baseLen < roomForBase ? baseLen : roomForBase;
    if (baseCopy && base) memcpy(out, base, baseCopy);
    if (suffixLen) memcpy(out + baseCopy, suffix, suffixLen);
    out[baseCopy + suffixLen] = '\0';
}

// ---------------------------------------------------------------------------
// The first-run question
// ---------------------------------------------------------------------------

// Whether to ask the user, once, whether Green Curve may check for updates.
//
// `GC_UPDATE_AUTO_CHECK_UNSET` was always meant to be a question, not a
// permanent state: update_schedule_policy.h says the first run "has to ask (or
// at minimum state) before the first request goes out".  Nothing ever asked.
// The only place the unset state was mentioned was a line of body text inside
// the Updates dialog, so a user who never opened that dialog got no checks
// ever, and no hint that updates existed at all.  UNSET is the shipped default,
// so that was every user.
//
// The gates are all about not being obnoxious, because this is a modal question
// nobody requested:
//
//   * `stateKnown` -- the service answered.  Asking about a preference we
//     cannot then save produces a question whose answer is discarded.
//   * `windowInteractive` -- the main window is visible and enabled.  A logon
//     start goes straight to the tray, and throwing a dialog at somebody who is
//     watching their desktop appear is exactly the behaviour that makes people
//     distrust an updater.  It waits until they open the window.
//   * `alreadyAsked` -- per process, so a failed save (service stopping, pipe
//     busy) cannot turn the next poll tick into a second dialog.  The answer
//     itself is machine scope and persisted, so a saved answer ends it for
//     good on every account.
static inline bool gc_update_should_prompt_auto_check(GcUpdateAutoCheck setting,
                                                      bool stateKnown,
                                                      bool windowInteractive,
                                                      bool alreadyAsked) {
    if (alreadyAsked) return false;
    if (!stateKnown) return false;
    if (!windowInteractive) return false;
    return setting == GC_UPDATE_AUTO_CHECK_UNSET;
}

#endif // GREEN_CURVE_UPDATE_PRESENTATION_POLICY_H
