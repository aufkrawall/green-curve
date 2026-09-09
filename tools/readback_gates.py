"""Source gates for the protocol-v14 hardware-readback provenance contract.

Split out of build.py so the build script stays under its size ratchet; build.py
owns the CLI and passes its paths and check helpers in through `ctx`.  Nothing
here imports build.py, so the dependency runs one way only.

Every rule in here guards a NEGATIVE: a published value that silently stops
carrying its provenance produces no build error, no crash and no log line -- it
produces a client that reports a confident match against a number the driver
never gave it.  The power-domain rules are the sharpest case: a fabricated 0%
power target reads as a real -100% request, and cost an RTX 3060 Laptop GPU
owner every Apply until it was found (reported 2026-09-07 against 0.25.0).

`ctx` is any object exposing SOURCE_DIR.
"""
import os


def _p(ctx, name):
    return os.path.join(ctx.SOURCE_DIR, name)


def check_all(ctx, require_text, forbid_text):
    # The v14 readback-validity contract has a producer on BOTH platforms. The
    # Windows service publishing all-zero bits would silently report every
    # domain as unavailable while still substituting intent for failed reads.
    state_sync_cpp = _p(ctx, "main_state_sync.cpp")
    gpu_state_cpp = _p(ctx, "main_gpu_state.cpp")
    gpu_backend_cpp_path = _p(ctx, "gpu_backend.cpp")
    readback_policy_h = _p(ctx, "control_readback_policy.h")
    require_text(state_sync_cpp, "apply_control_readback_validity(state, &facts);",
                 "the Windows service publishes per-domain readback validity")
    require_text(state_sync_cpp, "facts.gpuOffsetFromHardware = gpuOffsetFromHardware;",
                 "the Windows service publishes GPU offset readback provenance")
    require_text(state_sync_cpp, "facts.fanPolicyKnown = g_app.readback.fan.policy;",
                 "the Windows service publishes per-fan readback provenance")
    require_text(state_sync_cpp,
                 "merged.gpuOffsetReadbackValid = state->gpuOffsetReadbackValid;",
                 "the Windows GUI merge carries readback validity with its value")
    require_text(gpu_state_cpp,
                 "static int current_applied_gpu_offset_mhz(bool* fromHardware)",
                 "the Windows applied GPU offset reports whether it is a reading")
    require_text(gpu_state_cpp, "*fromHardware = false;  // remembered request",
                 "the persisted selective request is never reported as readback")
    require_text(gpu_state_cpp, "*fromHardware = false;  // active desired intent",
                 "the active-desired fallback is never reported as readback")
    require_text(gpu_backend_cpp_path,
                 "g_app.readback.gpuOffset = gpu_offset_readback_after_detection(",
                 "clock-offset detection owns the GPU scalar it overwrites")
    # The power read/write pair lives in its own shard; see gpu_backend_power.cpp.
    gpu_backend_power_cpp = _p(ctx, "gpu_backend_power.cpp")
    require_text(gpu_backend_power_cpp, "g_app.readback.powerLimit = true;",
                 "a Windows power reading records its own provenance")
    # A board whose driver refuses the power target must publish the neutral
    # board-default percentage, never 0: a fabricated 0% reads as a real -100%
    # request and made reset-before-apply issue a write the driver could only
    # refuse, failing the whole Apply.  Both unknown exits are gated.
    require_text(gpu_backend_power_cpp,
                 "g_app.powerLimitPct = POWER_LIMIT_DEFAULT_PCT;",
                 "an unreadable power target publishes the board default, not 0%")
    require_text(_p(ctx, "gpu_backend_reset_baseline.cpp"),
                 "power_reset_before_apply_required(",
                 "reset-before-apply never writes power on a board with no power "
                 "control surface")
    require_text(_p(ctx, "gpu_backend_apply.cpp"),
                 "invalidate_scalar_readbacks(&g_app.readback);",
                 "a rollback drops readback validity with the scalars it zeroes")
    require_text(readback_policy_h, "all_fans_known",
                 "a partially answering fan set is not a readback")
    # Cross-platform parity for the power control surface.  Linux refused the
    # whole apply -- VF curve included -- when an owned power request met a
    # board that exposes no power target, and a saved profile always carries
    # the mandatory power_limit_pct key, so every profile apply was blocked by
    # a domain the user never touched.  All three gates in front of the write
    # must consult the same inertness rule.
    linux_mutation_cpp = _p(ctx, "linux_backend_mutation.cpp")
    require_text(linux_mutation_cpp, "static bool linux_power_request_is_inert(",
                 "Linux names the inert-power-request rule once")
    require_text(linux_mutation_cpp,
                 "unavailableDomains &= ~(gc_u32)SERVICE_MUTATION_DOMAIN_POWER;",
                 "an inert power request is not an unavailable Linux domain")
    require_text(linux_mutation_cpp,
                 "if (d->hasPowerLimit && !powerInert &&",
                 "the Linux power preflight exempts an inert power request")
    require_text(linux_mutation_cpp,
                 "if (d->hasPowerLimit && !linux_power_request_is_inert(d, &snapshot, g))",
                 "an inert power request schedules no Linux power write")


