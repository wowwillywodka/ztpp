#pragma once
// Экранное меню настроек (ESC) с кликабельными мышью элементами + сохранение/загрузка в файл.
// Подключается ПОСЛЕ объявления FB в main.cpp (использует FB).
#include "font8x8.hpp"
#include "tuning.hpp"     // playerPhysics/gameDistOctagonal — тумблеры reference-точности (меню)
#include <cstdio>
#include <cstring>
#include <cmath>
#include <fstream>
#include <string>

// ---- Настройки ПРЕЗЕНТАЦИИ (вариант A: внутренний рендер фикс., меняем только финальный блит в окно) ----
//   aspect: 0 = 4:3 (как ЭЛТ-оригинал), 1 = 1:1 (квадратный пиксель, чётко), 2 = stretch (на всё окно).
//   filter: false = nearest (чёткие пиксели), true = linear (сглаживание). fullscreen — borderless desktop.
inline int&  presentAspect()     { static int a = 0; return a; }
inline bool& presentLinear()     { static bool l = false; return l; }
inline bool& presentFullscreen() { static bool f = false; return f; }
inline const char* presentAspectName() {
    switch (presentAspect()) { case 1: return "1:1 (square)"; case 2: return "Stretch"; default: return "4:3 (CRT)"; }
}
// Карта: показывать cell ID (hex-код клетки) поверх значков. Переключается в настройках.
inline bool& mapShowIds() { static bool s = false; return s; }
// TAB-карта: false = МЕНЮ ПАУЗЫ (хедер здание/этаж + SECURED, как ориг.); true = плоская FULL MAP. Тумблер настроек.
inline bool& pauseFullMap() { static bool s = false; return s; }
// РЕНДЕР-МАСШТАБ: внутренний fb = 640×RS (1/2/3). Игра/кокпит апскейлятся (та же картинка), но оверлеи (радар, cell ID,
// текст) рисуются на бо́льшем числе пикселей → мельче/чётче. Применяется при старте (меню сохраняет, действует с перезапуска).
inline int& presentRenderScale() { static int s = 1; return s; }
inline int  uiScale() { return presentRenderScale(); }   // масштаб меню/HUD-текста = RS

// ---- Текст битмап-шрифтом 8x8 ----
// Настоящий ZT-шрифт (Letters), ставится из main после загрузки GameData. Если у символа есть
// ZT-глиф — рисуем им (заглавные/цифры/.-:?), иначе fallback на public-domain FONT8X8.
inline const ZtFont* g_uiFont = nullptr;

inline void drawChar(FB& fb, int x, int y, char ch, uint32_t col, int sc) {
    unsigned char uc = (unsigned char)ch;
    const uint8_t* g = nullptr;
    if (g_uiFont && g_uiFont->have) {
        unsigned char up = (uc >= 'a' && uc <= 'z') ? (unsigned char)(uc - 32) : uc;  // ZT = заглавные
        if (up < 128 && g_uiFont->supported[up]) g = g_uiFont->glyph[up];
    }
    if (!g) {                                   // fallback: public-domain 8×8
        if (uc < 32 || uc > 127) uc = '?';
        g = FONT8X8[(int)uc - 32];
    }
    for (int ry = 0; ry < 8; ++ry)
        for (int rx = 0; rx < 8; ++rx)
            if (g[ry] & (1 << rx))
                for (int sy = 0; sy < sc; ++sy)
                    for (int sx = 0; sx < sc; ++sx)
                        fb.put(x + rx * sc + sx, y + ry * sc + sy, col);
}
inline int  textW(const char* s, int sc) { int n = 0; while (s[n]) ++n; return n * 8 * sc; }
inline void drawText(FB& fb, int x, int y, const char* s, uint32_t col, int sc) {
    for (; *s; ++s) { drawChar(fb, x, y, *s, col, sc); x += 8 * sc; }
}
inline void drawTextC(FB& fb, int cx, int y, const char* s, uint32_t col, int sc) {
    drawText(fb, cx - textW(s, sc) / 2, y, s, col, sc);
}
// Рендер строки ПРОИЗВОЛЬНЫМ ZT-шрифтом 8×8 (напр. gd.fontAlt = Font2 для имени уровня на паузе).
inline void drawCharFont(FB& fb, int x, int y, char ch, uint32_t col, int sc, const ZtFont* font) {
    unsigned char uc = (unsigned char)ch; const uint8_t* g = nullptr;
    if (font && font->have) { unsigned char up = (uc >= 'a' && uc <= 'z') ? (unsigned char)(uc - 32) : uc;
                              if (up < 128 && font->supported[up]) g = font->glyph[up]; }
    if (!g) { if (uc < 32 || uc > 127) uc = '?'; g = FONT8X8[(int)uc - 32]; }
    for (int ry = 0; ry < 8; ++ry) for (int rx = 0; rx < 8; ++rx) if (g[ry] & (1 << rx))
        for (int sy = 0; sy < sc; ++sy) for (int sx = 0; sx < sc; ++sx) fb.put(x + rx*sc + sx, y + ry*sc + sy, col);
}
inline void drawTextFontC(FB& fb, int cx, int y, const char* s, uint32_t col, int sc, const ZtFont* font) {
    int n = 0; while (s[n]) ++n; int x = cx - (n * 8 * sc) / 2;
    for (; *s; ++s) { drawCharFont(fb, x, y, *s, col, sc, font); x += 8 * sc; }
}

