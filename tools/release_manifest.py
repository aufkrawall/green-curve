"""Release archive manifest: what ships, what never ships, and where it lands.

Split out of build.py so the build script stays under its size ratchet.  Nothing
here imports build.py, so the dependency runs one way only.

The allowlist is deliberately exact in both directions: a missing file and an
unexpected one are equally a packaging bug.  Runtime output is *removed* rather
than ignored, so the allowlist can stay exact while a developer is still free to
run the freshly built binary out of dist/.

Container choice is part of the manifest, not a packaging detail.  A Linux
release has to carry Unix file modes -- the daemon binary and the setup script
are useless without the executable bit, and `greencurve-setup.sh` refuses to run
at all if `[ -x "$BINARY" ]` fails -- and 7-Zip on Windows cannot record them
(it stores Windows attributes only, so every member extracted as 0644).  It also
cannot be told to; there is no such switch.  The Linux archive is therefore a
.tar.xz written by the standard library, which sets the mode explicitly and so
produces byte-identical output from a Windows or a Linux host.  Windows keeps
the .7z its installer payload already shares.
"""
import io
import os
import shutil
import subprocess
import sys
import tarfile


# Files the binary legitimately writes beside itself at run time.  A developer
# who runs the freshly built binary out of dist/ must not have that turn into a
# packaging failure -- and the debug log records GPU identifiers and applied
# settings, so it must never be archived either.
RUNTIME_ARTIFACT_NAMES = (
    "greencurve_debug.txt",
    "greencurve_debug.txt.1",
    "config.ini",
    "greencurve_linux_probe.md",
    "greencurve-live.txt",
    "greencurve-live.json",
    "greencurve_cli_log.txt",
    "greencurve_log.txt",
)


def expected_release_names(os_name):
    if os_name == "windows":
        return {"greencurve.exe", "greencurve-service.exe", "README.md", "LICENSE"}
    if os_name == "linux":
        return {"greencurve", "greencurve-setup.sh", "README.md", "LICENSE"}
    raise ValueError(f"unsupported release OS: {os_name}")


def release_archive_extension(os_name):
    """Container for a release: .7z on Windows, .tar.xz on Linux.

    See the module docstring for why the Linux archive cannot be a .7z produced
    on a Windows host.  tar also happens to be the format a Linux user can
    already unpack; p7zip is a separate install on most distributions."""
    if os_name == "windows":
        return ".7z"
    if os_name == "linux":
        return ".tar.xz"
    raise ValueError(f"unsupported release OS: {os_name}")


def release_archive_paths(script_dir, version, os_name, arch):
    """Every archive (and checksum) name this target could have produced.

    Includes formats this build no longer emits.  A stale, broken
    greencurve-<version>-linux-<arch>.7z left beside the new tarball from an
    earlier build is a distribution hazard, not merely clutter, so packaging
    deletes the whole set before writing the current one."""
    return [os.path.join(script_dir, f"greencurve-{version}-{os_name}-{arch}{ext}{suffix}")
            for ext in (".7z", ".tar.xz") for suffix in ("", ".sha256")]


def release_member_mode(name):
    """Unix mode a release member must carry.

    The daemon binary and the setup wrapper are both invoked directly, and the
    wrapper's own `require_binary` gate tests `[ -x "$BINARY" ]`, so a lost
    executable bit fails the install rather than degrading it."""
    return 0o755 if name == "greencurve" or name.endswith(".sh") else 0o644


def is_release_text_name(name):
    """Members that must be LF-terminated inside a Linux archive."""
    return name.endswith((".sh", ".md")) or name == "LICENSE"


def normalize_release_text(data):
    """CRLF -> LF, and reject anything a plain conversion cannot explain.

    A surviving lone CR is not a line ending this project ever writes; treating
    it as one would silently rewrite content, so it is an error instead."""
    converted = data.replace(b"\r\n", b"\n")
    if b"\r" in converted:
        raise RuntimeError("release text carries a carriage return that is not part of a CRLF pair")
    return converted


