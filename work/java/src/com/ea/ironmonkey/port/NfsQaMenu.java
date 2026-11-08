package com.ea.ironmonkey.port;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.Context;
import android.graphics.Typeface;
import android.opengl.GLSurfaceView;
import android.util.DisplayMetrics;
import android.util.TypedValue;
import android.view.Gravity;
import android.view.ViewGroup;
import android.widget.Button;
import android.widget.CheckBox;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;
import android.widget.Toast;

import java.util.concurrent.atomic.AtomicBoolean;

/**
 * Restoration QA menus (stage 9), in the game's language (NfsQaText). The STORE shows the game's own cards for the QA
 * cash packs (NfsQaStore); its help button ("?", top right) opens one section with the cheats and the unlock/complete
 * actions; the help button of the pause menu opens the cheats together with "Controls", which shows the game's
 * original help window. The bridge hooks the two help buttons (StoreScreen/PauseScreen::OnButton "HELP") and calls
 * onStoreHelp/onPauseHelp on the GL thread. Every effect is chosen by the player; irreversible ones ask first.
 */
public final class NfsQaMenu {
    private static final String TAG = "NfsQaMenu";
    private static final int[] TOGGLE_TEXT = {NfsQaText.FREE, NfsQaText.NITRO, NfsQaText.WEAK, NfsQaText.TRAFFIC};
    private static final int[] TOGGLE_OFFSETS = {
        NfsQa.OPT_EVERYTHING_IS_FREE, NfsQa.OPT_INFINITE_NITRO, NfsQa.OPT_WEAKEN_OPPONENTS, NfsQa.OPT_DISABLE_TRAFFIC,
    };
    private static final int[] ACTION_TEXT = {NfsQaText.EVENTS, NfsQaText.CARS, NfsQaText.AWARDS};
    private static final int[] ACTION_DESC = {NfsQaText.EVENTS_DESC, NfsQaText.CARS_DESC, NfsQaText.AWARDS_DESC};
    private static final int[] ACTIONS = {NfsQa.UNLOCK_ALL_EVENTS, NfsQa.UNLOCK_ALL_CARS, NfsQa.AWARD_ALL};

    private static Activity sActivity;
    private static GLSurfaceView sView;
    private static final AtomicBoolean sOpening = new AtomicBoolean();

    private NfsQaMenu() {}

    /** Called from GameActivityMain.onCreate once the GL view exists. */
    public static void attach(Activity activity, GLSurfaceView view) {
        sActivity = activity;
        sView = view;
    }

    /** Store screen help button (bridge hook, GL thread). */
    public static void onStoreHelp() {
        open(false);
    }

    /** Pause menu help button (bridge hook, GL thread). */
    public static void onPauseHelp() {
        open(true);
    }

    /** QA store item chosen on a store card: confirm on the UI thread, then exactly one of the two runs. */
    static void confirmGrant(final String title, final int amount, final Runnable grant, final Runnable cancel) {
        final Activity a = sActivity;
        if (a == null) {
            cancel.run();
            return;
        }
        final AtomicBoolean answered = new AtomicBoolean();
        a.runOnUiThread(() -> {
            AlertDialog dialog = new AlertDialog.Builder(a)
                    .setTitle(title)
                    .setMessage(String.format(NfsQaText.locale(), NfsQaText.get(NfsQaText.CONFIRM_GRANT), NfsQaText.money(amount)))
                    .setPositiveButton(NfsQaText.get(NfsQaText.GET), (d, w) -> {
                        if (answered.compareAndSet(false, true)) grant.run();
                    })
                    .setNegativeButton(NfsQaText.get(NfsQaText.CANCEL), null)
                    .create();
            dialog.setOnDismissListener(d -> {
                if (answered.compareAndSet(false, true)) cancel.run();
            });
            dialog.show();
        });
    }

    private static void open(final boolean pause) {
        final Activity a = sActivity;
        final GLSurfaceView view = sView;
        if (a == null || view == null || !sOpening.compareAndSet(false, true)) return;
        final boolean[] state = new boolean[TOGGLE_OFFSETS.length];
        final int[] pending = {TOGGLE_OFFSETS.length};
        for (int i = 0; i < TOGGLE_OFFSETS.length; i++) {
            final int index = i;
            NfsQa.post(view, NfsQa.GET_DEBUG_BYTE, TOGGLE_OFFSETS[i], rc -> {
                state[index] = rc > 0;
                if (--pending[0] == 0) a.runOnUiThread(() -> show(a, pause, state));
            });
        }
    }

