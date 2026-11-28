"""OpenPGP header signatures for the Fedora/openSUSE rpms.

rpm verifies a v4 package through a detached OpenPGP signature over the exact
main-header bytes, stored in the signature header (RPMSIGTAG_RSA for RSA keys,
RPMSIGTAG_DSA for every other algorithm).  The main header already carries the
SHA-256 payload digest, so a header signature covers the payload as well; this
is what `rpm -K`, dnf, zypper and rpm 6's default `%_pkgverify_level all` check.

Signing goes through the local `gpg` with the key named by
GREENCURVE_RPM_SIGNING_KEY (a fingerprint) in the active GNUPGHOME.  The release
workflow imports the private key from a secret into a throwaway GNUPGHOME and
sets GREENCURVE_REQUIRE_RPM_SIGNATURE=1, so an unsigned release cannot happen
silently.  The matching public key is published at PUBLIC_KEY_RELPATH and every
signature is verified against it with gpgv before a package is accepted.
"""
import os
import shutil
import subprocess
import tempfile

SIGNING_KEY_ENV = "GREENCURVE_RPM_SIGNING_KEY"
REQUIRE_ENV = "GREENCURVE_REQUIRE_RPM_SIGNATURE"
PUBLIC_KEY_RELPATH = os.path.join("packaging", "greencurve-packages.asc")

SIGTAG_DSA = 267   # RPMSIGTAG_DSA: header-only signature, non-RSA keys
SIGTAG_RSA = 268   # RPMSIGTAG_RSA: header-only signature, RSA keys

# OpenPGP public-key algorithm ids (RFC 4880 9.1 / RFC 9580 9.1).
_ALGO_RSA = (1, 3)
# Hashes rpm (and Fedora's crypto policy) accepts for package signatures.
_ACCEPTED_HASHES = {8: "SHA256", 9: "SHA384", 10: "SHA512"}


def gpg_tool(name):
    """Path of gpg/gpgv/gpgconf, or None.  On Windows GnuPG for Windows wins
    over an MSYS build earlier on PATH (e.g. Git's), because only the native
    build understands a native Windows GNUPGHOME path."""
    if os.name == "nt":
        for root in (os.environ.get("ProgramFiles"), os.environ.get("ProgramFiles(x86)")):
            candidate = os.path.join(root or "", "GnuPG", "bin", name + ".exe")
            if root and os.path.isfile(candidate):
                return candidate
    return shutil.which(name)


def _gpg():
    gpg = gpg_tool("gpg")
    if not gpg:
        raise RuntimeError("rpm signing requested but gpg is not installed")
    return gpg


def parse_signature_packet(sig):
    """Return (version, pubkey_algo, hash_algo) of a binary OpenPGP signature
    packet; rejects anything rpm could not use."""
    if not sig or not sig[0] & 0x80:
        raise RuntimeError("not a binary OpenPGP packet")
    if sig[0] & 0x40:                       # new-format header
        tag = sig[0] & 0x3F
        first = sig[1]
        if first < 192:
            body = 2
        elif first < 224:
            body = 3
        elif first == 255:
            body = 6
        else:
            raise RuntimeError("partial-length OpenPGP signature packets are not supported")
    else:                                   # old-format header
        tag = (sig[0] >> 2) & 0x0F
        if sig[0] & 3 == 3:
            raise RuntimeError("indeterminate-length OpenPGP packets are not supported")
        body = 1 + (1, 2, 4)[sig[0] & 3]
    if tag != 2:
        raise RuntimeError(f"OpenPGP packet tag {tag} is not a signature")
    version, sig_type, pubkey_algo, hash_algo = sig[body], sig[body + 1], sig[body + 2], sig[body + 3]
    if version not in (4, 6):
        raise RuntimeError(f"unsupported OpenPGP signature version {version}")
    if sig_type != 0x00:
        raise RuntimeError(f"OpenPGP signature type {sig_type:#x} is not a binary-document signature")
    if hash_algo not in _ACCEPTED_HASHES:
        raise RuntimeError(f"OpenPGP signature hash algorithm {hash_algo} is not accepted by rpm")
    return version, pubkey_algo, hash_algo