def stage_release_file(source, destination, normalize):
    """Copy one file into the staging tree, rewriting line endings if asked.

    Linux text is *rewritten* rather than copied because the working-tree copy
    on a Windows host is CRLF (git's autocrlf smudge), and a CRLF
    greencurve-setup.sh is an unrunnable script: the kernel reads the shebang as
    an interpreter named "bash\\r"."""
    if not normalize or not is_release_text_name(os.path.basename(destination)):
        shutil.copy2(source, destination)
        return
    with open(source, "rb") as handle:
        data = normalize_release_text(handle.read())
    with open(destination, "wb") as handle:
        handle.write(data)


def _tar_member(tar, staging, root, name):
    info = tar.gettarinfo(os.path.join(staging, name), arcname=f"{root}/{name}")
    # Explicit, host-independent identity: the build host's uid/gid and umask
    # must not reach a user's machine, and the mode is the whole point.
    info.mode = release_member_mode(name)
    info.uid = info.gid = 0
    info.uname = info.gname = "root"
    return info


def write_linux_tarball(archive, staging, root, expected_names):
    """Write the Linux release as a mode-correct .tar.xz.

    tarfile records the Unix mode in the header, so the archive is identical
    whether it was built on Linux or on a Windows filesystem that has no
    executable bit to preserve in the first place."""
    with tarfile.open(archive, "w:xz", preset=9, format=tarfile.GNU_FORMAT) as tar:
        top = tarfile.TarInfo(root)
        top.type = tarfile.DIRTYPE
        top.mode = 0o755
        top.uname = top.gname = "root"
        top.mtime = int(os.path.getmtime(staging))
        tar.addfile(top)
        for name in sorted(expected_names):
            info = _tar_member(tar, staging, root, name)
            with open(os.path.join(staging, name), "rb") as handle:
                tar.addfile(info, handle)


def verify_linux_tarball(archive, expected_names, root):
    """Read a finished tarball back and assert everything a user depends on.

    Names, modes, ownership and line endings are all checked from the archive
    itself rather than from the staging tree, so a bug in the writer cannot pass
    by agreeing with the code that fed it."""
    expected = {root: (tarfile.DIRTYPE, 0o755)}
    for name in expected_names:
        expected[f"{root}/{name}"] = (tarfile.REGTYPE, release_member_mode(name))
    actual = {}
    with tarfile.open(archive, "r:xz") as tar:
        for member in tar.getmembers():
            actual[member.name] = (member.type, member.mode)
            if member.uid or member.gid:
                raise RuntimeError(f"archive member {member.name} carries a build-host owner "
                                   f"({member.uid}:{member.gid})")
            if not member.isfile():
                continue
            base = os.path.basename(member.name)
            if not is_release_text_name(base):
                continue
            data = tar.extractfile(member).read()
            if b"\r" in data:
                raise RuntimeError(f"archive member {member.name} carries CRLF line endings"
                                   + ("; Linux cannot run a CRLF shell script"
                                      if base.endswith(".sh") else ""))
            if base.endswith(".sh") and not data.startswith(b"#!"):
                raise RuntimeError(f"archive member {member.name} has no shebang")
    if actual != expected:
        raise RuntimeError(f"archive manifest mismatch: expected={sorted(expected.items())}, "
                           f"actual={sorted(actual.items())}")


def verify_seven_zip_manifest(seven, archive, expected_names, root):
    """Read a finished .7z back against the exact name allowlist.

    Moved out of build.py to sit beside the tarball's equivalent check; 7-Zip
    records no Unix mode, which is precisely why only Windows ships this
    format."""
    result = subprocess.run([seven, "l", "-slt", archive], text=True, capture_output=True)
    if result.returncode != 0:
        raise RuntimeError("7-Zip could not inspect the completed archive")
    in_entries = False
    paths = set()
    for line in result.stdout.splitlines():
        if line.startswith("----------"):
            in_entries = True
            continue
        if in_entries and line.startswith("Path = "):
            paths.add(line[7:].replace("\\", "/"))
    expected_paths = {root, *(f"{root}/{name}" for name in expected_names)}
    if paths != expected_paths:
        raise RuntimeError(f"archive manifest mismatch: expected={sorted(expected_paths)}, actual={sorted(paths)}")


