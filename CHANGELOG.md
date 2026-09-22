<!--
  HOW TO WRITE A SECTION HERE.  These notes become the GitHub release page
  verbatim: release.yml extracts the "## <VERSION>" section with awk.

  Write to be SCANNED.  Reading only the bold lead of each bullet must tell you
  what changed.

  - One bullet per user-visible change; one line where possible, never > two.
  - The bold lead is a complete sentence and stands alone.
  - Under ~400 words per release.  No nested bullets, no paragraphs of prose.
  - Same four sections every time: intro, "### Highlights" OR
    "### Fixes & Hardening", "### Compatibility notes",
    "### Downloads and verification", then the "**Full changelog:**" compare
    link.  Do not invent per-release subheads.
  - Say what changed FOR THE USER, not how the code changed.  No internal
    symbol/file names, no F-XXX codes, no story of how a bug was found -- that
    belongs in the commit message.
  - Copy the last release's "Compatibility notes" and "Downloads and
    verification" and adjust; update both versions in the compare link.

  0.24.0-0.25.2 are the reference shape.  0.26.0 was first written at 2824
  words against a ~400 word budget, which is why this note exists.
-->

# Changelog

## Unreleased

Setup now installs to any folder you choose instead of only one directly under
Program Files.

### Highlights

- **Setup now accepts any install folder, on any drive.** It shows how well
  that folder is protected before installing anything.
- **A new folder under a drive root no longer raises a false warning.** Setup
  protects such a folder as it creates it, and now says so.
- **Choosing a folder other accounts can change is an explicit decision.** Such
  a folder can be used to take over the background service; the warning says so
  and shows the admin command that fully protects the folder (it is never
  applied for you), ready to paste.
- **Installing the background service from a portable copy now asks first**
  when the folder it runs from is not protected, instead of installing quietly.
- **Portable copies keep working from every folder.** They show the same clear
  protection warnings instead of silence.
- **A failed service installation now tells you what went wrong and what to do
  about it.** It used to report only "exit code 1", with the real reason written
  to a log file you had no way to find.
- **Green Curve now needs a folder of its own.** Downloads, the desktop and drive roots are refused even when reached through another path.
- **Moving a setup installation now removes its old folder when empty.** Other files keep the folder in place.
- **The service is no longer reported as failed when it is merely slow to
  start.** Antivirus scanning the new files on first run was enough to trigger
  that.
- **Declining the Windows elevation prompt is no longer reported as an error.**
- **The service now reports its start-up progress to Windows**, so a slow start
  is no longer indistinguishable from a hung one.

### Compatibility notes

- Existing installations keep updating in place; nothing moves.
- Unattended updates never show prompts, wherever Green Curve is installed.
- Drives without file permissions (FAT/exFAT) and network folders are usable
  with a clear warning that files there cannot be protected.
- An existing installation in a folder Green Curve no longer accepts keeps
  working; the restriction applies when you install or move it.
- Installing from the archive requires an elevated PowerShell or Command
  Prompt. This was always true and is now stated, and refused clearly.

## 0.26.0

A reliability release: hardening around Apply, plus fixes from a full code audit.

**Updating is recommended.** These changes proactively harden against edge cases
identified during the audit — a failed Apply, the background service restarting
mid-write, or a card left on settings neither profile asked for.

### Fixes & Hardening

- **Profile switches under 3D load no longer fail or take the driver down.**
- **A profile switch can no longer briefly run the card above both profiles.**
  It stays capped at the lower of the two limits throughout.
- **A failed Apply no longer reports success, and now rolls back.**
- **Verification no longer passes a curve it never checked.**
- **Reset to stock no longer hides failures, or clears XBAR and MSVDD offsets it
  does not own.**
- **Hand-edited curve points keep their meaning when you save and reload.**
- **Linux: upgrading no longer makes the daemon forget its settings and start-up
  policy.**
- **Windows: CLI commands now print to your terminal.** Every one of them,
  `--help` included, previously printed nothing at all.
- **The debug log no longer contains your Windows account name.**
- **Machine-wide update settings now require an administrator.**
- **The high-overclock warning now covers XBAR voltage**, at +25 mV and up.
- **Smaller:** uniform fan-curve validation; Linux `--probe` now agrees with the
  daemon; generated Linux `.desktop` and systemd files handle unusual paths;
  mistyped commands report an error instead of failing silently.

### Compatibility notes

- Existing configuration and profiles remain fully compatible.
- A profile written by 0.26.0 and opened by an older version reads as plain
  frequencies — what that version would have done with it anyway.
