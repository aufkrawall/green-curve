# Debug and Binary Analysis Tools

This file is a project-local tool inventory and runtime diagnostic guide. Treat documented paths as hints until verified on the current machine.

## General rules

- Verify that a tool exists and runs before relying on it.
- Prefer repository-pinned or project-local tools when available.
- Prefer discovery (`Get-Command`, `where.exe`, `command -v`, tool manifests, environment variables) over stale hardcoded paths.
- Treat dumps, logs, captures, symbols, extracted strings, and diagnostic output as potentially sensitive.
- Do not mutate binaries, symbols, global debugger flags, registry/system settings, or persistent runtime configuration unless explicitly requested.
- When a preferred tool is unavailable, use a safe equivalent when practical and record the resulting coverage limitation.

## Tool/path resolution

For Windows, `tools/discover-debug-tools.ps1` is the shared non-mutating discovery helper. Its generic machine-local output is `debug-tool-manifest.json` (under `%LOCALAPPDATA%\LLMDebugTools\` by default).

Use the first reliable source available:

1. generated `debug-tool-manifest.json`
2. local, uncommitted `tool-paths.env`
3. repository-local or pinned tool locations
4. shell discovery such as `Get-Command`, `where.exe`, or `command -v`
5. documented project-specific known-good paths
6. safe system defaults/fallbacks

The helper must not install packages, download tools, edit PATH, or mutate debugger/system state. If it is unavailable, use the remaining discovery sources directly.

## Project path variables

Variables can be defined in a local, uncommitted `tool-paths.env` file (see `tool-paths.example.env`):

```text
PROJECT_ROOT=
BUILD_ROOT=
INSTALL_ROOT=
SYMBOL_ROOT=
LOG_ROOT=
DUMP_ROOT=
CAPTURE_ROOT=
WINDOWS_SDK_DEBUGGERS_X86=
WINDOWS_SDK_DEBUGGERS_X64=
WINDOWS_SDK_DEBUGGERS_ARM=
WINDOWS_SDK_DEBUGGERS_ARM64=
MSVC_TOOLS_X86=
MSVC_TOOLS_X64=
MSVC_TOOLS_ARM64=
SYSINTERNALS_ROOT=
LLVM_ROOT=
FFMPEG_ROOT=
```

## Windows debugging and binary analysis

Windows SDK Debugging Tools commonly live in architecture-specific subdirectories under `Windows Kits\10\Debuggers`, including `x64`, `x86`, `arm`, and `arm64`. `tools/discover-debug-tools.ps1` generates candidates from `ProgramFiles(x86)` and `ProgramFiles`, preferring any matching `WINDOWS_SDK_DEBUGGERS_*` override first and falling back to PATH. Discover the variants relevant to the host and target instead of assuming x64 (Green Curve targets both `windows-x64` and `windows-arm64`).

- When analyzing crash dumps, use the correct symbol path that includes both the Microsoft symbol server AND the local PDB directory:
  ```powershell
  cdb -z "$env:DUMP_ROOT\crash.dmp" -y "srv*;$env:SYMBOL_ROOT" -c ".ecxr; k; q"
  ```
  The `srv*`-only path misses local PDBs and produces incomplete stack traces.

Common tools, when installed (use `tools/discover-debug-tools.ps1` to inspect current machine locations):

| Tool | Purpose | Default / Discovered path |
| --- | --- | --- |
| `cdb.exe` | Command-line `.dmp` debugging and stack inspection | `C:\Program Files (x86)\Windows Kits\10\Debuggers\x64\cdb.exe` (or `debug-tool-manifest.json`) |
| `windbg.exe` | Interactive `.dmp` debugging | `C:\Program Files (x86)\Windows Kits\10\Debuggers\x64\windbg.exe` (or `debug-tool-manifest.json`) |
| `WinDbgX.exe` | Interactive WinDbg Preview `.dmp` debugging | `%LOCALAPPDATA%\Microsoft\WindowsApps\WinDbgX.exe` |
| `dumpchk.exe` | Validate dump readability and basic dump metadata | `C:\Program Files (x86)\Windows Kits\10\Debuggers\x64\dumpchk.exe` |
| `symchk.exe` | Verify/download symbols for binaries and dumps | `C:\Program Files (x86)\Windows Kits\10\Debuggers\x64\symchk.exe` |
| `dbh.exe` | Inspect symbols and PDB contents | `C:\Program Files (x86)\Windows Kits\10\Debuggers\x64\dbh.exe` |
| `pdbcopy.exe` | Copy/strip PDBs for symbol handling | `C:\Program Files (x86)\Windows Kits\10\Debuggers\x64\pdbcopy.exe` |
| `symstore.exe` | Add/query files in a symbol store | `C:\Program Files (x86)\Windows Kits\10\Debuggers\x64\symstore.exe` |
| `gflags.exe` | Configure debug/runtime flags; mutation-capable, use only with explicit intent | `C:\Program Files (x86)\Windows Kits\10\Debuggers\x64\gflags.exe` |
| `umdh.exe` | Heap snapshot and leak investigation | `C:\Program Files (x86)\Windows Kits\10\Debuggers\x64\umdh.exe` |
| `dumpbin.exe` | Inspect PE/COFF headers, imports, exports, sections, symbols, disassembly | Visual Studio MSVC tools (`VC\Tools\MSVC\<version>\bin\...`) or PATH |
| `undname.exe` | Undecorate MSVC C++ symbols | Visual Studio MSVC tools or PATH |
| `link.exe /dump` | `dumpbin`-style fallback inspection | Visual Studio MSVC tools or PATH |
| `lib.exe /list` | List static library contents | Visual Studio MSVC tools or PATH |
| `editbin.exe` | PE/COFF mutation; do not use unless explicitly requested | Visual Studio MSVC tools or PATH |
| `procdump.exe` | Capture process dumps | `<tool-dir>\sysinternals\` (see the manifest) or WinGet Sysinternals |
| `procmon.exe` | Trace process, registry, file, and network activity | `<tool-dir>\sysinternals\` (see the manifest) or WinGet Sysinternals |
| `procexp.exe` | Inspect processes, handles, DLLs, and threads | `<tool-dir>\sysinternals\` (see the manifest) or WinGet Sysinternals |
| `vmmap.exe` | Inspect process virtual memory layout | `<tool-dir>\sysinternals\` (see the manifest) or WinGet Sysinternals |
| `handle.exe` | Find open handles | `<tool-dir>\sysinternals\` (see the manifest) or WinGet Sysinternals |
| `listdlls.exe` | List loaded DLLs for a process | `<tool-dir>\sysinternals\` (see the manifest) or WinGet Sysinternals |
| `sigcheck.exe` | Inspect signatures, versions, hashes, and VirusTotal metadata | `<tool-dir>\sysinternals\` (see the manifest) or WinGet Sysinternals |
| `strings.exe` | Extract printable strings from binaries or dumps | `<tool-dir>\sysinternals\` (see the manifest) or WinGet Sysinternals |
| `llvm-strings.exe` | Extract printable strings from COFF objects / DLLs | `C:\Program Files\LLVM\bin\llvm-strings.exe` or `LLVM_ROOT` |
| `llvm-objdump.exe` | Disassemble / inspect sections of DLLs/objects | `C:\Program Files\LLVM\bin\llvm-objdump.exe` or `LLVM_ROOT` |
| `ffmpeg.exe` | Media conversion/inspection helper for captures | `<tool-dir>\ffmpeg\` or `FFMPEG_ROOT` or PATH |
| `ffprobe.exe` | Media metadata/probing helper for captures | `<tool-dir>\ffmpeg\` or `FFMPEG_ROOT` or PATH |

Typical discovery commands:

```powershell
.\tools\discover-debug-tools.ps1 -ProjectRoot .
Get-Command cdb, windbg, dumpbin, llvm-objdump, procdump, procmon, sigcheck -ErrorAction SilentlyContinue
where.exe cdb.exe
where.exe dumpbin.exe
```

## Linux debugging and binary analysis

Common tools for Linux host, daemon, and client investigation:

| Tool | Purpose |
| --- | --- |
| `gdb` / `lldb` | Debugging and core dump analysis |
| `file` | Architecture and ABI identification (`x86-64`, `aarch64`) |
| `readelf` | ELF headers, sections, symbols, program headers, dynamic metadata |
| `objdump` / `llvm-objdump` | Headers, disassembly, imports, sections |
| `nm` / `llvm-nm` | Symbol inspection |
| `strings` / `llvm-strings` | Embedded string inspection |
| `patchelf --print-rpath` | RPATH/RUNPATH inspection |
| `checksec` | Hardening summary |
| `strace` | Syscall tracing (e.g. tracking daemon socket creation and credentials) |

Prefer `readelf -d`, `objdump -p`, or equivalent static inspection for untrusted binaries. Use `ldd` only for trusted local build artifacts because loader-based dependency inspection can execute code in unsafe circumstances.

## Identity in the log is tokenized (F-03-001)

`source/log_redaction_policy.h` turns account names, SIDs, authentication LUIDs
and user-profile paths into stable non-reversible tokens — `[path #<16 hex>]`
and `[id #<16 hex>]` — so two events for one account stay correlatable without
the log naming the account. The log is **on by default** and is the file users
attach to bug reports, so no log call may carry a raw identity. A service-side
log naming another account's profile directory would cross a user boundary, so
the policy applies to every call site, not only to the identity code.

The policy is enforced mechanically: `security_gates.check_log_redaction()`
walks every `debug_log` / `debug_log_on_change` call with a balanced-paren scan
and fails the build when a call names an identity-bearing expression
(`g_userDataDir`, `g_app.configPath`, `g_debugLogPath`,
`g_forcedStartupUserSam`, `taskName`) without routing it through a tokenizer.
Whole-identifier matching, so `taskNameToken` is not flagged as `taskName`.
Error paths that only fire in rare conditions (for example the crash-artifact
directory failing to resolve in `main_crash_artifacts.cpp`) are covered too,
because the gate scans source rather than sampled logs. Add to
`IDENTITY_BEARING_LOG_ARGUMENTS` when a new identity-bearing global appears.

`gc_log_wide_identifier_token()` is the UTF-16 variant, added for the task name;
it hashes code units directly so it stays pure and assertable on a Linux host.

**Reading a tokenized log:** the token is stable per input, so grouping by token
still tells you "these lines are about the same account/path". To confirm WHICH
account, reproduce locally and compare — the hash is deliberately one-way.

Grep for `line(s) dropped` (the writer could not keep up) and
`produced for an earlier log route` (F-04-002: a line outlived its route slot
and was written into the current session's file instead of its own) — both mean
this file is not a complete record of what happened.

## Debug log routing (which greencurve_debug.txt to read, verified 2026-09-12)

Both the GUI and the service append to a file named `greencurve_debug.txt`, but
WHICH file changes at runtime (`effective_debug_log_path()` in
`main_diagnostics.cpp` and `debug_log_set_route_path()` in `main_debug_log_writer.cpp`):

1. Service process, before any authorized request resolves user paths: the
   SYSTEM machine dir `%SystemRoot%\System32\config\systemprofile\AppData\
   Local\Green Curve\greencurve_debug.txt` (the "early" log; needs elevation).
2. After `resolve_service_user_data_paths(sessionId)` succeeds for the active
   session (first non-handoff authorized request): the service re-points to
   THE ACTIVE USER's `%LOCALAPPDATA%\Green Curve\greencurve_debug.txt`, the
   same file the GUI appends to. Fast user switching closes/reopens onto the
   new user's file via generation-stamped log routing (`debug_log_set_route_path()`).
   Queued asynchronous log records retain their route generation (`kMaxRouteSlots = 8`),
   so records queued before the session change are drained to the prior profile
   rather than leaking across sessions. So the active-era user-side log is MERGED GUI+service.
3. What genuinely lives only in the early/systemprofile copy: boot-time
   listener failures before any authorized request, pre-logon/handoff-only
   windows (handoff intentionally skips user-path resolution), and eras of no
   interactive session. Other accounts' eras live in their own profiles.
4. Logging is gated by `[debug] enabled` / `GREEN_CURVE_DEBUG`; disabled means
   neither file grows or helps.

Failure-pattern grep (elevated for the systemprofile copy); beware bare-digit
patterns like `1368` matching inside `runtimeLastApply=` values -- use word
boundaries:

```powershell
Select-String -Path $log -Pattern '\b1368\b','ERROR_CANNOT_IMPERSONATE','pipe worker \d','admission refused','dropping stalled','protocol mismatch header'
```

Power-target domain (added 2026-09-09). When a user reports an Apply refused
with `Power target did not apply` (spelled `Power target did not reset` before
2026-09-13, when the reset phase started writing the apply's own target instead
of the board default), or an inert/greyed power field, these lines answer
*which* NVML call the board refuses and what Green Curve published:

```powershell
Select-String -Path $log -Pattern 'read_power_limit:','set_power_limit:','skipping power reset','power edit enabled','gpu capability probe: power','OC range hints refreshed'
```

Read them together: `from 0/0/0 mW` in the OC range hints plus
`control state readback validity: ... power=0` is the surfaceless-board
signature. A pre-0.26 log has none of the `read_power_limit:` lines — that
silence is the bug this logging was added for.

Apply-transition clock ceiling (F-APPLY-CEILING, added 2026-09-13). When a
driver crash / TDR / `nvlddmkm` event is reported *around a profile switch*,
these lines say whether the GPU was capped while the apply raised the curve:

```powershell
Select-String -Path $log -Pattern 'apply clock witness','apply ceiling:','apply curve peak','curve strategy:','nvml_set_gpu_locked_clocks','nvml_reset_gpu_locked_clocks','settle at stock baseline'
```

Start with the verdict line, which answers the whole question on its own:

```powershell
Select-String -Path $log -Pattern 'apply clock witness verdict'
```

```
apply clock witness verdict: peak gpc=2940 MHz (at post-curve-batch (pre-lock))
  vs ceiling=2957 MHz armed=1 -> HELD; load peak util=97% power=243.1 W
  temp=61 C loadMeaningful=1 samples=9
```

`-> HELD` (or `HELD (driver rounded up one bin)`) with **`loadMeaningful=1`** is
the only combination that clears an under-load run. `loadMeaningful=0` means the
GPU was near-idle and the result proves nothing — the log says so itself in a
following NOTE line. `-> EXCEEDED` means the clamp did not hold and F-APPLY-CEILING
needs a different mechanism on that driver.

Then read the sequence: `apply ceiling: plan arm=1` then `armed ...` must appear
BEFORE `curve batch pass 1 begin`, and `apply curve peak after batch:` reports
the peak the written curve reaches with `transition clamp armed=`. A peak above
the requested lock with `armed=0` is the uncapped-transition shape, and the code
logs its own `WARNING` line for it. Cross-check the Windows System log:

```powershell
Get-WinEvent -FilterHashtable @{LogName='System'; ProviderName='nvlddmkm'} | Select-Object TimeCreated,Id
```

`nvlddmkm` event 153 with an empty message body is what a TDR on this GPU looks
like here; correlate its timestamp against the apply phase lines above.

Pipe deadline domain (added 2026-09-11). When a user reports a brief "lost
connection to the service" that recovers on its own, these lines answer whether
the service failed or the client merely gave up early:

```powershell
Select-String -Path $log -Pattern 'Timed out during reading service response','response write failed','OVERRAN its handler budget','waited .* ms for the runtime lock','on the serialized dispatch queue','connection epoch advanced'
```

The pre-0.26 signature is a `read kind=2 reason=telemetry cadence
durationMs=500 success=0 ... Timed out during reading service response`
followed within ~250 ms by the service's own `response write failed ...
(error 232)` — ERROR_NO_DATA, meaning the client closed the pipe on an answer
the service had already produced. That is the client's deadline firing, not a
service fault; see `windows-architecture.md` F-PIPE-DEADLINE.

To size the problem rather than eyeball it, bucket the durations:

```powershell
(Select-String -Path $log -Pattern 'read kind=2 .* durationMs=(\d+)' -AllMatches).Matches |
  ForEach-Object { [int]$_.Groups[1].Value } |
  Group-Object { [math]::Floor($_ / 100) * 100 } | Sort-Object Name
```

A spike sitting exactly ON the deadline (rather than a smooth tail) is the
tell. From 0.26 the `deadlineMs=` field is on every GUI read line, and the
service-side `OVERRAN its handler budget` / dispatch-queue / runtime-lock-wait
lines say which component consumed the time — none of that was observable
before.

Read-miss / log-stall domain (added 2026-09-12). From 0.26 a GUI read line
carries `reachability=` (0 unknown, 1 unreachable, 2 connected) and
`deadlineExpired=`, so a busy service and an absent one are no longer the same
log line:

```powershell
Select-String -Path $log -Pattern 'read miss reason=','read miss cleared','reachability=1','line\(s\) dropped'
```

`GUI service state: read miss ... the service is reachable` is the healthy
degradation (presentation kept, status line says "busy"); only
`reachability=1` reaches the disconnect teardown.

To tell a GPU stall from a DISK stall when a handler overruns, read two things
together: `lockWaitMs=` on the `OVERRAN its handler budget` line, and whether
log lines appear in the file OUT OF timestamp order. `debug_log()` formats its
timestamp before queuing, so a line whose timestamp is far older than its
neighbours was blocked on the write path, not produced late. `lockWaitMs=0`
plus out-of-order lines from more than one thread means the VOLUME stalled --
cross-check the Windows System log for Volsnap event 25 or disk warnings:

```powershell
Get-WinEvent -FilterHashtable @{LogName='System'; StartTime=$t0; EndTime=$t1} |
  Where-Object { $_.Level -le 3 }  # 1=critical, 2=error, 3=warning
```

That is how the 2026-09-12 log-stall was identified (a 19 s telemetry handler
with `lockWaitMs=0` coinciding with a Volsnap error 25 event). Since
0.26 the service no longer flushes per line, so this signature should not
recur; a `debug log: N line(s) dropped` marker is the new tell that the writer
fell behind.

Rotation (added 2026-08-23; moved onto the writer thread 2026-09-12): every
append checks size via `debug_log_rotation_policy.h`; at
`GC_DEBUG_LOG_ROTATE_BYTES` (32 MiB) the writer truncates IN PLACE (close -> CREATE_ALWAYS -> reopen-append) and writes
a one-line marker. Truncation rather than rename is required because GUI +
service share the user-side file through append-only handles -- FILE_APPEND_DATA
writes always land at EOF, so a cooperative truncate moves EOF for every
writer. Legacy oversized files self-heal on the first post-update write.
Pure boundaries covered by regression tests 4700+.
