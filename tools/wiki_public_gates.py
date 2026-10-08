"""Public-wiki gates for Green Curve.

`AGENTS.md` and the `llm-wiki/*.md` topic pages are tracked in Git
and published on GitHub.  `llm-wiki/log/` (chronology) and `llm-wiki/private/`
(incident records, open security findings, machine-specific notes) stay
local-only.  Nothing about that split is visible to a compiler, and a slip is
permanent once pushed, so it is enforced here instead of being left to memory:

* nothing under the two private directories may be tracked;
* `.gitignore` must keep ignoring them, and must not start ignoring the public
  pages again (a re-ignored page silently drops out of `git add -A`);
* `AGENTS.md` must stay tracked as a regular file;
* the public pages must not contain email addresses, Windows auto-generated
  host names, non-documentation IPv4 addresses, secret-shaped tokens, or the
  current developer's OS account name;
* NO tracked file (path or text, any directory) may name a third-party GPU
  tuning, overclocking, monitoring or fan-control application, its binaries or
  its authors.  Docs and comments say "an external tool" and state the
  behaviour as our own observation or driver behaviour.  NVIDIA driver / NVML /
  NvAPI identifiers (for example the fan-control policy entry points), OS
  components and dev/build tooling are not covered.  The denylist is stored as
  SHA-256 digests (see the scheme comment below): this file is itself tracked
  and must not contain the names it forbids, in the clear or in pieces.

Home/profile paths and signing-key material are already covered for EVERY
tracked text file by `security_gates.check_no_developer_profile_paths()` and
`check_no_signing_key_material()`.

One-way dependency like the other gate modules: this file imports nothing from
`build.py`; `security_gates` calls `check_all()` with the live module as `ctx`
and a tracked-file list (None when git is unavailable, which cannot be assessed
and must not fail a tarball build).  Every rule has a pure function and a
self-test fed with bad input, so each gate is proven to fail rather than assumed
to.
"""
import getpass
import hashlib
import os
import re
import subprocess
import sys

PRIVATE_WIKI_PREFIXES = ("llm-wiki/log/", "llm-wiki/private/")
IGNORE_RULES_REQUIRED = ("/llm-wiki/log/", "/llm-wiki/private/")

# Patterns that would (re-)ignore a public page.  Compared as whole lines, so an
# unrelated rule such as `/llm-wiki/log/` never matches.
IGNORE_RULES_FORBIDDEN = frozenset({
    "AGENTS.md", "/AGENTS.md",
    "llm-wiki", "/llm-wiki", "llm-wiki/", "/llm-wiki/",
    "llm-wiki/*", "/llm-wiki/*", "llm-wiki/*.md", "/llm-wiki/*.md",
    "*.md", "/*.md",
})

# Tracked files that must exist for the policy to mean anything.
REQUIRED_TRACKED = (
    "AGENTS.md",
    "llm-wiki/index.md", "llm-wiki/secret-leak-prevention.md",
)

_EMAIL_RE = re.compile(r"[A-Za-z0-9._%+-]+@([A-Za-z0-9-]+(?:\.[A-Za-z0-9-]+)+)")
_ALLOWED_EMAIL_DOMAINS = frozenset({
    "example.com", "example.org", "example.net", "users.noreply.github.com",
})
_ALLOWED_EMAIL_SUFFIXES = (
    ".example", ".invalid", ".test", ".localhost", ".example.com",
    ".example.org", ".example.net", ".users.noreply.github.com",
)

_HOST_RE = re.compile(r"\b(?:DESKTOP|LAPTOP)-[A-Z0-9]{6,8}\b")

_OCTET = r"(?:25[0-5]|2[0-4]\d|1\d\d|[1-9]?\d)"
_IPV4_RE = re.compile(
    r"(?<![\w.])(" + _OCTET + r"(?:\." + _OCTET + r"){3})(?!\w|\.\d)")
_ALLOWED_IPV4_PREFIXES = ("127.", "192.0.2.", "198.51.100.", "203.0.113.")

