// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#ifndef GREEN_CURVE_GPU_WIRE_TYPES_H
#define GREEN_CURVE_GPU_WIRE_TYPES_H
#include <stdint.h>

// ---------------------------------------------------------------------------
// Fixed-width aliases for the binary IPC protocol.
//
// The wire structs historically used Win32 DWORD/ULONGLONG and MAX_PATH.  To
// stay byte-identical on Windows (where DWORD == `unsigned long`, 32-bit under
// LLP64) AND correctly sized on Linux (where `unsigned long` is 64-bit), we
// alias to `unsigned long` on Windows — the exact type DWORD already is, so all
// existing %lu format strings and DWORD assignments keep compiling — and to
// uint32_t on Linux.  Both are 32-bit, so the struct layout matches across OSes.
// ---------------------------------------------------------------------------
#if defined(_WIN32)
typedef unsigned long gc_u32;       // identical to Win32 DWORD
#else
typedef uint32_t gc_u32;
#endif
typedef uint8_t gc_u8;
typedef unsigned long long gc_u64;  // identical to Win32 ULONGLONG
typedef long long gc_i64;
static_assert(sizeof(gc_i64) == 8, "gc_i64 must stay an eight-byte wire value");
typedef gc_u8 gc_bool8;
static_assert(sizeof(gc_bool8) == 1, "gc_bool8 must stay a one-byte wire flag");

enum DesiredApplicationScope : gc_u32 {
    DESIRED_SCOPE_LEGACY = 0, // retain historical request-builder behavior
    DESIRED_SCOPE_UPDATE_INCLUDED = 1,
    DESIRED_SCOPE_REPLACE_PROFILE = 2,
    DESIRED_SCOPE_EXPLICIT_RELEASE = 3,
    // Orthogonal VF point-set policy for UPDATE_INCLUDED. Omitted scalars
    // and an omitted lock still retain their ownership.
    DESIRED_SCOPE_REPLACE_VF_POINTS = 1u << 8,
};

static inline gc_u32 desired_application_scope_kind(gc_u32 scope) {
    return scope & ~DESIRED_SCOPE_REPLACE_VF_POINTS;
}

static inline bool desired_application_scope_valid(gc_u32 scope) {
    return scope <= DESIRED_SCOPE_EXPLICIT_RELEASE ||
        scope == (DESIRED_SCOPE_UPDATE_INCLUDED | DESIRED_SCOPE_REPLACE_VF_POINTS);
}

static inline gc_bool8 gc_bool8_from_bool(bool value) {
    return value ? (gc_bool8)1u : (gc_bool8)0u;
}

static inline bool gc_bool8_is_canonical(gc_bool8 value) {
    return value == 0u || value == 1u;
}

static inline void canonicalize_gc_bool8(gc_bool8* value) {
    if (value) *value = *value ? (gc_bool8)1u : (gc_bool8)0u;
}

#endif
