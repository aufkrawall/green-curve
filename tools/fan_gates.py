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
    require_text(_p(ctx, "main_runtime_nvml.cpp"),
                 "g_app.fanTargetPercent[fan], g_app.readback.fan.target[fan]",
                 "Windows fan verification carries explicit target validity")
    forbid_text(main_gpu_front_cpp, "requestedPct > 0",
                "fan target validity cannot be inferred from a nonzero value")

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
    # The fan worker runs only for an intent proven on the served card (the
    # journal's ownership rule), never one matched by PCI location alone: a
    # revoked binding, or another card at the old location, gets no write.
    forbid_text(linux_fan_runtime_h, "&ctl.gpu.selectedGpu),\n",
                "the Linux fan worker never matches its intent by PCI location alone")
    require_text(linux_fan_runtime_h,
                 "linux_gpu_identity_matches(&ctl.activeTarget, &ctl.gpu.selectedGpu) && daemon_active_intent_on_served(ctl),",
                 "the Linux fan worker requires the intent proven on the served card")
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
                  "daemon_release_fan_to_driver(ctl)",
                  "fan worker is joined before the shutdown handback, so they cannot race")
    require_text(_p(ctx, "gpu_backend_apply.cpp"),
                 "service_apply_core_was_attempted(",
                 "the Windows apply evaluates core recovery from explicit attempts")
    require_text(_p(ctx, "gpu_backend_apply.cpp"),
                 "memApplied, powerChanged, fanChanged, clockCeiling.writeAttempted",
                 "a first fan write participates in Windows core recovery")
    for command in ("APPLY", "RESET"):
        ctx.require_order_in_operation(
            linux_daemon_cpp, f"case SERVICE_CMD_{command}:",
            "daemon_fan_ownership_allows_selected_write(",
            "linux_backend_capture_apply_snapshot(" if command == "APPLY" else "linux_backend_capture_reset_snapshot(",
            f"Linux {command} checks outstanding fan ownership before mutation")
    runtime = _p(ctx, "linux_auto_restore_runtime.h")
    ctx.require_order_in_operation(
        runtime, "static bool auto_restore_note_explicit_success(",
        "ctl.fanOwnershipHandbackPending", "linux_auto_restore_note_explicit_success(",
        "unrecovered fan ownership cannot clear the automatic-restore lockout")
    require_text(linux_daemon_serve_h, "daemon_fan_ownership_shutdown_should_release(ctl)",
                 "shutdown preserves inherited handback while releasing this process's writes")

    # Uncertainty idles the worker. Entering it after a write must first return
    # an owned manual duty to the driver, or that duty stays pinned untracked.
    ownership_h = _p(ctx, "linux_fan_ownership.h")
    ctx.require_order_in_operation(
        ownership_h, "static void daemon_enter_uncertain_state(",
        "ctl.stateUncertain = true", "daemon_fan_ownership_release_for_uncertain(",
        "entering uncertainty returns an owned manual fan")
    ctx.require_order_in_operation(
        ownership_h, "static void daemon_fan_ownership_release_for_uncertain(",
        "linux_backend_set_fan_auto(", "FAN_RUNTIME_EMERGENCY_PERCENT",
        "a refused uncertainty handback escalates to emergency cooling")
    for command in ("APPLY", "RESET"):
        ctx.forbid_text_in_operation(
            linux_daemon_cpp, f"case SERVICE_CMD_{command}:", "ctl.stateUncertain = true",
            f"Linux {command} enters uncertainty through the fan-returning helper")
        ctx.require_text_in_operation(
            linux_daemon_cpp, f"case SERVICE_CMD_{command}:", "daemon_enter_uncertain_state(",
            f"Linux {command} failures return an owned manual fan")
    for name in ("linux_auto_restore_runtime.h", "linux_startup_policy.h"):
        forbid_text(_p(ctx, name), "ctl.stateUncertain = true",
                    f"{name} enters uncertainty through the fan-returning helper")
    forbid_text(_p(ctx, "linux_startup_policy.h"), "daemon_enter_uncertain_state(",
                "automatic restore owns the startup uncertainty transition")
    # The worker's lockout already returned the fan; its helper records the
    # uncertainty and retires the marker without a second fan write.
    forbid_text(_p(ctx, "linux_fan_runtime.h"), "ctl.stateUncertain = true",
                "the fan worker's lockout enters uncertainty through the ownership helper")
    require_text(_p(ctx, "linux_fan_runtime.h"),
                 "    daemon_enter_uncertain_after_fan_lockout(ctl, why, autoOk);",
                 "a worker lockout retires the marker once the driver holds the fan")
    require_text(_p(ctx, "linux_fan_runtime.h"),
                 'daemon_fan_runtime_lockout(ctl, SERVICE_FAN_FAULT_REPEATED_FAILURES,\n'
                 '                    "fan curve runtime locked out", 0, 0, 0);',
                 "a curve lockout goes through the shared lockout helper")
    require_text(_p(ctx, "linux_fan_runtime.h"),
                 'daemon_fan_runtime_lockout(ctl, SERVICE_FAN_FAULT_REPEATED_FAILURES,\n'
                 '                    "fixed fan maintenance locked out", 0, 0, 0);',
                 "a fixed-duty lockout goes through the shared lockout helper")
    # A backend brought up by a request is recovered by the fan worker, outside
    # the request deadline. Automatic restore remains mutation-class and runs
    # the handback before selecting its restore target.
    forbid_text(_p(ctx, "linux_daemon_snapshot_runtime.cpp"),
                "daemon_fan_ownership_resume_deferred_handback(ctl)",
                "state reads do not perform fan hardware or durable-record writes")
    ctx.require_order_in_operation(
        linux_fan_runtime_h, "static pl_thread_ret fan_reassert_thread(",
        "daemon_fan_ownership_resume_deferred_handback(ctl)",
        "observedGeneration = fan_runtime_generation(ctl);",
        "the fan worker performs deferred recovery before its normal poll")
    ctx.require_order_in_operation(
        runtime, "ctl.gpuReady = true;", "daemon_fan_ownership_resume_deferred_handback(ctl);",
        "linux_backend_select_target(&ctl.gpu, target,",
        "an automatic-restore re-init runs the deferred fan handback before selecting")
    ctx.require_order_in_operation(
        runtime, "static LinuxAutoRestoreOutcome daemon_automatic_restore_write(",
        "auto_restore_prepare_backend(",
        "linux_auto_restore_may_write_after_prepare(",
        "automatic restore rechecks authority after backend preparation")
    ctx.require_order_in_operation(
        runtime, "static LinuxAutoRestoreOutcome daemon_automatic_restore_write(",
        "linux_auto_restore_may_write_after_prepare(",
        "LinuxMutationResult mutation = linux_backend_apply(",
        "no automatic mutation follows a failed post-preparation authority check")
    # A failed operation that started uncertain stays uncertain, in memory and
    # in the record it restores.
    ctx.require_text_in_operation(
        linux_daemon_cpp, "static bool restore_committed_record(",
        "linux_failed_operation_record(wasUncertain, hadPrevious)",
        "a failed operation keeps an uncertain record instead of rewriting ACTIVE intent")
    for command in ("APPLY", "RESET"):
        ctx.require_text_in_operation(
            linux_daemon_cpp, f"case SERVICE_CMD_{command}:",
            "linux_failed_operation_leaves_uncertain(wasUncertain,",
            f"a failed Linux {command} never clears existing uncertainty")
        ctx.forbid_text_in_operation(
            linux_daemon_cpp, f"case SERVICE_CMD_{command}:",
            "restore_committed_record(ctl, hadPrevious",
            f"Linux {command} passes its starting uncertainty to the record restore")


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
    zero_profile_policy_h = _p(ctx, "fan_zero_rpm_profile_policy.h")
    runtime_policy_h = _p(ctx, "fan_runtime_policy.h")
    win_runtime_cpp = _p(ctx, "main_fan_zero_rpm.cpp")
    linux_runtime_h = _p(ctx, "linux_fan_runtime.h")
    linux_mutation_cpp = _p(ctx, "linux_backend_mutation.cpp")

    require_text(gpu_core_h, "zeroRpmEnabled",
                 "native zero-RPM intent is part of the shared fan-curve model")
    require_text(gpu_core_h, "zeroRpmHysteresisC",
                 "zero-RPM fan-off hysteresis is independent on the wire")
    require_text(zero_policy_h, "fan_curve_zero_rpm_hysteresis",
                 "the anti-cycle threshold is one shared policy")
    require_text(zero_policy_h, "config->zeroRpmHysteresisC",
                 "zero-RPM thresholds use their dedicated value")
    forbid_text(zero_policy_h, "config->hysteresisC",
                "ordinary curve downshift hysteresis must not control fan stop")
    require_text(zero_profile_policy_h,
                 "fan_curve_migrate_legacy_zero_rpm_hysteresis",
                 "v23 shared-hysteresis profiles retain their old thresholds")
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
    forbid_text(linux_runtime_h, "linux_backend_set_curve_fan_percent(&ctl.gpu, 0",
                "Linux zero-RPM must not issue a manual 0% write")

    require_text(_p(ctx, "fan_curve_dialog.cpp"), "FAN_DIALOG_ZERO_RPM_ID",
                 "the Windows fan-curve editor exposes native zero-RPM")
    require_text(_p(ctx, "fan_curve_dialog.cpp"),
                 "fan_zero_rpm_gui_format_description",
                 "the Windows help renders the effective zero-RPM thresholds")
    require_text(_p(ctx, "fan_curve_dialog.cpp"),
                 "SS_LEFTNOWORDWRAP | SS_NOPREFIX",
                 "the Windows zero-RPM help keeps its explicit two-line layout")
    require_text(_p(ctx, "fan_curve_dialog.cpp"),
                 "FAN_DIALOG_ZERO_RPM_HYSTERESIS_ID",
                 "the Windows editor exposes an independent fan-off gap")
    require_text(_p(ctx, "ui_theme_button.cpp"),
                 "id == FAN_DIALOG_ZERO_RPM_ID",
                 "the Windows zero-RPM checkbox uses the established themed renderer")
    require_text(_p(ctx, "linux_tui_layout_fan_profiles.cpp"),
                 "ACTION_FAN_ZERO_RPM_TOGGLE",
                 "the Linux TUI exposes native zero-RPM")
    require_text(_p(ctx, "linux_tui_layout_fan_profiles.cpp"),
                 "TUI_FIELD_FAN_ZERO_RPM_HYSTERESIS",
                 "the Linux TUI exposes an independent fan-off gap")
    require_text(_p(ctx, "fan_curve.cpp"), "zero-RPM enabled | fan stop",
                 "the curve button distinguishes enabled intent from live fan state")
    forbid_text(_p(ctx, "fan_curve.cpp"), "zero-RPM off <=",
                "the curve button must not make enabled zero-RPM look disabled")
    require_text(_p(ctx, "main_fan_curve_profile_save.cpp"), '"zero_rpm_enabled=%d',
                 "Windows profiles persist native zero-RPM")
    require_text(_p(ctx, "main_fan_curve_profile_save.cpp"),
                 '"zero_rpm_hysteresis_c=%u',
                 "Windows profiles persist the independent fan-off gap")
    require_text(_p(ctx, "linux_fan_curve_profile_save.h"), '"zero_rpm_enabled"',
                 "Linux profiles persist native zero-RPM")
    require_text(_p(ctx, "linux_fan_curve_profile_save.h"),
                 '"zero_rpm_hysteresis_c"',
                 "Linux profiles persist the independent fan-off gap")
    require_text(_p(ctx, "linux_cli_options.cpp"), '"--fan-zero-rpm"',
                 "Linux scripted profile editing can set native zero-RPM")
    require_text(_p(ctx, "linux_cli_options.cpp"),
                 '"--fan-zero-rpm-hysteresis"',
                 "Linux scripted editing can set the independent fan-off gap")
    require_text(_p(ctx, "linux_cli_options.cpp"),
                 "opts->fanOverrides.zeroRpm = true",
                 "Linux records zero-RPM as a partial CLI override")
    require_text(_p(ctx, "linux_main.cpp"),
                 "merge_linux_cli_desired_settings",
                 "Linux validates partial fan overrides without replacing a saved curve")


