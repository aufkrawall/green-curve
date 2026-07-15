# Green Curve

Green Curve is a small NVIDIA VF-curve tuning tool with a full Windows implementation and a native Linux port (NvAPI + NVML, driven by a root systemd daemon). The app inspects and edits the live NVIDIA voltage/frequency curve on supported GeForce GPUs. Pascal, Turing, Ampere, Lovelace, and Blackwell are treated as tested known families; unrecognized future NVIDIA GPU families use a best-effort fallback backend behind a warning the user can disable.

Version: see the [`VERSION`](VERSION) file at the repository root.

> ⚠️ **Platform support:** **Windows x64** is the actively used platform. **Linux x64** has user hardware validation for its VF write path but remains experimental. **Windows arm64** and **Linux arm64** are compile- and binary-inspection-only targets; neither has completed a live GPU-control validation. Use experimental builds at your own risk.

## What it does

- Reads the live VF curve from the NVIDIA driver
- Lets you edit visible curve points in a simple Win32 GDI UI
- Point locking with a tri-state checkbox: one click puts a checkmark and **flattens the curve tail** (caps all points beyond the lock anchor to the same frequency); a second click switches to a filled dot that **pins the GPU clock** via NVML (hard lock, min=max frequency, no dynamic scaling); a third click clears the lock. Right-click opens a menu to pick any mode directly. The tick-versus-dot glyph makes the active mode clear at a glance
- Reads and writes global GPU clock offset, effective VRAM offset, power limit, and fan control with three modes: driver auto, fixed percentage, or a custom temperature curve
- Fan curve mode lets you define up to 8 temperature-to-speed points with configurable hysteresis and poll interval; the service reasserts the fan setting periodically
- 5 saved profile slots per user, with global hotkeys for instant switching (e.g. Ctrl+Alt+F2)
- Auto-profile switching: Green Curve watches the foreground window and applies a saved profile based on the running application (by executable name, window title, window class, or fullscreen state)
- Provides CLI modes for dump, JSON export, and probing driver capabilities
- Detects GPU family via public NVAPI architecture metadata and selects a matching VF backend at runtime
- Recognizes Pascal, Turing, Ampere, Lovelace, and Blackwell GPUs
- Writes a persistent Windows JSON probe report with `--probe --probe-output <path>` for unrecognized GPU families
- Tray icon changes shape to reflect live GPU state (default, OC active, custom fan active, or both); hover text shows the mode and active profile
- Windows uses a dedicated elevated background service for OC, UV, power, and long-running fan control while the GUI runs unelevated
- The Windows GUI adapts its graph and VF-point columns to the monitor work area and current per-monitor DPI; if minimum readable controls cannot fit, the complete interface scrolls instead of being clipped behind the taskbar
- Per-user Windows config now defaults to `%LOCALAPPDATA%\Green Curve\config.ini`, with one-time import from the legacy beside-exe config when present
- Configured logon profiles are applied by the background service; long-running custom fan control no longer depends on keeping the tray client running
- Multi-user: an administrator can share a profile with **all users** (applied on logon for accounts without their own profile, and loadable on demand by any user), stored machine-wide in `%ProgramData%\Green Curve\shared-profiles.ini`
- Native Linux build: NvAPI (`libnvidia-api.so.1`) + NVML VF-curve / clock / power / fan control, driven by a root systemd daemon over a Unix socket, with a dependency-free raw-terminal TUI client
- The Linux TUI mirrors the Windows workflow with responsive VF Curve, Fan Curve, and Profiles & Tools tabs; absolute per-point MHz editing; one-click flatten / second-click hard pin; global GPU and memory offsets; low-point exclusion; power control; custom fan curves; profiles; live exports; mouse controls; and complete keyboard navigation

## Technical notes

