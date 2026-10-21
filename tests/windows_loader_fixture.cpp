// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#if defined(GC_LOADER_FIXTURE_DLL)
extern "C" __declspec(dllexport) DWORD WINAPI GetThemeAppProperties() {
    return 0x4743;
}
#else
#include <uxtheme.h>
int main() { return GetThemeAppProperties() == 0x4743 ? 37 : 0; }
#endif
