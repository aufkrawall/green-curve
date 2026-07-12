// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// platform.h — thin OS-abstraction shim shared by the GPU backend so the
// NvAPI/NVML control logic compiles unchanged on both Windows and Linux.
//
// Design goals:
//   * On Windows, every wrapper expands to the *exact same* Win32 call the
//     backend used before this shim existed, so Windows codegen is unchanged
//     and the battle-tested behaviour is preserved bit-for-bit.
//   * On Linux, the wrappers map to POSIX / glibc equivalents.  The NVIDIA
//     driver libraries (libnvidia-api.so.1 / libnvidia-ml.so.1) are glibc
//     shared objects resolved at runtime via dlopen — this is why the Linux
//     artifact is dynamically linked, not static-musl (musl's static dlopen
//     is a failing stub).
//
// Only platform *primitives* live here (dynamic loading, sleep, atomics,
// mutexes, threads, bounded strings, subprocess capture).  GPU/driver data
// types live in gpu_core.h; nothing in this header pulls in <windows.h> UI or
// service headers.

#ifndef GREEN_CURVE_PLATFORM_H
#define GREEN_CURVE_PLATFORM_H

#define PL_ARRAY_COUNT(a) (sizeof(a) / sizeof((a)[0]))

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#if defined(_WIN32)
    #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
    #endif
    #ifndef NOMINMAX
    #define NOMINMAX
    #endif
    #include <windows.h>
#else
    #include <dlfcn.h>
    #include <pthread.h>
    #include <time.h>
    #include <unistd.h>
#endif

// ---------------------------------------------------------------------------
// Dynamic library loading
// ---------------------------------------------------------------------------

#if defined(_WIN32)
typedef HMODULE PlLib;
#define PL_LIB_NULL ((PlLib)nullptr)
#else
typedef void* PlLib;
#define PL_LIB_NULL ((PlLib)nullptr)
#endif

// Open a shared library by an already-resolved absolute path / soname.
static inline PlLib pl_lib_open(const char* path) {
    if (!path || !path[0]) return PL_LIB_NULL;
#if defined(_WIN32)
    return LoadLibraryA(path);
#else
    // RTLD_GLOBAL so subsequent NvAPI/NVML symbol lookups resolve against the
    // driver's own dependency graph, matching how the driver expects to load.
    return dlopen(path, RTLD_NOW | RTLD_GLOBAL);
#endif
}

static inline void* pl_lib_sym(PlLib lib, const char* name) {
    if (!lib || !name) return nullptr;
#if defined(_WIN32)
    return (void*)GetProcAddress(lib, name);
#else
    return dlsym(lib, name);
#endif
}

static inline void pl_lib_close(PlLib lib) {
    if (!lib) return;
#if defined(_WIN32)
    FreeLibrary(lib);
#else
    dlclose(lib);
#endif
}

// Open one of the NVIDIA driver libraries by canonical name.  On Windows the
// caller is expected to keep using its existing trusted-System32 loader; this
// helper is primarily for the Linux backend, where the libraries are resolved
// from the default loader search path (ldconfig).  kind: 0 = NvAPI, 1 = NVML.
enum PlDriverLib { PL_DRIVER_NVAPI = 0, PL_DRIVER_NVML = 1 };

static inline PlLib pl_open_driver_library(int kind) {
#if defined(_WIN32)
    // Windows uses dedicated trusted-path loaders in the backend; this fallback
    // exists only so the shim is self-contained.  System32 name only.
    return pl_lib_open(kind == PL_DRIVER_NVML ? "nvml.dll" : "nvapi64.dll");
#else
    if (kind == PL_DRIVER_NVML) {
        PlLib h = pl_lib_open("libnvidia-ml.so.1");
        if (!h) h = pl_lib_open("libnvidia-ml.so");
        return h;
    }
    // NvAPI on Linux ships as libnvidia-api.so.1 (driver >= 555).  This is the
    // same private QueryInterface entry point used on Windows via nvapi64.dll.
    PlLib h = pl_lib_open("libnvidia-api.so.1");
    if (!h) h = pl_lib_open("libnvidia-api.so");
    return h;
#endif
}

