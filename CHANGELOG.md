# Changelog

## 0.25.1

Green Curve 0.25.1 is a bug-fix release for power-limit handling and Linux
memory-clock-offset profiles after 0.25.0.

### Fixes

- **An unreadable power target is no longer treated as 0%.** Boards whose
  driver answers the power-limit constraints but refuses to report the limit
  itself (reported on an RTX 3060 Laptop GPU) could capture `0`, turn it into a
  silent request to halve the power limit, and then fail every Apply with
  "Power target did not reset". The power surface is now reported as partial,
  the untouched field stays neutral, and Apply, reset and rollback require a
  complete current/default readback pair on both Windows and Linux. A
  non-default power request on such a board still fails loudly and by name.
- **Linux memory-clock offsets now match Windows effective/display
  semantics.** Linux NVML offsets are converted at every boundary and capped at
  +/-3000 display MHz. Profiles and daemon records written before the change
  are converted once from stored effective units to display units, with
  checksum validation before mutation, so a pre-fix +2500 effective is applied
  as the same effective offset instead of being doubled to +5000.
- **Linux profile migration hardened.** Migration loads are read-only, and the
  migration marker parser rejects malformed records instead of guessing.

### Compatibility notes

- Existing profiles remain compatible. Linux profiles written before 0.25.1
  are migrated once on load; Windows profile handling is unchanged.
- The unreadable-power-target fix was driven by a reported 0.25.0 debug log and
  has regression and source-gate coverage, but no live laptop-GPU pass yet.
- No service-protocol or updater-path change since 0.25.0.

### Downloads and verification

- **Windows:** use the `setup.exe` for a normal install or upgrade; use the
  `.7z` archive for a portable copy.
- **Linux:** use the ready-to-install Arch Linux `.pkg.tar.zst` on pacman-based
  systems, or extract the `.tar.xz` archive and run `greencurve-setup.sh`.
- x64 and ARM64 packages are attached below. ARM64 remains compile- and
  binary-inspection-only; Windows x64 and Linux x64 are the hardware-tested
  targets.
- Every program package has a matching SHA-256 file and a GitHub
  build-provenance attestation. Verify an artifact with:

  ```bash
  gh attestation verify <artifact> --repo aufkrawall/green-curve
  ```

