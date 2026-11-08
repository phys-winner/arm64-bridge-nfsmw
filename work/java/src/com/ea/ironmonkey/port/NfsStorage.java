package com.ea.ironmonkey.port;

import android.content.Context;
import android.content.SharedPreferences;

import org.json.JSONArray;
import org.json.JSONObject;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.security.MessageDigest;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;

/**
 * Persistent data root (modernization 5.1.1). Everything the game and EA Nimble keep under /data/data/&lt;pkg&gt;
 * lives under Context.getExternalFilesDir(null) instead:
 * <pre>
 *   &lt;ext&gt;/                          &lt;- files/        engine saves (var/, var1/), EULA/telemetry flags, Nimble documents
 *   &lt;ext&gt;/cache/                    &lt;- cache/        Nimble cache persistence and log
 *   &lt;ext&gt;/shared_prefs/&lt;name&gt;.json  &lt;- shared_prefs/&lt;name&gt;.xml (NfsPrefs)
 * </pre>
 * The consumers get their directories from Context.getFilesDir()/getCacheDir()/getSharedPreferences() of the
 * Application (NfsApplication) or of GameActivityMain (the Activity handed to EAIO/EAMIO StorageDirectory), both
 * of which delegate here. Migration runs once in Application.attachBaseContext, before any consumer:
 * <pre>
 *   no state file -> copy every file (temp, fsync, rename), verify SHA-256 (prefs: parsed maps equal),
 *                    write state COPIED with the manifest
 *   COPIED        -> next start: originals unchanged since the copy and copies readable -> delete originals,
 *                    state DONE
 * </pre>
 * The state file files/nfs_storage.json is the only file left in /data/data/&lt;pkg&gt;/files. If the external root is
 * unavailable or any step fails, this process keeps using internal storage and the originals stay untouched.
 */
public final class NfsStorage {
    static final String TAG = "NfsStorage";
    private static final String STATE_FILE = "nfs_storage.json";
    private static final String TMP_SUFFIX = ".nfstmp";

    private static File sExtRoot;   // non-null only when the external root is active for this process
    private static final Map<String, NfsPrefs> sPrefs = new HashMap<>();

    private NfsStorage() {}

    // --- consumers --------------------------------------------------------------------------------------------------

    /** Redirected Context.getFilesDir(), or null to use the framework's internal directory. */
    public static synchronized File filesDir() {
        return sExtRoot;
    }

    /** Redirected Context.getCacheDir(), or null. */
    public static synchronized File cacheDir() {
        if (sExtRoot == null) return null;
        File d = new File(sExtRoot, "cache");
        if (!d.isDirectory() && !d.mkdirs()) NfsLog.w(TAG, "cannot create " + d);
        return d;
    }

    /** Redirected Context.getSharedPreferences(name, mode), or null. */
    public static synchronized SharedPreferences prefs(String name) {
        if (sExtRoot == null) return null;
        if (name == null) name = "null";
        NfsPrefs p = sPrefs.get(name);
        if (p == null) {
            p = new NfsPrefs(prefsFile(sExtRoot, name));
            sPrefs.put(name, p);
        }
        return p;
    }

    /** Redirected Context.deleteSharedPreferences(name); returns null when inactive. */
    public static synchronized Boolean deletePrefs(String name) {
        if (sExtRoot == null) return null;
        sPrefs.remove(name);
        File f = prefsFile(sExtRoot, name);
        return !f.exists() || f.delete();
    }

    private static File prefsFile(File ext, String name) {
        return new File(new File(ext, "shared_prefs"), name + ".json");
    }

    // --- migration --------------------------------------------------------------------------------------------------