// ---------------------------------------------------------------------------
// Sleep
// ---------------------------------------------------------------------------

static inline void pl_sleep_ms(unsigned int ms) {
#if defined(_WIN32)
    Sleep(ms);
#else
    struct timespec ts;
    ts.tv_sec = (time_t)(ms / 1000u);
    ts.tv_nsec = (long)((ms % 1000u) * 1000000ul);
    while (nanosleep(&ts, &ts) == -1) {
        // EINTR: resume the remaining interval written back into ts.
    }
#endif
}

// ---------------------------------------------------------------------------
// 32-bit atomics
//
// The recovery/lifecycle flags shared with the backend are 32-bit signed
// counters.  On Windows they remain `volatile LONG` accessed via Interlocked*
// (identical codegen to before the shim); on Linux they are int32 accessed via
// GCC/Clang __atomic builtins.
// ---------------------------------------------------------------------------

#if defined(_WIN32)
typedef volatile LONG pl_atomic32;
static inline int32_t pl_atomic_load32(pl_atomic32* p)              { return (int32_t)InterlockedExchangeAdd(p, 0); }
static inline void    pl_atomic_store32(pl_atomic32* p, int32_t v)  { InterlockedExchange(p, (LONG)v); }
static inline int32_t pl_atomic_exchange32(pl_atomic32* p, int32_t v){ return (int32_t)InterlockedExchange(p, (LONG)v); }
static inline int32_t pl_atomic_add32(pl_atomic32* p, int32_t v)    { return (int32_t)InterlockedExchangeAdd(p, (LONG)v); }
#else
typedef volatile int32_t pl_atomic32;
static inline int32_t pl_atomic_load32(pl_atomic32* p)              { return __atomic_load_n(p, __ATOMIC_SEQ_CST); }
static inline void    pl_atomic_store32(pl_atomic32* p, int32_t v)  { __atomic_store_n(p, v, __ATOMIC_SEQ_CST); }
static inline int32_t pl_atomic_exchange32(pl_atomic32* p, int32_t v){ return __atomic_exchange_n(p, v, __ATOMIC_SEQ_CST); }
static inline int32_t pl_atomic_add32(pl_atomic32* p, int32_t v)    { return __atomic_fetch_add(p, v, __ATOMIC_SEQ_CST); }
#endif

// ---------------------------------------------------------------------------
// Recursive mutex
// ---------------------------------------------------------------------------

#if defined(_WIN32)
typedef CRITICAL_SECTION pl_mutex;
static inline void pl_mutex_init(pl_mutex* m)    { InitializeCriticalSection(m); }
static inline void pl_mutex_lock(pl_mutex* m)    { EnterCriticalSection(m); }
static inline void pl_mutex_unlock(pl_mutex* m)  { LeaveCriticalSection(m); }
static inline void pl_mutex_destroy(pl_mutex* m) { DeleteCriticalSection(m); }
#else
typedef pthread_mutex_t pl_mutex;
static inline void pl_mutex_init(pl_mutex* m) {
    pthread_mutexattr_t a;
    pthread_mutexattr_init(&a);
    pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(m, &a);
    pthread_mutexattr_destroy(&a);
}
static inline void pl_mutex_lock(pl_mutex* m)    { pthread_mutex_lock(m); }
static inline void pl_mutex_unlock(pl_mutex* m)  { pthread_mutex_unlock(m); }
static inline void pl_mutex_destroy(pl_mutex* m) { pthread_mutex_destroy(m); }
#endif

// ---------------------------------------------------------------------------
// Threads (used by the fan-curve reassertion worker on the daemon side)
// ---------------------------------------------------------------------------

