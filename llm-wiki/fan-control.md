# Fan Control

## Fan modes

Defined in `source/gpu_core.h`:

| Mode | ID | Description |
|------|-----|-------------|
| `FAN_MODE_AUTO` | 0 | Driver-controlled automatic fan |
| `FAN_MODE_FIXED` | 1 | Fixed percentage manual fan |
| `FAN_MODE_CURVE` | 2 | Temperature-to-fan% curve with runtime enforcement |

## Fan curve data model

`FanCurveConfig` (`source/gpu_core.h`):

| Field | Type | Description |
|-------|------|-------------|
| `points` | `FanCurvePoint[8]` | Up to 8 temperature/fan% points |
| `pollIntervalMs` | int | Polling interval (normalized to 250ms min) |
| `hysteresisC` | int | Ordinary curve-downshift hysteresis (0-10C) |
| `zeroRpmEnabled` | `gc_bool8` | Opt-in native zero-RPM handoff below the first enabled point |
| `zeroRpmHysteresisC` | `gc_u8` | Independent zero-RPM fan-off gap (2-30C, default 5C) |

`FanCurvePoint`: `{ enabled, temperatureC, fanPercent }`

## Fan curve math (`source/fan_curve.cpp`)

- `fan_curve_set_default()`: 5-point default curve (30C→20%, 45C→35%, 60C→55%, 72C→72%, 84C→90%)
- `fan_curve_normalize()`: sorts enabled points by temperature, clamps poll interval to 250ms minimum
- Normalization clamps ordinary curve-downshift hysteresis to 0-10C and the
  separate zero-RPM fan-off gap to 2-30C. Enabling zero-RPM never rewrites the
  ordinary downshift value.
- If normalization sees fewer than two enabled points, it resets to the safe
  default curve while preserving normalized poll interval, both hysteresis
  values, and zero-RPM intent. This avoids the old out-of-bounds write risk
  from trying to append defaults while keeping all disabled points.
- `fan_curve_validate()`: ensures monotonically non-decreasing fan%, temperature gaps, enabled count
- `fan_curve_interpolate_percent()`: linear interpolation between adjacent enabled points
- `fan_curve_clamp_percentages()`: clamps all fan% to hardware min/max range

## Fan curve dialog (`source/fan_curve_dialog.cpp`)

- Modal Win32 dialog with temperature/fan% combo boxes per point
- Up to 8 editable points, enable/disable toggle per point
- Live preview of the curve shape
- Owner-drawn **Native zero-RPM** checkbox plus a dedicated **Zero-RPM
  fan-off gap** combo. **Curve downshift** remains a separate combo.
- A dynamic two-line, non-wrapping readout shows the actual ON temperature
  (first point) and OFF temperature (ON minus fan-off gap). Its 48dp region,
  expanded 580dp minimum client height, shared font, and `COL_LABEL`/`COL_BG`
  rendering keep the text unclipped and visually seamless with the dialog.
- The main-window curve button says **zero-RPM enabled** and describes **fan
  stop/start** thresholds. It does not use `zero-RPM off`, which could be read
  as the feature being disabled rather than the fan currently being stopped.
- Min dialog size: 500x580 dp

## Runtime enforcement

Fan curve mode runs a periodic worker in the service process:

1. Reads current GPU temperature via NVML
2. Interpolates target fan% from the curve
3. Applies via NVML manual fan speed
4. Tracks consecutive NVML failures; falls back to driver auto fan after repeated failures
5. Reasserts manual fan settings periodically (not just on temperature change)
6. Hysteresis prevents rapid oscillation around temperature boundaries

