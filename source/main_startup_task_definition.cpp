// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT

// ============================================================================
// Scheduled-task definition verification
// ============================================================================
//
// The startup task is app-owned: a task with the right name is not necessarily
// a task that still launches this executable, this user, and this config.  In
// particular, upgrades and manual Task Scheduler edits can leave an old task
// with a fixed logon delay or an elevated principal.  Query the registered
// XML and compare the small, security-relevant contract before accepting it.

#include <wctype.h>

struct StartupTaskXmlSpan {
    const WCHAR* begin;
    const WCHAR* end;
};

static bool startup_task_xml_is_space(WCHAR c) {
    return c == L' ' || c == L'\t' || c == L'\r' || c == L'\n';
}

static bool startup_task_xml_tag_separator(WCHAR c) {
    return c == L'>' || c == L'/' || startup_task_xml_is_space(c);
}

static bool startup_task_xml_tag_matches(const WCHAR* text, const WCHAR* end, const WCHAR* tag) {
    if (!text || !tag) return false;
    for (const WCHAR* p = tag; *p; ++p, ++text) {
        if (text >= end || towlower(*text) != towlower(*p)) return false;
    }
    return text < end && startup_task_xml_tag_separator(*text);
}

static const WCHAR* startup_task_xml_find_open_tag(const WCHAR* begin, const WCHAR* end, const WCHAR* tag) {
    if (!begin || !end || !tag) return nullptr;
    for (const WCHAR* p = begin; p < end; ++p) {
        if (*p != L'<' || p + 1 >= end) continue;
        WCHAR next = p[1];
        if (next == L'/' || next == L'?' || next == L'!') continue;
        if (startup_task_xml_tag_matches(p + 1, end, tag)) return p;
    }
    return nullptr;
}

static const WCHAR* startup_task_xml_find_close_tag(const WCHAR* begin, const WCHAR* end, const WCHAR* tag) {
    if (!begin || !end || !tag) return nullptr;
    for (const WCHAR* p = begin; p + 2 < end; ++p) {
        if (p[0] == L'<' && p[1] == L'/' && startup_task_xml_tag_matches(p + 2, end, tag)) return p;
    }
    return nullptr;
}

static size_t startup_task_xml_count_open_tags(const StartupTaskXmlSpan& scope,
    const WCHAR* tag) {
    size_t count = 0;
    const WCHAR* cursor = scope.begin;
    while (cursor && cursor < scope.end) {
        const WCHAR* found = startup_task_xml_find_open_tag(cursor, scope.end, tag);
        if (!found) break;
        ++count;
        cursor = found + 1;
    }
    return count;
}

// Task Scheduler has a fixed XML schema, but this classifier is also exercised
// directly by fixtures.  Verify direct children rather than merely finding the
// first matching descendant: a second trigger/action must not be hidden behind
// a valid first one.
static bool startup_task_xml_has_exact_direct_children(
    const StartupTaskXmlSpan& parent, const WCHAR* expectedTag,
    size_t expectedCount) {
    size_t directCount = 0;
    size_t matchingCount = 0;
    int depth = 0;
    const WCHAR* cursor = parent.begin;
    while (cursor && cursor < parent.end) {
        const WCHAR* open = cursor;
        while (open < parent.end && *open != L'<') ++open;
        if (open >= parent.end) break;
        if (open + 1 >= parent.end) return false;

        if (open[1] == L'!' || open[1] == L'?') {
            const WCHAR* end = open + 2;
            while (end < parent.end && *end != L'>') ++end;
            if (end >= parent.end) return false;
            cursor = end + 1;
            continue;
        }

        bool closing = open[1] == L'/';
        const WCHAR* name = open + (closing ? 2 : 1);
        const WCHAR* end = name;
        WCHAR quote = 0;
        while (end < parent.end) {
            if (quote) {
                if (*end == quote) quote = 0;
            } else if (*end == L'\'' || *end == L'"') {
                quote = *end;
            } else if (*end == L'>') {
                break;
            }
            ++end;
        }
        if (end >= parent.end || quote) return false;

        const WCHAR* beforeEnd = end;
        while (beforeEnd > name && startup_task_xml_is_space(beforeEnd[-1])) --beforeEnd;
        bool selfClosing = !closing && beforeEnd > name && beforeEnd[-1] == L'/';

        if (closing) {
            if (depth <= 0) return false;
            --depth;
        } else {
            if (depth == 0) {
                ++directCount;
                if (startup_task_xml_tag_matches(name, end + 1, expectedTag)) {
                    ++matchingCount;
                }
            }
            if (!selfClosing) ++depth;
        }
        cursor = end + 1;
    }
    return depth == 0 && directCount == expectedCount &&
        matchingCount == expectedCount;
}

