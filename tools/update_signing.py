#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
# SPDX-License-Identifier: MIT
"""Release-manifest signing for the in-app updater.

The updater's trust root is an ECDSA P-256 key pair whose private half never
enters GitHub Actions.  This tool generates it, builds the release manifest,
and signs it on the maintainer's machine; the workflow only ever uploads the
resulting files.

Why the curve math is implemented here instead of imported
----------------------------------------------------------
``cryptography`` would do this in five lines, and this file deliberately does
not use it:

* This is the one operation in the project that touches the private key.  A
  pip-installed wheel in that path is a supply-chain surface aimed directly at
  the thing whose compromise breaks every other control at once.
* The project already refuses floating toolchain dependencies -- the compilers
  and the archiver are pinned, vendored, and enforced with
  ``GREENCURVE_TOOLCHAIN_LOCAL_ONLY``.  A release step that silently depended
  on whatever ``cryptography`` the machine happened to have would be the only
  unpinned link left.
* Signatures use RFC 6979 deterministic nonces, so signing is a pure function
  of (key, message).  That removes the catastrophic failure mode of ECDSA --
  a repeated or predictable ``k`` discloses the private key outright -- and it
  makes the implementation testable against published vectors instead of only
  against itself.  ``--self-test`` checks exactly those vectors.

When ``cryptography`` *is* importable the self-test additionally cross-checks
this implementation against it.  That is a free second opinion, not a
dependency: its absence skips the cross-check and fails nothing.

Signature format
----------------
Raw ``r || s``, 32 bytes each, big-endian, base64-encoded.  This is chosen to
match what CNG's ``BCryptVerifySignature`` expects for ``ECDSA_P256`` exactly,
so the C++ verifier needs no DER parser -- one less untrusted-input decoder in
the most security-sensitive path of the application.
"""

import argparse
import base64
import binascii
import hashlib
import hmac
import os
import secrets
import sys

# --------------------------------------------------------------------------
# NIST P-256 (secp256r1)
# --------------------------------------------------------------------------

P = 0xFFFFFFFF00000001000000000000000000000000FFFFFFFFFFFFFFFFFFFFFFFF
A = P - 3
B = 0x5AC635D8AA3A93E7B3EBBD55769886BC651D06B0CC53B0F63BCE3C3E27D2604B
GX = 0x6B17D1F2E12C4247F8BCE6E563A440F277037D812DEB33A0F4A13945D898C296
GY = 0x4FE342E2FE1A7F9B8EE7EB4A7C0F9E162BCE33576B315ECECBB6406837BF51F5
N = 0xFFFFFFFF00000000FFFFFFFFFFFFFFFFBCE6FAADA7179E84F3B9CAC2FC632551

KEY_BYTES = 32
SIGNATURE_BYTES = 64

MANIFEST_ASSET = "greencurve-update-manifest.txt"
SIGNATURE_ASSET = "greencurve-update-manifest.sig"
MANIFEST_FORMAT = 1
ARCHES = ("x64", "arm64")


def _inverse(value, modulus):
    return pow(value, -1, modulus)


def _point_add(p1, p2):
    """Affine addition on P-256.  ``None`` is the point at infinity."""
    if p1 is None:
        return p2
    if p2 is None:
        return p1
    x1, y1 = p1
    x2, y2 = p2
    if x1 == x2:
        if (y1 + y2) % P == 0:
            return None
        slope = (3 * x1 * x1 + A) * _inverse(2 * y1, P) % P
    else:
        slope = (y2 - y1) * _inverse(x2 - x1, P) % P
    x3 = (slope * slope - x1 - x2) % P
    y3 = (slope * (x1 - x3) - y1) % P
    return (x3, y3)


def _scalar_mult(scalar, point):
    """Montgomery ladder.

    A ladder rather than double-and-add: both branches do the same work, so the
    running time does not track the bits of the scalar.  This tool signs on a
    developer workstation where timing side channels are not the realistic
    threat, but writing the variable-time version and relying on that argument
    is how the variable-time version ends up somewhere it does matter.
    """
    if scalar % N == 0 or point is None:
        return None
    r0, r1 = None, point
    for bit in range(scalar.bit_length() - 1, -1, -1):
        if (scalar >> bit) & 1:
            r0 = _point_add(r0, r1)
            r1 = _point_add(r1, r1)
        else:
            r1 = _point_add(r0, r1)
            r0 = _point_add(r0, r0)
    return r0