# Secret-shaped tokens with a distinctive prefix.  Deliberately not a generic
# "key = long string" rule: prose and workflow snippets in the wiki mention
# `TAG_SIGNING_KEY` and friends all the time and a noisy gate gets bypassed.
_TOKEN_RES = (
    ("GitHub token", re.compile(r"\bgh[pousr]_[A-Za-z0-9]{30,}\b")),
    ("GitHub fine-grained token", re.compile(r"\bgithub_pat_[A-Za-z0-9_]{20,}\b")),
    ("AWS access key id", re.compile(r"\bAKIA[0-9A-Z]{16}\b")),
    ("Slack token", re.compile(r"\bxox[abprs]-[A-Za-z0-9-]{10,}\b")),
    ("Google API key", re.compile(r"\bAIza[0-9A-Za-z_-]{35}\b")),
    ("API secret key", re.compile(r"\bsk-[A-Za-z0-9]{32,}\b")),
    ("PEM private key", re.compile(r"-----BEGIN (?:[A-Z ]+ )?PRIVATE KEY-----")),
)

# Third-party tool denylist, stored as SHA-256 digests so that this tracked file
# does not itself name what it forbids (neither in the clear nor in fragments).
#
# Scheme: a name is normalised to its alphanumeric word tokens, casefolded and
# joined WITHOUT separators ("Some-Tool 2" -> "sometool2"); the digest of that
# string is stored.  Scanned text is normalised the same way: split into
# alphanumeric tokens (case, punctuation, underscores and path separators do not
# matter) and every 1-, 2- and 3-token window is joined without separators and
# hashed.  Because the separator-free form is what is stored, "name-x",
# "name x", "namex" and "NAMEX.exe" all hit the same digest.
#
#   * _DENY_SINGLE entries match only as ONE whole token, so they are word
#     bounded and ordinary prose or identifiers that merely contain the words
#     never match (a long identifier is one token and hashes to something else).
#   * _DENY_MULTI entries may also match across 2-3 adjacent tokens.
#
# To add a name: digest = sha256(normalised_name.encode()).hexdigest(), add it to
# the matching set, and keep the name itself out of every tracked file.
_DENY_SINGLE = frozenset({
    "09fca0331742a45f8fafd2b22a4daa8698079fcc051045aa099711482ae2d239",
    "0a3f9c9182c3b8bab059fd4189583e8ab9a8cde8e7f0a321ded3368b8e69e59c",
    "10a597ba510078056cb401ec9f29c3622f3cda582cd3ac9f02cb1dbca879e3d3",
    "1960a67224ebe260f25a794b3bb51d66e5ef7c0ac8d403f7ebac3261c9e9e2be",
    "1a8835ec6af4408c0fe29c34924f5b3e72956f33104566e3c748d607822aa9f1",
    "264a6db6eca819d18fb31fc81e0c91dd66a3f7add6cd683916fddbe5c4e293df",
    "280970d1a0a96a8ef10bd1e3c310e6a3250f4a22991540dc298a40cc3f98b5bc",
    "2f1fc7dc83012fa66bc04d493b6fc957bd57a65771268765b4ed94a2e32945c2",
    "335fef785161dcdabcad6510d80b13ec7b4bc4232438991253a2e3d60ed58d44",
    "37d29f2089f20b42861cbf61a953c04bb37f2ab810c1e9c5ff3abe2d4ca5cc32",
    "3d717495dadc0c2d21b52c5b5f163742ea08bda5726abdf8211536dbb90de9ff",
    "4bfe776e66c99c2e4a23b1549da9abd7aefc1fb4f1b74cb94830e9bd9b2c5312",
    "52ed9cb1bb30ec37a5f3a27327744136051f2c6c473f161e533eaafc3d691e87",
    "5f1e90e5bbf30fa0c0291e7d1e7bfd17a09ae64689ced1caae917d5fd428eabe",
    "60091238febbae25e9d71672a130c63c1f094ffd9d3f91a38b7d982b0c40a35a",
    "764ed731d081c4d9df04e6be1c39ef58a3a6c44e2954bb7387e35a9f068a4b6a",
    "7fc9deee4d132eb0e73bbb68cc2ecaa04dfd60690534639ba291af7af28cbce6",
    "9b589da058ca5a8cc483624f20b5acf022a318591a1e21178ce19cc514dabd16",
    "abacbac95313c709aaf4110d30ee4afdfc0e85b414bc092faa62965895521d18",
    "d0c3268e36add27abef65fdfe242ecabc318e53c753ef1163b8984137b702925",
    "d9121ef64664409c552830971564c18d969736325357bb7d03df426b5b33bc52",
    "dbacfee11f31bc07025e86fe8f86ebcaffeaa090da27e818871d4018555cb874",
    "e69992c06393996b2871c41a6aa9a057d04ea880b82ffbfb13eeb5f92bff2055",
    "f100da96443143a0d1d462d380a4cb1cfea1e758e5af457c8c7ef9b2f3ed1ce4",
    "f496d75216515186e66eb20a373092468f2115931b7ce486391c29b94117c78a",
})

