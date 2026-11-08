#!/usr/bin/env python3
"""Stage 8 data additions as an OBB overlay (GU-07, GU-08, HC-06), without touching the OBB container.

The OBB stays byte-identical (OBB_UNPACK_POLICY=KEEP_CONTAINER). The bridge's NfsVfs serves the game a virtual zip:
the original OBB bytes up to its central directory, then an in-memory tail with the entries listed in
assets/obb_overlay.txt (read from assets/obb_overlay/<entry>, STORED, CRC-32), a rewritten central directory in
which those entries replace the originals, and a new end record (arm64_bridge/src/thunks_libc.cpp).

This tool writes the overlay entries into app/assets/obb_overlay/ and the manifest:
  GU-07  published/tweaks/lod_tweaks.sb   LODFourDistance 25 -> 50, LODFiveDistance 50 -> 100, LODTrafficDistance 20 -> 40
         published/tweaks/race_tweaks.sb  CarsMaxPrimitives 6000 -> 12000
         published/tweaks/tweaks.sb       ShadowCullDistance 125 -> 250
  GU-08  published/tweaks/tweaks.sb       CarHemisphereHighTierTextureSize 512 -> 1024
  HC-06  published/arguments.ini          skip_tutorial=true (new careers skip the tutorial race; read by
                                          App_Init_LoadArgumentsIni 0x75E40, INI tokenizer 0x3988A4); removed again in
                                          stage 12 (the skip left a new career without the tutorial's car, see ADDED)
  stage 9 published/tweaks/debug_options.sb EnableDebugBlackMarketCycle 0 -> 1, DebugBlackMarketCycleTime 120 -> 86400:
                                          the NFS Edition car at the dealer changes daily instead of weekly. The rotation
                                          (BlackMarket_CurrentNfsEditionCar 0x2550C8) and the cycle getter of the
                                          countdown (0x255010) both use this pair instead of 604800 s when it is enabled.
  stage 10 published/layouts/layouts.sb   the frame rate limit row (30 / 45 / 60 / display maximum) in the game's own
                                          settings pages; stage 13 moved it into a GRAPHICS frame with the graphics
                                          quality, the render scale and the nitrous motion blur, built by
                                          work/tools/settings_ui.py (append-only SBIN edits, work/tools/sbinw.py); the
                                          options live in the bridge (namespace portopts). Until stage 12 the rows'
                                          strings were added to the 11 string tables of the OBB (overlay entries
                                          published/stringdata/<language>/nfsmw_android.sb); since stage 13 they are
                                          assets/port_strings.txt, added by the bridge to the table the engine loads
                                          (portstrings.cpp), so a modified cache keeps its own tables
  stage 11 published/flow/update_check.sb the AAS output goes to /tutorial_check with the SKIP transition, so the first
                                          start skips the agreement screen (AAScreen, layout active_accept, UI_AAS_TEXT),
                                          which blocked the first start on Android TV (user report). UpdateCheck
                                          (OnEnter 0x1A1C54) sends AAS while <data root>/active_accepted is missing (check
                                          0x1A38B0, its only caller) and SKIP after the screen wrote it. AAScreen keeps the
                                          base screen input (vtable 0xAA0740 slots 5 and 18), unlike UserAgreementScreen
                                          with its button focus list (0x1A23A0) and D-pad handler (0x1A28B8). The User
                                          Agreement and Privacy links stay in the game's options
Tweak values are patched in place in the SBIN DATA section (old value checked) and the section's FNV-1 hash is
recomputed; the result is decoded again and compared. Source: original/<obb> (read only).

Manifest lines (stage 13): "<entry><TAB><CRC-32 of the original entry, hex>[<TAB>max]". The bridge applies an entry
only over a container entry with that CRC (a modified cache keeps its own file: the OBB of a mod that retunes
tweaks.sb keeps its values), and a "max" entry only at the MAX graphics quality (GU-07 and GU-08 are part of the port's
graphics improvements; tiers 1-4 keep the engine's own distances and texture sizes).

Usage: python -X utf8 -B work/tools/make_obb_overlay.py
"""
from __future__ import annotations

import hashlib
import json
import struct
import sys
import zipfile
from pathlib import Path

GAME = Path(__file__).resolve().parents[2]
OBB = GAME / "original" / "main.1003128.com.ea.games.nfs13_row.obb"
OBB_CRC = 0x2E46CC80
OUT = GAME / "app" / "assets" / "obb_overlay"
MANIFEST = GAME / "app" / "assets" / "obb_overlay.txt"
PORT_STRINGS = GAME / "app" / "assets" / "port_strings.txt"
sys.path.insert(0, str(GAME / "work" / "research" / "content"))
import sbin  # noqa: E402
import sbinw  # noqa: E402  (work/tools, next to this file)
import settings_ui  # noqa: E402  (work/tools)