- Amalgamated/unity-built C++ Win32 application built with `zig c++`
- Uses dynamically loaded NVIDIA driver interfaces available on the local machine
- Uses public NVAPI entry points exposed by the installed driver
- Uses NVML from the local NVIDIA driver install for supported management operations
- Windows uses a local named pipe and a machine-wide Windows service running as `LocalSystem`
- Does not ship NVIDIA driver binaries
- Debug logging is on by default so issues are easier to diagnose; log files are size-capped and rotated automatically
- Linux uses a root systemd daemon and a Unix-domain socket (`/run/greencurve/greencurve.sock`, `greencurve` admin group), mirroring the Windows service/pipe split; it is a glibc-dynamic binary because it must `dlopen` the NVIDIA driver libraries
- Tiny

## Build

```bash
python build.py
```

This builds the Windows and Linux x64/arm64 release matrix under `dist/` and packages one verified `.7z` archive per OS/architecture. Release packaging requires 7-Zip and rejects unexpected payload files.

Additional non-shipping checks:

```bash
python build.py --check
python build.py --test
python build.py --lsp
```

`--check` builds the selected target(s) into a temporary workspace without replacing release outputs. `--test` runs pure regression tests that do not touch GPU hardware. `--lsp` regenerates `compile_commands.json` for clangd.

## Windows Probe Report

If a future GPU family is unrecognized and uses the fallback backend, collect a Windows probe report with:

```powershell
greencurve.exe --probe --probe-output unrecognized_gpu_probe.json
```

Run it while the Windows background service is installed and healthy. The CLI entry point stays `greencurve.exe`, but hardware-backed probe generation is executed by `greencurve-service.exe` through the local service IPC path.

The generated JSON report includes:

- public NVAPI architecture and PCI identifiers
- selected GPU family and VF backend
- NVML and public clock/power/fan state
- raw results from the current private VF probe calls

That JSON file is the artifact to use when validating or correcting fallback support for a new NVIDIA GPU family.

Linux cross-build:

```bash
python build.py --target linux
```

This produces the selected Linux payload under `dist/linux-<arch>/greencurve/`,
a dynamically linked glibc binary (it must `dlopen` the proprietary NVIDIA driver
libraries, which a static musl binary cannot do).

The Linux binary is a native NVIDIA control port. It drives the GPU through the
same private NvAPI `nvapi_QueryInterface` path the Windows build uses — on Linux
via `libnvidia-api.so.1` (proprietary driver >= 555) — plus NVML
(`libnvidia-ml.so.1`), and supports VF-curve read/write, GPU/memory clock
offsets, power limit, hard clock locks, and fan control. A root daemon owns the
GPU and the unprivileged TUI/CLI talk to it over a Unix socket, mirroring the
Windows elevated-service / GUI split.

```bash
greencurve --probe                  # verify NvAPI + NVML, GPU, family, OC range
greencurve --self-test              # read-only validation of the apply path
greencurve --gpu 0000:01:00.0 --tui # select a stable PCI target on multi-GPU systems
sudo greencurve --service-install   # install + start the systemd daemon
greencurve --tui                    # edit and apply the VF curve / fan / power
greencurve --dump-live              # dump all 128 live/base/target VF values
greencurve --json-live              # same live state as machine-readable JSON
greencurve --apply-config           # apply the selected profile
greencurve --reset --apply-config   # reset OC/UV to driver defaults
sudo greencurve --service-remove
```

Running `greencurve` without arguments also opens the TUI. Its fixed header,
tabs, status/footer, graphs, tables, and controls reflow at compact, medium, and
wide terminal breakpoints; the minimum interactive size is 72x24 cells. Click
buttons, checkboxes, table fields, and either graph with the mouse. The wheel
scrolls the active table. `Tab`/`Shift+Tab` and the arrow keys move focus,
`Enter` edits or activates, `Page Up`/`Page Down` scroll by a page,
`Ctrl+Page Up`/`Ctrl+Page Down` changes tabs, and `Home`/`End` jumps through the
VF curve. Every mouse operation has a keyboard path.