Linux and Windows share the pure transition contract in
`fan_runtime_policy.h`: input is current temperature, prior applied percentage,
hysteresis, curve, and force-refresh state; output is the next percentage and
whether a write is required. Rising demand applies immediately. A falling
target is held until temperature crosses the configured hysteresis threshold.
The Linux daemon computes the initial percentage during Apply, waits
interruptibly for the normalized `pollIntervalMs`, and wakes immediately on new
desired state, reset, shutdown, or repeated NVML failure. After repeated
failures it restores driver automatic control and records an uncertain outcome.
The worker keys its hysteresis memory to `g_fanWakeGeneration`: a newly applied
curve discards the prior curve's percentage/auto state and observes the policy
that the transaction just established. Without that reset, the next poll could
reassert an old target over a successful new Apply.

### Linux fixed-duty maintenance (2026-09-24)

The Linux fan worker used to run for **curve** mode only: a fixed duty was
written once by the Apply and never observed again, although the daemon's
startup comment claimed the worker kept "a curve or fixed duty" asserted. After a
GPU reset or anything else that handed the fan back to the driver, the snapshot
still reported `Fixed N%` (it is intent, not readback). Windows re-asserts fixed
duty on its own 5 s cadence. Now `linux_fan_runtime.h` also runs for
`FAN_MODE_FIXED` every `LINUX_FAN_FIXED_MAINTENANCE_INTERVAL_MS` (5 s):
`linux_backend_fixed_fan_check()` reads each fan's control policy and intent
(`getTargetFanSpeed`) and the pure `fan_fixed_maintenance_policy.h` decides:
policy not manual or intent more than 1% off the driver-clamped target ->
re-write; policy unreadable -> telemetry failure (never a blind write). The
outcome feeds the same `fan_runtime_observe_result()` escalation as curve mode
(driver auto after 3 failures, 100% if auto is refused, record UNCERTAIN). Logs:
`daemon: fixed fan re-asserted at N% (<reason>) ok=`, `daemon: fixed fan
maintenance failed (n/3)`. Tests 6460-6467. Not run on Linux hardware.

### When the controller dies (2026-09-23)

The runtime above only protects the fan while its process lives. Both
platforms now record fan (Windows: all GPU) ownership durably before the first
write and, on the next start in the same boot, hand it back once if the
previous instance died without doing so: driver auto first, forced 100% if the
driver refuses. Windows additionally returns the rest of the GPU to stock,
matching its graceful stop; Linux leaves clocks, matching its own. See
[auto-restore-policy](auto-restore-policy.md#once-per-crash-ownership-handback-2026-09-23)
and [linux-scaffold](linux-scaffold.md). The fan is the one owned domain that is
unsafe without a controller, which is why it always goes first.

### Windows service fan worker lifecycle (2026-09-24)

The service drives curve/fixed fans from one worker thread
(`main_service_fan_worker.cpp`). Its start/stop and its runtime-mutex wait are
decided by the pure `fan_worker_lifecycle_policy.h` (tests 6300-6317):

- **A stop requested by the worker itself only signals.** Repeated failures
  escalate through `report_fan_runtime_failure()` ->
  `escalate_fan_runtime_failure()` (`main_fan_runtime_failure.cpp`), which stops
  the runtime and therefore the worker, from inside the worker. It used to be a
  self-join (always a 5 s timeout) that released the runtime mutex and re-took
  it while the escalation still held `g_appLock`; a telemetry request taking the
  mutex and then `g_appLock` in that window deadlocked the service (ABBA) until
  the fan-pulse wedge watchdog forced an emergency restart.
- **Escalation never holds `g_appLock`.** Counting is
  `note_fan_runtime_failure_locked()`; the NVML handback (auto, else 100%) and
  the runtime stop run after the lock is released, as `apply_fan_curve_tick()`
  already required for every other NVML call (a VEH-killed thread would orphan
  the lock).
- **Stopping never releases the caller's runtime mutex.** The worker acquires
  it with `lock_service_runtime_unless_signaled(g_serviceFanStopEvent)`
  (`WaitForMultipleObjects({stop, mutex})`, stop first), so a stop reaches it
  wherever it waits. Before, `stop_service_fan_runtime_thread()` dropped the
  mutex for up to 5 s in the middle of an Apply/Reset, letting a logon/resume
  restore or the updater (which serialize on the mutex only, not the pipe
  dispatch lock) run a whole hardware transaction inside it.
- **Ensure joins a retiring worker** (alive but stop signalled) instead of
  calling it "already running", then creates a fresh one.
- **The worker handle is owned under the runtime mutex.** The service main
  thread's startup/shutdown/watchdog paths now take it (the watchdog with
  `try_lock_service_runtime(0)` so it never blocks behind a long Apply).
  `service_fan_worker_note_unserialized()` logs any remaining unlocked caller.