def check_service_fan_worker_serialization(ctx, require_text, forbid_text):
    """The Windows service fan worker can neither deadlock the service nor
    open a hole in runtime serialization.

    * The worker used to escalate its own repeated failures while holding
      g_appLock, and the escalation stopped the worker from inside the worker:
      a self-join that released the runtime mutex for its five-second timeout
      and then re-took it -- still holding g_appLock.  Any telemetry request
      that took the mutex in that window and then g_appLock deadlocked the
      service until the wedge watchdog forced an emergency restart.
    * Every other stop released the runtime mutex while joining, which let a
      logon/resume restore or the updater run a whole hardware transaction in
      the middle of the caller's Apply or Reset.

    Both are properties no happy-path run shows; they are pinned here and by
    the pure policy tests (6300-6317).
    """
    worker_cpp = _p(ctx, "main_service_fan_worker.cpp")
    failure_cpp = _p(ctx, "main_fan_runtime_failure.cpp")
    fan_runtime_cpp = _p(ctx, "main_fan_runtime.cpp")
    zero_rpm_cpp = _p(ctx, "main_fan_zero_rpm.cpp")
    host_cpp = _p(ctx, "main_service_host.cpp")

    require_text(worker_cpp, "lock_service_runtime_unless_signaled(g_serviceFanStopEvent)",
                 "the fan worker's runtime-lock wait also ends on its stop event")
    require_text(worker_cpp,
                 "fan_worker_wait_for_runtime_lock(cancelEvent, g_serviceRuntimeLock, &waitResult)",
                 "the fan worker uses the natively tested cancellable wait")
    require_text(_p(ctx, "fan_worker_lock_wait_win32.h"),
                 "HANDLE handles[2] = { stopEvent, runtimeMutex };",
                 "the stop event is waited on first, so it wins over the mutex")
    require_text(worker_cpp, "fan_worker_stop_plan(g_serviceFanThread != nullptr,",
                 "stopping the fan worker goes through the pure stop plan")
    require_text(worker_cpp, "if (plan == FAN_WORKER_STOP_SIGNAL_ONLY) {",
                 "a stop requested by the fan worker itself only signals")
    require_text(worker_cpp, "fan_worker_ensure_plan(present, alive,",
                 "starting the fan worker goes through the pure ensure plan")
    forbid_text(worker_cpp, "bool lockHeld = service_runtime_lock_held_by_current_thread();",
                "stopping the fan worker must not release the caller's runtime lock")
    require_text(worker_cpp, "service_fan_worker_note_unserialized(",
                 "touching the fan worker handle without the runtime lock is logged")

    # Escalation writes NVML and stops the runtime: never under g_appLock.
    require_text(failure_cpp,
                 "    bool escalate = note_fan_runtime_failure_locked(action, detail);\n"
                 "    LeaveCriticalSection(&g_appLock);\n"
                 "    if (escalate) escalate_fan_runtime_failure(action, detail);",
                 "fan failure escalation runs only after g_appLock is released")
    for surface in (fan_runtime_cpp, zero_rpm_cpp, failure_cpp):
        forbid_text(surface, "handle_fan_runtime_failure(",
                    "the old escalate-under-g_appLock entry point stays gone")
    require_text(fan_runtime_cpp, '#include "main_fan_runtime_failure.cpp"',
                 "the fan runtime uses the split failure reducer")

    require_text(host_cpp, "service_shutdown_owned_fan();",
                 "ordinary service stop hands back the fan independently of GPU Reset")
    require_text(_p(ctx, "main_service_reset_fan.h"),
                 "const bool emergencyOk = nvml_set_fan_manual(FAN_RUNTIME_EMERGENCY_PERCENT,",
                 "ordinary service stop forces emergency cooling when Auto is unverified")

    # The service main thread owns the worker handle only under the runtime
    # lock, and never blocks its watchdog loop on it.
    require_text(host_cpp, "if (try_lock_service_runtime(0)) {",
                 "the watchdog checks fan worker health under a non-blocking runtime lock")