static bool startup_task_duration_is_well_formed(const WCHAR* value) {
    if (!value || towupper(value[0]) != L'P') return false;
    bool haveDigit = false;
    for (const WCHAR* p = value + 1; *p; ++p) {
        WCHAR c = towupper(*p);
        if (c >= L'0' && c <= L'9') {
            haveDigit = true;
            continue;
        }
        if (c != L'T' && c != L'D' && c != L'H' && c != L'M' &&
            c != L'S' && c != L'.' && c != L',') {
            return false;
        }
    }
    return haveDigit;
}

static bool startup_task_xml_get_scope(const StartupTaskXmlSpan& parent, const WCHAR* tag, StartupTaskXmlSpan* out) {
    if (!out) return false;
    out->begin = nullptr;
    out->end = nullptr;
    const WCHAR* open = startup_task_xml_find_open_tag(parent.begin, parent.end, tag);
    if (!open) return false;
    const WCHAR* openEnd = open;
    while (openEnd < parent.end && *openEnd != L'>') ++openEnd;
    if (openEnd >= parent.end || (openEnd > open && openEnd[-1] == L'/')) return false;
    const WCHAR* close = startup_task_xml_find_close_tag(openEnd + 1, parent.end, tag);
    if (!close) return false;
    out->begin = openEnd + 1;
    out->end = close;
    return true;
}

static bool startup_task_xml_decode_value(const WCHAR* begin, const WCHAR* end, WCHAR* out, size_t outCount) {
    if (!out || outCount == 0) return false;
    out[0] = 0;
    while (begin < end && startup_task_xml_is_space(*begin)) ++begin;
    while (end > begin && startup_task_xml_is_space(end[-1])) --end;

    size_t used = 0;
    for (const WCHAR* p = begin; p < end; ++p) {
        WCHAR value = *p;
        if (value == L'&') {
            if (end - p >= 5 && wcsncmp(p, L"&amp;", 5) == 0) {
                value = L'&';
                p += 4;
            } else if (end - p >= 4 && wcsncmp(p, L"&lt;", 4) == 0) {
                value = L'<';
                p += 3;
            } else if (end - p >= 4 && wcsncmp(p, L"&gt;", 4) == 0) {
                value = L'>';
                p += 3;
            } else if (end - p >= 6 && wcsncmp(p, L"&quot;", 6) == 0) {
                value = L'\"';
                p += 5;
            } else if (end - p >= 6 && wcsncmp(p, L"&apos;", 6) == 0) {
                value = L'\'';
                p += 5;
            } else {
                return false;
            }
        }
        if (used + 1 >= outCount) return false;
        out[used++] = value;
    }
    out[used] = 0;
    return true;
}

static bool startup_task_xml_get_value(const StartupTaskXmlSpan& parent, const WCHAR* tag, WCHAR* out, size_t outCount) {
    StartupTaskXmlSpan value = {};
    return startup_task_xml_get_scope(parent, tag, &value) &&
        startup_task_xml_decode_value(value.begin, value.end, out, outCount);
}

static void startup_task_validation_add_detail(char* detail, size_t detailSize, const char* text) {
    if (!detail || detailSize == 0 || !text || !text[0]) return;
    if (detail[0]) StringCchCatA(detail, detailSize, "; ");
    StringCchCatA(detail, detailSize, text);
}