Gated by `fan_gates.check_service_fan_worker_serialization()`. Not reproduced on
hardware: the deadlock was established by reading the lock order, not observed.

**Native fixture (2026-09-24).** The cancellable wait itself is
`fan_worker_wait_for_runtime_lock()` in `fan_worker_lock_wait_win32.h`, free of
service globals; `lock_service_runtime_unless_signaled()` calls it.
`tests/fan_worker_lock_tests.cpp` (6550-6569, Windows only) runs it against
real threads, events and mutexes: free mutex -> acquired and owned; stop plus
free mutex -> stop wins and the mutex is NOT taken; the deadlock contract (the
stopper holds the mutex, signals and joins a queued worker without releasing,
still owns it afterwards); a queued worker acquires when the holder releases;
a dead owner -> ABANDONED with ownership transferred. Outcomes are forced by
ownership, not timing; the 10 s bounded joins are hang detectors only. Not
covered: the service's own ABBA ordering between `g_appLock` and the runtime
mutex, and fan pulse age during a long Apply (needs a hardware trace).

## Native zero-RPM curve mode (protocol v24)

`zeroRpmEnabled` is opt-in, preserving the old all-manual curve for existing
profiles. It does **not** write manual 0%, because that bypasses the driver's
advertised minimum and is clamped or unsupported on many boards. Instead it
uses the portable NVIDIA operation: restore automatic fan policy below the
user's threshold, allowing VBIOS/firmware to stop the fans when the board
supports native fan stop. Firmware can still keep them spinning for its own
thermal reasons; Green Curve never claims it can force a stop.

The user retains full temperature control:

- **Fan ON temperature:** the temperature of the first enabled curve point.
- **Fan OFF temperature:** ON minus `zeroRpmHysteresisC`. This independent
  anti-cycle gap is 2-30C (5C default); it does not affect ordinary fan-speed
  curve updates.
- **Speeds above ON:** the ordinary editable curve. Manual percentages are
  clamped to `nvmlDeviceGetMinMaxFanSpeed`, but temperatures are not tied to
  the firmware duty minimum.

Example: first point 45C/30%, zero-RPM fan-off gap 12C, and curve-downshift
hysteresis 1C. The driver owns the fan below 45C; at 45C Green Curve takes
manual ownership at the curve target; after starting, it does not hand back
until 33C. In the 33–45C Schmitt band, automatic stays
automatic and manual stays manual according to the previous state. This avoids
repeated bearing/motor starts when temperature hovers around the boundary.
The 1C value still controls ordinary target reductions above the stop band.

`fan_runtime_next_action()` owns this state machine for both platforms. Windows
implements the automatic side in `main_fan_zero_rpm.cpp`; Linux uses
`linux_backend_fans_are_auto()` / `linux_backend_set_fan_auto()` in its worker
and can also start the initial Apply transaction in automatic mode. Both paths
periodically verify/reassert ownership and retain the normal failure ladder.
Green Curve's active intent remains `FAN_MODE_CURVE` while the live driver
policy is automatic.

