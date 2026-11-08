"""SBIN v3 writer for append-only edits (Firemonkeys IronMonkey .sb), used by make_obb_overlay.py.

The file is kept byte for byte: new strings go to the end of CHDR/CDAT, new objects to the end of DATA/OHDR, and
existing objects are only changed in place where a field keeps its size (an object reference, a number). Section
hashes are FNV-1 of the section body (the loader, libapp 0x4F457C, rejects a mismatch), sections are 4-byte
aligned. Layout of the parts this writer produces, as the original encoder lays them out (checked over all 185 .sb
files of the published OBB):
  OHDR[i]    = (offset << 3) | kind; offsets ascend with the index
  kind 0     typed struct: u16 struct index + body (STRU/FIEL layout), at offset = 2 (mod 4)
  kind 1     map: u16 count, u16 size, then per member {u16 key, u16 type, u32 value offset} followed by the value;
             4-byte values (i32, f32, object/array reference) start 4-aligned, others right after the member;
             size = end of the last value; at offset = 0 (mod 4)
  kind 2     array: u32 element type | struct index << 16, u32 count, elements (struct and 4-byte elements
             4-aligned); at offset = 0 (mod 4)
  CDAT       NUL-terminated strings back to back; CHDR {u32 offset, u32 length} per string
Value types: 2 u8, 5 i32, 9 bool, 10 f32, 12 char16 (locales.sb separators), 13 string, 15 object reference, 16 inline struct, 17 array reference,
18 enum (i32), 20 string (symbol). Gaps are zero bytes.
"""
from __future__ import annotations

import struct

SIZES = {1: 1, 2: 1, 3: 2, 4: 2, 5: 4, 6: 4, 7: 8, 8: 8, 9: 1, 10: 4, 11: 8, 12: 2, 13: 2, 14: 4, 15: 4, 17: 4, 18: 4, 20: 2}
PACK = {1: "<b", 2: "<B", 3: "<h", 4: "<H", 5: "<i", 6: "<I", 9: "<B", 10: "<f", 12: "<H", 14: "<I", 15: "<I", 17: "<I", 18: "<i"}
NULL = 0xFFFFFFFF
ORDER = ("ENUM", "STRU", "FIEL", "OHDR", "DATA", "CHDR", "CDAT")


def fnv1(data: bytes) -> int:
    h = 0x811C9DC5
    for b in data:
        h = ((h * 0x01000193) & 0xFFFFFFFF) ^ b
    return h