_DENY_MULTI = frozenset({
    "02f742a818f553a2522725600fa76cf6f672f36e6a57c3942bf416d1d3c1c6c4",
    "1eeeb3cabaa9f6078bd09864eb2140cc280c2333cd57e83275eaea48698055fd",
    "28891689cf0d71a14ed2b8cef539c6444ea0be1a4ea13cd2ab4c675fa195d54c",
    "3a92e46eb33521c8448a5c472a91cbb353f770f49b90ff9f7a03ef24d9ad03ba",
    "8573266168af983da9f7ae8643af38f64d22b438e81684fcdca525e8b294497d",
    "9312dc0c1a525b080a2e690f1d8bebd98a38f7204e28f28ddce1c08ecc709bfd",
    "9abcae2ce6b98bf05a3e8e35ab23da516c3053a1f9d554bfa7f86e23b357dbe0",
    "bcc34a59299812461ad4ea2889170ceca78f061686c8e08429ba888b0a3770c3",
    "ee9505d075c45e210e4f6cf133edca68f8e5d1e2a4b6d487de5a704249e894c2",
    "fdf5570e790722a9b660e68ce8b521b4c70ec6ab44178a4a511b78bdd026470e",
})
_DENY_ALL = _DENY_SINGLE | _DENY_MULTI
_TOKEN_RE = re.compile(r"[a-z0-9]+")
_MAX_WINDOW = 3

# Account names that are placeholders, not people.
_GENERIC_ACCOUNT_NAMES = frozenset({
    "user", "users", "test", "tester", "testuser", "admin", "administrator",
    "root", "default", "public", "guest", "runner", "runneradmin", "ubuntu",
    "vagrant", "builder", "build", "docker", "github", "system", "service",
    "owner", "dev", "developer",
})


def normalize_rel(path):
    """Repository-relative path in git spelling, case-folded for comparison.

    Windows checkouts are case-insensitive, so `LLM-WIKI/Log/x` must be treated
    as the private directory it would collide with.
    """
    rel = path.replace("\\", "/")
    while rel.startswith("./"):
        rel = rel[2:]
    return rel.casefold()


def private_wiki_errors(tracked):
    """Tracked paths inside the local-only wiki areas (pure)."""
    errors = []
    for path in tracked:
        rel = normalize_rel(path)
        for prefix in PRIVATE_WIKI_PREFIXES:
            if rel.startswith(prefix) or rel == prefix.rstrip("/"):
                errors.append(f"{path}: local-only wiki area must never be tracked")
                break
    return errors


def required_tracked_errors(tracked):
    """The public agent/wiki pages must actually be tracked (pure)."""
    present = {normalize_rel(path) for path in tracked}
    return [f"{rel}: must be tracked (public agent instructions / wiki)"
            for rel in REQUIRED_TRACKED if rel.casefold() not in present]


def gitignore_errors(text):
    """Rules `.gitignore` must keep, and rules it must never gain (pure)."""
    rules = []
    for raw in text.splitlines():
        line = raw.strip()
        if line and not line.startswith("#"):
            rules.append(line)
    errors = []
    for needed in IGNORE_RULES_REQUIRED:
        if needed not in rules:
            errors.append(f".gitignore: missing the `{needed}` rule "
                          "(local-only wiki area must stay ignored)")
    for rule in rules:
        if rule in IGNORE_RULES_FORBIDDEN:
            errors.append(f".gitignore: `{rule}` would ignore a public "
                          "agent-instruction / wiki page")
        if rule.startswith("!") and "llm-wiki" in rule and (
                "log" in rule or "private" in rule):
            errors.append(f".gitignore: `{rule}` re-includes a local-only wiki area")
    return errors


def agent_file_identity_errors(agents_mode, agents_data):
    """`AGENTS.md` must be tracked and be a regular file (pure).

    Modes are git index modes as strings ("100644", "120000").
    """
    if agents_mode is None:
        return ["AGENTS.md must be tracked"]
    if agents_mode == "120000":
        return ["AGENTS.md must be a regular file (not a symlink)"]
    return []


