"""Check that the release workflow will select the current release notes."""

import re
from pathlib import Path

_RELEASE_HEADING = re.compile(r"^## (\d+\.\d+(?:\.\d+)?)$", re.MULTILINE)
ALLOWED_CATEGORIES = frozenset(
    {"New", "Improved", "Fixed", "Changed", "Deprecated", "Removed", "Security"}
)


def validate_release_notes(version, changelog):
    sections = list(_RELEASE_HEADING.finditer(changelog))
    if len(sections) < 2 or sections[0].group(1) != version:
        raise ValueError(f"latest versioned changelog section must be ## {version}")
    current = changelog[sections[0].end():sections[1].start()]
    previous = sections[1].group(1)
    subheads = re.findall(r"^### (.+)$", current, re.MULTILINE)
    if len(subheads) < 3:
        raise ValueError("current release notes need at least one category subheading plus Compatibility notes and Downloads and verification")
    categories = subheads[:-2]
    tail = subheads[-2:]
    if tail != ["Compatibility notes", "Downloads and verification"]:
        raise ValueError("current release notes must end with Compatibility notes and Downloads and verification")
    for cat in categories:
        if cat not in ALLOWED_CATEGORIES and cat not in ("Highlights", "Fixes & Hardening"):
            raise ValueError(f"invalid category subheading: '{cat}'. Allowed: {', '.join(sorted(ALLOWED_CATEGORIES))}")
    compare = (f"**Full changelog:** [{previous}...{version}]"
               f"(https://github.com/aufkrawall/green-curve/compare/{previous}...{version})")
    if compare not in current:
        raise ValueError("current release notes need the previous-to-current compare link")


def check_repo(root):
    self_test()
    root = Path(root)
    version = (root / "VERSION").read_text(encoding="utf-8").strip()
    changelog = (root / "CHANGELOG.md").read_text(encoding="utf-8")
    validate_release_notes(version, changelog)
    print(f"Release notes for {version} match the current version and previous tag")


def self_test():
    good = ("## 0.27.0\n### New\n- **Feature.** Detail.\n### Improved\n- **Improvement.** Detail.\n"
            "### Fixed\n- **Bugfix.** Detail.\n### Compatibility notes\n"
            "### Downloads and verification\n"
            "**Full changelog:** [0.26.0...0.27.0]"
            "(https://github.com/aufkrawall/green-curve/compare/0.26.0...0.27.0)\n"
            "## 0.26.0\nEarlier release\n")
    validate_release_notes("0.27.0", good)
    validate_release_notes("0.27.0", good.replace("### New\n- **Feature.** Detail.\n", ""))
    validate_release_notes("0.27.0", good.replace("### New", "### Highlights"))
    for version, notes in (("0.28.0", good),
                           ("0.27.0", good.replace("### Compatibility notes", "")),
                           ("0.27.0", good.replace("### Compatibility notes",
                                                    "### Extra\n### Compatibility notes")),
                           ("0.27.0", good.replace("0.26.0...0.27.0", "0.25.2...0.27.0"))):
        try:
            validate_release_notes(version, notes)
        except ValueError:
            continue
        raise AssertionError("release note mismatch was accepted")
    print("release_prep self-tests passed")


if __name__ == "__main__":
    check_repo(Path(__file__).resolve().parent.parent)
