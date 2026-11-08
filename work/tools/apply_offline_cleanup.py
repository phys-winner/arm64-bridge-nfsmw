#!/usr/bin/env python3
"""Stage 9 (user request 2026-10-06): offline cleanup and direct launch, applied to the canonical app/ tree.

  * no permissions: INTERNET and ACCESS_NETWORK_STATE removed (every online service of the game is shut down)
  * res/xml/network_security_config.xml (+ android:networkSecurityConfig) and res/xml/nimble_log.xml removed,
    with their public.xml ids and the string nimble_configuration (only used by the removed manifest meta-data)
  * GameActivityMain is the launcher activity: the launcher alias com.ea.ironmonkey.GameActivity and
    PermissionsActivity are removed, as is GameActivityMain's <meta-data foo=bar>; the game-data check of the
    old route (NfsLaunch) runs at the start of GameActivityMain.onCreate, and while it reports missing data each
    lifecycle override only calls the framework method
  * classes removed: android/** (support library), com/android/** (install referrer, Play billing AIDL),
    com/google/** (Play services, gson), com/ea/eadp/** (EA push/telemetry, only used by Nimble) and the Nimble
    SDK; work/java/src/com/ea/nimble provides the offline core the game's native Nimble bridge still calls
    (Log/ILog, bridge.BaseNativeCallback) and build_port_code.py regenerates app/smali/com/ea/nimble from it
  * GameActivityMain: the ten com.ea.nimble.ApplicationLifecycle calls and the Amazon-only resources.json/gson
    branch (isAmazonDev is false for this package) removed; GameActivityMain$2 (gson TypeToken) deleted and the
    gson annotations of GameActivityMain$FSNode dropped
  * assets: guest_libs/libapp.jitprofile (JIT warm-up profile) and guest_libs/libnfsqa.so (the QA actions are
    host code of the bridge now) removed

Every edit checks its preimage; a second run reports "already applied". After it: build_port_code.py (java).
Usage: python -X utf8 -B work/tools/apply_offline_cleanup.py [--check]
"""
from __future__ import annotations

import argparse
import json
import re
import shutil
import sys
import zipfile
from pathlib import Path

GAME = Path(__file__).resolve().parents[2]
APP = GAME / "app"
SMALI = APP / "smali"
MANIFEST = APP / "AndroidManifest.xml"
GAM = SMALI / "com" / "ea" / "ironmonkey" / "GameActivityMain.smali"
FSNODE = SMALI / "com" / "ea" / "ironmonkey" / "GameActivityMain$FSNode.smali"
ANDROID_JAR = Path.home() / "AppData/Local/Android/Sdk/platforms/android-36/android.jar"

REMOVED_DIRS = ["android", "com/android", "com/google", "com/ea/eadp", "com/ea/nimble"]
REMOVED_FILES = ["smali/com/ea/ironmonkey/PermissionsActivity.smali", "smali/com/ea/ironmonkey/GameActivityMain$2.smali",
                 "res/xml/network_security_config.xml", "res/xml/nimble_log.xml",
                 "assets/guest_libs/libapp.jitprofile", "assets/guest_libs/libnfsqa.so"]

