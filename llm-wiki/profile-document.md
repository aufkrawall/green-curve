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
no-op comparisons and convergence use exact controls when available; captured
verification requires fresh exact evidence, while its kHz-only write contract
still refuses half-kHz targets. Exact failures log a bounded per-pass summary.
The production fake-driver and rollback fixtures cover positive/negative half
steps, ignored writes, no-op detection, range/grid refusal without setters and
untouched fractional controls. Native Linux and physical GPU replay remain
unverified.

## Precise memory and bounded arithmetic (2026-11-12)

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
GUI/TUI draft and capture integration remain pending.

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
(8520-8526). Client capture review remains pending. Fixtures load no driver and
perform no hardware mutation.

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
semantic values survive acceptance. Product callers and fresh destination
observation collection remain pending; these helpers perform no I/O or setters.

Last verified: 2026-11-13.
