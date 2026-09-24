# Green Curve

Green Curve is an open-source NVIDIA voltage/frequency (VF) curve tuning utility for Windows and Linux. It allows inspecting and editing the live VF curve, clock offsets, power limits, and fan curves on GeForce GPUs (Pascal, Turing, Ampere, Lovelace, and Blackwell), with a best-effort fallback backend for future architectures.

Windows GDI GUI:
<img width="883" height="757" alt="gcwin" src="https://github.com/user-attachments/assets/0f3ef4c0-7962-406c-b439-783aa5c3349b" />

Linux TUI (elements can be clicked with mouse):
<img width="1669" height="1386" alt="gclin2" src="https://github.com/user-attachments/assets/ff2e28dd-523d-4c3d-ab9f-05838a387e4f" />

Version: see the [`VERSION`](VERSION) file at the repository root.

> ⚠️ **Platform support:** **Windows x64** and **Linux x64** are fully validated on real NVIDIA hardware (curve writes, power, and fan control). **Windows arm64** and **Linux arm64** are compile targets and have not yet completed live hardware validation.
>
> **Integrated Grace/Blackwell parts (RTX Spark / GB10):** Green Curve probes each control domain read-only at startup and disables unavailable platform domains (such as unified memory or platform-managed cooling).

Before tuning an unvalidated GPU, driver, or architecture, run the read-only pre-flight (run elevated so the private VF surface is readable):

```bash
# Windows (elevated prompt)
greencurve.exe --self-test

# Linux
greencurve --self-test
```

It reports the loaded NVAPI image, whether the VF curve and control structs read back, the memory topology, and control domain availability.

## Features

- **Live VF-curve editing:** Inspect and modify visible curve points in a native Win32 GUI or raw-terminal Linux TUI.
- **Tri-state point locking:**
  - *Click 1 (Checkmark):* **Flattens the curve tail**, capping all points beyond the anchor to the anchor's frequency.
  - *Click 2 (Filled dot):* **Pins the GPU clock** via NVML (hard lock, minimum = maximum frequency, no dynamic scaling).
  - *Click 3:* Clears the lock. Right-clicking allows selecting any mode directly.
- **Clock and power tuning:** Adjust global GPU core clock offset, effective VRAM offset, and power target limits.
- **Fan control:** Three modes (driver auto, fixed percentage, or custom temperature curve). Supports up to 8 temperature-to-speed points with configurable hysteresis and polling intervals, plus native zero-RPM support where cards can stop fans at idle.
- **Advanced clock domains:** Exposes XBAR, SYS, and VIDEO offsets, along with active XBAR MSVDD voltage readback when supported by the driver.
- **Profiles and global hotkeys:** 5 saved profile slots per user with global hotkey switching (e.g. `Ctrl+Alt+F2`).
- **Auto-profile switching:** Automatically applies profiles based on the active foreground application (by executable name, window title, or fullscreen state).
- **Background service architecture:** Live hardware control is managed by a background service (`LocalSystem` on Windows, root systemd daemon on Linux) so the GUI and TUI clients run unprivileged.
- **Multi-user support (Windows):** Administrators can publish machine-wide profiles in `%ProgramData%\Green Curve\shared-profiles.ini` to be applied on logon for all users.
- **CLI automation:** Built-in commands for hardware probing, profile switching, and dumping live state (`--dump-live`, `--json-live`).

## Technical overview

- Native C++ application built without external runtime dependencies (Win32 GDI GUI on Windows; raw-terminal TUI on Linux).
- Uses dynamically loaded NVAPI and NVML interfaces from the locally installed NVIDIA driver; does not bundle proprietary driver binaries.
- Client/daemon split: unprivileged GUI and TUI clients communicate with the elevated background service via a local named pipe (Windows) or a restricted Unix domain socket (`0660 root:greencurve` on Linux).
- Built-in diagnostic logging: logs are size-capped, rotated automatically, and sanitize user account names and filesystem paths.

## Build

Green Curve requires Python 3.10+ to build. The build script automatically downloads and manages pinned toolchains (Zig, llvm-mingw, and 7-Zip archiver) under `compilers/`:

```bash
python build.py
```

Running `python build.py` builds the release matrix under `dist/`, packaging release archives (`.7z` on Windows, `.tar.xz` on Linux), standalone setup installers (`greencurve-<version>-windows-<arch>-setup.exe`), and Arch Linux packages (`greencurve-<version>-1-<arch>.pkg.tar.zst`).

Common build options:

```bash
python build.py --target windows         # build Windows targets only
python build.py --target linux           # build Linux targets only
python build.py --check                  # compile without replacing release outputs
python build.py --test                   # run automated regression tests
```

## Installing on Windows

You can install Green Curve using the setup installer or by extracting the portable archive:

### Option 1: Setup installer (Recommended)

