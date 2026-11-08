package com.ea.ironmonkey.port;

import java.io.File;
import java.nio.charset.StandardCharsets;
import java.util.Locale;

/**
 * Texts of the restoration QA menus and of the QA store items in the game's 11 languages. The language is the game's
 * own (files/var/locale: u16 length + language id, written by Locale::init and the in-game Settings → Language), so
 * the menus follow the language the game shows. PRICE_FREE must fit the price field of the store card's BUY button
 * (about 8 glyphs); Russian uses the game's own abbreviation (UI_FREE "Беспл.").
 */
final class NfsQaText {
    static final int TITLE = 0, CHEATS = 1, FREE = 2, NITRO = 3, WEAK = 4, TRAFFIC = 5, UNLOCKS = 6, EVENTS = 7, CARS = 8,
            AWARDS = 9, EVENTS_DESC = 10, CARS_DESC = 11, AWARDS_DESC = 12, IRREVERSIBLE = 13, APPLY = 14, CANCEL = 15,
            CLOSE = 16, GET = 17, DONE = 18, FAILED = 19, HELP = 20, CONTROLS = 21, PRICE_FREE = 22, ITEM_DESC = 23,
            CONFIRM_GRANT = 24;

    private static final String[] LANGUAGES = {"en", "fr", "de", "it", "es", "nl", "pt", "ru", "ja", "ko", "zh"};

