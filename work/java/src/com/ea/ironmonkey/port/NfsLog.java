package com.ea.ironmonkey.port;

import android.util.Log;

/**
 * Logcat gate of the release build (stage 12): the port's Java classes, the original com.ea.ironmonkey.Log (its
 * methods also test {@link #on}) and FMOD's Java audio device log through here, and nothing reaches logcat unless
 * logging was switched on with {@code adb shell setprop debug.nfsbridge.logs 1} before a cold start.
 *
 * The bridge reads the property in JNI_OnLoad (arm64_bridge/src/log_gate.cpp, which also gates every native log of
 * the bridge and the ARM32 guest) and sets {@link #on}; lines logged before the bridge loads (the data root
 * migration in NfsApplication) stay silent either way.
 */
public final class NfsLog {
    /** True when debug.nfsbridge.logs is 1; written by the bridge's JNI_OnLoad. */
    public static volatile boolean on;

    private NfsLog() {}

    public static int v(String tag, String msg) {
        return on ? Log.v(tag, msg) : 0;
    }

    public static int d(String tag, String msg) {
        return on ? Log.d(tag, msg) : 0;
    }

    public static int i(String tag, String msg) {
        return on ? Log.i(tag, msg) : 0;
    }

    public static int w(String tag, String msg) {
        return on ? Log.w(tag, msg) : 0;
    }

    public static int e(String tag, String msg) {
        return on ? Log.e(tag, msg) : 0;
    }

    public static int e(String tag, String msg, Throwable tr) {
        return on ? Log.e(tag, msg, tr) : 0;
    }
}
