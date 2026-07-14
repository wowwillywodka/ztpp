// ztpp — src/ui.cpp: холодные функции меню/настроек (отрисовка меню, клики, сохранение/загрузка).
// Мелкие хелперы (drawText/drawChar/синглтоны презентации) — inline в ui.hpp.
#include "ui.hpp"

MenuAction menuHit(int mx, int my, int page, double& frac) {
    using namespace menu;
    auto barFrac = [&](Rect b) { frac = (double)(mx - b.x) / b.w; if (frac < 0) frac = 0; if (frac > 1) frac = 1; };
    if (page == 0) {                                         // СТР.1: движение/видео — 4 ползунка + render/reference/noclip
        for (int i = 0; i < 4; ++i) {
            if (minusBtn(i).has(mx, my)) return (MenuAction)(MA_MV_DEC + i * 3);
            if (plusBtn(i).has(mx, my))  return (MenuAction)(MA_MV_INC + i * 3);
            if (bar(i).has(mx, my)) { barFrac(bar(i)); return (MenuAction)(MA_MV_BAR + i * 3); }
        }
        if (tog(0).has(mx, my)) return MA_REND;
        if (tog(1).has(mx, my)) return MA_REFERENCE;
        if (tog(2).has(mx, my)) return MA_NOCLIP;
    } else if (page == 1) {                                  // СТР.2: геймплей — Enemy speed + Wall anim + Enemies/Map
        if (minusBtn(0).has(mx, my)) return MA_ES_DEC;
        if (plusBtn(0).has(mx, my))  return MA_ES_INC;
        if (bar(0).has(mx, my)) { barFrac(bar(0)); return MA_ES_BAR; }
        if (minusBtn(1).has(mx, my)) return MA_WA_DEC;
        if (plusBtn(1).has(mx, my))  return MA_WA_INC;
        if (bar(1).has(mx, my)) { barFrac(bar(1)); return MA_WA_BAR; }
        if (minusBtn(2).has(mx, my)) return MA_SK_DEC;
        if (plusBtn(2).has(mx, my))  return MA_SK_INC;
        if (bar(2).has(mx, my)) { barFrac(bar(2)); return MA_SK_BAR; }
        if (minusBtn(3).has(mx, my)) return MA_SD_DEC;
        if (plusBtn(3).has(mx, my))  return MA_SD_INC;
        if (bar(3).has(mx, my)) { barFrac(bar(3)); return MA_SD_BAR; }
        if (tog(0).has(mx, my)) return MA_ENEMIES;
        if (tog(1).has(mx, my)) return MA_MAP;
        if (tog(2).has(mx, my)) return MA_MAPIDS;
        if (tog(3).has(mx, my)) return MA_PAUSEFULLMAP;
    } else if (page == 2) {                                  // СТР.3: видео — render scale / мышь / аспект / фильтр / фуллскрин
        if (rscaleBtn().has(mx, my)) return MA_RSCALE;
        if (minusBtn(1).has(mx, my)) return MA_MS_DEC;
        if (plusBtn(1).has(mx, my))  return MA_MS_INC;
        if (bar(1).has(mx, my)) { barFrac(bar(1)); return MA_MS_BAR; }
        if (tog(0).has(mx, my)) return MA_ASPECT;
        if (tog(1).has(mx, my)) return MA_FILTER;
        if (tog(2).has(mx, my)) return MA_FULLSCREEN;
    } else if (page == 3) {                                  // СТР.4: точность/звук — физика / дистанция / инвентарь / звук + громкость
        if (minusBtn(0).has(mx, my)) return MA_SV_DEC;
        if (plusBtn(0).has(mx, my))  return MA_SV_INC;
        if (bar(0).has(mx, my)) { barFrac(bar(0)); return MA_SV_BAR; }
        if (tog(0).has(mx, my)) return MA_PHYSICS;
        if (tog(1).has(mx, my)) return MA_GAMEDIST;
        if (tog(2).has(mx, my)) return MA_INVUNLIM;
        if (tog(3).has(mx, my)) return MA_SND;
    } else {                                                 // СТР.5: CONTROLS — выбор пресета + клик по кнопке-клавише → ребинд
        for (int p = 0; p < NUM_PRESETS; ++p)
            if (ctrlPresetBtn(p).has(mx, my)) return (MenuAction)(MA_PRESET + p);
        for (int i = 0; i < GA_COUNT; ++i)
            if (ctrlKeyBtn(i).has(mx, my)) return (MenuAction)(MA_KEYBIND + i);
    }
    if (pageBtn().has(mx, my))   return MA_PAGE;
    if (saveBtn().has(mx, my))   return MA_SAVE;
    if (quitBtn().has(mx, my))   return MA_QUIT;
    if (resumeBtn().has(mx, my)) return MA_RESUME;
    return MA_NONE;
}

