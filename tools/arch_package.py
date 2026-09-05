"""Arch Linux package builder for Green Curve.

Builds pacman-installable Arch Linux packages (.pkg.tar.zst or .pkg.tar.xz) from
compiled Linux binaries and packaging templates.  Supports building on both
Linux and Windows hosts using Python standard library plus optional zstandard.
"""
import gzip
import hashlib
import io
import os
import shutil
import subprocess
import sys
import tarfile
import time


def arch_target_carch(arch):
    """Map internal architecture name to Arch Linux architecture name."""
    if arch == "x64":
        return "x86_64"
    if arch == "arm64":
        return "aarch64"
    return arch


def arch_package_filename(version, arch, pkgrel=1, ext=".pkg.tar.zst"):
    """Standard Arch Linux package filename."""
    carch = arch_target_carch(arch)
    return f"greencurve-{version}-{pkgrel}-{carch}{ext}"


def arch_package_stale_paths(script_dir, version, arch, pkgrel=1):
    """Paths of any previous Arch package outputs and checksums for cleanup."""
    carch = arch_target_carch(arch)
    prefix = os.path.join(script_dir, f"greencurve-{version}-{pkgrel}-{carch}.pkg")
    return [
        f"{prefix}{ext}{suffix}"
        for ext in (".tar.zst", ".tar.xz", ".tar.gz")
        for suffix in ("", ".sha256")
    ]


def _normalize_text(text_or_bytes):
    """Ensure text is normalized to LF with no CRLF or stray CR."""
    if isinstance(text_or_bytes, str):
        data = text_or_bytes.encode("utf-8")
    else:
        data = text_or_bytes
    data = data.replace(b"\r\n", b"\n")
    if b"\r" in data:
        raise RuntimeError("text carries lone CR not part of CRLF")
    return data


def _sha256_bytes(data):
    return hashlib.sha256(data).hexdigest()


def _md5_bytes(data):
    return hashlib.md5(data).hexdigest()


def _build_mtree(entries, builddate):
    """Construct a BSD mtree representation of the package contents."""
    lines = ["#mtree", "/set type=file uid=0 gid=0 mode=644"]
    for path, is_dir, size, md5_val, sha256_val, mode in sorted(entries):
        if path == ".MTREE":
            continue
        rel_path = f"./{path}"
        if is_dir:
            lines.append(f"{rel_path} time={builddate}.0 type=dir mode={oct(mode)[2:]}")
        else:
            mode_str = f" mode={oct(mode)[2:]}" if mode != 0o644 else ""
            lines.append(
                f"{rel_path} time={builddate}.0{mode_str} size={size} "
                f"md5digest={md5_val} sha256digest={sha256_val}"
            )
    mtree_content = "\n".join(lines) + "\n"
    return gzip.compress(mtree_content.encode("utf-8"), mtime=builddate)


