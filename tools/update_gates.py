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
    # Setup runs in session 0 because the service launched it, and window
    # enumeration is session-scoped -- so setup cannot see, close, or even
    # detect a GUI in the user's session.  Left to itself it logs "no running
    # Green Curve window found" and then fails replacing greencurve.exe with
    # ERROR_ACCESS_DENIED, half-way through copying files.  The service must
    # close them first, and must do it before the command line is built, because
    # how many it closed decides whether setup should start one again.
    # The command line is built and encoded BEFORE the GUIs are closed, and the
    # measured count only selects between two already-validated strings.
    #
    # This is the reverse of the original ordering, and deliberately so. Closing
    # the user's windows is the one destructive step the service takes on their
    # behalf and it cannot be undone from there -- the service does not launch
    # processes into interactive sessions. So nothing fallible may sit after it.
    # Building the string and encoding it to UTF-16 both can fail, so both moved
    # in front; what must stay behind is the SELECTION, because "was a GUI
    # actually running" has to be measured rather than assumed (assuming it was
    # not is what shipped in 0.27 and never gave the user their window back).
    require_order(
        worker, "service_update_run_install",
        "gc_update_build_installer_command_line", "service_update_stop_gui_processes",
        "the installer command line is built and validated before any GUI is "
        "closed, so no fallible step remains after the windows are gone")
    require_order(
        worker, "service_update_run_install",
        "service_update_stop_gui_processes", "closedGuiCount > 0 ? wideRelaunch",
        "which command line is used is selected AFTER the GUIs were counted")
    require_order(
        worker, "service_update_run_install",
        "service_update_stop_gui_processes", "CreateProcessW",
        "the GUI is stopped before setup is launched")
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
                   "update_url_policy.h", "update_schedule_policy.h",
                   "update_presentation_policy.h", "update_transport_policy.h"):
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


def check_gui_cannot_choose_the_target(ctx, require_text, forbid_text):
    """The GUI triggers and displays; it never names what gets installed.

    The client sends four commands and, on exactly one of them, two integers.
    If it ever gained a path, a URL or a version field it would stop being a
    trigger and start being a payload -- and the thing on the other end runs as
    SYSTEM.  Checked structurally because the happy path looks identical either
    way.
    """
    dialog = _p(ctx, "gui_update_dialog.cpp")
    client = _p(ctx, "gui_update_client.cpp")
    for source in (dialog, client):
        for field in ("request.path", "request.desired", "request.targetGpu",
                      "request.profileSlot", "request.operationId"):
            forbid_text(source, field,
                        "the GUI must not put a target into an update request")
    require_text(dialog, "SERVICE_CMD_SET_UPDATE_POLICY",
                 "the policy command is the only one carrying client data")

    # Installing is a button press with a confirmation, never a side effect of
    # opening the dialog or of a refresh tick.
    require_text(dialog, "GUD_INSTALL_BTN_ID",
                 "installing is bound to an explicit button")
    require_text(dialog, "MB_YESNO",
                 "installing is confirmed before it is requested")
    # The tray opens the dialog; it does not install.  A context-menu click must
    # not be able to stop the service and drop the GPU to stock.
    forbid_text(_p(ctx, "gui_tray_menu.cpp"), "gui_update_request_install",
                "the tray must open the dialog rather than install directly")


def check_install_reservation_and_restore_gate(ctx, require_text, require_order):
    """The apply-in-flight race fix and the stale-restore gate stay structural.

    Both are negative properties: dropping the reservation check makes updates
    keep working, and dropping the version/freshness gate on the pending restore
    only misbehaves after a failed install.  Neither would fail a happy-path
    test, so they are asserted as source order/text here.
    """
    worker = _p(ctx, "main_service_update_worker.cpp")
    require_order(
        worker, "service_update_run_install",
        "lock_service_runtime", "service_update_set_install_reserved(true)",
        "the install reservation is set while the runtime lock is held")
    require_order(
        worker, "service_update_run_install",
        "service_update_set_install_reserved(true)", "CreateProcessW",
        "the reservation survives until setup is launched")
    pipe = _p(ctx, "main_service_pipe.cpp")
    require_text(pipe, "service_update_install_reject_mutation",
                 "APPLY/RESET refuse GPU writes while an install is reserved")
    handoff = _p(ctx, "gui_update_settings_handoff.cpp")
    require_text(handoff, "gc_update_restore_decide",
                 "a pending restore is only replayed for the exact update")
    require_text(handoff, "gui_update_pending_restore_age_seconds",
                 "a stale capture is discarded instead of replayed")


