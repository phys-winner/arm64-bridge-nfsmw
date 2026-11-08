package com.ea.ironmonkey.port;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.DialogInterface;
import android.os.SystemClock;
import android.view.KeyEvent;

import java.io.File;
import java.util.Locale;

/**
 * Back at a menu root closes the game without asking (touch_controls.md T-7). The engine answers the Back key on
 * the map, store, garage and similar screens with Activity.finish() (libapp 0x36EE10 / 0x1A28B8 → 0x546370), and
 * GameActivityMain.onBackPressed finishes during splash/resume states. With gesture navigation an accidental
 * edge swipe is a Back key, so a finish() that follows a Back key within 1.5 s asks first, in the game's current
 * language (files/var/locale). The game's own EXIT GAME prompt (YES button, touch) and every other finish()
 * pass unchanged. In a race Back still opens the pause menu (no finish involved).
 */
public final class NfsBack {
    private static final long WINDOW_MS = 1500;
    private static volatile long sLastBackDown;
    private static volatile boolean sConfirmed;
    private static boolean sShowing;

    private NfsBack() {}

    /** Called first in GameActivityMain.onKeyDown. */
    public static void onKeyDown(int keyCode, KeyEvent event) {
        if (keyCode == KeyEvent.KEYCODE_BACK && event != null && event.getRepeatCount() == 0) {
            sLastBackDown = SystemClock.uptimeMillis();
        }
    }

    /** Called from GameActivityMain.finish(); true = the finish is replaced by a confirmation. */
    public static boolean interceptFinish(final Activity a) {
        if (sConfirmed) return false;
        long t = sLastBackDown;
        if (t == 0 || SystemClock.uptimeMillis() - t > WINDOW_MS) return false;
        sLastBackDown = 0;
        a.runOnUiThread(new Runnable() {
            @Override
            public void run() {
                show(a);
            }
        });
        return true;
    }

    private static void show(final Activity a) {
        if (sShowing || a.isFinishing()) return;
        String[] s = texts(gameLanguage(a));
        sShowing = true;
        new AlertDialog.Builder(a)
                .setTitle(s[0])
                .setMessage(s[1])
                .setPositiveButton(s[2], new DialogInterface.OnClickListener() {
                    @Override
                    public void onClick(DialogInterface d, int w) {
                        sConfirmed = true;
                        a.finish();
                    }
                })
                .setNegativeButton(s[3], null)
                .setOnDismissListener(new DialogInterface.OnDismissListener() {
                    @Override
                    public void onDismiss(DialogInterface d) {
                        sShowing = false;
                    }
                })
                .show();
    }

    /** Language id written by the engine (u16 length + UTF-8), e.g. "en", "de"; device language as fallback. */
    private static String gameLanguage(Activity a) {
        try {
            File f = new File(new File(a.getFilesDir(), "var"), "locale");
            byte[] b = NfsStorage.readAll(f);
            if (b.length >= 3) {
                int n = (b[0] & 0xff) | ((b[1] & 0xff) << 8);
                if (n > 0 && 2 + n <= b.length) return new String(b, 2, n, "UTF-8").toLowerCase(Locale.ROOT);
            }
        } catch (Exception ignored) {
            // no user language yet
        }
        return Locale.getDefault().getLanguage();
    }

    private static String[] texts(String lang) {
        if (lang.startsWith("fr")) return new String[] {"Quitter le jeu ?", "Votre progression est sauvegardée automatiquement.", "Quitter", "Continuer"};
        if (lang.startsWith("de")) return new String[] {"Spiel beenden?", "Dein Fortschritt wird automatisch gespeichert.", "Beenden", "Weiterspielen"};
        if (lang.startsWith("it")) return new String[] {"Uscire dal gioco?", "I tuoi progressi vengono salvati automaticamente.", "Esci", "Continua"};
        if (lang.startsWith("es")) return new String[] {"¿Salir del juego?", "Tu progreso se guarda automáticamente.", "Salir", "Seguir jugando"};
        if (lang.startsWith("nl")) return new String[] {"Spel afsluiten?", "Je voortgang wordt automatisch opgeslagen.", "Afsluiten", "Doorspelen"};
        if (lang.startsWith("pt")) return new String[] {"Sair do jogo?", "Seu progresso é salvo automaticamente.", "Sair", "Continuar jogando"};
        if (lang.startsWith("ru")) return new String[] {"Выйти из игры?", "Прогресс сохраняется автоматически.", "Выйти", "Продолжить"};
        if (lang.startsWith("ja")) return new String[] {"ゲームを終了しますか？", "進行状況は自動的に保存されます。", "終了", "続ける"};
        if (lang.startsWith("ko") || lang.startsWith("kr")) return new String[] {"게임을 종료하시겠습니까?", "진행 상황은 자동으로 저장됩니다.", "종료", "계속하기"};
        if (lang.startsWith("zh") || lang.startsWith("cn")) return new String[] {"退出游戏？", "游戏进度会自动保存。", "退出", "继续游戏"};
        return new String[] {"Exit game?", "Your progress is saved automatically.", "Exit", "Keep playing"};
    }
}