#if defined(_WIN32)
typedef HANDLE pl_thread;
typedef DWORD  pl_thread_ret;
#define PL_THREAD_RET_OK ((pl_thread_ret)0)
typedef pl_thread_ret (WINAPI *pl_thread_fn)(void*);
static inline bool pl_thread_start(pl_thread* t, pl_thread_fn fn, void* arg) {
    HANDLE h = CreateThread(nullptr, 0, fn, arg, 0, nullptr);
    if (!h) return false;
    *t = h;
    return true;
}
static inline void pl_thread_join(pl_thread t) {
    if (!t) return;
    WaitForSingleObject(t, INFINITE);
    CloseHandle(t);
}
#else
typedef pthread_t pl_thread;
typedef void*     pl_thread_ret;
#define PL_THREAD_RET_OK ((pl_thread_ret)nullptr)
typedef pl_thread_ret (*pl_thread_fn)(void*);
static inline bool pl_thread_start(pl_thread* t, pl_thread_fn fn, void* arg) {
    return pthread_create(t, nullptr, fn, arg) == 0;
}
static inline void pl_thread_join(pl_thread t) {
    pthread_join(t, nullptr);
}
#endif

// ---------------------------------------------------------------------------
// Bounded string helpers (portable replacements for StringCch*)
//
// All guarantee NUL-termination when dstSize > 0.  gc_snprintf/gc_vsnprintf
// return the number of bytes written (excluding the NUL), or a negative value
// on encoding error — i.e. >= 0 means success, mirroring how the call sites
// previously tested !FAILED(StringCch...).
// ---------------------------------------------------------------------------

static inline int gc_vsnprintf(char* dst, size_t dstSize, const char* fmt, va_list ap)
#if defined(__GNUC__)
    __attribute__((format(printf, 3, 0)))
#endif
    ;
static inline int gc_vsnprintf(char* dst, size_t dstSize, const char* fmt, va_list ap) {
    if (!dst || dstSize == 0) return -1;
    // flawfinder: ignore -- gc_vsnprintf has a printf-format compiler attribute.
    int n = vsnprintf(dst, dstSize, fmt, ap);
    if (n < 0) { dst[0] = '\0'; return -1; }
    if ((size_t)n >= dstSize) return (int)(dstSize - 1); // truncated but NUL-terminated
    return n;
}

static inline int gc_snprintf(char* dst, size_t dstSize, const char* fmt, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 3, 4)))
#endif
    ;
static inline int gc_snprintf(char* dst, size_t dstSize, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = gc_vsnprintf(dst, dstSize, fmt, ap);
    va_end(ap);
    return n;
}

static inline void gc_strlcpy(char* dst, size_t dstSize, const char* src) {
    if (!dst || dstSize == 0) return;
    if (!src) { dst[0] = '\0'; return; }
    size_t i = 0;
    for (; i + 1 < dstSize && src[i]; i++) dst[i] = src[i];
    dst[i] = '\0';
}

static inline void gc_strlcat(char* dst, size_t dstSize, const char* src) {
    if (!dst || dstSize == 0 || !src) return;
    size_t len = strnlen(dst, dstSize);
    if (len >= dstSize) return; // not NUL-terminated; refuse
    gc_strlcpy(dst + len, dstSize - len, src);
}

// ---------------------------------------------------------------------------
// Subprocess capture (for nvidia-smi queries)
//
// Runs an executable with argv (argv[0] is the program; the array is
// NULL-terminated) and captures up to outSize-1 bytes of stdout into `out`
// (always NUL-terminated).  Returns true on a clean exit within timeoutMs.
// Implemented per-OS in platform_win32.cpp / platform_posix.cpp.
// ---------------------------------------------------------------------------

bool pl_run_capture(const char* const* argv, char* out, size_t outSize,
                    unsigned int timeoutMs);

#if defined(_WIN32)
// Append one argument using CommandLineToArgvW-compatible quoting.  The helper
// always quotes and escapes, so callers can build ShellExecute/CreateProcess
// parameter strings from argv-like inputs without argument injection.
bool pl_append_quoted_arg_w(WCHAR* cmd, size_t cmdCount, const WCHAR* arg);
#endif

#endif // GREEN_CURVE_PLATFORM_H
