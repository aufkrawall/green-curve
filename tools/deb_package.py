"""Debian/Ubuntu package builder for Green Curve.

Writes a dpkg-installable ``.deb`` from the shared Linux payload using only the
Python standard library, so a Windows host produces the same bytes as a Linux
one.  A .deb is an ``ar`` archive with exactly three members, in this order:
``debian-binary`` ("2.0\\n"), ``control.tar.xz`` and ``data.tar.xz``.  xz is
used for both tarballs because every supported dpkg reads it (zstd members
are Ubuntu-only and Debian's dpkg gained them late).

Maintainer scripts embed the same lifecycle functions as the Arch ``.INSTALL``
scriptlet (packaging/arch/greencurve.install) and only add a dispatcher for
dpkg's argument scheme, so all distributions share one enrollment/upgrade/
removal behaviour.
"""
import io
import lzma
import os
import re
import sys
import tarfile

try:
    import linux_payload
except ImportError:
    sys.path.insert(0, os.path.dirname(__file__))
    import linux_payload

AR_MAGIC = b"!<arch>\n"
DEB_MEMBERS = ("debian-binary", "control.tar.xz", "data.tar.xz")
MAINTAINER_SCRIPTS = ("postinst", "prerm", "postrm")

# dpkg calls: postinst configure <old-version-or-empty>; prerm remove|upgrade|
# deconfigure|failed-upgrade; postrm remove|purge|upgrade|disappear|...
_POSTINST_DISPATCH = """
case "$1" in
    configure)
        systemctl daemon-reload 2>/dev/null || true
        if [ -z "${2:-}" ]; then
            post_install
        else
            post_upgrade
        fi
        ;;
esac
exit 0
"""
_PRERM_DISPATCH = """
case "$1" in
    remove|deconfigure)
        pre_remove
        ;;
esac
exit 0
"""
_POSTRM_DISPATCH = """
case "$1" in
    remove|purge)
        post_remove
        ;;
esac
exit 0
"""


def deb_architecture(arch):
    """Internal arch name -> Debian architecture name."""
    mapping = {"x64": "amd64", "arm64": "arm64"}
    if arch not in mapping:
        raise RuntimeError(f"unsupported Debian architecture: {arch}")
    return mapping[arch]


def deb_package_filename(version, arch, debrev=1):
    """`debian` in the name marks the Debian/Ubuntu package beside the Arch and
    rpm ones; dpkg reads identity from the control file, never the name."""
    return f"greencurve-{version}-{debrev}-debian-{deb_architecture(arch)}.deb"


def deb_maintainer_script(functions, dispatch):
    """A POSIX-sh maintainer script: shebang, shared functions, dispatcher.

    Deliberately not `set -e`: like the pacman scriptlet every step is
    best-effort (a host without systemd still installs), and the functions
    already guard each command."""
    text = functions.decode("utf-8")
    return linux_payload.normalize_text("#!/bin/sh\n" + text.rstrip("\n") + "\n" + dispatch)


def deb_control_text(version, arch, debrev, installed_bytes):
    """dpkg control file.  Maintainer is required by dpkg; it names the project
    page rather than a mailbox."""
    lines = [
        f"Package: {linux_payload.PACKAGE_NAME}",
        f"Version: {version}-{debrev}",
        f"Architecture: {deb_architecture(arch)}",
        f"Maintainer: {linux_payload.PACKAGER} <{linux_payload.PACKAGE_URL}>",
        f"Installed-Size: {(installed_bytes + 1023) // 1024}",
        f"Depends: libc6 (>= {linux_payload.GLIBC_FLOOR})",
        "Section: utils",
        "Priority: optional",
        f"Homepage: {linux_payload.PACKAGE_URL}",
        f"Description: {linux_payload.PACKAGE_SUMMARY}",
        f" {linux_payload.PACKAGE_DESCRIPTION}",
        "",
    ]
    return "\n".join(lines)