def check_fanless_request_rule(ctx, require_text, forbid_text, require_order):
    """F-FANLESS: Auto on a GPU without a controllable fan is inert.

    Notebook boards leave cooling to the platform EC; the driver reports zero
    fans or refuses the count.  The fan domain still travels in every GUI Apply
    (default selection Auto) and every saved profile (mandatory fan keys).
    Windows used to turn that Auto into a fan-auto write that could only fail,
    rolling the whole Apply back (no undervolt possible); Linux refused it as an
    unavailable domain.  fan_request_policy.h is the one rule both use;
    tests/fan_request_apply_tests.cpp executes the Windows step and
    tests/linux_scope_preflight_tests.cpp the Linux preflight.
    """
    policy_h = _p(ctx, "fan_request_policy.h")
    apply_h = _p(ctx, "main_fan_apply.h")
    require_text(policy_h, "static inline FanRequestDisposition fan_request_disposition(",
                 "fan request disposition is one shared pure rule")
    require_text(policy_h, "return disposition == FAN_REQUEST_HARDWARE;",
                 "only a hardware fan request needs the fan control surface")
    # Windows: the "does this selection hold?" predicate, prevalidation, apply.
    require_text(apply_h, "if (!g_app.fanSupported) return fan_selection_holds_without_fans(wantMode);",
                 "Auto needs no write on a GPU without a controllable fan")
    forbid_text(apply_h, "if (!g_app.fanSupported) return false;",
                "a fanless GPU must not report every fan selection as a pending change")
    ctx.require_order_in_operation(
        apply_h, "static bool validate_desired_fan_settings_for_apply(",
        "disposition == FAN_REQUEST_REFUSED_NO_FAN", "fan_curve_validate(",
        "non-Auto fan requests on a fanless GPU are refused before any write")
    ctx.require_order_in_operation(
        apply_h, "static void apply_fan_settings(",
        "app_fan_request_disposition(desired) == FAN_REQUEST_INERT_NO_FAN",
        'set_last_apply_phase("apply: fan auto write");',
        "the inert fan step returns before the fan-auto write")
    require_text(_p(ctx, "main_fan_runtime.cpp"), '#include "main_fan_apply.h"',
                 "the fan apply shard is compiled into the Windows runtime")
    require_text(_p(ctx, "main_runtime_control.cpp"), "app_fan_control_known_absent()",
                 "GUI capture never sends a non-Auto fan mode the disabled controls cannot change")
    require_text(_p(ctx, "config_profiles_gui_state.cpp"), "fan_editor_adoptable_mode(",
                 "profile load never puts an unchangeable non-Auto fan mode into the editor")
    # Linux: capability parity, preflight, transaction phase, daemon gate.
    mutation_cpp = _p(ctx, "linux_backend_mutation.cpp")
    require_text(mutation_cpp, "snapshot->fanCountReported && snapshot->fanCount == 0",
                 "Linux classifies a reported zero-fan board ABSENT like the Windows probe")
    require_text(mutation_cpp, "if (fan_request_needs_fan_surface(fanDisposition)) requested |= LINUX_MUTATION_FAN;",
                 "an inert fan request schedules no Linux fan phase")
    forbid_text(mutation_cpp, "if (d->hasFan) requested |= LINUX_MUTATION_FAN;",
                "fan ownership alone must not schedule a fan write")
    preflight_h = _p(ctx, "linux_backend_preflight.h")
    require_text(preflight_h, "fanDisposition == FAN_REQUEST_REFUSED_NO_FAN",
                 "Linux preflight refuses non-Auto fan requests on a fanless GPU by name")
    require_order(preflight_h, "unavailableDomains &= ~(gc_u32)SERVICE_MUTATION_DOMAIN_FAN;",
                  "if (unavailableDomains != 0) {",
                  "an inert fan request is not an unavailable domain in preflight")
    forbid_text(preflight_h, "if (d->hasFan && !snapshot->fanValid)",
                "the fan snapshot gate applies only to hardware fan requests")
    require_text(_p(ctx, "linux_daemon_snapshot_runtime.cpp"),
                 "service_snapshot_missing_mutation_domains(&req->desired, &current)",
                 "the daemon precondition gate exempts an inert fan request")
    # The TUI judges drafts with the same snapshot helper as the daemon.
    domains_h = _p(ctx, "service_inert_request_domains.h")
    require_text(domains_h,
                 "FAN_REQUEST_INERT_NO_FAN)\n        missing &= ~(gc_u32)SERVICE_MUTATION_DOMAIN_FAN;",
                 "only the inert fan shape is exempt from the missing-domain mask")
    require_text(_p(ctx, "linux_tui_authority.h"),
                 "service_snapshot_missing_mutation_domains(desired, &response->snapshot) == 0",
                 "TUI draft binding does not detach an inert fan request")
    require_text(_p(ctx, "linux_tui_mutation_actions.cpp"),
                 "service_snapshot_missing_mutation_domains(",
                 "TUI Apply does not block an inert fan request")
    require_text(_p(ctx, "linux_tui_actions.cpp"),
                 "service_snapshot_fan_control_known_absent(",
                 "TUI keeps Auto selectable on a GPU without a controllable fan")


