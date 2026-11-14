// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#pragma once
#include "gpu_wire_types.h"

enum GraphicsScalarRecoveryMask : gc_u32 {
    GRAPHICS_SCALAR_RECOVERY_P0 = 1u,
    GRAPHICS_SCALAR_RECOVERY_GLOBAL = 2u,
    GRAPHICS_SCALAR_RECOVERY_ALL = 3u,
};
struct GraphicsScalarRecoveryState {
    gc_u32 covered, uncertain, ownedSurface;
    gc_u32 prepared, attempted;
};
struct GraphicsScalarPreimage {
    gc_u32 validMask, covered, uncertain, ownedSurface;
    int p0MHz, globalMHz;
};
// Permissions and attempt evidence belong to one serialized operation.
struct GraphicsScalarOperation {
    GraphicsScalarRecoveryState& state;
    explicit GraphicsScalarOperation(GraphicsScalarRecoveryState& value) : state(value) {
        state.prepared = state.attempted = 0;
    }
    ~GraphicsScalarOperation() { state.prepared = state.attempted = 0; }
    GraphicsScalarOperation(const GraphicsScalarOperation&) = delete;
    GraphicsScalarOperation& operator=(const GraphicsScalarOperation&) = delete;
};
static inline gc_u32 graphics_scalar_recovery_bit(gc_u32 surface) {
    return surface == CLOCK_OFFSET_SURFACE_P0 ? GRAPHICS_SCALAR_RECOVERY_P0 :
        surface == CLOCK_OFFSET_SURFACE_GLOBAL ? GRAPHICS_SCALAR_RECOVERY_GLOBAL : 0u;
}
static inline bool graphics_scalar_recovery_mask_valid(gc_u32 mask) {
    return (mask & ~(gc_u32)GRAPHICS_SCALAR_RECOVERY_ALL) == 0;
}
template<class Store>
static inline bool graphics_scalar_operation_prepare(GraphicsScalarRecoveryState& state, gc_u32 mask, Store store) {
    if (!graphics_scalar_recovery_mask_valid(mask) || !store()) return false;
    state.prepared = mask; return true;
}
static inline gc_u32 graphics_scalar_recovery_surface(gc_u32 bit) {
    return bit == GRAPHICS_SCALAR_RECOVERY_P0 ? CLOCK_OFFSET_SURFACE_P0 :
        bit == GRAPHICS_SCALAR_RECOVERY_GLOBAL ? CLOCK_OFFSET_SURFACE_GLOBAL : 0u;
}
template<class Read>
static inline bool graphics_scalar_capture(GraphicsScalarRecoveryState& state,
    GraphicsScalarPreimage& output, gc_u32 mask, Read read) {
    if (!graphics_scalar_recovery_mask_valid(mask)) return false;
    GraphicsScalarPreimage next = output;
    for (gc_u32 bit = 1; bit <= GRAPHICS_SCALAR_RECOVERY_GLOBAL; bit <<= 1) {
        if (!(mask & bit)) continue;
        int value = 0;
        if (!read(graphics_scalar_recovery_surface(bit), value)) return false;
        if (bit == GRAPHICS_SCALAR_RECOVERY_P0) next.p0MHz = value;
        else next.globalMHz = value;
        next.validMask |= bit;
    }
    next.covered = state.covered; next.uncertain = state.uncertain;
    next.ownedSurface = state.ownedSurface;
    output = next; return true;
}
template<class Write>
static inline bool graphics_scalar_restore(GraphicsScalarRecoveryState& state,
    const GraphicsScalarPreimage& before, Write write) {
    const gc_u32 attempted = state.attempted;
    if (!graphics_scalar_recovery_mask_valid(attempted)) return false;
    // A successful forward Reset may already have retired its live coverage.
    // A later persistence failure still needs durable rollback coverage.
    state.covered |= attempted; state.uncertain |= attempted;
    if (attempted & ~before.validMask) return false;
    bool ok = true;
    for (gc_u32 bit = 1; bit <= GRAPHICS_SCALAR_RECOVERY_GLOBAL; bit <<= 1) {
        if (!(attempted & bit)) continue;
        const bool restored = write(graphics_scalar_recovery_surface(bit),
            bit == GRAPHICS_SCALAR_RECOVERY_P0 ? before.p0MHz : before.globalMHz);
        if (restored) {
            state.covered = (state.covered & ~bit) | (before.covered & bit);
            state.uncertain = (state.uncertain & ~bit) | (before.uncertain & bit);
        }
        ok &= restored;
    }
    if (ok) {
        state.covered = before.covered; state.uncertain = before.uncertain;
        state.ownedSurface = before.ownedSurface;
    }
    return ok;
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
