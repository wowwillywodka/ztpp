// ztpp — src/ui.cpp: холодные функции меню/настроек (отрисовка меню, клики, сохранение/загрузка).
// Мелкие хелперы (drawText/drawChar/синглтоны презентации) — inline в ui.hpp.
#include "ui.hpp"
#include "profile.hpp"   // контур данных: описания сейв-слотов читаются из профиля билда
#include "version.hpp"   // ztppVersion() — экран ABOUT

MenuAction menuHit(int mx, int my, int page, double& frac) {
    using namespace menu;
    auto barFrac = [&](Rect b) { frac = (double)(mx - b.x) / b.w; if (frac < 0) frac = 0; if (frac > 1) frac = 1; };
    auto slider = [&](int slot, MenuAction dec) -> MenuAction {   // хит одного ползунка (dec/inc/bar подряд в enum)
        if (minusBtn(slot).has(mx, my)) return dec;
        if (plusBtn(slot).has(mx, my))  return (MenuAction)(dec + 1);
        if (bar(slot).has(mx, my)) { barFrac(bar(slot)); return (MenuAction)(dec + 2); }
        return MA_NONE;
    };
    MenuAction a = MA_NONE;
    if (menuMode() == 0) {                                   // ── КОРЕНЬ: CONTINUE/NEW GAME/PASSWORD/SAVE/LOAD/OPTIONS/EXIT/*DEBUG* ──
        for (int i = 0; i < NROOT; ++i) if (rootBtn(i).has(mx, my)) return rootAction(i);
        return MA_NONE;
    }
    if (menuMode() == 5) {                                   // ── ABOUT: только BACK ──
        if (resumeBtn().has(mx, my)) return MA_BACK;
        return MA_NONE;
    }
    if (menuMode() >= 3) {                                   // ── SAVE/LOAD: 6 слотов ──
        for (int i = 0; i < 6; ++i) if (slotBtn(i).has(mx, my)) return (MenuAction)(MA_SLOT + i);
        if (resumeBtn().has(mx, my)) return MA_BACK;
        return MA_NONE;
    }
    if (menuMode() == 1) {                                   // ── OPTIONS (полноэкранное) ──
        if (page == 0) {                                     // VIDEO: rscale + frame limit + fullscreen/aspect/filter
            if (rscaleBtn().has(mx, my)) return MA_RSCALE;
            if ((a = slider(1, MA_FL_DEC)) != MA_NONE) return a;
            if (tog(0).has(mx, my)) return MA_FULLSCREEN;
            if (tog(1).has(mx, my)) return MA_ASPECT;
            if (tog(2).has(mx, my)) return MA_FILTER;
        } else if (page == 1) {                              // AUDIO: sound/music + громкость/темп озвучки
            if ((a = slider(0, MA_SV_DEC)) != MA_NONE) return a;
            if ((a = slider(1, MA_VP_DEC)) != MA_NONE) return a;
            if (tog(0).has(mx, my)) return MA_SND;
            if (tog(1).has(mx, my)) return MA_MUSIC;
        } else if (page == 2) {                              // GAME: move/turn/mouse/sim-rate + инвентарь
            // (Player physics / Distance / TAB map убраны из UI по просьбе юзера 2026-07-22 —
            //  physics/gamedist остаются консольными тумблерами; TAB всегда = pause menu)
            if ((a = slider(0, MA_MV_DEC)) != MA_NONE) return a;
            if ((a = slider(1, MA_TN_DEC)) != MA_NONE) return a;
            if ((a = slider(2, MA_MS_DEC)) != MA_NONE) return a;
            if ((a = slider(3, MA_SB_DEC)) != MA_NONE) return a;
            if (tog(0).has(mx, my)) return MA_INVUNLIM;
        } else {                                             // CONTROLS: пресеты + переназначение клавиш
            for (int p = 0; p < NUM_PRESETS; ++p)
                if (ctrlPresetBtn(p).has(mx, my)) return (MenuAction)(MA_PRESET + p);
            for (int i = 0; i < GA_COUNT; ++i)
                if (ctrlKeyBtn(i).has(mx, my)) return (MenuAction)(MA_KEYBIND + i);
        }
    } else {                                                 // ── DEBUG (полноэкранное) ──
        // (Render / Reference(HUD) / Map / Internal res убраны из UI по просьбе юзера 2026-07-22 —
        //  остаются CLI/консоль; reference/faithful дефолты фиксированы)
        if (page == 0) {
            if ((a = slider(0, MA_ES_DEC)) != MA_NONE) return a;   // enemy speed
            if ((a = slider(1, MA_ET_DEC)) != MA_NONE) return a;   // enemy timers
            if ((a = slider(2, MA_WA_DEC)) != MA_NONE) return a;   // wall anim
            if (tog(0).has(mx, my)) return MA_NOCLIP;
            if (tog(1).has(mx, my)) return MA_ENEMIES;
        } else {
            if (tog(0).has(mx, my)) return MA_MAPIDS;
            if (tog(1).has(mx, my)) return MA_FPSINV;
        }
    }
    if (menuMode() == 1 && menuPreGame() && pwBtn().has(mx, my)) return MA_PASSWORD;   // ⭐OPTIONS до геймплея: ENTER PASSWORD
    if (pageBtn().has(mx, my))   return MA_PAGE;
    if (saveBtn().has(mx, my))   return MA_SAVE;
    if (quitBtn().has(mx, my))   return MA_QUIT;
    if (resumeBtn().has(mx, my)) return MA_BACK;             // низ подменю: BACK → корень
    return MA_NONE;
}