    private static final String[][] TEXT = {
        {   // en
            "Cheats and unlocks (QA)", "Cheats (active until the game is restarted)", "Everything is free",
            "Infinite nitrous", "Weaken opponents", "Disable traffic", "Unlock / complete (saved, cannot be undone)",
            "Unlock all events", "Unlock all cars", "Award all achievements",
            "Every event on the map becomes available.", "All cars become yours and appear in the garage.",
            "Every achievement is marked as earned, with its Speed Points.",
            "This changes your saved game and cannot be undone. Continue?", "Apply", "Cancel", "Close", "Get",
            "Done", "Failed (%d)", "Help", "Controls", "FREE", "QA item: added locally, no payment.",
            "Add %s to your wallet? This is a local QA grant; no payment is made.",
        },
        {   // fr
            "Triches et déblocages (QA)", "Triches (actives jusqu'au redémarrage du jeu)", "Tout est gratuit",
            "Nitro infini", "Adversaires affaiblis", "Désactiver la circulation",
            "Débloquer / terminer (sauvegardé, irréversible)", "Débloquer toutes les épreuves",
            "Débloquer toutes les voitures", "Obtenir tous les succès",
            "Toutes les épreuves de la carte deviennent disponibles.",
            "Toutes les voitures vous appartiennent et apparaissent au garage.",
            "Tous les succès sont marqués comme obtenus, avec leurs Speed Points.",
            "Cela modifie votre sauvegarde et est irréversible. Continuer ?", "Appliquer", "Annuler", "Fermer",
            "Obtenir", "Terminé", "Échec (%d)", "Aide", "Commandes", "GRATUIT",
            "Article QA : ajouté localement, sans paiement.",
            "Ajouter %s à votre portefeuille ? C'est un crédit QA local, sans paiement.",
        },
        {   // de
            "Cheats und Freischaltungen (QA)", "Cheats (aktiv bis zum Neustart des Spiels)", "Alles ist kostenlos",
            "Unendlich Nitro", "Gegner schwächen", "Verkehr deaktivieren",
            "Freischalten / abschließen (wird gespeichert, nicht umkehrbar)", "Alle Events freischalten",
            "Alle Autos freischalten", "Alle Erfolge erhalten", "Alle Events auf der Karte werden verfügbar.",
            "Alle Autos gehören dir und stehen in der Garage.",
            "Alle Erfolge werden als erreicht markiert, samt Speed Points.",
            "Dies ändert deinen Spielstand und kann nicht rückgängig gemacht werden. Fortfahren?", "Anwenden",
            "Abbrechen", "Schließen", "Erhalten", "Erledigt", "Fehlgeschlagen (%d)", "Hilfe", "Steuerung", "GRATIS",
            "QA-Artikel: wird lokal gutgeschrieben, ohne Bezahlung.",
            "%s deinem Konto gutschreiben? Das ist eine lokale QA-Gutschrift ohne Bezahlung.",
        },
        {   // it
            "Trucchi e sblocchi (QA)", "Trucchi (attivi fino al riavvio del gioco)", "Tutto è gratis",
            "Nitro infinito", "Avversari indeboliti", "Disattiva il traffico",
            "Sblocca / completa (salvato, irreversibile)", "Sblocca tutti gli eventi", "Sblocca tutte le auto",
            "Ottieni tutti gli obiettivi", "Tutti gli eventi sulla mappa diventano disponibili.",
            "Tutte le auto diventano tue e compaiono nel garage.",
            "Tutti gli obiettivi risultano ottenuti, con i relativi Speed Points.",
            "Questo modifica il salvataggio e non può essere annullato. Continuare?", "Applica", "Annulla", "Chiudi",
            "Ottieni", "Fatto", "Non riuscito (%d)", "Aiuto", "Comandi", "GRATIS",
            "Articolo QA: aggiunto localmente, senza pagamento.",
            "Aggiungere %s al portafoglio? È un accredito QA locale, senza pagamento.",
        },
        {   // es
            "Trucos y desbloqueos (QA)", "Trucos (activos hasta reiniciar el juego)", "Todo es gratis",
            "Nitro infinito", "Rivales debilitados", "Desactivar el tráfico",
            "Desbloquear / completar (se guarda, no se puede deshacer)", "Desbloquear todos los eventos",
            "Desbloquear todos los coches", "Conseguir todos los logros",
            "Todos los eventos del mapa quedan disponibles.",
            "Todos los coches pasan a ser tuyos y aparecen en el garaje.",
            "Todos los logros se marcan como conseguidos, con sus Speed Points.",
            "Esto modifica tu partida guardada y no se puede deshacer. ¿Continuar?", "Aplicar", "Cancelar",
            "Cerrar", "Obtener", "Hecho", "Error (%d)", "Ayuda", "Controles", "GRATIS",
            "Artículo QA: se añade localmente, sin pago.",
            "¿Añadir %s a tu cartera? Es una concesión QA local, sin pago.",
        },
        {   // nl
            "Cheats en ontgrendelingen (QA)", "Cheats (actief tot het spel opnieuw start)", "Alles is gratis",
            "Oneindige nitro", "Tegenstanders verzwakken", "Verkeer uitschakelen",
            "Ontgrendelen / voltooien (opgeslagen, onomkeerbaar)", "Alle evenementen ontgrendelen",
            "Alle auto's ontgrendelen", "Alle prestaties toekennen",
            "Alle evenementen op de kaart worden beschikbaar.", "Alle auto's worden van jou en staan in de garage.",
            "Alle prestaties worden als behaald gemarkeerd, met hun Speed Points.",
            "Dit wijzigt je opgeslagen spel en kan niet ongedaan worden gemaakt. Doorgaan?", "Toepassen",
            "Annuleren", "Sluiten", "Ontvangen", "Klaar", "Mislukt (%d)", "Help", "Besturing", "GRATIS",
            "QA-item: lokaal toegevoegd, zonder betaling.",
            "%s aan je portemonnee toevoegen? Dit is een lokale QA-toekenning zonder betaling.",
        },
        {   // pt (Brasil)
            "Trapaças e desbloqueios (QA)", "Trapaças (ativas até reiniciar o jogo)", "Tudo é grátis",
            "Nitro infinito", "Oponentes enfraquecidos", "Desativar o trânsito",
            "Desbloquear / concluir (salvo, não pode ser desfeito)", "Desbloquear todos os eventos",
            "Desbloquear todos os carros", "Conceder todas as conquistas",
            "Todos os eventos do mapa ficam disponíveis.",
            "Todos os carros passam a ser seus e aparecem na garagem.",
            "Todas as conquistas são marcadas como obtidas, com os Speed Points.",
            "Isto altera seu jogo salvo e não pode ser desfeito. Continuar?", "Aplicar", "Cancelar", "Fechar",
            "Obter", "Concluído", "Falhou (%d)", "Ajuda", "Controles", "GRÁTIS",
            "Item QA: adicionado localmente, sem pagamento.",
            "Adicionar %s à sua carteira? É uma concessão QA local, sem pagamento.",
        },
        {   // ru
            "Читы и открытия (QA)", "Читы (действуют до перезапуска игры)", "Всё бесплатно", "Бесконечное нитро",
            "Ослабить соперников", "Отключить трафик", "Открыть / завершить (сохраняется, необратимо)",
            "Открыть все события", "Открыть все машины", "Получить все награды",
            "Все события на карте станут доступны.", "Все машины станут вашими и появятся в гараже.",
            "Все награды будут отмечены как полученные, вместе с очками SP.",
            "Это изменит сохранение, отменить будет нельзя. Продолжить?", "Применить", "Отмена", "Закрыть",
            "Получить", "Готово", "Не удалось (%d)", "Помощь", "Управление", "БЕСПЛ.",
            "QA-предмет: начисляется локально, без оплаты.",
            "Зачислить %s? Это локальная QA-выдача, оплата не производится.",
        },
        {   // ja
            "チートとアンロック (QA)", "チート (ゲームを再起動するまで有効)", "すべて無料", "ニトロ無限",
            "ライバルを弱くする", "交通をオフ", "アンロック / コンプリート (セーブされ、元に戻せません)",
            "すべてのイベントをアンロック", "すべての車をアンロック", "すべての実績を獲得",
            "マップ上のすべてのイベントが利用可能になります。", "すべての車があなたのものになり、ガレージに入ります。",
            "すべての実績が獲得済みになり、スピードポイントも加算されます。",
            "セーブデータが変更され、元に戻せません。続けますか？", "適用", "キャンセル", "閉じる", "受け取る",
            "完了", "失敗しました (%d)", "ヘルプ", "操作方法", "無料", "QAアイテム：ローカルで付与され、支払いは発生しません。",
            "%s を所持金に追加しますか？ローカルのQA付与で、支払いは発生しません。",
        },
        {   // ko
            "치트 및 잠금 해제 (QA)", "치트 (게임을 다시 시작할 때까지 적용)", "모든 것이 무료", "무한 니트로",
            "상대 약화", "교통 끄기", "잠금 해제 / 완료 (저장되며 되돌릴 수 없음)", "모든 이벤트 잠금 해제",
            "모든 차량 잠금 해제", "모든 업적 획득", "지도의 모든 이벤트를 이용할 수 있게 됩니다.",
            "모든 차량이 내 것이 되어 차고에 들어옵니다.", "모든 업적이 획득 처리되고 스피드 포인트가 지급됩니다.",
            "저장 데이터가 변경되며 되돌릴 수 없습니다. 계속하시겠습니까?", "적용", "취소", "닫기", "받기", "완료",
            "실패 (%d)", "도움말", "조작법", "무료", "QA 아이템: 로컬로 지급되며 결제가 없습니다.",
            "%s을(를) 지갑에 추가할까요? 로컬 QA 지급이며 결제는 이루어지지 않습니다.",
        },
        {   // zh (简体)
            "作弊与解锁 (QA)", "作弊 (在重新启动游戏前有效)", "全部免费", "无限氮气", "削弱对手", "关闭交通",
            "解锁 / 完成 (将保存，无法撤销)", "解锁所有赛事", "解锁所有车辆", "获得所有成就", "地图上的所有赛事都将开放。",
            "所有车辆都将归你所有并出现在车库中。", "所有成就都将标记为已获得，并获得相应的速度点数。",
            "这会修改你的存档，且无法撤销。是否继续？", "应用", "取消", "关闭", "领取", "完成", "失败 (%d)", "帮助",
            "操作说明", "免费", "QA 物品：本地发放，无需付款。", "将 %s 加入你的钱包？这是本地 QA 发放，不会产生付款。",
        },
    };

