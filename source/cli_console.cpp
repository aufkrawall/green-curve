// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// Console output for a GUI-subsystem binary (F-01-001).
//
// THE DEFECT THIS EXISTS FOR (audit 2026-09-15, reproduced on 0.25.2):
//
//   greencurve.exe is linked -subsystem:windows, so Windows attaches no
//   console to it.  Every CLI verb therefore printed NOTHING in a terminal and
//   returned exit code 0: `greencurve.exe --help` produced zero bytes on both
//   stdout and stderr, and the full help text was discoverable only in
//   %LOCALAPPDATA%\Green Curve\greencurve_cli_log.txt -- a path neither the
//   README nor the help text names.  The same was true of --service-install,
//   which README.md documents as the archive-install path, so an administrator
//   running it in an elevated shell could not tell success from failure.
//
// THE RULE: a CLI verb reports to whatever the caller gave us, and always also
// to the file.  The file stays authoritative (Task Scheduler, the logon task
// and setup have no console at all); the console is an additional sink, never a
// replacement, so no existing no-console caller loses anything.
//
// Sink selection, in this order, because each answers a different question:
//   1. AttachConsole(ATTACH_PARENT_PROCESS) -- borrow the parent's console if
//      it has one.  Harmless when it fails.
//   2. STD_OUTPUT_HANDLE when it is a DISK or PIPE handle -- the caller
//      redirected us (`> out.txt`, `| more`, or a test harness capturing our
//      output).  That handle is inherited and valid whether or not step 1
//      succeeded, and honouring it is what makes redirection work at all.
//   3. CONOUT$ when step 1 succeeded -- the interactive console case.  Opened
//      by name rather than taken from STD_OUTPUT_HANDLE because a GUI-subsystem
//      process's inherited std handle is not guaranteed to address the console
//      we just attached to.
//
// KNOWN AND UNFIXABLE FROM INSIDE: cmd.exe and PowerShell do not WAIT for a
// GUI-subsystem process, so the shell prompt returns before our output lands
// and the text appears after it.  That is a property of the subsystem, not of
// this code; `start /wait greencurve.exe --help` orders it.  The alternative --
// shipping a second console-subsystem launcher stub -- is a packaging change,
// not a code change, and is deliberately not taken here.  Printing something
// out of order beats printing nothing.

#ifndef GREEN_CURVE_CLI_CONSOLE_SINK
#define GREEN_CURVE_CLI_CONSOLE_SINK

// The console we write to, or INVALID_HANDLE_VALUE when the caller has none.
static HANDLE g_cliConsoleHandle = INVALID_HANDLE_VALUE;
// Attach is attempted exactly once per process: AttachConsole() against a
// parent that has no console sets a last error we do not want to retry per line.
static bool g_cliConsoleResolved = false;
// True only for case 3 above.  A redirected sink is already positioned at the
// start of its own stream, so the prompt-separating newline would be a stray
// leading blank line in a captured file.
static bool g_cliConsoleIsInteractive = false;
static bool g_cliConsoleWroteAnything = false;

static bool gc_cli_handle_is_redirected_stream(HANDLE handle) {
    if (!handle || handle == INVALID_HANDLE_VALUE) return false;
    DWORD type = GetFileType(handle);
    return type == FILE_TYPE_DISK || type == FILE_TYPE_PIPE;
}

