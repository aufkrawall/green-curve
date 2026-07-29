// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// The payload container the installer carries, and its parser.
//
// A setup executable is the installer stub with one blob appended after the PE
// image.  Everything about that blob is described here in pure, allocation-free
// code so `build.py --test` can hammer the parser with truncated, overlapping,
// and hostile directories without a Windows process, a file, or a real
// compressor.  The runtime side (source/installer_payload.cpp) only maps the
// file and hands the bytes to these functions.
//
// Layout of a setup executable, low address to high:
//
//   [ PE image .............................................. ]
//   [ compressed archive blob ............................... ]
//   [ GcPayloadFooter (fixed size, last bytes of the file) ... ]
//
// The footer is read from the end because the PE image size is not knowable
// without parsing the PE, and appending is the only way to add a payload
// without disturbing the linker's own layout or the signature-free hardening
// gates build.py runs on the stub.
//
// The decompressed archive is a "GCAR" container: a directory of fixed-size
// records followed by the file data.  Names are flat (no directories, no path
// separators) because the payload is exactly the release manifest.

#ifndef GREEN_CURVE_INSTALLER_ARCHIVE_POLICY_H
#define GREEN_CURVE_INSTALLER_ARCHIVE_POLICY_H

#include <stddef.h>
#include <stdint.h>

// ---------------------------------------------------------------------------
// CRC-32 (IEEE 802.3, reflected, the same polynomial zlib and PNG use).
//
// Present so a corrupted download fails with "this setup file is damaged"
// instead of installing half a service binary.  Table-free: the payload is a
// few megabytes and this runs once, so a 256-entry table would cost more in
// binary size than it saves in time.
// ---------------------------------------------------------------------------
static inline uint32_t gc_crc32(const void* data, size_t size, uint32_t seed) {
    const uint8_t* bytes = (const uint8_t*)data;
    uint32_t crc = ~seed;
    for (size_t i = 0; i < size; i++) {
        crc ^= bytes[i];
        for (int bit = 0; bit < 8; bit++) {
            uint32_t mask = (uint32_t)0 - (crc & 1u);
            crc = (crc >> 1) ^ (0xEDB88320u & mask);
        }
    }
    return ~crc;
}

// ---------------------------------------------------------------------------
// Footer
// ---------------------------------------------------------------------------

// Bumped only on an incompatible format change.  An installer refuses a footer
// whose magic it does not recognize rather than guessing at field offsets.
#define GC_PAYLOAD_FOOTER_MAGIC "GCPAY001"
#define GC_PAYLOAD_FOOTER_MAGIC_LEN 8

enum GcPayloadMethod {
    // Stored verbatim.  Used when the build host cannot reach the Windows
    // compression API, and for payloads compression makes larger.
    GC_PAYLOAD_METHOD_STORE = 0,
    // Windows Compression API, COMPRESS_ALGORITHM_XPRESS_HUFF (cabinet.dll).
    // Fast to decompress and shrinks the two binaries by roughly half, which is
    // the "mild but fast" point on the curve; LZMS would win a few more percent
    // for a multi-second decompress on the user's machine.
    GC_PAYLOAD_METHOD_XPRESS_HUFF = 1,
};

#pragma pack(push, 1)
struct GcPayloadFooter {
    char     magic[GC_PAYLOAD_FOOTER_MAGIC_LEN];
    uint32_t method;            // GcPayloadMethod
    uint64_t archiveOffset;     // byte offset of the compressed blob in the file
    uint64_t compressedSize;    // bytes of the compressed blob
    uint64_t uncompressedSize;  // bytes of the decompressed GCAR container
    uint32_t archiveCrc32;      // CRC-32 of the decompressed container
    uint32_t footerCrc32;       // CRC-32 of every field above
};
#pragma pack(pop)