class Sbin:
    def __init__(self, raw: bytes):
        if raw[:4] != b"SBIN":
            raise ValueError("not SBIN")
        self.head = raw[:8]
        self.tags: list[str] = []
        secs: dict[str, bytes] = {}
        off = 8
        while off + 12 <= len(raw):
            tag = raw[off:off + 4].decode("latin1")
            size, digest = struct.unpack_from("<II", raw, off + 4)
            body = raw[off + 12:off + 12 + size]
            if fnv1(body) != digest:
                raise ValueError(f"{tag}: section hash mismatch")
            secs[tag] = body
            self.tags.append(tag)
            off = (off + 12 + size + 3) & ~3
        if off != len(raw) and (len(raw) - off) >= 4:
            raise ValueError("trailing data after the last section")
        if tuple(self.tags) != ORDER:
            raise ValueError(f"unexpected section order {self.tags}")
        self.enum = secs["ENUM"]
        st = secs["STRU"]
        self.structs = [struct.unpack_from("<HHH", st, i) for i in range(0, len(st) - len(st) % 6, 6)]
        fi = secs["FIEL"]
        self.fields = [struct.unpack_from("<HHHH", fi, i) for i in range(0, len(fi) - len(fi) % 8, 8)]
        self.stru_raw, self.fiel_raw = st, fi
        oh = secs["OHDR"]
        self.ohdr = list(struct.unpack_from("<%dI" % (len(oh) // 4), oh))
        self.data = bytearray(secs["DATA"])
        ch, cd = secs["CHDR"], secs["CDAT"]
        self.strings = [cd[o:o + n].decode("utf-8") for o, n in struct.iter_unpack("<II", ch)]
        self.index = {}
        for i, s in enumerate(self.strings):
            self.index.setdefault(s, i)
        self._size_cache: dict[int, int] = {}

    # --- strings and schema -----------------------------------------------------------------------------------
    def s(self, i: int) -> str:
        return self.strings[i]

    def sid(self, text: str) -> int:
        """Index of a string, appended when the table does not have it yet."""
        i = self.index.get(text)
        if i is None:
            i = len(self.strings)
            if i > 0xFFFF:
                raise ValueError("string table full")
            self.strings.append(text)
            self.index[text] = i
        return i

    def struct_index(self, name: str) -> int:
        for i, (n, _, _) in enumerate(self.structs):
            if self.s(n) == name:
                return i
        raise KeyError(name)

    def struct_fields(self, si: int) -> list[tuple[str, int, int, int]]:
        _, first, count = self.structs[si]
        return [(self.s(self.fields[f][0]), self.fields[f][1], self.fields[f][2], self.fields[f][3])
                for f in range(first, first + count)]

    def struct_size(self, si: int) -> int:
        if si in self._size_cache:
            return self._size_cache[si]
        end, align = 0, 1
        for _, ftype, foff, extra in self.struct_fields(si):
            size = self.struct_size(extra) if ftype == 16 else SIZES.get(ftype, 4)
            a = 4 if ftype == 16 else min(4, SIZES.get(ftype, 4))
            align = max(align, a)
            end = max(end, foff + size)
        end = (end + align - 1) // align * align
        self._size_cache[si] = end
        return end

    # --- reading ----------------------------------------------------------------------------------------------
    def obj(self, i: int) -> tuple[int, int]:
        e = self.ohdr[i]
        return e >> 3, e & 7

    def map_members(self, i: int) -> list[tuple[str, int, int]]:
        """[(key, type, absolute DATA offset of the value)] of map object i."""
        off, kind = self.obj(i)
        if kind != 1:
            raise ValueError(f"object {i} is kind {kind}, not a map")
        count, _ = struct.unpack_from("<HH", self.data, off)
        pos, out = off + 4, []
        for _ in range(count):
            key, typ, voff = struct.unpack_from("<HHI", self.data, pos)
            out.append((self.s(key), typ, off + voff))
            pos = (off + voff + SIZES.get(typ, 4) + 1) & ~1
        return out

    def map_get(self, i: int, key: str):
        for k, typ, at in self.map_members(i):
            if k == key:
                return self.read(typ, at)
        raise KeyError(key)

    def read(self, typ: int, at: int):
        if typ in (13, 20):
            return self.s(struct.unpack_from("<H", self.data, at)[0])
        if typ in PACK:
            v = struct.unpack_from(PACK[typ], self.data, at)[0]
            return bool(v) if typ == 9 else v
        raise ValueError(f"cannot read type {typ}")

    def array(self, i: int) -> tuple[int, int, int, int, int]:
        """(element type, struct index, count, DATA offset of element 0, element size) of array object i."""
        off, kind = self.obj(i)
        if kind != 2:
            raise ValueError(f"object {i} is kind {kind}, not an array")
        et, count = struct.unpack_from("<II", self.data, off)
        typ, si = et & 0xFFFF, et >> 16
        if typ == 16:
            size, base = self.struct_size(si), (off + 8 + 3) & ~3
        else:
            size, base = SIZES.get(typ, 4), off + 8
            if size >= 4:
                base = (base + 3) & ~3
        return typ, si, count, base, size

    def array_items(self, i: int) -> list:
        typ, _, count, base, size = self.array(i)
        if typ == 16:
            return [bytes(self.data[base + k * size:base + (k + 1) * size]) for k in range(count)]
        return [self.read(typ, base + k * size) for k in range(count)]

    def typed(self, i: int) -> tuple[int, int]:
        """(struct index, DATA offset of the body) of typed object i."""
        off, kind = self.obj(i)
        if kind != 0:
            raise ValueError(f"object {i} is kind {kind}, not a typed struct")
        return struct.unpack_from("<H", self.data, off)[0], off + 2

    def typed_name(self, i: int) -> str:
        return self.s(self.structs[self.typed(i)[0]][0])

    def decode_struct(self, si: int, at: int) -> dict:
        """Field values of the struct at DATA offset at (inline structs nested, references as indices)."""
        out = {}
        for name, ftype, foff, extra in self.struct_fields(si):
            out[name] = self.decode_struct(extra, at + foff) if ftype == 16 else self.read(ftype, at + foff)
        return out

    def field(self, si: int, at: int, path: tuple[str, ...]) -> tuple[int, int]:
        """(DATA offset, type) of a possibly nested field, e.g. ("rect", "top_left", "y")."""
        for depth, name in enumerate(path):
            for fname, ftype, foff, extra in self.struct_fields(si):
                if fname == name:
                    if depth == len(path) - 1:
                        return at + foff, ftype
                    if ftype != 16:
                        raise KeyError(f"{name} is not an inline struct")
                    si, at = extra, at + foff
                    break
            else:
                raise KeyError(name)
        raise KeyError(path)

    def get_field(self, i: int, *path: str):
        si, body = self.typed(i)
        at, typ = self.field(si, body, path)
        return self.read(typ, at)

    def set_field(self, i: int, *path_and_value) -> None:
        *path, value = path_and_value
        si, body = self.typed(i)
        at, typ = self.field(si, body, tuple(path))
        self.patch(typ, at, value)

    def _refs_in_struct(self, si: int, at: int) -> list[int]:
        """DATA offsets of the object/array references inside a struct (nested inline structs included)."""
        out = []
        for _, ftype, foff, extra in self.struct_fields(si):
            if ftype == 16:
                out.extend(self._refs_in_struct(extra, at + foff))
            elif ftype in (15, 17):
                out.append(at + foff)
        return out

    def clone(self, i: int) -> int:
        """Deep copy of object i and everything it references (pre-order, like the original encoder); returns the
        index of the copy. Strings are shared table entries; NULL references stay NULL."""
        off, kind = self.obj(i)
        if kind == 0:
            si, body = self.typed(i)
            payload = bytes(self.data[off:body + self.struct_size(si)])
            j = self._append(0, payload)
            nbody = (self.ohdr[j] >> 3) + 2
            slots = [(nbody + (at - body)) for at in self._refs_in_struct(si, body)]
        elif kind == 1:
            members = self.map_members(i)
            count, size = struct.unpack_from("<HH", self.data, off)
            j = self._append(1, bytes(self.data[off:off + size]))
            noff = self.ohdr[j] >> 3
            slots = [noff + (at - off) for _, typ, at in members if typ in (15, 17)]
        else:
            typ, si, count, base, size = self.array(i)
            j = self._append(2, bytes(self.data[off:base + count * size]))
            noff = self.ohdr[j] >> 3
            if typ in (15, 17):
                slots = [noff + (base - off) + k * size for k in range(count)]
            elif typ == 16:
                slots = []
                for k in range(count):
                    eat = base + k * size
                    slots.extend(noff + (at - off) for at in self._refs_in_struct(si, eat))
            else:
                slots = []
        for slot in slots:
            ref = struct.unpack_from("<I", self.data, slot)[0]
            if ref != NULL:
                struct.pack_into("<I", self.data, slot, self.clone(ref))
        return j

    # --- encoding ---------------------------------------------------------------------------------------------
    def encode_value(self, typ: int, value) -> bytes:
        if typ in (13, 20):
            return struct.pack("<H", self.sid(value))
        if typ == 9:
            return struct.pack("<B", 1 if value else 0)
        if typ in PACK:
            return struct.pack(PACK[typ], value)
        raise ValueError(f"cannot encode type {typ}")

    def encode_struct(self, si: int, values: dict) -> bytes:
        body = bytearray(self.struct_size(si))
        names = set()
        for name, ftype, foff, extra in self.struct_fields(si):
            names.add(name)
            if name not in values:
                raise KeyError(f"{self.s(self.structs[si][0])}.{name} missing")
            v = values[name]
            raw = self.encode_struct(extra, v) if ftype == 16 else self.encode_value(ftype, v)
            body[foff:foff + len(raw)] = raw
        extra_keys = set(values) - names
        if extra_keys:
            raise KeyError(f"unknown fields {sorted(extra_keys)}")
        return bytes(body)

    def _append(self, kind: int, payload: bytes) -> int:
        off = len(self.data)
        want = 2 if kind == 0 else 0
        while off % 4 != want:
            off += 1
        self.data.extend(b"\0" * (off - len(self.data)))
        self.data.extend(payload)
        if self.ohdr and (self.ohdr[-1] >> 3) >= off:
            raise ValueError("object offsets must ascend")
        self.ohdr.append((off << 3) | kind)
        return len(self.ohdr) - 1

    def add_typed(self, struct_name: str, values: dict) -> int:
        si = self.struct_index(struct_name)
        return self._append(0, struct.pack("<H", si) + self.encode_struct(si, values))

    def add_map(self, members: list[tuple[str, int, object]]) -> int:
        body = bytearray(4)
        for key, typ, value in members:
            raw = self.encode_value(typ, value)
            head = len(body)
            vpos = head + 8
            if SIZES.get(typ, 4) == 4:
                vpos = (vpos + 3) & ~3
            body.extend(struct.pack("<HHI", self.sid(key), typ, vpos))
            body.extend(b"\0" * (vpos - len(body)))
            body.extend(raw)
            if len(body) % 2:
                body.append(0)
        size = len(body)
        last = members[-1] if members else None
        if last is not None and SIZES.get(last[1], 4) == 1:
            size -= 1  # the size field ends at the last value byte; the pad byte is a gap
        struct.pack_into("<HH", body, 0, len(members), size)
        return self._append(1, bytes(body[:size]))

    def add_array(self, typ: int, items: list, struct_name: str | None = None) -> int:
        si = self.struct_index(struct_name) if struct_name else 0
        body = bytearray(struct.pack("<II", typ | (si << 16), len(items)))
        for item in items:
            if typ == 16:
                body.extend(item if isinstance(item, (bytes, bytearray)) else self.encode_struct(si, item))
            else:
                body.extend(self.encode_value(typ, item))
        return self._append(2, bytes(body))

    def patch(self, typ: int, at: int, value) -> None:
        raw = self.encode_value(typ, value)
        self.data[at:at + len(raw)] = raw

    def set_member(self, i: int, key: str, value) -> None:
        for k, typ, at in self.map_members(i):
            if k == key:
                self.patch(typ, at, value)
                return
        raise KeyError(key)

    def grow_last_array(self, i: int, items: list) -> None:
        """Appends elements to array object i in place; it has to be the last object of DATA."""
        if i != len(self.ohdr) - 1:
            raise ValueError("only the last object can grow in place")
        typ, si, count, base, size = self.array(i)
        if base + count * size != len(self.data):
            raise ValueError("array does not end DATA")
        for item in items:
            self.data.extend(item if typ == 16 and isinstance(item, (bytes, bytearray))
                             else self.encode_struct(si, item) if typ == 16 else self.encode_value(typ, item))
        off, _ = self.obj(i)
        struct.pack_into("<I", self.data, off + 4, count + len(items))

    def encode(self) -> bytes:
        cdat = bytearray()
        chdr = bytearray()
        for text in self.strings:
            raw = text.encode("utf-8")
            chdr.extend(struct.pack("<II", len(cdat), len(raw)))
            cdat.extend(raw + b"\0")
        bodies = {"ENUM": self.enum, "STRU": self.stru_raw, "FIEL": self.fiel_raw,
                  "OHDR": struct.pack("<%dI" % len(self.ohdr), *self.ohdr), "DATA": bytes(self.data),
                  "CHDR": bytes(chdr), "CDAT": bytes(cdat)}
        out = bytearray(self.head)
        for tag in ORDER:
            body = bodies[tag]
            out.extend(tag.encode("latin1") + struct.pack("<II", len(body), fnv1(body)) + body)
            if tag != ORDER[-1]:
                while len(out) % 4:
                    out.append(0)
        return bytes(out)