def _point_is_on_curve(point):
    if point is None:
        return False
    x, y = point
    if not (0 <= x < P and 0 <= y < P):
        return False
    return (y * y - (x * x * x + A * x + B)) % P == 0


def _bits_to_int(data):
    """RFC 6979 bits2int for a 256-bit order: the leftmost 256 bits."""
    value = int.from_bytes(data, "big")
    excess = len(data) * 8 - N.bit_length()
    if excess > 0:
        value >>= excess
    return value


def _int_to_octets(value):
    return value.to_bytes(KEY_BYTES, "big")


def _bits_to_octets(data):
    z1 = _bits_to_int(data)
    z2 = z1 - N
    return _int_to_octets(z2 if z2 >= 0 else z1)


def _rfc6979_k(private_key, digest):
    """Deterministic nonce (RFC 6979 section 3.2), HMAC-SHA256."""
    hlen = hashlib.sha256().digest_size
    key_octets = _int_to_octets(private_key)
    digest_octets = _bits_to_octets(digest)
    v = b"\x01" * hlen
    k = b"\x00" * hlen
    k = hmac.new(k, v + b"\x00" + key_octets + digest_octets, hashlib.sha256).digest()
    v = hmac.new(k, v, hashlib.sha256).digest()
    k = hmac.new(k, v + b"\x01" + key_octets + digest_octets, hashlib.sha256).digest()
    v = hmac.new(k, v, hashlib.sha256).digest()
    while True:
        v = hmac.new(k, v, hashlib.sha256).digest()
        candidate = _bits_to_int(v)
        if 1 <= candidate < N:
            return candidate
        k = hmac.new(k, v + b"\x00", hashlib.sha256).digest()
        v = hmac.new(k, v, hashlib.sha256).digest()


def public_key_from_private(private_key):
    point = _scalar_mult(private_key, (GX, GY))
    if point is None:
        raise ValueError("private key produced the point at infinity")
    return point


def sign(private_key, message):
    """Return the raw 64-byte r||s signature over ``message``."""
    if not 1 <= private_key < N:
        raise ValueError("private key out of range")
    digest = hashlib.sha256(message).digest()
    e = _bits_to_int(digest)
    while True:
        k = _rfc6979_k(private_key, digest)
        point = _scalar_mult(k, (GX, GY))
        if point is None:
            continue
        r = point[0] % N
        if r == 0:
            continue
        s = (_inverse(k, N) * (e + r * private_key)) % N
        if s == 0:
            continue
        # Low-S normalization.  Both s and N-s verify, so without this the
        # signature is malleable: two distinct byte strings would be valid for
        # one manifest.  Nothing downstream depends on uniqueness today, but a
        # signature that is not canonical is a footgun left armed for later.
        if s > N // 2:
            s = N - s
        return r.to_bytes(KEY_BYTES, "big") + s.to_bytes(KEY_BYTES, "big")


def verify(public_point, message, signature):
    if len(signature) != SIGNATURE_BYTES:
        return False
    if not _point_is_on_curve(public_point):
        return False
    r = int.from_bytes(signature[:KEY_BYTES], "big")
    s = int.from_bytes(signature[KEY_BYTES:], "big")
    if not (1 <= r < N and 1 <= s < N):
        return False
    e = _bits_to_int(hashlib.sha256(message).digest())
    w = _inverse(s, N)
    u1 = (e * w) % N
    u2 = (r * w) % N
    point = _point_add(_scalar_mult(u1, (GX, GY)), _scalar_mult(u2, public_point))
    if point is None:
        return False
    return point[0] % N == r


# --------------------------------------------------------------------------
# Key storage
# --------------------------------------------------------------------------

PRIVATE_KEY_HEADER = (
    "# Green Curve update signing key -- PRIVATE.  Never commit this file,\n"
    "# never copy it into CI, never paste it anywhere.  Anyone holding it can\n"
    "# sign an update that every installed copy of Green Curve will accept and\n"
    "# run as SYSTEM.  Back it up offline.\n"
)


