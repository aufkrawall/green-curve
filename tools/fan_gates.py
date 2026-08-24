"""Source gates for fan control.

Split out of build.py so the build script stays under its size ratchet; build.py
owns the CLI and passes its paths and check helpers in through `ctx`.  Nothing
here imports build.py, so the dependency runs one way only.

`ctx` is any object exposing SOURCE_DIR.
"""
import os


def _p(ctx, name):
    return os.path.join(ctx.SOURCE_DIR, name)


def check_manual_write_verification(ctx, require_text, forbid_text, backend_surface):
    """F-FAN-READBACK: a manual fan write is verified against driver *intent*.

    NVML exposes two different fan percentages and they are not
    interchangeable:

      * `nvmlDeviceGetFanSpeed_v2` is measured telemetry.  It reads 0 for as
        long as a zero-RPM fan stop is in effect, lags spin-up by seconds, and
        overshoots the request while the fan accelerates.
      * `nvmlDeviceGetTargetFanSpeed` is the driver echoing the duty it
        accepted — the actual readback.

    The Linux backend gated manual writes on `measured == requested`, so on an
    idle RTX 5070 every fixed/curve apply reported "fixed write/readback failed"
    and rolled the whole transaction back while the driver had in fact accepted
    the write.  Same defect class as the exact-equality memory-offset gate.
    """
    # `backend_surface` is build.py's concatenation of linux_backend.cpp and the
    # shards it #includes, so a guard does not move when a shard is split off.
    linux_backend_cpp = backend_surface
    linux_backend_mutation_cpp = _p(ctx, "linux_backend_mutation.cpp")
    fan_runtime_policy_h = _p(ctx, "fan_runtime_policy.h")
    main_gpu_front_cpp = _p(ctx, "main_gpu_front.cpp")

    require_text(fan_runtime_policy_h, "fan_manual_write_confirmed",
                 "the manual fan write verifier is one shared pure reducer")
    require_text(fan_runtime_policy_h, "fan_manual_effective_percent",
                 "the driver fan-range clamp is a shared pure reducer")
    require_text(linux_backend_cpp, "nvmlDeviceGetTargetFanSpeed",
                 "Linux resolves the fan intent getter, not only measured telemetry")
    require_text(linux_backend_cpp, "fan_manual_write_confirmed",
                 "Linux fixed-fan writes are verified through the shared reducer")
    require_text(main_gpu_front_cpp, "fan_manual_write_confirmed",
                 "Windows fan readback matching delegates to the same reducer")
    # The exact-equality gate that broke Linux fan control outright.
    forbid_text(linux_backend_cpp, "verify == (unsigned int)fanPercent",
                "manual fan writes must not be gated on measured fan speed")
    forbid_text(linux_backend_mutation_cpp, "verifyPercent == snapshot->fanPercent",
                "fan rollback must not be gated on measured fan speed")

    # Rollback restores what the driver was told to hold.  Restoring a measured
    # 0% from a stopped fan would pin the GPU at a duty nobody asked for.
    require_text(linux_backend_mutation_cpp, "nvml_read_fan_intent",
                 "the fan rollback snapshot captures driver intent")
    require_text(linux_backend_mutation_cpp, "snapshot->fanTargetKnown",
                 "the fan rollback snapshot records whether intent was readable")

    # The driver silently clamps a duty into nvmlDeviceGetMinMaxFanSpeed's
    # range (an RTX 5070 floors manual control at 30%), so the range has to be
    # known before a write can be verified or a percentage advertised.
    require_text(linux_backend_cpp, "getMinMaxFanSpeed(g->nvmlDevice",
                 "Linux queries the driver fan duty range")
    # Curve mode re-asserts every poll interval; an unconditional line there
    # floods the journal and buries the failures worth reading.
    require_text(linux_backend_cpp, "g->fanWriteLogged",
                 "the manual fan write outcome is logged on transitions only")
    require_text(linux_backend_cpp, "fan_manual_effective_percent",
                 "Linux clamps a manual duty into the driver range before writing")
    forbid_text(_p(ctx, "linux_daemon_snapshot_runtime.cpp"), "s->fanMaxPct = 100;",
                "the published fan range comes from the driver, not a hardcoded 0..100")


