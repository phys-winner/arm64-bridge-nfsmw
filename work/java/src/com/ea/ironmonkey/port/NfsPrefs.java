package com.ea.ironmonkey.port;

import android.content.SharedPreferences;
import android.util.Xml;

import org.json.JSONArray;
import org.json.JSONObject;
import org.xmlpull.v1.XmlPullParser;

import java.io.File;
import java.io.FileInputStream;
import java.io.InputStream;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.HashSet;
import java.util.Iterator;
import java.util.List;
import java.util.Map;
import java.util.Set;
import java.util.WeakHashMap;

/**
 * SharedPreferences stored as JSON under the external data root (modernization 5.1.1), because the framework
 * always places its own implementation in /data/data/&lt;pkg&gt;/shared_prefs. One instance per file, shared by all
 * callers of the same name. commit() and apply() both write synchronously through NfsStorage.writeAtomic
 * (temp file, fsync, rename); the files are a few hundred bytes.
 */
final class NfsPrefs implements SharedPreferences {
    private final File file;
    private final Map<String, Object> map;
    private final WeakHashMap<OnSharedPreferenceChangeListener, Object> listeners = new WeakHashMap<>();

    NfsPrefs(File file) {
        this.file = file;
        Map<String, Object> m = null;
        if (file.isFile()) {
            try {
                m = fromJson(new String(NfsStorage.readAll(file), "UTF-8"));
            } catch (Exception e) {
                NfsLog.e(NfsStorage.TAG, "prefs unreadable, starting empty: " + file, e);
            }
        }
        map = m != null ? m : new HashMap<String, Object>();
    }

    // --- serialisation -------------------------------------------------------------------------------------------

    static String toJson(Map<String, Object> m) throws Exception {
        JSONObject o = new JSONObject();
        for (Map.Entry<String, Object> e : m.entrySet()) {
            Object v = e.getValue();
            JSONObject t = new JSONObject();
            if (v instanceof String) {
                t.put("t", "s").put("v", v);
            } else if (v instanceof Boolean) {
                t.put("t", "b").put("v", v);
            } else if (v instanceof Integer) {
                t.put("t", "i").put("v", v);
            } else if (v instanceof Long) {
                t.put("t", "l").put("v", String.valueOf(v));
            } else if (v instanceof Float) {
                t.put("t", "f").put("v", String.valueOf(Float.floatToRawIntBits((Float) v)));
            } else if (v instanceof Set) {
                JSONArray a = new JSONArray();
                for (Object s : (Set<?>) v) a.put(s);
                t.put("t", "set").put("v", a);
            } else {
                continue;
            }
            o.put(e.getKey(), t);
        }
        return o.toString(1);
    }

    static Map<String, Object> fromJson(String s) throws Exception {
        JSONObject o = new JSONObject(s);
        Map<String, Object> m = new HashMap<>();
        for (Iterator<String> it = o.keys(); it.hasNext(); ) {
            String k = it.next();
            JSONObject t = o.getJSONObject(k);
            String ty = t.getString("t");
            if ("s".equals(ty)) m.put(k, t.getString("v"));
            else if ("b".equals(ty)) m.put(k, t.getBoolean("v"));
            else if ("i".equals(ty)) m.put(k, t.getInt("v"));
            else if ("l".equals(ty)) m.put(k, Long.parseLong(t.getString("v")));
            else if ("f".equals(ty)) m.put(k, Float.intBitsToFloat(Integer.parseInt(t.getString("v"))));
            else if ("set".equals(ty)) {
                JSONArray a = t.getJSONArray("v");
                Set<String> set = new HashSet<>();
                for (int i = 0; i < a.length(); i++) set.add(a.getString(i));
                m.put(k, set);
            }
        }
        return m;
    }

    /** Parses a framework shared_prefs XML file (map/string/boolean/int/long/float/set). */
    static Map<String, Object> fromFrameworkXml(File xml) throws Exception {
        Map<String, Object> m = new HashMap<>();
        InputStream in = new FileInputStream(xml);
        try {
            XmlPullParser p = Xml.newPullParser();
            p.setInput(in, "UTF-8");
            String setName = null;
            Set<String> set = null;
            for (int ev = p.getEventType(); ev != XmlPullParser.END_DOCUMENT; ev = p.next()) {
                if (ev == XmlPullParser.START_TAG) {
                    String tag = p.getName();
                    String name = p.getAttributeValue(null, "name");
                    String value = p.getAttributeValue(null, "value");
                    if ("string".equals(tag)) {
                        String text = p.nextText();
                        if (set != null) set.add(text);
                        else if (name != null) m.put(name, text);
                    } else if ("boolean".equals(tag) && name != null) {
                        m.put(name, Boolean.parseBoolean(value));
                    } else if ("int".equals(tag) && name != null) {
                        m.put(name, Integer.parseInt(value));
                    } else if ("long".equals(tag) && name != null) {
                        m.put(name, Long.parseLong(value));
                    } else if ("float".equals(tag) && name != null) {
                        m.put(name, Float.parseFloat(value));
                    } else if ("set".equals(tag) && name != null) {
                        setName = name;
                        set = new HashSet<>();
                    } else if (!"map".equals(tag) && !"null".equals(tag)) {
                        throw new IllegalStateException("unsupported prefs tag <" + tag + "> in " + xml);
                    }
                } else if (ev == XmlPullParser.END_TAG && "set".equals(p.getName()) && set != null) {
                    m.put(setName, set);
                    set = null;
                    setName = null;
                }
            }
        } finally {
            in.close();
        }
        return m;
    }

