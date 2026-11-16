// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#ifndef GREEN_CURVE_PROFILE_DESIRED_INI_EXT_H
#define GREEN_CURVE_PROFILE_DESIRED_INI_EXT_H

#include "gpu_core.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static inline void desired_ini_set_err(char* err, size_t errSize, const char* msg) {
    if (!err || errSize == 0) return;
    snprintf(err, errSize, "%s", msg ? msg : "");
    err[errSize - 1] = '\0';
}

static inline bool profile_parse_i64_strict(const char* text, gc_i64* out) {
    if (!text || !out) return false;
    while (*text == ' ' || *text == '\t') ++text;
    if (!*text) return false;
    const char* p = text;
    if (*p == '+' || *p == '-') ++p;
    if (*p < '0' || *p > '9') return false;
    while (*p >= '0' && *p <= '9') ++p;
    while (*p == ' ' || *p == '\t') ++p;
    if (*p != '\0') return false;
    errno = 0;
    char* end = nullptr;
    long long parsed = strtoll(text, &end, 10);
    if (errno == ERANGE || !end || end == text) return false;
    while (*end == ' ' || *end == '\t') ++end;
    if (*end != '\0') return false;
    *out = (gc_i64)parsed;
    return true;
}

static inline bool profile_parse_u32_strict(const char* text, gc_u32* out) {
    gc_i64 v = 0;
    if (!out || !profile_parse_i64_strict(text, &v) || v < 0 || v > 0xFFFFFFFFLL)
        return false;
    *out = (gc_u32)v;
    return true;
}

static inline bool profile_parse_bool8_strict(const char* text, gc_bool8* out) {
    gc_i64 v = 0;
    if (!out || !profile_parse_i64_strict(text, &v) || (v != 0 && v != 1))
        return false;
    *out = v ? (gc_bool8)1u : (gc_bool8)0u;
    return true;
}

template <class Base, class Over>
static inline void desired_merge_authoring_override(Base* base, const Over* over) {
    if (!base || !over) return;
    if (over->applicationScope != DESIRED_SCOPE_LEGACY)
        base->applicationScope = over->applicationScope;
    if (over->hasGpuOffset) {
        base->authoring.graphicsPrecise = over->authoring.graphicsPrecise;
        base->authoring.graphicsOffsetHz = over->authoring.graphicsOffsetHz;
        base->authoring.graphicsSurface = over->authoring.graphicsSurface;
        base->authoring.identityExclusions = over->authoring.identityExclusions;
        for (int i = 0; i < VF_NUM_POINTS; ++i) {
            if (over->authoring.graphicsExcluded[i])
                base->authoring.graphicsExcluded[i] = over->authoring.graphicsExcluded[i];
        }
    }
    if (over->hasLock) desired_lock_precision_copy(base, *over);
    if (over->authoring.topologyBound) desired_topology_copy(base, *over);
    for (int i = 0; i < VF_NUM_POINTS; ++i) {
        if (desired_curve_point_present(over, i)) {
            base->authoring.pointFrequencyPrecise[i] = over->authoring.pointFrequencyPrecise[i];
            base->authoring.pointFrequencyHz[i] = over->authoring.pointFrequencyHz[i];
        }
    }
}

static inline bool desired_ini_should_emit_all_domains(const DesiredSettings* d) {
    return !d || d->applicationScope == DESIRED_SCOPE_LEGACY;
}

