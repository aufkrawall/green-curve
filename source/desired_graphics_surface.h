// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#pragma once
#include "desired_authoring_types.h"

template <class D> static inline gc_u32 desired_named_graphics_surface(const D& d) {
    if (!d.hasGpuOffset || !d.authoring.graphicsPrecise) return CLOCK_OFFSET_SURFACE_UNSPECIFIED;
    return d.authoring.graphicsSurface == DESIRED_GRAPHICS_P0 ? CLOCK_OFFSET_SURFACE_P0
        : d.authoring.graphicsSurface == DESIRED_GRAPHICS_GLOBAL ? CLOCK_OFFSET_SURFACE_GLOBAL
        : CLOCK_OFFSET_SURFACE_UNSPECIFIED;
}