def _redact(text):
    return text if len(text) <= 6 else text[:3] + "..." + text[-2:]


def public_text_errors(rel, text, account_name=None):
    """Personal-data / secret-shaped findings in one public page (pure)."""
    errors = []
    name_re = None
    if (account_name and len(account_name) >= 4
            and account_name.casefold() not in _GENERIC_ACCOUNT_NAMES):
        name_re = re.compile(r"(?<![A-Za-z0-9])" + re.escape(account_name)
                             + r"(?![A-Za-z0-9])", re.IGNORECASE)
    for line_no, line in enumerate(text.splitlines(), 1):
        for match in _EMAIL_RE.finditer(line):
            domain = match.group(1).lower()
            if domain in _ALLOWED_EMAIL_DOMAINS or domain.endswith(
                    _ALLOWED_EMAIL_SUFFIXES):
                continue
            errors.append(f"{rel}:{line_no}: email address "
                          f"({_redact(match.group(0))})")
        for match in _HOST_RE.finditer(line):
            errors.append(f"{rel}:{line_no}: Windows auto-generated host name "
                          f"({_redact(match.group(0))})")
        for match in _IPV4_RE.finditer(line):
            address = match.group(1)
            if address == "0.0.0.0" or address.startswith(_ALLOWED_IPV4_PREFIXES):
                continue
            errors.append(f"{rel}:{line_no}: IPv4 address ({_redact(address)})")
        for label, pattern in _TOKEN_RES:
            for match in pattern.finditer(line):
                errors.append(f"{rel}:{line_no}: {label} "
                              f"({_redact(match.group(0))})")
        if name_re and name_re.search(line):
            errors.append(f"{rel}:{line_no}: the developer's own OS account name")
    return errors


def _digest(text):
    return hashlib.sha256(text.encode("utf-8")).hexdigest()


_DIGEST_CACHE = {}


def _denied_window(joined, single_only, singles, multis):
    cacheable = singles is _DENY_SINGLE and multis is _DENY_MULTI
    key = (joined, single_only)
    if cacheable:
        hit = _DIGEST_CACHE.get(key)
        if hit is not None:
            return hit
    digest = _digest(joined)
    hit = digest in multis or (single_only and digest in singles)
    if cacheable and len(_DIGEST_CACHE) < 500000:
        _DIGEST_CACHE[key] = hit
    return hit


def denied_tokens(line, singles=None, multis=None):
    """Words of `line` whose normalised form is on the denylist (pure).

    Returns the matching source windows (as plain token text) so a finding can
    point at the offending spot.  `singles` / `multis` default to the real digest
    sets; self-tests inject digests of made-up names instead.
    """
    singles = _DENY_SINGLE if singles is None else singles
    multis = _DENY_MULTI if multis is None else multis
    tokens = _TOKEN_RE.findall(line.casefold())
    found = []
    for start in range(len(tokens)):
        for size in range(1, _MAX_WINDOW + 1):
            if start + size > len(tokens):
                break
            joined = "".join(tokens[start:start + size])
            if len(joined) > 40:
                break
            if _denied_window(joined, size == 1, singles, multis):
                found.append(" ".join(tokens[start:start + size]))
    return found


def third_party_tool_errors(rel, text, singles=None, multis=None):
    """Third-party tool names in a tracked file's path or text (pure).

    Applies to EVERY tracked file: a comment in source, a test name, a string
    literal, a workflow step and a wiki page are all public.  Functional uses
    are reported to the maintainer rather than silently rewritten, which is why
    this gate reports and never fixes.
    """
    errors = []
    for word in denied_tokens(rel.replace("\\", "/"), singles, multis):
        errors.append(f"{rel}: path names a third-party tool ({word})")
    for line_no, line in enumerate(text.splitlines(), 1):
        for word in denied_tokens(line, singles, multis):
            errors.append(f"{rel}:{line_no}: third-party tool name "
                          f"({word}); say \"an external tool\"")
    return errors


def is_public_text_path(path):
    rel = normalize_rel(path)
    return rel == "agents.md" or (
        rel.startswith("llm-wiki/") and rel.endswith(".md"))