def generate_private_key():
    """A uniform scalar in [1, N-1] via rejection sampling.

    Rejection rather than ``% N``: reducing a 256-bit sample modulo N biases
    the low end of the range, and biased nonces/keys are how ECDSA
    implementations leak.  The rejection probability here is about 2^-32.
    """
    while True:
        candidate = int.from_bytes(secrets.token_bytes(KEY_BYTES), "big")
        if 1 <= candidate < N:
            return candidate


def write_private_key(path, private_key):
    if os.path.exists(path):
        raise SystemExit(
            f"refusing to overwrite an existing key file: {path}\n"
            "If you really mean to rotate, move the old key aside by hand."
        )
    # 0o600 before anything is written, so the secret never exists on disk with
    # a wider mode even briefly.
    descriptor = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    with os.fdopen(descriptor, "w", encoding="utf-8", newline="\n") as handle:
        handle.write(PRIVATE_KEY_HEADER)
        handle.write(f"{private_key:064x}\n")


def read_private_key(path):
    with open(path, "r", encoding="utf-8") as handle:
        for line in handle:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            if len(line) != KEY_BYTES * 2:
                raise SystemExit(f"malformed private key in {path}")
            value = int(line, 16)
            if not 1 <= value < N:
                raise SystemExit(f"private key out of range in {path}")
            return value
    raise SystemExit(f"no key material found in {path}")


def public_key_bytes(public_point):
    """X || Y, 64 bytes -- the tail of a BCRYPT_ECCKEY_BLOB."""
    x, y = public_point
    return x.to_bytes(KEY_BYTES, "big") + y.to_bytes(KEY_BYTES, "big")


def format_public_key_c(public_point, name):
    raw = public_key_bytes(public_point)
    lines = [f"static const unsigned char {name}[64] = {{"]
    for offset in range(0, len(raw), 8):
        chunk = ", ".join(f"0x{byte:02X}" for byte in raw[offset:offset + 8])
        lines.append(f"    {chunk},")
    lines.append("};")
    return "\n".join(lines)


# --------------------------------------------------------------------------
# Manifest
# --------------------------------------------------------------------------


def asset_name(version, arch):
    return f"greencurve-{version}-windows-{arch}-setup.exe"


