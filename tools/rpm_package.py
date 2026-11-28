"""Fedora and openSUSE package builder for Green Curve (RPM v4 format).

Writes an rpm-installable ``.rpm`` from the shared Linux payload with only the
Python standard library, so a Windows host produces the same bytes as a Linux
one and no rpmbuild is required.  Layout (rpm's lib/package.c reader):

    lead (96 bytes) | signature header (padded to 8) | main header | payload

Both headers are rpm "header blobs": magic, index-entry count, data length,
16-byte index entries, then the data store.  Each carries an immutable region
(tag 62 in the signature header, 63 in the main header) whose 16-byte trailer
is the last item in the data store.  rpm rejects a header unless entry data
offsets increase in index order, every entry is aligned for its type, and in
the main header every known tag has its registered type; rpm_header.py
enforces the same rules when reading back.

The payload is an xz-compressed ``newc`` cpio archive with ``./``-prefixed
names.  The signature header carries the SHA-1 and SHA-256 header digests, the
MD5 over header+payload and the sizes; the main header carries the SHA-256
payload digest.  These are what rpm's default `%_pkgverify_level digest`
checks.  The packages are unsigned (no OpenPGP signature).

Fedora and openSUSE get separate files with a distro tag in the name so users
can pick theirs at a glance; their scriptlets and payload are identical.
"""
import hashlib
import lzma
import os
import struct
import sys

try:
    import linux_payload
    import rpm_header
except ImportError:
    sys.path.insert(0, os.path.dirname(__file__))
    import linux_payload
    import rpm_header

from rpm_header import (INT16, INT32, STRING, BIN, STRING_ARRAY, I18NSTRING,
                        encode_header, parse_header)

RPM_LEAD_MAGIC = b"\xed\xab\xee\xdb"
LEAD_SIZE = 96

# Distribution profiles.  `distro` is the file-name tag.
RPM_DISTROS = {
    "fedora": {"distribution": "Fedora", "group": "Unspecified", "label": "Fedora"},
    "suse": {"distribution": "openSUSE", "group": "Hardware/Other", "label": "openSUSE"},
}

# rpmsenseFlags
SENSE_LESS = 1 << 1
SENSE_GREATER = 1 << 2
SENSE_EQUAL = 1 << 3
SENSE_RPMLIB = 1 << 24
# rpmfileAttrs
FILE_DOC = 1 << 1
FILE_LICENSE = 1 << 7
# PGPHASHALGO_SHA256
HASHALGO_SHA256 = 8

# Features the package relies on; rpm refuses it with a clear message on an
# rpm too old to provide them instead of misreading it.
RPMLIB_REQUIRES = (
    ("rpmlib(CompressedFileNames)", "3.0.4-1"),
    ("rpmlib(FileDigests)", "4.6.0-1"),
    ("rpmlib(PayloadFilesHavePrefix)", "4.0-1"),
    ("rpmlib(PayloadIsXz)", "5.2-1"),
)

# rpm runs scriptlets with $1 = number of instances left after the operation:
# %post 1 = fresh install, 2 = upgrade; %preun/%postun 0 = final removal.
_POSTIN_DISPATCH = """
systemctl daemon-reload 2>/dev/null || true
if [ "$1" -eq 1 ] 2>/dev/null; then
    post_install
else
    post_upgrade
fi
exit 0
"""
_PREUN_DISPATCH = """
if [ "$1" -eq 0 ] 2>/dev/null; then
    pre_remove
fi
exit 0
"""
_POSTUN_DISPATCH = """
if [ "$1" -eq 0 ] 2>/dev/null; then
    post_remove
fi
exit 0
"""


def rpm_architecture(arch):
    mapping = {"x64": "x86_64", "arm64": "aarch64"}
    if arch not in mapping:
        raise RuntimeError(f"unsupported rpm architecture: {arch}")
    return mapping[arch]