def check_install_failure_recovery(ctx, require_text, forbid_text):
    """What happens when an install does NOT succeed.

    Three regressions that a happy-path test cannot see, because in the happy
    path setup stops this service and none of this code runs at all:

    * The install reservation gates the fan runtime pulse, every auto-restore
      path, Apply, Reset and the controlled restart. Releasing it only when the
      installer was *observed to exit* meant a setup process that overran its
      timeout left GPU and fan control dead for the remaining life of the
      service process. Every non-success path must release it.
    * The updater closes every Green Curve window before starting setup, so
      setup must relaunch on failure too -- otherwise a failed silent install
      reports itself into a GUI that is no longer running.
    * The pre-install enumeration must distinguish "that process is already
      gone" from "that process refused us a handle". greencurve.exe is the CLI
      binary as well, so a short-lived helper exiting mid-enumeration is normal
      and must not abort the install; ERROR_ACCESS_DENIED still must.
    """
    worker = _p(ctx, "main_service_update_worker.cpp")
    forbid_text(worker, "processExited",
                "releasing the reservation must not depend on having observed "
                "the installer exit (that is how the timeout path leaked it)")
    # "Setup might be mid-file-replacement" is exactly "the setup process is
    # alive", so the reservation is ended by ending the process rather than by
    # guessing a grace period -- every value of which is wrong in one direction.
    require_text(worker, "TerminateProcess(pi.hProcess,",
                 "an overrunning setup is ended rather than waited out, so the "
                 "reservation's precondition is made false instead of assumed")
    require_text(worker, "if (!ok && !reservationHeld)",
                 "the reservation is held past this function only when the "
                 "setup process could not be proven dead")
    gui_stop = _p(ctx, "main_service_update_gui_stop.cpp")
    require_text(gui_stop, "if (openError == ERROR_INVALID_PARAMETER) continue;",
                 "a process that exited during enumeration is gone, not an "
                 "enumeration failure")
    require_text(gui_stop, 'return fail_closed(openError, "opening candidate GUI")',
                 "any other OpenProcess failure still fails closed")
    require_text(gui_stop, 'return fail_closed(identifyError, "identifying a candidate GUI")',
                 "a live process we cannot name fails closed rather than being "
                 "silently treated as somebody else's")
    # Relaunch is not conditional on the install having succeeded.
    forbid_text(_p(ctx, "installer_main.cpp"),
                "if (ok && context.plan.launchAfterInstall)",
                "setup must relaunch the GUI the updater closed even when the "
                "install failed")


def check_cache_is_reverified_not_trusted(ctx, require_text, forbid_text,
                                          require_order):
    """The cached manifest goes through the SAME gate as a fetched one.

    The cache exists so a service restart does not forget that an update was
    found -- previously it did, while `last_check` survived, so every surface
    went quiet for up to a full interval on an update that might already be
    downloaded and verified.

    The tempting shortcut is to cache the CONCLUSION ("0.30 is available") and
    restore it.  That would make a file in %ProgramData% authoritative over a
    signature, and it would not fail visibly: the badge would appear, the tray
    entry would appear, and only the install would refuse -- leaving the user
    with an alert they cannot act on and no explanation.  So the cache stores
    the two documents verbatim and the restore re-runs verify -> parse ->
    decide, in that order, exactly as `service_update_run_check()` does.

    The order is asserted structurally for the same reason the network path's
    is: both orders produce an identical happy path, so neither fails loudly
    when it regresses.
    """
    cache = _p(ctx, "main_service_update_cache.cpp")
    require_order(
        cache, "service_update_restore_from_cache",
        "gc_update_verify_manifest_signature", "gc_update_manifest_parse",
        "a cached manifest's signature is verified before it is parsed")
    require_order(
        cache, "service_update_restore_from_cache",
        "gc_update_manifest_parse", "gc_update_decide",
        "a cached manifest is parsed before a decision is derived from it")
    # The decision is RECOMPUTED against the running binary, never restored.
    # This is what makes the cache self-correcting after an install: the same
    # manifest that advertised 0.30 answers UP_TO_DATE once 0.30 is running.
    require_text(cache, "gc_update_version_parse(APP_VERSION, &installed)",
                 "the cached decision is recomputed against the RUNNING "
                 "version rather than restored from disk")
    forbid_text(cache, "last_decision",
                "no conclusion is persisted; only the signed documents are")
    # A re-adopted package is re-hashed through the same write-denying handle
    # the download used.  Trusting "we staged it last boot" would mean the one
    # file the service launches as SYSTEM is the one it never re-measured.
    require_text(cache, "service_update_staged_package_matches_manifest",
                 "a staged package left by a previous process is re-verified "
                 "before it is re-adopted")
    # And the name is rebuilt from the verified manifest, never read off disk,
    # so a file the sweep missed cannot be adopted under its own name.
    require_text(cache, "gc_update_select_asset(manifest,",
                 "the staged file's name comes from the verified manifest")