def signature_tag(sig):
    """The signature-header tag rpm expects for this signature."""
    _, pubkey_algo, _ = parse_signature_packet(sig)
    return SIGTAG_RSA if pubkey_algo in _ALGO_RSA else SIGTAG_DSA


def sign_header(header_blob, key):
    """Detached binary OpenPGP signature (SHA-256) over the main header."""
    result = subprocess.run(
        [_gpg(), "--batch", "--no-tty", "--yes", "--pinentry-mode", "loopback",
         "--passphrase", "", "--local-user", key, "--digest-algo", "SHA256",
         "--detach-sign", "--output", "-"],
        input=header_blob, capture_output=True)
    if result.returncode != 0 or not result.stdout:
        detail = result.stderr.decode(errors="replace").strip().splitlines()[-1:] or ["no output"]
        raise RuntimeError(f"gpg failed to sign the rpm header: {detail[0]}")
    parse_signature_packet(result.stdout)
    return result.stdout


def verify_header_signature(header_blob, sig, public_key_path):
    """gpgv-verify `sig` over `header_blob` with exactly the published key."""
    gpgv = gpg_tool("gpgv")
    if not gpgv:
        raise RuntimeError("gpgv is required to verify rpm signatures")
    with open(public_key_path, "rb") as handle:
        armored = handle.read()
    with tempfile.TemporaryDirectory(prefix="gc-rpmsig-") as tmp:
        keyring = os.path.join(tmp, "key.gpg")
        dearmor = subprocess.run([_gpg(), "--batch", "--dearmor", "--output", keyring],
                                 input=armored, capture_output=True)
        if dearmor.returncode != 0:
            raise RuntimeError(f"cannot read the published rpm public key {public_key_path}")
        data_path = os.path.join(tmp, "header")
        sig_path = os.path.join(tmp, "header.sig")
        with open(data_path, "wb") as handle:
            handle.write(header_blob)
        with open(sig_path, "wb") as handle:
            handle.write(sig)
        result = subprocess.run([gpgv, "--keyring", keyring, sig_path, data_path],
                                capture_output=True)
    if result.returncode != 0:
        raise RuntimeError("rpm header signature does not verify against the published key "
                           f"{os.path.basename(public_key_path)}")


class RpmSigner:
    """Callable header signer bound to one key, verifying every signature it
    makes against the published public key when that key exists."""

    def __init__(self, key, public_key_path=None):
        self.key = key
        self.public_key_path = public_key_path

    def __call__(self, header_blob):
        sig = sign_header(header_blob, self.key)
        if self.public_key_path:
            verify_header_signature(header_blob, sig, self.public_key_path)
        return sig


def signer_from_environment(script_dir):
    """The release signer, or None for an unsigned local build.

    Raises when GREENCURVE_REQUIRE_RPM_SIGNATURE=1 but no key is configured, or
    when a key is configured without the published public key to check it."""
    key = os.environ.get(SIGNING_KEY_ENV, "").strip()
    required = os.environ.get(REQUIRE_ENV, "") == "1"
    public_key = os.path.join(script_dir, PUBLIC_KEY_RELPATH)
    if not key:
        if required:
            raise RuntimeError(f"{REQUIRE_ENV}=1 but {SIGNING_KEY_ENV} is not set; "
                               "refusing to build unsigned release rpms")
        print("  linux-pkg: rpm signing not configured; building unsigned rpms (local build)")
        return None
    if not os.path.isfile(public_key):
        raise RuntimeError(f"{SIGNING_KEY_ENV} is set but {PUBLIC_KEY_RELPATH} is missing; "
                           "publish the public key so signatures can be verified")
    print(f"  linux-pkg: signing rpms with key ...{key[-16:]}")
    return RpmSigner(key, public_key)