**Full changelog:** [0.25.0...0.25.1](https://github.com/aufkrawall/green-curve/compare/0.25.0...0.25.1)

## 0.25.0

Green Curve 0.25.0 strengthens release packaging, Windows hardening, update
security, and mixed-apply rollback behavior while extending Blackwell telemetry
and shipping ready-to-install Arch Linux packages.

### Highlights

- **Ready-to-install Arch Linux packages.** Normal builds now produce native
  pacman packages for x86_64 and aarch64 alongside the existing Linux tarballs.
  The package provisions the service/group, desktop integration and icons, and
  its generated metadata is validated against pacman's accepted `.PKGINFO`
  keys before it can ship.
- **Blackwell MSVDD writes and telemetry corrected.** The XBAR MSVDD field is
  now resolved from its actual domain entry rather than the XBAR frequency
  entry, fixing rejected voltage-offset writes on Blackwell. Active XBAR
  voltage plus the applied offset are published on Windows and Linux; the
  service protocol is now v25. The corrected write/readback path was validated
  live on an RTX 5070 and restored to its original state afterwards.
- **Transactional apply rollback is stricter.** Core clock/power, advanced
  clock and fan failures now follow an explicit mixed-apply rollback contract,
  with transaction boundaries and partial-apply reporting kept consistent so a
  rolled-back request cannot be republished as live state.
- **Windows builds are hardened on both supported toolchain paths.** Stable
  GitHub release binaries are cross-compiled on Linux with pinned llvm-mingw
  for x64 and Zig/LLD for ARM64, with verified CFG/CET and PAC/BTI metadata.
  Native Windows builds additionally prefer verified clang-cl/lld-link and the
  MSVC ABI, while retaining a loud, verified llvm-mingw fallback.
- **Updater and service trust paths are harder to subvert.** The update path now
  fails closed when transport security policy cannot be applied, strengthens
  atomic-write and directory-handle pinning against reparse/TOCTOU attacks,
  tightens signature encoding/verification, and fixes named-pipe lifetime and
  cancellation races.
- **Build and diagnostic reliability improved.** Shared Zig-cache poisoning can
  now be detected and repaired safely, Windows pipe/signer regressions are
  covered, invalid clock-probe selectors are rejected, and several static
  analysis findings and self-test defects were fixed.

### Compatibility notes

- **Windows GUI/service protocol is now v25.** Keep the GUI and service from the
  same release together; mixed-version peers intentionally reject one another.
- The direct Arch packages are new in this release. ARM64 remains a compile- and
  binary-inspection-only target; Windows x64 and Linux x64 are the hardware-
  tested targets.
- The Blackwell MSVDD correction changes where the private driver control field
  is written. The corrected path has live RTX 5070 validation with exact
  readback.

### Downloads and verification

- **Windows:** use the `setup.exe` for a normal install or upgrade; use the
  `.7z` archive for a portable copy.
- **Linux:** use the ready-to-install Arch Linux `.pkg.tar.zst` on pacman-based
  systems, or extract the `.tar.xz` archive and run `greencurve-setup.sh`.
- x64 and ARM64 packages are attached below. ARM64 remains compile- and
  binary-inspection-only.
- Every program package has a matching SHA-256 file and a GitHub
  build-provenance attestation. Verify an artifact with:

  ```bash
  gh attestation verify <artifact> --repo aufkrawall/green-curve
  ```

**Full changelog:** [0.24.0...0.25.0](https://github.com/aufkrawall/green-curve/compare/0.24.0...0.25.0)

## 0.24.0

Green Curve 0.24.0 brings advanced auxiliary-clock tuning, native zero-RPM
custom fan curves, and a more resilient Windows service.

### Highlights

- **Advanced clocks on Windows and Linux.** The new Advanced Clocks dialog on
  Windows and Advanced tab on Linux can tune XBAR, SYS, and VIDEO clock offsets
  plus XBAR MSVDD voltage, with live readback and full profile support. Controls
  are available only when the installed NVIDIA driver exposes a validated
  interface.
- **Native zero-RPM fan curves.** A custom curve can now hand fan control back
  to NVIDIA below your chosen temperature so supported cards can stop their
  fans at idle, then resume the custom curve above it. The fan start/stop gap
  and ordinary curve-downshift hysteresis are independently adjustable.
- **More resilient Windows service communication.** The service can now handle
  several local requests independently, so one stalled client no longer blocks
  GPU controls. Reconnects and user-session transitions are also more reliable.
- **Safer, bounded diagnostics.** Debug logs are capped automatically, and
  account, session, and path details are recorded as opaque fingerprints rather
  than plain text.
- **Profile, Linux, and update-path fixes.** Advanced-clock settings now
  round-trip through profiles on both platforms, reset and restore paths are more
  consistent, and updater failure recovery is more robust.

### Compatibility notes

- Existing profiles remain compatible. Native zero-RPM mode is opt-in.
- Advanced clocks have been validated on a Windows RTX 5070. Linux
  advanced-clock writes have automated test and build coverage but have not had a
  live-hardware pass.
- Native zero-RPM uses the GPU firmware's automatic fan policy. Whether the
  fans physically stop is board- and firmware-dependent, and the new mode has
  not yet had a live-board validation.

### Downloads and verification

- **Windows:** use the `setup.exe` for a normal install or upgrade; use the
  `.7z` archive for a portable copy.
- **Linux:** extract the `.tar.xz` archive and run the included
  `greencurve-setup.sh`.
- x64 and ARM64 packages are attached below. ARM64 remains compile- and
  binary-inspection-only; Windows x64 and Linux x64 are the hardware-tested
  targets.
- Every program package has a matching SHA-256 file and a GitHub
  build-provenance attestation. Verify an artifact with:

  ```bash
  gh attestation verify <artifact> --repo aufkrawall/green-curve
  ```

**Full changelog:** [0.23.1...0.24.0](https://github.com/aufkrawall/green-curve/compare/0.23.1...0.24.0)