def check_fan_intent_consistency(ctx, require_text, forbid_text, require_order):
    """A profile switch never leaves a released fan curve reported as active.

    The GUI omitted an unchanged fan from its reset-before-apply request (legacy
    meaning: keep), the shared scope upgrade turned that request into a profile
    replacement (meaning: release), the service wrote driver Auto, stopped the
    curve and still reported Curve -- re-derived from the outgoing intent -- and
    the relaxed profile match kept the profile ticked.  fan_intent_policy.h,
    service_scoped_apply_policy.h and main_service_fan_intent.cpp are the fix;
    tests/fan_intent_tests.cpp executes them.
    """
    scoped_h = _p(ctx, "service_scoped_apply_policy.h")
    require_text(scoped_h, "kept |= SERVICE_LEGACY_UPGRADE_KEPT_FAN;",
                 "the legacy scope upgrade keeps an omitted owned fan")
    require_text(scoped_h, "if (!replaceProfile || replacement) return kept;",
                 "only a verified named-profile replacement releases omitted domains")
    capture_cpp = _p(ctx, "main_runtime_capture.cpp")
    require_text(capture_cpp, "gui_full_apply_carries_fan(fanChanged, editorSelectsManualFan)",
                 "a full GUI Apply always carries an owned Fixed/Curve fan")
    forbid_text(capture_cpp, "    if (!fanChanged) {\n        resetFull.hasFan = false;",
                "an unchanged owned fan must not be stripped from a replacement request")
    gpu_state_cpp = _p(ctx, "main_gpu_state.cpp")
    ctx.require_order_in_operation(
        gpu_state_cpp, "static int current_green_curve_fan_intent_mode()",
        "return service_reported_fan_intent_mode(", "if (g_app.activeFanMode == FAN_MODE_CURVE)",
        "the service reports fan mode from intent and runtime before any GUI-side mirror fallback")
    require_text(_p(ctx, "main_fan_runtime.cpp"),
                 "const int driven = fan_runtime_driven_mode(g_app.fanCurveRuntimeActive,",
                 "the service's activeFanMode follows its own runtime, never the outgoing intent")
    apply_runtime_cpp = _p(ctx, "main_service_apply_runtime.cpp")
    require_order(apply_runtime_cpp, "g_serviceActiveDesired = mergedActiveDesired;",
                  'service_enforce_fan_runtime_matches_intent("apply"',
                  "every successful Apply checks the fan runtime against the published intent")
    require_order(apply_runtime_cpp, 'service_enforce_fan_runtime_matches_intent("apply"',
                  "populate_control_state(&g_serviceControlState);\n"
                  "        g_serviceControlStateValid = true;\n"
                  "        mark_service_telemetry_cache_updated(\"service apply\");",
                  "the control state is captured after the fan invariant is enforced")
    require_text(_p(ctx, "main_service_runtime.cpp"), '#include "main_service_fan_intent.cpp"',
                 "the fan invariant shard is compiled into the service")
    require_text(_p(ctx, "profile_ownership_policy.h"), "profileFanMode == FAN_MODE_AUTO;",
                 "only an Auto profile may match an unclaimed fan")
    require_text(_p(ctx, "linux_daemon_snapshot_runtime.cpp"),
                 "s->activeFanMode = fan_owned_intent_mode(",
                 "the Linux snapshot never reports a stale fan mode for an unclaimed fan")


