# SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
# SPDX-License-Identifier: MIT
"""Additive signed metadata for clients requiring a bounded publication age."""
import re
import time

MANIFEST_ASSET = "greencurve-update-v2.txt"
SIGNATURE_ASSET = "greencurve-update-v2.sig"
MAX_LIFETIME = 30 * 24 * 60 * 60


def build_envelope(manifest, issued=None):
    issued = int(time.time()) if issued is None else issued
    if not isinstance(issued, int) or not 0 < issued < 10**12 - MAX_LIFETIME:
        raise ValueError("invalid issuance timestamp")
    return (f"freshness=1\nissued={issued}\nexpires={issued + MAX_LIFETIME}\n".encode("ascii")
            + manifest)


def unwrap(envelope, now=None):
    now = int(time.time()) if now is None else now
    match = re.match(rb"freshness=1\nissued=([0-9]{1,12})\nexpires=([0-9]{1,12})\n", envelope)
    if not match:
        raise ValueError("missing signed freshness header")
    issued, expires = map(int, match.groups())
    if not (0 < issued < expires <= issued + MAX_LIFETIME and
            now > 0 and issued <= now + 300 and now < expires):
        raise ValueError("expired or future-dated update metadata")
    return envelope[match.end():]


def run_self_tests():
    payload = b"format=1\nversion=1.2.3\n"
    envelope = build_envelope(payload, 1000)
    assert unwrap(envelope, 1000) == payload
    assert unwrap(envelope, 700) == payload
    for now in (699, 1000 + MAX_LIFETIME, 0):
        try:
            unwrap(envelope, now)
        except ValueError:
            continue
        raise AssertionError("invalid freshness accepted")