template <class ReadFn>
static inline bool desired_ini_load_controls_ext(ReadFn&& readKey, DesiredSettings* d,
                                                 char* err, size_t errSize) {
    if (!d) return false;
    char buf[128] = {};
    auto read_u32 = [&](const char* key, gc_u32* out, bool* seen = nullptr) -> bool {
        if (seen) *seen = false;
        buf[0] = '\0';
        if (!readKey(key, buf, sizeof(buf))) return true;
        if (seen) *seen = true;
        if (!profile_parse_u32_strict(buf, out)) {
            char msg[160] = {};
            snprintf(msg, sizeof(msg), "Invalid %s value", key);
            desired_ini_set_err(err, errSize, msg);
            return false;
        }
        return true;
    };
    auto read_i64 = [&](const char* key, gc_i64* out, bool* seen = nullptr) -> bool {
        if (seen) *seen = false;
        buf[0] = '\0';
        if (!readKey(key, buf, sizeof(buf))) return true;
        if (seen) *seen = true;
        if (!profile_parse_i64_strict(buf, out)) {
            char msg[160] = {};
            snprintf(msg, sizeof(msg), "Invalid %s value", key);
            desired_ini_set_err(err, errSize, msg);
            return false;
        }
        return true;
    };
    auto read_b8 = [&](const char* key, gc_bool8* out, bool* seen = nullptr) -> bool {
        if (seen) *seen = false;
        buf[0] = '\0';
        if (!readKey(key, buf, sizeof(buf))) return true;
        if (seen) *seen = true;
        if (!profile_parse_bool8_strict(buf, out)) {
            char msg[160] = {};
            snprintf(msg, sizeof(msg), "Invalid %s flag", key);
            desired_ini_set_err(err, errSize, msg);
            return false;
        }
        return true;
    };

    gc_u32 scope = DESIRED_SCOPE_LEGACY;
    bool seenScope = false;
    if (!read_u32("application_scope", &scope, &seenScope)) return false;
    if (seenScope) {
        if (!desired_application_scope_valid(scope)) {
            desired_ini_set_err(err, errSize, "Invalid application_scope value");
            return false;
        }
        d->applicationScope = scope;
    }

    bool seenMemPrecise = false, seenMemSurf = false, seenMemHz = false;
    if (!read_b8("mem_offset_precise", &d->memOffsetPrecise, &seenMemPrecise) ||
        !read_u32("mem_offset_surface", &d->memOffsetSurface, &seenMemSurf) ||
        !read_i64("mem_offset_hz", &d->memOffsetHz, &seenMemHz)) return false;
    if (!d->memOffsetPrecise && (seenMemSurf || seenMemHz)) {
        if (d->memOffsetSurface || d->memOffsetHz) {
            desired_ini_set_err(err, errSize, "mem_offset_precise required for mem_offset_hz/surface");
            return false;
        }
    }

    bool seenPowRatio = false, seenPowNum = false, seenPowDen = false;
    if (!read_b8("power_limit_ratio", &d->powerLimitRatio, &seenPowRatio) ||
        !read_i64("power_ratio_num", &d->powerRatioNumerator, &seenPowNum) ||
        !read_i64("power_ratio_den", &d->powerRatioDenominator, &seenPowDen)) return false;
    if (!d->powerLimitRatio && (seenPowNum || seenPowDen)) {
        if (d->powerRatioNumerator || d->powerRatioDenominator) {
            desired_ini_set_err(err, errSize, "power_limit_ratio required for power_ratio_num/den");
            return false;
        }
    }

    auto& a = d->authoring;
    bool seenGfxSurf = false, seenGfxHz = false;
    if (!read_b8("graphics_precise", &a.graphicsPrecise) ||
        !read_u32("graphics_surface", &a.graphicsSurface, &seenGfxSurf) ||
        !read_i64("graphics_offset_hz", &a.graphicsOffsetHz, &seenGfxHz) ||
        !read_b8("graphics_identity_exclusions", &a.identityExclusions) ||
        !read_b8("lock_precise", &a.lockPrecise) ||
        !read_i64("lock_frequency_hz", &a.lockFrequencyHz) ||
        !read_b8("topology_bound", &a.topologyBound) ||
        !read_u32("topology_count", &a.topologyCount)) return false;
    (void)seenGfxSurf;
    (void)seenGfxHz;
    return true;
}

template <class ReadFn>
static inline bool desired_ini_load_curve_ext(ReadFn&& readKey, DesiredSettings* d,
                                              char* err, size_t errSize) {
    if (!d) return false;
    char buf[128] = {};
    char key[64] = {};
    for (int i = 0; i < VF_NUM_POINTS; ++i) {
        snprintf(key, sizeof(key), "point%d_captured", i);
        buf[0] = '\0';
        bool seenCap = readKey(key, buf, sizeof(buf));
        if (seenCap) {
            if (!profile_parse_bool8_strict(buf, &d->curvePointCaptured[i])) {
                desired_ini_set_err(err, errSize, "Invalid pointN_captured flag");
                return false;
            }
        }
        snprintf(key, sizeof(key), "point%d_captured_offset_hz", i);
        buf[0] = '\0';
        bool seenCapHz = readKey(key, buf, sizeof(buf));
        if (seenCapHz) {
            if (!profile_parse_i64_strict(buf, &d->curvePointCapturedOffsetHz[i])) {
                desired_ini_set_err(err, errSize, "Invalid pointN_captured_offset_hz value");
                return false;
            }
            if (!d->curvePointCaptured[i] && d->curvePointCapturedOffsetHz[i] != 0) {
                desired_ini_set_err(err, errSize, "pointN_captured required for captured_offset_hz");
                return false;
            }
        }

        snprintf(key, sizeof(key), "point%d_freq_precise", i);
        buf[0] = '\0';
        if (readKey(key, buf, sizeof(buf))) {
            if (!profile_parse_bool8_strict(buf, &d->authoring.pointFrequencyPrecise[i])) {
                desired_ini_set_err(err, errSize, "Invalid pointN_freq_precise flag");
                return false;
            }
        }
        snprintf(key, sizeof(key), "point%d_freq_hz", i);
        buf[0] = '\0';
        if (readKey(key, buf, sizeof(buf))) {
            if (!profile_parse_i64_strict(buf, &d->authoring.pointFrequencyHz[i])) {
                desired_ini_set_err(err, errSize, "Invalid pointN_freq_hz value");
                return false;
            }
        }

        snprintf(key, sizeof(key), "point%d_graphics_excluded", i);
        buf[0] = '\0';
        if (readKey(key, buf, sizeof(buf))) {
            if (!profile_parse_bool8_strict(buf, &d->authoring.graphicsExcluded[i])) {
                desired_ini_set_err(err, errSize, "Invalid pointN_graphics_excluded flag");
                return false;
            }
        }

        snprintf(key, sizeof(key), "point%d_topology_uv", i);
        buf[0] = '\0';
        if (readKey(key, buf, sizeof(buf))) {
            if (!profile_parse_i64_strict(buf, &d->authoring.voltageUv[i])) {
                desired_ini_set_err(err, errSize, "Invalid pointN_topology_uv value");
                return false;
            }
        }
    }
    return true;
}

