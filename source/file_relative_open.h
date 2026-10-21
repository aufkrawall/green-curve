// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#ifndef GREEN_CURVE_FILE_RELATIVE_OPEN_H
#define GREEN_CURVE_FILE_RELATIVE_OPEN_H
#if defined(_WIN32)
#include <windows.h>
#include <winternl.h>
#include <wchar.h>

// Win32 has no directory-handle-relative create operation. Use the system's
// documented native file API so parent aliases cannot redirect a leaf open.
static inline HANDLE gc_open_relative_output(HANDLE directory, const WCHAR* name) {
    if (!name || !name[0] || wcschr(name, L'\\') || wcschr(name, L'/') ||
        wcschr(name, L':') || !wcscmp(name, L".") || !wcscmp(name, L"..")) {
        SetLastError(ERROR_INVALID_NAME);
        return INVALID_HANDLE_VALUE;
    }
    const size_t length = wcslen(name);
    if (length >= 32768) { SetLastError(ERROR_FILENAME_EXCED_RANGE); return INVALID_HANDLE_VALUE; }
    using Open = NTSTATUS (NTAPI*)(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES,
        PIO_STATUS_BLOCK, PLARGE_INTEGER, ULONG, ULONG, ULONG, ULONG, PVOID, ULONG);
    using Error = ULONG (NTAPI*)(NTSTATUS);
    HMODULE module = GetModuleHandleW(L"ntdll.dll");
    Open open = module ? reinterpret_cast<Open>(GetProcAddress(module, "NtCreateFile")) : nullptr;
    Error error = module ? reinterpret_cast<Error>(GetProcAddress(module, "RtlNtStatusToDosError")) : nullptr;
    if (!open || !error) { SetLastError(ERROR_PROC_NOT_FOUND); return INVALID_HANDLE_VALUE; }
    UNICODE_STRING leaf = {};
    leaf.Length = (USHORT)(length * sizeof(WCHAR));
    leaf.MaximumLength = leaf.Length;
    leaf.Buffer = const_cast<WCHAR*>(name);
    OBJECT_ATTRIBUTES attributes = {};
    attributes.Length = sizeof(attributes);
    attributes.RootDirectory = directory;
    attributes.ObjectName = &leaf;
    attributes.Attributes = 0x40; // OBJ_CASE_INSENSITIVE
    IO_STATUS_BLOCK io = {};
    HANDLE handle = INVALID_HANDLE_VALUE;
    NTSTATUS status = open(&handle, GENERIC_WRITE | FILE_READ_ATTRIBUTES | SYNCHRONIZE,
        &attributes, &io, nullptr, FILE_ATTRIBUTE_NORMAL, FILE_SHARE_READ,
        3, // FILE_OPEN_IF: open or create, never truncate
        0x20 | 0x40 | 0x200000, // SYNCHRONOUS_IO_NONALERT | NON_DIRECTORY_FILE | OPEN_REPARSE_POINT
        nullptr, 0);
    if (status < 0) { SetLastError(error(status)); return INVALID_HANDLE_VALUE; }
    return handle;
}
#endif
#endif
