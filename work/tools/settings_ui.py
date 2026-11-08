"""The port's rows in the game's own settings pages (data half; make_obb_overlay.py writes it). Stage 10 added the
frame rate limit to the DISPLAY frame; stage 13 moves it into a GRAPHICS frame of its own, with the render scale, the
graphics quality and the nitrous motion blur; stage 16 adds the iOS colour grading switch under the nitrous blur.

The engine builds its settings screens from published/layouts/layouts.sb. Every option row is a widget layout whose
class is chosen by name (CreateCustomWidget 0x394630, registry filled at 0x38E000..0x394000) and bound to a game option
by the entity property OptionName: RadioOptionWidget (ctor 0x3781B8) looks the option up with Options::FindOption
0x24D3A4, casts it to IntOption, highlights the child whose RadioIndex equals the value (Update 0x378AE8) and sets the
child's RadioIndex on a tap (vtable +0x138 0x378BC8 -> IntOption::Set 0x249EA0). The bridge (namespace portopts in
arm64_bridge/src/options.cpp) answers FindOption for the port's option names with IntOptions of its own.

Data added here, append-only (sbinw.py; every original object keeps its bytes except the moved entities):
  layouts.sb  five RadioOption widgets made like the game's ON/OFF and KPH/MPH switches (the OnOff layout
              btn_options_settings_hud cloned: btn_options_small2 buttons in one row, each with a RadioIndex and a
              Buttonname label):
                btn_options_settings_quality    1 / 2 / 3 / 4 / MAX       GraphicsQuality  (RadioIndex 0..4)
                btn_options_settings_scale      50% .. 100%               RenderScale      (RadioIndex 0..5)
                btn_options_settings_fps        30 / 45 / 60 / MAX        FrameRateLimit   (RadioIndex 0..3)
                btn_options_settings_nitroblur  ON / OFF                  NitroMotionBlur  (RadioIndex 1 / 0, the
                                                                          game's own ON and OFF strings)
                btn_options_settings_grading    ON / OFF                  ColourGrading    (RadioIndex 1 / 0, as the
                                                                          nitrous row; its note: MAX quality only)
              the four settings pages (main menu and pause menu, with and without the language row) get a GRAPHICS
              frame between DISPLAY and AUDIO, drawn with the page's own frame, titles and row metrics: frame title
              OPTIONS_TITLE_GRAPHICS (a copy of TITLE_DISPLAY), per row a centred title (a copy of TITLE_SPEEDOMETER)
              and the widget (a copy of the page's speedometer entity with the row's OptionName), and under the quality
              buttons a note in the look of the game's control hints (mw_black_10, grey 119/122/123): the quality
              takes effect at the next start. Everything below DISPLAY moves down by the frame's height; the page's
              extent (HelperRect of the main menu pages) and the entity that embeds the page (PLACE_HOLDER /
              PLACE_HOLDER_MULTILANG of options_detailed and paused_options_detailed) grow by the same amount,
              because ScrollAreaVertical takes its scroll range from that height (stage 10, p20a)
  strings     not in the OBB's string tables any more: assets/port_strings.txt ("LANGUAGE<TAB>ID<TAB>TEXT"), which
              the bridge adds to the table the engine loads (arm64_bridge/src/portstrings.cpp), so a modified cache
              keeps its own tables
"""
from __future__ import annotations

import struct

import sbinw

TEMPLATE = "btn_options_settings_hud"  # OnOff: two btn_options_small2 buttons, the look of the new rows
BUTTON_HEIGHT = 32.0
FRAME_TITLE_GAP = 14.0  # frame title (mw_black_14, y = top of the text) to the frame's top edge, as DISPLAY and AUDIO
NOTE_GAP = 2.0          # quality buttons' bottom edge to the top of the note
NOTE_SPACE = 14.0       # extra height of the quality row for its note (mw_black_10)
NOTE_FONT = "mw_black_10"
NOTE_COLOR = (119, 122, 123, 255)  # the control hints of options_help_swipe / options_help_tilt
WIDE = "BTN_MUSIC"       # rows of 4 to 6 buttons: the width of the page's full rows (MUSIC, SFX)
NARROW = (0.175711, 0.821705)  # ON / OFF: the width of the SPEEDOMETER and HUD switches of the single-language pages