static bool startup_task_open_query_output(WCHAR* pathOut, size_t pathOutCount, HANDLE* fileOut, char* detail, size_t detailSize) {
    if (!pathOut || pathOutCount == 0 || !fileOut) return false;
    *fileOut = INVALID_HANDLE_VALUE;
    pathOut[0] = 0;

    WCHAR userDataDir[MAX_PATH] = {};
    if (!utf8_to_wide(g_userDataDir, userDataDir, ARRAY_COUNT(userDataDir))) {
        set_message(detail, detailSize, "Could not convert startup-task data directory");
        return false;
    }

    SECURITY_ATTRIBUTES sa = {};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    DWORD nonce = GetTickCount() ^ GetCurrentThreadId();
    for (DWORD attempt = 0; attempt < 8; ++attempt) {
        HRESULT hr = StringCchPrintfW(pathOut, pathOutCount,
            L"%ls\\startup_task_query_%08lX_%lu.xml", userDataDir,
            (unsigned long)(nonce + attempt), (unsigned long)attempt);
        if (FAILED(hr)) {
            set_message(detail, detailSize, "Startup-task query output path is too long");
            return false;
        }
        HANDLE file = CreateFileW(pathOut, GENERIC_WRITE, FILE_SHARE_READ, &sa, CREATE_NEW,
            FILE_ATTRIBUTE_TEMPORARY | FILE_ATTRIBUTE_NOT_CONTENT_INDEXED, nullptr);
        if (file != INVALID_HANDLE_VALUE) {
            *fileOut = file;
            return true;
        }
        if (GetLastError() != ERROR_FILE_EXISTS && GetLastError() != ERROR_ALREADY_EXISTS) break;
    }
    set_message(detail, detailSize, "Could not create startup-task query output (error %lu)", GetLastError());
    return false;
}

static bool startup_task_decode_query_xml(const BYTE* bytes, size_t byteCount, WCHAR** xmlOut, char* detail, size_t detailSize) {
    if (!xmlOut) return false;
    *xmlOut = nullptr;
    if (!bytes || byteCount == 0 || byteCount > 128 * 1024) {
        set_message(detail, detailSize, "Startup-task XML output has an invalid size");
        return false;
    }

    bool utf16Le = byteCount >= 2 && bytes[0] == 0xFF && bytes[1] == 0xFE;
    bool utf16Be = byteCount >= 2 && bytes[0] == 0xFE && bytes[1] == 0xFF;
    size_t offset = (utf16Le || utf16Be) ? 2 : 0;
    if (!utf16Le && !utf16Be && byteCount >= 4 && bytes[1] == 0 && bytes[3] == 0) utf16Le = true;

    if (utf16Le || utf16Be) {
        if (((byteCount - offset) & 1u) != 0) {
            set_message(detail, detailSize, "Startup-task XML UTF-16 output is truncated");
            return false;
        }
        size_t charCount = (byteCount - offset) / 2;
        WCHAR* xml = (WCHAR*)malloc((charCount + 1) * sizeof(WCHAR));
        if (!xml) {
            set_message(detail, detailSize, "Out of memory decoding startup-task XML");
            return false;
        }
        for (size_t i = 0; i < charCount; ++i) {
            BYTE lo = bytes[offset + i * 2];
            BYTE hi = bytes[offset + i * 2 + 1];
            xml[i] = (WCHAR)(utf16Be ? ((unsigned int)lo << 8 | hi) : ((unsigned int)hi << 8 | lo));
        }
        xml[charCount] = 0;
        *xmlOut = xml;
        return true;
    }

    size_t utf8Offset = byteCount >= 3 && bytes[0] == 0xEF && bytes[1] == 0xBB && bytes[2] == 0xBF ? 3 : 0;
    int sourceBytes = (int)(byteCount - utf8Offset);
    UINT codePage = CP_UTF8;
    int chars = MultiByteToWideChar(codePage, MB_ERR_INVALID_CHARS, (const char*)bytes + utf8Offset, sourceBytes, nullptr, 0);
    if (chars <= 0) {
        codePage = CP_ACP;
        chars = MultiByteToWideChar(codePage, 0, (const char*)bytes + utf8Offset, sourceBytes, nullptr, 0);
    }
    if (chars <= 0) {
        set_message(detail, detailSize, "Could not decode startup-task XML output");
        return false;
    }
    WCHAR* xml = (WCHAR*)malloc(((size_t)chars + 1) * sizeof(WCHAR));
    if (!xml) {
        set_message(detail, detailSize, "Out of memory decoding startup-task XML");
        return false;
    }
    if (MultiByteToWideChar(codePage, codePage == CP_UTF8 ? MB_ERR_INVALID_CHARS : 0,
            (const char*)bytes + utf8Offset, sourceBytes, xml, chars) <= 0) {
        free(xml);
        set_message(detail, detailSize, "Could not convert startup-task XML output");
        return false;
    }
    xml[chars] = 0;
    *xmlOut = xml;
    return true;
}