def check_fan_fault_reporting(ctx, require_text, forbid_text, require_order):
    """A fan the runtime hands back is reported, and no longer claimed.

    The Windows service used to hand a failing fan to the driver silently: the
    owned intent kept the curve, so the GUI and the tray went on showing it.
    Both platforms now publish the protocol v39 ServiceFanFault record, the
    service's intent stops claiming a fan the driver holds, and every client
    announces the record (fan_fault_notice_policy.h).  Executed by
    tests/fan_handback_runtime_tests.cpp and tests/fan_runtime_followup_tests.cpp.
    """
    failure_cpp = _p(ctx, "main_fan_runtime_failure.cpp")
    require_text(failure_cpp,
                 "if (app_is_service_process()) service_fan_runtime_handback_publish(&fault);",
                 "every service handback publishes its record and updates the owned intent")
    require_order(failure_cpp, "stop_fan_curve_runtime();",
                  "service_fan_runtime_handback_publish(&fault);",
                  "the record follows the handback it describes")
    fault_cpp = _p(ctx, "main_service_fan_fault.cpp")
    require_text(fault_cpp, "g_serviceActiveDesired.hasFan = 0;",
                 "the service stops claiming a fan the driver took back")
    require_text(fault_cpp, "g_serviceActiveDesired.fanPercent = FAN_RUNTIME_EMERGENCY_PERCENT;",
                 "a refused handback owns the emergency duty, never the failed curve")
    require_text(fault_cpp, "service_clear_restart_reapply_snapshot();",
                 "releasing the last domain on handback clears the restart reapply snapshot")
    require_text(fault_cpp, "service_write_restart_reapply_snapshot();",
                 "handback with remaining owned domains refreshes the restart reapply snapshot")
    require_text(fault_cpp, "g_serviceActiveProfileSource = SERVICE_PROFILE_SOURCE_AD_HOC;",
                 "handback drops stale saved-slot attribution when owned domains remain")
    require_text(fault_cpp, "memset(&g_serviceActiveDesiredGpu, 0, sizeof(g_serviceActiveDesiredGpu));",
                 "handback clears the owner GPU when no owned domains remain")
    require_text(_p(ctx, "main_shell.cpp"), '#include "main_service_fan_fault.cpp"',
                 "the fan fault shard is compiled")
    require_text(_p(ctx, "main_state_sync.cpp"),
                 "snapshot->fanFault = service_fan_fault_for_snapshot_locked();",
                 "the Windows snapshot carries the fan fault record")
    require_text(_p(ctx, "main_state_sync.cpp"), "g_app.serviceFanFault = snapshot->fanFault;",
                 "the GUI adopts the fan fault record")
    require_order(_p(ctx, "gui_service_state.cpp"), "gui_announce_auto_restore_lockout();",
                  "gui_announce_fan_fault();",
                  "the fan fault notice has the last word on the status line")
    apply_runtime_cpp = _p(ctx, "main_service_apply_runtime.cpp")
    require_text(apply_runtime_cpp,
                 "if (fan_fault_should_clear_after_apply(desired && desired->hasFan,\n"
                 "                fanInvariant == SERVICE_FAN_INTENT_REPAIR_FAILED)) {",
                 "only an Apply whose fan invariant held or repaired clears the fan fault record")
    require_text(apply_runtime_cpp,
                 'service_fan_fault_clear("an Apply that set the fan");',
                 "a healthy fan Apply clears the fan fault record")
    require_text(apply_runtime_cpp,
                 'service_fan_fault_clear("the standby restore\'s fan stash replay", false);',
                 "a standby stash replay clears the fault record without dropping the stash early")
    require_text(apply_runtime_cpp, 'service_fan_fault_clear("a successful Reset");',
                 "a Reset clears the record")
    validation_h = _p(ctx, "service_protocol_validation.h")
    require_text(validation_h,
                 "if (!service_fan_fault_wire_valid(&s->fanFault)) s->fanFault = ServiceFanFault{};",
                 "the canonicalizer voids a malformed fan fault record")
    require_text(validation_h, "!service_fan_fault_wire_valid(&snapshot->fanFault)) return false;",
                 "the strict envelope check refuses a malformed fan fault record")
    require_text(validation_h, "if (!service_fan_fault_is_zero(snapshot.fanFault)) return false;",
                 "a stateless refusal carries no fan fault")
    require_text(_p(ctx, "linux_daemon_snapshot_runtime.cpp"), "s->fanFault = ctl.fanFault;",
                 "the Linux snapshot carries the fan fault record")
    require_text(_p(ctx, "linux_daemon_snapshot_runtime.cpp"),
                 "linux_daemon_fan_runtime_active(ctl.gpuReady,",
                 "the Linux snapshot derives fan runtime activity with the worker's own predicate")
    require_text(_p(ctx, "linux_fan_runtime.h"), "ctl.activeDesired.hasFan = 0;",
                 "a Linux driver-auto fan lockout stops claiming the released fan")
    require_text(_p(ctx, "linux_fan_runtime.h"),
                 "ctl.activeDesired.fanPercent = FAN_RUNTIME_EMERGENCY_PERCENT;",
                 "a Linux refused fan lockout owns the emergency duty, never the failed curve")
    require_text(_p(ctx, "linux_daemon.cpp"),
                 'if (req->desired.hasFan) daemon_fan_runtime_note_commit(ctl, "a committed Apply");',
                 "only a Linux Apply that sets the fan clears the fan fault record")
    require_text(_p(ctx, "linux_daemon.cpp"),
                 'daemon_fan_runtime_note_commit(ctl, "a committed Reset");',
                 "a committed Linux Reset clears the fan fault record")
    require_text(_p(ctx, "linux_fan_runtime.h"), "    daemon_fan_fault_record(ctl, kind,",
                 "every Linux worker lockout records the fan fault")
    require_text(_p(ctx, "linux_tui_refresh.cpp"),
                 "gc_fan_fault_observe(&state->fanFaultObserver, &next.snapshot.fanFault)",
                 "the TUI announces each fan fault once")
    require_text(_p(ctx, "linux_live_output.cpp"), '"Fan fault: %s' + chr(92) + 'n"',
                 "the Linux status output names a fan fault")