MANIFEST_EDITS = [
    ('    <uses-permission android:name="android.permission.INTERNET"/>\n', ''),
    ('    <uses-permission android:name="android.permission.ACCESS_NETWORK_STATE"/>\n', ''),
    (' android:networkSecurityConfig="@xml/network_security_config"', ''),
    ('android:launchMode="singleTask" android:name="com.ea.ironmonkey.GameActivityMain" android:resizeableActivity="false"',
     'android:exported="true" android:launchMode="singleTask" android:name="com.ea.ironmonkey.GameActivityMain" '
     'android:resizeableActivity="false"'),
    ('            <meta-data android:name="foo" android:value="bar"/>\n',
     '            <intent-filter>\n'
     '                <action android:name="android.intent.action.MAIN"/>\n'
     '                <category android:name="android.intent.category.LAUNCHER"/>\n'
     '            </intent-filter>\n'),
    ('        <activity android:configChanges="locale|keyboardHidden|orientation|screenSize|layoutDirection" '
     'android:launchMode="singleTask" android:name="com.ea.ironmonkey.PermissionsActivity" '
     'android:resizeableActivity="false" android:theme="@style/AppTheme"/>\n', ''),
    ('        <activity-alias android:exported="true" android:name="com.ea.ironmonkey.GameActivity" '
     'android:targetActivity="com.ea.ironmonkey.PermissionsActivity">\n'
     '            <intent-filter>\n'
     '                <action android:name="android.intent.action.MAIN"/>\n'
     '                <category android:name="android.intent.category.LAUNCHER"/>\n'
     '            </intent-filter>\n'
     '        </activity-alias>\n', ''),
    ('        <meta-data android:name="com.ea.nimble.configuration" android:value="@string/nimble_configuration"/>\n', ''),
    ('        <activity android:name="com.ea.nimble.WebView" '
     'android:theme="@android:style/Theme.Black.NoTitleBar.Fullscreen"/>\n', ''),
    ('        <receiver android:exported="false" android:name="com.ea.nimble.NimbleLocalNotificationReceiver"/>\n', ''),
]
PUBLIC_EDITS = [
    ('    <public type="string" name="nimble_configuration" id="0x7f0c004e" />\n', ''),
    ('    <public type="xml" name="network_security_config" id="0x7f0f0001" />\n', ''),
    ('    <public type="xml" name="nimble_log" id="0x7f0f0002" />\n', ''),
]
STRING_EDIT = ('    <string name="nimble_configuration">live</string>\n', '')

LAUNCH_GUARD_ONCREATE = (
    '    invoke-super {p0, p1}, Landroid/app/Activity;->onCreate(Landroid/os/Bundle;)V\n\n'
    '    sput-object p0, Lcom/ea/ironmonkey/GameActivityMain;->instance:Lcom/ea/ironmonkey/GameActivityMain;\n',
    '    invoke-super {p0, p1}, Landroid/app/Activity;->onCreate(Landroid/os/Bundle;)V\n\n'
    '    invoke-static {p0}, Lcom/ea/ironmonkey/port/NfsLaunch;->blockIfNoData(Landroid/app/Activity;)Z\n\n'
    '    move-result v1\n\n'
    '    if-eqz v1, :nfs_launch_go\n\n'
    '    return-void\n\n'
    '    :nfs_launch_go\n'
    '    sput-object p0, Lcom/ea/ironmonkey/GameActivityMain;->instance:Lcom/ea/ironmonkey/GameActivityMain;\n',
)
# method -> (super invocation, returns boolean)
GUARDED = {
    "onStart()V": ("invoke-super {p0}, Landroid/app/Activity;->onStart()V", False),
    "onRestart()V": ("invoke-super {p0}, Landroid/app/Activity;->onRestart()V", False),
    "onResume()V": ("invoke-super {p0}, Landroid/app/Activity;->onResume()V", False),
    "onPause()V": ("invoke-super {p0}, Landroid/app/Activity;->onPause()V", False),
    "onStop()V": ("invoke-super {p0}, Landroid/app/Activity;->onStop()V", False),
    "onDestroy()V": ("invoke-super {p0}, Landroid/app/Activity;->onDestroy()V", False),
    "onLowMemory()V": ("invoke-super {p0}, Landroid/app/Activity;->onLowMemory()V", False),
    "onBackPressed()V": ("invoke-super {p0}, Landroid/app/Activity;->onBackPressed()V", False),
    "onWindowFocusChanged(Z)V": ("invoke-super {p0, p1}, Landroid/app/Activity;->onWindowFocusChanged(Z)V", False),
    "onConfigurationChanged(Landroid/content/res/Configuration;)V":
        ("invoke-super {p0, p1}, Landroid/app/Activity;->onConfigurationChanged(Landroid/content/res/Configuration;)V", False),
    "onKeyDown(ILandroid/view/KeyEvent;)Z": ("invoke-super {p0, p1, p2}, Landroid/app/Activity;->onKeyDown(ILandroid/view/KeyEvent;)Z", True),
    "onKeyUp(ILandroid/view/KeyEvent;)Z": ("invoke-super {p0, p1, p2}, Landroid/app/Activity;->onKeyUp(ILandroid/view/KeyEvent;)Z", True),
    "finish()V": ("invoke-super {p0}, Landroid/app/Activity;->finish()V", False),
}
GUARD_MARK = "Lcom/ea/ironmonkey/port/NfsLaunch;->blocked()Z"
AMAZON_START = "    sget-boolean v1, Lcom/ea/ironmonkey/GameActivityMain;->isAmazonDev:Z\n\n    if-nez v1, :cond_8\n\n    goto :goto_0\n\n    :cond_8\n    :try_start_0\n"
AMAZON_END = ("    new-instance v1, Ljava/util/HashMap;\n\n    invoke-direct {v1}, Ljava/util/HashMap;-><init>()V\n\n"
              "    iput-object v1, p0, Lcom/ea/ironmonkey/GameActivityMain;->mResources:Ljava/util/Map;\n\n    :goto_0\n"
              "    iget-object v1, p0, Lcom/ea/ironmonkey/GameActivityMain;->mAssetLocationType:Lcom/ea/ironmonkey/GameActivityMain$AssetLocationType;\n")