# widget layout -> (default width, [(child entity id, string id, RadioIndex)])
WIDGETS = {
    "btn_options_settings_quality": (356, [("_tier1", "OPTIONS_BUTTON_GFX_1", 0), ("_tier2", "OPTIONS_BUTTON_GFX_2", 1),
                                           ("_tier3", "OPTIONS_BUTTON_GFX_3", 2), ("_tier4", "OPTIONS_BUTTON_GFX_4", 3),
                                           ("_max", "OPTIONS_BUTTON_GFX_MAX", 4)]),
    "btn_options_settings_scale": (356, [(f"_scale{p}", f"OPTIONS_BUTTON_SCALE_{p}", k)
                                         for k, p in enumerate((50, 60, 70, 80, 90, 100))]),
    "btn_options_settings_fps": (356, [("_fps30", "OPTIONS_BUTTON_FPS_30", 0), ("_fps45", "OPTIONS_BUTTON_FPS_45", 1),
                                       ("_fps60", "OPTIONS_BUTTON_FPS_60", 2), ("_fpsmax", "OPTIONS_BUTTON_FPS_MAX", 3)]),
    "btn_options_settings_nitroblur": (162, [("_on", "OPTIONS_BUTTON_ON", 1), ("_off", "OPTIONS_BUTTON_OFF", 0)]),
    "btn_options_settings_grading": (162, [("_gradeon", "OPTIONS_BUTTON_ON", 1), ("_gradeoff", "OPTIONS_BUTTON_OFF", 0)]),
}
# rows of the GRAPHICS frame, top to bottom: (entity suffix, title string, OptionName, widget layout, wide, note string)
ROWS = (
    ("GFX_QUALITY", "OPTIONS_TITLE_GFX_QUALITY", "GraphicsQuality", "btn_options_settings_quality", True,
     "OPTIONS_NOTE_GFX_RESTART"),
    ("RENDER_SCALE", "OPTIONS_TITLE_RENDER_SCALE", "RenderScale", "btn_options_settings_scale", True, None),
    ("FPS_LIMIT", "OPTIONS_TITLE_FPS_LIMIT", "FrameRateLimit", "btn_options_settings_fps", True, None),
    ("NITRO_BLUR", "OPTIONS_TITLE_NITRO_BLUR", "NitroMotionBlur", "btn_options_settings_nitroblur", False, None),
    ("COLOUR_GRADING", "OPTIONS_TITLE_COLOUR_GRADING", "ColourGrading", "btn_options_settings_grading", False,
     "OPTIONS_NOTE_GRADING_MAX"),
)
FRAME_TITLE_ID = "OPTIONS_TITLE_GRAPHICS"
# page: the original metrics, checked against the page before anything moves.
#   display     top and bottom of the DISPLAY frame
#   audio_title y of the AUDIO frame title (the gap below a frame repeats under the new one)
#   first       frame top to the first row title; title_button: row title to its buttons; pitch: row title to row title
#   pad         last buttons to the frame's bottom edge
PAGES = {
    "options_settings_multilang": dict(display=(135.0, 258.0), audio_title=264.0, first=13.0, title_button=12.0,
                                       pitch=52.0, pad=13.0),
    "options_settings": dict(display=(135.0, 258.0), audio_title=264.0, first=13.0, title_button=12.0, pitch=48.0,
                             pad=18.0),
    "paused_options_settings_multilang": dict(display=(135.0, 205.0), audio_title=212.0, first=13.0, title_button=12.0,
                                              pitch=52.0, pad=13.0),
    "paused_options_settings": dict(display=(130.0, 244.0), audio_title=246.0, first=10.0, title_button=15.0,
                                    pitch=47.0, pad=10.0),
}

