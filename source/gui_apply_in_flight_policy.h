// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// How Green Curve presents a hardware write that is IN FLIGHT: the tray icon
// theme, the tray tooltip, and the main window's status line.
//
// Do not confuse this with F-PENDING (gui_pending_changes_policy.h).  That is
// "typed into the editor but not applied yet" and is entirely a GUI-side draft
// concept.  This file is about the window between a profile switch / Apply
// being initiated and the service reporting the new settings actually live.
// That window is deliberately several seconds long: the service resets to a
// stock OC baseline, waits for the curve to settle so expected NVIDIA
// boost/temperature drift cannot be mistaken for a failed write, and only then
// writes the new intent.  Without a distinct presentation the tray keeps
// advertising the OLD profile's theme and tooltip for those seconds, which
// reads as "nothing happened".
//
// Every surface derives its wording from GUI_APPLY_IN_FLIGHT_PHRASE, so the
// tray tooltip and the window cannot describe the same state differently.
//
// The transitional theme deliberately outranks the OC/fan themes rather than
// blending with them: mid-write neither the old nor the new OC/fan state is
// truthfully "what the GPU has", so claiming either would be a lie for exactly
// as long as the answer is unknown.

#ifndef GREEN_CURVE_GUI_APPLY_IN_FLIGHT_POLICY_H
#define GREEN_CURVE_GUI_APPLY_IN_FLIGHT_POLICY_H

// platform.h for the bounded gc_appendf the tray formatters already use.
#include "platform.h"

#include <stddef.h>

// The tray icon themes.  Kept here rather than in gpu_core.h -- which is the
// platform-neutral GPU data model shared with the Linux backend and explicitly
// holds no UI state -- next to the rule that chooses between them.
enum {
    TRAY_ICON_STATE_DEFAULT = 0,
    TRAY_ICON_STATE_OC = 1,
    TRAY_ICON_STATE_FAN = 2,
    TRAY_ICON_STATE_OC_FAN = 3,
    // Greyscale transitional theme: a profile switch / Apply has been initiated
    // and the hardware does not hold the result yet.
    TRAY_ICON_STATE_PENDING = 4,
    TRAY_ICON_STATE_COUNT = 5,
};

// The one user-facing phrase, shared by every surface.
#define GUI_APPLY_IN_FLIGHT_PHRASE "changes pending"

static inline int gui_apply_in_flight_tray_icon_state(bool applyInFlight,
    bool customOc, bool customFan) {
    if (applyInFlight) return TRAY_ICON_STATE_PENDING;
    if (customOc && customFan) return TRAY_ICON_STATE_OC_FAN;
    if (customOc) return TRAY_ICON_STATE_OC;
    if (customFan) return TRAY_ICON_STATE_FAN;
    return TRAY_ICON_STATE_DEFAULT;
}

// Tray hover text.  Kept short: the shell truncates a NOTIFYICONDATA tip, and
// the whole point of this state is to be recognized at a glance.
static inline void gui_apply_in_flight_tray_tooltip(char* out, size_t outSize) {
    if (!out || outSize == 0) return;
    out[0] = '\0';
    gc_appendf(out, outSize, 0, "Green Curve - %s...",
        GUI_APPLY_IN_FLIGHT_PHRASE);
}

// Short form: the main-window status line, and the banner's headline.
//
// Deliberately short. The status label is one 18px line placed well into the
// background-service row, so anything longer is simply clipped -- which is part
// of why the first version of this feature was reported as no indicator at all.
static inline void gui_apply_in_flight_status_text(char* out, size_t outSize) {
    if (!out || outSize == 0) return;
    out[0] = '\0';
    gc_appendf(out, outSize, 0, "Applying settings to the GPU - %s",
        GUI_APPLY_IN_FLIGHT_PHRASE);
}

// The banner's second line.  Only the banner has the width to say why the wait
// is deliberate, and saying so is what stops a multi-second apply from reading
// as a hang.
static inline void gui_apply_in_flight_detail_text(char* out, size_t outSize) {
    if (!out || outSize == 0) return;
    out[0] = '\0';
    gc_appendf(out, outSize, 0,
        "The curve is reset and allowed to settle first, so expected boost "
        "drift cannot be mistaken for the new state.");
}

// Repaint cadence for the indeterminate sweep below.  This is PRESENTATION
// ONLY: the authoritative state is the mutation queue's in-flight flag, and the
// timer merely advances a frame counter, so a late, coalesced, or missed tick
// changes how the bar looks and nothing else.  Nothing waits on it and nothing
// is sequenced by it.
#define GUI_APPLY_IN_FLIGHT_FRAME_MS 100u
// One sweep every ~3 s: slow enough not to look frantic during a wait that is
// itself deliberate, fast enough to read as motion rather than a stuck bar.
#define GUI_APPLY_IN_FLIGHT_SWEEP_FRAMES 30u

// The vertical band the banner occupies, in the graph's own content
// coordinates.
//
// Pure so the invariant that actually broke is asserted rather than eyeballed:
// the banner must never reach above the header strip.  Every control in this
// window is a CHILD window, and a child paints OVER its parent rather than
// being clipped by it, so a banner drawn up there does not hide the GPU
// selector -- the selector appears to bleed through the banner.
struct GuiApplyBannerBand {
    bool visible;
    int top;
    int bottom;
};

static inline GuiApplyBannerBand gui_apply_in_flight_banner_band(
    int graphHeight, int headerStripHeight, int bannerHeight) {
    GuiApplyBannerBand band = {};
    if (graphHeight <= 0 || headerStripHeight < 0 || bannerHeight <= 0)
        return band;
    // A graph too short to hold the whole banner below the header strip gets no
    // banner at all.  Shrinking it toward the top is what would put it back
    // over the selector; the tray icon and status line still report the state.
    if (graphHeight < headerStripHeight + bannerHeight) return band;
    band.visible = true;
    band.top = headerStripHeight;
    band.bottom = headerStripHeight + bannerHeight;
    return band;
}

// An indeterminate progress sweep: a bar that enters from the left, crosses,
// and exits right, then repeats.  Indeterminate on purpose -- the apply has no
// meaningful percentage, and inventing one would be a lie about progress.
struct GuiApplySweep {
    int x;      // relative to the track's left edge; may be negative
    int width;
};

static inline GuiApplySweep gui_apply_in_flight_sweep(int trackWidth,
    unsigned int frame) {
    GuiApplySweep sweep = {};
    if (trackWidth <= 0) return sweep;
    sweep.width = trackWidth / 4;
    if (sweep.width < 1) sweep.width = 1;
    // Travel the full track plus the bar's own width so it starts fully off the
    // left edge and ends fully off the right one; a bar that pops in and out at
    // the edges reads as a glitch rather than as motion.
    int travel = trackWidth + sweep.width;
    unsigned int step = frame % GUI_APPLY_IN_FLIGHT_SWEEP_FRAMES;
    sweep.x = (int)(((long long)travel * step) /
        GUI_APPLY_IN_FLIGHT_SWEEP_FRAMES) - sweep.width;
    return sweep;
}

#endif  // GREEN_CURVE_GUI_APPLY_IN_FLIGHT_POLICY_H
