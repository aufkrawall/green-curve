# Green Curve — Overview

## Summary

Green Curve is a small NVIDIA GPU tuning tool. The next development/release version is **0.24.0**. `VERSION` is mandatory and is the single release-version source: `build.py` injects `APP_VERSION` into compile, test, and LSP commands and also feeds PE resources, archive names, and installer metadata. Header fallbacks are the neutral string `dev`; a source guard rejects embedded release fallbacks or missing injection.

Primary target is Windows (Win32 GDI) with an elevated background service. Linux is now a native NvAPI+NVML control port driven by a root systemd daemon and a CLI/TUI client.

## Architecture

### Windows (primary)

Two processes communicating over a local named pipe:

- **`greencurve.exe`** — unelevated GUI (WinMain in `source/entry.cpp`; UI in `source/ui_main.cpp`, `source/ui_main_window.cpp`, `source/ui_lock_checkbox.cpp`, and `source/main.cpp`)
- **`greencurve-service.exe`** — elevated background service (same source files, compiled with `-DGREEN_CURVE_SERVICE_BINARY=1`)

The service runs as `LocalSystem` and owns all live GPU control. The GUI sends
`ServiceRequest` structs over the pipe and receives `ServiceResponse` structs
back. Protocol version is 23 (magic `0x47535643u`); every response contains one
complete generation-stamped `ServiceStateEnvelope`, while mutating requests
also carry a client-generated operation ID and domain-scoped authority
preconditions. The fixed magic/version prefix permits header-first mismatch
diagnosis, and the envelope includes daemon identity plus typed GPU health. The
wire declarations live in `source/service_protocol.h`.

Protocol v14 added explicit validity bits for GPU offset, memory offset, power,
fan policy, and fan target readback. A desired value is never substituted for a
failed hardware read: clients must render that domain unavailable. Both the
Linux daemon (`linux_daemon_snapshot_runtime.cpp`) and the Windows service
(`main_state_sync.cpp` via the pure `source/control_readback_policy.h`) produce
these bits; a producer that left them zero would report every domain as
permanently unavailable.

Protocol v15 added `SERVICE_CMD_REFRESH_STARTUP_PROFILE`, which keeps the Linux
daemon's boot-apply snapshot equal to the profile slot it names after that
profile is edited, without letting a client re-bind the policy to another slot
or GPU. v16 gave that snapshot its own response member, `startupProfile`: v15
returned it in `desired`, which the envelope defines as the *active* intent and
which the daemon's end-of-request stamp rewrites, so the snapshot never reached
the client. One wire field, one meaning — `desired` is always the active intent.
See `linux-scaffold.md` and `source/startup_snapshot_policy.h`. v17 added
`SERVICE_CMD_RESUME_RESTORE`, the Linux standby-resume restore.

Protocol v19 added updater state/commands; v20 added Blackwell XBAR clock/MSVDD,
v21 added SYS clock, v22 added VIDEO clock, and v23 added native zero-RPM fan
curve intent. v24 assigns one v23 reserved fan-curve byte to the independent
zero-RPM fan-off gap; sizes stay fixed, but mixed-version peers are still
refused at the prefix because they disagree about the byte's meaning.
`ServiceResponse.outcomeSeverity` remains the v18 mechanism that
(`SUCCESS`/`WARNING`/`ERROR`). `status` is binary but a hardware write is not:
an apply can verify everything it was asked for, or commit while the driver
declines some VF points — the backend accepts the live value for those rather
than failing a whole profile, so both answer `SERVICE_STATUS_OK` and differ only
in the prose of `message`. Both producers derive the field at the single point
they write a response out, the validator rejects a severity that disagrees with
`status`, and the persisted operation records carry it so a deduplicated retry
replays the same answer. Its one consumer is the window's manual Apply/Reset
presentation (F-RESULT in `windows-ui-layout.md`), which shows a dialog only for
a warning or an error.

### Linux

- **`greencurve`** — glibc-dynamic binary in each Linux release payload (`dist/linux-<arch>/greencurve/greencurve`)
- Supports the responsive three-tab pseudo-GUI TUI, absolute live VF text/JSON export, config/profile editing, probe generation, root daemon install/remove, generation/topology-checked daemon apply/reset, and read-only self-test
- Uses `libnvidia-api.so.1` plus `libnvidia-ml.so.1`; the daemon owns GPU writes over a Unix socket
- See `llm-wiki/linux-scaffold.md` for the current Linux backend notes