def parse_deb_control(text):
    """Parse a single-paragraph control file into {field: value}; continuation
    lines are appended.  Rejects duplicate fields and malformed lines."""
    fields = {}
    last = None
    for lineno, line in enumerate(text.split("\n"), 1):
        if not line:
            continue
        if line[0] in " \t":
            if last is None:
                raise RuntimeError(f"control line {lineno}: continuation before any field")
            fields[last] += "\n" + line[1:]
            continue
        if ":" not in line:
            raise RuntimeError(f"control line {lineno}: not a 'Field: value' line: {line!r}")
        key, value = line.split(":", 1)
        if key in fields:
            raise RuntimeError(f"control line {lineno}: duplicate field {key}")
        fields[key] = value.strip()
        last = key
    return fields


def _ar_member(name, data, mtime):
    # dpkg-deb's own layout: space-padded name, no GNU '/' terminator.
    if len(name) > 16:
        raise RuntimeError(f"ar member name too long: {name}")
    header = (f"{name:<16}{mtime:<12}{0:<6}{0:<6}{'100644':<8}{len(data):<10}`\n").encode("ascii")
    if len(header) != 60:
        raise RuntimeError("internal: ar header is not 60 bytes")
    return header + data + (b"\n" if len(data) % 2 else b"")


def parse_ar(data):
    """Return [(name, bytes)] for a common/GNU ar archive."""
    if not data.startswith(AR_MAGIC):
        raise RuntimeError("not an ar archive")
    members = []
    offset = len(AR_MAGIC)
    while offset < len(data):
        header = data[offset:offset + 60]
        if len(header) != 60 or header[58:60] != b"`\n":
            raise RuntimeError(f"bad ar member header at offset {offset}")
        name = header[0:16].decode("ascii").rstrip(" ").rstrip("/")
        size = int(header[48:58].decode("ascii").strip())
        body = data[offset + 60:offset + 60 + size]
        if len(body) != size:
            raise RuntimeError(f"truncated ar member {name}")
        members.append((name, body))
        offset += 60 + size + (size % 2)
    return members


def _tar_xz(entries, mtime):
    """entries: [(path, bytes-or-None-for-dir, mode)] -> xz-compressed ustar."""
    buf = io.BytesIO()
    with tarfile.open(fileobj=buf, mode="w", format=tarfile.GNU_FORMAT) as tar:
        for path, data, mode in entries:
            info = tarfile.TarInfo(path)
            info.mode = mode
            info.mtime = mtime
            info.uid = info.gid = 0
            info.uname = info.gname = "root"
            if data is None:
                info.type = tarfile.DIRTYPE
                tar.addfile(info)
            else:
                info.size = len(data)
                tar.addfile(info, io.BytesIO(data))
    return lzma.compress(buf.getvalue(), preset=9)


def build_deb_package(script_dir, app_version, arch, binary_path, debrev=1, output_dir=None):
    """Assemble `greencurve-<v>-<rev>-debian-<arch>.deb` (+ .sha256), verify it
    by reading it back, and return its path."""
    files = linux_payload.payload_files(script_dir, binary_path)
    dirs = linux_payload.payload_dirs()
    functions = linux_payload.read_install_scriptlet(script_dir)
    mtime = linux_payload.build_date()

    control = linux_payload.normalize_text(
        deb_control_text(app_version, arch, debrev, linux_payload.installed_size(files)))
    md5sums = "".join(f"{linux_payload.md5_bytes(data)}  {path}\n"
                      for path, (data, _) in sorted(files.items())).encode("utf-8")
    scripts = {
        "postinst": deb_maintainer_script(functions, _POSTINST_DISPATCH),
        "prerm": deb_maintainer_script(functions, _PRERM_DISPATCH),
        "postrm": deb_maintainer_script(functions, _POSTRM_DISPATCH),
    }
    control_entries = [("./", None, 0o755), ("./control", control, 0o644),
                       ("./md5sums", md5sums, 0o644)]
    control_entries += [(f"./{name}", data, 0o755) for name, data in scripts.items()]
    data_entries = [("./", None, 0o755)]
    data_entries += [(f"./{d}/", None, 0o755) for d in dirs]
    data_entries += [(f"./{path}", data, mode) for path, (data, mode) in files.items()]

    deb = AR_MAGIC
    deb += _ar_member("debian-binary", b"2.0\n", mtime)
    deb += _ar_member("control.tar.xz", _tar_xz(control_entries, mtime), mtime)
    deb += _ar_member("data.tar.xz", _tar_xz(data_entries, mtime), mtime)

    target_dir = output_dir or script_dir
    os.makedirs(target_dir, exist_ok=True)
    name = deb_package_filename(app_version, arch, debrev)
    path = os.path.join(target_dir, name)
    linux_payload.remove_stale([path])
    with open(path, "wb") as handle:
        handle.write(deb)
    verify_deb_package(path, files, dirs, app_version, arch, debrev)
    linux_payload.write_sha256_file(path)
    print(f"Packaged Debian/Ubuntu package {name} ({len(deb):,} bytes / {len(deb) / 1024:.1f} KB)")
    return path