static inline bool desired_ini_validate_loaded(const DesiredSettings* d,
                                               char* err, size_t errSize) {
    if (!d) return false;
    if (!desired_application_scope_valid(d->applicationScope)) {
        desired_ini_set_err(err, errSize, "Invalid profile application scope");
        return false;
    }
    if (!desired_curve_intent_valid(d)) {
        desired_ini_set_err(err, errSize, "Invalid profile VF curve intent");
        return false;
    }
    if (!desired_scalar_intent_valid(d)) {
        desired_ini_set_err(err, errSize, "Invalid profile scalar precision intent");
        return false;
    }
    if (!desired_authoring_valid(d)) {
        desired_ini_set_err(err, errSize, "Invalid profile authoring or topology metadata");
        return false;
    }
    return true;
}

template <class WriteFn>
static inline bool desired_ini_append_controls_ext(WriteFn&& writeKv, const DesiredSettings* d) {
    if (!d) return true;
    char val[64] = {};
    auto emit_u32 = [&](const char* k, gc_u32 v) {
        snprintf(val, sizeof(val), "%lu", (unsigned long)v);
        return writeKv(k, val);
    };
    auto emit_i64 = [&](const char* k, gc_i64 v) {
        snprintf(val, sizeof(val), "%lld", (long long)v);
        return writeKv(k, val);
    };

    if (d->applicationScope != DESIRED_SCOPE_LEGACY) {
        if (!emit_u32("application_scope", d->applicationScope)) return false;
    }
    if (d->memOffsetPrecise) {
        if (!writeKv("mem_offset_precise", "1") ||
            !emit_u32("mem_offset_surface", d->memOffsetSurface) ||
            !emit_i64("mem_offset_hz", d->memOffsetHz)) return false;
    }
    if (d->powerLimitRatio) {
        if (!writeKv("power_limit_ratio", "1") ||
            !emit_i64("power_ratio_num", d->powerRatioNumerator) ||
            !emit_i64("power_ratio_den", d->powerRatioDenominator)) return false;
    }
    const auto& a = d->authoring;
    if (a.graphicsPrecise) {
        if (!writeKv("graphics_precise", "1") ||
            !emit_u32("graphics_surface", a.graphicsSurface) ||
            !emit_i64("graphics_offset_hz", a.graphicsOffsetHz)) return false;
        if (a.identityExclusions && !writeKv("graphics_identity_exclusions", "1")) return false;
    }
    if (a.lockPrecise) {
        if (!writeKv("lock_precise", "1") ||
            !emit_i64("lock_frequency_hz", a.lockFrequencyHz)) return false;
    }
    if (a.topologyBound) {
        if (!writeKv("topology_bound", "1") ||
            !emit_u32("topology_count", a.topologyCount)) return false;
    }
    return true;
}

template <class WriteFn>
static inline bool desired_ini_append_curve_ext(WriteFn&& writeKv, const DesiredSettings* d) {
    if (!d) return true;
    char key[64] = {};
    char val[64] = {};
    for (int i = 0; i < VF_NUM_POINTS; ++i) {
        if (d->curvePointCaptured[i]) {
            snprintf(key, sizeof(key), "point%d_captured", i);
            if (!writeKv(key, "1")) return false;
            snprintf(key, sizeof(key), "point%d_captured_offset_hz", i);
            snprintf(val, sizeof(val), "%lld", (long long)d->curvePointCapturedOffsetHz[i]);
            if (!writeKv(key, val)) return false;
        }
        if (d->authoring.pointFrequencyPrecise[i]) {
            snprintf(key, sizeof(key), "point%d_freq_precise", i);
            if (!writeKv(key, "1")) return false;
            snprintf(key, sizeof(key), "point%d_freq_hz", i);
            snprintf(val, sizeof(val), "%lld", (long long)d->authoring.pointFrequencyHz[i]);
            if (!writeKv(key, val)) return false;
        }
        if (d->authoring.graphicsExcluded[i]) {
            snprintf(key, sizeof(key), "point%d_graphics_excluded", i);
            if (!writeKv(key, "1")) return false;
        }
        if (d->authoring.topologyBound && d->authoring.voltageUv[i] > 0) {
            snprintf(key, sizeof(key), "point%d_topology_uv", i);
            snprintf(val, sizeof(val), "%lld", (long long)d->authoring.voltageUv[i]);
            if (!writeKv(key, val)) return false;
        }
    }
    return true;
}

#endif
