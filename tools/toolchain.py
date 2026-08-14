"""Pinned build toolchain: local resolution, offline enforcement, verification.

build.py has always pinned every compiler archive by SHA-256.  This module is
what turns those pins into an enforceable policy:

  * archives are resolved from the vendored ``compilers/`` tree before any
    network fetch, and the attested release build runs with the network path
    refused outright (``GREENCURVE_TOOLCHAIN_LOCAL_ONLY``), so a release can
    only ever be compiled by tools this repository already vouches for;
  * an extracted toolchain is re-checked against every digest recorded in
    ``compilers/<tool>-<version>/manifest.json``, not just the one driver
    binary.  That matters on the CI cache path: a restored cache is attacker-
    writable in a way a pinned archive is not, and llvm-mingw's Linux
    ``x86_64-w64-mingw32-clang++`` is a symlink to a 3 KB wrapper script that
    is byte-identical across upstream releases -- verifying only that file
    would accept a wholesale toolchain swap.

The manifests are the single source of truth for the digests.  build.py keeps
its own copies of the few constants its own gates assert on; check_pins()
fails the build if the two ever drift apart.
"""

import hashlib
import io
import json
import os
import re
import shutil
import sys
import tarfile
import tempfile
from contextlib import redirect_stdout
import urllib.request
import zipfile

LOCAL_ONLY_ENV = "GREENCURVE_TOOLCHAIN_LOCAL_ONLY"

_MANIFEST_CACHE = {}


def local_only():
    """True when the build may not fetch a toolchain over the network."""
    value = os.environ.get(LOCAL_ONLY_ENV, "").strip().lower()
    return value in ("1", "true", "yes", "on")


def host_key(platform=None):
    """Manifest key for the build host ('windows' or 'linux')."""
    platform = platform if platform is not None else sys.platform
    return "windows" if platform == "win32" else "linux"


