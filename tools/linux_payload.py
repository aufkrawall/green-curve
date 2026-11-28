"""Shared installed-file payload for every Linux distribution package.

The Arch (.pkg.tar.zst), Debian/Ubuntu (.deb), Fedora (.rpm) and openSUSE
(.rpm) packages install exactly the same files with the same modes; only the
container and the package-manager metadata differ.  Keeping the payload here
means a new unit file, icon size or doc lands in all four packages at once
instead of drifting between them.

Every distro package file name carries the distribution it is built for
(``greencurve-<version>-<rel>-<distro>-<cpu>.<ext>``), so a user picking a
download from a release page never has to guess which file fits their system.
"""
import hashlib
import os
import sys

try:
    from icon_render import render_icon, rgba_to_png
except ImportError:
    sys.path.insert(0, os.path.dirname(__file__))
    from icon_render import render_icon, rgba_to_png

PACKAGE_NAME = "greencurve"
PACKAGE_URL = "https://github.com/aufkrawall/green-curve"
PACKAGE_LICENSE = "MIT"
PACKAGE_SUMMARY = "NVIDIA VF curve, overclock, undervolt and fan control daemon & terminal UI"
PACKAGE_DESCRIPTION = (
    "Green Curve controls NVIDIA GPU voltage/frequency curves, overclocks, "
    "undervolts, power limits and fan curves. It ships a hardened systemd "
    "daemon, a standby-resume restore unit and a terminal UI."
)
PACKAGER = "Green Curve Build System"
# Oldest glibc symbol version the Linux binary links against (GLIBC_2.27,
# measured from the built ELF).  The deb/rpm dependency floors use it.
GLIBC_FLOOR = "2.27"

ICON_SIZES = (16, 32, 48, 64, 128, 256)

# Directories this package owns outright.  Shared system directories
# (usr/bin, the hicolor tree, ...) are listed separately in payload_dirs():
# a tar-based package manager wants them as members, while rpm must not claim
# them (that is the filesystem package's job).
OWNED_DIRS = (
    "usr/share/licenses/greencurve",
    "usr/share/doc/greencurve",
)

# Same order as the tree is created; parents always precede children.
_SHARED_DIRS = (
    "usr",
    "usr/bin",
    "usr/lib",
    "usr/lib/systemd",
    "usr/lib/systemd/system",
    "usr/lib/sysusers.d",
    "usr/share",
    "usr/share/applications",
    "usr/share/icons",
    "usr/share/icons/hicolor",
) + tuple(
    d for size in ICON_SIZES
    for d in (f"usr/share/icons/hicolor/{size}x{size}",
              f"usr/share/icons/hicolor/{size}x{size}/apps")
) + (
    "usr/share/pixmaps",
    "usr/share/licenses",
    "usr/share/doc",
)

# Installed path -> files the user should see flagged as doc/license by the
# package manager (rpm FILEFLAGS; ignored by the tar-based formats).
DOC_PATHS = frozenset(("usr/share/doc/greencurve/README.md",))
LICENSE_PATHS = frozenset(("usr/share/licenses/greencurve/LICENSE",))


def normalize_text(text_or_bytes):
    """LF-only text: a CRLF unit file or scriptlet breaks on Linux."""
    data = text_or_bytes.encode("utf-8") if isinstance(text_or_bytes, str) else text_or_bytes
    data = data.replace(b"\r\n", b"\n")
    if b"\r" in data:
        raise RuntimeError("text carries lone CR not part of CRLF")
    return data


def sha256_bytes(data):
    return hashlib.sha256(data).hexdigest()


def md5_bytes(data):
    return hashlib.md5(data).hexdigest()


def packaging_dir(script_dir):
    """Distro-neutral unit/desktop/sysusers/scriptlet sources.

    They live under packaging/arch/ because the Arch recipes reference them by
    relative name; the deb and rpm builders read the same files."""
    return os.path.join(script_dir, "packaging", "arch")


def _read_required(path):
    if not os.path.isfile(path):
        raise RuntimeError(f"missing required packaging file: {path}")
    with open(path, "rb") as handle:
        return normalize_text(handle.read())