def skip_active_accept(raw: bytes) -> bytes:
    """update_check.sb with AAS -> /tutorial_check and the transition of SKIP (a copy), checked by decoding."""
    sb = sbinw.Sbin(raw)
    outputs = {sb.map_get(i, "output"): i for i in sb.array_items(sb.map_get(0, "Outputs"))}
    aas, skip = outputs["AAS"], outputs["SKIP"]
    if (sb.map_get(aas, "node"), sb.map_get(skip, "node")) != ("/active_accept", "/tutorial_check"):
        raise SystemExit("update_check.sb: AAS/SKIP outputs differ from the original")
    sb.set_member(aas, "node", "/tutorial_check")
    sb.set_member(aas, "Transition", sb.clone(sb.map_get(skip, "Transition")))
    out = sb.encode()
    want = sbin.SB(raw).root()
    rows = {row["output"]: row for row in want["Outputs"]}
    rows["AAS"].update(node=rows["SKIP"]["node"], Transition=rows["SKIP"]["Transition"])
    if sbin.SB(out).root() != want:
        raise SystemExit("update_check.sb: decoded result differs from the expected flow")
    return out


EDITS = {
    "published/tweaks/lod_tweaks.sb": {"LODFourDistance": (25.0, 50.0), "LODFiveDistance": (50.0, 100.0),
                                       "LODTrafficDistance": (20.0, 40.0)},
    "published/tweaks/race_tweaks.sb": {"CarsMaxPrimitives": (6000, 12000)},
    "published/tweaks/tweaks.sb": {"ShadowCullDistance": (125.0, 250.0), "CarHemisphereHighTierTextureSize": (512, 1024)},
    "published/tweaks/debug_options.sb": {"EnableDebugBlackMarketCycle": (0, 1), "DebugBlackMarketCycleTime": (120, 86400)},
}
# HC-06 (published/arguments.ini skip_tutorial=true) was removed in stage 12: the skip set the tutorial progress to 5
# (TutorialCheck 0x1A16F0 → 0x2648A8) and went to the map, so a new career had no car (the tutorial branch creates the
# $25,000 Dodge Challenger SRT8 392 and makes it the current car) and only InitialCash 12,000, without the two tutorial
# events (event_01_race 10,000/8,000/7,000, event_02_timetrial 10,500/8,500/7,000).
ADDED: dict[str, bytes] = {}
BUILT = {"published/layouts/layouts.sb": ("settings_ui", settings_ui.build_layouts),
         "published/flow/update_check.sb": ("skip_active_accept", skip_active_accept)}
# entries that belong to the MAX graphics quality (GU-07, GU-08)
MAX_ONLY = {"published/tweaks/lod_tweaks.sb", "published/tweaks/race_tweaks.sb", "published/tweaks/tweaks.sb"}
PACK = {5: "<i", 6: "<I", 10: "<f", 9: "<B"}


def fnv1(data: bytes) -> int:
    h = 0x811C9DC5
    for b in data:
        h = ((h * 0x01000193) & 0xFFFFFFFF) ^ b
    return h


def sections(data: bytes) -> dict:
    out, off = {}, 8
    while off + 12 <= len(data):
        tag = data[off:off + 4]
        if not tag.isalpha():
            off = (off + 3) & ~3
            continue
        size, _ = struct.unpack_from("<II", data, off + 4)
        out[tag.decode()] = (off, size)
        off += 12 + size
    return out


def value_offset(sb: "sbin.SB", name: str) -> tuple[int, int]:
    """DATA offset and type of <root map>[name].Value."""
    root = sb.ohdr[0]
    off, kind = root >> 3, root & 7
    if kind != 1:
        raise SystemExit("tweak root is not a map")
    count, _ = struct.unpack_from("<HH", sb.D, off)
    pos = off + 4
    for _ in range(count):
        key, typ, voff = struct.unpack_from("<HHI", sb.D, pos)
        pos = (off + voff + sbin.SIZES.get(typ, 4) + 1) & ~1
        if sb.s(key) != name:
            continue
        if typ != 15:
            raise SystemExit(f"{name}: member type {typ}, expected an object reference")
        idx = struct.unpack_from("<I", sb.D, off + voff)[0]
        entry = sb.ohdr[idx]
        ooff, okind = entry >> 3, entry & 7
        if okind == 0:  # typed struct: u16 struct index + body
            si = struct.unpack_from("<H", sb.D, ooff)[0]
            _, first, nfields = sb.structs[si]
            for f in range(first, first + nfields):
                fname, ftype, fo, _ = sb.fields[f]
                if sb.s(fname) == "Value":
                    return ooff + 2 + fo, ftype
        elif okind == 1:  # untyped map: u16 count, u16 size, members {u16 key, u16 type, u32 offset}
            mcount, _ = struct.unpack_from("<HH", sb.D, ooff)
            mpos = ooff + 4
            for _ in range(mcount):
                mkey, mtype, moff = struct.unpack_from("<HHI", sb.D, mpos)
                mpos = (ooff + moff + sbin.SIZES.get(mtype, 4) + 1) & ~1
                if sb.s(mkey) == "Value":
                    return ooff + moff, mtype
        raise SystemExit(f"{name}: no Value member (object kind {okind})")
    raise SystemExit(f"{name}: not in the tweak file")