`--dump` and `--json` describe the selected saved profile. Use `--dump-live` or
`--json-live` when diagnosing or calculating from the daemon's current absolute
VF state: every populated point includes its index, voltage, base MHz, live MHz,
offset, staged target MHz, and the rule producing that target.

The daemon socket is restricted to `root` and the `greencurve` group
(`0660 root:greencurve`). To use the TUI or CLI without `sudo`, add your account
after installation, then start a new group session:

```bash
sudo usermod -aG greencurve "$USER"
# sign out and back in, or run: newgrp greencurve
```

The Linux VF write path is validated on real NVIDIA hardware; the apply pipeline
verifies each write by reading the curve back. Run `--probe` first to confirm
the driver libraries and the GPU family are detected.

Linux hardware writes are transactional. The daemon journals a checksummed,
versioned record before mutation, publishes it as active only after verified
success, and attempts rollback on any phase or persistence failure. Corrupt,
legacy, prepared, uncertain, or mismatched-GPU state is never replayed at
startup. On a multi-GPU system an exact PCI BDF selection is mandatory; stale,
missing, duplicate, or cross-API-mismatched identities allow telemetry but block
writes until the user selects a GPU explicitly.

## Safety warning

This tool can change GPU clocks, voltage/frequency behavior, power limit, and fan control. These actions can cause instability, crashes, thermal issues, data loss, reduced hardware lifespan, or hardware damage. Manual fan control in particular can be dangerous if used carelessly.

Use it only if you understand the risks and are able to monitor temperatures, stability, and cooling behavior yourself.

## No warranty / liability

This project is provided under the MIT license, without warranty of any kind. The software is offered as-is. You are fully responsible for any use, misuse, instability, damage, or loss resulting from it.

## Legal / distribution note

- Green Curve is an unofficial third-party utility and is not affiliated with or endorsed by NVIDIA.
- It relies on driver interfaces exposed on systems with NVIDIA drivers already installed.
- The project should only distribute its own source and binaries, and should not bundle NVIDIA driver binaries unless a separate NVIDIA license clearly permits that.
- Release review should re-check current NVIDIA NVML and driver license terms before publishing: <https://developer.nvidia.com/management-library-nvml> and <https://www.nvidia.com/en-us/drivers/nvidia-license/linux/>.
- NVIDIA names, product names, and trademarks remain the property of their respective owners.
- Public NVAPI SDK materials published by NVIDIA are currently available under MIT terms for the SDK repository/import-library package, but that does not automatically authorize repackaging unrelated NVIDIA-owned binaries.

## Windows Service Runtime

- The GUI always expects the `Green Curve Background Service` to own live GPU control.
- `greencurve.exe` is the unelevated GUI and tray client.
- `greencurve-service.exe` is the dedicated elevated service binary. Service install registers the binary **from whichever directory you launched `greencurve.exe`** (the service installs adjacent to the GUI) and hardens that binary's permissions so a standard user cannot replace it.
- The service is machine-wide, but per-user configs stay per-user. GPU state is machine-global, so the most recent apply wins.
- Logon tasks stay per-user and normally run immediately at least privilege.
  They send a settings-free, authenticated logon handoff; the service resolves
  the session's configured profile and remains the sole automatic writer.
- Selecting a logon profile keeps that silent handoff task enabled even when
  **Start program to tray on log in** is off; that checkbox controls only
  whether the GUI remains resident in the tray. Tray startup uses a separate
  per-user Windows Run entry, so the handoff task's three-minute safety limit
  can never terminate the resident GUI. An effective administrator-published
  all-users default also keeps the handoff task enabled after that account has
  launched Green Curve.
- When the service is not installed, stopped, or unresponsive, live OC, UV, power, and fan controls stay disabled in the GUI.

### Automatic apply and restore behavior

Green Curve intentionally does **not** promise that every service start restores
settings. Automatic writes are limited to real lifecycle events, so a service
repair, an emergency stop, or normal NVIDIA boost/temperature movement cannot
silently keep changing the GPU.

