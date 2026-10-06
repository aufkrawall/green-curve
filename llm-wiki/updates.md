# In-app updater

## Release input review (verified 2026-10-01)

Source anchors: `tools/update_manifest_tools.py`, `tools/update_signing.py`,
`tools/release_post.py`, `tools/release_renew.py`, `tools/release_prep.py`,
`tools/update_manifest_tools_tests.py`.
Initial publication AND renewal require the requested version to be
`releases/latest`, before downloads and again immediately before signing and
upload. The initial publisher previously discovered a non-latest target only
through failed anonymous delivery checks after uploading; dry-run did not
discover it at all. Rechecks detect a channel change during preparation but
are not an atomic lock against concurrent remote publishers; delivery checks
remain necessary.

Release version spelling now matches the native parser: no leading zero in a
multi-digit component, each component <=999999. The shared Python manifest
validator mirrors the native manifest fields/bounds: exact filenames, positive
bounded sizes, lowercase hashes, duplicate/unknown-key refusal, complete asset
triples, and optional minimum version. It validates generated manifests before
key reads, and validates the exact published v1 bytes before renewal. A valid
signature alone does not imply that the native parser accepts the document.
No normalization of signed bytes occurs. Raw `sign`/`verify` remain generic
cryptographic helpers. No real key or publication is used in regression tests.

## Signed freshness envelope (verified 2026-10-01)

Source anchors: `source/update_freshness_policy.h`,
`source/main_service_update_state.cpp`, `source/main_service_update_worker.cpp`,
`source/main_service_update_cache.cpp`,
`tools/update_freshness.py`, `tools/update_signing.py`, `tools/release_post.py`.
New clients fetch `greencurve-update-v2.txt` and `.sig`. A canonical LF header
(`freshness=1`, `issued`, `expires`, Unix seconds) encloses the unchanged v1
manifest; the existing P-256 signature covers the ENTIRE envelope, including
all asset digests. Older clients continue fetching the separate v1 assets.
There is no unsigned or v1 fallback in new clients. Old cached v1 metadata is
refused, rather than grandfathered. No wire or persistence struct was widened.

Signature verification precedes freshness parsing and v1 parsing on both
fetch and cache restore. Issuance may be at most 300 seconds ahead of the
client clock; expiry is strict (`now < expires`); maximum lifetime is 30 days.
The download gate, staged-package re-adoption, install policy, response
projection, and final launch gate all recheck time, so a once-fresh package
cannot remain usable forever. MANUAL_REQUIRED also refuses installation of
an already-staged package and greys the GUI's Install action. Failed checks
may preserve a staged package only while its signed metadata remains fresh.

Operational invariant: `prepare` and `release_post` generate/sign/publish BOTH
formats. **Renew the v2 envelope before its 30-day expiry** for the latest
release: `python tools/release_post.py --renew-freshness [--dry-run]`
(`tools/release_renew.py`, added 2026-10-01 after review found renewal was a
manual multi-step ritual every 30 days). It refuses unless the version is
`releases/latest` and its tag matches the reviewed commit, verifies the
PUBLISHED v1 signature and that v1 names the published installers by exact
size/SHA-256 with release-workflow provenance, signs a new envelope over the
EXACT v1 bytes via `update_signing.py renew-freshness` (which never
regenerates v1), uploads ONLY the v2 pair with `--clobber`, and anonymously
re-verifies v2 bytes, signature, expiry and an unchanged v1. Covered by
`tools/release_renew_tests.py` (run from `release_post.run_self_tests()`, so
by `--gates`). Without the flag `release_post` is initial publication, which
verifies provenance before signing, validates both local signatures even in
dry-run, requires the latest release before signing/upload, and compares all
four delivered assets with local bytes. No real
release asset was signed or published by either change; renewal remains a maintainer action.