Readback comparison in `intent_readback_status.h` understands the Schmitt band:
automatic is required below OFF, manual is required at/above ON, and either is
valid inside the band. Current temperature is required to evaluate this dynamic
expectation; without it, fan readback is unavailable rather than falsely
matched. Windows GUI profiles and Linux profiles persist strict
`zero_rpm_enabled=0|1` plus `zero_rpm_hysteresis_c=2..30`. The Linux CLI
exposes `--fan-zero-rpm 0|1` and `--fan-zero-rpm-hysteresis C`; the TUI has
separate **Curve downshift** and **Off gap** controls and shows computed
OFF/ON temperatures.
Linux fan CLI edits are field-masked partial overrides: toggling zero-RPM keeps
the profile's existing points, ON temperature, both hysteresis values, and
polling interval. A gap-only override likewise leaves the ordinary downshift
value and points untouched.
The merged curve is normalized and validated transactionally; conflicting
point temperatures fail with an explicit error instead of silently replacing
the user's curve with defaults.

### Shared failure escalation ladder

`fan_runtime_observe_result()` in `fan_runtime_policy.h` is the single pure
reducer for *both* platforms. It takes the previous consecutive-failure count,
one `FanRuntimeOutcome`, and a limit; it returns the new count, whether to log,
and an escalation:

| Escalation | Meaning |
|---|---|
| `NONE` | keep re-asserting |
| `RESTORE_AUTO` | limit reached; hand the fan back to the driver and mark state uncertain |
| `EMERGENCY_MAX` | driver refused the handback; force `FAN_RUNTIME_EMERGENCY_PERCENT` (100%) |

Two invariants this encodes, both previously violated on Linux only:

- **Telemetry loss escalates exactly like a refused write.** The runtime holds
  the GPU at a manual duty, so a failed *temperature read* strands that duty
  just as surely as a failed *fan write*. The Linux worker used to skip its
  whole body when `nvmlDeviceGetTemperature` failed — no counter, no lockout,
  no journal line — leaving the fan pinned indefinitely and silently.
- **A failed auto-restore escalates to maximum cooling.**
  `fan_runtime_escalation_after_auto_restore()` makes this explicit; Windows
  already did it, Linux only logged `auto=0` and gave up.

The counter saturates at `UINT_MAX` rather than wrapping, so a long-lived
failure can never roll back into an apparently healthy state.

On Windows, GUI telemetry is intentionally deferred while that GUI owns an
active Apply/Reset. The service uses a serialized pipe listener, so a telemetry
timeout during the known mutation means busy rather than unavailable. Cached fan
state remains visible and the operation response re-proves service health.

## Apply safety

- Fan settings are pre-validated before GPU clock, memory, power, or VF writes mutate hardware.
- Fan runtime transitions are staged: the previous runtime state is preserved until the requested driver/runtime state verifies. Auto mode applies the driver write before stopping the runtime. Fixed mode validates the percent before stopping the runtime. Curve mode validates the curve before mutating `g_app.activeFanCurve`.
- Multi-fan manual writes track which fans changed, require exact readback for success, and roll changed fans back to driver auto/default on partial failure. Rollback failures are tracked and logged individually (`rollbackFailures`). Pre-write fan policy is snapshotted for rollback.
- If fan settings are applied together with OC/UV/power, OC reset/apply happens first and fan changes use the transactional fan path afterward. If fan apply fails after earlier hardware writes succeeded, rollback to safe defaults is triggered.
- Fan-only GUI applies do not trigger OC/VF/memory/power reset or reapply.
- Fan runtime worker shutdown on timeout preserves the thread handle to prevent a new thread starting while the original may still reference shared events or runtime state.

## NVML fan API surface

Key functions used (see the NVML typedef table in `source/gpu_core.h`):

- `nvmlDeviceGetNumFans` / `nvmlDeviceGetMinMaxFanSpeed`
- `nvmlDeviceGetFanControlPolicy_v2` / `nvmlDeviceSetFanControlPolicy`
- `nvmlDeviceGetFanSpeed_v2` / `nvmlDeviceSetFanSpeed_v2`
- `nvmlDeviceGetTargetFanSpeed` (intended duty — the write readback)
- `nvmlDeviceSetDefaultFanSpeed_v2` (reset to auto)
- `nvmlDeviceGetFanSpeedRPM` (telemetry)
- `nvmlDeviceGetCoolerInfo` (fan target masks, control signals)

### Measured duty vs. intended duty (F-FAN-READBACK)