- App and background service move to protocol v27. They update together, so this
  needs nothing from you.

### Downloads and verification

- **Windows:** use the `setup.exe` for a normal install or upgrade; use the
  `.7z` archive for a portable copy.
- **Linux:** use the ready-to-install Arch Linux `.pkg.tar.zst` on pacman-based
  systems, or extract the `.tar.xz` archive and run `greencurve-setup.sh`.
- Every program package has a matching SHA-256 file and a GitHub
  build-provenance attestation. Verify an artifact with:

  ```bash
  gh attestation verify <artifact> --repo aufkrawall/green-curve
  ```

**Full changelog:** [0.25.2...0.26.0](https://github.com/aufkrawall/green-curve/compare/0.25.2...0.26.0)

## 0.25.2

Green Curve 0.25.2 is a reliability and hardening release, featuring
a transition clock ceiling during Apply to prevent profile-switch driver
resets under load, asynchronous log routing across Windows user sessions,
full Linux TUI support for surfaceless/laptop GPU reset, role-derived transport
deadlines on both platforms, and build pipeline enhancements.

### Fixes & Hardening

- **Transition clock ceiling during Apply (F-APPLY-CEILING).** An Apply operation
  raising the VF curve now arms the request's own lock target as a transition
  clock clamp before the first clock-affecting write and reset-to-stock baseline,
  holding the GPU capped across intermediate curve writes until the final lock
  step completes. This eliminates transient high-clock spikes and driver TDRs
  when switching to higher or pinned profiles under 3D load. Also adds an
  apply-clock witness sampling live GPU clock and load across transitions.
- **Cross-session log isolation (F-LOG-ASYNC).** Debug log records are now
  bound to their destination route generation upon enqueue. Session transitions
  advance the route generation and update the target path under thread-safe
  synchronization, preventing records queued during high I/O latency from
  spilling into a different user's profile. File path mutation and log draining
  are decoupled, eliminating data races on session switches.
- **Linux TUI Reset on surfaceless/laptop and non-Blackwell GPUs (F-POWER-RESET).**
  Aligned the terminal UI's preflight check with backend mutation policy: Reset
  now requires only core mutation domains (clocks, VF curve, locks, and fan).
  Power and advanced clock domains (XBAR, SYS_CLK, VIDEO_CLK) are optional,
  enabling Reset on mobile GPUs lacking power surfaces and discrete GPUs without
  auxiliary clock domains.
- **Linux Unix-socket request deadlines (F-DAEMON-DEADLINE).** Replaced
  monolithic socket timeouts with role-derived budgets under compile-time static
  assertions. Operation-result recovery queries now carry mutation-class
  budgets, eliminating false-positive "Apply failed" reports on lengthy writes.
- **Windows named-pipe request deadlines (F-PIPE-DEADLINE).** Client deadlines
  are derived from the background service's serialized dispatch, lock acquisition,
  and hardware refresh bounds.
- **Stale-read preservation (F-READ-MISS).** A missed state read from a busy
  service no longer tears down GUI presentation or treats the service as disconnected.
- **In-flight Apply button greying (F-INFLIGHT-APPLY).** The Apply button is
  greyed while hardware writes run, preventing duplicate submissions, while
  Reset remains immediately accessible as an escape hatch.
- **Updates dialog offloading.** Update commands run on detached worker threads
  with main-window message posting, preventing message-loop hangs.
- **Parallel multi-architecture build PDB collision fix.** Architecture-scoped
  scratch PDB naming in `build.py` eliminates parallel link collisions between
  x64 and ARM64 MSVC toolchain jobs.
- **Packaging and documentation templates.** Synchronized Arch Linux `PKGBUILD`,
  `PKGBUILD.bin`, and `README.md` documentation to current release versions.

### Compatibility notes

- Existing configuration and profiles remain fully compatible.
- Both Windows and Linux service transports remain on protocol v25.

### Downloads and verification

- **Windows:** use the `setup.exe` for a normal install or upgrade; use the
  `.7z` archive for a portable copy.
- **Linux:** use the ready-to-install Arch Linux `.pkg.tar.zst` on pacman-based
  systems, or extract the `.tar.xz` archive and run `greencurve-setup.sh`.
- Every program package has a matching SHA-256 file and a GitHub
  build-provenance attestation. Verify an artifact with:

  ```bash
  gh attestation verify <artifact> --repo aufkrawall/green-curve
  ```

**Full changelog:** [0.25.1...0.25.2](https://github.com/aufkrawall/green-curve/compare/0.25.1...0.25.2)

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
