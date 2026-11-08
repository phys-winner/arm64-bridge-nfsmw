package com.ea.nimble;

import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.os.Handler;
import android.os.Looper;
import com.ea.ironmonkey.port.NfsLog;

import com.ea.ironmonkey.port.NfsApplication;

import java.io.File;
import java.io.FileInputStream;
import java.io.Serializable;
import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;
import java.text.SimpleDateFormat;
import java.util.ArrayList;
import java.util.Date;
import java.util.HashMap;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.TimeZone;

/**
 * The static Nimble utilities the game's native Nimble bridge resolves by name. Notifications ("nimble.notification.*")
 * go through an in-process bus that replaces the support library's LocalBroadcastManager: receivers registered for an
 * action get the Intent asynchronously on the main thread. The native NimbleWrapper registers its MTX listeners here
 * (catalog refresh finished, restore finished).
 */
public final class Utility {
    private static final String TAG = "Nimble";
    private static final Handler MAIN = new Handler(Looper.getMainLooper());
    private static final Map<String, List<BroadcastReceiver>> RECEIVERS = new HashMap<>();

    private Utility() {}

    public static void registerReceiver(String action, BroadcastReceiver receiver) {
        if (action == null || receiver == null) return;
        synchronized (RECEIVERS) {
            List<BroadcastReceiver> list = RECEIVERS.get(action);
            if (list == null) RECEIVERS.put(action, list = new ArrayList<>());
            if (!list.contains(receiver)) list.add(receiver);
        }
    }

    public static void unregisterReceiver(BroadcastReceiver receiver) {
        synchronized (RECEIVERS) {
            for (List<BroadcastReceiver> list : RECEIVERS.values()) list.remove(receiver);
        }
    }

    public static void sendBroadcast(String action, Map<String, String> extras) {
        final Intent intent = new Intent(action);
        if (extras != null) for (Map.Entry<String, String> e : extras.entrySet()) intent.putExtra(e.getKey(), e.getValue());
        deliver(intent);
    }

    public static void sendBroadcastSerializable(String action, Map<String, Serializable> extras) {
        final Intent intent = new Intent(action);
        if (extras != null) for (Map.Entry<String, Serializable> e : extras.entrySet()) intent.putExtra(e.getKey(), e.getValue());
        deliver(intent);
    }

    private static void deliver(final Intent intent) {
        final List<BroadcastReceiver> targets;
        synchronized (RECEIVERS) {
            List<BroadcastReceiver> list = RECEIVERS.get(intent.getAction());
            targets = list == null ? new ArrayList<BroadcastReceiver>() : new ArrayList<>(list);
        }
        MAIN.post(() -> {
            Context context = NfsApplication.context();
            for (BroadcastReceiver r : targets) r.onReceive(context, intent);
        });
    }

    public static String readFile(String path) {
        try (FileInputStream in = new FileInputStream(new File(path))) {
            byte[] data = new byte[(int) new File(path).length()];
            int n = 0;
            while (n < data.length) {
                int r = in.read(data, n, data.length - n);
                if (r < 0) break;
                n += r;
            }
            return new String(data, 0, n, StandardCharsets.UTF_8);
        } catch (Exception e) {
            return null;
        }
    }

    public static String getUTCDateStringFormat(Date date) {
        if (date == null) return "";
        SimpleDateFormat format = new SimpleDateFormat("yyyyMMdd_HHmmss", Locale.US);
        format.setTimeZone(TimeZone.getTimeZone("UTC"));
        return format.format(date);
    }

    public static String SHA256HashString(String text) {
        if (text == null) return null;
        try {
            byte[] digest = MessageDigest.getInstance("SHA-256").digest(text.getBytes(StandardCharsets.UTF_8));
            StringBuilder out = new StringBuilder(digest.length * 2);
            for (byte b : digest) out.append(String.format(Locale.US, "%02x", b));
            return out.toString();
        } catch (Exception e) {
            return null;
        }
    }

    public static void loadLibrary(String name) {
        try {
            System.loadLibrary(name);
        } catch (Throwable t) {
            NfsLog.w(TAG, "loadLibrary(" + name + "): " + t);
        }
    }
}