def purge_runtime_artifacts(payload):
    """Delete run-time output the binary may have written into a payload dir."""
    removed = []
    for name in RUNTIME_ARTIFACT_NAMES:
        path = os.path.join(payload, name)
        if os.path.isfile(path):
            os.remove(path)
            removed.append(name)
    if removed:
        print(f"  Removed runtime artifacts from {payload}: {', '.join(removed)}")
    return removed


def validate_payload_file_names(payload, expected_binary_names):
    """Reject stale/linker side products before any archive is created."""
    actual = {name for name in os.listdir(payload)
              if os.path.isfile(os.path.join(payload, name))}
    unexpected = actual - set(expected_binary_names)
    missing = set(expected_binary_names) - actual
    if unexpected or missing:
        raise RuntimeError(f"payload manifest mismatch: missing={sorted(missing)}, unexpected={sorted(unexpected)}")


def find_seven_zip():
    """Locate a 7-Zip executable (PATH or the standard Windows install dirs)."""
    on_path = shutil.which("7z") or shutil.which("7za") or shutil.which("7zr")
    if on_path:
        return on_path
    for cand in (r"C:\Program Files\7-Zip\7z.exe", r"C:\Program Files (x86)\7-Zip\7z.exe"):
        if os.path.exists(cand):
            return cand
    return None


def report_packaging_skipped(skipped):
    """Explain a packaging skip loudly, and say where the binaries actually are.

    Failing the whole run over a missing archiver used to throw away a
    completed, verified build on any machine without 7-Zip, which is the common
    case for a plain source build.

    Only Windows can be skipped: the Linux tarball is written by the standard
    library and needs no external tool, so a Linux entry reaching here means the
    caller routed a target that had no reason to be skipped."""
    for os_name, _, _ in skipped:
        if os_name != "windows":
            raise ValueError(f"only Windows packaging needs an external archiver, got {os_name!r}")
    install_hint = ("winget install 7zip.7zip" if sys.platform == "win32"
                    else "sudo apt-get install p7zip-full")
    print("")
    print("!!! WARNING: 7-Zip not found -- Windows release archives were NOT built !!!")
    print("    The binaries below are complete and passed every build-time check;")
    print("    only the .7z archives, .sha256 files and the Windows setup .exe were")
    print("    skipped.  Any Linux target in this run was packaged normally.")
    print(f"    Install an archiver to get them: {install_hint}")
    print("    Searched: PATH (7z/7za/7zr)"
          + (r", C:\Program Files\7-Zip\7z.exe" if sys.platform == "win32" else ""))
    print("    Built binaries (under dist/, wiped and rebuilt by the next run):")
    for os_name, arch, binaries in skipped:
        for binary in binaries:
            print(f"      [{os_name}-{arch}] {binary}")
    print("")


def check_packaging_skip_warning():
    """Exercise the no-archiver path for real, not just by text match.

    The skip replaces a hard failure, so a broken warning would silently turn a
    packaging skip into a build that looks successful while saying nothing
    useful about where its outputs went."""
    windows = ("windows", "x64", ["<payload-win>/greencurve.exe",
                                  "<payload-win>/greencurve-service.exe"])
    captured = io.StringIO()
    saved_stdout = sys.stdout
    sys.stdout = captured
    try:
        report_packaging_skipped([windows])
    finally:
        sys.stdout = saved_stdout
    text = captured.getvalue()
    required = ["WARNING: 7-Zip not found", "setup .exe", "under dist/",
                "<payload-win>/greencurve.exe",
                "<payload-win>/greencurve-service.exe", "windows-x64"]
    for needle in required:
        if needle not in text:
            print("Regression source check FAILED: packaging-skip warning omits "
                  f"{needle!r}")
            sys.exit(1)
    # A Linux build no longer depends on 7-Zip at all, so it must never be able
    # to reach the skip path.  This is the assertion, not a smoke test.
    try:
        report_packaging_skipped([("linux", "arm64", ["<payload-linux>/greencurve"])])
    except ValueError:
        pass
    else:
        print("Regression source check FAILED: a Linux target was reported as "
              "skipped for want of 7-Zip, which it does not use")
        sys.exit(1)
    if release_archive_extension("linux") != ".tar.xz":
        print("Regression source check FAILED: the Linux release container must "
              "record Unix modes")
        sys.exit(1)