static bool startup_task_query_xml(const WCHAR* taskName, WCHAR** xmlOut, char* detail, size_t detailSize) {
    if (!xmlOut) return false;
    *xmlOut = nullptr;
    if (!taskName || !taskName[0]) {
        set_message(detail, detailSize, "Invalid startup-task name for XML query");
        return false;
    }

    WCHAR schtasksPath[MAX_PATH] = {};
    UINT pathLen = GetSystemDirectoryW(schtasksPath, ARRAY_COUNT(schtasksPath));
    if (pathLen == 0 || pathLen >= ARRAY_COUNT(schtasksPath) ||
        FAILED(StringCchCatW(schtasksPath, ARRAY_COUNT(schtasksPath), L"\\schtasks.exe"))) {
        set_message(detail, detailSize, "Could not locate schtasks.exe for XML query");
        return false;
    }

    WCHAR commandLine[2048] = {};
    if (FAILED(StringCchPrintfW(commandLine, ARRAY_COUNT(commandLine),
            L"\"%ls\" /query /tn \"%ls\" /xml", schtasksPath, taskName))) {
        set_message(detail, detailSize, "Startup-task XML query command is too long");
        return false;
    }

    WCHAR outputPath[MAX_PATH] = {};
    HANDLE output = INVALID_HANDLE_VALUE;
    if (!startup_task_open_query_output(outputPath, ARRAY_COUNT(outputPath), &output, detail, detailSize)) return false;

    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW | STARTF_USESTDHANDLES;
    si.wShowWindow = SW_HIDE;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = output;
    si.hStdError = output;
    PROCESS_INFORMATION pi = {};
    bool started = CreateProcessW(schtasksPath, commandLine, nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
        nullptr, nullptr, &si, &pi) != FALSE;
    CloseHandle(output);
    if (!started) {
        DWORD error = GetLastError();
        DeleteFileW(outputPath);
        set_message(detail, detailSize, "Could not start schtasks XML query (error %lu)", error);
        return false;
    }

    DWORD wait = WaitForSingleObject(pi.hProcess, 15000);
    if (wait == WAIT_TIMEOUT) {
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        DeleteFileW(outputPath);
        set_message(detail, detailSize, "Startup-task XML query timed out");
        return false;
    }
    DWORD exitCode = (DWORD)-1;
    GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    if (exitCode != 0) {
        DeleteFileW(outputPath);
        set_message(detail, detailSize, "schtasks XML query exited %lu", exitCode);
        return false;
    }

    HANDLE input = CreateFileW(outputPath, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (input == INVALID_HANDLE_VALUE) {
        DWORD error = GetLastError();
        DeleteFileW(outputPath);
        set_message(detail, detailSize, "Could not read startup-task XML query output (error %lu)", error);
        return false;
    }
    LARGE_INTEGER size = {};
    bool sizeOk = GetFileSizeEx(input, &size) != FALSE && size.QuadPart > 0 && size.QuadPart <= 128 * 1024;
    if (!sizeOk) {
        CloseHandle(input);
        DeleteFileW(outputPath);
        set_message(detail, detailSize, "Startup-task XML query output is empty or too large");
        return false;
    }
    size_t byteCount = (size_t)size.QuadPart;
    BYTE* bytes = (BYTE*)malloc(byteCount);
    if (!bytes) {
        CloseHandle(input);
        DeleteFileW(outputPath);
        set_message(detail, detailSize, "Out of memory reading startup-task XML");
        return false;
    }
    DWORD read = 0;
    bool readOk = ReadFile(input, bytes, (DWORD)byteCount, &read, nullptr) != FALSE && read == byteCount;
    CloseHandle(input);
    DeleteFileW(outputPath);
    if (!readOk) {
        free(bytes);
        set_message(detail, detailSize, "Could not read complete startup-task XML output");
        return false;
    }
    bool decoded = startup_task_decode_query_xml(bytes, byteCount, xmlOut, detail, detailSize);
    free(bytes);
    return decoded;
}

static bool startup_task_lookup_account_sid(const WCHAR* accountName, WCHAR* sidOut, size_t sidOutCount) {
    if (!accountName || !accountName[0] || !sidOut || sidOutCount == 0) return false;
    sidOut[0] = 0;
    DWORD sidBytes = 0;
    DWORD domainChars = 0;
    SID_NAME_USE use = SidTypeUnknown;
    LookupAccountNameW(nullptr, accountName, nullptr, &sidBytes, nullptr, &domainChars, &use);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || sidBytes == 0) return false;
    PSID sid = malloc(sidBytes);
    WCHAR* domain = (WCHAR*)malloc(((size_t)domainChars + 1) * sizeof(WCHAR));
    if (!sid || !domain) {
        free(sid);
        free(domain);
        return false;
    }
    DWORD sidSize = sidBytes;
    DWORD domainSize = domainChars + 1;
    bool ok = LookupAccountNameW(nullptr, accountName, sid, &sidSize, domain, &domainSize, &use) != FALSE;
    LPWSTR sidText = nullptr;
    if (ok) ok = ConvertSidToStringSidW(sid, &sidText) != FALSE && sidText && sidText[0];
    if (ok) ok = SUCCEEDED(StringCchCopyW(sidOut, sidOutCount, sidText));
    if (sidText) LocalFree(sidText);
    free(domain);
    free(sid);
    return ok;
}

