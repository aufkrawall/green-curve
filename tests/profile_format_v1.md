# Portable profile format, version 1

Contract for the shared codec and its regression/fuzz fixtures. Source anchors:
`source/profile_document.h`, `source/profile_document_validation.h`,
`source/profile_point_intent.h`, `source/profile_text_validation.h`,
`tests/profile_document_contract_tests.cpp`.

## Framing and scalar syntax

Input is at most 1 MiB, with lines at most 4096 bytes excluding LF/CRLF. Accept
LF, CRLF and one optional leading UTF-8 BOM. Reject invalid UTF-8, NUL, DEL,
isolated CR and ASCII controls other than framing whitespace. A quoted text
value cannot contain ASCII controls. The writer emits UTF-8/LF without a BOM.

Keys, sections and enums are case-sensitive. Permit surrounding ASCII spaces
and tabs around sections, keys and `=`; trim scalar values. Blank lines and
full-line `#` or `;` comments are allowed. Inline comments, includes, expansion,
unit suffixes and floating-point numbers are unsupported. Decimal integers
permit a leading minus only for signed fields, and leading zeros in values;
reject `+`, overflow and missing digits. Section indices use canonical decimal
without leading zeros. Booleans are exactly `0` or `1`.

Text values are double-quoted UTF-8; the only escapes are `\\` and `\"`.
Byte limits below exclude the terminator. Unknown keys/sections, duplicate
keys/sections, unsupported versions and inapplicable authoritative payloads
are errors, including an inapplicable payload whose numeric value is zero.
There is no arbitrary extension namespace. Optional observations may be omitted;
they never supply a missing authoritative value.

`N` is the record ordinal 1..5; `I` is a zero-based bounded record index.
All required parent/count relationships are validated after parsing, so section
and key order do not affect meaning. The writer orders records numerically and
keys in the order below. Parsing and encoding use temporary results and publish
only after complete validation; neither operation consults a GPU or writes files.

## Header and document

| Section | Keys, in writer order | Required meaning and bounds |
| --- | --- | --- |
| `[green_curve_profile]` | `format`, `major_version`, `profile_count` | All required: `green_curve_profiles`, `1`, and integer 0..5. No profile ordinal may exceed the declared count. |
| `[profile.N]` | `document_id`, `domains`, `application_scope`, `point_set_policy`, `topology_complete`, `legacy_indices_unresolved`, `topology_count`, `point_count`, `display_name` | All except name required. ID is 1..4294967295, unique within the file; it never selects a destination slot. Counts are 0..128 and must match complete record sets. Name is at most 128 UTF-8 bytes. |

`domains` is `none` or a comma-separated set of `vf`, `graphics`, `memory`,
`power`, `lock`, `fan`, `xbar`, `sys_clock`, `video_clock`. Whitespace around
tokens is allowed; empty or repeated tokens are errors. Writer order is the
listed order. Domain declarations must match included controls. For
`explicit_release`, domains may select release without new value sections.
An explicitly included empty VF point set is permitted: `merge_points` writes
no point; `replace_points` clears only the prior owned point set.

Scopes are `update_included`, `replace_profile`, `explicit_release`. Point-set
policies are `merge_points`, `replace_points`. The two are independent.
`topology_complete` and `legacy_indices_unresolved` are booleans and cannot
both be true. Local bank/export adapters may assign new file-local document
IDs on a temporary copy; IDs confer no overwrite or Apply authority.

## Source metadata

`[profile.N.source]` is optional. Every key is optional and defaults to zero,
unknown or empty; writer emits its complete canonical metadata when present.
These fields are provenance and cannot authorize a destination GPU.

| Key | Meaning |
| --- | --- |
| `os` | `unknown`, `windows`, `linux` |
| `backend_family` | Diagnostic integer 0..2147483647; never a dispatch allowlist |
| `device_id`, `subsystem_id` | Unsigned 32-bit numeric compatibility metadata |
| `driver_version` | Optional quoted text, at most 64 UTF-8 bytes |
| `capture_complete` | Boolean capture-completeness report |
| `captured_domains`, `omitted_domains`, `unavailable_domains` | Domain sets using the same tokens as `domains`; overlaps can describe partial capture of a domain |
| `pin_knowledge` | `unknown`, `absent`, `present`; missing getter evidence means unknown |

## Topology and points

Every topology index 0..`topology_count-1` must occur exactly once. Every point
record index 0..`point_count-1` must occur exactly once; its topology reference
is independent of that ordinal. Duplicate point references are invalid.

| Section | Keys, in writer order | Meaning |
| --- | --- | --- |
| `[profile.N.topology.I]` | `topology_id`, `source_index`, `voltage_uv`, `source_writable` | First three required. ID is a unique nonnegative signed-32-bit identity within this document. Index is -1 (unknown) or 0..127 and is diagnostic until complete topology evidence validates it. Voltage is positive signed-64-bit microvolts; for unresolved legacy indices it must be zero and the source index must be known and unique. Writable defaults to `unknown`, otherwise `writable` or `read_only`; it is provenance. |
| `[profile.N.point.I]` | `topology_id`, `kind`, applicable payload, `observed_frequency_hz`, `observed_offset_hz` | Reference and kind required. Observations optional: positive signed-64-bit frequency and any signed-64-bit offset, including zero. |

