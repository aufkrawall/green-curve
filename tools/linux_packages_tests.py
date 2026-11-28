# SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
# SPDX-License-Identifier: MIT
"""Regression fixtures for the Linux distribution packages.

Covers the release file-name contract (every package names its distribution),
legacy-name cleanup, and the deb/rpm container formats: each builder must
produce a package its own read-back accepts, and the read-back must reject the
corruptions dpkg/rpm would reject.
"""
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import arch_package  # noqa: E402
import deb_package  # noqa: E402
import linux_payload  # noqa: E402
import release_manifest  # noqa: E402
import rpm_header  # noqa: E402
import rpm_package  # noqa: E402

SCRIPT_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
VERSION = "9.8.7"


class FileNameTests(unittest.TestCase):
    def test_every_package_names_its_distribution(self):
        for arch, cpu, debcpu in (("x64", "x86_64", "amd64"), ("arm64", "aarch64", "arm64")):
            self.assertEqual(arch_package.arch_package_filename(VERSION, arch),
                             f"greencurve-{VERSION}-1-arch-{cpu}.pkg.tar.zst")
            self.assertEqual(deb_package.deb_package_filename(VERSION, arch),
                             f"greencurve-{VERSION}-1-debian-{debcpu}.deb")
            self.assertEqual(rpm_package.rpm_package_filename(VERSION, arch, "fedora"),
                             f"greencurve-{VERSION}-1-fedora-{cpu}.rpm")
            self.assertEqual(rpm_package.rpm_package_filename(VERSION, arch, "suse"),
                             f"greencurve-{VERSION}-1-suse-{cpu}.rpm")

    def test_rpm_distro_profiles_are_fedora_and_suse(self):
        self.assertEqual(sorted(rpm_package.RPM_DISTROS), ["fedora", "suse"])
        with self.assertRaises(RuntimeError):
            rpm_package.rpm_package_filename(VERSION, "x64", "mandriva")

    def test_unknown_cpu_is_refused(self):
        with self.assertRaises(RuntimeError):
            deb_package.deb_package_filename(VERSION, "riscv64")
        with self.assertRaises(RuntimeError):
            rpm_package.rpm_package_filename(VERSION, "riscv64", "fedora")

    def test_arch_stale_paths_cover_legacy_untagged_names(self):
        names = {os.path.basename(p) for p in arch_package.arch_package_stale_paths("d", VERSION, "x64")}
        self.assertIn(f"greencurve-{VERSION}-1-x86_64.pkg.tar.zst", names)
        self.assertIn(f"greencurve-{VERSION}-1-x86_64.pkg.tar.xz", names)
        self.assertIn(f"greencurve-{VERSION}-1-arch-x86_64.pkg.tar.zst", names)


class PackageBuildTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.mkdtemp(prefix="gc-linuxpkg-")
        cls.binary = os.path.join(cls.temp, "greencurve")
        with open(cls.binary, "wb") as handle:
            handle.write(b"\x7fELF\x02\x01\x01 fixture binary \n" * 64)
        cls.out = os.path.join(cls.temp, "out")
        os.makedirs(cls.out)
        # A legacy untagged Arch package and checksum must not survive a rebuild.
        cls.legacy = os.path.join(cls.out, f"greencurve-{VERSION}-1-x86_64.pkg.tar.xz")
        for path in (cls.legacy, cls.legacy + ".sha256"):
            with open(path, "w") as handle:
                handle.write("stale")
        cls.paths = release_manifest.build_linux_distro_packages(
            SCRIPT_DIR, VERSION, "x64", cls.binary, cls.out)
        cls.files = linux_payload.payload_files(SCRIPT_DIR, cls.binary)

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.temp, ignore_errors=True)

    def _path(self, suffix):
        return next(p for p in self.paths if p.endswith(suffix))

    def test_four_packages_with_checksums(self):
        names = sorted(os.path.basename(p) for p in self.paths)
        self.assertEqual(len(names), 4)
        self.assertTrue(any("-arch-x86_64.pkg.tar." in n for n in names))
        self.assertIn(f"greencurve-{VERSION}-1-debian-amd64.deb", names)
        self.assertIn(f"greencurve-{VERSION}-1-fedora-x86_64.rpm", names)
        self.assertIn(f"greencurve-{VERSION}-1-suse-x86_64.rpm", names)
        for path in self.paths:
            with open(path + ".sha256", encoding="utf-8") as handle:
                digest, name = handle.read().split()
            self.assertEqual(name, os.path.basename(path))
            self.assertEqual(digest, linux_payload.sha256_bytes(open(path, "rb").read()))

    def test_legacy_arch_name_removed(self):
        self.assertFalse(os.path.exists(self.legacy))
        self.assertFalse(os.path.exists(self.legacy + ".sha256"))

    def test_rpm_profiles_differ_only_in_distribution_tag(self):
        hdrs = {}
        for distro in ("fedora", "suse"):
            with open(self._path(f"-{distro}-x86_64.rpm"), "rb") as handle:
                _, _, hdrs[distro], _, _ = rpm_package.read_rpm(handle.read())
        self.assertEqual(hdrs["fedora"][1010][1], "Fedora")
        self.assertEqual(hdrs["suse"][1010][1], "openSUSE")
        for tag in (1024, 1025, 1026, 1035, 1117, 1049):
            self.assertEqual(hdrs["fedora"][tag], hdrs["suse"][tag], f"tag {tag}")

    def test_rpm_declares_dependencies_and_isa_provide(self):
        with open(self._path("-fedora-x86_64.rpm"), "rb") as handle:
            _, _, hdr, _, _ = rpm_package.read_rpm(handle.read())
        requires = dict(zip(hdr[1049][1], hdr[1050][1]))
        self.assertEqual(requires["glibc"], linux_payload.GLIBC_FLOOR)
        self.assertIn("rpmlib(PayloadIsXz)", requires)
        self.assertIn("greencurve(x86-64)", hdr[1047][1])
        # rpm only owns its own directories, never shared system ones.
        self.assertIn("/usr/share/doc/", hdr[1118][1])
        self.assertNotIn("bin", hdr[1117][1])

    def _corrupt_copy(self, path, offset_from_end):
        copy = path + ".corrupt"
        data = bytearray(open(path, "rb").read())
        data[-offset_from_end] ^= 0xFF
        with open(copy, "wb") as handle:
            handle.write(data)
        self.addCleanup(lambda: os.path.exists(copy) and os.remove(copy))
        return copy

    def test_rpm_payload_corruption_is_rejected(self):
        path = self._path("-fedora-x86_64.rpm")
        copy = self._corrupt_copy(path, 100)
        with self.assertRaises(Exception):
            rpm_package.verify_rpm_package(copy, self.files, VERSION, 1, "x64", "fedora")

    def test_rpm_wrong_distro_is_rejected(self):
        with self.assertRaisesRegex(RuntimeError, "1010"):
            rpm_package.verify_rpm_package(self._path("-suse-x86_64.rpm"), self.files,
                                           VERSION, 1, "x64", "fedora")

    def test_rpm_payload_mismatch_is_rejected(self):
        files = dict(self.files)
        files["usr/bin/greencurve"] = (b"other", 0o755)
        with self.assertRaisesRegex(RuntimeError, "mismatch"):
            rpm_package.verify_rpm_package(self._path("-fedora-x86_64.rpm"), files,
                                           VERSION, 1, "x64", "fedora")

    def test_deb_member_order_is_enforced(self):
        path = self._path(".deb")
        members = deb_package.parse_ar(open(path, "rb").read())
        swapped = deb_package.AR_MAGIC + b"".join(
            deb_package._ar_member(name, data, 0) for name, data in reversed(members))
        copy = path + ".swapped"
        with open(copy, "wb") as handle:
            handle.write(swapped)
        self.addCleanup(os.remove, copy)
        with self.assertRaisesRegex(RuntimeError, "ar members"):
            deb_package.verify_deb_package(copy, self.files, linux_payload.payload_dirs(),
                                           VERSION, "x64", 1)

    def test_deb_wrong_architecture_is_rejected(self):
        with self.assertRaisesRegex(RuntimeError, "Architecture"):
            deb_package.verify_deb_package(self._path(".deb"), self.files,
                                           linux_payload.payload_dirs(), VERSION, "arm64", 1)

    def test_maintainer_scripts_parse_as_posix_sh(self):
        sh = shutil.which("sh")
        git_sh = os.path.join(os.environ.get("ProgramFiles", ""), "Git", "usr", "bin", "sh.exe")
        if os.name == "nt" and os.path.isfile(git_sh):
            sh = git_sh
        if not sh:
            self.skipTest("no POSIX sh on this host; native Linux CI runs this")
        functions = linux_payload.read_install_scriptlet(SCRIPT_DIR)
        scripts = [deb_package.deb_maintainer_script(functions, d) for d in (
            deb_package._POSTINST_DISPATCH, deb_package._PRERM_DISPATCH, deb_package._POSTRM_DISPATCH)]
        scripts += [rpm_package.rpm_scriptlet(functions, d).encode() for d in (
            rpm_package._POSTIN_DISPATCH, rpm_package._PREUN_DISPATCH, rpm_package._POSTUN_DISPATCH)]
        for script in scripts:
            result = subprocess.run([sh, "-n"], input=script, capture_output=True)
            self.assertEqual(result.returncode, 0, result.stderr.decode(errors="replace"))