static bool startup_task_user_matches(const WCHAR* actual, const WCHAR* expectedSam) {
    if (!actual || !actual[0] || !expectedSam || !expectedSam[0]) return false;
    if (_wcsicmp(actual, expectedSam) == 0) return true;
    WCHAR expectedSid[256] = {};
    return startup_task_lookup_account_sid(expectedSam, expectedSid, ARRAY_COUNT(expectedSid)) &&
        _wcsicmp(actual, expectedSid) == 0;
}

static bool startup_task_arguments_match(const WCHAR* actual, const WCHAR* expectedConfigPath) {
    if (!actual || !expectedConfigPath) return false;
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(actual, &argc);
    if (!argv) return false;
    bool ok = argc == 3 &&
        _wcsicmp(argv[0], L"--logon-start") == 0 &&
        _wcsicmp(argv[1], L"--config") == 0 &&
        _wcsicmp(argv[2], expectedConfigPath) == 0;
    LocalFree(argv);
    return ok;
}

static bool startup_task_xml_element_is_empty(const StartupTaskXmlSpan& parent, const WCHAR* tag) {
    const WCHAR* open = startup_task_xml_find_open_tag(parent.begin, parent.end, tag);
    if (!open) return false;
    const WCHAR* openEnd = open;
    while (openEnd < parent.end && *openEnd != L'>') ++openEnd;
    if (openEnd >= parent.end) return false;
    const WCHAR* p = openEnd;
    while (p > open && startup_task_xml_is_space(p[-1])) --p;
    return p > open && p[-1] == L'/';
}

