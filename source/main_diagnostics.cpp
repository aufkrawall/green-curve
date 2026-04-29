// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT

static void close_debug_log_file() {
    EnterCriticalSection(&g_debugLogLock);
    if (g_debugLogFile != INVALID_HANDLE_VALUE) {
        FlushFileBuffers(g_debugLogFile);
        CloseHandle(g_debugLogFile);
        g_debugLogFile = INVALID_HANDLE_VALUE;
    }
    g_debugLogOpenPath[0] = 0;
    LeaveCriticalSection(&g_debugLogLock);
}

static DWORD debug_log_file_attributes() {
    return FILE_ATTRIBUTE_NORMAL | (g_app.isServiceProcess ? FILE_FLAG_WRITE_THROUGH : 0);
}

static HANDLE open_debug_log_file_locked(const char* debugPath) {
    if (!debugPath || !debugPath[0]) return INVALID_HANDLE_VALUE;
    char pathErr[256] = {};
    ensure_parent_directory_for_file(debugPath, pathErr, sizeof(pathErr));
    HANDLE h = CreateFileA(debugPath, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
        OPEN_ALWAYS, debug_log_file_attributes(), nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        StringCchCopyA(g_debugLogOpenPath, ARRAY_COUNT(g_debugLogOpenPath), debugPath);
    }
    return h;
}

static const char* debug_log_path() {
    return g_debugLogPath[0] ? g_debugLogPath : APP_DEBUG_LOG_FILE;
}

static const char* service_early_debug_log_path() {
    if (g_debugEarlyLogPath[0]) return g_debugEarlyLogPath;

    char programData[MAX_PATH] = {};
    DWORD n = GetEnvironmentVariableA("ProgramData", programData, ARRAY_COUNT(programData));
    if (n == 0 || n >= ARRAY_COUNT(programData)) {
        StringCchCopyA(programData, ARRAY_COUNT(programData), "C:\\ProgramData");
    }
    StringCchPrintfA(g_debugEarlyLogPath, ARRAY_COUNT(g_debugEarlyLogPath),
        "%s\\Green Curve\\%s", programData, APP_DEBUG_LOG_FILE);
    return g_debugEarlyLogPath;
}

static const char* json_snapshot_path() {
    return g_jsonPath[0] ? g_jsonPath : APP_JSON_FILE;
}

static const char* error_log_path() {
    return g_errorLogPath[0] ? g_errorLogPath : APP_LOG_FILE;
}

static const char* effective_debug_log_path() {
    if (g_app.isServiceProcess && !g_serviceUserPathsResolved) {
        return service_early_debug_log_path();
    }
    return debug_log_path();
}

static void debug_log(const char* fmt, ...) {
    if (!g_debug_logging || !fmt) return;
    char message[1024] = {};
    va_list ap;
    va_start(ap, fmt);
    StringCchVPrintfA(message, ARRAY_COUNT(message), fmt, ap);
    va_end(ap);
    char buf[1200] = {};
    int prefixLen = format_log_timestamp_prefix(buf, ARRAY_COUNT(buf));
    StringCchCatA(buf + prefixLen, ARRAY_COUNT(buf) - prefixLen, message);
    OutputDebugStringA(buf);

    EnterCriticalSection(&g_debugLogLock);
    const char* debugPath = effective_debug_log_path();

    if (!g_app.isServiceProcess && !g_debugLogPath[0]) {
        char pathErr[256] = {};
        resolve_data_paths(pathErr, sizeof(pathErr));
        debugPath = effective_debug_log_path();
    }

    if (g_debugLogFile != INVALID_HANDLE_VALUE && _stricmp(g_debugLogOpenPath, debugPath) != 0) {
        FlushFileBuffers(g_debugLogFile);
        CloseHandle(g_debugLogFile);
        g_debugLogFile = INVALID_HANDLE_VALUE;
        g_debugLogOpenPath[0] = 0;
    }

    if (g_debugLogFile == INVALID_HANDLE_VALUE) {
        g_debugLogFile = open_debug_log_file_locked(debugPath);
    }

    if (g_debugLogFile != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        if (!WriteFile(g_debugLogFile, buf, (DWORD)strlen(buf), &written, nullptr)) {
            CloseHandle(g_debugLogFile);
            g_debugLogFile = INVALID_HANDLE_VALUE;
            g_debugLogOpenPath[0] = 0;

            g_debugLogFile = open_debug_log_file_locked(debugPath);
            if (g_debugLogFile != INVALID_HANDLE_VALUE) {
                WriteFile(g_debugLogFile, buf, (DWORD)strlen(buf), &written, nullptr);
            }
        }
        if (g_app.isServiceProcess && g_debugLogFile != INVALID_HANDLE_VALUE) {
            FlushFileBuffers(g_debugLogFile);
        }
    }

    LeaveCriticalSection(&g_debugLogLock);
}