## Source file map

| File | Responsibility |
|------|---------------|
| `source/main.cpp` | Windows orchestration glue plus top-level declarations and include routing for the extracted runtime shards |
| `source/main_fan_runtime.cpp`, `source/main_gpu_front.cpp`, `source/main_gpu_state.cpp`, `source/main_shell.cpp`, `source/main_state_sync.cpp`, `source/main_data_paths.cpp` | Early GPU/backend helpers, tray/UI plumbing, runtime state sync, and data-path/config bootstrap split out of `main.cpp` |
| `source/desired_settings_schema.h`, `source/linux_daemon_state.h`, `source/service_restart_snapshot_schema.h` | The FROZEN on-disk `DesiredSettings` layouts and the records that embed them, plus the widening migrations and the size `static_assert`s that make the next field addition a build break rather than a silent upgrade regression. Size selects the layout, version selects the semantics — see [persistence-schema](persistence-schema.md) |
| `source/profile_curve_semantics.h`, `source/profile_curve_origin_io.h`, `source/config_profile_curve_format.cpp`, `source/linux_profile_curve_codec.h` | What a saved VF point's number IS: the `curve_semantics` markers, the one shared decode, and the per-platform readers/writers for `absolute_with_origin` (absolute MHz plus per-point `from_gpu_offset`) |
| `source/service_protocol.h`, `source/service_protocol_validation.h`, `source/service_protocol_wire_validation.h`, `source/service_desired_mutation_policy.h`, `source/service_operation_tracker.h` | Protocol-v24 complete state/health envelope (plus updater state, Linux startup-apply policy, advanced clocks, native zero-RPM intent, and its independent fan-off gap), server-derived mutation domains and authority preconditions, the outcome severity that separates a clean success from a warned one, bounded deduplication state that replays it, typed origins, request/response wire structs, exact size gates, and strict validation |
| `source/main_service_state_envelope.cpp`, `source/main_service_snapshot_request.cpp` | Immutable service-state publication, service instance/revision/GPU-generation ownership, topology signatures, phase transitions, and serialized authoritative full refresh |
| `source/main_service_server.cpp` | Aggregates `main_service_request_policy.cpp`, `main_service_pipe.cpp` (with `main_service_pipe_primitives.h`, `service_ipc_throttle_policy.h`, `service_pipe_prefix_read.h`, `main_service_pipe_file_commands.cpp`, and the verbatim switch shard `main_service_pipe_switch.cpp`), the six-worker listener `main_service_pipe_listener.cpp`, and `main_service_host.cpp` |
| `source/main_service_runtime.cpp` | Aggregates `main_service_runtime_identity.cpp`, `main_service_fan_worker.cpp`, and `main_service_apply_runtime.cpp` |
| `source/main_service_ipc.cpp` | Aggregates `main_service_connection.cpp`, `main_service_client_commands.cpp`, `main_service_admin_client.cpp`, and `main_service_machine_config.cpp` |
| `source/main_service_lifecycle_events.cpp`, `source/main_service_lifecycle_apply.cpp`, `source/main_service_logon_coordinator.cpp` | Lifecycle inbox/reducer bridge, serialized logon/standby/driver writes, and the single long-lived prerequisite worker |
| `source/main_probe_config.cpp` | Probe-report and config/profile tail split from `main.cpp` |
| `source/main_diagnostics.cpp` | Debug-log PRODUCER side only: path resolution, formatting, session markers. The handle, rotation, write and flush moved to `main_debug_log_writer.cpp` (F-LOG-ASYNC, 2026-09-12); secure file-write helpers to `main_secure_write.cpp` (F-MAINT-1), crash artifacts to `main_crash_artifacts.cpp` |
| `source/main_debug_log_writer.cpp` | The one thread that touches the debug-log handle: ring hand-off, rotation, write, batched flush, and the lock-free crash drain |
| `source/main_crash_artifacts.cpp` | Windows crash breadcrumbs/minidumps, the fast-fail reporter, artifact rotation, and the NVML VEH. No locks, no heap, environment-only path resolution |
| `source/crash_artifact_policy.h` | Pure cross-platform rule for where a crash artifact may go (never the CWD) and which files rotation may delete (timestamp order, own names only) |
| `source/linux_crash_report.{h,cpp}` | Startup half of the Linux crash report: directory resolution, rotation, and the pre-opened descriptor the signal handler writes into |
| `source/main_secure_write.cpp` | Reparse/junction verification + service-side safe writer for caller paths (split from `main_diagnostics.cpp`) |
| `source/main_service_persist.cpp`, `source/main_service_operation_persist.cpp` | Protected active-intent snapshot, sticky lockout, nonce authorization, and latest-operation correlation persistence |
| `source/main_service_recovery.cpp`, `source/main_service_recovery_clock.cpp`, `source/main_service_recovery_ledger.cpp` | Fail-closed restore disablement, current-boot awake-time proof, and deduplicated persistent recovery history |
| `source/main_service_controlled_restart.cpp` | Nonce-bound helper/clean-stop protocol and controlled-recovery startup validation |
| `source/main_service_selected_gpu_pnp.cpp`, `source/selected_gpu_pnp_policy.h` | Exact selected-adapter Configuration Manager notifications and pure PCI identity matching |
| `source/main_service_install.cpp` | Service install / SCM lifecycle: failure-action config + verification, install/remove (split from `main_service_server.cpp`) |
| `source/service_acl.cpp` / `.h` | F-SEC-1 service-binary DACL hardening helpers (protected DACL apply/restore, secure-root check) |
| `source/main_runtime_control.cpp`, `source/main_runtime_capture.cpp` | GUI desired-settings capture plus shared config/CLI conversion helpers split out of `main.cpp` |
| `source/main_tray_autostart.cpp` | Per-user HKCU Run registration for the independent resident `--tray-start` GUI process |
| `source/main_startup_task_runtime.cpp` | Per-user Windows scheduled-task creation, repair, and logon combo synchronization |
| `source/main_runtime_nvml.cpp` | NVML loading, fan/power/clock telemetry, and GPU-state refresh helpers split out of `main.cpp` |
| `source/main_runtime_gpu.cpp` | NVAPI curve/control helpers, apply/rollback verification, and live-state detection split out of `main.cpp` |
| `source/gui_mutation_worker.cpp`, `source/gui_service_state.cpp`, `source/ui_mutation_completion.cpp`, `source/gui_service_model.h`, `source/gui_draft_policy.h`, `source/gui_service_io_queue_policy.h` | Sole runtime GUI service-I/O coordinator, epoch/revision reducer, independent edit draft, read coalescing/mutation supersession, and GUI-thread-only state/render adoption |
| `source/gui_tray_visibility.cpp`, `source/gui_window_redraw.cpp`, `source/gui_window_redraw_policy.h`, `source/gui_selected_gpu_pnp.cpp`, `source/gui_process_cleanup.cpp` | Durable tray-hidden postcondition, visibility-preserving coherent redraw transactions, presentation-only selected-device invalidation, and safe coordinator/process teardown |
| `source/service_health_probe_policy.h`, `source/main_fan_telemetry.cpp` | Expected-busy service health policy and mutation-aware fan telemetry refresh |
| `source/win32_utf8_paths.h` | Strict UTF-8-to-UTF-16 path/profile wrappers used at Win32 filesystem and INI boundaries |
| `source/main_runtime_ui.cpp` | Dark-mode, font, GDI+ curve rendering, themed combo, and other Win32 UI helper code split out of `main.cpp` |
| `source/ui_theme_button.cpp`, `source/ui_theme_checkbox.cpp`, `source/ui_checkbox_state.h` | Owner-drawn button/checkbox rendering, the themed-control id tables, and the derived-tick/last-painted repaint gate (F-CHECKBOX-PAINT) |
| `source/entry.cpp` | `WinMain` plus the CLI dispatcher (`--dump`, `--json`, `--probe`, `--service-install`, ...) |
| `source/main_cli_options.cpp` | `parse_cli_options()`, split out of `main_runtime_nvml.cpp` |
| `source/main_cli_admin.cpp` | The machine-wide administrator CLI commands behind one dispatcher |
| `source/main_settings_transfer.cpp` | `--export-active-settings` / `--apply-settings-file`: carrying live settings across an upgrade as an explicit CLI Apply. See [installer.md](installer.md) |
| `source/theme_palette.h` | The window palette, shared by the application and the standalone installer |
| `source/installer_*.{h,cpp}` | The standalone Win32 setup program and uninstaller (no application headers, no third-party code). See [installer.md](installer.md) |
| `source/app_shared.h` | Windows application state/constants and the shared model includes; the wire protocol is in `service_protocol.h` |
| `source/app_shared.cpp` | Small utility implementations (`dp()`, `nvmin()`, `nvmax()`) |
| `source/ui_main.cpp`, `source/ui_main_window.cpp` | Graph painting, command/message routing, and main-window behavior |
| `source/ui_main_apply.cpp`, `source/oc_high_warning_policy.h` | Apply/Refresh/Reset commands and the high-overclock confirmation (pure warn decision) |
| `source/ui_oc_hints.cpp`, `source/oc_range_hint_policy.h` | Themed per-field range tooltips for the overclock row (pure bound derivation) |
| `source/ui_message_box.cpp`, `source/message_box_policy.h` | OS-theme-aware replacement for `MessageBox` used by every GUI prompt |
| `source/main_layout_policy.h`, `source/ui_main_layout.cpp`, `source/ui_main_controls.cpp`, `source/ui_main_control_lifecycle.cpp` | Pure responsive layout policy; work-area/DPI/scroll runtime; registered control creation and redraw-suppressed rebuild |
| `source/lock_checkbox_policy.h`, `source/ui_lock_checkbox.cpp` | Pure tri-state/activation policy plus one-shot mouse/Space gesture, state-stamp, double-click, and diagnostics handling |
| `source/gpu_backend.cpp` / `source/gpu_backend_apply.cpp` / `source/gpu_backend_reset_baseline.cpp` | VF-curve read/write via NVAPI private interfaces, NVML fan/power/clock operations, the service-side apply pipeline, and the reset-to-stock baseline every profile switch runs first |
| `source/control_readback_policy.h` | Pure protocol-v14 hardware-readback provenance for the Windows `ControlState`: the `HardwareReadbackValidity` block in `AppData`, rollback invalidation, post-detection GPU offset provenance, and the all-fans-answered rule |
| `source/config_utils.cpp` | INI config read/write helpers, mutex-protected config storage |
| `source/config_profiles.cpp`, `source/config_profiles_ui.cpp` | Profile slot I/O, save/load/clear, profile UI refresh, migration, and logon/startup task management |
| `source/config_profiles_gui_state.cpp` | Profile-intent projection onto the editor, profile status/state labels, background-service controls, and profile-row enablement |
| `source/gui_service_actionability_policy.h` | Pure "which control groups are actionable" decision from service/draft state (F-ACTIONABLE) |
| `source/fan_curve.cpp` | Fan curve math: default, normalize, validate, interpolate |
| `source/fan_curve.h` | Fan curve public API |
| `source/fan_zero_rpm_policy.h`, `source/fan_zero_rpm_profile_policy.h`, `source/main_fan_zero_rpm.cpp` | Shared native zero-RPM thresholds, independent fan-off gap, v23 profile migration, and the Windows automatic-policy handoff |
| `source/fan_curve_dialog.cpp` | Fan curve editor dialog (modal Win32 dialog) |
| `source/win32_raii.h` | RAII wrappers for Win32 handles (`ScopedHandle`, `ScopedCom`, `ScopedGdi`, etc.) |
| `source/linux_main.cpp`, `source/linux_cli_options.cpp`, `source/linux_live_output.cpp`, `source/intent_readback_status.h` | Linux `main()`/role dispatch, CLI parsing/help, complete live VF text/JSON output, and the pure configured-intent versus hardware-readback comparison shared with the TUI/tests |
| `source/linux_port.cpp` | Linux path/config/INI utilities and lower-level config primitives |
| `source/linux_port_profiles.cpp`, `source/linux_fan_curve_profile_save.h` | Linux profile load/save, fan-curve serialization, v23 zero-RPM gap migration, and Linux merge helpers |
| `source/linux_port_internal.h` | Linux-private INI/path/parser helper declarations shared by the split Linux source files |
| `source/startup_snapshot_policy.h`, `source/linux_startup_sync.{h,cpp}` | Pure decisions plus the shared TUI/CLI implementation that keeps the daemon's boot-apply snapshot equal to the profile slot it names, and reports it when they diverge |
| `source/linux_tui.cpp`, `source/linux_tui_render.cpp`, `source/linux_tui_actions.cpp`, `source/linux_tui_authority_runtime.cpp`, `source/linux_tui_refresh.cpp` | Raw ANSI terminal supervisor, diff renderer/input parser, responsive action/edit/profile controller, domain-aware draft authority, and health-driven refresh/rendering |
| `source/linux_tui_layout*.cpp` / `.h` | Pure POSIX-free responsive TUI cell-grid builder split across shared, VF, and fan/profile shards. The exact painted cells also own disjoint hitboxes, so pointer/focus coordinates cannot drift. Included directly by the test harness. |
| `source/linux_backend.cpp`, `source/linux_backend_nvml_write.cpp`, `source/linux_backend_discovery.cpp`, `source/linux_backend_mutation.cpp`, `source/linux_gpu_binding_policy.h`, `source/linux_architecture_policy.h`, `source/linux_vf_validation.h`, `source/linux_curve_targets.h` | Linux PCI binding/recovery, explicit-P0 modern NVML offset read/write with legacy fallback, architecture retention/fallback, atomic VF validation/publication, and transactional curve composition/mutation/rollback. |
| `source/linux_daemon*.cpp`, `source/linux_daemon_state.{h,cpp}`, `source/linux_daemon_lifecycle.h`, `source/linux_daemon_serve.h`, `source/linux_mutation_authority.h`, `source/linux_operation_runtime.h`, `source/linux_fan_runtime.h`, `source/linux_socket_permissions.h`, `source/linux_socket_path_permissions.h`, `source/linux_gpu_selection.h`, `source/linux_transaction.h`, `source/linux_service_install.cpp`, `source/linux_systemd_notify.cpp` | Linux header-first IPC, coherent state/health publication, domain-scoped authority, intent/operation journals, mutation deduplication, fan runtime with a shared failure-escalation ladder, async-signal-safe stop handling plus shutdown fan handback, classified `accept()` recovery, fail-closed filesystem-socket policy, normalized stable identity, and deterministic systemd restart/readiness verification. |