// Classify an already-decoded task definition without touching Task Scheduler.
// This split is deliberate: fixture tests can exercise canonical, compatible
// legacy, and broken XML deterministically, while the production wrapper below
// only supplies the registered XML and expected identity/path context.
StartupTaskDefinitionClass startup_task_definition_classify_xml(
    const WCHAR* xml, const WCHAR* expectedUser, const WCHAR* exePath,
    const WCHAR* cfgPath, const WCHAR* expectedWorkingDir,
    char* detail, size_t detailSize) {
    if (detail && detailSize) detail[0] = 0;
    if (!xml || !expectedUser || !exePath || !cfgPath || !expectedWorkingDir ||
        !xml[0] || !expectedUser[0] || !exePath[0] || !cfgPath[0] || !expectedWorkingDir[0]) {
        set_message(detail, detailSize, "Invalid expected startup-task definition");
        return STARTUP_TASK_DEFINITION_BROKEN;
    }

    StartupTaskXmlSpan document = { xml, xml + wcslen(xml) };
    bool broken = false;
    bool canonical = true;
    WCHAR value[2048] = {};
    StartupTaskXmlSpan scope = {};
    auto mark_broken = [&](const char* reason) {
        startup_task_validation_add_detail(detail, detailSize, reason);
        broken = true;
        canonical = false;
    };
    auto mark_legacy = [&](const char* reason) {
        startup_task_validation_add_detail(detail, detailSize, reason);
        canonical = false;
    };

    StartupTaskXmlSpan triggers = {};
    if (!startup_task_xml_get_scope(document, L"Triggers", &triggers) ||
        startup_task_xml_count_open_tags(document, L"Triggers") != 1) {
        mark_broken("missing or duplicate Triggers section");
    } else if (!startup_task_xml_has_exact_direct_children(
                   triggers, L"LogonTrigger", 1)) {
        mark_broken("extra or duplicate task trigger");
    }
    if (!triggers.begin ||
        !startup_task_xml_get_scope(triggers, L"LogonTrigger", &scope)) {
        mark_broken("missing LogonTrigger");
    } else {
        // Task Scheduler may omit a schema-default true value, or serialize it
        // as an empty element.  Only an explicit false/malformed value disables
        // the trigger and therefore makes the task unusable.
        if (startup_task_xml_find_open_tag(scope.begin, scope.end, L"Enabled") &&
            !startup_task_xml_element_is_empty(scope, L"Enabled")) {
            if (!startup_task_xml_get_value(scope, L"Enabled", value, ARRAY_COUNT(value)) ||
                _wcsicmp(value, L"true") != 0) {
                mark_broken("logon trigger is disabled");
            }
        }
        if (!startup_task_xml_get_value(scope, L"UserId", value, ARRAY_COUNT(value)) || !startup_task_user_matches(value, expectedUser)) {
            mark_broken("logon trigger user differs");
        }
        // A legacy delay still delivers the authenticated handoff, so it is
        // functional.  It is merely non-canonical and repaired best-effort.
        if (startup_task_xml_find_open_tag(scope.begin, scope.end, L"Delay")) {
            if (!startup_task_xml_element_is_empty(scope, L"Delay")) {
                value[0] = 0;
                if (!startup_task_xml_get_value(scope, L"Delay", value, ARRAY_COUNT(value)) ||
                    !startup_task_duration_is_well_formed(value)) {
                    mark_broken("logon delay is malformed");
                } else if (_wcsicmp(value, L"PT0S") != 0) {
                    mark_legacy("compatible legacy logon delay");
                }
            }
        }
        if (startup_task_xml_find_open_tag(scope.begin, scope.end, L"Repetition")) {
            mark_broken("logon trigger repetition is not allowed");
        }
        if (startup_task_xml_find_open_tag(scope.begin, scope.end, L"StartBoundary") ||
            startup_task_xml_find_open_tag(scope.begin, scope.end, L"EndBoundary")) {
            mark_broken("logon trigger boundary can suppress future handoffs");
        }
    }

    StartupTaskXmlSpan principals = {};
    if (!startup_task_xml_get_scope(document, L"Principals", &principals) ||
        startup_task_xml_count_open_tags(document, L"Principals") != 1 ||
        !startup_task_xml_has_exact_direct_children(principals, L"Principal", 1) ||
        !startup_task_xml_get_scope(principals, L"Principal", &scope)) {
        mark_broken("missing Principal");
    } else {
        if (!startup_task_xml_get_value(scope, L"UserId", value, ARRAY_COUNT(value)) || !startup_task_user_matches(value, expectedUser)) {
            mark_broken("principal user differs");
        }
        if (!startup_task_xml_get_value(scope, L"LogonType", value, ARRAY_COUNT(value)) || _wcsicmp(value, L"InteractiveToken") != 0) {
            mark_broken("principal logon type differs");
        }
        // LeastPrivilege is the schema default.  HighestAvailable is an older
        // Green Curve definition that remains safe/functional because the task
        // only sends an identity-bound handoff and performs no hardware write.
        if (startup_task_xml_find_open_tag(scope.begin, scope.end, L"RunLevel") &&
            !startup_task_xml_element_is_empty(scope, L"RunLevel")) {
            if (!startup_task_xml_get_value(scope, L"RunLevel", value, ARRAY_COUNT(value))) {
                mark_broken("principal run level is malformed");
            } else if (_wcsicmp(value, L"LeastPrivilege") != 0) {
                if (_wcsicmp(value, L"HighestAvailable") == 0) {
                    mark_legacy("compatible legacy HighestAvailable principal");
                } else {
                    mark_broken("principal run level is unsupported");
                }
            }
        }
    }

    StartupTaskXmlSpan actions = {};
    if (!startup_task_xml_get_scope(document, L"Actions", &actions) ||
        startup_task_xml_count_open_tags(document, L"Actions") != 1 ||
        !startup_task_xml_has_exact_direct_children(actions, L"Exec", 1) ||
        !startup_task_xml_get_scope(actions, L"Exec", &scope)) {
        mark_broken("missing, duplicate, or additional task action");
    } else {
        if (!startup_task_xml_get_value(scope, L"Command", value, ARRAY_COUNT(value)) || _wcsicmp(value, exePath) != 0) {
            mark_broken("action command differs");
        }
        if (!startup_task_xml_get_value(scope, L"Arguments", value, ARRAY_COUNT(value)) || !startup_task_arguments_match(value, cfgPath)) {
            mark_broken("action arguments differ");
        }
        if (!startup_task_xml_get_value(scope, L"WorkingDirectory", value, ARRAY_COUNT(value)) || _wcsicmp(value, expectedWorkingDir) != 0) {
            mark_broken("action working directory differs");
        }
    }

    if (!startup_task_xml_get_scope(document, L"Settings", &scope) ||
        startup_task_xml_count_open_tags(document, L"Settings") != 1) {
        // Schema defaults include battery gating and a very long execution
        // limit, so a wholly omitted Settings section is not a safe legacy
        // variant for a reliable handoff.
        mark_broken("missing or duplicate task Settings section");
    } else {
        // Like the trigger flag, task-level Enabled=true can be omitted/empty.
        if (startup_task_xml_find_open_tag(scope.begin, scope.end, L"Enabled") &&
            !startup_task_xml_element_is_empty(scope, L"Enabled")) {
            if (!startup_task_xml_get_value(scope, L"Enabled", value, ARRAY_COUNT(value)) ||
                _wcsicmp(value, L"true") != 0) {
                mark_broken("task is disabled");
            }
        }
        if (!startup_task_xml_get_value(scope, L"StartWhenAvailable", value, ARRAY_COUNT(value)) || _wcsicmp(value, L"true") != 0) {
            mark_broken("StartWhenAvailable is disabled or omitted");
        }
        if (!startup_task_xml_find_open_tag(scope.begin, scope.end, L"MultipleInstancesPolicy") ||
            startup_task_xml_element_is_empty(scope, L"MultipleInstancesPolicy")) {
            // IgnoreNew is the Task Scheduler schema default and is safe.
            mark_legacy("compatible legacy omitted MultipleInstancesPolicy default");
        } else if (!startup_task_xml_get_value(scope, L"MultipleInstancesPolicy", value, ARRAY_COUNT(value)) ||
                   _wcsicmp(value, L"IgnoreNew") != 0) {
            mark_broken("multiple-instance policy can suppress or delay the handoff");
        }
        if (!startup_task_xml_get_value(scope, L"ExecutionTimeLimit", value, ARRAY_COUNT(value))) {
            mark_broken("execution time limit is omitted or malformed");
        } else if (_wcsicmp(value, L"PT0S") == 0) {
            mark_legacy("compatible legacy PT0S execution time limit");
        } else if (_wcsicmp(value, L"PT3M") != 0) {
            mark_broken("execution time limit cannot contain the handoff wait");
        }

        auto require_false = [&](const WCHAR* tag, const char* reason) {
            if (!startup_task_xml_get_value(scope, tag, value, ARRAY_COUNT(value)) ||
                _wcsicmp(value, L"false") != 0) {
                mark_broken(reason);
            }
        };
        auto require_optional_value = [&](const WCHAR* tag,
                                           const WCHAR* expected,
                                           const char* reason) {
            if (!startup_task_xml_find_open_tag(scope.begin, scope.end, tag) ||
                startup_task_xml_element_is_empty(scope, tag)) {
                return; // known-safe schema default
            }
            if (!startup_task_xml_get_value(scope, tag, value, ARRAY_COUNT(value)) ||
                _wcsicmp(value, expected) != 0) {
                mark_broken(reason);
            }
        };
        require_false(L"DisallowStartIfOnBatteries",
            "battery power can prevent task start");
        require_false(L"StopIfGoingOnBatteries",
            "battery transition can terminate the handoff");

        require_optional_value(L"AllowHardTerminate", L"true",
            "AllowHardTerminate setting differs");
        require_optional_value(L"AllowStartOnDemand", L"true",
            "AllowStartOnDemand setting differs");
        require_optional_value(L"Hidden", L"false",
            "Hidden task setting differs");
        require_optional_value(L"WakeToRun", L"false",
            "WakeToRun setting differs");
        require_optional_value(L"Priority", L"7",
            "task priority differs");

        if (startup_task_xml_find_open_tag(scope.begin, scope.end, L"RunOnlyIfIdle") &&
            !startup_task_xml_element_is_empty(scope, L"RunOnlyIfIdle") &&
            (!startup_task_xml_get_value(scope, L"RunOnlyIfIdle", value, ARRAY_COUNT(value)) ||
             _wcsicmp(value, L"false") != 0)) {
            mark_broken("idle gating can prevent task start");
        }
        StartupTaskXmlSpan idleSettings = {};
        if (startup_task_xml_get_scope(scope, L"IdleSettings", &idleSettings)) {
            const WCHAR* idleTags[] = { L"StopOnIdleEnd", L"RestartOnIdle" };
            for (const WCHAR* tag : idleTags) {
                if (startup_task_xml_find_open_tag(idleSettings.begin, idleSettings.end, tag) &&
                    !startup_task_xml_element_is_empty(idleSettings, tag) &&
                    (!startup_task_xml_get_value(idleSettings, tag, value, ARRAY_COUNT(value)) ||
                     _wcsicmp(value, L"false") != 0)) {
                    mark_broken("idle settings can stop or repeat the handoff");
                }
            }
        }
        if (startup_task_xml_find_open_tag(scope.begin, scope.end, L"RestartOnFailure")) {
            mark_broken("Task Scheduler restart-on-failure repetition is not allowed");
        }
        if (startup_task_xml_find_open_tag(scope.begin, scope.end, L"RunOnlyIfNetworkAvailable") &&
            !startup_task_xml_element_is_empty(scope, L"RunOnlyIfNetworkAvailable") &&
            (!startup_task_xml_get_value(scope, L"RunOnlyIfNetworkAvailable", value, ARRAY_COUNT(value)) ||
             _wcsicmp(value, L"false") != 0)) {
            mark_broken("network gating can prevent task start");
        }
    }

    if (broken) return STARTUP_TASK_DEFINITION_BROKEN;
    if (!canonical) {
        if (detail && detailSize && !detail[0]) {
            StringCchCopyA(detail, detailSize, "compatible legacy definition");
        }
        return STARTUP_TASK_DEFINITION_COMPATIBLE_LEGACY;
    }
    if (detail && detailSize) {
        StringCchCopyA(detail, detailSize, "canonical immediate least-privilege definition");
    }
    return STARTUP_TASK_DEFINITION_CANONICAL;
}