LANGUAGES = ("ENG_US", "FRE_FR", "GER_DE", "ITA_IT", "SPA_ES", "DUT_NL", "POR_BR", "RUS_RU", "JPN_JP", "KOR_KR", "CHS_CN")
# Uppercase titles and labels like the game's own option strings (OPTIONS_TITLE_DISPLAY, OPTIONS_TITLE_SPEEDOMETER);
# the note in sentence case like the game's control hints (TUTORIALS_XPERIA_NITROUS_DESCRIPTION). Nitrous as the game
# calls it in each language (TUTORIALS_NITROUS, HUD_TAKEDOWN_REWARD: NITRO, N2O, НИТРО, ナイトロ, 니트로, 氮气).
TEXTS = {
    "ENG_US": {"OPTIONS_TITLE_GRAPHICS": "GRAPHICS", "OPTIONS_TITLE_GFX_QUALITY": "GRAPHICS QUALITY",
               "OPTIONS_BUTTON_GFX_MAX": "MAX", "OPTIONS_NOTE_GFX_RESTART": "Applies after the game restarts",
               "OPTIONS_TITLE_RENDER_SCALE": "RENDER SCALE", "OPTIONS_TITLE_FPS_LIMIT": "FRAME RATE LIMIT",
               "OPTIONS_BUTTON_FPS_MAX": "MAX", "OPTIONS_TITLE_NITRO_BLUR": "NITROUS MOTION BLUR",
               "OPTIONS_TITLE_COLOUR_GRADING": "COLOUR GRADING",
               "OPTIONS_NOTE_GRADING_MAX": "Only at the MAX graphics quality"},
    "FRE_FR": {"OPTIONS_TITLE_GRAPHICS": "GRAPHISMES", "OPTIONS_TITLE_GFX_QUALITY": "QUALITÉ GRAPHIQUE",
               "OPTIONS_BUTTON_GFX_MAX": "MAX", "OPTIONS_NOTE_GFX_RESTART": "S'applique au prochain lancement du jeu",
               "OPTIONS_TITLE_RENDER_SCALE": "ÉCHELLE DE RENDU", "OPTIONS_TITLE_FPS_LIMIT": "LIMITE DE FPS",
               "OPTIONS_BUTTON_FPS_MAX": "MAX", "OPTIONS_TITLE_NITRO_BLUR": "FLOU CINÉTIQUE DE LA NITRO",
               "OPTIONS_TITLE_COLOUR_GRADING": "ÉTALONNAGE DES COULEURS",
               "OPTIONS_NOTE_GRADING_MAX": "Uniquement avec la qualité graphique MAX"},
    "GER_DE": {"OPTIONS_TITLE_GRAPHICS": "GRAFIK", "OPTIONS_TITLE_GFX_QUALITY": "GRAFIKQUALITÄT",
               "OPTIONS_BUTTON_GFX_MAX": "MAX", "OPTIONS_NOTE_GFX_RESTART": "Gilt ab dem nächsten Spielstart",
               "OPTIONS_TITLE_RENDER_SCALE": "RENDERSKALIERUNG", "OPTIONS_TITLE_FPS_LIMIT": "BILDRATENBEGRENZUNG",
               "OPTIONS_BUTTON_FPS_MAX": "MAX", "OPTIONS_TITLE_NITRO_BLUR": "NITRO-BEWEGUNGSUNSCHÄRFE",
               "OPTIONS_TITLE_COLOUR_GRADING": "FARBKORREKTUR",
               "OPTIONS_NOTE_GRADING_MAX": "Nur bei Grafikqualität MAX"},
    "ITA_IT": {"OPTIONS_TITLE_GRAPHICS": "GRAFICA", "OPTIONS_TITLE_GFX_QUALITY": "QUALITÀ GRAFICA",
               "OPTIONS_BUTTON_GFX_MAX": "MAX", "OPTIONS_NOTE_GFX_RESTART": "Si applica al prossimo avvio del gioco",
               "OPTIONS_TITLE_RENDER_SCALE": "SCALA DI RENDERING", "OPTIONS_TITLE_FPS_LIMIT": "LIMITE FPS",
               "OPTIONS_BUTTON_FPS_MAX": "MAX", "OPTIONS_TITLE_NITRO_BLUR": "SFOCATURA DI MOVIMENTO N2O",
               "OPTIONS_TITLE_COLOUR_GRADING": "CORREZIONE COLORE",
               "OPTIONS_NOTE_GRADING_MAX": "Solo con qualità grafica MAX"},
    "SPA_ES": {"OPTIONS_TITLE_GRAPHICS": "GRÁFICOS", "OPTIONS_TITLE_GFX_QUALITY": "CALIDAD GRÁFICA",
               "OPTIONS_BUTTON_GFX_MAX": "MÁX.", "OPTIONS_NOTE_GFX_RESTART": "Se aplica al reiniciar el juego",
               "OPTIONS_TITLE_RENDER_SCALE": "ESCALA DE RENDERIZADO", "OPTIONS_TITLE_FPS_LIMIT": "LÍMITE DE FPS",
               "OPTIONS_BUTTON_FPS_MAX": "MÁX.", "OPTIONS_TITLE_NITRO_BLUR": "DESENFOQUE DE MOVIMIENTO DEL NITRO",
               "OPTIONS_TITLE_COLOUR_GRADING": "CORRECCIÓN DE COLOR",
               "OPTIONS_NOTE_GRADING_MAX": "Solo con calidad gráfica MÁX."},
    "DUT_NL": {"OPTIONS_TITLE_GRAPHICS": "GRAPHICS", "OPTIONS_TITLE_GFX_QUALITY": "GRAFISCHE KWALITEIT",
               "OPTIONS_BUTTON_GFX_MAX": "MAX", "OPTIONS_NOTE_GFX_RESTART": "Wordt toegepast na een herstart van het spel",
               "OPTIONS_TITLE_RENDER_SCALE": "RENDERSCHAAL", "OPTIONS_TITLE_FPS_LIMIT": "FPS-LIMIET",
               "OPTIONS_BUTTON_FPS_MAX": "MAX", "OPTIONS_TITLE_NITRO_BLUR": "NITRO-BEWEGINGSONSCHERPTE",
               "OPTIONS_TITLE_COLOUR_GRADING": "KLEURCORRECTIE",
               "OPTIONS_NOTE_GRADING_MAX": "Alleen bij grafische kwaliteit MAX"},
    "POR_BR": {"OPTIONS_TITLE_GRAPHICS": "GRÁFICOS", "OPTIONS_TITLE_GFX_QUALITY": "QUALIDADE GRÁFICA",
               "OPTIONS_BUTTON_GFX_MAX": "MÁX.", "OPTIONS_NOTE_GFX_RESTART": "Aplicado ao reiniciar o jogo",
               "OPTIONS_TITLE_RENDER_SCALE": "ESCALA DE RENDERIZAÇÃO", "OPTIONS_TITLE_FPS_LIMIT": "LIMITE DE FPS",
               "OPTIONS_BUTTON_FPS_MAX": "MÁX.", "OPTIONS_TITLE_NITRO_BLUR": "DESFOQUE DE MOVIMENTO DO NITRO",
               "OPTIONS_TITLE_COLOUR_GRADING": "CORREÇÃO DE COR",
               "OPTIONS_NOTE_GRADING_MAX": "Somente com qualidade gráfica MÁX."},
    "RUS_RU": {"OPTIONS_TITLE_GRAPHICS": "ГРАФИКА", "OPTIONS_TITLE_GFX_QUALITY": "КАЧЕСТВО ГРАФИКИ",
               "OPTIONS_BUTTON_GFX_MAX": "МАКС.", "OPTIONS_NOTE_GFX_RESTART": "Применяется после перезапуска игры",
               "OPTIONS_TITLE_RENDER_SCALE": "МАСШТАБ РЕНДЕРА", "OPTIONS_TITLE_FPS_LIMIT": "ОГРАНИЧЕНИЕ FPS",
               "OPTIONS_BUTTON_FPS_MAX": "МАКС.", "OPTIONS_TITLE_NITRO_BLUR": "РАЗМЫТИЕ ДВИЖЕНИЯ ПРИ НИТРО",
               "OPTIONS_TITLE_COLOUR_GRADING": "ЦВЕТОКОРРЕКЦИЯ",
               "OPTIONS_NOTE_GRADING_MAX": "Только при качестве графики МАКС."},
    "JPN_JP": {"OPTIONS_TITLE_GRAPHICS": "グラフィック", "OPTIONS_TITLE_GFX_QUALITY": "グラフィック品質",
               "OPTIONS_BUTTON_GFX_MAX": "最高", "OPTIONS_NOTE_GFX_RESTART": "ゲームの再起動後に適用されます",
               "OPTIONS_TITLE_RENDER_SCALE": "レンダリングスケール", "OPTIONS_TITLE_FPS_LIMIT": "フレームレート上限",
               "OPTIONS_BUTTON_FPS_MAX": "最大", "OPTIONS_TITLE_NITRO_BLUR": "ナイトロ時のモーションブラー",
               "OPTIONS_TITLE_COLOUR_GRADING": "カラーグレーディング",
               "OPTIONS_NOTE_GRADING_MAX": "グラフィック品質が最高のときのみ"},
    "KOR_KR": {"OPTIONS_TITLE_GRAPHICS": "그래픽", "OPTIONS_TITLE_GFX_QUALITY": "그래픽 품질",
               "OPTIONS_BUTTON_GFX_MAX": "최고", "OPTIONS_NOTE_GFX_RESTART": "게임을 다시 시작하면 적용됩니다",
               "OPTIONS_TITLE_RENDER_SCALE": "렌더링 배율", "OPTIONS_TITLE_FPS_LIMIT": "프레임 속도 제한",
               "OPTIONS_BUTTON_FPS_MAX": "최대", "OPTIONS_TITLE_NITRO_BLUR": "니트로 모션 블러",
               "OPTIONS_TITLE_COLOUR_GRADING": "색 보정",
               "OPTIONS_NOTE_GRADING_MAX": "그래픽 품질이 최고일 때만 적용됩니다"},
    "CHS_CN": {"OPTIONS_TITLE_GRAPHICS": "图形", "OPTIONS_TITLE_GFX_QUALITY": "画质",
               "OPTIONS_BUTTON_GFX_MAX": "最高", "OPTIONS_NOTE_GFX_RESTART": "重启游戏后生效",
               "OPTIONS_TITLE_RENDER_SCALE": "渲染比例", "OPTIONS_TITLE_FPS_LIMIT": "帧率上限",
               "OPTIONS_BUTTON_FPS_MAX": "最高", "OPTIONS_TITLE_NITRO_BLUR": "氮气动态模糊",
               "OPTIONS_TITLE_COLOUR_GRADING": "调色",
               "OPTIONS_NOTE_GRADING_MAX": "仅在画质为最高时生效"},
}
# the same in every language
NUMBERS = {"OPTIONS_BUTTON_FPS_30": "30", "OPTIONS_BUTTON_FPS_45": "45", "OPTIONS_BUTTON_FPS_60": "60",
           "OPTIONS_BUTTON_GFX_1": "1", "OPTIONS_BUTTON_GFX_2": "2", "OPTIONS_BUTTON_GFX_3": "3",
           "OPTIONS_BUTTON_GFX_4": "4"}