Run `greencurve-<version>-windows-<arch>-setup.exe`. The installer:
- Installs to `%ProgramFiles%\Green Curve` by default.
- Registers and starts the background service with proper administrator-only permissions.
- Upgrades existing installations seamlessly: saves active settings, replaces files, restarts the service, and restores settings.
- Provides desktop and Start menu shortcuts, plus an uninstaller in Add/Remove Programs.

For unattended or scripted deployments:

```powershell
greencurve-0.27.0-windows-x64-setup.exe /S
```

Flags include `/D=<path>` to choose the installation folder, `--no-start-menu`, `--desktop`, `--launch`, and `--uninstall`.

### Option 2: Portable archive

1. Extract the `.7z` archive into a dedicated administrative folder (e.g. `C:\Program Files\Green Curve`). Do not extract to user folders or Downloads, as the background service binary must be protected from unprivileged modification.
2. From an elevated PowerShell or Command Prompt, register the service:
   ```powershell
   greencurve.exe --service-install
   ```

## Antivirus false positives

Antivirus scanners may flag Green Curve binaries (GUI, background service, or installer) with generic heuristic or machine learning tags such as `Trojan:Win32/Wacatac.B!ml`, `Gen:Variant`, or `HEUR:Trojan...`.

### Why this happens

- **Unsigned utility:** Green Curve is an open-source project and does not carry an expensive commercial Authenticode code-signing certificate. Scanners treat newly released unsigned binaries with suspicion.
- **Hardware control capabilities:** Green Curve requires an elevated background service, interacts directly with low-level GPU driver interfaces, adjusts clocks and voltages, and registers global hotkeys—actions commonly flagged by generic behavioral heuristics.

### Recommended actions

1. **Verify the download:** Verify the file's SHA-256 checksum or check its GitHub build provenance attestation:
   ```bash
   gh attestation verify <file> --repo aufkrawall/green-curve
   ```
