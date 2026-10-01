# SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
# SPDX-License-Identifier: MIT
"""Signing inputs must be readable by the native updater; no real keys used."""
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

import update_signing
import update_manifest_tools


class SigningInputTests(unittest.TestCase):
    def test_versions_rejected_by_native_client_are_never_prepared(self):
        for version in ("01.2.3", "1.02", "1.2.00", "1000000.2.3", "1.0000000"):
            with self.subTest(version=version), tempfile.TemporaryDirectory() as directory:
                (Path(directory) / update_signing.asset_name(version, "x64")).write_bytes(b"fixture")
                with self.assertRaises(ValueError):
                    update_signing.build_manifest(version, directory)

    def test_invalid_minimum_version_is_never_prepared(self):
        with tempfile.TemporaryDirectory() as directory:
            (Path(directory) / update_signing.asset_name("1.2.3", "x64")).write_bytes(b"fixture")
            for floor in ("01.2", "1000000.0", ""):
                with self.subTest(floor=floor), self.assertRaises(ValueError):
                    update_signing.build_manifest("1.2.3", directory, floor)

    def test_malformed_renewal_never_reads_the_private_key(self):
        with tempfile.TemporaryDirectory() as directory:
            manifest = Path(directory) / "manifest.txt"
            manifest.write_bytes(b"# fixture\nformat=1\nversion=1.2.3\n")
            with patch.object(update_signing, "read_private_key") as read_key:
                with self.assertRaises(ValueError):
                    update_signing.main(["renew-freshness", str(manifest), "--key", "placeholder",
                                         "--dir", directory])
                read_key.assert_not_called()


class ManifestInputTests(unittest.TestCase):
    def setUp(self):
        self.payload = (b"# fixture\nformat=1\nversion=1.2.3\nmin_from=1.0\n"
                        b"x64_file=greencurve-1.2.3-windows-x64-setup.exe\n"
                        b"x64_size=7\nx64_sha256=" + b"a" * 64 + b"\n")

    def test_native_accepted_spellings_preserve_values(self):
        for version in ("0.27", "0.27.0", "999999.999999.999999"):
            self.assertEqual(update_manifest_tools.validate_version(version), version)
        for payload in (self.payload, self.payload.replace(b"\n", b"\r\n"),
                        self.payload.rstrip(b"\n")):
            entries = update_manifest_tools.parse_manifest(payload)
            self.assertEqual(entries["min_from"], "1.0")
            self.assertEqual(entries["x64_size"], "7")

    def test_native_rejected_fields_are_rejected_before_signing(self):
        candidates = (
            self.payload + b"version=1.2.3\n",
            self.payload + b"min_from=1.1\n",
            self.payload + b"unknown=value\n",
            self.payload.replace(b"format=1", b"format=01"),
            self.payload.replace(b"version=1.2.3", b"version=01.2.3"),
            self.payload.replace(b"min_from=1.0", b"min_from=1000000.0"),
            self.payload.replace(b"x64_size=7", b"x64_size=07"),
            self.payload.replace(b"x64_size=7", b"x64_size=0"),
            self.payload.replace(b"x64_size=7", b"x64_size=268435457"),
            self.payload.replace(b"a" * 64, b"A" * 64),
            self.payload.replace(b"windows-x64", b"windows-arm64"),
            self.payload.replace(b"x64_size=7\n", b""),
            self.payload.replace(b"x64_size=7", b"x64_size=7\0"),
            self.payload.replace(b"x64_size=7", b"x64_size=" + b"7" * 510),
            self.payload + b"#" * 4096,
            b"",
        )
        for index, payload in enumerate(candidates):
            with self.subTest(index=index), self.assertRaises(ValueError):
                update_manifest_tools.parse_manifest(payload)

    def test_asset_size_ceiling_is_accepted(self):
        accepted = self.payload.replace(b"x64_size=7", b"x64_size=268435456")
        self.assertEqual(update_manifest_tools.parse_manifest(accepted)["x64_size"], "268435456")


def run_tests():
    result = unittest.TextTestRunner().run(
        unittest.TestSuite(unittest.defaultTestLoader.loadTestsFromTestCase(cls)
                           for cls in (SigningInputTests, ManifestInputTests)))
    if not result.wasSuccessful():
        raise RuntimeError("update signing input regression tests failed")


if __name__ == "__main__":
    run_tests()