def build_arch_package(script_dir, app_version, arch, binary_path, pkgrel=1, output_dir=None):
    """Assemble and compress an Arch Linux package installable via pacman -U.

    Creates greencurve-<version>-<pkgrel>-<arch>.pkg.tar.zst (or .pkg.tar.xz if
    zstandard is unavailable) in output_dir (defaults to script_dir), along with
    its .sha256 checksum file.
    """
    carch = arch_target_carch(arch)
    if not os.path.isfile(binary_path):
        raise RuntimeError(f"binary not found: {binary_path}")

    with open(binary_path, "rb") as f:
        binary_data = f.read()

    arch_dir = os.path.join(script_dir, "packaging", "arch")
    service_path = os.path.join(arch_dir, "greencurve.service")
    resume_path = os.path.join(arch_dir, "greencurve-resume.service")
    sysusers_path = os.path.join(arch_dir, "greencurve.sysusers")
    desktop_path = os.path.join(arch_dir, "greencurve.desktop")
    install_path = os.path.join(arch_dir, "greencurve.install")
    license_path = os.path.join(script_dir, "LICENSE")
    readme_path = os.path.join(script_dir, "README.md")

    for req in (service_path, resume_path, sysusers_path, desktop_path,
                install_path, license_path, readme_path):
        if not os.path.isfile(req):
            raise RuntimeError(f"missing required packaging file: {req}")

    with open(service_path, "rb") as f:
        service_data = _normalize_text(f.read())
    with open(resume_path, "rb") as f:
        resume_data = _normalize_text(f.read())
    with open(sysusers_path, "rb") as f:
        sysusers_data = _normalize_text(f.read())
    with open(desktop_path, "rb") as f:
        desktop_data = _normalize_text(f.read())
    with open(install_path, "rb") as f:
        install_data = _normalize_text(f.read())
    with open(license_path, "rb") as f:
        license_data = _normalize_text(f.read())
    with open(readme_path, "rb") as f:
        readme_data = _normalize_text(f.read())

    # Map of all installed filesystem files (path -> (bytes, mode))
    fs_files = {
        "usr/bin/greencurve": (binary_data, 0o755),
        "usr/lib/systemd/system/greencurve.service": (service_data, 0o644),
        "usr/lib/systemd/system/greencurve-resume.service": (resume_data, 0o644),
        "usr/lib/sysusers.d/greencurve.conf": (sysusers_data, 0o644),
        "usr/share/applications/greencurve.desktop": (desktop_data, 0o644),
        "usr/share/licenses/greencurve/LICENSE": (license_data, 0o644),
        "usr/share/doc/greencurve/README.md": (readme_data, 0o644),
    }

    # Explicit directory paths in installation order
    dirs = [
        "usr",
        "usr/bin",
        "usr/lib",
        "usr/lib/systemd",
        "usr/lib/systemd/system",
        "usr/lib/sysusers.d",
        "usr/share",
        "usr/share/applications",
        "usr/share/licenses",
        "usr/share/licenses/greencurve",
        "usr/share/doc",
        "usr/share/doc/greencurve",
    ]

    installed_size = sum(len(data) for data, _ in fs_files.values())
    builddate = int(os.environ.get("SOURCE_DATE_EPOCH", int(time.time())))

    pkginfo_lines = [
        "# Generated by Green Curve build.py",
        "pkgname = greencurve",
        "pkgbase = greencurve",
        f"pkgver = {app_version}-{pkgrel}",
        "pkgdesc = NVIDIA VF curve, overclock, undervolt and fan control daemon & terminal UI",
        "url = https://github.com/aufkrawall/green-curve",
        f"builddate = {builddate}",
        "packager = Green Curve Build System",
        f"size = {installed_size}",
        f"arch = {carch}",
        "license = MIT",
        "depend = glibc",
        "optdepend = nvidia-utils: proprietary NVIDIA driver libraries (libnvidia-api.so.1, libnvidia-ml.so.1)",
        "makepkgopt = strip",
        "makepkgopt = docs",
        "makepkgopt = purge",
        "install = .INSTALL",
        "",
    ]
    pkginfo_data = _normalize_text("\n".join(pkginfo_lines))

    buildinfo_lines = [
        "format = 2",
        "pkgname = greencurve",
        "pkgbase = greencurve",
        f"pkgver = {app_version}-{pkgrel}",
        f"pkgarch = {carch}",
        f"pkgbuild_sha256sum = {_sha256_bytes(pkginfo_data)}",
        "packager = Green Curve Build System",
        f"builddate = {builddate}",
        "buildenv = check",
        f"installed = glibc-2.39-1-{carch}",
        "",
    ]
    buildinfo_data = _normalize_text("\n".join(buildinfo_lines))

    # Collect entries for .MTREE
    mtree_entries = []
    for d in dirs:
        mtree_entries.append((d, True, 0, "", "", 0o755))
    for p, (data, mode) in fs_files.items():
        mtree_entries.append((p, False, len(data), _md5_bytes(data), _sha256_bytes(data), mode))
    for p, data in [
        (".PKGINFO", pkginfo_data),
        (".BUILDINFO", buildinfo_data),
        (".INSTALL", install_data),
    ]:
        mtree_entries.append((p, False, len(data), _md5_bytes(data), _sha256_bytes(data), 0o644))

    mtree_data = _build_mtree(mtree_entries, builddate)

    # Determine compressor: prefer zstandard (module or CLI), fallback to xz
    has_zstd_mod = False
    try:
        import zstandard as zstd
        has_zstd_mod = True
    except ImportError:
        pass

    has_zstd_cli = bool(shutil.which("zstd"))

    if has_zstd_mod or has_zstd_cli:
        pkg_ext = ".pkg.tar.zst"
    else:
        pkg_ext = ".pkg.tar.xz"
        print("  Notice: zstandard not found; packaging Arch package as .pkg.tar.xz")

    target_dir = output_dir if output_dir else script_dir
    pkg_name = arch_package_filename(app_version, arch, pkgrel=pkgrel, ext=pkg_ext)
    archive_path = os.path.join(target_dir, pkg_name)

    # Clean stale previous archives
    for stale in arch_package_stale_paths(target_dir, app_version, arch, pkgrel=pkgrel):
        if os.path.exists(stale):
            os.remove(stale)

    # Assemble tar archive into buffer or file
    tar_buf = io.BytesIO()
    with tarfile.open(fileobj=tar_buf, mode="w", format=tarfile.GNU_FORMAT) as tar:
        def add_file_entry(name, data, mode):
            ti = tarfile.TarInfo(name)
            ti.size = len(data)
            ti.mode = mode
            ti.mtime = builddate
            ti.uname = "root"
            ti.gname = "root"
            ti.uid = 0
            ti.gid = 0
            tar.addfile(ti, io.BytesIO(data))

        # Root metadata files
        add_file_entry(".PKGINFO", pkginfo_data, 0o644)
        add_file_entry(".BUILDINFO", buildinfo_data, 0o644)
        add_file_entry(".INSTALL", install_data, 0o644)
        add_file_entry(".MTREE", mtree_data, 0o644)

        # Directory entries
        for d in dirs:
            ti = tarfile.TarInfo(d)
            ti.type = tarfile.DIRTYPE
            ti.mode = 0o755
            ti.mtime = builddate
            ti.uname = "root"
            ti.gname = "root"
            ti.uid = 0
            ti.gid = 0
            tar.addfile(ti)

        # Regular filesystem files
        for name, (data, mode) in fs_files.items():
            add_file_entry(name, data, mode)

    raw_tar = tar_buf.getvalue()

    # Compress
    if pkg_ext == ".pkg.tar.zst":
        if has_zstd_mod:
            cctx = zstd.ZstdCompressor(level=19)
            compressed = cctx.compress(raw_tar)
            with open(archive_path, "wb") as f:
                f.write(compressed)
        else:
            proc = subprocess.run(
                ["zstd", "-19", "-T0", "-o", archive_path],
                input=raw_tar,
                check=True,
                capture_output=True,
            )
    else:
        import lzma
        compressed = lzma.compress(raw_tar, preset=9)
        with open(archive_path, "wb") as f:
            f.write(compressed)

    # Verify finished package
    verify_arch_package(archive_path, fs_files, dirs)

    # Write SHA256 checksum file
    pkg_size = os.path.getsize(archive_path)
    with open(archive_path, "rb") as f:
        pkg_sha256 = hashlib.sha256(f.read()).hexdigest()
    hash_path = archive_path + ".sha256"
    with open(hash_path, "w") as f:
        f.write(f"{pkg_sha256}  {pkg_name}\n")

    print(f"Packaged Arch Linux package {pkg_name} ({pkg_size:,} bytes / {pkg_size / 1024:.1f} KB)")
    return archive_path


