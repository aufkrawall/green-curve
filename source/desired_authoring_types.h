// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#ifndef GREEN_CURVE_DESIRED_AUTHORING_TYPES_H
#define GREEN_CURVE_DESIRED_AUTHORING_TYPES_H
#include "gpu_wire_types.h"

enum DesiredGraphicsSurface : gc_u32 {
    DESIRED_GRAPHICS_AUTO, DESIRED_GRAPHICS_P0, DESIRED_GRAPHICS_GLOBAL, DESIRED_GRAPHICS_VF_COMPONENT
};
// Resolved runtime authority, separate from portable observations and local
// acknowledgments. Full destination voltages bind indices across restart.
// FROZEN: schema-5 runtime payload. A future layout gets a new type and moves
// the live alias; historical record decoders keep this exact definition.
struct DesiredAuthoringFieldsV1 {
    gc_bool8 pointFrequencyPrecise[128];
    gc_bool8 graphicsPrecise, lockPrecise, identityExclusions, topologyBound;
    gc_u32 graphicsSurface;
    gc_i64 pointFrequencyHz[128];
    gc_i64 graphicsOffsetHz, lockFrequencyHz;
    gc_i64 voltageUv[128];
    gc_bool8 graphicsExcluded[128];
    gc_u32 topologyCount, reserved;
};
using DesiredAuthoringFields = DesiredAuthoringFieldsV1;
static_assert(sizeof(DesiredAuthoringFieldsV1) == 2336, "authoring version 1 is frozen");
static_assert(sizeof(DesiredAuthoringFields) == 2336, "authoring extension requires schema versioning");
#endif
