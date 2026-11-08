package com.ea.ironmonkey.port;

import android.opengl.GLSurfaceView;

/**
 * Game-thread bridge to the engine's own local QA/cheat sequences (arm64 bridge: namespace qa of jni_bridge.cpp;
 * ARM32 build: libnfsport nfsqa.c). All actions are queued on the GL thread (the engine's game thread) via
 * GLSurfaceView.queueEvent.
 */
public final class NfsQa {
    public static final String TAG = "NfsQa";

    public static final int GRANT_CASH = 1;
    public static final int SET_DEBUG_BYTE = 2;
    public static final int GET_DEBUG_BYTE = 3;
    public static final int UNLOCK_ALL_CARS = 4;
    public static final int UNLOCK_ALL_EVENTS = 5;
    public static final int AWARD_ALL = 6;
    public static final int GET_CASH = 7;
    public static final int SAVE = 8;
    /** Pause menu → "Controls": the game's original help window (the flow output the help button had). */
    public static final int OPEN_PAUSE_HELP = 9;

    /** DebugOptions byte offsets (hidden_content.md HC-02). */
    public static final int OPT_AI_DISABLE_OPPONENTS = 40;
    public static final int OPT_EVERYTHING_IS_FREE = 53;
    public static final int OPT_DISABLE_TRAFFIC = 72;
    public static final int OPT_INFINITE_NITRO = 73;
    public static final int OPT_WEAKEN_OPPONENTS = 74;

    private NfsQa() {}

    static native int nativeAction(int action, int arg);

    public interface Result {
        void done(int rc);
    }

    private static boolean sLoaded;

    static synchronized boolean ensureLoaded() {
        if (!sLoaded) {
            try {
                System.loadLibrary("nfsport");
            } catch (UnsatisfiedLinkError e) {
                NfsLog.i(TAG, "libnfsport not packaged; expecting natives from the runtime bridge: " + e.getMessage());
            }
            sLoaded = true;
        }
        return true;
    }

    /** Runs one action on the GL (game) thread; the callback runs on that thread too. */
    public static void post(GLSurfaceView view, final int action, final int arg, final Result cb) {
        ensureLoaded();
        view.queueEvent(new Runnable() {
            @Override
            public void run() {
                int rc;
                try {
                    rc = nativeAction(action, arg);
                } catch (UnsatisfiedLinkError e) {
                    NfsLog.e(TAG, "natives unavailable: " + e);
                    rc = -101;
                }
                NfsLog.i(TAG, "action " + action + " arg " + arg + " -> " + rc);
                if (cb != null) cb.done(rc);
            }
        });
    }
}
