<!--
SPDX-License-Identifier: MIT
Copyright (c) 2026 aufkrawall
-->

# Secret Leak Prevention

This procedure is part of the normal agent commit workflow. It applies whenever an agent is authorized to create a Git commit, independently of whether a full security audit is being performed.

The goal is to prevent credentials, private keys, tokens, sensitive configuration, private data, local diagnostic artifacts, or accidentally copied secrets from entering Git history or commit metadata.

## Sensitive material to protect

Treat at least the following as sensitive in Green Curve:

- update-signing private keys (e.g. ECDSA P-256 keys, `.pem`, `.p8`, `*signing-key*`, `*update-key*`; these must live strictly outside the repository as documented in `updates.md`);
- passwords, API keys, bearer tokens, OAuth/client secrets, GitHub tokens, access tokens, cloud credentials, and connection strings containing credentials;
- real `.env`/local configuration values (e.g. uncommitted `tool-paths.env`), credential stores, auth caches, package-manager tokens, and machine-specific secret files;
- dumps (e.g. `*.dmp`), traces, logs (e.g. `greencurve_debug.txt`, `myeasylog.log`, support logs containing Windows user profile paths), captures, VF baseline measurements (`gc_baseline_*`), screenshots, user data, or generated artifacts;
- local developer user names or private profile paths (strictly guarded by `check_no_developer_profile_paths()` in `tools/security_gates.py`);
- credentials or sensitive values embedded in source, tests, fixtures, examples, generated files, documentation, wiki pages, changelog entries, commit messages, trailers, or other Git metadata.

Public identifiers, intentionally public test fixtures (like the fuzz corpus in `tests/fuzz-corpus/` or public updater keys in `source/update_verify_keys.h`), and documented placeholders are not secrets, but verify that they are genuinely non-sensitive before committing them.

## Mandatory pre-commit check

Before every agent-created commit:

1. Inspect the complete staged-file inventory (`git status`) and relevant untracked files intended for staging.
2. Review the staged patch, not only the working-tree diff (`git diff --cached`).
3. Check for unexpectedly staged local configuration, credential files, keys, dumps, logs, captures, databases, generated artifacts, or other sensitive files. Nothing under `llm-wiki/log/` or `llm-wiki/private/` may ever be staged (they are gitignored; the build gates fail if one is tracked).
4. Run repository-provided security gates: `python build.py` executes `tools/security_gates.py`, verifying that no private keys, developer paths, private wiki areas, or prohibited artifacts are tracked.
5. When a suitable local secrets scanner such as `gitleaks` is available, run it against staged changes:
   ```powershell
   gitleaks protect --staged --verbose
   ```
6. If automated secret scanners are unavailable, the check is still mandatory: perform a targeted manual search across the staged diff for credential markers, private keys, and suspicious tokens:
   ```powershell
   git diff --cached | Select-String -Pattern 'key|secret|token|password|bearer|private|begin.*key|users[\\/]|/home/' -CaseSensitive:$false
   ```
   Record in the agent context that automated scanning was unavailable and manual review was performed.
7. Review the planned commit message/body/trailers before committing. Do not paste raw secrets, sensitive log excerpts, private URLs containing credentials, tokens, or personal data into commit metadata.
8. If the staged change touches `AGENTS.md` or any `llm-wiki/*.md` page, also apply "Public wiki safety" below to the staged patch.
9. If any suspected secret or sensitive artifact is found, stop the commit until it is removed, redacted, replaced with a safe fixture/placeholder, or explicitly established as safe to commit.

A clean scanner result does not replace staged-diff review. Secret scanners can miss custom formats, encoded values, private data, or sensitive artifacts that are not recognizable as credentials.

## Mandatory post-commit check

Immediately after every agent-created commit and before any push:

1. Inspect the exact commit that was created, including its complete patch, file list, commit message/body/trailers, and author/committer metadata:
   ```powershell
   git show --format=fuller --stat --patch HEAD
   ```
2. Confirm that the commit contains only intended task-owned files and no secret-bearing local/generated artifacts.
3. Re-run local secret scanning against the new commit if supported:
   ```powershell
   gitleaks detect --source . --log-opts="-1" --verbose
   ```
4. If automated commit scanning is unavailable, manually re-check the committed patch and metadata for credential material, developer profile paths, and sensitive values.
5. Do not push, publish, open a release from, or otherwise share a commit that fails this check.

