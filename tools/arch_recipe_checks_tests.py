# SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
# SPDX-License-Identifier: MIT
"""Regression fixtures for Arch recipe digest drift and unpinned sources."""
import hashlib
from pathlib import Path
import tempfile
import unittest

import arch_recipe_checks


class ArchRecipeTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.asset = self.root / "resume.service"
        self.asset.write_bytes(b"[Service]\nNoNewPrivileges=true\n")
        digest = hashlib.sha256(self.asset.read_bytes()).hexdigest()
        self.recipe = self.root / "PKGBUILD"
        self.text = (f'source=("$url/archive/$pkgver.tar.gz" "resume.service")\n'
                     f"sha256sums=('{('a' * 64)}' '{digest}')\n"
                     'source_x86_64=("$url/payload.tar.xz")\n'
                     f"sha256sums_x86_64=('{('b' * 64)}')\n")
        self.recipe.write_text(self.text, encoding="utf-8")

    def test_current_assets_and_pinned_remote_sources_pass(self):
        arch_recipe_checks.check_recipe(self.recipe)

    def test_changed_service_with_stale_checksum_is_rejected(self):
        self.asset.write_bytes(self.asset.read_bytes() + b"ProtectHome=yes\n")
        with self.assertRaisesRegex(ValueError, "resume.service checksum"):
            arch_recipe_checks.check_recipe(self.recipe)

    def test_unpinned_source_archive_is_rejected(self):
        self.recipe.write_text(self.text.replace('a' * 64, "SKIP"), encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "pinned SHA-256"):
            arch_recipe_checks.check_recipe(self.recipe)

    def test_unpinned_architecture_payload_is_rejected(self):
        self.recipe.write_text(self.text.replace('b' * 64, "SKIP"), encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "pinned SHA-256"):
            arch_recipe_checks.check_recipe(self.recipe)

    def test_missing_checksum_is_rejected(self):
        self.recipe.write_text(self.text.replace('sha256sums_x86_64=', 'missing='), encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "counts differ"):
            arch_recipe_checks.check_recipe(self.recipe)


def run_tests():
    result = unittest.TextTestRunner().run(
        unittest.defaultTestLoader.loadTestsFromTestCase(ArchRecipeTests))
    if not result.wasSuccessful():
        raise RuntimeError("Arch recipe checksum regression tests failed")


if __name__ == "__main__":
    run_tests()