def check_standby_fan_stash(ctx, require_text, forbid_text, require_order):
    """A fan handback during standby does not cost the restore its fan.

    The Windows fan worker keeps running across suspend.  A handback between
    suspend and the standby restore's write rewrites the owned intent to driver
    auto, which made that restore skip the fan.  The handback now keeps the
    intent it replaced (service_standby_fan_stash_policy.h), tagged with the
    reducer's standby stash epoch, and the next standby restore replays it
    once.  Executed by tests/standby_fan_stash_tests.cpp and
    tests/fan_handback_runtime_tests.cpp (14340-14366).
    """
    fault_cpp = _p(ctx, "main_service_fan_fault.cpp")
    require_order(fault_cpp,
                  "service_lifecycle_standby_fan_handback_window(&standbyEpoch);",
                  "g_serviceActiveDesired.hasFan = 0;",
                  "the handback reads the standby window before it rewrites the owned intent")
    require_order(fault_cpp, "g_serviceFanFault = ServiceFanFault{};",
                  "service_standby_fan_stash_clear(reason);",
                  "a Reset or a deliberate fan Apply supersedes a pending standby replay")
    require_text(fault_cpp, "fault->kind == SERVICE_FAN_FAULT_REPEATED_FAILURES;",
                 "only repeated-failure handbacks keep or capture a standby stash")
    require_text(fault_cpp,
                 "service_standby_fan_stash_use(&g_serviceStandbyFanStash, standbyEpoch,",
                 "keeping an existing stash checks the epoch and unchanged pre-handback intent")
    require_text(_p(ctx, "service_standby_fan_stash_policy.h"),
                 "faultKind != SERVICE_FAN_FAULT_REPEATED_FAILURES",
                 "confirmed fan stalls are excluded from Windows standby stash capture")
    require_text(_p(ctx, "main_service_lifecycle_events.cpp"),
                 "service_standby_fan_handback_window(\n        &g_serviceLifecycleState, epochOut);",
                 "the window comes from the real lifecycle reducer state")
    apply_cpp = _p(ctx, "main_service_lifecycle_apply.cpp")
    require_order(apply_cpp, "const gc_u64 stashEpoch = g_serviceLifecycleState.standbyStashEpoch;",
                  "const bool replayFanStash = service_standby_fan_stash_peek(",
                  "the standby restore judges the stash against the reducer's current epoch")
    require_order(apply_cpp, "const bool replayFanStash = service_standby_fan_stash_peek(",
                  "if (!service_standby_restore_select(replayFanStash, &fanStash,",
                  "the standby restore decides on the stash before choosing what to restore")
    require_order(apply_cpp, "if (!service_standby_restore_select(replayFanStash, &fanStash,",
                  "service_build_full_restore_request(restoreIntent, &desired);",
                  "the restore request is built from the selected intent")
    require_order(apply_cpp, "&applySeverity, replayFanStash);",
                  "if (service_standby_fan_stash_used_up(replayFanStash, success, writeAttempted))",
                  "the stash is used up only by a restore that reached the hardware")
    require_order(apply_cpp, "if (service_standby_fan_stash_should_restore_profile(",
                  "g_serviceActiveProfileSlot = fanStash.replacedProfileSlot;",
                  "a replayed profile's attribution is restored only when outcome severity is not error")
    # The restart snapshot write follows the proof commit in the same block.
    require_order(apply_cpp, "g_serviceActiveProfileSlot = fanStash.replacedProfileSlot;",
                  "proofCommitOk = service_restore_mature_oc_apply_proof(",
                  "a replayed profile's attribution is restored before the proof and restart snapshot")


