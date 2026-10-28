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
        self.text = ('pkgver=1.0.0\n'
                     '# Remote digests verified for release: 1.0.0\n'
                     f'source=("$url/archive/$pkgver.tar.gz" "resume.service")\n'
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

    def _tag_archive_recipe(self, body):
        recipe = self.root / "PKGBUILD.tag"
        header = ('url="https://github.com/o/green-curve"\n'
                  'pkgver=1.0.0\n'
                  '# Remote digests verified for release: 1.0.0\n'
                  'source=("$pkgname-$pkgver.tar.gz::$url/archive/refs/tags/$pkgver.tar.gz")\n'
                  f"sha256sums=('{'c' * 64}')\n")
        recipe.write_text(header + body, encoding="utf-8")
        return recipe

    def test_matching_remote_digest_release_has_no_drift(self):
        self.assertIsNone(arch_recipe_checks.check_recipe(self.recipe))

    def test_version_bump_before_repin_reports_drift(self):
        self.recipe.write_text(self.text.replace("pkgver=1.0.0", "pkgver=1.1.0"),
                               encoding="utf-8")
        self.assertEqual(arch_recipe_checks.check_recipe(self.recipe),
                         ("1.0.0", "1.1.0"))

    def test_remote_sources_without_digest_release_are_rejected(self):
        self.recipe.write_text(self.text.replace(
            "# Remote digests verified for release: 1.0.0\n", ""), encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "Remote digests verified for release"):
            arch_recipe_checks.check_recipe(self.recipe)

    def test_local_only_recipe_needs_no_digest_release(self):
        digest = hashlib.sha256(self.asset.read_bytes()).hexdigest()
        self.recipe.write_text(f'source=("resume.service")\nsha256sums=(\'{digest}\')\n',
                               encoding="utf-8")
        self.assertIsNone(arch_recipe_checks.check_recipe(self.recipe))

    def test_tag_archive_dir_must_match_repository_name(self):
        recipe = self._tag_archive_recipe('build() {\n    cd "$pkgname-$pkgver"\n}\n')
        with self.assertRaisesRegex(ValueError, r"unpacks to green-curve-\$pkgver"):
            arch_recipe_checks.check_recipe(recipe)

    def test_tag_archive_dir_via_srcname_passes(self):
        recipe = self._tag_archive_recipe('_srcname="green-curve-$pkgver"\n'
                                          'build() {\n    cd "$_srcname"\n}\n'
                                          'package() {\n    cd "$_srcname"\n}\n')
        arch_recipe_checks.check_recipe(recipe)

    def test_tag_archive_without_cd_is_rejected(self):
        recipe = self._tag_archive_recipe('build() {\n    true\n}\n')
        with self.assertRaisesRegex(ValueError, "no cd"):
            arch_recipe_checks.check_recipe(recipe)


def run_tests():
    result = unittest.TextTestRunner().run(
        unittest.defaultTestLoader.loadTestsFromTestCase(ArchRecipeTests))
    if not result.wasSuccessful():
        raise RuntimeError("Arch recipe checksum regression tests failed")


if __name__ == "__main__":
    run_tests()