def check_all(ctx, require_text):
    """Guards for how releases are staged, archived, and verified.

    These describe build.py's packaging path but live beside the manifest they
    protect, which also keeps the build script under its size ratchet."""
    build_script = os.path.join(ctx.SCRIPT_DIR, "build.py")
    self_path = os.path.join(ctx.SCRIPT_DIR, "tools", "release_manifest.py")
    require_text(build_script, "def package_release_archive",
                 "release archives are produced by build.py")
    require_text(build_script,
                 'f"greencurve-{APP_VERSION}-{os_name}-{arch}{release_archive_extension(os_name)}"',
                 "archive name carries version + os + arch + the per-OS container")
    require_text(build_script, '"a", "-t7z"', "Windows release files are packaged with 7-Zip")
    require_text(build_script, "def detect_binary_arch",
                 "packaging reads each binary's real PE/ELF machine arch")
    require_text(build_script, "architecture mismatch:",
                 "packaging aborts on a cross-arch bundle mismatch")
    require_text(self_path, "def verify_seven_zip_manifest",
                 "completed Windows archives are read back against an exact manifest")
    require_text(self_path, "def verify_linux_tarball",
                 "completed Linux archives are read back against an exact manifest")
    # F-LNX-EOL / F-LNX-MODE.  A Windows checkout hands the packager a CRLF
    # setup script and a filesystem with no executable bit; both used to travel
    # straight into the Linux archive and make it unusable.
    require_text(build_script, "stage_release_file(extra",
                 "staged release text goes through the normalizing writer, not a verbatim copy")
    require_text(build_script, 'normalize=(os_name == "linux")',
                 "Linux release text is rewritten to LF at packaging time")
    require_text(self_path, 'raise RuntimeError("release text carries a carriage return',
                 "an unexplained CR is a packaging failure, not something to rewrite")
    require_text(self_path, "Linux cannot run a CRLF shell script",
                 "the finished tarball is rejected if any shipped text carries CRLF")
    require_text(self_path, "def release_member_mode",
                 "every Linux release member has a declared Unix mode")
    require_text(build_script, "release_archive_paths(SCRIPT_DIR, APP_VERSION, os_name, arch)",
                 "a superseded archive from an earlier container format is deleted, not left to ship")
    gitattributes = os.path.join(ctx.SCRIPT_DIR, ".gitattributes")
    require_text(gitattributes, "*.sh text eol=lf",
                 "shell scripts are pinned to LF regardless of host autocrlf")
    # This used to name build.py, where the only match was the guard's own
    # source line -- a gate that could never fail.  The real fixture (a stray
    # main.lib staged into a payload) lives in the build-script regression test.
    require_text(os.path.join(ctx.SCRIPT_DIR, "tools", "security_gates.py"),
                 'open(os.path.join(payload, "main.lib"), "wb")',
                 "packaging regression stages a Zig main.lib side product")
    require_text(os.path.join(ctx.SCRIPT_DIR, "tools", "security_gates.py"),
                 "unexpected package file accepted",
                 "packaging regression rejects Zig main.lib")
    # A missing archiver must not discard an otherwise complete, verified build.
    require_text(self_path, "def report_packaging_skipped",
                 "a missing 7-Zip is reported as a skip, not a build failure")
    require_text(build_script, "                report_packaging_skipped(skipped)",
                 "packaging degrades gracefully when find_seven_zip() returns None")
    require_text(build_script, "            seven = find_seven_zip()",
                 "main() resolves the archiver before staging anything")
    require_text(build_script, 'if seven or entry[0] != "windows"',
                 "a missing 7-Zip withholds only the Windows archives, never the Linux tarball")
    require_text(build_script, "package_release_archive(os_name, arch, binaries, seven=seven)",
                 "the archiver is resolved once up front and passed in, not per-archive")


def release_archive_root(os_name):
    """Top-level folder inside a release archive.

    Windows uses the product name so extracting the archive already produces the
    same "Green Curve" folder the installer creates, and the two distribution
    forms end up indistinguishable on disk.  Linux keeps the lowercase,
    space-free name its tarball conventions, service files, and shell paths
    expect."""
    return "Green Curve" if os_name == "windows" else "greencurve"