def check_linux_standby_fan_stash(ctx, require_text, forbid_text, require_order):
    """A Linux fan lockout after a suspend does not cost the resume restore.

    The Linux daemon has no suspend edge; the window is a grown suspended-time
    offset (CLOCK_BOOTTIME - CLOCK_MONOTONIC) since the last resume restore.  A
    lockout inside it keeps the intent it replaced, and the resume restore
    replays it once with only that lockout's uncertainty waived
    (linux_standby_fan_stash_policy.h).  Executed by
    tests/linux_standby_fan_stash_tests.cpp,
    tests/fan_runtime_followup_tests.cpp and
    tests/linux_daemon_commit_rollback_tests.cpp.
    """
    runtime_h = _p(ctx, "linux_fan_runtime.h")
    require_order(runtime_h, "daemon_enter_uncertain_after_fan_lockout(ctl, why, autoOk);",
                  "daemon_standby_fan_stash_note_lockout(ctl, kind, hadIntent, &previousDesired,",
                  "the lockout notes the fault kind and stash after it entered uncertainty (serial bumped)")
    ownership_h = _p(ctx, "linux_fan_ownership.h")
    for anchor in ("    ctl.stateUncertain = true;\n    ++ctl.uncertainSerial;\n    daemon_fan_ownership_release_for_uncertain",
                   "    ctl.stateUncertain = true;\n    ++ctl.uncertainSerial;\n    if (!ctl.fanOwnershipMarkerCommitted) return;"):
        require_text(ownership_h, anchor,
                     "every entry into uncertainty bumps the serial a stash is tagged with")
    stash_h = _p(ctx, "linux_standby_fan_stash.h")
    require_text(stash_h, "clock_gettime(CLOCK_BOOTTIME, &boot)",
                 "the window is the kernel's suspended-time offset, not a timer")
    require_text(_p(ctx, "linux_standby_fan_stash_policy.h"),
                 "faultKind != SERVICE_FAN_FAULT_REPEATED_FAILURES",
                 "confirmed fan stalls are excluded from Linux standby stash capture")
    daemon_cpp = _p(ctx, "linux_daemon.cpp")
    require_text(daemon_cpp, 'daemon_standby_fan_stash_clear(ctl, "an explicit Apply");',
                 "an explicit Apply supersedes a pending resume replay")
    require_text(daemon_cpp, 'daemon_standby_window_close(ctl, "an explicit Apply");',
                 "an explicit Apply closes the suspend window")
    require_text(daemon_cpp, 'daemon_standby_fan_stash_clear(ctl, "an explicit Reset");',
                 "an explicit Reset supersedes a pending resume replay")
    require_text(daemon_cpp, 'daemon_standby_window_close(ctl, "an explicit Reset");',
                 "an explicit Reset closes the suspend window")
    require_order(daemon_cpp, 'daemon_standby_window_close(ctl, "daemon start");',
                  "pl_thread_start(&fanThread, fan_reassert_thread, &ctl);",
                  "the window baseline exists before the fan worker can lock out")
    restore_h = _p(ctx, "linux_auto_restore_runtime.h")
    require_order(restore_h, 'daemon_standby_window_close(ctl, "the resume restore");',
                  "const bool replay = daemon_standby_fan_stash_peek(ctl) == LINUX_STANDBY_FAN_STASH_REPLAY;",
                  "the resume restore closes the window before it decides on the stash")
    require_text(restore_h, "if (!auto_restore_authorize(ctl, trigger, uncertainAtStart, &verdict)) {",
                 "the guard decision takes the stash-aware uncertainty")
    require_text(restore_h, "&ctl.autoRestoreGuard, uncertainAfterPrepare,",
                 "the post-preparation recheck re-evaluates the stash-aware uncertainty")
    require_order(restore_h, "LINUX_AUTO_RESTORE_TRIGGER_RESUME, &target, &intent, role, &uuid,",
                  "if (linux_standby_fan_stash_used_up(replay, outcome.success, outcome.attempted)) {",
                  "the stash is used up only by a restore that reached the hardware")
    forbid_text(restore_h, "ctl.stateUncertain, nowMs);",
                "the guard decision must not bypass the stash-aware uncertainty helper")


