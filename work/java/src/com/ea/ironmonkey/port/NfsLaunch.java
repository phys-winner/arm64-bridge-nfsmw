package com.ea.ironmonkey.port;

import android.app.Activity;
import android.app.AlertDialog;

/**
 * Launch check of GameActivityMain, the launcher activity (the androeed-injected PermissionsActivity and its
 * launcher alias are gone). GameActivityMain.onCreate calls blockIfNoData() right after super.onCreate(): with
 * game data the game starts as before; without it the activity only shows which file is missing and finishes
 * on OK, and each of its lifecycle overrides calls just the framework method while blocked() is true (no game
 * code, no native boot). No runtime permission is needed for the embedded OBB or for the app's own
 * Android/obb directory.
 */
public final class NfsLaunch {
    private static volatile boolean sBlocked;

    private NfsLaunch() {}

    public static boolean blockIfNoData(final Activity a) {
        if (NfsObb.resolveAgainIfMissing(a) != NfsObb.SOURCE_NONE) {
            sBlocked = false;
            return false;
        }
        sBlocked = true;
        NfsLog.e(NfsObb.TAG, "no game data: " + NfsObb.describe(a));
        new AlertDialog.Builder(a)
                .setMessage(NfsObb.missingMessage(a))
                .setCancelable(false)
                .setPositiveButton("OK", (dialog, which) -> a.finish())
                .show();
        return true;
    }

    public static boolean blocked() {
        return sBlocked;
    }
}
