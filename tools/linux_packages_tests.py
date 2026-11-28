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
import rpm_sign  # noqa: E402

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
        sh = _posix_sh()
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


def _posix_sh():
    git_sh = os.path.join(os.environ.get("ProgramFiles", ""), "Git", "usr", "bin", "sh.exe")
    if os.name == "nt" and os.path.isfile(git_sh):
        return git_sh
    return shutil.which("sh")


class GroupEnrollmentTests(unittest.TestCase):
    """The shared install scriptlet must create the greencurve group even when
    systemd-sysusers exits 0 without creating it (dpkg/rpm have no sysusers
    hook; a container or chroot may not apply the file).  The earlier
    `systemd-sysusers || groupadd` skipped groupadd on that exit status, which
    a real dpkg install in a chroot showed."""

    STUBS = {
        # getent succeeds only once a group "exists" (marker file).
        "getent": '[ -f "$STATE/group" ]',
        "groupadd": 'echo groupadd >> "$STATE/log"; [ -f "$STATE/groupadd_fails" ] || : > "$STATE/group"',
        "systemd-sysusers": 'echo "sysusers $*" >> "$STATE/log"; '
                            '[ -f "$STATE/sysusers_creates" ] && : > "$STATE/group"; exit 0',
        "logname": "exit 1",
    }

    def setUp(self):
        self.sh = _posix_sh()
        if not self.sh:
            self.skipTest("no POSIX sh on this host; native Linux CI runs this")
        self.temp = tempfile.mkdtemp(prefix="gc-enroll-")
        self.addCleanup(shutil.rmtree, self.temp, True)
        self.state = os.path.join(self.temp, "state")
        self.stubs = os.path.join(self.temp, "stubs")
        os.makedirs(self.state)
        os.makedirs(self.stubs)
        for name, body in self.STUBS.items():
            path = os.path.join(self.stubs, name)
            with open(path, "w", newline="\n") as handle:
                handle.write(f"#!/bin/sh\n{body}\n")
            os.chmod(path, 0o755)

    def _run(self):
        functions = linux_payload.read_install_scriptlet(SCRIPT_DIR).decode("utf-8")
        script = functions + '\nPATH="$STUBS:$PATH"\nunset SUDO_USER GREENCURVE_USER\n_setup_group_enrollment\n'
        stubs = self.stubs.replace("\\", "/")
        if os.name == "nt" and stubs[1:3] == ":/":
            # PATH is colon-separated: Git's sh needs /c/... rather than C:/...
            stubs = "/" + stubs[0].lower() + stubs[2:]
        env = dict(os.environ, STATE=self.state.replace("\\", "/"), STUBS=stubs)
        result = subprocess.run([self.sh, "-s"], input=script.encode(), capture_output=True, env=env)
        self.assertEqual(result.returncode, 0, result.stderr.decode(errors="replace"))
        log_path = os.path.join(self.state, "log")
        log = open(log_path).read().split("\n") if os.path.exists(log_path) else []
        return [line for line in log if line], result.stdout.decode(errors="replace")

    def test_groupadd_runs_when_sysusers_exits_zero_without_creating(self):
        log, _ = self._run()
        self.assertEqual(log, ["sysusers greencurve.conf", "groupadd"])
        self.assertTrue(os.path.exists(os.path.join(self.state, "group")))

    def test_no_groupadd_when_sysusers_creates_the_group(self):
        open(os.path.join(self.state, "sysusers_creates"), "w").close()
        log, _ = self._run()
        self.assertEqual(log, ["sysusers greencurve.conf"])

    def test_nothing_runs_when_group_exists(self):
        open(os.path.join(self.state, "group"), "w").close()
        log, _ = self._run()
        self.assertEqual(log, [])

    def test_warns_when_group_cannot_be_created(self):
        open(os.path.join(self.state, "groupadd_fails"), "w").close()
        log, out = self._run()
        self.assertEqual(log, ["sysusers greencurve.conf", "groupadd"])
        self.assertIn("could not create the 'greencurve' group", out)


def _sig_packet(version=4, sig_type=0, pubkey_algo=1, hash_algo=8):
    body = bytes([version, sig_type, pubkey_algo, hash_algo]) + b"\0" * 8
    return bytes([0xC2, len(body)]) + body   # new-format header, tag 2


class SignaturePacketTests(unittest.TestCase):
    def test_rsa_sha256_maps_to_rsaheader(self):
        self.assertEqual(rpm_sign.signature_tag(_sig_packet()), rpm_sign.SIGTAG_RSA)

    def test_eddsa_maps_to_dsaheader(self):
        self.assertEqual(rpm_sign.signature_tag(_sig_packet(pubkey_algo=22)), rpm_sign.SIGTAG_DSA)

    def test_old_format_header_is_parsed(self):
        body = bytes([4, 0, 1, 10]) + b"\0" * 8
        self.assertEqual(rpm_sign.parse_signature_packet(bytes([0x88, len(body)]) + body)[2], 10)

    def test_weak_hash_and_wrong_type_are_rejected(self):
        for packet, why in ((_sig_packet(hash_algo=2), "hash"), (_sig_packet(sig_type=1), "type"),
                            (_sig_packet(version=3), "version"), (b"\x99\x01\x0d", "tag")):
            with self.assertRaises(RuntimeError, msg=why):
                rpm_sign.parse_signature_packet(packet)


def _gpg_tools():
    return rpm_sign.gpg_tool("gpg") and rpm_sign.gpg_tool("gpgv")


