# Build System

## Security gate and artifact follow-up (verified 2026-10-01)

Source anchors: `tools/secret_scan.py`, `.github/workflows/ci.yml`, `.github/workflows/release.yml`,
`build.py`, `tools/pe_verify.py`, `source/cfg_glue.cpp`.
Both CI and release scan full Git history with pinned gitleaks 8.30.1 from its
SHA-256-verified archive, with redacted output and full checkout depth. Release
also executes --test and 5000 fuzz runs before building/publishing; exit traps
remove temporary tag-signing material and write credentials even on failure.
The broad fuzz-corpus scanner allowlist was removed. Source gates enforce this
workflow wiring; manual staged/post-commit review remains a separate gate.

Both Windows x64 toolchains now opt into CET shadow stacks; pe_verify requires
the extended DLL-characteristics bit on every x64 image, including installer
family images. _FORTIFY_SOURCE=3 replaces 2; Linux adds stack-clash protection.
All target check builds validated the changed flags and actual PE/ELF artifacts.

ARM64 release CFG remains deferred, NOT enabled blindly. A pinned-Zig compile
probe accepts -mguard=cf, but disassembly passes the indirect target in x15.
The current C shim receives an ordinary argument in x0 and is not an ARM64
CFG ABI implementation. Enabling the flag alone would be unsound. A future
change needs a validated ABI-correct check/runtime integration and native
ARM64 acceptance. MSVC ARM64 retains its existing OS CFG. ELF GNU property
note enforcement remains the documented Zig/CRT limitation; BTI/PAC/endbr64
instrumentation and the existing artifact gates remain in force.
The historical CET opt-in-absent discussion below describes pre-follow-up
artifacts; current x64 builds now carry the opt-in.

## Build tool

`python build.py` is the only build tool. No CMake, no package manager, no test runner.
Parallel scheduling and build-state helpers live in `tools/build_scheduler.py`
and `tools/build_state.py` (one-way dependencies, same pattern as the other
`tools/` modules); `build.py` remains the single entry point.

## Windows toolchains (dual variants, `--toolchain {auto,clang-cl,llvm-mingw}`)

A native Windows `python build.py` now produces both Windows variants for each
requested architecture. The `msvc` variant uses `clang-cl` + `lld-link` from a
standalone LLVM install (also: VS-bundled Clang component, then PATH), with
headers/libs from the local Visual Studio + Windows SDK. The `release` variant
uses the pinned llvm-mingw x64 path and pinned Zig ARM64 path with the same
release flag builders used by the Linux-hosted GitHub release job.
`tools/msvc_toolchain.py` owns MSVC discovery (fail-closed: each requested
architecture is compile+link probed with the real flag set before a build
depends on it),
the hardened flag builders, and the object-first compile/link helpers. The
native Windows default requires a verified MSVC installation because it must
produce both variants; `--toolchain clang-cl` and `--toolchain llvm-mingw` select
one explicitly. Linux hosts and Linux-only targets use the release path.

MSVC-ABI compile flags (cl-style; clang-cl rejects GNU-style flags under -WX,
and `-Oz` is unavailable — `-O1` is the size floor):

- `-std:c++17 -O1 -DNDEBUG -c`, `-D_CRT_SECURE_NO_WARNINGS` (ucrt deprecations
  the MinGW headers never emit), `-GS`, `-guard:cf`, `-sdl`, `-W4 -WX`,
  `-Wno-unused-function -Wno-unused-parameter` (same suppressions as MinGW),
  `-EHs-c- -GR-` (the -fno-exceptions/-fno-rtti spellings),
  `-ftrivial-auto-var-init=pattern`, `-fno-delete-null-pointer-checks` (both
  accepted cleanly under -WX), `-Zi` for objects.
- ARM64 adds `--target=aarch64-pc-windows-msvc -mbranch-protection=standard`
  and stays NO-LTO (see below).
- x64 compiles add `-flto=thin` (since 2026-09-25, optimization parity with the
  llvm-mingw release path and its measured antivirus verdict effect — dead-CRT
  elimination). Size came out roughly neutral: x64 GUI +3,072 bytes, service
  −5,120 bytes; the page previously said "No LTO: it measured larger", so the
  size verdict is settings-dependent, and parity — not size — is the reason
  now.
- The MSVC-ABI images keep the static UCRT's own kernel32!IsDebuggerPresent
  import (its invalid-parameter fault handler; no project reference). It is
  the single toolchain exemption in `tools/pe_verify.py` / `tools/pe_strings.py`
  (clang-cl only); the llvm-mingw/Zig release images keep the hard ban. The
  runtime's imports are deliberately not overridden (see
  antivirus-heuristics.md, rejected alternatives).
- Link (lld-link): `-guard:cf -opt:ref,icf -subsystem:windows`, plus
  `-cetcompat -opt:lldlto=2 -debug:full -pdb:<relative>` on x64 (cetcompat and
  the explicit ThinLTO level are x64-only).
- Artifact gates extended in `tools/pe_verify.py` (toolchain-aware): x64+clang-cl
  requires the EX_DLLCHARACTERISTICS CET_COMPAT debug entry; arm64+clang-cl
  requires a nonempty GFIDS table (CFG metadata the Zig build never had);
  llvm-mingw gates unchanged. RSDS sanitization now applies to both arches.
- `verify_windows_private_symbols` dispatches on the PDB file magic (MSF) rather
  than the arch: clang-cl emits PDBs for both architectures, Zig arm64 still
  produces the bare DWARF image.
- MinGW CRT glue split: `process_hardening.cpp` (fatal-dump hook +
  `initialize_process_mitigations()`) is linked by BOTH toolchains AND by the
  installer stubs (`INSTALLER_SOURCE_NAMES` / `UNINSTALLER_SOURCE_NAMES`,
  enforced by a `check_all` gate:
  dropping it fails every MinGW stub link with undefined
  `gc_invoke_fatal_dump_hook` — the 2026-08-29 release-packaging CI failure).
  All Windows entry points now call `initialize_process_mitigations()`,
  including setup/uninstaller (`installer_main.cpp:WinMain`, since 4b91037).
  Dynamic loader restrictions begin at that call; they cannot protect static
  imports resolved before entry.
  `cfg_glue.cpp` (the MinGW CFG shim) and `ssp_glue.cpp` (canary glue) are
  MinGW-only, replaced under clang-cl by /GS and the OS GFIDS validator.
- The MinGW driver implicitly appends `user32` to every link;
  `WINDOWS_SERVICE_LINK_LIBS` now names it explicitly (lld-link has no
  implicit libraries).