// ---- Большой шрифт ZT (Font_grph 8×16, ЦВЕТНОЙ со своей палитрой): меню/настройки/сюжет/пауза ----
inline const ZtFontBig* g_uiFontBig = nullptr;   // ставится из main после загрузки GameData

inline void drawCharBig(FB& fb, int x, int y, char ch, uint32_t col, int sc) {
    unsigned char uc = (unsigned char)ch;
    unsigned char up = (uc >= 'a' && uc <= 'z') ? (unsigned char)(uc - 32) : uc;  // Font_grph = заглавные
    if (!(g_uiFontBig && g_uiFontBig->have) || up >= 128 || !g_uiFontBig->supported[up]) {
        drawChar(fb, x, y, ch, col, sc * 2);    // fallback: public-domain 8×8, ×2 по высоте (col)
        return;
    }
    // Рисуем НАСТОЯЩЕЙ палитрой шрифта (контур/заливка/тень), idx 0 = прозрачный. col игнорируется.
    for (int ry = 0; ry < 16; ++ry)
        for (int rx = 0; rx < 8; ++rx) {
            uint8_t idx = g_uiFontBig->pix[up][ry][rx];
            if (idx == 0) continue;
            uint32_t c = g_uiFontBig->pal[idx];
            for (int sy = 0; sy < sc; ++sy)
                for (int sx = 0; sx < sc; ++sx)
                    fb.put(x + rx * sc + sx, y + ry * sc + sy, c);
        }
}
inline int  textWBig(const char* s, int sc) { int n = 0; while (s[n]) ++n; return n * 8 * sc; }
inline void drawTextBig(FB& fb, int x, int y, const char* s, uint32_t col, int sc) {
    for (; *s; ++s) { drawCharBig(fb, x, y, *s, col, sc); x += 8 * sc; }
}
inline void drawTextBigC(FB& fb, int cx, int y, const char* s, uint32_t col, int sc) {
    drawTextBig(fb, cx - textWBig(s, sc) / 2, y, s, col, sc);
}

// ---- Примитивы ----
struct Rect { int x, y, w, h; bool has(int mx, int my) const { return mx >= x && mx < x + w && my >= y && my < y + h; } };

inline void fbDim(FB& fb, int pct) {                 // затемнить весь экран (pct% яркости)
    for (auto& p : fb.px) {
        uint32_t c = p;
        uint32_t r = ((c >> 16) & 0xFF) * pct / 100;
        uint32_t g = ((c >> 8)  & 0xFF) * pct / 100;
        uint32_t b = ( c        & 0xFF) * pct / 100;
        p = 0xFF000000u | (r << 16) | (g << 8) | b;
    }
}
inline void fbBox(FB& fb, const Rect& r, uint32_t fill, uint32_t border) {
    fb.rect(r.x, r.y, r.w, r.h, fill);
    for (int i = 0; i < r.w; ++i) { fb.put(r.x + i, r.y, border); fb.put(r.x + i, r.y + r.h - 1, border); }
    for (int i = 0; i < r.h; ++i) { fb.put(r.x, r.y + i, border); fb.put(r.x + r.w - 1, r.y + i, border); }
}