AMAZON_KEEP_TAIL = ("    :goto_0\n"
                    "    iget-object v1, p0, Lcom/ea/ironmonkey/GameActivityMain;->mAssetLocationType:Lcom/ea/ironmonkey/GameActivityMain$AssetLocationType;\n")
LIFECYCLE_CALL = re.compile(r"    invoke-static \{[^}]*\}, Lcom/ea/nimble/ApplicationLifecycle;->[A-Za-z]+\([^)]*\)[VZ]\n\n")
FSNODE_ANNOTATION = re.compile(r"    \.annotation runtime Lcom/google/gson/annotations/SerializedName;\n        value = \"[a-z]+\"\n    \.end annotation\n")


def edit(text: str, old: str, new: str, what: str, log: list) -> str:
    if old in text:
        if text.count(old) != 1:
            raise SystemExit(f"{what}: preimage not unique")
        log.append(what)
        return text.replace(old, new)
    if new and new in text:
        return text
    if not new:
        return text  # removal already applied
    raise SystemExit(f"{what}: preimage not found")


def guard_method(text: str, signature: str, super_call: str, returns: bool, log: list) -> str:
    header = re.search(r"^\.method [^\n]* " + re.escape(signature) + r"\n", text, re.M)
    if not header:
        raise SystemExit(f"GameActivityMain.{signature} not found")
    end = text.index("\n.end method", header.end())
    body = text[header.end():end]
    if GUARD_MARK in body:
        return text
    lines = body.split("\n")
    locals_at = next(i for i, l in enumerate(lines) if l.strip().startswith(".locals"))
    if int(lines[locals_at].split()[1]) < 1:
        raise SystemExit(f"GameActivityMain.{signature}: no local register for the guard")
    at = locals_at + 1
    depth = 0
    while at < len(lines):  # past .param/.annotation blocks and blank lines
        stripped = lines[at].strip()
        if stripped.startswith((".param", ".annotation")):
            depth += 1
        elif stripped in (".end param", ".end annotation"):
            depth -= 1
        elif stripped and depth == 0 and not stripped.startswith((".line", ".prologue")):
            break
        at += 1
    guard = ["    invoke-static {}, " + GUARD_MARK, "", "    move-result v0", "", "    if-eqz v0, :nfs_launch_go", "",
             "    " + super_call, ""]
    guard += ["    move-result v0", "", "    return v0", ""] if returns else ["    return-void", ""]
    guard += ["    :nfs_launch_go"]
    lines[at:at] = guard
    log.append(f"launch guard in {signature}")
    return text[:header.end()] + "\n".join(lines) + text[end:]