void drawMenu(FB& fb, double mv, double tn, double st, int fps, double espd, int page,
                     bool faithful, bool noclip, bool reference, bool enemiesOn, bool gameMap, const char* status) {
    using namespace menu;
    int k = K();
    char buf[32], tg[40];

    if (menuMode() == 0) {                                   // ── КОРЕНЬ: малый чёрный бокс, шрифт+палитра ЭКРАНА ОПЦИЙ ──
        Rect rb = rootBox();
        fbBox(fb, rb, 0xFF000000u, 0xFF7E92AAu);
        // Заголовок + пункты — палитрой текста экрана опций (ROM CRAM line3 0x1CDAE); стрелка-курсор у выбранного.
        drawTextBigC(fb, FBW / 2, rb.y + 10*k, "PAUSED", 0, 2*k, false, g_uiOptPal);
        static const char* items[NROOT] = {"CONTINUE", "NEW GAME", "SAVE GAME", "LOAD GAME", "OPTIONS", "ABOUT", "EXIT", "*DEBUG*"};
        if (menuSel() < 0) menuSel() = 0; if (menuSel() >= NROOT) menuSel() = NROOT - 1;
        for (int i = 0; i < NROOT; ++i) {
            Rect b = rootBtn(i);
            int ty = b.y + (b.h - 32*k) / 2 + 4*k;
            drawTextBigC(fb, FBW / 2, ty, items[i], 0, 2*k, false, g_uiOptPal);
            if (i == menuSel()) {                             // ⭐СТРЕЛКА-КУРСОР «────►» слева от пункта (ROM sub_10B4C8, 16×8)
                int aw = textWBig(items[i], 2*k), as = 2*k;
                drawMenuArrow(fb, FBW / 2 - aw / 2 - 16*as - 4*k, ty + 8*k, as);
            }
        }
        return;
    }

    if (menuMode() == 5) {                                   // ── ABOUT: версия/инфо о порте ──
        fb.clear(0xFF000000u);
        drawTextBigC(fb, FBW / 2, PY() + 8*k, "ABOUT", 0, 2*k);
        char ver[48]; std::snprintf(ver, sizeof ver, "ZTPP  v%s", ztppVersion());
        drawTextBigC(fb, FBW / 2, RY(70),  ver, 0, 3*k, false, g_uiOptPal);
        static const char* L[] = {
            "FAITHFUL PORT OF ZERO TOLERANCE",
            "TECHNOPOP / ACCOLADE, 1994, SEGA MEGA DRIVE",
            "",
            "REIMPLEMENTATION IN CPP / SDL2",
            "ALL GAME DATA IS READ FROM YOUR ORIGINAL ROM",
            "MECHANICS RECONSTRUCTED FROM THE DISASSEMBLY",
            "",
            "PROJECT LEAD: WILLY WODKA",
            "REVERSE ENGINEERING AND CODE: CLAUDE (ANTHROPIC)",
            "LICENSE: LGPL-2.1   YM2612: NUKED-OPN2 (LGPL-2.1)",
            "",
            "ZERO TOLERANCE IS A TRADEMARK OF ITS OWNERS",
            "THIS PROJECT IS NOT AFFILIATED WITH THEM",
        };
        int y = RY(130);
        for (const char* s2 : L) { if (s2[0]) drawTextBigC(fb, FBW / 2, y, s2, 0, k, false, g_uiOptPal); y += 20*k; }
        drawBtn(fb, resumeBtn(), "BACK", 0xFF2C5A36u, 0xFF8AE0A0u);
        return;
    }
    if (menuMode() >= 3) {                                   // ── SAVE/LOAD GAME: слоты (Doom-style) ──
        fb.clear(0xFF000000u);
        drawTextBigC(fb, FBW / 2, PY() + 8*k, menuMode() == 3 ? "SAVE GAME" : "LOAD GAME", 0, 2*k);
        for (int i = 0; i < 6; ++i) {
            char nm[32]; std::snprintf(nm, sizeof nm, i == 0 ? "ztpp_quick.sav" : "ztpp_save%d.sav", i);
            std::string desc = i == 0 ? "QUICK SLOT (F5/F9)  - empty -" : "- empty -";
            { std::ifstream f(profilePath(nm)); std::string line;
              while (std::getline(f, line))
                  if (!line.compare(0, 5, "desc=")) { desc = (i == 0 ? std::string("QUICK  ") : std::string()) + line.substr(5); break; } }
            char lbl[96]; std::snprintf(lbl, sizeof lbl, "%d. %s", i, desc.c_str());
            drawBtn(fb, slotBtn(i), lbl, 0xFF2C3A50u, 0xFF6A7E96u);
        }
        drawBtn(fb, resumeBtn(), "BACK", 0xFF2C5A36u, 0xFF8AE0A0u);
        if (status && status[0]) drawText(fb, 8*k, RY(6), status, 0xFFB8C0C8u, k);
        return;
    }
    // ── OPTIONS / DEBUG: ПОЛНОЭКРАННОЕ ЧЁРНОЕ (как оригинальное меню OPTIONS ZT) ──
    fb.clear(0xFF000000u);
    const bool dbg = (menuMode() == 2);
    const int npage = dbg ? NPAGE_DBG : NPAGE_OPT;
    static const char* optNames[NPAGE_OPT] = {"VIDEO", "AUDIO", "GAME", "CONTROLS"};
    char title[32];
    if (dbg) std::snprintf(title, sizeof(title), "DEBUG  %d/%d", page + 1, npage);
    else     std::snprintf(title, sizeof(title), "OPTIONS - %s", optNames[page < NPAGE_OPT ? page : 0]);
    drawTextBigC(fb, FBW / 2, PY() + 8*k, title, 0, 2*k);                  // Font_grph родная жёлтая

    if (!dbg && page == 0) {                                 // ── OPTIONS: VIDEO ──
        std::snprintf(tg, sizeof(tg), "Render scale: %dx (restart)", presentRenderScale());
        drawBtn(fb, rscaleBtn(), tg, 0xFF2C3A50u, 0xFF6A7E96u);
        std::snprintf(buf, sizeof(buf), "%d fps", fps); drawSlider(fb, 1, "Frame limit (display)", (double)fps, 5, 60, buf);
        (void)st;                                            // Stretch убран из UI (юзер: не нужен)
        std::snprintf(tg, sizeof(tg), "Fullscreen: %s", presentFullscreen() ? "ON" : "OFF");
        drawBtn(fb, tog(0), tg, presentFullscreen() ? 0xFF315A31u : 0xFF2C3A50u, 0xFF6A7E96u);
        std::snprintf(tg, sizeof(tg), "Aspect: %s", presentAspectName());
        drawBtn(fb, tog(1), tg, 0xFF2C3A50u, 0xFF6A7E96u);
        std::snprintf(tg, sizeof(tg), "Filter: %s", presentLinear() ? "LINEAR" : "NEAREST");
        drawBtn(fb, tog(2), tg, 0xFF2C3A50u, 0xFF6A7E96u);
    } else if (!dbg && page == 1) {                          // ── OPTIONS: AUDIO ──
        std::snprintf(buf, sizeof(buf), "%.2f", soundVolume()); drawSlider(fb, 0, "Sound volume", soundVolume(), 0.0, 1.0, buf);
        if (voicePace() > 0.01) std::snprintf(buf, sizeof(buf), "x%.1f", voicePace()); else std::snprintf(buf, sizeof(buf), "OFF");
        drawSlider(fb, 1, "Voice pace (0=off)", voicePace(), 0.0, 2.0, buf);
        std::snprintf(tg, sizeof(tg), "Sound: %s", soundOn() ? "ON" : "OFF");
        drawBtn(fb, tog(0), tg, soundOn() ? 0xFF315A31u : 0xFF6E2828u, 0xFF6A7E96u);
        std::snprintf(tg, sizeof(tg), "Music: %s", musicOn() ? "ON" : "OFF");
        drawBtn(fb, tog(1), tg, musicOn() ? 0xFF315A31u : 0xFF6E2828u, 0xFF6A7E96u);
    } else if (!dbg && page == 2) {                          // ── OPTIONS: GAME (движение + точность/QoL) ──
        std::snprintf(buf, sizeof(buf), "%.3f", mv); drawSlider(fb, 0, "Move speed", mv, 0.015, 0.30, buf);
        std::snprintf(buf, sizeof(buf), "%.3f", tn); drawSlider(fb, 1, "Turn speed", tn, 0.008, 0.15, buf);
        std::snprintf(buf, sizeof(buf), "%.2f", mouseSensitivity()); drawSlider(fb, 2, "Mouse sens (0=off)", mouseSensitivity(), 0.0, 2.0, buf);
        std::snprintf(buf, sizeof(buf), "%.0f Hz", simBaseFps()); drawSlider(fb, 3, "Game speed (Hz, ROM=15)", simBaseFps(), 10.0, 60.0, buf);
        // (Player physics / Distance / TAB map убраны из UI — консольные тумблеры; TAB = pause menu)
        std::snprintf(tg, sizeof(tg), "Inventory: %s", inventoryUnlimited() ? "UNLIMITED" : "ZT (5 slots)");
        drawBtn(fb, tog(0), tg, inventoryUnlimited() ? 0xFF315A31u : 0xFF2C3A50u, 0xFF6A7E96u);
    } else if (!dbg) {                                       // ── OPTIONS: CONTROLS — 4 пресета + переназначение клавиш ──
        for (int p = 0; p < NUM_PRESETS; ++p) {              // ряд выбора пресета (текущий подсвечен зелёным)
            char pl[8]; std::snprintf(pl, sizeof(pl), "P%d", p + 1);
            bool cur = (curPreset() == p);
            drawBtn(fb, ctrlPresetBtn(p), pl, cur ? 0xFF315A31u : 0xFF2C3A50u, cur ? 0xFF8AE0A0u : 0xFF6A7E96u);
        }
        for (int i = 0; i < GA_COUNT; ++i) {                 // 12 действий активного пресета: лейбл + кнопка-клавиша
            Rect kb = ctrlKeyBtn(i);
            drawText(fb, PX() + 16*k, kb.y + 4*k, gaLabel(i), 0xFFD8E0EAu, 2*k);
            bool act = (rebindAction() == i);
            const char* kn = act ? "press key..." : (keyBindNames()[i].empty() ? "?" : keyBindNames()[i].c_str());
            drawBtn(fb, kb, kn, act ? 0xFF5A4A1Eu : 0xFF2C3A50u, act ? 0xFFE0C060u : 0xFF6A7E96u);
        }
    } else if (page == 0) {                                  // ── DEBUG 1/2: враги/анимация ──
        // (Render/Reference убраны из UI — фиксированные дефолты; консоль/CLI остаются)
        (void)faithful; (void)reference;
        std::snprintf(buf, sizeof(buf), "x%.1f", espd); drawSlider(fb, 0, "Enemy speed", espd, 0.2, 2.5, buf);
        std::snprintf(buf, sizeof(buf), "x%.1f", enemyTimerScale()); drawSlider(fb, 1, "Enemy timers", enemyTimerScale(), 0.3, 4.0, buf);
        std::snprintf(buf, sizeof(buf), "x%.2f", wallAnimSlow()); drawSlider(fb, 2, "Wall anim slow", wallAnimSlow(), 1.0, 4.0, buf);
        std::snprintf(tg, sizeof(tg), "Noclip: %s", noclip ? "ON" : "OFF");
        drawBtn(fb, tog(0), tg, noclip ? 0xFF315A31u : 0xFF2C3A50u, 0xFF6A7E96u);
        std::snprintf(tg, sizeof(tg), "Enemies: %s", enemiesOn ? "ON" : "OFF");
        drawBtn(fb, tog(1), tg, enemiesOn ? 0xFF315A31u : 0xFF6E2828u, 0xFF6A7E96u);
    } else {                                                 // ── DEBUG 2/2: карта/сим ──
        (void)gameMap;                                       // (Map GAME/CLASSIC и Internal res убраны из UI)
        std::snprintf(tg, sizeof(tg), "Cell IDs on map: %s", mapShowIds() ? "ON" : "OFF");
        drawBtn(fb, tog(0), tg, mapShowIds() ? 0xFF315A31u : 0xFF2C3A50u, 0xFF6A7E96u);
        std::snprintf(tg, sizeof(tg), "FPS-invariant sim: %s", fpsInvariant() ? "ON" : "OFF");
        drawBtn(fb, tog(1), tg, fpsInvariant() ? 0xFF315A31u : 0xFF2C3A50u, 0xFF6A7E96u);
    }

    if (!dbg && menuPreGame())                                    // ⭐только ДО геймплея (OPTIONS с титула, как в оригинале)
        drawBtn(fb, pwBtn(), "ENTER PASSWORD", 0xFF3A2C5Au, 0xFFB0A0E0u);
    std::snprintf(tg, sizeof(tg), "PAGE %d/%d  (>)", page + 1, npage);
    drawBtn(fb, pageBtn(), tg, 0xFF394A66u, 0xFF8AB0E0u);
    drawBtn(fb, saveBtn(),   "SAVE SETTINGS", 0xFF2C4A6Eu, 0xFF8AB0E0u);
    drawBtn(fb, quitBtn(),   "QUIT GAME",     0xFF6E2828u, 0xFFE08A8Au);
    drawBtn(fb, resumeBtn(), "BACK",          0xFF2C5A36u, 0xFF8AE0A0u);

    if (status && status[0])
        drawText(fb, 8*k, RY(6), status, 0xFFB8C0C8u, k);
}

