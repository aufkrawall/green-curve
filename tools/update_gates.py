"""Source gates for the in-app updater.

Split out of build.py so the build script stays under its size ratchet; build.py
owns the CLI and passes its paths and check helpers in through `ctx`.  Nothing
here imports build.py, so the dependency runs one way only.

`ctx` is any object exposing SOURCE_DIR and SCRIPT_DIR.

Why this feature earns its own gate module
------------------------------------------
The updater ends a successful run by launching a downloaded executable with
SYSTEM rights.  Almost every property that makes that safe is a *negative* --
something the code must NOT do -- and negatives do not fail loudly when they
regress:

* If the downgrade check is dropped, updates keep working.  They just also
  accept a replayed older release, which is the one attack neither the GitHub
  attestation nor the published `.sha256` files can catch.
* If the signature check moves after the parse, updates keep working.  The
  manifest parser just becomes reachable with attacker-controlled bytes.
* If a client-supplied field joins the update commands, updates keep working.
  The service just gains a way to be told which file to run as SYSTEM.

None of those has a test that would obviously fail, because the happy path is
unchanged in all three.  So they are asserted structurally, here.
"""
import os


def _p(ctx, name):
    return os.path.join(ctx.SOURCE_DIR, name)


def check_signature_precedes_parse(ctx, require_order):
    """The manifest's signature is verified BEFORE the manifest is parsed.

    This is the single most important ordering in the feature.  The parser is
    written defensively and is fuzzed, but "the parser is robust" is a much
    weaker property than "the parser never sees unauthenticated input", and only
    the second one survives a future bug in the parser itself.

    Checked as source order inside the check routine rather than as behaviour,
    because a test that exercised it would have to already trust the thing it is
    trying to prove.
    """
    worker = _p(ctx, "main_service_update_worker.cpp")
    require_order(
        worker, "service_update_run_check",
        "gc_update_verify_manifest_signature", "gc_update_manifest_parse",
        "the manifest signature is verified before the manifest is parsed")


def check_verification_precedes_launch(ctx, require_order, require_text):
    """The staged installer is re-verified immediately before it is launched,
    through a handle that is held across process creation.

    Verifying by path and then launching by path leaves a window in which the
    bytes that were measured and the bytes that get executed are not required to
    be the same file -- and this process is SYSTEM.  The staging directory's
    DACL already keeps standard users out, so this is defense in depth, but it
    is the kind that costs one `CloseHandle` placement and is worth keeping
    honest.
    """
    worker = _p(ctx, "main_service_update_worker.cpp")
    require_order(
        worker, "service_update_run_install",
        "gc_update_install_decision", "gc_update_staged_file_matches",
        "an install is gated before the staged file is re-verified")
    require_order(
        worker, "service_update_run_install",
        "gc_update_staged_file_matches", "CreateProcessW",
        "the staged installer is re-verified before it is launched")
    require_text(
        worker, "FILE_SHARE_READ",
        "the staged installer is opened with writes and deletes denied")


def check_install_requires_consent(ctx, require_text, forbid_text):
    """No automatic path may reach the install.

    `userConsented` is set in exactly one function, which is reached only from
    the SERVICE_CMD_INSTALL_UPDATE handler.  The automatic tick is allowed to
    check and to download -- neither changes the running system -- but an
    install stops the GUI, stops the service (which resets the GPU to stock on
    the way down) and replaces binaries.  Doing that unattended is not an
    update, it is a crash with extra steps.
    """
    worker = _p(ctx, "main_service_update_worker.cpp")
    require_text(worker, "gate.userConsented = true",
                 "the install gate records consent explicitly")
    # The automatic tick may start a check/download job and nothing else.
    forbid_text(worker,
                "service_update_start_worker(GC_UPDATE_WORK_INSTALL, err",
                "the automatic update tick must never start an install")


def check_no_client_supplied_target(ctx, require_text, forbid_text):
    """The update commands are triggers, never payloads.

    The service resolves the URL, the version, the asset name, the digest and
    the staging path itself, from constants compiled into the binary and from a
    signature-verified manifest.  A command that accepted any of those from an
    unprivileged GUI would be a local privilege escalation however carefully the
    named file were checked afterwards.
    """
    validation = _p(ctx, "service_protocol_validation.h")
    require_text(validation, "update command carries a path",
                 "the validator refuses a path on an update command")
    require_text(validation, "update command carries settings",
                 "the validator refuses settings on an update command")
    require_text(validation, "update command carries mutation fields",
                 "the validator refuses mutation fields on an update command")

    # The URL builders take no caller-supplied host and no configurable base.
    url_policy = _p(ctx, "update_url_policy.h")
    require_text(url_policy, "GC_UPDATE_REPO_OWNER",
                 "the update host and repository are compiled in")
    # Matched as a string LITERAL (`"http://`), not as the bare word: the file
    # legitimately discusses plaintext redirects in prose, and a gate that
    # cannot tell an explanation from a URL is one that gets deleted the first
    # time it cries wolf.  Note `"https://` does not contain this substring.
    forbid_text(url_policy, '"http://',
                "the updater must not build a plaintext URL")


