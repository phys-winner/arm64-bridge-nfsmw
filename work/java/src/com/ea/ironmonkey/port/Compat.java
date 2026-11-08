package com.ea.ironmonkey.port;

import android.app.PendingIntent;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.os.Build;

/**
 * Static drop-ins for platform calls whose contract changed with newer targetSdk
 * (lessons §6.14): call sites are rewritten mechanically from invoke-virtual/static
 * of the framework method to invoke-static of the helper with the same registers.
 */
public final class Compat {
    private static final int FLAG_IMMUTABLE = 0x04000000; // PendingIntent.FLAG_IMMUTABLE (API 23)
    private static final int FLAG_MUTABLE = 0x02000000;   // PendingIntent.FLAG_MUTABLE (API 31)
    private static final int RECEIVER_EXPORTED = 0x2;     // Context.RECEIVER_EXPORTED (API 33)

    private Compat() {}

    /** Adds FLAG_IMMUTABLE when the caller chose neither mutability flag (required on target 31+). */
    static int piFlags(int flags) {
        if (Build.VERSION.SDK_INT >= 23 && (flags & (FLAG_IMMUTABLE | FLAG_MUTABLE)) == 0) {
            flags |= FLAG_IMMUTABLE;
        }
        return flags;
    }

    public static PendingIntent getActivity(Context c, int requestCode, Intent intent, int flags) {
        return PendingIntent.getActivity(c, requestCode, intent, piFlags(flags));
    }

    public static PendingIntent getBroadcast(Context c, int requestCode, Intent intent, int flags) {
        return PendingIntent.getBroadcast(c, requestCode, intent, piFlags(flags));
    }

    public static PendingIntent getService(Context c, int requestCode, Intent intent, int flags) {
        return PendingIntent.getService(c, requestCode, intent, piFlags(flags));
    }

    /**
     * Context.registerReceiver(receiver, filter) with an explicit export flag (required on target 34+
     * for non-system broadcasts). EXPORTED keeps the original semantics of an unflagged registration.
     */
    public static Intent registerReceiver(Context c, BroadcastReceiver receiver, IntentFilter filter) {
        if (Build.VERSION.SDK_INT >= 33) {
            return c.registerReceiver(receiver, filter, RECEIVER_EXPORTED);
        }
        return c.registerReceiver(receiver, filter);
    }
}
