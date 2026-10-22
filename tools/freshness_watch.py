#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
# SPDX-License-Identifier: MIT
"""Fail while the published update freshness envelope is close to expiry.

Clients refuse update metadata once its signed v2 envelope expires, and
renewing it (`release_post.py --renew-freshness`) is a maintainer action.
`.github/workflows/freshness.yml` runs this every day, so a failing scheduled
run is the reminder, RENEW_AHEAD_DAYS before clients would notice anything.

Reads only what an anonymous client reads (the v2 pair from
releases/latest/download) plus the public key in source/update_verify_keys.h:
no credentials, no signing key, nothing written.

Exit status: 0 current, 1 renew now (or expired/invalid), 2 could not check.
"""

import argparse
import base64
import binascii
import hashlib
import os
import sys
import time
import urllib.parse
from pathlib import Path

import release_post
import update_freshness
import update_signing

RENEW_AHEAD_DAYS = 10
CLOCK_SKEW_SECONDS = 300  # GC_UPDATE_FRESH_CLOCK_SKEW in update_freshness_policy.h


def assess(envelope, signature_text, pubkey_hex, now, renew_ahead_seconds):
    """(ok, message) for one delivered v2 pair.  Pure: no network, no clock."""
    try:
        signature = base64.b64decode(signature_text.strip(), validate=True)
    except (binascii.Error, ValueError):
        return False, "the delivered v2 signature is not valid base64"
    raw = bytes.fromhex(pubkey_hex)
    point = (int.from_bytes(raw[:update_signing.KEY_BYTES], "big"),
             int.from_bytes(raw[update_signing.KEY_BYTES:], "big"))
    if not update_signing.verify(point, envelope, signature):
        return False, ("the delivered v2 envelope does not verify against "
                       "GC_UPDATE_PUBLIC_KEY_ACTIVE")
    try:
        issued, expires, _ = update_freshness.read_header(envelope)
    except ValueError as error:
        return False, f"the delivered v2 envelope is malformed: {error}"
    when = time.strftime("%Y-%m-%d %H:%M UTC", time.gmtime(expires))
    if issued > now + CLOCK_SKEW_SECONDS:
        return False, "the v2 envelope is dated in the future; clients refuse it"
    remaining = expires - now
    if remaining <= 0:
        return False, (f"the v2 envelope EXPIRED at {when}; clients refuse update "
                       "metadata until it is renewed")
    days = remaining / 86400
    if remaining <= renew_ahead_seconds:
        return False, (f"the v2 envelope expires at {when} (in {days:.1f} days); "
                       "renew it with release_post.py --renew-freshness")
    return True, f"the v2 envelope is current; it expires at {when} (in {days:.1f} days)"


def _require_allowlisted_host(url):
    host = urllib.parse.urlparse(url).hostname or ""
    if host not in release_post.ALLOWLISTED_REDIRECT_HOSTS:
        raise ValueError(f"download ended on an unexpected host: {host or '(none)'}")


def _report_failure(message):
    # A workflow annotation puts the reason on the run's summary page.
    if os.environ.get("GITHUB_ACTIONS") == "true":
        print(f"::error title=Update freshness::{message}")
    else:
        print(f"ERROR: {message}")


def run_self_tests():
    # A throwaway key derived from a fixed label: no key material in the file.
    private_key = int.from_bytes(
        hashlib.sha256(b"freshness_watch self-test").digest(), "big") % (update_signing.N - 1) + 1
    public = update_signing.public_key_bytes(
        update_signing.public_key_from_private(private_key)).hex()
    other = update_signing.public_key_bytes(
        update_signing.public_key_from_private(private_key + 1)).hex()
    issued = 1_800_000_000
    envelope = update_freshness.build_envelope(b"format=1\nversion=1.2.3\n", issued)
    expires = issued + update_freshness.MAX_LIFETIME
    signature = base64.b64encode(update_signing.sign(private_key, envelope)).decode()
    ahead = RENEW_AHEAD_DAYS * 86400

    ok, message = assess(envelope, signature + "\n", public, issued + 86400, ahead)
    assert ok and "current" in message, message
    # The reminder window, its exact edge, and expiry.
    ok, message = assess(envelope, signature, public, expires - ahead + 1, ahead)
    assert not ok and "renew it" in message, message
    ok, _ = assess(envelope, signature, public, expires - ahead - 1, ahead)
    assert ok
    ok, message = assess(envelope, signature, public, expires, ahead)
    assert not ok and "EXPIRED" in message, message
    # Anything a client would refuse is a failure, never "current".
    ok, message = assess(envelope, signature, public,
                         issued - CLOCK_SKEW_SECONDS - 1, ahead)
    assert not ok and "future" in message, message
    tampered = envelope.replace(b"1.2.3", b"1.2.4")
    assert not assess(tampered, signature, public, issued, ahead)[0]
    assert not assess(envelope, signature, other, issued, ahead)[0]
    assert not assess(envelope, "not base64!", public, issued, ahead)[0]
    unsigned_header = b"freshness=1\nissued=5\nexpires=4\n"
    bad = base64.b64encode(update_signing.sign(private_key, unsigned_header)).decode()
    ok, message = assess(unsigned_header, bad, public, 4, ahead)
    assert not ok and "malformed" in message, message

    _require_allowlisted_host("https://release-assets.githubusercontent.com/x")
    for url in ("https://example.invalid/x", "file:///tmp/x"):
        try:
            _require_allowlisted_host(url)
        except ValueError:
            continue
        raise AssertionError(f"unexpected host accepted: {url}")
    print("freshness_watch self-tests passed")


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--repo", default=release_post.DEFAULT_REPO)
    parser.add_argument("--renew-ahead-days", type=int, default=RENEW_AHEAD_DAYS)
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args(argv)
    if args.self_test:
        run_self_tests()
        return 0

    root = Path(__file__).resolve().parent.parent
    base = f"https://github.com/{args.repo}/releases/latest/download"
    try:
        envelope, final_url = release_post.fetch_url(
            f"{base}/{update_freshness.MANIFEST_ASSET}")
        _require_allowlisted_host(final_url)
        signature, final_url = release_post.fetch_url(
            f"{base}/{update_freshness.SIGNATURE_ASSET}")
        _require_allowlisted_host(final_url)
        pubkey_hex = release_post.extract_active_public_key_hex(root)
    except (OSError, ValueError) as error:
        _report_failure(f"could not check the published freshness envelope: {error}")
        return 2
    ok, message = assess(envelope, signature.decode("ascii", "replace"), pubkey_hex,
                         int(time.time()), args.renew_ahead_days * 86400)
    if not ok:
        _report_failure(message)
        return 1
    print(message)
    return 0


if __name__ == "__main__":
    sys.exit(main())