NUMBERS.update({f"OPTIONS_BUTTON_SCALE_{p}": f"{p}%" for p in (50, 60, 70, 80, 90, 100)})
GAME_STRINGS = {"OPTIONS_BUTTON_ON", "OPTIONS_BUTTON_OFF"}  # in every table of the OBB


def strings_for(lang: str) -> list[tuple[str, str]]:
    return list(TEXTS[lang].items()) + list(NUMBERS.items())


def port_strings() -> str:
    """assets/port_strings.txt: every string id the rows use, in every language (portstrings.cpp)."""
    used = {FRAME_TITLE_ID} | {row[1] for row in ROWS} | {row[5] for row in ROWS if row[5]}
    used |= {sid for _, children in WIDGETS.values() for _, sid, _ in children}
    lines = []
    for lang in LANGUAGES:
        pairs = strings_for(lang)
        ids = [sid for sid, _ in pairs]
        if set(ids) != used - GAME_STRINGS or len(ids) != len(set(ids)):
            raise SystemExit(f"{lang}: strings {sorted(set(ids) ^ (used - GAME_STRINGS))} differ from the rows")
        for sid, text in pairs:
            if not text or any(c in text for c in "\t\r\n"):
                raise SystemExit(f"{lang} {sid}: empty or with a tab or line break")
            lines.append(f"{lang}\t{sid}\t{text}\n")
    return "".join(lines)