The two fan percentages are **not** interchangeable, and confusing them broke
manual fan control on Linux outright:

| Entry point | Meaning | Suitable as a write readback |
|---|---|---|
| `nvmlDeviceGetFanSpeed_v2` | measured/actual duty | **no** |
| `nvmlDeviceGetTargetFanSpeed` | duty the driver intends to hold | yes |

Measured on an RTX 5070 (driver 610.43.03), immediately after
`nvmlDeviceSetFanSpeed_v2(35)` returned `NVML_SUCCESS`:

```
readback getFanSpeed_v2    -> pct=0     (fan stopped; zero-RPM fan stop)
readback getTargetFanSpeed -> pct=35    (exactly what was written)
readback policy            -> 1         (MANUAL; the speed write flips it)
```

The measured value stays 0 while a zero-RPM fan stop is in effect and
overshoots during spin-up (a settled 55% duty measured 59%), so an equality
gate on it can never pass. `fan_manual_write_confirmed()` in
`fan_runtime_policy.h` is the one shared verifier: intent within ±2 confirms;
without an intent getter it falls back to a ±2 window on the measured value,
and a 0% request then requires an exact measured 0 (a stopped fan reads 0 under
driver auto too).

### The driver clamps a manual duty into its advertised range

`nvmlDeviceGetMinMaxFanSpeed` reports **30..100** on the RTX 5070. Writing 10%
returns `NVML_SUCCESS` and reports an intent of **30**. Green Curve therefore
clamps through `fan_manual_effective_percent()` before writing and verifies
against the clamped value — same "the driver snapped our request, that is not a
failure" contract as the memory-offset grid. The Linux daemon publishes the real
range in `ServiceSnapshot.fanMinPct/fanMaxPct` (it used to hardcode 0..100).

### Logging is transition-gated

Curve mode re-asserts the duty every poll interval (250 ms minimum), so
`nvml_set_fan()` logs `fan: set fixed N%% ...` only when the *effective* duty or
the ok/fail state changes (`LinuxGpuState::fanWriteLogged`), and
`nvml_query_ranges()` logs the duty range only on a transition. Gating on the
effective duty means a curve wandering across 27/28/29% while the driver floor
pins all three at 30% produces no lines at all. Verified: two fan lines in a
full minute of 500 ms-interval curve mode.

## Invariants

- Fan state is tracked in both GUI (`guiFanMode`, `guiFanCurve`, `guiFanFixedPercent`) and service (`activeFanMode`, `activeFanCurve`, `activeFanFixedPercent`).
- `activeFanMode` / `ServiceSnapshot.activeFanMode` represent **Green Curve-owned fan intent**, not arbitrary live driver fan policy. If an external fan controller makes NVML report manual fan policy while Green Curve intent is Default/Auto, Green Curve preserves Auto in the GUI/profile and logs the external policy instead of adopting Fixed Custom.
- Live fan telemetry remains separate: current fan percent, RPM, target policy, and `fanIsAuto` still come from NVML snapshots/telemetry. Control-state fan mode is intent; do not derive `fanIsAuto` from it.
- Profile Save preserves the visible GUI fan mode on the no-user-edits path. This prevents an external manual fan policy from being saved as Fixed Custom when the GUI/profile intent is Auto.
- Fan modes are mutually exclusive; switching modes stops the previous runtime
- `fanSupported` / `fanRangeKnown` must be true before fan controls are enabled.
  `fanRangeKnown` gates on fan *presence*, not on `nvmlDeviceGetMinMaxFanSpeed`
  succeeding — a driver without the range getter can still be driven manually
  over the full 0..100 span, and tying the gate to the getter would disable the
  controls outright.
- **A manual fan write is verified against driver intent, never against measured
  fan speed.** See "Measured duty vs. intended duty" above. Both the forward
  write and the rollback snapshot/restore in `linux_backend_mutation.cpp` use
  `nvml_read_fan_intent()`; the snapshot records `fanTargetKnown[]` so a missing
  intent getter is distinguishable from an intent of 0. Guarded by
  `tools/fan_gates.py`, including `forbid_text` on the two exact-equality
  expressions that caused the bug.
