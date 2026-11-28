"""RPM v4 header blob encoder and validating parser.

Split from rpm_package.py so the format rules sit in one small module with
their own self-tests.  The parser applies the same structural checks as rpm's
own hdrblobVerifyInfo/hdrblobVerifyRegion (lib/header.c), so a header that
passes here is not rejected by rpm for layout reasons:

- the first index entry is the region tag (62 signature / 63 main header),
  type BIN, count 16, pointing at a trailer that is the LAST 16 bytes of the
  data store and whose offset is -(index entries * 16);
- every other entry's data starts at or after the previous entry's end
  (offsets increase in index order) and before the trailer;
- every entry is aligned for its type (INT16: 2, INT32: 4, INT64: 8);
- in the main header, every tag listed in TAG_TYPES carries exactly that type
  (rpm's hdrchkTagType; unknown tags are allowed, as in rpm).
"""
import struct

HEADER_MAGIC = b"\x8e\xad\xe8\x01\x00\x00\x00\x00"
NULL, CHAR, INT8, INT16, INT32, INT64, STRING, BIN, STRING_ARRAY, I18NSTRING = range(10)
_ALIGN = {INT16: 2, INT32: 4, INT64: 8}
_INT_FMT = {INT8: "B", CHAR: "B", INT16: "H", INT32: "I", INT64: "Q"}

TAG_HEADERSIGNATURES = 62
TAG_HEADERIMMUTABLE = 63

# Registered types (rpmtag.h) of every main-header tag rpm_package emits.
TAG_TYPES = {
    100: STRING_ARRAY, 1000: STRING, 1001: STRING, 1002: STRING, 1004: I18NSTRING,
    1005: I18NSTRING, 1006: INT32, 1007: STRING, 1009: INT32, 1010: STRING,
    1011: STRING, 1014: STRING, 1015: STRING, 1016: I18NSTRING, 1020: STRING,
    1021: STRING, 1022: STRING, 1023: STRING, 1024: STRING, 1025: STRING,
    1026: STRING, 1028: INT32, 1030: INT16, 1033: INT16, 1034: INT32,
    1035: STRING_ARRAY, 1036: STRING_ARRAY, 1037: INT32, 1039: STRING_ARRAY,
    1040: STRING_ARRAY, 1044: STRING, 1045: INT32, 1047: STRING_ARRAY, 1048: INT32,
    1049: STRING_ARRAY, 1050: STRING_ARRAY, 1085: STRING_ARRAY, 1086: STRING_ARRAY,
    1087: STRING_ARRAY, 1088: STRING_ARRAY, 1095: INT32, 1096: INT32,
    1097: STRING_ARRAY, 1112: INT32, 1113: STRING_ARRAY, 1116: INT32,
    1117: STRING_ARRAY, 1118: STRING_ARRAY, 1124: STRING, 1125: STRING,
    1126: STRING, 5011: INT32, 5092: STRING_ARRAY, 5093: INT32,
}


def _encode_value(typ, value):
    if typ == STRING:
        if "\0" in value:
            raise RuntimeError("rpm STRING value contains NUL")
        return value.encode("utf-8") + b"\0", 1
    if typ in (STRING_ARRAY, I18NSTRING):
        if not value or any("\0" in v for v in value):
            raise RuntimeError("rpm string array is empty or contains NUL")
        return b"".join(v.encode("utf-8") + b"\0" for v in value), len(value)
    if typ == BIN:
        return bytes(value), len(value)
    if typ in _INT_FMT:
        if not value:
            raise RuntimeError("rpm integer array is empty")
        return struct.pack(f">{len(value)}{_INT_FMT[typ]}", *value), len(value)
    raise RuntimeError(f"unsupported rpm type {typ}")


