package com.ea.ironmonkey.port;

import android.app.Activity;
import android.content.Context;
import android.content.SharedPreferences;
import android.os.Handler;
import android.os.Looper;
import android.os.Process;

/**
 * Frame rate limit (stage 10): the settings row 30 / 45 / 60 / MAX of the game's own settings pages (MAX = the
 * display's maximum refresh rate), 60 fps by default. The row is the engine's RadioOption widget (layouts.sb in the OBB
 * overlay, work/tools/settings_ui.py) and its option lives in the arm64 bridge (arm64_bridge/src/framerate.cpp), which
 * also paces the GL thread: GameRenderer.onDrawFrame calls {@link #beforeFrame()} before the RunLoop tick and
 * {@link #afterFrame()} after it. This class keeps the choice in the port's preferences (the game's save is not touched),
 * hands it and the display timing to the bridge, lets NfsDisplay pick the display mode for the limit and gives NfsPerf
 * the frame target. Without the bridge (an ARM32 build) the natives are missing and nothing is paced.
 */
public final class NfsFrameRate {
    private static final String TAG = "NfsFrameRate";
    /** RadioIndex of the settings row -> limit in fps; 0 = the display's maximum. */
    static final int[] RATES = {30, 45, 60, 0};
    static final int DEFAULT_INDEX = 2;
    private static final String PREFS = "nfs_port";
    private static final String KEY = "frame_rate_limit";

    private static volatile boolean sBridge;
    private static volatile int sIndex = DEFAULT_INDEX;
    private static volatile float sHz;
    private static volatile long sAppOffset;
    private static volatile long sDeadline;
    private static Activity sActivity;
    private static Handler sMain;

    private NfsFrameRate() {}

    /** NfsDisplay.apply (UI thread, GameActivityMain.onCreate after the native libraries are loaded). */
    static void init(Activity activity) {
        sActivity = activity;
        sMain = new Handler(Looper.getMainLooper());
        int index = DEFAULT_INDEX;
        try {
            index = prefs(activity).getInt(KEY, DEFAULT_INDEX);
        } catch (Throwable t) {
            NfsLog.w(TAG, "preferences unreadable, using the default: " + t);
        }
        sIndex = index >= 0 && index < RATES.length ? index : DEFAULT_INDEX;
        boolean bridge = false;
        if (Process.is64Bit()) {
            try {
                nativeConfigure(sIndex, sHz, sAppOffset, sDeadline);
                bridge = true;
            } catch (UnsatisfiedLinkError e) {
                NfsLog.w(TAG, "bridge natives missing, frames are not paced: " + e);
            }
        }
        sBridge = bridge;
        NfsLog.i(TAG, "frame rate limit " + describe(sIndex) + (bridge ? "" : " (not paced)"));
    }

    /** The limit in fps, 0 for the display's maximum. */
    static int limit() {
        return RATES[sIndex];
    }

    /** NfsDisplay: the active refresh rate and its vsync timing (Display.getAppVsyncOffsetNanos, ...DeadlineNanos). */
    static void setDisplay(float hz, long appVsyncOffsetNanos, long presentationDeadlineNanos) {
        sHz = hz;
        sAppOffset = appVsyncOffsetNanos;
        sDeadline = presentationDeadlineNanos;
        push();
    }

    private static void push() {
        float hz = sHz;
        int limit = limit();
        NfsPerf.setFrameRate(limit > 0 && hz > 1f ? Math.min(limit, hz) : hz);
        if (sBridge) nativeConfigure(sIndex, hz, sAppOffset, sDeadline);
    }

    /** Bridge, GL thread: the player picked a value in the settings row, or the settings went back to the defaults. */
    static void onLimitChanged(final int index) {
        Handler main = sMain;
        if (index < 0 || index >= RATES.length || main == null) return;
        sIndex = index;
        main.post(new Runnable() {
            @Override
            public void run() {
                Activity a = sActivity;
                try {
                    if (a != null) prefs(a).edit().putInt(KEY, index).apply();
                } catch (Throwable t) {
                    NfsLog.w(TAG, "cannot save the frame rate limit: " + t);
                }
                NfsLog.i(TAG, "frame rate limit " + describe(index) + " saved");
                if (a != null) NfsDisplay.requestMode(a);
                push();
            }
        });
    }

    /** GameRenderer.onDrawFrame, before the RunLoop tick (GL thread): waits for the frame's start time. */
    public static void beforeFrame() {
        if (sBridge) nativeBeforeFrame();
    }

    /** GameRenderer.onDrawFrame, after the RunLoop tick (GL thread): the frame's presentation time. */
    public static void afterFrame() {
        if (sBridge) nativeAfterFrame();
    }

    static String describe(int index) {
        return RATES[index] > 0 ? RATES[index] + " fps" : "display maximum";
    }

    private static SharedPreferences prefs(Context context) {
        // NfsApplication redirects the application's preferences to the external data root (NfsStorage)
        return context.getApplicationContext().getSharedPreferences(PREFS, Context.MODE_PRIVATE);
    }

    private static native void nativeConfigure(int index, float hz, long appVsyncOffsetNanos, long presentationDeadlineNanos);

    private static native void nativeBeforeFrame();

    private static native void nativeAfterFrame();
}
