package com.ea.ironmonkey.port;

import android.graphics.Rect;
import android.os.Build;
import android.view.View;

import java.util.ArrayList;
import java.util.List;

/**
 * Gesture navigation vs. the game's touch zones (touch_controls.md T-2). Steering is a drag that starts on the
 * left half; with Android 10+ gesture navigation a drag that starts at the left or right screen edge becomes the
 * system Back gesture (in a race: an unwanted pause). The game view excludes one strip per side edge from system
 * gestures, 32 dp wide and at most 200 dp tall (the system limit per edge), centred at 60 % of the height where
 * the thumbs rest. The top part of each edge and the bottom edge keep their system gestures.
 */
public final class NfsTouch {
    private static final int STRIP_DP = 32;
    private static final int TALL_DP = 200;

    private NfsTouch() {}

    public static void install(View view) {
        if (Build.VERSION.SDK_INT < 29 || view == null) return;
        view.addOnLayoutChangeListener(new View.OnLayoutChangeListener() {
            @Override
            public void onLayoutChange(View v, int l, int t, int r, int b, int ol, int ot, int or, int ob) {
                if (r - l != or - ol || b - t != ob - ot) update(v);
            }
        });
        update(view);
    }

    private static void update(View v) {
        int w = v.getWidth();
        int h = v.getHeight();
        if (w <= 0 || h <= 0) return;
        float dp = v.getResources().getDisplayMetrics().density;
        int strip = Math.round(STRIP_DP * dp);
        int tall = Math.min(h, Math.round(TALL_DP * dp));
        int top = Math.max(0, Math.min(h - tall, Math.round(h * 0.6f - tall / 2f)));
        List<Rect> rects = new ArrayList<>(2);
        rects.add(new Rect(0, top, strip, top + tall));
        rects.add(new Rect(w - strip, top, w, top + tall));
        v.setSystemGestureExclusionRects(rects);
    }
}
