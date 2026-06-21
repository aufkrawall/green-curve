// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// win32_compat.h — Linux-only stand-ins for the handful of Win32 types and
// macros that appear in otherwise-portable shared declarations (AppData's UI
// handle fields, the recovery-flag globals, CliOptions, and Win32-only helper
// prototypes that are never *called* on Linux).
//
// The opaque handle types are `void*`: on Linux the UI/handle fields of AppData
// are simply unused.  Integer aliases match the Win32 widths so any incidental
// sizeof/layout is correct.  Critical sections are backed by a real pthread
// recursive mutex so config-storage locking works in the daemon.
//
// This header is NEVER included on Windows (app_shared.h includes <windows.h>
// there); it must not be relied on for any Win32 *behavior* beyond the few
// shims defined here.

#ifndef GREEN_CURVE_WIN32_COMPAT_H
#define GREEN_CURVE_WIN32_COMPAT_H

#if defined(_WIN32)
#error "win32_compat.h is Linux-only; on Windows include <windows.h>"
#endif

#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include <wchar.h>

// ---- Opaque handle types (unused on Linux) --------------------------------
typedef void* HANDLE;
typedef void* HMODULE;
typedef void* HINSTANCE;
typedef void* HWND;
typedef void* HDC;
typedef void* HBITMAP;
typedef void* HBRUSH;
typedef void* HPEN;
typedef void* HFONT;
typedef void* HICON;
typedef void* HCURSOR;
typedef void* HMENU;
typedef void* HGDIOBJ;

// ---- Integer aliases (Win32 widths) ---------------------------------------
typedef int32_t  LONG;
typedef uint32_t ULONG;
typedef uint32_t DWORD;
typedef int64_t  LONGLONG;
typedef uint64_t ULONGLONG;
typedef uint16_t WORD;
typedef uint8_t  BYTE;
typedef unsigned int UINT;
typedef int      BOOL;
typedef wchar_t  WCHAR;   // never dereferenced on Linux; matches L"" literals

#ifndef TRUE
#define TRUE 1
#endif
#ifndef FALSE
#define FALSE 0
#endif

#ifndef MAX_PATH
#define MAX_PATH 260
#endif

#define WINAPI
#define CALLBACK
#define APIENTRY

// Win32 UI macros referenced by (Windows-only) color/message #defines in
// app_shared.h.  They only ever expand at use sites in Windows-only code, but
// must be defined so the translation unit preprocesses cleanly on Linux.
#ifndef RGB
#define RGB(r, g, b) ((DWORD)(((BYTE)(r)) | (((WORD)((BYTE)(g))) << 8) | (((DWORD)((BYTE)(b))) << 16)))
#endif
#ifndef WM_APP
#define WM_APP 0x8000
#endif

// ---- CRITICAL_SECTION backed by a recursive pthread mutex -----------------
// Real behavior so config-storage locking works in the Linux daemon.
typedef struct {
    pthread_mutex_t mtx;
    int initialized;
} CRITICAL_SECTION;

static inline void InitializeCriticalSection(CRITICAL_SECTION* cs) {
    pthread_mutexattr_t a;
    pthread_mutexattr_init(&a);
    pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&cs->mtx, &a);
    pthread_mutexattr_destroy(&a);
    cs->initialized = 1;
}
static inline void EnterCriticalSection(CRITICAL_SECTION* cs) {
    if (!cs->initialized) InitializeCriticalSection(cs);
    pthread_mutex_lock(&cs->mtx);
}
static inline void LeaveCriticalSection(CRITICAL_SECTION* cs) {
    pthread_mutex_unlock(&cs->mtx);
}
static inline void DeleteCriticalSection(CRITICAL_SECTION* cs) {
    if (cs->initialized) { pthread_mutex_destroy(&cs->mtx); cs->initialized = 0; }
}

#endif // GREEN_CURVE_WIN32_COMPAT_H
