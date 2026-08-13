# Compilers — Green Curve Build Toolchain

This folder is the record of the exact toolchain `build.py` is allowed to
build with: upstream sources, integrity checksums, access timestamps, licences,
and the digest of every binary the build actually executes.

Nothing here is advisory. `build.py` refuses to run a compiler, linker or
archiver whose SHA-256 does not match what these manifests record.

## Contents

| Directory | Component | Version | Hosts |
|-----------|-----------|---------|-------|
| `zig-0.13.0/` | Zig compiler | 0.13.0 | Windows x86_64 + Linux x86_64 |
| `llvm-mingw-20260519/` | llvm-mingw (MinGW toolchain) | 20260519 (LLVM 22.1.6) | Windows x86_64 + Linux x86_64 |
| `7zip-26.02/` | 7-Zip archiver | 26.02 | Windows x86_64 + Linux x86_64 |

## Why these versions

Both compilers sit at a **compatibility ceiling**, not at whatever upstream
released most recently. Newer versions were tried and rejected with evidence,
recorded here so the question does not have to be re-litigated from scratch.

**Zig 0.13.0.** Zig compiles the Windows ARM64 target (llvm-mingw's aarch64
linker hits a misaligned `ldr`/`str` bug), so its clang sees the Windows
sources. From **0.14.1** on, that clang reports
`-Wcast-function-type-mismatch` for the ~20 `GetProcAddress` →
typed-function-pointer casts across `ui_main_layout.cpp`,
`main_service_recovery_clock.cpp`, `nvapi_loader.cpp` and others, and `-Werror`
turns every one into a build failure. From **0.15.1** on there is a second,
independent blocker: `zig objcopy` answers `error: unimplemented` for every ELF
operation — `--extract-to`, `--only-keep-debug` and `--add-gnu-debuglink` alike
— which breaks Linux private-symbol extraction in `tools/crash_artifacts.py`.
That code cannot switch to `llvm-objcopy` without making the Linux build depend
on the Windows toolchain, which it deliberately does not.

**llvm-mingw 20260519.** 20260616 and later carry the same
`-Wcast-function-type-mismatch` behaviour for the x64 Windows build, and their
clang-tidy raises one additional `clang-analyzer-security.ArrayBound` finding in
`source/linux_terminal_launch.cpp`. That finding is a false positive —
`readlink` is bounded by `sizeof(selfPath) - 1`, so the index is always in
range — but it would still need a baseline entry.

**To move either forward**, fix the `GetProcAddress` call sites first (each
needs an intermediate `void*` cast, or a single helper that does the cast once)
rather than suppressing the diagnostic across the whole build. Zig can then go
to 0.14.1; going beyond that also needs the ELF objcopy path rewritten.

**7-Zip is part of the toolchain.** It writes the Windows `.7z` release
archives, so it decides the bytes of a published artifact just as much as the
compiler does. It is pinned and vendored rather than installed from a
distribution package at whatever version is current that week.

## Where the archives live

The archives themselves are **not** in git — one is 187 MB and GitHub rejects
any file over 100 MB. They are published as assets on this repository's own
[`Compilers-1.0`](https://github.com/aufkrawall/green-curve/releases/tag/Compilers-1.0)
release, and every `url` in a manifest points there rather than at the upstream
project. A release attested to this repository should not depend on a third
party still serving a byte-identical file.

Each manifest keeps the `upstream_url` it was originally mirrored from, so the
provenance stays checkable.

To vendor them locally:

    python build.py --fetch-toolchain

That downloads each pinned archive into the matching directory here and
verifies it against the recorded SHA-256. `.gitignore` keeps the archives
themselves untracked, so a populated `compilers/` tree still reports clean.

## How the pins are enforced

- **Archive digest** — verified on download *and* on every reuse of a vendored
  copy. This is the complete guarantee: it covers every file inside.
- **Extracted binaries** — `extracted_binaries` records a digest for each
  binary in `bin/`. This is what guards a restored CI cache, which is far
  easier to tamper with than a pinned archive. Verifying only the driver would
  not be enough: llvm-mingw's Linux `x86_64-w64-mingw32-clang++` is a symlink to
  a 3 KB wrapper script that is byte-identical across upstream releases, so that
  digest alone cannot even tell one llvm-mingw from another.
- **`GREENCURVE_TOOLCHAIN_LOCAL_ONLY=1`** — makes any toolchain fetch a hard
  error instead of a silent fallback. The release workflow sets it, so an
  attested build can only use what was vendored here first.
- **`build.py --verify-toolchain`** — re-checks everything on demand and prints
  what is installed. `--toolchain-manifest PATH` writes that as JSON; the
  release workflow attests that file alongside the binaries, so a consumer can
  verify not only what was built but what built it.

## Verification

Each archive's SHA-256 was verified at download time. For Zig that value is the
one published by ziglang.org in `download/index.json`; llvm-mingw and 7-Zip
publish no checksum file, so those digests were computed on first download and
pinned from then on. Each manifest records which case applies in
`checksum_source`.

## License

Each directory includes the original license file from the respective upstream
project:

- **Zig** — MIT License (`zig-0.13.0/LICENSE`)
- **llvm-mingw** — Apache 2.0 with LLVM Exceptions (`llvm-mingw-20260519/LICENSE.TXT`)
- **7-Zip** — GNU LGPL v2.1+ / BSD 3-clause (`7zip-26.02/License.txt`)