def current_account_name():
    """The OS account name of whoever is running the build, or None.

    The gate must be able to reject the maintainer's real name without that
    name being written into a tracked file, so it is derived at run time.
    Skipped on hosted CI, whose account names are generic and appear in
    ordinary prose ("runner").
    """
    if os.environ.get("GITHUB_ACTIONS") or os.environ.get("CI"):
        return None
    try:
        return getpass.getuser()
    except Exception:  # pylint: disable=broad-except
        return os.path.basename(os.path.expanduser("~")) or None


def _git(root, *args):
    try:
        return subprocess.run(["git", *args], cwd=root, capture_output=True)
    except (OSError, ValueError):
        return None


def _index_entry(root, path):
    """(mode, bytes) of a path as staged/committed, or (None, None)."""
    listing = _git(root, "ls-files", "-s", "--", path)
    if listing is None or listing.returncode != 0 or not listing.stdout.strip():
        return None, None
    meta = listing.stdout.decode("utf-8", "replace").split("\t", 1)[0].split()
    if len(meta) < 2:
        return None, None
    blob = _git(root, "cat-file", "blob", meta[1])
    if blob is None or blob.returncode != 0:
        return None, None
    return meta[0], blob.stdout


def _ignored_by_git(root, path):
    """True/False from `git check-ignore`, or None when it cannot be asked."""
    result = _git(root, "check-ignore", "-q", "--no-index", "--", path)
    if result is None or result.returncode not in (0, 1):
        return None
    return result.returncode == 0


def check_all(ctx, tracked):
    """Fail the build when the public/private wiki split is violated."""
    if tracked is None:
        return  # tarball/export build: no git index to assess
    root = ctx.SCRIPT_DIR
    errors = []
    errors += private_wiki_errors(tracked)
    errors += required_tracked_errors(tracked)
    try:
        with open(os.path.join(root, ".gitignore"), "r", encoding="utf-8") as handle:
            errors += gitignore_errors(handle.read())
    except OSError:
        errors.append(".gitignore: unreadable")
    for probe in ("llm-wiki/log/recent.md", "llm-wiki/private/notes.md"):
        if _ignored_by_git(root, probe) is False:
            errors.append(f"{probe}: git does not ignore this local-only path")
    for probe in ("AGENTS.md", "llm-wiki/index.md"):
        if _ignored_by_git(root, probe) is True:
            errors.append(f"{probe}: git ignores this public page")
    agents_mode, agents_data = _index_entry(root, "AGENTS.md")
    if agents_mode is not None:
        errors += agent_file_identity_errors(agents_mode, agents_data)
    account = current_account_name()
    for path in tracked:
        try:
            with open(os.path.join(root, path), "r", encoding="utf-8") as handle:
                text = handle.read()
        except (OSError, UnicodeDecodeError):
            text = ""  # binary or unreadable: only the path is checked
        errors += third_party_tool_errors(path, text)
        if is_public_text_path(path):
            errors += public_text_errors(path, text, account)
    if errors:
        print("Regression source check FAILED: public wiki / agent-instruction "
              "rules (see llm-wiki/secret-leak-prevention.md, 'Public wiki "
              "safety'):")
        for error in errors[:30]:
            print(f"  {error}")
        if len(errors) > 30:
            print(f"  ... and {len(errors) - 30} more")
        sys.exit(1)


