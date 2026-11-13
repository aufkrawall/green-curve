// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#pragma once
#include "gpu_wire_types.h"

enum GraphicsScalarRecoveryMask : gc_u32 {
    GRAPHICS_SCALAR_RECOVERY_P0 = 1u,
    GRAPHICS_SCALAR_RECOVERY_GLOBAL = 2u,
    GRAPHICS_SCALAR_RECOVERY_ALL = 3u,
};
static inline gc_u32 graphics_scalar_recovery_bit(gc_u32 surface) {
    return surface == CLOCK_OFFSET_SURFACE_P0 ? GRAPHICS_SCALAR_RECOVERY_P0 :
        surface == CLOCK_OFFSET_SURFACE_GLOBAL ? GRAPHICS_SCALAR_RECOVERY_GLOBAL : 0u;
}
static inline bool graphics_scalar_recovery_mask_valid(gc_u32 mask) {
    return (mask & ~(gc_u32)GRAPHICS_SCALAR_RECOVERY_ALL) == 0;
}
static inline bool graphics_scalar_recovery_adopt(gc_u32& live, gc_u32 pending, bool sameTarget) {
    if (!graphics_scalar_recovery_mask_valid(live) || !graphics_scalar_recovery_mask_valid(pending) ||
        (pending && !sameTarget)) return false;
    live |= pending; return true;
}
// Publish recovery coverage only after durable preparation. A store failure
// cannot authorize a setter, and an already covered surface needs no new I/O.
template<class Store>
static inline bool graphics_scalar_recovery_prepare(gc_u32& covered, gc_u32 surface, Store store) {
    const gc_u32 bit = graphics_scalar_recovery_bit(surface);
    if (!bit || !graphics_scalar_recovery_mask_valid(covered)) return false;
    const gc_u32 next = covered | bit;
    if (next != covered && !store(next)) return false;
    covered = next; return true;
}
