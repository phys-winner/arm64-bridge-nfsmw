package com.ea.ironmonkey.port;

import android.annotation.TargetApi;
import android.content.Context;
import android.os.Build;
import android.os.PerformanceHintManager;
import android.os.Process;

/**
 * Frame work hints (modernization 5.4, stage 8 frame-rate work). The engine's whole frame (IsisApp::Update +
 * Render, one RunLoop tick) runs on the GL thread inside GameRenderer.onDrawFrame. On API 31+ an ADPF
 * PerformanceHintManager session for that thread gets the frame period (display mode, or the lower frame rate limit
 * of the settings, NfsFrameRate) as its target and the measured onDrawFrame time after every frame (the pacer's wait
 * before the frame is outside it), so the CPU scheduler places and clocks the thread for the frame deadline instead
 * of for its average load. Without the service (API &lt; 31, or createHintSession returns
 * null when the power HAL has no support) nothing changes. The GL thread is looked up on every frame, so a
 * recreated GL thread gets a new session.
 */
public final class NfsPerf {
    private static final String TAG = "NfsPerf";
    private static volatile Context sContext;
    private static volatile long sTargetNanos = 16666667L;
    private static volatile boolean sTargetChanged;
    private static boolean sUnavailable;
    private static long sStart;

    private NfsPerf() {}

    public static void init(Context context) {
        sContext = context.getApplicationContext();
    }

    /**
     * Frame rate the GL thread runs at (NfsFrameRate: the active display mode's refresh rate, or the frame rate limit when
     * that is lower); the frame target follows it without a restart.
     */
    public static void setFrameRate(float hz) {
        if (hz < 1f) return;
        long target = (long) (1e9 / hz);
        if (target != sTargetNanos) {
            sTargetNanos = target;
            sTargetChanged = true;
        }
    }

    /** GameRenderer.onDrawFrame entry (GL thread). */
    public static void frameStart() {
        sStart = System.nanoTime();
    }

    /** GameRenderer.onDrawFrame exit (GL thread). */
    public static void frameEnd() {
        if (sUnavailable || Build.VERSION.SDK_INT < 31) return;
        long duration = System.nanoTime() - sStart;
        if (duration <= 0) return;
        try {
            Api31.report(duration);
        } catch (Throwable t) {
            sUnavailable = true;
            NfsLog.w(TAG, "ADPF hint session disabled: " + t);
        }
    }

    @TargetApi(31)
    private static final class Api31 {
        private static PerformanceHintManager.Session sSession;
        private static int sTid = -1;

        static void report(long duration) {
            int tid = Process.myTid();
            if (sSession == null || tid != sTid) {
                if (sSession != null) {
                    sSession.close();
                    sSession = null;
                }
                Context c = sContext;
                if (c == null) return;
                PerformanceHintManager m = (PerformanceHintManager) c.getSystemService(Context.PERFORMANCE_HINT_SERVICE);
                sTid = tid;
                sSession = m != null ? m.createHintSession(new int[] {tid}, sTargetNanos) : null;
                if (sSession == null) {
                    sUnavailable = true;
                    NfsLog.i(TAG, "ADPF hint sessions are not supported on this device");
                    return;
                }
                sTargetChanged = false;
                NfsLog.i(TAG, "ADPF hint session for GL thread " + tid + ", target " + sTargetNanos + " ns, preferred update rate "
                        + m.getPreferredUpdateRateNanos() + " ns");
            }
            if (sTargetChanged) {
                sTargetChanged = false;
                long target = sTargetNanos;
                sSession.updateTargetWorkDuration(target);
                NfsLog.i(TAG, "ADPF target " + target + " ns");
            }
            sSession.reportActualWorkDuration(duration);
        }
    }
}
