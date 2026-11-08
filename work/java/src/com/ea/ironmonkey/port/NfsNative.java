package com.ea.ironmonkey.port;

import android.app.Activity;
import android.content.res.AssetManager;
import android.os.Build;
import android.os.Process;

/**
 * Native library bootstrap (phase 7). GameActivityMain.onCreate calls this where the original loaded
 * fmodex, fmodevent, c++_shared, Nimble and app.
 *
 * ARM32 process: the game's own libraries, in the original order.
 * 64-bit process (arm64-only build): lib/arm64-v8a/libapp.so is the Dynarmic bridge (built for Android 10+, where
 * the linker supports ELF thread-local storage; libapp_legacy.so is the same bridge for Android 7-9, whose
 * thread-locals go through emulated TLS calls). It registers every
 * game native in JNI_OnLoad; nativeBootGuest then maps the ARM32 libraries from assets/guest_libs/ and
 * runs their constructors and JNI_OnLoads before any game native is called. A failed boot throws
 * instead of leaving natives that silently do nothing.
 *
 * Stage 16 (report: version 1.1 closes at once on an Android emulator for PC where 1.0 ran): libapp.so of 1.1 is the
 * first bridge with an ELF TLS segment (PT_TLS, TLSDESC relocations); 1.0 had only the emulated-TLS kind. The arm64
 * translators of PC emulators run this process on an x86 CPU (the VM's os.arch is then x86_64, not aarch64) and are
 * not all able to load or run such a library. A translated process therefore takes libapp_legacy.so and asks the
 * bridge for its exact JIT settings, as in 1.0; and whichever library is tried first, the other one is tried when it
 * does not load.
 */
public final class NfsNative {
    private static final String TAG = "NfsNative";

    private NfsNative() {}

    public static void loadGameLibraries(Activity activity) {
        if (Process.is64Bit()) {
            String arch = System.getProperty("os.arch", "");
            boolean translated = !arch.isEmpty() && !"aarch64".equals(arch);
            String bridge = loadBridge(Build.VERSION.SDK_INT >= 29 && !translated ? "app" : "app_legacy");
            NfsLog.i(TAG, "bridge lib" + bridge + ".so (Android API " + Build.VERSION.SDK_INT + ", os.arch " + arch
                    + (translated ? ", translated" : "") + ")");
            if (!nativeBootGuest(activity.getAssets(), translated)) {
                throw new UnsatisfiedLinkError("ARM32 guest libraries failed to start (Arm32Bridge log)");
            }
            NfsLog.i(TAG, "ARM32 guest started under the arm64 bridge");
            NfsOptions.init(activity);
            return;
        }
        System.loadLibrary("fmodex");
        System.loadLibrary("fmodevent");
        System.loadLibrary("c++_shared");
        System.loadLibrary("Nimble");
        System.loadLibrary("app");
    }

    /** Loads the preferred bridge library, else the other one; returns the name of the one that loaded. */
    private static String loadBridge(String preferred) {
        String other = "app".equals(preferred) ? "app_legacy" : "app";
        try {
            System.loadLibrary(preferred);
            return preferred;
        } catch (UnsatisfiedLinkError first) {
            try {
                System.loadLibrary(other);
            } catch (UnsatisfiedLinkError second) {
                UnsatisfiedLinkError both = new UnsatisfiedLinkError("neither lib" + preferred + ".so (" + first.getMessage()
                        + ") nor lib" + other + ".so (" + second.getMessage() + ") loads");
                both.initCause(first);
                throw both;
            }
            // NfsLog is set up by the bridge's JNI_OnLoad, so the reason is logged only now
            NfsLog.w(TAG, "lib" + preferred + ".so did not load (" + first.getMessage() + "), using lib" + other + ".so");
            return other;
        }
    }

    private static native boolean nativeBootGuest(AssetManager assets, boolean translated);
}