def check_fan_stall_detection(ctx, require_text, forbid_text, require_order):
    """A commanded fan that does not turn is handed back to the driver.

    fan_stall_policy.h decides from readings the runtime already takes; both
    platforms run it after every runtime tick and escalate a confirmed stall
    through their existing handback.  Executed by tests/fan_stall_tests.cpp,
    tests/fan_handback_runtime_tests.cpp and tests/fan_runtime_followup_tests.cpp.
    """
    runtime_cpp = _p(ctx, "main_fan_runtime.cpp")
    require_text(runtime_cpp, '#include "main_fan_stall.cpp"',
                 "the Windows stall check is compiled into the fan runtime")
    require_order(runtime_cpp, "    apply_fan_curve_tick_once();",
                  "    fan_stall_check_after_tick(readSerialBefore);",
                  "every Windows runtime tick ends with the stall check")
    nvml_cpp = _p(ctx, "main_runtime_nvml_fans.cpp")
    for flag in ("g_app.readback.fan.rpm[fan] = true;", "g_app.readback.fan.measured[fan] = true;",
                 "g_app.fanReadSerial++;"):
        require_text(nvml_cpp, flag, "the fan read records which sensors answered")
    require_text(_p(ctx, "main_shell.cpp"), "g_app.fanRuntimeGeneration++;",
                 "a runtime start is a new command for the stall check")
    stall_cpp = _p(ctx, "main_fan_stall.cpp")
    require_text(stall_cpp, "cause.kind = SERVICE_FAN_FAULT_STALL;",
                 "a confirmed Windows stall is reported as a stall")
    require_text(stall_cpp, "escalate_fan_runtime_handback(&cause);",
                 "a confirmed Windows stall uses the shared handback")
    require_text(stall_cpp,
                 "(curve && !g_app.fanCurveDriverAutoActive && g_app.fanCurveHasLastAppliedTemp);",
                 "a zero-RPM curve's driver-automatic phase commands no manual duty")
    policy_h = _p(ctx, "fan_stall_policy.h")
    require_text(policy_h, "if (!s->policyKnown || !s->policyManual) return FAN_STALL_FAN_NOT_JUDGED;",
                 "only a fan the driver reports under manual policy is judged")
    require_text(policy_h, "const bool rpmUsable = s->rpmKnown && rpmProven;",
                 "an unproven tach never judges a fan")
    require_text(_p(ctx, "linux_backend.cpp"), '"nvmlDeviceGetFanSpeedRPM"',
                 "the Linux backend loads the tach getter for the stall check")
    require_text(_p(ctx, "linux_backend_fan_control.h"),
                 "s.policyManual = s.policyKnown && value == NVML_FAN_POLICY_MANUAL;",
                 "the Linux stall sample reports the driver's own fan policy")
    linux_runtime_h = _p(ctx, "linux_fan_runtime.h")
    require_text(linux_runtime_h,
                 "daemon_fan_stall_check(ctl, &stall, &stallLog, observedGeneration, true,",
                 "the Linux fixed-duty poll runs the stall check, keyed to the wake generation")
    require_text(linux_runtime_h,
                 "daemon_fan_stall_check(ctl, &stall, &stallLog, observedGeneration,\n"
                 "                                       manualCommanded, pct);",
                 "the Linux curve poll runs the stall check, keyed to the wake generation")
    require_text(linux_runtime_h, "manualCommanded = !decision.useDriverAuto;",
                 "a Linux zero-RPM curve's driver-automatic phase commands no manual duty")
    require_text(linux_runtime_h,
                 "daemon_fan_runtime_lockout(ctl, SERVICE_FAN_FAULT_STALL, summary, r.fanIndex,",
                 "a confirmed Linux stall locks the worker out and is reported as a stall")


def check_all(ctx, require_text, forbid_text, require_order, backend_surface):
    check_fanless_request_rule(ctx, require_text, forbid_text, require_order)
    check_fan_intent_consistency(ctx, require_text, forbid_text, require_order)
    check_fan_fault_reporting(ctx, require_text, forbid_text, require_order)
    check_standby_fan_stash(ctx, require_text, forbid_text, require_order)
    check_linux_standby_fan_stash(ctx, require_text, forbid_text, require_order)
    check_fan_stall_detection(ctx, require_text, forbid_text, require_order)
    reset_fan_h = _p(ctx, "main_service_reset_fan.h")
    require_order(reset_fan_h,
                  "if (!nvml_set_fan_auto(detail, detailSize)",
                  "stop_fan_curve_runtime();",
                  "Reset verifies the driver handoff before stopping cooling")
    require_text(reset_fan_h, "app_fan_control_known_absent()",
                 "Reset explicitly skips fan handback on fanless GPUs")
    require_text(reset_fan_h, "service_owned_fan_is_non_auto()",
                 "Reset and shutdown never skip owned non-Auto (including emergency) fan intent")
    require_text(reset_fan_h, "live_fan_policy_readback_complete() && g_app.fanIsAuto",
                 "Reset only short-circuits Auto handback on complete positive policy readback")
    require_text(reset_fan_h, "!live_fan_policy_readback_unsupported()",
                 "Reset accepts Auto handback on older drivers without policy readback when all writes succeeded")
    require_text(_p(ctx, "main_runtime_nvml_fans.cpp"), "live_fan_policy_readback_unsupported()",
                 "nvml_set_fan_auto preserves Auto support on drivers without getFanControlPolicy_v2")
    require_text(_p(ctx, "main_service_apply_runtime.cpp"),
                 "if (retainFan) service_reset_retain_running_fan();",
                 "Reset keeps only live fan ownership when its handoff fails")
    require_text(_p(ctx, "main_service_apply_runtime.cpp"),
                 "service_reset_retain_owned_fan(",
                 "Reset preserves unreturned emergency manual fan ownership when Auto fails")
    require_text(_p(ctx, "main_service_pipe_switch.cpp"),
                 '"explicit Reset hardware write did not complete",',
                 "Reset lockout keeps the independent live fan runtime")
    check_manual_write_verification(ctx, require_text, forbid_text, backend_surface)
    check_service_fan_worker_serialization(ctx, require_text, forbid_text)
    check_runtime_failsafe(ctx, require_text, forbid_text, require_order)
    check_power_limit_range(ctx, require_text, forbid_text)
    check_native_zero_rpm(ctx, require_text, forbid_text)