static void debug_log_session_marker(const char* phase, const char* kind, const char* extra) {
    if (!g_debug_logging) return;
    SYSTEMTIME now = {};
    GetLocalTime(&now);
    DWORD pid = GetCurrentProcessId();
    DWORD sessionId = 0;
    ProcessIdToSessionId(pid, &sessionId);
    const char* configPath = g_app.configPath[0] ? g_app.configPath : "<unset>";
    debug_log("\n===== SESSION %s =====\n", phase ? phase : "MARK");
    debug_log("time=%04u-%02u-%02u %02u:%02u:%02u.%03u pid=%lu session=%lu kind=%s version=%s build=%lu protocol=%lu elevated=%d serviceProcess=%d serviceInstalled=%d serviceRunning=%d serviceAvailable=%d config=%s\n",
        now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond, now.wMilliseconds,
        pid,
        sessionId,
        kind ? kind : "unknown",
        APP_VERSION,
        (unsigned long)APP_BUILD_NUMBER,
        (unsigned long)SERVICE_PROTOCOL_VERSION,
        is_elevated() ? 1 : 0,
        g_app.isServiceProcess ? 1 : 0,
        g_app.backgroundServiceInstalled ? 1 : 0,
        g_app.backgroundServiceRunning ? 1 : 0,
        g_app.backgroundServiceAvailable ? 1 : 0,
        configPath);
    if (extra && extra[0]) {
        debug_log("details=%s\n", extra);
    }
    debug_log("========================\n");
}

static void set_last_apply_phase(const char* phase) {
    if (!phase || !phase[0]) {
        g_lastApplyPhase[0] = 0;
        return;
    }
    StringCchCopyA(g_lastApplyPhase, ARRAY_COUNT(g_lastApplyPhase), phase);
    debug_log("apply phase: %s\n", g_lastApplyPhase);
}

