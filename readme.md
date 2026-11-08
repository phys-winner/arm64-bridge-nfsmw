# ARM64 bridge for Need for Speed: Most Wanted

A game-specific ARM32-to-ARM64 compatibility bridge for the Android release of
**Need for Speed: Most Wanted (1.3.128 / version code 1003128)**. It runs the game's
ARMv7 native libraries inside an ARM64 Android process using Dynarmic's A32 JIT,
with native host implementations of the Android and system interfaces they use.

This repository contains the bridge, its regression tests, dependency patches,
Java compatibility code, and source tools used by the port. It contains no APKs,
OBBs, original game libraries, extracted game resources, signing keys, saves, or
device-specific test records. Supply your own legally obtained game files.

## How it works

The host `libapp.so` loads ARM32 guest ELF libraries from uncompressed
`assets/guest_libs/` entries in the APK. Dynarmic translates guest instructions to
AArch64; the bridge handles ELF relocation, guest memory, JNI, threading, file
access, OpenGL ES/EGL, OpenSL ES audio, zlib, and Android calls.

The accompanying port code provides modern storage and OBB handling, touch and
gamepad support, frame rate and graphics settings, and offline service shims.
The bridge also includes game-specific render queue and memory fixes, optional
colour grading and nitrous motion blur, and rendering scale controls. Graphics
settings UI overlays are generated from the user's original OBB by source tools.

This is a compatibility layer for this particular game build, not a general
Android emulator. Native hooks and data edits depend on the expected guest layout.
Other game versions and modified libraries require separate validation.

## Source layout

| Directory | Contents |
| --- | --- |
| `arm64_bridge/src` | Native loader, JIT integration, host thunks, game hooks |
| `arm64_bridge/tests` | Native, JNI/ART, and Python regression tests |
| `arm64_bridge/patches` | Dynarmic/Oaknut patches and their CMake application script |
| `work/java/src` | Port-owned Java compatibility and offline service code |
| `work/java/stubs` | Compile-time controller interface stubs; never package these |
| `work/native` | ARM32 support sources and the physics update assembly source |
| `work/tools` | Java build, guest layout, OBB staging, overlay, and cleanup tools |
| `work/research/content/sbin.py` | SBIN reader used by the overlay tools |

## Dependencies

- Windows with PowerShell, Git, Python 3, and a JDK (Java 8 source compatibility).
- Android SDK, NDK **27.2.12479018 (r27c)**, CMake **3.22.1**, and Ninja.
- An external extracted **Boost 1.83.0** header tree, supplied with `-BoostRoot`
  or `BOOST_ROOT`. Boost sources are not copied into this repository.
- [Dynarmic](https://github.com/azahar-emu/dynarmic), pinned to
  `e77b1ba0b7da7cbe93021b01a663acfe7c4dd516`. CMake fetches this revision and
  its submodules into the ignored build directory and applies the included
  patches. This also obtains its transitive dependencies, including Oaknut,
  fmt, mcl, and robin-map.
- For Java-to-smali conversion: Android platform **android-36**, SDK build tools
  **36.1.0**, and an external `baksmali.jar` supplied through `BAKSMALI_JAR`.
- APK integration additionally needs apktool and Android build/signing tools.
  Supply your own signing key outside the tracked tree.

## Build the ARM64 libraries

Run these commands from the repository root, after installing the dependencies.
Set `BOOST_ROOT` to your external Boost directory and `ANDROID_SDK_ROOT` if the
SDK is not installed in the standard per-user location.

```powershell
./arm64_bridge/build_arm64.ps1 -BridgeTarget app -AndroidPlatform android-29 -AllowNetwork
./arm64_bridge/build_arm64.ps1 -BridgeTarget app_legacy -AndroidPlatform android-26 -BuildDirectory build_arm64_legacy -AllowNetwork
```

The stripped outputs are `arm64_bridge/dist/libapp.so` and
`arm64_bridge/dist/libapp_legacy.so`. The wrapper checks ELF64/AArch64 and at least
16 KiB alignment of every load segment. The API 29 build uses native ELF TLS;
the API 26 variant provides the legacy TLS path selected by `NfsNative` for older
Android versions or translated processes. These are build targets, not a promise
of support for every Android device or PC emulator.

To build offline, first clone the pinned Dynarmic revision recursively into an
external directory and pass `-DynarmicSourceDirectory` with that directory.
The wrapper applies the same patches there; use a dedicated checkout because
patching modifies its working tree. Boost remains an external input.

The patches include game-specific interpreter fallback behaviour and optional
unsafe JIT optimizations. They are required by this bridge and should not be
applied to an unrelated emulator without reviewing their assumptions.

## Integrate with your own game files

The repository builds the bridge independently. It is **not a complete APK
reconstruction kit**: original app smali, Android manifest, resources, and
version-specific integration edits are external inputs. Do not expect decoding
an arbitrary stock APK and running these tools to produce the finished port.

For an integration tree prepared for the port:

1. Place the apktool tree in local `app/`, which is ignored by Git. Retain the
   original guest libraries and prepare the activity/JNI integration expected by
   `work/java/src/com/ea/ironmonkey/port/NfsNative.java`.
2. Build both host libraries, then run `python work/tools/bridge_layout.py` to
   move the five expected ARM32 libraries to `app/assets/guest_libs/`, install the
   ARM64 host libraries, and mark `.so` entries uncompressed in `apktool.yml`.
   This edits the local integration tree; keep a backup of your game input.
3. Set `JAVA_HOME`, `ANDROID_SDK_ROOT`, and `BAKSMALI_JAR`, then run
   `python work/tools/build_port_code.py --java` to compile the port classes
   into the prepared smali tree. This replaces the port and Nimble package
   directories. `--native` builds the older ARM32 helper only; ARM64 uses the
   equivalent host code in the bridge.
4. To generate the graphics/settings overlays, place the expected original OBB
   at `original/main.1003128.com.ea.games.nfs13_row.obb` and run
   `python work/tools/make_obb_overlay.py`. It validates the original container,
   writes `app/assets/obb_overlay/`, its manifest, and `port_strings.txt`.
   `stage_obb.py` optionally embeds the original OBB without unpacking it.
5. Use your own apktool/build-tools workflow to rebuild, align, and sign the APK.
   Preserve uncompressed guest libraries and Android's required library alignment.

`build_physics_update.py` assembles the frame-time replacement from source and
writes a checked patch specification for the expected original guest library.
Applying guest binary patches and preparing the other original app integration
edits are separate steps. The original game's binaries and patched byte ranges
are not distributed here.

## Checks

Host-side ELF import coverage tests require only Python:

```powershell
python -B -m unittest discover -s arm64_bridge/tests -p "test_*.py"
```

To build native and JNI regression targets, add `-BuildTests` to the bridge build.
The device runner requires an explicitly selected device through `-Serial`:

```powershell
./arm64_bridge/tests/run_android_tests.ps1 -Serial $env:ANDROID_SERIAL
```

JNI tests also require `JAVA_HOME` and the SDK build tools. Test outputs and
device details belong in the ignored build tree. `check_symbols.py` can inspect
locally supplied ELF32 guest imports against the bridge's providers. Passing
these checks does not replace gameplay, graphics, audio, and long-session tests
on the target device.

## License

Original project code is licensed under **GPL-3.0-or-later**, a strong copyleft
license. See [LICENSE](LICENSE) for its terms and [NOTICE](NOTICE) for third-party
attribution. External dependencies retain their own licenses. No license to the
original game, its data, or its proprietary components is granted by this project.