// Resolve the console sink once.  Returns true when CLI output has somewhere to
// go besides the log file.
static bool gc_cli_console_ready() {
    if (g_cliConsoleResolved) return g_cliConsoleHandle != INVALID_HANDLE_VALUE;
    g_cliConsoleResolved = true;

    bool attached = AttachConsole(ATTACH_PARENT_PROCESS) != FALSE;

    HANDLE std_out = GetStdHandle(STD_OUTPUT_HANDLE);
    if (gc_cli_handle_is_redirected_stream(std_out)) {
        g_cliConsoleHandle = std_out;
        g_cliConsoleIsInteractive = false;
        debug_log("cli console: writing to a redirected standard output stream (attached=%d)\n",
            attached ? 1 : 0);
        return true;
    }
    if (!attached) {
        debug_log("cli console: no parent console and no redirected output; the CLI log file is the only sink\n");
        return false;
    }
    // CreateFileW, not the A form: "CONOUT$" is a device name rather than a
    // path, but the project's Unicode-safety gate is deliberately absolute
    // about the ANSI path APIs and an exception here would be one more thing to
    // remember. The wide call is correct either way.
    HANDLE console = CreateFileW(L"CONOUT$", GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (console == INVALID_HANDLE_VALUE) {
        debug_log("cli console: attached to the parent console but CONOUT$ could not be opened (error %lu)\n",
            GetLastError());
        FreeConsole();
        return false;
    }
    g_cliConsoleHandle = console;
    g_cliConsoleIsInteractive = true;
    debug_log("cli console: attached to the parent console\n");
    return true;
}

// One line to the console, if there is one.  Never fails the caller: a console
// write error costs a line of feedback, never the operation the CLI is running.
static void gc_cli_console_write(const char* text) {
    if (!text || !text[0]) return;
    if (!gc_cli_console_ready()) return;
    if (!g_cliConsoleWroteAnything) {
        g_cliConsoleWroteAnything = true;
        // The shell printed its prompt and returned before we got here (see the
        // header note), so the cursor sits after it.  One newline keeps our
        // first line off the prompt.  Not written for a redirected sink.
        if (g_cliConsoleIsInteractive) {
            DWORD written = 0;
            WriteFile(g_cliConsoleHandle, "\n", 1, &written, nullptr);
        }
    }
    size_t length = strlen(text);
    if (length > 0xFFFFFFFFu) return;
    DWORD written = 0;
    WriteFile(g_cliConsoleHandle, text, (DWORD)length, &written, nullptr);
}

// Bound the now-appending CLI log (F-01-003).  Checked once per invocation,
// before the append handle is opened, so the truncation can be a plain
// CREATE_ALWAYS with no open handle to coordinate with -- unlike the debug log,
// only one process writes this file at a time.
static void gc_cli_log_trim_if_oversized(const char* path) {
    if (!path || !path[0]) return;
    WIN32_FILE_ATTRIBUTE_DATA info = {};
    if (!gc_GetFileAttributesExUtf8(path, GetFileExInfoStandard, &info)) return;
    long long size = ((long long)info.nFileSizeHigh << 32) |
                     (long long)info.nFileSizeLow;
    if (!gc_debug_log_rotation::should_rotate(
            size, (long long)gc_debug_log_rotation::kCliRotateBytes)) {
        return;
    }
    FILE* truncated = gc_fopen_utf8(path, "w");
    if (!truncated) {
        debug_log("cli log: size cap reached at %lld bytes but truncation failed\n", size);
        return;
    }
    char timestamp[64] = {};
    format_log_timestamp_prefix(timestamp, sizeof(timestamp));
    fprintf(truncated, "%s%s", timestamp,
            gc_debug_log_rotation::cli_marker_line());
    fclose(truncated);
    debug_log("cli log: truncated at %lld bytes (cap %d)\n", size,
        (int)gc_debug_log_rotation::kCliRotateBytes);
}

// The single CLI output primitive: timestamped to the log file, bare to the
// console.  The timestamp is what makes the file useful across sessions and
// what makes console output unreadable, so the two sinks get different shapes
// of the same line.
static void gc_cli_emit(FILE* logf, const char* text) {
    if (!text || !text[0]) return;
    if (logf) {
        char timestamp[64] = {};
        format_log_timestamp_prefix(timestamp, sizeof(timestamp));
        fprintf(logf, "%s%s", timestamp, text);
        fflush(logf);
    }
    gc_cli_console_write(text);
}

// printf-style line to the CONSOLE ONLY, for the paths that run before (or
// instead of) the log file: a command line that failed to parse, and a log file
// that could not be opened.
static void gc_cli_console_writef(const char* fmt, ...) {
    if (!fmt) return;
    char line[1024] = {};
    va_list ap;
    va_start(ap, fmt);
    StringCchVPrintfA(line, ARRAY_COUNT(line), fmt, ap);
    va_end(ap);
    gc_cli_console_write(line);
}

// printf-style CLI line, to both sinks.  A function rather than a macro body so
// entry.cpp's CLI_LOG stays one line; the formatting buffer matches the longest
// message any caller builds (ServiceResponse::message plus its prefix).
static void gc_cli_logf(FILE* logf, const char* fmt, ...) {
    if (!fmt) return;
    char line[1024] = {};
    va_list ap;
    va_start(ap, fmt);
    StringCchVPrintfA(line, ARRAY_COUNT(line), fmt, ap);
    va_end(ap);
    gc_cli_emit(logf, line);
}

#endif // GREEN_CURVE_CLI_CONSOLE_SINK