def check_runtime_failsafe(ctx, require_text, forbid_text, require_order):
    """F-LNX-FAN / F-LNX-LIFECYCLE: nothing may strand a manual fan duty.

    The fan runtime pins a manual duty; every path that stops tracking
    temperature must escalate, and no exit path may leave that duty behind.
    """
    linux_fan_runtime_h = _p(ctx, "linux_fan_runtime.h")
    linux_daemon_lifecycle_h = _p(ctx, "linux_daemon_lifecycle.h")
    linux_daemon_serve_h = _p(ctx, "linux_daemon_serve.h")
    linux_daemon_cpp = _p(ctx, "linux_daemon.cpp")
    fan_runtime_policy_h = _p(ctx, "fan_runtime_policy.h")

    forbid_text(linux_fan_runtime_h, "CLOCK_REALTIME",
                "Linux fan poll deadline must not follow wall-clock steps")
    require_text(linux_fan_runtime_h, "clock_gettime(CLOCK_MONOTONIC, &deadline)",
                 "Linux fan poll deadline uses the monotonic clock")
    require_text(linux_daemon_lifecycle_h, "pthread_condattr_setclock",
                 "fan wake condition is bound to an explicit clock")
    require_text(linux_daemon_lifecycle_h, "CLOCK_MONOTONIC",
                 "fan wake condition uses CLOCK_MONOTONIC, not the default realtime clock")
    forbid_text(linux_daemon_lifecycle_h, "PTHREAD_COND_INITIALIZER = ",
                "fan wake condition is not left on the default realtime clock")

    # Telemetry loss and refused writes share one escalation ladder on both
    # platforms; the Linux daemon previously ignored temperature-read failures.
    require_text(linux_fan_runtime_h, "fan_runtime_observe_result",
                 "Linux fan runtime routes results through the shared failure reducer")
    require_text(linux_fan_runtime_h, "FAN_RUNTIME_OUTCOME_TELEMETRY_FAILED",
                 "Linux fan runtime escalates temperature-read failures")
    require_text(linux_fan_runtime_h, "fan_runtime_escalation_after_auto_restore",
                 "Linux fan runtime forces maximum cooling when auto restore fails")
    require_text(linux_fan_runtime_h, "runtimeGeneration != observedGeneration",
                 "a newly applied curve cannot inherit the previous curve's hysteresis state")
    require_text(fan_runtime_policy_h, "FAN_RUNTIME_ESCALATION_EMERGENCY_MAX",
                 "shared fan policy defines the emergency escalation")

    # systemctl stop must not strand a manual fan duty.
    require_text(linux_daemon_lifecycle_h, "sigaction(SIGTERM",
                 "Linux daemon installs a SIGTERM stop handler")
    require_text(linux_daemon_lifecycle_h, "sigaction(SIGINT",
                 "Linux daemon installs a SIGINT stop handler")
    require_text(linux_daemon_serve_h, "daemon_release_fan_to_driver",
                 "Linux daemon hands the fan back to the driver on shutdown")
    require_order(linux_daemon_cpp, "pl_thread_join(fanThread)",
                  "daemon_release_fan_to_driver()",
                  "fan worker is joined before the shutdown handback, so they cannot race")


def check_power_limit_range(ctx, require_text, forbid_text):
    """F-POWER-RANGE: one authoritative power-limit percentage range.

    The power limit is a percentage *of the board default*, so above 100% is a
    normal request (an RTX 5070 defaults to 250 W with a 300 W ceiling).  The
    Linux UI normalizer clamped it to 0..100 with the generic percent clamp, so
    a 105% target was rewritten to 100% on load, on save, and on every TUI
    refresh after an apply — the daemon held 105% while the TUI said 100%.
    """
    gpu_core_h = _p(ctx, "gpu_core.h")
    # The normalizer moved out of linux_port.cpp into a pure header precisely so
    # the regression harness can pin these clamps; linux_port.cpp is not part of
    # that harness, which is why the 0..100 clamp went unnoticed.
    ui_policy_h = _p(ctx, "desired_settings_ui_policy.h")

    require_text(gpu_core_h, "POWER_LIMIT_MAX_PCT = 150",
                 "the power-limit range is defined once, shared by both platforms")
    require_text(gpu_core_h, "clamp_power_limit_pct",
                 "the power-limit clamp is a single shared helper")
    require_text(ui_policy_h, "clamp_power_limit_pct",
                 "the UI normalizer uses the power-limit range, not a percent clamp")
    forbid_text(ui_policy_h, "clamp_percent",
                "the power limit must not go through the generic 0..100 percent clamp")
    forbid_text(_p(ctx, "linux_port.cpp"), "normalize_desired_settings_for_ui(DesiredSettings",
                "the normalizer stays in the tested pure header, not back in linux_port.cpp")
    require_text(_p(ctx, "linux_tui_actions.cpp"), "clamp_power_limit_pct",
                 "the Linux TUI power field clamps to the shared range")
    require_text(_p(ctx, "linux_cli_options.cpp"), "POWER_LIMIT_MIN_PCT",
                 "--power-limit rejects out-of-range values instead of clamping silently")