- **Native zero-RPM never writes manual 0%.** It restores NVIDIA automatic
  policy with `nvmlDeviceSetDefaultFanSpeed_v2`; VBIOS/firmware owns the actual
  stop decision. `tools/fan_gates.py` pins both platform paths, UI/profile/CLI
  carriage, the shared 2C Schmitt policy, and forbids manual-zero shortcuts.
- **No exit path may strand a manual fan duty.** Nothing re-asserts the curve
  once the controlling process is gone, so the Linux daemon hands the fan back
  to the driver in `daemon_release_fan_to_driver()` after the worker is joined,
  escalating to 100% if the driver refuses. This is only reachable because the
  daemon now installs SIGTERM/SIGINT handlers; before that `systemctl stop`
  killed it at default disposition and the whole teardown path was dead code.
- **The fan poll deadline must not follow the wall clock.**
  `g_fanWakeCondition` is initialized with
  `pthread_condattr_setclock(CLOCK_MONOTONIC)` and the deadline is computed
  from `CLOCK_MONOTONIC`. With the default realtime clock, a backwards NTP
  step or manual date change stalled re-assertion for the size of the step
  while a manual duty stayed pinned. Guarded by a `forbid_text` on the
  realtime clock token in `linux_fan_runtime.h`.
- Up to `MAX_GPU_FANS` (8) fans tracked per GPU
- **Windows fan escalation runs outside `g_appLock`, and nothing that stops the
  fan worker releases the runtime mutex.** See "Windows service fan worker
  lifecycle" above; `fan_worker_lifecycle_policy.h` is the decision, the fan
  gates pin the call sites.
- **An update install never freezes a manual duty.** The install reservation
  blocks the fan pulse, so it hands a curve/fixed runtime to driver auto first
  and restores it if the reservation is released without setup stopping the
  service (`update_install_fan_policy.h`, [updates](updates.md)).
- **Reset ordering (build 351, `F-RESET-INTENT`):** `service_reset_all()` must clear the active-desired intent (`g_serviceHasActiveDesired`/`g_serviceActiveDesired`) **before** `refresh_global_state()` + `initialize_gui_fan_settings_from_live_state()` + `populate_control_state()`. Fan intent is derived by `current_green_curve_fan_intent_mode()`, which returns the active-desired `fanMode` (or `activeFanMode`) — so if the clear runs *after* those (as it used to), the RESET re-reads the stale profile's Curve mode into `g_app.activeFanMode` and reports it, and `detect_locked_tail_from_curve()` (via `should_auto_detect_locked_tail_from_live_curve()`) preserves the old lock. Symptom of the old ordering: after Reset the GUI still shows "Custom Curve" fan mode and re-adopts the lock instead of Auto/stock. Guarded by `require_order` in `build.py`. This was NOT caused by the build-350 drift-isolation work (that touched no fan/reset code); the reset ordering was latent since the fan-intent-ownership change (build 321).

## Source of truth

- `source/fan_curve.h` / `source/fan_curve.cpp`: curve math, validation, interpolation
- `source/fan_zero_rpm_policy.h`: first-point threshold discovery, minimum
  Schmitt gap, and wire-flag canonicalization
- `source/linux_cli_fan_override_policy.h`: transactional field-masked Linux
  CLI fan overrides that preserve the rest of a saved custom curve
- `source/fan_runtime_policy.h`: platform-neutral hysteresis/next-action reducer,
  the shared `fan_runtime_observe_result()` failure escalation ladder, and the
  shared manual-write verifier `fan_manual_write_confirmed()` /
  `fan_manual_effective_percent()`
- `source/linux_backend_nvml_write.cpp`: the Linux NVML write helpers (clock
  offsets, power limit, fan policy/duty) plus `nvml_query_ranges()`; `#include`d
  by `linux_backend.cpp`, not compiled separately