def run_self_tests():
    """Prove every rule fails on bad input and stays quiet on good input."""
    failures = []

    def expect(condition, message):
        if not condition:
            failures.append(message)

    # --- tracked private areas -------------------------------------------------
    expect(private_wiki_errors(["llm-wiki/log/recent.md"]),
           "a tracked llm-wiki/log file must fail")
    expect(private_wiki_errors(["llm-wiki/private/notes.md"]),
           "a tracked llm-wiki/private file must fail")
    expect(private_wiki_errors(["llm-wiki\\log\\archive.md"]),
           "backslash spelling of a private path must fail")
    expect(private_wiki_errors(["LLM-Wiki/Private/x.md"]),
           "case variants collide on Windows and must fail")
    expect(private_wiki_errors(["./llm-wiki/log/x.md"]),
           "a leading ./ must not hide a private path")
    expect(private_wiki_errors(["llm-wiki/private"]),
           "a file named like the private directory must fail")
    expect(not private_wiki_errors(
        ["llm-wiki/index.md", "llm-wiki/updates.md", "AGENTS.md",
         "llm-wiki/logbook.md", "llm-wiki/privately.md", "tools/log_gates.py"]),
           "public pages and look-alike names must pass")

    # --- required tracked files ------------------------------------------------
    full = ["AGENTS.md", "llm-wiki/index.md",
            "llm-wiki/secret-leak-prevention.md"]
    expect(not required_tracked_errors(full), "all required files present must pass")
    expect(required_tracked_errors([p for p in full if p != "AGENTS.md"]),
           "a missing AGENTS.md must fail")
    expect(required_tracked_errors([p for p in full if p != "llm-wiki/index.md"]),
           "a missing wiki index must fail")

    # --- .gitignore ------------------------------------------------------------
    good_ignore = "# c\n/llm-wiki/log/\n/llm-wiki/private/\n*.exe\n"
    expect(not gitignore_errors(good_ignore), "the intended .gitignore must pass")
    expect(gitignore_errors("*.exe\n/llm-wiki/log/\n"),
           "a missing private-directory rule must fail")
    expect(gitignore_errors("/llm-wiki/private/\n"),
           "a missing log-directory rule must fail")
    expect(gitignore_errors(good_ignore + "llm-wiki/\n"),
           "re-ignoring the whole wiki must fail")
    expect(gitignore_errors(good_ignore + "AGENTS.md\n"),
           "re-ignoring AGENTS.md must fail")
    expect(gitignore_errors(good_ignore + "*.md\n"),
           "ignoring every markdown file must fail")
    expect(gitignore_errors(good_ignore + "!llm-wiki/log/keep.md\n"),
           "a negation that re-includes a private area must fail")
    expect(not gitignore_errors(good_ignore + "# AGENTS.md\n  \n"),
           "a comment mentioning a forbidden rule must not fail")

    # --- AGENTS.md identity ----------------------------------------------------
    agents = b"# Agent Instructions\nline\n"
    expect(not agent_file_identity_errors("100644", agents),
           "a regular AGENTS.md must pass")
    expect(agent_file_identity_errors("120000", b"other.md"),
           "a symlinked AGENTS.md must fail")
    expect(agent_file_identity_errors(None, None),
           "an untracked AGENTS.md must fail")

    # --- public page content ---------------------------------------------------
    def hits(text, account=None):
        return public_text_errors("page.md", text, account)

    expect(hits("contact: someone@gmail.com"), "an email address must fail")
    expect(hits("mail first.last@corp.acme.io"), "a corporate email must fail")
    expect(not hits("`https://github.com@evil.example/a` defeats a check"),
           "the documented evil.example URL fixture must pass")
    expect(not hits("noreply: 123+bot@users.noreply.github.com, a@example.com"),
           "documentation and noreply addresses must pass")
    expect(hits("host DESKTOP-AB12CD3 crashed"), "a Windows auto host name must fail")
    expect(hits("laptop LAPTOP-ZX98YUT1"), "a LAPTOP auto host name must fail")
    expect(not hits("DESKTOP-class machines, desktop-shell"),
           "ordinary prose around the word desktop must pass")
    expect(hits("server at 203.0.114.7 answered"), "a public IPv4 address must fail")
    expect(hits("lan 192.168.1.20"), "a private LAN IPv4 address must fail")
    expect(not hits("loopback 127.0.0.1 and 0.0.0.0 and 192.0.2.1"),
           "loopback and documentation addresses must pass")
    expect(not hits("version 0.26.0.261, Defender 1.459.359.0, driver 610.43.03"),
           "dotted version numbers must not read as IPv4")
    expect(hits("token ghp_" + "a1B2c3D4e5F6g7H8i9J0k1L2m3N4o5P6q7R8"),
           "a GitHub token must fail")
    expect(hits("github_pat_" + "11ABCDEFG0abcdefghij_klmnopqrstuvwxyz0123456789"),
           "a fine-grained GitHub token must fail")
    expect(hits("key AKIA" + "ABCDEFGHIJKLMNOP"), "an AWS key id must fail")
    expect(hits("-----BEGIN " + "RSA PRIVATE KEY-----"), "a PEM private key must fail")
    expect(not hits("the TAG_SIGNING_KEY secret and update-signing-key.txt names"),
           "prose that merely names key secrets must pass")
    expect(hits("owned by Alexandra today", account="Alexandra"),
           "the developer's own account name must fail")
    expect(hits("owned by alexandra.", account="Alexandra"),
           "the account name check is case-insensitive")
    expect(not hits("owned by Alexandrapolis", account="Alexandra"),
           "a longer word containing the name must pass")
    expect(not hits("a runner and a user", account="runner"),
           "generic account names must not be treated as a person")
    expect(not hits("any text", account="ab"), "a very short name is not checked")
    expect(not hits("anything at all", account=None), "no account name means no check")

    expect(is_public_text_path("llm-wiki/updates.md"), "wiki pages are public text")
    expect(is_public_text_path("AGENTS.md"),
           "AGENTS.md is public text")
    expect(not is_public_text_path("README.md"), "README is not in this gate's scope")
    expect(not is_public_text_path("llm-wiki/data.json"), "only markdown is scanned")

    # --- third-party tool denylist ---------------------------------------------
    # The tests inject digests of MADE-UP names, so no real product is named in
    # this file either.  The real digest sets are checked structurally below.
    fake_single = frozenset({_digest("zorbtune"), _digest("zorbcontrol"),
                             _digest("quuxmon9")})
    fake_multi = frozenset({_digest("quuxprecisionx9"), _digest("blarggz")})

    def denied(text, rel="page.md"):
        return third_party_tool_errors(rel, text, fake_single, fake_multi)

    for sample in ("uses ZorbTune here", "ZORBTUNE.EXE is flagged",
                   "(zorbtune), and Zorbtune's model", "zorb_tune_cli zorbtune.log",
                   "run Quux Precision X9 now", "quux-precision-x9", "QuuxPrecisionX9",
                   "quux_precision_x9.exe", "Blarg-GZ and blarggz", "Blarg GZ",
                   "ZorbControl app", "quuxmon9 reading", "QUUXMON9"):
        expect(denied(sample), f"denylist must catch {sample!r}")
    expect(denied("ok", rel="tools/zorbtune-inspect/x.txt"),
           "a tracked PATH naming a denied tool must fail")
    expect(denied("ok", rel="x\\zorbtune\\y.md"), "backslash paths are checked too")
    expect(":2:" in denied("line one\nthe ZorbTune tool")[0],
           "findings must carry the line number")
    for benign in ("zorbtunes and zorbtuner", "unzorbtune, zorbtuned",
                   "nvmlDeviceGetZorbControlPolicy_v2 / nvmlDeviceSetZorbControlPolicy",
                   "`zorbControlSignal` and zorb_control_policy and ZORB_CONTROL_POLICY",
                   "# Zorb Control\nmanual zorb control and Zorb control failed",
                   "quux precision x8, quux precision, precision x9 and quux x9",
                   "quuxmon, quuxmon99, quuxmon 9"):
        expect(not denied(benign), f"look-alike must pass: {benign!r}")
    # A two-token window needs the adjacent tokens: an unrelated word between
    # them breaks the match, and a SINGLE-only entry never spans tokens.
    expect(not denied("quux, precision then x9"), "a window needs adjacent tokens")
    expect(not denied("zorb tune"), "single-token entries must not span tokens")
    expect(denied("quux precision x9") and denied("quux  precision\tx9"),
           "whitespace between tokens does not matter")

    # The real sets: structural checks only (they must never be edited into
    # plain names, and prose like "fan control" must stay benign).
    for digest_set in (_DENY_SINGLE, _DENY_MULTI):
        expect(len(digest_set) > 5, "real denylist digests must be present")
        expect(all(re.fullmatch(r"[0-9a-f]{64}", d) for d in digest_set),
               "real denylist entries must be SHA-256 hex digests")
    expect(not _DENY_SINGLE & _DENY_MULTI, "a digest belongs to one set only")
    for benign in ("manual fan control and Fan Control", "fan_control_policy",
                   "nvmlDeviceGetFanControlPolicy_v2", "NVML_FAN_CONTROL_POLICY",
                   "fanControlSignal", "Fan control change failed",
                   "restart the service"):
        expect(not third_party_tool_errors("x.md", benign),
               f"real denylist must not flag {benign!r}")

    if failures:
        for failure in failures:
            print(f"wiki_public_gates self-test FAILED: {failure}")
        sys.exit(1)
    print("wiki_public_gates self-tests passed")