def _isa(arch):
    # Fedora/openSUSE ISA provide spelling: greencurve(x86-64), greencurve(aarch-64).
    return {"x64": "x86-64", "arm64": "aarch-64"}[arch]


def rpm_package_filename(version, arch, distro, release=1):
    if distro not in RPM_DISTROS:
        raise RuntimeError(f"unknown rpm distro profile: {distro}")
    return f"greencurve-{version}-{release}-{distro}-{rpm_architecture(arch)}.rpm"


def rpm_scriptlet(functions, dispatch):
    """Shared lifecycle functions plus rpm's $1 dispatcher, run by /bin/sh."""
    return functions.decode("utf-8").rstrip("\n") + "\n" + dispatch


def _cpio_entry(name, ino, mode, nlink, mtime, data):
    namez = name.encode("utf-8") + b"\0"
    fields = (ino, mode, 0, 0, nlink, mtime, len(data), 0, 0, 0, 0, len(namez), 0)
    header = b"070701" + b"".join(f"{v:08X}".encode("ascii") for v in fields)
    out = header + namez
    out += b"\0" * (-len(out) % 4)
    out += data
    out += b"\0" * (-len(data) % 4)
    return out


def _file_table(files, owned_dirs):
    """Header-ordered (rpm requires full paths sorted) list of
    (path, data-or-None, full st_mode, flags)."""
    rows = [(p, data, 0o100000 | mode, FILE_DOC if p in linux_payload.DOC_PATHS
             else FILE_LICENSE if p in linux_payload.LICENSE_PATHS else 0)
            for p, (data, mode) in files.items()]
    rows += [(d, None, 0o040755, 0) for d in owned_dirs]
    rows.sort(key=lambda row: "/" + row[0])
    return rows


def build_cpio(rows, mtime):
    out = bytearray()
    for ino, (path, data, mode, _) in enumerate(rows, 1):
        out += _cpio_entry("./" + path, ino, mode, 2 if data is None else 1, mtime,
                           b"" if data is None else data)
    out += _cpio_entry("TRAILER!!!", 0, 0, 1, 0, b"")
    return bytes(out)