    /** The game's own pack titles (published/stringdata/<LANG>/nfsmw_android.sb, STORE_TITLE_PACK_1..5). */
    private static final String[][] PACK_TITLES = {
        {"Rookie Cash Pack", "Gearhead Cash Pack", "Street Racer Cash Pack", "Speed Demon Cash Pack", "Most Wanted Cash Pack"},
        {"Pack cash Débutant", "Pack cash Fana de voitures", "Pack cash Pilote de rue", "Pack cash Fou du volant",
         "Pack cash Most Wanted"},
        {"Rookie Cash-Paket", "Autofreak Cash-Paket", "Streetracer Cash-Paket", "Temporausch Cash-Paket", "Most Wanted Cash-Paket"},
        {"Cash Pack Principiante", "Cash Pack Smanettone", "Cash Pack Pilota", "Cash Pack Speed Demon", "Cash Pack Most Wanted"},
        {"Pack de efectivo para novatos", "Pack de efectivo para aficionados al motor", "Pack de efectivo para pilotos callejeros",
         "Pack de efectivo para fanáticos de la velocidad", "Pack de efectivo para Most Wanted"},
        {"Cashpakket Rookie", "Cashpakket Verzamelaar", "Cashpakket Straatracer", "Cashpakket Snelheidsduivel",
         "Cashpakket Most Wanted"},
        {"Pacote de Dinheiro Novato", "Pacote de Dinheiro Louco por Carros", "Pacote de Dinheiro Corredor de Rua",
         "Pacote de Dinheiro Demônio da Velocidade", "Pacote de Dinheiro Most Wanted"},
        {"Набор Rookie Cash", "Набор Gearhead Cash", "Набор Street Racer Cash", "Набор Speed Demon Cash", "Набор Most Wanted Cash"},
        {"ルーキー キャッシュパック", "ギアヘッド キャッシュパック", "ストリートレーサー キャッシュパック", "スピードデーモン キャッシュパック",
         "モスト・ウォンテッド キャッシュパック"},
        {"루키 캐시 팩", "기어헤드 캐시 팩", "스트리트 레이서 캐시 팩", "스피드 데몬 캐시 팩", "모스트 원티드 캐시 팩"},
        {"新手现金组合包", "车迷现金组合包", "街头赛车手现金组合包", "极速怪兽现金组合包", "最高通缉现金组合包"},
    };