// The struct is written byte-for-byte by build.py, so its size is part of the
// format.  A silent layout change here would make every future setup file
// unreadable by an installer built from the same source, which is exactly the
// class of bug a static assertion catches for free.
static_assert(sizeof(struct GcPayloadFooter) == 8 + 4 + 8 + 8 + 8 + 4 + 4,
              "GcPayloadFooter must stay packed at 44 bytes");

static inline uint32_t gc_payload_footer_expected_crc(const struct GcPayloadFooter* footer) {
    // Everything except the trailing footerCrc32 field itself.
    return gc_crc32(footer, sizeof(*footer) - sizeof(uint32_t), 0);
}

enum GcPayloadStatus {
    GC_PAYLOAD_OK = 0,
    GC_PAYLOAD_ERR_NO_FOOTER,      // file too small, or magic absent
    GC_PAYLOAD_ERR_FOOTER_CRC,     // footer bytes are damaged
    GC_PAYLOAD_ERR_METHOD,         // compression method this build cannot read
    GC_PAYLOAD_ERR_RANGE,          // blob does not lie inside the file
    GC_PAYLOAD_ERR_SIZE,           // implausible or zero sizes
};

// Validate a footer that was read from the last sizeof(GcPayloadFooter) bytes
// of a file of `fileSize` bytes.  Pure: no I/O, no allocation.
//
// `maxUncompressed` is the caller's ceiling for one allocation.  It exists so a
// hostile or corrupt footer cannot ask the installer to reserve an absurd
// amount of memory before a single byte has been verified.
static inline GcPayloadStatus gc_payload_validate_footer(const struct GcPayloadFooter* footer,
                                                         uint64_t fileSize,
                                                         uint64_t maxUncompressed) {
    if (!footer) return GC_PAYLOAD_ERR_NO_FOOTER;
    for (int i = 0; i < GC_PAYLOAD_FOOTER_MAGIC_LEN; i++) {
        if (footer->magic[i] != GC_PAYLOAD_FOOTER_MAGIC[i]) return GC_PAYLOAD_ERR_NO_FOOTER;
    }
    if (footer->footerCrc32 != gc_payload_footer_expected_crc(footer)) return GC_PAYLOAD_ERR_FOOTER_CRC;
    if (footer->method != GC_PAYLOAD_METHOD_STORE &&
        footer->method != GC_PAYLOAD_METHOD_XPRESS_HUFF) {
        return GC_PAYLOAD_ERR_METHOD;
    }
    if (footer->compressedSize == 0 || footer->uncompressedSize == 0) return GC_PAYLOAD_ERR_SIZE;
    if (footer->uncompressedSize > maxUncompressed) return GC_PAYLOAD_ERR_SIZE;
    if (footer->method == GC_PAYLOAD_METHOD_STORE &&
        footer->compressedSize != footer->uncompressedSize) {
        return GC_PAYLOAD_ERR_SIZE;
    }
    // The blob must end exactly where the footer begins: any gap would mean the
    // file was concatenated from parts we do not understand.
    if (fileSize < sizeof(struct GcPayloadFooter)) return GC_PAYLOAD_ERR_NO_FOOTER;
    uint64_t footerStart = fileSize - sizeof(struct GcPayloadFooter);
    if (footer->archiveOffset > footerStart) return GC_PAYLOAD_ERR_RANGE;
    if (footer->compressedSize != footerStart - footer->archiveOffset) return GC_PAYLOAD_ERR_RANGE;
    if (footer->archiveOffset == 0) return GC_PAYLOAD_ERR_RANGE;  // no stub at all
    return GC_PAYLOAD_OK;
}

// ---------------------------------------------------------------------------
// GCAR container
// ---------------------------------------------------------------------------

#define GC_ARCHIVE_MAGIC "GCAR0001"
#define GC_ARCHIVE_MAGIC_LEN 8
// The release manifest is four files plus the uninstaller; the ceiling only
// needs to make a corrupt count obviously wrong before any memory is touched.
#define GC_ARCHIVE_MAX_FILES 16
#define GC_ARCHIVE_MAX_NAME 63