def check_no_certificate_bypass(ctx, forbid_text):
    """There is no way to turn TLS verification off.

    A single `SECURITY_FLAG_IGNORE_*` would undo the transport half of the
    design, and these flags are exactly the kind of thing that gets added
    temporarily while debugging a proxy and then never removed.
    """
    fetch = _p(ctx, "main_service_update_fetch.cpp")
    for flag in ("SECURITY_FLAG_IGNORE_UNKNOWN_CA",
                 "SECURITY_FLAG_IGNORE_CERT_CN_INVALID",
                 "SECURITY_FLAG_IGNORE_CERT_DATE_INVALID",
                 "SECURITY_FLAG_IGNORE_WRONG_USAGE",
                 "WINHTTP_OPTION_SECURITY_FLAGS",
                 "WINHTTP_DISABLE_SECURE_PROTOCOLS"):
        forbid_text(fetch, flag,
                    "the updater must never weaken TLS verification")
    # Redirects must stay disabled at the WinHTTP level so each hop is checked
    # by hand before the request is sent, rather than after the fact.
    forbid_text(fetch, "WINHTTP_OPTION_REDIRECT_POLICY_ALWAYS",
                "the updater must validate redirects itself, not delegate them")


def check_staging_is_protected(ctx, require_text, forbid_text):
    """The download lands in the hardened machine-scope directory, never a
    user-writable temp folder.

    Staging into the invoking user's %TEMP% and then running the file elevated
    is the classic form of this bug, and the installer's own design notes
    already refuse it for the same reason.
    """
    state = _p(ctx, "main_service_update_state.cpp")
    require_text(state, "ensure_machine_config_directory",
                 "update staging inherits the hardened machine-config DACL")
    require_text(state, "FILE_ATTRIBUTE_REPARSE_POINT",
                 "the staging directory is refused when it is a reparse point")
    # API names and the environment lookup, not the bare word "%TEMP%": both
    # files explain in prose why a temp folder is the wrong place, and a gate
    # that flags its own rationale is one that gets deleted rather than fixed.
    worker = _p(ctx, "main_service_update_worker.cpp")
    for source in (state, worker):
        for temp in ("GetTempPath", "GetTempFileName", '"TEMP"', 'L"TEMP"'):
            forbid_text(source, temp,
                        "the updater must not stage an executable in a temp folder")


def check_public_keys_only(ctx, require_text, forbid_text):
    """The embedded key material is public, and a rollover slot exists.

    Removing a key is what breaks clients -- a client that cannot verify cannot
    update itself out of the problem -- so the rollover slot has to be published
    long before it is needed.
    """
    keys = _p(ctx, "update_verify_keys.h")
    require_text(keys, "GC_UPDATE_PUBLIC_KEY_ACTIVE", "an active key is embedded")
    require_text(keys, "GC_UPDATE_PUBLIC_KEY_NEXT",
                 "a rollover key is published before it is needed")
    forbid_text(keys, "PRIVATE KEY",
                "no private key material may appear in a tracked header")


def check_policy_stays_unit_tested(ctx, require_text, harness_source_path):
    """The pure policy stays asserted on both hosts.

    Each of these headers gates whether a downloaded executable is run as
    SYSTEM, and each is unit-testable without Windows.  Losing that coverage is
    silent -- the Windows shards still compile and still work -- so the harness
    is required to keep including them.
    """
    for header in ("update_version_policy.h", "update_manifest_policy.h",
                   "update_url_policy.h", "update_schedule_policy.h"):
        require_text(harness_source_path, '#include "%s"' % header,
                     "the regression harness asserts %s" % header)
    require_text(harness_source_path, "gc_update_is_newer",
                 "the downgrade gate is unit-tested (no other control catches "
                 "a replayed older release)")
    require_text(harness_source_path, "https://github.com@evil.example/a",
                 "the URL allowlist is tested against an embedded-credentials "
                 "authority, which defeats a naive prefix check")


def check_uninstall_key_agrees_with_setup(ctx, require_text):
    """Setup and the updater name the SAME Add/Remove Programs key.

    installer.md invariant 11 keeps the setup program and the application model
    from including each other's headers, so the key is spelled twice.  A
    disagreement would not fail anything: the updater would simply decide every
    installation is a portable copy and quietly stop offering updates.  Same
    failure shape as the setup icon id, which is guarded the same way.
    """
    literal = (
        r'L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\Green Curve"')
    require_text(_p(ctx, "installer_common.h"), literal,
                 "setup writes the Add/Remove Programs key it is expected to")
    require_text(_p(ctx, "update_verify_keys.h"), literal,
                 "the updater reads the SAME Add/Remove Programs key setup "
                 "writes (a drift here silently disables updates)")


def check_all(ctx, require_text, forbid_text, require_order, harness_source_path):
    check_signature_precedes_parse(ctx, require_order)
    check_verification_precedes_launch(ctx, require_order, require_text)
    check_install_requires_consent(ctx, require_text, forbid_text)
    check_no_client_supplied_target(ctx, require_text, forbid_text)
    check_no_certificate_bypass(ctx, forbid_text)
    check_staging_is_protected(ctx, require_text, forbid_text)
    check_public_keys_only(ctx, require_text, forbid_text)
    check_policy_stays_unit_tested(ctx, require_text, harness_source_path)
    check_uninstall_key_agrees_with_setup(ctx, require_text)
