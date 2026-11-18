// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <strsafe.h>
#include "win32_utf8_paths.h"
#include <shobjidl.h>
#include <string>
#include <cstdio>

// Balanced, window-thread COM lifetime; no shell execution or path rewriting.
static inline bool profile_transfer_choose_file(HWND owner, bool importing, std::string& output, char* error, size_t size) {
    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    if (FAILED(initialized)) { std::snprintf(error, size, "Could not initialize the native file picker"); return false; }
    struct ComScope { ~ComScope() { CoUninitialize(); } } scope;
    IFileDialog* dialog = nullptr;
    HRESULT status = CoCreateInstance(importing ? CLSID_FileOpenDialog : CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER,
        IID_IFileDialog, (void**)&dialog);
    if (SUCCEEDED(status)) {
        DWORD flags = 0; status = dialog->GetOptions(&flags);
        if (SUCCEEDED(status)) status = dialog->SetOptions(flags | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST |
            (importing ? FOS_FILEMUSTEXIST : FOS_OVERWRITEPROMPT));
        const COMDLG_FILTERSPEC types[] = {{L"Green Curve profiles (*.gcprofile)", L"*.gcprofile"},
            {L"Older Green Curve settings (*.ini)", L"*.ini"}, {L"All files", L"*.*"}};
        if (SUCCEEDED(status)) status = dialog->SetFileTypes(importing ? 3 : 1, types);
        if (SUCCEEDED(status) && !importing) status = dialog->SetDefaultExtension(L"gcprofile");
        if (SUCCEEDED(status) && !importing) status = dialog->SetFileName(L"Green Curve profiles.gcprofile");
        if (SUCCEEDED(status)) status = dialog->SetTitle(importing ? L"Import profiles from file" : L"Export profiles to file");
        if (SUCCEEDED(status)) status = dialog->Show(owner);
        IShellItem* item = nullptr;
        if (SUCCEEDED(status)) status = dialog->GetResult(&item);
        PWSTR wide = nullptr;
        if (SUCCEEDED(status)) status = item->GetDisplayName(SIGDN_FILESYSPATH, &wide);
        if (SUCCEEDED(status)) {
            char path[4096]{};
            if (!gc_wide_to_utf8(wide, path, sizeof(path))) status = HRESULT_FROM_WIN32(ERROR_FILENAME_EXCED_RANGE);
            else output = path;
        }
        if (wide) CoTaskMemFree(wide);
        if (item) item->Release(); dialog->Release();
    }
    if (FAILED(status) && status != HRESULT_FROM_WIN32(ERROR_CANCELLED))
        std::snprintf(error, size, "Native file selection failed (0x%08lX)", (unsigned long)status);
    return SUCCEEDED(status);
}