def apply() -> list:
    log = []
    m = MANIFEST.read_text(encoding="utf-8")
    for old, new in MANIFEST_EDITS:
        m = edit(m, old, new, "manifest: " + (old.strip()[:70] if old.strip() else "?"), log)
    MANIFEST.write_text(m, encoding="utf-8", newline="")
    public = APP / "res" / "values" / "public.xml"
    p = public.read_text(encoding="utf-8")
    for old, new in PUBLIC_EDITS:
        p = edit(p, old, new, "public.xml: " + old.strip(), log)
    public.write_text(p, encoding="utf-8", newline="")
    strings = APP / "res" / "values" / "strings.xml"
    s = strings.read_text(encoding="utf-8")
    s = edit(s, STRING_EDIT[0], STRING_EDIT[1], "strings.xml: nimble_configuration", log)
    strings.write_text(s, encoding="utf-8", newline="")
    for other in sorted((APP / "res").glob("values-*/strings.xml")):
        if 'name="nimble_configuration"' in other.read_text(encoding="utf-8"):
            raise SystemExit(f"{other}: nimble_configuration also defined there")

    g = GAM.read_text(encoding="utf-8")
    calls = LIFECYCLE_CALL.findall(g)
    if calls:
        if len(calls) != 10:
            raise SystemExit(f"GameActivityMain: expected 10 ApplicationLifecycle calls, found {len(calls)}")
        g = LIFECYCLE_CALL.sub("", g)
        log.append("GameActivityMain: 10 ApplicationLifecycle calls removed")
    if AMAZON_START in g:
        start = g.index(AMAZON_START)
        end = g.index(AMAZON_END, start)
        if g.count(AMAZON_START) != 1 or "Lcom/google/gson/Gson;" not in g[start:end]:
            raise SystemExit("GameActivityMain: Amazon branch preimage")
        g = g[:start] + AMAZON_KEEP_TAIL + g[end + len(AMAZON_END):]
        log.append("GameActivityMain: Amazon resources.json/gson branch removed")
    g = edit(g, LAUNCH_GUARD_ONCREATE[0], LAUNCH_GUARD_ONCREATE[1], "GameActivityMain.onCreate: launch check", log)
    for signature, (super_call, returns) in GUARDED.items():
        g = guard_method(g, signature, super_call, returns, log)
    GAM.write_text(g, encoding="utf-8", newline="")

    f = FSNODE.read_text(encoding="utf-8")
    if FSNODE_ANNOTATION.search(f):
        f = FSNODE_ANNOTATION.sub("", f)
        log.append("GameActivityMain$FSNode: gson annotations removed")
        FSNODE.write_text(f, encoding="utf-8", newline="")

    for rel in REMOVED_FILES:
        path = APP / rel
        if path.exists():
            path.unlink()
            log.append(f"deleted {rel}")
    for rel in REMOVED_DIRS:
        path = SMALI / rel
        if path.exists():
            count = sum(1 for _ in path.rglob("*.smali"))
            shutil.rmtree(path)
            log.append(f"deleted smali/{rel} ({count} classes)")
    return log


def check() -> dict:
    """Every class the remaining smali references exists in app/smali or in android.jar."""
    have = {p.relative_to(SMALI).as_posix()[:-6] for p in SMALI.rglob("*.smali")}
    with zipfile.ZipFile(ANDROID_JAR) as jar:
        have |= {n[:-6] for n in jar.namelist() if n.endswith(".class")}
    missing = {}
    for path in SMALI.rglob("*.smali"):
        for ref in set(re.findall(r"L([a-zA-Z][\w/$]*);", path.read_text(encoding="utf-8"))):
            if ref not in have and not ref.startswith("dalvik/annotation/"):  # VM system annotations
                missing.setdefault(ref, []).append(path.relative_to(SMALI).as_posix())
    manifest = MANIFEST.read_text(encoding="utf-8")
    components = re.findall(r'android:name="(com\.ea\.[\w.]+)"', manifest)
    absent = [c for c in components if c.replace(".", "/") not in have]
    return {"classes": len([p for p in SMALI.rglob("*.smali")]), "unresolved": missing, "manifest_components_missing": absent,
            "permissions": re.findall(r"uses-permission[^>]*", manifest)}


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--check", action="store_true", help="only verify references (run after build_port_code.py)")
    args = ap.parse_args()
    out = {} if args.check else {"applied": apply()}
    out["check"] = check()
    print(json.dumps(out, indent=1))
    return 1 if out["check"]["unresolved"] or out["check"]["manifest_components_missing"] else 0


if __name__ == "__main__":
    sys.exit(main())
