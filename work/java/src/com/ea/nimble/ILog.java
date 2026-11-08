package com.ea.nimble;

/**
 * Nimble log facade, kept because the game's native Nimble bridge (libapp.so "CppBridge") writes its log
 * through it: {@code Log.getComponent().writeWithTitle(level, title, format, args)}. Besides it only the offline
 * network status (Network) and bridge.BaseNativeCallback remain. The rest of the original Nimble SDK (Synergy,
 * tracking, identity, friends, MTX, push, web view, notifications) is not in this build: its services are shut
 * down, and the native bridge skips calls into classes it cannot load (seen with the runtime JNI trace,
 * setprop debug.nfsbridge.jnitrace 1).
 */
public interface ILog {
    int LEVEL_VERBOSE = 100;
    int LEVEL_DEBUG = 200;
    int LEVEL_INFO = 300;
    int LEVEL_WARN = 400;
    int LEVEL_ERROR = 500;
    int LEVEL_FATAL = 600;

    void writeWithTitle(int level, String title, String format, Object... args);
}