Refusal reasons are classified (`GcUpdateFreshStatus`): EXPIRED ("waiting to
be renewed"; the publisher's job, recovers on a later check), FUTURE/NO_CLOCK
("check this PC's date and time"), MALFORMED/BAD_LIFETIME (tooling produced
something this build never accepts). Before 2026-10-01 every case told the
user to check their clock. The service log line carries status, issued,
expires and now. Residuals: replay remains possible INSIDE the signed 30-day window;
REGRESSED/STALE remain notices; an attacker may withhold metadata; client clock
correctness and first-install/key trust remain required. Bootstrap freshness
is not guaranteed for old v1 clients.

Regression anchors: `tests/security_audit_tests.cpp`,
`tests/review_followup_tests.cpp` (6250-6258 status classification), the
update-manifest fuzz target, `tools/update_freshness.py` self-tests, signer
forced-zero retry, and release publication/renewal fixtures.

The update cache and staging directory resolve `%ProgramData%\Green Curve`
through `resolve_machine_config_dir()`, NOT the bank-content-gated
`resolve_machine_config_path()` (see windows-architecture.md, shared bank
content proof); only the update SETTINGS live inside the bank file and fall
back to defaults while it is unproven. The staging directory additionally
proves an Administrators owner through a no-follow handle. Live SYSTEM install/GUI close/relaunch was not
performed. Treat the historical v1 discussion below as legacy behavior.

Source anchors: `source/update_version_policy.h`, `source/update_manifest_policy.h`,
`source/update_url_policy.h`, `source/update_schedule_policy.h`,
`source/update_verify_keys.h`, `source/service_protocol_update.h`,
`source/main_service_update_state.cpp`, `source/main_service_update_fetch.cpp`,
`source/main_service_update_verify.cpp`, `source/main_service_update_worker.cpp`,
`source/main_service_update_worker_thread.cpp`,
`source/main_service_update_gui_stop.cpp`,
`source/main_service_update_guard.cpp`,
`source/main_service_update_commands.cpp`, `source/gui_update_client.cpp`,
`source/gui_update_dialog.cpp`, `source/gui_update_command_worker.cpp`,
`source/gui_update_first_run.cpp`, `tools/update_signing.py`,
`tools/release_post.py`, `tools/release_prep.py`,
`tools/update_gates.py`, `source/update_restore_policy.h`.

## Summary

Green Curve checks GitHub for a newer release, downloads the setup executable,
verifies it against a signature it can prove came from the maintainer, and — on
an explicit click — runs it silently. The **LocalSystem service** does all of
it; the GUI triggers and displays.

## The trust root, and why attestation is not it

```
embedded ECDSA P-256 public key   (private half never enters GitHub Actions)
  └─ signs greencurve-update-manifest.txt   (exact bytes)
       └─ binds version + arch + asset name + byte size + sha256
            └─ the setup executable staged on disk
```

Three candidate controls were considered; only the first is a gate.

| Control | What it actually proves |
|---|---|
| **Self-managed signing key** | The maintainer intended to ship these bytes. GitHub never held the private half, so a compromised account cannot produce one. **This is the gate.** |
| GitHub build-provenance attestation | CI built this artifact from repo X at commit Y. An attacker who can push a commit can run `release.yml` and mint a **genuinely valid** attestation for hostile code. Excellent forensics (public transparency log), not prevention. |
| The published `.sha256` files | Nothing about authenticity. Same release, same host, same account as the `.exe` — whoever can swap one can swap the other. Detects corruption and truncation only. |

Verifying a Sigstore bundle in-process was rejected: it needs DSSE parsing, an
X.509 chain to the Fulcio root, SAN workflow-identity matching, and a Rekor
inclusion proof against the log timestamp (the certs are short-lived and already
expired at verification time). That is a large pile of new untrusted-input
parsing in the highest-consequence code path in the application, in exchange for
a guarantee the signing key already dominates.

**Authenticode is a different problem and is still absent.** It governs
SmartScreen and the "unknown publisher" UAC caption. It has no bearing on the
update channel, and the update channel does not need a CA.

### Key rotation

Two slots ship from the first release that has an updater at all
(`GC_UPDATE_PUBLIC_KEY_ACTIVE`, `GC_UPDATE_PUBLIC_KEY_NEXT`); a signature is
accepted when **any** slot verifies it. Rotating: sign with NEXT → ship a build
that promotes NEXT to ACTIVE and adds a fresh NEXT → only once that build is
broadly deployed, drop the old key. **Step three is the dangerous one.**
Removing a key is what breaks clients, and a client that cannot verify cannot
update itself out of the problem.

The private keys live outside the repository. `.gitignore` covers the obvious
names and `security_gates.check_no_signing_key_material()` hard-fails if key
material ever becomes tracked, because `.gitignore` alone is defeated by
`git add -f`.

## Why the service, not the GUI

The GUI's manifest asks for `asInvoker`, so it runs unelevated. Had it done the
download, the file would have to live somewhere that account can write, and
anything else running as that user could swap it between the hash check and the
elevation prompt. `installer.md` already refuses the `%TEMP%` shape of this bug
for the same reason.

The service stages into `%ProgramData%\Green Curve\updates`, inside the
directory `ensure_machine_config_directory()` already creates and hardens
(SYSTEM + Administrators full, Users read-only). A standard user can see the
staged installer and cannot replace it.

**A consequence worth stating: there is no UAC prompt.** The service is already
SYSTEM, so a consented update is genuinely seamless.

## The order is the security property

`service_update_run_check()` then `service_update_run_download()`:

1. Fetch the manifest and its detached signature from two fixed URLs.
2. **Verify the signature over the manifest's exact bytes.** Nothing below runs
   if this fails, and in particular the manifest is *not parsed first* — the
   parser only ever sees authenticated input.
3. Parse and decide. A version not strictly newer is refused; see the downgrade
   note below.
4. Download into the staging directory, bounded by the size the signed manifest
   declared, aborting **mid-transfer** the moment it is exceeded.
5. Re-open with `FILE_SHARE_READ` (writes and deletes denied) and verify byte
   length then SHA-256 through that handle.
6. `service_update_run_install()` re-verifies through a freshly pinned handle
   and holds it across `CreateProcessW`.

Step 6 closes the time-of-check/time-of-use window: verifying by path and
launching by path would leave a moment where the bytes measured and the bytes
executed need not be the same file, in a SYSTEM process.

Steps 2 and 6 are enforced as **source order** by `tools/update_gates.py`,
because neither fails visibly when it regresses — the happy path is identical
either way.

### The downgrade refusal is load-bearing

A replayed older release is genuinely signed, genuinely attested and correctly
hashed. Neither the signature, the attestation nor the digest rejects it. Only
`gc_update_is_newer()` does, which is why `update_version_policy.h` is treated as
a security boundary rather than a display helper. Equal is not newer either:
re-installing the running version would stop the service, reset the GPU and
re-apply for nothing.

### The mix-and-match binding

Per-arch `file`/`size`/`sha256` are validated together, and the filename must be
exactly what `release.yml` produces for that version and architecture. Without
it a signed manifest could name the arm64 asset in the x64 slot, or an older
signed asset could be renamed into a newer release and satisfy a digest check
that only looked at bytes.

### The install reservation closes the apply/update race

`service_update_run_install()` used to sample the service runtime lock with a
zero timeout and immediately release it. A queued APPLY could start between
that sample and setup's launch, letting the update stop the service through a
half-finished GPU write. The updater now acquires the runtime lock, sets a
process-wide install reservation while holding it, and only then proceeds to
the GUI stop / re-verify / `CreateProcessW` boundary. Every GPU-write path
(explicit APPLY/RESET, lifecycle logon/standby/driver restore, fan runtime,
controlled-recovery restart) checks the reservation after acquiring the same
lock and defers. The reservation is cleared on every pre-launch failure and on
an installer that exits non-zero without stopping the service; after a launched
install it stays set for the remaining service lifetime.

### A failed install cannot seed a later silent restore

The GUI captures live settings before it asks the service to install, because
the service is about to close it. That capture could outlive a refused or
failed install and be replayed automatically at a later startup. The capture is
now bound to the exact release version the service advertised
(`[green_curve_update_restore] expected_version`), and replay is refused unless
the running binary is exactly that version, the capture is under 24 hours old,
and the capture still verifies. A failed command also discards the capture
immediately. The renamed `pending-update-restore.applying.ini` is deleted by
the apply helper after its one attempt.

### A manual check cannot destroy a verified staged package

Every check used to end in `IDLE`, so clicking **Check now** after a successful
download made the Install button grey itself out while the verified file stayed
on disk. A check now re-validates any staged package against the latest trusted
manifest: a match returns to `READY`, a mismatch discards the stale package and
redownloads, and a failed transport leaves a verified package in `READY` rather
than demoting it to `FAILED`.

### Relaunch targets the authenticated session, not the console

The service authorizes the active RDP session when it is the active session,
but setup's relaunch helper used the physical console only. The service now
records the authenticated caller session for the install, passes
`--launch-session <id>` on setup's command line, and the installer parser
validates it strictly. Setup launches the GUI for that exact session and never
falls back to launching with a SYSTEM environment or an elevated token: if the
user environment block cannot be built, the convenience relaunch is skipped
instead of degraded.

### Upgrade settings capture backward compatibility

The in-app updater GUI captures active GPU intent in the interactive user
session into `%LOCALAPPDATA%\Green Curve\pending-update-restore.ini` before
requesting the install. Setup itself runs in Session 0 as LocalSystem, where the
background service strictly enforces the interactive session authorization rule
(`callerSessionId == activeSessionId`) and refuses all settings export or
mutation requests.

The capture owner must be established before setup skips its own export:
- In 0.27.0+, the updater background service passes `--settings-captured-by-gui`.
- Older services (including 0.26.0) omit the marker. Silent setup accepts a
  legacy handoff only when `gc_setup_launched_by_service()` in
  `source/installer_main.cpp` establishes Session 0 and a live parent matching
  the running GreenCurveService PID from the SCM. The parent handle is held
  during the check and its creation time must precede setup's, rejecting a
  recycled parent PID. Unknown origin keeps capture owned by setup.
- `gc_installer_settings_capture_handled_by_gui()` in
  `source/installer_plan_policy.h` consumes that proof independently of
  `--launch-session`, which is only a relaunch request. This covers legacy
  `--no-launch` too and preserves manual silent setup's capture.
- `gc_capture_active_settings()` in `source/installer_apply.cpp` avoids futile
  Session 0 IPC but retains `GC_SETTINGS_CAPTURE_FAILED` and calls
  `gc_log_fail` when no handoff was established. The installation can complete,
  but the retained failure log explains that active settings were not carried.
  Session 0 does not prove that there was no active interactive user intent.

Last verified: 2026-09-27 review follow-up. Pure/parser cases 6474-6479 and
6680-6686 plus source gates cover the decision and native integration. The
focused manual-launch probe failed before the fix and passes after it. A live
elevated updater/SCM launch has not been replayed for this follow-up.

### Wire, crypto, and key-storage hardening

- `ServiceUpdateState` is now validated like every other wire block: enum
  ranges, interval bounds, boolean canonicalization, reserved bytes, and
  terminated strings.
- The verifier rejects malleable high-S ECDSA signatures, matching the
  signer's canonical low-S encoding.
- The signing tool creates keys with an owner-only Windows ACL (inheritance
  removed) and refuses to read a key unless its owner, protected-DACL bit, and
  sole full-control ACE all match the current user. The implementation uses
  native security-descriptor APIs rather than localized `icacls` text, instead
  of relying on POSIX `0600` bits that Windows ignores.
- The GUI's update-state cache is protected by an SRWLOCK and consumed as a
  snapshot, because the mutation worker and the UI thread feed/read it
  concurrently.
- The GUI-process enumeration is fail-closed: snapshot, image-path, or
  open-process failures abort the install instead of being read as "no GUI".

## No GitHub API, therefore no JSON parser

The manifest and signature are fetched from
`https://github.com/<owner>/<repo>/releases/latest/download/<fixed name>`.
Three reasons, all pointing the same way:

- A JSON parser would be a second untrusted-input decoder running **before** any
  signature check — the least defended code in the feature.
- The unauthenticated API is 60 requests/hour per IP, shared behind CGNAT or a
  corporate NAT, so scheduled checks would fail for reasons a user cannot see.
- GitHub's definition of "latest" already excludes drafts and pre-releases, so
  that requirement is met by the URL rather than by client-side filtering that
  could be wrong.

The asset URL is built afterwards from a manifest that has already passed
signature verification.

## Redirects are followed by hand

`releases/latest/download/...` redirects, and the asset is served from a
different host than the request started on, so following redirects is
mandatory — which makes the redirect target an input an attacker wants. WinHTTP's
automatic handling is **disabled** (`WINHTTP_DISABLE_REDIRECTS`) and every hop is
re-validated: HTTPS, default port, allowlisted host, no embedded credentials,
inside the hop budget. Letting WinHTTP follow and checking only the final URL
would be too late — the request would already have been sent.

`https://github.com@evil.example/...` is the case that defeats a naive
"starts with https://github.com" check; the parser takes the host as the bytes
after the last `@` in the authority precisely so it cannot disagree with the HTTP
stack about who is being called. Asserted at 4160.

No certificate-bypass flag exists anywhere in `main_service_update_fetch.cpp`,
and a gate forbids one being added. TLS is pinned to 1.2+ rather than left at
the OS default.

## The read loops sit behind a transport seam

Source anchor: `source/update_transport_policy.h`.

The two loops that consume a response -- `gc_update_read_document()` for the
manifest and its signature, `gc_update_stream_asset()` for the installer -- were
moved behind a two-function reader (`available` + `read`) plus a sink, so a fake
can drive them. The connection half stayed exactly where it was.

**Why only this half.** Session setup, TLS pinning and the hand-validated
redirect chain are the highest-consequence code in the application, and their
security-critical predicates (`gc_update_url_is_acceptable`,
`gc_update_host_is_allowed`, `gc_update_redirect_is_acceptable`) are *already*
pure and asserted. Refactoring the code that does TLS to test the loop around
them is a poor risk trade. The read loops are different: they carry a property
that is both security-relevant and completely invisible when it regresses.

**The property.** `gc_update_stream_asset()` compares the running total against
the manifest-declared size **between the read and the write**. Move that
comparison after the write, or to the end of the loop, and nothing observable
changes: the download is still refused, the digest still fails, the update still
does not install. It just writes everything a hostile server chose to send into
a directory a LocalSystem process launches executables from first.

This is why assertion 4384 measures **what the fake sink received** rather than
the return value. Confirmed by mutation on 2026-08-15: swapping the write ahead
of the check still returns `TOO_LARGE`, so a test that only checked the return
code passes it. Only the sink measurement fails.

**The seam is a transcription, not a redesign.** `available` and `read` map
one-to-one onto `WinHttpQueryDataAvailable` and `WinHttpReadData`, in that
order, with the same treatment of a zero-length read and the same chunk
clamping. A seam that "improved" the loops would mean the new tests assert new
behaviour and the refactor itself rests on review alone; this way the tests
describe what the shipped code already did. The Win32 side keeps the exact
error strings, including the `GetLastError()` values a pure function cannot
know, via `gc_update_fetch_describe()`.

The loops now run on the Linux host too, like the rest of the policy suites.

### What the seam does not buy

Measured against this feature's actual bug history, and worth knowing before
anyone extends it: of the seven findings the five live runs produced, **one**
was in the transport layer at all (the 945-character `Location` header), and
that one lives in `gc_update_http_read_location()`, which is below any
reasonable seam. The rest were policy, GUI or session-boundary bugs. The
transport has empirically been the reliable part; this closes a real hole, not
the one the feature keeps falling into.

## Live-run finding (2026-08-14): the URL buffer was too small for GitHub

The first real run of the updater failed with **"update failed: redirect
without a usable Location header"**, which reads like a server fault and was
not one.

`releases/latest/download/...` is a two-hop chain. The first `Location` is 95
characters; the second is **945** -- `release-assets.githubusercontent.com` plus
SAS parameters (`sp`/`sv`/`sr`/`se`/`sig`/`skoid`/`sktid`/`skt`/`ske`/`sks`/
`skv`), a JWT, and `response-content-disposition`. `GC_UPDATE_URL_MAX_CHARS` was
512, so `gc_update_http_read_location()` rejected the header as oversized --
correctly, by its own rule -- and the caller reported it as absent.

Raised to 4096, roughly four times the observed length, so a parameter GitHub
adds later does not break every installed client again. The bound itself stays:
an unbounded header is one a hostile server chooses the size of, and refusing an
over-long `Location` rather than truncating it is deliberate, because a
truncated URL could parse as a *different, allowlisted* one.

Two things this cost that are worth remembering:

- **No unit test could have caught it.** Every URL fixture was hand-written and
  therefore short. The regression test now uses the real 945-character URL,
  captured live and redacted to same-length filler, and asserts the buffer has
  2x headroom over it (assertion 4174). Setting the constant back to 512 makes
  it fail.
- **The error message pointed away from the cause.** "Redirect without a usable
  Location header" describes what the function saw, not why, and a client-side
  size limit is invisible in that wording. The comment at the check now names
  the measured length so the next person reading it has the number.

A client that cannot fetch cannot be fixed by an update, so this shipped
permanently into 0.23 and every copy of it must be replaced by hand.

## Live-run findings (2026-08-14, second run): two more, both ours

**1. A manual check found the update and refused to download it.** The download
gate required the automatic-check setting to be ON, on the reasoning that a user
who had not enabled checking had not agreed to the traffic. Right for the timer,
wrong for a button: pressing **Check now** *is* that agreement. The manual path
therefore found 0.24, downloaded nothing, and left **Install greyed out with no
explanation** -- the worst possible presentation of a deliberate decision.

`gc_update_download_allowed()` now takes `userRequested`. A user-requested check
downloads whenever there is something to download; the automatic path still
requires the setting. Neither changes the running system -- the file lands in a
directory a standard user cannot write, and installing remains separate.

**2. Clicking Install did nothing at all.** Two causes stacked:

- *The dialog stopped polling.* It decided whether to poll from the cached
  `phase`, which was fetched **before** the worker thread had run, so it saw
  IDLE, cancelled its timer, and never displayed whatever happened next. The
  service now publishes `workerRunning`, set **before** the thread is created
  and stamped onto the reply to the command that started it, so the very first
  response already says "busy". No race and no timing assumption -- the point of
  taking this from the service rather than inferring it client-side.
- *The install was silently refused.* `SHQueryUserNotificationState()` was
  called from the LocalSystem service, which lives in session 0 and has no
  interactive desktop, so its answer describes nothing the user can see. The
  code refused on `QUNS_BUSY`, which is exactly what a non-interactive session
  can report -- an install refused on a completely idle machine.

  That check moved to the GUI, the only Green Curve process in the user's
  session, where it now **warns** instead of refusing. It is a courtesy gate,
  not a security one (installing stops the service, returning the GPU to stock
  for a few seconds), so client-side is acceptable: a client can only make
  itself more restrictive. The gate that actually protects the hardware --
  `applyInFlight`, read from the runtime mutex -- stays in the service, where it
  is answerable. The session-0 value is still logged, so "why was my install
  refused" stays answerable and so there is data if it ever moves into a
  session-aware helper.

**The common thread in all three live findings** (this pair plus the URL buffer)
is that each was a deliberate decision that was correct in one context and
applied in another, and each surfaced to the user as silence or as a message
that pointed away from the cause. The unit tests all passed throughout: they
encoded the same wrong assumption the code did.

## Live-run finding (2026-08-14, third run): the installer command line

The install reached the setup program and did nothing. No version change, no
`greencurve-setup-error.log` anywhere -- and the absence of that log was the
clue, because setup writes one whenever a *step* fails. Nothing had failed,
because nothing had run.

The updater built:

    "<setup>" /S /D=C:\Program Files\Green Curve --no-launch

`CommandLineToArgvW()` splits an unquoted argument on spaces, so setup received
`/D=C:\Program`, `Files\Green`, `Curve`, `--no-launch`. It took the directory as
`C:\Program`, met `Files\Green` as an unknown switch, and refused the whole
command line -- exactly as `installer_cli_policy.h` promises it will ("unknown
switches are rejected, not ignored: an updater passing a typo must fail
loudly"). Exit code 3, before step one.

**The default install directory contains a space, so this failed for every
standard installation.** It was not an edge case; it was every case.

`/D=` keeps its NSIS spelling for humans, where the convention is that it comes
last and runs to end-of-line. That convention does not survive argv splitting,
which is what setup actually uses, so `/D=` is only safe unquoted when the path
has no spaces -- i.e. exactly when nobody needs it. The updater now emits
`--dir "<path>"`, the form that takes the following argv entry and can be quoted
like anything else.

The builder moved into `update_install_policy.h` as a pure function so the
string can be tested, and the test that was missing is now assertion 4260-4269:
it builds the command line, splits it with the real `CommandLineToArgvW`, and
feeds the result to the real `gc_installer_parse_options()`. Reintroducing
`/D=` makes it fail. Writing that test also caught a second flaw -- the builder
left a truncated command line in the caller's buffer when it ran out of room,
which a caller that checked the return value carelessly would have handed to a
SYSTEM process. It now empties the buffer on failure.

### What this one says about the others

Three live findings, three different mechanisms, one shape: each was a boundary
between two components that were individually correct. The URL buffer was our
limit against GitHub's real headers; the download gate was our policy against a
user's real intent; this was our string against our own parser. Unit tests on
either side of each boundary passed. **The tests that now exist all cross the
boundary** -- real signed URL, real installer parser, real CNG verifier against
the real Python signer -- because that is the only place these failures live.

## Live-run finding (2026-08-14, fourth run): setup runs in session 0

The install finally reached the setup program and got four files in before
stopping. The failure log named it exactly:

    stop: no running Green Curve window found
    ...
    FAILED: Could not replace C:\Program Files\Green Curve\greencurve.exe
            (error 5). Close any running Green Curve program and retry.

**The service launches setup, so setup runs in session 0, and window
enumeration is session-scoped.** `EnumWindows` there cannot see -- cannot even
detect -- a GUI in the user's interactive session. Setup concluded no GUI was
running, skipped its stop step, and then could not replace `greencurve.exe`
because the GUI still had it mapped. It had already replaced `LICENSE`,
`README.md` and `greencurve-service.exe`, so the install directory was left
half-updated.

This is a direct consequence of the service-launches-setup design, which exists
for good reasons (no UAC prompt, staging in a directory a standard user cannot
write). Setup's window-based stop is correct for the interactive case it was
written for; it simply cannot work from session 0.

### The service closes the GUI, because only it can

A service cannot post a window message across sessions either, so the request
travels the one channel that already crosses them: **the state every GUI polls**.
`ServiceUpdateState.guiShutdownRequested` is set, each GUI sees it on its next
poll and runs its ordinary Exit path -- releasing the tray icon, the
single-instance mutex and the service connection, none of which a
`TerminateProcess` would.

The service then verifies rather than assumes: it enumerates processes whose
**full image path** is `<installDir>\greencurve.exe` (matched by path, not by
name -- another `greencurve.exe` elsewhere on the machine is not ours to kill,
and this runs as SYSTEM), waits on their handles, escalates to
`TerminateProcess` if they ignore the request, and **fails the install closed**
if any survive. Proceeding would just reproduce the error-5 failure with a
half-copied directory.

### The relaunch flag became a measured decision

Because the service now counts the GUIs it closed, it knows whether one was
running -- so `--launch` / `--no-launch` is passed from that count rather than
guessed. Previously it was hard-coded to `--no-launch` on the reasoning that the
service could not know; it can now, so it does. Silent mode defaults `launch`
off, so the flag is always passed explicitly rather than relying on that default.

## Live-run finding (2026-08-14, fifth run): session 0 again, twice

The update **succeeded** -- 0.27 across the board, service running. Two things
did not happen: the GUI was not restarted, and the previous settings were not
restored. Both are the same root cause as the window-enumeration failure, in
two more places.

### The GUI was not restarted

Two faults stacked.

1. `service_update_stop_gui_processes()` never assigned `*closedCountOut`. It
   set it to 0 on entry and returned without updating it, so the caller always
   saw "no GUI was running" and always passed `--no-launch`. The log said it
   plainly -- `found GUI process pid=3668`, `asked 1 GUI process(es) to close`,
   `all GUI processes exited`, then `... /S --no-launch ...`. A scripted edit
   had silently failed to apply and nothing caught it, because the count only
   affects a flag whose wrong value still produces a successful install.
2. Even with `--launch`, setup could not have done it. `gc_launch_installed_gui`
   finds the interactive token via `GetShellWindow()`, which is session-scoped
   and null in session 0.

Setup now falls back to `WTSQueryUserToken` + `CreateProcessAsUser` with
`lpDesktop = winsta0\default`, which answers the same question by session id
rather than by window and works because setup runs as SYSTEM.
`CreateProcessWithTokenW` would have started the process in the *caller's*
session, producing a GUI nobody can see.

**Audit change (2026-08-14):** the service records the authenticated caller
session before it closes the GUI and passes it as `--launch-session <id>`, so
the helper no longer guesses between physical console and active RDP. If the
user environment block cannot be built, setup skips the convenience relaunch
instead of launching the GUI with SYSTEM's environment or an elevated token.

### The settings were not restored

On 2026-09-23 the updater's GUI capture began distinguishing a service-confirmed
empty active state from a failed service read or failed capture file/version
binding. A failed capture now prompts before requesting installation, because
stopping the service returns the GPU to stock. The user may explicitly proceed;
only a successful capture travels as a restore file. `source/gui_update_settings_handoff.cpp`
returns `GcSettingsCaptureResult`, and `source/gui_update_dialog.cpp` owns the
warning. The updater's session-0 setup child skips its redundant capture via
the explicit `--settings-captured-by-gui` marker in both command forms; it
cannot query the authorized user session.
This UI branch has a source gate and pure result-classification tests, but has
not been clicked through live.

The service log is unambiguous:

    service auth reject: source=client ping pid=1280 session=0 activeSession=1
                         user=NT AUTHORITY\SYSTEM
    service auth reject: source=client ping pid=4964 session=0 activeSession=1

Those two processes are setup's settings-export helpers -- the installed binary
and the payload binary, exactly the two-attempt rule. Setup ran them as SYSTEM
in session 0, and `service_caller_is_authorized()` restricts control to the
active interactive session, so both were refused and the capture recorded
`exit=1 snapshot=0`.

**The settings transfer itself is fine.** The same machinery ran successfully
39 minutes earlier during a manual install (`settings transfer: exporting active
intent gpu=475MHz mem=3000MHz ... applying restored intent`), because that setup
ran in the user's session. Only the service-launched path fails, and it fails on
authorization rather than on any of the historical transfer bugs.

### The GUI owns the transfer now, not setup

Three places could own this. Two are worse:

* **Setup**, launching its helpers into the interactive session with
  `WTSQueryUserToken`. Workable, but the export *writes* its file and the
  staging directory is deliberately admin-only, so it drags the settings file
  into a user-writable location -- and it cannot be verified without SYSTEM.
* **The service**, exporting its own in-memory intent and replaying it after the
  restart. No IPC, no session, very tempting -- and it opens a path on which the
  service writes to the GPU without a client asking, which is exactly what
  [auto-restore-policy.md](auto-restore-policy.md) exists to prevent. A
  convenience is not a good reason to open it.
* **The GUI**, which is already in the authorized session, already holds a
  service connection, and is already being closed and restarted by the update.

The GUI wins on every axis: no new privilege, no session boundary, no new
unattended-write path, and it uses the explicit-client-apply model the policy
already blesses. `source/gui_update_settings_handoff.cpp`:

1. Before sending `INSTALL_UPDATE`, the GUI exports the active intent to
   `%LOCALAPPDATA%\Green Curve\pending-update-restore.ini` and records the
   advertised release version in the same file.
2. The relaunched GUI replays only if the running build is exactly that release
   and the capture is under 24 hours old; it renames the capture to
   `.applying.ini` **first** so it is consumed exactly once, and replays it as a
   detached child on the ordinary `--apply-settings-file` verb -- adding no new
   apply logic and never blocking the window while the service comes up.
   A failed install leaves the running build on the old version, so the stale
   capture is discarded instead of applied.

Since 2026-09-23, the updater marks both setup command forms as GUI-handled,
so the session-0 child does not repeat the capture. Interactive setup continues
to perform its own capture.

Every failure degrades to "no worse than before": nothing applied means nothing
to carry; a failed export leaves the update untouched; a failed relaunch leaves
the file for the next manual start only if the new binary is running, which is
the one case where replaying it is still correct; a failed apply is consumed
and logged.

### The capture crosses a version boundary, and only the reader can be fixed

Source anchors: `source/main_shell.cpp` (the `--apply-settings-file` loader),
`source/config_profile_repair.cpp`, `source/profile_curve_semantics.h`,
`source/main_runtime_capture.cpp` (the exporter).

This is the one place in the product where **two different releases exchange a
curve**, and it runs in one direction only: the *installed, older* GUI writes
the capture, the *newly installed, newer* GUI reads it. The writer is a binary
already sitting on the user's machine. Whatever it emits is fixed history, so
every compatibility decision has to be taken on the reading side.

0.25.2 and earlier wrote the `[curve]` section as **base MHz** under a
whole-section `curve_semantics=base_plus_gpu_offset`, but only when the capture
carried a nonzero GPU offset; otherwise the section was unmarked and the values
were absolute. 0.26.0 writes **absolute MHz** under `absolute_with_origin` with
per-point `pointN_from_gpu_offset` flags. Reading a 0.25.2 capture with the
0.26.0 rule would take `point70_mhz=2322` as a frequency the user typed, apply a
curve 475 MHz below the one that was running, and report success -- every number
valid, only its meaning wrong, and visible to nobody except the person whose
overclock disappeared during an update.

The loader therefore runs the same two-step the slot loader does, in this order:

1. `restore_curve_point_origins_from_section()` -- claims the section only when
   the marker is `absolute_with_origin`, and returns false otherwise;
2. `curve_section_uses_base_plus_gpu_offset_semantics()` +
   `restore_curve_points_from_base_plus_gpu_offset()` -- the legacy add-back,
   reached only because step 1 declined.

Reconstructed points are marked `fromGpuOffset`, not typed: the base in that
file was sampled on the *old* install, so holding the rebuilt absolute is the
2026-09-17 under-load failure in [clock-transition-audit.md](clock-transition-audit.md)
arriving by a different road.

Asserted at 5200-5216 against a real INI file in the exact byte shape 0.25.2
emits. **Mutation tested 2026-09-17**: removing the `+ offsetCompMHz` add-back
fails 5207 and nothing else in the suite.

### `0.26` and `0.26.0` are the same version; the manifest text is not

`gc_update_version_parse()` treats an absent patch component as zero, so the two
spellings produce an identical triple and `gc_update_version_compare()` calls
them **equal**. A machine on either is `UP_TO_DATE` against the other, and
`gc_update_restore_decide()` accepts a capture bound to one while running the
other. The short spelling is therefore not a version-ordering hazard.

What is *not* interchangeable is the manifest's literal `version=` text:
`gc_update_expected_asset_name()` builds the setup filename from it, and the
filename must match what `release.yml` produced byte for byte or the update
verifies and then 404s -- at the one moment the user has already consented.
So `VERSION`, the tag, the asset names and `prepare --version` must all carry
one spelling, for that reason and not for the ordering one. Asserted at
4316 (equality) and 4319/4326 (filename derivation).

## Channel trust: the attack the signing key cannot answer

Source anchor: `source/update_channel_policy.h`.

Every control above answers *is this the maintainer's code?*. None answered *am
I still being told about releases at all?*, and that is the one an attacker with
network position can exploit **without a signing key**:

| Attack | What the client sees |
|---|---|
| Drop the traffic | Checks fail forever. The user is pinned on a vulnerable release and nothing says so -- the failures surface only in a dialog nobody opens. |
| Replay an old signed manifest | It verifies, it parses, and the client reports **"up to date"** while a fixed release exists. The signature, the digest and the downgrade refusal all pass, because the document is genuine. |

The replay is the worse of the two: the user is actively reassured.

### The manifest grammar is frozen, so this cannot be fixed in the manifest

For legacy v1 clients, a signed timestamp cannot be added to their manifest.
Current clients use the additive envelope documented above.
`gc_update_manifest_parse()` **refuses unknown keys** -- deliberately, and in
0.23 as well as today -- so adding any field to the manifest permanently breaks
the updater in every already-deployed client. The refusal also applies to deployed older clients.

**This is a constraint on the whole feature, not a detail of this section.** The
manifest grammar cannot gain a field until no 0.23 copies remain, which for a
tool with no telemetry means effectively never. Anything that needs new signed
data needs a new document at a new URL, fetched only by clients that know to ask
for it.

### So both signals are derived client-side

- **The high-water mark.** The highest version ever advertised by a
  *signature-verified* manifest is persisted at machine scope
  (`highest_seen_version` in the `[updates]` section). A later verified manifest
  advertising something strictly older means the channel went backwards. It is
  raised only after the signature verified -- gated as source order, because an
  attacker who could raise it at will would suppress the very signal it exists
  to produce, by advertising something enormous once.
- **Staleness.** Thirty days with automatic checking ON and not one successful
  check. With checking off, silence is the user's own decision and saying
  anything about it would be nagging them about a setting they chose. Never
  checked is new, not stale; a clock that moved backwards is a broken clock, not
  an attack.

`REGRESSED` outranks `STALE`: a successful check that came back rolled-back is a
stronger signal than silence, and reporting the weaker one would bury it.

### Neither blocks an update, and the wording says less than it knows

Refusing to update because the channel looks odd would hand an attacker a denial
of service through the mechanism meant to detect one. Both are notices.

The warning describes **what was observed, never what it means**. A withdrawn
release looks identical to a replay from the client's side, and telling a user
they may be under attack when their maintainer simply deleted a bad build is how
the next warning gets ignored.

It surfaces in the dialog *above* every reassuring line, and through its **own**
tray notification -- deliberately independent of the update alert, because a
replay produces `alert == NONE`, which is precisely the state that would
otherwise swallow it. Asserted at 4420-4449 and gated in
`check_update_is_actually_surfaced`.

### What is still not covered

- **The first install.** The bootstrap download from GitHub carries no signature
  check; whoever controls it controls the embedded key. Trust on first install,
  and unavoidable without Authenticode.
- **Signing key compromise** remains total. Two slots and a rotation procedure
  exist; the key's security is the system's security.
- **No certificate pinning.** Deliberate -- TLS interception can deny but not
  substitute, because the detached signature is the gate.

## Consent

| Action | Automatic? |
|---|---|
| Check | Only when the user turned it on. Starts **UNSET**, not ON — an upgrade from a build without this feature cannot silently begin making outbound requests, and the first run discloses that a check reveals IP, version and architecture. |
| Download | Yes, once an update is known and checking is on. It changes nothing about the running system. |
| **Install** | **Never.** `service_update_run_install()` is reachable only from `SERVICE_CMD_INSTALL_UPDATE`, which only a client sends — that is what makes the gate's `userConsented` truthful rather than decorative. |

An install additionally defers while a hardware apply holds the runtime lock or
a fullscreen application is in the foreground, because stopping the service
resets the GPU to stock on the way down.

**Path-risk consent (2026-09-22).** Installing *to a location* is a separate,
older consent and stays where it belongs: setup's interactive folder page asks
for an explicit acknowledgment when the chosen folder is not protected like
Program Files (see `installer.md` "Where may it be installed?"). The silent
path this updater drives deliberately never blocks on that acknowledgment —
whoever chose the path was the decision-maker, and an unattended machine must
never stall behind a dialog in session 0. Silent runs log the classification
instead, and the GUI surfaces it as a standing warning.

## Protocol v19: triggers, never payloads

Four commands (`GET_UPDATE_STATE`, `CHECK_FOR_UPDATE`, `INSTALL_UPDATE`,
`SET_UPDATE_POLICY`) and a `ServiceUpdateState` stamped onto responses by the
same single place that stamps `outcomeSeverity` — so the GUI and tray learn
about an update through polling that already happens. Since 2026-08-29 that
stamp lives inside the `stateEnvelopeAuthorized` gate together with
`populate_service_state_response()`: the envelope publishes the machine's
update posture (available version, staged/verified flags, install phase), so a
caller that failed the active-session/PID/integrity gates no longer receives
it. `check_update_envelope_is_authorized_only()` in `tools/update_gates.py`
pins the stamp inside that gate, because moving it back out leaves the happy
path unchanged and nothing else would fail.

The entire client-supplied surface is two bounded integers on
`SET_UPDATE_POLICY`. `service_request_reject_reason()` refuses a path, settings,
a target GPU, a profile identity or preconditions on any update command. A
command that accepted "which file" from an unprivileged GUI would be a local
privilege escalation however carefully the named file were then checked.

An out-of-range interval is **refused, not clamped**: the clamp exists for a
hand-edited config file, whereas a request is machine-written and a bad one means
the client is confused.

## Settings are machine scope

One machine has one installation, one service and therefore one update policy.
Per-user settings would let two accounts disagree about whether a shared service
may make outbound requests, a question with no defensible answer. They live in
the `[updates]` section of the protected
`%ProgramData%\Green Curve\shared-profiles.ini`.

## UI

- One **Updates** button, sharing the right edge of the button row with License
  and anchored as the full pair so the two stay adjacent at every width. It is
  outlined in `COL_PENDING` orange when an update is waiting — see below.
- The dialog owns status, Check now, Install, Releases page and the auto-check
  toggle. It polls only while the service is working.
- A tray entry that appears **only** when there is news, so the menu never grows
  a permanently greyed item. It opens the dialog rather than installing — one
  click in a context menu should not stop the service and drop the GPU to stock.
- **No new tray icon theme.** Five already exist for fan/OC/pending state and an
  update axis would multiply them; the button, tooltip and menu entry carry it.
- A portable `.7z` copy is told *why* it cannot install rather than being shown a
  greyed button.

### Telling the user, which for two releases it did not do

Source anchors: `source/update_presentation_policy.h`,
`source/gui_update_client.cpp`, `source/tray_presentation.cpp`,
`source/ui_theme_button.cpp`.

Until 2026-08-15 the *only* surface that mentioned an available update was the
tray context menu. The main window's Updates button, the tray icon and the tray
tooltip were byte-identical whether the machine was current or had a verified
installer staged and waiting. A user who never right-clicks the tray icon —
most of them — was never told anything at all.

Three surfaces now answer to one decision, `gc_update_alert_kind()`:

| Surface | What it shows |
|---|---|
| **Updates button** | `COL_PENDING` orange border and label, the same F-PENDING accent the fan-curve button uses for "there is something in here you have not dealt with". Reusing that colour rather than inventing one keeps the window to a single unfinished-business signal. |
| **Tray tooltip** | ` \| Update 0.30`, or ` \| Update 0.30 (manual)`. Appended to *every* tooltip shape including the in-flight and service-down ones — an update does not stop existing because a write is running, and those are states a user lingers on. |
| **Tray menu** | `Update to 0.30...`, or `Get update 0.30 (manual install)...`. |

Two decisions in that table are deliberate and easy to get wrong later:

- **`MANUAL_REQUIRED` alerts.** Newer, but below the release's `min_from` floor,
  so it must be fetched by hand. It was previously mentioned *nowhere* outside
  the dialog, which made the one update the user is genuinely stranded on the
  quietest thing in the feature. It gets different wording because offering
  "Update to 0.30..." for something the updater will then refuse to install
  would be a worse lie than the silence it replaced.
- **`NO_ASSET` does not alert.** "No build for this machine's architecture" is
  our packaging fault and there is no action the user can take. A daily badge
  nobody can clear is nagging, not informing. The dialog still says it.

A portable `.7z` copy is **not** filtered out, though a comment in
`gui_update_client.cpp` used to claim it was. The code never did that, and the
code was right: both surfaces open the dialog, which explains the portable case
and offers the releases page. Filtering would make portable users the only ones
never told anything.

The button is owner-drawn from a value that changes on a *service response*
rather than on any window message, so nothing would otherwise invalidate it.
`gui_update_refresh_alert_presentation()` keeps a last-painted mirror in
`g_app.updateAlertPainted` and invalidates only on the transition — the same
idiom `ui_checkbox_state.h` uses, for the same reason: an owner-drawn control
stores nothing that could be asked instead. It is called from
`update_tray_icon()`, which already runs on every poll tick (1 s visible, 3 s in
the tray), *before* that function's no-tray-icon early return — the button must
repaint whether or not a tray icon exists.

### The tooltip suffix wins the space, and the base is truncated

`NOTIFYICONDATA::szTip` is 128 characters and the profile name in there is
user-chosen. `gc_update_compose_tray_tooltip()` therefore reserves the suffix
and truncates the *base*, which is the inversion of what looks natural. The
reason: the base describes state the window also shows, while the suffix is the
only passive notice that an update exists at all. Letting a long profile name
push it out would reintroduce exactly the silence this whole section exists to
end. Asserted at 4345-4346.

### `UNSET` is now a question, not a permanent state

`update_schedule_policy.h` always said the first run "has to ask (or at minimum
state) before the first request goes out". **Nothing ever asked.** The only
place the unset state surfaced was a line of body text inside the Updates
dialog, so a user who never opened that dialog got no checks ever and no hint
that updates existed — and `UNSET` is the shipped default, so that was
everybody. The feature was, for its first two releases, effectively opt-in via a
dialog nobody had a reason to open.

`gui_update_maybe_prompt_first_run()` asks once, from the main window's poll
tick, and records the answer through the ordinary `SET_UPDATE_POLICY` command.
Each gate exists to stop it being obnoxious:

- **the service answered** — otherwise the answer cannot be saved and the single
  question is spent for nothing;
- **the window is visible and enabled, and no apply is in flight** — a logon
  start goes straight to the tray, and ambushing somebody watching their desktop
  appear is how an updater earns distrust. It waits until they open the window;
- **not already asked in this process** — a failed save must not turn the next
  poll tick into a second dialog.

A *dismissed* dialog is not recorded as "no": it is no answer, and consuming the
machine's one question with it would be wrong. The preference stays `UNSET` and
the next GUI start asks again.

The default itself is unchanged — nothing checks until a human says so. What
changed is that the human is now asked.

## Surviving a restart: the manifest cache

Source anchor: `source/main_service_update_cache.cpp`.

Everything the last check learned lived only in `GcUpdateRuntimeState`, which is
process memory. Only the *policy* was persisted (`auto_check`,
`interval_seconds`, `consecutive_failures`, `last_check`). So any restart —
reboot, SCM restart, controlled recovery — reset `decision` to `REJECTED` and
`manifestValid` to false, **while `lastCheckUnix` survived**, which kept the next
automatic check up to a full interval away.

Concretely: an update found and staged at 10:00, a reboot at 10:05, and a
machine showing no update anywhere until 10:00 the *next day*. On a machine
rebooted daily an update could stay unadvertised indefinitely. The verified
installer meanwhile sat orphaned in the staging directory and was deleted and
re-downloaded by the next check, because nothing knew it was there.

### The conclusion is not what is cached

Caching "decision = AVAILABLE, version = 0.30" would make a file in
`%ProgramData%` authoritative over a signature, and it would not fail visibly:
the badge would appear, the tray entry would appear, and only the install would
refuse — leaving a user with an alert they cannot act on and no explanation.

So the cache stores the two documents the check fetched, byte for byte, and the
restore re-runs **the same gate in the same order**: verify the detached
signature over the manifest's exact bytes, only then parse, only then decide.
Every invariant above holds identically for a cached manifest and a fetched one,
including the downgrade refusal.

The decision is **recomputed, never restored**, against whatever `APP_VERSION`
is running by then. That is what makes the cache self-correcting: the same
cached manifest that advertised 0.30 to a 0.23.1 machine answers `UP_TO_DATE`
once 0.30 is the running binary, and the leftover installer is swept.

A staged package is re-adopted only after `gc_update_select_asset()` names it
from the freshly verified manifest (never read off disk) and
`service_update_staged_package_matches_manifest()` re-hashes it through a
write-and-delete-denying handle. The install still re-verifies through a freshly
pinned handle before `CreateProcessW`, so this is a "must we download it again?"
question, not a trust decision.

### Where the files live, and why not atomically

`%ProgramData%\Green Curve\update-manifest.cache` and `.cache.sig`, beside
`shared-profiles.ini` — **not** in the staging directory, because
`service_update_clear_staging()` deletes every file in there before each
download and a cache a download erases is not a cache. The same protected DACL
is re-applied to each file.

The two writes are not a transaction. A crash between them leaves a new manifest
beside an old signature, which fails verification, and a failed restore deletes
both and continues exactly as if no cache existed. Every torn state is
fail-closed into "no cache", so there is nothing for atomicity to buy. The
manifest is written *first* on purpose: the reverse order's torn state pairs an
old manifest with a signature that genuinely covers it, silently pinning the
cache a release behind.

## Release procedure

The workflow cannot sign — that is the point.

1. Run the Release workflow. It builds, attests and publishes the binaries.
2. **Download the published setup executables** and sign *those*:
   ```
   gh release download <V> --pattern '*-setup.exe' --dir relbits
   python tools/update_signing.py prepare --version <V> --dir relbits \
       --key <path to the offline private key>
   ```
   It writes the manifest and `.sig`, verifies its own signature before writing
   anything a client could fetch, and prints the public key so it can be checked
   against `update_verify_keys.h`.

   Signing a *local* build instead would be a mistake even when the build is
   reproducible: the manifest has to describe the bytes GitHub actually serves,
   and any difference makes every client refuse the download after having
   fetched it.  Hashing the published artifacts removes the question.
3. `gh release upload <V> greencurve-update-manifest.txt greencurve-update-manifest.sig`

`update_signing.py prepare` deliberately does not upload. Since 2026-09-27,
`tools/release_post.py` provides a separate command that automates downloading,
checksum/provenance checks, signing, uploading, and anonymous verification.
Its `--dry-run` performs the same provenance prerequisites and local signing,
then stops before upload and anonymous delivery checks.

### Release automation provenance gates (2026-09-30)

Sources: `tools/release_post.py:reviewed_release_commit`,
`remote_release_commit`, `verify_installer_provenance`, `run_post_release`,
`tools/release_post_tests.py`, `tools/release_manifest.py:check_all`, and
`.github/workflows/release.yml`. The CLI's enforcement flags are documented in
[GitHub's attestation manual](https://cli.github.com/manual/gh_attestation_verify).

- Resolve the reviewed commit from the local `refs/tags/<version>^{commit}`,
  or an explicit `--source-commit` containing a full 40-character SHA. Missing
  local tags require that explicit input; do not silently trust a moving branch
  or infer the reviewed commit from remote metadata.
- Resolve GitHub's actual tag reference and peel annotated tags, bounded to 16
  objects with cycle/type checks. Require equality to the reviewed commit;
  release `targetCommitish` can be a branch and is deliberately not used.
- Require the x64 and ARM64 installers and both checksum assets. Download only
  those version-specific names; require both files and valid matching checksums
  before proceeding. A partial release cannot become a signed partial manifest.
- Verify both installers with `gh attestation verify --repo ... --source-digest
  <reviewed SHA> --signer-workflow <repo>/.github/workflows/release.yml` before
  invoking `prepare`. Refusal on either architecture stops before signing or
  upload. The release workflow enforces the same source/workflow constraints
  for its artifacts; build source gates preserve those arguments.
- Anonymous post-upload signature, byte-equality, host, size, and hash checks
  remain delivery verification. Release-body mismatch still warns rather than
  failing; that policy was not part of the provenance correction.

The 25 deterministic tests in `tools/release_post_tests.py` execute production
manifest construction with mocked commands/network/signatures and run through
`release_post.run_self_tests()` in the normal build regression gates. The first
17-test suite failed against the original implementation (11 failures, 4
errors); the completed suite passes. Coverage includes rejection before
signing/publication, both architectures, wrong source/workflow, local/remote tag
identity, annotated and cyclic tags, explicit SHA input, missing assets/files,
checksum failures, dry runs, local signature refusal, and delivery corruption.
The ignored `audit/release_post_review_probe.py` is historical pre-fix evidence,
not a current acceptance test.

Real read-only CLI verification passed for both published 0.27.0 installers at
the reviewed release commit; intentional wrong-source and wrong-workflow checks
were rejected. The sandbox denied writes to the ordinary GitHub CLI TUF cache,
so verification used a child-process-only `LOCALAPPDATA` override under ignored
`audit/`. No real signing key, upload, or live installer execution was used at the time.
Attestations remain forensic evidence rather than a replacement for the offline
signing trust root. Full post-publication execution of the corrected command
was performed live for the first time by the 0.28 release on 2026-10-05
(dry run first, then download/provenance/sign/upload/anonymous delivery),
see "Last verified"; `--renew-freshness` remains the one path with no live run.

#### Key-file ACL gate

`_verify_private_key_permissions()` in `tools/update_signing.py` refuses to read
a private key file unless its DACL is the native owner-only form (one owner, one
full-control ACE, inheritance removed); `_harden_private_key_file()` creates
that form and runs on `gen-key`. The check only fires on read, so a key created
before the check existed is never hardened until something reads it, which is
the shape of a gate that only fails on a path nobody has exercised yet. Repair
is to harden that one file, then confirm the derived public key matches
`GC_UPDATE_PUBLIC_KEY_ACTIVE` before signing. A rollover key must pass the same
gate before a future rotation. Loose ACLs on a key file are usually a
machine-configuration problem (inherited ACEs from a parent directory), so
check the parent as well as the file.

`tools/windows_key_acl.py` owns the correctly sized `TOKEN_USER` query plus
exact native owner/DACL construction and verification. It explicitly assigns the
token user's SID as owner because an elevated hosted token may default ownership
to its token-owner group, and the hardener removes unrelated explicit ACEs. The
build self-test runs a real key create/read and widened-ACL repair round trip on
Windows.

### Testing a release without publishing one

`GC_UPDATE_REPO_OWNER`/`GC_UPDATE_REPO_NAME` are compile-time constants, and a
pre-release cannot be `latest` by construction, so there is no way to exercise
the fetch/install path on the real repo without shipping a real release to real
users. The route that works is a throwaway **public** repo (private repos 404
for the anonymous fetch) plus test binaries built with `GC_UPDATE_REPO_NAME`
repointed, published one hop at a time so `latest` advances in the order the
test wants.

Two things this must get right or it proves nothing:

- **The client under test is built from the released tag, not from HEAD.**
  Shipped 0.23 differs from HEAD by ~2400 lines in the updater. A HEAD→HEAD
  test proves the new code updates itself, not that the copies in users' hands
  can be updated — which is the one property no later release can fix.
- **The repointed constant must never be committed.** `update_gates.py:147`
  requires only that the identifier exists, not what it holds, so nothing would
  catch a test constant shipping. Build from a detached worktree with the
  change uncommitted, and verify the string in the built binary both ways
  (test repo present, real-repo release URL absent).
- **Automated 2-hop simulation (`tools/simulate_release.py`):**
  Formalizes the throwaway clone-repo runbook into a repeatable, safe sequence:
  1. `init`: Validates signing key ACL, gh auth, and sets clone repo (`aufkrawall/green-curve-update-test`) visibility to public.
  2. `build-baseline`: Checks out stable tag (e.g. `0.27.0`) in an isolated worktree, repoints `GC_UPDATE_REPO_NAME`, and builds the installer for manual installation by the operator.
  3. `publish-hop1`: Checks out candidate HEAD in an isolated worktree, repoints repo, signs v1 + v2 manifests with offline P-256 key, and publishes release candidate to the test repo. The installed client detects and applies the update, verifying settings transfer.
  4. `publish-hop2`: Builds and publishes follow-up patch to test repo. The newly upgraded client detects and applies the update, verifying that the new updater code has not regressed.
  5. `teardown`: Deletes simulation releases/tags and reverts test repo visibility to private.
  Runbook anchored in `update-procedure.md` Section 10; unit tests in `tools/simulate_release_tests.py`.

Everything else is repo-independent and therefore faithful: asset names carry
the version and arch but not the repo, and the host allowlist is unchanged
because a fork's assets are served from the same three hosts.

Until step 3 completes, clients get a 404 and report "no published update
manifest was found". That is the intended failure mode — a release with no
manifest is simply not offered.

### Pre-releases are excluded by the URL, not by client-side filtering

`releases/latest/download/...` resolves to what GitHub calls the latest
release, and GitHub's definition of that is *the most recent non-draft,
non-prerelease release*.  A release marked pre-release can never be "latest",
so marking one is sufficient to keep the updater away from it.  Nothing in the
client needs to filter, which is the point: a requirement met by the URL cannot
be got wrong by a parser.

Two consequences worth knowing:

- If the newest release is a pre-release, `latest` still points at the previous
  **stable** one, so stable users are offered that and nothing changes for them.
- A user already running the pre-release is *not* dragged back to the older
  stable: `gc_update_decide()` refuses anything not strictly newer, so they see
  no update until a stable release overtakes their version.

### The first release carrying the updater bootstraps nothing

0.22.2 and earlier have no updater, so nobody auto-updates *to* the first
release that has one; it is installed by hand like every release before it.
The feature only starts doing work for the release after it.

## Why the signer is pure standard library

`tools/update_signing.py` implements P-256 with RFC 6979 deterministic nonces on
`hashlib`/`hmac` alone. This is the one operation that touches the private key,
so a pip-installed wheel in that path is a supply-chain surface aimed at the
thing whose compromise breaks every other control. Determinism also makes it
testable against the published RFC vectors instead of only against itself.
Signatures are raw `r||s` so the C++ verifier needs no DER decoder.

`--self-test` checks the RFC 6979 A.2.5 vectors and cross-checks against
`cryptography` when that package happens to be installed — a free second
opinion, not a dependency.

## Invariants

1. The manifest's signature is verified before the manifest is parsed.
2. A version not strictly newer than the installed one is never offered.
3. An asset's name, size and digest are validated together, and the name must be
   the one the release workflow produces for that version and architecture.
4. Every request and every redirect hop is HTTPS, default-port, allowlisted, and
   free of embedded credentials.
5. The staged file is written into a directory a standard user cannot write, and
   is re-verified through a write/delete-denying handle held across process
   creation.
6. No update command carries a path, a version, a digest, settings, a GPU or a
   profile identity.
7. An install happens only after an explicit client request; no timer reaches it.
8. Private signing key material is never tracked.
9. The GPU install reservation is released on **every** path except a successful
   install (where setup is stopping this service anyway) and a setup process the
   kernel refused to terminate. It is never held on a timer.
10. Nothing that can fail happens after the user's windows have been closed: the
    installer command line is built and UTF-16-encoded **before** the GUI stop,
    and the measured count only selects between two already-valid strings.
11. Pre-install process enumeration distinguishes "already gone" from "refused
    us a handle". A PID that died mid-enumeration is not a failure; anything
    else, including a live process we cannot name, fails closed.
12. A cached manifest is verified before it is parsed and decided on, exactly as
    a fetched one is. No conclusion is ever restored from disk; only the signed
    documents are, and the decision is recomputed against the running version.
13. An update the user can act on reaches at least one surface that does not
    require a click to find. Gated by
    `update_gates.check_update_is_actually_surfaced()`, because silence is the
    one failure mode with no crash, no log line and no failing test.

### The reservation's precondition is made false, not waited out

`GC_UPDATE_INSTALL_ABANDON_TIMEOUT_MS` is not a grace period during which the
GPU is hopefully safe. The reservation's only justification is "setup might be
part way through replacing files", which is exactly co-extensive with "the setup
process is alive" — so at the timeout the process is **terminated**, and the
reservation is released only once the kill is confirmed. Any fixed grace period
would be wrong in one direction or the other; this has no tunable at all.

This matters more than it looks because the flag gates the fan runtime pulse,
all three auto-restore paths, Apply, Reset and the controlled restart — and the
fan thread stamps its heartbeat *before* taking the lock, so a stuck reservation
looks healthy to the watchdog and is never recovered. The earlier form released
it only when the installer had been *observed to exit*, which left one hung
installer disabling GPU and fan control for the remaining life of the service.

### Setup relaunches the GUI even when the install fails

The updater closes every window before setup starts, and the service does not
launch processes into interactive sessions — so reporting a failure into a GUI
that has been closed reports it to nobody. `gc_run_silent_install()` therefore
relaunches on `plan.launchAfterInstall` regardless of the install result;
`gc_launch_installed_gui()` refuses when `greencurve.exe` is absent, so a
half-copied installation starts nothing rather than something broken.

## Live-run findings (2026-08-17): the fork test, 0.23 -> 0.23.1 -> 0.23.2

The first run against a throwaway repo with a client built from the **released**
0.23 tag. Both hops installed successfully; the service-side WinHTTP client, the
redirect chain, the protected staging directory, the verify-then-launch sequence
and the silent install all worked on real hardware for the first time. Four
things were wrong, and the shape of all four is the same as every earlier round:
each was invisible from inside the program.

### 1. Every existing 0.23 machine loses its settings on its first update

`update_restore_policy.h` did not exist in 0.23, so a 0.23 capture carries no
`expected_version` key. `gc_update_restore_decide()` parsed the missing value,
got an invalid version, and discarded:

    update handoff: discarding the pending restore
    (captured for version '<none>', running 0.23.1, age -1s)

Hop 2 restored correctly because both sides were the new code — which is exactly
why a HEAD→HEAD test would have passed and proved nothing. **This is the finding
that justified building hop 1 from the released tag.**

It cannot be fixed on the writing side: the writing side is a binary already
installed on every user's machine. `gc_update_restore_is_legacy_capture()` now
recognises an absent key as a pre-binding capture and falls back to freshness,
with a **one-hour** window rather than the version-bound day, because with no
version to compare freshness is the entire gate. A present-but-unparseable
version is still refused — corruption and history must not collapse into one
case. Asserted at 4279-4286.

### 2. The manifest cache had never written a single file

    update cache: cannot write C:\ProgramData\Green Curve\update-manifest.cache:
    Path is outside the caller's profile directory

`main_service_update_cache.cpp` wrote through `write_text_file_atomic_service()`,
chosen (per its own comment) for the reparse-point check — and that helper also
confines the write to the **calling client's profile**, which is right for a path
a client named and wrong for `%ProgramData%\Green Curve`, which is inside no
user's profile. So the feature added in `a12fac5` was a no-op from the day it
shipped, on every machine.

Nothing surfaced because the restore path is deliberately written to treat a
missing cache as the ordinary state of a machine that has never checked. A cache
that never stored is indistinguishable from a cache that was never needed.

The writer now takes a `GcServiceWriteScope`. The machine scope is a **different
containment root, not an absent one**: the write must still land inside the
machine config directory, verified through the reopened handle exactly as the
profile scope is. Gated by `check_machine_state_is_machine_scoped`.

#### The first fix was wrong in a way nothing could have caught

Round 2 log, on the build that contained the fix:

    update cache: cannot write ...: Path is outside the machine configuration directory

The scope plumbing was correct; the **root** was wrong.
`resolve_service_machine_data_dir()` reads like the right function and returns
the service account's `%LOCALAPPDATA%\Green Curve`, not
`%ProgramData%\Green Curve`. So the cache was refused a second time, with a
different message and the identical outcome.

Neither the unit tests nor `check_machine_state_is_machine_scoped` could see it:
both assert that a machine scope is *used*, and neither can know which directory
a runtime resolver returns. It was found only because the dialog logging added
in the same commit printed the error on every check. Anything comparing against
"the machine config directory" must resolve it through
`resolve_machine_config_dir_w()`, the same way the writer does.

The containment helper now also strips trailing separators from the root: a root
ending in a backslash makes every child fail the boundary test, and that refusal
is indistinguishable from a real containment violation.

**The cache has still never completed a round trip.** Two silent failures deep,
this remains the least-proven part of the feature.

### 3. The tray said nothing unless right-clicked

The "no new tray icon theme" decision stands, but it left the tray with no
unprompted signal at all: the tooltip suffix needs a hover, the orange button
needs the main window open, and the ordinary way to run this app is minimised
with the window closed. The menu entry — the one surface that did carry it —
requires a right-click nobody has a reason to perform.

`gc_update_should_notify()` + `gc_update_compose_notification()` raise a silent
balloon on the NONE→alerting edge, once per process, and only once a tray icon
exists to carry it (refused *without* consuming the one-shot otherwise, so it is
deferred rather than spent). Per process rather than persisted: a machine
rebooted daily should be told again, because the update is still waiting.
Asserted at 4400-4419.

### 5. Confirmed live: 0.23's greyed Install is a real user trap

Round 2, 2026-08-17. Pressing **Check now** a second time after a successful
download left `phase=IDLE` with the verified package still staged, and 0.23's
enable rule is `phase == READY && packageVerified && isInstalledCopy`, so Install
greyed out permanently:

    16:03:43  update state: phase=4      (downloaded + verified)
    16:03:56  update state: phase=0      (second check, ends IDLE)
    16:04:03  update state: phase=0      (and every check after)

No later check recovers it, because `gc_update_download_allowed()` refuses to
re-download something already staged, and nothing else sets READY. The only ways
out are a service restart or a reboot, neither of which a user would guess.

This is the bug "A manual check cannot destroy a verified staged package"
already describes, and it is fixed twice over in the current code -- the check
re-validates the staged package back to READY, and the dialog's `ready` no
longer tests the phase at all. What round 2 adds is that it is **not
theoretical**: it is reachable by a user doing something entirely reasonable
(pressing Check now again to see whether anything changed), and it is present in
the only release the public has. Anyone stuck on it must restart the service
before they can install anything.

### 4. The Updates dialog stopped refreshing whenever the service was idle

Reported as "the button has an orange outline, but clicking it does not show the
available new version until manually checking". The old rule polled only while
the service was busy, on the assumption that the only thing that changes update
state is a job this dialog started — false for an automatic check, a re-adopted
staged package, or a service restart. The dialog now refreshes for as long as it
is open.

**Superseded 2026-09-11 (F-PIPE-DEADLINE):** that refresh is no longer a poll.
It sent a blocking `SERVICE_CMD_GET_UPDATE_STATE` from `WM_TIMER` every 700 ms
with a 5000 ms deadline, on the thread that pumps the main window -- and an
update command shares the service's ONE serialized dispatch lock with a full
`GET_SNAPSHOT` (measured p90 1140 ms, max 1968 ms), so the whole UI stopped
repainting for the duration. The timer now only re-projects the shared cache
onto the controls and sends nothing at all. Nothing is lost: the service stamps
`ServiceUpdateState` onto every response and `gui_update_note_response()` feeds
that cache from inside the transport, so the coordinator's once-per-second read
already keeps it current. The poll was a second code path for data that was
already arriving.

**The precise mechanism behind the report is still unconfirmed**, because the
dialog logged nothing whatsoever, so there was no way afterwards to tell which
branch of `gui_update_status_text()` ran or whether the state it read matched
the service's. It now logs the rendered status line together with every field it
was derived from, change-gated so an open dialog does not write a line every
700 ms. If it recurs, that line answers it.

## Update commands run off the GUI message thread (F-PIPE-DEADLINE, 2026-09-11)

`source/gui_update_command_worker.cpp` owns the blocking sender and a detached
worker thread; `gui_update_dialog.cpp` only dispatches.

- **One command at a time**, claimed with an interlocked flag rather than
  relying on the buttons being greyed. `gud_refresh_controls()` ORs
  `gui_update_command_active()` into its busy state, so Check/Install grey while
  one is on the wire.
- **The completion is posted to the MAIN window**, not the dialog
  (`APP_WM_UPDATE_COMMAND_COMPLETE`). The main window outlives every dialog, so
  a completion can never land on a destroyed or recycled dialog HWND, and
  closing the dialog mid-command waits for and cancels nothing. The handler
  tolerates `g_updateDialog.hwnd` being null.
- **The thread is detached on purpose**: nothing joins it, it owns only the heap
  it allocated, and the in-flight claim is released on the message thread when
  the completion is handled -- or on the worker if the post failed.
- **Optimistic UI is reconciled on the completion.** A refused
  `SET_UPDATE_POLICY` puts the auto-check box back; a refused `INSTALL_UPDATE`
  discards the pre-capture (`settingsCaptured` travels with the request,
  because the call site can no longer see the answer).
- **Failures the user did not ask for are not boxed.** `reportFailure` is false
  for the first-run preference and the cold-cache fetch; those only log.
- **Deadline:** these use `service_send_request_split()` with the health-probe
  response budget, not a number chosen to bound a freeze -- there is no freeze
  left to bound.
- **Gates** (`tools/update_gates.py`): the payload-free `forbid_text` list now
  covers `gui_update_command_worker.cpp` and `gui_update_first_run.cpp` as well
  as the dialog and client; `CreateThread` is required in the worker; and
  `gui_update_send(` is forbidden in the dialog, so the blocking call cannot
  come back to a window procedure.
- `gui_update_first_run.cpp` holds the once-per-machine auto-check question,
  split out only for the source-size guideline and included at the end of
  `gui_update_dialog.cpp` because it calls `gui_update_set_policy()`.


## Fans during an install (2026-09-24)

The install reservation blocks every GPU write including the fan pulse
(`service_update_install_release_and_block`). A curve or fixed fan therefore
froze at its last manual duty until setup stopped the service -- normally
seconds, but up to `GC_UPDATE_INSTALL_TIMEOUT_MS` (10 min) if setup stalls, for
example under an antivirus scan of the downloaded installer.

`service_update_hand_fans_to_driver_for_install()` (in
`main_service_update_guard.cpp`, under the same runtime-lock hold that sets the
reservation) now captures the runtime with the pure
`update_install_fan_policy.h`, stops it with driver-auto restore, and logs
whether auto was confirmed. Every non-success exit releases the reservation
through `service_update_release_install_reservation()`, which restarts the
captured runtime (`update_install_fan_restore_plan`: only once released, never
twice). A successful install needs no restore: the graceful service stop
returns the fan to the driver anyway. Tests 6320-6332; gates in
`update_gates.check_install_reservation_and_restore_gate` /
`check_install_failure_recovery` (the worker may no longer call
`service_update_set_install_reserved(false)` directly).

**Rejected, 2026-09-24:** launching setup `CREATE_SUSPENDED` before closing the
GUIs and resuming it afterwards. It would remove the one remaining fallible
step (CreateProcess, e.g. blocked by antivirus) after the windows are gone, but
suspended-create + `ResumeThread` in a binary that already "reads like a
downloader" is the process-hollowing shape ML engines flag
([antivirus-heuristics](antivirus-heuristics.md)). Still open: if setup cannot
be started, the closed GUIs are not relaunched.

## Open questions / stale-risk

- **RESOLVED 2026-08-17 — the fetch and install paths have now run on real
  hardware.** Two full update cycles completed against a throwaway repo:
  WinHTTP, the hand-validated redirect chain, the protected staging directory,
  the verify-then-launch sequence, the GUI stop, the silent install and the
  relaunch all worked. The settings restore worked for a version-bound capture
  and failed for a legacy one; see finding 1 above. What remains unproven is
  listed below.
- **CLOSED 2026-08-18: the manifest cache round-trips.** Observed live, both
  halves, after three separate silent failures had to be fixed in sequence
  (write scope, containment root, and a startup sweeper that deleted the files):

      update cache: restored decision=2 published=0.23.10 installed=0.23.9 arch=x64
      update cache: re-adopted the staged package
                    greencurve-0.23.10-windows-x64-setup.exe (no re-download needed)

  The manifest was re-verified, re-parsed and the decision recomputed against
  the running binary, and the staged installer was re-adopted by re-hashing
  rather than re-downloading. Survives both a service restart and a GUI
  restart.
- **The legacy-capture path has not been exercised live.** Finding 1's fix is
  unit tested at 4279-4286, but the only real proof is a 0.23 client updating to
  a build that contains the fix and keeping its settings.
- **The tray balloon has never been seen.** `gc_update_should_notify()` is unit
  tested; whether the shell actually renders it on this machine, and whether it
  fires at a moment the user is present for, is not something a test can answer.
- *(historical, 2026-08-14)* As of 2026-08-14 release `0.23` is published *with* a signed
  manifest, and the fetch path was proven end to end with `curl`: both fixed
  URLs resolve, the redirect lands on `release-assets.githubusercontent.com`
  (allowlisted), the signature verifies over the network-fetched bytes against
  the embedded key, and the asset's size and SHA-256 match the manifest exactly.
  What that does **not** cover is the code that will actually do it -- WinHTTP,
  the in-code redirect validation, the protected staging directory, the
  verify-then-launch sequence and the install itself.
- *(closed 2026-08-17)* "The install path has never run on a real machine" —
  it has now, twice, including the settings transfer. Auto-check still starts
  UNSET; that is the consent design, not a hedge against this being unproven. Note that the 2026-08-15 first-run prompt does not weaken
  this: nothing checks until a human answers yes, and nothing installs without a
  second explicit click. It replaces "never asked, therefore never checked" with
  "asked once", which is what the design always specified.
- **The manifest cache has not been exercised across a real reboot.** Its
  verify/parse/decide ordering is gated structurally and its policy is unit
  tested, but the round trip -- a real check writing the files, a real service
  restart reading them back, and a real staged package being re-adopted rather
  than re-downloaded -- has only been reasoned about. The failure modes are all
  fail-closed into "no cache", i.e. exactly today's behaviour, so the downside
  is bounded.
- **The order gates only prove textual order, not reachability.** Disabling a
  verify with `if (false && ...)` keeps the call textually ahead of the parse
  and passes both `check_signature_precedes_parse` and
  `check_cache_is_reverified_not_trusted`; confirmed by mutation on 2026-08-15.
  Swapping the two blocks *is* caught. This limitation is inherent to the
  structural approach and predates the cache, but it is worth writing down
  rather than rediscovering.
- `service_update_foreground_app_active()` uses `SHQueryUserNotificationState`,
  which reports the state of the *session the service queries from*. Its
  behaviour from a LocalSystem service across sessions is unverified; it may be
  conservative (refusing installs) or ineffective (allowing them mid-game).
- The check rides the existing service watchdog tick rather than owning a timer.
  The tick's period has no bearing on the interval, which is measured in wall
  time, but it does bound how promptly a due check starts.
- Backoff caps at the steady-state interval, so a machine that keeps failing
  checks at the same rate a healthy one does. That is deliberate; it is not a
  bandaid for a race (AGENTS.md's no-timing-fixes rule is about crash/race
  fixes, and this is a user-facing schedule).

## Last verified

- 2026-10-05 (**0.28 released**): published stable from commit `358793a`,
  tag `0.28`, with a floorless signed manifest so all public clients (0.23+)
  are eligible. CI run `37356433779` and release run `37358341063` were green.
  This was the first release on the 2026-10-01 freshness contract: all FOUR
  version-independent updater assets were published (v1 manifest+sig plus the
  v2 freshness envelope+sig; 21 release assets total). The published x64/ARM64
  setup executables were downloaded back from GitHub, cross-checked against the
  release's `.sha256` sidecars and GitHub's independent asset digests,
  provenance-verified against `release.yml` at `358793a`, then signed with the
  active key by `tools/release_post.py` (first full live run of that command,
  dry-run parity included).
  Published x64 setup: size `1970220`, SHA-256
  `33aee0fd6448d5f86864d703c7138e0b86340c723086f6a68bceed33c0508c5d`.
  Published ARM64 setup: size `2951212`, SHA-256
  `f62275a7615bac3bd9717edb3ee70f268762d7dc47552f95d888878965e89390`.
  v2 envelope: issued `2026-10-05T18:53:55Z`, expires `2026-11-04T18:53:55Z`
  (30 days) -- **renew with `release_post.py --renew-freshness` before that
  instant**; expiry is fail-closed for cached and staged metadata. Renewal
  itself is still unexercised live.
  Post-publication checks through the client's fixed `releases/latest/download`
  URLs: v1 and v2 signatures verified over the network bytes against
  `GC_UPDATE_PUBLIC_KEY_ACTIVE`, fetched bytes identical to the locally
  prepared set, v2 wraps the exact v1 bytes, both installers matched signed
  size/SHA-256 through the manifest URLs with redirects ending on
  `release-assets.githubusercontent.com`, and the release body matches the
  `CHANGELOG.md` extraction. Policy harness against the actual fetched
  manifest (update policy headers byte-identical 0.23..HEAD, re-checked): all
  of 0.23/0.23.1/0.24.0/0.25.0/0.25.1/0.25.2/0.26.0/0.27.0 plus the `0.26` and
  `0.27` no-patch spellings read `AVAILABLE` on BOTH architectures; `0.28` and
  `0.28.0` read `UP_TO_DATE`; `0.29` reads `REJECTED`; no floor. Attestation
  verified for the published x64 setup (Sigstore bundle binds
  `release.yml` workflow_dispatch at `358793a`), commit status
  `Provenance & Attestation` posted `success`, and the tag is SSH-signed with
  the `Verified` badge. The version is spelled `0.28` (short form); operator-side notes for this
  release are kept in the maintainer's private notes. Manual checklist items NOT rerun and recorded as release risk:
  disposable-elevated upgrade/move/uninstall (the 2026-10-05 two-hop clone
  simulation live-verified the upgrade hop, settings handoff, and service
  lifecycle), live post-stop rollback fault injection (covered by pure fault
  injection, a native fixture, and builds), and GPU-hardware power-only
  Apply / crash handback (no GPU-clock code changed since 0.27.0).
- 2026-09-27 (**0.27.0 released**): published stable from commit `2acdcad`,
  tag `0.27.0`, with a floorless signed manifest so all public clients (0.23+)
  are eligible. CI run `36272351981` and release run `36274844164` were green.
  The published x64/ARM64 setup executables were downloaded back from GitHub,
  cross-checked against the release's own `.sha256` assets, signed with the active
  key (`tools/update_signing.py prepare`), and uploaded as the two version-independent
  updater assets (`greencurve-update-manifest.txt` and `greencurve-update-manifest.sig`,
  19 assets total).
  Published x64 setup: size `1953615`, SHA-256
  `bcf93c4251a7937b88e461dc51a8b7b41f60bdad9134970edcbaca283072d0d9`.
  Published ARM64 setup: size `2933583`, SHA-256
  `534196bff535fd81bd52a73ad0f621664ce959f9fd725581b9032eb50c39b624`.
  Post-publication checks through the client's fixed `releases/latest/download`
  URLs: manifest and signature fetched anonymously, signature verified over the
  network bytes against `GC_UPDATE_PUBLIC_KEY_ACTIVE`, fetched bytes identical
  to the locally prepared pair, both installers matched signed size/SHA-256
  with redirects ending on `release-assets.githubusercontent.com`. The release
  body matches the `CHANGELOG.md` extraction. Attestation verified and binds the
  setup executables and artifacts to `release.yml` at `2acdcad`. Commit status check
  under context `Provenance & Attestation` posted `success`.
  Policy harness against the manifest: `AVAILABLE` for installed 0.23..0.26.0 on
  both architectures; `UP_TO_DATE` for 0.27.0; `REJECTED` for 0.28.0.
- 2026-09-24: The install reservation hands a manual fan runtime to driver auto
  and every non-success release restores it (see "Fans during an install").
  The pending-restore age-failure log line now tokenizes its path (F-03-001).
  `python build.py --test` (6320-6332) and the full build passed; no live
  install was run.
- 2026-09-23: The GUI distinguishes no active intent from a failed settings
  capture and asks before proceeding after failure. Its setup child receives
  the capture handoff marker in either relaunch form. Pure/parser cases
  5790-5801, sanitizer regression tests, and the full build matrix passed;
  the warning and live updater flow were not clicked through. Exercise that
  prompt and setup's final warning during the next planned interactive upgrade
  on a disposable installation; no further capture code change is planned
  solely to close this verification gap.
- 2026-09-17 (**0.26.0 released**): published stable from commit `344cf44`,
  tag `0.26.0`, with a floorless signed manifest so all public clients (0.23+)
  are eligible. CI run `35256205838` and release run `35257005671` were green.
  The published x64/ARM64 setup executables were downloaded back from GitHub,
  cross-checked against both the release's own `.sha256` assets and GitHub's
  independently published asset digests, signed with the active key, and
  uploaded as the two version-independent updater assets (19 assets total).
  Published x64 setup: size `1867577`, SHA-256
  `9e76495e657d457769b2161a1a21a2b0d9dd73d3068d2dc510104e00dc96473f`.
  Published ARM64 setup: size `2836281`, SHA-256
  `03934e6ea3a8c488ac2ad076b18fdb638ac48209ae7bcfbae6f997565a410668`.
  Post-publication checks through the client's fixed `releases/latest/download`
  URLs: manifest and signature fetched anonymously, signature verified over the
  network bytes against `GC_UPDATE_PUBLIC_KEY_ACTIVE`, fetched bytes identical
  to the locally prepared pair, both installers matched signed size/SHA-256
  with redirects ending on `release-assets.githubusercontent.com`. The release
  body is byte-equal to the `CHANGELOG.md` extraction. Attestation verified and
  binds the x64 setup to `release.yml` at `344cf44` (a control run against a
  nonexistent repo fails, so the silent success is real).

  **Two checks this release added, because "the update must work" was being
  taken on faith.**

  - *The policy harness was run against the manifest fetched from the live
    fixed URL*, not a fixture: `AVAILABLE` for installed 0.23 / 0.23.1 / 0.24.0
    / 0.25.0 / 0.25.1 / 0.25.2 on both architectures, `UP_TO_DATE` for 0.26 and
    0.26.0, `REJECTED` for 0.27 and 0.29 (the local test tags).
  - *That harness is compiled from current headers, which is only meaningful if
    the clients run the same code* -- so it was checked rather than assumed.
    `update_version_policy.h`, `update_manifest_policy.h`, `update_url_policy.h`
    and `update_verify_keys.h` are **byte-identical** at tags 0.23, 0.23.1,
    0.24.0, 0.25.0, 0.25.1 and 0.25.2 and at HEAD, as are
    `main_service_update_fetch.cpp` and `main_service_update_verify.cpp` at
    0.25.2. The harness result therefore *is* each installed client's decision,
    embedded key included. Worth repeating each release, and worth noticing the
    day it stops being true.

  **A live installed-client click-through was run and passed** (maintainer,
  2026-09-17): a machine downgraded to 0.25.2 found, downloaded and installed
  0.26.0 through the in-app updater with no reported problem. That is the first
  live click-through since 0.23.1 and the first ever across a protocol bump and
  a saved-curve format change. Detail beyond "it worked fine" was not recorded;
  what specifically remains unconfirmed is whether the **settings handoff**
  carried a curve with a nonzero GPU offset across the version boundary, which
  is the path the 5200-5216 assertions cover and the only part of the hop whose
  failure would be silent.

- 2026-09-13 (**0.25.2 released**): published stable from commit `24095e8`,
  tag `0.25.2`, with a floorless signed manifest so all public clients (0.23+)
  are eligible. CI run `34758736305` and release run `34759132697` were green.
  The published x64/ARM64 setup executables were downloaded back from GitHub,
  cross-checked against the release's own `.sha256` assets, signed with the
  active key, and uploaded as the two version-independent updater assets (19
  assets total). Post-publication checks through the client's fixed
  `releases/latest/download` URLs: manifest and signature were fetched
  anonymously, the signature verified over network bytes against
  `GC_UPDATE_PUBLIC_KEY_ACTIVE`, fetched bytes were identical to the locally
  prepared pair, and both installers matched signed size/SHA-256 with
  redirects ending on the allowlisted `release-assets.githubusercontent.com`
  host. Attestation verified for the released setup executable.

- 2026-09-11 (**0.25.1 released**): published stable from commit `77f3813`,
  tag `0.25.1`, with a floorless signed manifest so all public clients (0.23+)
  are eligible. CI run `34601323219` and release run `34602102089` were green.
  The published x64/ARM64 setup executables were downloaded back from GitHub,
  cross-checked against the release's own `.sha256` assets, signed with the
  active key, and uploaded to the release. Post-publication checks through the
  client's fixed `releases/latest/download` URLs: manifest and signature were
  fetched anonymously, the signature verified over the network bytes against
  `GC_UPDATE_PUBLIC_KEY_ACTIVE`, the fetched bytes were identical to the
  locally prepared pair, and both installers matched the signed size/SHA-256
  with redirects ending on the allowlisted `release-assets.githubusercontent.com`
  host. A policy harness compiled against the actual fetched manifest returned
  `AVAILABLE` for installed 0.23/0.23.1/0.24.0/0.25.0 on x64 and ARM64,
  `UP_TO_DATE` for 0.25.1, and `REJECTED` for 0.25.2. Updater, installer and
  wire-protocol files are unchanged since 0.25.0.


- 2026-09-06 (**0.25.0 released**): published stable from commit `bdcc91d`, tag
  `0.25.0`, with a floorless signed manifest so public 0.24.0 installations are
  eligible. Before publication, the exact 0.24.0 version policy, manifest
  grammar, and embedded signing-key headers were verified byte-identical to
  HEAD and a focused regression returned `AVAILABLE` for 0.24.0 on x64 and
  ARM64 with the exact 0.25.0 setup names. The active private key passed its
  owner-only ACL gate and derived to the public key embedded in both releases.
  After upload, all four client URLs (`releases/latest` manifest, signature,
  x64 setup, ARM64 setup) resolved through the allowlisted GitHub asset host;
  the network-fetched signature verified and both served installers matched
  the signed size/SHA-256 fields. CI run `34017285953` and release run
  `34017597506` were green, including provenance verification. The exact
  public installed-client click-through was not rerun; the version-bound path
  previously completed two live Windows cycles and its updater/installer
  command contract is unchanged from 0.24.0.

- 2026-08-25 (**0.24.0 released**): published from commit `3d27b51`, tag
  `0.24.0`, with a floorless signed manifest so public 0.23/0.23.1 installations
  are eligible. The manifest and signature fetched through the client's fixed
  `releases/latest/download` URLs match the locally verified bytes; both setup
  executables fetched from those URLs match the signed size/digest and terminate
  on the allowlisted GitHub asset host. The active signing key's public half is
  byte-identical to the key embedded in 0.23.1 and 0.24.0. The focused regression
  also returns `AVAILABLE` for an installed 0.23.1 on x64 and ARM64. A live
  installed-client click-through was not repeated for this release.

- 2026-08-18 (**0.23.1 released**): published from commit `4cd54a2`, tag `0.23.1`,
  with a signed `greencurve-update-manifest.txt` + `.sig`. This is the first
  release the updater will actually deliver to users -- 0.23 is what they all
  run, and this is what 0.23 will offer them.

  Verified before and after publishing:
  - the signing key's public half is byte-identical to `GC_UPDATE_PUBLIC_KEY_ACTIVE`
    as embedded in **0.23** and in HEAD, so every deployed client can verify it;
  - manifest digests cross-checked against GitHub's independently published
    `.sha256` files, both architectures;
  - the signature verifies over the **network-fetched** bytes from the client's
    own two fixed URLs;
  - the asset URL the client builds resolves 200 on the allowlisted
    `release-assets.githubusercontent.com`, and the downloaded bytes match the
    signed size (1565424) and digest exactly;
  - no `min_from`, deliberately: 0.23 users must be offered this.

  **Wire compatibility with 0.23 was checked and holds.** The new `channelState`
  byte occupies one of `updateReserved[2]`; 0.23's validator does not check the
  reserved bytes (that hardening postdates it), so an old GUI ignores the byte
  rather than rejecting the response.

  The release build first failed on toolchain verification -- see the symlink
  chain note in [build.md](build.md); fixed in `4cd54a2` before publishing.



- 2026-08-15 (notification and restart persistence): `python build.py` (all four
  targets) and `--test` pass. Assertions 4320-4355 cover which decisions alert,
  both menu captions, the tooltip suffix and the truncation inversion, and every
  gate on the first-run question. Two new source gates:
  `check_cache_is_reverified_not_trusted` (verify before parse before decide, no
  persisted conclusion, re-hash before re-adopt) and
  `check_update_is_actually_surfaced` (each of the three surfaces, plus the
  prompt). **Mutation tested**: removing the `MANUAL_REQUIRED` alert fails 4321;
  letting the base win the tooltip space fails 4345; dropping the button accent
  fails its gate; swapping verify and parse in the cache fails the order gate.
  The one mutation *not* caught is recorded under open questions.
  `ui_main_window.cpp` was split (1289 -> 984) with the right-click menus moving
  to `ui_main_context_menus.cpp`; the source guards address the two as one
  surface via `build_state.concatenated_gate_surface()`.
- 2026-08-14 (first real release): `0.23` published from commit `53614ec` with
  a signed `greencurve-update-manifest.txt` + `.sig`. The publish procedure in
  this page was followed exactly, including signing the *downloaded* artifacts
  rather than a local build; the manifest digests were additionally cross-checked
  against GitHub's own published `.sha256` files, and the signing key against
  `update_verify_keys.h`. Verified from the client's own URLs with `curl`: both
  documents fetch, the redirect target is allowlisted, the signature verifies
  over the fetched bytes, and the asset matches its declared size and digest.
  The service-side client, the staging path and the install remain unexercised.
- 2026-09-05 (audit hardening): updater transport, Base64 decoding, and maintainer tooling hardened:
  - `WinHttpSetTimeouts`, `WINHTTP_OPTION_SECURE_PROTOCOLS`, and `WINHTTP_OPTION_DISABLE_FEATURE` return values are verified and fail closed via `gc_update_http_close()`. Added `WINHTTP_DISABLE_COOKIES` to enforce stateless fetches.
  - Strict Base64 signature decoding now permits at most one trailing `\r\n` or `\n` (rejecting lone `\r` and multiple line breaks) and enforces zero unused pad bits per RFC 4648 §3.5 (`quad[1] & 0x0F == 0` for `==`, `quad[2] & 0x03 == 0` for `=`), eliminating Base64 signature malleability.
  - `tools/update_signing.py:verify()` updated to match the shipped C++ low-S invariant (`1 <= s <= N // 2`) and verified with regression self-tests.
- 2026-08-14 (implementation): `python build.py --test`,
  `--check --target all --arch x64`, and `--fuzz --fuzz-target update_manifest`
  (20,000 runs) all pass. Assertions 4100-4259 cover version ordering and the
  downgrade refusal, the manifest grammar and its bindings, the URL/redirect
  allowlist, the schedule and install gate, the v19 wire rules, the
  signer/verifier known-answer test, and strict base64. Fuzz target 6 covers the
  manifest parser and URL allowlist. The known-answer test was confirmed live by
  corrupting the fixture signature and observing assertion 4233 fail. Nothing
  has been run against a real release or a real install.
