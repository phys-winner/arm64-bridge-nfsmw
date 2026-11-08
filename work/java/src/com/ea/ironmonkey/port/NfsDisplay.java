package com.ea.ironmonkey.port;

import android.app.Activity;
import android.content.Context;
import android.hardware.display.DisplayManager;
import android.os.Build;
import android.os.Handler;
import android.os.Looper;
import android.view.Display;
import android.view.Window;
import android.view.WindowManager;

/**
 * Display mode (modernization 5.4, stage 10 frame rate limit). The game renders continuously on a GLSurfaceView, so its
 * presentation rate is the vsync of the active display mode, or the frame rate limit of the settings row when that is
 * lower (NfsFrameRate, paced by the arm64 bridge); the race simulation is time based (PhysicsUpdate 0x3AC1B8 feeds the
 * measured frame delta into the physics world). The window asks for a mode at the current resolution
 * (preferredDisplayModeId, API 23+): with the limit at the display maximum the fastest one, with a limit the slowest
 * rate that is a whole multiple of it (60 Hz for 30 and 60 fps on a 60/120 Hz panel, 90 Hz for 45 fps where it exists),
 * and the fastest one when no rate is a multiple (45 fps on 60/120 Hz). The active mode is read back and logged; later
 * mode changes (OS policy, LTPO) are logged from onDisplayChanged without a restart, and every rate the window actually
 * gets, with its vsync timing, goes to NfsFrameRate (bridge pacer, ADPF frame target in NfsPerf). No refresh rate is
 * hard-coded.
 */
public final class NfsDisplay {
    private static final String TAG = "NfsDisplay";

    private NfsDisplay() {}

    public static void apply(final Activity a) {
        NfsPerf.init(a);
        NfsFrameRate.init(a);
        final Display d = a.getWindowManager().getDefaultDisplay();
        publish(d);
        if (Build.VERSION.SDK_INT < 23) return;
        try {
            requestMode(a);
            DisplayManager dm = (DisplayManager) a.getSystemService(Context.DISPLAY_SERVICE);
            final int id = d.getDisplayId();
            dm.registerDisplayListener(new DisplayManager.DisplayListener() {
                private int last = -1;

                @Override
                public void onDisplayChanged(int displayId) {
                    if (displayId != id) return;
                    Display.Mode m = d.getMode();
                    publish(d);
                    if (m.getModeId() != last) {
                        last = m.getModeId();
                        NfsLog.i(TAG, "active mode " + describe(m));
                    }
                }

                @Override
                public void onDisplayAdded(int displayId) {}

                @Override
                public void onDisplayRemoved(int displayId) {}
            }, new Handler(Looper.getMainLooper()));
            new Handler(Looper.getMainLooper()).postDelayed(new Runnable() {
                @Override
                public void run() {
                    publish(d);
                    NfsLog.i(TAG, "active mode " + describe(d.getMode()) + ", window rate " + d.getRefreshRate());
                }
            }, 3000);
        } catch (Throwable t) {
            NfsLog.w(TAG, "display mode request failed: " + t);
        }
    }

    /** The mode for the current frame rate limit (UI thread); also after the player changes the limit. */
    static void requestMode(Activity a) {
        if (Build.VERSION.SDK_INT < 23) return;
        try {
            Display d = a.getWindowManager().getDefaultDisplay();
            Display.Mode cur = d.getMode();
            Display.Mode[] modes = d.getSupportedModes();
            float[] rates = new float[modes.length];
            boolean[] sameSize = new boolean[modes.length];
            for (int i = 0; i < modes.length; i++) {
                rates[i] = modes[i].getRefreshRate();
                sameSize[i] = modes[i].getPhysicalWidth() == cur.getPhysicalWidth() && modes[i].getPhysicalHeight() == cur.getPhysicalHeight();
            }
            int pick = chooseMode(rates, sameSize, NfsFrameRate.limit());
            Display.Mode best = pick >= 0 ? modes[pick] : cur;
            Window w = a.getWindow();
            WindowManager.LayoutParams lp = w.getAttributes();
            if (lp.preferredDisplayModeId != best.getModeId()) {
                lp.preferredDisplayModeId = best.getModeId();
                w.setAttributes(lp);
            }
            int limit = NfsFrameRate.limit();
            NfsLog.i(TAG, "requested mode " + describe(best) + " for " + (limit > 0 ? limit + " fps" : "the display maximum") + " (was "
                    + describe(cur) + ", " + modes.length + " modes)");
        } catch (Throwable t) {
            NfsLog.w(TAG, "display mode request failed: " + t);
        }
    }

    /**
     * Index of the mode for a frame rate limit among the modes of the current resolution (sameSize), -1 if none:
     * limit 0 = the fastest; otherwise the slowest rate that is a whole multiple of the limit, else the fastest.
     */
    static int chooseMode(float[] rates, boolean[] sameSize, int limit) {
        int fastest = -1, multiple = -1;
        for (int i = 0; i < rates.length; i++) {
            if (!sameSize[i]) continue;
            if (fastest < 0 || rates[i] > rates[fastest] + 0.1f) fastest = i;
            if (limit > 0 && rates[i] >= limit - 0.5f) {
                float ratio = rates[i] / limit;
                if (Math.abs(ratio - Math.round(ratio)) < 0.02f && (multiple < 0 || rates[i] < rates[multiple] - 0.1f)) multiple = i;
            }
        }
        return multiple >= 0 ? multiple : fastest;
    }

    private static void publish(Display d) {
        NfsFrameRate.setDisplay(d.getRefreshRate(), d.getAppVsyncOffsetNanos(), d.getPresentationDeadlineNanos());
    }

    private static String describe(Display.Mode m) {
        return "id=" + m.getModeId() + " " + m.getPhysicalWidth() + "x" + m.getPhysicalHeight() + "@" + m.getRefreshRate();
    }
}