def sha256_file(path):
    digest = hashlib.sha256()
    with open(path, "rb") as handle:
        for chunk in iter(lambda: handle.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest().lower()


def component_dir(compilers_dir, tool, version):
    return os.path.join(compilers_dir, f"{tool}-{version}")


def load_manifest(compilers_dir, tool, version):
    """Read (and cache) compilers/<tool>-<version>/manifest.json."""
    path = os.path.join(component_dir(compilers_dir, tool, version),
                        "manifest.json")
    cached = _MANIFEST_CACHE.get(path)
    if cached is not None:
        return cached
    try:
        with open(path, "r", encoding="utf-8") as handle:
            manifest = json.load(handle)
    except (OSError, ValueError) as exc:
        print(f"ERROR: cannot read pinned toolchain manifest {path}: {exc}")
        sys.exit(1)
    _MANIFEST_CACHE[path] = manifest
    return manifest


def archive_record(manifest, host=None):
    """The archive entry for *host* in a manifest, or exit if absent."""
    host = host or host_key()
    for entry in manifest.get("archives", []):
        if entry.get("host_platform") == host:
            return entry
    print(f"ERROR: {manifest.get('tool')} {manifest.get('version')} has no "
          f"pinned archive for host '{host}'")
    sys.exit(1)


def resolve_archive(label, archive_name, local_dir, url, sha256, dest_path):
    """Copy *archive_name* out of *local_dir*, or download it, into *dest_path*.

    The vendored copy always wins, so a machine that has run
    ``fetch-toolchain`` never touches the network again.  Either way the result
    is verified against *sha256* before the caller extracts it.
    """
    local_path = os.path.join(local_dir, archive_name)

    if os.path.exists(local_path):
        print(f"Using local {label} archive: {local_path}")
        shutil.copy2(local_path, dest_path)
        if sha256 and not _verify(dest_path, sha256):
            os.remove(dest_path)
            print(f"ERROR: local {label} archive failed SHA-256 verification")
            sys.exit(1)
        return dest_path

    if local_only():
        print(f"ERROR: {LOCAL_ONLY_ENV} is set and {label} is not vendored")
        print(f"  expected: {local_path}")
        print(f"  This build may only use toolchains from compilers/;")
        print(f"  run 'python build.py --fetch-toolchain' first.")
        sys.exit(1)

    print(f"Downloading {label} {archive_name}...")
    try:
        urllib.request.urlretrieve(url, dest_path)
    except Exception as exc:  # noqa: BLE001 - any failure is fatal and reported
        print(f"Failed to download {label}: {exc}")
        print(f"Please obtain from: {url}")
        sys.exit(1)

    if sha256 and not _verify(dest_path, sha256):
        os.remove(dest_path)
        print(f"ERROR: {label} archive SHA-256 verification failed")
        sys.exit(1)
    return dest_path


def _verify(path, expected):
    actual = sha256_file(path)
    if actual == expected.lower():
        return True
    print(f"  expected: {expected.lower()}")
    print(f"  actual:   {actual}")
    return False


def fetch_into_compilers(compilers_dir, tool, version, hosts=None):
    """Populate compilers/<tool>-<version>/ with the pinned archive(s).

    This is the one place that is *allowed* to reach the network under a
    local-only policy, because it is the explicit "go get the pinned bytes"
    step rather than a silent fallback inside a build.
    """
    manifest = load_manifest(compilers_dir, tool, version)
    target_dir = component_dir(compilers_dir, tool, version)
    os.makedirs(target_dir, exist_ok=True)
    wanted = hosts or [host_key()]
    for entry in manifest.get("archives", []):
        if entry.get("host_platform") not in wanted:
            continue
        name = entry["filename"]
        dest = os.path.join(target_dir, name)
        if os.path.exists(dest) and sha256_file(dest) == entry["sha256"].lower():
            print(f"  have {name}")
            continue
        print(f"  get  {name}")
        try:
            urllib.request.urlretrieve(entry["url"], dest + ".part")
        except Exception as exc:  # noqa: BLE001 - reported and fatal
            print(f"ERROR: cannot fetch {name}: {exc}")
            print(f"  from {entry['url']}")
            sys.exit(1)
        if not _verify(dest + ".part", entry["sha256"]):
            os.remove(dest + ".part")
            print(f"ERROR: {name} failed SHA-256 verification")
            sys.exit(1)
        os.replace(dest + ".part", dest)
    return target_dir


def verify_tree(label, root, manifest, host=None):
    """Re-verify an extracted toolchain against its manifest.

    Returns True when every recorded entry matches.  A mismatch is printed and
    reported as False so the caller can refresh from the pinned archive rather
    than build with something it cannot account for.
    """
    host = host or host_key()
    entries = (manifest.get("extracted_binaries") or {}).get(host) or []
    if not entries:
        print(f"WARNING: {label} manifest records no binaries for '{host}'; "
              f"cannot verify the extracted toolchain")
        return False
    by_path = {entry["path"].replace("/", os.sep): entry for entry in entries}
    checked = 0
    root_abs = os.path.abspath(root)
    for entry in entries:
        relative = entry["path"].replace("/", os.sep)
        path = os.path.join(root_abs, relative)
        link = entry.get("symlink")
        if link:
            if not os.path.islink(path):
                print(f"ERROR: {label} pinned symlink is not a symlink: {entry['path']}")
                return False
            actual_link = None
            try:
                actual_link = os.readlink(path)
            except OSError as exc:
                print(f"ERROR: {label} cannot read symlink {entry['path']}: {exc}")
                return False
            if os.path.normpath(actual_link) != os.path.normpath(link):
                print(f"ERROR: {label} symlink text mismatch for {entry['path']}")
                print(f"  pinned: {link}")
                print(f"  actual: {actual_link}")
                return False
            target = os.path.abspath(os.path.join(os.path.dirname(path), actual_link))
            target_rel = os.path.relpath(target, root_abs)
            if target_rel.startswith("..") or os.path.isabs(target_rel):
                print(f"ERROR: {label} symlink escapes the toolchain: {entry['path']}")
                return False
            target_entry = by_path.get(target_rel)
            if not target_entry or target_entry.get("symlink"):
                print(f"ERROR: {label} symlink target is not independently pinned: "
                      f"{entry['path']} -> {target_rel}")
                return False
            checked += 1
            continue
        if os.path.islink(path):
            print(f"ERROR: {label} pinned regular file is a symlink: {entry['path']}")
            return False
        expected = entry.get("sha256")
        if not expected:
            continue
        try:
            actual = sha256_file(path)
        except OSError as exc:
            print(f"ERROR: {label} cannot read {entry['path']}: {exc}")
            return False
        if actual != expected.lower():
            print(f"ERROR: {label} digest mismatch for {entry['path']}")
            print(f"  expected (pinned): {expected.lower()}")
            print(f"  actual:            {actual}")
            return False
        checked += 1
    print(f"  {label}: {checked} pinned binaries verified")
    return True


def safe_extract_target(base_dir, archive_name):
    """Resolve an archive member under base_dir and reject traversal.

    Also strips the archive's single root folder, which is what lets a
    ``zig-windows-x86_64-<version>/`` payload land directly in ``zig/`` --
    upstream renames that folder every release (and reordered the name entirely
    in 0.14.1), so the build must not depend on what it is called.
    """
    relative = archive_name.replace("\\", "/")
    parts = relative.split("/", 1)
    if len(parts) != 2 or not parts[1]:
        return None
    relative = os.path.normpath(parts[1])
    if os.path.isabs(relative) or relative == ".." or relative.startswith(".." + os.sep):
        raise RuntimeError(f"Unsafe archive member: {archive_name}")
    base_abs = os.path.abspath(base_dir)
    target = os.path.abspath(os.path.join(base_abs, relative))
    if os.path.commonpath([base_abs, target]) != base_abs:
        raise RuntimeError(f"Unsafe archive member: {archive_name}")
    return target


def extract_archive(archive_path, dest_dir, is_zip, materialize_links=False):
    """Unpack a pinned toolchain archive into *dest_dir*, root folder stripped.

    *materialize_links* recreates symlinks and hard links instead of dropping
    them.  llvm-mingw needs it -- every ``<triple>-clang++`` driver is a symlink
    to a shared wrapper, so skipping links would leave the toolchain with no
    usable compiler at all.  Zig's archives contain no links and are extracted
    without it.
    """
    os.makedirs(dest_dir, exist_ok=True)
    if is_zip:
        with zipfile.ZipFile(archive_path, "r") as archive:
            for member in archive.namelist():
                target = safe_extract_target(dest_dir, member)
                if not target:
                    continue
                if member.endswith("/"):
                    os.makedirs(target, exist_ok=True)
                else:
                    os.makedirs(os.path.dirname(target), exist_ok=True)
                    with open(target, "wb") as handle:
                        handle.write(archive.read(member))
        return

    links = []
    with tarfile.open(archive_path, "r:xz") as archive:
        for member in archive.getmembers():
            target = safe_extract_target(dest_dir, member.name)
            if not target:
                continue
            if member.isdir():
                os.makedirs(target, exist_ok=True)
            elif member.isfile():
                os.makedirs(os.path.dirname(target), exist_ok=True)
                source = archive.extractfile(member)
                if source:
                    with source, open(target, "wb") as handle:
                        handle.write(source.read())
                    if member.mode:
                        os.chmod(target, member.mode & 0o777)
            elif member.issym() or member.islnk():
                links.append((member, target))
        if not materialize_links:
            return
        for member, target in links:
            source = (safe_extract_target(dest_dir, member.linkname)
                      if member.islnk() else
                      os.path.abspath(os.path.join(os.path.dirname(target),
                                                   member.linkname)))
            if (not source or os.path.commonpath(
                    [os.path.abspath(dest_dir), source]) != os.path.abspath(dest_dir)):
                raise RuntimeError(
                    f"Unsafe archive link: {member.name} -> {member.linkname}")
            os.makedirs(os.path.dirname(target), exist_ok=True)
            if member.islnk():
                os.link(source, target)
            else:
                os.symlink(member.linkname, target)


def extract_flat_tar(archive_path, dest_dir, members):
    """Extract named top-level files from a tarball that has no root folder.

    The 7-Zip Linux tarball unpacks straight into the current directory, so the
    strip-one-component logic used for Zig and llvm-mingw cannot be applied to
    it.
    """
    os.makedirs(dest_dir, exist_ok=True)
    found = set()
    with tarfile.open(archive_path, "r:xz") as archive:
        for member in archive:
            name = member.name.lstrip("./")
            if name not in members or not member.isfile():
                continue
            target = os.path.join(dest_dir, os.path.basename(name))
            source = archive.extractfile(member)
            if not source:
                continue
            with source, open(target, "wb") as handle:
                handle.write(source.read())
            if member.mode:
                os.chmod(target, member.mode & 0o777)
            found.add(name)
    missing = sorted(set(members) - found)
    if missing:
        print(f"ERROR: {os.path.basename(archive_path)} is missing {missing}")
        sys.exit(1)


# --- 7-Zip -----------------------------------------------------------------
#
# The Windows release archives are written by 7-Zip, which makes it part of the
# toolchain that shapes a published artifact -- not an incidental helper.  It is
# pinned and vendored exactly like the compilers instead of being installed from
# a distribution repository at whatever version happens to be current.

SEVEN_ZIP_VERSION = "26.02"
SEVEN_ZIP_DIR_NAME = "seven-zip"


def seven_zip_dir(script_dir):
    return os.path.join(script_dir, SEVEN_ZIP_DIR_NAME)


def seven_zip_exe(script_dir, platform=None):
    name = "7zr.exe" if host_key(platform) == "windows" else "7zz"
    return os.path.join(seven_zip_dir(script_dir), name)


def find_pinned_seven_zip(script_dir):
    """Path to the vendored 7-Zip, or None when it has not been unpacked."""
    path = seven_zip_exe(script_dir)
    return path if os.path.exists(path) else None


def ensure_seven_zip(script_dir, compilers_dir):
    """Unpack the pinned 7-Zip into <repo>/seven-zip/ if it is not there yet."""
    manifest = load_manifest(compilers_dir, "7zip", SEVEN_ZIP_VERSION)
    host = host_key()
    target_dir = seven_zip_dir(script_dir)
    exe = seven_zip_exe(script_dir)
    if os.path.exists(exe) and verify_tree("7-Zip", target_dir, manifest, host):
        return exe

    entry = archive_record(manifest, host)
    local_dir = component_dir(compilers_dir, "7zip", SEVEN_ZIP_VERSION)
    os.makedirs(target_dir, exist_ok=True)
    staged = os.path.join(target_dir, entry["filename"])
    resolve_archive("7-Zip", entry["filename"], local_dir, entry["url"],
                    entry["sha256"], staged)

    if entry["format"] == "raw":
        os.replace(staged, exe)
    else:
        extract_flat_tar(staged, target_dir, {"7zz", "License.txt"})
        os.remove(staged)
        os.chmod(exe, 0o755)

    if not verify_tree("7-Zip", target_dir, manifest, host):
        print("ERROR: extracted 7-Zip failed pinned digest verification")
        sys.exit(1)
    print(f"7-Zip {SEVEN_ZIP_VERSION} installed at {exe}")
    return exe


# --- Reporting -------------------------------------------------------------


def check_pins(compilers_dir, pins):
    """Assert build.py's inline digests match the manifests.

    *pins* maps a human label to (tool, version, host, field, value), where
    field is 'archive' or an extracted binary path.  Any drift is fatal: the
    two copies exist so the gates can assert on build.py's constants, not so
    they can disagree.
    """
    problems = []
    for label, (tool, version, host, field, value) in sorted(pins.items()):
        manifest = load_manifest(compilers_dir, tool, version)
        if field == "archive":
            recorded = archive_record(manifest, host).get("sha256", "")
        else:
            recorded = ""
            for entry in (manifest.get("extracted_binaries") or {}).get(host, []):
                if entry.get("path") == field:
                    recorded = entry.get("sha256", "")
                    break
        if recorded.lower() != (value or "").lower():
            problems.append(f"  {label}: build.py has {value}, "
                            f"{tool}-{version} manifest has {recorded or '(absent)'}")
    if problems:
        print("Regression source check FAILED: toolchain pins disagree with "
              "compilers/ manifests")
        for problem in problems:
            print(problem)
        sys.exit(1)


_SHA1_REF = re.compile(r"^[0-9a-f]{40}$")


def unpinned_actions(workflow_paths):
    """Report every `uses:` in a workflow that is not pinned to a commit SHA.

    A tag like @v4 is a moving target the action's owner can repoint at any
    time, which would let the workflow that signs our attestations change
    underneath a release without a single commit in this repository.
    """
    problems = []
    for path in workflow_paths:
        if not path or not os.path.exists(path):
            continue
        with open(path, "r", encoding="utf-8", errors="replace") as handle:
            for number, line in enumerate(handle, 1):
                stripped = line.strip()
                if not stripped.startswith(("uses:", "- uses:")):
                    continue
                ref = stripped.split("uses:", 1)[1].split("#", 1)[0].strip()
                if "@" not in ref:
                    problems.append(f"{os.path.basename(path)}:{number}: {ref} has no ref")
                    continue
                if not _SHA1_REF.match(ref.rsplit("@", 1)[1]):
                    problems.append(
                        f"{os.path.basename(path)}:{number}: {ref} is not pinned to a commit SHA")
    return problems


def report(compilers_dir, components, path=None):
    """Print, and optionally write, the toolchain this build actually used.

    The release workflow attests the written file alongside the binaries, so a
    consumer can see exactly which compilers produced them.
    """
    host = host_key()
    doc = {"host_platform": host, "components": []}
    print("=== Pinned toolchain ===")
    print(f"  local-only: {'yes' if local_only() else 'no'}")
    for tool, version, root in components:
        manifest = load_manifest(compilers_dir, tool, version)
        entry = archive_record(manifest, host)
        doc["components"].append({
            "tool": manifest.get("tool", tool),
            "version": version,
            "archive": entry["filename"],
            "archive_sha256": entry["sha256"],
            "mirror_url": entry.get("url"),
            "upstream_url": entry.get("upstream_url"),
            "installed_at": os.path.relpath(root, os.path.dirname(compilers_dir)),
        })
        print(f"  {manifest.get('tool', tool)} {version}")
        print(f"    archive {entry['filename']}")
        print(f"    sha256  {entry['sha256']}")
        if not verify_tree(f"{tool} {version}", root, manifest, host):
            print(f"ERROR: {tool} {version} failed verification")
            sys.exit(1)
    if path:
        with open(path, "w", encoding="utf-8", newline="\n") as handle:
            json.dump(doc, handle, indent=2)
            handle.write("\n")
        print(f"  manifest written to {path}")
    return doc


def run_self_tests():
    """Regression self-tests for verify_tree's symlink handling.

    The CI-cache threat model depends on every restored tool being the exact
    pinned file.  A symlink entry must therefore verify the link text, the link
    type, and the target, and an ordinary file substituted where a symlink was
    pinned must be rejected.
    """
    with tempfile.TemporaryDirectory(prefix="greencurve-toolchain-") as temp:
        root = os.path.join(temp, "root")
        os.makedirs(os.path.join(root, "bin"))
        target = os.path.join(root, "bin", "real.exe")
        with open(target, "wb") as handle:
            handle.write(b"pinned bytes")
        digest = sha256_file(target)
        manifest = {
            "extracted_binaries": {
                "windows": [
                    {"path": "bin/real.exe", "sha256": digest},
                    {"path": "bin/tool.exe", "symlink": "real.exe"},
                ]
            }
        }
        try:
            os.symlink("real.exe", os.path.join(root, "bin", "tool.exe"))
        except OSError:
            # No symlink privilege on this host: still prove the substitution
            # arm, which is the actual cache-tampering case.
            with open(os.path.join(root, "bin", "tool.exe"), "wb") as handle:
                handle.write(b"attacker bytes")
            with redirect_stdout(io.StringIO()):
                accepted = verify_tree(
                    "toolchain-test", root, manifest, host="windows")
            if accepted:
                raise RuntimeError("verify_tree accepted a regular file where a "
                                   "symlink was pinned")
            return
        if not verify_tree("toolchain-test", root, manifest, host="windows"):
            raise RuntimeError("verify_tree rejected a correct pinned symlink")
        os.remove(os.path.join(root, "bin", "tool.exe"))
        with open(os.path.join(root, "bin", "tool.exe"), "wb") as handle:
            handle.write(b"attacker replacement")
        with redirect_stdout(io.StringIO()):
            accepted = verify_tree(
                "toolchain-test", root, manifest, host="windows")
        if accepted:
            raise RuntimeError("verify_tree accepted a tampered symlink path")