@unittest.skipUnless(_gpg_tools(), "gpg/gpgv not installed")
class RpmSigningTests(unittest.TestCase):
    """End-to-end header signing with throwaway keys in a private GNUPGHOME."""

    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.mkdtemp(prefix="gc-rpmsign-")
        cls.home = os.path.join(cls.temp, "gnupg")
        os.makedirs(cls.home)
        os.chmod(cls.home, 0o700)
        cls.old_home = os.environ.get("GNUPGHOME")
        os.environ["GNUPGHOME"] = cls.home
        cls.rsa = cls._make_key("Green Curve rpm test RSA", "rsa3072")
        cls.other = cls._make_key("Green Curve rpm test other", "rsa3072")
        cls.binary = os.path.join(cls.temp, "greencurve")
        with open(cls.binary, "wb") as handle:
            handle.write(b"\x7fELF fixture for signing\n" * 32)
        cls.files = linux_payload.payload_files(SCRIPT_DIR, cls.binary)

    @classmethod
    def tearDownClass(cls):
        gpgconf = rpm_sign.gpg_tool("gpgconf")
        if gpgconf:
            subprocess.run([gpgconf, "--kill", "all"], capture_output=True)
        if cls.old_home is None:
            os.environ.pop("GNUPGHOME", None)
        else:
            os.environ["GNUPGHOME"] = cls.old_home
        shutil.rmtree(cls.temp, ignore_errors=True)

    @classmethod
    def _make_key(cls, uid, algo):
        gpg = rpm_sign.gpg_tool("gpg")
        subprocess.run([gpg, "--batch", "--pinentry-mode", "loopback", "--passphrase", "",
                        "--quick-gen-key", uid, algo, "sign", "never"],
                       check=True, capture_output=True)
        listing = subprocess.run([gpg, "--batch", "--with-colons", "--list-secret-keys", uid],
                                 check=True, capture_output=True, text=True).stdout
        fpr = next(line.split(":")[9] for line in listing.splitlines() if line.startswith("fpr:"))
        pub = os.path.join(cls.temp, f"{fpr}.asc")
        with open(pub, "wb") as handle:
            handle.write(subprocess.run([gpg, "--batch", "--armor", "--export", fpr],
                                        check=True, capture_output=True).stdout)
        return fpr, pub

    def _build(self, signer, name):
        out = os.path.join(self.temp, name)
        return rpm_package.build_rpm_package(SCRIPT_DIR, VERSION, "x64", self.binary, "fedora",
                                             output_dir=out, signer=signer)

    def test_signed_package_verifies_and_uses_rsaheader(self):
        fpr, pub = self.rsa
        path = self._build(rpm_sign.RpmSigner(fpr, pub), "signed")
        _, sig, _, header_blob, _ = rpm_package.read_rpm(open(path, "rb").read())
        self.assertIn(rpm_sign.SIGTAG_RSA, sig)
        self.assertNotIn(rpm_sign.SIGTAG_DSA, sig)
        rpm_sign.verify_header_signature(header_blob, sig[rpm_sign.SIGTAG_RSA][1], pub)

    def test_signature_does_not_verify_with_another_key_or_altered_header(self):
        fpr, pub = self.rsa
        path = self._build(rpm_sign.RpmSigner(fpr, pub), "signed2")
        _, sig, _, header_blob, _ = rpm_package.read_rpm(open(path, "rb").read())
        header_sig = sig[rpm_sign.SIGTAG_RSA][1]
        with self.assertRaisesRegex(RuntimeError, "does not verify"):
            rpm_sign.verify_header_signature(header_blob, header_sig, self.other[1])
        altered = header_blob[:-1] + bytes([header_blob[-1] ^ 1])
        with self.assertRaisesRegex(RuntimeError, "does not verify"):
            rpm_sign.verify_header_signature(altered, header_sig, pub)

    def test_signer_refuses_a_key_that_is_not_the_published_one(self):
        with self.assertRaisesRegex(RuntimeError, "does not verify"):
            self._build(rpm_sign.RpmSigner(self.other[0], self.rsa[1]), "mismatch")

    def test_unsigned_package_fails_a_signed_expectation(self):
        path = self._build(None, "unsigned")
        with self.assertRaisesRegex(RuntimeError, "expected one OpenPGP header signature"):
            rpm_package.verify_rpm_package(path, self.files, VERSION, 1, "x64", "fedora",
                                           expect_signed=True)

    def test_environment_policy(self):
        saved = {k: os.environ.pop(k, None) for k in (rpm_sign.SIGNING_KEY_ENV, rpm_sign.REQUIRE_ENV)}
        try:
            self.assertIsNone(rpm_sign.signer_from_environment(self.temp))
            os.environ[rpm_sign.REQUIRE_ENV] = "1"
            with self.assertRaisesRegex(RuntimeError, "refusing to build unsigned"):
                rpm_sign.signer_from_environment(self.temp)
            os.environ[rpm_sign.SIGNING_KEY_ENV] = self.rsa[0]
            # self.temp has no packaging/greencurve-packages.asc
            with self.assertRaisesRegex(RuntimeError, "is missing"):
                rpm_sign.signer_from_environment(self.temp)
        finally:
            for key, value in saved.items():
                os.environ.pop(key, None)
                if value is not None:
                    os.environ[key] = value


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
    for case in (FileNameTests, PackageBuildTests, GroupEnrollmentTests, SignaturePacketTests,
                 RpmSigningTests, RpmHeaderTests):
        suite.addTests(loader.loadTestsFromTestCase(case))
    result = unittest.TextTestRunner().run(suite)
    if not result.wasSuccessful():
        raise RuntimeError("Linux distribution package regression tests failed")


if __name__ == "__main__":
    run_tests()