void saveSettings(const char* path, double mv, double tn, double st, int fps, double es, bool fa, bool nc, bool ref, bool en, bool gm) {
    std::ofstream f(path);
    if (!f) return;
    f << "move_speed=" << mv << "\n"
      << "turn_speed=" << tn << "\n"
      << "hstretch="   << st << "\n"
      << "frame_limit=" << fps << "\n"
      << "enemy_speed=" << es << "\n"
      << "faithful="   << (fa ? 1 : 0) << "\n"
      << "noclip="     << (nc ? 1 : 0) << "\n"
      << "reference="  << (ref ? 1 : 0) << "\n"
      << "enemies="    << (en ? 1 : 0) << "\n"
      << "game_map="   << (gm ? 1 : 0) << "\n"
      << "aspect_mode=" << presentAspect() << "\n"
      << "filter_linear=" << (presentLinear() ? 1 : 0) << "\n"
      << "fullscreen=" << (presentFullscreen() ? 1 : 0) << "\n"
      << "map_cell_ids=" << (mapShowIds() ? 1 : 0) << "\n"
      << "render_scale=" << presentRenderScale() << "\n"
      << "player_physics=" << (playerPhysics() ? 1 : 0) << "\n"
      << "dist_octagonal=" << (gameDistOctagonal() ? 1 : 0) << "\n"
      << "inv_unlimited=" << (inventoryUnlimited() ? 1 : 0) << "\n"
      << "wall_anim_slow=" << wallAnimSlow() << "\n"
      << "stair_skew=" << faStairK() << "\n"
      << "stair_descend=" << faStairUni() << "\n"
      << "mouse_sens=" << mouseSensitivity() << "\n"
      << "sound_on=" << (soundOn() ? 1 : 0) << "\n"
      << "sound_vol=" << soundVolume() << "\n"
      << "sprite_size_k=" << faSpriteSizeK() << "\n"
      << "enemy_timer_scale=" << enemyTimerScale() << "\n"
      << "voice_pace=" << voicePace() << "\n"
      << "music_on=" << (musicOn() ? 1 : 0) << "\n"
      << "fps_invariant=" << (fpsInvariant() ? 1 : 0) << "\n"
      << "sim_base_fps=" << simBaseFps() << "\n"
      << "internal_res=" << faIntRes() << "\n";
    f << "control_preset=" << curPreset() << "\n";                                       // активный пресет управления
    for (int p = 0; p < NUM_PRESETS; ++p) for (int a = 0; a < GA_COUNT; ++a)             // 4 пресета × 12 биндингов
        f << gaIniKey(a) << "_p" << p << "=" << keyBindP(p, a) << "\n";
}

