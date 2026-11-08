package com.ea.ironmonkey.port;

import android.content.Context;
import android.content.SharedPreferences;
import android.os.Handler;
import android.os.Looper;

/**
 * The port's rows in the game's own settings pages (stage 13; stage 10 added the frame rate limit). The rows are the
 * engine's RadioOption widgets (layouts.sb in the OBB overlay, work/tools/settings_ui.py); their options live in the
 * arm64 bridge (arm64_bridge/src/options.cpp), which tells this class about every change. The values are RadioIndex
 * numbers kept in the port's preferences (shared_prefs/nfs_port.json of the external data root), never in the game's
 * save:
 * <ul>
 * <li>frame_rate_limit: 0..3 = 30 / 45 / 60 / display maximum, default 60 (NfsFrameRate);</li>
 * <li>render_scale: 0..5 = 50 / 60 / 70 / 80 / 90 / 100 % of the screen width for the 3D scene, default 100 %
 * (applies at once);</li>
 * <li>graphics_quality: 0..4 = engine tiers 1-4 and MAX (tier 4 with the port's effects), default MAX (applies at the
 * next start: the engine builds its quality profile once);</li>
 * <li>nitro_blur: 0..1 = off / on, a stronger motion blur while the nitrous burns, default off (applies at once);</li>
 * <li>colour_grading: 0..1 = off / on, the iOS colour grading of the MAX quality (the red and grey look of the
 * cutscenes and race results), default off (applies at once; stage 16).</li>
 * </ul>
 */
public final class NfsOptions {
    private static final String TAG = "NfsOptions";
    static final int FRAME_RATE_LIMIT = 0;
    static final int RENDER_SCALE = 1;
    static final int GRAPHICS_QUALITY = 2;
    static final int NITRO_BLUR = 3;
    static final int COLOUR_GRADING = 4;
    private static final String PREFS = "nfs_port";
    private static final String[] KEYS = {"frame_rate_limit", "render_scale", "graphics_quality", "nitro_blur",
            "colour_grading"};
    private static final int[] DEFAULTS = {2, 5, 4, 0, 0};
    private static final int[] LAST = {3, 5, 4, 1, 1};

    private static volatile Context sContext;
    private static Handler sMain;

    private NfsOptions() {}

    /**
     * NfsNative.loadGameLibraries, right after the bridge booted the ARM32 guest: before the OBB overlay is built
     * (NfsObb.installNative; the MAX-only tweak entries depend on the quality) and before the engine measures its
     * quality tier (nativeOnCreate).
     */
    public static void init(Context context) {
        sContext = context.getApplicationContext();
        sMain = new Handler(Looper.getMainLooper());
        SharedPreferences prefs = null;
        try {
            prefs = prefs(context);
        } catch (Throwable t) {
            NfsLog.w(TAG, "preferences unreadable, using the defaults: " + t);
        }
        StringBuilder log = new StringBuilder();
        for (int id = RENDER_SCALE; id <= COLOUR_GRADING; id++) {
            int value = DEFAULTS[id];
            try {
                if (prefs != null) value = prefs.getInt(KEYS[id], DEFAULTS[id]);
            } catch (Throwable t) {
                NfsLog.w(TAG, KEYS[id] + " unreadable: " + t);
            }
            if (value < 0 || value > LAST[id]) value = DEFAULTS[id];
            try {
                nativeSetOption(id, value);
            } catch (UnsatisfiedLinkError e) {
                NfsLog.w(TAG, "bridge natives missing, port options inactive: " + e);
                return;
            }
            log.append(id == RENDER_SCALE ? "" : ", ").append(KEYS[id]).append('=').append(value);
        }
        NfsLog.i(TAG, log.toString());
    }

    /** Bridge (GL thread): the player changed a row, or the settings went back to the defaults. */
    static void onOptionChanged(final int id, final int value) {
        if (id == FRAME_RATE_LIMIT) {
            NfsFrameRate.onLimitChanged(value);
            return;
        }
        Handler main = sMain;
        if (id < 0 || id >= KEYS.length || value < 0 || value > LAST[id] || main == null) return;
        main.post(new Runnable() {
            @Override
            public void run() {
                Context c = sContext;
                try {
                    if (c != null) prefs(c).edit().putInt(KEYS[id], value).apply();
                } catch (Throwable t) {
                    NfsLog.w(TAG, "cannot save " + KEYS[id] + ": " + t);
                }
                NfsLog.i(TAG, KEYS[id] + "=" + value + " saved" + (id == GRAPHICS_QUALITY ? " (applies at the next start)" : ""));
            }
        });
    }

    private static SharedPreferences prefs(Context context) {
        // NfsApplication redirects the application's preferences to the external data root (NfsStorage)
        return context.getApplicationContext().getSharedPreferences(PREFS, Context.MODE_PRIVATE);
    }

    private static native void nativeSetOption(int id, int value);
}