2. **Add an exclusion:** Add the Green Curve installation folder (e.g. `C:\Program Files\Green Curve`) to your antivirus exclusion list.
3. **Build from source:** If you prefer not to use prebuilt binaries, you can easily build Green Curve locally with `python build.py` (see [Build](#build)).

## Updates

Green Curve includes an optional updater that can check for, download, and install releases from GitHub:

- **Opt-in checking:** On the first launch, Green Curve asks whether you want automatic daily update checks. You can toggle this setting or click **Check now** at any time in the **Updates** dialog.
- **Cryptographic verification:** Releases carry an ECDSA P-256 signed manifest with exact file sizes and SHA-256 hashes. The updater checks the signature against an embedded public key before reading the manifest or downloading files.
- **Safe installation:** Updates are never applied without user consent. Clicking **Install** saves current active GPU settings, stops the background service, updates binaries, restarts the service, and restores your active settings.

## Installing on Linux

### Option 1: Setup script

Extract the `.tar.xz` archive and run the setup script:

```bash
tar xf greencurve-<version>-linux-<arch>.tar.xz
sudo ./greencurve-setup.sh install
```

The script installs and starts the root systemd daemon (`greencurve.service`), creates the `greencurve` group, adds your user to it, creates a desktop launcher, and symlinks the binary to `/usr/local/bin/greencurve`.

After installation, reload your group membership (log out and back in, or run `newgrp greencurve` in your current shell).

Useful setup commands:
```bash
./greencurve-setup.sh status                   # check daemon and socket status
sudo ./greencurve-setup.sh uninstall           # uninstall daemon (keeps settings)
sudo ./greencurve-setup.sh uninstall --purge   # complete removal including /var/lib/greencurve
```

### Option 2: Arch Linux (pacman)

Arch Linux packages are built automatically under `dist/` by `python build.py`. Install directly using `pacman`:

```bash
sudo pacman -U greencurve-0.27.0-1-x86_64.pkg.tar.zst
```

Alternatively, PKGBUILD templates are available under [`packaging/arch/`](packaging/arch/) to build from source (`makepkg -si`) or install from prebuilt binaries (`makepkg -si -p PKGBUILD.bin`).

### Launching and navigation

Run `greencurve` (or `greencurve --tui`) from a terminal. If launched from a graphical file manager without a terminal, Green Curve automatically launches inside your desktop session's terminal emulator.

The TUI supports full keyboard navigation and mouse interactions (clicks and scroll wheel):
- `Tab` / `Shift+Tab` and arrow keys move focus.
- `Enter` activates buttons or enters numeric edit mode.
- `Ctrl+Page Up` / `Ctrl+Page Down` switches tabs.
- `Home` / `End` jumps through the VF curve.

### Startup apply behavior

By default, the Linux daemon re-applies the last active settings on startup. You can configure startup behavior via the TUI (**Profiles & Tools** tab) or CLI:

```bash
greencurve --show-startup            # show configured boot action and status
greencurve --startup-profile 3       # apply saved profile 3 at startup
greencurve --startup-profile last    # re-apply last applied settings (default)
greencurve --startup-profile none    # leave GPU untouched at boot
```

Linux writes are transactional: the daemon journals settings before applying, verifies curve readback, and automatically rolls back if applying fails.

## CLI commands

```bash
greencurve --probe                  # verify NVAPI/NVML, GPU architecture, and clock ranges
greencurve --self-test              # read-only validation of apply paths
greencurve --gpu 0000:01:00.0 --tui # select target PCI GPU on multi-GPU systems
sudo ./greencurve --service-install # install and verify background daemon
greencurve --tui                    # launch the terminal interface
greencurve --dump-live              # print all 128 live/base/target VF points
greencurve --json-live              # dump live GPU state as JSON
greencurve --apply-config           # apply active profile settings
greencurve --reset --apply-config   # reset clocks and voltages to driver defaults
sudo greencurve --service-remove    # uninstall daemon service
```

## GPU Probe Report

If you are running an unrecognized GPU family or need to report driver compatibility details, generate a diagnostic probe report:

```powershell
# Windows (requires background service to be running)
greencurve.exe --probe --probe-output gpu_probe.json
```

```bash
# Linux
greencurve --probe --probe-output gpu_probe.json
```

The report contains public NVAPI and PCI identifiers, selected VF backend, NVML state, and raw results from VF capability probes. Review and redact any sensitive host information before sharing.

## Windows Service & Automatic Apply

The background service (`greencurve-service.exe`) manages GPU access, allowing the GUI to run without elevation.

### Automatic restore events

Green Curve applies settings only during specific lifecycle events:
- **User logon:** Applies the account's configured logon profile.
- **Standby resume:** Restores the active curve, offsets, power, and fan settings once after waking.
- **Driver recovery:** Restores active settings once after a confirmed driver recovery event, provided the system has been stable for at least 10 minutes. Repeated crashes or TDR events lock out automatic restoration until manually re-applied by the user.
- **Service starts/restarts:** Manual or unexpected service restarts do not automatically replay settings (preventing restart loops after emergency stops).
- Normal boost clock and temperature drift are never artificially overwritten.

## Multi-user setup (Windows)

Because the background service runs as `LocalSystem`, installing into a secure directory like `%ProgramFiles%\Green Curve\` ensures standard user accounts cannot modify or tamper with the service executable.

### Sharing profiles across accounts

1. As an administrator, select a profile slot and check **"Share slot N with all users"** (elevates via UAC).
2. The profile is saved to `%ProgramData%\Green Curve\shared-profiles.ini` (admin-writable, all-users-readable) and serves as the logon default for accounts without a personal profile.
3. Standard users can load and apply shared profiles on demand via the **Shared profiles…** dialog.

### Restricting standard users

Administrators can prevent standard accounts from applying arbitrary custom overclocks:
- Right-click the **"Share with all users"** checkbox and select **"Restrict standard users to shared profiles"**, or run elevated:
  ```powershell
  greencurve.exe --set-restrict-shared 1
  ```
- When enabled, the background service rejects custom tuning requests from non-admin accounts and allows only stock resets or admin-approved shared profiles.

## Privacy & Data Handling

- **No telemetry:** Green Curve contains no telemetry, analytics, cloud tracking, or remote error reporting.
- **Network access:** The only network feature is the optional GitHub release check, which runs only if enabled. It queries GitHub for version manifests; no hardware information, profiles, or settings are ever transmitted.
- **Local logs:** Debug logs are stored locally (`%LOCALAPPDATA%\Green Curve\greencurve_debug.txt` on Windows, or `/var/lib/greencurve/` / user config directory on Linux). Logs are size-capped, rotated, and redact sensitive account names and filesystem paths. Logging can be disabled by setting `[debug] enabled=0` in `config.ini` or environment variable `GREEN_CURVE_DEBUG=0`.
- **Crash dumps:** Windows writes local minidumps (`greencurve_crash_*.dmp`) next to the binary. On Linux, crash breadcrumbs are written to the debug log and stderr; core dumps are managed by the system's `core_pattern`.

## Safety warning

Modifying GPU clocks, voltage curves, power limits, and fan speeds can cause system instability, crashes, reduced hardware lifespan, or hardware damage. Custom fan curves in particular carry risk if configured with insufficient cooling thresholds.

Use this software at your own risk. Monitor your temperatures, stability, and cooling behavior when applying custom settings.

## License & Disclaimers

- Licensed under the **MIT License**. Copyright (c) 2026 aufkrawall. See [`LICENSE`](LICENSE) for details.
- Green Curve is an independent open-source project and is not affiliated with, endorsed by, or sponsored by NVIDIA Corporation.
- NVIDIA, GeForce, Pascal, Turing, Ampere, Lovelace, and Blackwell are trademarks of NVIDIA Corporation.