    /** Called from NfsApplication.attachBaseContext with the framework context. */
    public static synchronized void init(Context base) {
        long t0 = System.currentTimeMillis();
        File ext;
        try {
            ext = base.getExternalFilesDir(null);
        } catch (Exception e) {
            ext = null;
        }
        File internalFiles = base.getFilesDir();
        File state = new File(internalFiles, STATE_FILE);
        if (ext == null || !(ext.isDirectory() || ext.mkdirs())) {
            NfsLog.e(TAG, "external data root unavailable; keeping internal storage " + internalFiles);
            return;
        }
        try {
            JSONObject st = state.isFile() ? new JSONObject(new String(readAll(state), "UTF-8")) : null;
            String phase = st != null ? st.optString("state") : "NONE";
            if ("DONE".equals(phase)) {
                sExtRoot = ext;
            } else if ("COPIED".equals(phase)) {
                sExtRoot = ext;
                finish(base, st, state);
            } else {
                List<Item> items = inventory(base, ext);
                JSONArray manifest = new JSONArray();
                for (Item it : items) manifest.put(copy(it));
                JSONObject out = new JSONObject();
                out.put("state", items.isEmpty() ? "DONE" : "COPIED");
                out.put("time", System.currentTimeMillis());
                out.put("root", ext.getAbsolutePath());
                out.put("files", manifest);
                writeAtomic(state, out.toString(1).getBytes("UTF-8"));
                sExtRoot = ext;
                NfsLog.i(TAG, "migrated " + items.size() + " files to " + ext + " (state "
                        + out.getString("state") + ")");
            }
        } catch (Exception e) {
            sExtRoot = null;
            NfsLog.e(TAG, "migration failed; keeping internal storage for this run", e);
        }
        NfsLog.i(TAG, "root=" + (sExtRoot != null ? sExtRoot : internalFiles) + " in "
                + (System.currentTimeMillis() - t0) + " ms");
    }

    private static final class Item {
        final File src;
        final File dst;
        final String rel;
        final boolean prefs;
        final boolean critical;

        Item(File src, File dst, String rel, boolean prefs, boolean critical) {
            this.src = src;
            this.dst = dst;
            this.rel = rel;
            this.prefs = prefs;
            this.critical = critical;
        }
    }

    private static List<Item> inventory(Context base, File ext) {
        List<Item> out = new ArrayList<>();
        File internalFiles = base.getFilesDir();
        walk(internalFiles, ext, "files/", out, new File(internalFiles, STATE_FILE), true);
        walk(base.getCacheDir(), new File(ext, "cache"), "cache/", out, null, false);
        File[] prefs = new File(base.getDataDir(), "shared_prefs").listFiles();
        if (prefs != null) {
            for (File f : prefs) {
                String n = f.getName();
                if (f.isFile() && n.endsWith(".xml")) {
                    String name = n.substring(0, n.length() - 4);
                    out.add(new Item(f, prefsFile(ext, name), "shared_prefs/" + n, true, true));
                }
            }
        }
        return out;
    }

    private static void walk(File dir, File dstDir, String rel, List<Item> out, File skip, boolean critical) {
        File[] list = dir != null ? dir.listFiles() : null;
        if (list == null) return;
        for (File f : list) {
            if (f.equals(skip)) continue;
            if (f.isDirectory()) {
                walk(f, new File(dstDir, f.getName()), rel + f.getName() + "/", out, skip, critical);
            } else if (f.isFile() && !f.getName().endsWith(TMP_SUFFIX)) {
                out.add(new Item(f, new File(dstDir, f.getName()), rel + f.getName(), false, critical));
            }
        }
    }

    /** Copies one item; a failure of a critical item (files/, shared_prefs/) aborts the migration. */
    private static JSONObject copy(Item it) throws Exception {
        byte[] data = readAll(it.src);
        String srcSha = sha256(data);
        try {
            copyVerified(it, data, srcSha);
        } catch (Exception e) {
            if (it.critical) throw e;
            NfsLog.w(TAG, "cache file not copied (disposable): " + it.rel + ": " + e);
            JSONObject j = new JSONObject();
            j.put("src", it.src.getAbsolutePath());
            j.put("rel", it.rel);
            j.put("size", data.length);
            j.put("sha256", srcSha);
            j.put("skipped", String.valueOf(e));
            return j;
        }
        JSONObject j = new JSONObject();
        j.put("src", it.src.getAbsolutePath());
        j.put("dst", it.dst.getAbsolutePath());
        j.put("rel", it.rel);
        j.put("size", data.length);
        j.put("sha256", srcSha);
        j.put("prefs", it.prefs);
        return j;
    }

