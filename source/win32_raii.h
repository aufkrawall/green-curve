#ifndef GREEN_CURVE_WIN32_RAII_H
#define GREEN_CURVE_WIN32_RAII_H

#include "app_shared.h"

struct ScopedHandle {
    HANDLE handle;

    ScopedHandle() : handle(nullptr) {}
    explicit ScopedHandle(HANDLE value) : handle(value) {}
    ~ScopedHandle() { reset(); }

    ScopedHandle(const ScopedHandle&) = delete;
    ScopedHandle& operator=(const ScopedHandle&) = delete;

    HANDLE get() const { return handle; }
    bool valid() const { return handle && handle != INVALID_HANDLE_VALUE; }
    operator HANDLE() const { return handle; }

    HANDLE detach() {
        HANDLE value = handle;
        handle = nullptr;
        return value;
    }

    void reset(HANDLE value = nullptr) {
        if (valid()) CloseHandle(handle);
        handle = value;
    }
};

struct ScopedGdiObject {
    HGDIOBJ object;

    ScopedGdiObject() : object(nullptr) {}
    explicit ScopedGdiObject(HGDIOBJ value) : object(value) {}
    ~ScopedGdiObject() { reset(); }

    ScopedGdiObject(const ScopedGdiObject&) = delete;
    ScopedGdiObject& operator=(const ScopedGdiObject&) = delete;

    HGDIOBJ get() const { return object; }
    bool valid() const { return object != nullptr; }
    operator HGDIOBJ() const { return object; }

    HGDIOBJ detach() {
        HGDIOBJ value = object;
        object = nullptr;
        return value;
    }

    void reset(HGDIOBJ value = nullptr) {
        if (object) DeleteObject(object);
        object = value;
    }
};

#endif
