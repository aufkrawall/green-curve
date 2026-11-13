// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#if defined(_WIN32)
static int installer_stale_reader_cases(const wchar_t* directory, const wchar_t* staged,
    const wchar_t* backup, const wchar_t* destination) {
    const auto readByte = [](HANDLE reader, char expected) {
        char byte = 0; DWORD count = 0;
        return ReadFile(reader, &byte, 1, &count, nullptr) && count == 1 && byte == expected;
    };
    const auto readPath = [&](const wchar_t* path, char expected) {
        HANDLE reader = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr,
            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (reader == INVALID_HANDLE_VALUE) return false;
        bool ok = readByte(reader, expected); CloseHandle(reader); return ok;
    };
    char prior = 'O';
    for (int rollback = 0; rollback < 2; ++rollback) {
        wchar_t path[MAX_PATH]{};
        StringCchPrintfW(path, MAX_PATH, L"%ls.%ls", destination, rollback ? L"gcrestore" : L"gcnew");
        HANDLE writer = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (writer == INVALID_HANDLE_VALUE) return 8751;
        DWORD written = 0;
        bool created = WriteFile(writer, "R", 1, &written, nullptr) && written == 1;
        CloseHandle(writer);
        if (!created) { DeleteFileW(path); return 8751; }
        HANDLE held = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (held == INVALID_HANDLE_VALUE) { DeleteFileW(path); return 8752; }
        bool discarded = false;
        HRESULT result = rollback ? gc_restore_previous_install_file(backup, destination, &discarded) :
            gc_replace_staged_install_file(staged, destination, &discarded);
        DWORD error = GetLastError();
        const bool retainedReader = readByte(held, 'R');
        const char expected = SUCCEEDED(result) ? (rollback ? 'O' : 'N') : prior;
        const bool destinationOk = readPath(destination, expected);
        const bool nameAbsent = GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES;
        const bool ok = retainedReader && destinationOk &&
            (SUCCEEDED(result) && discarded && nameAbsent);
        CloseHandle(held);
        wchar_t pattern[MAX_PATH]{};
        StringCchPrintfW(pattern, MAX_PATH, L"%ls.gcattempt-*", path);
        WIN32_FIND_DATAW entry{};
        HANDLE search = FindFirstFileW(pattern, &entry);
        const bool attemptGone = search == INVALID_HANDLE_VALUE && GetLastError() == ERROR_FILE_NOT_FOUND;
        if (search != INVALID_HANDLE_VALUE) FindClose(search);
        DeleteFileW(path);
        if (!ok || !attemptGone) {
            fprintf(stderr, "installer held-reader fixture: rollback=%d attemptGone=%d hr=0x%08lx error=%lu discarded=%d reader=%d destination=%d absent=%d\n",
                rollback, attemptGone ? 1 : 0, (unsigned long)result, (unsigned long)error,
                discarded ? 1 : 0, retainedReader ? 1 : 0, destinationOk ? 1 : 0, nameAbsent ? 1 : 0);
            return 8753 + rollback;
        }
        prior = expected;
    }
    // A non-sharing reader still blocks recovery; the installed target and
    // the obstructing leaf must both remain intact.
    wchar_t blocked[MAX_PATH]{};
    StringCchPrintfW(blocked, MAX_PATH, L"%ls.gcnew", destination);
    HANDLE writer = CreateFileW(blocked, GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (writer == INVALID_HANDLE_VALUE) return 8757;
    DWORD count = 0;
    bool written = WriteFile(writer, "R", 1, &count, nullptr) && count == 1;
    CloseHandle(writer);
    HANDLE reader = CreateFileW(blocked, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    bool refused = written && reader != INVALID_HANDLE_VALUE &&
        FAILED(gc_replace_staged_install_file(staged, destination)) && readPath(destination, 'O');
    if (reader != INVALID_HANDLE_VALUE) CloseHandle(reader);
    refused = refused && readPath(blocked, 'R');
    DeleteFileW(blocked);
    if (!refused) return 8757;
    // Mapping a COPY of this fixture as an image must not permit deletion of
    // that image. The project's running-image deletion prohibition stays intact.
    wchar_t image[MAX_PATH]{}, self[MAX_PATH]{};
    StringCchPrintfW(image, MAX_PATH, L"%ls\\image.gcnew", directory);
    if (!GetModuleFileNameW(nullptr, self, MAX_PATH) || !CopyFileW(self, image, TRUE)) return 8758;
    HANDLE file = CreateFileW(image, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, 0, nullptr);
    HANDLE mapping = file == INVALID_HANDLE_VALUE ? nullptr :
        CreateFileMappingW(file, nullptr, PAGE_READONLY | SEC_IMAGE, 0, 0, nullptr);
    bool imageRefused = mapping && !gc_discard_stale_install_temporary(image) &&
        GetFileAttributesW(image) != INVALID_FILE_ATTRIBUTES;
    if (mapping) CloseHandle(mapping);
    if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    DeleteFileW(image);
    if (!imageRefused) return 8758;
    return 0;
}
#endif