    private static void copyVerified(Item it, byte[] data, String srcSha) throws Exception {
        if (it.prefs) {
            Map<String, Object> m = NfsPrefs.fromFrameworkXml(it.src);
            writeAtomic(it.dst, NfsPrefs.toJson(m).getBytes("UTF-8"));
            if (!NfsPrefs.fromJson(new String(readAll(it.dst), "UTF-8")).equals(m)) {
                throw new IOException("prefs verify mismatch " + it.dst);
            }
        } else {
            writeAtomic(it.dst, data);
            String dstSha = sha256(readAll(it.dst));
            if (!dstSha.equals(srcSha)) throw new IOException("hash mismatch " + it.dst);
        }
    }

    /** Second start after COPIED: delete the originals that nobody touched since the copy. */
    private static void finish(Context base, JSONObject st, File state) throws Exception {
        JSONArray files = st.getJSONArray("files");
        int deleted = 0, kept = 0;
        for (int i = 0; i < files.length(); i++) {
            JSONObject j = files.getJSONObject(i);
            File src = new File(j.getString("src"));
            if (!src.exists()) continue;
            boolean unchanged = sha256(readAll(src)).equals(j.getString("sha256"));
            boolean copyOk;
            if (j.has("skipped")) {
                copyOk = true;  // disposable cache entry, never copied
            } else {
                File dst = new File(j.getString("dst"));
                copyOk = dst.isFile() && dst.canRead();
            }
            if (unchanged && copyOk && src.delete()) {
                deleted++;
            } else {
                kept++;
                NfsLog.w(TAG, "keeping original " + src + " (unchanged=" + unchanged + ", copy=" + copyOk + ")");
            }
        }
        pruneEmpty(base.getFilesDir(), false);
        pruneEmpty(base.getCacheDir(), false);
        if (kept == 0) {
            st.put("state", "DONE");
            st.put("done_time", System.currentTimeMillis());
            writeAtomic(state, st.toString(1).getBytes("UTF-8"));
        }
        NfsLog.i(TAG, "originals deleted=" + deleted + " kept=" + kept + (kept == 0 ? " (state DONE)" : " (state COPIED)"));
    }

    private static boolean pruneEmpty(File dir, boolean self) {
        File[] list = dir != null ? dir.listFiles() : null;
        if (list == null) return false;
        boolean empty = true;
        for (File f : list) {
            if (!(f.isDirectory() && pruneEmpty(f, true))) empty = false;
        }
        return empty && self && dir.delete();
    }

    // --- file helpers -----------------------------------------------------------------------------------------------

    static byte[] readAll(File f) throws IOException {
        InputStream in = new FileInputStream(f);
        try {
            ByteArrayOutputStream bo = new ByteArrayOutputStream((int) Math.max(16, f.length()));
            byte[] buf = new byte[65536];
            for (int n; (n = in.read(buf)) > 0; ) bo.write(buf, 0, n);
            return bo.toByteArray();
        } finally {
            in.close();
        }
    }

    /** Writes via a temp file in the same directory, fsync, then rename over the destination. */
    static void writeAtomic(File dst, byte[] data) throws IOException {
        File dir = dst.getParentFile();
        if (dir != null && !dir.isDirectory() && !dir.mkdirs()) throw new IOException("mkdirs " + dir);
        File tmp = new File(dir, dst.getName() + TMP_SUFFIX);
        FileOutputStream out = new FileOutputStream(tmp);
        try {
            out.write(data);
            out.flush();
            out.getFD().sync();
        } finally {
            out.close();
        }
        if (!tmp.renameTo(dst)) {
            tmp.delete();
            throw new IOException("rename " + tmp + " -> " + dst);
        }
    }

    static String sha256(byte[] data) throws Exception {
        byte[] d = MessageDigest.getInstance("SHA-256").digest(data);
        StringBuilder sb = new StringBuilder(64);
        for (byte b : d) sb.append(String.format("%02x", b & 0xff));
        return sb.toString();
    }
}