def _read_tar_xz(blob, label):
    files, dirs = {}, set()
    with tarfile.open(fileobj=io.BytesIO(lzma.decompress(blob)), mode="r") as tar:
        for member in tar.getmembers():
            if member.uid != 0 or member.gid != 0:
                raise RuntimeError(f"{label}: {member.name} has non-root owner")
            name = member.name[2:] if member.name.startswith("./") else member.name
            name = name.rstrip("/")
            if member.isdir():
                if name:
                    dirs.add(name)
            elif member.isfile():
                files[name] = (tar.extractfile(member).read(), member.mode & 0o7777)
            else:
                raise RuntimeError(f"{label}: unexpected member type for {member.name}")
    return files, dirs


def verify_deb_package(path, files, dirs, version, arch, debrev):
    """Read the finished .deb back: member order, control metadata, scripts
    (LF, executable, carrying every lifecycle hook), md5sums and the payload."""
    label = os.path.basename(path)
    with open(path, "rb") as handle:
        members = parse_ar(handle.read())
    names = tuple(name for name, _ in members)
    if names != DEB_MEMBERS:
        raise RuntimeError(f"{label}: ar members {names} != {DEB_MEMBERS}")
    blobs = dict(members)
    if blobs["debian-binary"] != b"2.0\n":
        raise RuntimeError(f"{label}: debian-binary is not 2.0")

    ctrl_files, _ = _read_tar_xz(blobs["control.tar.xz"], label + " control")
    expected_ctrl = {"control", "md5sums", *MAINTAINER_SCRIPTS}
    if set(ctrl_files) != expected_ctrl:
        raise RuntimeError(f"{label}: control members {sorted(ctrl_files)}")
    for name, (data, mode) in ctrl_files.items():
        if b"\r" in data:
            raise RuntimeError(f"{label}: control member {name} has CR line endings")
        want_mode = 0o755 if name in MAINTAINER_SCRIPTS else 0o644
        if mode != want_mode:
            raise RuntimeError(f"{label}: control member {name} mode {oct(mode)}")
    fields = parse_deb_control(ctrl_files["control"][0].decode("utf-8"))
    expected = {"Package": "greencurve", "Version": f"{version}-{debrev}",
                "Architecture": deb_architecture(arch)}
    for key, value in expected.items():
        if fields.get(key) != value:
            raise RuntimeError(f"{label}: control {key}={fields.get(key)!r}, expected {value!r}")
    for key in ("Maintainer", "Description", "Installed-Size", "Depends"):
        if not fields.get(key):
            raise RuntimeError(f"{label}: control lacks {key}")
    for name, hook in (("postinst", "post_install"), ("postinst", "post_upgrade"),
                       ("prerm", "pre_remove"), ("postrm", "post_remove")):
        text = ctrl_files[name][0].decode("utf-8")
        # A bare call line; the definition reads `<hook>() {`.
        if not text.startswith("#!/bin/sh\n") or not re.search(rf"^\s+{hook}$", text, re.M):
            raise RuntimeError(f"{label}: {name} does not dispatch {hook}")

    data_files, data_dirs = _read_tar_xz(blobs["data.tar.xz"], label + " data")
    linux_payload.check_payload_matches(label, data_files, data_dirs, files, dirs)
    md5_lines = ctrl_files["md5sums"][0].decode("utf-8").splitlines()
    md5_map = {line.split("  ", 1)[1]: line.split("  ", 1)[0] for line in md5_lines}
    for rel, (data, _) in files.items():
        if md5_map.get(rel) != linux_payload.md5_bytes(data):
            raise RuntimeError(f"{label}: md5sums entry wrong or missing for {rel}")