    private static void show(final Activity a, final boolean pause, boolean[] state) {
        LinearLayout row = new LinearLayout(a);
        row.setOrientation(LinearLayout.HORIZONTAL);
        int pad = dp(a, 20);
        row.setPadding(pad, dp(a, 4), pad, 0);
        LinearLayout box = new LinearLayout(a);
        box.setOrientation(LinearLayout.VERTICAL);
        row.addView(box, new LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f));
        box.addView(header(a, NfsQaText.get(NfsQaText.CHEATS)));
        for (int i = 0; i < TOGGLE_OFFSETS.length; i++) {
            final int offset = TOGGLE_OFFSETS[i];
            final String name = NfsQaText.get(TOGGLE_TEXT[i]);
            CheckBox box1 = new CheckBox(a);
            box1.setText(name);
            box1.setChecked(state[i]);
            box1.setOnCheckedChangeListener((b, on) -> {
                NfsQa.post(sView, NfsQa.SET_DEBUG_BYTE, (offset << 8) | (on ? 1 : 0), null);
                NfsLog.i(TAG, "cheat " + offset + " -> " + on);
            });
            box.addView(box1);
        }
        if (!pause) {
            LinearLayout actions = new LinearLayout(a);
            actions.setOrientation(LinearLayout.VERTICAL);
            actions.setPadding(dp(a, 16), 0, 0, 0);
            row.addView(actions, new LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f));
            actions.addView(header(a, NfsQaText.get(NfsQaText.UNLOCKS)));
            for (int i = 0; i < ACTIONS.length; i++) {
                final int index = i;
                Button button = new Button(a, null, android.R.attr.borderlessButtonStyle);
                button.setText(NfsQaText.get(ACTION_TEXT[i]));
                button.setAllCaps(false);
                button.setGravity(Gravity.START | Gravity.CENTER_VERTICAL);
                button.setOnClickListener(v -> confirmAction(a, index));
                actions.addView(button);
            }
        }
        ScrollView scroll = new ScrollView(a);
        scroll.addView(row);
        AlertDialog.Builder builder = new AlertDialog.Builder(a)
                .setTitle(NfsQaText.get(pause ? NfsQaText.HELP : NfsQaText.TITLE))
                .setView(scroll)
                .setNegativeButton(NfsQaText.get(NfsQaText.CLOSE), null);
        if (pause) builder.setPositiveButton(NfsQaText.get(NfsQaText.CONTROLS),
                (d, w) -> NfsQa.post(sView, NfsQa.OPEN_PAUSE_HELP, 0, null));
        AlertDialog dialog = builder.create();
        dialog.setOnDismissListener(d -> sOpening.set(false));
        dialog.show();
        if (!pause) widen(a, dialog);
    }

    /** Landscape: the two columns get 80% of the screen width (the theme's dialog width wraps CJK/German labels). */
    private static void widen(Activity a, AlertDialog dialog) {
        DisplayMetrics metrics = a.getResources().getDisplayMetrics();
        if (metrics.widthPixels <= metrics.heightPixels || dialog.getWindow() == null) return;
        int width = Math.min((int) (metrics.widthPixels * 0.8f), dp(a, 760));
        dialog.getWindow().setLayout(width, ViewGroup.LayoutParams.WRAP_CONTENT);
    }

    private static TextView header(Context context, String text) {
        TextView view = new TextView(context);
        view.setText(text);
        view.setTypeface(Typeface.DEFAULT_BOLD);
        view.setPadding(0, dp(context, 12), 0, dp(context, 4));
        return view;
    }

    private static void confirmAction(final Activity a, final int index) {
        new AlertDialog.Builder(a)
                .setTitle(NfsQaText.get(ACTION_TEXT[index]))
                .setMessage(NfsQaText.get(ACTION_DESC[index]) + "\n\n" + NfsQaText.get(NfsQaText.IRREVERSIBLE))
                .setPositiveButton(NfsQaText.get(NfsQaText.APPLY), (d, w) -> NfsQa.post(sView, ACTIONS[index], 0, rc -> {
                    NfsLog.i(TAG, "action " + ACTIONS[index] + " -> " + rc);
                    toast(a, rc == 0 ? NfsQaText.get(NfsQaText.DONE)
                            : String.format(NfsQaText.locale(), NfsQaText.get(NfsQaText.FAILED), rc));
                }))
                .setNegativeButton(NfsQaText.get(NfsQaText.CANCEL), null)
                .show();
    }

    private static void toast(final Activity a, final String text) {
        a.runOnUiThread(() -> Toast.makeText(a, text, Toast.LENGTH_SHORT).show());
    }

    private static int dp(Context context, int value) {
        return (int) TypedValue.applyDimension(TypedValue.COMPLEX_UNIT_DIP, value, context.getResources().getDisplayMetrics());
    }
}