def check_download_is_bounded_before_it_is_written(ctx, require_text, require_order,
                                                   forbid_text, harness_source_path):
    """The streamed asset is bounded BEFORE each chunk reaches the sink.

    `gc_update_stream_asset()` compares the running total against the size the
    signature-verified manifest declared, and does it between the read and the
    write.  Moving that comparison after the write, or out to the end of the
    loop, is invisible: the download is still refused, the digest still fails,
    the update still does not install.  It just writes everything a hostile
    server chose to send into a directory a LocalSystem process launches
    executables from.

    Asserted twice over, because neither half is sufficient alone.  The source
    order below survives a refactor that keeps the calls but drops the test;
    assertion 4383-4386 measures what the fake sink actually received, which
    survives a refactor that keeps the test but reorders it.
    """
    transport = _p(ctx, "update_transport_policy.h")
    # The needles name the CALL, not the member: `!sink->write` also appears in
    # the argument guard above, which legitimately runs first.
    require_order(
        transport, "static inline GcUpdateFetchResult gc_update_stream_asset(",
        "return GC_UPDATE_FETCH_TOO_LARGE", "sink->write(sink->ctx, chunk, got)",
        "an oversized response is refused before the chunk is written")
    # The ceiling comes from the signed manifest, never from the transport.  A
    # Content-Length would be the server telling us how much to trust it.
    forbid_text(_p(ctx, "main_service_update_fetch.cpp"), "WINHTTP_QUERY_CONTENT_LENGTH",
                "the download ceiling comes from the signed manifest, not from "
                "a header the server chooses")
    # The loops must stay behind the seam; re-inlining WinHTTP into them would
    # silently take them back out of the harness's reach -- and out of the
    # Linux host entirely.  Named as the include and the handle type rather
    # than as "WinHttp", because the header's own comment explains which calls
    # the seam mirrors and a prose mention is not a coupling.
    forbid_text(transport, "#include <winhttp.h>",
                "the read loops stay transport-agnostic and therefore testable")
    forbid_text(transport, "HINTERNET",
                "the read loops name no transport handle type")
    require_text(harness_source_path, '#include "update_transport_policy.h"',
                 "the read loops are asserted on both hosts")
    require_text(harness_source_path, "fake_http_available",
                 "the read loops are driven by a fake transport")


def check_update_is_actually_surfaced(ctx, require_text, require_order):
    """An available update reaches at least one PASSIVE surface.

    Every other gate in this file protects against doing something unsafe.
    This one protects against doing nothing at all, which was the state of the
    feature until the presentation policy landed: the only mention of an
    available update was a tray context-menu entry, so a user who never
    right-clicked the tray icon was never told, and the main window looked
    identical whether the machine was current or had a verified installer
    staged and waiting.

    That failure is invisible from inside the program -- no crash, no log line,
    no failing test -- which is exactly the shape of thing that belongs here.
    """
    require_text(_p(ctx, "ui_theme_button.cpp"), "dis->CtlID == UPDATE_BTN_ID",
                 "the Updates button carries the pending accent when an "
                 "update is available")
    require_text(_p(ctx, "tray_presentation.cpp"), "gc_update_tray_tooltip_suffix",
                 "the tray tooltip names an available update")
    require_text(_p(ctx, "gui_tray_menu.cpp"), "gc_update_tray_menu_label",
                 "the tray menu entry is built from the shared alert policy")
    # All three read one decision, so a fourth surface cannot introduce a
    # disagreement about whether there is news.
    require_text(_p(ctx, "gui_update_client.cpp"), "gc_update_alert_kind",
                 "every surface derives its alert from the same policy")
    # The prompt is what makes UNSET a question rather than a permanent state
    # in which no check ever runs.
    require_text(_p(ctx, "gui_update_dialog.cpp"),
                 "gc_update_should_prompt_auto_check",
                 "the unset auto-check preference is actually asked about")
    # The passive surfaces above all require the user to be looking at
    # something: the button needs the window open, the tooltip needs a hover,
    # the menu needs a right-click.  The balloon is the only one that reaches a
    # user running minimised, which is how this app is normally run.
    require_text(_p(ctx, "gui_update_client.cpp"), "gc_update_should_notify",
                 "an available update raises a tray notification on the edge")
    require_text(_p(ctx, "gui_update_client.cpp"), "NIF_INFO",
                 "the tray notification is actually sent to the shell")
    # A replayed manifest verifies, parses and reports "up to date", so the
    # ordinary alert is NONE and every update surface stays silent. The channel
    # warning is the only thing that speaks, and it must not be conditioned on
    # the update alert -- that is the exact state the attack produces.
    require_text(_p(ctx, "gui_update_client.cpp"), "gc_update_should_notify_channel",
                 "a suspect update channel raises its own notification")
    require_text(_p(ctx, "gui_update_dialog.cpp"), "gc_update_channel_warning",
                 "the dialog says so before it says anything reassuring")
    # The dialog's own window handle must be recorded BEFORE the first refresh.
    # gud_refresh_controls() early-outs on a null handle, and WM_CREATE is
    # dispatched from inside CreateWindowExA -- so the assignment made from its
    # return value comes too late, every label stays empty, and the poll timer
    # that same function starts never starts. Three live reports, no crash, no
    # failing test: the state was always right and nothing asked for it.
    require_order(
        _p(ctx, "gui_update_dialog.cpp"), "gud_create_controls",
        "g_updateDialog.hwnd = hwnd", "gud_refresh_controls",
        "the dialog records its window before it first populates itself")
    require_text(_p(ctx, "main_service_update_worker.cpp"), "gc_update_channel_note_version",
                 "the high-water mark is raised only from a verified manifest")
    require_order(
        _p(ctx, "main_service_update_worker.cpp"), "service_update_run_check",
        "gc_update_verify_manifest_signature", "gc_update_channel_note_version",
        "the high-water mark is raised only after the signature verified")