- Installer stubs build with the same toolchain; the uninstaller/setup GUIDs
  (`CLSID_TaskScheduler`, `IID_ITaskService`) are `DEFINE_GUID`-instantiated
  under `_MSC_VER` in installer_autostart.cpp because the SDK's taskschd.h only
  declares them (MinGW's libuuid carries them).

Empirical verification (2026-08-28, RTX 5070 host): x64 self-test FULL under
real CFG + strict hardware shadow stacks; arm64 BTI/PAC/AUT preserved through
lld-link; regression suite + fuzzing + tidy green. Costs accepted knowingly:
the hermetic model covers the pinned `release` Windows variant, while the
`msvc` variant depends on an unattestable local MSVC/SDK installation; Linux
hosts still build Windows through the pinned release path.

## Packaging and distribution output layout under `dist/`

Every compiled binary and release package is written under its target-specific subfolder in `dist/`, never into the repository root:

- `dist/windows-<arch>/<variant>/`: on native Windows builds, both variants (`msvc` and `release`) stage their payload (`greencurve/`), package archive (`.7z`), and standalone installer (`-setup.exe`) alongside `.sha256` checksums.
- `dist/windows-<arch>/`: on Linux CI cross-compiles where a single release variant is built.
- `dist/linux-<arch>/`: Linux tarballs (`.tar.xz`), Arch Linux packages (`.pkg.tar.zst`), and checksums are packaged alongside the binary payload (`greencurve/greencurve`).
- `dist/symbols/<target>/`: private PDB and DWARF debug symbol artifacts.

`tools/build_variants.py:package_dir()` derives the package directory directly from `payload_dir()` (`os.path.dirname(payload_dir(...))`), keeping packaging consistent across all targets and host platforms. Stale artifacts in both `package_dir` and the legacy repository root are purged automatically during the packaging step.

## Download Sources

Zig and the Windows-hosted llvm-mingw archive are downloaded from the project's
own GitHub Release (`Compilers-1.0`). The Linux-hosted llvm-mingw archive comes
from the matching upstream llvm-mingw release because the project mirror does
not yet carry that host variant. Systems with the matching archive under
`compilers/` use that local vendor copy first. SHA-256 verification runs in
every case. The native Windows `release` variant uses the Windows-host archive;
it is the same pinned compiler release and source-level flag path as the
Linux-hosted release, but byte-for-byte Linux release reproduction still
requires a Linux host (or a Linux build environment).

- Release base: `https://github.com/aufkrawall/green-curve/releases/download/Compilers-1.0/`
- Vendor fallback: `compilers/<tool>/<archive>` (checked first)

The `compilers/` directory also stores per-tool `manifest.json` files with
archive checksums, license files, and extracted-binary integrity markers.

### Pinned symlink chains (release-blocking, 2026-08-18)

`verify_tree()` required a pinned symlink to resolve to a non-symlink in ONE
hop. llvm-mingw's **Linux-hosted** archive aliases in more than one --
`aarch64-w64-mingw32-addr2line -> llvm-addr2line -> llvm-symbolizer`, 72 chains
-- so the rule rejected the upstream archive and the 0.23.1 release build failed
at the verification step.

**Only CI reaches this.** The Windows-hosted archive has zero symlinks, so every
local build passed; the Linux-hosted path is exercised only by the release and
CI workflows. Worth remembering as a class: toolchain rules validated on one
host archive may be untested on the other.

The walk now follows the chain, and the security property is why it walks rather
than trusts: every hop must itself be a pinned entry whose link text matches the
manifest, the chain must terminate at a pinned regular file whose digest is
verified, a hop missing from the manifest or escaping the root is a hard
failure, and a bounded hop count turns a cycle into a refusal rather than a
hang. `toolchain.run_self_tests()` covers the chain, an unpinned intermediate
hop, and a cycle.

### llvm-mingw (Windows-target builds on Windows or Linux)

- Downloads **llvm-mingw 20260519** (clang 22.1.6, LLD) automatically.
- Windows hosts use `llvm-mingw-20260519-ucrt-x86_64.zip` in `./llvm-mingw/`;
  Linux hosts use the upstream
  `llvm-mingw-20260519-ucrt-ubuntu-22.04-x86_64.tar.xz` in
  `./llvm-mingw-linux/`, so a checkout shared with Windows cannot mix caches.
- Windows invokes `clang++.exe`; Linux invokes the target-prefixed
  `x86_64-w64-mingw32-clang++` wrapper. The generic Linux `clang++` defaults to
  `x86_64-unknown-linux-gnu` and cannot accept the Windows CFG flags.
- Both the archive and extracted compiler entry point have host-specific pinned
  SHA-256 digests. Tar links must resolve inside the verified toolchain root.
- `verify_tree()` now verifies pinned symlinks structurally: the path must
  still be a symlink, the link text must match the manifest exactly, the
  resolved target must stay inside the toolchain, and the target must itself be
  an independently pinned entry. An ordinary file substituted where a symlink
  was pinned is rejected, which closes the restored-cache tampering gap that
  existed when only `os.path.exists()` was checked. `toolchain.run_self_tests()`
  runs on every build.
- Resource compilation uses the host-native `llvm-rc[.exe]`.
- Static libc++ linked via `-static` to avoid `libc++.dll` runtime dependency
- CFG fully supported via `-mguard=cf` (Guard CF function table, FID table, long-jump target table)

### Zig 0.13.0 (Linux cross-builds + Windows arm64)

- Downloads **Zig 0.13.0** automatically on first run into `./zig/`
- Uses `zig c++` (bundled clang) as:
  - Linux cross-compiler (x64 + arm64)
  - **Windows arm64 compiler** (since build 334, replaces llvm-mingw to dodge the
    aarch64 COFF LLD "misaligned ldr/str offset" bug)
- ARM64 translation units compile to individual objects with
  `-mbranch-protection=standard -fno-lto -O2` before linking. This preserves
  BTI/PAC across final code generation and keeps Zig's `main.lib` side product
  in scratch storage.
- SHA-256 verified after download/copy
- Cached toolchain binaries are verified against executable digests pinned in
  `build.py`, not adjacent `.sha256` markers. The markers are rewritten after a
  trusted extraction, but they are not the authority because a writable cache
  attacker could replace both a binary and its adjacent marker.

## Targets

**`python build.py` (no args) builds the full matrix: Windows + Linux, each for
x64 and arm64, and packages each into a release archive — a `.7z` for Windows, a
`.tar.xz` for Linux (see [the container table](#release-packaging) below for why
they differ). On native Windows, each Windows architecture is built twice and
packaged under separate `msvc` and `release` subfolders. Linux-hosted release
builds retain the public root-level package names and use the release toolchain
once.**

Each `(os, arch)` target builds into its **own isolated folder** under `dist/`,
using the canonical binary names (no arch suffixes, no shared root/temp paths):

```
# native Windows
dist/windows-x64/msvc/greencurve/{greencurve.exe, greencurve-service.exe}
dist/windows-x64/release/greencurve/{greencurve.exe, greencurve-service.exe}
dist/windows-arm64/msvc/greencurve/{greencurve.exe, greencurve-service.exe}
dist/windows-arm64/release/greencurve/{greencurve.exe, greencurve-service.exe}
# Linux and Linux-hosted Windows release builds
dist/linux-x64/greencurve/greencurve
dist/linux-arm64/greencurve/greencurve
```

### Release packaging

Release archives are staged from an exact allowlist rather than archiving the
build directory. Each archive contains only canonical binaries plus `README.md`
and `LICENSE`. The root folder comes from `release_archive_root()`:
**`Green Curve/` on Windows** (so extracting matches what the installer creates)
and `greencurve/` on Linux. The build rejects unexpected payload files and reads
the completed archive back against the manifest.

**The container is per-OS and that is a correctness requirement, not a
preference** (`release_archive_extension()`):

| OS | Archive | Written by |
|----|---------|-----------|
| Windows | `greencurve-<VERSION>-windows-<arch>.7z` | 7-Zip |
| Linux | `greencurve-<VERSION>-linux-<arch>.tar.xz` | Python `tarfile` (`w:xz`, preset 9) |

On native Windows, the Windows archive, checksum, and setup executable are
written beside the selected variant payload under
`dist/windows-<arch>/msvc/` or `dist/windows-<arch>/release/`. Linux-hosted
release builds continue to write the public package names at the repository
root, which is the layout consumed by `release.yml` and the updater.

A Linux release has to carry Unix file modes — `greencurve` and
`greencurve-setup.sh` are both invoked directly, and the wrapper's own
`require_binary` aborts on `[ -x "$BINARY" ]` — and **7-Zip on Windows cannot
record them**. It stores Windows attributes only (a shipped member reads
`Attributes = A`, no mode), and no switch changes that; `os.chmod(path, 0o755)`
on Windows is likewise a no-op that leaves `0o666`. `tarfile` writes the mode
into the header explicitly, along with `uid/gid 0` and `uname/gname root`, so
the tarball is identical whether it was produced on Windows or Linux. tar is
also the format a Linux user can already unpack; p7zip is a separate install on
most distributions.

`verify_linux_tarball()` reads the finished file back and asserts member names,
`release_member_mode()` per member (`0o755` for the binary and `*.sh`, `0o644`
otherwise), zero ownership, LF-only text, and a `#!` shebang on the script.
`verify_seven_zip_manifest()` is the Windows counterpart (names only — the
format records nothing else worth checking).

**Linux release text is rewritten, not copied.** `stage_release_file()` sends
`*.sh`, `*.md` and `LICENSE` through `normalize_release_text()` for the Linux
archive instead of `shutil.copy2`. This working tree is CRLF on Windows
(`core.autocrlf=true`), and a verbatim copy shipped `#!/usr/bin/env bash\r`. A
lone CR that survives CRLF→LF conversion is a hard error rather than something
to rewrite. `.gitattributes` pins `*.sh text eol=lf` so the source is LF in the
first place; the packaging-time rewrite covers clones that predate it. Binaries
are still copied byte-for-byte.

7-Zip is required only for the *Windows packaging* step, and not for the build.
When `find_seven_zip()` finds nothing (PATH `7z`/`7za`/`7zr`, then the standard
Windows install dirs), `main()` packages every Linux target normally, skips only
the Windows ones, prints `report_packaging_skipped()` — a loud warning naming
every skipped binary and an install hint — and still exits 0. For those targets
that is the same end state as `--no-package`: the binaries under `dist/` are
complete and have passed every verification gate; only the `.7z` archives, their
`.sha256` files, and the Windows setup `.exe` are missing.
`report_packaging_skipped()` raises on a non-Windows entry, so a Linux target
cannot be withheld for want of a tool it does not use. The archiver is resolved
**once, before any staging**, so a missing 7-Zip can never abort a run half-way
through packaging.

Packaging first deletes every name the target could have produced
(`release_archive_paths()`, both containers plus `.sha256`), so a stale — and
now known-broken — `greencurve-<VERSION>-linux-<arch>.7z` from an earlier build
cannot sit beside the current tarball and be distributed by mistake.

Each Windows target and native-Windows variant additionally produces
`greencurve-<VERSION>-windows-<arch>-setup.exe` from the same staged folder, so
an archive and an installed copy are the same bits. See
[installer.md](installer.md).
Every host stores the setup payload uncompressed (`METHOD_STORE`, deliberate
since 2026-09-23 — see [installer.md](installer.md#compression-none-deliberately--2026-09-23));
the setup stub and uninstaller are cross-compiled on Linux and pass the normal
PE verification.

### Antivirus-heuristic hygiene (every Windows PE, 2026-09-23)

- **Checksum.** `pe_verify.stamp_pe_checksum()` writes the correct
  `OptionalHeader.CheckSum` (CheckSumMappedFile algorithm, whole file incl.
  overlay) as the LAST byte edit: after RSDS sanitization in
  `_verify_windows_artifact()`, after linking for the setup stub/uninstaller,
  and again after the payload append for the finished setup file.
  `verify_release_binary()` rejects a stale/zero checksum. Toolchain-neutral on
  purpose: lld-link's `/release` also works (verified), but llvm-mingw and Zig
  have no equivalent and CI builds with those.
- **Identity.** GUI and service get separate resource scripts
  (`icon.rc`/`icon-service.rc` -> `icon.res`/`icon-service.res`, generated by
  `build_state.compile_windows_resources()`; identities in
  `build_state.WINDOWS_BINARY_IDENTITIES`). Every PE carries `CompanyName` and an
  `OriginalFilename` equal to the name it ships as — the setup file's is the full
  release name. The GUI and service manifests are separate resources with
  architecture-neutral `processorArchitecture="*"` identities; the service no
  longer embeds the GUI's `GreenCurve` assembly identity. `verify_release_binary(...,
  original_filename=)` requires the identity (`pe_verify.verify_version_identity`
  plus `verify_windows_manifest_identity`). Before this the service claimed
  `greencurve.exe`.
- **Import surface.** `verify_windows_binary_metadata()` parses the PE import
  table and applies per-image required/forbidden DLL and API policies. The GUI,
  setup, and uninstaller may not gain session-token, network, or process-injection
  imports; the service's WinHTTP and token imports are required by its signed
  updater and session handoff. Delay imports are rejected. The legacy MinGW
  linker may emit a harmless export directory, so the no-exports assertion is
  strict only for clang-cl.
- Self-tests: `pe_verify.run_self_tests()` (checksum vectors cross-checked
  against pefile and lld `/release`, identity/import parser fixtures including
  forbidden APIs) and `build_state.run_resource_identity_self_tests()` (separate
  GUI/service manifests and resource names), both run by `build.py --test`.
- Rationale and what was deliberately NOT done:
  [antivirus-heuristics.md](antivirus-heuristics.md).

### Scope flags

| Flag | Purpose |
|------|---------|
| `--target {windows,linux,all}` | OS to build (default `all`) |
| `--arch {x64,arm64,all}` | Architecture(s) to build (default `all`) |
| `--toolchain {auto,clang-cl,llvm-mingw}` | Native Windows `auto` builds both variants; explicit values select one |
| `--jobs N` / `-j N` | Cap concurrent compiler/linker subprocesses (default `auto`; `1` = legacy serial build) |
| `--no-package` | Skip the release archives and the setup executables |

### ARM64 notes

- Native Windows builds both ARM64 paths: `clang-cl
  aarch64-pc-windows-msvc` for `msvc`, and Zig `aarch64-windows-gnu` for
  `release`; Linux uses `aarch64-linux-gnu`. Both Windows paths compile each
  source to an object with `-mbranch-protection=standard` before linking. Final
  binaries must contain BTI and matching PAC/AUT instructions (Windows uses the
  B key; Linux the A key) or the build fails. The clang-cl arm64 path
  additionally produces CFG metadata (GFIDS) and PDB symbols, replacing the old
  DWARF/objcopy dance; the historical lld "misaligned ldr/str" bug did not
  reproduce with the verified toolchain's lld-link, and the dual-compile+link
  toolchain probe would catch it before a real build depends on arm64.
- Full LTO and ThinLTO were tested for Linux ARM64, and full LTO for Windows
  ARM64, with the pinned Zig 0.13 toolchain on 2026-08-01. All three linked but
  emitted zero BTI/PAC/AUT instructions, so the artifact verifier rejected
  them. ARM64's no-LTO policy is therefore a security invariant, not an
  untested assumption.
- Windows ARM64 does not enable x64-specific CFG glue. PE ASLR/DEP/high-entropy
  VA remain mandatory and are verified post-link.
- x64 Windows `msvc` uses clang-cl/lld-link with real CFG and CET; the
  `release` variant retains llvm-mingw, LTO, ICF, CFG, and the narrowly audited
  known CFG-shim duplicate. Linux x64 also uses full LTO while retaining PIE,
  RELRO, immediate binding, NX stack, CET instrumentation and split private
  symbols.

### Parallel builds (`--jobs`)

`python build.py` runs the full matrix concurrently within each compiler
variant by default. `--jobs N` caps the number of compiler/linker subprocesses;
`--jobs 1` keeps the serial build. The default is `auto`: bounded by CPU count
and physical RAM (about 4 GB per job, so a 32 GB / 16-thread machine resolves
to 7 jobs, and a small CI runner stays serial).

- Phase 1: each Windows variant is a separate sequential batch on native
  Windows. Within a batch, the x64/arm64 GUI and service tasks are independent
  members of one `ThreadPoolExecutor`; the Linux targets form another batch.
  Variant payloads, PDB/symbol paths, temp outputs, archives, and setup files
  are isolated. `generate_icon()` + `compile_resources()` are hoisted to run
  once before the pools, and worker `print`s are made atomic for the duration
  so concurrent logs cannot interleave.
- Phase 2: each binary parallelizes its own translation units.
  - Windows x64 `msvc`: clang-cl object-first compilation with `/GS`,
    `/guard:cf`, `/cetcompat`, and lld-link. The `release` variant uses the
    10-TU LTO object-first path with `-flto -c`, `-gcodeview`, `--icf=safe`,
    static linking, stripping, and the known CFG-shim duplicate audit.
  - Linux x64: the 23 TUs compile to LTO bitcode objects with `-flto -c`, then
    link with the same `-flto` plus the complete ELF hardening and symbol flags.
  - ARM64 (both Windows variants + Linux): the object-first paths keep their
    exact commands and BTI/PAC link, but the per-object loop is a bounded
    parallel map.
- One `JobLimiter` semaphore spans every nested pool, so the total number of
  concurrent compiler/linker processes never exceeds `--jobs` even while a
  variant's binaries compile objects at once.
- **Every Zig *link* runs under a cross-process lock** (see the next section):
  only one Zig process ever mutates the shared global cache at a time. Plain
  `-c` compiles never write the global cache (verified empirically on the
  pinned 0.13.0), so compile parallelism is unaffected.
- Every binary in every variant still runs the full verification stack
  (PE/ELF hardening, version match, private-symbol extraction, CET/`endbr64`,
  BTI/PAC) before it can be packaged.

### Zig cache integrity and link serialization

The release matrix runs up to four Zig-driven links concurrently
(windows-arm64 GUI/service, linux-x64, linux-arm64) sharing one
`ZIG_GLOBAL_CACHE_DIR`. On the pinned Zig 0.13.0 that shared cache can end up
**poisoned on Windows**: a cache manifest survives while its artifact directory
(``o/<hash>/compiler_rt.lib`` or ``libcompiler_rt.a``) is absent. Zig never
self-heals that state — a manifest hit whose file is missing is a hard error —
so every later link of that target fails with
`ld.lld: error: cannot open .../o/<hash>/libcompiler_rt.a` (ELF) or
`lld-link: error: could not open '.../compiler_rt.lib': ...` (COFF), and the
state persists across runs. Upstream acknowledges a residual Windows-specific
cache race (ziglang/zig#14815 thread), cache-race regressions as late as
0.14.0 (#23110), and that partial cache pruning is unsupported (#18763). CI
never exercises the failure path: its earlier `--check --target all` step warms
the cache before the release-build step, and the release workflow runs on
Linux. The 2026-08-28 matrix failure on the native Windows host is the
canonical incident (the full investigation is kept in the local-only log).

`tools/zig_cache.py` (one-way dependency) provides two defenses, wired at
every Zig link call site (`_run_zig_link` in build.py for the three release
link functions and the `--jobs 1` serial fallback, and -- via `ctx._run_zig_link`
-- for both native-Linux regression fixtures in
`security_gates.run_linux_fixtures()`, which since 2026-09-15 cross-LINKS them
on non-Linux hosts instead of compiling them to an object;
`zig_cache.run_zig_link` for the arm64 installer link and the fuzz fixture
link):

- **Cross-process link lock.** One exclusive byte-range lock on
  `build-tmp/zig-link-cache.lock` (`LockFileEx` on Windows, `flock` on POSIX;
  per handle, so it excludes other threads of the same process as well as
  other build invocations; kernel-blocking acquire — no polling, no timeout).
  The lock file must stay OUTSIDE every wipeable cache root, or a repair could
  delete it out from under waiting acquirers. Windows named mutexes were
  rejected because they are re-entrant per thread.
- **Poisoned-cache repair.** When a link fails on a diagnostic naming an
  artifact inside a cache root whose file is verifiably absent, the affected
  root is removed and the link retried exactly once — loudly, under the same
  lock. The `h/` manifest file names are not derivable from the `o/` directory
  names (verified 0/10 pairs), so the poisoned manifest cannot be targeted
  surgically; removing the root is the upstream-documented remedy. Failures
  that do not match the signature, or whose named artifact still exists, are
  never repaired and propagate unchanged.
- **Compile-path diagnosis.** Compiles cache their objects in
  `ZIG_LOCAL_CACHE_DIR`; a poisoned local cache cannot be wiped safely while
  sibling compiles run, so `_run_compiler` turns that failure into an explicit
  actionable error naming the cache root to delete.

Deterministic self-tests (real-error-line parser fixtures, lock exclusivity,
wipe/refusal rules, retry-once with an injected runner) and source guards that
require the wrapper at every Zig link site run inside
`run_build_script_regression_tests` (`python build.py --test`).

### Packaging arch guard

`package_release_archive` calls `detect_binary_arch()` (reads the PE
`IMAGE_FILE_HEADER.Machine` / ELF `e_machine`) on every binary and **aborts the
build** if a binary's real architecture doesn't match the archive's target — so a
cross-arch bundle (arm64 GUI + x64 service, or a stale binary) is impossible. The
canonical names never collide because each target lives in its own
`dist/<os>-<arch>/greencurve/` folder.

## Additional modes

| Flag | Purpose |
|------|---------|
| `--check` | Build requested `--target`/`--arch` combinations into isolated `build-tmp/` directories |
| `--test` | Run pure regression tests (no GPU hardware) |
| `--lsp` | Regenerate `compile_commands.json` for clangd |
| `--sanitizer` | Build with UBSan (now default in `--test`, kept for backward compat) |
| `--asan` | Build with AddressSanitizer in addition to default UBSan |
| `--tidy` | Run host-correct clang-tidy and fail on findings outside the baseline |
| `--tidy-baseline` | Merge current findings into `tidy-baseline.txt` (adds only; drops an entry solely when its source file is gone) |

## Static analysis (`--tidy`)

`tools/static_analysis.py` owns the runner. Windows uses the clang-tidy bundled
with llvm-mingw; Linux resolves a native `clang-tidy` from `PATH`, regenerates
the compile database on that host, and filters out PE-only commands such as
`-mguard=cf`. A temporary filtered database prevents a shared source such as
`fan_curve.cpp` from accidentally selecting its Windows command on Linux.

**The LSP database is generated from the single-command builders**
(`generate_lsp_files()` -> `get_windows_{gui,service}_compile_command()`), so
those builders must emit every flag as ONE argument. A bare
`*"-DFOO=1"` conditional explodes the string into single characters: the
corrupted service entry then made clang-tidy fail SILENTLY on `main.cpp`
(an execution failure with no diagnostic, not a finding — 2026-08-29, fixed).
The same corruption would have broken the legacy `--jobs 1` llvm-mingw x64
service build, whose link is that command. The build-script self-tests
(`run_build_script_regression_tests`) now reject single-character arguments
and misplaced service defines in both builders' x64/arm64 commands.

- Check families: `bugprone-*`, `cert-*`, `clang-analyzer-*`, `misc-*`.
- **Disabled deliberately**, with reasons recorded in `TIDY_CHECKS`:
  - `misc-const-correctness` — pure style; alone produced >3000 hits that
    buried every real finding.
  - `misc-include-cleaner`, `bugprone-suspicious-include`,
    `misc-use-internal-linkage` — incompatible with the deliberate amalgamated
    shard layout, where `.cpp` files are `#include`d into an aggregator.
  - `clang-analyzer-optin.performance.Padding` — struct layout is dictated by
    the IPC wire format and Win32 handle grouping.
  - `bugprone-reserved-identifier` / `cert-dcl37-c` / `cert-dcl51-cpp` —
    `_WIN32_WINNT` and `_WIN32_IE` are the documented Win32 configuration
    macros.
  - `cert-dcl50-cpp`, `cert-err33-c`, `bugprone-easily-swappable-parameters`,
    `bugprone-narrowing-conversions`, and a few `misc-*` — project conventions
    (C varargs logging helpers, intentionally ignored bounded-printf returns).
- **clang-tidy needs llvm-mingw's libc++ headers passed explicitly.** It runs
  its own driver and does not inherit `clang++`'s mingw search path, so without
  `-isystem <llvm-mingw>/include/c++/v1` a header such as `<cstddef>` is not
  found and the whole translation unit is reported as
  `clang-diagnostic-error`. That was two spurious "errors" on the first run.
- Findings are compared against `tidy-baseline.txt` (path + check name, without
  line numbers so unrelated edits above a finding do not churn the baseline).
  Currently **34** baselined entries, and the file records which clang-tidy
  wrote it (`# Generated by:`) purely so version skew is diagnosable.
- **Hard ratchet:** a new finding, `clang-diagnostic-error`, non-zero analyzer
  exit, or missing analyzer fails `--tidy`. Existing baseline entries do not.
- **The baseline is portable across clang-tidy versions on purpose**, because it
  is written on one host and enforced on another (dev LLVM 22 vs CI's Ubuntu
  clang-tidy 18). Two version-dependent details broke that once, and both are
  now pinned rather than papered over — see "Cross-version portability" below.
- **`--tidy-baseline` only ever adds.** It merges this run's findings into the
  file and drops an entry only when its source file no longer exists. Nothing
  else is pruned automatically: "not reported on this host" is not "fixed", so
  auto-pruning would let a Linux run delete the Windows backlog, or an older
  clang-tidy delete what a newer one still reports. Retiring a genuinely fixed
  finding is a deliberate one-line deletion from `tidy-baseline.txt`; if that
  was premature, the next run fails loudly instead of silently.
- The first real Linux ratchet run found and fixed object-representation
  comparisons over `termios` and `DesiredSettings`, static `strerror()` storage
  retained across calls, duplicate-branch logging code, and header-defined
  external functions. The obsolete permission-diagnostic baseline entry was
  removed rather than replaced.

### Cross-version portability (2026-07-30)

The Linux CI job failed on three findings the baseline already carried. Both
causes were clang-tidy version behaviour, verified locally against a real
clang-tidy 18.1.1 (`pip install clang-tidy==18.1.1`) beside the host's 22.1.8:

- **Alias names are version-dependent.** clang-tidy prints every *enabled* alias
  of a check. LLVM 20 promoted `cert-env33-c` to `bugprone-command-processor`
  and `cert-err34-c` to `bugprone-unchecked-string-to-number-conversion`, so one
  unchanged finding prints as `[cert-err34-c]` on clang-tidy 18 and as
  `[bugprone-unchecked-string-to-number-conversion,cert-err34-c]` on 20+.
  Comparing that printed list verbatim made the baseline non-portable. Entries
  now record the whole alias set and match by **set intersection** (`_covered`):
  one shared name means one check.
- **The analyzed file set was version-dependent.** The project `#include`s
  amalgamated `.cpp` shards, so most diagnostics land in a non-main file.
  clang-tidy 18 suppresses those unless a header filter matches; 20+ shows them
  by default. CI was therefore silently analyzing less than the dev host — it
  never saw `linux_backend_discovery.cpp` or `linux_daemon_transport.cpp` at
  all. `SHARD_HEADER_FILTER` (`[/\\](source|tests)[/\\][^/\\]*\.cpp$`) now pins
  the surface explicitly: shards in, `.h` headers out.

Residual, and expected: analyzer *capability* still differs between versions.
LLVM 22 reports `clang-analyzer-optin.taint.TaintedAlloc` in
`linux_daemon_transport.cpp` that 18 does not. That prints as an advisory naming
the running version, never as a failure, and `--tidy-baseline` will not delete
it. Verified after the fix: `--tidy` is clean on both 18.1.1 and 22.1.8, and
still fails (exit 1) when a baseline line is removed.

### Recurrent Linux CI failure (2026-09-23)

Three consecutive Linux CI runs (`084e37b`, `b3231f7`, `e3e432c`) failed on the
same two LLVM 18.1.3 findings after builds and tests passed. The response
absence check had compared raw object bytes, including potentially indeterminate
alignment padding; it now checks every typed state field and declared reserved
array recursively. The regression poisons a snapshot padding byte, preserves
rejection of nonzero payload/reserved fields, and covers the startup profile.
The second finding was `run_all_tests_middle(char** argv)`: Linux excludes the
Windows-only tests that use the argument, so the parameter is now marked
`[[maybe_unused]]`.

Verified locally on 2026-09-23 with `python build.py --test`,
`python build.py --test --asan`, `python build.py --tidy` (LLVM 22.1.6; no new
findings), and the full Windows/Linux x64/ARM64 build matrix. CI run on
`51a3a52` confirmed it on Ubuntu LLVM 18.1.3.

### Release-packaging failure: `<strsafe.h>` before libc++ (2026-09-23)

CI `release-packaging` (Linux-hosted llvm-mingw/Zig, the path stable releases
use) failed compiling `installer_apply.cpp` while the `windows` job
(clang-cl/MSVC STL) and local default builds passed. `installer_transaction.cpp`
(added in `897945f`) included `<string>`/`<vector>`, but it is a shard included
mid-TU, after `installer_common.h` pulled in `<strsafe.h>`. strsafe #defines
`strcpy`, `sprintf`, `wcscpy`, ... to `*_instead_use_StringCch...` names, and
libc++'s `<cstring>`/`<cwchar>`/`<cstdio>` then fail on `using ::strcpy`.
Fix: `installer_common.h` includes the C++ standard headers ahead of
`<strsafe.h>`; installer sources must not include extension-less standard
headers themselves (gate in `tools/installer_build.py`, runs under `--test`;
mutation-verified). **Local reproduction:** the default `auto` toolchain hides
this — use `python build.py --target windows --toolchain llvm-mingw` for any
installer change that adds includes.

`--sanitizer --check` intentionally avoids regenerating `compile_commands.json`, so sanitizer verification does not dirty the tracked LSP database.

On Windows, `--test --asan` runs the test executable with `llvm-mingw/bin` prepended to `PATH`; the bundled `libclang_rt.asan_dynamic-x86_64.dll` is otherwise not discoverable by the Windows loader.

Note: The Linux target enables `-fexceptions` and `-frtti` to support STL `<string>` and `<vector>`, overriding the common `-fno-exceptions -fno-rtti`. This is a portability tradeoff in the native Linux client/daemon; future cleanup can disable exceptions/RTTI if STL usage is replaced with C-style arrays.

## Compiler flags (common)

- `-std=c++17`
- `-Oz` (size-optimized)
- `-DNDEBUG`, `-fno-exceptions`, `-fno-rtti`
- `-fstack-protector-strong` (canaries emitted via `ssp_glue.cpp`; works with llvm-mingw)
- `-ffunction-sections`, `-fdata-sections`, `-Wl,--gc-sections`
- `-Wall`, `-Wextra`, `-Wshadow`, `-Wformat=2`, `-Wnull-dereference`, `-Wundef`, `-Werror`
- `-D_FORTIFY_SOURCE=3`
- Version injected: `-DAPP_VERSION="X.XX"` from `VERSION` file
- Build number injected: `-DAPP_BUILD_NUMBER=N` from `BUILD_NUMBER`

## Windows-specific

- Compiler: host-native llvm-mingw (`llvm-mingw/bin/clang++.exe` on Windows,
  `llvm-mingw-linux/bin/x86_64-w64-mingw32-clang++` on Linux), both targeting
  `x86_64-w64-windows-gnu`
- Linker: `--subsystem,windows,--dynamicbase,--nxcompat,--high-entropy-va`
- LTO + ICF: `-flto -Wl,--icf=safe` reduces binary size via cross-module optimization and identical code folding
- `-Wl,-Xlink=-merge:.buildid=.rdata` (same `-Wl` argument as ICF): LLD's MinGW mode otherwise puts the CodeView/RSDS debug directory in a dedicated `.buildid` section. Merged, the PDB linkage is unchanged (verified with `llvm-symbolizer` against the private PDB) and the x64 images carry only MSVC-style section names; `pe_verify.verify_no_buildid_section()` gates x64. Zig ARM64 still emits `.buildid` (its DWARF `.debug` extraction path was not changed).
- Hardening flags: `-mguard=cf -fcf-protection=full -ftrivial-auto-var-init=pattern -fno-delete-null-pointer-checks`
  - CFG (`-mguard=cf`) enabled via `source/cfg_glue.cpp` which overrides the
    MinGW CRT's `__guard_check_icall_fptr` with a proper implementation that
    validates targets against all loaded modules (not just our GFIDS table).
    This prevents crashes on `GetProcAddress`-returned function pointers.
    - Uses `GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, ...)`
      module-range check (robust across CRT init, NvAPI dispatch, NVML exports).
    - ntdll!`RtlValidateUserCallTarget` (ordinal 1647) was also tested — it
      is the OS-level CFG bitmap validator and works correctly for exports,
      but fast-fails during MinGW CRT init when called from our override.
      Module-range check is the proven choice.
    - **Known warning**: `ld.lld: warning: duplicate symbol: __guard_check_icall_fptr`.
      This is cosmetic and unavoidable. The MinGW CRT's `loadcfg.o` always
      references `__guard_check_icall_fptr`, pulling in `mingw_cfguard_support.o`
      which defines it as a data pointer in `.00cfg`. Our override defines it as
      a function. LLD uses our definition (first on command line). The CRT's
      `.00cfg` data pointer is dead code eliminated by `--gc-sections` + LTO.
  - CET forward-edge (`-fcf-protection=full`) **is effective on our own code**;
    only the shadow-stack opt-in bit is missing. See
    "Windows x64 — CET: `endbr64` present, CET_COMPAT opt-in absent" below for
    the measured evidence. (This previously cited
    `audit/security-audit-report.md` (F-04-003); that file does not exist and
    `audit/` is gitignored, so the substance is inlined below instead of
    pointing at a dead anchor.)
- Static C++ runtime: `-static` (embeds libc++ statically, avoids `libc++.dll` dependency)
- GUI links: `-luser32 -lgdi32 -ladvapi32 -lshell32 -lole32 -lwtsapi32 -luuid`
- Service links: `-lgdi32 -ladvapi32 -lshell32 -lole32 -lwtsapi32 -luserenv -luuid` (no `-luser32`)
- Service compiled with `-DGREEN_CURVE_SERVICE_BINARY=1`
- CRT override files: `ssp_glue.cpp` (stack protector canaries) + `cfg_glue.cpp` (CFG indirect-call validation for external DLL exports)
- Runtime mitigation policy (`initialize_process_mitigations()`): System32-only
  runtime DLL search, dynamic-code prohibition, extension-point disablement, and
  strict handle checking. These do not constrain the separate installer/update
  child processes.
- Resource compilation: `llvm-rc.exe` for icon resources
- Icons are generated programmatically -- no binary art is tracked (`*.ico` is
  untracked, `*.rc` is gitignored, both are rebuilt from source).
  `tools/icon_render.py` owns the rendering (colour math, `ICON_STYLES`, PNG
  rasterization, ICO packaging); `build.py` keeps `ICON_OUTPUTS`,
  `ICON_RC_CONTENT`, and the staleness check. It moved out of `build.py` on
  2026-07-30 to stay under `BUILD_SCRIPT_SIZE_RATCHET`, with the same one-way
  dependency as the gate modules.
  - Five tray themes: `tray_default`, `tray_oc`, `tray_fan`, `tray_oc_fan`,
    `tray_pending` (resource IDs 111-115, GUI `.rc` only; the service `.rc`
    carries only icon 101 since 2026-09-23). `tray_pending` is **derived**, not
    authored: `_grayscale_style("tray_default")` applies Rec. 709 luminance to
    every colour entry and passes non-colour entries (badge shape, `None`)
    through untouched, so it is provably the same artwork with the hue removed.
  - `generate_icon()` treats **both** `build.py` and `tools/icon_render.py` as
    staleness inputs; an edit to the styles alone must still invalidate the
    `.ico` files.

## Linux-specific

- Target: `x86_64-linux-gnu` / `aarch64-linux-gnu` — **glibc-dynamic** (NOT static
  musl: the backend must `dlopen` the glibc NVIDIA driver libs, which static musl
  cannot do).
- Flags: `-fPIE -pie -Wl,-z,relro,-z,now -Wl,-z,noexecstack -fstack-protector-strong
  -ftrivial-auto-var-init=pattern -fexceptions -frtti -ldl -lpthread`, plus
  `-flto -fcf-protection=full` on x64 or
  `-fno-lto -mbranch-protection=standard -O2` on arm64. Debug info is split by
  `zig objcopy --strip-all --extract-to`; there is no bare `-s` link flag.
  - `-fcf-protection=full` is **x86-only**. `linux_flags_for_arch("arm64")`
    removes it rather than leaving it unused: clang hard-errors with
    `option 'cf-protection=return' cannot be specified on this target`.
  - The arm64 release goes through the object-first `_link_arm64_linux()` path,
    which bypasses `linux_flags_for_arch()` entirely, so
    `-ftrivial-auto-var-init=pattern` is repeated in its own object flag list.
    A flag added only to `LINUX_FLAGS` silently misses arm64.
- See "Linux — CET reaches our objects, the property note does not" below.
- Source files: the native backend (`linux_backend`, `linux_daemon`, `linux_gpu`,
  shared `gpu_core.h`/`vf_backends`/`platform`) plus the client
  (`linux_main`/`linux_port*`/`linux_tui`). See `linux-scaffold.md`.

## Output handling

- Builds to `.new` temp file, then atomically replaces the existing binary
- Previous binary backed up to `.bak`
- `--check` mode uses a temporary workspace under `build-tmp/` and cleans up after

## Version management

- Version read from `VERSION` file at repo root
- Injected into compile via `-DAPP_VERSION`
- Also injected into the installer's VERSIONINFO and manifest by
  `tools/installer_build.py`. The former `version.nsh`/`installer.nsi` NSIS pair
  was deleted when the custom installer replaced it — see [installer.md](installer.md).
- `BUILD_NUMBER` is gitignored local release state and auto-incremented by real
  builds when `build.py` detects code or build-tool input changes via
  `.build_fingerprint`; the fingerprint includes the `tools/*.py`/PowerShell
  scripts that generate resources, packages, and PE gates.
- `--check`, `--test`, `--lsp`, `--fuzz`, `--tidy`, and `--tidy-baseline` do
  not increment `BUILD_NUMBER`.
- GUI and service session markers log both version and build number

## Source of truth

- `build.py`: all build logic, source lists, flags, test/check/lsp modes, and output handling
- `tools/security_gates.py`: fuzzing (`--fuzz`) and CET verification
  (`--check-cet`), split out of `build.py` to stay under
  `BUILD_SCRIPT_SIZE_RATCHET`. Imports nothing from `build.py`; `build.py`
  passes its own module in as the path/helper context. See
  [testing.md](testing.md).
- `tools/static_analysis.py`: host selection, compile-database filtering,
  clang-tidy checks, and the hard baseline ratchet.
- `tools/ui_gates.py`, `tools/fan_gates.py`, `tools/linux_gates.py`: source-guard
  groups on the same one-way-dependency contract. `linux_gates.py` owns
  F-LNX-TUI plus the newer F-LNX-EXIT / F-LNX-TERM / F-LNX-DEBUGLOG /
  F-LNX-STARTUP / F-LNX-PKG groups.
- `tools/icon_render.py`: the deterministic icon renderer (see *Windows-specific*
  above). Same one-way-dependency contract; it imports only `math`, `struct`,
  and `zlib`.
- `tools/release_manifest.py`: the release archive allowlist, the archive root
  folder name, `purge_runtime_artifacts()`, `find_seven_zip()` /
  `report_packaging_skipped()` (the graceful no-archiver path), and `check_all()`
  — the packaging/archive source guards, which live beside the manifest they
  describe rather than in build.py. The allowlist is exact in both
  directions, so run-time output the binary writes beside itself (the debug log
  above all, which records GPU identifiers and applied settings) is **removed**
  from a payload before the manifest check rather than whitelisted — otherwise
  running the freshly built binary out of `dist/` would either fail the build or
  ship a developer's log.
- `VERSION` file: single source of truth for version number
- `BUILD_NUMBER` file: gitignored local source of truth for build number

## CI release gates

`.github/workflows/ci.yml` now treats static analysis, fuzzing, and packaging as
merge gates on both native hosts for `main` and pull requests; stable release
publication remains a separate manual workflow. The workflow itself is pinned to
`permissions: contents: read` and `PYTHONUNBUFFERED=1`. Build-script self-tests
validate step indentation so a malformed workflow cannot silently disable every
merge gate. Test and check-build invocations are separate steps: PowerShell can
otherwise report only the final native command's exit and hide an earlier
failure in the same multiline step.

- Windows: regression/UBSan/ASan, the clang-tidy ratchet, bounded fuzzing, and
  full verified x64+ARM64 Windows release archives/setup executables.
- Linux: x64+ARM64 check builds, regression/ASan, native clang-tidy, bounded
  fuzzing, and full verified x64+ARM64 Linux release archives. CI pins the
  analyzer to major version 18. That Linux CI
  runner deliberately has **no p7zip installed** — the `.tar.xz` needs none,
  so a reintroduced 7-Zip dependency fails packaging there instead of going
  unnoticed.

This closes the former gap where CI compiled but never exercised the exact
release-package paths.

`python build.py --test` already enables UBSan, so CI runs it once. The former
additional `--test --sanitizer` step was identical coverage, not a stronger
gate; retaining ASan separately is meaningful because it selects a different
runtime and catches a different class of defect.

The release metadata check in `tools/release_prep.py` validates that the newest
numbered `CHANGELOG.md` section matches `VERSION`, has the standard section
headings, and links from the immediately preceding release. It runs in the
build regression gates through `tools/release_manifest.py` and early in the
manual release workflow. It deliberately does not count words or contact the
remote; the workflow's separate tag check prevents republishing a version.

`.github/workflows/release.yml` is the manual stable-release workflow
(`workflow_dispatch` only). On one `ubuntu-latest` runner it:

- cross-compiles the published Windows x64 artifacts with pinned llvm-mingw
  and the Windows ARM64 artifacts with pinned Zig/LLD. Native Windows CI proves
  the preferred clang-cl/MSVC-ABI path separately, but those native-CI binaries
  are not uploaded as stable release assets; release notes must distinguish the
  two paths;

- verifies a clean checkout before and after the build, so the artifacts
  correspond 100% to the exact dispatch commit;
- reads the version from `VERSION` and pre-checks `git ls-remote --tags origin`
  so a rerun cannot silently overwrite an existing `<VERSION>` tag;
- checks the versioned release notes before fetching the toolchain;
- fetches the repository-pinned toolchain without a mutable Actions cache,
  records and attests that toolchain, sets
  `GREENCURVE_TOOLCHAIN_LOCAL_ONLY=1`, and runs bare `python build.py` — the
  full six-target matrix plus verified packages;
- fails if any of the six artifacts or their `.sha256` files is missing;
- attests every program package plus the toolchain record with the pinned
  `actions/attest-build-provenance` action (repo + commit binding), then
  verifies each one afterwards with
  `gh attestation verify <artifact> --repo <owner>/<repo> --source-digest
  <dispatch SHA> --signer-workflow <repo>/.github/workflows/release.yml`.
  `tools/release_manifest.py:check_all` source-gates both constraints;
- publishes a passing commit status check under context `Provenance & Attestation`
  to the released commit SHA via GitHub Statuses API, attaching the visible green
  checkmark circle (`✔`) to the release tag and commit;
- extracts the exact `## <VERSION>` section from `CHANGELOG.md` as the release
  page, failing if that reviewed section is missing or empty. User-visible
  changes, compatibility limits, downloads, and verification therefore lead
  the page instead of commit/tree hashes and internal updater mechanics;
- creates the stable tag `<VERSION>` (optionally SSH-signed if `TAG_SIGNING_KEY`
  secret is configured to display the "Verified" badge) and a non-draft,
  non-prerelease GitHub Release from the exact commit.

The tag convention is `<version>` with no prefix (e.g. `0.22.2`), matching the
public repository's existing tags (`0.22.1`, `0.22`, ...).

## Public repository

`aufkrawall/green-curve` (public, default branch `main`) is the development
repository. Everything ignored or untracked (logs, dumps, PDBs, toolchains,
release archives, baselines, `llm-wiki/log/`, `llm-wiki/private/`,
`update-procedure.md`) must never be committed; `tools/security_gates.py`
enforces the parts that can be checked mechanically. Force-pushing or deleting
refs on the public repo is never an option (it rewrites history and can disturb
forks); a bad publish is undone with `git revert`. Releases are published only
by the manual `release.yml` workflow. The operator-side publication and
backup/restore procedure is kept out of this page, in the gitignored
`update-procedure.md` and the maintainer's private notes.

## Known hardening limitations

Crash symbols are private build outputs, never release payloads, on **both**
platforms. Native Windows variants use separate symbol trees such as
`dist/symbols/windows-x64/msvc` and `dist/symbols/windows-x64/release`; Linux
and Linux-hosted release builds retain the existing unscoped paths. x64 links
use CodeView plus a matching PDB; the PE RSDS path is sanitized to the PDB
basename and the PDB is structurally checked. Zig's ARM64 PE linker does not
implement PDB output, so the release ARM64 variant uses DWARF and extracts a
verified matching `.debug` image before stripping the shipped executable. The
MSVC ARM64 variant emits and verifies its own PDB. These symbols are required
for useful dump analysis and remain gitignored.

Linux got the same treatment on 2026-07-31 (F-LNX-SYMBOLS). It previously
shipped a bare `-s` strip with **no extraction at all**, so a systemd-coredump
core had nothing anywhere to match it against. `LINUX_FLAGS` now carries
`-g -gdwarf-4` with the same prefix maps the Windows ARM64 path uses, plus
`-Wl,--build-id=sha1`; `crash_artifacts.extract_linux_symbols()` splits the
DWARF into `dist/symbols/linux-<arch>/greencurve.debug` with
`zig objcopy --strip-all --extract-to`, adds a `.gnu_debuglink`, and strips the
shipped binary. The build **fails** if the symbol file is missing, under 4 KiB,
or carries a build-id different from the binary's — a mismatched `.debug` is one
gdb and `coredumpctl` silently ignore, which is indistinguishable from having no
symbols. Extraction runs before `verify_release_binary()`, so the
private-workspace-path scan still applies to the exact bytes that ship. Note the
objcopy input and output paths must differ: it opens the output first and would
otherwise truncate its own input (`TRUNCATED_ELF`).

### Historical Windows x64 CET assessment (before 2026-10-01)

Corrected 2026-07-25. This section previously claimed `-fcf-protection=full` was
"compiled but never materializes in the binary". That is **false**: the
instrumentation is present on every one of our own indirect-call targets. Only
the *shadow-stack opt-in bit* is missing, and that bit is unrelated to
`endbr64`.

Separate the two halves of CET before reading the numbers:

| Half | Mechanism | Needs | Windows user-mode enforcement today |
|---|---|---|---|
| Forward edge (IBT) | `endbr64` at indirect-call targets | `-fcf-protection=full` (compiler) | **not enforced** |
| Backward edge (shadow stack) | Hardware return-address check | `CET_COMPAT` PE opt-in (linker) | enforced when opted in |

Measured on build 439 `greencurve.exe` (`.text` 439,808 bytes) by walking the
PE load-config Guard CF function table and testing each target's first four
bytes for `f3 0f 1e fa` (`build.py --check-cet` re-runs this):

- 312 CFG guard targets; 19 `endbr64` total in `.text`; 18 CFG targets start with `endbr64`.
- Attributing all 294 un-instrumented targets by symbol: **zero originate from Green Curve source.**
  - 251 libc++/libc++abi (mostly the bundled `itanium_demangle` vtable methods pulled in by `-static`)
  - 23 libunwind (`_ZN9libunwind*`, `unw_*`, `__gxx_personality_seh0`)
  - 20 MinGW CRT/startup (`*CRTStartup`, `_pei386_runtime_relocator`, `_assert`, …)
- All 18 instrumented targets *are* ours, and they are exactly the address-taken
  set: `WndProc`, `FanCurveDialogProc`, `LicenseDialogProc`,
  `AutoProfileDialogProc`, the subclass procs, `gui_mutation_worker_proc`,
  `logon_sync_thread_proc`, `service_fan_runtime_thread_proc`,
  `gui_selected_gpu_notification_callback`, `service_status_change_callback`,
  `ap_winevent_proc`, `green_curve_unhandled_exception_filter`,
  `__stack_chk_fail`, `__guard_check_icall_fptr`.

Root cause of the 294: the prebuilt llvm-mingw static libraries are compiled by
the toolchain vendor **without** `-fcf-protection`. `llvm-objdump -d` over
`libmingwex.a`, `libmingw32.a`, `libmsvcrt.a` and `libucrt.a` finds **zero**
`endbr64`. Their address-taken functions (C++ vtable slots above all) still land
in the linker-generated Guard CF table, which is built from relocations, so they
inflate the denominator without being ours to fix.

**LTO does not strip the instrumentation** (a previously suspected cause,
disproved). A probe with address-taken `static` callbacks shows LTO makes
placement *more precise*: without LTO a directly-called `extern "C"` function
still gets a defensive `endbr64`; with LTO whole-program info proves its address
is never taken and the redundant `endbr64` is dropped, while all three
genuinely address-taken statics keep theirs. Lower raw counts under LTO are
inlining plus this refinement, not loss of coverage.

What *is* absent, verified on the shipped binaries: the
`IMAGE_DEBUG_TYPE_EX_DLLCHARACTERISTICS` debug-directory entry (type 20) that
carries `IMAGE_DLLCHARACTERISTICS_EX_CET_COMPAT`, plus `GuardRFFailureRoutine`
and `GuardRFFailureRoutineFunctionPointer` (both `0x0`). MinGW-mode `ld.lld` has
no `/cetcompat` equivalent. Note the bit lives in the **debug directory**, not
the load config — an earlier probe that read it out of
`IMAGE_LOAD_CONFIG_DIRECTORY64` was reading `GuardLongJumpTargetCount` and
reported nonsense.

Runtime impact: **none today.** Windows does not enforce user-mode IBT, so the
`endbr64` instructions are inert NOPs either way, and without the CET_COMPAT bit
shadow stacks are not enabled. Contrary to an earlier note in this file's
history, setting CET_COMPAT would *not* fault on the un-instrumented targets —
that bit gates the backward edge only and never consults `endbr64`.

Fixing the remaining gap needs an MSVC-ABI toolchain (`clang-cl` + `lld-link`
with `/cetcompat`), which would also require the non-redistributable MSVC CRT
and Windows SDK and so breaks the hermetic self-downloading model `build.py` is
built on. ASLR, DEP, HEVA and CFG are all confirmed present and unaffected.
(As of 2026-09-23 native Windows builds both the MSVC-ABI and release variants;
the gap below applies to the `release` llvm-mingw/Zig variant, including every
Linux-host cross-build. See the "Windows toolchains" section.)

#### MSVC-ABI toolchain assessment (2026-08-28; adopted the same day for native Windows hosts)

Full empirical trial on the dev machine (VS 2026 Community 18.9, MSVC toolsets
14.44/14.51, SDK 10.0.26100, standalone LLVM 22.1.8). The whole Windows source
set compiles clean under clang-cl `-W4 -WX` (after `-D_CRT_SECURE_NO_WARNINGS`
and the same `-Wno-unused-*` as the MinGW build); GUI + service link with
`lld-link /guard:cf /cetcompat /opt:ref,icf` (static /MT CRT); the regression
suite and `--self-test` pass on real hardware (RTX 5070). Trial artifacts lived
in `temp/cfg_audit/` (untracked).

Verified gains over the MinGW build:

- **Real kernel-enforced CFG**: the MinGW `cfg_glue.cpp` shim is a weaker
  module-range check; the MSVC-ABI build gets the GFIDS bitmap validator.
  Negative control (call a non-GFIDS address in our own image) fast-fails with
  `0xC0000409`; all NVAPI/NVML `GetProcAddress` calls pass, including exports
  NOT in nvml.dll's GFIDS table (the OS tolerates non-enforcing modules and
  their exports — nvapi64.dll ships GuardFlags=0x100 with a **zero** GFIDS
  table, nvml.dll covers only 21% of exports).
- **Hardware shadow stacks**: `lld-link /cetcompat` sets the
  `EX_DLLCHARACTERISTICS` CET_COMPAT bit (debug directory, type 20). Verified
  via `GetProcessMitigationPolicy(ProcessUserShadowStackPolicy)`:
  Enable=1 Strict=1. The non-CET-COMPAT `nvapi64.dll` still loads fine
  (`BlockNonCetBinaries` policy bit defaults to 0). No setjmp/longjmp/fibers in
  the source, so SHSTK-safe. The MinGW `ld.lld` GNU mode has no such flag —
  this was the documented gap.
- `/GS` cookie lands in the PE load config (`SecurityCookie` nonzero, vs 0 on
  MinGW where `ssp_glue.cpp` provides `__stack_chk_guard`).

Verdict / constraints (why not adopted by default):

- Breaks the hermetic self-downloading toolchain: MSVC headers/libs and the
  Windows SDK cannot be mirrored to `Compilers-*` (license); only the LLVM
  part (clang-cl/lld-link) is pinnable.
- Breaks Linux-host Windows cross-builds: `release.yml` runs the full six-target
  matrix on one `ubuntu-latest` runner; MSVC-ABI Windows targets would need a
  `windows-latest` runner and a re-architected, two-runner release flow.
- Porting surface: `cfg_glue.cpp` splits (fatal-dump hook +
  `initialize_process_mitigations()` move out; the MinGW CFG shim dies);
  `--check-cet`'s `endbr64` gate semantics change (clang-cl emits no `endbr64`
  — guarded calls go through the check/dispatch pointer instead); installer,
  fuzz, ASan/tidy plumbing need the same treatment.
- Not available under clang-cl: XFG (`/guard:xfg`) and `/Qspectre` are cl.exe
  only; `-fstack-protector-strong` is silently ignored (fine: `/GS` covers it).
- Size/perf: `/O1` (clang-cl rejects `-Oz`) 884 KB vs 834 KB MinGW `-Oz`+LTO;
  microbenches a wash (±20% per loop, opposite directions, identical results).
- ARM64: clang-cl `aarch64-pc-windows-msvc` accepts `-mbranch-protection=standard`
  and emits PAC/BTI (27 hits in a trial TU), and lld-link would give ARM64 PDB
  support (replacing the DWARF/objcopy dance) — but the historical lld aarch64
  COFF misaligned-ldr/str bug was not re-tested end-to-end. Open question.

### Linux — CET reaches our objects, the property note does not

Measured 2026-07-28 on bare-metal Arch. Until that date the Linux build carried
**zero `endbr64`**: `-fcf-protection=full` lived in `WINDOWS_FLAGS` only, and
`--check-cet` parses PE load-config, so nothing looked at the ELF side. Adding
the flag took the shipped `dist/linux-x64` binary from 0 to 551 `endbr64`.

This matters more on Linux than the Windows case above. Windows does not enforce
user-mode IBT, so its `endbr64` are inert. Linux kernels do enforce shadow stack
on capable hardware — a Zen 3 development machine reports `user_shstk` in
`/proc/cpuinfo`. Zen 3 has **no IBT**, so on this CPU it is specifically the
backward edge that is enforceable.

| Level | State | Evidence |
|---|---|---|
| Our objects | IBT+SHSTK property on all 19, `endbr64` at indirect-branch targets | `--check-cet` |
| Linked image | `endbr64` present, **no `.note.gnu.property`** | `readelf -n` |
| Loader | Neither IBT nor shadow stack ever enabled | note absent |

Root cause of the missing note: `ld.lld` intersects
`GNU_PROPERTY_X86_FEATURE_1_AND` across all inputs, and the bundled Zig CRT
objects carry no property, so the intersection is empty. Confirmed directly —
a single one of our `.o` files carries `x86 feature: IBT, SHSTK`, and even a
trivial `int main(){}` linked through `zig cc` comes out with no note.

**Rejected approach:** forcing it. `zig cc` rejects both `-Wl,-z,shstk` and
`-Wl,-z,force-ibt` with `unsupported linker extension flag`, so the pinned
toolchain cannot emit the property regardless. Even if it could, promising
SHSTK on behalf of unmarked vendor CRT objects is a claim we cannot back. The
gate therefore **reports** the linked-image state rather than asserting it.
Closing this needs a Zig that marks its CRT, or a different libc path.

arm64 Linux is the same shape: 74 BTI + 256 PAC + 296 AUT instructions in the
shipped binary, no `.note.gnu.property`, so BTI enforcement never turns on.

`endbr64` is **not** required per-function. `-fcf-protection` emits it only at
functions reachable by indirect branch; a directly-called `static` correctly has
none (136 of our 439 functions). An early version of this gate asserted it
everywhere and failed on correct output. The per-object property bits are the
exact "was the flag applied to this TU" signal, so that is what is enforced.

### Windows ARM64 — no CFG metadata

The ARM64 build uses Zig's `aarch64-windows-gnu` target and does not support
Windows CFG metadata. It does emit ARMv8.3 branch protection; final PE code is
gated on BTI plus PAC/AUT instructions. Switching toolchains remains an option
if Windows CFG for GNU ARM64 becomes available.

### MinGW security cookie not in PE load config

The PE load-config `Security Cookie` field is zero because MinGW uses `__stack_chk_guard` (via `ssp_glue.cpp` and `-fstack-protector-strong`) rather than the MSVC `/GS` cookie mechanism. Stack smashing protection is active and verified — the strings `*** STACK SMASHING DETECTED: greencurve ***` are present in both binaries. The zero field is a MinGW ABI difference, not a security weakness.

## Last Verified

- 2026-10-05: Stable 0.28 release run `37358341063` built the full matrix
  on `ubuntu-latest` from commit `358793a` using the pinned hermetic release
  toolchains, verified the exact artifact set and clean tree, created tag and
  release `0.28`, and verified provenance for every program artifact plus the
  toolchain record (17 program assets; 21 after the four updater assets).
  CI run `37356433779` passed on main at `358793a` (secrets, Linux incl.
  ASan/tidy/fuzz, Windows dual-variant, release-packaging), and local
  `python build.py --test` was green before the push. Commit status check
  `Provenance & Attestation` posted `success`. Post-release commit `baad3e1`
  repinned the Arch recipes from the published0.28 bytes: `PKGBUILD` pins the
  tag archive (537 files compared byte-for-byte with `git archive` of
  `358793a`), `PKGBUILD.bin` pins both published Linux archives (matching the
  `.sha256` sidecars and GitHub's asset digests); `python build.py --gates`
  verified the recipes after the repin.
- 2026-09-27: Stable 0.27.0 release run `36274844164` built the full matrix
  on `ubuntu-latest` from commit `2acdcad` using the pinned hermetic release
  toolchains, verified the exact artifact set and clean tree, created tag and
  release `0.27.0`, and verified provenance for every program artifact plus the
  toolchain record. CI run `36272351981` passed on main at `2acdcad`. Commit
  status check `Provenance & Attestation` posted `success`. The published
  x64/ARM64 setup executables were downloaded back, signed into a floorless
  update manifest with the active key (`tools/update_signing.py prepare`), and
  uploaded as the two version-independent updater assets (19 assets total).
  Anonymous post-publication checks through `releases/latest/download` verified
  the ECDSA P-256 signature, exact byte identity, package hashes, and redirect
  hosts.
- 2026-09-26: 0.27.0 GitHub release rollout simulation and dry-run completed.
  Metadata validation, `release_prep.py`, and `release.yml` awk extraction passed.
  Packaging pipeline produced all 16 Windows variant artifacts (msvc and release)
  and all 8 Linux packages. The offline active signing key was verified against
  `GC_UPDATE_PUBLIC_KEY_ACTIVE`. Simulated `tools/update_signing.py prepare`
  and verified manifest against all public versions (0.23..0.26.0 -> AVAILABLE,
  0.27.0 -> UP_TO_DATE, 0.28.0 -> REJECTED). Added regression tests 5907..5922 in
  `tests/regression_main.cpp`. Full `python build.py`, `python build.py --test`,
  and `python build.py --tidy` passed.
- 2026-09-24: 0.27.0 metadata preflight passed its positive and negative
  cases through `python build.py --test`; the full Windows/Linux package build
  produced versioned 0.27.0 artifacts. No remote CI or live install was run.
- 2026-09-23: Native Windows default now builds isolated `msvc` and `release`
  variants for both architectures, packages each variant separately, and keeps
  Linux-hosted release package names unchanged. The variant self-tests,
  `python build.py --test`, `--test --asan`, `--tidy`, bounded fuzzing, native
  x64/ARM64 builds, packaging, and the full matrix passed; the native Windows
  CI step asserts all four archive/setup/checksum files for both variants.
  Exact Linux-host
  byte reproduction remains a Linux-host property; the native Windows release
  variant uses the matching pinned toolchain versions and flags.

- 2026-09-17: Stable 0.26.0 release run `35257005671` built the full matrix
  on `ubuntu-latest` from commit `344cf44` using the pinned hermetic release
  toolchains, verified the exact artifact set and clean tree, created tag and
  release `0.26.0`, and verified provenance for every program artifact plus the
  toolchain record. CI run `35256205838` passed native Windows, Linux, and the
  Linux-host Windows release-packaging path. The published x64/ARM64 setup
  executables were signed into a floorless update manifest with the active key
  and uploaded as the two version-independent updater assets (19 assets total).
  `python build.py --test` passed locally on the release commit, including the
  new 5200-5221 update-handoff assertions and the advanced 4311-4326 updater
  release fixture. The release workflow file was unchanged from 0.25.2.

- 2026-09-13: Stable 0.25.2 release run `34759132697` built the full matrix
  on `ubuntu-latest` from commit `24095e8` using the pinned hermetic release
  toolchains, verified the exact artifact set and clean tree, created tag and
  release `0.25.2`, and verified provenance for every program artifact plus the
  toolchain record. CI run `34758736305` passed native Windows, Linux, and the
  Linux-host Windows release-packaging path. The published x64/ARM64 setup
  executables were signed into a floorless update manifest with the active key
  and uploaded as the two version-independent updater assets (19 assets total).

- 2026-09-11: Stable 0.25.1 release run `34602102089` built the full matrix
  on `ubuntu-latest` from commit `77f3813` using the pinned hermetic release
  toolchains, verified the exact artifact set and clean tree, created tag and
  release `0.25.1`, and verified provenance for every program artifact plus the
  toolchain record. CI run `34601323219` passed native Windows, Linux, and the
  Linux-host Windows release-packaging path. The release workflow file was
  unchanged from the successful 0.25.0 run. The published x64/ARM64 setup
  executables were signed into a floorless update manifest with the active key
  and uploaded as the two version-independent updater assets (19 assets total).


- 2026-09-06: Stable 0.25.0 release run `34017597506` built the full matrix on
  `ubuntu-latest` from commit `bdcc91d` using the pinned hermetic release
  toolchains, verified the exact artifact set and clean tree, created the tag
  and release, and verified provenance for every program artifact plus the
  toolchain record. CI run `34017285953` independently passed native Windows,
  Linux, and the matching Linux-host Windows release-packaging path. The 0.25.0
  notes now explicitly distinguish published llvm-mingw/Zig artifacts from the
  native clang-cl/MSVC-ABI build path.

- 2026-08-28: **Adopted the MSVC-ABI (clang-cl + lld-link) toolchain as the
  default for the Windows targets on native Windows hosts** (`--toolchain auto`,
  loud fallback to llvm-mingw; see the "Windows toolchains" section).
  `process_hardening.cpp` split out of `cfg_glue.cpp`; `tools/msvc_toolchain.py`
  and `tools/pe_verify.py` added; installer stubs, gates, and PDB verification
  extended to both toolchains. The MinGW assessment section below remains the
  decision record. `python build.py --test`, full `python build.py` with
  packaging (six targets + two setup executables), `--check-cet`, bounded
  fuzzing, `--tidy`, the regression suite, and the x64 `--self-test` (FULL
  verdict under real CFG + /cetcompat) all passed. `--toolchain llvm-mingw --check`
  reproduced the legacy artifacts byte-for-byte.
- 2026-08-28: Empirical clang-cl/MSVC-ABI hardening assessment (see the
  "MSVC-ABI toolchain assessment" section above);
  not adopted. Along the way: fixed a real 16-vs-32 stack overflow in the
  `--self-test` ClkDomains survey (silent on MinGW, crash on MSVC-ABI) with a
  new source gate in `tools/security_gates.py`, and renamed a `small` local in
  `tests/regression_main.cpp` that collides with MSVC's `rpcndr.h` macro.
  `python build.py --test` and the full six-target `python build.py` with
  packaging passed.
- 2026-08-28: Fixed the native-Windows matrix build failure caused by a
  poisoned shared Zig global cache (all four Zig-driven links failed with
  `cannot open/could not open .../o/<hash>/compiler_rt*`). Added
  `tools/zig_cache.py`: cross-process link serialization plus poisoned-cache
  repair and compile-path diagnosis, with deterministic self-tests and source
  guards. The wrapper healed the real poisoned cache end-to-end; `python
  build.py --test` and the full six-target `python build.py` with packaging
  passed.
- 2026-08-22: Fixed CI's malformed Linux analysis-tools step, added explicit
  read-only workflow permissions, and added a workflow-structure self-test.
  Added process dynamic-code/extension-point mitigations with source gates.
  Windows x64 check, regression/ASan, toolchain verification, tidy ratchet, and
  bounded fuzzing passed.
- 2026-07-25 (build 439): Re-measured CET on a real Windows x64 build and
  **corrected this page** — `-fcf-protection=full` is effective on our own code
  (18/18 address-taken functions carry `endbr64`); the 294 uninstrumented Guard
  CF targets are all prebuilt llvm-mingw runtime (libc++/libc++abi, libunwind,
  MinGW CRT), whose archives contain zero `endbr64`. The prior "compiled but
  never materializes" claim was false, and an LTO-strips-it hypothesis was
  tested and disproved. `python build.py --check-cet` now re-runs this
  measurement against a purpose-built unstripped probe and fails if the flag
  ever stops applying (negative control performed). Added `--fuzz`; see
  [testing.md](testing.md).
- 2026-07-15: Presentation-silent tray/automatic profile completion passed
  `python build.py --test`, `python build.py --test --asan`, the Windows
  x64/ARM64 warning-as-error `--check`, and full `python build.py` at build 430.
  The final build inspected Windows GUI/service and Linux binaries, retained
  private Windows symbols, and produced all four exact-manifest 0.20 archives.
- 2026-07-15: The tray-hidden visibility-postcondition fix passed
  `python build.py --test`, `python build.py --test --asan`, the Windows
  x64/ARM64 warning-as-error `--check`, and full `python build.py` at build 429.
  The final build inspected Windows GUI/service and Linux binaries, retained
  private Windows symbols, and produced all four exact-manifest 0.20 archives.
- 2026-07-15: Broad pre-ship source-review fixes passed `python build.py --test`,
  `python build.py --test --asan`, the complete Windows/Linux x64/ARM64
  `--check` matrix, and full `python build.py` at build 427. The final build
  inspected Windows GUI/service and Linux binaries, retained private Windows
  symbols, and produced all four exact-manifest 0.20 archives.
- 2026-07-15: Targeted release-review fixes passed the full `python build.py`
  matrix at build 425: Windows GUI/service and Linux binaries for x64/ARM64,
  private-symbol inspection, and all four exact-manifest 0.20 archives. The
  pure suite, ASan+UBSan suite, and separate Linux x64/ARM64 checks also passed.
- 2026-07-15: The tray-residency/no-intent recovery hardening passed the full
  `python build.py` matrix at build 424: Windows GUI/service and Linux binaries
  for x64/ARM64, private-symbol inspection, and all four 0.20 archives.
- 2026-07-15: Release 0.20 full `python build.py` passed at build 422. It
  compiled and inspected Windows GUI/service plus Linux binaries for x64 and
  ARM64, retained private Windows symbols, and produced the four exact-manifest
  `greencurve-0.20-<os>-<arch>.7z` archives. The expanded pure/source suite and
  separate Linux x64/ARM64 check builds also passed.
- 2026-07-11: Release 0.19 made `VERSION` mandatory, made `--check` honor every
  requested architecture, changed ARM64 to object-first no-LTO code generation,
  added final PE/ELF hardening and BTI/PAC gates, exact archive manifests, and a
  duplicate-symbol diagnostic allowlist. Windows/Linux x64 and ARM64 checks,
  x64 UBSan checks, and the final four-archive build passed at build 387.

- 2026-05-20: Build 141 verified with `python build.py`, `python build.py --check`, `python build.py --target linux --check`, `python build.py --test`, and `python build.py --test --asan`. Binary inspection confirmed `DYNAMIC_BASE`, `NX_COMPAT`, `HIGH_ENTROPY_VA`, and `GUARD_CF` on both Windows executables.
- 2026-06-21: Release metadata is aligned to `0.16`; `python build.py` packages archives as `greencurve-0.16-<os>-<arch>.7z`. Full release-matrix verification for this change was recorded in the local-only log.
- 2026-06-28: Compiler downloads redirected to GitHub Release `Compilers-1.0`. Local `compilers/` vendor fallback added. SHA-256 cross-verified against upstream. `python build.py --check --target windows` passed.
- 2026-06-28: Toolchain cache trust changed from adjacent sentinel files to pinned executable digests in `build.py`; security hardening pass verified by `python build.py --test`, `python build.py --test --asan`, `python build.py --target windows --check`, `python build.py --target linux --check`, and full `python build.py` (build 317).
- 2026-06-28: `finalize_output()` tolerates llvm-mingw aarch64 writing the final `.exe` path directly instead of the requested `.exe.new`, but only when the output mtime proves it came from the compile that just succeeded.
- 2026-06-28: Windows arm64 GUI linker workaround adjusted to `gui_arch_opt = ["-O1", "-fno-lto"]` after the same llvm-mingw/LLD `misaligned ldr/str offset` bug reappeared with further unity-source growth. Full `python build.py` passed with build 321.
- 2026-06-29: Windows arm64 service linker workaround adjusted to `service_arch_opt = ["-O1", "-fno-lto"]` after the same llvm-mingw/LLD `misaligned ldr/str offset` bug reappeared with service unity-source growth. Full `python build.py` passed with build 323.
- 2026-06-29: Windows arm64 GUI linker workaround additionally filters `-ffunction-sections`, `-fdata-sections`, and `-Wl,--gc-sections` after build 327 source growth re-hit `misaligned ldr/str offset`.
- 2026-06-29: Release metadata aligned to `0.17`; full `python build.py` passed with build 330 and packaged `greencurve-0.17-<os>-<arch>.7z` archives for Windows/Linux x64+arm64.
