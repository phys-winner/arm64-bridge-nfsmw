#!/usr/bin/env python3
"""Build the port's own code into the canonical app/ tree (reproducible, no network).

  java : work/java/src/**/*.java -> javac(--release 8, android.jar) -> d8 -> baksmali
         -> app/smali/com/ea/ironmonkey/port/ and app/smali/com/ea/nimble/ (both directories replaced:
            the port classes and the minimal offline Nimble kept for the game's native Nimble bridge);
         work/java/stubs/**/*.java (stage 15: compile-time stubs of game classes in app/smali, e.g. the MOGA
         SDK used by NfsGamepad) are on the javac class path only and never packaged
  native: work/native/nfsvfs/{nfsvfs.c,nfsqa.c} -> NDK clang armv7a (API 21)
         -> app/lib/armeabi-v7a/libnfsport.so

Since phase 7 app/ is in the arm64 bridge layout (work/tools/bridge_layout.py): --native, the ARM32
libnfsport.so, is only for rebuilding an ARM32 tree passed with --app. The QA actions (nfsqa.c) are host code
of the arm64 bridge since stage 9 (no ARM32 guest library).

Options: --app DIR (default GAME/app) and --extra-src DIR (additional Java sources, e.g. the
diagnostic fixture under work/java/fixture) to build into a non-canonical tree.

Usage: python -X utf8 -B work/tools/build_port_code.py [--java] [--native]
(no flag = java). Prints a JSON summary with input/output SHA-256.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import shutil
import subprocess
import sys
from pathlib import Path

GAME = Path(__file__).resolve().parents[2]
SDK = Path(os.environ.get("ANDROID_SDK_ROOT", str(Path(os.environ.get("LOCALAPPDATA", "")) / "Android" / "Sdk")))
JAVA_HOME = Path(os.environ.get("JAVA_HOME", ""))
JAVAC = JAVA_HOME / "bin" / "javac.exe"
JAVA = JAVA_HOME / "bin" / "java.exe"
D8_JAR = SDK / "build-tools" / os.environ.get("ANDROID_BUILD_TOOLS_VERSION", "36.1.0") / "lib" / "d8.jar"
BAKSMALI = Path(os.environ.get("BAKSMALI_JAR", "baksmali.jar"))
ANDROID_JAR = SDK / "platforms" / os.environ.get("ANDROID_COMPILE_PLATFORM", "android-36") / "android.jar"
NDK = SDK / "ndk" / os.environ.get("ANDROID_NDK_VERSION", "27.2.12479018")
CLANG = NDK / "toolchains" / "llvm" / "prebuilt" / "windows-x86_64" / "bin" / "clang.exe"
READELF = NDK / "toolchains" / "llvm" / "prebuilt" / "windows-x86_64" / "bin" / "llvm-readelf.exe"

JAVA_SRC = GAME / "work" / "java" / "src"
JAVA_STUBS = GAME / "work" / "java" / "stubs"
JAVA_BUILD = GAME / "work" / "java" / "build"
SMALI_PKGS = [Path("com") / "ea" / "ironmonkey" / "port", Path("com") / "ea" / "nimble"]
NATIVE_SRCS = [GAME / "work" / "native" / "nfsvfs" / "nfsvfs.c", GAME / "work" / "native" / "nfsvfs" / "nfsqa.c"]
NATIVE_BUILD = GAME / "work" / "native" / "build" / "armeabi-v7a"
LIB_NAME = "libnfsport.so"
OLD_LIB_NAMES = ["libnfsvfs.so"]
CFLAGS = ["--target=armv7a-linux-androideabi21", "-shared", "-fPIC", "-O2", "-Wall", "-Wextra", "-Werror",
          "-fvisibility=hidden", "-ffile-prefix-map=" + str(GAME) + "=.", "-Wl,--build-id=sha1",
          "-Wl,-z,relro,-z,now", "-Wl,--no-undefined"]


def sha(p: Path) -> str:
    return hashlib.sha256(p.read_bytes()).hexdigest()


def run(cmd: list, cwd: Path | None = None) -> None:
    cp = subprocess.run([str(c) for c in cmd], cwd=cwd, capture_output=True, text=True)
    if cp.returncode != 0:
        sys.stderr.write(cp.stdout + cp.stderr)
        raise SystemExit(f"FAILED ({cp.returncode}): {' '.join(map(str, cmd))[:300]}")
    noise = [l for l in (cp.stdout + cp.stderr).splitlines() if l.strip() and "release 8" not in l
             and "obsolete" not in l and "-Xlint:-options" not in l]
    if noise:
        sys.stderr.write("\n".join(noise[:40]) + "\n")


def build_java(app: Path, extra: list) -> dict:
    for tool in (JAVAC, JAVA, D8_JAR, BAKSMALI, ANDROID_JAR):
        if not tool.exists():
            raise SystemExit(f"missing tool: {tool}")
    if JAVA_BUILD.exists():
        shutil.rmtree(JAVA_BUILD)
    classes = JAVA_BUILD / "classes"
    dex = JAVA_BUILD / "dex"
    smali = JAVA_BUILD / "smali"
    stub_classes = JAVA_BUILD / "stub-classes"
    for d in (classes, dex, smali, stub_classes):
        d.mkdir(parents=True)
    classpath = [ANDROID_JAR]
    stubs = sorted(JAVA_STUBS.rglob("*.java")) if JAVA_STUBS.is_dir() else []
    if stubs:
        run([JAVAC, "--release", "8", "-Xlint:-options", "-encoding", "UTF-8", "-cp", ANDROID_JAR,
             "-d", stub_classes, *stubs])
        classpath.append(stub_classes)
    sources = sorted(JAVA_SRC.rglob("*.java")) + [p for d in extra for p in sorted(Path(d).rglob("*.java"))]
    run([JAVAC, "--release", "8", "-Xlint:-options", "-encoding", "UTF-8", "-cp", os.pathsep.join(map(str, classpath)),
         "-d", classes, *sources])
    class_files = sorted(classes.rglob("*.class"))
    run([JAVA, "-cp", D8_JAR, "com.android.tools.r8.D8", "--release", "--min-api", "21",
         "--lib", ANDROID_JAR, "--output", dex, *class_files])
    run([JAVA, "-jar", BAKSMALI, "disassemble", dex / "classes.dex", "-o", smali])
    outputs = {}
    for pkg in SMALI_PKGS:
        produced = smali / pkg
        if not produced.is_dir():
            raise SystemExit(f"no smali produced under {produced}")
        target = app / "smali" / pkg
        if target.exists():
            shutil.rmtree(target)
        shutil.copytree(produced, target)
        outputs.update({p.relative_to(app).as_posix(): sha(p) for p in sorted(target.rglob("*.smali"))})
    return {"sources": {p.relative_to(JAVA_SRC).as_posix() if JAVA_SRC in p.parents else p.name: sha(p) for p in sources},
            "smali": outputs}


def build_native(app: Path) -> dict:
    APP_LIB = app / "lib" / "armeabi-v7a" / LIB_NAME
    if not CLANG.exists():
        raise SystemExit(f"missing tool: {CLANG}")
    NATIVE_BUILD.mkdir(parents=True, exist_ok=True)
    out = NATIVE_BUILD / LIB_NAME
    run([CLANG, *CFLAGS, "-o", out, *NATIVE_SRCS, "-llog"])
    APP_LIB.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(out, APP_LIB)
    for old in OLD_LIB_NAMES:
        stale = APP_LIB.parent / old
        if stale.exists():
            stale.unlink()
    return {"sources": {p.name: sha(p) for p in NATIVE_SRCS}, "output": str(APP_LIB).replace("\\", "/"),
            "sha256": sha(APP_LIB), **elf_summary(out)}


def elf_summary(out: Path) -> dict:
    dyn = subprocess.run([str(READELF), "--dyn-syms", "-d", "-l", str(out)], capture_output=True, text=True).stdout
    exports = sorted({l.split()[-1] for l in dyn.splitlines() if "GLOBAL" in l and " FUNC " in l and "UND" not in l})
    needed = [l.split("[")[1].rstrip("]") for l in dyn.splitlines() if "(NEEDED)" in l]
    return {"exports": exports, "needed": needed}


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--java", action="store_true")
    ap.add_argument("--native", action="store_true")
    ap.add_argument("--app", type=Path, default=GAME / "app")
    ap.add_argument("--extra-src", action="append", default=[])
    args = ap.parse_args()
    default = not args.java and not args.native
    summary = {}
    if args.java or default:
        summary["java"] = build_java(args.app.resolve(), args.extra_src)
    if args.native:
        summary["native"] = build_native(args.app.resolve())
    print(json.dumps(summary, indent=1))
    return 0


if __name__ == "__main__":
    sys.exit(main())