static void write_crash_breadcrumb_direct(const char* text) {
    if (!text || !text[0]) return;
    OutputDebugStringA(text);

    char path[MAX_PATH] = {};
    const char* dataDir = g_userDataDir[0] ? g_userDataDir : ".";
    StringCchPrintfA(path, ARRAY_COUNT(path), "%s\\%s", dataDir, "greencurve_crash.txt");
    char pathErr[256] = {};
    ensure_parent_directory_for_file(path, pathErr, sizeof(pathErr));

    HANDLE h = CreateFileA(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
        OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    WriteFile(h, text, (DWORD)strlen(text), &written, nullptr);
    FlushFileBuffers(h);
    CloseHandle(h);
}

static LONG WINAPI green_curve_unhandled_exception_filter(EXCEPTION_POINTERS* info) {
    SYSTEMTIME now = {};
    GetLocalTime(&now);
    DWORD code = 0;
    void* address = nullptr;
    void* faultAddr = nullptr;
    bool faultWrite = false;
    if (info && info->ExceptionRecord) {
        code = info->ExceptionRecord->ExceptionCode;
        address = info->ExceptionRecord->ExceptionAddress;
        if (code == 0xC0000005 && info->ExceptionRecord->NumberParameters >= 2) {
            faultWrite = (info->ExceptionRecord->ExceptionInformation[0] != 0);
            faultAddr = (void*)info->ExceptionRecord->ExceptionInformation[1];
        }
    }

    char text[2048] = {};
    int len = 0;
    len += StringCchPrintfA(text + len, ARRAY_COUNT(text) - len,
        "\r\n%04u-%02u-%02u %02u:%02u:%02u.%03u CRASH pid=%lu tid=%lu exception=0x%08lX address=%p",
        now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond, now.wMilliseconds,
        GetCurrentProcessId(),
        GetCurrentThreadId(),
        code,
        address);
    if (code == 0xC0000005 && faultAddr) {
        len += StringCchPrintfA(text + len, ARRAY_COUNT(text) - len,
            " fault=%s@%p", faultWrite ? "write" : "read", faultAddr);
    }
    len += StringCchPrintfA(text + len, ARRAY_COUNT(text) - len,
        " source=%s phase=%s serviceProcess=%d config=%s\r\n",
        g_pendingOperationSource[0] ? g_pendingOperationSource : "<none>",
        g_lastApplyPhase[0] ? g_lastApplyPhase : "<none>",
        g_app.isServiceProcess ? 1 : 0,
        g_app.configPath[0] ? g_app.configPath : "<unset>");
    if (g_app.fanCurveRuntimeActive || g_app.fanFixedRuntimeActive) {
        len += StringCchPrintfA(text + len, ARRAY_COUNT(text) - len,
            "  fanRuntime: mode=%d curveActive=%d fixedActive=%d fixedPct=%d temp=%d consecutiveFailures=%u lastApplyMs=%llu\r\n",
            g_app.activeFanMode,
            g_app.fanCurveRuntimeActive ? 1 : 0,
            g_app.fanFixedRuntimeActive ? 1 : 0,
            g_app.activeFanFixedPercent,
            g_app.gpuTemperatureValid ? g_app.gpuTemperatureC : -1,
            g_app.fanRuntimeConsecutiveFailures,
            g_app.fanRuntimeLastApplyTickMs);
    }
    write_crash_breadcrumb_direct(text);
    return EXCEPTION_EXECUTE_HANDLER;
}

static void write_error_report_log_for_user_failure(const char* summary, const char* details) {
    char logErr[256] = {};
    if (!write_error_report_log(summary, details, logErr, sizeof(logErr)) && logErr[0]) {
        debug_log("error report log failed: %s\n", logErr);
    }
}

static bool write_text_file_atomic(const char* path, const char* data, size_t dataSize, char* err, size_t errSize) {
    if (g_app.isServiceProcess && g_serviceUserPathsResolved) {
        return write_text_file_atomic_service(path, data, dataSize, err, errSize);
    }
    if (!path || !data) {
        set_message(err, errSize, "Invalid file write arguments");
        return false;
    }

    if (!ensure_parent_directory_for_file(path, err, errSize)) {
        return false;
    }

    char tempPath[MAX_PATH] = {};
    StringCchPrintfA(tempPath, ARRAY_COUNT(tempPath), "%s.tmp", path);

    HANDLE h = CreateFileA(tempPath, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        set_message(err, errSize, "Cannot create %s (error %lu)", tempPath, GetLastError());
        return false;
    }

    DWORD totalWritten = 0;
    bool ok = true;
    while (totalWritten < dataSize) {
        DWORD chunk = 0;
        DWORD toWrite = (DWORD)nvmin((int)(dataSize - totalWritten), 1 << 20);
        if (!WriteFile(h, data + totalWritten, toWrite, &chunk, nullptr) || chunk == 0) {
            ok = false;
            set_message(err, errSize, "Failed writing %s (error %lu)", tempPath, GetLastError());
            break;
        }
        totalWritten += chunk;
    }
    if (ok && !FlushFileBuffers(h)) {
        ok = false;
        set_message(err, errSize, "Failed flushing %s (error %lu)", tempPath, GetLastError());
    }
    CloseHandle(h);

    if (!ok) {
        DeleteFileA(tempPath);
        return false;
    }

    if (!MoveFileExA(tempPath, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        set_message(err, errSize, "Failed finalizing %s (error %lu)", path, GetLastError());
        DeleteFileA(tempPath);
        return false;
    }
    return true;
}

static bool write_text_file_atomic_service(const char* path, const char* data, size_t dataSize, char* err, size_t errSize) {
    if (!path || !data) {
        set_message(err, errSize, "Invalid file write arguments");
        return false;
    }
    if (!ensure_parent_directory_for_file(path, err, errSize)) return false;

    char tempPath[MAX_PATH] = {};
    HANDLE h = INVALID_HANDLE_VALUE;
    DWORD pid = GetCurrentProcessId();
    for (unsigned int attempt = 0; attempt < 32; attempt++) {
        StringCchPrintfA(tempPath, ARRAY_COUNT(tempPath), "%s.tmp.%08lx.%08lx.%02u",
            path,
            (unsigned long)pid,
            (unsigned long)GetTickCount(),
            attempt);
        h = CreateFileA(tempPath,
            GENERIC_READ | GENERIC_WRITE,
            0,
            nullptr,
            CREATE_NEW,
            FILE_ATTRIBUTE_TEMPORARY | FILE_ATTRIBUTE_NOT_CONTENT_INDEXED,
            nullptr);
        if (h != INVALID_HANDLE_VALUE) break;
        if (GetLastError() != ERROR_FILE_EXISTS && GetLastError() != ERROR_ALREADY_EXISTS) {
            set_message(err, errSize, "Cannot create %s (error %lu)", tempPath, GetLastError());
            return false;
        }
    }
    if (h == INVALID_HANDLE_VALUE) {
        set_message(err, errSize, "Cannot create a unique temporary output file");
        return false;
    }

    char verifyErr[256] = {};
    bool ok = service_verify_written_file_path(tempPath, verifyErr, sizeof(verifyErr));
    if (!ok) {
        CloseHandle(h);
        DeleteFileA(tempPath);
        StringCchCopyA(err, errSize, verifyErr[0] ? verifyErr : "Temporary output path failed verification");
        return false;
    }

    DWORD totalWritten = 0;
    while (totalWritten < dataSize) {
        DWORD chunk = 0;
        DWORD toWrite = (DWORD)nvmin((int)(dataSize - totalWritten), 1 << 20);
        if (!WriteFile(h, data + totalWritten, toWrite, &chunk, nullptr) || chunk == 0) {
            ok = false;
            set_message(err, errSize, "Failed writing %s (error %lu)", tempPath, GetLastError());
            break;
        }
        totalWritten += chunk;
    }
    if (ok && !FlushFileBuffers(h)) {
        ok = false;
        set_message(err, errSize, "Failed flushing %s (error %lu)", tempPath, GetLastError());
    }
    CloseHandle(h);
    if (!ok) {
        DeleteFileA(tempPath);
        return false;
    }
    if (!service_verify_written_file_path(tempPath, verifyErr, sizeof(verifyErr))) {
        DeleteFileA(tempPath);
        StringCchCopyA(err, errSize, verifyErr[0] ? verifyErr : "Temporary output path failed final verification");
        return false;
    }
    if (!MoveFileExA(tempPath, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        set_message(err, errSize, "Failed finalizing %s (error %lu)", path, GetLastError());
        DeleteFileA(tempPath);
        return false;
    }
    if (!service_verify_written_file_path(path, verifyErr, sizeof(verifyErr))) {
        StringCchCopyA(err, errSize, verifyErr[0] ? verifyErr : "Written file failed final verification");
        return false;
    }
    return true;
}

static bool section_name_matches(const char* line, const char* section) {
    if (!line || line[0] != '[') return false;
    size_t len = strlen(section);
    if (strncmp(line + 1, section, len) != 0) return false;
    return line[1 + len] == ']';
}

static bool section_should_be_preserved(const char* line, const char* const* replaceSections, int replaceCount) {
    if (!line || line[0] != '[') return true;
    for (int i = 0; i < replaceCount; i++) {
        if (section_name_matches(line, replaceSections[i])) return false;
    }
    return true;
}

static bool write_config_sections_atomic(const char* path, const char* newSectionsData, const char* const* replaceSections, int replaceCount, char* err, size_t errSize) {
    if (!path || !newSectionsData) {
        set_message(err, errSize, "Invalid config write arguments");
        return false;
    }

    // Read existing file if present.
    char* preserved = nullptr;
    size_t preservedSize = 0;
    HANDLE hExisting = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hExisting != INVALID_HANDLE_VALUE) {
        DWORD fileSize = GetFileSize(hExisting, nullptr);
        if (fileSize != INVALID_FILE_SIZE && fileSize > 0 && fileSize < 8 * 1024 * 1024) {
            preserved = (char*)malloc(fileSize + 1);
            if (preserved) {
                DWORD read = 0;
                if (ReadFile(hExisting, preserved, fileSize, &read, nullptr) && read == fileSize) {
                    preservedSize = fileSize;
                    preserved[preservedSize] = 0;
                } else {
                    free(preserved);
                    preserved = nullptr;
                    preservedSize = 0;
                }
            }
        }
        CloseHandle(hExisting);
    }

    size_t newSectionsSize = strlen(newSectionsData);
    size_t outCapacity = preservedSize + newSectionsSize + 256;
    char* out = (char*)malloc(outCapacity);
    if (!out) {
        free(preserved);
        set_message(err, errSize, "Out of memory building config");
        return false;
    }
    size_t outUsed = 0;

    auto appendOut = [&](const char* data, size_t len) -> bool {
        if (outUsed + len + 1 > outCapacity) {
            size_t newCap = outCapacity * 2 + len + 256;
            char* tmp = (char*)realloc(out, newCap);
            if (!tmp) return false;
            out = tmp;
            outCapacity = newCap;
        }
        memcpy(out + outUsed, data, len);
        outUsed += len;
        return true;
    };

    // Copy preserved content, skipping replaced sections.
    if (preserved && preservedSize > 0) {
        const char* p = preserved;
        while (*p) {
            const char* end = p;
            while (*end && *end != '\r' && *end != '\n') end++;
            size_t lineLen = end - p;

            if (p[0] == '[' && section_should_be_preserved(p, replaceSections, replaceCount)) {
                // Copy this line and all subsequent lines until next section.
                if (!appendOut(p, lineLen)) { free(preserved); free(out); return false; }
                if (*end == '\r') { if (!appendOut("\r", 1)) { free(preserved); free(out); return false; } end++; }
                if (*end == '\n') { if (!appendOut("\n", 1)) { free(preserved); free(out); return false; } end++; }
                p = end;
                while (*p) {
                    const char* nextEnd = p;
                    while (*nextEnd && *nextEnd != '\r' && *nextEnd != '\n') nextEnd++;
                    if (p[0] == '[') break;
                    if (!appendOut(p, nextEnd - p)) { free(preserved); free(out); return false; }
                    if (*nextEnd == '\r') { if (!appendOut("\r", 1)) { free(preserved); free(out); return false; } nextEnd++; }
                    if (*nextEnd == '\n') { if (!appendOut("\n", 1)) { free(preserved); free(out); return false; } nextEnd++; }
                    p = nextEnd;
                }
            } else {
                // Skip this line (belongs to a replaced section or is not a section header).
                if (p[0] == '[') {
                    // Skip entire section.
                    if (*end == '\r') end++;
                    if (*end == '\n') end++;
                    p = end;
                    while (*p) {
                        const char* nextEnd = p;
                        while (*nextEnd && *nextEnd != '\r' && *nextEnd != '\n') nextEnd++;
                        if (p[0] == '[') break;
                        if (*nextEnd == '\r') nextEnd++;
                        if (*nextEnd == '\n') nextEnd++;
                        p = nextEnd;
                    }
                } else {
                    // Normal line outside any replaced section - copy it.
                    if (!appendOut(p, lineLen)) { free(preserved); free(out); return false; }
                    if (*end == '\r') { if (!appendOut("\r", 1)) { free(preserved); free(out); return false; } end++; }
                    if (*end == '\n') { if (!appendOut("\n", 1)) { free(preserved); free(out); return false; } end++; }
                    p = end;
                }
            }
        }
    }

    free(preserved);

    // Append new sections.
    if (!appendOut(newSectionsData, newSectionsSize)) { free(out); return false; }
    if (outUsed == 0 || out[outUsed - 1] != '\n') {
        if (!appendOut("\r\n", 2)) { free(out); return false; }
    }

    bool ok = write_text_file_atomic(path, out, outUsed, err, errSize);
    free(out);
    return ok;
}

static bool write_log_snapshot(const char* path, char* err, size_t errSize) {
    const size_t TEXT_SIZE = 65536;
    char* text = (char*)malloc(TEXT_SIZE);
    if (!text) {
        set_message(err, errSize, "Out of memory allocating log snapshot buffer");
        return false;
    }
    memset(text, 0, TEXT_SIZE);

    size_t used = 0;
    auto appendf = [&](const char* fmt, ...) -> bool {
        if (used >= TEXT_SIZE) return false;
        va_list ap;
        va_start(ap, fmt);
        int written = _vsnprintf_s(text + used, TEXT_SIZE - used, _TRUNCATE, fmt, ap);
        va_end(ap);
        if (written < 0) {
            used = TEXT_SIZE - 1;
            text[TEXT_SIZE - 1] = 0;
            return false;
        }
        used += (size_t)written;
        return true;
    };

    appendf("GPU: %s\r\n", g_app.gpuName);
    appendf("Populated points: %d\r\n\r\n", g_app.numPopulated);
    appendf("GPU offset: %d MHz", g_app.gpuClockOffsetkHz / 1000);
    if (g_app.gpuOffsetRangeKnown) appendf(" (range %d..%d)", g_app.gpuClockOffsetMinMHz, g_app.gpuClockOffsetMaxMHz);
    appendf("\r\n");
    int curveMinkHz = 0;
    int curveMaxkHz = 0;
    bool curveRangeKnown = get_curve_offset_range_khz(&curveMinkHz, &curveMaxkHz);
    appendf("VF curve delta clamp: %d..%d kHz", curveMinkHz, curveMaxkHz);
    appendf("%s\r\n",
        g_app.curveOffsetRangeKnown ? " (driver curve range)" :
        curveRangeKnown ? " (graphics offset fallback)" :
        " (default fallback)");
    appendf("Mem offset: %d MHz", mem_display_mhz_from_driver_khz(g_app.memClockOffsetkHz));
    if (g_app.memOffsetRangeKnown) appendf(" (range %d..%d)", g_app.memClockOffsetMinMHz, g_app.memClockOffsetMaxMHz);
    appendf("\r\n");
    appendf("Power limit: %d%% (%d mW current / %d mW default)\r\n", g_app.powerLimitPct, g_app.powerLimitCurrentmW, g_app.powerLimitDefaultmW);
    appendf("\r\nGUI state\r\n=========\r\n");
    appendf("GUI GPU offset: %d MHz\r\n", g_app.guiGpuOffsetMHz);
    appendf("GUI GPU exclude low 70: %s\r\n", g_app.guiGpuOffsetExcludeLowCount > 0 ? "yes" : "no");
    appendf("GUI fan mode: %s\r\n", fan_mode_label(g_app.guiFanMode));
    appendf("GUI fan fixed pct: %d\r\n", g_app.guiFanFixedPercent);
    appendf("Applied/session GPU offset: %d MHz\r\n", g_app.appliedGpuOffsetMHz);
    appendf("Applied/session GPU exclude low 70: %s\r\n", g_app.appliedGpuOffsetExcludeLowCount > 0 ? "yes" : "no");
    appendf("Active fan mode: %s\r\n", fan_mode_label(g_app.activeFanMode));
    appendf("Active fan fixed pct: %d\r\n", g_app.activeFanFixedPercent);
    if (g_app.serviceControlStateValid) {
        appendf("\r\nService control state\r\n====================\r\n");
        appendf("GPU offset: %d MHz\r\n", g_app.serviceControlState.gpuOffsetMHz);
        appendf("Exclude low 70: %s\r\n", g_app.serviceControlState.gpuOffsetExcludeLowCount > 0 ? "yes" : "no");
        appendf("Mem offset: %d MHz\r\n", g_app.serviceControlState.memOffsetMHz);
        appendf("Power limit: %d%%\r\n", g_app.serviceControlState.powerLimitPct);
        appendf("Fan mode: %s\r\n", fan_mode_label(g_app.serviceControlState.fanMode));
        appendf("Fan fixed pct: %d\r\n", g_app.serviceControlState.fanFixedPercent);
    }
    if (g_app.fanSupported) {
        appendf("Fan: %s\r\n", g_app.fanIsAuto ? "auto" : "manual");
        for (unsigned int fan = 0; fan < g_app.fanCount; fan++) {
            appendf("  Fan %u: %u%% / %u RPM / policy=%u signal=%u target=0x%X requested=%u%%\r\n",
                fan, g_app.fanPercent[fan], g_app.fanRpm[fan], g_app.fanPolicy[fan], g_app.fanControlSignal[fan], g_app.fanTargetMask[fan], g_app.fanTargetPercent[fan]);
        }
    } else {
        appendf("Fan: unsupported\r\n");
    }
    appendf("\r\n%-6s  %-10s  %-10s  %-12s\r\n", "Point", "Freq(MHz)", "Volt(mV)", "Offset(kHz)");
    appendf("------  ----------  ----------  ------------\r\n");
    for (int i = 0; i < VF_NUM_POINTS; i++) {
        if (g_app.curve[i].freq_kHz > 0 || g_app.curve[i].volt_uV > 0) {
            appendf("%-6d  %-10u  %-10u  %-12d\r\n",
                i,
                displayed_curve_mhz(g_app.curve[i].freq_kHz),
                g_app.curve[i].volt_uV / 1000,
                g_app.freqOffsets[i]);
        }
    }

    bool ok = write_text_file_atomic(path, text, used, err, errSize);
    free(text);
    return ok;
}

static bool write_error_report_log(const char* summary, const char* details, char* err, size_t errSize) {
    char* text = (char*)VirtualAlloc(nullptr, 73728, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!text) {
        set_message(err, errSize, "Out of memory generating error log");
        return false;
    }

    size_t used = 0;
    auto appendf = [&](const char* fmt, ...) -> bool {
        if (used >= 73728) return false;
        va_list ap;
        va_start(ap, fmt);
        int written = _vsnprintf_s(text + used, 73728 - used, _TRUNCATE, fmt, ap);
        va_end(ap);
        if (written < 0) {
            used = 73727;
            text[73727] = 0;
            return false;
        }
        used += (size_t)written;
        return true;
    };

    SYSTEMTIME now = {};
    GetLocalTime(&now);
    appendf("Green Curve error report\r\n");
    appendf("Generated: %04u-%02u-%02u %02u:%02u:%02u\r\n\r\n",
        now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond);
    appendf("Config path: %s\r\n", g_app.configPath[0] ? g_app.configPath : "<unset>");
    appendf("Service mode: installed=%d running=%d available=%d using=%d broken=%d\r\n\r\n",
        g_app.backgroundServiceInstalled ? 1 : 0,
        g_app.backgroundServiceRunning ? 1 : 0,
        g_app.backgroundServiceAvailable ? 1 : 0,
        g_app.usingBackgroundService ? 1 : 0,
        g_app.backgroundServiceBroken ? 1 : 0);
    if (summary && *summary) appendf("Summary: %s\r\n", summary);
    if (details && *details) appendf("Details: %s\r\n", details);
    if (g_lastOperationIntent[0]) {
        appendf("\r\nOperation intent\r\n================\r\n%s", g_lastOperationIntent);
    }
    if (g_lastOperationPlan[0]) {
        appendf("\r\nApply plan\r\n==========\r\n%s", g_lastOperationPlan);
    }
    if (g_lastOperationBeforeSnapshot[0]) {
        appendf("\r\nState before apply\r\n==================\r\n%s", g_lastOperationBeforeSnapshot);
    }
    if (g_lastOperationAfterSnapshot[0]) {
        appendf("\r\nState after apply\r\n=================\r\n%s", g_lastOperationAfterSnapshot);
    }
    appendf("\r\nCurrent state snapshot\r\n======================\r\n");
    appendf("GPU: %s\r\n", g_app.gpuName);
    appendf("Populated points: %d\r\n\r\n", g_app.numPopulated);
    char liveOffsetState[256] = {};
    describe_live_gpu_offset_state(liveOffsetState, sizeof(liveOffsetState));
    appendf("GPU offset: %d MHz", g_app.gpuClockOffsetkHz / 1000);
    if (g_app.gpuOffsetRangeKnown) appendf(" (range %d..%d)", g_app.gpuClockOffsetMinMHz, g_app.gpuClockOffsetMaxMHz);
    appendf("\r\n");
    appendf("GPU offset state: %s\r\n", liveOffsetState);
    int curveMinkHz = 0;
    int curveMaxkHz = 0;
    bool curveRangeKnown = get_curve_offset_range_khz(&curveMinkHz, &curveMaxkHz);
    appendf("VF curve delta clamp: %d..%d kHz", curveMinkHz, curveMaxkHz);
    appendf("%s\r\n",
        g_app.curveOffsetRangeKnown ? " (driver curve range)" :
        curveRangeKnown ? " (graphics offset fallback)" :
        " (default fallback)");
    appendf("Mem offset: %d MHz", mem_display_mhz_from_driver_khz(g_app.memClockOffsetkHz));
    if (g_app.memOffsetRangeKnown) appendf(" (range %d..%d)", g_app.memClockOffsetMinMHz, g_app.memClockOffsetMaxMHz);
    appendf("\r\n");
    appendf("Power limit: %d%% (%d mW current / %d mW default)\r\n", g_app.powerLimitPct, g_app.powerLimitCurrentmW, g_app.powerLimitDefaultmW);
    if (g_app.fanSupported) {
        appendf("Fan: %s\r\n", g_app.fanIsAuto ? "auto" : "manual");
        for (unsigned int fan = 0; fan < g_app.fanCount; fan++) {
            appendf("  Fan %u: %u%% / %u RPM / policy=%u signal=%u target=0x%X requested=%u%%\r\n",
                fan, g_app.fanPercent[fan], g_app.fanRpm[fan], g_app.fanPolicy[fan], g_app.fanControlSignal[fan], g_app.fanTargetMask[fan], g_app.fanTargetPercent[fan]);
        }
    } else {
        appendf("Fan: unsupported\r\n");
    }
    appendf("\r\n%-6s  %-10s  %-10s  %-12s\r\n", "Point", "Freq(MHz)", "Volt(mV)", "Offset(kHz)");
    appendf("------  ----------  ----------  ------------\r\n");
    for (int i = 0; i < VF_NUM_POINTS; i++) {
        if (g_app.curve[i].freq_kHz > 0 || g_app.curve[i].volt_uV > 0) {
            appendf("%-6d  %-10u  %-10u  %-12d\r\n",
                i,
                displayed_curve_mhz(g_app.curve[i].freq_kHz),
                g_app.curve[i].volt_uV / 1000,
                g_app.freqOffsets[i]);
        }
    }

    bool ok = write_text_file_atomic(error_log_path(), text, used, err, errSize);
    VirtualFree(text, 0, MEM_RELEASE);
    return ok;
}

static bool write_json_snapshot(const char* path, char* err, size_t errSize) {
    char* json = (char*)VirtualAlloc(nullptr, 131072, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!json) {
        set_message(err, errSize, "Out of memory generating JSON");
        return false;
    }

    size_t used = 0;
    auto append = [&](const char* fmt, ...) -> bool {
        if (used >= 131072) return false;
        va_list ap;
        va_start(ap, fmt);
        int written = _vsnprintf_s(json + used, 131072 - used, _TRUNCATE, fmt, ap);
        va_end(ap);
        if (written < 0) {
            used = 131071;
            json[131071] = 0;
            return false;
        }
        used += (size_t)written;
        return true;
    };

    append("{\n  \"gpu\": \"");
    for (const unsigned char* p = (const unsigned char*)g_app.gpuName; p && *p; ++p) {
        switch (*p) {
            case '\\': append("\\\\"); break;
            case '"': append("\\\""); break;
            case '\n': append("\\n"); break;
            case '\r': append("\\r"); break;
            case '\t': append("\\t"); break;
            default:
                if (*p < 32) append("\\u%04x", *p);
                else append("%c", *p);
                break;
        }
    }
    append("\",\n  \"populated\": %d,\n", g_app.numPopulated);
    append("  \"gpu_offset_mhz\": %d,\n", g_app.gpuClockOffsetkHz / 1000);
    append("  \"mem_offset_mhz\": %d,\n", mem_display_mhz_from_driver_khz(g_app.memClockOffsetkHz));
    append("  \"power_limit_pct\": %d,\n", g_app.powerLimitPct);
    if (g_app.fanSupported) {
        if (g_app.fanIsAuto) append("  \"fan\": \"auto\",\n");
        else append("  \"fan\": %u,\n", g_app.fanPercent[0]);
    } else {
        append("  \"fan\": null,\n");
    }
    append("  \"fans\": [\n");
    for (unsigned int fan = 0; fan < g_app.fanCount; fan++) {
        append("    {\"index\": %u, \"percent\": %u, \"requested_percent\": %u, \"rpm\": %u, \"policy\": %u, \"signal\": %u, \"target\": %u}%s\n",
            fan, g_app.fanPercent[fan], g_app.fanTargetPercent[fan], g_app.fanRpm[fan], g_app.fanPolicy[fan], g_app.fanControlSignal[fan], g_app.fanTargetMask[fan],
            (fan + 1 < g_app.fanCount) ? "," : "");
    }
    append("  ],\n  \"points\": [\n");
    bool first = true;
    for (int i = 0; i < VF_NUM_POINTS; i++) {
        if (g_app.curve[i].freq_kHz > 0 || g_app.curve[i].volt_uV > 0) {
            append("%s    {\"index\": %d, \"freq_mhz\": %u, \"volt_mv\": %u, \"offset_khz\": %d}",
                first ? "" : ",\n",
                i,
                displayed_curve_mhz(g_app.curve[i].freq_kHz),
                g_app.curve[i].volt_uV / 1000,
                g_app.freqOffsets[i]);
            first = false;
        }
    }
    append("\n  ]\n}\n");

    bool ok = write_text_file_atomic(path, json, used, err, errSize);
    VirtualFree(json, 0, MEM_RELEASE);
    return ok;
}