def main_header_entries(version, release, arch, distro, rows, mtime, functions,
                        payload_digest, installed_bytes):
    profile = RPM_DISTROS[distro]
    dirnames, dirindexes, basenames = [], [], []
    for path, _, _, _ in rows:
        parent, base = ("/" + path).rsplit("/", 1)
        parent += "/"
        if parent not in dirnames:
            dirnames.append(parent)
        dirindexes.append(dirnames.index(parent))
        basenames.append(base)
    evr = f"{version}-{release}"
    requires = [(name, ver, SENSE_RPMLIB | SENSE_LESS | SENSE_EQUAL) for name, ver in RPMLIB_REQUIRES]
    requires += [("/bin/sh", "", 0),
                 ("glibc", linux_payload.GLIBC_FLOOR, SENSE_GREATER | SENSE_EQUAL)]
    provides = [("greencurve", evr, SENSE_EQUAL), (f"greencurve({_isa(arch)})", evr, SENSE_EQUAL)]
    n = len(rows)
    return {
        100: (STRING_ARRAY, ["C"]),                          # HEADERI18NTABLE
        1000: (STRING, linux_payload.PACKAGE_NAME),          # NAME
        1001: (STRING, version),                             # VERSION
        1002: (STRING, str(release)),                        # RELEASE
        1004: (I18NSTRING, [linux_payload.PACKAGE_SUMMARY]),  # SUMMARY
        1005: (I18NSTRING, [linux_payload.PACKAGE_DESCRIPTION]),  # DESCRIPTION
        1006: (INT32, [mtime]),                              # BUILDTIME
        1007: (STRING, "greencurve-build"),                  # BUILDHOST
        1009: (INT32, [installed_bytes]),                    # SIZE
        1010: (STRING, profile["distribution"]),             # DISTRIBUTION
        1011: (STRING, "Green Curve"),                       # VENDOR
        1014: (STRING, linux_payload.PACKAGE_LICENSE),       # LICENSE
        1015: (STRING, linux_payload.PACKAGER),              # PACKAGER
        1016: (I18NSTRING, [profile["group"]]),              # GROUP
        1020: (STRING, linux_payload.PACKAGE_URL),           # URL
        1021: (STRING, "linux"),                             # OS
        1022: (STRING, rpm_architecture(arch)),              # ARCH
        1024: (STRING, rpm_scriptlet(functions, _POSTIN_DISPATCH)),   # POSTIN
        1025: (STRING, rpm_scriptlet(functions, _PREUN_DISPATCH)),    # PREUN
        1026: (STRING, rpm_scriptlet(functions, _POSTUN_DISPATCH)),   # POSTUN
        1028: (INT32, [0 if d is None else len(d) for _, d, _, _ in rows]),  # FILESIZES
        1030: (INT16, [m for _, _, m, _ in rows]),           # FILEMODES
        1033: (INT16, [0] * n),                              # FILERDEVS
        1034: (INT32, [mtime] * n),                          # FILEMTIMES
        1035: (STRING_ARRAY, ["" if d is None else hashlib.sha256(d).hexdigest()
                              for _, d, _, _ in rows]),      # FILEDIGESTS
        1036: (STRING_ARRAY, [""] * n),                      # FILELINKTOS
        1037: (INT32, [f for _, _, _, f in rows]),           # FILEFLAGS
        1039: (STRING_ARRAY, ["root"] * n),                  # FILEUSERNAME
        1040: (STRING_ARRAY, ["root"] * n),                  # FILEGROUPNAME
        1044: (STRING, f"greencurve-{evr}.src.rpm"),         # SOURCERPM (marks a binary rpm)
        1045: (INT32, [0xFFFFFFFF] * n),                     # FILEVERIFYFLAGS (verify all)
        1047: (STRING_ARRAY, [p[0] for p in provides]),      # PROVIDENAME
        1048: (INT32, [r[2] for r in requires]),             # REQUIREFLAGS
        1049: (STRING_ARRAY, [r[0] for r in requires]),      # REQUIRENAME
        1050: (STRING_ARRAY, [r[1] for r in requires]),      # REQUIREVERSION
        1086: (STRING_ARRAY, ["/bin/sh"]),                   # POSTINPROG
        1087: (STRING_ARRAY, ["/bin/sh"]),                   # PREUNPROG
        1088: (STRING_ARRAY, ["/bin/sh"]),                   # POSTUNPROG
        1095: (INT32, [1] * n),                              # FILEDEVICES
        1096: (INT32, list(range(1, n + 1))),                # FILEINODES
        1097: (STRING_ARRAY, [""] * n),                      # FILELANGS
        1112: (INT32, [p[2] for p in provides]),             # PROVIDEFLAGS
        1113: (STRING_ARRAY, [p[1] for p in provides]),      # PROVIDEVERSION
        1116: (INT32, dirindexes),                           # DIRINDEXES
        1117: (STRING_ARRAY, basenames),                     # BASENAMES
        1118: (STRING_ARRAY, dirnames),                      # DIRNAMES
        1124: (STRING, "cpio"),                              # PAYLOADFORMAT
        1125: (STRING, "xz"),                                # PAYLOADCOMPRESSOR
        1126: (STRING, "9"),                                 # PAYLOADFLAGS
        5011: (INT32, [HASHALGO_SHA256]),                    # FILEDIGESTALGO
        5092: (STRING_ARRAY, [payload_digest]),              # PAYLOADDIGEST
        5093: (INT32, [HASHALGO_SHA256]),                    # PAYLOADDIGESTALGO
    }


