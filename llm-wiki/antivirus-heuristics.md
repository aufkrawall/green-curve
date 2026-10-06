# Binary hygiene: role-separated images and honest metadata

Summary: Green Curve ships unsigned Windows binaries, so it has no publisher
reputation to lean on. The build therefore makes every shipped image look like
exactly what it is: each executable links only the code its role can run, its
identity metadata is correct and consistent, and nothing is packed, hidden or
disguised. Source and artifact gates keep it that way. What remains are the
imports the features genuinely need. This page records principles and
invariants, not scan verdicts; per-hash verdicts are local-only and are not
tracked.

Source anchors: `tools/pe_verify.py` (`stamp_pe_checksum`, `verify_pe_checksum`,
`verify_version_identity`, `verify_windows_binary_metadata`, `pe_imports`,
`verify_windows_manifest_identity`, `SERVICE_FORBIDDEN_UI_FUNCTIONS`,
`GUI_FORBIDDEN_SERVICE_FUNCTIONS`, `verify_service_resources`,
`verify_no_buildid_section`), `tools/pe_strings.py` (forbidden-name scan),
`tools/pe_resources.py` (manifest/version resource parsing),
`tools/pe_layout.py` (`.buildid` merge), `source/app_shared.h`
(`app_main_window`, `app_is_service_process`), `tools/security_gates.py`
(`check_binary_role_gates`), `tools/build_state.py` (`TRAY_ICON_RC_LINES`,
`WINDOWS_BINARY_IDENTITIES`, `VERSION_COMPANY_NAME`, `build_manifest_content`,
`compile_windows_resources`), `build.py` (`_verify_windows_artifact`,
`verify_release_binary`), `tools/installer_build.py` (`_append_payload`,
`installer_original_filename`, `_verify_uninstaller_surface`,
`UNINSTALLER_SOURCE_NAMES`), `source/installer_register.cpp`
(`gc_uninstall_execute`).

## Principles

- **Each image carries only its role.** The GUI contains no service runtime
  (SCM status, pipe accept, client impersonation); the service contains no GUI
  code (windows, tray, timers, DPI, GDI); the uninstaller contains no installer
  (no payload extraction, no install orchestration, no token relaunch). The
  roles are separated at compile time through constant-folding accessors, so
  the linker drops the dead branches, and artifact gates prove the result.
- **Metadata tells the truth.** Every PE has its own `OriginalFilename`,
  `CompanyName`, `Comments`, a correct PE checksum, a per-binary manifest
  identity and `requestedExecutionLevel`, and a bare `<stem>.pdb` CodeView
  name. All of it is parsed from the built artifact and gated, not just
  generated.
- **Nothing is packed or disguised.** The setup payload is a stored (never
  compressed) overlay with a plain footer; setup carries no decompressor; the
  uninstaller does not copy itself or delete its running image but schedules
  removal for the next restart; the service reads the memory clock through the
  driver API instead of spawning a hidden child with captured output; plain
  text buffers use ordinary heap allocation.
- **No undocumented aliases.** RNG calls use `BCryptGenRandom`; the
  `SystemFunction0*` alias family is forbidden in sources and shipped images.
- **Import bans match families, not just exact names** (name, +A/W/Ex/ExW,
  never a raw prefix), and a raw-byte scan covers every shipped image for the
  injection, anti-debug, download/exec and keyboard-hook families.
- **Debug layout:** the linker's `.buildid` section is merged into `.rdata`
  after linking (`tools/pe_layout.py`). The merge preserves RVAs and the
  CodeView record; if it cannot be proven safe the build FAILS. It never falls
  back to renaming the section.

## Deliberately kept (features need them)

- GUI: `SetWinEventHook(EVENT_SYSTEM_FOREGROUND)` + `GetForegroundWindow` +
  `GetWindowTextW` + process enumeration (auto-profiles), `RegisterHotKey`
  (hotkeys). Heuristic models may read this import set as input monitoring.
- Service: WinHTTP download + `WTSQueryUserToken`/`ImpersonateLoggedOnUser` +
  `CreateProcessW` (signed updater, per-user paths). Heuristic models may read
  this as a downloader.
