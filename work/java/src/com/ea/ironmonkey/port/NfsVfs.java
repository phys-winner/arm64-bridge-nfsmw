package com.ea.ironmonkey.port;

/** Natives: libnfsvfs.so on arm32 builds, the runtime bridge on arm64 builds. */
public final class NfsVfs {
    private NfsVfs() {}

    static native int nativeInstall(String virtualPath, int fd, long offset, long length);

    static native long[] nativeStats();
}
