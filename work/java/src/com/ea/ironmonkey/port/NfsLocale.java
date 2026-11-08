package com.ea.ironmonkey.port;

import android.app.LocaleManager;
import android.content.Context;
import android.os.Build;
import android.os.LocaleList;

import java.io.File;
import java.util.Locale;

/**
 * Per-app language (API 33+, android:localeConfig) through the game's own language path (modernization 5.1.2).
 *
 * The engine (Locale::init, libapp 0x3F0570) reads the user language from files/var/locale; only when that file is
 * missing does it derive the language from GameActivityMain.GetDeviceLocale() (Locale.getDefault()) and write it
 * back. The in-game Settings → Language selector rewrites the same file. So a language picked in the system's
 * per-app language settings is applied at the next cold start by dropping var/locale once, when the per-app
 * selection differs from the one seen at the previous start. Nothing reacts while the game runs (no hot swap);
 * an in-game choice stays in effect until the per-app selection changes again. API &lt; 33: no-op.
 *
 * GetDeviceLocale() reads {@link #engineLocale()}: the framework's default Locale follows a per-app language only
 * when the APK has Android resources for it (values-ru), so French, German, Japanese … would otherwise reach the
 * engine as the system language (final16K API 37 runs p09-loc-*).
 */
public final class NfsLocale {
    private static final String SEEN_FILE = "nfs_app_locales";
    private static volatile Locale sAppLocale;

    private NfsLocale() {}

    static void onStart(Context base) {
        if (Build.VERSION.SDK_INT < 33) return;
        File root = NfsStorage.filesDir();
        if (root == null) root = base.getFilesDir();
        String tags;
        try {
            LocaleManager lm = base.getSystemService(LocaleManager.class);
            LocaleList app = lm.getApplicationLocales();
            tags = app.toLanguageTags();
            sAppLocale = app.isEmpty() ? null : app.get(0);
        } catch (Throwable t) {
            NfsLog.w(NfsStorage.TAG, "per-app locales unavailable: " + t);
            return;
        }
        File seen = new File(root, SEEN_FILE);
        String old = null;
        try {
            if (seen.isFile()) old = new String(NfsStorage.readAll(seen), "UTF-8");
        } catch (Exception e) {
            NfsLog.w(NfsStorage.TAG, "cannot read " + seen + ": " + e);
        }
        if (old != null && !old.equals(tags)) {
            File user = new File(new File(root, "var"), "locale");
            boolean reset = user.delete();
            NfsLog.i(NfsStorage.TAG, "per-app language '" + old + "' -> '" + tags + "': game language re-derived from the device locale (var/locale removed=" + reset + ")");
        }
        if (old == null || !old.equals(tags)) {
            try {
                NfsStorage.writeAtomic(seen, tags.getBytes("UTF-8"));
            } catch (Exception e) {
                NfsLog.w(NfsStorage.TAG, "cannot write " + seen + ": " + e);
            }
        }
    }

    /** Locale for GameActivityMain.GetDeviceLocale(): the per-app language seen at process start, else the default. */
    public static Locale engineLocale() {
        Locale app = sAppLocale;
        return app != null ? app : Locale.getDefault();
    }
}
