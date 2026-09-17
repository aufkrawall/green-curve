"""Source gates for F-PERSIST-SCHEMA -- the on-disk DesiredSettings layouts.

Split out of build.py so the build script stays under its size ratchet; build.py
owns the CLI and passes its check helpers in through `ctx`. Nothing here imports
build.py, so the dependency runs one way only.

What these rules protect is a class of bug that is silent by construction.
Three files embed DesiredSettings as raw bytes -- the Linux daemon state record,
the Linux daemon startup-policy record and the Windows controlled-restart
snapshot -- and each carries a version number that nobody bumped while the
struct grew underneath it. The loaders then admitted a file only when its size
equalled the CURRENT struct, so:

  * every record an older build wrote was classified corrupt and deleted, and
  * every backward-compatibility branch ever written to migrate an older
    generation was declared over a struct that had since changed, making it
    unreachable code that could never once have fired.

0.26.0 is where that became visible: adding curvePointFromGpuOffset[] took
DesiredSettings from 836 to 964 bytes, which on a 0.25.2 Linux upgrade discards
the restore-last intent and downgrades the startup policy to "apply nothing" --
fail-safe, but the administrator is never told, and the Arch package restarts a
running daemon during post_upgrade, so it happens immediately.

The fix is a rule, not a patch: SIZE selects the LAYOUT, VERSION selects the
SEMANTICS within it. These gates keep the pieces of that rule from being quietly
removed -- the frozen layouts, their exact byte sizes, the widening migrations,
and the static_asserts that turn the next field addition into a build break
instead of an upgrade regression.

`ctx` is any object exposing SOURCE_DIR.
"""
import os


def _p(ctx, name):
    return os.path.join(ctx.SOURCE_DIR, name)


def check_all(ctx, require_text, forbid_text):
    schema_h = _p(ctx, "desired_settings_schema.h")
    daemon_state_h = _p(ctx, "linux_daemon_state.h")
    daemon_state_cpp = _p(ctx, "linux_daemon_state.cpp")
    protocol_h = _p(ctx, "service_protocol.h")

    # The frozen layout and its exact size. 836 is not a measurement, it is the
    # contract with every 0.25.2 file still on disk.
    require_text(schema_h, "struct DesiredSettingsSchema1",
                 "the outgoing DesiredSettings layout is frozen, not re-derived")
    require_text(schema_h, "sizeof(DesiredSettingsSchema1) == 836",
                 "the frozen 0.25.2 DesiredSettings layout stays 836 bytes")
    require_text(schema_h, "desired_settings_widen_from_schema1",
                 "a frozen record widens into the live struct field by field")
    require_text(schema_h, "desired_settings_narrow_to_schema1",
                 "the harness can build byte-exact schema-1 fixtures")

    # The live struct's assert must keep naming the persistence bumps. Without
    # that sentence the next person bumps only the protocol version, which is
    # exactly what happened here.
    require_text(protocol_h, "LINUX_DAEMON_RECORD_VERSION, LINUX_DAEMON_STARTUP_VERSION and",
                 "the DesiredSettings size assert names the persisted records")

    # Every record that embeds DesiredSettings pins its own size, so a change to
    # DesiredSettings *or* GpuAdapterInfo breaks the build at the record.
    require_text(daemon_state_h, "sizeof(LinuxDaemonStateRecord) == 1176",
                 "the live daemon state record pins its on-disk size")
    require_text(daemon_state_h, "sizeof(LinuxDaemonStartupRecord) == 1236",
                 "the live startup-policy record pins its on-disk size")
    require_text(daemon_state_h, "sizeof(LinuxDaemonStateRecordSchema1) == 1048",
                 "the frozen schema-1 state record stays 1048 bytes")
    require_text(daemon_state_h, "sizeof(LinuxDaemonStateRecordSchema1V1) == 1036",
                 "the frozen pre-operation-id state record stays 1036 bytes")
    require_text(daemon_state_h, "sizeof(LinuxDaemonStartupRecordSchema1) == 1108",
                 "the frozen schema-1 startup record stays 1108 bytes")
    require_text(daemon_state_h,
                 "daemon state record layouts must be distinguishable by size",
                 "size-based dispatch is only sound while the sizes differ")

    # A legacy struct declared over the LIVE DesiredSettings is the original
    # defect: it compiles, it looks like a migration, and it can never match a
    # real file. The frozen layouts must embed the frozen struct.
    forbid_text(daemon_state_h, "struct LinuxDaemonStateRecordV1",
                "the dead v1 record declared over the live struct is gone")
    require_text(daemon_state_h, "DesiredSettingsSchema1 desired;",
                 "frozen records embed the frozen DesiredSettings layout")

    # The loaders dispatch on size and migrate by version.
    require_text(daemon_state_cpp, "sizeof(LinuxDaemonStateRecordSchema1)",
                 "the state loader admits the 0.25.2 record size")
    require_text(daemon_state_cpp, "sizeof(LinuxDaemonStartupRecordSchema1)",
                 "the startup loader admits the 0.25.2 record size")
    require_text(daemon_state_cpp, "matches no known record",
                 "a rejected record says what its size was and what was expected")
    require_text(daemon_state_h, "linux_daemon_state_record_widen_schema1",
                 "a schema-1 state record is widened rather than deleted")
    require_text(daemon_state_h, "linux_daemon_startup_widen_schema1",
                 "a schema-1 startup policy is widened rather than declared corrupt")
    # The value migration rides on the version, never on the size.
    require_text(daemon_state_h,
                 "in->version <= LINUX_DAEMON_RECORD_PRE_DISPLAY_MEM_UNITS_VERSION",
                 "pre-parity memory offsets are halved by version, once")


