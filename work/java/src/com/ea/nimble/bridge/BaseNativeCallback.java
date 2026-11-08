package com.ea.nimble.bridge;

import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.os.Bundle;

import java.util.HashMap;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;

/**
 * Java side of the native Nimble bridge's callbacks. libNimble.so's JNI_OnLoad caches the class loader through
 * this class, and libapp.so creates instances with a callback id ({@code <init>(I)V}). Kept from the original
 * Nimble SDK without the network/Synergy/SafetyNet callback interfaces, whose components are not in this build.
 * As in the original, native calls are made from a worker thread, never from the finalizer thread.
 */
public class BaseNativeCallback extends BroadcastReceiver {
    private static final ExecutorService WORKER = Executors.newSingleThreadExecutor(runnable -> {
        Thread thread = new Thread(runnable, "NimbleNativeCallback");
        thread.setDaemon(true);
        return thread;
    });

    private final int m_id;

    public BaseNativeCallback(int id) {
        m_id = id;
    }

    public static native void nativeCallback(int id, Object... args);

    public static native void nativeFinalize(int id);

    public static void sendNativeCallback(final int id, final Object... args) {
        WORKER.execute(() -> nativeCallback(id, args));
    }

    @Override
    public void onReceive(Context context, Intent intent) {
        HashMap<String, Object> extras = new HashMap<>();
        Bundle bundle = intent.getExtras();
        if (bundle != null) {
            for (String key : bundle.keySet()) extras.put(key, bundle.get(key));
        }
        sendNativeCallback(m_id, intent.getAction(), extras);
    }

    @Override
    protected void finalize() throws Throwable {
        super.finalize();
        finalizeLater(m_id);
    }

    /** Releases a native callback id from the worker thread (finalizers must not run guest code). */
    static void finalizeLater(final int id) {
        WORKER.execute(() -> nativeFinalize(id));
    }
}
