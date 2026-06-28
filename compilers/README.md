# Compilers — Green Curve Build Toolchain

This folder contains the exact pre-built compiler toolchain archives used
by `build.py`, their integrity checksums, upstream sources, access
timestamps, and extracted binary fingerprints.

## Contents

| Directory | Component | Version | Host |
|-----------|-----------|---------|------|
| `zig-0.13.0/` | Zig compiler | 0.13.0 | Windows x86_64 + Linux x86_64 |
| `llvm-mingw-20260519/` | llvm-mingw (MinGW toolchain) | 20260519 (LLVM 22.1.6) | Windows x86_64 |

## License

Each subdirectory includes the original license file from the respective
upstream project:

- **Zig** — MIT License (`zig-0.13.0/LICENSE`)
- **llvm-mingw** — Apache 2.0 with LLVM Exceptions (`llvm-mingw-20260519/LICENSE.TXT`)

## Downloads

Archives were downloaded from the official upstream sources on
**2026-06-28** and verified by SHA-256 against the values published by the
upstream projects. See each `manifest.json` for full details.

## Verification

Each archive's SHA-256 was verified at download time and confirmed to
match the upstream-published checksum.  After extraction, integrity
sentinels were written for key binaries and later re-verified — see the
individual `manifest.json` files.