- Setup: `WTSQueryUserToken` + `CreateProcessWithTokenW`/`AsUserW` (launch the
  GUI unelevated in the user's session after install). **Not in the
  uninstaller.**
- Uninstaller: `TerminateProcess` on the GUI and timed-out cleanup helpers,
  Task Scheduler COM/`schtasks.exe` for logon-task removal, a `HKEY_USERS` Run
  sweep, `MOVEFILE_DELAY_UNTIL_REBOOT` for the running image. These are what an
  uninstaller does; hiding them would be worse.
- Toolchain runtime artifacts: MinGW TLS callbacks and pseudo-relocation
  `VirtualProtect`/`VirtualQuery`; the static UCRT's `IsDebuggerPresent` import
  and `.fptable` section in MSVC-ABI images; no Rich header from LLD. These are
  properties of the toolchain runtime, not app code.

## Rejected alternatives

- **Hiding imports or strings** (`GetProcAddress`, string obfuscation,
  overriding a runtime import slot): hiding is itself an evasion signal, makes
  the binary harder to audit, and contradicts the project's no-`GetProcAddress`
  rule. The one toolchain-inherent import (see above) is left as the runtime
  emits it.
- **Faking toolchain fingerprints** (a forged Rich header, rewriting section
  names to look standard): deceptive; a duplicate `.rdata` name only disguises
  the section.
- **Copy the uninstaller to `%TEMP%` and run the copy** (NSIS/Inno style): a
  copy-self-and-execute pattern, plus an elevated binary in a user-writable
  folder. Restart-scheduled removal is used instead.
- **Embedding the payload as an `RT_RCDATA` resource:** still an embedded PE,
  no benefit over a stored overlay.
- **Resolving needed APIs dynamically to shrink the import table.**
- **Merging `.fptable` into `.rdata`:** it is the static UCRT's writable
  api-set thunk table; the merge would be flag-wrong.
- **`/MD` dynamic CRT for the MSVC-ABI variant:** adds a redistributable
  dependency to a portable single-exe tool.
- **Removing helper termination from the uninstaller:** cleanup helpers that
  time out must be terminated, or cleanup would keep running after the
  uninstaller reports failure.

## Not done — outside the code (maintainer decisions)

- **Authenticode signing** is the largest external trust signal and remains
  absent (see [updates.md](updates.md)). A no-cost option to evaluate: SignPath
  Foundation for qualifying open-source projects; paid options include OV
  certificates and Microsoft Artifact Signing. Microsoft's
  [SmartScreen guidance](https://learn.microsoft.com/en-us/windows/apps/package-and-deploy/smartscreen-reputation)
  says unsigned hashes start reputation anew on every release; signing lets
  publisher reputation accumulate but does not guarantee an immediate clean
  verdict, and EV no longer bypasses SmartScreen automatically.
- **False-positive review:** a detected file can be submitted to Microsoft's
  WDSI portal as a software developer (see Microsoft's
  [developer FAQ](https://learn.microsoft.com/en-us/defender-xdr/developer-faq);
  there is no general allow-list program). Uploading is publishing, so it is the
  maintainer's call; any binary change invalidates the hash.
- **User-facing documentation policy:** unsigned open-source hardware utilities
  cannot prevent arbitrary heuristic/ML false positives across vendors, so the
  README says so, asks users to verify the exact download, points to the fully
  auditable source and the Python-only local build, and advises scoped
  exceptions rather than disabled protection.

## Invariants

- `stamp_pe_checksum()` is the last byte edit to every shipped PE; the setup
  file is restamped after the payload append (the checksum covers the overlay).
- `pe_layout.normalize_pe_debug_layout()` runs on every Windows image BEFORE the
  checksum stamp (GUI/service in `build.py::_verify_windows_artifact`; setup
  stub and uninstaller in `tools/installer_build.py` before `_append_payload`).
  It merges `.buildid` into `.rdata` or raises; it never renames the section.
  No shipped PE, any arch or toolchain, carries `.buildid`
  (`verify_no_buildid_section`), and every CodeView path is a bare
  `<stem>.pdb` basename.
- No source contains the `SystemFunction0*` alias family
  (`security_gates.check_no_system_function_aliases`), and no shipped image
  carries it as text.
- Import bans match family spellings; `SetWindowsHookEx*` and
  `CreateRemoteThread*` are really banned. The forbidden-string scan covers
  every shipped image.
- `IsDebuggerPresent` is banned (import and string) in the llvm-mingw/Zig
  release images; the single exemption is the MSVC-ABI (clang-cl) static UCRT
  fault-handler import. `CheckRemoteDebuggerPresent` and
  `NtQueryInformationProcess` are banned in every variant, and no source may
  use any of these APIs.
- clang-cl x64 compiles are `-flto=thin` and x64 links carry `-opt:lldlto=2`;
  arm64 stays no-LTO on BOTH toolchains (branch-protection codegen invariant).
- Every shipped PE carries the shared non-empty `Comments` sentence and a
  consistent VarFileInfo translation; the parsed RT_MANIFEST carries the
  per-binary `requestedExecutionLevel` and the comctl32 v6 shape.
- GUI and service manifests use `processorArchitecture="*"` and distinct
  identities; the artifact gate reads the embedded resource.
- `verify_windows_binary_metadata()` parses the import table of every shipped
  image. GUI/setup/uninstaller may not gain token, network or process-injection
  imports; the service's WinHTTP/token imports are required by its signed
  updater and session handoff.
- Every Windows `verify_release_binary()` call passes `original_filename`;
  omitting it fails the identity gate by design.
- Setup accepts only a stored payload; the built stub contains no
  `cabinet.dll`, `CreateDecompressor` or `CloseDecompressor` text.
- No source contains `FileRenameInfo`, `FILE_DISPOSITION_FLAG_POSIX_SEMANTICS`,
  `FileDispositionInfoEx` or `(FILE_INFO_BY_HANDLE_CLASS)21`.
- `UNINSTALLER_SOURCE_NAMES` does not link `installer_apply.cpp`,
  `installer_payload.cpp`, `installer_prior.cpp`,
  `installer_register_install.cpp` or `installer_move_cleanup.cpp`; the built
  uninstaller contains no `cabinet.dll`, `GCAR0001`, `GCPAY001`,
  `WTSQueryUserToken`, `CreateProcessWithTokenW` or install-orchestrator
  progress strings (`_verify_uninstaller_surface`).
- The service image imports none of `SERVICE_FORBIDDEN_UI_FUNCTIONS` and no
  gdi32/comctl32; its only icon group is 101. Service-reachable code tests the
  window through `app_main_window()`, never `g_app.hMainWnd` directly.
- The GUI image imports none of `GUI_FORBIDDEN_SERVICE_FUNCTIONS`. Shared code
  tests the process role through `app_is_service_process()`, never
  `g_app.isServiceProcess` directly. `CreateNamedPipeW` is excluded from the
  ban: the no-LTO Zig ARM64 GUI link keeps it, and creating a pipe is weak
  evidence compared with accepting and impersonating a client.
- Both accessors keep their constant-folding bodies
  (`security_gates.check_binary_role_gates`): the service sees a constant-null
  window, the Windows GUI a constant-false process role.
  `gpu_backend_snapshot.h` is exempt (the clock-transition harness compiles it
  against a stand-in `g_app`).
- Windows has no subprocess-capture helper; `platform_win32.cpp` may not
  contain `CreatePipe` (source gate).
- `GC_SETUP_UNINSTALL_EXE` is `greencurve-uninstall.exe`; the move-cleanup and
  install-location allowlists name both it and the legacy `uninstall.exe`.

## Open questions / stale-risk

- Whether any of this changes vendor verdicts is only knowable by scanning the
  exact artifacts being shipped; every rebuild produces new hashes (the build
  number is a fingerprint input), and ML verdicts are retrained continuously.
  Treat each hygiene change as reasoned, not measured, until a release is
  scanned. ARM64 verdicts have never been measured.
- Setup still carries the token-duplication relaunch shape. An
  Explorer-mediated `IShellDispatch2::ShellExecute` relaunch would remove it
  but needs Explorer as the session shell and must keep the session-0 fallback
  the in-app updater needs; left as a maintainer decision.
- The zig linker whitelist rejects the `-merge:` flag, which is why the merge is
  a post-link step in `tools/pe_layout.py`; revisit if zig ever forwards it.
- The restart-pending uninstall finish page has not been clicked through live.
- Authenticode and a Windows-hosted clang-cl release path remain open external
  release decisions; neither can be replaced by hiding imports.

Last verified: 2026-10-06 (public rewrite; chronology, hashes and scan verdicts
live in the local-only notes).
