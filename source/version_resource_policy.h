// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// The fixed file version of a PE image that is MAPPED in memory, read from
// its own RT_VERSION resource.
//
// Exists so the service can read the version of the driver image it runs
// rather than of whatever file now sits at that image's path: a driver update
// replaces the files in place while the old process keeps the old image, and
// the loader keeps reporting the load-time path, so GetModuleFileNameW +
// GetFileVersionInfoW reads the NEW file for both sides of an "in-process vs
// on disk" comparison.
//
// Walked by hand rather than through FindResource/LoadResource/LockResource/
// SizeofResource: those four would be new imports in the SYSTEM service, and
// together they are the classic resource-dropper import shape that heuristic
// scanners weigh (llm-wiki/antivirus-heuristics.md).  Every offset is bounded
// by the image's own SizeOfImage.  Pure, so the layout rules run on either
// host; module_version_win32.h hands it a mapped module.

#ifndef GREEN_CURVE_VERSION_RESOURCE_POLICY_H
#define GREEN_CURVE_VERSION_RESOURCE_POLICY_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#define GC_VS_FIXED_FILE_INFO_SIGNATURE 0xFEEF04BDu
// VS_VERSIONINFO: wLength, wValueLength, wType, L"VS_VERSION_INFO\0", then
// padding to a 32-bit boundary: the VS_FIXEDFILEINFO starts at byte 40.
#define GC_VS_FIXED_FILE_INFO_OFFSET 40u
#define GC_VS_FIXED_FILE_INFO_SIZE 52u
#define GC_PE_RT_VERSION 16u
#define GC_PE_VS_VERSION_INFO_ID 1u

static inline uint32_t gc_version_resource_u16(const unsigned char* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8);
}