// ---- Геометрия меню (база 640x640, всё ×uiScale() для render-scale) ----
namespace menu {
    const int NPAGE = 4;                                     // страниц настроек (движение/гейм/видео/точность)
    inline int K()  { return uiScale(); }                   // масштаб всего меню = render scale
    inline int PX() { return 140 * K(); }
    inline int PY() { return 40  * K(); }
    inline int PW() { return 360 * K(); }
    inline int PH() { return 560 * K(); }
    inline int sliderY(int i) { static const int y[4] = {84, 144, 204, 264}; return y[i] * K(); }
    inline Rect minusBtn(int i) { int k = K(); return {PX() + 20*k,  sliderY(i) + 18*k, 30*k, 26*k}; }
    inline Rect bar(int i)      { int k = K(); return {PX() + 58*k,  sliderY(i) + 18*k, 240*k, 26*k}; }
    inline Rect plusBtn(int i)  { int k = K(); return {PX() + 306*k, sliderY(i) + 18*k, 30*k, 26*k}; }
    inline Rect tog(int j)      { int k = K(); return {PX() + 20*k, (320 + j*38)*k, PW() - 40*k, 30*k}; }
    inline Rect pageBtn()       { int k = K(); return {PX() + 20*k, 458*k, PW() - 40*k, 30*k}; }
    inline Rect saveBtn()       { int k = K(); return {PX() + 20*k, 496*k, PW() - 40*k, 30*k}; }
    inline Rect quitBtn()       { int k = K(); return {PX() + 20*k, 526*k, PW() - 40*k, 22*k}; }
    inline Rect resumeBtn()     { int k = K(); return {PX() + 20*k, 550*k, PW() - 40*k, 22*k}; }
    inline Rect rscaleBtn()     { int k = K(); return {PX() + 20*k, sliderY(0), PW() - 40*k, 30*k}; }  // стр.3: render scale
}

enum MenuAction {
    MA_NONE = 0,
    MA_MV_DEC, MA_MV_INC, MA_MV_BAR,     // 1..3  скорость движения
    MA_TN_DEC, MA_TN_INC, MA_TN_BAR,     // 4..6  скорость поворота
    MA_ST_DEC, MA_ST_INC, MA_ST_BAR,     // 7..9  растяжка
    MA_FL_DEC, MA_FL_INC, MA_FL_BAR,     // 10..12 лимит кадров
    MA_ES_DEC, MA_ES_INC, MA_ES_BAR,     // 13..15 СКОРОСТЬ ВРАГОВ
    MA_WA_DEC, MA_WA_INC, MA_WA_BAR,     // замедление анимации стен
    MA_SK_DEC, MA_SK_INC, MA_SK_BAR,     // лестница: текстурный скос
    MA_SD_DEC, MA_SD_INC, MA_SD_BAR,     // лестница: сила спуска
    MA_MS_DEC, MA_MS_INC, MA_MS_BAR,     // чувствительность мыши
    MA_REND, MA_NOCLIP, MA_REFERENCE, MA_ENEMIES, MA_MAP, MA_MAPIDS, MA_PAUSEFULLMAP,
    MA_ASPECT, MA_FILTER, MA_FULLSCREEN, MA_RSCALE,      // СТР.3 видео: аспект / фильтр / фуллскрин / render scale
    MA_PHYSICS, MA_GAMEDIST, MA_INVUNLIM, MA_SND,        // СТР.4 точность: физика / дистанция / инвентарь / звук вкл-выкл
    MA_SV_DEC, MA_SV_INC, MA_SV_BAR,                     // громкость звука
    MA_PAGE, MA_SAVE, MA_QUIT, MA_RESUME
};

// Хит-тест клика (с учётом СТРАНИЦЫ). Для *_BAR возвращает долю 0..1 в frac.
inline MenuAction menuHit(int mx, int my, int page, double& frac) {
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
    } else {                                                 // СТР.4: точность/звук — физика / дистанция / инвентарь / звук + громкость
        if (minusBtn(0).has(mx, my)) return MA_SV_DEC;
        if (plusBtn(0).has(mx, my))  return MA_SV_INC;
        if (bar(0).has(mx, my)) { barFrac(bar(0)); return MA_SV_BAR; }
        if (tog(0).has(mx, my)) return MA_PHYSICS;
        if (tog(1).has(mx, my)) return MA_GAMEDIST;
        if (tog(2).has(mx, my)) return MA_INVUNLIM;
        if (tog(3).has(mx, my)) return MA_SND;
    }
    if (pageBtn().has(mx, my))   return MA_PAGE;
    if (saveBtn().has(mx, my))   return MA_SAVE;
    if (quitBtn().has(mx, my))   return MA_QUIT;
    if (resumeBtn().has(mx, my)) return MA_RESUME;
    return MA_NONE;
}

