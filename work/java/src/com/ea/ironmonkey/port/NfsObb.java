package com.ea.ironmonkey.port;

import android.content.Context;
import android.content.pm.PackageInfo;
import android.content.res.AssetFileDescriptor;
import android.os.ParcelFileDescriptor;
import android.system.Os;

import java.io.File;
import java.io.FileDescriptor;
import java.io.RandomAccessFile;

/**
 * Selects the game data source for the IronMonkey OBB zip.
 *
 * Priority (stage 15): an external file installed in getObbDir() (Android/obb/<package>/<name>), then the whole
 * OBB embedded as a STORED asset (assets/obb/<name>) inside the installed APK set. Both are read in place through
 * NfsVfs (no extraction, the OBB overlay applies); the external file falls back to a direct open by the engine.
 * No source is tied to a size: either one is taken when it is a whole zip, so a modified cache of any size works
 * external as well as embedded (the engine's assets/obb.size marker, which pinned the original 623470192 bytes, is
 * gone; GameActivityMain selects OBB mode without it).
 */
public final class NfsObb {
    public static final String TAG = "NfsObb";
    public static final int SOURCE_NONE = 0;
    public static final int SOURCE_EMBEDDED = 1;
    public static final int SOURCE_EXTERNAL = 2;
    public static final String ASSET_DIR = "obb/";
    public static final String VIRTUAL_DIR = "/nfsvfs-embedded/";

    private static int sSource = -1;
    private static String sPath;
    private static String sDetail = "";
    private static AssetFileDescriptor sAfd;
    private static boolean sNativeInstalled;

    private NfsObb() {}

    public static String obbName(Context ctx) {
        int vc = 0;
        try {
            PackageInfo pi = ctx.getPackageManager().getPackageInfo(ctx.getPackageName(), 0);
            vc = pi.versionCode;
        } catch (Exception e) {
            NfsLog.e(TAG, "versionCode: " + e);
        }
        return "main." + vc + "." + ctx.getPackageName() + ".obb";
    }

    /** Resolve the source once per process. Returns SOURCE_* (never throws). */
    public static synchronized int resolve(Context ctx) {
        if (sSource >= 0) return sSource;
        Context app = ctx.getApplicationContext() != null ? ctx.getApplicationContext() : ctx;
        String name = obbName(app);
        File ext = new File(app.getObbDir(), name);
        // An installed external cache wins: the original data or a modified cache of any size, as long as the file is
        // a whole zip (end record and central directory inside the file). A copy that is still in progress has no end
        // record yet and is not taken; the embedded data serves meanwhile.
        if (ext.isFile() && isWholeZip(ext)) {
            sSource = SOURCE_EXTERNAL;
            sPath = ext.getAbsolutePath();
            sDetail = "external " + sPath + " length=" + ext.length();
            NfsLog.i(TAG, "source=EXTERNAL " + sDetail);
            return sSource;
        }
        String externalProblem = "external " + ext.getAbsolutePath()
                + (ext.isFile() ? " (length " + ext.length() + ") is not a whole zip" : ext.exists() ? " is not a file" : " is missing");
        String embeddedProblem;
        try {
            AssetFileDescriptor afd = app.getAssets().openFd(ASSET_DIR + name);
            long len = afd.getLength();
            if (isWholeZip(afd.getFileDescriptor(), afd.getStartOffset(), len)) {
                sAfd = afd;
                sSource = SOURCE_EMBEDDED;
                sPath = VIRTUAL_DIR + name;
                sDetail = "embedded " + ASSET_DIR + name + " offset=" + afd.getStartOffset() + " length=" + len
                        + " (" + externalProblem + ")";
                NfsLog.i(TAG, "source=EMBEDDED " + sDetail);
                return sSource;
            }
            embeddedProblem = "embedded " + ASSET_DIR + name + " (length " + len + ") is not a whole zip";
            afd.close();
        } catch (Exception e) {
            embeddedProblem = "embedded not available: " + e.getClass().getSimpleName() + ": " + e.getMessage();
        }
        sSource = SOURCE_NONE;
        sPath = ext.getAbsolutePath();
        sDetail = externalProblem + "; " + embeddedProblem;
        NfsLog.e(TAG, "source=NONE " + sDetail);
        return sSource;
    }