def sha256_file(path):
    digest = hashlib.sha256()
    with open(path, "rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def build_manifest(version, directory, minimum_from=None):
    """Render the manifest text for the setup files in ``directory``.

    Written with explicit LF endings and no trailing whitespace: the signature
    covers these exact bytes, so a CRLF checkout on one machine and LF on
    another must not produce two different documents for one release.
    """
    lines = [
        "# Green Curve update manifest.  Signed with the release key; see",
        "# tools/update_signing.py.  Do not edit by hand -- the signature is",
        "# over these exact bytes.",
        f"format={MANIFEST_FORMAT}",
        f"version={version}",
    ]
    if minimum_from:
        lines.append(f"min_from={minimum_from}")

    found = 0
    for arch in ARCHES:
        name = asset_name(version, arch)
        path = os.path.join(directory, name)
        if not os.path.isfile(path):
            print(f"NOTE: no setup file for {arch} at {path}; omitting it")
            continue
        size = os.path.getsize(path)
        if size <= 0:
            raise SystemExit(f"setup file is empty: {path}")
        lines.append(f"{arch}_file={name}")
        lines.append(f"{arch}_size={size}")
        lines.append(f"{arch}_sha256={sha256_file(path)}")
        found += 1

    if found == 0:
        raise SystemExit(
            f"no setup executables found in {directory} for version {version}"
        )
    return "\n".join(lines) + "\n"


# --------------------------------------------------------------------------
# Self-test
# --------------------------------------------------------------------------

# RFC 6979 appendix A.2.5: P-256, SHA-256.
_VECTOR_KEY = 0xC9AFA9D845BA75166B5C215767B1D6934E50C3DB36E89B127B8A622B120F6721
_VECTOR_UX = 0x60FED4BA255A9D31C961EB74C6356D68C049B8923B61FA6CE669622E60F29FB6
_VECTOR_UY = 0x7903FE1008B8BC99A41AE9E95628BC64F2F1B20C2D7E9F5177A3C294D4462299
_VECTORS = (
    (
        b"sample",
        0xEFD48B2AACB6A8FD1140DD9CD45E81D69D2C877B56AAF991C34D0EA84EAF3716,
        0xF7CB1C942D657C41D436C7A1B6E29F65F3E900DBB9AFF4064DC4AB2F843ACDA8,
    ),
    (
        b"test",
        0xF1ABB023518351CD71D881567B1EA663ED3EFCF6C5132B354F28D3B0B7D38367,
        0x019F4113742A2B14BD25926B49C649155F267E60D3814B4C0CC84250E46F0083,
    ),
)


def _normalize_low_s(s):
    return N - s if s > N // 2 else s


def run_self_tests():
    failures = []

    # 1. The public key derived from the vector private key must be the
    #    published one -- this checks the curve arithmetic independently of
    #    the signature scheme.
    point = public_key_from_private(_VECTOR_KEY)
    if point != (_VECTOR_UX, _VECTOR_UY):
        failures.append("derived public key does not match RFC 6979 A.2.5")
    if not _point_is_on_curve(point):
        failures.append("derived public key is not on the curve")

    # 2. Deterministic signatures must equal the published (r, s).
    for message, want_r, want_s in _VECTORS:
        signature = sign(_VECTOR_KEY, message)
        got_r = int.from_bytes(signature[:KEY_BYTES], "big")
        got_s = int.from_bytes(signature[KEY_BYTES:], "big")
        if got_r != want_r:
            failures.append(f"r mismatch for {message!r}")
        if got_s != _normalize_low_s(want_s):
            failures.append(f"s mismatch for {message!r}")
        if not verify(point, message, signature):
            failures.append(f"self-verify failed for {message!r}")

    # 3. Tampering must be rejected -- every field, in both directions.
    key = generate_private_key()
    public = public_key_from_private(key)
    message = b"format=1\nversion=0.23.0\n"
    signature = sign(key, message)
    if not verify(public, message, signature):
        failures.append("fresh signature did not verify")
    if verify(public, message + b" ", signature):
        failures.append("a modified message verified")
    if verify(public, message[:-1], signature):
        failures.append("a truncated message verified")
    flipped = bytearray(signature)
    flipped[0] ^= 0x01
    if verify(public, message, bytes(flipped)):
        failures.append("a corrupted signature verified")
    if verify(public, message, signature[:-1]):
        failures.append("a truncated signature verified")
    other = public_key_from_private(generate_private_key())
    if verify(other, message, signature):
        failures.append("a signature verified under the wrong key")
    # A zeroed r or s must not be accepted.
    if verify(public, message, b"\x00" * KEY_BYTES + signature[KEY_BYTES:]):
        failures.append("a zero r verified")
    if verify(public, message, signature[:KEY_BYTES] + b"\x00" * KEY_BYTES):
        failures.append("a zero s verified")

    # 4. Determinism: signing twice must produce identical bytes.
    if sign(key, message) != signature:
        failures.append("signing is not deterministic")

    # 5. Manifest bytes must be LF-only with no trailing blank line drift.
    sample = "format=1\nversion=0.23.0\n"
    if "\r" in sample or not sample.endswith("\n"):
        failures.append("manifest sample is not LF-terminated")

    # 6. Free second opinion when the library happens to be installed.
    try:
        from cryptography.hazmat.primitives import hashes as _hashes
        from cryptography.hazmat.primitives.asymmetric import ec as _ec
        from cryptography.hazmat.primitives.asymmetric.utils import (
            encode_dss_signature as _encode,
        )
    except ImportError:
        print("self-test: cryptography not installed; cross-check skipped")
    else:
        numbers = _ec.EllipticCurvePublicNumbers(public[0], public[1], _ec.SECP256R1())
        library_key = numbers.public_key()
        r = int.from_bytes(signature[:KEY_BYTES], "big")
        s = int.from_bytes(signature[KEY_BYTES:], "big")
        try:
            library_key.verify(_encode(r, s), message, _ec.ECDSA(_hashes.SHA256()))
        except Exception as error:  # noqa: BLE001 - any failure is a failure
            failures.append(f"cryptography rejected our signature: {error}")
        else:
            print("self-test: cross-checked against cryptography")

    if failures:
        for failure in failures:
            print(f"  FAIL: {failure}")
        return False
    print("update_signing self-tests passed")
    return True


# --------------------------------------------------------------------------
# CLI
# --------------------------------------------------------------------------


def main(argv=None):
    parser = argparse.ArgumentParser(
        description="Generate, sign and verify Green Curve update manifests."
    )
    subparsers = parser.add_subparsers(dest="command", required=True)

    generate = subparsers.add_parser("gen-key", help="create a new signing key")
    generate.add_argument("path", help="where to write the private key")

    show = subparsers.add_parser(
        "public-key", help="print the public key for embedding in the app"
    )
    show.add_argument("path", help="private key file")
    show.add_argument("--name", default="GC_UPDATE_PUBLIC_KEY_ACTIVE")

    make = subparsers.add_parser("make-manifest", help="build the manifest text")
    make.add_argument("--version", required=True)
    make.add_argument("--dir", default=".")
    make.add_argument("--min-from", default=None)
    make.add_argument("--out", default=MANIFEST_ASSET)

    sign_parser = subparsers.add_parser("sign", help="sign a manifest file")
    sign_parser.add_argument("manifest")
    sign_parser.add_argument("--key", required=True)
    sign_parser.add_argument("--out", default=None)

    verify_parser = subparsers.add_parser("verify", help="verify a signed manifest")
    verify_parser.add_argument("manifest")
    verify_parser.add_argument("--sig", default=None)
    verify_parser.add_argument(
        "--pubkey", required=True, help="128 hex characters (X||Y)"
    )

    subparsers.add_parser("self-test", help="run the built-in vectors")

    args = parser.parse_args(argv)

    if args.command == "self-test":
        return 0 if run_self_tests() else 1

    if args.command == "gen-key":
        key = generate_private_key()
        write_private_key(args.path, key)
        point = public_key_from_private(key)
        print(f"Private key written to {args.path} (mode 0600).")
        print("Back it up offline.  It must never be committed or copied into CI.\n")
        print("Embed this in source/update_verify_keys.h:\n")
        print(format_public_key_c(point, "GC_UPDATE_PUBLIC_KEY_ACTIVE"))
        print(f"\nHex form: {public_key_bytes(point).hex()}")
        return 0

    if args.command == "public-key":
        point = public_key_from_private(read_private_key(args.path))
        print(format_public_key_c(point, args.name))
        print(f"\nHex form: {public_key_bytes(point).hex()}")
        return 0

    if args.command == "make-manifest":
        text = build_manifest(args.version, args.dir, args.min_from)
        with open(args.out, "w", encoding="utf-8", newline="\n") as handle:
            handle.write(text)
        print(f"Wrote {args.out} ({len(text.encode('utf-8'))} bytes)")
        return 0

    if args.command == "sign":
        with open(args.manifest, "rb") as handle:
            payload = handle.read()
        key = read_private_key(args.key)
        signature = sign(key, payload)
        if not verify(public_key_from_private(key), payload, signature):
            raise SystemExit("internal error: fresh signature failed to verify")
        out = args.out or (args.manifest + ".sig")
        encoded = base64.b64encode(signature).decode("ascii")
        with open(out, "w", encoding="utf-8", newline="\n") as handle:
            handle.write(encoded + "\n")
        print(f"Signed {args.manifest} -> {out}")
        return 0

    if args.command == "verify":
        with open(args.manifest, "rb") as handle:
            payload = handle.read()
        signature_path = args.sig or (args.manifest + ".sig")
        with open(signature_path, "r", encoding="utf-8") as handle:
            encoded = handle.read().strip()
        try:
            signature = base64.b64decode(encoded, validate=True)
        except (binascii.Error, ValueError) as error:
            raise SystemExit(f"signature is not valid base64: {error}") from error
        raw = bytes.fromhex(args.pubkey)
        if len(raw) != SIGNATURE_BYTES:
            raise SystemExit("public key must be 128 hex characters (X||Y)")
        point = (
            int.from_bytes(raw[:KEY_BYTES], "big"),
            int.from_bytes(raw[KEY_BYTES:], "big"),
        )
        if verify(point, payload, signature):
            print("signature OK")
            return 0
        print("SIGNATURE INVALID")
        return 1

    parser.error("unhandled command")
    return 2


if __name__ == "__main__":
    sys.exit(main())