def _layouts(sb: sbinw.Sbin) -> tuple[int, list[int], dict[str, int]]:
    array = sb.map_get(0, "Layouts")
    refs = sb.array_items(array)
    return array, refs, {sb.map_get(r, "name"): r for r in refs}


def _entities(sb: sbinw.Sbin, layout: int) -> list[int]:
    return sb.array_items(sb.map_get(layout, "entities"))


def _by_id(sb: sbinw.Sbin, layout: int) -> dict[str, int]:
    return {sb.get_field(e, "entityid"): e for e in _entities(sb, layout) if sb.get_field(e, "entityid")}


def _content(sb: sbinw.Sbin, entity: int) -> int:
    return sb.get_field(entity, "content")


def _y(sb: sbinw.Sbin, entity: int, corner: str = "top_left") -> float:
    return sb.get_field(entity, "rect", corner, "y")


def _is_frame(sb: sbinw.Sbin, entity: int) -> bool:
    content = _content(sb, entity)
    return (content != sbinw.NULL and sb.typed_name(content) == "SubLayout"
            and sb.get_field(content, "layout") == "frame")


def _set_label(sb: sbinw.Sbin, entity: int, string_id: str) -> None:
    """The Buttonname StringID override of a SubLayout button entity."""
    subprops = sb.get_field(_content(sb, entity), "subproperties")
    for item in sb.array_items(subprops):
        if sb.s(struct.unpack_from("<H", item, 0)[0]) == "Buttonname":
            props = struct.unpack_from("<I", item, 4)[0]
            sb.set_member(props, "StringID", string_id)
            return
    raise SystemExit("button entity without a Buttonname override")


def _add_widget(sb: sbinw.Sbin, byname: dict[str, int], name: str, width: int, children: list) -> int:
    layout = sb.clone(byname[TEMPLATE])
    sb.set_member(layout, "name", name)
    sb.set_member(layout, "widget_class", "RadioOption")
    sb.set_field(sb.map_get(layout, "maximum_size"), "width", 1000)
    sb.set_field(sb.map_get(layout, "default_size"), "width", width)
    first = _entities(sb, layout)[0]
    made = []
    for k, (child_id, label, radio) in enumerate(children):
        child = sb.clone(first)
        sb.set_field(child, "entityid", child_id)
        sb.set_field(child, "rect", "top_left", "anchor_x", k / len(children))
        sb.set_field(child, "rect", "bottom_right", "anchor_x", (k + 1) / len(children))
        _set_label(sb, child, label)
        sb.set_field(child, "properties", sb.add_map([("RadioIndex", 13, f"={radio}")]))
        made.append(child)
    sb.set_member(layout, "entities", sb.add_array(15, made))
    return layout