## Build

- **Only build tool:** `python build.py` (the full Windows/Linux x64/ARM64
  matrix, archives, and Windows setup executables on either Windows or Linux;
  native Windows emits separate `msvc` and `release` variant folders)
- Downloads Zig 0.13.0 plus a host-native llvm-mingw 20260519 automatically
- C++17, `-Oz`, `-fno-exceptions`, `-fno-rtti`, `-Werror`
- See `build.md` for full details

## GPU family support

| Family | Architecture ID | VF backend status |
|--------|----------------|-------------------|
| Pascal | `0x00000130` | Tested known backend |
| Turing | `0x00000160` | Tested known backend |
| Ampere | `0x00000170` | Tested known backend |
| Lovelace | `0x00000190` | Tested known backend |
| Blackwell | `0x000001B0` | Tested known backend |

Only unrecognized future NVIDIA GPU families use the best-effort fallback backend. Their VF write path remains enabled, but the GUI shows a warning that can be disabled by the user via `[warnings] hide_unrecognized_gpu_warning=1`.

## GPU selection

Windows builds expose a GPU selector in the main window. Linux exposes `--gpu DDDD:BB:DD.F` plus a TUI selector. Both persist stable PCI identity and pass it through protocol-v13's target field. Interactive mutations bind that target to the accepted service instance and GPU generation; VF-dependent mutations additionally bind to the topology signature. Linux multi-GPU writes fail closed unless BDF plus nonconflicting PCI device/subsystem identity resolves uniquely across NVML/NvAPI; a compatible sole-device fallback is single-GPU-only.

## Window title

The Windows title bar and Linux TUI show the version from the mandatory injected `APP_VERSION`. Logs and session markers use the same version/build-number path; identifiers and paths are fingerprinted in diagnostics.

## Conventions

- Debug logging is **default-on** (`APP_DEBUG_DEFAULT_ENABLED 1`)
- Logs: `%LOCALAPPDATA%\Green Curve\greencurve_debug.txt` (Windows), `~/.local/share/greencurve/greencurve_debug.txt` (Linux)
- Config: `%LOCALAPPDATA%\Green Curve\config.ini` (Windows), per-profile INI sections
- 5 profile slots, slot 1 is default
- No network, no telemetry, no cloud sync
- MIT license, copyright (c) 2026 aufkrawall

Last verified: 2026-07-27