static StartupTaskDefinitionClass startup_task_definition_classify_current(
    const WCHAR* taskName, const WCHAR* exePath, const WCHAR* cfgPath,
    char* detail, size_t detailSize) {
    if (detail && detailSize) detail[0] = 0;
    if (!taskName || !exePath || !cfgPath || !taskName[0] || !exePath[0] || !cfgPath[0]) {
        set_message(detail, detailSize, "Invalid expected startup-task definition");
        return STARTUP_TASK_DEFINITION_BROKEN;
    }

    WCHAR* xml = nullptr;
    if (!startup_task_query_xml(taskName, &xml, detail, detailSize)) {
        return STARTUP_TASK_DEFINITION_BROKEN;
    }

    WCHAR expectedUser[512] = {};
    if (g_forcedStartupUserSam[0]) {
        StringCchCopyW(expectedUser, ARRAY_COUNT(expectedUser), g_forcedStartupUserSam);
    } else if (!get_current_user_sam_name(expectedUser, ARRAY_COUNT(expectedUser))) {
        free(xml);
        set_message(detail, detailSize, "Could not resolve expected startup-task user");
        return STARTUP_TASK_DEFINITION_BROKEN;
    }

    WCHAR expectedWorkingDir[MAX_PATH] = {};
    StringCchCopyW(expectedWorkingDir, ARRAY_COUNT(expectedWorkingDir), exePath);
    WCHAR* slash = wcsrchr(expectedWorkingDir, L'\\');
    if (!slash) slash = wcsrchr(expectedWorkingDir, L'/');
    if (!slash) {
        free(xml);
        set_message(detail, detailSize, "Could not resolve expected startup-task working directory");
        return STARTUP_TASK_DEFINITION_BROKEN;
    }
    *slash = 0;

    StartupTaskDefinitionClass classification = startup_task_definition_classify_xml(
        xml, expectedUser, exePath, cfgPath, expectedWorkingDir, detail, detailSize);
    free(xml);
    return classification;
}