| Point kind | Authoritative payload |
| --- | --- |
| `absent` | None; allowed for separately retained observations |
| `absolute` | Required positive signed-64-bit `target_hz` |
| `projection` | None; requires an included graphics component |
| `driver_offset` | Required signed-64-bit `offset_hz`, including owned zero |

Every point must reference document topology. Absent points without observations
are omitted by the writer. Converting a captured tail into reviewed flatten
intent retains its old offsets as `observed_offset_hz`, not as tail authority.
Source writable flags and moving observations cannot change semantic equality.

## Graphics, memory and power

| Section | Keys, in writer order | Meaning |
| --- | --- | --- |
| `[profile.N.graphics]` | `offset_hz`, `exclusion_policy`, applicable count | Offset and policy required. Offset is signed-64-bit physical Hz. Policy is `none`, `identities` or `legacy_count_unresolved`. |
| `[profile.N.exclusion.I]` | `topology_id` | Required only for `identities`; every index 0..`excluded_count-1` must exist. References must be unique and belong to the document topology. Exclusion order has no semantic meaning. |
| `[profile.N.memory]` | `offset_hz`, `surface` | Offset required, any signed-64-bit physical Hz. Surface defaults to `destination`, otherwise `p0` or `global`; a named source surface is retained. Runtime feasibility is separate from storage bounds. |
| `[profile.N.power]` | `mode`, `numerator`, `denominator` | All required: `board_default_ratio` and positive signed-64-bit terms. Accept equivalent unreduced ratios; writer reduces them. |
| `[profile.N.power_observation]` | `observed_default_mw`, `observed_limit_mw` | Independently optional positive signed-64-bit mW; require at least one. Allowed without power intent, retaining a partial getter result without inventing a ratio. |

For graphics `identities`, `excluded_count` is required, 0..128, and
`exclude_low_count` is forbidden. For `legacy_count_unresolved`,
`exclude_low_count` is required, 0..128, with no identity records. For `none`,
neither count nor exclusion records may occur. A different destination topology
never silently reinterprets an identity set or an unresolved legacy count.

## Lock intent and observations

| Section | Keys, in writer order | Meaning |
| --- | --- | --- |
| `[profile.N.lock]` | `mode`, `anchor_topology_id`, `target_hz`, `track_anchor` | All required. Mode is `flatten` or `hard`; anchor must reference topology; target is positive signed-64-bit Hz; tracking is boolean. |
| `[profile.N.lock_observation]` | `provenance`, `recognition_present`, applicable recognition values, `pin_range_present`, applicable pin values | Both presence booleans required. Provenance defaults to `user`, otherwise `recognized_flatten`, `driver_getter`, `applied_intent`, `user_confirmed`. Allowed without lock intent, including provenance with both groups absent. |

If recognition is present, require `anchor_voltage_uv` > 0,
`tail_count` 1..128 and `tolerance_hz` >= 0, all signed 64-bit except count.
If a pin range is present, require positive signed-64-bit `pin_min_hz` and
`pin_max_hz >= pin_min_hz`. Inactive group's value keys are forbidden. These
observations do not establish read-only getter evidence or Apply authorization.
The provenance and source report distinguish getter evidence from applied intent
and user confirmation; none may substitute sampled operating clocks as authority.

## Complete Green Curve fan configuration

| Section | Keys, in writer order | Meaning |
| --- | --- | --- |
| `[profile.N.fan]` | `mode`, `duty_pct`, `curve_present` | All required. Mode is `auto`, `fixed` or `curve`; duty is 0..100, including the dormant fixed-duty preference. Curve presence is boolean; Curve mode requires it. |
| `[profile.N.fan.curve]` | `poll_interval_ms`, `hysteresis_c`, `zero_rpm_enabled`, `zero_rpm_hysteresis_c`, `point_count` | All required when curve configuration is present. Poll is 250..5000 in 250 ms steps; hysteresis 0..10; native zero-RPM boolean; fan-off gap 2..30; point count exactly 8. Dormant curve configuration is retained in Auto/Fixed mode. |
| `[profile.N.fan.point.I]` | `enabled`, `temperature_c`, `duty_pct` | All eight indices 0..7 required with curve configuration. Boolean enablement; temperature 0..100 C; duty 0..100. Preserve disabled points too. |

The existing fan validator also requires at least two enabled points with
distinct temperatures. No missing field is silently normalized or invented.
Reserved in-memory fan bytes are canonical zero and have no file keys.

## Auxiliary controls

| Section | Keys | Meaning |
| --- | --- | --- |
| `[profile.N.aux.xbar]` | `clock_offset_hz`, `voltage_offset_uv` | Independently optional signed-64-bit values; require at least one. Clock and voltage presence are separate. |
| `[profile.N.aux.sys]` | `offset_hz` | Required signed-64-bit physical clock adjustment |
| `[profile.N.aux.video]` | `offset_hz` | Required signed-64-bit physical clock adjustment |

All listed tuning sections require their declared domain. Unavailable hardware,
out-of-current-range values and currently infeasible curve shapes remain valid
documents for inspection and transfer. Mapping/planning reports those separately;
decoding never clamps, interpolates, adopts live values or repairs new-format data.