def check_profile_curve_format(ctx, require_text, forbid_text):
    """F-CURVE-PROVENANCE: a saved point keeps what its number IS.

    `base_plus_gpu_offset` was a whole-section statement, so saving a profile
    that mixed one hand-typed point with projected neighbours flattened the
    difference -- and the loader then marked every restored point offset-derived,
    quietly handing a number the user typed back to a stock base that moves with
    load. Both platforms now write absolute MHz plus a per-point origin flag.
    """
    semantics_h = _p(ctx, "profile_curve_semantics.h")
    windows_format_cpp = _p(ctx, "config_profile_curve_format.cpp")
    origin_io_h = _p(ctx, "profile_curve_origin_io.h")
    linux_codec_h = _p(ctx, "linux_profile_curve_codec.h")
    config_profiles_cpp = _p(ctx, "config_profiles.cpp")
    linux_profiles_cpp = _p(ctx, "linux_port_profiles.cpp")
    capture_cpp = _p(ctx, "main_runtime_capture.cpp")
    shell_cpp = _p(ctx, "main_shell.cpp")

    require_text(semantics_h, "PROFILE_CURVE_SEMANTICS_ABSOLUTE_WITH_ORIGIN",
                 "the saved-curve format is named in one shared place")
    require_text(semantics_h, "PROFILE_CURVE_POINT_ORIGIN_SUFFIX",
                 "per-point provenance has one key spelling for both platforms")

    # Nothing may write the lossy whole-section format again. Reading it stays,
    # because profiles written by older builds are still on disk.
    for path in (config_profiles_cpp, linux_codec_h, capture_cpp):
        forbid_text(path, 'curve_semantics=base_plus_gpu_offset',
                    "no save path writes the flattening whole-section format")
    require_text(origin_io_h, "restore_curve_point_origins_from_section",
                 "the Windows loader restores per-point provenance")
    require_text(semantics_h, "profile_curve_decode_from_marker",
                 "one shared function decides what a curve_semantics marker means")
    require_text(semantics_h, "return PROFILE_CURVE_DECODE_ABSOLUTE;",
                 "an unknown marker reads as absolute, never as base+offset")
    require_text(linux_codec_h, "linux_profile_read_curve_point_origins",
                 "the Linux loader restores per-point provenance")

    # Provenance is restored BEFORE the legacy reconstruction, and the legacy
    # path only runs when the section is not in the new format -- otherwise a
    # new file would be re-flattened by the old code path.
    require_text(config_profiles_cpp,
                 "!restore_curve_point_origins_from_section(path, curveSection, desired) &&",
                 "the slot loader prefers per-point provenance over the legacy path")
    require_text(shell_cpp,
                 '!restore_curve_point_origins_from_section(path, "curve", desired) &&',
                 "the global [curve] loader prefers per-point provenance too")
    require_text(linux_profiles_cpp,
                 "curveDecode == PROFILE_CURVE_DECODE_BASE_PLUS_GPU_OFFSET",
                 "the Linux legacy reconstruction runs only for the legacy marker")
    require_text(linux_profiles_cpp,
                 "curveDecode == PROFILE_CURVE_DECODE_UNMARKED",
                 "the Linux pre-marker rule is keyed to the shared decode answer")

    # All three Windows writers go through the one record builder.
    require_text(windows_format_cpp, "profile_curve_point_record_for_save",
                 "one place decides what a saved curve point contains")
    for path in (config_profiles_cpp, capture_cpp):
        require_text(path, "profile_curve_point_record_for_save(desired, i,",
                     "every Windows curve writer uses the shared record builder")