def check_machine_state_is_machine_scoped(ctx, require_text, forbid_text):
    """The service's own machine-scope writes use the machine-scope root.

    `write_text_file_atomic_service()` confines a write to the CALLING CLIENT's
    profile, which is right for a path a client named and wrong for a path the
    service chose for itself.  The update-manifest cache lives in
    %ProgramData%\\Green Curve, which is inside no user's profile, so it was
    refused on every single write from the day the cache shipped until
    2026-08-17 -- and nothing surfaced, because the restore path treats a
    missing cache as the ordinary state of a machine that has never checked.

    That is the exact shape this file exists for: a failure with no crash, no
    failing test, and a log line nobody had reason to read.  The scope argument
    is one token, so a future edit can drop it without anything else changing.
    """
    cache = _p(ctx, "main_service_update_cache.cpp")
    require_text(cache, "GC_SERVICE_WRITE_MACHINE_CONFIG",
                 "the manifest cache is written with the machine-scope root")
    forbid_text(cache, "write_text_file_atomic_service(",
                "the manifest cache must not use the caller-profile writer")
    # The machine scope is a DIFFERENT containment root, not an absent one.
    policy = _p(ctx, "main_service_request_policy.cpp")
    require_text(policy, "service_path_is_within_machine_config",
                 "the machine scope still confines the write to a root")
    require_text(policy, "resolve_machine_config_dir_w",
                 "the machine-scope root is the machine config directory")
    # Deliberately NOT a forbid_text on resolve_service_machine_data_dir: the
    # comment at that call site names the wrong function in order to explain
    # why it is wrong, and a gate that flags its own rationale gets deleted
    # rather than fixed. Requiring the correct resolver is the real property.


def check_all(ctx, require_text, forbid_text, require_order, harness_source_path):
    check_gui_cannot_choose_the_target(ctx, require_text, forbid_text)
    check_signature_precedes_parse(ctx, require_order)
    check_verification_precedes_launch(ctx, require_order, require_text)
    check_install_requires_consent(ctx, require_text, forbid_text)
    check_no_client_supplied_target(ctx, require_text, forbid_text)
    check_no_certificate_bypass(ctx, forbid_text)
    check_staging_is_protected(ctx, require_text, forbid_text)
    check_public_keys_only(ctx, require_text, forbid_text)
    check_policy_stays_unit_tested(ctx, require_text, harness_source_path)
    check_uninstall_key_agrees_with_setup(ctx, require_text)
    check_install_reservation_and_restore_gate(ctx, require_text, require_order)
    check_install_failure_recovery(ctx, require_text, forbid_text)
    check_cache_is_reverified_not_trusted(ctx, require_text, forbid_text,
                                          require_order)
    check_update_is_actually_surfaced(ctx, require_text, require_order)
    check_machine_state_is_machine_scoped(ctx, require_text, forbid_text)
    check_download_is_bounded_before_it_is_written(ctx, require_text, require_order,
                                                   forbid_text, harness_source_path)
