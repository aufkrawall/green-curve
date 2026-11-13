# Profile document (authoring model)

Source anchors: `source/profile_document.h`, `source/profile_point_intent.h`,
`source/tuning_units.h`, `source/profile_document_validation.h`,
`source/profile_text_validation.h`, `tests/profile_document_tests.cpp` (7960-8000),
`tests/profile_document_contract_tests.cpp` (8280-8321).

## Summary

A new, platform-neutral authoring type, `ProfileDocument`, built 2026-11-12.
It is the in-memory shape the wider plan calls "the durable authoring
model": tuning intent that must survive capture, editing, slot storage,
process restart and export **without a connected GPU**, and stay a valid,
inspectable document even when it currently conflicts with hardware (an
unreachable target, a point outside the live range, a GPU that is absent or
does not match). Offline CLI import/export and flat local-slot storage consume
this type independently of the live wire `DesiredSettings` (`gpu_core.h`).
The service keeps its separate resolved runtime representation; GUI/TUI draft,
destination resolution and capture integration remain pending below.

- `ProfilePointIntent` (`profile_point_intent.h`): one VF point's authoritative
  meaning. Four mutually exclusive kinds -- `PROFILE_POINT_ABSENT`,
  `PROFILE_POINT_ABSOLUTE_FREQUENCY`, `PROFILE_POINT_PROJECTION`,
  `PROFILE_POINT_CAPTURED_OFFSET` -- with exactly one authoritative payload
  per kind (`absoluteFrequencyHz` or `capturedOffsetHz`) and a separately
  labeled, non-authoritative `observedFrequencyHz` preview that is never
  substituted for a missing payload and never compared or persisted as if
  it were authoritative. Zero is a valid owned `capturedOffsetHz` (contract
  6 of the authoring plan): `profile_point_intent_structurally_valid()`
  accepts it, and `profile_point_intent_semantically_equal()` treats a
  captured zero and an absent point as different. `profile_point_intent_canonical()`
  clears every field a kind does not own, so a trust boundary (eventually
  IPC or disk) cannot smuggle a stale value through an inactive field.
  `profile_point_intent_type_absolute_mhz()` is the "user typed an MHz
  value" conversion: it sets kind to ABSOLUTE_FREQUENCY and drops captured
  authority, matching the live editor's existing rule that typing a point
  converts it (`main_runtime_control.cpp`'s equivalent today).
- `ProfileDocument` (`profile_document.h`): versioned document-local intent.
  `topology[]` (exact voltage in microvolts, a stable within-document
  `topologyId`, and a diagnostic-only `sourceIndex`) is index-parallel with
  `points[]`; `lock`, `power`, `fan`, `graphicsOffset`, memory and the
  advanced XBAR/SYS/VIDEO clock fields are each an explicit `present`/`has*`
  struct, not inferred from a nonzero value. `domains` (`ProfileDomainMask`)
  and `applicationScope` (`ProfileApplicationScope`: UPDATE_INCLUDED /
  REPLACE_PROFILE / EXPLICIT_RELEASE) are separate from each other and from
  per-point presence, per the plan's §5.5 scope contract.
  `profile_document_structurally_valid()` checks only well-formedness
  (bounded topology count, no duplicate topology identity, every point's own
  kind/payload rule, a lock's anchor actually referencing a topology point in
  the SAME document, a present power intent carrying a valid ratio) -- never
  hardware feasibility; a document can be structurally valid and completely
  infeasible on the currently selected GPU, which is the point (§7.2:
  storage must not repair an infeasible document). `profile_document_semantically_equal()`
  compares intent, presence, scope and authoritative values only, ignoring
  every observational field (per-point preview frequency, power's observed
  mW pair, lock provenance, capture completeness) and the display name.
- `TuningPowerRatio` / unit helpers (`tuning_units.h`): power intent is an
  exact reduced ratio of board default (`tuning_power_ratio_from_mw()`,
  `..._from_percent()`), never raw mW, so a captured 249000/250000 mW limit
  stays 249/250 instead of rounding to 100%. `tuning_power_ratio_resolve()`
  uses checked 64-bit quotient/remainder arithmetic against a caller-supplied FRESH
  destination default and reports `exact=false` (never a silent round) when
  the division does not come out even, and `overflowed=true` (never a wrapped
  value) when the result would overflow or the inputs are
  invalid -- it performs no destination-granularity/write-surface check
  itself, since that is platform-specific and belongs where the destination
  is actually known. `tuning_hz_to_khz()` reports `hadRemainder` so the
  scale-2 Pascal half-kHz case (`tests/linux_vf_write_tests.cpp`'s raw 30001
  fixture, ported here as a 15000500 Hz synthetic case) is never silently
  narrowed into a clean kHz value without the caller knowing.

## Runtime point adapter (2026-11-12)

Source anchors: `source/desired_curve_intent.h`,
`source/gpu_backend_apply_targets.h`, `source/gpu_backend_apply_verify.h`,
`source/gpu_backend_apply_correction.h`, `source/linux_curve_targets.h`,
`tests/captured_intent_runtime_tests.cpp`, `tests/clock_transition_tests.cpp`.

One adapter interprets runtime absent, absolute, projected and captured point
fields. Captured presence is independent of MHz and includes zero; contradictory
tags, frequency payloads and lock-tail overlap refuse before mutation. Checked
conversion reports a sub-kHz remainder instead of dropping it. Both target
builders preserve the captured offset independently of the graphics component
or measured scalar shift. Captured readback must equal the programmed target;
the legacy projected-clock tolerance does not excuse a different captured offset.

The Windows correction loop receives a temporary offset-intent view without
creating sampled MHz authority in the stored request. Linux post-write checks
verify captured offsets as well as absolute targets. Domain masks, restore
builders and both merges retain captured zero. Hardware-independent point
equality compares authority, including ignoring projected preview drift for
applied-profile matching; structural runtime equality stays separate.

Schema migration can preserve this intent, but IPC captured replay remains
explicitly disabled until document/editor persistence is connected. Explicit
runtime scopes now use the transition compiler below. The Windows write interface requires exact physical kHz; the Linux
raw control surface now supports its exact integer-Hz grid as described below.
Runtime schema 4 now carries exact memory Hz/surface and rational power intent,
with scoped merges, cleanup, restart migration and backend consumers. See
[persistence-schema.md](persistence-schema.md). Document/local-slot producers
and precise graphics/absolute-frequency evolution remain pending.

Fixtures 8040-8060 and the production Windows target/verify fixture cover zero,
negative offsets, preview/global-offset independence, strict readback, merge,
restore, unavailable-domain projection and malformed intent. No hardware write
or native Linux execution is claimed by these host-neutral fixtures.

## Exact power destination controls (2026-11-13)

Source anchors: `source/power_ratio_control.h`, `source/gpu_backend_power.cpp`,
`source/linux_backend_nvml_write.cpp`, `source/linux_backend_preflight.h`,
`tests/power_ratio_control_tests.cpp`, `tests/power_recovery_tests.cpp`,
`tests/linux_scope_preflight_tests.cpp`.

The shared planner resolves a positive rational intent against fresh NVML
current/default controls. Known board bounds are authoritative; unavailable
constraints use the documented percentage fallback bounds. Fractions of mW,
whole-watt CLI limitations and arithmetic overflow cause explicit refusals,
never rounding or clamping. An unavailable default-only surface is inert only
for 1/1 without a forced repair. Two immediate reads must agree before mutation;
post-write verification requires the exact target and unchanged denominator,
bounds and write grid. No timing assumptions or additional retries are used.

Windows precise and legacy setters share the selected-GPU writer and its
uncertainty tracking. The precise entry point does not round for the trusted
CLI fallback; legacy percentage recovery retains its established behavior.
Linux percentage requests now use the exact engine and preflight board bounds
before any transaction writes. Protocol 32 accepts precise ratios with explicit
scopes; document/editor producers remain pending.

Fixtures 8170-8199 cover exact 249/250, partial reads, changing defaults/bounds,
failed or rounded writes, unavailable surfaces, CLI grid refusal, forced repair,
stale denominators and production Windows/Linux preflight call sites. These
are fake-driver checks, with no physical GPU mutation.

## Runtime application scopes (2026-11-12)

Source anchors: `source/service_scoped_apply_policy.h`,
`source/main_service_apply_runtime.cpp`, `source/linux_daemon_scoped_apply.h`,
`source/linux_backend_mutation.cpp`, `source/service_lifecycle_policy.h`,
`tests/scoped_apply_tests.cpp`, `tests/linux_scope_preflight_tests.cpp`.

The shared compiler returns separate hardware-transition and durable-ownership
requests. UPDATE_INCLUDED merges only supplied fields and points, retaining an
omitted lock. REPLACE_PROFILE releases previously owned omissions. The optional
VF point-set replacement bit replaces points while preserving omitted scalar
and lock intent. EXPLICIT_RELEASE clears the selected ownership and writes
defaults; its resulting ownership replays as an update, never another release.
Releasing the final field retires active ownership. Legacy requests keep their
historical behavior.

Cleanup includes owned zero, uses exact raw-zero controls and excludes foreign,
missing and fixed-floor entries. A directly owned point whose identity is no
longer writable refuses instead of being silently dropped. Retained graphics
dependencies are replayed together, and a conflicting captured point cannot
silently remove a retained pin. Independent scalar domains stay omitted.
Each intent selects its graphics route separately: leaving a selective VF
component for a uniform scalar releases the old raw ownership. Compiled
cleanup points do not promote that scalar back into a VF component. Linux
preflight checks the independently selected scalar surface as well as VF.
Compiled locks use their intended frequency rather than a drifted live anchor.
Removing an owned flatten carries a transition-ceiling requirement even without
a global baseline reset. Partial scopes cannot request that reset. Automatic
replay retains the resolved scope; release operations cannot be assigned as
startup profiles. Numeric requests that legacy normalization would change are
refused for explicit scopes.

Linux baseline preflight no longer requires a memory getter/setter when memory
is not requested. The production preflight fixture covers absent/read-only
memory surfaces with counted zero setters; the production raw writer fixture
covers scoped update and replacement while preserving an unowned fractional
control. Full native Linux orchestration and physical transitions remain
unverified. Client document/editor scope presentation remains pending.

## Exact VF controls and recovery (2026-11-12)

Source anchors: `source/vf_curve_units.h`, `source/linux_backend_vf_read.h`,
`source/linux_backend_vf_write.h`, `source/linux_backend_rollback.h`,
`source/gpu_backend.cpp`, `source/main_runtime_gpu.cpp`,
`tests/linux_vf_write_tests.cpp` (8090-8098),
`tests/captured_intent_runtime_tests.cpp` (8099-8102).

Control readers retain exact physical Hz alongside the legacy kHz projection.
The scale-2 raw value 30001 retains 15000500 Hz; a failed fresh read invalidates
exact evidence. Linux captured writes compile checked Hz directly into raw
driver units, verify exact controls and preserve untouched raw entries. The
surface grid is derived from its declared scale without adding a family
allowlist. The planner checks exact captured bounds, including fractional
excesses, independently of a sampled MHz preview.

Linux recovery snapshots retain exact controls. Baseline reset, explicit reset
and ownership cleanup clear raw fractional values to exact zero. Windows raw
no-op comparisons and convergence use exact controls when available. Its exact
memory-preservation route now retains sub-kHz controls; the general captured
target adapter and IPC replay remain pending. Captured verification requires
fresh exact evidence. Exact failures log a bounded summary.
The production fake-driver and rollback fixtures cover positive/negative half
steps, ignored writes, no-op detection, range/grid refusal without setters and
untouched fractional controls. Native Linux and physical GPU replay remain
unverified.

## Windows VF preservation across memory writes (2026-11-13)

Source anchors: `source/vf_exact_control_write.h`,
`source/curve_preservation_precision.h`, `source/gpu_backend_exact_curve.h`,
`source/gpu_backend_apply.cpp`, `tests/windows_vf_exact_tests.cpp` (8580-8600).

Fresh exact controls are checked before a memory mutation when a VF curve is
loaded. A 500 Hz offset is nonzero even when its kHz preview is zero. Outgoing
snapshots retain exact Hz through reset/scalar refreshes; unowned residuals survive
target construction, while explicit point and lock-tail authority overrides them.
Unavailable exact evidence, read-only nonzero controls and unrepresentable
preservation refuse before the first transition write. An unavailable VF surface
does not prevent an independent memory-only operation when no curve is loaded.

The exact writer converts all targets before its first setter, validates fresh
control templates, preserves unselected raw entries and verifies selected raw
controls directly. It uses bounded batch/per-point fallback under the existing
deadline, without sleeps or placeholder acceptance. Fresh voltages must match
the original topology before replay. One `curve-exact:` line summarizes its
result; `curve-preserve:` identifies pre-mutation refusals. Fake-driver fixtures
exercise memory-clobbered half steps, owned zero, one-point driver behavior,
failed/ignored writes, stale topology, grid refusal and deadline exhaustion.
No hardware replay is claimed.

## Precise memory and bounded arithmetic (2026-11-12)

### Linux VF preservation across memory writes (2026-11-13)

Source anchors: `source/linux_memory_curve_policy.h`,
`source/linux_memory_curve_preservation.h`, `source/linux_backend_preflight.h`,
`source/linux_backend_mutation.cpp`, `source/linux_backend_rollback.h`,
`tests/linux_vf_write_tests.cpp` (8610-8620),
`tests/linux_scope_preflight_tests.cpp` (8621-8623).

Memory Apply preflights readable exact VF controls when that surface exists,
then holds a fresh control image immediately before its memory setter. It
rechecks identity, full membership and voltages before exact raw restoration,
excluding moving frequency samples. Unknown VF surfaces leave independent memory
support available. Explicit baseline reset intentionally releases the old curve.

The shared exact writer runs under the declared hardware-write budget and
verifies all selected controls. It neither adopts them as owned intent nor
changes clock-pin controls. Failed memory phases include VF restoration in
rollback even if the later curve phase never ran. The production helper and
rollback fixtures cover half steps, precise memory surfaces, topology changes,
ignored writes, stale preflight controls and unavailable VF. `mem-curve:` logs
one bounded failure/result summary. Native Linux and hardware execution remain
unverified on this Windows host.

### Shared memory units and verification

Source anchors: `source/memory_offset_units.h`,
`source/gpu_backend_memory_offset.h`, `source/main_runtime_nvml.cpp`,
`source/linux_backend_rollback.h`, `source/tuning_units.h`,
`tests/memory_recovery_tests.cpp`, `tests/profile_document_tests.cpp`.

Memory conversion retains the NVML effective-MHz control exactly (one effective
MHz is 500000 physical Hz). The strict setter checks the API unit grid, requires
a fresh programmable-control getter before mutation and verifies that control
directly afterwards. A quantized-match hint or a P-state frequency cannot prove
an exact captured offset. Legacy integer-MHz writes retain their established
verification. Linux recovery snapshots preserve the effective control separately
from their legacy integer display-MHz projection, including the observed API
surface. Exact recovery verifies that same surface without accepting a grid
snap or switching to another getter after a write. A failed precise setter
does not fall through to a second API. Missing surface evidence refuses rather
than degrading to legacy tolerance. Legacy integer tuning keeps its established
verification and fallback behavior.

Source anchors for Linux verification: `source/linux_backend_clock_offsets.h`,
`source/linux_backend_rollback.h`, `tests/linux_clock_offset_tests.cpp`
(8150-8163), `tests/clock_transition_tests.cpp` (snapshot surface propagation).
The [NVML query documentation](https://docs.nvidia.com/deploy/nvml-api/latest/api/group__nvmlDeviceQueries.html)
specifies a P-state for modern offset controls. The implementation binds exact
readback to its observed interface instead of treating API substitution as proof
of identical values. An invalid modern read can still use a valid legacy getter
for ordinary observation; it is labeled as a legacy surface.

Windows and Linux exact writes now share `source/nvml_exact_clock_control.h`.
The Windows getter retains the chosen surface in internal application state;
strict memory writes bind that surface and never fall through after a failed
setter. Modern observation validates version, domain and P0 metadata before
publishing a readback. Device changes clear the surface. Conservative memory
range reports remain diagnostic, preserving the existing conditional-write
contract. Production Windows cases 8200-8205 exercise separate P0/global
controls, missing setters/getters, malformed metadata and conservative ranges;
Linux cases 8150-8163 continue through the shared helper.

Clock observation is shared in `source/nvml_clock_observation.h`. The current
control and its range have independent validity/status, so missing ranges do
not erase a readable offset. Modern metadata still must name the requested
control. Range-requiring legacy callers remain explicit; exact post-write
verification stays on its named API. Windows cache scaling checks the physical
kHz range before narrowing. Cases 8265-8271 exercise current-only, range-only,
malformed modern ranges and independently failed getters through production
observation/reader helpers.

Signed unit conversion avoids negating the signed minimum and checks scaling
before multiplication. Power-ratio resolution computes quotient/remainder
without constructing an overflowing product, so an exactly representable ratio
is not refused merely because its intermediate product exceeds 64 bits. Unit
cases 8070-8086 and the production Linux rollback fixture cover these contracts.

## Transfer document contract (2026-11-13)

The exhaustive version-1 key/section contract is published beside the tests:
[profile_format_v1.md](../tests/profile_format_v1.md). The shared codec implements
this table. Local flat-slot storage and offline CLI transfer use the same document;
GUI/TUI draft and capture review remain pending.

The document now carries an independent VF point-set policy, source topology
completeness versus unresolved legacy indices, identity-based exclusions,
exact memory-surface intent, complete optional fan configuration, source
capture omissions and pin knowledge, and bounded lock-recognition evidence.
Former captured tail offsets can remain observations on absent points after a
reviewed representation change. Those observations never become authority.

The pure validator checks version/enums, strict bounded UTF-8 text, domain
membership, every topology reference, projection dependencies, exclusion
identity uniqueness, lock metadata and the existing fan limits. Hardware
bounds remain outside structural validation: even signed-64-bit extremes are
retained for inspection. Equivalent power ratios and reordered exclusion sets
compare semantically equal; moving observations remain excluded. Unit cases
8280-8321 execute these boundaries. No authoring struct is persisted as bytes.

## Passive capture foundation (2026-11-13)

Source anchors: `source/hardware_capture_types.h`, `source/hardware_capture.h`,
`source/hardware_capture_getters.h`, `tests/hardware_capture_tests.cpp` (8480-8501).

The getter-only collector reads fresh VF membership, physical microvolt identity,
exact Hz offsets and separate frequency samples. NVML memory retains half-MHz
physical steps; power keeps independently valid current/default observations.
Scalar/table composition defaults to unresolved, with a portable graphics
observation that cannot silently become an independent graphics component.
Pin knowledge requires an optional validated getter; current clocks never prove
absence. External fan behavior is excluded; explicitly requested owned fan
intent can be copied with its complete curve configuration.

At most three immediate read pairs compare topology, bindings, controls, ranges
and validity while excluding moving frequency values. Failure leaves the caller's
previous result intact. This is a best-effort consistent observation; external
writers can still interleave or make an ABA change. Fingerprints detect stale
observations and confer no authorization or ownership. Protocol 33 connects the
collector to dedicated Windows/Linux handlers using existing sessions only.
Linux capture skips ordinary snapshot assembly when stamping its envelope;
Windows never uses the lazy driver loader. A different target, deferred recovery
or uncertain Linux session refuses without discovery or handback. See
`source/main_service_capture.cpp`, `source/linux_backend_capture.h`,
`source/linux_daemon_capture.h`, `tests/hardware_capture_service_tests.cpp`
(8520-8526). Both CLI clients now report completeness and explicitly review
partial/exact saving; see [config-profiles.md](config-profiles.md#passive-capture-cli-2026-11-13).
GUI/TUI and flatten/pin proposal review remain pending. Fixtures load no driver
and perform no hardware mutation.

## Invariants

- No authoring-model or codec operation reads `g_app`,
  `LinuxGpuState`, or calls a driver. A document must stay constructible,
  comparable and exportable with no GPU selected at all.
- Structural validity and hardware feasibility are different questions
  answered by different code; this module answers only the first one.
- Equality always excludes observational-only fields. A point's preview
  frequency, a power ratio's observed mW pair, and a lock's provenance tag
  are provenance, not meaning -- the same rule `curve_point_offset_policy.h`
  and `gpu_backend_apply_verify.h` already apply to the live runtime's own
  offset-intent/absolute-intent split (`linux-scaffold.md`, "Linux VF
  baseline reset and curve/plateau verification parity").
- `ProfileDocument` embeds the existing `FanCurveConfig` (`gpu_core.h`) and
  reuses `fan_curve_equals()` (`fan_curve.h`) rather than re-deriving fan
  equality; `VF_NUM_POINTS` and `LockMode` are the same constants/enum the
  live runtime uses, not a parallel redefinition.

## Open questions / stale-risk

- **Editor integration pending.** Offline import/export and local records consume
  documents; the historical adapter retains unresolved source indices. GUI/TUI
  draft integration remains pending. The historical Apply loader refuses a new
  document requiring destination review rather than interpreting it as defaults.
- **Destination mapping remains pending.** New graphics exclusions name source
  topology identities. Historical counts and index-only topology carry explicit
  unresolved markers; neither may acquire destination voltages by assumption.
- **Capture producers remain pending.** Bounded recognition/pin observations
  and per-point observed offsets can survive review/storage independently of
  lock or point authority. Their presence alone never establishes a getter or
  authorizes Apply; pin knowledge defaults to unknown.
- **Mapping integration pending.** The pure exact-voltage mapper and reviewed
  conversion primitives exist below. Their GUI/TUI/service binding and complete
  historical-format coverage remain pending. Offline file/storage commands never
  substitute destination data.

## Portable codec

Source anchors: `source/profile_codec.h`, `source/profile_codec_types.h`,
`source/profile_codec_fields.h`, `source/profile_codec_decode.h`,
`source/profile_codec_encode.h`, `tests/profile_codec_tests.cpp` (8340-8356),
`tests/profile_codec_fuzz.h`, `tools/security_gates.py` (`profile_codec`).

`ProfileBank` contains zero to five independent documents. Decode and encode
publish only fully validated temporary results. Neither operation performs
file I/O, reads hardware, changes assignments or grants Apply authorization.
The bounded UTF-8 decoder accepts LF/CRLF and a leading BOM, checks decimal
overflow and field presence, and rejects unknown/duplicate sections and keys,
inactive payload keys, broken counts/references and incomplete fan records.
Limits are 1 MiB per input and 4096 bytes per line. Diagnostics carry a stable
reason and line number; input values are not echoed into errors.

Writer output has fixed section/key order, reduced ratios, sorted identity
exclusions and escaped quoted text. Point observations, partial power reads,
lock recognition, dormant fan settings and source metadata survive round trips
independently of authority. Signed-64-bit limits remain document-valid even
when destination resolution will refuse them. Numeric values permit leading
zeros; record ordinals are canonical decimal. All eight fan records remain
required when a configuration is present.

Native regression covers a full five-document bank, arbitrary section/key
order, UTF-8/CRLF, exact values, observations, maximum input and malformed data.
The separate fuzz target checks raw parsing, unchanged output on failure,
structured exact-value round trips and canonical re-encoding on both hosts.

## Destination mapping and conversion primitives

Source anchors: `source/profile_mapping.h`, `source/profile_conversion_grid.h`,
`source/profile_conversion_batch.h`, `tests/profile_mapping_tests.cpp`
(8420-8444), `tests/profile_conversion_tests.cpp` (8460-8474),
`tests/profile_codec_fuzz.h`.

Mapping requires complete destination graphics membership for point/lock work.
It uses exact microvolt identities, with source indices accepted only after a
complete topology match proves their meaning. Missing/duplicate voltages,
read-only writes, repeated destinations, unresolved historical indices and
unavailable domains are distinct results. Exclusions need identity mapping but
do not become writes to excluded read-only points. Additional destination points
still require the complete curve planner; successful mapping is not feasibility
or write authorization. Default power on an unavailable surface remains a
candidate inert request, subject to the backend's prior-ownership cleanup check.

Historical-index acceptance creates a separate derived document and binds the
review to source revision, GPU generation and destination revision. Grid batches
also bind the exact surface, step/origin and fresh power default, compare current
semantic source values and re-run a caller-supplied complete planner. Failure
publishes no partial conversion. Original source documents stay unchanged.
Negative offsets floor toward a lower resulting clock, including checked signed
minima; power conversions produce a new exact lower-limit/default ratio. Unknown
grids remain unresolved and voltage adjustments have no automatic safe direction.

Each batch reports affected count, first/worst physical voltage and maximum
error. `profile_conversion_summary()` produces one bounded `profile-map:` line
for preview/accept/reject. Observations and names that change without changing
semantic values survive acceptance. Conversion-batch product callers remain
pending. CLI mapping preview consumes fresh passive destination observations
through the runtime resolution path below; the pure helpers perform no I/O or setters.

Last verified: 2026-11-13.

## Runtime topology binding during scoped Apply (2026-11-13)

Source anchors: `source/desired_topology_binding.h`,
`source/service_scoped_apply_policy.h`, `source/main_service_apply_runtime.cpp`,
`source/linux_backend_apply_context.h`, `tests/desired_topology_binding_cases.h`
(8680-8690).

The shared scoped planner resolves bound incoming and outgoing graphics intent
before merge or cleanup. An identical complete voltage table preserves indices,
including distinct identities at equal voltages. Otherwise each authoritative
point, anchor or exclusion requires one unique exact destination voltage.
Missing/ambiguous identities, duplicate destinations and non-writable targets
refuse before publishing a plan. Remapping preserves exact point payloads,
captured zero, exclusions and lock identity, then stamps the full current table.
Both platform Apply summaries include the number of remapped identities.

Cleanup uses the remapped outgoing identity set and retains its topology binding.
Explicit independent graphics surfaces stay independent of VF ownership; identity
exclusions replace ordinal exclusions for bound VF components. Fan-only updates
do not require a fresh VF observation and retain dormant graphics authority.
These checks do not enable runtime authoring replay: the capacity guard described
in `persistence-schema.md` still requires reviewed destination adapters.
Native regression fixtures cover shifted tables, overlapping index moves,
full-match duplicate voltages, missing/ambiguous matches, fixed floors and scoped
replacement. No physical or native Linux replay is claimed.

## Exact runtime resolution and CLI mapping preview (2026-11-13)

Source anchors: `source/profile_runtime_resolution.h`,
`source/profile_check_commands.h`, `source/profile_commands_windows.h`,
`source/profile_commands_linux.h`, `tests/profile_check_commands_cases.h`
(8700-8717), `tests/profile_commands_cli_tests.py`, `tests/profile_codec_fuzz.h`.

`--check-profile FILE [--profile N]` validates the portable file and requires a
selection for a multi-entry bank before contacting the service. It uses passive
Capture in the existing bound session, maps exact voltage identities and reports
included controls and exact resolved values. It has no write or commit callback.
Missing pin/fan capture getters never imply absent write capability. Parse,
mapping, conversion and session failures preserve both input files and drafts.

The resolver compiles explicit scope, point-set policy, captured zero, exact
absolute/graphics/lock frequencies, exclusions, named physical memory and rational
power, owned fan data and auxiliary controls. It stamps the full destination
topology, uses destination-selected graphics routes, retains explicit named memory
surfaces, and publishes no partial runtime result on failure. Values exceeding application bounds or a kHz-only
auxiliary surface require explicit correction/conversion; the source document
remains unchanged. Explicit release still requires current ownership resolution.

This command is a mapping preview, not a completed feasibility gate: range,
grid and transition checks remain outstanding, with decision-required exit 3
even when mapping succeeds. Runtime replay stays guarded until those production
adapters are complete. Native fixtures cover file preservation, bank selection,
read-only destinations, exact values, exclusions and missing sessions. The
portable-codec fuzz target also checks exact runtime resolution, source
immutability and unchanged output on refusal. Hardware replay remains unverified.

## Passive constraint provenance and omitted curve authority (2026-11-13)

Source anchors: `source/hardware_capture_types.h`,
`source/hardware_capture_getters.h`, `source/hardware_capture_wire_validation.h`,
`source/hardware_capture_runtime.h`, `source/profile_runtime_resolution.h`,
`source/profile_mapping.h`, `tests/hardware_capture_constraints_cases.h`
(8720-8731, 8735-8737), `tests/profile_check_commands_cases.h` (8732-8734).

Capture schema 2 carries the backend's exact physical-Hz VF offset grid and
write-support trait. Fresh NVML graphics bounds are labeled as reported graphics
evidence; unavailable ranges use the existing application policy with distinct
provenance. This is not a dedicated per-point driver-range getter or setter
availability proof. All new fields participate in coherent read comparison and
fingerprinting. Wire validation checks flags, inactive payloads and provenance;
the bounded Capture summary prints ranges and grid with their source.

A read-only backend contributes point observations without writable intent.
Domain membership requires authoritative writable points, independently of the
captured observation mask. Point-set replacement is active only when the document
includes the VF domain: a memory-only or observation-only capture cannot clear
retained curve authority. Semantic equality and mapping/planning requirements
also ignore the inactive point-set policy. The production scoped-plan fixture
checks that omitted curve points survive scalar-only runtime resolution.

## Exact frequency snapshots (2026-11-13)

Source anchors: `source/vf_curve_units.h`, `source/gpu_backend.cpp`,
`source/gpu_backend_snapshot.h`, `source/linux_backend_vf_read.h`,
`source/main_state_sync.cpp`, `tests/vf_frequency_precision_cases.h`,
`tests/linux_vf_read_tests.cpp`, `tests/clock_transition_tests.cpp`.

Both backends retain physical-Hz graphics status frequencies before projecting
into the legacy kHz display table. Pascal scale-2 status values therefore keep
half-kHz remainders. Frequency and offset precision flags describe their cached
samples, not an atomic cross-interface read or proof that a future write is
safe. Capture uses the same checked frequency conversion. Foreign-domain cache
entries are zero; malformed or empty status samples cannot grant exact validity.

The settled Windows reader saves matching exact arrays with each complete
legacy sample. A final failed read invalidates precision proof even when the
previous complete arrays remain for diagnostics. A service snapshot carries
only kHz display data and invalidates both local exact flags; selection teardown
and Linux freshness reset invalidate them too. Cases 8740-8747 and 8759 exercise
raw bounds, missing outputs, fractional-Hz refusal, foreign entries and failed
reads; the settled-reader fixture verifies array pairing and final-failure
invalidation. Exact authoring remains gated pending complete Apply integration.

## Exact-Hz feasibility and baseline evidence (2026-11-13)

Source anchors: `source/curve_feasibility_plan.h`, `source/curve_plan_precision.h`,
`source/desired_curve_intent.h`, `source/curve_feasibility_message.h`,
`source/gpu_backend_apply_lock_pretail.h`, `source/linux_backend_curve_plan.h`,
`tests/curve_plan_precision_cases.h` (8760-8779).

The shared planner performs point, prefix, anchor and tail arithmetic in Hz.
Precise absolute/lock intent requires matching exact status/control arrays;
missing arrays, inconsistent legacy projections or an unknown hard-pin grid
produce UNAVAILABLE rather than a representability claim. Backend prechecks
supply the actual VF grid; Windows backstops retain the exact original arrays
through reset and scalar readbacks. Named hard-pin targets use the documented
integer-MHz NVML surface. Projection components preserve exact graphics intent
and identity-based exclusions independently of display MHz.

Captured and projected controls remain offset authority, including zero. The
checked exact-baseline adapter preserves output on failure. Captured/projected
controls retain the signed physical envelope; absolute compilation keeps checked
64-bit differences until destination range/grid validation. Absolute intent uses
the exact sampled base. Range excesses keep the existing nearest-bin proof rule, and
legacy prefix/tail verification keeps its established display-bin semantics;
precise authority does not acquire a new display-rounding tolerance. First and
worst issues retain exact target/base/adjustment/limit values, with bounded
numeric diagnostics and explicit grid-conversion refusals.

Pre-write baseline classification inspects exact programmed offsets when
available. A half-kHz nonzero control cannot become stock evidence because its
legacy projection is zero. The focused fixture failed at 8760 before planner
integration; it covers exact remainder grids, absent/mismatched samples,
conditional range evidence, zero-preview projections, exact prefix ordering,
hard-pin grids and checked-output failures. Existing curve-plan cases still
pass. Runtime authoring/replay remains gated pending the complete writer and
transition-protection integration; CLI preview does not yet claim feasibility.

## Captured remainders through Windows corrections (2026-11-13)

Source anchors: `source/gpu_backend_apply_targets.h`,
`source/gpu_backend_exact_curve.h`, `source/gpu_backend_apply.cpp`,
`tests/clock_transition_tests.cpp`, `tests/windows_vf_exact_tests.cpp` (8780-8782),
`tools/apply_ceiling_gates.py`.

The Windows target builder accepts physical-Hz captured offsets independently
of legacy kHz projections, checking the physical range before projection. The
original DesiredSettings remains authoritative; the initial exact writer overlays
those controls before backend-grid validation and mutation. Scalar-shift
compensation still applies only to projected intent, never captured controls.
The diagnostic counts captured points and sub-kHz values without dumping tables.

If another absolute point requires a correction pass, exact-mode transactions
rebuild physical targets through the same capture-preserving overlay and exact
writer. A correction cannot erase a captured remainder merely because the
legacy correction plan contains a kHz preview. Grid/range and topology checks
precede that write; membership and unrelated raw controls remain untouched.
Fake-driver cases cover positive/negative half-kHz controls, owned zero, an
absolute correction and untouched raw remainders, plus zero-write refusals for
unsupported fractions and ranges. The builder fixture fails before the fix.
Runtime replay remains gated pending transition and full authoring integration.

## Shared exact target compiler and Linux adapter (2026-11-13)

Source anchors: `source/curve_authoring_targets.h`,
`source/linux_curve_authoring_targets.h`, `source/linux_backend_curve_build.h`,
`source/linux_backend_curve_plan.h`, `source/linux_backend_apply_context.h`,
`source/linux_curve_targets.h`, `source/curve_point_verify_policy.h`,
`tests/curve_authoring_target_cases.h`, `tests/linux_authoring_factory_cases.h`
(8783-8809).

The shared compiler retains physical-Hz offsets and absolute targets in separate
channels. It requires matching exact samples, the complete destination voltage
binding, writable membership, a resolved graphics route and representable raw
controls. Captured zero remains owned; typed points override the graphics
component; identity exclusions affect only that component. Lock tails retain
platform strategy and precedence. Failure leaves the caller's output unchanged.
Unknown-family default write support remains available through its backend.

Only an explicitly authorized baseline reset may defer a baseline-dependent
range failure over a shaped estimate. Deferred results carry write shape and
absolute intent, with placeholder offsets that are never ready for writing.
Direct offset/grid failures refuse. A verified transaction baseline must either
compile fully representable targets or fail; the compiler does not clamp.

Linux uses the exact compiler for authoring point-table work, with separate
precision masks for submitted offsets and frequency verification. Scalar-only
named graphics intent stays a separate surface. Rebuilds recheck full feasibility
against the fresh baseline, retain the same topology/write shape, and replace
both targets and exact verification metadata. Failed/deferred targets cannot
reach the writer. Readbacks compare exact physical frequencies under the existing
quantization tolerance, without adding display rounding. Legacy replacement
cleanup rebinds owned voltage identities and clears released controls exactly.

The focused fixtures execute the production Linux factory and rebuild helper
without a driver. Removing the metadata assignment fails at 8800. Coverage also
checks output preservation, signed remainders, missing samples, exclusions,
lock strategies, fixed floors, unknown-family fallback, deferred versus proven
range failure, exact tolerance edges and zero-write failed rebuilds. Runtime
replay remains gated pending route and transition integration on both platforms.

## Windows authoring targets and corrections (2026-11-13)

Source anchors: `source/windows_curve_authoring_targets.h`,
`source/gpu_backend_apply_authoring.h`, `source/gpu_backend_apply.cpp`,
`source/gpu_backend_apply_diagnostics.h`, `tests/windows_vf_exact_tests.cpp`
(8810-8827), `tools/apply_ceiling_gates.py`.

Windows consumes shared exact targets for bound authoring requests. Frozen
paired samples determine the initial offsets before a memory write. Controls
outside the authored set can be transported across that write without gaining
ownership; corrections retain their original raw preimage. The metadata keeps
physical absolute targets, captured controls, preserved controls and baseline
dependence separate. Full voltage bindings accompany every exact write.

Corrections re-evaluate feasibility and compile over fresh paired samples under
the existing pass/time budget. They reject changed write shape, fixed-control
authority or voltage binding, and stop when offsets no longer change. Captured
offsets and owned zero compare exactly; absolute frequency targets retain the
established quantization tolerance without display rounding. Flatten prefix
checks use physical intent over the latest baseline. Hard-pin tail frequency
remains the pin's responsibility; preserved tail offsets still verify exactly.
Exact boost tallies omit transported unowned controls. Legacy requests retain
their existing correction policy and diagnostics.

Fake-driver fixtures execute the production builder, exact writer and repeated
correction path. They check signed half-kHz offsets, owned zero, independent
base shifts, preservation after a memory clobber, exact tolerance edges,
unwritable preservation, stale samples, changed topology and fixed-point exit.
Replacing the saved unowned preimage with current controls fails case 8813.
Replay gates remain closed: transition/ownership review, fresh-control
preconditions and client authoring integration are still pending.

## Named Linux graphics controls (2026-11-13)

Source anchors: `source/graphics_offset_control.h`,
`source/desired_graphics_surface.h`, `source/linux_graphics_offset_control.h`,
`source/linux_backend_mutation.cpp`, `source/linux_apply_baseline.h`,
`source/linux_backend_rollback.h`, `tests/linux_clock_offset_tests.cpp`
(8828-8842), `tests/clock_transition_tests.cpp`.

An explicitly resolved P0 or global graphics scalar requires the named getter
and setter, an integer physical-MHz target and any valid reported bounds. A
missing range retains application policy; it does not invalidate current control
evidence. AUTO is unresolved. VF components retain their separate backend grid
and do not require a named scalar getter. No route depends on a GPU allowlist.

Linux binds the rollback preimage to the requested named surface before
transaction preflight. Apply, baseline preparation and rollback keep that
surface; a mutating failed setter or a failed verification cannot switch APIs.
An unchanged named scalar skips writing only after a fresh read. Named scalar
writes trigger fresh VF target compilation when the transaction also owns
points, so absolute targets use the resulting physical baseline. Historical
requests retain their established route.

The fake-driver cases cover distinct simultaneous P0/global values, no-op
readback, signed targets, failed and rounded writes, post-write getter failure,
missing getters/setters, invalid metadata, sub-MHz refusal, reported bounds and
missing ranges. The transition fixture executes the actual baseline/rollback
helpers and checks their surface arguments and refusal without surface evidence.
These adapters remain behind the existing authoring replay gate. Cross-platform
transition/ownership review and client integration remain pending.

## Windows graphics routing and scalar repair (2026-11-13)

Source anchors: `source/windows_graphics_offset_route.h`,
`source/gpu_backend_graphics_offset.h`, `source/gpu_backend_apply_precise_scalar.h`,
`source/gpu_backend_apply_authoring.h`, `source/gpu_backend_apply.cpp`,
`source/main_gpu_rollback.h`, `source/main_service_apply_runtime.cpp`,
`source/gpu_selection_write_evidence.h`, `tests/memory_recovery_tests.cpp`
(8843-8860), `tests/windows_vf_exact_tests.cpp` (8861-8869).

Explicit VF components, including sub-MHz values and zero, use the exact curve
batch rather than deriving their setter from an integer preview. Explicit named
graphics scalars use their selected getter, bounds and exact setter/readback;
they contribute no invented VF component. Historical routing remains unchanged.
An existing owned named surface is cleared before switching to another named
surface. The new surface is validated before that handback begins.

Named Windows writes record their repair surface before mutation. Verified
nonzero controls retain their owned surface for later Reset or rollback; an
uncertain write cannot be repaired through another API or an unrelated Apply.
Reset and rollback clear known named controls before removing VF protection,
including on backends whose ordinary graphics offset uses the VF table. A
failed repair prevents curve reset and restriction release. GPU selection clears
this session-specific evidence. Repeated matching requests skip only after a
fresh read; forced repair still invokes the setter.

Bound curve authority gets fresh exact target compilation before the first
transition setter, checking voltage bindings, membership, grids and ranges.
Only an authorized baseline reset may defer a shaped-estimate range failure.
The passive read does not prove that a separate scalar is absent. Complete
authoring bindings also route whole-MHz points through the exact compiler.

Fake drivers exercise named writes, surface switches, mutating failures, ignored
writes, failed readback, retained protection, repair and selection invalidation.
Restoring the old VF-global scalar-reset bypass fails at 8850.
Executing preflight cases assert zero setters for changed topology and off-grid
captured values, plus authorized versus unauthorized baseline deferral. Route
fixtures cover a signed half-kHz component and owned zero. Runtime authoring
remains gated pending transition/ownership review, durable recovery coverage,
control preconditions and client integration; these tests do not verify hardware.

## Explicit VF submission limits (2026-11-13)

Source anchors: `source/vf_control_write_policy.h`,
`source/vf_exact_control_write.h`, `source/gpu_backend_exact_curve.h`,
`source/gpu_backend_apply_authoring.h`, `source/linux_backend_vf_write.h`,
`source/linux_backend_mutation.cpp`, `tests/windows_vf_exact_tests.cpp`
(8870-8876), `tests/linux_vf_write_tests.cpp` (8877-8883).

The shared exact writer supports an explicit single-batch policy. It permits
one setter at most, verifies the result and reports a restricted mismatch;
neither repeated batches nor per-point fallback may follow it. Windows authoring
also suppresses frequency correction writes under this policy. A matching raw
readback needs no setter. Invalid policies refuse before writing, and caller
deadlines still apply.

Windows authoring chooses this limit without an installed transition ceiling.
Linux selects it from the compiled exact metadata, including transition cleanup,
and caps its existing batch loop at one. Exact Linux points disappearing from
membership or the populated table refuse before any setter. Historical requests
retain their established strategy. Unknown-family backend support is unchanged.

Counted fake drivers cover complete, partial and ignored batches; no-op and
deadline behavior; invalid policies; exact zero and signed remainders; missing
points; correction suppression; and the future-family backend. Removing the
single-batch early exit fails case 8870. These are submission limits, not a
completed transition policy: scalar ordering, baseline/recovery protection and
unknown external pin handling still require review before replay activation.