| Event | Expected behavior |
|---|---|
| Windows user logon | The service applies the account's configured **Apply profile after user log in** profile once. A WTS logon notification and the authenticated scheduled-task handoff are coalesced, so Fast Startup and autologon use the same event-only path. Merely connecting, unlocking, or switching to an already logged-in session is a readiness cue, not a new apply authorization. The tray-start checkbox controls only the GUI tray, not this permission. |
| Standby resume | Green Curve restores the complete current in-memory intent once (curve, offsets, power, lock, and fan), unless automatic restoration was previously safety-locked. Standby itself does not require a 10-minute proving period. A proof that was already mature before standby remains mature after a successful restore; an immature or unavailable proof restarts from that restore. |
| Confirmed driver recovery | Green Curve restores once only after the current proof has accumulated 10 minutes of **awake** stability in the current Windows boot. Sleep and hibernation do not advance this proof period, and a successful standby restore does not reset a proof that was already mature. |
| TDR/restart spam, a failed real hardware write, or driver recovery during the 10-minute proving period | Automatic restoration is disabled persistently. A later automatic action cannot clear this lockout; use a successful explicit **Apply** from the GUI, CLI, hotkey, or tray to acknowledge the condition and re-arm it. |
| Service install, repair, ordinary/manual start, or Task Manager termination | No automatic restore. In particular, killing `greencurve-service.exe` is treated as an emergency stop: even if the SCM starts it again, it will not replay settings solely because of that termination. |
| Normal VF curve / boost / temperature drift | Diagnostic-only. Green Curve does not continually monitor and “correct” expected NVIDIA drift. |

The current-boot check uses Windows' per-boot `BootIdentifier`, not a boot time
derived from the adjustable wall clock. Time synchronization or another clock
correction therefore cannot invalidate a mature 10-minute proof while Windows
is still in the same boot.

The service retries only unavailable prerequisites at logon (session identity,
profile materialization, and driver readiness), not a failed GPU write. It also
revalidates the Windows login identity and selected GPU immediately before the
single write. A scheduled task delay, elevation, or repeat policy is neither
required nor used to decide GPU writes. Compatible tasks created by older Green
Curve versions (a valid delay, `HighestAvailable`, or the old unlimited
execution setting with the correct user and command) keep working and are
normalized best-effort. Disabled, wrong-user/stale-command, extra-trigger,
battery/idle-gated, scheduler-repeating, too-short, or unsafe
multiple-instance definitions are repaired and reported if repair fails.
The saved profile choice is retained, but the warning means logon-event
redundancy is degraded until the task is repaired.

## Multi-user setup (Windows)

Because the service installs next to the GUI binary, **where you place the two executables matters** on machines with more than one user account.

### Where to put the binaries