#pragma pack(push, 1)
struct GcArchiveHeader {
    char     magic[GC_ARCHIVE_MAGIC_LEN];
    uint32_t fileCount;
};

struct GcArchiveEntry {
    char     name[GC_ARCHIVE_MAX_NAME + 1];  // NUL-terminated, flat file name
    uint64_t dataOffset;                     // from the start of the container
    uint64_t dataSize;
    uint32_t dataCrc32;
    uint32_t flags;                          // GcArchiveEntryFlags
};
#pragma pack(pop)

enum GcArchiveEntryFlags {
    // Written to the install directory as a normal payload file.
    GC_ARCHIVE_FLAG_NONE = 0,
    // The uninstaller: extracted like any other file, but also recorded as the
    // UninstallString in Add/Remove Programs.
    GC_ARCHIVE_FLAG_UNINSTALLER = 1u << 0,
};

static_assert(sizeof(struct GcArchiveHeader) == 12, "GcArchiveHeader must stay packed at 12 bytes");
static_assert(sizeof(struct GcArchiveEntry) == 64 + 8 + 8 + 4 + 4,
              "GcArchiveEntry must stay packed at 88 bytes");

enum GcArchiveStatus {
    GC_ARCHIVE_OK = 0,
    GC_ARCHIVE_ERR_MAGIC,
    GC_ARCHIVE_ERR_COUNT,       // zero, or beyond GC_ARCHIVE_MAX_FILES
    GC_ARCHIVE_ERR_TRUNCATED,   // directory does not fit in the container
    GC_ARCHIVE_ERR_NAME,        // empty, unterminated, or path-bearing name
    GC_ARCHIVE_ERR_RANGE,       // an entry's data escapes the container
    GC_ARCHIVE_ERR_CRC,         // an entry's bytes do not match its checksum
};