- `tools/fan_gates.py`: fan source guards (manual-write verification, runtime
  failsafe/lifecycle, native zero-RPM, power-limit range), invoked from
  `run_source_regression_checks()`
- `source/fan_curve_dialog.cpp`: curve editor dialog
- `source/main_fan_runtime.cpp` / `source/main_fan_zero_rpm.cpp`: fan runtime
  timer, fan apply helpers, fixed/curve orchestration, and the Windows automatic
  handoff
- `source/main_fan_telemetry.cpp`: GUI/service telemetry refresh, including
  mutation-aware Windows probe deferral
- `source/linux_fan_runtime.h`: Linux interruptible fan worker and failure lockout
- `source/fan_worker_lifecycle_policy.h`, `source/main_service_fan_worker.cpp`:
  Windows fan worker stop/ensure plans and the cancellable runtime-mutex wait
- `source/main_fan_runtime_failure.cpp`: Windows failure counting (under
  `g_appLock`) and escalation (outside it)
- `source/linux_daemon_lifecycle.h`: daemon run state, monotonic fan wake
  condition, and async-signal-safe stop handling
- `source/linux_daemon_serve.h`: listener construction, the shutdown-aware
  accept loop, and the shutdown fan handback
- `source/main_gpu_state.cpp`: `current_green_curve_fan_intent_mode()` / fixed-percent / curve helpers used by snapshots, profile save, and tray state
- `source/main_state_sync.cpp` and `source/main_data_paths.cpp`: service snapshot/control-state fan intent projection and runtime bootstrap; live telemetry stays separate
- `source/main_gpu_front.cpp`: rollback helper that returns fan state to driver auto during partial-apply recovery
- `source/gpu_backend.cpp` / `source/gpu_backend_apply.cpp`: NVML fan write operations and the service-side apply pipeline that invokes the fan helpers
- `source/gpu_core.h`: fan mode enums, fan structs, NVML fan function types

## The Linux daemon ran a diverged copy of the curve math (fixed 2026-07-28)

Until 2026-07-28 `linux_port.cpp` carried its own `fan_curve_set_default`,
`fan_curve_normalize`, `fan_curve_validate`, `fan_curve_interpolate_percent` and
`fan_curve_format_summary`. `fan_curve.cpp` was **not** in `LINUX_SOURCE_FILES`,
so every fan-curve assertion in the harness validated code the Linux binary did
not execute. The two copies had diverged in the degenerate branch
(`enabledCount < 2`), in two ways:

| | `fan_curve.cpp` (tested) | `linux_port.cpp` (shipped on Linux) |
|---|---|---|
| Degenerate reset | full 5-point default, then `return` | keeps `defaults.points[0..1]`, falls through |
| Active points | 5 | 2 |
| Fan at 85 °C | 90% | **35%** |
| Fan at 95 °C | 90% | **35%** |
| Array writes | in bounds | `points[8]`, `points[9]` — **out of bounds** |

The overflow: falling through with `enabledCount = 2` reaches the disabled-point
loop, which writes `config->points[enabledCount + i]` for `i < disabledCount`.
One enabled point (7 disabled) reaches `points[8]`; none (8 disabled) reaches
`points[9]`. `FAN_CURVE_MAX_POINTS` is 8. Reproduced under ASan as a 12-byte
stack-buffer-overflow. Every path that normalizes a user-supplied curve
reached it (daemon request handling, profile load in `linux_port_profiles.cpp`,
TUI apply); `validate_service_request_for_ipc()` canonicalizes the `enabled`
flags but never requires two of them, so the invariant has to hold in the
normalizer itself.

Why nothing caught it: the harness tested `fan_curve.cpp`, and the
`service_request` fuzz target stops at the IPC validator, which *accepts* this
input — the defect is one step downstream in the daemon's own normalize call.