def encode_header(entries, region_tag):
    """entries: {tag: (type, value)} -> header blob with an immutable region."""
    data = bytearray()
    index = []
    for tag in sorted(entries):
        typ, value = entries[tag]
        data += b"\0" * (-len(data) % _ALIGN.get(typ, 1))
        blob, count = _encode_value(typ, value)
        index.append((tag, typ, len(data), count))
        data += blob
    nindex = len(index) + 1
    trailer_offset = len(data)
    data += struct.pack(">iiii", region_tag, BIN, -nindex * 16, 16)
    index.insert(0, (region_tag, BIN, trailer_offset, 16))
    out = bytearray(HEADER_MAGIC + struct.pack(">II", nindex, len(data)))
    for tag, typ, offset, count in index:
        out += struct.pack(">IIiI", tag, typ, offset, count)
    return bytes(out + data)


def _decode_value(typ, store, offset, count, limit):
    """Returns (value, byte length) or raises when the data overruns `limit`."""
    if typ in (STRING, STRING_ARRAY, I18NSTRING):
        n = 1 if typ == STRING else count
        if typ == STRING and count != 1:
            raise RuntimeError("rpm STRING entry with count != 1")
        values, pos = [], offset
        for _ in range(n):
            end = store.find(b"\0", pos, limit)
            if end < 0:
                raise RuntimeError("unterminated rpm string")
            values.append(store[pos:end].decode("utf-8"))
            pos = end + 1
        return (values[0] if typ == STRING else values), pos - offset
    if typ == BIN:
        length = count
        if offset + length > limit:
            raise RuntimeError("rpm BIN entry overruns data")
        return bytes(store[offset:offset + length]), length
    if typ in _INT_FMT:
        size = struct.calcsize(_INT_FMT[typ]) * count
        if offset + size > limit:
            raise RuntimeError("rpm integer entry overruns data")
        return list(struct.unpack_from(f">{count}{_INT_FMT[typ]}", store, offset)), size
    raise RuntimeError(f"unsupported rpm type {typ}")


def parse_header(blob, start, region_tag, typecheck):
    """Parse and validate the header at blob[start:].

    Returns ({tag: (type, value)}, total header length)."""
    if blob[start:start + 8] != HEADER_MAGIC:
        raise RuntimeError(f"bad rpm header magic at offset {start}")
    il, dl = struct.unpack_from(">II", blob, start + 8)
    if il < 2 or il > 0xFFFF or dl > 256 * 1024 * 1024:
        raise RuntimeError(f"implausible rpm header size il={il} dl={dl}")
    index_start = start + 16
    store_start = index_start + il * 16
    if store_start + dl > len(blob):
        raise RuntimeError("rpm header data store overruns the file")
    store = blob[store_start:store_start + dl]
    entries = [struct.unpack_from(">IIiI", blob, index_start + 16 * i) for i in range(il)]

    rtag, rtype, roffset, rcount = entries[0]
    if (rtag, rtype, rcount) != (region_tag, BIN, 16):
        raise RuntimeError(f"rpm header region entry is {entries[0]}, expected tag {region_tag}")
    if roffset != dl - 16:
        raise RuntimeError("rpm region trailer is not the last item of the data store")
    ttag, ttype, toffset, tcount = struct.unpack_from(">iiii", store, roffset)
    if (ttag, ttype, tcount) != (region_tag, BIN, 16) or toffset != -il * 16:
        raise RuntimeError("rpm region trailer does not cover every index entry")

    tags = {}
    end = 0
    for tag, typ, offset, count in entries[1:]:
        if offset < end:
            raise RuntimeError(f"rpm tag {tag}: data overlaps the previous entry")
        if offset % _ALIGN.get(typ, 1):
            raise RuntimeError(f"rpm tag {tag}: misaligned for type {typ}")
        if typecheck and tag in TAG_TYPES and TAG_TYPES[tag] != typ:
            raise RuntimeError(f"rpm tag {tag}: type {typ} != registered {TAG_TYPES[tag]}")
        if tag in tags:
            raise RuntimeError(f"rpm tag {tag} appears twice")
        if count == 0:
            raise RuntimeError(f"rpm tag {tag}: zero count")
        value, length = _decode_value(typ, store, offset, count, roffset)
        end = offset + length
        tags[tag] = (typ, value)
    return tags, 16 + il * 16 + dl