void drawMenu(FB& fb, double mv, double tn, double st, int fps, double espd, int page,
                     bool faithful, bool noclip, bool reference, bool enemiesOn, bool gameMap, const char* status) {
    using namespace menu;
    int k = K();
    fbDim(fb, 28);
    fbBox(fb, {PX(), PY(), PW(), PH()}, 0xFF12161Eu, 0xFF7E92AAu);
    char title[24]; std::snprintf(title, sizeof(title), page == CTRL_PAGE ? "CONTROLS  %d/%d" : "SETTINGS  %d/%d", page + 1, NPAGE);
    drawTextBigC(fb, PX() + PW() / 2, PY() + 8*k, title, 0xFFFFD050u, 2*k);
    char buf[32], tg[40];

    if (page == 0) {                                         // ── СТР.1: движение / видео ──
        std::snprintf(buf, sizeof(buf), "%.3f", mv); drawSlider(fb, 0, "Move speed", mv, 0.015, 0.30, buf);
        std::snprintf(buf, sizeof(buf), "%.3f", tn); drawSlider(fb, 1, "Turn speed", tn, 0.008, 0.15, buf);
        std::snprintf(buf, sizeof(buf), "%.2f", st); drawSlider(fb, 2, "Stretch",    st, 0.5,   4.0,  buf);
        std::snprintf(buf, sizeof(buf), "%d fps", fps); drawSlider(fb, 3, "Frame limit", (double)fps, 5, 60, buf);
        std::snprintf(tg, sizeof(tg), "Render: %s", faithful ? "FAITHFUL" : "DDA");
        drawBtn(fb, tog(0), tg, 0xFF2C3A50u, 0xFF6A7E96u);
        std::snprintf(tg, sizeof(tg), "Reference (HUD): %s", reference ? "ON" : "OFF");
        drawBtn(fb, tog(1), tg, reference ? 0xFF315A31u : 0xFF2C3A50u, 0xFF6A7E96u);
        std::snprintf(tg, sizeof(tg), "Noclip: %s", noclip ? "ON" : "OFF");
        drawBtn(fb, tog(2), tg, noclip ? 0xFF315A31u : 0xFF2C3A50u, 0xFF6A7E96u);
    } else if (page == 1) {                                  // ── СТР.2: геймплей ──
        std::snprintf(buf, sizeof(buf), "x%.1f", espd); drawSlider(fb, 0, "Enemy speed", espd, 0.2, 2.5, buf);
        std::snprintf(buf, sizeof(buf), "x%.2f", wallAnimSlow()); drawSlider(fb, 1, "Wall anim slow", wallAnimSlow(), 1.0, 4.0, buf);
        std::snprintf(buf, sizeof(buf), "%.2f", faStairK());   drawSlider(fb, 2, "Stair skew",    faStairK(),   0.0, 3.0, buf);
        std::snprintf(buf, sizeof(buf), "%.2f", faStairUni()); drawSlider(fb, 3, "Stair descend", faStairUni(), 0.0, 1.0, buf);
        std::snprintf(tg, sizeof(tg), "Enemies: %s", enemiesOn ? "ON" : "OFF");
        drawBtn(fb, tog(0), tg, enemiesOn ? 0xFF315A31u : 0xFF6E2828u, 0xFF6A7E96u);
        std::snprintf(tg, sizeof(tg), "Map: %s", gameMap ? "GAME" : "CLASSIC");
        drawBtn(fb, tog(1), tg, 0xFF2C3A50u, 0xFF6A7E96u);
        std::snprintf(tg, sizeof(tg), "Cell IDs on map: %s", mapShowIds() ? "ON" : "OFF");
        drawBtn(fb, tog(2), tg, mapShowIds() ? 0xFF315A31u : 0xFF2C3A50u, 0xFF6A7E96u);
        std::snprintf(tg, sizeof(tg), "TAB map: %s", pauseFullMap() ? "FULL MAP" : "PAUSE MENU");
        drawBtn(fb, tog(3), tg, pauseFullMap() ? 0xFF315A31u : 0xFF2C3A50u, 0xFF6A7E96u);
    } else if (page == 2) {                                  // ── СТР.3: видео (presentation, вариант A) ──
        std::snprintf(tg, sizeof(tg), "Render scale: %dx (restart)", presentRenderScale());
        drawBtn(fb, rscaleBtn(), tg, 0xFF2C3A50u, 0xFF6A7E96u);
        std::snprintf(buf, sizeof(buf), "%.2f", mouseSensitivity()); drawSlider(fb, 1, "Mouse sens (0=off)", mouseSensitivity(), 0.0, 2.0, buf);
        std::snprintf(tg, sizeof(tg), "Aspect: %s", presentAspectName());
        drawBtn(fb, tog(0), tg, 0xFF2C3A50u, 0xFF6A7E96u);
        std::snprintf(tg, sizeof(tg), "Filter: %s", presentLinear() ? "LINEAR" : "NEAREST");
        drawBtn(fb, tog(1), tg, 0xFF2C3A50u, 0xFF6A7E96u);
        std::snprintf(tg, sizeof(tg), "Fullscreen: %s", presentFullscreen() ? "ON" : "OFF");
        drawBtn(fb, tog(2), tg, presentFullscreen() ? 0xFF315A31u : 0xFF2C3A50u, 0xFF6A7E96u);
    } else if (page == 3) {                                  // ── СТР.4: ТОЧНОСТЬ + ЗВУК (reference, ZT_PHYSICS §11) ──
        // Звук-громкость = слайдер слот 0 (y=84). Заголовок «Accuracy» — над тумблерами, не поверх слайдера.
        std::snprintf(buf, sizeof(buf), "%.2f", soundVolume()); drawSlider(fb, 0, "Sound volume", soundVolume(), 0.0, 1.0, buf);
        std::snprintf(tg, sizeof(tg), "Sound: %s", soundOn() ? "ON" : "OFF");
        drawBtn(fb, tog(3), tg, soundOn() ? 0xFF315A31u : 0xFF6E2828u, 0xFF6A7E96u);
        drawText(fb, PX() + 20*k, 296*k, "Accuracy vs original", 0xFFB8C0C8u, 2*k);
        std::snprintf(tg, sizeof(tg), "Player physics: %s", playerPhysics() ? "ZT INERTIA" : "FREE (no phys)");
        drawBtn(fb, tog(0), tg, playerPhysics() ? 0xFF315A31u : 0xFF2C3A50u, 0xFF6A7E96u);
        std::snprintf(tg, sizeof(tg), "Distance: %s", gameDistOctagonal() ? "OCTAGONAL (ZT)" : "EUCLID");
        drawBtn(fb, tog(1), tg, gameDistOctagonal() ? 0xFF315A31u : 0xFF2C3A50u, 0xFF6A7E96u);
        std::snprintf(tg, sizeof(tg), "Inventory: %s", inventoryUnlimited() ? "UNLIMITED" : "ZT (5 slots)");
        drawBtn(fb, tog(2), tg, inventoryUnlimited() ? 0xFF315A31u : 0xFF2C3A50u, 0xFF6A7E96u);
    } else {                                                 // ── СТР.5: CONTROLS — 4 пресета управления + переназначение клавиш ──
        for (int p = 0; p < NUM_PRESETS; ++p) {              // ряд выбора пресета (текущий подсвечен зелёным)
            char pl[8]; std::snprintf(pl, sizeof(pl), "P%d", p + 1);
            bool cur = (curPreset() == p);
            drawBtn(fb, ctrlPresetBtn(p), pl, cur ? 0xFF315A31u : 0xFF2C3A50u, cur ? 0xFF8AE0A0u : 0xFF6A7E96u);
        }
        for (int i = 0; i < GA_COUNT; ++i) {                 // 12 действий активного пресета: лейбл + кнопка-клавиша
            Rect kb = ctrlKeyBtn(i);
            drawText(fb, PX() + 16*k, kb.y + 6*k, gaLabel(i), 0xFFD8E0EAu, 2*k);
            bool act = (rebindAction() == i);
            const char* kn = act ? "press key..." : (keyBindNames()[i].empty() ? "?" : keyBindNames()[i].c_str());
            drawBtn(fb, kb, kn, act ? 0xFF5A4A1Eu : 0xFF2C3A50u, act ? 0xFFE0C060u : 0xFF6A7E96u);
        }
    }

    std::snprintf(tg, sizeof(tg), "PAGE %d/%d  (>)", page + 1, NPAGE);
    drawBtn(fb, pageBtn(), tg, 0xFF394A66u, 0xFF8AB0E0u);
    drawBtn(fb, saveBtn(),   "SAVE SETTINGS", 0xFF2C4A6Eu, 0xFF8AB0E0u);
    drawBtn(fb, quitBtn(),   "QUIT GAME",     0xFF6E2828u, 0xFFE08A8Au);
    drawBtn(fb, resumeBtn(), "RESUME",        0xFF2C5A36u, 0xFF8AE0A0u);

    if (status && status[0])
        drawTextC(fb, PX() + PW() / 2, PY() + PH() - 12*k, status, 0xFFB8C0C8u, k);
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
      << "sprite_size_k=" << faSpriteSizeK() << "\n";
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
        else if (parseKeyBind(k, (int)d)) {}                 // переназначаемые клавиши (4 пресета + control_preset)
    }
    return true;
}
