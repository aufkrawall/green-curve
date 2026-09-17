# Changelog

## 0.26.0

Two passes over the code: a targeted hardening of what happens to your clocks
while a profile is being applied, and a code audit over the whole repository.
The clock work is the substantial one — it closes several ways an Apply could
briefly run the card above what either the old or the new profile allows, and
several ways a failed Apply could report success or leave the card uncapped.

**Updating is recommended.** Most of what is below is hardening around the
moment a profile is applied, which is where the failures people actually hit
live: an Apply ending in an error, the background service restarting itself
mid-write and switching automatic restore off, or a card left holding settings
neither profile asked for. None of it is urgent if 0.25.2 has been behaving
itself for you — it is simply a sturdier build to be on, and the fixes are hard
to get any other way.

The app and its background service now speak a newer internal protocol (v27),
because curve points had to start carrying one more piece of information. They
are installed and updated together, so this needs nothing from you; a
half-updated pair refuses to talk rather than guessing, as before.

### Clock and profile-switching safety

- **Switching profiles can no longer briefly run the card above both profiles.**
  The transition clamp introduced in 0.25.2 armed only when the profile you were
  switching *to* pinned a clock. But the risk comes from what you are leaving: an
  Apply first resets to stock, and for the window between that reset and the new
  curve being written, a card that was held down by a flatten floor, a negative
  offset or an old pin runs at *stock* — above both profiles. Switching between
  two unpinned undervolts got no protection at all. Protection is now armed
  whenever an Apply could put the card above what it is already entitled to run,
  and the ceiling is the lower of the outgoing and incoming limits, so a
  low-pin → high-pin switch keeps the old pin holding until the new curve
  verifies.
- **If that protection cannot be installed, the Apply is refused before anything
  is written**, naming the ceiling it wanted and the capability it is missing,
  instead of walking on unprotected. Unsupported and unprobeable GPUs keep their
  full read and write surface as before — only the one transition that cannot be
  made safe is refused.
- **With one exception, for cards that have never had the clock control this
  uses.** The refusal above treated “the driver declined this clamp” and “this
  card has no such clamp at all” as the same answer. The clamp is a GPU-locked-
  clock control that exists on Turing and newer; on Pascal the driver answers
  *not supported* to every form of it. Since the protection is wanted for any
  Apply that resets to stock while your current settings hold the clocks down —
  which is what an undervolt is — that turned every profile switch on a Pascal
  card into a refusal, for a cap that card never had. A card that genuinely has
  no such control now proceeds as it always did, with the unprotected transition
  stated plainly in the debug log; a card that *does* have the control and
  merely declined it this time is still refused, as is any request that pins a
  clock itself, because that request would fail at its own final step anyway.
  The debug log states what was actually established in that case — that the
  driver reported every clamp form the request was allowed to use as
  unsupported — rather than diagnosing the card, which is not the same claim.
- **A refused VF-curve write no longer reports success.** One refusal path set its
  flags but skipped the branch that records the failure, so the Apply returned
  “succeeded”, released the clamp, and left the card uncapped over a curve it had
  never written. The refusal itself came from three parts of the code disagreeing
  about the legal offset range; they now share one definition, so a limit the
  program generates cannot be rejected by the program that generated it.
- **A failed Apply now actually rolls back, and stays capped until it has.**
  Recovery used to require at least one success *and* one failure, so a
  first-and-only write that changed the hardware and then failed looked like
  “nothing to undo” while the card sat half-written. Reset and rollback used to
  discard each domain's result and lift the clock cap unconditionally — even
  right after the guard had decided to keep it. The cap is now lifted only once
  the card is verified back at stock, per domain.
- **Verification no longer accepts a curve it did not check.** With a hard clock
  lock, the check returned success outright — the individual points you typed
  below the lock anchor were never verified, although they run at their own
  voltages. And a point that came back far from its target had the target
  overwritten with the reading, so the check was comparing the reading against
  itself. Points reading *below* target are still accepted (the driver is allowed
  to be conservative); points reading *above* target now fail. Points you did not
  type an explicit frequency for are verified against the offset that was asked
  for rather than against a preview computed before the write, which is what the
  driver actually guarantees.
- **Asking to lock at a point that is not on the card's curve is refused up front**
  instead of being silently applied as an ordinary unlocked profile.