For a multi-commit outgoing branch, run an additional secrets review over the complete outgoing range (`git log -p origin/main..HEAD`) before pushing.

## Public wiki safety

`AGENTS.md` and the `llm-wiki/*.md` topic pages are tracked in Git and published on GitHub. They obey the same masking rule as public commits and code comments. `llm-wiki/log/` and `llm-wiki/private/` are gitignored and local-only; they do not exist in a fresh clone, so a topic page must never depend on them.

Never write any of the following into a tracked page:

- **Personal data:** real names (the public handle `aufkrawall` is fine), email addresses, machine/host names, IP addresses, user-profile or home directories, and tool install locations specific to the developer machine. Use placeholders (`%LOCALAPPDATA%`, `$HOME`, `<user>`, `<tool-dir>`).
- **Secrets and key custody:** secrets or key material of any kind, signing-key file locations, key-directory ACL details, and release-operator runbook steps (the gitignored `update-procedure.md` owns those).
- **Incident narration:** what leaked, where, which versions shipped it, history rewrites, "exposed since X". State the invariant or rule the change enforces instead.
- **Open or unverified security findings:** anything unfixed, deferred or only suspected, and exploitation steps for it. Findings that are fixed may stay only as neutral design rationale ("the check and the write must name the same object"), never as an attack walkthrough.
- **Tone:** profanity, disparaging remarks about people, vendors, tools or other AI products, and self-critical post-mortem narration. Keep the technical substance, in neutral wording.
- **Third-party confidential material** (NDA content, copied proprietary documentation).
- **Third-party tool names:** never name another GPU tuning, overclocking, monitoring or fan-control application, its binaries or its authors, in any tracked file (wiki, source comments, tests, strings, workflows). Say "an external tool", and state the behaviour as our own observation or as driver behaviour rather than as analysis of another product. NVIDIA driver, NVML and NvAPI identifiers, OS components and dev/build tooling are not covered.

Where the content goes instead:

| Content | Home |
|---|---|
| Chronology, partial investigations, timelines | `llm-wiki/log/recent.md` (and its archives) |
| Incident/remediation records, leak stories, open security findings, machine-specific notes | `llm-wiki/private/` |
| Release-operator procedure | `update-procedure.md` (gitignored) |
| The rule or invariant itself, in neutral words | the public topic page |

Enforcement is layered, and none of it replaces reviewing the staged patch:

- `.gitignore` ignores `llm-wiki/log/` and `llm-wiki/private/`.
- `tools/wiki_public_gates.py` (run by `python build.py --test` and `--gates`) fails if any file under those two directories is tracked, if either is no longer ignored, if a tracked agent-instruction or wiki page contains an email address, a Windows host-name pattern, an IPv4 address outside documentation ranges, or a secret-shaped token, or if ANY tracked path or text file names a third-party tool (case-insensitive, word-bounded denylist, built from fragments so the file does not contain the names; NVML identifiers and the phrase "fan control" never match). It self-tests each rule with bad inputs.
- `check_no_developer_profile_paths()` and `check_no_signing_key_material()` already scan every tracked text file, so they cover the wiki too.
- CI scans full history with `gitleaks` (`tools/secret_scan.py`, `.gitleaks.toml`).

Before committing wiki changes, skim the staged patch for the words these rules are about (incident, leak, exposed, remediation, vulnerability, exploit, unfixed) and for names, paths and hosts, and justify or remove every hit.

## Remediation when a secret reaches a commit

If a real credential or other sensitive value is found after commit:

- stop immediately before pushing or sharing the commit;
- remove the sensitive material from the working tree and amend/rewrite the affected local commit (`git reset --soft HEAD~1` or `git commit --amend`);
- remember the public remediation rule: never name the leaked secret, the exposed value, or the affected version range in the commit subject, body, code comments, or any tracked wiki page. Describe only the rule the change enforces;
- rotate and revoke any real credential that was exposed, especially if the commit, patch, terminal output, or logs left the local machine;
- record the incident details only in the gitignored, local-only `llm-wiki/private/` (or `llm-wiki/log/`), never in a tracked topic page.

## Reporting

When reporting secret-check results:

- state which staged/commit scope was checked and which scanner or manual fallback was used;
- report suspected secrets using a redacted fingerprint or location, never the complete value;
- distinguish "scanner unavailable" from "scan passed";
- do not claim that a repository is secret-free merely because one scan found no matches.