def plan(metrics: dict) -> dict:
    """y positions of the GRAPHICS frame of one page and the distance everything below DISPLAY moves."""
    display_bottom = metrics["display"][1]
    gap = metrics["audio_title"] - display_bottom
    title_y = display_bottom + gap
    top = title_y + FRAME_TITLE_GAP
    rows = []
    y = top + metrics["first"]
    for suffix, *_rest, note in ROWS:
        button = y + metrics["title_button"]
        rows.append({"suffix": suffix, "title": y, "button": button,
                     "note": button + BUTTON_HEIGHT + NOTE_GAP if note else None})
        y += metrics["pitch"] + (NOTE_SPACE if note else 0.0)
    # a note under the last row's buttons needs its own space above the frame's bottom edge (stage 16)
    bottom = rows[-1]["button"] + BUTTON_HEIGHT + metrics["pad"] + (NOTE_SPACE if rows[-1]["note"] is not None else 0.0)
    return {"frame_title": title_y, "frame": (top, bottom), "rows": rows, "shift": bottom - display_bottom}


def _check_page(sb: sbinw.Sbin, name: str, page: int, metrics: dict) -> int:
    """The page's own metrics as PAGES records them; returns the DISPLAY frame entity."""
    ids = _by_id(sb, page)
    top, bottom = metrics["display"]
    frames = [e for e in _entities(sb, page) if _is_frame(sb, e) and (_y(sb, e), _y(sb, e, "bottom_right")) == (top, bottom)]
    checks = {
        "DISPLAY frame": len(frames) == 1,
        "TITLE_DISPLAY": _y(sb, ids["TITLE_DISPLAY"]) == top - FRAME_TITLE_GAP,
        "TITLE_AUDIO": _y(sb, ids["TITLE_AUDIO"]) == metrics["audio_title"],
        "TITLE_SPEEDOMETER": _y(sb, ids["TITLE_SPEEDOMETER"]) == top + metrics["first"],
        "BTN_SPEEDOMETER": _y(sb, ids["BTN_SPEEDOMETER"]) == top + metrics["first"] + metrics["title_button"],
        "BTN_SPEEDOMETER height": _y(sb, ids["BTN_SPEEDOMETER"], "bottom_right") - _y(sb, ids["BTN_SPEEDOMETER"]) == BUTTON_HEIGHT,
    }
    failed = [k for k, ok in checks.items() if not ok]
    if failed:
        raise SystemExit(f"{name}: page metrics differ from the original: {failed}")
    return frames[0]


