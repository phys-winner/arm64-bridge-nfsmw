package com.ea.nimble;

import java.util.IllegalFormatException;

/**
 * The Nimble log component for the native bridge. Same output as the original LogImpl with the shipped
 * res/xml/nimble_log.xml (level "error"): logcat tag "Nimble", messages "NIM_ERROR>title> text", nothing
 * below LEVEL_ERROR, no log file.
 */
public final class Log {
    private static final String TAG = "Nimble";
    private static final int THRESHOLD = ILog.LEVEL_ERROR;

    private static final ILog INSTANCE = new ILog() {
        @Override
        public void writeWithTitle(int level, String title, String format, Object... args) {
            if (level < THRESHOLD || format == null) return;
            String text = format;
            if (args != null && args.length > 0) {
                try {
                    text = String.format(format, args);
                } catch (IllegalFormatException e) {
                    text = format;
                }
            }
            String line = (level >= ILog.LEVEL_FATAL ? "NIM_FATAL" : "NIM_ERROR") + ">" + (title == null ? "" : title) + "> " + text;
            com.ea.ironmonkey.port.NfsLog.e(TAG, line);
        }
    };

    private Log() {}

    public static synchronized ILog getComponent() {
        return INSTANCE;
    }
}
