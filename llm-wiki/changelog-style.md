# Changelog style

Summary: `CHANGELOG.md` is user-facing release notes. The GitHub release page is
generated from it verbatim (`release.yml` extracts the `## <VERSION>` section
with `awk`), so whatever is written here is what users read. The rule exists
because 0.26.0 was first written at 6x the house length, in dense prose, and
also dropped two sections users need.

Source anchors: `CHANGELOG.md` (its own HTML-comment header),
`.github/workflows/release.yml` (the `Write release notes` step),
`AGENTS.md` (the binding short rule), `update-procedure.md` §5.2
and the §8 checklist.

## Where the rule lives, and what each copy is for

Nothing enforces this automatically, so the rule has to be *found*. Four
placements, deliberately redundant because they fail in different ways:

| Where | Found when | Survives a fresh clone |
|---|---|---|
| `AGENTS.md` | loaded into every agent session | yes |
| `update-procedure.md` §5.2 + §8 checklist | the runbook is open during a release | no (local-only, gitignored) |
| this page (routed from `index.md`) | the wiki is consulted | yes |
| the HTML comment atop `CHANGELOG.md` | **the file being edited is opened** | yes |

The `CHANGELOG.md` header is the copy a human contributor sees without ever
opening the agent files. It is also the highest-signal placement, because it is
impossible to add a release section without having the rule on screen. Keep it
in sync with the short rule in `AGENTS.md`; it is not a duplicate to be trimmed
away.

The `update-procedure.md` placement closes the gap that actually caused 0.26.0:
the release runbook is what an operator follows, it said only "keep it
non-empty and user-facing", and nothing in it pointed anywhere else.

## The shape

Every release section, without exception:

```
## <VERSION>

<one or two sentences: what kind of release this is>

<optional: a short "Updating is recommended" paragraph when it is warranted>

### New                   <- new features / capabilities
### Improved              <- enhancements to existing features / performance / UX
### Fixed                 <- bugfixes and reliability corrections
### Removed               <- deleted features or deprecated components
(Allowed category subheads: New, Improved, Fixed, Removed, Changed, Security)

- **Topic lead:** description of what is different for the user. Alternatively,
  a bold sentence followed by regular-font detail. Avoid all-bold bullets.

### Compatibility notes

### Downloads and verification

**Full changelog:** [<prev>...<VERSION>](https://github.com/aufkrawall/green-curve/compare/<prev>...<VERSION>)
```

`Compatibility notes` and `Downloads and verification` are boilerplate; copy
them from the previous release and adjust. The compare link is not optional --
it is the escape hatch for anyone who *does* want the engineering detail.

## Size and scannability

Under ~400 words per section including boilerplate. But word count is the
symptom, not the rule. **The rule is that it must be scannable**: someone who
reads only the bold lead of each bullet must come away knowing what area changed,
with the details readable in regular font.

- One bullet per user-visible change.
- One line where possible, never more than two.
- Use topic-lead bolding (`- **Topic:** description`) or a bold lead sentence
  followed by regular-font detail. Avoid uniform lists of 100% bold bullets where
  nothing visually stands out.
- No nested bullets. A change needing sub-bullets is several changes, or is
  over-explained.
- No multi-paragraph entries, no wall of text.

| Release | Words | |
|---|---|---|
| 0.24.0 | 358 | house |
| 0.25.0 | 467 | house |
| 0.25.1 | 350 | house |
| 0.25.2 | 506 | house |
| 0.26.0 as first written | 2824 | the defect |
| 0.26.0 as shipped | 368 | the target |

A bigger release earns more bullets, not longer ones. 0.26.0 is a large release
and still fits in 368 words, because the bullets are one line each.

## What does not belong

The failure mode is not "too much information", it is **information for the
wrong audience**. Each of these was in the 0.26.0 draft:

- Internal names: `curve_semantics`, `DesiredSettings`, `absolute_with_origin`,
  protocol/struct/field names, file and function names.
- `F-XXX` defect codes. They are for the wiki and commit messages; a user cannot
  look one up.
- Post-mortem narration -- how the bug was found, which three faults lined up,
  what the watchdog was measuring instead. A user needs "this no longer
  happens", not the investigation.
- Nested bullets, and entries that run to a paragraph.
- Byte counts, record sizes, version numbers of internal formats.
- The *reason* a fix was hard. Users want "this no longer happens"; the reason
  is what the commit message is for.

## What does belong

- What a user would notice: the thing that used to go wrong, and that it does
  not any more.
- A named limit or setting they can act on (`high_oc_warn_msvdd_offset_mv`,
  `+25 mV`) -- user-facing knobs are fine, internal constants are not.
- Compatibility consequences: whether their profiles still load, whether they
  need to do anything.

## Invariants

- The section heading is exactly `## <VERSION>` matching `VERSION`; `release.yml`
  greps for it and fails the release if it is missing or empty.
- The section stops at the next `^## `, so nothing may rely on content below it.
- It must be non-empty and describe real changes since the last release.
- Detail that is genuinely worth keeping goes to the commit message and to the
  `llm-wiki/` topic pages, both of which have no size limit and the right
  audience (both are public, so both follow the masking rules in
  `secret-leak-prevention.md`). Cutting from the changelog is not losing it.

## Open questions / stale-risk

- Adherence rests entirely on the rule being read; see the placement table
  above. The `CHANGELOG.md` header is the load-bearing copy. If the other three
  ever contradict it, the header is the one a stranger will follow.
- No automated gate gets applied to this. A word-count check in `build.py` was
  considered and rejected: a chatty changelog fails *visibly* to the first human
  who reads it, unlike the invisible regressions the other gates exist for, and
  a hard limit would be gamed by writing denser jargon. Revisit only if the rule
  is broken again after being written down.
- The house sections of 0.24.0-0.25.2 are the reference shape but are themselves
  somewhat jargon-heavy ("role-derived transport deadlines", `F-APPLY-CEILING`).
  Copy their structure and length, not their vocabulary.

## Last verified

- 2026-09-26: adopted topic-lead bolding (`**Topic:** description`) across release notes to eliminate uniform all-bold lists and ensure clear visual contrast between the bold anchor and regular body text.
- 2026-09-24: adopted categorized change subheadings (`### New`, `### Improved`,
  `### Fixed`, `### Removed`, `### Changed`, `### Security`) aligned with
  CaptureEngine conventions while retaining `Compatibility notes`, `Downloads and
  verification`, and compare links. `tools/release_prep.py` validates allowed
  categories and structure during builds.
- 2026-09-17: written after rewriting the 0.26.0 section from 2824 to 368 words
  and restoring its `Compatibility notes` / `Downloads and verification` /
  compare-link tail. Rewritten twice: the first pass cut to 667 words but kept
  multi-sentence bullets, which is shorter without being *scannable* -- that is
  why the rule above is stated as scannability first and word count second. The
  binding short rule lives in `AGENTS.md`.
- 2026-09-17 (discoverability pass): the rule was reachable only from
  agent-side files, and `update-procedure.md` -- the document actually open
  during a release -- did not mention it. Fixed by adding the rules to §5.2 and
  the §8 checklist, and by putting an HTML-comment header at the top of
  `CHANGELOG.md` itself. Verified the header is excluded from the `awk`
  release-notes extraction.