Fix: `fan_curve.cpp` is in `LINUX_SOURCE_FILES` and the duplicates are deleted.
A source guard forbids `linux_port.cpp` from defining any of them again.
Assertions 1930-1946 cover the degenerate branch with canary bytes around the
config, the 5-point/90% expectations, and preservation of the user's poll
interval and hysteresis; they fail against the old implementation (UBSan traps
the overflow outright).

**Degree sign:** the shared text now uses `GC_DEGREE` from `platform.h`, because
Windows writes captions through the ANSI (CP-1252) entry points where U+00B0 is
one `0xB0` byte, while the Linux TUI writes UTF-8 where it is `0xC2 0xB0`. A
bare `0xB0` is an invalid continuation byte and renders as a replacement glyph.
A guard forbids the hardcoded form in `fan_curve.cpp`, and assertion 1946
validates every byte of the summary as UTF-8 on Linux.

## Open questions

- No thermal failsafe: a user curve may request a low duty at a high
  temperature and the runtime obeys it. Handing back to driver auto above a
  critical temperature would change configured semantics, so it is a product
  decision, not a fix.
- The fan pulse cannot run while an Apply holds the runtime mutex (up to the
  ~20 s handler budget); the fan keeps its last duty for that time.

## Last Verified

- 2026-09-24: Windows fan worker self-stop/deadlock and runtime-mutex release
  fixed; update-install fan handback added. `python build.py --test` (new tests
  6300-6332, mutation-checked: breaking the self-stop rule fails 6300) and the
  full `python build.py` matrix passed. No hardware run of either path.
- 2026-08-24 (build 118): Split ordinary curve-downshift hysteresis from the
  zero-RPM fan-off gap, added a 2-30C dedicated profile/wire/CLI/GUI/TUI field,
  migrated v23 profiles by inheriting their prior effective gap, and bumped the
  protocol to v24 without changing wire sizes. Dynamic two-line Windows ON/OFF
  text and expanded geometry replace the clipped three-line description; the
  main summary now explicitly says `zero-RPM enabled` and `fan stop/start`.
  Compiled regressions, clang-tidy, ASan, 20k `service_request` fuzz iterations,
  packaged Linux round-trip/migration/error cases, and the complete x64/ARM64
  Windows/Linux release build passed. No live zero-RPM hardware pass was
  available; firmware stop behavior remains board-dependent.
- 2026-07-28 (build 26): Deduplicated the fan-curve math (above). Not yet
  re-verified against hardware — the fix changes what the daemon runs in the
  degenerate-config path, so a fan-curve hardware pass is worth repeating.
- 2026-07-27: Manual fan control verified working on RTX 5070 / Arch / driver
  610.43.03 after fixing the measured-vs-intended readback confusion. Hardware
  acceptance: fixed 35% (`nvidia-smi` reports 35%), fixed 55%, curve mode
  tracking temperature at a 500 ms interval, driver-range clamp (27% -> 30%),
  reset handing the fan back to driver auto (`policy=0`, 0%), and
  `systemctl stop` handback. `python build.py --test` passes; Linux x64/arm64
  build; all Windows TUs cross-compile clean (a full Windows link cannot run on
  this Linux host — llvm-mingw ships PE tools).
- 2026-07-13: Added the shared deterministic runtime reducer and Linux
  temperature-derived initial write, configured interruptible interval,
  immediate wake events, downward hysteresis, and repeated-failure transition
  back to driver automatic control. Covered by compiled pure tests and Linux
  x64/ARM64 checks.
- 2026-07-13: Fixed a runtime-log-confirmed false "service not responding"
  transition by deferring Windows fan telemetry and health pings while the GUI's
  own serialized mutation occupies the pipe. Added pure policy/source guards.
- 2026-06-28: Fan intent ownership documented after fixing a Save bug where an external controller's manual NVML policy could be adopted as Fixed Custom. Verified `python build.py --test` and full `python build.py` (build 321).
- 2026-05-20: Cross-checked degenerate fan-curve normalization fix and regression tests. Broader staged fan runtime and multi-fan rollback behavior last cross-checked 2026-04-29.