    private NfsQaText() {}

    static String packTitle(int pack) {
        return PACK_TITLES[language()][pack];
    }

    /** Index of the game's current language in LANGUAGES (English when unknown). */
    static int language() {
        String id = null;
        File root = NfsStorage.filesDir();
        if (root != null) {
            try {
                byte[] raw = NfsStorage.readAll(new File(new File(root, "var"), "locale"));
                if (raw.length >= 2) {
                    int n = (raw[0] & 0xFF) | ((raw[1] & 0xFF) << 8);
                    if (n > 0 && 2 + n <= raw.length) id = new String(raw, 2, n, StandardCharsets.UTF_8);
                }
            } catch (Exception ignored) {
            }
        }
        if (id == null) id = NfsLocale.engineLocale().getLanguage();
        id = id.toLowerCase(Locale.ROOT);
        if (id.startsWith("cn") || id.startsWith("zh")) return 10;
        if (id.startsWith("kr")) return 9;
        for (int i = 0; i < LANGUAGES.length; i++) if (id.startsWith(LANGUAGES[i])) return i;
        return 0;
    }

    static String get(int key) {
        return TEXT[language()][key];
    }

    static Locale locale() {
        String tag = LANGUAGES[language()];
        return "pt".equals(tag) ? new Locale("pt", "BR") : "zh".equals(tag) ? Locale.SIMPLIFIED_CHINESE : new Locale(tag);
    }

    /** The game's FORMAT_CASH per language ("${0:n}", "{0:n} $", ...), in LANGUAGES order. */
    private static final String[] CASH = {"$%s", "%s $", "%s $", "%s$", "%s $", "$%s", "$%s", "%s$", "$%s", "$%s", "$%s"};

    static String money(int amount) {
        return String.format(CASH[language()], String.format(locale(), "%,d", amount));
    }
}