class RpmHeaderTests(unittest.TestCase):
    ENTRIES = {
        1000: (rpm_header.STRING, "greencurve"),
        1030: (rpm_header.INT16, [0o100755, 0o040755]),
        1028: (rpm_header.INT32, [5, 0]),
        1117: (rpm_header.STRING_ARRAY, ["a", "b"]),
    }

    def _blob(self):
        return rpm_header.encode_header(self.ENTRIES, rpm_header.TAG_HEADERIMMUTABLE)

    def test_round_trip(self):
        tags, length = rpm_header.parse_header(self._blob(), 0, rpm_header.TAG_HEADERIMMUTABLE, True)
        self.assertEqual(length, len(self._blob()))
        self.assertEqual({t: v for t, v in tags.items()}, self.ENTRIES)

    def test_int32_entries_are_aligned(self):
        blob = self._blob()
        il = struct.unpack_from(">I", blob, 8)[0]
        for i in range(1, il):
            _, typ, offset, _ = struct.unpack_from(">IIiI", blob, 16 + 16 * i)
            self.assertEqual(offset % {3: 2, 4: 4}.get(typ, 1), 0)

    def _patch_entry(self, index, field, value):
        blob = bytearray(self._blob())
        struct.pack_into(">I" if field != 2 else ">i", blob, 16 + 16 * index + 4 * field, value)
        return bytes(blob)

    def test_registered_type_mismatch_is_rejected(self):
        blob = self._patch_entry(1, 1, rpm_header.BIN)  # NAME declared as BIN
        with self.assertRaisesRegex(RuntimeError, "registered"):
            rpm_header.parse_header(blob, 0, rpm_header.TAG_HEADERIMMUTABLE, True)

    def test_misaligned_entry_is_rejected(self):
        blob = self._blob()
        _, _, offset, _ = struct.unpack_from(">IIiI", blob, 16 + 16 * 2)  # FILESIZES INT32
        with self.assertRaisesRegex(RuntimeError, "misaligned|overlaps"):
            rpm_header.parse_header(self._patch_entry(2, 2, offset + 1), 0,
                                    rpm_header.TAG_HEADERIMMUTABLE, True)

    def test_wrong_region_tag_is_rejected(self):
        with self.assertRaisesRegex(RuntimeError, "region"):
            rpm_header.parse_header(self._blob(), 0, rpm_header.TAG_HEADERSIGNATURES, False)

    def test_trailer_must_cover_all_entries(self):
        blob = bytearray(self._blob())
        struct.pack_into(">i", blob, len(blob) - 8, -16)
        with self.assertRaisesRegex(RuntimeError, "trailer"):
            rpm_header.parse_header(bytes(blob), 0, rpm_header.TAG_HEADERIMMUTABLE, True)


def run_tests():
    loader = unittest.defaultTestLoader
    suite = unittest.TestSuite()
    for case in (FileNameTests, PackageBuildTests, RpmHeaderTests):
        suite.addTests(loader.loadTestsFromTestCase(case))
    result = unittest.TextTestRunner().run(suite)
    if not result.wasSuccessful():
        raise RuntimeError("Linux distribution package regression tests failed")


if __name__ == "__main__":
    run_tests()