def _add_frame(sb: sbinw.Sbin, name: str, page: int) -> dict:
    metrics = PAGES[name]
    display_frame = _check_page(sb, name, page, metrics)
    ids = _by_id(sb, page)
    geometry = plan(metrics)
    display_bottom, shift = metrics["display"][1], geometry["shift"]
    moved, grown = [], []
    for e in _entities(sb, page):
        top, bottom = _y(sb, e), _y(sb, e, "bottom_right")
        top_abs = sb.get_field(e, "rect", "top_left", "anchor_y") == 0.0
        bottom_abs = sb.get_field(e, "rect", "bottom_right", "anchor_y") == 0.0
        if top_abs and top >= display_bottom:
            sb.set_field(e, "rect", "top_left", "y", top + shift)
            if bottom_abs:
                sb.set_field(e, "rect", "bottom_right", "y", bottom + shift)
            moved.append(e)
        elif bottom_abs and bottom > display_bottom:  # the page's extent (HelperRect of the main menu pages)
            content = _content(sb, e)
            if content == sbinw.NULL or sb.typed_name(content) != "HelperRect":
                raise SystemExit(f"{name}: {sb.get_field(e, 'entityid')!r} spans the DISPLAY frame's bottom edge")
            sb.set_field(e, "rect", "bottom_right", "y", bottom + shift)
            grown.append(e)
    if not moved:
        raise SystemExit(f"{name}: nothing below the DISPLAY frame")

    frame = sb.clone(display_frame)
    sb.set_field(frame, "entityid", "FRAME_GRAPHICS")
    sb.set_field(frame, "rect", "top_left", "y", geometry["frame"][0])
    sb.set_field(frame, "rect", "bottom_right", "y", geometry["frame"][1])
    frame_title = sb.clone(ids["TITLE_DISPLAY"])
    sb.set_field(frame_title, "entityid", "TITLE_GRAPHICS")
    sb.set_field(_content(sb, frame_title), "stringid", FRAME_TITLE_ID)
    for corner in ("top_left", "bottom_right"):
        sb.set_field(frame_title, "rect", corner, "y", geometry["frame_title"])

    def centred_text(entity_id: str, string_id: str, y: float) -> int:
        text = sb.clone(ids["TITLE_SPEEDOMETER"])
        sb.set_field(text, "entityid", entity_id)
        sb.set_field(_content(sb, text), "stringid", string_id)
        for corner in ("top_left", "bottom_right"):
            sb.set_field(text, "rect", corner, "x", 0.0)
            sb.set_field(text, "rect", corner, "anchor_x", 0.5)
            sb.set_field(text, "rect", corner, "y", y)
        sb.set_field(text, "visible", 1)
        return text

    rows = [frame_title]
    for (suffix, title_id, option, widget, wide, note_id), at in zip(ROWS, geometry["rows"]):
        rows.append(centred_text(f"TITLE_{suffix}", title_id, at["title"]))
        button = sb.clone(ids["BTN_SPEEDOMETER"])
        sb.set_field(button, "entityid", f"BTN_{suffix}")
        sb.set_field(_content(sb, button), "layout", widget)
        sb.set_member(sb.get_field(button, "properties"), "OptionName", option)
        for corner, narrow in zip(("top_left", "bottom_right"), NARROW):
            sb.set_field(button, "rect", corner, "x", sb.get_field(ids[WIDE], "rect", corner, "x") if wide else 0.0)
            sb.set_field(button, "rect", corner, "anchor_x",
                         sb.get_field(ids[WIDE], "rect", corner, "anchor_x") if wide else narrow)
        sb.set_field(button, "rect", "top_left", "y", at["button"])
        sb.set_field(button, "rect", "bottom_right", "y", at["button"] + BUTTON_HEIGHT)
        rows.append(button)
        if note_id:
            note = centred_text(f"NOTE_{suffix}", note_id, at["note"])
            sb.set_field(_content(sb, note), "font", NOTE_FONT)
            for channel, value in zip("rgba", NOTE_COLOR):
                sb.set_field(note, "color", channel, value)
            rows.append(note)

    refs = _entities(sb, page)
    last_frame = max(k for k, e in enumerate(refs) if _is_frame(sb, e))
    anchor = max(k for k, e in enumerate(refs) if sb.get_field(e, "entityid").startswith(("BTN_", "TITLE_")))
    # frames first (drawn under every row), the rows after the page's own rows, before the gamepad highlights
    refs = refs[:last_frame + 1] + [frame] + refs[last_frame + 1:anchor + 1] + rows + refs[anchor + 1:]
    sb.set_member(page, "entities", sb.add_array(15, refs))
    return {"shift": shift, "frame": geometry["frame"], "moved": len(moved), "grown": len(grown)}


def _placeholders(sb: sbinw.Sbin, byname: dict[str, int]) -> list[tuple[int, str]]:
    """(entity, page) for every SubLayout entity that embeds one of the settings pages (options_detailed and
    paused_options_detailed, PLACE_HOLDER and PLACE_HOLDER_MULTILANG)."""
    out = []
    for li in byname.values():
        for e in _entities(sb, li):
            content = sb.get_field(e, "content")
            if content != sbinw.NULL and sb.typed_name(content) == "SubLayout" and sb.get_field(content, "layout") in PAGES:
                out.append((e, sb.get_field(content, "layout")))
    return out


def build_layouts(raw: bytes) -> bytes:
    sb = sbinw.Sbin(raw)
    _, refs, byname = _layouts(sb)
    clash = [name for name in WIDGETS if name in byname]
    if clash:
        raise SystemExit(f"{clash} already exist")
    widgets = [_add_widget(sb, byname, name, width, children) for name, (width, children) in WIDGETS.items()]
    shifts = {name: _add_frame(sb, name, byname[name])["shift"] for name in PAGES}
    # The scroll range of ScrollAreaVertical comes from the height of the entity that embeds the page (75..605 in
    # options_detailed), not from the page's content: it grows by the page's shift, so the last row (DEFAULT
    # SETTINGS / RESET DATA) scrolls into view exactly as before.
    holders = _placeholders(sb, byname)
    if len(holders) != 4:
        raise SystemExit(f"expected 4 page placeholders, found {len(holders)}")
    for e, page in holders:
        if sb.get_field(e, "rect", "bottom_right", "anchor_y") != 0.0:
            raise SystemExit(f"placeholder of {page} is not absolutely positioned")
        sb.set_field(e, "rect", "bottom_right", "y", _y(sb, e, "bottom_right") + shifts[page])
    sb.set_member(0, "Layouts", sb.add_array(15, refs + widgets))
    out = sb.encode()
    verify_layouts(raw, out)
    return out