def signature_header(header_blob, payload, payload_raw_size):
    return encode_header({
        269: (STRING, hashlib.sha1(header_blob).hexdigest()),     # SHA1 (header)
        273: (STRING, hashlib.sha256(header_blob).hexdigest()),   # SHA256 (header)
        1000: (INT32, [len(header_blob) + len(payload)]),         # SIZE
        1004: (BIN, hashlib.md5(header_blob + payload).digest()),  # MD5
        1007: (INT32, [payload_raw_size]),                        # PAYLOADSIZE
    }, region_tag=rpm_header.TAG_HEADERSIGNATURES)


def build_lead(name_evr, arch):
    archnum = {"x64": 1, "arm64": 19}[arch]
    name = name_evr.encode("ascii")[:65]
    return (RPM_LEAD_MAGIC + struct.pack(">BBhh", 3, 0, 0, archnum) + name.ljust(66, b"\0")
            + struct.pack(">hh", 1, 5) + b"\0" * 16)


def build_rpm_package(script_dir, app_version, arch, binary_path, distro, release=1,
                      output_dir=None):
    """Assemble `greencurve-<v>-<rel>-<distro>-<arch>.rpm` (+ .sha256), verify it
    by reading it back, and return its path."""
    files = linux_payload.payload_files(script_dir, binary_path)
    functions = linux_payload.read_install_scriptlet(script_dir)
    mtime = linux_payload.build_date()
    rows = _file_table(files, linux_payload.OWNED_DIRS)
    cpio = build_cpio(rows, mtime)
    payload = lzma.compress(cpio, preset=9)
    header = encode_header(
        main_header_entries(app_version, release, arch, distro, rows, mtime, functions,
                            hashlib.sha256(payload).hexdigest(),
                            linux_payload.installed_size(files)),
        region_tag=rpm_header.TAG_HEADERIMMUTABLE)
    sig = signature_header(header, payload, len(cpio))
    sig += b"\0" * (-len(sig) % 8)
    blob = build_lead(f"greencurve-{app_version}-{release}", arch) + sig + header + payload

    target_dir = output_dir or script_dir
    os.makedirs(target_dir, exist_ok=True)
    name = rpm_package_filename(app_version, arch, distro, release)
    path = os.path.join(target_dir, name)
    linux_payload.remove_stale([path])
    with open(path, "wb") as handle:
        handle.write(blob)
    verify_rpm_package(path, files, app_version, release, arch, distro)
    linux_payload.write_sha256_file(path)
    print(f"Packaged {RPM_DISTROS[distro]['label']} package {name} "
          f"({len(blob):,} bytes / {len(blob) / 1024:.1f} KB)")
    return path


def read_rpm(data):
    """Split and parse an rpm: returns (lead, sig_tags, header_tags, header_blob, payload)."""
    if len(data) < LEAD_SIZE or not data.startswith(RPM_LEAD_MAGIC):
        raise RuntimeError("not an rpm (bad lead magic)")
    lead = data[:LEAD_SIZE]
    if lead[4] != 3:
        raise RuntimeError(f"rpm lead major {lead[4]} != 3")
    sig_tags, sig_len = parse_header(data, LEAD_SIZE, rpm_header.TAG_HEADERSIGNATURES, typecheck=False)
    hdr_start = LEAD_SIZE + sig_len + (-sig_len % 8)
    if data[LEAD_SIZE + sig_len:hdr_start] != b"\0" * (hdr_start - LEAD_SIZE - sig_len):
        raise RuntimeError("signature header padding is not zero")
    hdr_tags, hdr_len = parse_header(data, hdr_start, rpm_header.TAG_HEADERIMMUTABLE, typecheck=True)
    header_blob = data[hdr_start:hdr_start + hdr_len]
    return lead, sig_tags, hdr_tags, header_blob, data[hdr_start + hdr_len:]


