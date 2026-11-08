#!/usr/bin/env python3
"""Phase 7: canonical app/ tree in the arm64 bridge layout (the ARM32 tree is archived in
out/snapshots/arm32_ready_final.zip and out/ARM32_LASTGOOD).

  app/lib/armeabi-v7a/{libapp,libfmodex,libfmodevent,libc++_shared,libNimble}.so
        -> app/assets/guest_libs/ (the same bytes, now ARM32 guests read from the APK)
  app/lib/armeabi-v7a/libnfsport.so -> removed (both halves, NfsVfs and the QA actions, are bridge host
        code; the QA half was the ARM32 guest libnfsqa.so until stage 9)
  arm64_bridge/dist/libapp.so -> app/lib/arm64-v8a/libapp.so (ELF64 AArch64, every PT_LOAD p_align >= 16 KiB)
  arm64_bridge/dist/libapp_legacy.so -> app/lib/arm64-v8a/libapp_legacy.so (stage 13: the same bridge built for
        API 26 with emulated TLS; NfsNative loads it below Android 10, libapp.so is the API 29 build with ELF TLS)
  apktool.yml doNotCompress += so (guest ELFs are mmapped from the APK; the host lib stays page-aligned)

Idempotent; every step checks its preimage. Prints a JSON summary with SHA-256 of the results."""
from __future__ import annotations

import hashlib
import json
import os
import shutil
import subprocess
import sys
from pathlib import Path

GAME = Path(__file__).resolve().parents[2]
APP = GAME / "app"
ARM32 = APP / "lib" / "armeabi-v7a"
GUESTS = APP / "assets" / "guest_libs"
HOST = APP / "lib" / "arm64-v8a" / "libapp.so"
DIST = GAME / "arm64_bridge" / "dist" / "libapp.so"
HOST_LEGACY = APP / "lib" / "arm64-v8a" / "libapp_legacy.so"
DIST_LEGACY = GAME / "arm64_bridge" / "dist" / "libapp_legacy.so"
YML = APP / "apktool.yml"
GUEST_NAMES = ["libapp.so", "libfmodex.so", "libfmodevent.so", "libc++_shared.so", "libNimble.so"]
SDK = Path(os.environ.get("ANDROID_SDK_ROOT", str(Path.home() / "AppData/Local/Android/Sdk")))
READELF = SDK / "ndk/27.2.12479018/toolchains/llvm/prebuilt/windows-x86_64/bin/llvm-readelf.exe"


def sha(p: Path) -> str:
    return hashlib.sha256(p.read_bytes()).hexdigest()


def check_host(lib: Path) -> None:
    header = lib.read_bytes()[:20]
    if header[:4] != b"\x7fELF" or header[4] != 2 or int.from_bytes(header[18:20], "little") != 183:
        raise SystemExit(f"{lib} is not an ELF64 AArch64 shared object")
    out = subprocess.run([str(READELF), "-lW", str(lib)], capture_output=True, text=True, check=True).stdout
    aligns = [int(line.split()[-1], 16) for line in out.splitlines() if line.strip().startswith("LOAD")]
    if not aligns or min(aligns) < 0x4000:
        raise SystemExit(f"{lib}: PT_LOAD alignment {aligns} is below 16 KiB")


def main() -> int:
    changes = []
    GUESTS.mkdir(parents=True, exist_ok=True)
    for name in GUEST_NAMES:
        source, target = ARM32 / name, GUESTS / name
        if target.is_file() and not source.exists():
            continue
        if not source.is_file() or target.exists():
            raise SystemExit(f"preimage mismatch: {source} -> {target}")
        shutil.move(str(source), str(target))
        changes.append(f"moved {name} to assets/guest_libs")
    nfsport = ARM32 / "libnfsport.so"
    if nfsport.exists():
        nfsport.unlink()
        changes.append("removed lib/armeabi-v7a/libnfsport.so")
    if ARM32.exists():
        leftovers = sorted(p.name for p in ARM32.iterdir())
        if leftovers:
            raise SystemExit(f"unexpected files left in {ARM32}: {leftovers}")
        ARM32.rmdir()

    for dist, host in ((DIST, HOST), (DIST_LEGACY, HOST_LEGACY)):
        check_host(dist)
        if not host.is_file() or sha(host) != sha(dist):
            host.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(dist, host)
            changes.append(f"installed arm64 bridge lib/arm64-v8a/{host.name}")

    text = YML.read_text(encoding="utf-8")
    anchor = "doNotCompress:\n"
    if "\n- so\n" not in text:
        if text.count(anchor) != 1:
            raise SystemExit("preimage mismatch: apktool.yml doNotCompress")
        YML.write_text(text.replace(anchor, anchor + "- so\n", 1), encoding="utf-8", newline="\n")
        changes.append("apktool.yml doNotCompress += so")

    summary = {
        "changes": changes,
        "host": {"path": str(HOST.relative_to(GAME)).replace("\\", "/"), "sha256": sha(HOST)},
        "host_legacy": {"path": str(HOST_LEGACY.relative_to(GAME)).replace("\\", "/"), "sha256": sha(HOST_LEGACY)},
        "guests": {p.name: sha(p) for p in sorted(GUESTS.glob("*.so"))},
    }
    print(json.dumps(summary, indent=1))
    return 0


if __name__ == "__main__":
    sys.exit(main())