def check_native_zero_rpm(ctx, require_text, forbid_text):
    """F-FAN-ZERO-RPM: fan stop uses driver auto, never a manual 0% write.

    NVIDIA does not expose a portable force-fan-stop switch.  The supported
    cross-platform operation is to restore the automatic temperature policy;
    firmware then decides whether this board can stop its fans.  The custom
    curve takes manual ownership again at its first enabled point, with a
    Schmitt band to avoid repeated motor starts around that boundary.
    """
    gpu_core_h = _p(ctx, "gpu_core.h")
    zero_policy_h = _p(ctx, "fan_zero_rpm_policy.h")
    runtime_policy_h = _p(ctx, "fan_runtime_policy.h")
    win_runtime_cpp = _p(ctx, "main_fan_zero_rpm.cpp")
    linux_runtime_h = _p(ctx, "linux_fan_runtime.h")
    linux_mutation_cpp = _p(ctx, "linux_backend_mutation.cpp")

    require_text(gpu_core_h, "zeroRpmEnabled",
                 "native zero-RPM intent is part of the shared fan-curve model")
    require_text(zero_policy_h, "fan_curve_zero_rpm_hysteresis",
                 "the anti-cycle threshold is one shared policy")
    require_text(zero_policy_h, "hysteresis < FAN_ZERO_RPM_MIN_HYSTERESIS_C",
                 "native zero-RPM enforces a minimum two-degree Schmitt band")
    require_text(runtime_policy_h, "useDriverAuto",
                 "the shared runtime reducer selects policy, not manual 0%")

    require_text(win_runtime_cpp, "fan_runtime_next_action",
                 "Windows consumes the shared zero-RPM state machine")
    require_text(win_runtime_cpp, "nvml_set_fan_auto",
                 "Windows restores NVIDIA automatic fan control below threshold")
    require_text(linux_runtime_h, "fan_runtime_next_action",
                 "Linux consumes the shared zero-RPM state machine")
    require_text(linux_runtime_h, "linux_backend_fans_are_auto",
                 "Linux verifies the automatic-policy handoff")
    require_text(linux_mutation_cpp, "fanUseDriverAuto",
                 "the initial Linux curve transaction can start in driver auto")
    forbid_text(win_runtime_cpp, "nvml_set_fan_manual(0",
                "zero-RPM must not bypass a VBIOS minimum with a manual 0% write")
    forbid_text(linux_runtime_h, "linux_backend_set_curve_fan_percent(&g_gpu, 0",
                "Linux zero-RPM must not issue a manual 0% write")

    require_text(_p(ctx, "fan_curve_dialog.cpp"), "FAN_DIALOG_ZERO_RPM_ID",
                 "the Windows fan-curve editor exposes native zero-RPM")
    require_text(_p(ctx, "fan_curve_dialog.cpp"),
                 "FAN_ZERO_RPM_GUI_DESCRIPTION",
                 "the Windows zero-RPM help uses the measured multiline layout")
    require_text(_p(ctx, "fan_curve_dialog.cpp"),
                 "SS_LEFTNOWORDWRAP | SS_NOPREFIX",
                 "the Windows zero-RPM help keeps its explicit three-line layout")
    require_text(_p(ctx, "ui_theme_button.cpp"),
                 "id == FAN_DIALOG_ZERO_RPM_ID",
                 "the Windows zero-RPM checkbox uses the established themed renderer")
    require_text(_p(ctx, "linux_tui_layout_fan_profiles.cpp"),
                 "ACTION_FAN_ZERO_RPM_TOGGLE",
                 "the Linux TUI exposes native zero-RPM")
    require_text(_p(ctx, "main_probe_config.cpp"), '"zero_rpm_enabled"',
                 "Windows profiles persist native zero-RPM")
    require_text(_p(ctx, "linux_port_profiles.cpp"), '"zero_rpm_enabled"',
                 "Linux profiles persist native zero-RPM")
    require_text(_p(ctx, "linux_cli_options.cpp"), '"--fan-zero-rpm"',
                 "Linux scripted profile editing can set native zero-RPM")
    require_text(_p(ctx, "linux_cli_options.cpp"),
                 "opts->fanOverrides.zeroRpm = true",
                 "Linux records zero-RPM as a partial CLI override")
    require_text(_p(ctx, "linux_main.cpp"),
                 "merge_linux_cli_desired_settings",
                 "Linux validates partial fan overrides without replacing a saved curve")


def check_all(ctx, require_text, forbid_text, require_order, backend_surface):
    check_manual_write_verification(ctx, require_text, forbid_text, backend_surface)
    check_runtime_failsafe(ctx, require_text, forbid_text, require_order)
    check_power_limit_range(ctx, require_text, forbid_text)
    check_native_zero_rpm(ctx, require_text, forbid_text)
