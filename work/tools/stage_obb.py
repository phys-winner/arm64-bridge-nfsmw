#!/usr/bin/env python3
"""Stage the whole main OBB container into the canonical app/ tree (no unpacking).

Copies SOURCE (default: the immutable original OBB) to
app/assets/obb/main.<versionCode>.<package>.obb and verifies SHA-256. The OBB itself is
Git-ignored; its hash is the provenance record (STATE.md / cache_embedding.md). There is no
size marker any more (stage 15): the port takes an embedded or external OBB of any size when it
is a whole zip (NfsObb), and the engine's assets/obb.size, which pinned the original size, is gone.

Usage: python -X utf8 -B work/tools/stage_obb.py [--source PATH] [--expect-sha256 HEX]
"""
from __future__ import annotations

import argparse
import hashlib
import json
import shutil
import sys
from pathlib import Path

GAME = Path(__file__).resolve().parents[2]
NAME = "main.1003128.com.ea.games.nfs13_row.obb"
ORIGINAL = GAME / "original" / NAME
ORIGINAL_SHA = "66dd4e695e698929f789e7c825eabe3ba5a50ed2ce28b628c96e5dbc008043a1"
DEST = GAME / "app" / "assets" / "obb" / NAME


def sha256(p: Path) -> str:
    h = hashlib.sha256()
    with open(p, "rb") as f:
        for chunk in iter(lambda: f.read(8 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--source", type=Path, default=ORIGINAL)
    ap.add_argument("--expect-sha256", default=None)
    args = ap.parse_args()
    src = args.source.resolve()
    expect = args.expect_sha256 or (ORIGINAL_SHA if src == ORIGINAL.resolve() else None)
    if not expect:
        raise SystemExit("--expect-sha256 is required for a non-original source")
    src_sha = sha256(src)
    if src_sha != expect:
        raise SystemExit(f"source hash mismatch: {src_sha} != {expect}")
    with open(src, "rb") as f:
        f.seek(-22, 2)
        if f.read(4) != b"PK\x05\x06":
            raise SystemExit("source has no plain ZIP EOCD in its last 22 bytes (comment present?)")
    DEST.parent.mkdir(parents=True, exist_ok=True)
    copied = False
    if not DEST.exists() or DEST.stat().st_size != src.stat().st_size or sha256(DEST) != src_sha:
        tmp = DEST.with_suffix(".obb.tmp")
        shutil.copyfile(src, tmp)
        if sha256(tmp) != src_sha:
            tmp.unlink()
            raise SystemExit("copy verification failed")
        tmp.replace(DEST)
        copied = True
    size = DEST.stat().st_size
    print(json.dumps({"source": str(src), "dest": str(DEST.relative_to(GAME)).replace("\\", "/"),
                      "sha256": src_sha, "size": size, "copied": copied}, indent=1))
    return 0


if __name__ == "__main__":
    sys.exit(main())
