# Archived Log (before 2026-07-01)

Historical entries archived from recent.md.

---

## 2026-06-29: fix GPU OC settings not reset to defaults on service termination (Task Manager kill without GUI)

**Bug:** When the Green Curve service is killed via Task Manager and the GUI
process was never started, GPU OC settings (GPU offset, memory offset, power
limit, locked clocks, fan mode) remain active on the GPU hardware indefinitely.
The service shutdown path only reset fan to auto (`nvml_set_fan_auto()`), not
the full OC profile.

**Root cause:** Two missing reset paths:

1. **Controlled stop** (`main_service_server.cpp:1158`): Only called
   `nvml_set_fan_auto()` — did NOT reset GPU offset, memory offset, power
   limit, or locked clocks. OC settings persisted through `net stop` and
   system shutdown.

2. **Task Manager kill**: No cleanup code runs. On next service start (SCM
   restart), the "no restart snapshot" branch in the startup coordinator
   (`main_service_recovery.cpp:544`) treated it as a clean boot and left
   hardware unchanged — the stale OC settings were never cleaned up.

**Fixes:**

1. **`source/main_service_server.cpp:1158-1161`** — Replaced the standalone
   `nvml_set_fan_auto()` in the controlled shutdown path with
   `service_reset_all()` which resets GPU offset, memory offset, power limit,
   locked clocks, AND fan to driver defaults. `service_reset_all()` internally
   calls `nvml_set_fan_auto()` so fan behavior is preserved.

2. **`source/main_service_recovery.cpp:544-572`** — In the no-snapshot startup
   branch, added live GPU state detection via `hardware_initialize()`. If any
   OC settings are still active (gpuClockOffsetkHz ≠ 0, memClockOffsetkHz ≠ 0,
   powerLimitPct ≠ 100, or fan not auto), calls `service_reset_all()` to clean
   up stale OC settings from an unexpected process termination.

3. **`build.py:3017-3022,3101-3107`** — Updated regression tests to match
   the new shutdown and startup coordinator log messages.

**Behavior matrix:**
| Scenario | Before | After |
|----------|--------|-------|
| Fresh boot | No change (GPU at defaults) | No change (no-op) |
| Controlled stop (`net stop`) | Only fan → auto | Full GPU reset |
| Task Manager kill + SCM restart | Stale OC persists | Reset on next start |
| System shutdown | Only fan → auto | Full GPU reset |

---

## 2026-06-29: fix OC settings not reapplied after GPU Device Manager suspend/resume

**Bug:** After suspending GPU in Device Manager (disable) and resuming (enable),
OC settings (mem offset, power limit, clock lock) were not automatically
reapplied. Only fan settings survived.

**Root cause (two sub-issues):**

1. **Missing `DBT_DEVNODES_CHANGED` handler** (`main_service_server.cpp`):
   Device Manager enable/disable does NOT trigger
   `GUID_DEVINTERFACE_DISPLAY_ADAPTER` removal/arrival events. It fires
   `DBT_DEVNODES_CHANGED` instead, which was unhandled. The service never knew
   the GPU driver was reloaded, so no OC persistence check ran.

2. **Partial (fan-only) apply to fresh service lost OC fields**
   (`main_service_runtime.cpp`): After a service crash/restart, if the user
   changed only fan mode and clicked Apply, the GUI took a "fan-only apply
   shortcut" — sending only `fanMode=2` via IPC (no mem/power/lock). The
   service received this with `g_serviceHasActiveDesired=false` and initialized
   `mergedActiveDesired` from empty defaults, producing incomplete
   `g_serviceActiveDesired`. Any subsequent crash snapshot saved only the
   partial state.

**Fixes:**

1. **`source/main_service_server.cpp:786-801`** — Added `DBT_DEVNODES_CHANGED`
   case to the `SERVICE_CONTROL_DEVICEEVENT` handler that dispatches a worker
   thread calling `service_check_oc_persistence(true)`. This directly detects
   Device Manager enable/disable and verifies/reapplies OC settings if lost,
   analogous to the Windows standby resume (`PBT_APMRESUMEAUTOMATIC`) handler.

2. **`source/main_service_runtime.cpp:1088-1118`** — In
   `service_apply_desired_settings()`, when `g_serviceHasActiveDesired` was
   false (fresh service after crash/restart), read live GPU state via NVML to
   populate missing mem offset and power limit fields before finalizing
   `g_serviceActiveDesired`. This ensures the crash-restart snapshot captures
   the full OC profile even after a partial (fan-only) apply.

3. **`source/main_service_install.cpp:47`** — Fixed pre-existing llvm-mingw
   fallback definition of `DBT_DEVICEREMOVECOMPLETE` from `0x0007` to
   `0x0004` (collided with `DBT_DEVNODES_CHANGED` = `0x0007`, triggering a
   compile error when both are used in the same switch)

**Unaffected:**
- Task Manager kill + SCM restart: No snapshot written (`request_service_restart()`
  not called) → `service_load_restart_reapply_snapshot()` returns false →
  startup coordinator leaves hardware unchanged. Correct.
- 10-min OC stabilization window (`main_service_recovery.cpp:559`): Retained as-is.

---

## 2026-06-29: fix transient "background service needs repair" during VF drift recovery

**Root cause:** The single-instance named pipe server blocks on
`lock_service_runtime()` (WaitForSingleObject INFINITE) in GET_SNAPSHOT
and GET_TELEMETRY handlers. When the recovery reapply thread holds that same
lock for extended NVML operations, the pipe server cannot accept new client
connections, so the GUI's PING (500 ms timeout) fails with "Timed out waiting
for the background service" and shows "needs repair" — which self-resolves
once the reapply releases the lock.

**Fix (`source/main_service_runtime.cpp:367-390`):**
- Added `try_lock_service_runtime(DWORD timeoutMs)` — same recursive
  owner/depth tracking as `lock_service_runtime()` but with a caller-chosen
  timeout instead of INFINITE. Returns false on timeout.

**Fix (`source/main_service_server.cpp`):**
- GET_SNAPSHOT and GET_TELEMETRY now use `try_lock_service_runtime(250)`.
- When the lock is busy (recovery reapply in progress), serve cached
  `g_serviceControlState` immediately — logged as
  `"service snapshot/telemetry: runtime lock busy (recovery reapply in
  progress), serving cached globals/telemetry"`.
- This keeps the pipe responsive to PINGs (and other clients) during
  recovery, preventing the transient false-positive "needs repair".

**Fix (`source/main_service_recovery.cpp`):**
- Added `"reapply thread: acquiring/releasing runtime lock"` debug logging
  around lock operations for future diagnosis.

## 2026-06-29: service uninstall — revert to driver defaults before stopping service

**Change:** In `service_install_or_remove()` (`source/main_service_install.cpp:311`),
when uninstalling (`enable == false`) and the service is running and responsive
(`g_app.backgroundServiceAvailable`), call `service_client_reset()` over IPC before
issuing `SERVICE_CONTROL_STOP`. This resets GPU offsets, power limit, fan, and
locked clocks to driver defaults — preventing an overclock from persisting after
the service is removed.

- Only triggers when the service is responsive; broken/stopped service skips IPC.
- `service_client_reset()` is already `static` in the same TU (compiled as a shard
  after `main_service_ipc.cpp`), so no new includes or linkage changes needed.
- Existing `service_reset_all()` is already efficient — it short-circuits on zero
  values — so calling it when no custom profile is active is a no-op.
- Verified: `python build.py` (all 4 targets) and `python build.py --test` pass.

## 2026-06-29: fix glow timer — duplicate static stubs need identical logic in both copies

**Critical architectural finding (revised):** `main_shell.cpp` contains `static` stubs
for several functions (e.g. `update_background_service_controls()` at line 308).
`config_profiles_ui.cpp` then `#include`s at line 633 and defines the **same**
`static` function at line 492. Both `static` definitions coexist in the same TU
(the compiler silently picks one — either may win depending on flags/optimizer).

The glow timer logic MUST be placed in **both** copies to guarantee it runs.
Similarly for other duplicated stubs: `update_profile_state_label()`,
`update_profile_action_buttons()`, `set_profile_status_text()`, `is_elevated()`,
`apply_system_titlebar_theme()`.

All these are in the common section of `config_profiles_ui.cpp` (before line 345
`#ifndef GREEN_CURVE_SERVICE_BINARY`) and in `main_shell.cpp` lines 295-312.
Future changes to these functions should also duplicate across both copies.

## 2026-06-29: fix glow timer — stub in main_shell.cpp takes priority over config_profiles_ui.cpp

**Critical architectural finding:** `main_shell.cpp` contains empty stubs for several
functions (e.g. `update_background_service_controls()` at line 308) that serve as
the **actual compiled implementations** for the GUI binary, even though the real
implementations exist in `config_profiles_ui.cpp:492` (which is `#include`d by
`main_shell.cpp` at line 633). Both are `static` functions in the same TU.

The glow timer start/stop MUST go into the **stub** at `main_shell.cpp`, not into
the real function in `config_profiles_ui.cpp`. The same applies to:
`update_profile_state_label()`, `update_profile_action_buttons()`,
`set_profile_status_text()`, `is_elevated()`, `apply_system_titlebar_theme()`.

This happens because `config_profiles_ui.cpp` guards its GUI section with
`#ifndef GREEN_CURVE_SERVICE_BINARY` (lines 345-1071), and `main_shell.cpp`
provides fallback stubs that are compiled for BOTH GUI and service builds.
In the GUI build where both exist, the first definition (stub) wins.

## 2026-06-29: breathing glow on service install controls when service not installed

Added a sinusoidal breathing-glow animation on the service install checkbox and
description label when the background service is not installed or broken.

**Changes:**
- Added `SERVICE_GLOW_TIMER_ID 4`, `serviceGlowTickMs` state field in `AppData`
- `update_background_service_controls()` starts/stops the glow timer based on
  `!backgroundServiceInstalled || backgroundServiceBroken`
- Timer handler (40ms interval) invalidates the checkbox + status label
- `draw_themed_button()`: checkbox border pulses between `COL_BUTTON_BORDER` and
  `COL_CURVE` (green accent) using `sinf`-based lerp (~2.5s period)
- `WM_CTLCOLORSTATIC`: service enable label + status label text pulses between
  `COL_TEXT` and `COL_CURVE` in sync with the checkbox border

**Files:** `app_shared.h`, `config_profiles_ui.cpp`, `main_runtime_ui.cpp`,
`ui_main_window.cpp`

## 2026-06-29: switch Windows arm64 to Zig (durable LLD fix), two cosmetic GUI fixes

**arm64 linker bug — permanently fixed.** Switched Windows arm64 GUI and service
from llvm-mingw/LLD to Zig's bundled clang/LLD (`zig c++` with target
`aarch64-windows-gnu`).  The llvm-mingw aarch64 COFF LLD "misaligned ldr/str
offset" bug was layout-sensitive and required escalating workarounds across
builds 162→330 (`-O2` → `-O1` → `-fno-lto` → section splitting removal).  Zig's
LLD doesn't have this bug, so we can use full `-Oz -flto` with all section
optimizations.

Changes in `build.py`:
- `get_windows_gui_compile_command(arch="arm64")` now returns `ZIG_EXE c++ ...`
- `get_windows_service_compile_command(arch="arm64")` same
- Removed `windows_flags_for_arch()` (dead code)
- Removed old `gui_arch_opt` / `service_arch_opt` workaround variables
- Updated regression tests to check for Zig path instead of old workaround strings

## 2026-06-29: fix VF lock checkbox tick gray overdraw, service label font color

Two cosmetic GUI fixes:

1. **VF curve lock checkbox tick looked gray** (build 331). Root cause:
   `draw_lock_checkbox()` is split across `main_shell.cpp` (body) and `main.cpp`
   (tail after `#include`). The tail at `main.cpp:640` had a redundant second
   `draw_checkbox_tick_smooth(hdc, &box, COL_LABEL)` call that overdrew the
   correct white tick `RGB(0xE8,0xF2,0xFF)` from `main_shell.cpp:728` with gray
   `RGB(0xA0,0xA0,0xB0)`. Fix: removed the redundant `COL_LABEL` draw call.

2. **Service install description/label text used grayed-out font**. Root cause:
   `WM_CTLCOLORSTATIC` in `ui_main_window.cpp:683` defaulted ALL static controls
   to `COL_LABEL`. Added specific checks for `SERVICE_ENABLE_LABEL_ID`,
   `SERVICE_STATUS_ID`, and `LOGON_HINT_ID` to use `COL_TEXT` instead.

## 2026-06-29: no-snapshot service start is non-mutating; adjacent service install; 0.17 bump

Fresh 0.17 build 324 logs after uninstalling the old service, replacing both
binaries, starting the GUI, and installing/starting the service showed an
unexpected auto-apply:

- Service install correctly staged/hardened the adjacent binary at
  `C:\Program Files\greencurve\greencurve-service.exe`.
- The service startup coordinator found `recoveryRestart=0` / no restart
  snapshot, reset to defaults, then ran active-session reconcile.
- `service_reconcile_active_session_apply()` resolved `[profiles] logon_slot=1`
  for the already-logged-in admin user and applied slot 1 before the user's
  manual Apply click.
- The GUI then reported "No changes to apply" because the hardware already
  matched the loaded profile. IPC was not desynced; the service had really
  auto-applied the logon slot.

Corrected invariant: a service start with no restart snapshot is **not** proof
that Green Curve owns the hardware state and must be non-mutating. Installing,
repairing, manually starting, or auto-starting the service while a user is
already logged in no longer resets the GPU and no longer synthesizes a logon
profile apply. Hardware writes happen only through explicit GUI/CLI apply/reset,
real Windows session-change logon/switch events, or restart-snapshot recovery.

Implementation notes:
- Removed `service_reset_to_defaults_on_resume()` and the synthetic
  `service_reconcile_active_session_apply()` path from no-snapshot startup.
- `service_startup_coordinator_thread_proc()` now logs
  `no restart snapshot; leaving hardware unchanged...` and records final active
  desired state without writing hardware.
- Source checks now forbid the removed reset/reconcile hooks in no-snapshot
  startup and forbid `service start reconcile` in the session router.
- Service install now uses the current executable directory for side-by-side
  GUI/service layout, while still hardening/verifying the adjacent directory and
  binary DACL before SCM registration. Pipe identity compares against the
  SCM-registered service binary path, and uninstall restores inherited DACLs for
  both the captured SCM binary path and its adjacent directory.
- Version metadata was bumped to 0.17. The per-row VF lock checkbox also ignores
  `WM_LBUTTONDBLCLK` so the OS-classified double-click half cannot advance from
  FLATTEN directly to HARD.
- The Windows ARM64 GUI re-hit llvm-mingw/LLD `misaligned ldr/str offset` after
  the adjacent uninstall cleanup grew the unity source. The final scoped
  workaround keeps ARM64 GUI at `-O1 -fno-lto` and filters
  `-ffunction-sections`, `-fdata-sections`, and `-Wl,--gc-sections` for that
  binary only. x64 and the ARM64 service keep their prior flags.

Validation: `python build.py --test` passed with updated source checks; full
`python build.py` passed build 330 and produced:
- `greencurve-0.17-windows-x64.7z` (303,266 bytes)
- `greencurve-0.17-windows-arm64.7z` (344,165 bytes)
- `greencurve-0.17-linux-x64.7z` (91,085 bytes)
- `greencurve-0.17-linux-arm64.7z` (104,796 bytes)

---

## 2026-06-29: app-start profile reset skip, quieter diagnostics, ARM64 service link workaround

Follow-up on the same RTX 3070/0.16 reporter log. The reporter was still on the
old build (0.16 build 314/protocol 7), not a build containing the service/fan/VF
hardening from 2026-06-28.

- **18:46 event interpretation:** the log line at `2026-06-28 18:46:10` was
  `kind=gui ... serviceRunning=1 serviceAvailable=1`, not a `kind=service`
  startup. The GUI launched, saw `[profiles] app_launch_slot=1`, and ran
  `maybe_load_app_launch_profile_to_gui: applying slot 1 with reset-before-apply`.
  That reset the OC baseline and then re-applied the slot, which explains the
  visible stock-baseline dip while the service process stayed alive.
- **Fix:** normal GUI app-start auto-apply now asks the background service for
  active desired state and calls `desired_settings_match_active_service_intent()`
  before setting `resetOcBeforeApply`. If the service already owns the same
  Green Curve intent (curve/lock/offsets/power/fan intent), the GUI only loads
  the slot into the editor, updates `selected_slot`, logs
  `already active in background service; skipping reset-before-apply`, and does
  not disturb the GPU.
- **Logging:** high-frequency GPU-offset display and global-control refresh logs
  are now `debug_log_on_change`; GUI tail-drift diagnostics now log first,
  changed, or reappeared drift shape instead of every telemetry poll. The service
  VF drift monitor from the previous fix remains the path that actually queues a
  conservative reapply.
- **Build quirk:** the Windows ARM64 service hit the known llvm-mingw/LLD
  `misaligned ldr/str offset` linker bug at the old `-O2` workaround after this
  source growth. `service_arch_opt` now matches the GUI's
  `["-O1", "-fno-lto"]` on ARM64 only; x64 remains `-Oz`+LTO.

Validation: `python build.py --test` passed after the source checks were updated;
full `python build.py` passed build 323 (Windows/Linux x64+arm64 and all release
archives). The existing duplicate CFG symbol linker warning still appears on
Windows x64.

---

## 2026-06-28: 0.16 service reset race, fan intent, VF drift, and Blackwell label fixes

Implemented the fix plan from the RTX 3070 / 0.16 report and the separate RTX 5070
best-effort-label observation.

- **Service startup race root cause:** the 0.16 log showed `service_reconcile_active_session_apply()`
  applying the active user's profile while the independent no-snapshot startup worker
  later ran `service_reset_to_defaults_on_resume()`, resetting OC/fan to stock while
  the service stayed alive. `service_launch_startup_coordinator()` now owns both paths:
  restart snapshots run the recovery reapply path alone; no-snapshot starts run
  reset-to-defaults first, then active-session reconcile in the same worker.
  **Superseded 2026-06-29:** no-snapshot startup is now non-mutating, because fresh
  0.17 logs showed reset/reconcile still caused surprising auto-apply on service install.
- **Fan Save root cause:** Green Curve was conflating live NVML fan policy with Green
  Curve fan intent. External fan controllers can make NVML report manual,
  but that must not make Green Curve save/show Fixed Custom when the profile/GUI intent
  is Default/Auto. Added `current_green_curve_fan_intent_*()` helpers, changed service
  snapshots/control state to report intent, kept live fan telemetry separate, and made
  Profile Save preserve the visible GUI fan mode on the no-edits path.
- **VF drift:** tail drift is no longer just a GUI-side diagnostic. The service main-loop
  now calls `service_check_active_vf_drift_monitor()` from `source/main_service_vf_drift.cpp`,
  which only acts when active desired VF intent exists, hardware is ready, no recovery/reapply
  is running, and drift is confirmed across samples. It caps/backoffs reapply queues and uses
  the existing recovery reapply worker rather than writing from the monitor path.
- **Blackwell/best-effort label:** Windows backend selection now uses shared
  `vf_backend_for_architecture()`. NVAPI architecture/PCI query status and selected
  backend are logged, and a same-PCI last-known known backend is retained after transient
  metadata failures so a known GPU does not randomly downgrade to future/best-effort.
- **Regression coverage:** added pure backend mapping tests plus source guards for
  startup reset-before-reconcile ordering, fan intent preservation, VF drift gating, and
  metadata-backend retention.
- **ARM64 build note:** full matrix initially re-hit the known llvm-mingw/LLD
  Windows-arm64 GUI `misaligned ldr/str offset` bug after the source growth. Targeted
  flag testing showed `gui_arch_opt = ["-O1", "-fno-lto"]` links; x64 remains unchanged
  and the service kept its separate O2 workaround at that time. Superseded on
  2026-06-29: service ARM64 now also uses `["-O1", "-fno-lto"]`.

Validation: `python build.py --test` passed; full `python build.py` passed (build 321,
all Windows/Linux x64+arm64 targets and release archives). The existing duplicate CFG
symbol linker warning still appears on Windows x64.

---

## 2026-06-28: Two VF-curve lock-checkbox GUI bug fixes (build 318; root-cause note corrected 2026-07-12)

Fixed two user-reported bugs in the owner-drawn per-row "Lk" lock checkboxes.

- **Corrupted FLATTEN tick (rendering).** `draw_lock_checkbox()` (`main_shell.cpp`) drew
  its checkmark with a raw GDI `Polyline` (no anti-aliasing, gray `COL_TEXT`, fixed pen
  width), so the tick looked jagged and off-color next to the themed checkboxes
  ("Background service installed" etc.). Now it calls the shared
  `draw_checkbox_tick_smooth(hdc, &box, RGB(0xE8, 0xF2, 0xFF))` (GDI+ `GDP_SMOOTH_AA`),
  which was already in scope (`main_runtime_ui.cpp` is `#include`d before the function).
  The HARD `●` dot is unchanged (no themed counterpart).
- **Single click advances twice (input).** The lock-checkbox `WM_COMMAND` branch
  (`ui_main_window.cpp`) matched on the control ID only, unlike sibling branches
  that checked `BN_CLICKED`. Restricting it to `BN_CLICKED` was correct. The original
  claim that `WS_TABSTOP` caused `BN_SETFOCUS` was not: focus notifications require
  `BS_NOTIFY`, which these controls do not set. `BS_OWNERDRAW` does automatically
  emit a distinct `BN_DBLCLK`, and the ID-only branch accepted that and every other
  notification. The exact historical producer of the reported apparent single-click
  sequence was therefore unproven until the 2026-07-12 gesture/state-stamp hardening.

Regression at the time: four source checks in `build.py`
(`draw_checkbox_tick_smooth` present + `Polyline(hdc, pts, 3)` forbidden in
`main_shell.cpp`; a `BN_CLICKED` gate and cycle diagnostic present in
`ui_main_window.cpp`). These source-only input checks were superseded by compiled
policy/native-control coverage on 2026-07-12.

Validation: `python build.py --test` passed; full `python build.py` passed (build 318,
all four x64/arm64 Windows+Linux targets, `-Werror`). GUI visual check left to the user.

---

## 2026-06-28: Codex Security findings hardening pass

Implemented the repository security-finding fix plan while preserving the
multi-user service model and unsupported/new NVIDIA GPU best-effort writes.

- Windows service install now stages `greencurve-service.exe` into
  `%ProgramFiles%\Green Curve`, hardens/verifies the directory and binary DACLs,
  and registers only that protected path with SCM.
- `%ProgramData%\Green Curve` and `shared-profiles.ini` writes now fail closed
  when protected DACL application/verification fails; `BUILTIN\Users` read
  access is preserved.
- IPC protocol bumped to 8. Wire booleans use `gc_bool8` and request/response
  validators canonicalize malformed non-`0/1` flags before privileged service
  logic or GUI adoption.
- APPLY/RESET validates requested GPU index and PCI identity before mutating
  selected GPU state. Windows restart-reapply snapshots now carry target
  `GpuAdapterInfo`; old/no-identity snapshots are cleared and unmatched targets
  skip reapply instead of using an ordinal fallback.
- Active-session debounce and service user-path caching now key by
  `{WTS session id, user SID}` so numeric session-id reuse cannot reuse paths or
  suppress a different user's profile apply.
- Fixed `abs(INT_MIN)` in profile repair, elevated-helper argv quoting,
  service crash artifact directory selection, Linux socket I/O deadlines,
  Linux `active.bin` deletion on reset, Linux protected systemd daemon staging,
  and toolchain cache verification against pinned executable digests.

Validation:
- `python build.py --test` passed.
- `python build.py --test --asan` passed.
- `python build.py --target windows --check` passed (existing duplicate CFG
  symbol linker warning still appears).
- `python build.py --target linux --check` passed.
- Full `python build.py` passed (build 317) and produced:
  - `greencurve-0.16-windows-x64.7z` (299,578 bytes)
  - `greencurve-0.16-windows-arm64.7z` (318,139 bytes)
  - `greencurve-0.16-linux-x64.7z` (91,051 bytes)
  - `greencurve-0.16-linux-arm64.7z` (104,807 bytes)
- During the full build, llvm-mingw aarch64 wrote the GUI output directly to
  the final `.exe` path despite `-o <path>.exe.new`; `finalize_output()` now
  accepts that only when the output timestamp proves it was produced by the just
  completed compile.

---

## 2026-06-28: compiler vendor switch to own GitHub Release

Changed `build.py` to download Zig 0.13.0 and llvm-mingw 20260519 from the
project's own GitHub Release (`Compilers-1.0`) instead of upstream URLs.

- New `_resolve_archive()` helper checks `compilers/<tool>/` locally first
  (vendor fallback), then downloads from GitHub Release.
- Removed hardcoded ziglang.org and mstorsjo/llvm-mingw URLs.
- Existing SHA-256 checksums kept; cross-verified against upstream
  `ziglang.org/download/index.json` before committing.
- Created `compilers/` directory with per-tool `manifest.json`, license
  files, and README. Actual archive blobs hosted on GitHub Releases (not in
  repo tree, due to file-size limits).
- `compilers/` archive blobs excluded via `.gitignore` (matched existing
  `*.zip` rule; added `*.tar.xz`).
- Build check (`python build.py --check --target windows`) passed.

---

## 2026-06-21: release audit for 0.16, known GPU-family policy, arm64 GUI build fix

Release audit changes:
- Version aligned to **0.16** (`VERSION`, `APP_VERSION` fallbacks in
  `app_shared.h`/`linux_port.h`, `version.nsh` generated by the build). Build
  number after the successful release build: **314**.
- Pascal, Turing, Ampere, Lovelace, and Blackwell are now treated as tested known
  VF backends. `gpu_family_uses_best_guess_backend()` returns true only for
  `GPU_FAMILY_UNKNOWN`, and the Pascal/Turing/Ampere `VfBackendSpec` entries no
  longer set `bestGuessOnly`.
- Only unrecognized future NVIDIA GPU families use the best-effort fallback
  backend. The GUI warning remains write-enabled, shows a TaskDialog checkbox
  (`pszVerificationText`), and persists `[warnings]
  hide_unrecognized_gpu_warning=1` when the user disables it. Session-only
  suppression still prevents repeat prompts in one run.
- Cleaned stale release-facing text: README probe wording, Linux TUI banner,
  generated `--write-assets` README/unit text, and Linux probe report guidance now
  describe the daemon-backed native Linux path instead of the old scaffold state.
- Audited the current shared-logon combo work and kept the unified item-data
  selector. Fixed new GUI status strings to ASCII hyphens so ANSI Win32 window
  text does not mojibake.

Build fix:
- Full `python build.py` initially failed at Windows arm64 GUI link with the
  known llvm-mingw/LLD `misaligned ldr/str offset` bug. Temporary flag trials
  showed `-Wl,--icf=none` did **not** help, while `-O2` plus the existing
  `-fno-lto` did. `get_windows_gui_compile_command()` now applies
  `gui_arch_opt = ["-O2", "-fno-lto"]` for arm64 GUI only; x64 keeps `-Oz` + LTO.

Validation:
- `python build.py --test` passed.
- `python build.py --test --asan` passed.
- `python build.py` passed and produced:
  - `greencurve-0.16-windows-x64.7z` (296,241 bytes)
  - `greencurve-0.16-windows-arm64.7z` (312,295 bytes)
  - `greencurve-0.16-linux-x64.7z` (89,641 bytes)
  - `greencurve-0.16-linux-arm64.7z` (102,891 bytes)
- The duplicate `__guard_check_icall_fptr` linker warning still appears on x64
  Windows and remains the documented cosmetic CFG override warning.
- Source-size audit still reports the pre-existing F-MAINT-1 large-file warning
  (16 files over the guideline); not split during this release pass to avoid risky
  broad refactors.

---

## 2026-06-21: restricted user can auto-apply a shared profile at logon (policy fix)

Bug: with `[policy] restrict_non_admin_to_shared=1`, a standard user could not
auto-apply a loaded admin-shared profile at logon — and, inversely, the
service-side logon router applied a non-admin's per-user `logon_slot` custom OC
with **no** policy check (a bypass). Root cause: the shared-only policy was
enforced only in the interactive `SERVICE_CMD_APPLY` handler and keyed off the
GUI-only `g_app.loadedSharedSlot`, which logon auto-apply never sets (it loads
the per-user `logon_slot` via `load_profile_from_config`).

Fix (decisions: user picks a specific shared profile; enforce policy on ALL
logon paths):
- New per-user **`[profiles] logon_shared_slot`** (a SHARED BANK slot) = "apply
  admin shared profile N at my logon". Set via **"Shared profiles… → Apply
  shared profile N at every logon"** (`show_shared_profiles_menu`,
  `SHARED_PROFILE_LOGON_MENU_BASE`). Persisted by both config rewriters
  (`save_profile_to_config`/`clear_profile_from_config`); included in
  `should_enable_startup_task_from_config`.
- Pure, unit-tested decision **`resolve_logon_profile_source()`** (`app_shared.cpp`,
  declared in `app_shared.h`): explicit published shared choice wins for anyone;
  a restricted (policy && !admin) user otherwise gets ONLY the machine-wide
  shared default, never per-user custom OC; admins/unrestricted keep
  per-user-first. build.py harness returns 140–149; INI roundtrip 150–153.
- Client logon paths (`apply_logon_shared_slot_if_configured` in
  `config_profiles_ui.cpp`; CLI `applyConfig` branch in `entry.cpp`) load the
  bank copy and tag `g_app.loadedSharedSlot` → `SERVICE_REQUEST_FLAG_SHARED_SLOT`;
  restricted user with only a per-user `logon_slot` is skipped with guidance
  instead of a rejection.
- Service resolver (`service_session_logon_resolve_and_load_profile`) reworked to
  use the pure decision + `service_session_user_is_local_admin()`
  (`WTSQueryUserToken` + `token_is_local_admin`) + `get_machine_restrict_policy`,
  closing the bypass. Heavy debug logging of the resolve branch.
- Wiki: config-profiles.md (Startup and logon, Shared-only policy),
  windows-architecture.md (Logon behavior, Security), build.md (ARM64 notes).
  **Windows-only** — Linux has no shared-bank policy.
- **arm64 GUI LTO fallout (build fix):** the new GUI menu code tipped the
  Windows-arm64 GUI binary over the known llvm-mingw aarch64 "misaligned ldr/str
  offset" LLD bug — but via **LTO**, not the `-Oz` lever the service hit.
  Bisected (file → `ui_main_window.cpp`; lever → `-flto`, NOT `-Oz`/`-O2`/`--icf`).
  Fix: `get_windows_gui_compile_command` now appends `-fno-lto` for arm64
  (`gui_arch_opt`), mirroring the service's `-O2` (`service_arch_opt`). Guarded by
  a `require_text(build_script, "-fno-lto", ...)` source check. x64 keeps LTO.
- **Verified:** `python build.py` full matrix (win+linux × x64+arm64, all 6
  binaries link), `--test`, `--asan`, `--target linux --check` all green.
- **Stale-risk / open:** a live admin+standard end-to-end (set policy, pick shared
  logon, log off/on, confirm bank copy applied and per-user custom NOT applied for
  the non-admin) still to be recorded here once run on real hardware.

---

## 2026-06-20: arm64 robustness (maximize odds it works on real arm64, no HW)

Research: NVIDIA ships a Linux-aarch64 **discrete-GPU** driver with NVML; NvAPI
(`libnvidia-api.so.1`) is listed but aarch64 packages have historically dropped
libs, so verify per version. **Tegra/Jetson** = different L4T stack, no NVML
(unsupported). Windows-arm64 dGPU driver basically doesn't exist yet.

Implemented + verified here (x64 + arm64 cross-compile + `--test` green; x64
Windows still byte-identical):
- **WS2 compile-time layout proof:** `gpu_core.h` gained `offsetof`/`sizeof`
  `static_assert`s on the bitfield NVAPI/NVML structs + a little-endian assert.
  These run during the arm64 cross-compile → any AAPCS64 divergence fails the
  build. Confirmed identical on `x86_64-linux-gnu` and `aarch64-linux-gnu`.
- **WS4 runtime self-validation + fail-safe** (`linux_backend.cpp`):
  `nvapi_status_name` (names `-9` INCOMPATIBLE_STRUCT_VERSION etc.);
  `linux_backend_curve_plausible` (populated/range/monotonic check);
  apply path **REFUSES VF writes when the read is implausible** → NVML-only
  fallback (a layout mismatch can't write garbage); `linux_backend_self_test`
  (`--self-test`) reads getInfo+getStatus+**getControl** (write struct, read-only)
  + NVML ranges and reports whether writes should work — no GPU changes.
  `linux_gpu.cpp` probe detects Tegra via `/etc/nv_tegra_release`.
- **WS1 driver inspection (no HW):** `build.py --inspect-aarch64-driver <dir>`
  finds `libnvidia-{ml,api}.so` in an extracted aarch64 driver and verifies our
  symbols via bundled `llvm-nm` (reads aarch64 ELF). Accurate verdict
  (FULL / NVML-ONLY / NVML-MISSING).
- F-ARM64 source checks added; support matrix in `linux-scaffold.md`.

---

## 2026-06-20: build.py — multi-arch matrix + 7-Zip release archives

`python build.py` (no args) now builds the **full matrix** by default: Windows +
Linux, each x64 + arm64, and packages each into
`greencurve-<VERSION>-<os>-<arch>.7z` with the binaries (+ README + LICENSE) under
a `greencurve/` subfolder (the Linux binary renamed to `greencurve` inside).
`--target {windows,linux,all}` and new `--arch {x64,arm64,all}` (both default
`all`) scope it; `--no-package` skips archiving. 7-Zip located on PATH or the
standard install dir.

ARM64 enablement (all 4 targets build clean, verified):
- New `windows_flags_for_arch`/`linux_flags_for_arch` + `*_output_for_arch` +
  `package_release_archive` + `find_seven_zip`. Compile fns take `arch=`.
- **Windows arm64**: `-target aarch64-w64-mingw32`; x86 CET → `-mbranch-protection=
  standard`; `-mguard=cf` dropped (cfg_glue CFG override is x64-only; the file
  still builds for `initialize_process_mitigations`). `main_diagnostics.cpp` VEH
  thread-exit redirect gained an aarch64 path (`Pc`/`X0`/`Sp`) under
  `#if defined(__aarch64__)` (x64 path unchanged → x64 .exe byte-identical).
  **Per-binary opt quirk**: GUI links at `-Oz` but the *service* hits LLD
  "misaligned ldr/str offset" at `-Oz` → service built at `-O2` (GUI fails at
  `-O2`; opposite — LLD aarch64-COFF layout sensitivity). `--no-relax` is ELF-only;
  `-mno-outline` didn't help the service.
- **Linux arm64**: `-target aarch64-linux-gnu` (+ `-O2`).
- F-LNX/build source checks added (default all, arm64 triples, branch-protection,
  package fn, archive name, 7z, arm64 VEH). `--test` green.

Follow-up tuning (same day):
- **x64/linux-x64 NOT sacrificed**: arm64 changes are isolated behind
  `arch=="arm64"` / `#if defined(__aarch64__)`; x64 Windows .exe byte-identical,
  x64/linux-x64 flags untouched.
- Linux arm64 gained `-mbranch-protection=standard` (BTI/PAC), matching Windows
  arm64. (Windows arm64 dropped CFG — its cfg_glue override is x64-only — and uses
  BTI/PAC instead, the stronger HW CFI; the arm64 service's `-O2` is a forced LLD
  workaround, not a security/perf compromise.)
- **Packaging arch guard**: `detect_binary_arch()` reads each binary's real PE
  `Machine` / ELF `e_machine`; `package_release_archive` aborts on any mismatch,
  so a cross-arch bundle (arm64 GUI + x64 service, or a stale project-folder
  binary) is impossible. Verified detection on all 6 outputs + the abort path.
  Inside archives binaries use canonical names; on disk arm64 outputs carry an
  `-arm64`/`aarch64` suffix (no collision with x64).
- **Per-target output folders (revised):** dropped the suffixed root binaries —
  each `(os,arch)` now builds into its own isolated `dist/<os>-<arch>/greencurve/`
  with canonical names (`greencurve.exe`/`greencurve-service.exe`/`greencurve`),
  wiped fresh per target so no two targets share an output or temp path. That
  payload folder is what the 7z wraps (`greencurve/` root). Project root no longer
  holds loose binaries. `dist/` gitignored. x64 .exe still byte-identical.

---

## 2026-06-20: started — complete Linux support (native NvAPI+NVML backend)

### Why / direction (approved plan)
The old Linux `--apply-config` blocker ("no verified VF read/write surface on
Linux") is **obsolete**. The *same*
undocumented NvAPI `nvapi_QueryInterface(id)` mechanism we use on Windows via
`nvapi64.dll` is available on Linux via **`libnvidia-api.so.1`** (full 128-pt VF
read + per-point offset write; reported working on RTX 5090 / driver 590.x). NvAPI IDs +
struct layouts are driver-ABI constants → identical cross-OS. NVML is already
cross-platform (`libnvidia-ml.so.1`).

Approved approach: **shared neutral core (`gpu_core.h`) + platform shim
(`platform.h`)** so ~85–90% of the backend compiles for both OSes; **root systemd
daemon + Unix-socket IPC** for the privilege boundary;
**TUI/CLI clients only** (no native GUI). Full Windows feature parity, **not**
gated as experimental.

### Hard constraints captured
- **glibc-dynamic, NOT static-musl.** musl's static `dlopen` is a failing stub;
  the NVIDIA libs are glibc `.so`s loaded at runtime. The Linux artifact must
  switch to `x86_64-linux-gnu` (drop `-static`, add `-ldl`). [Phase 1]
- **NvAPI Linux ABI quirks:** error codes are **negative** (`-1` generic, `-9`
  INCOMPATIBLE_STRUCT_VERSION) vs Windows `0x8000xxxx` (success is `0` both);
  `SetControl` mask must have **exactly one bit set per call**; version field is
  `(ver<<16)|size` (same macro). Audit status checks in Phase 0c.
- **No VEH port:** Linux driver-reload resilience = systemd `Restart=on-failure`
  + startup restart-reapply, not the Windows VEH.

### Landed (Phase 0a) — platform shim, additive, Windows unchanged
- New `source/platform.h` + `platform_win32.cpp` + `platform_posix.cpp`:
  `pl_lib_open/sym/close`, `pl_open_driver_library` (NvAPI/NVML sonames),
  `pl_sleep_ms`, `pl_atomic_*32`, `pl_mutex_*`, `pl_thread_*`,
  `gc_snprintf/vsnprintf/strlcpy/strlcat` (StringCch* replacements), and
  `pl_run_capture()` (subprocess stdout capture: Win32 CreateProcess+pipe ↔
  POSIX posix_spawnp+pipe+poll, both with timeout/kill). On Windows the wrappers
  expand to the identical Win32 calls.
- `platform_win32.cpp` added to `WINDOWS_SOURCE_FILES`. Windows GUI+service build
  clean; `python build.py --test` passes; `platform_posix.cpp` compiles clean for
  `x86_64-linux-gnu` (`-Wall -Wextra -Werror`).

### Landed (Phase 0b) — `gpu_core.h` extraction, Windows byte-identical
- New `source/gpu_core.h`: all platform-neutral data (NVAPI entry-point IDs +
  private struct layouts, NVML typedefs/enums/structs, `VfBackendSpec`,
  `VFCurvePoint`, fan structs, `ControlState`, `GpuAdapterInfo`, `LockMode`,
  `DesiredSettings` + `validate_desired_settings_for_ipc()`, `lock_mode_sync_allowed()`,
  the service protocol enums + `ServiceSnapshot`/`ServiceRequest`/`ServiceResponse`,
  `NvmlApi`). Pulls in NO OS headers. `app_shared.h` now `#include`s it and keeps
  only Win32/UI/service decls (`AppData`, `CliOptions`, control IDs, colors, the
  `debug_log_on_change` StringCch macro). app_shared.h 1200→557 lines.
- **ABI trick:** the IPC structs' Win32 `DWORD`/`ULONGLONG`/`MAX_PATH` became
  `gc_u32`/`gc_u64`/`GC_REQUEST_PATH_MAX`. `gc_u32 == unsigned long` on Windows
  (the exact type `DWORD` is → all `%lu` format strings + DWORD assignments keep
  compiling) and `uint32_t` on Linux; both 32-bit → identical wire layout.
  Result: **Windows .exe/.exe sizes byte-identical** to pre-extraction
  (565,760 / 225,792) — zero codegen change. `--test` passes.
- build.py source checks: `shared_h`/`app_shared_h` now point at a concatenation
  of app_shared.h + gpu_core.h (`build-tmp/_shared_header_surface.h`) so the
  "shared header surface" `require_text` invariants pass regardless of which
  header a symbol moved to. `gpu_core.h` verified to compile standalone for glibc.
### Landed (Phase 1 foundation) — `win32_compat.h`, app_shared.h Linux-includable
- New `source/win32_compat.h` (Linux-only): opaque `void*` handle types (HWND,
  HMODULE, HICON, …), Win32 integer aliases (DWORD=uint32_t, LONG, ULONGLONG, …),
  `WCHAR`, `MAX_PATH`, `RGB`/`WM_APP` macros, and `CRITICAL_SECTION` backed by a
  real recursive pthread mutex (so config-storage locking works in the daemon).
  `#error`s if ever included on Windows.
- `app_shared.h`: the `<windows.h>`/commctrl/shlobj include block is now
  `#if defined(_WIN32) … #else #include "win32_compat.h" #endif`; it also includes
  `platform.h`; `debug_log_on_change` has a Linux variant using `gc_snprintf`/
  `gc_strlcpy`. Result: **app_shared.h (incl. AppData/CliOptions/HeapBuffer/g_app)
  compiles clean on x86_64-linux-gnu.** Windows .exe sizes still byte-identical
  (565,760 / 225,792).

### Entanglement finding (drives remaining work)
The 4 "backend" files are NOT uniformly portable. Win32 GUI/CLI hit counts:
`gpu_backend.cpp` 2, `gpu_backend_apply.cpp` 2 (both just the GUI-process branch
calling `populate_edits`/`invalidate_main_window` — guardable) → the apply/curve
crown jewels are ~95% portable. But `main_runtime_nvml.cpp` (31 GUI/CLI hits,
incl. `parse_cli_options` with `CommandLineToArgvW`/`LPWSTR`) and
`main_fan_runtime.cpp` (15) interleave Win32 CLI/telemetry-GUI with the NVML
primitives → these need **decomposition** (split portable NVML/fan ops into a
shared TU) before they can compile on Linux. The VfBackendSpec dispatch tables
live in `main_gpu_front.cpp`.
### Landed (Phase 1) — glibc-dynamic Linux target + native driver probe
- build.py: `LINUX_TARGET` `x86_64-linux-musl` → `x86_64-linux-gnu`, dropped
  `-static`, added `-ldl -lpthread` (mandatory: static musl can't dlopen the
  glibc NVIDIA libs). Existing Linux sources rebuild clean.
- New `source/linux_gpu.cpp` + `linux_gpu.h`: self-contained native probe that
  loads `libnvidia-api.so.1` (NvAPI) + `libnvidia-ml.so.1` (NVML) via the shim
  and reports the read-only control surfaces — NvAPI QueryInterface→Init→
  EnumPhysicalGPUs→name+arch→`GpuFamily`, and NVML count/name/PCI/temp/power/
  clock + the GPC/mem **clock-offset range** (the writable OC surface, read
  read-only). Reuses gpu_core.h (no duplicated model); uses `nvapi_ok()`
  (status==0; Linux NvAPI errors are negative). `linux_gpu.h` is kept free of
  gpu_core.h so linux_main.cpp's `--probe` can call it without colliding with
  linux_port.h's parallel structs. **Linux binary compiles AND links**
  (greencurve-x86_64-linux-gnu, ~234 KB); Windows + `--test` still green.

### Landed (Phase 1, cont.) — shared VfBackendSpec tables + Linux VF read
- New `source/vf_backends.cpp` + `vf_backends.h`: the 6 `VfBackendSpec` tables
  (`g_vfBackend*`) + their layout static_asserts moved out of `main.cpp` (were
  `static const`, now `extern const`, external linkage) into a TU compiled on
  BOTH platforms, plus a pure `vf_backend_for_architecture(arch, &family)`. Added
  to both source lists; build.py static_assert source-checks repointed to
  vf_backends.cpp. Windows .exe still byte-identical (565,760 / 225,792).
- `linux_gpu.cpp` now does a real **VF-curve read** in `--probe`: selects the
  family backend via the shared table and runs the same private-NVAPI
  getInfo(mask/numClocks)+getStatus(freq/volt) sequence as the Windows backend,
  reporting populated point count + the peak point's MHz/mV. Linux builds+links.

### Landed (Phases 2–5) — full Linux backend + daemon + systemd + checks
User has Linux NVIDIA hardware but wanted everything implemented HERE
(cross-compiled + link-verified) with ~zero Linux-side coding. Done:
- `linux_backend.{h,cpp}` — native apply: VF curve write (single-bit-mask control
  writes + 25-pass readback-verified correction loop, faithful port of
  apply_curve_offsets_verified), clock/mem offset, power %, GPU locked clocks
  (HARD pin), fan; FLATTEN lock = uniform tail floor. `nvapi_ok()` = status==0.
- `linux_daemon.{h,cpp}` — root Unix-socket daemon (`/run/greencurve/greencurve.sock`,
  0660 root:greencurve), binary ServiceRequest/Response, SO_PEERCRED audit,
  validate_desired_settings_for_ipc clamp, ServiceSnapshot builder, fan-curve
  reassertion thread, restart-reapply via `/var/lib/greencurve/active.bin`;
  `--service-install/--service-remove` (systemd unit Restart=on-failure + group).
- Client: `linux_port.h` unified onto `gpu_core.h` DesiredSettings; `--apply-config`
  blocker replaced with a daemon round-trip; new `--daemon`/`--service-*` roles;
  refreshed `--help`. `linux_gpu.cpp --probe` does a real VF read via vf_backends.
- Tests: F-LNX source-invariant checks (blocker gone, nvapi_ok negative-error,
  single-bit mask, IPC clamp, SO_PEERCRED, restart-reapply, glibc-dynamic target,
  shared VfBackendSpec). `python build.py --test` green; full Linux binary
  compiles+links (~256 KB); Windows .exe still byte-identical.

### Remaining / follow-ups
- On-HW validation of the VF write path (esp. Blackwell). Selective GPU-offset
  exclude-low not yet translated to per-point VF on Linux (global offset only).
  Multi-GPU defaults to ordinal 0. `main_runtime_nvml.cpp` etc. were NOT shared
  (too Win32-entangled — CLI/GUI interleaved); the apply algorithm is a faithful
  port with cross-references, not a shared TU.
2. **Apply pipeline reuse**: `gpu_backend.cpp`/`gpu_backend_apply.cpp` are ~95%
   portable (only the GUI-process branch — `populate_edits`/`invalidate_main_window`
   — needs a `#if defined(_WIN32)` guard) but link-depend on the whole GPU
   subsystem (`main_runtime_gpu/nvml.cpp`, `main_gpu_front.cpp`, `main_fan_runtime.cpp`)
   which interleaves Win32 CLI/telemetry-GUI (esp. `main_runtime_nvml.cpp`:
   `parse_cli_options`). Needs decomposition into portable cores + a Linux
   link-glue TU (`debug_log`, `g_app`, `set_message`, config get/set, trusted-path
   → `pl_run_capture`). This is the bulk of the port.
3. **Daemon**: `linux_daemon.cpp` — Unix-socket server (`SO_PEERCRED` auth), binary
   ServiceRequest/Response loop → reused apply/reset/snapshot handlers, fan
   reassertion thread, startup restart-reapply. Replace the `--apply-config`
   blocker (`linux_main.cpp:648`) with a socket round-trip; unify linux_port.h
   onto gpu_core.h `DesiredSettings`.
4. **systemd**: `--service-install/--service-remove` (Restart=on-failure unit +
   socket + `greencurve` admin group); `/etc/greencurve/shared-profiles.ini`.
5. **Tests/docs**: portable unit subset (`--target linux --test`), source-invariant
   checks, README/wiki, version bump.
- **Write-path validation is hardware-gated**: the VF write/apply path cannot be
  proven without a Linux NVIDIA box. Per the approved plan it is implemented
  assuming correctness, with the apply pipeline's existing post-write readback as
  the in-built safety check.

---

## 2026-06-20: implemented — shared-only policy (restrict non-admins to admin profiles)

### What / why
Admins on shared machines wanted to let standard users apply ONLY admin-defined
profiles, not arbitrary OC. File permissions on the GUI binary do NOT achieve
this (the service accepts APPLY from any active-session client — CLI, custom
build), so enforcement must be server-side. The active-session router also means
logon profile-apply no longer depends on the GUI, so locking down the GUI doesn't
break autostart.

### Implementation (all enforcement server-side)
- **Policy storage**: `[policy] restrict_non_admin_to_shared` in the protected
  `%ProgramData%` shared bank. `get/set_machine_restrict_policy` (set is admin-only).
- **Caller admin detection**: `token_is_local_admin(HANDLE)` checks TokenGroups
  for BUILTIN\Administrators INCLUDING `USE_FOR_DENY_ONLY`, so an unelevated admin
  still counts. Wired through `get_pipe_client_identity` → `service_caller_is_authorized`
  (new `callerIsAdmin` out-param). `current_user_is_local_admin()` is the GUI twin.
- **Protocol**: `ServiceRequest.flags` bit 30 = `SERVICE_REQUEST_FLAG_SHARED_SLOT`,
  slot in bits 8..15 (bit 0 stays the interactive flag). No version bump.
- **APPLY enforcement** (`main_service_server.cpp`): policy on + caller non-admin →
  require the shared-slot flag, then load the service's OWN copy of that shared
  slot from the bank and apply it (ignore client settings); reject anything else.
  RESET stays allowed (safe). Admins / unrestricted machines unaffected.
- **GUI**: `show_shared_profiles_menu` sets `g_app.loadedSharedSlot`;
  `populate_desired_into_gui` clears it; `service_client_apply_desired` tags the
  request only when the editor holds the unmodified shared profile. A restricted
  user sees a service-status hint; a custom apply gets the clear server rejection.
  Admin toggle: right-click the share checkbox → "Restrict standard users to
  shared profiles"; CLI `--set-restrict-shared <0|1>`.

### Tests
- Compiled (codes 135–139): shared-slot flag encode/decode (slot in bits 8..15,
  no collision with the interactive bit).
- Source checks (F-15-014): `token_is_local_admin`, `restrict_non_admin_to_shared`,
  `set_machine_restrict_policy`, the rejection message, `SERVICE_REQUEST_FLAG_SHARED_SLOT`
  in both service + GUI, `--set-restrict-shared`, `restricted_to_shared_profiles`.
- `python build.py` + `--test` + `--asan` + `--target linux --check` pass.

### Notes / possible follow-up
- The restricted GUI still shows editable OC controls (server rejects custom
  applies with a clear message); greying them out is optional polish.

---

## 2026-06-19: fixed — "service not responding" after switching user accounts (Build 288)

### Root cause (pre-existing; the per-user pipe ACL was redundant AND harmful)
`create_restricted_pipe_security_descriptor()` baked the **active console
user's SID** into the named-pipe ACL (`...(A;;GRGW;;;<userSid>)`, AU read-only).
After a logoff/login (or fast-user-switch) the now-active user is a *different*
SID, so their unelevated GUI was denied at `CreateFile`; the server's
`ConnectNamedPipe` on the stale-ACL listening instance never completed, so that
instance kept listening and rejecting the new active user **until a service
restart (reboot)** — exactly matching "works after reboot into the same account".

The per-user ACL was **redundant**: `service_caller_is_authorized()`
(`main_service_runtime.cpp`) already runs on EVERY request (incl. PING) and
rejects any caller whose pipe-handle-resolved session ≠ the active interactive
session. That server-side check (unspoofable, OS-resolved) is the real F-SEC-3
enforcement.

### Fix
- Pipe SDDL is now **user-agnostic**: `D:(A;;GA;;;SY)(A;;GA;;;BA)(A;;GRGW;;;AU)`
  (SYSTEM+Admins full, Authenticated Users read+write; `PIPE_REJECT_REMOTE_CLIENTS`
  still blocks the network). The new active user connects immediately; a
  non-active session still gets the clean server-side rejection message.
- `g_servicePipeRecycleEvent` (added with the active-session router) is now
  defense-in-depth only — comments updated; the ACL no longer depends on it.

### Security note
Not a weakening: active-session enforcement moved entirely to the robust
server-side check that was already authoritative. The pipe ACL still admits only
authenticated local users (no anonymous, no remote).

### Tests / wiki
- Source checks: require server-side `"Service control is restricted to the
  active interactive session"` + `get_pipe_client_identity` + `(A;;GRGW;;;AU)`;
  **forbid** `GRGW;;;%s` (no per-user baking). Updated the two old F-SEC-3
  pipe-ACL checks. `windows-architecture.md` Security section rewritten.
- `python build.py` (288) + `--test` + `--asan` + `--target linux --check` pass.

### Wanted
- Real-machine recheck: change OC in account A, log off, log in to B → B's GUI
  should reach the service immediately (no reboot). Confirm a *background*
  non-active session still gets "restricted to the active interactive session".

---

## 2026-06-19: fixed — "Startup task still exists after delete" + log-spam dedup (Build 287)

### Root cause (regression from the Build 285 direct-path change)
`set_startup_task_enabled_direct()` (`main_runtime_control.cpp`) accepted
`schtasks /delete` **exit 0 OR 1** as success. schtasks returns non-zero for
BOTH "already absent" and "access denied", so a delete that was actually denied
(a task created by an admin / at HighestAvailable that a non-elevated token can't
remove) was treated as success; the follow-up `wait_for_startup_task_state(false)`
then saw the task still present and returned the dead-end error
**"Startup task still exists after delete"** — and the wrapper did NOT fall back
to the elevated helper because that message isn't access-denied text. Before
Build 285 the non-elevated path always used the elevated helper, so this only
regressed when the direct-first path landed. There were also **no schtasks logs**
(query/create/delete), so it was undiagnosable from the user's logs.

### Fix
- Direct path now **verifies by STATE, not the exit code**: after `/delete` it
  waits for the task to disappear; if it persists it sets a new
  `outNeedsElevation` out-param and returns false. The wrapper
  `set_startup_task_enabled()` falls back to the elevated helper when
  `needsElevation || err_returned_access_denied(err)`. The elevated helper
  (already scoped to the requesting user via `--for-user`) can delete the
  protected task.
- Added targeted `debug_log` of the schtasks create/delete command name + exit
  code (NOT in the per-poll `/query`, which would re-spam).

### Log-spam reduction (without losing data)
- New `debug_log_on_change(fmt, ...)` macro (`app_shared.h`): logs a line only
  when its formatted text changes from the previous call **at that call site**
  (per-site static cache). Applied to the worst idempotent repeaters seen in a
  732-line capture: `vf_curve_global_gpu_offset_supported` (108×),
  `current_applied_gpu_offset_*: skipping detection` (68×),
  `scm: service binary directory is` (25×), `share-all-users controls: refreshed`
  (26×), `get_effective_control_state: using cached service state` (23×). Genuine
  per-snapshot telemetry (tail-drift, populate_global_controls) was left intact.

### Tests
- Source checks (F-15-010/011): `outNeedsElevation` + "needs elevation" +
  `schtasks /delete|create exit=` logging present; `forbid` the old
  `exitCode != 0 && exitCode != 1` accept-exit-1 logic and the old
  "still exists after delete" message; `debug_log_on_change` exists + applied.
- `python build.py` (287) + `--test` + `--test --asan` + `--target linux --check` pass.

### Wanted
- Real-machine recheck: disable a logon task that was created elevated /
  HighestAvailable from a non-elevated GUI → should now UAC-prompt once and
  succeed instead of erroring.

---

## 2026-06-19: finished — multi-user / restricted-user experience (Build 286)

### Problem
The Build 285 multi-user work was ~80% done but had a silent footgun and gaps:
1. **Headline feature broken.** Clicking "All users" wrote only `[profiles]
   logon_slot=N` to the bank; it did NOT publish the `[profileN]` data. The
   service logon resolver requires `is_profile_slot_saved(machinePath, slot)`, so
   the obvious admin action set a default that resolved to empty → restricted
   users got nothing.
2. **No on-demand read** of the admin's profiles (only an automatic logon apply).
3. **Fragile storage.** The bank lived next to the service binary and its path
   was resolved by querying the SCM and *parsing the quoted command line*.

### What shipped
1. **Active-user session router** (`main_service_sessions.cpp`, was in-flight)
   committed + covered by source checks: only the ACTIVE session drives the GPU;
   profile reapplied on logon/logoff/console connect-disconnect (FUS-safe,
   debounced) + on service (re)start; pipe ACL recycled per session change.
2. **Coherent share action** (`config_profiles.cpp`):
   `share_profile_slot_for_all_users()` = publish data + set all-users default;
   `unshare_profile_slot_for_all_users()` reverses both (default cleared only if
   it points at that slot). CLI `--share-slot` / `--unshare-slot`.
3. **Shared bank moved to `%ProgramData%\Green Curve\shared-profiles.ini`**
   (`resolve_machine_config_path` → `FOLDERID_ProgramData`; dropped SCM
   command-line parsing). Hardened **file + directory** DACL
   (`apply_protected_machine_config_dir_dacl`, SDDL `D:P(A;OICI;FA;;;SY)
   (A;OICI;FA;;;BA)(A;OICI;0x1200a9;;;BU)`). `migrate_legacy_machine_config()`
   moves the old `machine.ini` once at service start. `MACHINE_CONFIG_FILE_NAME`
   renamed `shared-profiles.ini`; `LEGACY_MACHINE_CONFIG_FILE_NAME` = `machine.ini`.
4. **GUI:** the overloaded "All users" button → a labeled **"Share slot N with
   all users"** checkbox (`SHARE_ALL_USERS_CHECK_ID`, bound to the selected
   profile slot; `update_share_all_users_check_state`). Right-click = advanced
   bank menu. New **"Shared profiles…"** button (`show_shared_profiles_menu`)
   lets ANY user load a published profile into the editor read-only and Apply it.
   New dedicated layout row (`sharedY`) between logon and service rows.
5. **Policy refinement** (`main_state_sync.cpp`): `%ProgramData%` is allowed for
   deliberately-shared, non-sensitive config (the bank); sensitive SYSTEM
   artifacts still never go there. `service_cleanup_legacy_programdata()` now
   preserves `shared-profiles.ini` while sweeping legacy sensitive files.

### Tests
- New compiled test (codes 130–134): shared-bank **directory** DACL round-trip.
- Source checks updated/added: `update_share_all_users_check_state`,
  `FOLDERID_ProgramData`, `migrate_legacy_machine_config`,
  `apply_protected_machine_config_dir_dacl`, `share_/unshare_profile_slot_for_all_users`,
  `--share-slot`/`--unshare-slot`, `SHARE_ALL_USERS_CHECK_ID`,
  `show_shared_profiles_menu`, active-session router anchors, `--for-user`.
- `python build.py` + `--test` + `--test --asan` + `--target linux --check` pass.

### Open / wanted
- Real-machine, two-account verification (admin shares slot N → restricted user
  gets it on logon AND can load it on demand; FUS reapply; legacy migration).
- `main_service_ipc.cpp` is now 1309 lines (F-MAINT-1 soft cap is ~800) — split
  candidate (e.g. carve out the machine-config/ProgramData helpers).

---

## 2026-06-14: implemented — machine-wide default logon profile for all Windows users (Build 285)

### What was missing
Profiles and logon startup tasks were strictly per-user. An admin who configured
"Apply profile X after user log in" only configured it for their own account;
restricted/standard users logging into the same machine did not get that profile
applied automatically, even though the background service runs as `LocalSystem`.

### What was implemented
1. **Machine-wide config file** (`machine.ini`) next to the installed service
   binary, storing `[profiles] logon_slot=<slot>`.
2. **Admin-only writes, user-readable**: the file gets a protected DACL
   (`SYSTEM` + `Administrators`: Full, `BUILTIN\Users`: Read). Writing helpers
   also require `is_elevated()`.
3. **Service session-change handling**: the service now accepts
   `SERVICE_CONTROL_SESSIONCHANGE` and spawns a thread on `WTS_SESSION_LOGON`.
   The thread resolves the user's per-user config; if no per-user `logon_slot`
   exists, it loads and applies the machine default. Per-user always wins.
4. **GUI "All users" button** next to the logon profile combo; sets the
   current per-user logon slot as the machine-wide default (or clears it when
   "Disabled" is selected). When the GUI is unelevated, clicking the button
   triggers a UAC `runas` launch of the same executable with
   `--set-machine-logon-slot <slot>` / `--clear-machine-logon-slot`, waits for
   completion, then refreshes the UI, so users never need to start the whole
   GUI as administrator. The button's control ID was also added to the
   `destroy_edit_controls()` whitelist so it survives GPU/service state
   transitions that rebuild the per-point edit controls.
5. **Machine-wide profile bank**: admins can publish full profile sections into
   `machine.ini` so restricted users without their own `config.ini` still get
   the saved OC profile applied via the service. Right-click the "All users"
   button for **"Publish slot N to all users"** / **"Clear machine-wide slot N"**.
6. **User-profile install warning**: when the service is installed from inside a
   user profile, the GUI status label warns that other users may be unable to
   launch Green Curve and recommends `%ProgramFiles%`.
7. **CLI**: `--set-machine-logon-slot <slot>`, `--clear-machine-logon-slot`,
   `--publish-slot-to-machine <slot>`, `--clear-machine-slot <slot>` (all require
   elevation).

### Source anchors
- `source/app_shared.h`: `MACHINE_CONFIG_FILE_NAME`, `machineLogonSlotCache`,
  helper declarations, machine profile bank menu IDs.
- `source/service_acl.h` / `source/service_acl.cpp`:
  `apply_protected_machine_config_dacl()` and `machine_config_dacl_is_hardened()`.
- `source/main_service_ipc.cpp`: `resolve_machine_config_path()` (queries SCM for
  the registered service binary directory, parsing the quoted command line),
  `get_machine_logon_slot()`, `set_machine_logon_slot()`, `clear_machine_logon_slot()`,
  `install_dir_is_under_user_profile_w()`, `service_install_dir_is_under_user_profile()`.
- `source/main_service_server.cpp`: `SERVICE_ACCEPT_SESSIONCHANGE`,
  `WTS_SESSION_LOGON` handler.
- `source/main_service_runtime.cpp`: `service_session_logon_thread_proc()`,
  `service_session_logon_resolve_and_load_profile()` (loads full profiles from
  `machine.ini`).
- `source/config_profiles.cpp`: `copy_profile_slot_to_machine_config()`,
  `clear_machine_profile_slot()`, `is_machine_profile_slot_saved()`.
- `source/config_profiles_ui.cpp`: `update_machine_logon_button_state()`,
  profile state label updates for shared machine bank slots.
- `source/ui_main_window.cpp`: `MACHINE_LOGON_BUTTON_ID` click handler,
  right-click context menu for machine profile bank, `run_elevated_command()` helper.
- `source/main_runtime_nvml.cpp`: CLI parsing for machine logon/profile-bank flags.
- `source/entry.cpp`: CLI handling and help text.

### Tests
- Source regression guards for all new source anchors + `SERVICE_ACCEPT_SESSIONCHANGE`.
- Unit test (codes 117–121): machine config DACL round-trip.
- `python build.py`, `--test`, `--test --asan`, `--target linux --check` pass.

---

## 2026-06-10: fixed — HARD pin reverted by snapshot sync before Apply/save (Build 272) + review hardening

### Root cause (supersedes the Build 265/269 "fixed" claims for pin apply/save)
The snapshot lockMode sync in `apply_service_snapshot_to_app()`
(`main_state_sync.cpp`) had **no dirty/intent guard**. After a FLATTEN apply, a
2nd checkbox click (FLATTEN→HARD, same ci/MHz — only the mode differs) set
`g_app.lockMode=HARD` + dirty, but the per-second telemetry snapshot still
carried the service's APPLIED mode (FLATTEN) at the matching lock point and
overwrote `lockMode`/`appliedLockMode` back to FLATTEN within ~1 s. Hence:
- Apply → `capture_gui_apply_settings` saw FLATTEN==FLATTEN → `lockChanged=false`
  → **"No changes to apply"** (the Build-265 comparison itself was correct; it
  compared already-reverted state).
- Save → `capture_gui_config_settings` captured the reverted FLATTEN →
  `lock_mode=1` written.
- Load of a HARD profile → `populate_desired_into_gui` leaves the GUI **not
  dirty** (`preserveDirty`), so the same sync clobbered the loaded HARD before
  the user could Apply.
Everything downstream was already correct (IPC clamp, service merge, NVML
locked-clocks call, restart persist = whole-struct dump, INI round-trip).

### Fixes
- **`lock_mode_sync_allowed(guiLockMode, guiAppliedLockMode, guiDirty)`**
  (`app_shared.h`, pure → harness-testable): adopt the snapshot's lock mode only
  when clean AND `lockMode == appliedLockMode`. Wired into the sync block;
  skipped syncs log once per state change ("lockMode sync skipped (pending lock
  intent ...)").
- **Intent invariant:** `sync_applied_lock_state_from_curve()`
  (`gpu_backend.cpp`) now also syncs `appliedLockMode = lockMode` (not-dirty
  guard unchanged) — curve detection must never fake pending intent, else the
  gate would permanently block adopting a service-side HARD.
- **`desired_has_any_action()` counts `hasLock`** (Windows
  `config_profiles_ui.cpp` + Linux `linux_port.cpp`): pin-only profiles were
  "nothing to do" on CLI/apply gating paths (`entry.cpp`).
- **Persistence clamps:** `service_load_restart_reapply_snapshot()` and
  `load_profile_from_config()` now run `validate_desired_settings_for_ipc()`
  after parse (corrupt bytes can't reach apply/curve math unclamped); the
  validator also clamps `lockMHz` (>5000 → 5000) — the one lock field it missed.
- **Diagnostics:** WAIT_ABANDONED config-mutex acquisitions logged
  (`config_utils.cpp` — proceeding is the correct Win32 pattern, ownership IS
  acquired); resume-reapply `CreateThread` failure logged
  (`main_service_server.cpp`).

### User-facing note
Profiles saved while the bug was active contain `lock_mode=1` (flatten) where
pin was intended — re-save once after Build 272.

### Broad review — verified NON-findings (do not re-chase)
A 3-agent review (pin trace / IPC security audit / general sweep) was run; every
load-bearing claim was re-verified against code. Confirmed solid: pipe DACL
(console-user SDDL + AU read-only fallback), `FILE_FLAG_FIRST_PIPE_INSTANCE` +
`PIPE_REJECT_REMOTE_CLIENTS`, exact-size framing + magic/version, caller
identity OS-resolved from the pipe handle (`GetNamedPipeClientProcessId` →
`ProcessIdToSessionId`, active-session-only), `validate_desired_settings_for_ipc`
before any use (APPLY handler), reparse-checked atomic writes, protected binary
DACL, DLL-search hardening. **Invalidated agent claims:** "session-ID spoofing
via request field" (session is an out-param of `service_caller_is_authorized`,
never read from the payload); "CreateThread unchecked at server" (checked);
"WAIT_ABANDONED = corruption" (ownership acquired; log added only);
"`g_serviceReapplyInProgress` raced" (`volatile LONG` + Interlocked);
"`detect_locked_tail_from_curve` resets HARD→FLATTEN as root cause" (detection
is suppressed while a lock is set; FLATTEN-on-fresh-detect is correct).

### Tests
Harness: `lock_mode_sync_allowed` truth table (codes 120–124), `lockMHz` clamp
(125–127). Source guards: gate wired + skip log + `appliedLockMode` invariant +
`hasLock` in both `desired_has_any_action` + both persistence clamps + the two
new diagnostics.

### Verified
`python build.py` (272), `--test`, `--test --asan`, `--target linux --check`
pass. Real-hardware pin apply/save/load recheck wanted (user has the GPU).

---

## 2026-06-07: fixed — GUI visual corruption during service start/restart (Build 271)

### Root causes
1. **Frozen GUI thread.** The service toggle runs on the GUI thread and blocked it
   without pumping messages: `wait_for_background_service_ready` did `Sleep(200)` in
   a loop (5–15 s) and `wait_for_helper_process_bounded` did `WaitForSingleObject`
   for up to `ELEVATED_HELPER_TIMEOUT_MS` (60 s). While frozen the window cannot
   service `WM_PAINT`, so occlusion-revealed regions show stale/garbage content and
   the DWM marks it "not responding" (ghosting).
2. **Unsuppressed bulk control rebuild.** Service-state transitions destroy+recreate
   ~40 child controls; without redraw suppression each `CreateWindow`/`DestroyWindow`
   flashes partial/stale content.

### Fixes (`main_service_ipc.cpp`, `main_runtime_control.cpp`, `ui_main_window.cpp`, `main_gpu_front.cpp`)
- **Message-pumping waits:** `wait_object_pumping_ui()` (built on `MsgWaitForMultipleObjectsEx` + `PeekMessage`) keeps the window repainting; `UiInputGuard` RAII disables the main window for the wait so pumped messages cannot re-enter command handlers (standard modal-wait pattern). Applied to `wait_for_background_service_ready`, `wait_for_helper_process_bounded`, and `wait_for_startup_task_state`. No GUI window (service process) ⇒ inert fallback to plain wait.
- **Redraw-suppressed rebuild:** `rebuild_edit_controls()` wraps destroy+create in `WM_SETREDRAW FALSE/TRUE` and does one `RedrawWindow(... RDW_ALLCHILDREN|RDW_UPDATENOW ...)`. All 3 service-state rebuild sites route through it.
- Bonus: the "Installing/Stopping background service…" status now actually paints during the operation (previously hidden behind the frozen window).

### Notes
- Window-creation foundations were already sound: `WS_CLIPCHILDREN`, no `CS_HREDRAW/VREDRAW`, full-client backbuffer BitBlt.
- WM_QUIT during a pumped wait is re-posted so the main loop still exits cleanly.

### Verified
`python build.py` (271), `--test`, `--test --asan`, `--target linux --check` pass; GUI smoke-launch (controls + tooltip + service connect) runs without crashing.

---

## 2026-06-07: fixed — pin (hard) lock dropped on profile save + lock-checkbox UX (Build 269)

### Root cause
Saving a profile silently downgraded a pinned (HARD/dot) lock to FLATTEN (tick).
The live apply path carried `lockMode` correctly, but the profile-**save** path
assembled `DesiredSettings` without it:
- `merge_desired_settings()` (Windows, `config_profiles_ui.cpp`) merged gpu/mem/power/fan/curve but **ignored all lock fields**.
- `capture_gui_config_settings()` (`main_runtime_gpu.cpp`) copied `lockCi`/`lockMHz`/`lockTracksAnchor` but **not `lockMode`** (both the GUI and live branches).
- `build_full_live_desired_settings()` + `initialize_desired_settings_defaults()` leave `lockMode = NONE`.
Result: every GUI save of a locked profile wrote `lock_mode=0`; on load `populate_desired_into_gui` fell back to FLATTEN.

### Fixes
- **Lock state is now merged as a unit** in `merge_desired_settings()` (Windows): `hasLock`/`lockCi`/`lockMHz`/`lockMode`/`lockTracksAnchor`. Linux twin (`linux_port_profiles.cpp`) now also carries `lockTracksAnchor` (the Linux `DesiredSettings` has no `lockMode`).
- `capture_gui_config_settings()` sets `full.lockMode` from `guiDesired` (GUI branch) and `g_app.lockMode` (live branch), plus a resolved-lock debug line.
- The service active-desired merge (`main_service_runtime.cpp:984`) is now fully covered by the unit merge (its manual re-patch is redundant but retained).
- **`LockMode` given a fixed underlying type (`: int`)** so the IPC sanitizer `validate_desired_settings_for_ipc()` can read/clamp a hostile out-of-range value without UB (UBSan flagged the unfixed enum). 
- Richer save logging in `save_profile_to_config()` (lock ci/mhz/mode/tracks-anchor written).

### Lock-checkbox UX
- **Right-click context menu** (`show_lock_context_menu` in `ui_main_window.cpp`, `WM_CONTEXTMENU`): choose No lock / Flatten / Pin directly; switch FLATTEN↔PIN without cycling through NONE. New IDs `LOCK_CTX_*` in `app_shared.h`.
- **Distinct glyphs** in `draw_lock_checkbox()` (`main_shell.cpp`): FLATTEN = checkmark, HARD = filled dot (was filled box vs box+faint dot).
- **Hover tooltip** (`create_lock_tooltips` in `ui_main.cpp`): comctl32 loaded dynamically (GUI doesn't link it), `TOOLTIPS_CLASS` window rebuilt with the checkboxes; teardown in `destroy_edit_controls`. New `AppData.hLockTooltip`.

### Tests
- Compiled harness: `lock_mode` INI round-trip (HARD/FLATTEN) and `validate_desired_settings_for_ipc` lockMode clamp (999→HARD, -5→NONE).
- Source checks: merge/capture lockMode markers, save lock logging, right-click menu, tooltip presence.
- Incidental: repaired 3 pre-existing stale source-regression checks left behind by the earlier "Reduce debug logging" / audit commits (pipe connect-retry, restricted pipe ACL log wording, initial tail uniform-floor comment).

### Verified
`python build.py`, `--test`, `--test --asan`, `--target linux --check` all pass (Build 269).

---

## 2026-06-07: implemented — NVML hard clock pinning (Build 265)

### What was implemented
Tri-state lock checkbox: unchecked (no lock) → checkmark (flatten lock) → dot (hard lock/pin) → unchecked. Uses `nvmlDeviceSetGpuLockedClocks` / `nvmlDeviceResetGpuLockedClocks` via the `NvmlApi` struct.

### Key bugs fixed during implementation
- **Tail verification for HARD mode:** `verify_curve_request` and monotonicity check now skip for HARD mode (NVML pins clock, tail points irrelevant).
- **NVML reset placement:** reset code was accidentally nested inside the selective offset `if` block. Moved to top-level of `apply_desired_settings_service` — runs unconditionally for all non-HARD applies.
- **GUI→service NVML ownership:** `unlock_all()` in the GUI process cannot reset NVML (no device handle). The service apply pipeline handles all NVML resets.
- **`lockMode` propagation:** missing from `g_serviceActiveDesired` merge, snapshot adoption (`apply_service_snapshot_to_app`), and profile restore path. All fixed.
- **`detect_locked_tail_from_curve`:** set `lockMode = LOCK_MODE_NONE` when detecting a lock from the live curve. Fixed to set `LOCK_MODE_FLATTEN`.
- **Dirty detection:** `lockChanged` in `capture_gui_apply_settings` didn't compare `lockMode`. Fixed.

### Source anchors
- `source/app_shared.h` — `LockMode` enum, `lockMode` fields in `AppData`/`DesiredSettings`/`ServiceSnapshot`, `SERVICE_PROTOCOL_VERSION = 7`
- `source/main_runtime_nvml.cpp` — `nvml_set_gpu_locked_clocks` / `nvml_reset_gpu_locked_clocks` wrappers
- `source/gpu_backend_apply.cpp` — NVML reset at top of apply, HARD mode skip for tail verification/monotonicity
- `source/main_state_sync.cpp` — `lockMode` adoption in snapshot sync
- `source/main_service_runtime.cpp` — `lockMode` in `g_serviceActiveDesired` merge
- `source/config_profiles.cpp` — `lock_mode` INI read/write with backward compat

---

## 2026-06-07: research — NVML locked clocks APIs for hard clock pinning

### Question
Can we implement a "hard lock" feature that pins the GPU to a specific VF point (disabling all dynamic clocking), as opposed to the existing "flatten lock" that only flattens the VF curve tail?

### Findings
- **`nvmlDeviceSetGpuLockedClocks(device, minMHz, maxMHz)`** and **`nvmlDeviceSetMemoryLockedClocks`** are exported from the installed `nvml.dll` (confirmed via `dumpbin /exports`).
- Setting `min == max` pins the clock to that exact frequency. Special values: `tdp`, `unlimited`.
- Requires admin (service already elevated). Volta+ for GPU, Ampere+ for memory. Not persistent across reboots.
- **Not used by the project** — `NvmlApi` struct has no fields for these APIs.
- The public NVAPI SDK has **no** clock locking APIs. The private NVAPI IDs `0x0F4DAE6B` (Pstates20 Set) and `0xAD95F5ED` (Power Policies Set) are probed but unused.
- Captured VF curve data with a flatten lock active only shows VF tail flattening (one uniform large negative offset). A locked-clocks pin is a separate NVML call and is not visible in the VF curve data.
- The NVML locked clocks API coexists with VF curve offsets — locked clock sets the frequency, VF offsets shift voltage at that frequency.

### Implementation path
1. Add `setGpuLockedClocks` / `resetGpuLockedClocks` / `setMemoryLockedClocks` / `resetMemoryLockedClocks` to `NvmlApi` struct.
2. Resolve via `nvml_resolve()` in `main_runtime_nvml.cpp`.
3. UI: distinct "hard lock" mode (pin frequency) vs existing "flatten lock" (VF tail flatten).
4. Combine with VF curve offsets for undervolt-at-locked-frequency (needs GPU testing).

### Source anchors
- `source/app_shared.h:923-955` — `NvmlApi` struct (no locked clocks fields)
- `source/main_runtime_nvml.cpp:788-819` — NVML resolution (no locked clocks resolve)
- NVML docs: https://docs.nvidia.com/deploy/nvml-api/group__nvmlDeviceCommands.html
- `llm-wiki/gpu-backend.md` — "NVML locked clocks APIs" section added

---

## 2026-06-05 (F-REL-2g): tray icon shows default when the GPU/driver is unavailable

### Nit
With the driver disabled in Device Manager (kept disabled), the tray icon kept
reporting OC / custom-fan active.

### Root cause (log-confirmed)
Disabling the driver fired a device event → recovery restart (`request_service_restart`,
13:54:25). The fresh process can't init NvAPI (`nvapi_init: result=-6` looping) and
never sets `g_app.loaded`, so `snapshot->loaded=false`. But the snapshot still
carries the **pending DESIRED** profile (the reapply F-REL-2c keeps waiting for the
driver), and `update_tray_icon` reported OC/fan from that desired state even though
nothing is actually applied (the GPU is gone).

### Fix
`update_tray_icon` (`main_fan_runtime.cpp`) now gates the OC/fan-active detection on
real GPU availability: `hardwareLive = g_app.loaded` (and, when using the background
service, also `g_app.backgroundServiceAvailable`). When the GPU live state is
unavailable (driver disabled / removed / service down), the tray shows the default
icon instead of a false OC/fan-active state. `g_app.loaded` mirrors the service
snapshot's `loaded` flag, which is false whenever the service cannot initialize the
GPU — so no protocol change was needed. When the driver returns, the service
reapplies (F-REL-2c) and the tray returns to the OC/fan icon.

Note: the main window (if open) may still show the pending desired profile while the
GPU is disabled — that's the configured intent, not live state; only the tray's
active/inactive indicator was in scope here.

### rev.2 — also gate the tray TOOLTIP
The icon theme was gated but `build_tray_tooltip()` (`main_gpu_front.cpp`)
independently recomputed `live_state_has_custom_oc/fan` + the profile, so hovering
still showed "OC / Fan | Profile X". Factored the availability check into a shared
`tray_hardware_live()` helper used by BOTH `update_tray_icon` (icon) and
`build_tray_tooltip` (tooltip); when the GPU is unavailable the tooltip now reads
"Green Curve - GPU driver unavailable" instead of a false active state.

Tests: `require_text` `tray_hardware_live()` (icon) + `static bool tray_hardware_live()`
(helper) + "Green Curve - GPU driver unavailable" (tooltip). `python build.py`,
`--test`, `--test --asan` pass.

---

## 2026-06-05 (F-REL-2f): keep the tray icon current while minimized to tray

### Nit
After a service reset to defaults, the tray icon only updated once the user opened
the window. While the GUI sat in the tray (window hidden), the icon stayed stale.

### Root cause
`update_fan_telemetry_timer()` (`main_gpu_front.cpp`) **kills the telemetry timer
and returns when the window is not visible** (the per-second poll is what calls
`sync_fan_ui_from_cached_state()` → `update_tray_icon()`). So in tray-only mode
there was no polling → the icon never refreshed until the window opened (which
restarts the timer).

### Fix
When hidden, instead of stopping entirely, keep a SLOW poll (≥3 s,
`TRAY_HIDDEN_POLL_INTERVAL_MS`) **only while a tray icon is present**
(`g_app.trayIconAdded`).  That keeps the tray icon/tooltip current (e.g. reflects
the reset) at minimal cost; the fast foreground-rate poll + boost still apply only
when visible.  With no tray icon there is nothing visible to update, so polling
still stops entirely. `update_tray_icon()` is anti-flickered (only re-renders on a
state/tip change), so the slow poll is cheap.

Tests: `require_text` "TRAY_HIDDEN_POLL_INTERVAL_MS". `python build.py`, `--test`,
`--test --asan` pass.

---

## 2026-06-05 (F-REL-2e rev.2): full GUI resync (not just field state) when a reset clears the adopted lock

### Why rev.1 was incomplete
rev.1 cleared the internal lock state + the single locked edit field, but the
graph, the lock CHECKBOX control, the tail fields, and the "Lock:" header stayed
stale (worse: an inconsistent mix — point 76 showed the live 2482 while the tail
and graph still showed 2962). Root cause: those are resynced only by
`update_all_gui_for_service_state()` (what the **Refresh button** calls via
`refresh_curve`), and the per-second telemetry poll (`main_fan_runtime.cpp`)
deliberately SKIPS that resync (lines ~85-89: only on first edit-control creation)
so it never wipes in-progress edits.

### Fix (rev.2)
When the reset-clear fires in `apply_service_snapshot_to_app` (same narrow guard:
authoritative no-lock + not dirty + adopted lock `lockedCi==appliedLockCi`), set a
new flag `g_guiForceFullRefresh`. The telemetry poll then calls
`update_all_gui_for_service_state()` once (→ `populate_edits()` resyncs every
per-point field + lock checkbox from the now-default live curve, then
`invalidate_main_window()` redraws graph + header) and clears the flag. Removed
the rev.1 piecemeal single-field write. Net: the whole GUI snaps to default on a
reset, exactly as if the user pressed Refresh — which they confirmed is the
workaround.

Safety unchanged: only fires when not dirty and the lock is ADOPTED (matches the
last applied lock), so it never wipes a fresh user edit nor a loaded-but-unapplied
profile (Load sets a lock whose ci != appliedLockCi). The broad lock-detection
gate and live-detection "preserve intent" logic remain untouched.

Tests: `require_text` "clearing stale adopted GUI lock" + "g_guiForceFullRefresh".
`python build.py`, `--test`, `--test --asan` pass.

---

## 2026-06-05 (F-REL-2e): GUI clears a stale ADOPTED lock after the service resets to defaults

### Field bug (cosmetic; hardware confirmed correct via an external tool)
After the service was killed and restarted → reset to defaults, the GUI graph and
offsets correctly showed default, but the **lock checkbox stayed checked, VF point
76 still showed 2962 MHz, and the header still showed "Lock: 2962 MHz"** — none of
which matched the live default curve (ci76 ≈ 2482).

### Root cause (log-confirmed)
Sequence: user Applied a profile (lock ci76@2962) → GUI **adopted** the lock
(`adopted service lock ci=76`), not dirty → user killed the service → restart →
`reset GPU to driver defaults`. The live curve refreshed to default, but the
GUI's adopted lock was never cleared: `should_accept_service_curve_lock_detection()`
returns **false whenever `g_app.lockedCi >= 0`**, so the snapshot's authoritative
`hasLock=false` (after reset) could never reach the clear branch — the clear was
gated behind the very condition it needed to undo. (Also `detect_locked_tail_from_curve:
live lock detection suppressed; preserving intent ci=76` kept the GUI's own
detection from clearing it.)

### Fix
`apply_service_snapshot_to_app` (`main_state_sync.cpp`) now has a narrow clear that
runs OUTSIDE that gate: when the snapshot authoritatively reports **no lock**, the
user is **not dirty-editing**, and the current GUI lock is an **adopted** lock
(`lockedCi == appliedLockCi && lockedFreq == appliedLockFreq`), it clears the GUI
lock state and refreshes the formerly-locked point's edit field from the live
curve. The subsequent UI refresh then unchecks the lock box and updates the
"Lock:" header. Deliberately narrow so it never fights a real recovery (which
reports the active desired lock, i.e. `hasLock=true`) nor wipes a fresh unapplied
user edit (which marks the GUI dirty). The broad gate / live-detection
"preserve intent" logic is left untouched (avoids regressing normal lock display).

Tests: `require_text` "clearing stale adopted GUI lock". `python build.py`,
`--test`, `--test --asan` pass. Real-GUI recheck of the kill→reset case wanted.

---

## 2026-06-05 (F-REL-2d rev.2): no-snapshot resume does a FULL reset to defaults (OC + fan), fixing partial reset + stale GUI

### What the field log showed (greencurve_debug.txt, manual kill while a curve+undervolt were active)
Session 2 (restart after the kill) ended INCONSISTENT:
- `hardware_initialize: stale mem VF offset 3000000 kHz detected (smi=0 pstate=17001); resetting to 0` — mem offset reset, but only as a coincidental NVML-register cleanup (gated on `!hasActiveDesired && smi/pstate==0`), not a deliberate reset.
- `curve tail bookends: ci76=2962 …` — the **VF curve undervolt was still applied** (NOT reset).
- `apply_control_state_to_gui: … fanMode=1 fanPct=30` — control state still said **fixed 30%** even though the fan had been set to auto → the GUI's stale "fixed custom" display.

So the first F-REL-2d (fan-only reset) left a partial leftover OC and a stale control state.

### Fix (rev.2)
`service_reset_to_defaults_on_resume()` (`main_service_recovery.cpp`) now calls
the existing **`service_reset_all()`** instead of just `nvml_set_fan_auto`: it
resets VF curve offsets, GPU/mem offsets, power limit, AND fan→auto, then
**repopulates `g_serviceControlState`** — so the no-snapshot path leaves a clean,
consistent stock state and the GUI shows default/auto (fixes the cosmetic
"fixed custom"). Still gated on "no snapshot" (= fresh boot or out-of-band kill,
NOT a controlled recovery), so a real driver-recovery still reapplies the
profile. Driver-readiness pre-wait (~10 s) retained; `service_reset_all` is
forward-declared in `main.cpp` (cross-shard call resolves).

This makes "reset clocks+fan to default on external termination" DELIBERATE and
COMPLETE (per user request), replacing the previous coincidental/partial reset.

Tests: `require_text` updated to the new helper + "reset GPU to driver defaults
(OC + fan auto)". `python build.py`, `--test`, `--test --asan` pass.

---

## 2026-06-05 (F-REL-2d): reset fan to driver-default after an out-of-band service kill

### Field bug
A user-defined fan curve was active; the user killed the service process via Task
Manager. The fan stayed pinned at a fixed PWM afterward (NVML manual fan control
persists in the driver once the controlling process dies).

### Root cause
The restart-reapply snapshot is written LAZILY — only when WE trigger a
restart/recovery (`request_service_restart`, the device-removal handler) or on
apply/reset failure — **not on a normal successful apply**. So an out-of-band
kill leaves the in-memory `g_serviceActiveDesired` lost and NO snapshot on disk.
The SCM still relaunches the service (non-graceful exit → SC_ACTION_RESTART), but
`service_startup_reapply_thread_proc` saw no snapshot → `nothing to auto-reapply`
→ returned, leaving the fan pinned at the PWM the killed instance last wrote.

### Design (snapshot = "controlled recovery, reapply intended")
Absence of the snapshot is the signal that this was NOT a controlled recovery
(fresh boot or a manual kill), so we deliberately do **not** auto-reapply the
profile (avoids undesired auto-apply after a manual stop). But we now reset the
fan to the driver's automatic policy so it is not left pinned:
`service_reset_fan_to_auto_on_resume()` (`main_service_recovery.cpp`) waits
briefly (~10 s) for the driver, then calls `nvml_set_fan_auto()` (idempotent).
On a genuine fresh boot the fan is already auto → no-op; the per-user logon task
re-applies the profile after login. OC/clock offsets are left untouched (stable,
driver-persistent; resetting could cause a transient).

### Edge-case status (this session)
- **Manual kill while fan curve active** → FIXED (fan reset to auto on resume).
- **Graceful stop** (GUI checkbox / `sc stop`) → already resets fan to auto in the
  normal shutdown teardown (`nvml_set_fan_auto`); unchanged.
- **Driver crash loop on apply** → dormant breaker (F-REL-2) retains snapshot and
  stops reapplying. Residual: the fan may stay pinned during dormancy — NOT reset,
  because touching NVML on a crash-looping driver risks re-triggering the loop.
  Documented as a known minor residual.
- **No / slow driver** → F-REL-2c (wait indefinitely; reapply once active).
- **Corrupt snapshot** → validated load fails → no-snapshot path → fan auto. Safe.
- **OC after a manual kill** → left applied (driver-persistent, stable); only the
  fan is reset. (Open: reset OC too? not requested; would add a transient.)

Tests: `require_text` for the call + the helper's reset. `python build.py`,
`--test`, `--test --asan` pass. Real-hardware recheck of the kill case wanted.

---

## 2026-06-05 (F-REL-2b): reapply not abandoned when the driver is slow to init (prolonged Device Manager disable)

### Field bug (from `logs/greencurve_debug.txt`)
Driver-restart utility reconnect: service restarted AND reapplied (driver "ready on attempt 1").
Prolonged GPU disable in Device Manager → re-enable: service restarted but did
**not** reapply OC. Log: restart at 03:54:10 → `service startup reapply: restart
snapshot found, waiting for GPU driver readiness` → `nvapi_init: result=-6`
repeatedly for ~30 s → 03:54:41 `GPU driver not ready in time; leaving snapshot
for next start` (abandoned). Driver only became usable ~03:55:38 (telemetry
worked), but **nothing retried** — the snapshot sat on disk unused.

### Root cause
`service_startup_reapply_thread_proc()` (`main_service_recovery.cpp`) waited only
60×500 ms (~30 s) for driver readiness, then RETURNED without reapplying. A
re-enabled GPU can take ~90 s before NvAPI initializes (`nvapi_init=-6` until
then), far longer than the 30 s wait — and there is no "next start" to pick up
the retained snapshot because the device already arrived.

### Fix (F-REL-2b + F-REL-2c)
**Goal (per user): once ANY driver is active, the configured OC/fan are GUARANTEED
to be reapplied — never give up just because no/slow driver is present.**
- Startup readiness poll lengthened to 240×500 ms (~120 s) as a fast inline path
  (500 ms granularity). Wait-for-resource poll (no readiness event), lock released
  between polls so fan runtime / pipe server keep running.
- On startup-wait timeout, **do not abandon** — hand off to the main-loop reapply
  worker (`service_queue_recovery_reapply`) after promoting the loaded disk
  snapshot to the in-memory active desired (the worker bails on
  `!g_serviceHasActiveDesired`).
- **F-REL-2c — the worker waits for a driver INDEFINITELY (no time cap).** The
  worker (`service_reapply_thread_proc`, relaunched by the main loop every ~3 s,
  paced ~5 s) now gates the apply on real driver readiness via
  `hardware_initialize() && numPopulated>0`. "Driver not ready yet" (clean init
  failure — slow re-enable, or no driver installed at all) is retried **without**
  consuming `SERVICE_RECOVERY_REAPPLY_MAX_ATTEMPTS`, with throttled logging
  (every ~12th poll). The bounded budget now applies ONLY to the dangerous case
  (driver ready but the apply repeatedly fails/crashes), which the restart-loop
  dormant breaker also guards. The disk snapshot persists across restarts and is
  re-armed on device-arrival.

Net: driver-readiness wait is unbounded (guarantee); only a ready-but-failing
apply is bounded (crash-loop safety). Tests: `require_text` for the hand-off,
`require_order` (promote before queue), and `require_text` for the uncounted
not-ready wait. `python build.py`, `--test`, `--test --asan` pass. Real-hardware
recheck of the prolonged-disable case still wanted.

---

## 2026-06-04 (audit hardening, F-MAINT-1): split oversized shards + line-count guard

Pure-move splits (no behavior change) to bring the worst files toward the
~600-800 line guideline. The shards are `#include`d into the single `main.cpp`
TU, so each new file is wired into the include chain at the right point and
ordering is preserved; the source-grep test suite self-protects (a moved asserted
string makes `--test` fail loudly), so several `require_text`/`require_order`
guards were repointed to the new shards.

- `main_diagnostics.cpp` 1176 → 557; new `main_secure_write.cpp` (627) = reparse
  verification + service safe-writer (included right after main_diagnostics.cpp).
- `main_service_runtime.cpp` 1894 → 1176; new `main_service_persist.cpp` (221) =
  restart/recovery persistence, and `main_service_recovery.cpp` (514) =
  recovery-reapply machinery (included, in order, after main_state_sync.cpp).
- `main_service_server.cpp` 1323 → 1008; new `main_service_install.cpp` (323) =
  install/SCM lifecycle (included after main_service_ipc.cpp).
- `build.py` `warn_oversized_source_files()` prints a non-fatal NOTE for source
  files over ~800 lines (`app_shared.h` exempt).

Not split (deferred, flagged by the guard): `gpu_backend_apply.cpp` (1234) is
essentially ONE function (`apply_desired_settings_service`) — splitting needs a
real refactor of the domain-critical VF-write path, not a pure move, so it should
be its own validated effort. `app_shared.h` (1049, shared protocol/types header)
is intentionally exempt. Remaining marginally-over files (main_service_runtime
1176, main_service_server 1008, main_runtime_nvml ~1000, main_state_sync ~1037,
etc.) are left for follow-up pure moves driven by the guard.

`python build.py` (full), `--test`, and `--test --asan` all pass.

---

## 2026-06-04 (audit hardening, F-REL-2): restart-loop breaker goes dormant (retains snapshot) + minidump rotation

Reshaped per the agreed design (the elaborate "no-driver state machine" was not
needed — see the discussion: a true no-driver machine doesn't loop because
nvml/nvapi don't load; the loop is a *transitional-driver* phenomenon):

- **Breaker → dormant, retain snapshot** (`service_startup_reapply_thread_proc`,
  `main_service_runtime.cpp`). On ≥5 restarts in 5 min it no longer discards the
  snapshot + clears history; it just skips reapply this round and KEEPS both. The
  dormant process stops crashing (reapply is the crash source), and re-arms
  automatically when the 5-min window ages the restart ticks out, or when a driver
  event re-arms it. Fixes the old behavior that permanently dropped the user's
  profile (and the stale comment claiming the profile "stays on disk").
- **Re-arm on driver event** (`DBT_DEVICEARRIVAL`, `main_service_server.cpp`): a
  genuine device arrival / driver (re)install now calls
  `service_clear_restart_history()` before the restart, so the fresh process
  reapplies the retained snapshot even if a recent loop had tripped the breaker.
- **Minidump rotation** (`service_rotate_minidumps(10)`, `main_state_sync.cpp`),
  called at each service start (= once per restart cycle): deletes the
  lexicographically-oldest `greencurve_veh_*.dmp` until ≤10 remain, so a sustained
  restart loop / poison-pill can't fill the disk with crash dumps.

Why this is safe per the SCM model: the SCM failure actions (2s/5s/10s, then 10s
repeating; count resets after 10 min of healthy run) already give effectively
unlimited spaced restarts, so a transitional driver self-heals; the breaker only
guards the poison-pill / never-succeeds case. Needs real-hardware validation
(driver install/upgrade + a forced poison-pill).

Tests: `require_text` for `going DORMANT`, the device-arrival
`service_clear_restart_history();`, and `service_rotate_minidumps` (defined +
called). `--check` + `--test` pass.

---

## 2026-06-04 (audit hardening, F-SEC-3): pipe write restricted to the active console user

`create_restricted_pipe_security_descriptor()` (`main_service_server.cpp`)
previously granted Authenticated Users (AU) `GRGW` permanently, so any local
authenticated user could send `APPLY`/`RESET` to the LocalSystem service and
drive GPU OC. Now, once the active console user is resolved (`WTSQueryUserToken`),
the SDDL is `D:(A;;GA;;;SY)(A;;GA;;;BA)(A;;GR;;;AU)(A;;GRGW;;;<userSID>)` — AU
downgraded to **read-only**, read+write granted to the console user's SID.

No-lockout fallback: if the console user can't be resolved (boot/pre-login,
RDP-only, transient token failure) AU stays `GRGW`. The GUI client opens with
`GENERIC_READ | GENERIC_WRITE` (`main_service_ipc.cpp:298`), so AU-read-only
effectively blocks non-console users from issuing requests, while the console
user connects via its SID ACE (elevated GUIs via the `BA` ACE). ACL mode is
logged only on transitions (the SD is rebuilt per pipe instance in the server
loop). Needs manual validation across boot→login→RDP/fast-user-switch.

Tests: `require_text` for both the `(A;;GR;;;AU)` restricted ACE and the
`(A;;GRGW;;;AU)` fallback. `--check` + `--test` pass.

---

## 2026-06-04 (audit hardening, F-DOM-1): best-guess warning re-shows per session; mem out-of-range stays intentional

Two domain-safety touch-ups (no hardware-write behavior change):
- **Best-guess-family VF warning** (`show_best_guess_support_warning`,
  `main_gpu_state.cpp`) was permanently dismissible via a persisted
  `hide_best_guess_warning_<family>` config flag. Removed that permanent
  suppression (and the dialog "don't show again" verification / legacy second
  MessageBox). It now re-shows **once per GUI session** for best-guess families
  via the new in-memory `g_bestGuessWarningShownThisSession`
  (`should_show_best_guess_warning`, `main_gpu_front.cpp`) — writing VF data to an
  unvalidated architecture warrants a per-run reminder, but it won't nag twice in
  one session.
- **Memory offset outside the driver-reported range** is **still applied** (NOT
  gated) — NVIDIA's reported mem range is often conservative and the driver
  clamps/rejects unsupported values; gating would break legitimate mem OC.
  Kept the write-anyway, clarified the log to mark it as deliberate (and it is
  already absolute-bounded to ±5000 MHz at the IPC boundary). GPU offset (gated
  when range known) and power limit (50–150%) are unchanged.

Constraint respected: write support for unsupported/best-guess GPUs stays
enabled by default; nothing is hard-blocked.

Tests: `require_text` for `g_bestGuessWarningShownThisSession`;
`forbid_text("hide_best_guess_warning_")`; updated the mem-write source guard to
the new wording. `--check` + `--test` pass.

---

## 2026-06-04 (audit hardening, F-REL-1): verify the SCM auto-restart safety net at startup

The entire driver-recovery design depends on the SCM relaunching the process
after its non-zero exit (`SC_ACTION_RESTART`). LocalSystem cannot SET the failure
actions at runtime (lacks `SERVICE_CHANGE_CONFIG` — `service_ensure_failure_actions_configured()`
no-ops with ACCESS_DENIED), so if they were never installed or got cleared, the
service silently never comes back after a GPU driver event (the build 229/230
class of failure). LocalSystem CAN query config, so new
`service_verify_restart_safety_net()` (`main_service_server.cpp`) reads the actual
`SERVICE_CONFIG_FAILURE_ACTIONS` via `QueryServiceConfig2W` and logs
`auto-restart net is ARMED/NOT ARMED` (+ a loud WARNING with reinstall guidance
when not armed). Called from `service_main` right after the (best-effort) ensure.

Log-only for now; surfacing to the GUI would need a `ServiceSnapshot` field +
protocol bump (follow-up). Tests: 2 `require_text` guards. `--check` + `--test` pass.

---

## 2026-06-04 (audit hardening, F-SEC-5 + policy): no files under %ProgramData% — use %LOCALAPPDATA% only

**Policy (user directive):** the program must not create any file under
`%ProgramData%\Green Curve`; everything goes under `%LOCALAPPDATA%\Green Curve`.
For the LocalSystem service, `%LOCALAPPDATA%` resolves to the SYSTEM profile
(`...\config\systemprofile\AppData\Local\Green Curve`), which is **admin-only** —
this also resolves F-SEC-5 (SYSTEM-process minidumps were previously written to a
user-readable `%ProgramData%` location).

**Change:** new crash-safe helper `resolve_service_machine_data_dir()`
(`main_state_sync.cpp`) resolves `%LOCALAPPDATA%\Green Curve` (env var, with a
`%SystemRoot%\System32\config\systemprofile\AppData\Local` fallback for services
lacking the var). All four machine-fixed service artifacts now use it:
- `service_restart_history.bin` + `service_restart_reapply.bin` (`main_service_runtime.cpp`)
- early service debug log (`service_early_debug_log_path`, `main_diagnostics.cpp`)
- VEH minidump `greencurve_veh_*.dmp` + `greencurve_crash.txt` (`write_veh_minidump`)

It's a FIXED path (SYSTEM profile) so restart-loop history stays consistent
pre/post-login and across restarts — do NOT switch these to `g_userDataDir`
(which flips to the console user's dir after login).

`service_cleanup_legacy_programdata()` runs once at `service_main` startup:
best-effort delete of every file in the old `%ProgramData%\Green Curve` + rmdir,
removing any previously world-readable SYSTEM dumps. (Migration note: a pending
reapply snapshot in the old location is lost once on upgrade — acceptable.)

Tests: source guards — `resolve_service_machine_data_dir` /
`service_cleanup_legacy_programdata` exist and are wired; `forbid_text("ProgramData")`
on `main_service_runtime.cpp` and `main_diagnostics.cpp` (the cleanup that still
references ProgramData lives in `main_state_sync.cpp`). `--check` + `--test` pass.

---

## 2026-06-04 (audit hardening, F-SEC-1): protect the installed SYSTEM service binary's DACL

**Root cause:** `get_secure_service_install_dir_w()` returns the *running
exe's own directory* (no writability check); the service is registered with the
SCM by absolute path. If the GUI runs from a user-writable folder, the SYSTEM
service binary inherited that folder's user-writable permissions. Invariant: the
registered service binary is writable only by SYSTEM and Administrators.

**Fix (harden-in-place, user-chosen):** new standalone unit `source/service_acl.cpp`
(`service_acl.h`):
- `apply_protected_service_binary_dacl()` — protected DACL `D:P(A;;FA;;;SY)(A;;FA;;;BA)(A;;0x1200a9;;;BU)`
  (SYSTEM + Administrators Full, BUILTIN\Users Read+Execute, inheritance disabled
  via `PROTECTED_DACL_SECURITY_INFORMATION`) + best-effort owner→Administrators
  (closes the owner-implicit-WRITE_DAC rewrite hole; requires elevation).
- `restore_inherited_dacl()` — empty UNPROTECTED DACL re-enables inheritance.
- `service_path_is_under_secure_root()` — true under %ProgramFiles(.x86/W6432)% / %SystemRoot%.
- `service_binary_dacl_is_hardened()` — verifies protected + no non-admin write
  (used by the unit test).

Wiring: `ensure_secure_service_binary_path()` (`main_service_ipc.cpp`) hardens the
binary after staging and **fails the install closed** if it can't (runs elevated,
before `CreateService`); warns when the install dir is not under a secure root
(%ProgramFiles% recommended). `cleanup_secure_service_binary_after_remove()`
reverts the DACL so the user can delete/replace the unregistered binary
(requirement). While merely halted-but-registered, an elevated admin can still
overwrite it (Admins=Full → Explorer UAC prompt works).

Admin requirement to update the binary while registered is intentional. Combined
with F-SEC-2: statically-imported DLLs (dbghelp/version) are now also protected
because the binary's directory hosting is the planting vector.

Tests: `fan_curve_regression.cpp` ACL round-trip (codes 110–116) asserts
apply→hardened, restore→not-hardened on a real temp file (no second identity
needed); 4 `require_text` guards. `service_acl.cpp` added to `WINDOWS_SOURCE_FILES`,
the harness link (+`-ladvapi32`), and LSP. `--check` + `--test` pass.

---

## 2026-06-04 (audit hardening, F-SEC-2): DLL-search hardening at startup

`initialize_process_mitigations()` (`source/cfg_glue.cpp`) — already called by
both the GUI (`entry.cpp`) and service (`main.cpp`) entry points before any
runtime `LoadLibrary` — now also calls
`SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32 | LOAD_LIBRARY_SEARCH_USER_DIRS)`
and `SetDllDirectoryW(L"")`. This removes the application directory and CWD from
the DLL search path so a planted non-KnownDLL (e.g. `dbghelp.dll`, `version.dll`
— both implicit imports) cannot hijack the LocalSystem service.

Scope/limits: this protects every *runtime* `LoadLibrary`. Our own helper loads
already use absolute System32 paths (`load_system_library_a`), and NVIDIA's
nvml/nvapi DLLs + deps live in System32, so resolution is unchanged.
**Statically-imported DLLs are bound by the loader before this runs**, so the
install-directory ACL (F-SEC-1, pending) is the primary defense for those.

Tests: two `require_text` guards in `build.py` assert the hardening calls exist
in `cfg_glue.cpp`. `python build.py --check` builds both binaries cleanly
(service 198,144 B).

---

## 2026-06-04 (audit hardening, F-SEC-4): complete the IPC trust-boundary validator

`validate_desired_settings_for_ipc()` (`source/app_shared.h`) previously clamped
only power/gpu/mem offsets, curve-point MHz, and fan percent. It left `lockCi`,
`gpuOffsetExcludeLowCount`, `fanMode`, and the embedded `FanCurveConfig`
(per-point temp/percent, hysteresis, poll interval) unchecked. This is the single
trust boundary any Authenticated User can reach (the pipe ACL grants AU write), so
all index/mode/curve fields are now clamped here as defense in depth:
- `lockCi` → `[-1, VF_NUM_POINTS-1]` (keeps the -1 "no explicit lock" sentinel).
- `gpuOffsetExcludeLowCount` → `[0, VF_NUM_POINTS]`.
- `fanMode` → `[FAN_MODE_AUTO, FAN_MODE_CURVE]`.
- fan-curve points → percent `[0,100]`, temp `[0,150]`; hysteresis
  `[0, FAN_CURVE_MAX_HYSTERESIS_C]`; pollIntervalMs `>= 1`.

The APPLY handler (`main_service_server.cpp:707`) is the only path that applies
`request.desired`; RESET/GPU-prepare only use `targetGpu`, so APPLY coverage is
complete. Downstream apply code still range-checks `lockCi`
(`gpu_backend_apply.cpp:155`) — these clamps are belt-and-suspenders, not a
replacement.

Tests: extended the generated `fan_curve_regression.cpp` harness (return codes
55–66) with edge cases for every new clamp, plus three `require_text`
source-regression guards in `build.py`. `python build.py --test` passes.

---

## 2026-06-04 (build 230): SCM relaunch fixed for real — recovery exit must NOT report SERVICE_STOPPED (build 229's flag fix could not take effect)

### What build 229 got wrong
Build 229 added `SERVICE_CONFIG_FAILURE_ACTIONS_FLAG` /
`fFailureActionsOnNonCrashFailures = TRUE` so the reported-`SERVICE_STOPPED`
recovery exit would trigger `SC_ACTION_RESTART`. **It still didn't restart.**
Field log (`logs/greencurve_debug.txt`, reconnect via a driver-restart utility):
- `service: ChangeServiceConfig2(FAILURE_ACTIONS) failed (error 5)` at startup.
- Recovery itself worked: `service recovery monitor: … requesting service restart
  (crashCount=1)` → `request_service_restart` → `service_main: restart requested …`
  → `SESSION END`. Then **nothing** — the SCM never relaunched.
- `sc qfailureflag GreenCurveService` → `FAILURE_ACTIONS_ON_NONCRASH_FAILURES: FALSE`.
- `sc qfailure` → the 2s/5s/10s RESTART actions ARE present (from an old install).
- `sc sdshow` → the `SY` (LocalSystem) ACE is `CCLCSWRPWPDTLOCRRC` — **no `DC`
  (`SERVICE_CHANGE_CONFIG`)**; only `BA` (Administrators) has `DC`.

### Real root cause
The service runs as **LocalSystem**, which lacks `SERVICE_CHANGE_CONFIG` on its own
service object, so `service_ensure_failure_actions_configured()` can never set the
flag at runtime (`ERROR_ACCESS_DENIED`). The flag is only settable at INSTALL
(elevated/admin), which wasn't re-run for 229. So the flag stayed `FALSE` and the
reported-`SERVICE_STOPPED` exit looked *graceful* → no relaunch. Build 228's
"applied at every service start" self-repair was **never functional** for the
LocalSystem token.

### Fix (build 230) — stop depending on the flag
- `service_main` recovery exit: **terminate via `ExitProcess(1)` WITHOUT reporting
  `SERVICE_STOPPED`.** The SCM queues `SC_ACTION_RESTART` *by default* when a
  service dies without reporting STOPPED — no flag, no admin, no reinstall; it uses
  the RESTART actions already present. (Removed the `SERVICE_STOPPED` /
  `dwWin32ExitCode = ERROR_SERVICE_SPECIFIC_ERROR` report from that branch.)
- `service_configure_failure_actions()`: kept the non-crash flag as secondary
  defense-in-depth (only effective on a fresh install); `dwResetPeriod` 1 day →
  **10 min** so the SCM failure COUNT resets between spaced-out driver events (only
  3 RESTART actions); ACCESS_DENIED now logged as *expected for LocalSystem*.
- Graceful/user stop still reports `SERVICE_STOPPED` with `dwWin32ExitCode =
  NO_ERROR` (no spurious restart).
- Tests: FP-06-003 now asserts the recovery exit terminates *without* reporting
  STOPPED (`require_text "terminating WITHOUT reporting SERVICE_STOPPED"`) and
  `forbid_text "ERROR_SERVICE_SPECIFIC_ERROR"` guards against reverting to the
  reported-STOPPED design.

### Deploy note
The currently-installed service already has the RESTART actions, so deploying the
build-230 service binary and restarting the service is enough — **no reinstall
needed** for the relaunch to work. (A reinstall is only needed to refresh the
failure-action config / flag / reset-period on the SCM side.)

### Verify
After deploying build 230, trigger a driver restart/reconnect and confirm the
service comes back: debug log shows `restart requested — terminating WITHOUT
reporting SERVICE_STOPPED …` followed by a fresh `BEGIN service service_main
bootstrap` within ~2 s, and the service process is running again.

---

## 2026-06-04 (build 229): Service never auto-restarted after GPU reconnect — missing non-crash failure-actions flag (SUPERSEDED by build 230)

### Problem
The build-228 recovery restart did not work in the field: after a GPU device
reconnect / driver-upgrade install the "green curve" **service process was gone
and never came back**. The recovery trigger fired correctly (the process did
exit via `ExitProcess(1)`), but the SCM never relaunched it.

### Root cause
The recovery exit branch (`service_main`, `main_service_server.cpp`) reports
`SERVICE_STOPPED` with a non-zero `dwWin32ExitCode` before `ExitProcess(1)`, and
relied on the `SC_ACTION_RESTART` failure action to relaunch. But Windows queues
failure actions for a **reported** `SERVICE_STOPPED` only if the service opted in
via `SERVICE_CONFIG_FAILURE_ACTIONS_FLAG` with
`fFailureActionsOnNonCrashFailures = TRUE`. `service_configure_failure_actions()`
set the `SC_ACTION_RESTART` actions but **never set that flag**. With the default
(`FALSE`), the SCM treats a reported `SERVICE_STOPPED` (even with a non-zero exit
code) as a *graceful* stop → no failure action → service stays dead. This was
independent of which of the four triggers fired (all funnel through the same
exit). The build-228 wiki text documented the wrong assumption verbatim.

### Change
- `service_configure_failure_actions()` (`main_service_server.cpp`): after the
  existing `ChangeServiceConfig2W(..., SERVICE_CONFIG_FAILURE_ACTIONS, ...)`, add
  a second `ChangeServiceConfig2W(..., SERVICE_CONFIG_FAILURE_ACTIONS_FLAG, ...)`
  with `fFailureActionsOnNonCrashFailures = TRUE`. Applied at install AND every
  service start, so installs predating this build self-repair on next start.
- Graceful-shutdown report (normal/user stop) now sets `dwWin32ExitCode =
  NO_ERROR` and `dwServiceSpecificExitCode = 0` explicitly, so enabling the flag
  cannot turn a clean stop into a spurious auto-restart. (The two early
  init-failure STOPPED reports already run before any non-zero code is set.)
- Logging: failure-action success messages now mention the non-crash flag.
- Test: **FP-06-008** (`build.py`) asserts `SERVICE_CONFIG_FAILURE_ACTIONS_FLAG`
  + `fFailureActionsOnNonCrashFailures = TRUE` are present and that the actions
  are configured before the flag — would have failed before this fix.

### Verify
`sc qfailureflag GreenCurveService` → `FAILURE_ACTIONS_ON_NONCRASH_FAILURES: TRUE`.
After a real driver upgrade / device disable-enable, the debug log shows the
restart request followed by a fresh `BEGIN service service_main bootstrap` and
`service startup reapply: … applied successfully`. Negative check: a normal
`sc stop` stays stopped (no auto-restart).

### Not the cause (noted)
The `DBT_DEVICEARRIVAL` guard requiring a prior removal
(`main_service_server.cpp`) is unrelated here — the process *did* exit, so a
trigger fired. Revisit only if a "silent" arrival (no prior removal) is observed.

---
## 2026-06-04 (build 228): Recovery rearchitected — restart the service process instead of in-process NVML/NvAPI reload

### Problem
After a GPU device reconnect / in-place driver-upgrade install (which replaces
all driver files), the service could no longer talk to NVML/NvAPI and lost all
functionality. Applying the OC profile **without** such an event was 100% stable.
The in-process recovery (`service_recover_gpu_connection` Phase A–E) had been
patched across ~10 build cycles (RC1–RC9, builds 194–227) and still failed.

### Root cause (first-principles; the project's own builds 182/183 corroborate it)
In-process reload of `nvml.dll`/`nvapi64.dll` cannot recover a driver change.
The NVIDIA user-mode DLLs are driver-pinned/threaded, so `FreeLibrary` does not
unmap them and the follow-up `LoadLibrary` returns the **old** image; `nvmlInit`
then returns `ALREADY_INITIALIZED` on a device handle bound to the dead driver
instance. NvAPI (VF-curve writes) is entangled with the process-global NVIDIA UMD
stack (DXGI/D3DKMT → `nvldumdx`/`nvwgf2umx` …) — a process-wide singleton that
cannot be force-reloaded and must version-match the kernel driver. Only a **fresh
process** reliably re-binds a version-matched UMD to the new driver (the known-
stable fresh-boot path). Build 183 used a restart and worked; build 186 reverted
to in-process for UX; RC1–RC9 never made it robust.

### Change (decision: always recover by restarting the process)
- **Removed** the in-process recovery: `service_recover_gpu_connection` (Phase
  A–E, ~430 lines), `service_recovery_thread_proc`, `service_safe_close_nvml/
  nvapi`, the recovery-thread hang/heartbeat watchdog (`g_serviceRecoveryHeartbeatMs`),
  `service_reregister_device_notification`, and `nvapi_reset_qi_cache`. Net
  ~860 fewer source lines.
- `launch_recovery_thread()` kept as the single chokepoint but gutted to just
  `request_service_restart(...)`, so all 4 trigger call sites (DBT_DEVICEARRIVAL,
  VEH crash via main-loop monitor, fan-pulse wedge, on-disk version change)
  auto-route to a restart with no call-site edits.
- `request_service_restart()` (`main.cpp`): snapshot active profile, record the
  restart for loop protection, signal exit — does **not** touch NVML.
- `service_main` restart-exit branch: reports `SERVICE_STOPPED` with non-zero
  `dwWin32ExitCode` + `ExitProcess(1)`, **skipping** `nvml_set_fan_auto()`/
  `close_nvml()` (which can hang on a dead driver). SCM failure action relaunches.
- SCM failure actions (`SC_ACTION_RESTART` 2/5/10 s) factored into
  `service_configure_failure_actions()` and now applied at **install AND every
  service start** (`service_ensure_failure_actions_configured()`), so old installs
  self-repair.
- Fresh process restores via `service_startup_reapply_thread_proc` (waits for
  driver-ready, logs loaded nvml/nvapi versions, re-applies snapshot).
- **Persisted restart-loop protection:** `service_record_restart_event` /
  `service_count_recent_restarts` keep an on-disk ring of restart ticks
  (`service_restart_history.bin`); ≥ 5 restarts in 5 min → startup reapply skips
  (clears snapshot) to break an infinite snapshot→apply→crash→restart loop. An
  in-memory counter can't do this (resets each restart).
- VEH stays as the crash **detector** only (invalidate NVML without nvmlShutdown,
  `crashCount++`, ExitThread); removed its now-dead RC5a recovery-thread cleanup,
  kept RC7/RC8a reapply-thread cleanup.
- Logging: per-trigger version-delta + restart-decision markers; boot logs loaded
  DLL versions, recovery-restart vs normal boot, restart-loop counter, per-attempt
  reapply outcome.
- `build.py`: FP-06 inverted to assert the restart contract (added `require_order`
  helper); FP-08-001..004 (tested the removed in-process internals) deleted.

### Files
`source/main.cpp`, `source/main_service_runtime.cpp`, `source/main_service_server.cpp`,
`source/main_diagnostics.cpp`, `source/main_runtime_nvml.cpp`, `source/gpu_backend.cpp`,
`build.py`, `llm-wiki/gpu-backend.md`.

### Verification
- `python build.py` → build 228, both binaries (only the pre-existing
  `__guard_check_icall_fptr` CFG LTO warning).
- `python build.py --test` → compiled regression tests + source regression checks
  (run_source_regression_checks, incl. the new FP-06 restart-contract checks) pass.
- **Manual end-to-end on real GPU still pending** (decisive): apply OC + fan
  profile; trigger a driver-restart utility, a full driver-upgrade install, and an
  eGPU/device reconnect; confirm debug log shows trigger (with version delta) →
  snapshot → non-zero exit → SCM relaunch → fresh DLL versions → startup reapply
  succeeded → GUI reconnects, OC/fan fully active; confirm repeated forced crashes
  trip restart-loop protection instead of looping.

### Open questions / stale-risk
- Multi-GPU: post-restart selected-GPU is restored from config `[gpu] selected_index`
  (ordinal). PCI-identity re-match on the fresh process is available but the
  reapply path still skips PCI matching while `g_serviceInitInProgress` is set;
  fine for single-GPU, revisit for multi-GPU correctness.
- Restart-loop threshold (5 / 5 min) is a first guess; tune from field logs.

---

## 2026-06-03 (build 227): Fix GPU recovery hang after driver upgrade — Phase C no longer makes device-level calls (RC9 rev.2)

### Problem

The RC9 v1 `setjmp`/`longjmp` crash guard did not work with the llvm-mingw
(MinGW) toolchain.  After an access violation inside nvml.dll during Phase C's
temperature probe, the VEH invoked but `longjmp` failed silently (or the VEH
fell through to `ExitThread`).  The recovery thread still died ~286ms later
(lock abandoned).  Additionally, the temperature probe is a blocking kernel-
mode call that can **hang** (not just AV) on a transitional driver — VEH
cannot detect hangs.

### What changed (build 227, RC9 rev.2)

| Fix | File | Change |
|-----|------|--------|
| RC9e | `main_service_runtime.cpp` | **Removed** the setjmp/longjmp guard, temperature probe, and `nvapi_enum_gpu()` from Phase C entirely. Phase C now only loads libraries, resolves function pointers, initializes sessions, and logs versions — no device-level NVML/NvAPI calls. Added `g_serviceRecoveryHeartbeatMs` heartbeat to detect hangs in Phase D. |
| RC9f | `main_service_server.cpp` | Added recovery thread hang watchdog in the main loop: if the heartbeat is stale for > 30s, `TerminateThread` the recovery thread, clean up the abandoned runtime lock, and retry. |
| RC9a | kept | Clear `g_serviceFanPulseInFlight` in Phase A ✓ |
| RC9d | kept | Skip PCI matching during Phase C recovery ✓ |

### Revised approach

- Phase C (`service_recover_gpu_connection`) after version logging proceeds
  directly to Phase D (`hardware_initialize`).  No temperature probe, no
  `nvapi_enum_gpu` — these are already called by Phase D.
- Phase D's `hardware_initialize` can still AV or hang, but now the recovery
  thread heartbeat lets the main-loop watchdog detect the hang and
  `TerminateThread` the stuck recovery thread after 30 seconds.  The abandoned
  runtime lock is recovered via `WAIT_ABANDONED` handling.
- The main-loop recovery monitor then retries recovery after the cooldown
  expires.

---

## 2026-06-03 (build 226): Fix GPU recovery crash loop after driver upgrade / device reconnect (RC9 v1 — REVERTED)

### What was tried (build 226, RC9 v1 — REVERTED in build 227)

The `setjmp`/`longjmp` crash guard approach did not work with the llvm-mingw
toolchain.  The VEH handler called `longjmp` from inside the exception handler,
but the recovery thread still terminated (the lock was abandoned ~286ms after
Phase C entered the setjmp guard).  Replaced in build 227 with the Phase-C-
skips-device-calls approach described above.

### Problem

After GPU device reconnect or driver upgrade install, the service entered a
permanent crash-recovery loop visible in the log:
```
recovery: === phase C: re-initializing GPU libraries ===
nvml: components resolved - fan=7/8 clock=8/8 power=3/3 temperature=1
nvml_ensure_ready: nvmlInit returned 0 (NVML_SUCCESS — fresh first-reference client)
recovery: nvml_ensure_ready() succeeded on attempt 1
nvapi_init: result=0 dll=loaded
recovery: nvapi_init() succeeded on attempt 1
recovery: reloaded NVML version 8.17.16.1047
recovery: reloaded NvAPI version 32.0.16.1047
(wedge + cooldown loop repeats forever — Phase D/E never appear)
```

The recovery thread ran Phase A (stop fan), B (close stale handles), and C
(re-init libraries).  nvmlInit and NvAPI_Initialize both **succeeded** — the
driver control object was alive — but device-handle operations like
`nvmlDeviceGetTemperature` (the RC8g diagnostic probe at
`main_service_runtime.cpp:1016`) **access-violated** inside nvml.dll because
the kernel driver's per-device state was still transitional after the upgrade.

The VEH handler (`green_curve_vectored_handler`) caught the AV and called
`ExitThread(0)` to kill the crashing thread — killing the **recovery thread**
itself.  Phase D (hardware_initialize) and Phase E (resume fan, clear flags)
never ran, and the service was stuck in a `wedge → cooldown → recovery →
crash → wedge` loop.

### Additional contributing issues

1. **`g_serviceFanPulseInFlight` not cleared in Phase A.**  When Phase A
   stopped the fan runtime thread, it left `g_serviceFanPulseInFlight=true`
   from the last fan pulse.  After the recovery thread died in Phase C, the
   wedge watchdog detected the stale pulse flag and fired every 3 seconds.
   The 10s cooldown in `launch_recovery_thread()` then paced the useless
   recovery attempts at 10s intervals — no convergence.

2. **NVML PCI matching could also AV.**  `nvml_select_device_for_selected_gpu`
   calls `nvmlDeviceGetPciInfo` on each candidate device handle, which could
   also AV on a transitional driver.  This runs inside `nvml_ensure_ready()`
   which is called during Phase C, before the temperature probe.

### Fixes (4 changes, build 226 RC9)

| Fix | File | Change |
|-----|------|--------|
| RC9a | `main_service_runtime.cpp:800-808` | Clear `g_serviceFanPulseInFlight` and heartbeat in Phase A after stopping fan thread |
| RC9b | `main_service_runtime.cpp:27-30` | Add `setjmp`/`longjmp` crash guard for Phase C post-init validation (probe + nvapi_enum_gpu) |
| RC9c | `main_diagnostics.cpp:551-563` | VEH checks `g_recoveryProbeActive` and `longjmp`s instead of `ExitThread` for recovery thread Phase C crashes |
| RC9d | `main_runtime_nvml.cpp:684-686` | Skip PCI-identity matching during Phase C (when `g_serviceInitInProgress` is set); ordinal fallback is safe because the GPU index is preserved by `service_safe_close_nvapi()` |

### How it works

- `setjmp` is called before the temperature probe and `nvapi_enum_gpu()` in
  Phase C.  If either operation AVs (transitional driver), the VEH detects
  `g_recoveryProbeActive == 1` and the crashing TID matches the recovery
  thread, then calls `longjmp` back to the guard point instead of `ExitThread`.
- The recovery thread then: closes the fresh-but-nonfunctional NVML/NvAPI
  handles, waits 5 seconds for the driver to settle, retries the entire re-init
  (nvml_ensure_ready → nvapi_init → probe → enum), and if the retry also
  fails, returns false gracefully (next main-loop cycle retries after cooldown).
- The fan pulse flag is cleared in Phase A so the wedge watchdog does not fire
  during recovery — letting the recovery thread run to completion without
  interference.
- PCI matching is skipped entirely during Phase C, avoiding the dangerous
  `nvmlDeviceGetPciInfo` call on transitional driver handles.

### Verification

1. Build succeeds: `python build.py`
2. Regression tests pass: `python build.py --test`
3. Apply OC successfully (baseline: all settings work)
4. After GPU device reconnect / driver upgrade: service recovers to full
   functionality; logs show Phase D and Phase E completing, fan pulse resumes,
   reapply thread runs within 16 seconds.

---

## 2026-06-03 (build 198 -> 199): Fix VEH-killed reapply thread, crash loop detection, Phase C retries (RC8)

### Problems (build 198)

After the reapply thread was VEH-killed during a transitional driver state (post
GPU device reconnect or driver upgrade install), the debug log showed a
recovery→reapply→crash→recovery cycle that never resolved.  Three bugs left
the service non-functional:

1. **`g_serviceInitInProgress` stuck.**  The VEH RC7 cleanup
   (`main_diagnostics.cpp:528`) cleared the reapply thread's handle and TID but
   did **not** clear `g_serviceInitInProgress`, which the reapply thread had set
   at `main_service_runtime.cpp:241`.  With this flag stuck at 1,
   `nvml_ensure_ready()` at `main_runtime_nvml.cpp:771` skipped the
   `nvml_crash_recovery_active()` guard, disabling VEH crash protection until
   the next recovery's Phase E cleared it.

2. **VEH-killed reapply thread treated as success.**  The VEH redirects via
   `ExitThread(0)` → exit code 0.  The health check at
   `main_service_runtime.cpp:443` treated `exitCode == 0` as success and cleared
   `g_serviceReapplyInProgress`.  The retry-re-queue logic at lines 448-463 was
   inside `else if (exitCode != 0)` (line 445) — **never reached** for VEH-killed
   threads.  The reapply was silently dropped.

3. **No crash loop limit.**  The existing `record_driver_recovery()` /
   `MAX_RECOVERIES_BEFORE_BACKOFF = 3` (5-min window) was only called from
   `service_check_oc_persistence()` (the TDR path), never from the recovery
   flow.  The crash-recovery-reapply loop ran unboundedly.

4. **Phase C retries too short.**  5 × 500ms = 2.5s for both `nvml_ensure_ready()`
   and `nvapi_init()` retry loops after a driver upgrade.

5. **Reapply early-exit paths leaked `g_serviceInitInProgress`.**  Two early-return
   paths returned without clearing the flag.

### Fixes (7 changes, build 199 RC8a–RC8g)

| Fix | File | Change |
|-----|------|--------|
| RC8a | `main_diagnostics.cpp:528` | VEH reapply cleanup clears `g_serviceInitInProgress` |
| RC8b | `main_service_runtime.cpp:210,222` | Both early-exit paths clear `g_serviceInitInProgress` |
| RC8c | `main_service_runtime.cpp:443` | Health check detects VEH-killed threads via `g_serviceReapplyInProgress` |
| RC8d | `main_service_runtime.cpp:757,1054,169` | `record_driver_recovery()` in Phase A; loop check before Phase D queue; defense-in-depth in `service_queue_recovery_reapply()` |
| RC8e | `main_service_runtime.cpp:922,950` | Phase C retry count: 5 → 10 attempts |
| RC8f | `main_service_runtime.cpp:1055` | Extended reapply delay: 30s when `crashCount > 1` |
| RC8g | `main_service_runtime.cpp:978` | Temperature probe after Phase C re-init (diagnostic) |

### Source regression checks (FP-08-004)

Added in `build.py`:
- RC8a: `require_text` for `InterlockedExchange(&g_serviceInitInProgress, 0)` in diagnostics
- RC8b: `require_text` for `g_serviceInitInProgress, 0)` in service_runtime
- RC8c: `require_text` for `was VEH-killed` in health check
- RC8d: `require_text` for `crash loop detected`
- RC8e: `forbid_text` for `attempt < 5;` + `require_text` for `attempt < 10;`
- RC8f: `require_text` for `crash count` in Phase D delay
- RC8g: `require_text` for `NVML probe`

### Verification
- `python build.py --check` — both binaries compile cleanly
- `python build.py --test` — all regression checks pass (FP-01 through FP-08-004)
- `python build.py --test --asan` — passes under AddressSanitizer
- Manual verification deferred to live GPU: apply OC profile, trigger
  a driver-restart utility, confirm recovery completes and settings reappear.  If reapply
  keeps crashing, confirm crash loop detection fires after 3 recoveries in 5 min
  and breaks the loop.

---

## 2026-06-02 (build 197 -> 198): Recovery reapply moved to dedicated thread (RC7)

### Problem

After GPU device reconnect or driver upgrade, the in-process recovery completed
successfully (phases A–E), but the reapply of OC/fan settings **ran on the main
service loop thread** (`service_maybe_run_recovery_reapply` → called from
`service_main`).  If the apply path triggered a VEH crash (NVML/NvAPI access
violation on a still-transitional driver), the VEH called ExitThread on the main
loop thread, killing the service's entire control loop.  The SCM then restarted
the process, causing a brief GUI disconnection and preventing the reapply from
ever completing.  Additionally, the GUI falsely reported settings as "active"
(`g_serviceHasActiveDesired == true`) even though the hardware reapply never
succeeded.

### Fix (7 changes, build 198 RC7a–RC7g)

All changes in `source/main.cpp`, `main_service_runtime.cpp`,
`main_service_server.cpp`, `main_state_sync.cpp`, `main_diagnostics.cpp`,
`app_shared.h`.

1. **RC7a — Dedicated reapply thread**: `service_maybe_run_recovery_reapply()`
   was rewritten as `service_maybe_launch_recovery_reapply_thread()`, which
   creates a **dedicated thread** (`service_reapply_thread_proc`) to run the
   apply.  If VEH kills this thread, the main loop survives and the next
   iteration retries.  Thread management mirrors the recovery-thread pattern
   (handle cleanup, VEH-side cleanup).

2. **RC7b — Reapply-in-progress flag**: Added `g_serviceReapplyInProgress`
   (`volatile LONG`) and `ServiceSnapshot::serviceReapplyInProgress`.  After
   Phase D queues the reapply, this flag is set.  The GUI reads it via
   `populate_service_snapshot` and shows "reapplying..." instead of "settings
   active".  Cleared on successful apply or RESET.

3. **RC7c — Force-fresh DLL loading**: After Phase C re-initializes NVML and
   NvAPI, the loaded version is logged (via `service_get_module_version`) so
   diagnosis can confirm the new driver files were picked up.  The existing
   `load_system_library_a` (full path + `LoadLibraryA`) already forces fresh
   loads from disk.

4. **RC7d — Device notification re-registration**: Phase E now calls
   `service_reregister_device_notification()` (defined in
   `main_service_server.cpp`) to re-register the GPU device event handle
   after recovery, preventing stale handles from missing future device events.

5. **RC7e — Keep disk snapshot on final failure**: The old code called
   `service_clear_restart_reapply_snapshot()` when
   `SERVICE_RECOVERY_REAPPLY_MAX_ATTEMPTS` was reached.  Now the snapshot is
   preserved as a safety net for a future service restart or manual retry.

6. **RC7f — Per-step apply logging**: The reapply thread logs full intent
   (GPU offset, mem offset, power limit, fan mode, lock CI/MHz) before
   attempting the apply, plus the success/failure result.  Phase C now logs
   the loaded module versions (NVML and NvAPI).  The thread also reports
   driver responsiveness after each failed attempt.

7. **RC7g — Driver-health polling**: After a failed reapply, the retry delay
   is reduced from 5s to 1s if a lightweight NVML temperature read succeeds,
   indicating the driver has settled.

### Files changed

| File | Changes |
|------|---------|
| `source/main.cpp` | Added `g_serviceReapplyThread`, `g_serviceReapplyThreadId`, `g_serviceReapplyInProgress` globals; updated forward decls |
| `source/app_shared.h` | Added `serviceReapplyInProgress` to `ServiceSnapshot` |
| `source/main_service_runtime.cpp` | Replaced `service_maybe_run_recovery_reapply` with reapply thread proc + launcher + health check; added version logging in Phase C; added reapply-in-progress set/clear |
| `source/main_service_server.cpp` | Added `service_reregister_device_notification()`; added reapply thread cleanup in shutdown; changed main loop call to `service_check_reapply_thread_health()` |
| `source/main_state_sync.cpp` | Added `serviceReapplyInProgress` to `populate_service_snapshot` |
| `source/main_diagnostics.cpp` | Added VEH cleanup for reapply thread kills (mirrors RC5a recovery-thread pattern) |
| `build.py` | Updated regression source checks for renamed function |

### Verification

- `python build.py --check` — both binaries compile cleanly
- `python build.py --test` — all regression source checks pass
- `python build.py` — bumps to build 198, produces working executables
- Manual device-reconnect testing deferred to real GPU hardware

## 2026-06-01 (build 196 -> 197): Apply-after-recovery silently fails when VEH triggers a new recovery during the apply

### Problem
Build 196 (RC5a/RC5b/RC5c) broke the recovery-thread-VEH-kill wedge loop,
but a **setting-apply after a successful recovery reapply still fails**,
with the lock tail stuck at stock 2482 MHz and a fan curve maintenance
error. 2026-06-01 18:14–18:18 log session
(`logs/greencurve_debug.txt` + `logs/greencurve_log.txt` 18:16:25).

User-visible failure (`logs/greencurve_log.txt`):
- `Summary: Setting apply reported one or more failures`
- `Live GPU offset state: uniform 0 MHz`
- `Populated points: 128`
- `Lock tail verified at 2482 MHz @ 925 mV` (target was 2962 MHz)
- `Fan control change failed: Failed to start fan curve maintenance`

### Timeline (debug log)
- `18:15:01.464` — first Apply after a previous recovery succeeded; tail
  bookends settled at ci76=2962.
- `18:16:21.034` — second Apply starts; parameters identical to the
  first (`gpu=0 exclude=0 mem=3000 power=100 fanMode=2 lockCi=76
  lockMHz=2962 curvePoints=53`).
- `18:16:22.143` — `service recovery monitor: main loop launching
  in-process recovery (crashCount=3 pending=0)` — VEH crash on a
  different thread queues a recovery while the second Apply is still
  running.
- `18:16:22.145` — Recovery thread: Phase A (stop fan), then Phase B
  `service_safe_close_nvml: closing NVML (no shutdown)` +
  `service_safe_close_nvapi: closing NvAPI` — **both run WITHOUT
  `g_serviceRuntimeLock` held**.
- `18:16:22.190` — Apply (still holds the lock from the start of the
  request): `vf_curve_global_gpu_offset_supported: no backend or not
  writable`; `current_applied_gpu_offset_mhz: not Blackwell, returning
  NVML offset=0 kHz -> 0 MHz`; `apply_desired_settings: hasGpuOffset=1
  gpuOffsetMHz=0 family=blackwell backend=<none> bestGuess=0 read=0
  write=0`.
- `18:16:22.206` — Apply: `apply phase: apply: VF curve batch write`;
  correction pass computes `target=2962 MHz offset=480000 live=2482
  offs=0` for ci=74..126 — `offs=0` for every point because the
  backend is gone.
- `18:16:22.364` — Apply: `apply curve: settled refresh failed after
  curve batch`.
- All 25 correction passes complete with `offs=0`; user sees the
  failure summary above.

### Root cause
The recovery thread's `service_recover_gpu_connection()` previously took
`g_serviceRuntimeLock` only at the **start of Phase C** (the re-init
phase). Phases A (stop fan) and B (close stale NVML/NvAPI handles) ran
**without the lock**.

The pipe-server APPLY path takes the lock at
`main_service_server.cpp:649` and holds it across the entire
`service_apply_desired_settings()` call (2–15+ seconds on a
still-transitional driver: `Sleep(1000)` floor at
`gpu_backend_apply.cpp:62`, up-to-25-pass correction loop at
`gpu_backend_apply.cpp:741-934`, ~150-160 ms per
`read_live_curve_snapshot_settled(6, 25, ...)` at
`gpu_backend.cpp:825-852`, 5 s `WaitForSingleObject` on `nvidia-smi`
fallback at `gpu_backend.cpp:663`).

When a VEH-detected crash fires on a different thread during an
in-flight apply, `launch_recovery_thread()` is called and the recovery
thread tears down NVML/NvAPI under the apply's feet. The apply's
`vf_curve_global_gpu_offset_supported()` returns "no backend or not
writable", every curve write silently fails (`offs=0`), the lock tail
stays at stock 2482 MHz, and the fan curve maintenance thread fails to
start because the underlying NVML is gone.

The wedge watchdog in the main loop (`main_service_server.cpp:1017-1029`)
also had this same race: it `TerminateThread`'d the wedged fan thread,
then briefly checked the lock for orphaning, then **pre-emptively
called `service_safe_close_nvml()`** to free the stale module before
launching the recovery thread. That close also ran without the lock
and could race a concurrent apply.

### Fixes (build 197)
- **RC6a — `service_recover_gpu_connection()` takes
  `g_serviceRuntimeLock` at the start of Phase A (before the Phase B
  close).** The lock is held through all of Phases A, B, C, D, E. The
  redundant `lock_service_runtime()` at the start of Phase C is
  removed. All early-return `unlock_service_runtime()` cleanup paths
  and the final Phase E `unlock_service_runtime()` are preserved
  unchanged. `stop_service_fan_runtime_thread()`
  (`main_service_runtime.cpp:1611`) already handles being called with
  the lock held (releases/retakes around `WaitForSingleObject` on the
  fan thread) — no change needed. Function-level comment updated to
  describe the RC6 lock discipline.
- **RC6b — main loop wedge watchdog no longer pre-emptively closes
  NVML before launching the recovery thread.** The
  `service_safe_close_nvml()` call at `main_service_server.cpp:1027`
  is removed; the recovery thread (RC6a) now owns the NVML/NvAPI
  close in Phase B under the lock. The wedge watchdog keeps the
  brief `lock_service_runtime()` / `unlock_service_runtime()` orphan
  self-test (the existing `WAIT_ABANDONED` handling at
  `main_service_runtime.cpp:952-964` correctly resets the owner-TID
  and depth when a `TerminateThread` orphaned the lock).
- **RC6c — FP-08-003 source regression checks** in `build.py`:
  - RC6c-1: assert the function-level comment block above
    `service_recover_gpu_connection()` references the RC6 fix.
  - RC6c-2: extract the function body, count
    `lock_service_runtime();` calls **before** the first
    `service_safe_close_nvml();` (using a negative lookbehind to
    avoid matching the substring inside `unlock_service_runtime();`),
    and assert the count is exactly 1.
  - RC6c-3: assert the position of that single pre-close
    `lock_service_runtime();` is **before** the
    `service_safe_close_nvml();` position (i.e. Phase A lock comes
    before Phase B close, in source order).
  - RC6c-4: locate the wedge-watchdog block in
    `main_service_server.cpp` and assert it does NOT contain
    `service_safe_close_nvml();`.
- **RC6d — hermetic compiled test for lock-serialization.** A new
  test case in `fan_curve_regression.cpp`: two `CreateThread`
  instances against a fresh local `CRITICAL_SECTION` (not the
  production `g_serviceRuntimeLock`, which is created lazily by
  `ensure_service_runtime_lock()` and may be in an undefined state
  in a hermetic test). Each thread enters the CS, sleeps 200 ms,
  leaves. The test measures wall-clock time from
  `QueryPerformanceCounter` before `CreateThread` to after
  `WaitForMultipleObjects(TRUE)` with microsecond resolution, and
  asserts: (a) `maxConcurrent` (peak count of threads inside the CS
  simultaneously) is exactly 1; (b) `finalCount` is 0 after both
  threads release; (c) total wall-clock time is >= 350 ms (400 ms
  expected minus 50 ms slop for tick resolution and scheduler noise).
  Without serialization, the threads would overlap and the total
  would be ~200 ms, failing the threshold. Adds ~400 ms to the
  regression test runtime.

### Why RC5a/RC5b/RC5c didn't catch this
Build 195 addressed the recovery thread being killed by a VEH, but did
not address the recovery thread **tearing down NVML during an
in-flight apply**. The 18:14–18:18 log shows the recovery thread
running to completion (no VEH kill of the recovery thread this time);
it just ran in parallel with the apply. The crash that triggered the
recovery was on a *different* thread. The fix in RC5c (defer the
reapply to the queued reapply worker) reduced the in-recovery
`g_serviceGpuRecovering=1` window from 2-15+ s to ~0 s, but the
recovery's Phase A/B are still pre-`g_serviceRuntimeLock` and the
recovery itself takes < 1 s to do Phase A/B, which is plenty of time
to race an apply's VF curve batch write.

### Verification
- `python build.py --check` clean (`greencurve.exe` 541 KB,
  `greencurve-service.exe` 194 KB).
- `python build.py --test` passes all source regression checks
  (FP-08-001 RC1-RC4, FP-08-002 RC5a/RC5b/RC5c, FP-08-003 RC6a/RC6b/RC6c,
  plus the RC6d compiled lock-serialization test).
- `python build.py --test --asan` passes all regression checks under
  AddressSanitizer.
- Manual verification deferred to a live device-reconnect /
  driver-upgrade test on a real machine.

---

## 2026-06-01 (build 194 -> 195): In-process GPU recovery survives a VEH crash of the recovery thread

### Problem
Build 194 (RC1–RC4) broke the visible recovery wedge loop and preserved
user intent, but a single VEH crash *inside the recovery thread itself*
still permanently wedges the service. The 2026-06-01 17:23–17:25 log
session (`logs/greencurve_debug.txt`) shows successful recovery
phases A-C, a 4.7 s silence inside `apply_desired_settings_service`
(`service_apply_desired_settings: interactive=0 gpu=0 exclude=0 mem=3000
power=100 fanMode=2 lockCi=76 lockMHz=2962 curvePoints=53` is the last
log line), then `service_runtime_pulse: new VEH crash detected, launching
in-process recovery (crashCount=2)` and then
`launch_recovery_thread: recovery already in progress, skipping` every
~1–2 s forever. `crashCount` increments 2 → 3 → 4 → 5 over the next
30 s, each VEH crash is on a different thread (fan runtime / main
loop), each is skipped because the stuck flag check fires first. The
10 s cooldown (RC2) is correct but never reached.

### Root cause
The VEH in `main_diagnostics.cpp:431` `green_curve_vectored_handler` is
a **process-wide** VEH that `ExitThread(0)`s **any** thread that
access-violates inside `nvml.dll` / `nvapi64.dll`. It does not know
which thread it is killing, and it does **not** touch the recovery
flags (`g_serviceGpuRecovering`, `g_nvapiRecoveryInProgress`,
`g_serviceInitInProgress`). If the VEH kills the recovery thread
mid-Phase-D's `service_apply_desired_settings()` call:

- `g_serviceGpuRecovering = 1` (set at Phase A, only cleared in Phase E)
- `g_nvapiRecoveryInProgress = 1` (set at Phase A, only cleared in Phase E)
- `g_serviceInitInProgress = 1` (set at Phase C, only cleared in Phase E)
- `g_serviceRuntimeLock` abandoned, with stale
  `g_serviceRuntimeLockOwnerThreadId` and `g_serviceRuntimeLockDepth`
- recovery thread's TID is captured to a local and **discarded**;
  thread handle is `CloseHandle`'d immediately

No code path clears the stuck flags after a VEH kill. The service is
permanently wedged in "recovery already in progress, skipping" state —
only a process restart recovers it.

### Why build 194 made this worse
The apply path is fundamentally slow on a still-transitional driver: a
`Sleep(1000)` floor at `gpu_backend_apply.cpp:62`, plus an up-to-25
-pass curve correction loop, plus unbounded NVML/NVAPI readback/verify
loops. Total 2–15+ seconds. With Phase D holding
`g_serviceRuntimeLock` and `g_serviceGpuRecovering=1` for the entire
apply window, the recovery thread becomes the longest-running thread
in the service — long enough that a second VEH crash on a different
thread is statistically likely to land inside the same apply window.

### Fixes (build 195)
- **RC5a — VEH clears stuck recovery flags when it kills the recovery
  thread.** Add `g_serviceRecoveryThreadId` (volatile DWORD) and
  `g_serviceRecoveryThreadHandle` (volatile HANDLE) globals to
  `app_shared.h` / `app_shared.cpp`, mirroring the existing
  `g_fanRuntimeThreadId` pattern at `main.cpp:15`. In
  `green_curve_vectored_handler`, after the existing
  `g_nvmlCrashCount++` / `InterlockedExchange(&g_nvmlVhCrashed, 1)`
  block and before the `ExitThread` pivot, add a
  `GetCurrentThreadId() == g_serviceRecoveryThreadId` check that
  clears all three stuck recovery flags and the handle. All three
  stores are `InterlockedExchange` (lock-free, VEH-safe). The
  `g_serviceRuntimeLock` is still abandoned, but
  `lock_service_runtime()` already handles `WAIT_ABANDONED` correctly
  (resets owner-TID and depth to the new owner at
  `main_service_runtime.cpp:952-964`).
- **RC5b — `launch_recovery_thread` defensively detects a dead previous
  recovery thread.** The recovery thread's handle is no longer
  `CloseHandle`'d immediately after `CreateThread`; it is stored in
  `g_serviceRecoveryThreadHandle` so the next launch can detect a
  dead previous recovery thread. In `launch_recovery_thread()`, before
  the `g_serviceGpuRecovering` early-return, do a non-blocking
  `WaitForSingleObject(g_serviceRecoveryThreadHandle, 0)`. If
  signaled, the previous thread is dead — close the handle and
  clear the three stuck flags defensively. This is the backstop for
  any non-VEH kill. The recovery thread's normal-exit path clears
  `g_serviceRecoveryThreadId` (the handle is closed lazily by
  either the next `launch_recovery_thread` call or RC5a's VEH-side
  cleanup).
- **RC5c — Phase D defers the reapply to the queued reapply worker.**
  Replace the inline `service_reapply_desired_preserving_intent()`
  call in `service_recover_gpu_connection()` Phase D with
  `service_queue_recovery_reapply("driver recovery",
  NVML_CRASH_RECOVERY_WINDOW_MS + 1000)` (16 s). The queued reapply
  worker (`main_service_runtime.cpp:190`) already exists, already has
  timeouts + retries, and runs in the main service loop context
  where a VEH crash on any thread cannot kill the recovery thread.
  The 16 s delay is the 15 s `NVML_CRASH_RECOVERY_WINDOW_MS` safety
  window plus 1 s margin — the worker bails on
  `nvml_crash_recovery_active()` for the first 15 s, so the first
  successful attempt lands at 16 s when the safety window has just
  expired. If the desired was loaded from the disk snapshot (no
  in-memory `g_serviceHasActiveDesired`), the recovery thread
  promotes it to in-memory before queueing so
  `service_queue_recovery_reapply()` does not bail on the
  `!g_serviceHasActiveDesired` early-return. Behavior change:
  settings reappear ~16 s after a device reconnect instead of
  inline, but they appear reliably (today: appear immediately or
  never).

### New globals
- `g_serviceRecoveryThreadId` (volatile DWORD, app_shared.h): TID of
  the live recovery thread (0 if none). Set by
  `service_recovery_thread_proc` on entry, cleared on normal return.
  Used by the VEH (RC5a) to detect when it is killing the recovery
  thread.
- `g_serviceRecoveryThreadHandle` (volatile HANDLE, app_shared.h):
  HANDLE of the live recovery thread (nullptr if none). Set by
  `launch_recovery_thread` after `CreateThread` succeeds. Closed
  lazily by either the next `launch_recovery_thread` call (RC5b) or
  the VEH-side cleanup (RC5a) if a VEH crash lands first.

### Verification
- `python build.py` clean (no new errors or warnings; the pre-existing
  `__guard_check_icall_fptr` duplicate-symbol LTO warning is unchanged).
- `python build.py --test` passes all source regression checks,
  including the new RC5a/RC5b/RC5c checks under "FP-08-002".
- `python build.py --test --asan` passes all regression checks under
  AddressSanitizer.
- Manual verification deferred to a live device-reconnect / driver-upgrade
  test on a real machine.

### Open questions
- The apply path still has no per-call wall-clock timeout. The 16 s
  reapply delay + 5 s retry interval already give the driver time to
  settle; adding a per-call timeout would be invasive and is not
  justified by the new log evidence. If a future log shows a single
  apply taking > 60 s, consider adding a `TerminateThread` watchdog
  for the queued reapply worker.

---

## 2026-06-01 (build 193 -> 194): In-process GPU recovery no longer wedges after device reconnect or driver upgrade

### Problem
Build 193's in-process recovery was supposed to handle device reconnects and
driver-upgrade installs without restarting the service, but in practice it
entered a permanent recovery wedge. The 2026-06-01 16:12 session
(`logs/greencurve_debug.txt`) shows successful recovery phases A-D, then
`recovery: in-process GPU recovery COMPLETE`, then immediately
`service_main: fan pulse wedged for N ms` every ~3 s with N growing by 3 s
each iteration (17766 -> 20797 -> 23813 -> 26828 -> 29844 -> 32859). The GUI
continually received `crash recovery active, skipping hardware_initialize`
snapshots. No apply or reset would work. The service appeared to recover but
never actually finished the post-recovery reapply.

### Root Causes
1. **RC1 — Phase C cleared the safety guard that Phase D needed.**
   The old Phase C cleared `g_nvmlCrashCount` / `g_nvmlCrashTickMs` /
   `g_serviceGpuRecovering` so the recovery's own `nvml_ensure_ready()` could
   run. But Phase D's `hardware_initialize()` relies on
   `nvml_crash_recovery_active()` returning true to skip `refresh_global_state()`
   (the NVML reads that wedge on a still-transitional driver). Clearing the
   guard made `refresh_global_state()` run, and it wedged.
2. **RC2 — wedged recovery produced a ~3 s recovery-loop storm.**
   The wedge watchdog, main-loop monitor, and `service_runtime_pulse()` each
   independently called `launch_recovery_thread()` when a previous recovery
   wedged. With no cooldown, a single stuck recovery spawned a new one every
   ~3 s.
3. **RC3 — apply/reset failure wiped the active desired.**
   The old `service_apply_desired_settings` and `service_reset_all`
   unconditionally did `g_serviceHasActiveDesired = false; memset(...)` on
   any failure. A single transient apply (e.g. mid-recovery) wiped the
   in-memory profile, so the next recovery had nothing to reapply.
4. **RC4 — `FreeLibrary`+`LoadLibrary` may not pick up replaced on-disk driver
   files.** After a driver upgrade, the in-process `nvml.dll`/`nvapi64.dll` is
   the old version and the on-disk file is the new one. A plain
   `FreeLibrary`+`LoadLibrary` may return the old module.

### Fixes
- **RC1:** Keep the broader crash-recovery safety guard SET throughout
  Phase C/D. Add a separate `g_serviceInitInProgress` flag so the recovery's
  own `nvml_ensure_ready()` and `nvapi_qi()` calls bypass the safety-guard
  early-return via a scoped `initInProgress == 0` check. The guard is cleared
  in Phase E (after the reapply succeeds), at the same time
  `g_serviceInitInProgress` is cleared.
- **RC2:** Add `g_serviceLastRecoveryAttemptMs` +
  `SERVICE_RECOVERY_RELAUNCH_INTERVAL_MS` (10 s) in
  `launch_recovery_thread()`. A second launch within 10 s logs "cooldown
  active" and returns.
- **RC3:** `service_apply_desired_settings` no longer clears the active
  desired on failure (logs "preserving active desired" instead). On failure
  it now also calls `service_write_restart_reapply_snapshot()` as a safety
  net. `service_reset_all` still clears on success but writes the disk
  snapshot on partial failure. `service_reapply_desired_preserving_intent`
  always calls `service_write_restart_reapply_snapshot()`.
- **RC4:** Add `service_nvml_disk_version_changed()` /
  `service_nvapi_disk_version_changed()` that compare the in-process
  module's `GetModuleFileNameW` + `GetFileVersionInfo` against the on-disk
  `%SystemRoot%\System32\nvml.dll` / `nvapi64.dll` version. Called at the
  start of Phase B in `service_recover_gpu_connection` and in the
  `DBT_DEVICEARRIVAL` handler (via a new public
  `service_check_disk_version_on_device_arrival()` wrapper). When a version
  mismatch is detected, the log includes both versions so post-mortem
  analysis can confirm the upgrade happened. The actual reload is performed
  by the subsequent `service_safe_close_*` + Phase C `LoadLibrary`. `version.lib`
  is now linked into both `greencurve.exe` and `greencurve-service.exe`.

### New globals
- `g_serviceInitInProgress` (volatile LONG, app_shared.h): 1 while the
  recovery's own re-init is in progress (Phase C/D). Checked by
  `nvml_ensure_ready()` and `nvapi_qi()` to bypass the safety-guard
  early-return only for the recovery thread.
- `g_serviceLastRecoveryAttemptMs` (volatile ULONGLONG, app_shared.h):
  timestamp of the most recent `launch_recovery_thread()` call. Used by the
  10 s cooldown.

### Verification
- `python build.py` clean (no new errors or warnings; the pre-existing
  `__guard_check_icall_fptr` duplicate-symbol LTO warning is unchanged).
- `python build.py --test` passes all source regression checks, including
  the new RC1-RC4 checks under "FP-08-001".
- `python build.py --test --asan` passes all regression checks under
  AddressSanitizer.
- Manual verification deferred to a live device-reconnect / driver-upgrade
  test on a real machine.

### Follow-up
- The version-detect is currently informational only (logs a warning). A
  future build could add a `MoveFileEx` + `LoadLibrary` force-reload to
  guarantee the new on-disk file is picked up even when the loader's
  mapping table is sticky.

---

## 2026-05-31 (build 191 -> 193): Preserve and retry desired settings after driver reconnect recovery

### Problem
The service survived a GPU driver reconnect/driver-upgrade style reset far
enough to reinitialize NVML and NvAPI, but it did not reliably restore full
functionality afterwards. The 2026-05-31 log showed successful in-process
recovery phases A-D followed by `recovery: no reapply snapshot found`, then fan
manual-policy failures and a later manual GUI apply failure.

### Evidence
- Latest available VEH dump in `%ProgramData%\Green Curve` faults inside
  `nvml.dll!nvmlDeviceGetTemperature`, called from the service fan runtime.
  This is consistent with stale NVML state after driver reconnect.
- `logs/greencurve_debug.txt` shows recovery reloaded NVML/NvAPI and rebuilt
  hardware state, but skipped settings reapply because only the disk restart
  snapshot was consulted.
- Manual apply after recovery could fail with fan/OC driver errors; the old
  apply path then cleared `g_serviceHasActiveDesired`, losing automatic retry
  intent.

### Root Causes
1. Normal in-process recovery often has no restart-reapply snapshot on disk.
   The live service intent is `g_serviceActiveDesired`, so recovery must prefer
   that in-memory profile and use the disk snapshot only as fallback.
2. Failed recovery reapply cleared active desired settings, so transient
   post-driver-reset errors became permanent until the user manually reapplied.
3. A pipe-server request can be the first thread to touch stale NVML/NvAPI and
   crash under VEH. Recovery launch therefore cannot depend only on fan runtime
   pulse paths.
4. The post-recovery safety marker must not look newer than the recovery that
   created it; otherwise the service can relaunch recovery after the cooldown
   instead of clearing the marker and running queued reapply.
5. VEH cleanup must not call `nvmlShutdown()` on a broken driver stack.

### Fixes
- Recovery now chooses desired settings from active in-memory service state
  first, then falls back to `service_restart_reapply.bin`.
- Recovery reapply uses a preserving wrapper: on failure it restores the
  previous desired profile, keeps retry intent, and logs the preserved state.
- A queued recovery-reapply worker retries transient OC/fan failures for a
  bounded window after backend recovery, and clears the queue on successful
  apply/reset.
- The main service loop now launches recovery when it observes VEH crash or
  pending-device-recovery flags, even if no fan runtime is currently active.
- VEH service cleanup closes stale NVML by freeing the module without calling
  `nvmlShutdown()`.
- NvAPI close/reload now preserves the user's selected GPU index/explicit
  selection flag while clearing stale adapter handles.
- Recovery stamps `g_nvmlCrashTickMs` from the same completion tick as
  `g_serviceLastRecoveryTickMs`, so cooldown expiry clears the intentional
  safety marker instead of treating it as a fresh crash.

### Verification
- `python build.py --check` passes after source-regression checks were updated
  for active-desired reapply, queued retry, main-loop recovery launch, and
  no-`nvmlShutdown()` VEH cleanup.
- `python build.py --test` passes.
- `python build.py` succeeds and produces build 193 (`greencurve.exe` and
  `greencurve-service.exe`). The existing duplicate CFGuard symbol linker
  warning is still emitted but is non-fatal.

### Files changed
- `source/main_service_runtime.cpp`
- `source/main_service_server.cpp`
- `source/main_diagnostics.cpp`
- `source/main.cpp`
- `build.py`
- `llm-wiki/gpu-backend.md`
- `llm-wiki/index.md`
- `llm-wiki/log/recent.md`

---

## 2026-05-30 (build 187): Fix GPU recovery — clear recovery flags during re-init, hold g_serviceRuntimeLock, keep reconnect timer alive

### Problem
After build 186, the service survived GPU reconnect but the GUI stayed stuck on
"waiting for GPU snapshot". The log showed the recovery thread repeatedly failing
with `nvml_ensure_ready() failed` + `nvapi_init() FAILED` in a ~4s loop.

### Root Cause 1: Recovery flags blocked the recovery thread itself
`g_serviceGpuRecovering` caused `nvml_ensure_ready()` to return false immediately.
`g_nvapiRecoveryInProgress` caused `nvapi_qi()` to return nullptr, so `nvapi_init()`
always failed. The recovery thread deadlocked its own re-init by setting these
flags before attempting to use NVML/NvAPI.

### Fix 1: Clear flags during re-init (under lock)
In Phase C, the recovery thread now temporarily clears both flags while holding
`g_serviceRuntimeLock` (a mutex, auto-released on thread death). The pipe server
blocks on `lock_service_runtime()` during GET_SNAPSHOT/GET_TELEMETRY, preventing
races. On failure, flags are re-set to `g_serviceGpuRecovering=1` so the next
detection cycle retries.

### Root Cause 2: g_app.deviceRemoved blocked nvml_ensure_ready
The recovery thread didn't clear `g_app.deviceRemoved`, which `nvml_ensure_ready()`
checks first and returns false.

### Fix 2: Clear deviceRemoved before Phase C
Moved `g_app.deviceRemoved = false` to Phase C, before calling `nvml_ensure_ready()`.

### Root Cause 3: GUI reconnect timer killed after first ping
The timer handler killed itself on the first successful ping, then called
`refresh_curve()` once. If the snapshot had `loaded=false` (recovery still in
progress), the GUI stayed stuck with no automatic retry.

### Fix 3: Keep timer alive until g_app.loaded
The reconnect timer now checks `g_app.loaded` after refreshing. If false, the
timer stays alive and retries in 3 seconds.

### Files changed
- `source/main_service_runtime.cpp` — Phase C/D/E restructured: lock acquired,
  flags cleared, deviceRemoved cleared, Sleep drops/re-acquires lock, failure
  paths set `g_serviceGpuRecovering=1`
- `source/ui_main_window.cpp` — reconnect timer keeps polling until loaded

### What changed
The architecture was redesigned from "ExitProcess + SCM restart" to a proper
**in-process recovery** that never exits. The key insight: the old approach
failed because:

1. `nvmlShutdown()` can hang at 100% CPU on a dead driver → **skip it**.
   Just `FreeLibrary` the stale module without shutdown. Fresh `LoadLibrary`
   gets a clean instance.
2. Wedged threads (hung inside nvml.dll, not AV) can be `TerminateThread`ed
   → `FreeLibrary` the module → re-init. The orphaned critical section is
   detected via `TryEnterCriticalSection` and re-initialized.
3. NvAPI wasn't closed because other threads might hold cached pointers →
   **stop all GPU-touching threads first**, then close safely.

### New functions (`main_service_runtime.cpp`)
- `service_safe_close_nvml()` — FreeLibrary without nvmlShutdown
- `service_safe_close_nvapi()` — FreeLibrary with cache invalidation
- `service_recover_gpu_connection()` — 5-phase recovery: (A) stop threads,
  (B) close handles, (C) wait for driver settle, (D) re-init NVML/NvAPI,
  (E) reapply settings + resume fan
- `service_recovery_thread_proc()` / `launch_recovery_thread()` — thread entry

### Recovery triggers (no ExitProcess anywhere)
- **DBT_DEVICEARRIVAL** → `launch_recovery_thread()` (was `request_service_restart`)
- **VEH crash** → `service_runtime_pulse()` detects `crashCount > 0` → `launch_recovery_thread()`
- **Fan pulse wedge** >12s → `TerminateThread` + `service_safe_close_nvml()` + `launch_recovery_thread()`
- All three paths end in `service_recover_gpu_connection()` which re-inits NVML/NvAPI
  and reapplies the saved OC/fan profile from the restart-reapply snapshot.

### New safety flags
- `g_serviceGpuRecovering` (volatile LONG) — set during recovery; `nvml_ensure_ready()`
  returns false; pipe server serves cached data; dot not stack multiple recovery threads.
- `g_nvapiRecoveryInProgress` (volatile LONG) — set during NvAPI close/reload;
  `nvapi_qi()` returns nullptr → all NvAPI call sites fail gracefully.

### VEH now writes minidump
Before the ExitThread redirect, `write_veh_minidump()` writes a focused `.dmp`
to `%ProgramData%\Green Curve\greencurve_veh_*.dmp` with crash context
(minidump + breadcrumb text). Previously VEH-handled crashes were invisible.

### GUI auto-reconnect
`SERVICE_RECONNECT_TIMER_ID` (3s interval) polls the service when
`backgroundServiceAvailable == false`. Starts on `WM_CREATE`, `refresh_curve()`,
and IPC failures. Stops on successful reconnect.

### Recovery status in snapshot
`ServiceSnapshot` now has `serviceInRecovery` and `lastRecoveryTickMs` fields
so the GUI can show "GPU reconnecting..." instead of "service not responding".

### Files changed
- `source/app_shared.h` — recovery flags, SERVICE_RECONNECT_TIMER_ID, snapshot fields
- `source/app_shared.cpp` — g_serviceGpuRecovering, g_nvapiRecoveryInProgress globals
- `source/main.cpp` — forward declarations, g_serviceLastRecoveryTickMs
- `source/main_service_runtime.cpp` — recovery functions + pulse → recovery instead of restart
- `source/main_service_server.cpp` — arrival → recovery, wedge → recovery, no ExitProcess
- `source/main_diagnostics.cpp` — VEH minidump, debug_log_veh
- `source/main_runtime_nvml.cpp` — g_serviceGpuRecovering guard in nvml_ensure_ready
- `source/gpu_backend.cpp` — nvapi_qi recovery guard, nvapi_reset_qi_cache
- `source/main_gpu_front.cpp` — start_service_reconnect_timer_if_needed
- `source/ui_main_window.cpp` — reconnect timer handling in WM_TIMER + refresh_curve
- `source/main_state_sync.cpp` — snapshot recovery status fields
- `build.py` — updated FP-06 source regression checks
- `llm-wiki/log/recent.md`

### Verification
- `python build.py --test` passes
- `python build.py` produces build 186 (both .exe and -service.exe)

### Problem (build 182 verification: log 23:40, no crash dump)
After a GPU device reconnect, build 182 no longer crash-looped, but the GUI
**still could not talk to the service** and one CPU thread sat at **100%**. The
log shows the fan thread entering `nvml_ensure_ready: VEH crash flag set,
re-initializing NVML` → `TDR recovery history reset`, and then **the
`drain nvmlShutdown[..]` line never appears** — the thread is wedged *inside
nvml.dll* during the shutdown-drain / re-init, spinning at 100% CPU while
holding `g_serviceRuntimeLock`. With that lock held, the pipe server can never
answer the GUI → every client request times out with ERROR_BROKEN_PIPE (109).

### Root cause (final): in-process NVML re-init after a driver restart is unfixable
Across builds the in-process recovery has failed THREE distinct ways:
1. **Crash loop** — calling a stale device handle access-violates; VEH
   ExitThreads the caller; watchdog recreates it; repeat (builds ≤181).
2. **Silent no-op** — `nvmlInit_v2()` returns `ALREADY_INITIALIZED` and hands
   back a handle bound to the dead driver; reads AV forever (build 182's first
   theory).
3. **100% CPU hang** — `nvmlShutdown()` / re-init spins inside the
   dead-but-still-mapped `nvml.dll`, never returning, holding the runtime lock
   (build 182 actual). A hang the VEH cannot catch.

Conclusion: the already-mapped `nvml.dll`/`nvapi64.dll` retain internal state
bound to the dead driver instance and **cannot be safely refreshed in-process**.

### Fix (build 183): recover by restarting the service PROCESS
Detect the driver-restart condition WITHOUT touching NVML, then exit the process
so a fresh one maps clean driver DLLs and re-applies the saved profile.

1. **`nvml_ensure_ready()` (`main_runtime_nvml.cpp`)** — on `g_nvmlVhCrashed ||
   nvml_crash_recovery_active()`, set not-ready and `return false` immediately.
   No `nvmlShutdown`, no `close_nvapi`, no re-init in-process. Removed the entire
   shutdown-drain / `forceFreshInit` path.
2. **`service_runtime_pulse()` (`main_service_runtime.cpp`)** — restructured: if
   `deviceRemoved` → idle; if `g_nvmlCrashCount > 0 || pendingDeviceRecovery` →
   `request_service_restart()` and return (NO NVML/NVAPI call); else run the fan
   tick. The check precedes `apply_fan_curve_tick()`.
3. **`request_service_restart()` (`main.cpp`)** — idempotent; snapshots the
   active desired settings to `%ProgramData%\Green Curve\service_restart_reapply.bin`
   (`service_write_restart_reapply_snapshot`) and signals `g_serviceStopEvent`.
4. **`service_main` loop (`main_service_server.cpp`)** — when the restart flag is
   set, `ExitProcess(1)` (skips the NVML cleanup, which would itself hang).
   Reports `SERVICE_STOP_PENDING` + non-zero `dwServiceSpecificExitCode` so the
   SCM failure-action restarts us.
5. **SCM auto-restart (`service_install_or_remove`)** —
   `ChangeServiceConfig2W(SERVICE_CONFIG_FAILURE_ACTIONS)` with three
   `SC_ACTION_RESTART` steps (2s/5s/10s), 1-day reset. **Requires a service
   reinstall to take effect** (the GUI reinstalls on build change).
6. **Startup reapply (`service_startup_reapply_thread_proc`)** — on boot, if the
   snapshot file exists, wait up to ~30s for `hardware_initialize` + populated
   curve, then `service_apply_desired_settings(resetOcBeforeApply=true)`, then
   delete the snapshot. Normal boots (no file) are unaffected → existing
   logon-apply behavior unchanged.
7. **Device-event handlers** — REMOVAL just sets flags + snapshots (no
   blocking/NVML); ARRIVAL calls `request_service_restart()` instead of
   `nvml_ensure_ready()` (both ran on the SCM control thread and could block).
8. **Wedge watchdog** — the fan thread stamps `g_serviceFanPulseHeartbeatMs` /
   `g_serviceFanPulseInFlight` around each pulse; the main loop forces a restart
   if a pulse is in-flight > `SERVICE_FAN_PULSE_WEDGE_TIMEOUT_MS` (12s),
   covering the case where a read HANGS (not AVs) so the VEH never fires.

### Files changed
- `source/main.cpp` — `request_service_restart` (def moved after debug_log), restart snapshot fwd-decls, heartbeat globals, `SERVICE_FAN_PULSE_WEDGE_TIMEOUT_MS`
- `source/main_runtime_nvml.cpp` — `nvml_ensure_ready()` no in-process re-init during recovery
- `source/main_service_runtime.cpp` — restart-based recovery in pulse; snapshot read/write; startup reapply thread; pulse heartbeat
- `source/main_service_server.cpp` — ExitProcess on restart; SCM failure actions; non-blocking device handlers; wedge watchdog; launch startup reapply
- `build.py` — FP-06-001..006 added; stale FP-01-003/FP-03-003/FP-03-005/FP-04-002/FP-05-001 retired or converted to forbid_text
- `llm-wiki/gpu-backend.md`, `llm-wiki/log/recent.md`

### Verification
- `python build.py --check` / `--test` pass; `python build.py` produced build 183.
- **Needs reinstall + end-to-end test**: reinstall the service (for the SCM
  failure actions), apply a fan-curve + OC profile, run a driver-restart utility, and
  confirm: log shows `recovery via process restart` → process exits → SCM
  relaunches → `service startup reapply: applied successfully`; GUI reconnects
  and reset/apply work; no 100%-CPU thread; no `greencurve_crash_*.dmp`.

### Open questions / stale-risk
- SCM failure-action restart requires the service to exit non-zero AND the
  failure action to be configured — verify both on the user's machine after
  reinstall (older installs won't have the failure actions).
- The normal SCM-stop path still calls `nvml_set_fan_auto`/`close_nvml`; if NVML
  is wedged at stop time this could hang shutdown. Not the reported bug; revisit
  if stop-hangs are observed.

---

## 2026-05-29 (build 182): GPU reconnect — NVML never re-initialized (nvmlShutdown never called); RESET/APPLY crashed pipe server; fatal pipe double-close

### Problem (build 181 verification: log + crash dump 18:54:03)
Build 181's back-off engaged (`recovery deferred during NVML crash back-off`),
but NVML **never recovered** (crashCount climbed 2→6 over a minute, never a
`reapply after recovery successful`), and clicking the GUI's **reset to
defaults** returned `Failed reading service response (error 109)` =
ERROR_BROKEN_PIPE. The crash dump showed the *process itself* died with
`STATUS_INVALID_HANDLE (0xC0000008)` in `ConnectNamedPipe` — NOT an nvml AV, so
the VEH didn't catch it; it hit the unhandled filter and terminated the service.

### Root causes (three, found via the dump + code audit)

**Bug C1 — `nvmlShutdown()` was NEVER called during recovery, so NVML never
re-initialized (`main_runtime_nvml.cpp` / `main_probe_config.cpp`).** The VEH
(`green_curve_vectored_handler`) sets `g_app.nvmlReady=false` and
`g_app.nvmlDevice=nullptr` **before** calling `close_nvml()`, and BOTH
`close_nvml()` (`if (g_app.nvmlReady && g_nvml_api.shutdown)`) and
`nvml_ensure_ready()` (`if (g_nvml_api.shutdown && g_app.nvmlDevice)`) guard the
shutdown on those now-zeroed fields → shutdown skipped. The NVIDIA driver keeps
`nvml.dll` mapped, so its internal init state survived; `nvmlInit_v2()` returned
`NVML_ERROR_ALREADY_INITIALIZED` and `nvmlDeviceGetHandleByIndex` handed back a
device handle bound to the **dead driver instance** → every read access-violated
forever. (This invalidates the old wiki claim that "nvmlShutdown+init doesn't
recover NVML" — shutdown was never actually being executed.)

**Bug C2 — RESET/APPLY command handlers ran NVML during the recovery window
(`main_service_server.cpp`).** `SERVICE_CMD_RESET`/`SERVICE_CMD_APPLY` call
`service_reset_all` / `service_apply_desired_settings` (NVAPI/NVML writes +
refresh) directly on the pipe server thread. Clicking reset during the
transitional window crashed that thread (VEH ExitThread) → response never sent →
ERROR_BROKEN_PIPE.

**Bug C3 — fatal pipe-handle double-close (`main_service_server.cpp`).** The
pipe loop closed its local `pipe` at end of each iteration but never cleared the
shared `g_servicePipeHandle`; the next iteration's pre-publish
`CloseHandle(g_servicePipeHandle)` (line 417) re-closed it, and the watchdog
also closes it on thread death — all unsynchronized. During the crash storm this
double-closed/raced a handle whose value had been reused, so `ConnectNamedPipe`
ran on an invalid handle → `STATUS_INVALID_HANDLE`. Strict Handle Checks turns
that into a hard fault the VEH can't catch → process death → broken pipe.

### Fixes (build 182)

1. **`nvml_ensure_ready()` drains the init refcount before re-init.** On VEH
   crash recovery (`forceFreshInit`), after the library is loaded and pointers
   resolved, call `nvmlShutdown()` in a loop until it returns
   `NVML_ERROR_UNINITIALIZED`, then `nvmlInit_v2()`. If init still reports
   `ALREADY_INITIALIZED`, drain harder and retry once. This must run on the fan
   runtime thread, never in the VEH (NVML lock may be held by the faulting
   call). Removed the broken `&& g_app.nvmlDevice` shutdown guard.
2. **RESET/APPLY reject during `nvml_crash_recovery_active()`** with a friendly
   "GPU driver is recovering after a device reconnect; please retry in a few
   seconds." message + cached snapshot, instead of crashing the pipe server.
   The fan runtime thread auto-reapplies the active profile once NVML recovers.
3. **Race-free pipe handle ownership.** New `service_close_owned_pipe()` uses
   `InterlockedCompareExchangePointer` to atomically claim the slot before
   closing, so exactly one of the pipe thread or the watchdog closes a handle.
   The buggy pre-publish `CloseHandle(g_servicePipeHandle)` is removed; publish
   uses `InterlockedExchangePointer`; the watchdog reclaims via
   `InterlockedExchangePointer`. The pipe thread can only be VEH-killed inside a
   command handler (never inside the close helper), so there is no leak window.

### Net effect
- C1 makes NVML genuinely recover, so the crash storm ends and the active OC/fan
  profile auto-reapplies once the driver settles.
- Even if C1 were insufficient on some driver, **C2+C3 alone fix the reported
  symptom**: RESET returns a graceful message (not error 109), and a pipe-thread
  death can no longer kill the whole process — the watchdog recreates it and the
  GUI keeps getting cached snapshots.

### Files changed
- `source/main_runtime_nvml.cpp` — `nvml_ensure_ready()` shutdown-drain before re-init; `forceFreshInit`
- `source/main_service_server.cpp` — RESET/APPLY recovery-window guards; `service_close_owned_pipe()`; race-free publish/reclaim
- `build.py` — FP-05-001..003 regression checks (+ `forbid_text` for the removed broken guards)
- `llm-wiki/gpu-backend.md`, `llm-wiki/log/recent.md` — this entry

### Verification
- `python build.py --check` / `--test` pass; `python build.py` produced build 182.
- Needs end-to-end test: a driver-restart utility with a fan-curve + OC profile, confirm log shows `drain nvmlShutdown[..]` then `reapply after recovery successful`, GUI reset/apply work after ~one back-off cycle, and NO `greencurve_crash_*.dmp` with `ConnectNamedPipe`/`0xC0000008`.

---

## 2026-05-29 (build 181): Fix STILL-perpetual crash loop after GPU reconnect — `wasFreshInit` skip was dead code; gate recovery reapply behind crash back-off

### Problem (build 180 verification log, 2026-05-29)
After a GPU device reconnect (via a driver-restart utility), build 180 still entered a
**perpetual crash loop** — `crashCount` climbed 1→15+ over ~36 s and never
recovered. The debug log showed every recovery cycle reaching
`apply phase: hardware initialize: global state refresh` and then the fan
runtime thread dying ("stale handle detected, recreating"). There was **never**
a `hardware initialize: complete`, never a `reapply after recovery successful`,
and never a `recovery reapply back-off` — i.e. neither build-180 mitigation
actually engaged.

### Root causes (two independent bugs, both in build 180's own mitigations)

**Bug A — `wasFreshInit` skip was dead code (`main_state_sync.cpp`):**
Build 180 intended to skip `refresh_global_state()` (NVML reads that AV during
the transitional window) on a "fresh init" via
`bool wasFreshInit = (g_app.gpuHandle == nullptr)`. But that line sat at the
*end* of `hardware_initialize()`, **after** `nvapi_enum_gpu()` had already set
`g_app.gpuHandle = handles[i]` (gpu_backend.cpp:157,182). So `wasFreshInit` was
**always false** and `refresh_global_state()` **always ran** — the exact NVML
access-violation the build was supposed to prevent. This crashed the fan thread
(recovery) AND could crash the pipe server thread (GUI snapshot) since both call
`hardware_initialize()`.

**Bug B — recovery reapply back-off never engaged (`main_service_runtime.cpp`):**
The `g_recoveryFailCount` exponential back-off only incremented when
`service_apply_desired_settings()` **returned false**. But the reapply
**crashed** (VEH → `ExitThread`) instead of returning, so the counter stayed 0,
the back-off never applied, and the heavy reapply ran on *every* pulse (~every
2–3 s as the watchdog recreated the thread), crashing every time. The NVML
crash back-off (`nvmlBackoff`) correctly throttled the *fan tick* but did **not**
gate the *recovery reapply*, which ran regardless.

### Fixes (build 181)

1. **`main_state_sync.cpp` — skip keyed on a real recovery signal.** Replaced
   the dead `wasFreshInit` check with `nvml_crash_recovery_active()` (see #3).
   On normal startup it is false (refresh runs); during the post-crash window it
   is true (refresh skipped on both the fan-recovery and pipe-server paths).

2. **`main_service_runtime.cpp` — gate the whole recovery reapply behind the
   NVML crash back-off.** Restructured the recovery section to
   `if (deviceRemoved) … else if (nvmlBackoff) { defer, keep fan thread alive }
   else { re-init + reapply }`. While inside the back-off window NO NVML/NVAPI
   call is issued, so the fan thread no longer crashes and `g_serviceRuntimeLock`
   is no longer abandoned. The back-off doubles per crash up to 30 s, giving the
   driver progressively more time to settle. A successful reapply clears
   `g_nvmlCrashCount`/`g_nvmlCrashTickMs` and `pendingDeviceRecovery`; a non-
   crashing failure stamps the back-off so the next pulse defers. Removed the
   broken `g_recoveryFailCount`. The fan tick (which calls `nvml_ensure_ready`
   → processes the VEH flag) acts as the readiness probe before the heavy
   reapply runs.

3. **`main.cpp` — shared `nvml_crash_recovery_active()` helper +
   `NVML_CRASH_RECOVERY_WINDOW_MS` (35 s).** True while `g_nvmlCrashCount > 0`
   and `now - g_nvmlCrashTickMs < 35 s` (just above the 30 s back-off ceiling +
   grace). Used by `hardware_initialize()`, the SNAPSHOT handler, and the
   TELEMETRY handler so GUI requests serve cached globals instead of access-
   violating. The fan thread clears the count on success → window ends early;
   the time cap guarantees telemetry self-heals even with no fan thread (auto
   fan mode).

4. **`main_service_server.cpp` — SNAPSHOT handler no longer consumes
   `g_nvmlVhCrashed`.** It now checks `nvml_crash_recovery_active()` so *every*
   snapshot in the window is safe (the old consume-once flag let the *second*
   snapshot crash the pipe server). The flag is left intact for the fan thread's
   `nvml_ensure_ready()` to drive the NVML/NVAPI re-init.

### Why the GUI was "stuck"
With the service crash-looping forever, the pipe server kept dying and the GUI
timed out / showed the service as not-responding, so live controls stayed
disabled. Once the service recovers (bounded ~30–60 s now), snapshots succeed
with fresh data and Apply is enabled again.

### Files changed
- `source/main.cpp` — `NVML_CRASH_RECOVERY_WINDOW_MS` + `nvml_crash_recovery_active()`
- `source/main_state_sync.cpp` — `hardware_initialize()` skip via helper (was dead `wasFreshInit`)
- `source/main_service_runtime.cpp` — recovery reapply gated by `nvmlBackoff`; crash-count-driven back-off; removed `g_recoveryFailCount`; telemetry `recentCrash` uses helper
- `source/main_service_server.cpp` — SNAPSHOT handler uses recovery-window helper, stops consuming `g_nvmlVhCrashed`
- `build.py` — `forbid_text()` helper; new FP-04-001..004 regression checks
- `llm-wiki/log/recent.md` — this entry

### Verification
- `python build.py --check` passes (both binaries)
- `python build.py --test` passes (pure tests + FP-04 source checks)
- `python build.py` produced build 181
- Needs end-to-end test: apply fan-curve + OC profile, run a driver-restart utility, confirm the service stops crash-looping, auto-reapplies within ~30–60 s, and the GUI re-enables Apply.

---

## 2026-05-28 (build 180): Fix perpetual crash loop after GPU reconnect — skip refresh_global_state() in recovery, protect pipe server, add backoff

### Problem (build 179)
After GPU device reconnect, the service entered a **perpetual crash loop**:
1. Fan runtime thread T1: recovery reapply → `hardware_initialize()` succeeds → `refresh_global_state()` → **NVML read access-violates** → VEH kills T1 → `g_serviceRuntimeLock` abandoned
2. Pipe server thread acquires abandoned lock → telemetry handler → `service_refresh_telemetry_for_request()` → no fan thread → calls `service_runtime_pulse()` → same crash → VEH kills pipe thread → GUI gets ERROR_BROKEN_PIPE (109)
3. Main loop watchdog recreates both threads → goto 1, repeating ~every 2 seconds indefinitely

The VF curve readback (NVAPI) in `hardware_initialize()` succeeded, but the subsequent NVML telemetry reads in `refresh_global_state()` (power limit, clock offsets, fans) crashed because the GPU kernel driver was still in a transitional state after the reconnect — even though NVML was freshly re-initialized.

### Fixes

1. **Skip `refresh_global_state()` on fresh init** (`source/main_state_sync.cpp:300-312`): After `close_nvapi()` clears `g_app.gpuHandle` to nullptr, `hardware_initialize()` detects this (`wasFreshInit`) and skips the `refresh_global_state()` call. The VF curve readback already provides all necessary data. Telemetry is refreshed on the next pulse.

2. **Don't call `service_runtime_pulse()` from pipe server during crash recovery** (`source/main_service_runtime.cpp:455`): Added `recentCrash` guard using `g_nvmlCrashTickMs` (10-second window). When NVML recently crashed, the telemetry handler uses cached data instead of calling `service_runtime_pulse()` — preventing the pipe server thread from being killed by VEH.

3. **Recovery reapply backoff** (`source/main_service_runtime.cpp:577-627`): Added `g_recoveryFailCount` / `g_recoveryFailTickMs` static variables. After each consecutive recovery reapply failure, exponential backoff is applied (5s, 10s, 20s, 40s, 60s cap). On success, the counter is cleared. This prevents the endless crash-spiral when the GPU driver is still unstable after reconnect.

### Files changed
- `source/main_state_sync.cpp` — skip `refresh_global_state()` on fresh init
- `source/main_service_runtime.cpp` — recent-crash guard in telemetry handler; recovery reapply backoff

### Verification
- `python build.py --check` passes
- `python build.py --test` passes
- `python build.py` produces build 180

### Problem (build 176)
After GPU device reconnect, the service survived the NVML crash (VEH), re-initialized NVML, and freed NVAPI (`FreeLibrary` in `nvml_ensure_ready()`), but the `nvapi_qi()` function had a **static-local** cached `nvapi_QueryInterface` function pointer. After `FreeLibrary`, this cache was still non-null and pointed to unloaded/unmapped nvapi64.dll memory.

When the recovery path called `service_apply_desired_settings()` → `hardware_initialize()` → early return (because `g_app.gpuHandle`, `g_app.loaded`, `g_app.vfBackend` were all still set) → `reset_oc_before_gui_apply()` → `nvapi_set_gpu_offset()` → `nvapi_qi()` → called through stale cached pointer → **access violation at unmapped address**.

### Root causes

**Bug A — `nvapi_qi()` static-local cache:**
The function used a function-local `static qi_func qi = nullptr` to cache the `GetProcAddress` result. Once set, it was never cleared — not even by `close_nvapi()`. After `FreeLibrary(g_app.hNvApi)`, the DLL was unmapped but the cached pointer remained, causing the next call to jump to freed memory.

**Bug B — `close_nvapi()` didn't clear hardware-init guards:**
After `close_nvapi()` freed the NVAPI DLL, `g_app.gpuHandle`, `g_app.loaded`, and `g_app.vfBackend` were still set from before the crash. When `hardware_initialize()` was called next, it returned early at line 275 (`if (g_app.gpuHandle && g_app.loaded && g_app.vfBackend) return true;`) without re-initializing NVAPI, leaving `g_app.hNvApi = nullptr` and stale handles in place.

**Bug C — Device arrival handler:**
In `main_service_server.cpp`'s `DBT_DEVICEARRIVAL` handler, `deviceWasRemoved` was read **after** `g_app.deviceRemoved` was already cleared to false, making the check always fail.

### Fix

1. **`gpu_backend.cpp`: Made nvapi_qi() cache a module-level static (`g_nvapiQi`)** that `close_nvapi()` can reset to `nullptr`. On the next call, `nvapi_qi()` properly reloads `nvapi64.dll` fresh.
2. **`gpu_backend.cpp`: Updated `close_nvapi()`** to also clear `g_app.gpuHandle`, `g_app.loaded`, `g_app.vfBackend`, and `g_nvapiQi` — forcing `hardware_initialize()` to fully re-enumerate GPUs with fresh NVAPI handles on the next call.
3. **`main_runtime_nvml.cpp`: `nvml_ensure_ready()`** now calls `close_nvapi()` instead of inline `FreeLibrary`, ensuring all NVAPI state (including the module-level cache, adapter cache, and hardware-init guards) is properly cleared.
4. **`main_service_runtime.cpp`: Recovery path** also uses `close_nvapi()` instead of inline `FreeLibrary`.
5. **`main_service_server.cpp`: Device arrival handler** now saves `deviceWasRemoved` BEFORE clearing `g_app.deviceRemoved`.
6. **`main.cpp`**: Added `close_nvapi()` forward declaration so the above call sites compile.
7. **`build.py`**: Added 6 new source regression checks (FP-03-001 through FP-03-005) verifying the fixes.
8. **Debug logging**: Added recovery state logging (`hNvApi`, `gpuHandle`, `loaded`, `vfBackend`, `nvmlReady`) for future diagnosis.

### Files changed
- `source/gpu_backend.cpp` — module-level `g_nvapiQi` cache; `close_nvapi()` clears all GPU state
- `source/main_runtime_nvml.cpp` — `nvml_ensure_ready()` uses `close_nvapi()` instead of inline `FreeLibrary`
- `source/main_service_runtime.cpp` — recovery path uses `close_nvapi()`; added debug logging
- `source/main_service_server.cpp` — device arrival handler fix
- `source/main.cpp` — added `close_nvapi()` forward declaration
- `build.py` — 6 new regression source checks
- `llm-wiki/log/recent.md` — this entry

### Testing
- `python build.py --check` passes for both GUI and service binaries
- `python build.py --test` passes (all regression tests)
- `python build.py` produces build 179
- Needs end-to-end test: apply profile, GPU reconnect, verify auto-restore + GUI connectivity

### Problem (build 174)
The direct reapply (build 174) started `service_apply_desired_settings()` after GPU
reconnect, but crashed during the **reset-before-apply** phase which writes VF curve
points one-by-one via **NVAPI**.  NVAPI handles were also stale after the driver
restart (only NVML was invalidated).  The NVAPI call crashed, VEH killed the fan
thread mid-apply — service appeared dead to the user.

### Fix (build 175-176)
1. **`nvml_ensure_ready()` VEH re-init**: call `FreeLibrary(g_app.hNvApi)` to
   invalidate NVAPI alongside NVML.  The subsequent reapply path calls
   `nvapi_query_interface()` which re-loads nvapi64.dll fresh.
2. **Post-reapply fan thread ensure**: call `ensure_service_fan_runtime_thread()`
   after the reapply to guarantee the fan thread is running (VEH may have killed
   the previous one and the watchdog might not have triggered yet).

### Files changed
- `source/main_runtime_nvml.cpp` — added `FreeLibrary(g_app.hNvApi)` in VEH re-init
- `source/main_service_runtime.cpp` — added `ensure_service_fan_runtime_thread()`
  after recovery reapply

### Testing
- Build 176 compiles clean, all regression tests pass.
- Needs end-to-end test: apply profile, GPU reconnect, verify auto-restore.

---

## 2026-05-28 (build 174): Skip NVML persistence probe — directly reapply settings after GPU reconnect

### Problem (build 173)
After GPU device reconnect, the service recovered NVML (nvmlShutdown + nvmlInit_v2
succeeded, fresh device handle obtained) but `service_check_oc_persistence(true)`
called `getGpcClkVfOffset` which returned **NVML_ERROR_NOT_INITIALIZED (error 3)**
— the "fresh" handle was functional enough for init/select but `getGpcClkVfOffset`
still failed.  The TDR loop guard (3 recoveries in 5 min) was also exhausted
during the crash+backoff phase, permanently blocking reapply.

Log evidence:
```
[14:37:51.914] oc_persistence: getGpcClkVfOffset failed (result=3), skipping reapply
...
[14:37:59.957] oc_persistence: loop detected (3 recoveries in 300000 ms), blocking re-apply
```

### Root cause
`getGpcClkVfOffset` is an NVML function that requires additional initialization
not covered by nvmlShutdown + nvmlInit_v2 after a driver restart.  Even `close_nvml()`
+ `FreeLibrary` + reload (build 173) returned the same cached DLL mapping from
Windows, which didn't actually refresh the internal state needed for this call.

### Fix
1. **Recovery path (`service_runtime_pulse()`)**: skip `service_check_oc_persistence(true)`
   entirely.  Instead, directly call `service_apply_desired_settings()` with the
   cached `g_serviceActiveDesired` — same code path as the manual Apply button
   (proven working after reconnect in build 173).  This uses NVAPI for VF curve/memory OC
   and NVML for fan control, both of which recover after nvmlShutdown + re-init.
2. **`nvml_ensure_ready()` VEH re-init**: use `nvmlShutdown()` + `nvmlInit_v2()`
   instead of `close_nvml()` (FreeLibrary + reload).  nvmlShutdown() properly
   resets NVML's internal context so `nvmlInit_v2()` obtains a fresh context and
   valid device handle.  TDR recovery history is zeroed after successful re-init
   to prevent the "loop detected" guard from blocking reapply.
3. **`g_serviceActiveDesired.resetOcBeforeApply = true`** set before calling
   `service_apply_desired_settings()` so the OC baseline is reset before reapply.

### Files changed
- `source/main_runtime_nvml.cpp` — `nvml_ensure_ready()` VEH re-init: shutdown+init
  without FreeLibrary; TDR budget reset
- `source/main_service_runtime.cpp` — recovery section: direct
  `service_apply_desired_settings()` instead of `service_check_oc_persistence()`

### Testing
- Build 174 compiles clean, all regression tests pass.
- Needs end-to-end test: start service, apply profile, GPU device reconnect
  (via a driver-restart utility), verify settings are auto-restored within ~30s backoff window.

---

## 2026-05-28 (build 171-173): NVML crash backoff + stale-handle re-init + OC persistence reapply after GPU reconnect

### Problem
After GPU device reconnect (driver restart), the service survived (build 170)
but NVML still crashed on every pulse (stale device handle).  The VEH killed
the fan thread in an infinite loop.  Even when the crash loop eventually
stopped (backoff, build 171), OC persistence was never actually checked
because `nvml_ensure_ready()` returned `true` with the stale handle and
`service_check_oc_persistence()` silently returned at its `getGpcClkVfOffset`
probe.  The GUI showed cached profile values but hardware was at stock.

### Root cause (build 171 failure)
1. `nvml_ensure_ready()`: returned `true` immediately when `g_app.nvmlReady`
   was still `true` and `g_app.nvmlDevice` was non-null — both were stale from
   before the driver restart.
2. `service_check_oc_persistence(true)`: called `getGpcClkVfOffset` on the
   stale handle → returned non-NVML_SUCCESS → function returned **silently**
   at line 64 with no debug log and no reapply.
3. Recovery logic in `service_runtime_pulse()`: used `else if (nvmlBackoff)`
   which only fired when backoff was active.  When backoff expired AND
   `nvml_ensure_ready()` returned true, neither branch matched — OC
   persistence was never called.

### Fix (build 172-173)
- **`nvml_ensure_ready()`**: checks `g_nvmlVhCrashed` flag at entry; if set,
  calls `close_nvml()` + full re-init to obtain fresh handles.  Clears the
  VEH flag so subsequent calls don't re-close NVML.
- **Recovery section in `service_runtime_pulse()`**: redesigned with
  `recoveryAttempted` / `persistenceNeeded` booleans.
  - `persistenceNeeded = (crashCount > 0)` ensures OC persistence is checked
    in ALL post-crash paths, not just the explicit recovery branch.
  - Three sub-paths all lead to the same tail: clear backoff state + call
    `service_check_oc_persistence(true)`.
- **`service_check_oc_persistence()`**: added `debug_log` when
  `getGpcClkVfOffset` fails (`"oc_persistence: getGpcClkVfOffset failed"`)
  for future diagnosis.
- **`main.cpp`**: added `g_nvmlCrashTickMs` (ULONGLONG) and
  `g_nvmlCrashCount` (LONG) globals for crash back-off state.
- **`main_diagnostics.cpp`**: VEH sets crash tick and increments crash count.

### Files changed
- `source/main.cpp` — new globals `g_nvmlCrashTickMs`, `g_nvmlCrashCount`
- `source/main_diagnostics.cpp` — VEH sets crash tick and increments count
- `source/main_runtime_nvml.cpp` — `nvml_ensure_ready()` VEH-aware re-init
- `source/main_service_runtime.cpp` — redesigned recovery section,
  `service_check_oc_persistence()` debug logging

### Testing
- Build 173 compiles clean, all regression tests pass.
- Needs end-to-end test: start service, apply profile, GPU device reconnect
  (via a driver-restart utility), verify that settings are auto-reapplied within ~30s.

---

## 2026-05-28 (build 168-170): Fix CS corruption — release g_appLock before NVML calls in apply_fan_curve_tick

### Problem
The GUI showed "service not responding" after GPU device reconnect (via a driver-restart utility).
Build 167's `g_nvmlVhCrashed` flag correctly prevented the snapshot handler from
crashing (log: `"NVML recently crashed, using cached globals"`), but the
snapshot response was never sent — the GUI timed out after ~2s.

### Root cause
`apply_fan_curve_tick()` held `g_appLock` (EnterCriticalSection at line 445)
while calling `nvml_read_temperature()` (line 503). When the call crashed in
nvml.dll (stale driver handle), the VEH killed the thread via ExitThread while
the critical section was still locked.

Windows critical sections do not recover from dead owners: `OwningThread` still
points to the terminated thread and `RecursionCount` remains >0. On the next
fan runtime pulse, `EnterCriticalSection(&g_appLock)` deadlocks because the CS
internal semaphore is never signaled. The stuck fan thread holds the runtime
mutex (acquired before the pulse), so the snapshot handler also blocks on
`lock_service_runtime()` → GUI times out.

Log evidence:
```
# Fan thread killed by VEH (NVML crash while holding g_appLock):
[14:10:22.750] ensure_service_fan_runtime_thread: stale handle detected, recreating
[14:10:22.751] service_fan_runtime_thread: started
# No pulse logs appear — first EnterCriticalSection deadlocks:
[14:10:25.757] ensure_service_fan_runtime_thread: already running
[14:10:34.786] ensure_service_fan_runtime_thread: already running
# Snapshot handler acquires abandoned mutex, completes curve read...
[14:10:35.926] warning: service runtime lock was abandoned by previous owner
[14:10:36.031] service snapshot: NVML recently crashed, using cached globals
# ...but then blocks on something — response never sent:
[14:10:37.919] startup service snapshot failed: Timed out during reading service response
```

### Fix
Restructured `apply_fan_curve_tick()` into three phases:

1. **Phase 1 (under CS):** Snapshot mode flags, target percent, curve config,
   timing state to local variables. Release CS.
2. **Phase 2 (NO CS):** Execute NVML function calls
   (`nvml_read_temperature`, `nvml_manual_fan_matches_target`,
   `nvml_set_fan_manual`) without holding `g_appLock`. If nvml.dll crashes,
   the VEH kills the thread but the CS is clean.
3. **Phase 3 (under CS):** Re-acquire CS and write results back to `g_app`
   (fan mode, percent, last-applied temp, runtime success/failure, sync UI).

All three NVML call sites now execute without holding the critical section.
If the VEH kills the thread, the CS remains in a valid unlocked state.

### Files changed
- `source/main_fan_runtime.cpp` — restructured `apply_fan_curve_tick()`

## 2026-05-28 (build 167): Replace broken VEH error-return simulation with g_nvmlVhCrashed flag

### Problem
Build 166 tried to simulate error returns from crashed NVML functions by
reading the return address from [RSP] and setting RAX to an error code.
This is fundamentally wrong: when a crash happens INSIDE a function (not at a
CALL instruction), RSP points into the function's local stack frame, not at
the return address. The VEH read garbage from [RSP] as the return address,
causing the thread to jump to random code.

### Root cause
On x64, the return address is pushed by the CALL instruction BEFORE the
callee establishes its stack frame. Once inside the function (e.g.,
nvml.dll's internal function), RSP may be at any offset within the local
frame. Reading [RSP] as the return address is incorrect — the actual return
address is further up the stack at an unknown offset.

Log evidence (build 166 test log):
```
[14:00:12.608] read_live_curve_snapshot_settled completed
[14:00:13.120] ensure_service_fan_runtime_thread: already running
[14:00:14.488] startup service snapshot failed: Timed out during reading service response
```
The snapshot handler processed the VF curve read but then called
`refresh_global_state()` which crashed in NVML. The broken VEH attempted to
simulate an error return, jumped to garbage, and the thread never completed
the response — GUI timed out.

### Fix
1. **Add `g_nvmlVhCrashed` flag** (`volatile LONG` in `main.cpp`, set by VEH
   on any NVML crash).
2. **VEH terminates ALL crashing threads via ExitThread** (not just fan runtime).
   The pipe watchdog recreates the pipe server thread. The flag ensures the
   NEW thread won't call NVML and crash again.
3. **Snapshot handler checks flag**: if `g_nvmlVhCrashed` was set, skips
   `refresh_global_state()` (which calls NVML directly) and uses cached
   globals instead. The flag is atomically read-and-cleared via
   `InterlockedExchange`.
4. **`service_runtime_pulse()` clears the flag** after successful NVML
   recovery (`InterlockedExchange(&g_nvmlVhCrashed, 0)`), so subsequent
   snapshot requests get fresh data.

### Files changed
- `source/main.cpp` — added `g_nvmlVhCrashed` flag
- `source/main_diagnostics.cpp` — VEH sets flag, ExitThread for all threads
- `source/main_service_server.cpp` — snapshot handler checks flag
- `source/main_service_runtime.cpp` — clears flag after NVML recovery

## 2026-05-28 (build 166): VEH — only ExitThread fan runtime; simulate error return for pipe server

### Problem
After build 165 stopped the NVAPI cross-thread crash, the pipe server thread
still crashed when processing SNAPSHOT requests (NVML stale handle → AV inside
nvml.dll).  The VEH killed it via ExitThread, orphaning the named pipe and
abandoning the runtime mutex.  The pipe watchdog recreated the thread, but
the GUI connection was lost (ERROR_PIPE_BUSY) and the GUI timed out with
"background service was not responding".

Log evidence:
```
service_main: pipe server thread died, recreating   (3-second intervals)
warning: service runtime lock was abandoned by previous owner
service_send_request: CreateFileW pipe failed error=231 retry=1  (ERROR_PIPE_BUSY)
refresh_background_service_state: ping FAILED err=[Timed out waiting for the background service]
```

### Root cause
The VEH killed ANY thread crashing in nvml.dll — fan runtime AND pipe server.
Killing the pipe server thread orphaned:
- The named pipe handle (client connection half-open)
- The service runtime mutex (WAIT_ABANDONED)
- The snapshot / telemetry response never sent

### Fix
Check `GetCurrentThreadId() == g_fanRuntimeThreadId` in the VEH:

- **Fan runtime thread**: ExitThread(0) as before — watchdog recreates.
- **Other threads (pipe server)**: Simulate a function return by popping the
  return address from [RSP], incrementing RSP by 8, and setting RAX to
  `NVML_ERROR_NOT_SUPPORTED` (3).  The caller (e.g. `nvml_read_clock_offsets`)
  sees the error and returns false, allowing the snapshot handler to recover
  with cached data instead of crashing the thread.

### Files changed
- `source/main_diagnostics.cpp` — thread-ID-aware VEH dispatch

## 2026-05-28 (build 165): Fix VEH-triggered cross-thread crash — don't FreeLibrary NVAPI from VEH

### Problem
After the VEH killed the fan runtime thread and it was restarted, the service
crash-dumped with an access violation at an unmapped address
(`0x00007ffd57d5a470`).  The crash was NOT inside nvml.dll — it was in our
own code at `greencurve_service+0x5aeb`, calling through a function pointer
to freed/unmapped memory.

### Root cause
The VEH handler did:
```cpp
close_nvml();                            // zeros g_nvml_api — safe
FreeLibrary(g_app.hNvApi);               // unloads nvapi64.dll — NOT safe
g_app.hNvApi = nullptr;
```

`nvapi_qi()` caches the `nvapi_QueryInterface` function pointer in a static
local.  When the VEH calls `FreeLibrary(g_app.hNvApi)`, nvapi64.dll is
unloaded from the process.  Any other thread (e.g. the pipe server thread
processing a `SERVICE_CMD_GET_SNAPSHOT`) that holds a reference to the
cached `nvapi_qi` pointer then calls through unmapped memory → access
violation.

The crash dump confirmed this:
- Fault address: `0x00007ffd57d5a470` (unmapped — previously nvapi64.dll).
- Not caught by VEH (crash address is in our exe, not nvml.dll).
- Hit unhandled exception filter → process terminated.

### Fix
Removed `FreeLibrary(g_app.hNvApi)` from the VEH handler.  NVAPI invalidation
now happens only in `service_runtime_pulse()` when the recovered fan runtime
thread detects stale NVML handles — a safe context where no other thread is
concurrently using NVAPI.

### Files changed
- `source/main_diagnostics.cpp` — removed `FreeLibrary(g_app.hNvApi)` from VEH,
  added explanatory comment.

## 2026-05-27 (build 164): Pipe server thread watchdog + NVAPI invalidation on VEH crash

### Problem
After the VEH killed the fan runtime thread and it was restarted, the pipe
server thread also crashed when the GUI connected and the snapshot handler
called `refresh_global_state()` → `nvml_read_clock_offsets()` →
`nvmlDeviceGetPerformanceState()`. The VEH killed IT too (via ExitThread),
orphaning the named pipe and causing the GUI to time out with
"background service was not responding".

Additionally, stale NVAPI handles could also cause crashes — the VEH only
invalidated NVML but left NVAPI loaded.

### Changes
1. **Store g_fanRuntimeThreadId** — set in `ensure_service_fan_runtime_thread()`
   for selective VEH behaviour (e.g. log which thread crashed).
2. **Invalidate NVAPI in VEH** — `FreeLibrary(g_app.hNvApi)` alongside
   `close_nvml()` so stale NVAPI pointer is reloaded fresh.
3. **Store g_servicePipeHandle globally** — the pipe server thread saves the
   `CreateNamedPipeW` handle in `g_servicePipeHandle` so the main loop can
   close orphaned handles when the thread is killed.
4. **Pipe server thread watchdog** — in the main loop's 3-second watchdog,
   check if `g_servicePipeThread` is signaled (thread died). If so, close
   orphaned pipe handle, close old thread handle, create new pipe server thread.
5. **Clear g_servicePipeHandle on normal close** — at all points where the
   pipe server thread closes the pipe normally, set `g_servicePipeHandle =
   INVALID_HANDLE_VALUE` to avoid double-close by the watchdog.

## 2026-05-27 (build 162): Fix VEH RSP alignment — subtract 8 for missing CALL

### Problem (build 161)
Crash still happened in `ntdll!SbSelectProcedure+0x16b` with `movaps` fault.
RSP was aligned to 16 bytes (RSP & ~15), but ExitThread's prologue
(`push rbp; mov rbp, rsp`) shifted RSP DOWN by 8 bytes, making RBP end in
0x8 instead of 0x0. All subsequent `movaps [rbp+offset]` instructions faulted.

### Root cause
VEH sets Rip/Rsp directly → OS resumes at the target without a CALL instruction.
A normal CALL pushes the return address (8 bytes), so the callee's `push rbp`
restores 16-byte alignment. Without the CALL, `push rbp` shifts RSP from
aligned (0x...0) to misaligned (0x...8).

### Fix
```cpp
// WAS (build 161, broken):
info->ContextRecord->Rsp = info->ContextRecord->Rsp & ~(ULONG_PTR)15;

// NOW (build 162, correct):
info->ContextRecord->Rsp = (info->ContextRecord->Rsp & ~(ULONG_PTR)15) - 8;
```

## 2026-05-27 (build 161): Fix VEH — align RSP to 16-byte boundary for ExitThread

### Problem
First VEH incarnation (stack-scanning) set RIP to garbage on stack → crash in our
own code at `greencurve_service+0x4701`. Second incarnation (ExitThread termination)
crashed during thread cleanup (`ntdll!SbSelectProcedure+0x16b`) because RSP from
the nvml crash context (`0x...678`) was not 16-byte aligned.

The x64 Windows ABI requires 16-byte RSP alignment at every CALL.  ExitThread →
RtlExitUserThread → LdrShutdownThread → DLL detach (rpcrt4) → CloseHandle →
ntdll!SbSelectProcedure uses `movaps xmmword ptr [rbp+170h],xmm0` which faults
on misaligned addresses.  This crashed the entire process rather than just the
fan runtime thread.

### Fix
```cpp
info->ContextRecord->Rsp = info->ContextRecord->Rsp & ~(ULONG_PTR)15;
```
This ensures ExitThread's thread cleanup path has properly aligned stack for
movaps instructions.

### Confirmed Working
- Log line: `stale handle detected, recreating` at 14:46:23 (build 160) — fan
  thread was terminated and restarted correctly the first time.
- Second restart exposed the alignment bug (build 160 → crash).
- Build 161 fixes alignment; should survive multiple consecutive restarts.

## 2026-05-27: VectoredExceptionHandler — survive nvml.dll crash without device events

### Summary
A driver-restart utility restarts the WDDM driver without
sending `DBT_DEVICEREMOVEPENDING` or `DBT_DEVICEARRIVAL`, so device notification
handling cannot prevent the crash. When the driver restarts, the already-loaded
nvml.dll has stale internal device handles. Calling `nvmlDeviceGetTemperature`
reads from invalid memory → access violation inside nvml.dll.

Since llvm-mingw does not support `__try/__except` SEH blocks, we register a
VectoredExceptionHandler (first-chance VEH) that:

1. Catches the access violation inside nvml.dll
2. Invalidates NVML state (`nvmlReady=false`, `close_nvml()` / FreeLibrary)
3. Scans the stack for a return address in our module via `VirtualQuery`
4. Sets `RIP`/`RSP`/`RAX` to resume past the crash with `NVML_ERROR_UNKNOWN`
5. The caller (`nvml_read_temperature`) sees the error and returns false
6. Fan runtime thread survives and recovers on next pulse

### Files changed
- `main_diagnostics.cpp`: Added `green_curve_vectored_handler()` VEH function
- `main_service_server.cpp`: `AddVectoredExceptionHandler(1, ...)` at service start
- `main.cpp`: Forward declaration for VEH

## 2026-05-27: Real root cause — SERVICE_ACCEPT_DEVICE_EVENTS missing from dwControlsAccepted

### Summary
The service never received DBT_DEVICEREMOVEPENDING / DBT_DEVICEARRIVAL because
`dwControlsAccepted` was missing `SERVICE_ACCEPT_DEVICE_EVENTS` (0x1000). Without
it, the SCM filters out `SERVICE_CONTROL_DEVICEEVENT` entirely — regardless of
`RegisterDeviceNotificationW` flags. This was the REAL root cause.

The earlier `RegisterDeviceNotificationW` flag fix (SERVICE_NOTIFY_STATUS_CHANGE →
DEVICE_NOTIFY_SERVICE_HANDLE) was correct but irrelevant while the SCM blocked
all device events.

Changes in this commit:
- `main_service_server.cpp`: add local `#define SERVICE_ACCEPT_DEVICE_EVENTS 0x00001000`
  (missing from llvm-mingw winsvc.h) and include it in `dwControlsAccepted`

The `deviceRemoved` guards from commit 671cdfc remain as defense-in-depth for
race conditions between device removal notification and in-flight NVML calls.

## 2026-05-27: Root cause fix — RegisterDeviceNotificationW wrong flag + NVML stale-handle crash guards

### Summary
Fixed two bugs that caused the service to crash during GPU device disconnect/reconnect:

1. **RegisterDeviceNotificationW used wrong flag** — The 3rd argument was
   `SERVICE_NOTIFY_STATUS_CHANGE` (0x00000002) instead of `DEVICE_NOTIFY_SERVICE_HANDLE`
   (0x00000001). 0x02 is NOT a valid flag for `RegisterDeviceNotificationW`, so the
   kernel ignored the registration and **no device events ever reached the service
   control handler**. The debug log confirmed zero "device event" messages despite
   "registered for GUID_DEVINTERFACE_DISPLAY_ADAPTER" succeeding. Fixed by changing
   the flag to `DEVICE_NOTIFY_SERVICE_HANDLE`.

2. **nvml.dll stays loaded for process lifetime** — Unlike NVAPI where `FreeLibrary()`
   actually unloads the DLL, nvml.dll is kept mapped by the NVIDIA driver even after
   `FreeLibrary()`. When the GPU device is disconnected without device notifications
   arriving (due to bug #1), the fan runtime thread calls `nvmlDeviceGetTemperature`
   with a stale device handle → access violation in nvml.dll (read of invalid memory).
   The crash breadcrumb showed `deviceRemoved=0 gpuDriverDll=1` confirming the device
   notification handler never ran.

3. **Added defense-in-depth guards** — Even with the device notification flag fixed,
   added `deviceRemoved` checks:
   - `nvml_ensure_ready()` now returns false immediately when `g_app.deviceRemoved`
     is true, preventing any nvml.dll calls during device removal
   - `service_runtime_pulse()` now skips NVML staleness recovery when
     `g_app.deviceRemoved` is true, avoiding unnecessary `close_nvml()`/re-init loops

### Details
- `main_service_server.cpp` line 862: `SERVICE_NOTIFY_STATUS_CHANGE` → `DEVICE_NOTIFY_SERVICE_HANDLE`
- `main_runtime_nvml.cpp` `nvml_ensure_ready()`: Added early-return when `deviceRemoved`
- `main_service_runtime.cpp` `service_runtime_pulse()`: Added `deviceWasRemoved` guard
  before NVML recovery code
- Build 155, all regression tests pass

## 2026-05-26: Driver upgrade resilience — device notifications, NVAPI invalidation, startup recovery

### Summary
Made the Green Curve service survive NVIDIA driver upgrade installs without crashing
and automatically re-apply OC/fan settings after driver reinstall. Previously the
service would crash (stale NVAPI/NVML handles pointing to unloaded driver code) and
never recover its settings after the driver was reinstalled.

1. **Device change notifications for GUID_DEVINTERFACE_DISPLAY_ADAPTER** — Service
   registers `RegisterDeviceNotificationW()` with the display adapter class GUID so it
   receives `SERVICE_CONTROL_DEVICEEVENT` only for GPU/display device events (not USB,
   network, or other device changes which would cause false positives). The handler on
   `DBT_DEVICEREMOVEPENDING`/`DBT_DEVICEREMOVECOMPLETE` immediately stops the fan
   runtime thread and closes NVML to prevent further calls on stale handles. On
   `DBT_DEVICEARRIVAL` the removal flag is cleared so subsequent runtime checks can
   detect the device is back.

2. **NVAPI DLL invalidation on NVML recovery** — When `service_runtime_pulse()` detects
   NVML staleness (driver update without reboot), it now also frees and nullifies
   `g_app.hNvApi`. The next `nvapi_qi()` call will re-load `nvapi64.dll` fresh with
   valid function pointers. This prevents crashes from NVAPI calls hitting unloaded
   driver code during driver upgrade.

3. **Startup deferred recovery** — If the service had `pendingDeviceRecovery=true` at
   crash time (meaning active settings were lost mid-upgrade), it schedules a 3-second
   deferred recovery thread on startup that calls `service_check_oc_persistence(true)`
   to re-apply settings once the driver has settled.

### Details
- `AppData` struct added: `deviceRemoved`, `pendingDeviceRecovery`, `deviceRemoveTimeMs`
- `close_nvapi()` added to `gpu_backend.cpp` — frees DLL, clears adapter cache
- Crash breadcrumb enhanced: `deviceRemoved` field in crash text for diagnosis
- Device notification handle `g_serviceDeviceNotifyHandle` tracked and cleaned up on shutdown
- Crash breadcrumb: deviceRemoved state included in text for diagnosis

### Validation
- `python build.py --test` passes (compiled + source regression tests).
- `python build.py` passes, build 143.

### Files touched
- `source/app_shared.h` — deviceRemoved, pendingDeviceRecovery, deviceRemoveTimeMs
- `source/main_service_server.cpp` — device notification registration, SERVICE_CONTROL_DEVICEEVENT handling, startup recovery thread, shutdown cleanup
- `source/main_service_runtime.cpp` — NVAPI invalidation in service_runtime_pulse
- `source/gpu_backend.cpp` — close_nvapi() function
- `source/main_diagnostics.cpp` — deviceRemoved in crash breadcrumb
- `llm-wiki/log/recent.md` — this entry

---

## 2026-05-22: Fix auto-reapply OC persistence — reset-before-apply, broadened checks, TDR vs standby separation

### Summary
Fixed three issues in the automatic OC re-apply feature used on standby/resume
and GPU driver recovery (TDR/update):

1. **Auto-reapply now resets to defaults before applying** — `service_check_oc_persistence()`
   sets `g_serviceActiveDesired.resetOcBeforeApply = true` before calling
   `service_apply_desired_settings()`, so the safe reset sequence from
   `reset_oc_before_gui_apply()` (GPU offset→0, power→100%, VF offsets→0, 1s settle)
   runs before re-applying. Previously, auto-reapply applied settings on top of
   whatever the driver currently had, which could cause unexpected resulting clocks.

2. **Standby/resume no longer counts toward TDR loop detection** — Added `bool
   isDriverRecovery` parameter to `service_check_oc_persistence()`. Only actual
   driver recoveries (NVML staleness in `service_runtime_pulse()`) call
   `record_driver_recovery()`. Standby/resume events get the same persistence
   check but do not consume the 3-recovery/5-minute TDR loop budget.

3. **Persistence check broadened to GPU offset + memory offset + power limit** —
   Previously only checked GPU offset (`getGpcClkVfOffset`). Now also checks
   memory offset (`getMemClkVfOffset`) and power limit percentage
   (`getPowerLimit`/`getPowerDefaultLimit`), with per-field tolerance. All three
   must be intact to skip re-apply. Detail logging shows which specific settings
   were lost.

### Details
- `service_check_oc_persistence()` signature: `void()` → `void(bool isDriverRecovery)`
- `record_driver_recovery()` and TDR backoff check wrapped in `if (isDriverRecovery)`
- GPU offset check: unchanged tolerance (5 MHz)
- New memory offset check: 5 MHz tolerance, guarded by `getMemClkVfOffset` availability
- New power limit check: 2% tolerance, guarded by `getPowerLimit`/`getPowerDefaultLimit` availability
- Old log: `"oc_persistence: GPU offset intact"` / `"GPU offset lost"`
- New logs: `"oc_persistence: checking (driver recovery/standby resume)"`,
  `"oc_persistence: all settings intact (gpu=... mem=... power=...)"`,
  `"oc_persistence: settings lost: <detail>, re-applying with reset"`

### Validation
- `python build.py --test` passes (compiled + source regression tests).
- `python build.py` passes, build 142.

### Files touched
- `source/main_service_runtime.cpp` — `service_check_oc_persistence()`, NVML recovery caller
- `source/main_service_server.cpp` — standby/resume caller
- `build.py` — source regression checks updated
- `llm-wiki/log/recent.md` — this entry

---
## 2026-05-20: Audit fix wave — parser, fan-curve, IPC, file-write, and diagnostics hardening

### Summary
Implemented a focused code-audit fix wave while preserving best-guess VF curve
read/write support for unsupported/untested GPU architectures such as Pascal,
Turing, and Ampere.

### Fixes
- Fixed `fan_curve_normalize()` out-of-bounds risk when a config had fewer than
  two enabled points. Degenerate curves now reset to the default curve while
  preserving normalized poll interval and hysteresis.
- Hardened `parse_int_strict()` on Windows and Linux with `errno == ERANGE`
  checks, so huge INI/CLI integers are rejected instead of accepted as saturated
  `LONG_MIN`/`LONG_MAX`.
- Made `HeapBuffer::read_at()` / `write_at()` use subtraction-based bounds checks
  to avoid `size_t` addition overflow.
- Fixed a service pipe ACL fail-open bug: if restricted ACL creation fails or
  returns no descriptor, the pipe server defers creation instead of falling back
  to a default DACL.
- Made service startup fail closed if the pipe server thread cannot be created.
- Hardened client IPC by checking `SetNamedPipeHandleState()`, validating service
  response magic/version before using payloads, defensively NUL-terminating
  response text fields, and summarizing pipe connect retries.
- Replaced duplicated file write loops with `write_all_to_handle()` using
  `size_t` accounting, and changed non-service atomic writes to unique
  `CREATE_NEW` temp files.
- Replaced `atoi()` parsing of `nvidia-smi` max clocks with strict MHz-prefix
  parsing.
- Aggregated large VF offset warnings into one summary per curve batch, reducing
  locked-tail log spam while preserving diagnostic evidence.
- Fixed `python build.py --test --asan` on Windows by adding the bundled
  llvm-mingw `bin` directory to the test process `PATH`.

### Validation
- `python build.py --test` passes.
- `python build.py --test --asan` passes.
- `python build.py --check` passes.
- `python build.py --target linux --check` passes.
- `python build.py` passes and produced build 141.
- Binary inspection confirmed `DYNAMIC_BASE`, `NX_COMPAT`, `HIGH_ENTROPY_VA`,
  and `GUARD_CF` on both Windows executables.

### Files touched
- `build.py`
- `source/app_shared.h`
- `source/config_utils.cpp`
- `source/fan_curve.cpp`
- `source/gpu_backend.cpp`
- `source/gpu_backend_apply.cpp`
- `source/linux_port.cpp`
- `source/main_diagnostics.cpp`
- `source/main_service_ipc.cpp`
- `source/main_service_server.cpp`
- `llm-wiki/*` relevant pages

---

## 2026-05-20: Hotfix — ScopedProcess::assign() closes pipe handles, causing STATS_INVALID_HANDLE crash

### Crash
Build 139 (with cross-talk fix) crashed immediately at service startup:
```
STATUS_INVALID_HANDLE (c0000008) — PeekNamedPipe with invalid handle
```
The Strict Handle Checks mitigation (added in build 133) turned this from a
silent PeekNamedPipe failure into a hard crash.

### Root cause
In `read_nvidia_smi_max_clocks()` (`source/gpu_backend.cpp:203`), the code
called `proc.assign_pipes(hRead, hWrite)` to store pipe handles, then later
called `proc.assign(pi.hProcess, pi.hThread)` to store process/thread handles.
But `ScopedProcess::assign()` starts with `cleanup()`, which closes ALL stored
handles — including `pipeRead` (= `hRead`) and `pipeWrite` (= `hWrite`).

After `assign()`:
1. `hRead` was closed → `PeekNamedPipe(hRead, ...)` crashed with invalid handle
2. `hWrite` was closed → `CloseHandle(hWrite)` was a double-close
3. The old `proc.pipeWrite = nullptr` after `assign()` was too late (handle
   already closed)

Without Strict Handle Checks, this was silently tolerated: `PeekNamedPipe` just
returned FALSE and `GetLastError` = `ERROR_INVALID_HANDLE`, causing the read
loop to exit early and return no nvidia-smi data. The function then silently
exited with `smiClocksRead = true` and max clocks never populated, but this
was harmless for the service's core functionality.

### Fix
Before calling `proc.assign()`, null out `proc.pipeRead` and `proc.pipeWrite` so
that `cleanup()` inside `assign()` skips them. After `assign()`, restore
`proc.pipeRead = hRead` so the ScopedProcess destructor still closes the read
pipe on function exit.

`proc.pipeWrite` stays null (the child process owns the inherited copy of the
write handle; our copy is closed manually via `CloseHandle(hWrite)`).

### Files touched
- `source/gpu_backend.cpp` — detach pipe handles before assign(), restore afterward

### Build
- Build 140 passes: `python build.py` + `python build.py --test` pass

---

## 2026-05-20: Extend non-tail cross-talk acceptance to explicit points near locked tail

### Summary
Build 89's cross-talk fix for non-tail points excluded user-explicit points
(`!userExplicitPoint` guard). This prevented the correction loop from
converging on explicit non-tail points adjacent to the flattened locked tail
(e.g., point 75 stuck at 2917 MHz instead of 2932 MHz, 15 MHz off due to
hardware cross-talk from +480 MHz tail offsets).

### Fix
In `source/gpu_backend_apply.cpp`, removed the blanket `!userExplicitPoint`
exclusion and replaced it with a proximity check: a non-tail explicit point
within 5 VF points of any locked tail point is considered "near the locked
tail" and also benefits from cross-talk acceptance. The logic:

1. Iterates `lockedTailMask` once per diverging point to find the minimum
   distance to any tail point.
2. If distance ≤ 5, `nearLockedTail = true` → `acceptNonTailReadback` includes
   this explicit point.
3. The debug log annotates accepted explicit points with
   `"(cross-talk near locked tail)"` for diagnosis.
4. Genuine failures on explicit points far from the tail (distance > 5) remain
   strict — the correction loop keeps the requested target.

The threshold of 5 points is conservative: cross-talk from flattened tail
offsets (~480 MHz peak) shifts immediately adjacent points (~10-20 MHz) most
strongly, with negligible effect beyond a few points.

### Regression check updated
The source regression check at `build.py:1496` matched the old hardcoded
`"non-tail readback point %d actual %u MHz != target"` string. Updated to
match the new format string `"non-tail %s point %d actual %u MHz != target"`
which covers both "explicit" and "readback" variants.

### Files touched
- `source/gpu_backend_apply.cpp` — proximity-based cross-talk acceptance
- `build.py` — updated regression check needle

### Build
- Build 138 passes: `python build.py` + `python build.py --test` pass

---

## 2026-05-20: Fix CFG duplicate symbol warning — root cause found, cosmetic only

### Investigation
The `ld.lld: warning: duplicate symbol: __guard_check_icall_fptr` warning is
unavoidable with MinGW's CFG implementation. Root cause:

1. **`loadcfg.o`** in `libmingwex.a` always references `__guard_check_icall_fptr`
   as an undefined symbol (for the PE load config directory). This triggers the
   linker to extract `mingw_cfguard_support.o` from the same library.
2. `mingw_cfguard_support.o` defines `__guard_check_icall_fptr` as a **data
   pointer** in `.00cfg` section (initialized to point to the local dummy check
   function). Our `cfg_glue.cpp` defines it as a **function** in `.text`.
3. The dispatch function `__guard_dispatch_icall_dummy` is just `jmpq *%rax`
   (no validation at all) — the compiler emits DIRECT calls to
   `__guard_check_icall_fptr`, not through the `.00cfg` function pointer.

### Conclusion
`--allow-multiple-definition` is required. The warning is cosmetic — LLD uses
our definition (first on command line). The CRT's `.00cfg` data pointer is
never referenced (dead code eliminated by `--gc-sections` + LTO).

### Attempted fixes that didn't help
- Extracting and renaming `__guard_check_icall_fptr` in the CRT object: works
  for the patched object, but `loadcfg.o` always pulls in the ORIGINAL
  `mingw_cfguard_support.o` from the library.
- Removing `.00cfg` section from CRT object: caused orphaned relocation errors.
- Providing our own `.00cfg$Z` with function pointers: the compiler calls
  `__guard_check_icall_fptr` directly as a FUNCTION, not through `.00cfg`.

### Build
- Build 137 passes: `python build.py` + `python build.py --test` pass
- Warning is cosmetic and documented in `WINDOWS_FLAGS`

---

## 2026-05-20: Add Strict Handle Checks process mitigation + reparse point protection audit

### Summary
Added `SetProcessMitigationPolicy(ProcessStrictHandleCheckPolicy)` to both GUI and
service binaries at startup. Also audited junction redirection protection — already
implemented in code.

### Strict Handle Checks
Enabled via `initialize_process_mitigations()` in `cfg_glue.cpp`, called from both
entry points:
- GUI: `entry.cpp:372` (after `SetProcessDPIAware` + `init_dpi`)
- Service: `main.cpp:674` (service WinMain)

What it does: any use of an invalid handle (double-close, use-after-close, wrong
handle value) raises `STATUS_INVALID_HANDLE` instead of silently corrupting state.
Set permanent (`HandleExceptionsPermanentlyEnabled=1`) so child threads inherit it.

### Junction redirection protection audit
Code already has reparse point verification before writes:
- `verify_parent_no_reparse_point()` (main_diagnostics.cpp:312-346) — checks every
  path component for FILE_ATTRIBUTE_REPARSE_POINT before any write
- `service_path_has_reparse_component()` (main_service_server.cpp:145-190) —
  service-specific path validation
- `service_verify_written_file_path()` (main_service_server.cpp:235-268) — post-write
  verification via `GetFinalPathNameByHandleA`
- `write_text_file_atomic_service()` — uses `CREATE_NEW` to prevent race/CVE-2024 style
  TOCTOU through temp file swap

Attack surface assessed: a GPU utility reading/writing config only in its own
AppData directory, with no untrusted file I/O. Windows `ProcessReparsePointMitigationPolicy`
was tested (enum values 17-22 via `SetProcessMitigationPolicy`) but not available
on this Windows 11 build. Code-level protection is the pragmatic choice.

### Files touched
- `source/cfg_glue.cpp` — added `initialize_process_mitigations()` with Strict
  Handle Checks, raised local `_WIN32_WINNT` to 0x0A00 for API visibility
- `source/main.cpp` — added `extern` declaration + call in service WinMain
- `source/entry.cpp` — added call in GUI WinMain

### Build
- Build 133 passes: `python build.py` + `python build.py --test` pass
- Both binaries start cleanly with mitigations active

---

## 2026-05-20: CFG override investigation — `RtlValidateUserCallTarget` vs module-range check

### Summary
Tested ntdll's `RtlValidateUserCallTarget` (ordinal 1647) as a stricter CFG validation
backend. It works correctly when called directly (both name and ordinal resolve), but
calls from the MinGW CRT's `__guard_dispatch_icall_fptr` during CRT init cause fast-fail
crashes. The module-range check approach (`GetModuleHandleExW(FROM_ADDRESS)`) is the
robust choice for our use case.

### RtlValidateUserCallTarget verification
- Export found in ntdll.dll at name `RtlValidateUserCallTarget` and ordinal 1647
- Both resolve to the same address (`0x7FFA15044930`)
- Accepts ntdll exports (e.g., `NtQuerySystemInformation`) — returns cleanly
- Rejects wild/invalid addresses — fast-fail as expected
- Verified by `llvm-readobj` on `C:\Windows\System32\ntdll.dll`

### Why not use RtlValidateUserCallTarget
When installed as `__guard_check_icall_fptr` override in a `-mguard=cf` binary, the
MinGW CRT's `__guard_dispatch_icall_fptr` calls our override during CRT initialization.
At that point, calling `RtlValidateUserCallTarget` triggers `STATUS_STACK_BUFFER_OVERRUN`
(0xC0000409) / fast-fail. The cause is likely a mismatch between the MinGW CRT's CFG
dispatch setup and the strict OS-level bitmap validator during early process startup.

### Discovery: `__guard_dispatch_icall_fptr`
The compiler emits indirect calls through `__guard_dispatch_icall_fptr` (a global
function pointer), NOT direct calls to `__guard_check_icall_fptr`. The MinGW CRT
provides `__guard_dispatch_icall_fptr` which internally calls `__guard_check_icall_fptr`.
Our override of the latter works through the former.

### Final implementation
Module-range check via `GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT)`:
- Fast (in-kernel hash lookup, no I/O)
- Works for all scenarios: GetProcAddress returns, NvAPI internal dispatch functions,
  NVML exports, CRT init
- Only truly wild addresses (not in any loaded module) trigger fast-fail
- Matches the practical behavior of production CFG implementations

### Files touched
- `source/cfg_glue.cpp` — final module-range check version with RtlValidateUserCallTarget
  investigation note

### Build
- Build 131 passes: `python build.py` + `python build.py --test` pass

---

## 2026-05-20: Add LTO (`-flto`) + ICF (`--icf=safe`) for size optimization

### Summary
Added Link-Time Optimization and Identical Code Folding to Windows builds.

### Changes
- `-flto` in WINDOWS_FLAGS: enables cross-module inlining and dead code elimination
- `-Wl,--icf=safe` in WINDOWS_FLAGS: merges identical functions (safe mode keeps
  address-sensitive functions separate)

### Size savings
- GUI: 529,920 → 525,824 bytes (0.8%)
- Service: 178,688 → 174,592 bytes (2.3%)

### Regression check
All hardening/Spectre flags tested:
- `--integritycheck` ❌ LLD/MinGW doesn't support it
- `--mretpoline` ✅ compiles but not beneficial for user-mode GPU utility
- `-mspeculative-load-hardening` ✅ compiles but not beneficial
- `-fstack-clash-protection` ❌ confirmed no-op on Windows/MinGW
- `-mno-outline-atomics` ❌ x86_64 ignores this flag (ARM-only)

### Build
- Build 129 passes: `python build.py` + `python build.py --test` pass

---

## 2026-05-20: Fix CFG runtime crash — override MinGW's buggy `__guard_check_icall_fptr`

### Problem
`-mguard=cf` (Control Flow Guard) was disabled because the MinGW CRT's
`__guard_check_icall_fptr` crashed on indirect calls through
`GetProcAddress`-returned function pointers (NvAPI/NVML). The CRT's
implementation only validates targets against our own binary's GFIDS
table, not externally-loaded DLL exports.

### Fix
Added `source/cfg_glue.cpp` which provides an override for
`__guard_check_icall_fptr` that uses `GetModuleHandleExW` with
`FROM_ADDRESS` to check if the target address falls within any loaded
module. If the target is in any loaded module, it's a valid CFG target
(the module has been authenticated by the OS loader). Only truly wild
addresses (not belonging to any loaded module) trigger a CFG fast-fail.

The override uses `--allow-multiple-definition` to take priority over
the MinGW CRT's `libmingwex.a:mingw_cfguard_support.o`.

### Verification
- `python build.py --test` passes
- `python build.py` produces binaries with CFG enabled:
  - `DllCharacteristics: 0xC160` (GUARD_CF + NX + ASLR + HEVA)
  - `GuardCFFunctionCount: 25` (24 native + 1 from our override)
  - `GuardFlags: 0x00010500` (CF_INSTRUMENTED + FID_TABLE)
- Service runs with full NVML/NvAPI functionality under CFG:
  - Snapshot processing, VF curve readback, telemetry all work
  - No crashes, no exceptions logged

### Files touched
- `source/cfg_glue.cpp` — new file: overrides CRT's `__guard_check_icall_fptr`
- `build.py` — added `cfg_glue.cpp` to `WINDOWS_SOURCE_FILES`, re-enabled
  `-mguard=cf`, added `--allow-multiple-definition`

### Build
- Build 127 passes: `python build.py` + `python build.py --test` pass

---

## 2026-05-20: Add CET/Shadow Stack instrumentation (`-fcf-protection=full`)

### Summary
Added `-fcf-protection=full` to Windows hardening flags. This instructs the compiler to emit `endbr64` instructions at function entries, which are required for Intel CET (Control-flow Enforcement Technology) / Shadow Stack.

### Verification
- Compiles and links cleanly with llvm-mingw (clang 22.1.6, LLD 22.1.6)
- Full service binary (with NVML/NvAPI) runs correctly — no CFG-like crash
- `endbr64` confirmed present at `main()` entry via `llvm-objdump -d`
- Binary still builds with all warnings-as-errors and tests pass

### Limitation
The CET_COMPAT bit in `ImageLoadConfigDirectory.DllCharacteristicsEx` is NOT set. LLD in MinGW/GNU mode (`ld.lld.exe`) accepts `/cetcompat` but doesn't write the bit. Without it, Windows won't enable CET enforcement even on CET-capable hardware.

The `endbr64` instructions are harmless NOPs on all CPUs (both with and without CET support). This future-proofs the binary for when LLD adds proper MinGW CET support.

*(Re-verified 2026-07-25 — **this entry is the accurate one.** A later summary in `build.md` degraded it into the false claim that the flag "is compiled but never materializes in the binary"; measurement confirms `endbr64` is present on 18/18 of our address-taken functions. One correction to the wording above: the CET_COMPAT bit is not in `IMAGE_LOAD_CONFIG_DIRECTORY64` — it lives in an `IMAGE_DEBUG_TYPE_EX_DLLCHARACTERISTICS` (type 20) debug-directory entry. Also note CET_COMPAT gates the shadow stack (backward edge), which does not consult `endbr64` (forward edge / IBT). See `llm-wiki/build.md`.)*

### Files touched
- `build.py` — added `-fcf-protection=full` to WINDOWS_FLAGS; added source regression check

### Build
- Build 125 passes: `python build.py` + `python build.py --test` pass

---

## 2026-05-20: Fix service crash during NVML init — pipe thread stack too small (128 KB)

### Context
After switching from Zig to llvm-mingw, the service process crashed with `ERROR_BROKEN_PIPE` (error 109) whenever the GUI sent a SNAPSHOT request. The crash happened consistently at `nvmlInit_v2()`.

The crash also occurred with the Zig-built binary (build 116), but was missed because the identity path check (GUI expecting service at `build/nvuv/` vs SCM path at `greencurve/`) also blocked communication.

### Root cause
The pipe server thread was created with only **128 KB reserved stack**:
```cpp
g_servicePipeThread = CreateThread(nullptr, 128 * 1024, ..., STACK_SIZE_PARAM_IS_A_RESERVATION, ...);
```

NVML's `nvmlInit_v2()` requires more stack for its internal initialization (loading sub-DLLs, connecting to the kernel driver, allocating internal state). The stack overflow manifests as an access violation inside `nvml.dll`, which the existing `green_curve_unhandled_exception_filter` silently suppresses (returns `EXCEPTION_EXECUTE_HANDLER` without logging when the crash address falls in a GPU driver DLL).

### Fix
Increased pipe thread stack reservation from 128 KB to **1 MB (1024 * 1024)**.

### Debug improvements
- `green_curve_unhandled_exception_filter` now always writes a crash breadcrumb to `greencurve_crash.txt` even for GPU driver DLL crashes, logging the exception code, address, and fault info. Previously these were silently swallowed.

### Files touched
- `source/main_service_server.cpp` — pipe thread stack 128 KB → 1 MB
- `source/main_diagnostics.cpp` — GPU driver DLL crash logging added

### Build
- Build 124 passes: `python build.py` + `python build.py --test` pass
- Service + GUI communicate correctly; identity verified, snapshot and telemetry working

---

## 2026-05-20: Switch Windows toolchain from Zig to llvm-mingw — CFG + hardening

### Context
Zig 0.13.0's `zig c++` uses lld-link in MinGW mode, which cannot produce Control Flow Guard metadata. The `.gfids$y` sections are discarded and no Guard CF function table is emitted — the OS-level CFG mitigation is completely absent.

Zig 0.16.0 was also tested: `/guard:cf` is explicitly blocked ("unrecognized file extension"), and it's missing `__guard_dispatch_icall_dummy`.

### Solution
Switched to **llvm-mingw** (mstorsjo/llvm-mingw 20260519, clang 22.1.6, LLD), a portable MinGW toolchain that fully supports CFG.

### Changes
1. **`build.py`**:
   - Added `llvm-mingw/` constants (URL, SHA256, paths)
   - Added `download_llvm_mingw()` — downloads/extracts/verifies 178 MB zip
   - Windows compile functions now use `llvm-mingw/bin/clang++.exe` instead of `zig c++`
   - Resource compilation uses `llvm-mingw/bin/llvm-rc.exe` instead of `zig rc`
   - Added `-mguard=cf` (Control Flow Guard)
   - Added `-ftrivial-auto-var-init=pattern` (stack variable initialization)
   - Added `-fno-delete-null-pointer-checks` (null pointer check preservation)
   - Added `-static` (static libc++ to avoid `libc++.dll` runtime dependency)
   - Added `-luuid` to link libs (FOLDERID_* symbols are in MinGW's libuuid.a)
   - Regression tests use llvm-mingw on Windows, keep Zig for Linux
   - `ensure_toolchain()` dispatches to correct download per target
   - Removed `verify_cached_zig()` (no longer needed; Zig only for Linux)
   - Source regression checks for new hardening flags

2. **`llm-wiki/build.md`**: Updated for dual-toolchain setup (llvm-mingw + Zig), new hardening flags, static libc++

### CFG verification
```
greencurve.exe:
  DLL characteristics: 0xC160 (Control Flow Guard ✓)
  Guard CF function count: 0x12E (302 functions)
  Guard Flags: 0x10500 (CF instrumented + FID table + Long jump target table)
  Guard CF address taken IAT: 1 entry
  Guard CF long jump target table: 1 entry

greencurve-service.exe:
  DLL characteristics: 0xC160 (Control Flow Guard ✓)
  Guard CF function count: 0x18 (24 functions)
  Guard Flags: 0x10500
```

### Statically linked libc++
Both binaries no longer depend on `libc++.dll`. Only native Windows DLLs + UCRT API sets (always present on Windows 10+).

### Build
- `python build.py --check` passes (both GUI + service)
- `python build.py --test` passes (all regression + source checks with UBSan)
- `python build.py` produces clean release binaries (build 116)

### Files touched
- `build.py` — major toolchain switch
- `llm-wiki/build.md` — updated
- `llm-wiki/index.md` — updated stale-risk dates
- `llm-wiki/log/recent.md` — this entry

### Open questions
- `-fstack-clash-protection` is a no-op on Windows/MinGW (clang issue, not toolchain-specific)
- UBSan in `--test` mode uses static linking via `-static`; MinGW's LLD in GNU mode ignores COFF `--dependent-lib` directives, so the static library path must be explicit

---

## 2026-05-19: Fix stale NVML memory VF offset after an external tool's reset

### Context
When the user resets to driver defaults in an external tool and restarts Green Curve service + GUI, the GUI showed `mem=3000` even though the actual VRAM clock was at stock (confirmed by hardware sensor).

### Root cause
A reset to defaults that clears the NvAPI PState offsets does NOT clear the NVML memory VF offset register (`nvmlDeviceGetMemClkVfOffset`). On a fresh service start:

1. `nvml_read_clock_offsets()` reads the stale NVML register → `g_app.memClockOffsetkHz = 3000000`
2. `detect_clock_offsets()` cross-validates via PState20 vs SMI max clocks (`pstateMemMaxMHz - smiMemMaxMHz`) and normally corrects this
3. But when nvidia-smi fails (`smiMemMaxMHz == 0`), the PState/SMI correction is skipped and the stale NVML value leaks through to the GUI

### Fix
Added stale memory offset detection in `hardware_initialize()`. On a fresh service start (no active desired state), if `memClockOffsetkHz != 0` AND the PState/SMI cross-validation couldn't run (`smiMemMaxMHz == 0` or `pstateMemMaxMHz == 0`), the stale NVML register is reset to 0 via `nvapi_set_mem_offset(0)`. A debug log entry records the stale value and the reset.

This only affects fresh service starts (not TDR recovery, where `g_serviceHasActiveDesired` is preserved).

### Files touched
- `source/main_state_sync.cpp`: stale mem offset reset in `hardware_initialize()`
- `build.py`: added FP-02-003 source regression check

## 2026-05-19: GUI reverted to show live driver values (tail drift workaround removed)

### Context
Since the uniform tail floor fix (Build 109) eliminated tail drift, the GUI workaround that hid live readback values behind intent/locked values is no longer needed. Reverted to show real driver-reported values.

### Changes
1. **`source/ui_main.cpp`**: Removed `gui_curve_point_is_effective_locked_tail()` (unused after removal of locked tail override).
2. **`displayed_curve_mhz_for_gui_point()`**: Removed locked tail early return (`return g_app.lockedFreq`). Now shows live driver readback via `displayed_curve_mhz(g_app.curve[ci].freq_kHz)`.
3. **Info line**: Always shows live max freq/volt from driver readback. When locked, additionally shows lock target info but the peak is still live.
4. **`populate_edits()`**: No longer overwrites tail MHz edit boxes with `g_app.lockedFreq` or disables them.
5. **`apply_lock()`**: No longer overwrites tail edit boxes with lockedFreq or disables them.
6. **`sync_locked_tail_preview_from_anchor()`**: No longer syncs lockedFreq to tail edit boxes.
7. **Drift diagnostic log**: Changed message from `"live readback drift hidden in graph"` to `"gui locked tail live readback drift:"`.
8. **`build.py`**: Updated source regression checks for the changed diagnostic log message.
9. **Graph, edit boxes, info line**: Now all show real driver-reported values.

### Files touched
- `source/ui_main.cpp`: multiple locations (see above)
- `build.py`: source regression checks updated

## 2026-05-19: Uniform tail floor offset (fix v2)

### Context
The initial fix (range limit increase + diagnostic logging) was insufficient. After deploying build 108 with wider limits, the tail drift actually DOUBLED (from +15 MHz to +30 MHz average maximum). The wider range allowed larger positive per-point offsets, making the driver boost tail points even more.

### Root cause confirmed
The flat reference tail's uniform large negative offset is NOT about range limits. The actual issue is that **per-point tail offsets are ineffective on Blackwell** — the driver ignores individual tail-point deltas. The correction loop writes different offsets to different tail points but the live frequency doesn't converge.

Key data:
- Baseline tail (build 108, pre-range-increase): avg drift +13 MHz, max +15 MHz (at 2977 MHz)
- Post-apply (build 108 with wider range): avg drift +23.6 MHz, max +30 MHz (at 2992 MHz)
- Flat reference tail (one uniform negative offset on all tail points): 0/51 points drifted (perfect flat at 2962 MHz)
- The wider range made things WORSE: +7..+15 kHz larger offsets → +7..+15 MHz more frequency

### Fix: uniform tail floor offset
Instead of computing per-point offsets for tail points (which the driver ignores), apply a SINGLE uniform offset equal to `rangeMinKHz` (driver's minimum supported offset) to ALL tail points beyond the lock point. This matches the flat reference tail's uniform-offset pattern.

Changes:
1. **`source/gpu_backend_apply.cpp` lines 398-436**: Initial tail offset computation now uses `floorTailOffsetKHz` (= rangeMinKHz, typically -1000000 kHz) for all tail points except the lock point (which keeps per-point computation).
2. **`source/gpu_backend_apply.cpp` lines 772-777**: Correction pass also uses `correctionFloorTailOffsetKHz` for tail points (non-lock) instead of per-point delta computation.
3. **`build.py`**: Added source regression checks for the new uniform tail floor constants and diagnostics.

### Open question
The uniform floor offset approach with `rangeMinKHz` needs GPU testing. If the driver also ignores the uniform floor offset (i.e., the NvAPI write path for tail points is completely ignored), we may need to investigate other NvAPI write mechanisms for tail points. However, the flat reference tail's offset is only about 5% beyond our -1000000 range minimum, suggesting the range minimum should be close enough.

### Files touched
- `source/gpu_backend_apply.cpp`: uniform floor offset for initial + correction pass
- `build.py`: source regression checks for new constants

### Baseline data saved
(see above for earlier baselines)
- Post-build-108 apply curve (wider range, tail at 2992 MHz — WORSE than baseline)

---

## 2026-05-19: VF curve code audit bugfixes & reliability improvements

### Changes

Seven fixes across the VF curve code path (reading, writing, offset detection, profile save/load):

1. **TOCTOU race fix** (`source/main_gpu_state.cpp:313-365`)
   - `load_runtime_selective_gpu_offset_request()` read `selective_gpu_offset_mhz` under mutex, released mutex, then read `selective_gpu_offset_exclude_low_count` without mutex.
   - Fixed: both reads now happen under the same mutex, eliminating cross-process TOCTOU race.

2. **Cross-process mutex guard for profile save** (`source/config_profiles.cpp:462-464`)
   - `save_profile_to_config()` used process-local `EnterCriticalSection(&g_configLock)` for the existing-config read, which doesn't synchronize between GUI and service.
   - Fixed: replaced with cross-process `enter_config_storage_lock`/`leave_config_storage_lock`.

3. **`originalCurvePopulated` staleness fix** (`source/gpu_backend_apply.cpp:281-287`)
   - After dedicated GPU offset write, the refresh of originals did not update `originalCurvePopulated`, potentially using stale populated masks in the correction loop.
   - Fixed: `originalCurvePopulated` is now refreshed alongside the other original arrays.

4. **O(1) populated ordinal lookup** (`source/app_shared.h:534`, `source/gpu_backend.cpp:597-608`, `source/main_gpu_front.cpp:610-623`)
   - `is_gpu_offset_excluded_low_point()` iterated all 128 points every call, causing O(N²) worst-case in `detect_live_selective_gpu_offset_state()`.
   - Fixed: added `populatedOrdinal[VF_NUM_POINTS]` to `AppData`, initialized in `rebuild_visible_map()`. Lookup is now O(1).

5. **Selective offset detection logging** (`source/main_gpu_state.cpp:227-247`)
   - Added candidate rejection summary when no selective offset is found but candidates exist, showing the first 5 candidate MHz values that were rejected.
   - Helps debug false-negative detection scenarios.

6. **Load-time lock-below-pre-tail validation** (`source/config_profiles.cpp:279-294`)
   - Previously, a profile with lock target below a pre-tail curve point would only fail at apply time with a cryptic monotonicity error.
   - Fixed: now validated at profile load time with a clear error message specifying which point exceeds the lock.

7. **Corrected non-tail readback log scope** — existing log already covers the acceptance path, no additional log needed.

## 2026-05-18: GPU offset+lock conflict UX fixes (failed apply diagnosis & graph display)

### Changes
Three fixes for the failed-apply scenario where a large GPU offset conflicted with a VF curve lock:

**IPC clamp logging** (`source/main_service_server.cpp:504`)
- When `validate_desired_settings_for_ipc()` clamps an out-of-range GPU offset (e.g. 3000→1000), the clamp event is now logged with pre/post values. Previously clamping was invisible.

**Graph display of explicit pre-tail points** (`source/ui_main.cpp:114`)
- `displayed_curve_mhz_for_gui_point()` now checks `g_app.guiCurvePointExplicit[ci]` and reads the user's intended MHz from the edit control when available.
- Previously, after `apply_service_snapshot_to_app()` overwrote `g_app.curve[]` with live data, the graph showed stock frequencies for explicit points (74, 75) instead of the user's intended 2902/2932 MHz.
- Now the graph displays the user's typed values even after a failed apply snapshot refresh.

**Monotonicity error message improvement** (`source/gpu_backend_apply.cpp:238`)
- When a lock+GPU offset combination fails monotonicity (pre-tail point boosted above lock target), the error message now suggests enabling "exclude low VF points" as an alternative to lowering the offset or raising the lock.
- Only shown when the GPU supports `gpuPolicyViaCurveBatch` (Blackwell).

### Verification
- `python build.py` passes, build 106.

---

## 2026-05-18: VF curve validation additions (P0, P1, P2)

### Changes
Three new validation checks added:

**P0 — Offset bounds clamping diagnostic** (`source/main_runtime_nvml.cpp:861`)
- `clamp_freq_delta_khz()` now logs a debug message when it clamps a value to the driver range.
- Previously clamping was silent, making it hard to diagnose why aggressive offsets weren't applied.

**P1 — Voltage consistency diagnostic** (`source/gpu_backend_apply.cpp`)
- Snapshots per-point voltage at the start of the apply flow alongside existing freq/offset snapshots.
- After the correction loop, checks for voltage drift >10 mV and logs a diagnostic.
- Voltage is inherently immutable on NVIDIA VF tables; any change indicates driver state shift or NVAPI read instability.

**P2 — vfMask editability check** (`source/gpu_backend.cpp:362`, `source/main_runtime_gpu.cpp:20`)
- `nvapi_set_point()` and `apply_curve_offsets_verified()` now check `g_app.vfMask` before writing each point.
- Points not in the mask are skipped with a debug log — prevents writing to phantom/non-existent points.

### Verification
- `python build.py` passes, both binaries compile clean.
- Build number bumped to 105.

---

## 2026-05-18: Preserve sparse VF explicit points across fan-only applies and saves

### Problem
After saving a locked curve with point 76 at 2962 MHz and pre-tail points 74/75 edited, the saved config could later contain only the 51-point locked tail. Reboot/logon apply then restored point 75 to its default/live value instead of the intended 2932 MHz.

### Root cause
The sparse save logic depends on `guiCurvePointExplicit[]`. A fan-only apply shortcut sent a partial `DesiredSettings` with only fan data. The service replaced `g_serviceActiveDesired` with that partial request, and the GUI/client repainted or repopulated from the partial fan-only request. That erased the explicit pre-tail point mask while leaving the lock visible, so the next profile save persisted only the lock tail.

### Fix
- Added shared `DesiredSettings` helpers to detect fan-only partial applies.
- Service fan-only applies now merge fan data into the existing active desired state, preserving lock and sparse curve points.
- GUI/client fan-only applies skip VF edit repaint and do not repopulate the editor from fan-only data.
- Added logs for captured curve point lists and save-time point 74/75/76 presence.
- Memory/power-only partial applies also preserve existing sparse curve intent; curve/lock/GPU-offset applies still replace it.

### Verification
- `python build.py --test` passes.
- `python build.py --check` passes.
- `python build.py` passes and bumped local build number to 104.

---

## 2026-05-17: Fix GUI max-voltage display for locked VF tails

### Problem
After flattening the VF curve at 925 mV, the GUI could later show a higher max/lock voltage such as 975-990 mV even though hardware voltage sensors under load stayed at or below the intended 925 mV limit.

### Root cause
NVAPI live readback can temperature-shift or re-shape the flattened tail. The background service could then rediscover a later flat-looking suffix and expose that as `ServiceSnapshot` lock state. The GUI trusted that live-derived lock and the graph plotted raw tail readback, so display drift looked like a user/configured higher-voltage undervolt.

### Fix
- Service snapshots now prefer active desired lock intent over live-detected lock suffixes.
- Live lock auto-detection no longer clears or migrates an existing configured lock when detection is suppressed.
- Client/service post-apply lock sync preserves requested lock MHz instead of replacing it with live readback MHz.
- The GUI graph draws the effective locked tail from configured intent and logs when live drift is hidden in the display.
- Tail drift diagnostics skip hidden/unpopulated VF endpoints and remain diagnostic-only; no automatic VF reapply was added.

### Verification
- `python build.py --test` passes.
- `python build.py --check` passes.
- `python build.py` passes and bumped local build number to 103.

---

## 2026-05-17: Separate VF curve intent from live readback artifacts

### Problem
After configuring only high VF edits (74=2902, 75=2932, 76=2962 MHz) plus a flattened tail, the saved/live state could show an unintended pre-lock plateau around points 69-73. Logs showed point 73 being accepted from live readback during correction and later saved as explicit profile intent.

### Root cause
GUI apply/save captured every visible VF point when any curve/lock edit existed. The backend correction loop also allowed non-tail readback cross-talk to mutate verification targets with misleading "tail point" logging. Together, stock/readback artifacts could leak into `DesiredSettings`, service active desired, and profile files.

### Fix
- GUI apply/save now captures sparse VF intent only: user-explicit points plus the locked tail.
- Profile save writes only `desired->hasCurvePoint` entries; missing curve points are valid.
- Service active desired preserves requested lock MHz instead of replacing it with live readback.
- Backend tail verification remains strict; non-explicit non-tail readback artifacts can be accepted only as verification-only state.
- Runtime telemetry scans/logs the whole locked tail but does not automatically reapply VF settings.
- Selective GPU offset detection now rejects two-point high-edit false positives when a locked tail is skipped.
- `config_profile_repair.cpp` conservatively strips old locked-profile stock scaffolds and the observed point-73 readback artifact signature.

### Build
- `python build.py --test` passes.
- `python build.py --check` passes.

---

## 2026-05-16: Fix VF curve point edits falsely detected as selective GPU offset (round 2)

### Problem
When a user edits individual VF curve points (lock 925mV, points 74=2902, 75=2932, 76=2962 MHz), the GUI shows a global GPU offset of 476 MHz and "exclude first 74 points" — even though the user never touched those controls. The previous zero-gap fix (build 98) didn't help because with exclude=74, all zero-offset points are in the excluded region and only the two non-zero offset points (74, 75) remain in the included region — a perfect match.

### Root cause
The detection heuristic `detect_live_selective_gpu_offset_state()` is fundamentally unable to distinguish between a genuine selective offset and individual VF point edits when the edited points form a contiguous block at the end of the populated region. The per-point offset pattern is identical in both cases.

### Fix
Added `lastApplyUsedGpuOffset` flag tracked across apply paths and propagated through `ServiceSnapshot` to both processes. When the last apply did not use a GPU offset, detection is skipped entirely.

Implementation:
1. **`app_shared.h`**: Added `bool lastApplyUsedGpuOffset` to both `AppData` and `ServiceSnapshot` structs
2. **`main_state_sync.cpp`**: 
   - `hardware_initialize()`: set `lastApplyUsedGpuOffset = true` (startup detection still works)
   - `populate_service_snapshot()`: propagate flag to snapshot
   - `apply_service_snapshot_to_app()`: apply flag from snapshot; also apply snapshot's 0/0 state when `lastApplyUsedGpuOffset` is false
3. **`gpu_backend_apply.cpp`**: 
   - `reset_oc_before_gui_apply()`: set `lastApplyUsedGpuOffset = false`
   - Dedicated GPU offset apply: set `lastApplyUsedGpuOffset = true`
   - Curve batch success/failure: set `lastApplyUsedGpuOffset = gpuPolicyViaCurveBatch`
4. **`main_gpu_state.cpp`**: 
   - `current_applied_gpu_offset_mhz()`: skip detection when `!lastApplyUsedGpuOffset`
   - `current_applied_gpu_offset_excludes_low_points()`: same

### Files changed
- `source/app_shared.h` — added field to both structs
- `source/main_state_sync.cpp` — init, populate, apply
- `source/gpu_backend_apply.cpp` — set flag in all apply paths
- `source/main_gpu_state.cpp` — check flag before detection

### Build
- Build 100 passes: both exes compile cleanly with zero warnings

---

## 2026-05-14: Fix tail drift after apply — monotonicity enforcement must run for both gpu offset paths

### Problem
With GPU offset = 0 + lock, the tail was flat at 2962 right after apply,  
but after ~30 seconds the higher-voltage tail points (ci86+) drifted to 2977  
while the lock point (ci76) stayed at 2962. This was a consistent +15 MHz  
drift across all high-voltage tail points.

### Root cause
The monotonicity enforcement block at `gpu_backend_apply.cpp:829` was gated on  
`gpuPolicyViaCurveBatch` being true (GPU offset > 0). When applying with  
GPU offset = 0 + lock, `gpuPolicyViaCurveBatch` is false, and the entire  
monotonicity enforcement was skipped.

Without the enforcement, the GPU's internal curve handling drifts the
tail upward at higher voltages because the flattened tail violates the
ascending monotonicity requirement at stock frequencies. The GPU adjusts
the high-voltage points upward to maintain hardware monotonicity.

### Fix
1. Removed the `&& gpuPolicyViaCurveBatch` condition from line 829, so  
   monotonicity enforcement runs for ALL lock/flatten applies.
2. Added stock-base offset computation for non-tail points in the  
   `!gpuPolicyViaCurveBatch` path, matching the correction loop fix.

### Files changed
- `source/gpu_backend_apply.cpp` — monotonicity enforcement runs for both paths

### Build
- Build 92 passes with `python build.py --test`

---

## 2026-05-14: Fix lock handler overwriting profile's lockedFreq with stock value

### Problem
After loading a profile with `lock_mhz=2962`, if the user ever ticks the lock
checkbox in the GUI, the handler at `ui_main.cpp:416` unconditionally overwrites
`g_app.lockedFreq = get_edit_value(hEditsMhz[vi])` — reading the edit box's
current value (stock frequency ~2482 MHz), discarding the profile's 2962.

The capture then sees `g_app.lockedFreq = 2482`, the edit box is empty (or shows
stock), falls back to `currentMHz`, and anchor tracking computes:
`2482 + (475 - 0) = 2957` instead of the intended 2962.

### Fix
In `source/ui_main.cpp:416`, gate the overwrite behind `if (g_app.lockedFreq == 0)`.
If `lockedFreq` was already set by a profile load (2962), ticking lock preserves it.
Only when no profile is loaded (lockedFreq=0) does it fall back to the edit box.

### Files changed
- `source/ui_main.cpp` — preserve existing lockedFreq when ticking lock checkbox

### Build
- Build 91 passes with `python build.py --test`

---

## 2026-05-14: Add post-apply tail bookends and telemetry snapshot logging

### Problem
All apply sessions show tail flat at 2962 MHz (51/51 OK), but the user
reports 2977 MHz at 990 mV later. The curve shifts AFTER the apply —
likely from GPU dynamic boost, thermal drift, or driver re-tuning.

The existing post-apply dump only logged tail points that were OFF
(delta > tolerance), making it impossible to detect small drifts
(5-15 MHz) in an otherwise-flat tail.

### Changes
1. **`source/gpu_backend_apply.cpp`** — Post-apply curve dump now:
   - Logs tail points with delta > 2 MHz (even when within tolerance)
   - Always logs tail bookends (first and last tail ci) with actual/target
   - Format: `post-apply tail: ci=XX actual=YY target=ZZ delta=NN`
   - Format: `post-apply tail bookends: first=ciN actual=N target=N last=ciM actual=M target=M`

2. **`source/main_state_sync.cpp`** — On each telemetry snapshot:
   - Logs tail MHz at lockedCi, +10, +20, +30, +50
   - Detects post-apply curve shifts between user actions
   - Format: `curve tail bookends: ci76=2962 ci86=2962 ci96=2962 ci106=2962 ci126=2962`

### Source regression checks added
- `post-apply tail bookends` (gpu_backend_apply.cpp)
- `post-apply tail: ci=%d actual=%u` (gpu_backend_apply.cpp)
- `curve tail bookends` (main_state_sync.cpp)

### Build
- Build 90 passes with `python build.py --test`

---

## 2026-05-14: Accept non-tail point cross-talk deviations when tail is flat

### Problem
With a saved profile (GPU offset 475, lock 2962 @ ci=76), applying with GPU offset
set to 0 in the GUI produced a spurious error:

    VF point 75 verified at 2572 MHz instead of requested 2452 MHz

The tail was perfectly flat (51/51 points at 2962 MHz), but the apply failed because
a non-tail boost-region point (ci=75) was shifted by ~120 MHz from stock (2452→2572).

### Root cause
On Blackwell, writing large flatten offsets (up to ~480 MHz) to tail points 76-126
shifts adjacent boost-region point 75's live frequency via hardware cross-talk.
The correction loop tried to enforce point 75's stock target (2452), computing
desiredOffset=0 (target==stockBase → diff=0). Writing offset 0 did nothing to
the live 2572 MHz, producing a mismatch after all 25 correction passes.

### Fix
In `source/gpu_backend_apply.cpp` divergence check: when
`!gpuPolicyViaCurveBatch && hasLock && !lockedTailMask[ci] && lockMhz > 0`,
accept the actual MHz for cross-talk-affected non-tail points. This is the same
mechanism already used for diverged tail points.

### Files changed
- `source/gpu_backend_apply.cpp` — accept actual MHz for cross-talk non-tail points

### Build
- Build 89 passes with `python build.py --test`

---

## 2026-05-13: Fix correction loop cumulative offset bug for non-tail points

### Problem
When applying with GPU offset = 0 + lock (via saved profile), the correction loop
accumulated wrong offsets for non-tail explicit curve points. Each correction pass
wrote a progressively larger negative offset (0 → -120000 → -240000 → ... → -960000)
without converging, because `curve_delta_khz_for_target_display_mhz` used the live
base (`g_app.curve.75.freq_kHz - g_app.freqOffsets[75]` = 2572000 - 0 = 2572000)
instead of the stock base (2452000). The live freq reflected the batch-written
offset but `freqOffsets` was not updated, producing a base that was already at the
target — making each correction pass compound instead of converge.

### Root cause
In `source/gpu_backend_apply.cpp`, the correction loop at line 712 computed
offsets via `curve_delta_khz_for_target_display_mhz(ci, targetMHz)`, which uses
the LIVE frequency minus freqOffsets as base. For non-tail explicit curve points
with `!gpuPolicyViaCurveBatch`:
1. Initial batch writes +120 offset via curve batch → live freq changes to target
   (2572) but `g_app.freqOffsets` stays 0.
2. Correction loop sees base = 2572 - 0 = 2572, target = 2572, delta = 0 — correct.
   **BUT**: somehow delta = -120000 was computed (possibly due to GPU-side drift or
   NVAPI readback inconsistencies).
3. Correction loop writes offset -120 → `freqOffsets` becomes -120000.
4. Next pass: base = 2572 - (-120000) = 2692, delta = 2572 - 2692 = -120,
   desiredOffset = -240000. Repeats until offset hits range limit at pass 8.

### Fix
1. In the correction loop, for non-tail explicit curve points when
   `!gpuPolicyViaCurveBatch`, compute the offset relative to the ORIGINAL stock
   base (`originalCurveFreqkHz[ci] - originalCurveOffsets[ci]`) instead of the
   live base, avoiding cumulative frequency offset artifacts.
2. Added post-apply curve state dump logging to detect weird curve shifts that
   don't match the intended VF curve shape.

### Files changed
- `source/gpu_backend_apply.cpp` — stock base offset computation + state dump
- `build.py` — source regression checks

### Build
- Build 87 passes: both binaries compile cleanly
- `python build.py --test` passes

---

## 2026-05-13: Fix anchor tracking delivering wrong lock target with stale lockedFreq

### Problem
With a fresh config (no saved profile), setting up GPU offset + lock in the GUI
produced the wrong lock target. The user locked at point 76, but `g_app.lockedFreq`
was set to 2482 MHz (the stock frequency) instead of the intended 2962 MHz.
Anchor tracking then computed: 2482 + (475 - 0) = 2957 instead of 2962.

### Root cause
`source/main_runtime_control.cpp:5` — `effectiveLockTargetMHz` was initialized
from `g_app.lockedFreq`, and the edit box was only read if `lockedFreq <= 0`.
Since `lockedFreq` = 2482 > 0, the edit box was never consulted, even though
the user had typed 2962 in the lock point's MHz field.

### Fix
Always read the edit box when capturing lock MHz, regardless of
`effectiveLockTargetMHz > 0`. The edit box always has the user's latest typed
value. Only fall back to `g_app.lockedFreq` when the edit box is empty.

### Files changed
- `source/main_runtime_control.cpp` — unconditional edit box read for lock capture
- `build.py` — new source regression check and file variable

### Build
- Build 85 passes: both `greencurve.exe` and `greencurve-service.exe` compile cleanly
- `python build.py --test` passes

---

## 2026-05-13: Fix flatten tail not 100% flat when selective GPU offset + lock combined

### Problem
With selective GPU offset (+475 MHz excl=60) + lock (2962 MHz @ ci=76), the VF curve
tail was not perfectly flat. Only 3 of 51 tail points exactly matched the lock target.
Tail points settled at 2970-2977 MHz instead of 2962 MHz. An external tool confirmed the
curve was not properly flattened.

### Root cause
Two interacting bugs in `source/gpu_backend_apply.cpp`:

**Bug 1 — Monotonicity enforcement added selective offset to tail point bases (lines 368-378)**
When computing the monotonicity check for a tail point, the code used
`prevBase + prevSelectiveKHz` for the previous point even when the previous point
was ALSO a tail point. This caused the selective offset (+475 MHz) to leak into
tail targets, raising them above lockMhz. E.g., point 78's target was clamped to
2980 MHz instead of 2962 MHz because point 77's base+selective (2505+475) exceeded lockMhz.

**Bug 2 — Correction loop gave up on converging tail points (lines 738-752)**
The divergence check used `curve_point_verify_tolerance_mhz()` (returns 4 MHz for
tightly-spaced tail points) to decide if a tail point had "diverged". After one
correction pass that closed a 500+ MHz gap to 8-15 MHz, the point was declared
diverged and the actual MHz was accepted, preventing further correction.

### Fix
1. In the initial flatten's monotonicity enforcement (line 371-378): for a tail
   point's previous point that is ALSO a tail point (`lockedTailMask[prevCi]`),
   use `lockMhz * 1000` as the previous target instead of `base + selectiveOffset`.
2. In the correction loop's divergence check (line 751-776): track error across
   correction passes via `prevErrorKHz[ci]`. Only declare diverged when error
   INCREASED (`errorKHz > prevErrorKHz[ci]`) or STUCK (`errorKHz == prevErrorKHz[ci]`).
   Allow converging points (`errorKHz < prevErrorKHz[ci]`) to continue to
   subsequent passes.
3. Tighten flatten verification tolerance from 15 MHz to 8 MHz (line 591).
4. Add convergence summary logging (converging/worsening/stuck/outOfRange counts).

### Files changed
- `source/gpu_backend_apply.cpp` — monotonicity fix, correction loop fix, tolerance tighten, logging

### Build
- Build 83 passes: both `greencurve.exe` and `greencurve-service.exe` compile cleanly
- `python build.py --test` passes (all regression + source checks)

---

## 2026-05-13: OC persistence check on resume and driver recovery

### Problem
After Windows standby/resume or a driver update without reboot, the GPU driver resets
all OC/VF/fan settings to defaults. The service had no mechanism to detect this or
re-apply the last known good state, leaving the GPU running stock clocks until the
user manually re-applied via the GUI.

### Solution
Two event-driven detection paths, zero polling:

**Trigger 1 — Power events (standby/resume):**
- Switched to `RegisterServiceCtrlHandlerExW` to receive `SERVICE_CONTROL_POWEREVENT`
- Added `SERVICE_ACCEPT_POWEREVENT` to the service's accepted controls
- On `PBT_APMRESUMEAUTOMATIC`: spawns a dedicated thread that verifies OC settings
  via `service_apply_desired_settings()` from the stored `g_serviceActiveDesired`

**Trigger 2 — NVML recovery (driver update):**
- In `service_runtime_pulse()`, after the fan tick, checks if NVML has gone stale
  (temperature read fails)
- If stale: closes old NVML handle, reinitializes, calls `service_check_oc_persistence()`

**TDR loop protection:**
- `service_check_oc_persistence()` records each driver recovery in a ring buffer
- If 3+ recoveries occur within 5 minutes, re-apply is blocked (TDR loop detected)
- Uses `g_nvml_api.getGpcClkVfOffset` (1 cheap NVML call) to read live GPU offset
- Only re-applies when live offset differs from desired (>5 MHz tolerance)

### Files changed
- `source/main_service_server.cpp` — Ex handler, powerevent, resume thread
- `source/main_service_runtime.cpp` — TDR loop detection, OC check, NVML recovery
- `build.py` — 9 source regression checks
- `llm-wiki/log/recent.md` — this entry

### Build
- Build 80 passes: both `greencurve.exe` and `greencurve-service.exe` compile cleanly
- `python build.py --test` passes
- `python build.py --check --target windows` passes

---

## 2026-05-13: Bugfix — fan curve hysteresis defeated by timed re-apply after 15 seconds

### Problem
The fan curve hysteresis only works for small temperature drops within the first 15 seconds.
After `FAN_RUNTIME_REAPPLY_INTERVAL_MS` (15 s) elapses, the timed re-apply path called
`nvml_manual_fan_matches_target()` with the curve's target percent. Since hysteresis had
prevented the apply, the actual fan speed did not match the target, so `matches=false` and
`shouldApply=true` was forced — defeating the user-configured hysteresis entirely.

Example with hysteresis=3°C:
1. Temp 71°C → fan 73%
2. Temp drops to 70°C: target = 72%. Hysteresis check: 70 ≤ 71-3? No → skip (correct)
3. 15 seconds later: timed re-apply sees target=72%, actual=73%. Mismatch → forces re-apply
   to 72% despite only 1°C drop (wrong — bypassed hysteresis)

### Fix
When `targetPercent < lastAppliedPercent` and hysteresis blocked the drop, return immediately
from the tick function instead of falling through to the timed re-apply path. The fan will
adjust naturally when the temperature drops past the hysteresis threshold.

### Files changed
- `source/main_fan_runtime.cpp` — early return when hysteresis blocks a fan decrease

---

## 2026-05-13: Code audit fix wave — UBSan default, ScopedProcess, ASan, lock state IPC, static_asserts

Implemented 13 fix recommendations from a code audit (F-01-003 excluded):

### F-01-001: UBSan now default in --test mode
- `--test` always compiles with `-fsanitize=undefined` by default
- `--sanitizer` flag kept for backward compatibility (now a no-op)

### F-01-002: Targeted clang-tidy checks
- Replaced blanket `modernize-*` and `cppcoreguidelines-*` removal with 7 specific noisy check removals
- Preserves valuable correctness diagnostics

### F-04-001: ASan compile mode
- Added `--asan` flag that adds `-fsanitize=address` to test compile command
- Documented in `build.py` --help

### F-05-001: Rollback retry loops
- Added `retry_op` lambda in `rollback_to_safe_defaults()` with 3-attempt retry for VF curve, GPU, and power-limit resets
- Each attempt separated by 10ms delay

### F-06-001: ScopedProcess RAII wrapper
- Added `ScopedProcess` struct to `win32_raii.h` with process/thread handle management, pipe assignment, wait, exit_code, and terminate
- Refactored both nvidia-smi callers (`read_nvidia_smi_max_clocks`, `nvapi_set_power_limit`) in `gpu_backend.cpp` to use ScopedProcess
- Eliminates orphaned process risk on timeout/failure

### F-07-001: Config truncation detection
- `get_config_int()` now checks `GetPrivateProfileStringA` return value against buffer size
- Truncated values (n >= sizeof(buf)-1) return `defaultVal` instead of parsing partial data

### F-08-001: IPC regression tests
- Added ServiceRequest/ServiceResponse size sanity checks
- Added validate_desired_settings_for_ipc extreme edge case tests (zero, negative, overflow values)
- Added SERVICE_PROTOCOL_MAGIC and VF_NUM_POINTS invariant checks

### F-10-001: Config mutex timeout diagnostic
- Added `OutputDebugStringA` warning when config interprocess mutex times out

### F-11-001: Service event creation safety
- Added null check and fatal diagnostic for `g_serviceStopEvent` creation failure
- Added warning log for `g_servicePipeWakeEvent` creation failure

### F-12-001: Backend layout static_asserts
- Added 3 `static_assert` checks verifying VF status, info, and control buffer offsets fit within buffer sizes
- All present backend specs share the same layout, so one set of asserts covers all

### F-13-001: HeapBuffer allocation assertion
- Added `(void)(ptr != nullptr || size == 0)` assertion expression in HeapBuffer constructor

### F-14-001: Exceptions/RTTI tradeoff documentation
- Updated `build.md` with note about Linux exceptions/RTTI being a pre-production tradeoff

### F-15-001: Lock state through IPC
- Added `hasLock`, `lockCi`, `lockMHz`, `lockTracksAnchor` fields to `ServiceSnapshot`
- `populate_service_snapshot()` now populates lock state from `g_app`
- `apply_service_snapshot_to_app()` adopts service lock state directly instead of re-running `detect_locked_tail_from_curve()` when snapshot carries valid lock
- Prevents GUI/service lock state disagreement

### Files changed
- `source/win32_raii.h` — ScopedProcess struct
- `source/app_shared.h` — HeapBuffer assertion, ServiceSnapshot lock fields
- `source/config_utils.cpp` — truncation detection, mutex timeout warning
- `source/gpu_backend.cpp` — ScopedProcess for nvidia-smi callers
- `source/main_gpu_front.cpp` — retry loops in rollback_to_safe_defaults
- `source/main_state_sync.cpp` — lock state propagation in snapshot
- `source/main_service_server.cpp` — event creation safety checks
- `source/main.cpp` — backend layout static_asserts
- `source/.clangd` — targeted clang-tidy checks
- `build.py` — UBSan default, ASan flag, IPC regression tests, new source regression checks
- `llm-wiki/testing.md` — updated test inventory
- `llm-wiki/build.md` — updated build modes

### Build
- Build 79 passes: both exes compile cleanly, `python build.py --test` passes
- `python build.py --check --target windows` passes with static_asserts verified
- `python build.py --test --sanitizer` passes

---

## 2026-05-12: Bugfix — tray icon shows "Default" after GPU offset apply in GUI service mode

### Problem
After applying a GPU offset (selective, via curve batch), the tray icon stayed green
("Default") instead of switching to the OC amber color. VRAM OC worked correctly.

### Root cause
`live_state_has_custom_oc()` in `main_fan_runtime.cpp` had a guard:
`if (!g_app.vfBackend || !g_app.vfBackend->writeSupported) return false;`
`g_app.vfBackend` is only set by `hardware_initialize()` which runs in the SERVICE
process. The GUI process never initializes it, so `vfBackend` stays nullptr. The
function returned false before checking `freqOffsets` — even though the service
snapshot contained correct non-zero per-point offsets.

Additionally, `update_tray_icon()` was never called from the GUI-side apply path.
It was only called from `refresh_global_state()` (which for mem/power/fan-only
changes) and fan runtime start/stop events. Added `update_tray_icon()` to the
GUI-side apply wrapper in `gpu_backend.cpp`.

### Files changed
- `source/main_fan_runtime.cpp` — removed vfBackend guard from live_state_has_custom_oc()
- `source/gpu_backend.cpp` — added update_tray_icon() after snapshot apply in GUI path

### Build
- Build 64 passes: both exes compile cleanly, `python build.py --test` passes

---

## 2026-05-12: Bugfix — profile save captures flattened VF curve, lock re-inferred on load+apply

### Problem
Saving "defaults" to a profile slot after a previous lock+flatten was applied
saved the flattened curve frequencies as explicit VF curve points. Loading and
applying the "default" profile would then re-infer a lock from the flat tail,
re-applying the flatten instead of restoring stock frequencies.

### Root cause
`build_full_live_desired_settings()` saved ALL 128 VF curve points from
`g_app.curve` — including tail points that were flattened by a previous
profile's lock (e.g. 2487 MHz instead of stock ~3300+ MHz). When this
profile was loaded, `infer_profile_lock_from_curve()` detected the flat
tail and created an inferred lock (`lockCi=76 lockMHz=2487`). The apply
path then ran the flatten-undervolt code, re-flattening the tail.

### Fix
`build_full_live_desired_settings()` no longer saves VF curve points or
the applied lock. Without explicit curve points, `hasCurvePoint[i]` stays
false for all points, `infer_profile_lock_from_curve()` has no flat tail
to detect, and `reset-before-apply` restores factory base frequencies.

### Files changed
- `source/main_gpu_state.cpp` — removed curve point loop and lock capture
  from `build_full_live_desired_settings`

---

## 2026-05-12: Bugfix — profile save captures stale GUI values from previous profile load

### Problem
Saving a profile to a slot without making any GUI edits (e.g. after loading a different
profile) saved the loaded profile's stale values instead of the current live hardware
state. Loading and applying the saved profile would then apply unexpected settings.

### Root cause
`capture_gui_config_settings()` used `includeCurrentGlobals=true` and
`captureAllCurvePoints=true`, unconditionally capturing ALL values from GUI edit controls.
After loading a profile, the GUI edit controls contained the loaded profile's values
(e.g. +475 MHz GPU offset). The Save handler captured these stale values and wrote them
to the new profile slot — even though the user never typed anything.

### Fix
Added `g_app.guiHasUserModifiedValues` flag:
- Reset to `false` in `populate_desired_into_gui()` (any programmatic population of the GUI)
- Set to `true` in every EN_CHANGE / user edit handler (curve points, GPU/mem offset,
  power limit, exclude-low, fan mode combo, fan curve dialog commit)
- In the Save button handler: use `build_full_live_desired_settings()` (live hardware
  state) when `guiHasUserModifiedValues` is false; use `capture_gui_config_settings()`
  (GUI state) only when the user has actively edited values.

### Files changed
- `source/app_shared.h` — added `guiHasUserModifiedValues` field
- `source/config_profiles_ui.cpp` — reset flag in populate_desired_into_gui()
- `source/ui_main_window.cpp` — set flag in EN_CHANGE/combo handlers; use flag in Save
- `source/fan_curve_dialog.cpp` — set flag on fan curve dialog commit

---

## 2026-05-12: Bugfix — profile switch TDR caused by dangerous reset order; auto-apply paths skip reset

### Problem
Switching between saved profiles in the GUI (Load → Apply) could cause a GPU driver
TDR/crash. Auto-apply paths (app-launch, logon, CLI `--apply-config`) also applied
profiles without any reset to defaults.

### Root cause (TDR)
`reset_oc_before_gui_apply()` in `gpu_backend_apply.cpp` reset VF curve offsets to zero
**before** resetting the GPU offset. When VF curve offsets are zeroed, the high-voltage
tail points snap back to factory base frequencies (~3300+ MHz on Blackwell). But the GPU
offset from the previous profile (e.g. +475 MHz) was still active during this window:

```
OLD (dangerous):
  1: Zero VF curve offsets   ← tail at 3300 + 475 = 3775 MHz → TDR
  2: Zero GPU offset

NEW (safe):
  1: Zero GPU offset          ← remove global boost first
  2: Zero mem offset
  3: Power limit to 100%
  4: Zero VF curve offsets    ← tail at 3300 MHz (safe, no boost)
```

### Root cause (auto-apply gap)
`resetOcBeforeApply` was only set to `true` in `capture_gui_apply_settings()`
(`main_runtime_gpu.cpp:363`), which serves only the GUI Apply button. The three other
apply paths called `apply_desired_settings()` directly with default-initialized
`DesiredSettings` where `resetOcBeforeApply = false`:
- App-launch auto-apply (`config_profiles_ui.cpp:630`)
- Logon auto-apply (`config_profiles_ui.cpp:739`)
- CLI `--apply-config` (`entry.cpp:215`)

### Fix
1. Reordered reset steps in `reset_oc_before_gui_apply()`: GPU offset first, then mem
   offset, power limit, VF curve offsets last.
2. Set `desired.resetOcBeforeApply = true` in all three auto-apply paths.
3. Added debug logging for reset-before-apply in auto-apply paths.

### Files changed
- `source/gpu_backend_apply.cpp` — reordered reset steps
- `source/config_profiles_ui.cpp` — reset-before-apply for app-launch and logon paths
- `source/entry.cpp` — reset-before-apply for CLI --apply-config path

---

## 2026-05-12: Bugfix — correction loop wrote selective offset instead of flatten delta for tail points

### Problem (build 57)
The previous fix (build 57) broadened the divergence check to accept actual MHz for tail
points, but the correction loop still wrote `gpu_offset_component_mhz_for_point()` (the
selective offset, +475 MHz) for tail points instead of flattening them to the lock target.
This meant tail points stayed at boosted frequencies (e.g. 2992-3637 MHz) instead of being
flattened to 2957 MHz. The divergence check then accepted all these non-flattened frequencies.

### Root cause
The condition `if (isTail && gpuPolicyViaCurveBatch)` at line 694 (and 807 in monotonicity
enforcement) made the correction loop preserve the selective offset for ALL tail points.
But the lock flatten requires tail points to have LOWER offsets than the selective boost.
The correction loop should compute `curve_delta_khz_for_target_display_mhz(ci, targetMHz)`
for tail points to reach the lock target, just like it does for non-selective-offset points.

### Fix
Changed the condition from `isTail && gpuPolicyViaCurveBatch` to
`gpuPolicyViaCurveBatch && !isTail` in both locations:
1. Correction loop offset computation (line 694)
2. Post-correction monotonicity enforcement offset computation (line 807)

This preserves the selective offset for boost-region (non-tail) points while computing
proper flatten deltas for tail points. The divergence check from build 57 stays in place
to accept actual MHz for tail points that can't converge due to hardware rounding.

### Files changed
- `source/gpu_backend_apply.cpp` — condition fix in correction loop and monotonicity
  enforcement

### Build
- Build 59 passes: both `greencurve.exe` and `greencurve-service.exe` compile cleanly
- `python build.py --test` passes

---

## 2026-05-11: Structural fix — correction loop + post-correction flow consolidation

### Problem
The uncommitted edits from the monotonicity fix introduced a structural bug:
1. Success/failure counting (`successCount++` / `failCount++`) was placed INSIDE the correction pass `for` loop, executing on every iteration instead of once after convergence
2. The `break` on `verify_curve_request()` convergence was removed, causing all 25 iterations to run unconditionally
3. A duplicate `if (curveRequest) { if (curveRequestOk) ... else ... }` block existed OUTSIDE the main `else` block, creating redundant success/failure counting
4. The `else` block's closing brace was missing after consolidation, causing "function definition is not allowed here" compiler errors in `gpu_backend.cpp` (which includes `gpu_backend_apply.cpp`)

### Fix
Consolidated the entire correction+post-correction flow into a single well-structured block:
- Correction loop (inside `if (!curveRequestOk)`) properly has `break` on `verify_curve_request()` convergence
- Post-loop: `curveRequestOk && !curveBatchOk` log, monotonicity enforcement, then success/failure counting — all inside the single `if (curveRequest)` block
- Removed the duplicate `if (curveRequest)` block outside the `else`
- Added the missing `}` closing the `else` block before `if (memApplied...)`
- The post-correction tail cleanup (spurious offset reset) is replaced by the upstream monotonicity enforcement fix — a root-cause fix rather than symptom cleanup

### Files changed
- `source/gpu_backend_apply.cpp` — structural fix to correction loop and post-correction flow

### Build
- Build 55 passes: both `greencurve.exe` and `greencurve-service.exe` compile cleanly
- `python build.py --test` passes

---

## 2026-05-11: Bugfix -- selective boost + lock creates non-monotonic curve when lock target is below boosted point

### Root cause
When a selective offset (e.g. +475 MHz to points 60+) is active alongside a lock (tail
flatten), the lock's target frequency may be lower than the boosted frequency of the
previous point. Example:
- Point 75 stock: 2452 MHz, with +475 → boosted to 2927 MHz
- Lock target (from profile saved by old corrupted code): 2917 MHz
- The GPU enforces monotonicity: point 76 can't be lower than point 75 (2927 > 2917)

The GPU clamps point 76 to point 75's boosted frequency, making both display as 2917 MHz.
The user's intent (lock at a specific point, separate from boost) was physically
impossible on a monotonically non-decreasing VF curve.

### Fix
In the lock target computation (targetCurveOffsets for tail points), when
`gpuPolicyViaCurveBatch` is active, enforce monotonicity against the previous
point's boosted target. Each tail point's target becomes `max(lockMhz, prevTarget)`
where `prevTarget` includes the selective offset boost. This propagates through the
tail automatically, creating a flat curve at the boosted level.

This handles both old corrupted profiles (where the saved lock target is too low)
and new profiles where the user's lock target might conflict with the selective boost.

### Files changed
- `source/gpu_backend_apply.cpp` — lock target monotonicity clamp for tail points


## 2026-05-11: Bugfix -- spurious tail offsets cause non-monotonic VF curve display after correction loop

### Root cause
After the correction loop converges, tail points that were diverged (accepted at their actual
MHz because lockMhz was unreachable) retained large negative offsets (e.g. -870000 kHz) that
the GPU partially rejected due to monotonicity constraints. The NVAPI curve readback returned
`freq_kHz` and `freqOffsets[ci]` that were physically inconsistent:
- `freq_kHz` ≈ 2917000 (actual GPU frequency, clamped by monotonicity)
- `freqOffsets[ci]` ≈ -870000 (spurious reported offset from failed flatten)
- `base = freq - offset ≈ 3787 MHz` (physically impossible base frequency)

This made the VF curve readback show non-monotonic values (point 75=2932, point 76=2917)
and caused the GUI to display frequencies that didn't match the actual GPU state.

### Fix
After the correction loop completes, add a cleanup pass for tail points:
- For each tail point with `abs(freqOffsets) > 12000` (non-trivial offset)
- Compute `base = freq_kHz - freqOffsets[ci]`
- If base is < 150 MHz or > 4000 MHz (physically impossible), reset `freqOffsets[ci]` to 0
- This removes spurious offsets that the GPU rejected, making the displayed frequency
  consistent with the actual GPU state

### Files changed
- `source/gpu_backend_apply.cpp` — post-correction tail offset cleanup


## 2026-05-11: Bugfix -- detection picks residual noise offsets as selective pattern after profile switch

### Root cause
After applying Profile 2 (no selective offset), the correction loop (triggered by tail flatten
failures) writes all point offsets to 0, but the batch tail write still creates ~1-7 MHz
residual offsets on non-tail points via frequency drift. The detection function
`detect_live_selective_gpu_offset_state()` found `exclude=61 MHz=1 matches=14 violations=0`
as "best" because the permissive 30 MHz tolerance matched the small residuals.

The fundamental issue: the detection function only rejected exactly-0 detected MHz. Residual
noise in the 1-30 MHz range was treated as a valid selective pattern, and with the multi-
exclude scan, picking the smallest exclude count (61) that has the most "matches" (14).

### Fix
In `detect_live_selective_gpu_offset_state()`, after computing `detectedMHz`, reject any
result where `abs(detectedMHz) <= TOLERANCE_MHZ (30)`. Below 30 MHz is within the noise
floor — it's not a meaningful user-set selective offset (which is always significantly
larger, typically 100+ MHz for overclocking).

### Files changed
- `source/main_gpu_state.cpp` — detection function minimum threshold check


## 2026-05-11: Bugfix -- stale g_serviceActiveDesired causes selective offset to persist after profile switch

### Root cause
In `main_service_runtime.cpp`, after a successful apply, `populate_control_state()` was called
(line 509) BEFORE `g_serviceActiveDesired = *desired` (line 512). When the new profile has
`hasGpuOffset=true/gpuOffsetMHz=0` (i.e., no selective offset), the call chain was:

1. `populate_control_state()` → `current_applied_gpu_offset_mhz()` (in service process)
2. Steps 1-4 (session, runtime, detection) all fail to find a selective offset
3. Step 5: `service_active_desired_gpu_offset_fallback()` checks `g_serviceActiveDesired`
   which still contains the PREVIOUS profile's values (e.g. 475/60)
4. The fallback returns true because tail points from the lock flatten still have
   non-zero offsets, satisfying `live_curve_has_any_nonzero_offsets()`
5. The stale 475/60 value propagates to `g_serviceControlState` and from there to
   the next telemetry snapshot, making the GUI display the previous profile's offset

### Fix
Move `g_serviceActiveDesired = *desired` and `g_serviceHasActiveDesired = true` to
BEFORE `populate_control_state()`. This ensures that when `current_applied_gpu_offset_mhz()`
runs during state capture, the service active desired fallback sees the new profile's
values (gpuOffsetMHz=0) and correctly returns false.

### Files changed
- `source/main_service_runtime.cpp` — reorder: update g_serviceActiveDesired before
  populate_control_state


## 2026-05-11: Bugfix -- correction loop corrupts selective pattern AND tail divergence detection

### Issues
Two related problems were fixed together:

### Issue 1: Correction loop destroys selective offset pattern

**Root cause**: The VF curve correction loop (25 passes, triggered when tail flatten points
fail initial verification) recomputed offsets for ALL non-tail points — both excluded and
included — using `curve_delta_khz_for_target_display_mhz()`. This function computes the delta
from the current live frequency to the target. When large tail-flatten offset writes dragged
adjacent non-tail point frequencies (observed on Blackwell), each correction pass produced
different offsets for non-tail points:

- **Excluded points** (should have offset 0): accumulated small non-zero offsets (1-10 MHz)
  as the correction loop tried to keep them at their original base frequency against
  tail-write-induced drift.
- **Included points** (should have offset +475 MHz): lost their selective offset, ending
  up with ~10 MHz offset after the correction pass dragged their frequency down and
  then re-targeted them to the dragged frequency.

After 16 correction passes, ALL 75 populated points had ~1-10 MHz offsets with no
distinguishable pattern. The detection function `detect_live_selective_gpu_offset_state()`
then found `exclude=1 MHz=1 matches=75 violations=0` as the best match — since all points
had nearly identical small offsets, the smallest possible exclude count won the tiebreaker.

**Fix**: In the correction loop, when `gpuPolicyViaCurveBatch` is active, directly set
non-tail point offsets from the requested selective values (`gpu_offset_component_mhz_for_point()`)
instead of computing them from drifted live frequencies. Excluded points get offset=0,
included points get `gpuOffsetMHz * 1000`.

### Issue 2: Tail points can't reach lockMhz due to hardware range limits

**Root cause**: On Blackwell, tail points at ~2932 MHz need -1450000 kHz offset to reach
lockMhz (2482 MHz), but the hardware VF offset range is only -1000000..1000000 kHz. The
correction loop kept computing clamped range-limit offsets (-1000000) that never moved the
frequency, creating an infinite retry loop. The divergence guard (which accepts the actual
MHz for points that exceed the range) was skipping tail points entirely.

**Fix**: Three changes:
1. **Divergence guard**: Removed the `if (lockedTailMask[ci]) continue;` skip so tail
   points are also checked. When a tail point's required unclamped delta exceeds the
   hardware range, `verifyDesired.curvePointMHz[ci]` is updated to the actual MHz.
2. **Correction loop**: For tail points, when `verifyDesired.curvePointMHz[ci]` has been
   updated by the divergence guard (differs from lockMhz), use the diverged value as the
   target instead of lockMhz. This stops writing range-limited clamped offsets.
3. **verify_curve_request**: In the selective-path tail check, when a tail point's
   `verifyDesired.curvePointMHz[ci]` has been updated by the divergence guard, use the
   diverged value as the target for verification instead of lockMhz.

### Files changed
- `source/gpu_backend_apply.cpp` — selective pattern preservation, tail divergence guard,
  tail correction target, and tail verify target

### Evidence from build 44 debug log (Issue 1)
```
curve correction pass 1: target point 75 live=2917 MHz offset=475000 desiredOffset=10000
...
curve correction pass 16: target point 75 live=2452 MHz offset=10000 desiredOffset=10000
detect_live_selective: BEST exclude=1 MHz=1 matches=75 violations=0
current_applied_gpu_offset_mhz: preserving session selective value=1 MHz
```

### Expected behavior after fix (Build 46)
1. Selective offset pattern is preserved: excluded points keep offset 0, included points
   keep offset 475000 kHz throughout all correction passes.
2. Tail points that can't reach lockMhz due to hardware range limits are accepted at their
   actual MHz after one divergence-guard detection, preventing the infinite correction loop.
3. Detection function correctly identifies exclude=60 MHz=475.


## 2026-05-11: Bugfix -- selective offset detection used hardcoded exclude count of 70, breaking exclude != 70

### Root cause
`detect_live_selective_gpu_offset_state()` only tried exclude count 70 when scanning for
selective (exclude-low) GPU offset patterns. Any apply with a different exclude count
(e.g. 60) produced per-point offsets that did not match the 70-based expected mask.
The detection would either find a spurious low offset (e.g. 7 MHz instead of 475 MHz)
or fail entirely.

Consequences:
- `current_applied_gpu_offset_mhz()` stored `appliedGpuOffsetExcludeLowCount = 70`
  (hardcoded at line 757) regardless of the actual exclude count.
- `current_applied_gpu_offset_excludes_low_points()` similarly stored hardcoded 70.
- The service snapshot then reported exclude=70, causing the GUI to overwrite the
  user's correct exclude=60 with 70 after the next state clean cycle.
- `live_selective_gpu_offset_matches_requested_state()` also used hardcoded 70.

### Fix
`detect_live_selective_gpu_offset_state()` now:
- Accepts an optional `int* detectedExcludeLowCountOut` parameter.
- Loops over exclude counts 1..numPopulated and evaluates each candidate offset
  against every exclude count.
- Picks the best match (most included matches, fewest excluded violations) across
  all exclude counts and returns both the detected offset MHz and the matching
  exclude count.

Callers updated:
- `current_applied_gpu_offset_mhz()` — uses the returned exclude count instead of
  hardcoded 70.
- `current_applied_gpu_offset_excludes_low_points()` — same.
- `live_selective_gpu_offset_matches_requested_state()` — now delegates to the
  multi-exclude detection instead of its own hardcoded-70 inner loop.
- `live_selective_gpu_offset_matches_requested_state_with_tolerance()` — passes
  the new parameter.
- Declaration in `main.cpp` updated with default `nullptr` for the new parameter.

Debug logging greatly expanded: every exclude-try x candidate combination is logged
with match/violation counts, and the final BEST result is clearly marked.

### Files changed
- `source/main_gpu_state.cpp` — `detect_live_selective_gpu_offset_state()`,
  `current_applied_gpu_offset_mhz()`, `current_applied_gpu_offset_excludes_low_points()`,
  `live_selective_gpu_offset_matches_requested_state()`,
  `live_selective_gpu_offset_matches_requested_state_with_tolerance()`
- `source/main.cpp` — declaration update


## 2026-05-07: Version bump 0.12 → 0.13, remove "<name> Edition" title, fix APP_VERSION quoting

- Bumped all version references (`VERSION`, `build.py`, `app_shared.h`, `linux_port.h`) to `0.13`
- Removed `APP_EDITION "<name> Edition"` from `app_shared.h` and the `\"%s\"` edition format from `entry.cpp` window title
- Title bar now shows `Green Curve v0.13` instead of `Green Curve v0.12 "<name> Edition"`
- **Root cause of quote chars in title:** `build.py` line 242 had `f'-DAPP_VERSION="\\"{APP_VERSION}\\""'` — the `\"` escaping on Windows produced `-DAPP_VERSION="0.13"` with literal quote chars embedded in the string. Fixed by removing the `-DAPP_VERSION` command-line flag entirely and adding `#include "app_shared.h"` to `string_table.cpp` so `APP_VERSION` comes from the clean `#define APP_VERSION "0.13"` in the header.
- Build 39 clean with `-Werror`

---

## 2026-05-07: Bugfix — service pipe ACL excludes non-admin users after boot (ERROR_ACCESS_DENIED)

- **Root cause:** `create_restricted_pipe_security_descriptor()` called `WTSQueryUserToken()` first, which fails at boot (no user logged in), causing the function to return `false`. The pipe then got the default LocalSystem-process ACL (SYSTEM + Administrators only). Non-elevated GUI got `ERROR_ACCESS_DENIED (5)` on `CreateFileW`.
- Since no client could ever connect, the pipe was never disconnected/recreated, so the bad ACL persisted forever.
- **Fix:** Restructured the function to build the base SDDL with `(A;;GRGW;;;AU)` (Authenticated Users) unconditionally, then optionally append the per-user SID when `WTSQueryUserToken` succeeds after login.
- The elevated GUI worked because it runs as Administrator (matches `BA` ACE).
- Confirmed fixed in build 38.

## 2026-05-07: Bugfix — pipe name session ID prevents GUI from finding service after reboot

- **Root cause:** The named pipe used session ID: `\\.\pipe\GreenCurveService_<sessionId>`. The service auto-starts at boot (session 0, before user login) creating `GreenCurveService_0`. After login, the GUI looks for `GreenCurveService_<userSessionId>` (e.g., `GreenCurveService_1`) — which doesn't exist.
- **Fix:** Removed session ID from `background_service_pipe_name()`. Pipe name is now fixed `\\.\pipe\GreenCurveService`. The security descriptor already restricts access to the active console session user, so session-based pipe names were redundant.

## 2026-05-07: Bugfix — unchecking service checkbox deletes binary from disk

- **Root cause:** `cleanup_secure_service_binary_after_remove()` called `DeleteFileW()` on the service binary in the install directory. After changing `get_secure_service_install_dir_w()` to return the GUI exe directory (adjacent layout), this destroyed the user's manually-placed binary.
- **Fix:** Removed `DeleteFileW()` and `RemoveDirectoryW()` from the cleanup function. SCM unregistration (`DeleteService`) is sufficient. The binary stays on disk where the user placed it.

## 2026-05-07: Service install path moved from Program Files to GUI-adjacent directory

- `get_secure_service_install_dir_w()` changed from `SHGetKnownFolderPath(FOLDERID_ProgramFiles, ...)` to `GetModuleFileNameW(nullptr, ...)` trimming the filename, so the service installs adjacent to whichever GUI binary was executed.
- This means the service runs from the user's manual installation directory (e.g., `%USERPROFILE%\Programme\greencurve\`) instead of `C:\Program Files\Green Curve\`.
- Required corresponding fixes: `cleanup_secure_service_binary_after_remove()` no longer deletes the binary (user manages it manually), and `ensure_secure_service_binary_path` skips the copy when source == target.

## 2026-05-07: Bugfix — VF point 75 divergence in correction loop with lock tail

- **Root cause:** Lock tail offset (+480 MHz at ci=76) shifts adjacent non-tail point bases on Blackwell (point 75 base drifted 2460 → 2572 MHz). The correction loop each pass computed `target - (live_freq - current_offset) = -120000 - offset` → each pass added another -120000 kHz. After ~8 passes the offset hit the -1000000 kHz range limit and produced "minimum curve offset" error.
- **Fix:** After each correction pass, check non-tail points whose required delta exceeds the hardware range. Accept their actual MHz as the new target (`verifyDesired.curvePointMHz[ci] = actualMHz`), preventing the divergent negative-offset growth.
- Also increased correction passes from 2 → 25 with early break on convergence (build 31), though the divergence guard makes the actual count largely irrelevant.
- Added `appliedLockVi/Ci/Freq` fields to track lock checkbox toggles as detectable changes for "No changes to apply" check.
- Added originals refresh after reset-before-apply to fix stale `originalCurveFreqkHz[]`.

## 2026-05-07: Bugfix — reset-before-apply stale originals cause wrong VF offsets; error log write fails

- **Root cause (stale originals)**: When applying settings with `resetOcBeforeApply=true`, `reset_oc_before_gui_apply()` resets OC offsets and calls `read_live_curve_snapshot_settled()` which re-reads the live curve. But the `originalCurveFreqkHz[]` capture at the top of `apply_desired_settings_service()` saved the PRE-reset curve frequencies. After reset, the GPU's base frequencies can shift (temperature-dependent boost), so offset computations using stale originals computed wrong deltas — points that should stay at their base frequency got offset 0 against a stale base, landing at the wrong MHz.
- **Evidence**: Log shows point 75 verified at 2572 MHz instead of requested 2452 MHz (+120 MHz drift). The offset was computed as `desired(2452) - staleOriginals(2452) = 0`, but the post-reset base was actually 2572 MHz.
- **Fix**: After `reset_oc_before_gui_apply()` completes, refresh `originalCurveOffsets[]`, `originalCurveFreqkHz[]`, and `originalCurvePopulated[]` from the live `g_app` state so all offset computations use post-reset baselines.
- Added debug logging for the refreshed original at point 75.

- **Root cause (error log write failure)**: `write_text_file_atomic_service()` in `main_diagnostics.cpp` created the temp file with share mode `0` (exclusive access), but then `service_verify_written_file_path()` tried to re-open it with `FILE_SHARE_READ`. The exclusive handle blocked the verification open, causing "Cannot verify written file path".
- **Fix**: Changed `CreateFileA` share mode from `0` to `FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE` so the verification re-open succeeds.

---

## 2026-05-07: Add MiniDumpWriteDump crash dumps to localappdata

- Previously, the `SetUnhandledExceptionFilter` handler only wrote a text crash breadcrumb (`greencurve_crash.txt`) — no `.dmp` files were generated.
- Added `MiniDumpWriteDump` to the exception filter in `main_diagnostics.cpp`:
  - Writes `greencurve_crash_YYYYMMDD_HHMMSS.dmp` to `%LOCALAPPDATA%\Green Curve\`
  - Uses `MiniDumpWithDataSegs` — captures stack, thread info, and data segments without full memory overhead
  - Both GUI and service processes have this (both install the same exception filter)
  - Keeps the existing text breadcrumb alongside the dump
- Added `-ldbghelp` to both `WINDOWS_LINK_LIBS` and `WINDOWS_SERVICE_LINK_LIBS` in `build.py`
- Added source regression check for `MiniDumpWriteDump` presence in diagnostics shard

---

## 2026-05-07: Bugfix — lock VF curve checkbox produces "No changes to apply"

- **Root cause**: When the user checks a lock VF curve checkbox (with no other OC settings active), `capture_gui_apply_settings()` in `main_runtime_gpu.cpp` detected no changes because:
  1. The curve-unchanged check intentionally skips locked-tail points (lines 303-311) since they are dynamically flattened
  2. GPU offset is 0 (unchanged), mem offset is 0 (unchanged), power is 100 (unchanged), fan is auto (unchanged)
  3. There was no tracking of the previously-applied lock state, so the lock state change itself was invisible to the "no changes" check
- **Fix**: Added `appliedLockVi/Ci/Freq` fields to `AppData` (in `app_shared.h`) to track what lock was last successfully applied:
  - `sync_applied_lock_state_from_curve()` in `gpu_backend.cpp` syncs the applied lock state from the live curve whenever hardware is read and no GUI edits are pending
  - After a successful apply in `gpu_backend_apply.cpp`, applied lock state is updated to match what was just written
  - `apply_service_desired_to_gui()` in `main_state_sync.cpp` syncs applied lock state from service desired settings
  - In `capture_gui_apply_settings()`, added a `lockChanged` boolean that detects whether the lock state differs from the last applied state
  - The fan-only shortcut and "No changes to apply" check now also require `!lockChanged`
- Added debug logging for the lock state comparison
- Builds clean with zero warnings (`-Werror`)

---

## 2026-05-07: Bugfix — external hardware reset causes stale GUI profile restore

- **Root cause**: `maybe_load_selected_profile_to_gui_without_apply()` in `config_profiles_ui.cpp` always restored the saved profile into GUI controls after refreshing the service snapshot, without checking whether the hardware state matched the profile.
- **Evidence**: After an external tool reset to defaults, logs showed service snapshot at defaults (`gpu=0 mem=0 power=100 fanMode=0`) but GUI populated with stale profile values (`gpu=475 mem=3000 power=100 fanMode=2`).
- **Fix**: Added `profile_mismatches_live_hardware()` helper that:
  1. Captures the service's active desired state (if any) to determine if the profile was previously applied through the service
  2. Builds a live desired from current hardware via `build_full_live_desired_settings()`
  3. Compares profile control fields (gpu offset, mem offset, power limit, fan mode, curve points) against live state
  4. Skips profile restore on mismatch, showing live defaults instead
  5. Preserves normal restore for "saved but never applied" profiles (service has no active desired)
- Added comprehensive debug logging for comparison decisions and mismatched fields.
- Updated `llm-wiki/config-profiles.md` with external reset detection documentation.

---

## 2026-05-07: Code audit fix wave — stack safety, pipe ACL, RAII, diagnostics

Implemented 7 fix recommendations from a code audit:

### F-01-001: Large stack buffers moved to heap
- Replaced `unsigned char buf[0x4000]` (16 KB) stack allocations with `HeapBuffer` heap allocations in `gpu_backend.cpp` (`nvapi_read_curve`, `nvapi_read_offsets`, `nvapi_set_point`) and `main_runtime_nvml.cpp` (`nvapi_get_vf_info_cached`)
- Replaced `char buf[4096]` stack allocation with `malloc`/`free` in `read_nvidia_smi_max_clocks`
- Moved `HeapBuffer` struct from `main_runtime_gpu.cpp` into `app_shared.h` so it is available to all shard files
- This eliminates stack-overflow risk on the 64 KB fan-runtime thread when VF read/write functions nest multiple 16 KB buffers

### F-01-003: Service pipe ACL restricted (no IU fallback)
- Removed the "Interactive Users" (IU) SDDL fallback in `service_pipe_server_thread_proc()`; when the restricted per-session ACL cannot be built, the pipe creation is deferred instead of falling back to a broader ACL

### F-01-006: Sanitizer build support (already implemented, regression checks added)
- `--sanitizer` flag was already fully implemented in `build.py`; added source regression checks confirming the flag and `SANITIZER_FLAGS` are referenced

### F-01-007: Debug logging privacy notice
- Added a two-line privacy notice to the session marker in `main_diagnostics.cpp` informing users that logs may contain GPU identifiers, config paths, and applied settings, and how to disable logging

### F-01-008: Move semantics for RAII wrappers
- Added move constructors and move assignment operators to `ScopedHandle`, `ScopedGdiObject`, and `ScopedServiceHandle` in `win32_raii.h`

### F-01-009: PDB files (already gitignored, verified)
- Confirmed `*.pdb` is already in `.gitignore`; no tracked PDB files exist

### F-01-010: Defensive response message NUL termination
- Added `response.message[ARRAY_COUNT(response.message) - 1] = '\0';` before writing service response, as a belt-and-suspenders measure

### Regression hardening
- Added 13 new `require_text()` source regression checks for the above fixes in `build.py`
- All builds and tests pass: `--check --target windows`, `--test`

---

## 2026-05-07: Bugfix — pipe server identity verification from unelevated GUI (take 2)

- Root cause: `validate_service_pipe_server_identity()` required server binary path verification via `QueryFullProcessImageNameW`, which fails from an unelevated GUI against a LocalSystem service. SCM fallback via `QueryServiceConfigW` also failed because `OpenServiceW` was called with `SERVICE_QUERY_STATUS` instead of `SERVICE_QUERY_CONFIG`.
- Fixed: when image path cannot be queried (from unelevated context), log a warning and accept the PID match. The PID is already validated against the SCM, which is sufficient for the threat model.
- Also fixed the SCM fallback `OpenServiceW` access right to include `SERVICE_QUERY_CONFIG`.

- Fixed `validate_service_pipe_server_identity()` failing when the unelevated GUI cannot query the LocalSystem service process image path via `QueryFullProcessImageNameW` (access denied).
- Added SCM fallback: when `get_process_image_path()` fails, query the service binary path from the SCM via `QueryServiceConfigW` and use that for verification instead.
- This prevents the service from being falsely marked as "broken" when the GUI runs unelevated, which was causing unnecessary repair loops.
- Updated source regression check from `get_process_image_path` to `SCM binary path`.

## 2026-05-07: Code audit fix wave (phase 1-2)

Implemented fixes for 10 code-audit findings (F-15-001 excluded by user request):

### Security fixes

- **F-04-001 (pipe spoofing):** Added `validate_service_pipe_server_identity()` with server executable path verification. After PID matching, the client now opens the server process with `PROCESS_QUERY_LIMITED_INFORMATION`, calls `QueryFullProcessImageNameW`, and compares against the expected service binary path under Program Files. Added `get_process_image_path()` helper and `get_secure_service_install_dir_w()` forward declaration.
- **F-04-002 (LocalSystem TOCTOU):** `write_text_file_atomic_service()` now opens the parent directory handle with `FILE_FLAG_OPEN_REPARSE_POINT` before creating any temp file, resolves the final path, and verifies it is within the caller's profile directory. This prevents reparse-point redirection races before the first write.

### Correctness fixes

- **F-01-001 (VF preservation failure):** `gpu_backend_apply.cpp` now sets `curveRequestOk = false` when memory-only apply with VF preservation fails, so the caller sees an accurate failure result.
- **F-01-002 (fan rollback bypass):** Fan apply failure now triggers `rollback_to_safe_defaults()` when earlier GPU/memory/power writes succeeded, instead of returning immediately.
- **F-01-003 (multi-GPU identity):** NVML device selection now fails closed on multi-GPU systems when PCI identity is available but does not match. Ordinal fallback is only allowed on single-GPU systems or when no PCI identity is available.
- **F-02-001 (fan runtime state corruption):** `apply_fan_settings()` now validates before mutating state. Auto mode applies the driver write before stopping the runtime. Fixed mode validates the percent before stopping the runtime. Curve mode validates the curve before mutating `g_app.activeFanCurve`.
- **F-15-002 (multi-fan transactional):** `nvml_set_fan_manual()` now snapshots pre-write fan policy, tracks individual rollback failures, and logs them.

### Reliability fixes

- **F-02-002 (fan thread abandonment):** `stop_service_fan_runtime_thread()` now preserves the thread handle on timeout instead of closing it, preventing a new thread from starting while the original may still reference shared state.
- **F-03-001 (signed overflow):** `load_runtime_selective_gpu_offset_request()` now rejects persisted GPU offset values outside ±1000000 MHz to prevent signed overflow when multiplied by 1000 for kHz conversion.

### Regression hardening

- Added 9 new `require_text()` source regression checks for the above fixes in `build.py`.
- All builds and tests pass: `--check --target all`, `--test`, `--sanitizer --test`, `--sanitizer --check`.

---

- Logs showed the broken service window at 15:24 was a real app-version mismatch: GUI `0.12` build 10 was talking to a still-running service `0.11` build 8. Later same-version build drift (`0.12` build 10 service with newer GUIs) was accepted correctly.
- Added a repair path for installed-but-broken service state: the GUI now preserves the ping failure reason, labels the service as repair-needed, and clicking the installed service control repairs/restarts instead of forcing remove-then-install.
- The elevated install helper now stops an existing service before replacing the staged Program Files service binary, then updates the SCM command line and restarts it. This keeps version mismatch rejection intact while making stale service-binary replacement recoverable.

---

## 2026-04-29: ASCII caption cleanup

- Switched the GUI caption helper back to a plain ANSI write so the `v0.12 "<name> Edition"` title renders reliably.
- Reverted the top-level window class, creation, and single-instance lookup back to the ANSI API path too; mixing Unicode class creation with the existing ANSI fallback window procedure caused the title to collapse to `G`.
- Kept the executable icon on the restored Green Curve resource path; if Explorer lags after a rebuild, that is a shell cache issue rather than a code-path regression.

## 2026-04-29: Title-bar caption correction

- Corrected the earlier icon mistake: the `.exe` icon generator is back to the original project graphic.
- Kept the caption as `v0.12 "<name> Edition"` while logs and build fingerprints remain on the plain `0.12.x` path.

---

## 2026-04-29: Service restart and GPU selector layout follow-up

- Fixed a post-replacement service usability regression: GUI ping now rejects service app-version/protocol mismatches, but accepts and logs same-version/protocol build-number drift. This avoids forcing a service reinstall after halting the service, replacing binaries, and starting the installed service again.
- Moved the GPU selector from the vertical space above the global OC controls to the top-right graph header gap so it no longer overlays or truncates VF curve edit fields.
- Added source regression checks for compatible build mismatch acceptance and graph-header selector placement.

---

## 2026-04-29: Audit blocker repair wave

- Kept best-guess Pascal/Turing/Ampere VF writes enabled, but updated warning/status/probe expectations to clearly label the backend experimental and write-enabled.
- Added Windows GUI GPU selection and shared `GpuAdapterInfo` protocol data. Service snapshots include adapter lists and selected adapter state; apply/reset requests carry a target GPU and the service reinitializes or rejects mismatches.
- Hardened GUI-to-service trust by validating the connected named-pipe server PID against the SCM `GreenCurveService` process and creating the service pipe with first-instance/local-client protection where supported.
- Added a service-only safe file writer for LocalSystem writes under the caller profile: random same-directory temp, `CREATE_NEW`, temp/final path validation, atomic move, and post-move verification.
- Added GUI reset-before-apply semantics for OC/UV/power applies. Fan-only applies keep the existing fan shortcut and avoid resetting or reapplying OC/UV/power.
- Tightened fan apply safety: pre-validation before hardware mutation, staged runtime transitions, multi-fan exact readback, and rollback of changed fans on partial failure.
- Repaired the Linux split by moving shared Linux-private INI/path/parser declarations into `source/linux_port_internal.h` while keeping `source/linux_port.h` public.
- Fixed sanitizer check mode so it does not regenerate tracked `compile_commands.json`; tightened Zig cache trust so a bare cached `zig.exe` without the hash sentinel is not trusted.
- Restored Linux hardening flags. Inspection shows the static musl binary is now ET_DYN PIE, while Zig 0.13 still does not emit GNU_RELRO/GNU_STACK program headers for this static output.
- Verified `python build.py --test`, `python build.py --check`, `python build.py --target linux --check`, `python build.py --sanitizer --test`, and `python build.py --sanitizer --check` during the repair wave.

---

## 2026-04-28: Harden wrong-clock stale-state paths

- Re-audited the previous selective-offset + lock fix and confirmed the boost-region absolute-target loop is skipped when `gpuPolicyViaCurveBatch` is active.
- Found adjacent stale-state risks: persisted runtime selective offset could be trusted after live hardware reset, and service-local lock markers could be reused by a later no-lock interactive apply.
- Changed runtime selective offset persistence to require live VF shape verification before display/reuse. Uniform or zero GPU offsets now clear the runtime selective keys instead of storing `0`.
- Changed service apply lock handling so service requests only honor an explicit incoming lock; successful no-lock clock/curve applies clear stale service lock markers.
- Added source regression checks for live selective verification, stale persisted-selective logging, runtime clear behavior, and request-scoped service locks.

---

## 2026-04-28: Restore selected profile into GUI editor on startup

- Fresh logs showed the GUI/service build mismatch was fixed and RAM/GPU offsets were reported correctly after restart, but the VF curve fields were populated from the live service snapshot instead of the selected saved profile.
- Root cause: normal startup only auto-loaded a profile when `[profiles] app_launch_slot` was enabled; with app-start auto-apply disabled, `populate_edits()` rebuilt the curve editor from live hardware state.
- Added GUI-only startup restore for saved `[profiles] selected_slot` when app-start auto-apply is disabled. It refreshes the service snapshot, loads the selected profile into the editor with `populate_desired_into_gui()`, logs the restore, and explicitly does not apply hardware settings.
- Added source regression checks for the GUI-only selected-profile restore path.

---

## 2026-04-28: Preserve explicit high memory offsets

- Investigated logs where profile/config requested `mem_offset_mhz=3000`, but apply used `mem=2000` and wrote `target_driver_kHz=2000000`
- Root cause: memory offset flow treated NVML-reported memory range as a hard cap, so a reported max of `2000` caused explicit higher requests to be clamped/rejected before the driver write
- Changed config/profile load to preserve explicit `mem_offset_mhz` values outside the reported range and log the range mismatch
- Changed service-side apply to attempt explicit memory offsets even outside the reported range; the driver can still reject the write, and that remains an apply failure
- Follow-up from fresh logs: the GUI captured `mem=3000`, but the already-running service process still logged `mem=2000`, showing a stale service binary could silently keep the old clamp behavior. Bumped IPC protocol to 4 and added session marker version/protocol logging so old services are rejected instead of silently accepting new GUI requests.
- Added tracked `BUILD_NUMBER` plus `build.py` source fingerprinting: real builds bump the number after code changes, inject `APP_BUILD_NUMBER`, and log it in both GUI/service session markers. Check/test/LSP runs do not bump it.
- Added GUI/service identity validation to service ping: service responses now report `serviceVersion` and `serviceBuildNumber`, GUI rejects mismatches, and IPC protocol moved to 5 for the response layout change.
- Added source regression checks covering the no-clamp/no-reject behavior
- Verified with `python build.py --test` and `python build.py --check`

---

## 2026-04-28: AGENTS follow-up lint and regression alignment

- Audited the updated `AGENTS.md` expectations against current repo state
- Confirmed pure regression infrastructure already exists through `python build.py --test`; no new external test framework is needed
- Fixed stale regression expectations for the then-current IPC contract: service protocol version was 3 and memory offset clamped to `-5000..5000` MHz
- Linted wiki entries for stale protocol/source-check/build-source facts and refreshed the affected pages
- Source-size audit remains within the requested 600-800 line band for source files

---

## 2026-04-28: Continued config profile banding

- Split the profile system across `source/config_profiles.cpp` and `source/config_profiles_ui.cpp`
- Kept the core INI/profile load-save-clear logic in `source/config_profiles.cpp`
- Moved profile UI refresh, migration, prompt handling, and startup/logon behavior into `source/config_profiles_ui.cpp`
- Added the new file to the amalgamated Windows source bundle through `source/main_shell.cpp`
- Verified `python build.py --check` still passes after the split

## 2026-04-28: Continued Linux port banding

- Split `source/linux_port.cpp` into band-sized Linux source units: `source/linux_port.cpp`, `source/linux_port_profiles.cpp`, and an expanded `source/linux_main.cpp`
- Moved Linux CLI parsing, probe report generation, and asset generation into `source/linux_main.cpp`
- Kept Linux config/path/INI primitives in `source/linux_port.cpp` and profile serialization/load-save logic in `source/linux_port_profiles.cpp`
- Updated `build.py` and the Linux wiki pages to reflect the new source layout
- The Windows build check passes again after working around Zig's missing `icon.res` by generating a valid COFF placeholder object at that path

## 2026-04-28: Continued gpu_backend banding

- Split the service-side apply logic out of `source/gpu_backend.cpp` into `source/gpu_backend_apply.cpp`
- Moved the fan-apply helper into `source/main_fan_runtime.cpp` and the partial-apply rollback helper into `source/main_gpu_front.cpp`
- The touched files now land in the 600-800 line band again after removing the spacing noise from the extraction pass
- `python build.py --check` passes again once `ZIG_GLOBAL_CACHE_DIR` and `ZIG_LOCAL_CACHE_DIR` point at the repo scratch directory instead of the inaccessible default temp/cache location

---

## 2026-04-28: Continued main.cpp banding

- Split `source/main.cpp` into additional band-sized shards: `source/main_fan_runtime.cpp`, `source/main_gpu_front.cpp`, `source/main_gpu_state.cpp`, `source/main_shell.cpp`, `source/main_state_sync.cpp`, `source/main_service_runtime.cpp`, `source/main_service_ipc.cpp`, `source/main_service_server.cpp`, and `source/main_probe_config.cpp`
- Kept the remaining `source/main.cpp` body down to 662 lines while preserving the build
- Verified `python build.py --check` still passes after the split
- Current source-size audit has the new `main_*` shards in range, with `main.cpp` now in-band too

## 2026-04-28: Began main-file modularization

- Split the first helper block out of `source/main.cpp` into `source/main_diagnostics.cpp`
- Moved debug logging, crash breadcrumb, and exception-filter helpers into the new file
- Updated `build.py` source regression checks to follow the moved logging helpers
- Current worktree now has the refactor checkpoint commit plus the first source split

## 2026-04-28: Continued main-runtime sharding

- Split the large post-diagnostics runtime block of `source/main.cpp` into four banded shards: `source/main_runtime_control.cpp`, `source/main_runtime_nvml.cpp`, `source/main_runtime_gpu.cpp`, and `source/main_runtime_ui.cpp`
- Moved the helper code for startup/CLI plumbing, NVML telemetry, NVAPI control flow, and Win32 theming/GDI+ into those shards
- Verified the build still succeeds after the split
- Current source-size audit now has `main.cpp` and `main_diagnostics.cpp` within the 600-800 line band, plus four new runtime shards in range

---

## 2026-04-27: Initial llm-wiki bootstrap

- Created `llm-wiki/` directory structure with 8 topic pages + log
- Bootstrapped from full repo inspection: `source/`, `build.py`, `VERSION`, `README.md`, `AGENTS.md`, `docs/linux-continuation.md`, `.gitignore`
- All pages cross-checked against actual source code and build script
- Pages created:
  - `index.md` — routing table
  - `overview.md` — project summary, source map, conventions
  - `build.md` — build system, targets, flags
  - `windows-architecture.md` — GUI/service split, IPC protocol, service lifecycle
  - `gpu-backend.md` — VF-curve backend, GPU family dispatch, NVAPI/NVML interface
  - `fan-control.md` — fan modes, curve math, runtime enforcement
  - `config-profiles.md` — INI config, profile slots, startup/logon tasks
  - `linux-scaffold.md` — Linux cross-build, TUI, blockers
  - `testing.md` — regression tests, source checks
  - `log/recent.md` — this file
