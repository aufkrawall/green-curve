<!--
SPDX-License-Identifier: MIT
Copyright (c) 2026 aufkrawall
-->

# Agent Instructions

Keep always-on rules here (commit gates, tool and platform precedence, stop conditions) and keep full procedures, style guides, and worked examples in `llm-wiki/`, referenced by path. Do not maintain the same rule in both places; the exception is a highest-stakes commit gate, which may stay here as a compressed checklist so it holds even when no wiki page is loaded.

## Critical workflow

- Windows-first project: prefer PowerShell 7.6, Windows-native paths, and installed project tools unless we are on Linux!
- Use the repository's declared platform priority, build system, package manager, toolchain, and pinned/project-local tools; never silently substitute another ecosystem, tool version, or globally installed tool. If declarations conflict or a required tool is unavailable, stop and report instead of working around it!
- Rebuild with `python build.py` after implementing changes!
- Always git commit after code changes!
- Before committing, run relevant tests/unit tests and ensure build/test results succeed.
- Verify the change itself, not just command exit status: prefer a check that would fail without the change; otherwise inspect the artifact directly (it exists and its content, format, or size changed as expected); otherwise read the actual output to confirm the relevant tests or build ran. If none is practical, state what was and was not verified!
- Keep large logs, generated output, traces, dumps, and minified files out of working context unless needed; inspect targeted ranges or summaries and retain full output only as evidence.
- When committing, use a concise title plus a short bullet-point body for non-trivial changes stating what changed and why!
- Split up non-trivial tasks into a series of small, self-contained commits (but don't compile every small commit!), not one large commit, so later review stays easy; each commit must independently hold together and pass verification and secret-leak checks!
- Commit completed code changes with plain git commands only: `git status`, `git add -A`, `git commit -m "<message>"`!
- Do not push to remote, generally just commit locally!
- Always consult `llm-wiki/` for code, bug, build, test, config, debugging, or behavior work!
- Keep `llm-wiki/` linted / quality-checked and updated when durable project knowledge changes!
- Always update `llm-wiki/` after code changes!
- Mistrust code, code annotations and llm-wiki! Each of them might be stale or outdated! Come to your own conclusion and act based on that!
- When fixing a bug or implementing a feature, generally always add new regression test units, or adjust existing ones!
- When fixing a bug or implementing a feature, generally always increase or improve debug logging to make bug diagnosis easier!
- Every agent-created commit must pass the mandatory pre-commit and post-commit secret-leak checks in `llm-wiki/secret-leak-prevention.md`; never push a commit that has not passed the post-commit check!

## Secret leak prevention

- Treat secret safety as a commit gate, not an optional security-audit task!
- Before committing, inspect staged/untracked task-owned files, the staged patch, and the planned commit message; run repository-provided or available local secret scanning when possible!
- After committing, inspect the exact created commit including patch and metadata, and run commit/history secret scanning when available!
- If scanners are unavailable, perform the documented manual fallback; scanner absence never means the check may be skipped!
- Stop before push on any suspected leak. Remove/redact it, rewrite affected local commits as appropriate, and rotate/revoke real credentials according to project policy!
- Never reproduce full discovered secrets in logs, reports, changelogs, issues, PRs, or commit messages!
- Treat dumps, logs, media, captures, credentials, private keys, tokens, symbols, and user data as sensitive!
- Do not commit secrets, dumps, logs, captures, private-symbol PDBs, large generated artifacts, user names or private user data!
- A remediation commit is PUBLIC: never name the thing being remediated in the
  subject, body, or a tracked code comment. No leaked value, no affected version
  range, no "this was exposed since X"! Describe the rule the change enforces.
  The incident record belongs in the gitignored, local-only `llm-wiki/log/` or
  `llm-wiki/private/` -- never in a tracked file, and never in a topic page!
- `AGENTS.md` and the `llm-wiki/*.md` topic pages are PUBLIC (tracked
  and published on GitHub). They obey the same masking rule as public commits;
  see "`llm-wiki/` workflow" and `llm-wiki/secret-leak-prevention.md`
  ("Public wiki safety")!

## Engineering rules

- Prefer root-cause fixes over workarounds; do not hide, ignore, weaken, or paper over failures!
- Perform thorough thinking about actual root causes of crashes and other issues for proper fixes!
- If the result after thorough thinking is that proper fixes require bigger changes, they generally should be implemented!
- Do not just mitigate fallout, take the hard route of proper and solid root cause fixes!
- Do not use sleeps, wait tables, polling delays, or timing bandaids as crash/race fixes!
- Do not introduce nor accept racy, timing-sensitive, or fragile behavior!
- Preserve intended features, compatibility guarantees, performance characteristics, and public contracts unless the requested change intentionally alters them.
- Keep behavioral diffs focused; do not mix unrelated formatting, generated churn, cleanup, or opportunistic refactors when they can be separated.
- Keep source files roughly 600-800 lines maximum; split up files when needed!

## Non-negotiable project constraints

- We want to keep read and write support for unsupported GPUS by default!

## Changelog and release notes

- `CHANGELOG.md` is USER-FACING release notes, not an engineering record. The
  engineering record is `llm-wiki/` and the git history; do not duplicate it here!
- Write it to be SCANNED, not read. Someone skimming only the bold lead of each
  bullet must come away knowing what area changed!
- One bullet per user-visible change, ONE LINE where possible, never more than
  two. Use topic-lead bolding (`- **Topic:** description`) or a bold sentence
  with regular-font detail; avoid all-bold bullets where nothing stands out!
- Keep a release section under ~400 words. 0.26.0 was first written at 2824!
- No nested bullets, no wall-of-text paragraphs, no multi-paragraph entries!
- Every release section uses the SAME structure: one-or-two-sentence intro,
  category subheadings (`### New`, `### Improved`, `### Fixed`, `### Removed`,
  `### Changed`, `### Security`), `### Compatibility notes`,
  `### Downloads and verification`, then the `**Full changelog:**` compare link.
  Never invent arbitrary per-release subheads!
- Say what changed FOR THE USER, never how the code changed: no internal
  symbol/file/function names, no `F-XXX` codes, no protocol/struct/field names,
  no post-mortem narration of how a bug was found or why it was hard!
- A bug entry says what went wrong and that it is fixed. It does not explain the
  mechanism, enumerate the contributing faults, or tell the story!
- Detail worth keeping goes in the commit message and the `llm-wiki/` topic
  pages (both public, so both follow the masking rule), which have no size limit
  and the right audience. Cutting it here is not losing it!
- See `llm-wiki/changelog-style.md`.

## Build, diagnostics, and tests

- Fix pre-existing, as well as newly introduced LSP errors/warnings along they way!
- We are paranoid about having sufficient regression tests, better too many than too few!
- For every bug fix or behavioral correction, explicitly assess both regression coverage and diagnostics even when existing tests pass. Strongly prefer a focused automated regression test that fails before the fix and passes after it.
- For features, cover the new contract and important edge cases when suitable test infrastructure exists.
- Add focused regression tests where possible, especially tests that would have failed before the fix!
- If no regression-test infrastructure exists for the area, consider adding suitable unit infrastructure such as GoogleTest!
- Do not add low-value tests merely to satisfy a blanket rule. If focused automation is genuinely impractical or adds little value, preserve a reproducible verification method and state why automated coverage was omitted.
- If additional regression coverage or diagnostics are deliberately not added for a non-trivial behavioral change, state the reason.
- Do not add sleeps or timing assumptions to tests!
- Check whether touched/new code has sufficient unit coverage, and add new test units accordingly!

## Debugging and logging

- We are paranoid about having sufficient debug logging!
- Add additional debug logging when it helps diagnose issue root causes, state transitions, failure modes, unexpected runtime conditions, or future regressions!
- Keep diagnostics non-secret, low-overhead, and economical to consume (human- and token-efficient): single-line entries with stable prefixes; log each distinct event once; rate-limit repeats with counters and summaries; cap collections and truncate long values (first few items plus totals, sizes/hashes instead of full bodies); keep verbose detail behind an explicit flag!
- Ensure builds preserve useful debug symbols etc. so crash dumps contain actionable information!

## Test apps and computer use

- Prefer scripted, API-, CLI-, or harness-driven verification, including scripted input and screenshots, over interactive computer use; computer use remains allowed when GUI interaction itself is what must be verified.
- Keep runs short and bounded: start with a brief duration, extend only when evidence requires it, give every started process an explicit stop condition, and never leave test apps running longer than needed.
- Own the full lifecycle: shut down everything started, including child processes, when done or on failure, then confirm nothing lingers in the background.
- Start interdependent apps in dependency order and let each signal readiness (open port, created file, health check, visible process state) before starting the next; use only a brief stagger when no such signal exists.

## Debugging and binary analysis

- Always analyze available .dmp crash dumps when they exist!
- Inspect relevant dumps, logs, traces, symbols, and produced artifacts when they can establish the reported failure or its root cause.
- Prefer project-documented debugger and symbol-path guidance.
- When `tools/discover-debug-tools.ps1` and `debug-tool-manifest.json` exist on Windows, use the manifest as machine-specific path evidence instead of duplicating SDK/MSVC discovery logic.
- Verify tool availability before relying on documented paths. Treat hardcoded paths as examples unless the repository declares them mandatory.
- Do not mutate global debugger flags, registry/system settings, binaries, symbols, or persistent environment state unless explicitly requested and justified.
- Common installed Windows tools for `.dmp` files, symbol, PE/COFF:

| Tool | Purpose | Installed/default path |
| --- | --- | --- |
| `cdb.exe` | Command-line `.dmp` debugging and stack inspection | `C:\Program Files (x86)\Windows Kits\10\Debuggers\x64\cdb.exe` (or `debug-tool-manifest.json`) |
| `windbg.exe` | Interactive `.dmp` debugging | `C:\Program Files (x86)\Windows Kits\10\Debuggers\x64\windbg.exe` (or `debug-tool-manifest.json`) |

## `llm-wiki/` workflow

- `llm-wiki/` is canonical LLM-maintained derived memory, not the sole source of truth.
- PUBLIC vs LOCAL: `llm-wiki/*.md` topic pages (and this file) are tracked in git and public. `llm-wiki/log/` (rolling log, archives) and `llm-wiki/private/` (incident/remediation records, open security findings, machine-specific notes) are gitignored and local-only; they are absent from a fresh clone, so never make a topic page depend on them. `tools/wiki_public_gates.py` (run by `python build.py --test` and `--gates`) fails if either is tracked or no longer ignored, or if a public page contains an email, host name, IP address or secret-shaped token.
- Vendor neutrality, for EVERY tracked file (docs, comments, tests, strings, workflows): never name third-party GPU tuning, overclocking, monitoring or fan-control applications, their binaries or their authors. Write "an external tool" / "another fan controller" and state the behaviour as our own observation or as driver behaviour, never as analysis of another product. NVIDIA driver, NVML and NvAPI names (including NVML fan-control policy entry points), OS components and dev/build tooling are fine. `tools/wiki_public_gates.py` enforces a denylist over every tracked path and text file (`python build.py --test` / `--gates`); if a hit sits in a user-visible string, functional code, a test name or a detection list, do not change behaviour: report it to the maintainer.
- Public-wiki safety, for EVERY edit to a topic page: no real names (the public handle `aufkrawall` is fine), emails, host/machine names, IPs, user-profile or home paths, dev-machine tool locations, key/signing-key locations or ACL details, tokens or key material, leaked values, affected version ranges or "exposed since X" narration, release-operator runbook steps, exploit steps for anything unfixed, open/deferred security findings, profanity, or disparaging remarks about people/vendors/tools. Describe the rule or invariant instead; put the story in `llm-wiki/log/` or `llm-wiki/private/`!
- For substantial work, start with `llm-wiki/index.md`, read only relevant topic pages, then read `llm-wiki/log/recent.md` for active/stale-risk areas.
- Read archives only when historical context is needed or explicitly linked.
- For trivial localized edits, skip broad wiki loading unless the area is unfamiliar or stale-risk is likely.
- If `llm-wiki/` is missing during substantial work, create `index.md`, `overview.md`, and `log/recent.md` by inspecting repo structure, build/test entry points, config, docs, and workflows. A fresh clone has the topic pages but no `log/` or `private/`; create them locally when needed (they are gitignored).
- Mistrust wiki claims until verified against code (but mistrust code too!), tests, build scripts, config, or observed behavior.
- Prefer updating existing pages over creating new ones; create new pages only for reusable topics.
- Keep topic pages focused on current best understanding; put chronology, partial investigations, and temporary notes in `llm-wiki/log/recent.md`, and incident/remediation records and open security findings in `llm-wiki/private/`.
- Mark uncertainty explicitly as open question, stale-risk, or unverified claim.
- Do not dump raw logs or long command output unless it establishes durable knowledge.
- Update the wiki when durable knowledge changes: architecture, behavior, build/test/package/deploy/debug workflows, bugs/root causes, invariants, conventions, rejected approaches, follow-ups, or code style.
- Do not update the wiki for trivial edits with no future-useful context.
- `llm-wiki/debug-tools.md` contains additional available debug commands and tool paths.
- `llm-wiki/index.md` is a compact routing table with page link, purpose, last verified date, and stale-risk.
- Durable topic pages should include summary, source anchors, invariants, diagnostics/failure modes, open questions/stale-risk, and last verified details. Open questions are verification gaps and design limits; unfixed or unverified SECURITY findings go to `llm-wiki/private/` instead.
- `llm-wiki/log/recent.md` is newest-first rolling memory; archive older entries when it gets too long.
- After both wiki updates and code changes, perform a semantic quality check for contradictions, stale claims, duplicates, orphan pages, broken links, missing source anchors, and merge/delete/archive candidates.
