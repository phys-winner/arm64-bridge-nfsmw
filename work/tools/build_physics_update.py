#!/usr/bin/env python3
"""Stage 8 frame-rate work (ST-3): assemble the replacement body of PhysicsSubSystem::Update (libapp 0x3AC1B8)
from work/native/physics_update/physics_update.S at its own address and write the patch spec
work/patches/libapp_st3_physics_update.json (preimage = the original bytes of the base libapp).

NDK r27c clang (armv7a, ARM state, softfp); lld places .text at 0x3AC1B8 and resolves the one call to the
unchanged callee sub_6866C0 as an absolute symbol. The body must fit the original function (0x178 bytes);
the original bytes after it stay (unreachable). No network, no device.

Usage: python -X utf8 -B work/tools/build_physics_update.py [--base app/assets/guest_libs/libapp.so]
Then apply with the skill's artifacts.py patch (the spec's "apply" line).
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import subprocess
import sys
from pathlib import Path

GAME = Path(__file__).resolve().parents[2]
NDK = Path(os.environ.get("LOCALAPPDATA", "")) / "Android" / "Sdk" / "ndk" / "27.2.12479018"
BIN = NDK / "toolchains" / "llvm" / "prebuilt" / "windows-x86_64" / "bin"
SRC = GAME / "work" / "native" / "physics_update" / "physics_update.S"
BUILD = GAME / "work" / "native" / "build" / "physics_update"
SPEC = GAME / "work" / "patches" / "libapp_st3_physics_update.json"
FUNC, FUNC_SIZE, CALLEE = 0x3AC1B8, 0x178, 0x6866C0
BASE_SHA = "550d64e7784a39f67b2ce4d65fa5a9b60d5c6377aba8c01f754f2d583388e720"


def run(cmd: list) -> str:
    cp = subprocess.run([str(c) for c in cmd], capture_output=True, text=True)
    if cp.returncode != 0:
        raise SystemExit(f"FAILED: {' '.join(map(str, cmd))}\n{cp.stdout}\n{cp.stderr}")
    return cp.stdout


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--base", type=Path, default=GAME / "app" / "assets" / "guest_libs" / "libapp.so")
    args = ap.parse_args()
    base = args.base.read_bytes()
    if hashlib.sha256(base).hexdigest() != BASE_SHA:
        raise SystemExit(f"base libapp is not {BASE_SHA[:16]}...: {args.base}")
    BUILD.mkdir(parents=True, exist_ok=True)
    elf, raw = BUILD / "physics_update.elf", BUILD / "physics_update.bin"
    run([BIN / "clang.exe", "--target=armv7a-linux-androideabi21", "-marm", "-mfloat-abi=softfp", "-mfpu=vfpv3-d16",
         "-nostdlib", "-static", "-fuse-ld=lld", f"-Wl,-Ttext={FUNC:#x}", f"-Wl,--defsym=sub_6866C0={CALLEE:#x}",
         "-Wl,-e,physics_update", "-Wl,--build-id=none", "-o", elf, SRC])
    run([BIN / "llvm-objcopy.exe", "-O", "binary", "--only-section=.text", elf, raw])
    body = raw.read_bytes()
    if not body or len(body) > FUNC_SIZE or len(body) % 4:
        raise SystemExit(f"body size {len(body)} does not fit 0x{FUNC_SIZE:x}")
    syms = run([BIN / "llvm-nm.exe", elf])
    if f"{FUNC:08x} T physics_update" not in syms:
        raise SystemExit(f"physics_update not at 0x{FUNC:x}:\n{syms}")
    disasm = run([BIN / "llvm-objdump.exe", "-d", "--no-show-raw-insn", elf])
    before = base[FUNC:FUNC + len(body)]
    spec = {
        "file": "app/assets/guest_libs/libapp.so",
        "base_sha256": BASE_SHA,
        "id": "ST-3 PhysicsSubSystem::Update: one whole-ms Bullet step per rendered frame with the fraction carried "
              "(stage 8 frame-rate work, work/evidence/stage8-framerate/RESULT.md); +0x70 = the real time of the step "
              "for the engine's per-tick timers, e.g. the crash cutscene (stage 14, work/evidence/stage14-crash-memory)",
        "source": "work/native/physics_update/physics_update.S (built by work/tools/build_physics_update.py)",
        "patches": [{"offset": f"0x{FUNC:X}", "before": before.hex(), "after": body.hex(),
                     "insn": f"sub_3AC1B8 body, {len(body)} of 0x{FUNC_SIZE:x} bytes; BL 0x{CALLEE:X} unchanged callee"}],
        "apply": "python -X utf8 -B <skill>/scripts/artifacts.py patch --root GAME --path app/assets/guest_libs/libapp.so "
                 f"--expect-sha256 {BASE_SHA} --patch 0x{FUNC:X}:<before>:<after>",
    }
    patched = bytearray(base)
    patched[FUNC:FUNC + len(body)] = body
    spec["result_sha256"] = hashlib.sha256(patched).hexdigest()
    SPEC.write_text(json.dumps(spec, indent=1) + "\n", encoding="utf-8", newline="\n")
    print(disasm[disasm.find("<physics_update>"):])
    print(json.dumps({"bytes": len(body), "result_sha256": spec["result_sha256"], "spec": str(SPEC.relative_to(GAME))}, indent=1))
    return 0


if __name__ == "__main__":
    sys.exit(main())