def parse_cpio(data):
    """newc cpio -> [(name, mode, data)] until TRAILER!!!"""
    out, pos = [], 0
    while True:
        if data[pos:pos + 6] != b"070701":
            raise RuntimeError(f"bad cpio magic at {pos}")
        fields = [int(data[pos + 6 + 8 * i:pos + 14 + 8 * i], 16) for i in range(13)]
        mode, size, namesize = fields[1], fields[6], fields[11]
        name_start = pos + 110
        name = data[name_start:name_start + namesize - 1].decode("utf-8")
        pos = name_start + namesize
        pos += -pos % 4
        body = data[pos:pos + size]
        pos += size + (-size % 4)
        if name == "TRAILER!!!":
            return out
        out.append((name, mode, body))


def verify_rpm_package(path, files, version, release, arch, distro):
    """Read the finished rpm back as rpm would: structure, digests, metadata,
    scriptlets, header file list and the cpio payload."""
    label = os.path.basename(path)
    with open(path, "rb") as handle:
        data = handle.read()
    _, sig, hdr, header_blob, payload = read_rpm(data)
    checks = {
        "SHA1": (sig[269][1], hashlib.sha1(header_blob).hexdigest()),
        "SHA256": (sig[273][1], hashlib.sha256(header_blob).hexdigest()),
        "SIZE": (sig[1000][1], [len(header_blob) + len(payload)]),
        "MD5": (sig[1004][1], hashlib.md5(header_blob + payload).digest()),
        "PAYLOADDIGEST": (hdr[5092][1], [hashlib.sha256(payload).hexdigest()]),
    }
    for name, (got, want) in checks.items():
        if got != want:
            raise RuntimeError(f"{label}: {name} digest/size mismatch")
    expect = {1000: "greencurve", 1001: version, 1002: str(release),
              1022: rpm_architecture(arch), 1010: RPM_DISTROS[distro]["distribution"],
              1124: "cpio", 1125: "xz"}
    for tag, want in expect.items():
        if hdr[tag][1] != want:
            raise RuntimeError(f"{label}: header tag {tag} is {hdr[tag][1]!r}, expected {want!r}")
    if 1044 not in hdr:
        raise RuntimeError(f"{label}: SOURCERPM missing; rpm would treat it as a source package")
    for tag, hook in ((1024, "post_install"), (1024, "post_upgrade"),
                      (1025, "pre_remove"), (1026, "post_remove")):
        text = hdr[tag][1]
        if "\r" in text or f"\n    {hook}\n" not in text:
            raise RuntimeError(f"{label}: scriptlet tag {tag} does not dispatch {hook}")

    cpio = lzma.decompress(payload)
    if [len(cpio)] != sig[1007][1]:
        raise RuntimeError(f"{label}: PAYLOADSIZE mismatch")
    entries = parse_cpio(cpio)
    dirnames, dirindexes, basenames = hdr[1118][1], hdr[1116][1], hdr[1117][1]
    header_paths = [dirnames[i] + b for i, b in zip(dirindexes, basenames)]
    if header_paths != sorted(header_paths):
        raise RuntimeError(f"{label}: header file list is not sorted")
    cpio_paths = [name[1:] for name, _, _ in entries]
    if not all(name.startswith("./") for name, _, _ in entries) or cpio_paths != header_paths:
        raise RuntimeError(f"{label}: cpio entries do not match the header file list")
    modes, digests = hdr[1030][1], hdr[1035][1]
    found_files, found_dirs = {}, set()
    for i, (name, mode, body) in enumerate(entries):
        if mode != modes[i]:
            raise RuntimeError(f"{label}: cpio/header mode mismatch for {name}")
        rel = name[2:]
        if mode & 0o170000 == 0o040000:
            found_dirs.add(rel)
            continue
        if digests[i] != hashlib.sha256(body).hexdigest():
            raise RuntimeError(f"{label}: FILEDIGESTS mismatch for {name}")
        found_files[rel] = (body, mode & 0o7777)
    linux_payload.check_payload_matches(label, found_files, found_dirs, files,
                                        list(linux_payload.OWNED_DIRS))
