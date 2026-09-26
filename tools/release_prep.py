"""Check that the release workflow will select the current release notes."""

import argparse
import os
import re
import subprocess
import sys
from pathlib import Path

_RELEASE_HEADING = re.compile(r"^## (\d+\.\d+(?:\.\d+)?)$", re.MULTILINE)
ALLOWED_CATEGORIES = frozenset(
    {"New", "Improved", "Fixed", "Changed", "Deprecated", "Removed", "Security"}
)


def validate_release_notes(version, changelog):
    sections = list(_RELEASE_HEADING.finditer(changelog))
    if len(sections) < 2 or sections[0].group(1) != version:
        raise ValueError(f"latest versioned changelog section must be ## {version}")
    current = changelog[sections[0].end():sections[1].start()]
    previous = sections[1].group(1)
    subheads = re.findall(r"^### (.+)$", current, re.MULTILINE)
    if len(subheads) < 3:
        raise ValueError("current release notes need at least one category subheading plus Compatibility notes and Downloads and verification")
    categories = subheads[:-2]
    tail = subheads[-2:]
    if tail != ["Compatibility notes", "Downloads and verification"]:
        raise ValueError("current release notes must end with Compatibility notes and Downloads and verification")
    for cat in categories:
        if cat not in ALLOWED_CATEGORIES and cat not in ("Highlights", "Fixes & Hardening"):
            raise ValueError(f"invalid category subheading: '{cat}'. Allowed: {', '.join(sorted(ALLOWED_CATEGORIES))}")
    compare = (f"**Full changelog:** [{previous}...{version}]"
               f"(https://github.com/aufkrawall/green-curve/compare/{previous}...{version})")
    if compare not in current:
        raise ValueError("current release notes need the previous-to-current compare link")


def check_ssh_key_passphrase(key_path):
    """Ensure an SSH private key can decrypt non-interactively without a passphrase."""
    key = Path(key_path)
    if not key.exists():
        return False, f"key not found: {key}"
    res = subprocess.run(
        ["ssh-keygen", "-y", "-P", "", "-f", str(key)],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True
    )
    if res.returncode != 0:
        return False, (
            "SSH signing key is password-protected or invalid. "
            "Actions secrets and automated tag signing require a passphrase-free key. "
            f"Remove passphrase with: ssh-keygen -p -P <old> -N \"\" -f \"{key}\""
        )
    return True, res.stdout.strip()


def check_update_signing_key(root, key_path=None):
    """Validate offline update signing key existence, owner-only ACL, and public key match."""
    root = Path(root)
    if key_path:
        key = Path(key_path)
    else:
        userprofile = os.environ.get("USERPROFILE") or os.environ.get("HOME")
        if not userprofile:
            return False, "could not determine user profile directory"
        key = Path(userprofile) / ".greencurve-keys" / "update-signing-key.txt"
    if not key.exists():
        return False, f"update signing key not found at {key}"

    if sys.platform == "win32":
        try:
            tools_dir = str(root / "tools")
            if tools_dir not in sys.path:
                sys.path.insert(0, tools_dir)
            import windows_key_acl
            windows_key_acl.verify_owner_only(str(key))
        except (ImportError, Exception) as exc:
            return False, f"ACL check failed: {exc}"

    res = subprocess.run(
        [sys.executable, str(root / "tools" / "update_signing.py"), "public-key", str(key)],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True
    )
    if res.returncode != 0:
        return False, f"failed to derive public key: {res.stderr.strip()}"
    m_hex = re.search(r"Hex form:\s*([0-9a-fA-F]{128})", res.stdout)
    if not m_hex:
        return False, "could not parse public key hex from update_signing.py"
    pubkey_hex = m_hex.group(1).lower()

    header_text = (root / "source" / "update_verify_keys.h").read_text(encoding="utf-8")
    m_head = re.search(r"GC_UPDATE_PUBLIC_KEY_ACTIVE\[[^\]]+\]\s*=\s*\{(.*?)\};", header_text, re.DOTALL)
    if not m_head:
        return False, "could not find GC_UPDATE_PUBLIC_KEY_ACTIVE in update_verify_keys.h"
    expected_hex = bytes(int(x, 16) for x in re.findall(r"0x([0-9A-Fa-f]{2})", m_head.group(1))).hex().lower()
    if pubkey_hex != expected_hex:
        return False, f"public key mismatch: derived {pubkey_hex}, expected {expected_hex}"

    return True, pubkey_hex


def check_repo(root):
    self_test()
    root = Path(root)
    version = (root / "VERSION").read_text(encoding="utf-8").strip()
    changelog = (root / "CHANGELOG.md").read_text(encoding="utf-8")
    validate_release_notes(version, changelog)
    print(f"Release notes for {version} match the current version and previous tag")


def run_preflight(root, key_path=None):
    root = Path(root)
    print("=== Release preflight validation ===")
    check_repo(root)

    print("Checking offline update signing key...")
    ok, detail = check_update_signing_key(root, key_path)
    if not ok:
        raise ValueError(f"update signing key check failed: {detail}")
    print(f"  Update signing key OK (matches GC_UPDATE_PUBLIC_KEY_ACTIVE): {detail[:16]}...{detail[-16:]}")

    userprofile = os.environ.get("USERPROFILE") or os.environ.get("HOME")
    if userprofile:
        ssh_key = Path(userprofile) / ".ssh" / "greencurve_tag_signer"
        if ssh_key.exists():
            print("Checking SSH tag signing key...")
            ok, detail = check_ssh_key_passphrase(ssh_key)
            if not ok:
                raise ValueError(f"SSH tag signing key check failed: {detail}")
            print("  SSH tag signing key OK (passphrase-free, verified for Actions)")

    print("Preflight checks passed: changelog, version, and signing keys are ready for release.")


def self_test():
    good = ("## 0.27.0\n### New\n- **Feature.** Detail.\n### Improved\n- **Improvement.** Detail.\n"
            "### Fixed\n- **Bugfix.** Detail.\n### Compatibility notes\n"
            "### Downloads and verification\n"
            "**Full changelog:** [0.26.0...0.27.0]"
            "(https://github.com/aufkrawall/green-curve/compare/0.26.0...0.27.0)\n"
            "## 0.26.0\nEarlier release\n")
    validate_release_notes("0.27.0", good)
    validate_release_notes("0.27.0", good.replace("### New\n- **Feature.** Detail.\n", ""))
    validate_release_notes("0.27.0", good.replace("### New", "### Highlights"))
    for version, notes in (("0.28.0", good),
                           ("0.27.0", good.replace("### Compatibility notes", "")),
                           ("0.27.0", good.replace("### Compatibility notes",
                                                    "### Extra\n### Compatibility notes")),
                           ("0.27.0", good.replace("0.26.0...0.27.0", "0.25.2...0.27.0"))):
        try:
            validate_release_notes(version, notes)
        except ValueError:
            continue
        raise AssertionError("release note mismatch was accepted")
    print("release_prep self-tests passed")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--preflight", action="store_true", help="Run full preflight checks including local signing keys")
    parser.add_argument("--key", help="Path to offline update signing private key")
    args = parser.parse_args()

    root = Path(__file__).resolve().parent.parent
    if args.preflight:
        run_preflight(root, args.key)
    else:
        check_repo(root)


if __name__ == "__main__":
    main()