// A payload file name is a bare name.  Anything that could redirect a write out
// of the install directory is rejected here rather than being sanitized,
// because there is no legitimate payload that needs it and a "cleaned" path is
// a much harder thing to reason about than a refusal.
static inline bool gc_archive_name_is_safe(const char* name) {
    if (!name || !name[0]) return false;
    size_t length = 0;
    while (name[length]) {
        char c = name[length];
        if (c == '\\' || c == '/' || c == ':') return false;
        // Control characters and the Win32 wildcard/redirection set.
        if ((unsigned char)c < 0x20) return false;
        if (c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|') return false;
        length++;
        if (length > GC_ARCHIVE_MAX_NAME) return false;
    }
    // "." and ".." are directory references, never payload files.  A trailing
    // dot or space is stripped by Win32 path canonicalization, which would make
    // the extracted name differ from the verified one.
    if (name[0] == '.' && (name[1] == 0 || (name[1] == '.' && name[2] == 0))) return false;
    if (name[length - 1] == '.' || name[length - 1] == ' ') return false;
    return true;
}

// Validate the whole container: magic, directory bounds, every name, every
// data range, and every entry checksum.  `entriesOut` (optional) receives the
// directory so the caller does not have to re-derive the offsets.
//
// Verifying up front means extraction is a straight copy that cannot fail
// halfway through on data the parser never looked at.
static inline GcArchiveStatus gc_archive_validate(const void* container, uint64_t size,
                                                  const struct GcArchiveEntry** entriesOut,
                                                  uint32_t* countOut) {
    if (entriesOut) *entriesOut = nullptr;
    if (countOut) *countOut = 0;
    if (!container || size < sizeof(struct GcArchiveHeader)) return GC_ARCHIVE_ERR_MAGIC;
    const uint8_t* base = (const uint8_t*)container;
    const struct GcArchiveHeader* header = (const struct GcArchiveHeader*)base;
    for (int i = 0; i < GC_ARCHIVE_MAGIC_LEN; i++) {
        if (header->magic[i] != GC_ARCHIVE_MAGIC[i]) return GC_ARCHIVE_ERR_MAGIC;
    }
    if (header->fileCount == 0 || header->fileCount > GC_ARCHIVE_MAX_FILES) return GC_ARCHIVE_ERR_COUNT;

    uint64_t directoryBytes = (uint64_t)header->fileCount * sizeof(struct GcArchiveEntry);
    uint64_t dataStart = sizeof(struct GcArchiveHeader) + directoryBytes;
    if (dataStart > size) return GC_ARCHIVE_ERR_TRUNCATED;

    const struct GcArchiveEntry* entries =
        (const struct GcArchiveEntry*)(base + sizeof(struct GcArchiveHeader));
    for (uint32_t i = 0; i < header->fileCount; i++) {
        const struct GcArchiveEntry* entry = &entries[i];
        // The name field must be NUL-terminated inside its own storage before
        // it is read as a C string.
        bool terminated = false;
        for (size_t c = 0; c < sizeof(entry->name); c++) {
            if (entry->name[c] == 0) { terminated = true; break; }
        }
        if (!terminated || !gc_archive_name_is_safe(entry->name)) return GC_ARCHIVE_ERR_NAME;
        if (entry->dataOffset < dataStart) return GC_ARCHIVE_ERR_RANGE;
        if (entry->dataSize > size) return GC_ARCHIVE_ERR_RANGE;
        if (entry->dataOffset > size - entry->dataSize) return GC_ARCHIVE_ERR_RANGE;
        if (gc_crc32(base + entry->dataOffset, (size_t)entry->dataSize, 0) != entry->dataCrc32) {
            return GC_ARCHIVE_ERR_CRC;
        }
        // Duplicate names would make extraction order decide what lands on
        // disk, so the manifest must be unambiguous.
        for (uint32_t j = 0; j < i; j++) {
            const char* a = entries[j].name;
            const char* b = entry->name;
            size_t k = 0;
            while (a[k] && b[k]) {
                char ca = a[k], cb = b[k];
                if (ca >= 'A' && ca <= 'Z') ca = (char)(ca - 'A' + 'a');
                if (cb >= 'A' && cb <= 'Z') cb = (char)(cb - 'A' + 'a');
                if (ca != cb) break;
                k++;
            }
            if (a[k] == 0 && b[k] == 0) return GC_ARCHIVE_ERR_NAME;
        }
    }
    if (entriesOut) *entriesOut = entries;
    if (countOut) *countOut = header->fileCount;
    return GC_ARCHIVE_OK;
}

static inline const char* gc_archive_status_text(GcArchiveStatus status) {
    switch (status) {
        case GC_ARCHIVE_OK:            return "ok";
        case GC_ARCHIVE_ERR_MAGIC:     return "payload container header is missing or unrecognized";
        case GC_ARCHIVE_ERR_COUNT:     return "payload container file count is invalid";
        case GC_ARCHIVE_ERR_TRUNCATED: return "payload container directory is truncated";
        case GC_ARCHIVE_ERR_NAME:      return "payload container holds an unsafe or duplicate file name";
        case GC_ARCHIVE_ERR_RANGE:     return "payload container entry points outside the container";
        case GC_ARCHIVE_ERR_CRC:       return "payload container entry failed its checksum";
    }
    return "payload container is invalid";
}

static inline const char* gc_payload_status_text(GcPayloadStatus status) {
    switch (status) {
        case GC_PAYLOAD_OK:             return "ok";
        case GC_PAYLOAD_ERR_NO_FOOTER:  return "this file carries no Green Curve payload";
        case GC_PAYLOAD_ERR_FOOTER_CRC: return "the payload footer is damaged";
        case GC_PAYLOAD_ERR_METHOD:     return "the payload uses a compression method this installer cannot read";
        case GC_PAYLOAD_ERR_RANGE:      return "the payload does not lie inside this file";
        case GC_PAYLOAD_ERR_SIZE:       return "the payload reports an implausible size";
    }
    return "the payload is invalid";
}

#endif // GREEN_CURVE_INSTALLER_ARCHIVE_POLICY_H
