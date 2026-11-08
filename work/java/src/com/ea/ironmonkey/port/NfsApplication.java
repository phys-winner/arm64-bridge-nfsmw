package com.ea.ironmonkey.port;

import android.app.Application;
import android.content.Context;
import android.content.SharedPreferences;

import java.io.File;

/**
 * Application class of the port (replaces android.support.multidex.MultiDexApplication, a no-op since minSdk 24
 * has native multidex). Migrates the persistent data to the external root before any component runs and routes
 * the application context's files/cache/shared_prefs there (NfsStorage); the engine gets its paths from
 * GameActivityMain, which delegates the same way. Also applies a changed per-app language (API 33+) through the
 * game's own language file before the engine starts (NfsLocale).
 */
public class NfsApplication extends Application {
    private static volatile Context sContext;

    /** The application context (Nimble notification bus, QA texts); null before attachBaseContext. */
    public static Context context() {
        return sContext;
    }

    @Override
    protected void attachBaseContext(Context base) {
        super.attachBaseContext(base);
        sContext = this;
        NfsStorage.init(base);
        NfsLocale.onStart(base);
    }

    @Override
    public File getFilesDir() {
        File f = NfsStorage.filesDir();
        return f != null ? f : super.getFilesDir();
    }

    @Override
    public File getCacheDir() {
        File f = NfsStorage.cacheDir();
        return f != null ? f : super.getCacheDir();
    }

    @Override
    public SharedPreferences getSharedPreferences(String name, int mode) {
        SharedPreferences p = NfsStorage.prefs(name);
        return p != null ? p : super.getSharedPreferences(name, mode);
    }

    @Override
    public boolean deleteSharedPreferences(String name) {
        Boolean r = NfsStorage.deletePrefs(name);
        return r != null ? r : super.deleteSharedPreferences(name);
    }
}