bool loadSettings(const char* path, double& mv, double& tn, double& st, int& fps, double& es, bool& fa, bool& nc, bool& ref, bool& en, bool& gm) {
    std::ifstream f(path);
    if (!f) return false;
    std::string line;
    while (std::getline(f, line)) {
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string k = line.substr(0, eq), v = line.substr(eq + 1);
        double d = std::atof(v.c_str());
        if      (k == "move_speed")  mv = d;
        else if (k == "turn_speed")  tn = d;
        else if (k == "hstretch")    st = d;
        else if (k == "frame_limit") fps = (int)d;
        else if (k == "enemy_speed") es = d;
        else if (k == "faithful")    fa = (d != 0);
        else if (k == "noclip")      nc = (d != 0);
        else if (k == "reference")   ref = (d != 0);
        else if (k == "enemies")     en = (d != 0);
        else if (k == "game_map")    gm = (d != 0);
        else if (k == "aspect_mode") presentAspect() = (int)d;
        else if (k == "filter_linear") presentLinear() = (d != 0);
        else if (k == "fullscreen")  presentFullscreen() = (d != 0);
        else if (k == "map_cell_ids") mapShowIds() = (d != 0);
        else if (k == "render_scale") presentRenderScale() = (int)d < 1 ? 1 : ((int)d > 3 ? 3 : (int)d);
        else if (k == "player_physics") playerPhysics() = (d != 0);
        else if (k == "dist_octagonal") gameDistOctagonal() = (d != 0);
        else if (k == "inv_unlimited")  inventoryUnlimited() = (d != 0);
        else if (k == "wall_anim_slow") wallAnimSlow() = (d < 1.0 ? 1.0 : (d > 4.0 ? 4.0 : d));
        else if (k == "stair_skew")     faStairK()   = (d < 0.0 ? 0.0 : (d > 3.0 ? 3.0 : d));
        else if (k == "stair_descend")  faStairUni() = (d < 0.0 ? 0.0 : (d > 1.0 ? 1.0 : d));
        else if (k == "mouse_sens")     mouseSensitivity() = (d < 0.0 ? 0.0 : (d > 2.0 ? 2.0 : d));
        else if (k == "sound_on")       soundOn() = (d != 0);
        else if (k == "sound_vol")      soundVolume() = (d < 0.0 ? 0.0 : (d > 1.0 ? 1.0 : d));
        else if (k == "sprite_size_k")  faSpriteSizeK() = (d < 0.3 ? 0.3 : (d > 3.5 ? 3.5 : d));
        else if (k == "enemy_timer_scale") enemyTimerScale() = (d < 0.3 ? 0.3 : (d > 4.0 ? 4.0 : d));
        else if (k == "voice_pace") voicePace() = (d < 0.0 ? 0.0 : (d > 2.0 ? 2.0 : d));
        else if (k == "music_on") musicOn() = (d != 0.0);
        else if (k == "fps_invariant") fpsInvariant() = (d != 0.0);
        else if (k == "sim_base_fps") simBaseFps() = (d < 10.0 ? 10.0 : (d > 60.0 ? 60.0 : d));
        else if (k == "internal_res") faIntRes() = ((int)d < 1 ? 1 : ((int)d > 4 ? 4 : (int)d));
        else if (parseKeyBind(k, (int)d)) {}                 // переназначаемые клавиши (4 пресета + control_preset)
    }
    return true;
}