// Один ползунок: лейбл + [-] [полоса+значение] [+]
inline void drawSlider(FB& fb, int i, const char* label, double val, double lo, double hi, const char* valstr) {
    using namespace menu;
    int k = K();
    drawText(fb, PX() + 20*k, sliderY(i), label, 0xFFD8E0EAu, 2*k);
    Rect mb = minusBtn(i), b = bar(i), pb = plusBtn(i);
    fbBox(fb, mb, 0xFF38465Cu, 0xFF6A7E96u); drawTextC(fb, mb.x + mb.w / 2, mb.y + 6*k, "-", 0xFFFFFFFFu, 2*k);
    fbBox(fb, pb, 0xFF38465Cu, 0xFF6A7E96u); drawTextC(fb, pb.x + pb.w / 2, pb.y + 6*k, "+", 0xFFFFFFFFu, 2*k);
    // полоса: трек + заливка по доле + рамка (рамку рисуем вручную, чтобы не затереть заливку)
    double f = (val - lo) / (hi - lo); if (f < 0) f = 0; if (f > 1) f = 1;
    fb.rect(b.x, b.y, b.w, b.h, 0xFF1C2430u);
    fb.rect(b.x, b.y, (int)(b.w * f), b.h, 0xFF3A6EA8u);
    const uint32_t brd = 0xFF6A7E96u;
    for (int j = 0; j < b.w; ++j) { fb.put(b.x + j, b.y, brd); fb.put(b.x + j, b.y + b.h - 1, brd); }
    for (int j = 0; j < b.h; ++j) { fb.put(b.x, b.y + j, brd); fb.put(b.x + b.w - 1, b.y + j, brd); }
    drawTextC(fb, b.x + b.w / 2, b.y + 6*k, valstr, 0xFFFFFFFFu, 2*k);
}
inline void drawBtn(FB& fb, const Rect& r, const char* label, uint32_t fill, uint32_t border) {
    fbBox(fb, r, fill, border);
    drawTextC(fb, r.x + r.w / 2, r.y + (r.h - 16 * menu::K()) / 2, label, 0xFFFFFFFFu, 2 * menu::K());
}

// Полная отрисовка меню поверх кадра.
inline void drawMenu(FB& fb, double mv, double tn, double st, int fps, double espd, int page,
                     bool faithful, bool noclip, bool reference, bool enemiesOn, bool gameMap, const char* status) {
    using namespace menu;
    int k = K();
    fbDim(fb, 28);
    fbBox(fb, {PX(), PY(), PW(), PH()}, 0xFF12161Eu, 0xFF7E92AAu);
    char title[24]; std::snprintf(title, sizeof(title), "SETTINGS  %d/%d", page + 1, NPAGE);
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
    } else {                                                 // ── СТР.4: ТОЧНОСТЬ + ЗВУК (reference, ZT_PHYSICS §11) ──
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
    }

    std::snprintf(tg, sizeof(tg), "PAGE %d/%d  (>)", page + 1, NPAGE);
    drawBtn(fb, pageBtn(), tg, 0xFF394A66u, 0xFF8AB0E0u);
    drawBtn(fb, saveBtn(),   "SAVE SETTINGS", 0xFF2C4A6Eu, 0xFF8AB0E0u);
    drawBtn(fb, quitBtn(),   "QUIT GAME",     0xFF6E2828u, 0xFFE08A8Au);
    drawBtn(fb, resumeBtn(), "RESUME",        0xFF2C5A36u, 0xFF8AE0A0u);

    if (status && status[0])
        drawTextC(fb, PX() + PW() / 2, PY() + PH() - 12*k, status, 0xFFB8C0C8u, k);
}

// ---- Настройки: файл key=value ----
inline void saveSettings(const char* path, double mv, double tn, double st, int fps, double es, bool fa, bool nc, bool ref, bool en, bool gm) {
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
      << "sound_vol=" << soundVolume() << "\n";
}
// Ранний ридер ТОЛЬКО render_scale (нужен до создания fb/окна — задаёт их размер). 1..3.
inline int loadRenderScaleEarly(const char* path) {
    std::ifstream f(path); std::string line;
    while (std::getline(f, line)) {
        size_t eq = line.find('='); if (eq == std::string::npos) continue;
        if (line.substr(0, eq) == "render_scale") { int v = std::atoi(line.substr(eq + 1).c_str()); return v < 1 ? 1 : (v > 3 ? 3 : v); }
    }
    return 1;   // дефолт ×1 (= ztpp_settings.ini)
}
inline bool loadSettings(const char* path, double& mv, double& tn, double& st, int& fps, double& es, bool& fa, bool& nc, bool& ref, bool& en, bool& gm) {
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
    }
    return true;
}