    // --- SharedPreferences ---------------------------------------------------------------------------------------

    @Override
    public synchronized Map<String, ?> getAll() {
        Map<String, Object> copy = new HashMap<>();
        for (Map.Entry<String, Object> e : map.entrySet()) {
            Object v = e.getValue();
            copy.put(e.getKey(), v instanceof Set ? new HashSet<>((Set<?>) v) : v);
        }
        return copy;
    }

    @Override
    public synchronized String getString(String key, String def) {
        Object v = map.get(key);
        return v instanceof String ? (String) v : def;
    }

    @Override
    @SuppressWarnings("unchecked")
    public synchronized Set<String> getStringSet(String key, Set<String> def) {
        Object v = map.get(key);
        return v instanceof Set ? new HashSet<>((Set<String>) v) : def;
    }

    @Override
    public synchronized int getInt(String key, int def) {
        Object v = map.get(key);
        if (v != null && !(v instanceof Integer)) throw new ClassCastException(key + " is not an int");
        return v != null ? (Integer) v : def;
    }

    @Override
    public synchronized long getLong(String key, long def) {
        Object v = map.get(key);
        if (v != null && !(v instanceof Long)) throw new ClassCastException(key + " is not a long");
        return v != null ? (Long) v : def;
    }

    @Override
    public synchronized float getFloat(String key, float def) {
        Object v = map.get(key);
        if (v != null && !(v instanceof Float)) throw new ClassCastException(key + " is not a float");
        return v != null ? (Float) v : def;
    }

    @Override
    public synchronized boolean getBoolean(String key, boolean def) {
        Object v = map.get(key);
        if (v != null && !(v instanceof Boolean)) throw new ClassCastException(key + " is not a boolean");
        return v != null ? (Boolean) v : def;
    }

    @Override
    public synchronized boolean contains(String key) {
        return map.containsKey(key);
    }

    @Override
    public Editor edit() {
        return new Ed();
    }

    @Override
    public synchronized void registerOnSharedPreferenceChangeListener(OnSharedPreferenceChangeListener l) {
        listeners.put(l, this);
    }

    @Override
    public synchronized void unregisterOnSharedPreferenceChangeListener(OnSharedPreferenceChangeListener l) {
        listeners.remove(l);
    }

    private final class Ed implements Editor {
        private final Map<String, Object> puts = new HashMap<>();
        private final Set<String> removes = new HashSet<>();
        private boolean clear;

        @Override public synchronized Editor putString(String k, String v) { return put(k, v); }
        @Override public synchronized Editor putStringSet(String k, Set<String> v) { return put(k, v == null ? null : new HashSet<>(v)); }
        @Override public synchronized Editor putInt(String k, int v) { return put(k, v); }
        @Override public synchronized Editor putLong(String k, long v) { return put(k, v); }
        @Override public synchronized Editor putFloat(String k, float v) { return put(k, v); }
        @Override public synchronized Editor putBoolean(String k, boolean v) { return put(k, v); }

        private Editor put(String k, Object v) {
            if (v == null) {
                removes.add(k);
                puts.remove(k);
            } else {
                puts.put(k, v);
                removes.remove(k);
            }
            return this;
        }

        @Override public synchronized Editor remove(String k) { removes.add(k); puts.remove(k); return this; }
        @Override public synchronized Editor clear() { clear = true; return this; }

        @Override
        public boolean commit() {
            List<String> changed = new ArrayList<>();
            List<OnSharedPreferenceChangeListener> ls;
            boolean ok;
            synchronized (NfsPrefs.this) {
                synchronized (this) {
                    if (clear) {
                        changed.addAll(map.keySet());
                        map.clear();
                    }
                    for (String k : removes) if (map.remove(k) != null) changed.add(k);
                    for (Map.Entry<String, Object> e : puts.entrySet()) {
                        Object old = map.put(e.getKey(), e.getValue());
                        if (old == null || !old.equals(e.getValue())) changed.add(e.getKey());
                    }
                    puts.clear();
                    removes.clear();
                    clear = false;
                }
                ok = true;
                if (!changed.isEmpty() || !file.exists()) {
                    try {
                        NfsStorage.writeAtomic(file, toJson(map).getBytes("UTF-8"));
                    } catch (Exception e) {
                        NfsLog.e(NfsStorage.TAG, "prefs write failed: " + file, e);
                        ok = false;
                    }
                }
                ls = new ArrayList<>(listeners.keySet());
            }
            for (String k : changed) for (OnSharedPreferenceChangeListener l : ls) l.onSharedPreferenceChanged(NfsPrefs.this, k);
            return ok;
        }

        @Override
        public void apply() {
            commit();
        }
    }
}
