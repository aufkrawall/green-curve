// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
// Diagnostic shape of the last Linux VF read, kept even when the snapshot is
// refused. A refused snapshot publishes zero points and one reason; this adds
// the mask extent, where the graphics run ends and what follows it, so a
// single probe report from a new family shows why its table did not validate.

#ifndef GREEN_CURVE_LINUX_VF_LAYOUT_SUMMARY_H
#define GREEN_CURVE_LINUX_VF_LAYOUT_SUMMARY_H

#include "linux_vf_validation.h"
#include "platform.h"

struct LinuxVfLayoutSummary {
    bool maskValid;         // getInfo answered with the expected version
    int maskBits;
    int maskFirst;          // -1 when no bit is set
    int maskLast;
    bool statusValid;       // getStatus answered with the expected version/mask
    int rawPopulated;       // populated points in the whole read window
    int graphicsPopulated;  // populated points kept as the graphics run
    int graphicsUnmasked;   // kept points whose mask bit is clear
    int firstIndex;         // first/last kept point, -1 when none
    int lastIndex;
    VFCurvePoint first;
    VFCurvePoint last;
    int droppedPopulated;   // populated points past the graphics run
    int firstDroppedIndex;  // -1 when none
    VFCurvePoint firstDropped;
};

struct LinuxVfLayoutText {
    char text[224];
};

static inline void linux_vf_layout_reset(LinuxVfLayoutSummary* s) {
    if (!s) return;
    *s = {};
    s->maskFirst = -1;
    s->maskLast = -1;
    s->firstIndex = -1;
    s->lastIndex = -1;
    s->firstDroppedIndex = -1;
}

static inline void linux_vf_layout_note_mask(LinuxVfLayoutSummary* s,
                                             const unsigned char* mask,
                                             unsigned int maskBytes) {
    if (!s || !mask) return;
    s->maskValid = true;
    s->maskBits = 0;
    s->maskFirst = -1;
    s->maskLast = -1;
    for (unsigned int bit = 0; bit < maskBytes * 8u; ++bit) {
        if ((mask[bit / 8u] & (1u << (bit % 8u))) == 0) continue;
        if (s->maskFirst < 0) s->maskFirst = (int)bit;
        s->maskLast = (int)bit;
        ++s->maskBits;
    }
}

// `raw` is the status window BEFORE the graphics-run trim. `mask` is optional;
// without it graphicsUnmasked stays zero.
static inline void linux_vf_layout_note_status(LinuxVfLayoutSummary* s,
                                               const VFCurvePoint* raw,
                                               int count,
                                               const unsigned char* mask,
                                               unsigned int maskBytes) {
    if (!s || !raw || count <= 0) return;
    if (count > VF_NUM_POINTS) count = VF_NUM_POINTS;
    s->statusValid = true;
    s->rawPopulated = 0;
    s->graphicsPopulated = 0;
    s->graphicsUnmasked = 0;
    s->droppedPopulated = 0;
    s->firstIndex = s->lastIndex = s->firstDroppedIndex = -1;
    const int domainEnd = linux_vf_graphics_domain_length(raw, count);
    for (int i = 0; i < count; ++i) {
        if (raw[i].freq_kHz == 0) continue;
        ++s->rawPopulated;
        if (i >= domainEnd) {
            if (s->firstDroppedIndex < 0) {
                s->firstDroppedIndex = i;
                s->firstDropped = raw[i];
            }
            ++s->droppedPopulated;
            continue;
        }
        if (s->firstIndex < 0) {
            s->firstIndex = i;
            s->first = raw[i];
        }
        s->lastIndex = i;
        s->last = raw[i];
        ++s->graphicsPopulated;
        if (mask && (unsigned int)i / 8u < maskBytes &&
            (mask[i / 8] & (1u << (i % 8))) == 0)
            ++s->graphicsUnmasked;
    }
}

// One stable line, e.g. "mask=83[0..82] status=83 graphics=80[0..79]
// 600MHz@650mV..1785MHz@1045mV unmasked=0 dropped=3[first 80=405MHz@540mV]".
static inline LinuxVfLayoutText linux_vf_layout_text(
    const LinuxVfLayoutSummary* s) {
    LinuxVfLayoutText out = {};
    if (!s || !s->maskValid) {
        gc_strlcpy(out.text, sizeof(out.text), "mask=none status=not-read");
        return out;
    }
    int used = s->maskBits > 0
        ? gc_snprintf(out.text, sizeof(out.text), "mask=%d[%d..%d]",
                      s->maskBits, s->maskFirst, s->maskLast)
        : gc_snprintf(out.text, sizeof(out.text), "mask=0");
    if (used < 0 || (size_t)used >= sizeof(out.text)) return out;
    char* tail = out.text + used;
    const size_t tailSize = sizeof(out.text) - (size_t)used;
    if (!s->statusValid) {
        gc_strlcpy(tail, tailSize, " status=not-read");
        return out;
    }
    if (s->graphicsPopulated <= 0) {
        gc_snprintf(tail, tailSize, " status=%d graphics=0 dropped=%d",
                    s->rawPopulated, s->droppedPopulated);
        return out;
    }
    int more = gc_snprintf(tail, tailSize,
        " status=%d graphics=%d[%d..%d] %uMHz@%umV..%uMHz@%umV unmasked=%d"
        " dropped=%d",
        s->rawPopulated, s->graphicsPopulated, s->firstIndex, s->lastIndex,
        s->first.freq_kHz / 1000u, s->first.volt_uV / 1000u,
        s->last.freq_kHz / 1000u, s->last.volt_uV / 1000u,
        s->graphicsUnmasked, s->droppedPopulated);
    if (more < 0 || (size_t)more >= tailSize || s->firstDroppedIndex < 0)
        return out;
    gc_snprintf(tail + more, tailSize - (size_t)more,
                "[first %d=%uMHz@%umV]", s->firstDroppedIndex,
                s->firstDropped.freq_kHz / 1000u,
                s->firstDropped.volt_uV / 1000u);
    return out;
}

#endif // GREEN_CURVE_LINUX_VF_LAYOUT_SUMMARY_H
