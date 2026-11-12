# Profile document (authoring model)

Source anchors: `source/profile_document.h`, `source/profile_point_intent.h`,
`source/tuning_units.h`, `tests/profile_document_tests.cpp` (7960-8000).

## Summary

A new, platform-neutral authoring type, `ProfileDocument`, built 2026-11-12.
It is the in-memory shape the wider plan calls "the durable authoring
model": tuning intent that must survive capture, editing, slot storage,
process restart and export **without a connected GPU**, and stay a valid,
inspectable document even when it currently conflicts with hardware (an
unreachable target, a point outside the live range, a GPU that is absent or
does not match). Nothing in the runtime or IPC layer produces or consumes
one yet -- this is intentionally a from-scratch, GPU-free type, separate
from the live wire `DesiredSettings` (`gpu_core.h`), not a widening of it.
Migrating the runtime/IPC/disk schema onto this intent is a separate,
later piece of work; see "Open questions / stale-risk" below.

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
  does checked 64-bit multiplication against a caller-supplied FRESH
  destination default and reports `exact=false` (never a silent round) when
  the division does not come out even, and `overflowed=true` (never a wrapped
  value) when the checked multiply would overflow or the inputs are
  invalid -- it performs no destination-granularity/write-surface check
  itself, since that is platform-specific and belongs where the destination
  is actually known. `tuning_hz_to_khz()` reports `hadRemainder` so the
  scale-2 Pascal half-kHz case (`tests/linux_vf_write_tests.cpp`'s raw 30001
  fixture, ported here as a 15000500 Hz synthetic case) is never silently
  narrowed into a clean kHz value without the caller knowing.

## Invariants

- No field, constructor or function in these three headers reads `g_app`,
  `LinuxGpuState`, or calls a driver. A document must stay constructible,
  comparable and (eventually) exportable with no GPU selected at all.
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

- **No runtime/IPC/disk consumer yet.** This type is exercised only by its
  own pure tests. The schema migration that lets the live service/GUI/TUI
  actually build, persist and transmit a `ProfileDocument` is separate,
  later work; until it lands, nothing here changes observed product
  behavior.
- **Graphics-offset exclusion identity is simplified.** `ProfileGraphicsOffset::excludeLowCount`
  is a count, matching the live runtime's `gpuOffsetExcludeLowCount`
  exactly, not yet the per-topology-identity exclusion set the plan's §5.4/§9.1
  ask for once a destination can have different additional/missing points.
  Revisit when destination mapping (topology-identity resolution across a
  different GPU) is actually implemented.
- **Lock recognition evidence is not modeled yet.** `ProfileLockIntent`
  carries mode/anchor/target/tracking/provenance only; the bounded
  recognition evidence (anchor voltage, plateau, tolerance, tail count) a
  flatten/pin capture will need is deferred until that capture work exists,
  to avoid guessing its shape now.
- **No codec.** The portable `.gcprofile` format, its bounded parser, legacy
  adapters and destination mapping are unrelated, later work; this type is
  deliberately shaped to make that codec straightforward (see the §9.1
  section/key table it mirrors), not to replace it.

Last verified: 2026-11-12.