def verify_arch_package(archive_path, fs_files, dirs):
    """Verify all files, metadata, modes, and line endings in the completed package."""
    if not os.path.isfile(archive_path):
        raise RuntimeError(f"package file missing: {archive_path}")

    # Decompress tar stream
    if archive_path.endswith(".zst"):
        try:
            import zstandard as zstd
            with open(archive_path, "rb") as f:
                dctx = zstd.ZstdDecompressor()
                raw_tar = dctx.decompress(f.read())
        except ImportError:
            proc = subprocess.run(
                ["zstd", "-d", "-c", archive_path],
                capture_output=True,
                check=True,
            )
            raw_tar = proc.stdout
    elif archive_path.endswith(".xz"):
        import lzma
        with open(archive_path, "rb") as f:
            raw_tar = lzma.decompress(f.read())
    else:
        raise RuntimeError(f"unrecognized package extension: {archive_path}")

    expected_files = {
        ".PKGINFO": 0o644,
        ".BUILDINFO": 0o644,
        ".INSTALL": 0o644,
        ".MTREE": 0o644,
    }
    for p, (_, mode) in fs_files.items():
        expected_files[p] = mode

    found_files = {}
    found_dirs = set()

    with tarfile.open(fileobj=io.BytesIO(raw_tar), mode="r") as tar:
        for member in tar.getmembers():
            if member.uid != 0 or member.gid != 0:
                raise RuntimeError(f"member {member.name} has non-root owner {member.uid}:{member.gid}")
            if member.isdir():
                found_dirs.add(member.name)
                if member.mode != 0o755:
                    raise RuntimeError(f"directory {member.name} has unexpected mode {oct(member.mode)}")
            elif member.isfile():
                found_files[member.name] = member.mode
                data = tar.extractfile(member).read()
                if member.name in (".PKGINFO", ".BUILDINFO", ".INSTALL") or member.name.endswith((".service", ".conf", ".desktop", ".md", "LICENSE")):
                    if b"\r" in data:
                        raise RuntimeError(f"member {member.name} has CRLF line endings")
                if member.name == ".PKGINFO":
                    text = data.decode("utf-8")
                    if "pkgname = greencurve" not in text:
                        raise RuntimeError(".PKGINFO missing pkgname = greencurve")
                    if "install = .INSTALL" not in text:
                        raise RuntimeError(".PKGINFO missing install = .INSTALL")
                elif member.name == ".INSTALL":
                    text = data.decode("utf-8")
                    if "post_install()" not in text:
                        raise RuntimeError(".INSTALL missing post_install()")
                elif member.name == ".MTREE":
                    decomp = gzip.decompress(data).decode("utf-8")
                    if "#mtree" not in decomp:
                        raise RuntimeError(".MTREE invalid header")

    missing_files = set(expected_files) - set(found_files)
    if missing_files:
        raise RuntimeError(f"missing files in Arch package: {sorted(missing_files)}")

    for name, expected_mode in expected_files.items():
        actual_mode = found_files.get(name)
        if actual_mode != expected_mode:
            raise RuntimeError(f"mode mismatch for {name}: expected {oct(expected_mode)}, got {oct(actual_mode)}")

    missing_dirs = set(dirs) - found_dirs
    if missing_dirs:
        raise RuntimeError(f"missing directories in Arch package: {sorted(missing_dirs)}")