def patch_tweaks(raw: bytes, edits: dict) -> bytes:
    data = bytearray(raw)
    secs = sections(raw)
    doff, dsize = secs["DATA"]
    if fnv1(raw[doff + 12:doff + 12 + dsize]) != struct.unpack_from("<I", raw, doff + 8)[0]:
        raise SystemExit("DATA hash is not FNV-1: format differs")
    sb = sbin.SB(raw)
    for name, (old, new) in edits.items():
        rel, typ = value_offset(sb, name)
        fmt = PACK[typ]
        cur = struct.unpack_from(fmt, raw, doff + 12 + rel)[0]
        if (round(cur, 4) if typ == 10 else cur) != old:
            raise SystemExit(f"{name}: preimage {cur} != {old}")
        struct.pack_into(fmt, data, doff + 12 + rel, new)
    struct.pack_into("<I", data, doff + 8, fnv1(bytes(data[doff + 12:doff + 12 + dsize])))
    check = sbin.SB(bytes(data)).root()
    for name, (_, new) in edits.items():
        got = check[name]["Value"]
        if (round(got, 4) if isinstance(got, float) else got) != new:
            raise SystemExit(f"{name}: decoded {got} after patch, expected {new}")
    return bytes(data)


def main() -> int:
    z = zipfile.ZipFile(OBB)
    crc = 0
    import zlib
    with open(OBB, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 22), b""):
            crc = zlib.crc32(chunk, crc)
    if crc != OBB_CRC:
        raise SystemExit(f"{OBB} CRC {crc:08x} != original {OBB_CRC:08x}")
    names = set(z.namelist())
    summary = {}
    if MAX_ONLY - set(EDITS):
        raise SystemExit(f"MAX_ONLY names entries this tool does not write: {sorted(MAX_ONLY - set(EDITS))}")
    OUT.mkdir(parents=True, exist_ok=True)
    entries = []
    for entry, edits in EDITS.items():
        raw = z.read(entry)
        out = patch_tweaks(raw, edits)
        dest = OUT / entry
        dest.parent.mkdir(parents=True, exist_ok=True)
        dest.write_bytes(out)
        entries.append(entry)
        summary[entry] = {"original_sha256": hashlib.sha256(raw).hexdigest(), "sha256": hashlib.sha256(out).hexdigest(),
                          "edits": {k: list(v) for k, v in edits.items()}}
    for entry, (label, build) in BUILT.items():
        raw = z.read(entry)
        out = build(raw)
        dest = OUT / entry
        dest.parent.mkdir(parents=True, exist_ok=True)
        dest.write_bytes(out)
        entries.append(entry)
        summary[entry] = {"original_sha256": hashlib.sha256(raw).hexdigest(), "sha256": hashlib.sha256(out).hexdigest(),
                          "bytes": len(out), "built": label}
    for entry, body in ADDED.items():
        if entry in names:
            raise SystemExit(f"{entry} exists in the OBB; this tool only adds it")
        dest = OUT / entry
        dest.parent.mkdir(parents=True, exist_ok=True)
        dest.write_bytes(body)
        entries.append(entry)
        summary[entry] = {"sha256": hashlib.sha256(body).hexdigest(), "added": True}
    lines = []
    for e in entries:
        base = f"{z.getinfo(e).CRC:08x}" if e in names else "-"
        lines.append(e + "\t" + base + ("\tmax" if e in MAX_ONLY else "") + "\n")
    MANIFEST.write_text("".join(lines), encoding="utf-8", newline="\n")
    strings = settings_ui.port_strings()
    PORT_STRINGS.write_text(strings, encoding="utf-8", newline="\n")
    summary["port_strings"] = {"path": str(PORT_STRINGS.relative_to(GAME)), "lines": strings.count("\n"),
                               "sha256": hashlib.sha256(strings.encode("utf-8")).hexdigest()}
    stale = sorted(p for p in OUT.rglob("*") if p.is_file() and p.relative_to(OUT).as_posix() not in entries)
    for p in stale:  # entries dropped from the lists above (the APK packs everything under OUT)
        p.unlink()
    for d in sorted((p for p in OUT.rglob("*") if p.is_dir()), key=lambda p: len(p.parts), reverse=True):
        if not any(d.iterdir()):
            d.rmdir()
    summary["manifest"] = {"path": str(MANIFEST.relative_to(GAME)), "lines": [line.rstrip("\n") for line in lines],
                           "removed": [p.relative_to(OUT).as_posix() for p in stale]}
    print(json.dumps(summary, indent=1))
    return 0


if __name__ == "__main__":
    sys.exit(main())
