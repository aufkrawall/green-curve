// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#ifndef GREEN_CURVE_DESIRED_SETTINGS_AUTHORING_SCHEMA_H
#define GREEN_CURVE_DESIRED_SETTINGS_AUTHORING_SCHEMA_H
#include <stddef.h>
// FROZEN: schema 5 extends the exact-scalar prefix with bound authoring intent.
struct DesiredSettingsSchema5 {
    DesiredSettingsSchema4 prefix;
    DesiredAuthoringFieldsV1 authoring;
};
static_assert(sizeof(DesiredSettingsSchema5) == 4488, "desired schema 5 is frozen");
static_assert(offsetof(DesiredSettingsSchema5, authoring) == 2152, "schema-5 prefix moved");

static inline void desired_settings_widen_schema5(DesiredSettings* out, const DesiredSettingsSchema5* in) {
    if (!out || !in) return;
    desired_settings_widen_schema4(out, &in->prefix);
    out->authoring = in->authoring;
}
static inline void desired_settings_narrow_schema5(DesiredSettingsSchema5* out, const DesiredSettings* in) {
    if (!out || !in) return;
    *out = {};
    desired_settings_narrow_schema4(&out->prefix, in);
    out->authoring = in->authoring;
}
#endif