- **Recommended: `%ProgramFiles%\Green Curve\`** (e.g. `C:\Program Files\Green Curve`). Put both `greencurve.exe` and `greencurve-service.exe` there, launch the GUI from there, then install the service. `%ProgramFiles%` is admin-only-writable but readable/executable by **every** account, so all users can launch the GUI while a standard user cannot tamper with the SYSTEM service binary. (The bundled installer already targets this location.)
- **Avoid running from inside a user profile** (e.g. `C:\Users\<name>\Downloads\...`). Other accounts — especially standard/restricted users — cannot read or execute a binary that lives in another user's profile, so they will not be able to start Green Curve. The GUI shows a warning in its status line when it detects it is running from a user-profile folder and recommends reinstalling under `%ProgramFiles%`.

### One-time admin setup

1. Copy `greencurve.exe` and `greencurve-service.exe` into `%ProgramFiles%\Green Curve\`.
2. Launch the GUI from there and enable the background service (this prompts for elevation). The service is machine-wide and starts at boot for all users.

### Sharing a profile with all users

1. As an administrator, select a saved profile slot and tick **"Share slot N with all users"** (a UAC prompt handles elevation — you do not need to run the whole GUI as admin). This publishes the full profile **and** marks it as the all-users default in one step.
2. Any account that logs in **without its own logon profile** then has that profile applied automatically by the service.
3. Any user (including standard/restricted accounts) can also click **"Shared profiles…"** to load a shared profile on demand and Apply it. Only the currently active session can drive the GPU, so users cannot fight over it.
4. Power users: right-click the share checkbox for advanced bank management (publish/clear an individual slot without changing the default).

### Restricting standard users to shared profiles

For managed/multi-user PCs, an admin can require that **standard (non-admin) users may only apply admin-published shared profiles**, not arbitrary OC of their own:

- Enable it from the GUI (right-click the **"Share with all users"** checkbox → **"Restrict standard users to shared profiles"**) or the CLI: `greencurve.exe --set-restrict-shared 1` (and `0` to turn it off). Both require administrator rights.
- Enforcement is in the **background service**, which is the real security boundary — when the policy is on, a non-admin's request to apply custom settings is rejected, and only "load a shared profile and Apply" is honored (the service applies its own copy of the admin's profile). This holds regardless of which client is used, so it cannot be bypassed with a copied binary or the CLI.
- Administrators are unaffected (even when running the GUI unelevated). Returning to stock (**Reset**) is always allowed. Logon auto-apply of shared profiles continues to work because the service applies them directly — it does not depend on the user running the GUI.
- Note: file-system permissions on the GUI binary are *not* a reliable way to restrict settings changes; use this policy instead.
- Under the hood, the policy is the `restrict_non_admin_to_shared` key in the `[policy]` section of `%ProgramData%\Green Curve\shared-profiles.ini` (`1` = on, `0`/absent = off). That file is admin-writable / all-users-readable, so for imaging or scripted deployment set the policy with the admin-only `--set-restrict-shared` command above rather than editing the file directly (a standard user cannot write it anyway).

### Where data lives

- **Per-user settings** stay private to each account: `%LOCALAPPDATA%\Green Curve\config.ini`.
- **Shared profiles + the all-users default** are stored machine-wide in `%ProgramData%\Green Curve\shared-profiles.ini`, protected so administrators can write it and all users can read it.
- CLI equivalents (all require elevation): `--share-slot <slot>` / `--unshare-slot <slot>`; advanced `--publish-slot-to-machine <slot>`, `--clear-machine-slot <slot>`, `--set-machine-logon-slot <slot>`, `--clear-machine-logon-slot`.

## Privacy & Data Handling

- Green Curve does not transmit any data over the network. There is no telemetry, analytics, cloud sync, or remote logging.
- Debug logs are written locally to `%LOCALAPPDATA%\Green Curve\greencurve_debug.txt` on Windows and `~/.local/share/greencurve/greencurve_debug.txt` on Linux. Log files are size-capped and rotated automatically.
- Probe reports (`--probe --probe-output`) are written only to a local file you specify. They contain GPU identifiers, driver capabilities, and VF-curve samples, but no personal data.
- The Windows background service runs as `LocalSystem` so it can access GPU management interfaces, but per-user configuration and profiles remain in the individual user's local app data.
- Profiles an administrator explicitly shares with all users are stored machine-wide in `%ProgramData%\Green Curve\shared-profiles.ini` (admin-writable, all-users-readable); nothing from a user's private config is shared unless the admin publishes it.
- Enabling **Start program to tray on log in** creates one per-user Windows
  `Run` value for tray startup; disabling the option removes it. Apart from that
  opt-in value and standard uninstall metadata, Green Curve creates no registry
  keys.

## Release readiness notes

- Built for local Windows use on systems with an installed NVIDIA driver
- A dedicated Windows background service binary is now shipped; no network-facing service or kernel component is shipped
- Hardware behavior can still vary by board vendor, VBIOS, cooling design, and driver version
- Long-running custom fan control now reasserts manual fan settings periodically and falls back to driver auto fan after repeated NVML failures

## License

MIT, copyright (c) 2026 aufkrawall. See `LICENSE`.