def read_install_scriptlet(script_dir):
    """The pacman-style lifecycle functions (post_install, post_upgrade,
    pre_remove, post_remove).  The deb and rpm maintainer scripts embed these
    same functions and only add a dispatcher for their own argument scheme."""
    return _read_required(os.path.join(packaging_dir(script_dir), "greencurve.install"))


def payload_files(script_dir, binary_path):
    """Installed path (no leading slash) -> (bytes, mode) for every regular file."""
    if not os.path.isfile(binary_path):
        raise RuntimeError(f"binary not found: {binary_path}")
    with open(binary_path, "rb") as handle:
        binary_data = handle.read()
    src = packaging_dir(script_dir)
    files = {
        "usr/bin/greencurve": (binary_data, 0o755),
        "usr/lib/systemd/system/greencurve.service":
            (_read_required(os.path.join(src, "greencurve.service")), 0o644),
        "usr/lib/systemd/system/greencurve-resume.service":
            (_read_required(os.path.join(src, "greencurve-resume.service")), 0o644),
        "usr/lib/sysusers.d/greencurve.conf":
            (_read_required(os.path.join(src, "greencurve.sysusers")), 0o644),
        "usr/share/applications/greencurve.desktop":
            (_read_required(os.path.join(src, "greencurve.desktop")), 0o644),
        "usr/share/licenses/greencurve/LICENSE":
            (_read_required(os.path.join(script_dir, "LICENSE")), 0o644),
        "usr/share/doc/greencurve/README.md":
            (_read_required(os.path.join(script_dir, "README.md")), 0o644),
    }
    # Deterministic icon rendering: identical bytes from a Windows or Linux host.
    for size in ICON_SIZES:
        files[f"usr/share/icons/hicolor/{size}x{size}/apps/greencurve.png"] = (
            rgba_to_png(render_icon(size, "app"), size), 0o644)
    files["usr/share/pixmaps/greencurve.png"] = (rgba_to_png(render_icon(256, "app"), 256), 0o644)
    return files


def payload_dirs():
    """Every directory of the installed tree, parents first."""
    return list(_SHARED_DIRS) + list(OWNED_DIRS)


def installed_size(files):
    return sum(len(data) for data, _ in files.values())


def build_date():
    """SOURCE_DATE_EPOCH when set (reproducible builds), else now."""
    import time
    return int(os.environ.get("SOURCE_DATE_EPOCH", int(time.time())))


def write_sha256_file(archive_path):
    """Write `<archive>.sha256` in `sha256sum -c` format and return the digest."""
    with open(archive_path, "rb") as handle:
        digest = hashlib.sha256(handle.read()).hexdigest()
    with open(archive_path + ".sha256", "w", encoding="utf-8", newline="\n") as handle:
        handle.write(f"{digest}  {os.path.basename(archive_path)}\n")
    return digest


def remove_stale(paths):
    """Delete earlier outputs (and their checksums) so no stale package ships."""
    removed = []
    for path in paths:
        for candidate in (path, path + ".sha256"):
            if os.path.exists(candidate):
                os.remove(candidate)
                removed.append(os.path.basename(candidate))
    if removed:
        shown = ", ".join(removed[:4]) + (f" (+{len(removed) - 4} more)" if len(removed) > 4 else "")
        print(f"  linux-pkg: removed {len(removed)} stale output(s): {shown}")
    return removed


def check_payload_matches(label, found_files, found_dirs, files, dirs):
    """Shared read-back comparison: every expected file with its exact bytes and
    mode, every expected directory, nothing extra.

    found_files: path -> (bytes, mode & 0o7777); found_dirs: set of paths."""
    missing = sorted(set(files) - set(found_files))
    extra = sorted(set(found_files) - set(files))
    if missing or extra:
        raise RuntimeError(f"{label}: payload mismatch; missing={missing[:5]} extra={extra[:5]}")
    for path, (data, mode) in files.items():
        got_data, got_mode = found_files[path]
        if got_mode != mode:
            raise RuntimeError(f"{label}: mode mismatch for {path}: "
                               f"expected {oct(mode)}, got {oct(got_mode)}")
        if got_data != data:
            raise RuntimeError(f"{label}: content mismatch for {path} "
                               f"({len(got_data)} vs {len(data)} bytes)")
    missing_dirs = sorted(set(dirs) - set(found_dirs))
    if missing_dirs:
        raise RuntimeError(f"{label}: missing directories {missing_dirs[:5]}")