- **Reset to stock no longer reports success over failures, or wipes settings it
  does not own.** The baseline reset returned early on the core domain, so
  failures in the XBAR, SYS and VIDEO domains were written into a buffer nobody
  read and the reset still reported success; the VIDEO path had no failure branch
  at all. It also zeroed every nonzero advanced offset it found while Apply
  restored only the fields you had asked for — so a core-only profile switch
  wiped an XBAR or MSVDD offset set elsewhere and never put it back. Those fields
  are now preserved independently.
- Diagnostics: the apply witness records the measured clock and live 3D load
  across the transition and freezes its evidence at verified handoff, so a
  legitimately higher final pin is no longer reported as having exceeded the
  earlier transition cap, and an idle test run can be told apart from a loaded
  one.

- **A profile switch under 3D load no longer fails, and no longer takes the
  driver down with it.** Switching between two saved profiles while a game was
  running ended with an error, a restarted background service and automatic
  restore switched off. Three separate faults lined up:
  - Curve points that come from a GPU offset rather than from a number you
    typed were being held to a stale absolute frequency. A profile stores each
    point as its *stock* frequency plus your offset and rebuilds the absolute MHz
    on load, using the stock frequency as it was when you **saved**; the editor
    shows the same projection for any point you did not type yourself. Under load
    the driver reports a different stock frequency for the same point — a whole
    VF bin, 30 MHz, on this card — so a point carrying exactly the offset you
    asked for read back 30 MHz away from that rebuilt number and was rejected.
    Such points are now written and checked against the **offset**, which is what
    you actually asked for. The same applies to a point a profile saved with a
    zero offset: that is a record of where stock sat when you saved, not a
    request to hold that frequency, and stock moves under load. This is tracked
    per point, so hand-editing one field of a loaded profile leaves that one
    point a real absolute target — it still holds the driver to your number —
    while its neighbours keep offset intent. That provenance now survives every
    path a request can take: the merge into the service's durable intent (so an
    automatic restore or resume replays the same offset intent, not a stale
    absolute), the Windows and Linux settings merges, and a Linux terminal-UI
    edit, which makes the edited point a real absolute exactly like the
    graphical editor already did. The same numbers with a different origin are
    now also treated as a different request.
  - The routine that nudges points onto target was working from its own private
    copy of the rule above, so it undid the fix. A profile whose points had just
    been written correctly would have them overwritten the moment *any* other
    point needed correcting — in the reported case the flat tail, which missed
    by one bin because the stock frequency had moved. The apply then failed on a
    value that routine had invented. Every place that decides what to write to a
    curve point now asks the same single piece of code.
  - **The log now says when an apply lands far below the curve you asked for.**
    An apply could report success while a point sat hundreds of MHz below its
    target, because the offset it wrote did verify — the profile simply was not
    doing what it said. Any point ending 100 MHz or more below target is now
    counted and named in the debug log.
  - The same routine was also measuring against the card's stock frequency as
    it had been at the *start* of the apply, not as it was right then. Under
    load the driver moves that figure, so the routine kept recomputing the same
    value it had already written and the point never budged — the flat tail
    converged on the first try every time because it alone was reading the
    current figure.
  - The same routine could not tell that it had stopped making progress. Each pass rewrote identical values and read back
    identical frequencies, 25 times, about a second each. It now stops as soon
    as a pass improves nothing, and the apply fails immediately with the real
    reason instead of grinding.
  - That stop was then checked *before* the pass was checked against the curve
    you asked for, and the two use different yardsticks: the progress check
    wants an exact match, while the apply is verified against a small
    per-point tolerance. A pass that put the whole curve inside tolerance
    without hitting any point exactly could therefore stop and be reported as a
    failure, rolling back a curve that had in fact landed. The pass is now
    verified first, so a correct result is never thrown away for missing an
    exact number.
  - While that ran, the service's own watchdog — which exists to catch the
    graphics driver hanging — measured how long a queued fan update had been
    waiting, not whether anything was actually stuck. A slow-but-healthy apply
    looks exactly like a hang by that measure, so the watchdog restarted the
    service and killed the apply. It now asks whether the work itself has
    stopped moving, which a real hang does and a slow apply does not.

### Upgrading, and what a saved profile remembers

Adding that per-point information to curve points made each saved record 128
bytes longer, and two things had been quietly relying on the old size.