def verify_layouts(raw: bytes, out: bytes) -> None:
    """Decode the result again: the widgets and the frames as intended, every original layout still reachable, every
    original entity of a page in its order and either in place or moved by the page's shift."""
    old, new = sbinw.Sbin(raw), sbinw.Sbin(out)
    _, old_refs, old_names = _layouts(old)
    _, new_refs, new_names = _layouts(new)
    if set(new_names) != set(old_names) | set(WIDGETS) or len(new_refs) != len(old_refs) + len(WIDGETS):
        raise SystemExit("layout list differs")
    for name, (_, children) in WIDGETS.items():
        widget = new_names[name]
        if new.map_get(widget, "widget_class") != "RadioOption":
            raise SystemExit(f"{name}: widget class")
        made = _entities(new, widget)
        if len(made) != len(children):
            raise SystemExit(f"{name}: children")
        for child, (child_id, label, radio) in zip(made, children):
            content = _content(new, child)
            if new.typed_name(content) != "SubLayout" or new.get_field(content, "layout") != "btn_options_small2":
                raise SystemExit(f"{name}: child is not a btn_options_small2 button")
            if new.map_get(new.get_field(child, "properties"), "RadioIndex") != f"={radio}":
                raise SystemExit(f"{name}: RadioIndex of {child_id}")
            subprops = new.array_items(new.get_field(content, "subproperties"))
            labels = [new.map_get(struct.unpack_from("<I", item, 4)[0], "StringID") for item in subprops
                      if new.s(struct.unpack_from("<H", item, 0)[0]) == "Buttonname"]
            if labels != [label]:
                raise SystemExit(f"{name}: label of {child_id} is {labels}")
    added = 1 + 1 + sum(2 + (1 if row[5] else 0) for row in ROWS)  # frame, frame title, rows (+ note)
    for name, metrics in PAGES.items():
        geometry = plan(metrics)
        page_old, page_new = old_names[name], new_names[name]
        ids = _by_id(new, page_new)
        for suffix, title_id, option, widget, _, note_id in ROWS:
            btn, title = ids[f"BTN_{suffix}"], ids[f"TITLE_{suffix}"]
            if new.map_get(new.get_field(btn, "properties"), "OptionName") != option:
                raise SystemExit(f"{name}: OptionName of {suffix}")
            if new.get_field(_content(new, btn), "layout") != widget:
                raise SystemExit(f"{name}: widget of {suffix}")
            if new.get_field(_content(new, title), "stringid") != title_id:
                raise SystemExit(f"{name}: title string of {suffix}")
            if note_id and new.get_field(_content(new, ids[f"NOTE_{suffix}"]), "font") != NOTE_FONT:
                raise SystemExit(f"{name}: note font")
        frame = ids["FRAME_GRAPHICS"]
        if (_y(new, frame), _y(new, frame, "bottom_right")) != geometry["frame"] or not _is_frame(new, frame):
            raise SystemExit(f"{name}: GRAPHICS frame")
        before, after = _entities(old, page_old), _entities(new, page_new)
        if len(after) != len(before) + added:
            raise SystemExit(f"{name}: entity count {len(after)} != {len(before)} + {added}")
        kept = [e for e in after if new.get_field(e, "entityid") not in
                {"FRAME_GRAPHICS", "TITLE_GRAPHICS"} | {f"{p}_{row[0]}" for row in ROWS for p in ("TITLE", "BTN", "NOTE")}]
        if len(kept) != len(before):
            raise SystemExit(f"{name}: original entities")
        display_bottom = metrics["display"][1]
        for e_old, e_new in zip(before, kept):
            if old.get_field(e_old, "entityid") != new.get_field(e_new, "entityid"):
                raise SystemExit(f"{name}: entity order")
            top_old, top_new = _y(old, e_old), _y(new, e_new)
            want = top_old + geometry["shift"] if top_old >= display_bottom and \
                old.get_field(e_old, "rect", "top_left", "anchor_y") == 0.0 else top_old
            if top_new != want:
                raise SystemExit(f"{name}: {old.get_field(e_old, 'entityid')!r} at {top_new}, expected {want}")
    for (e_old, page), (e_new, _) in zip(_placeholders(old, old_names), _placeholders(new, new_names)):
        grown = _y(new, e_new, "bottom_right") - _y(old, e_old, "bottom_right")
        if grown != plan(PAGES[page])["shift"]:
            raise SystemExit(f"placeholder of {page} grew by {grown}")