    static boolean isWholeZip(File file) {
        RandomAccessFile f = null;
        try {
            f = new RandomAccessFile(file, "r");
            return isWholeZip(f.getFD(), 0, f.length());
        } catch (Exception e) {
            NfsLog.w(TAG, "external " + file + ": " + e);
            return false;
        } finally {
            if (f != null) {
                try {
                    f.close();
                } catch (Exception ignored) {
                }
            }
        }
    }

    /**
     * A zip whose end record (no comment) is the last 22 bytes of [offset, offset + length) of fd and whose central
     * directory lies inside that range. pread leaves the descriptor's position alone (the embedded one is the APK's,
     * handed to NfsVfs later).
     */
    static boolean isWholeZip(FileDescriptor fd, long offset, long length) throws Exception {
        if (length < 22) return false;
        byte[] end = new byte[22];
        for (int done = 0; done < end.length; ) {
            int n = Os.pread(fd, end, done, end.length - done, offset + length - end.length + done);
            if (n <= 0) return false;
            done += n;
        }
        if (le32(end, 0) != 0x06054b50L) return false;
        long cdSize = le32(end, 12), cdOffset = le32(end, 16);
        return cdSize > 0 && cdOffset + cdSize <= length - 22;
    }

    private static long le32(byte[] b, int at) {
        return (b[at] & 0xFFL) | (b[at + 1] & 0xFFL) << 8 | (b[at + 2] & 0xFFL) << 16 | (b[at + 3] & 0xFFL) << 24;
    }

    /** Launch check: a missing source is looked up again (the file may have been copied since the last check). */
    public static synchronized int resolveAgainIfMissing(Context ctx) {
        if (sSource == SOURCE_NONE) sSource = -1;
        return resolve(ctx);
    }

    public static synchronized String describe(Context ctx) {
        resolve(ctx);
        return sDetail;
    }

    public static String missingMessage(Context ctx) {
        String name = obbName(ctx);
        return "Game data was not found.\n\nInstall the complete game package (with embedded data), or copy\n"
                + name + "\ninto\nAndroid/obb/" + ctx.getPackageName() + "/\n\nDetails: " + describe(ctx);
    }

    /** Path handed to the engine (GameActivityMain.getObbFullPath). */
    public static synchronized String getPath(Context ctx) {
        resolve(ctx);
        return sPath;
    }

    /**
     * Must run after libapp.so is loaded and before nativeOnCreate(). The embedded container is always read
     * through NfsVfs; the external file (of the lite build, or one installed next to a full build) is registered
     * too (stage 8), so the OBB overlay (assets/obb_overlay.txt) applies to both. If the external registration
     * fails the engine still opens the file directly.
     */
    public static synchronized boolean installNative(Context ctx) {
        int source = resolve(ctx);
        if (source == SOURCE_NONE) return false;
        if (sNativeInstalled) return true;
        try {
            System.loadLibrary("nfsport");
        } catch (UnsatisfiedLinkError e) {
            NfsLog.i(TAG, "libnfsport not packaged; expecting natives from the runtime bridge: " + e.getMessage());
        }
        int rc;
        try {
            if (source == SOURCE_EMBEDDED) {
                ParcelFileDescriptor pfd = sAfd.getParcelFileDescriptor();
                rc = NfsVfs.nativeInstall(sPath, pfd.getFd(), sAfd.getStartOffset(), sAfd.getLength());
            } else {
                File file = new File(sPath);
                ParcelFileDescriptor pfd = ParcelFileDescriptor.open(file, ParcelFileDescriptor.MODE_READ_ONLY);
                try {
                    rc = NfsVfs.nativeInstall(sPath, pfd.getFd(), 0, file.length());
                } finally {
                    pfd.close();
                }
            }
        } catch (UnsatisfiedLinkError e) {
            NfsLog.e(TAG, "NfsVfs natives unavailable: " + e);
            return source == SOURCE_EXTERNAL;
        } catch (Exception e) {
            NfsLog.e(TAG, "NfsVfs install failed: " + e);
            return source == SOURCE_EXTERNAL;
        }
        sNativeInstalled = rc == 0;
        NfsLog.i(TAG, "NfsVfs.install rc=" + rc + " path=" + sPath);
        return sNativeInstalled || source == SOURCE_EXTERNAL;
    }

    public static String stats() {
        try {
            long[] s = NfsVfs.nativeStats();
            return "opens=" + s[0] + " readBytes=" + s[1] + " installed=" + s[2];
        } catch (Throwable t) {
            return "n/a";
        }
    }
}