- **Upgrading on Linux no longer forgets what the daemon was doing.** The
  background daemon keeps two files: the settings it last applied, so it can put
  them back, and your start-up policy, so it knows what to apply at boot. Both
  embed the record that just changed size, and both were accepted only at
  *exactly* the size the running build expected — so upgrading from 0.25.2
  deleted the first and treated the second as damaged, leaving the daemon
  applying nothing until you set it up again. On Arch the package restarts a
  running daemon as part of the upgrade, so it took effect immediately, and the
  only trace was a line in the debug log. Both files are now recognised at their
  older sizes and converted on read, keeping the settings and the policy, and
  the daemon says in the log which generation it converted. A file it genuinely
  cannot identify is still discarded, but it now says how big it was and what it
  expected instead of just "invalid". The same applies to the short-lived file
  the Windows service writes before restarting itself for driver recovery.
- **Saving a profile no longer forgets which points you typed.** A profile
  recorded "these points are stock plus your GPU offset" once for the whole
  curve, which could not describe the case the work above exists for: load an
  offset profile, hand-edit one point, and that point is a real target while its
  neighbours are still projections. Saving flattened the difference, and loading
  then marked *every* point as a projection — so one save-and-reload handed the
  number you typed back to a stock frequency that moves under load. Profiles now
  store the actual frequency of every point plus a per-point note of which ones
  came from your offset, so a point you typed keeps holding the driver to your
  number across any number of saves. Profiles written by earlier versions are
  read exactly as before; a profile written now and opened by an older version
  is read as plain frequencies, which is what that version would have done with
  it anyway. This also covers the profile the installer carries across an
  upgrade.
- A request arriving at the background service with a malformed value in that
  new per-point field is now rejected rather than corrected. Nothing could act
  on it either way, but the check belonged with the others.

### Audit fixes

The repository-wide audit found no release-blocking defect; everything below is a
fix for something it turned up.

- **Command-line output now actually appears in your terminal.** `greencurve.exe`
  is a GUI-subsystem program, so Windows gives it no console — every CLI command
  printed *nothing at all* and exited with success. `greencurve.exe --help`
  produced zero output; `--service-install` gave no hint whether the service had
  been installed or had failed. All CLI output now goes to your terminal **and**
  to the log file, and the help text names the log path. Note that Windows does
  not make a shell wait for a GUI program, so the output arrives just after your
  prompt returns; `start /wait greencurve.exe --help` keeps it in order.
- **The debug log no longer contains your Windows account name.** Five places
  wrote your user-profile path (and the scheduled-task name, which embeds your
  computer and account names) into `greencurve_debug.txt` — the file you attach
  to a bug report. They are now written as stable anonymous tokens, which keeps
  the log just as diagnosable without identifying you. A build check now enforces
  this so it cannot come back.
- **Changing the machine-wide update policy now requires an administrator.** A
  standard user signed in at the console could permanently turn off automatic
  update checking for everyone on the machine, with no elevation prompt.
  Applying settings, resetting, and running a one-off update check or install are
  unchanged — those stay available to any user at the console, as before.
- **The "high overclock" confirmation now covers the XBAR voltage offset.** A
  +200 MHz core clock asked for confirmation, but a +100 mV rail voltage — the
  one setting here that can damage a card rather than just destabilise it — did
  not. Hand-typed voltage offsets of +25 mV or more now prompt, with the same
  never-nag rules as the clock domains (loading a saved profile, re-applying, or
  lowering an existing offset all stay silent). Configurable via
  `high_oc_warn_msvdd_offset_mv`; `0` disables it.
- **Linux: generated desktop and systemd files handle unusual paths correctly.**
  The `.desktop` launcher and the optional `greencurve-apply.service` unit are
  now escaped according to each file format's own rules, and a path containing a
  line break or control character is refused with a clear message instead of
  producing a malformed file.
- **Linux: `--probe` and the running daemon now agree.** They used two separate
  copies of the GPU VF-table read that disagreed about what a valid driver
  response is, so on some GPUs `--probe` could report a readable curve that the
  terminal UI then refused to drive. Both now use the same rule.
- **The fan curve is validated the same way everywhere.** Curves arriving from
  something other than the graphical editor are now held to the same rules the
  editor enforces (ordered points, the documented 0.25–5 s poll range).
- Assorted smaller fixes: the CLI log is appended instead of being overwritten by
  the next command, a mistyped command line now reports an error and a non-zero
  exit code instead of failing silently, the debug log says so when a line was
  written into a different session's file, and the log line during an unlocked
  profile switch is no longer silent about why no clock clamp was armed.

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