static inline uint32_t gc_version_resource_u32(const unsigned char* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

// Whether [offset, offset + length) lies inside `size` bytes.
static inline bool gc_version_resource_in(size_t size, uint64_t offset, uint64_t length) {
    return offset <= size && length <= size - offset;
}

// Returns true and the file version (MS/LS halves, as in VS_FIXEDFILEINFO)
// only for a well-formed root block: declared length within `size`, the
// VS_VERSION_INFO key, a value large enough, and the fixed-info signature.
static inline bool gc_version_resource_file_version(const void* data, size_t size,
                                                    uint32_t* outMs, uint32_t* outLs) {
    if (!data || !outMs || !outLs) return false;
    const unsigned char* bytes = (const unsigned char*)data;
    const size_t end = GC_VS_FIXED_FILE_INFO_OFFSET + GC_VS_FIXED_FILE_INFO_SIZE;
    if (size < end) return false;
    const size_t length = gc_version_resource_u16(bytes);
    if (length < end || length > size ||
        gc_version_resource_u16(bytes + 2) < GC_VS_FIXED_FILE_INFO_SIZE) return false;
    static const char kKey[] = "VS_VERSION_INFO";
    for (size_t i = 0; i < sizeof(kKey); ++i) {  // UTF-16LE, terminator included
        if (bytes[6 + 2 * i] != (unsigned char)kKey[i] || bytes[7 + 2 * i] != 0) return false;
    }
    const unsigned char* fixed = bytes + GC_VS_FIXED_FILE_INFO_OFFSET;
    if (gc_version_resource_u32(fixed) != GC_VS_FIXED_FILE_INFO_SIGNATURE) return false;
    *outMs = gc_version_resource_u32(fixed + 8);
    *outLs = gc_version_resource_u32(fixed + 12);
    return true;
}

// The OffsetToData of the resource directory entry with integer `id` in the
// directory at `directory` (relative to the resource section), or 0.
static inline uint32_t gc_pe_resource_entry(const unsigned char* rsrc, size_t rsrcSize,
                                            uint32_t directory, uint32_t id) {
    if (!gc_version_resource_in(rsrcSize, directory, 16)) return 0;
    const unsigned char* dir = rsrc + directory;
    const uint32_t count = gc_version_resource_u16(dir + 12) + gc_version_resource_u16(dir + 14);
    if (!gc_version_resource_in(rsrcSize, (uint64_t)directory + 16, (uint64_t)count * 8)) return 0;
    for (uint32_t i = 0; i < count; ++i) {
        const unsigned char* entry = dir + 16 + 8 * i;
        // Integer IDs only: a named entry has the high bit set and never matches.
        if (gc_version_resource_u32(entry) == id) return gc_version_resource_u32(entry + 4);
    }
    return 0;
}

// The VS_VERSIONINFO block of the image mapped at `base`: RT_VERSION / 1 /
// first language.  Image layout, so RVAs are offsets from `base`.
// `mappedSize` is how many bytes are readable from `base`; for an image the
// loader mapped (whose headers it has already validated) pass SIZE_MAX and
// the image's own SizeOfImage is the bound.
static inline bool gc_pe_image_version_resource(const unsigned char* base, size_t mappedSize,
                                                const unsigned char** outData, size_t* outSize) {
    if (!base || !outData || !outSize) return false;
    if (!gc_version_resource_in(mappedSize, 0, 64) || base[0] != 'M' || base[1] != 'Z') return false;
    const uint32_t nt = gc_version_resource_u32(base + 0x3C);
    if (!gc_version_resource_in(mappedSize, nt, 24 + 2) ||
        gc_version_resource_u32(base + nt) != 0x00004550u) return false;  // "PE\0\0"
    const uint32_t optionalSize = gc_version_resource_u16(base + nt + 20);
    const unsigned char* optional = base + nt + 24;
    if (!gc_version_resource_in(mappedSize, (uint64_t)nt + 24, optionalSize)) return false;
    const uint32_t magic = optionalSize >= 2 ? gc_version_resource_u16(optional) : 0;
    const uint32_t directories = magic == 0x20Bu ? 112u : magic == 0x10Bu ? 96u : 0u;
    // SizeOfImage (offset 56) bounds every RVA; the resource directory is #2.
    if (!directories || optionalSize < directories + 3 * 8 ||
        gc_version_resource_u32(optional + directories - 4) < 3) return false;
    const uint32_t imageSize = gc_version_resource_u32(optional + 56);
    const size_t size = imageSize < mappedSize ? imageSize : mappedSize;
    if (!gc_version_resource_in(size, (uint64_t)nt + 24, optionalSize)) return false;
    const uint32_t rsrcRva = gc_version_resource_u32(optional + directories + 2 * 8);
    const uint32_t rsrcSize = gc_version_resource_u32(optional + directories + 2 * 8 + 4);
    if (!rsrcRva || !gc_version_resource_in(size, rsrcRva, rsrcSize)) return false;
    const unsigned char* rsrc = base + rsrcRva;
    // Type and name levels are subdirectories; the language level is data.
    uint32_t next = gc_pe_resource_entry(rsrc, rsrcSize, 0, GC_PE_RT_VERSION);
    if (!(next & 0x80000000u)) return false;
    next = gc_pe_resource_entry(rsrc, rsrcSize, next & 0x7FFFFFFFu, GC_PE_VS_VERSION_INFO_ID);
    if (!(next & 0x80000000u)) return false;
    const uint32_t languages = next & 0x7FFFFFFFu;
    if (!gc_version_resource_in(rsrcSize, languages, 16 + 8) ||
        gc_version_resource_u16(rsrc + languages + 12) + gc_version_resource_u16(rsrc + languages + 14) == 0)
        return false;
    const uint32_t leaf = gc_version_resource_u32(rsrc + languages + 16 + 4);
    if ((leaf & 0x80000000u) || !gc_version_resource_in(rsrcSize, leaf, 16)) return false;
    const uint32_t dataRva = gc_version_resource_u32(rsrc + leaf);
    const uint32_t dataSize = gc_version_resource_u32(rsrc + leaf + 4);
    if (!gc_version_resource_in(size, dataRva, dataSize)) return false;
    *outData = base + dataRva;
    *outSize = dataSize;
    return true;
}

// A driver image version for the log. NVIDIA numbers its user-mode images
// <a>.<b>.<c>.<d> with the release spelled by the last digit of <c> and all of
// <d>: 32.0.15.8180 is release 581.80. Diagnostics only, nothing decides on
// it. "32.0.15.8180 (581.80)"; without a release reading just the four
// fields; "unknown" for 0/0.
static inline void gc_driver_version_text(uint32_t ms, uint32_t ls, char* out, size_t outSize) {
    if (!out || !outSize) return;
    if (!ms && !ls) {
        snprintf(out, outSize, "unknown");
        return;
    }
    const unsigned a = ms >> 16, b = ms & 0xFFFFu, c = ls >> 16, d = ls & 0xFFFFu;
    if (d < 10000u) {
        const unsigned release = (c % 10u) * 10000u + d;
        snprintf(out, outSize, "%u.%u.%u.%u (%u.%02u)", a, b, c, d, release / 100u, release % 100u);
    } else {
        snprintf(out, outSize, "%u.%u.%u.%u", a, b, c, d);
    }
}

// The same spelling as a value, for use inside one log call's arguments.
struct GcDriverVersionLabel {
    char text[48];
};

static inline GcDriverVersionLabel gc_driver_version_label(uint32_t ms, uint32_t ls) {
    GcDriverVersionLabel label = {};
    gc_driver_version_text(ms, ls, label.text, sizeof(label.text));
    return label;
}

#endif  // GREEN_CURVE_VERSION_RESOURCE_POLICY_H
