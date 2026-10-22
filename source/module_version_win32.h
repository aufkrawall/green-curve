// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// The version of a module as it is MAPPED in this process, read from the
// image's own version resource (version_resource_policy.h).  Not
// GetModuleFileNameW + GetFileVersionInfoW: the loader keeps the load-time
// path, and once a driver update has replaced the file there, that path names
// the new image while this process still runs the old one.

#ifndef GREEN_CURVE_MODULE_VERSION_WIN32_H
#define GREEN_CURVE_MODULE_VERSION_WIN32_H
#if defined(_WIN32)
#include <windows.h>
#include <stdint.h>
#include "version_resource_policy.h"

// `module` is a loaded module, or a LOAD_LIBRARY_AS_IMAGE_RESOURCE mapping
// (its low tag bits are cleared).  A plain LOAD_LIBRARY_AS_DATAFILE mapping is
// file layout, not image layout, and is refused.
static inline bool gc_module_mapped_version(HMODULE module, DWORD* outMs, DWORD* outLs) {
    if (!module || !outMs || !outLs) return false;
    const ULONG_PTR handle = (ULONG_PTR)module;
    if ((handle & 3) == 1) return false;
    const unsigned char* data = nullptr;
    size_t size = 0;
    uint32_t ms = 0, ls = 0;
    if (!gc_pe_image_version_resource((const unsigned char*)(handle & ~(ULONG_PTR)3), SIZE_MAX,
                                      &data, &size) ||
        !gc_version_resource_file_version(data, size, &ms, &ls)) return false;
    *outMs = ms;
    *outLs = ls;
    return true;
}

#endif
#endif  // GREEN_CURVE_MODULE_VERSION_WIN32_H
