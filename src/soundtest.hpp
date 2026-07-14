#pragma once
// ── SOUND TEST — внутриигровое меню сверки звуков (F2) ────────────────────────────────────────────
// Прокрутка по hex-id таблицы 0xc5eb4 (↑/↓, PgUp/PgDn ±16), проигрывание (Enter/Space, авто при
// листании), СТОП (Backspace/S), подстройка FM-ноты (←/→; дефолт = нота из дизасма p2; R — сброс),
// длительность удержания FM (клавиши [ / ]). Для сверки «что есть какой звук» на слух с оригиналом.
// Подключается ПОСЛЕ FB (main.cpp), ui.hpp (drawText/fbBox), sound.hpp.
#include "ui.hpp"
#include "sound.hpp"
#include <cstdio>
#include <algorithm>

namespace sndtest {

inline bool& open()   { static bool b = false; return b; }
inline int&  cursor() { static int c = 1; return c; }         // текущий id
inline int*  noteOv() { static int n[0x100]; return n; }      // подстройка ноты по id; -1 = дефолт (p2 из дизасма)
inline bool& inited() { static bool b = false; return b; }
inline void  init()   { if (inited()) return; for (int i = 0; i < 0x100; ++i) noteOv()[i] = -1; inited() = true; }

inline int  count()      { return snd::sfxCount(); }
inline int  tblNote(int id){ return (id >= 0 && id < count()) ? snd::sfxTable()[id].p2 : 0; }
// Дефолт = нота из ROM (p2) + sfxTranspose (GEMS-октава +12) — как реально играет игра; override = абсолютная нота.
inline int  curNote(int id){ init(); int o = noteOv()[id]; return (o < 0) ? (tblNote(id) + snd::sfxTranspose()) : o; }
inline const char* typeName(int t){ switch (t){ case 0:return "MUS"; case 1:return "FM "; case 2:return "DAC"; case 3:return "DAC"; } return "?  "; }

// Подтверждённые на слух ярлыки (пополняется по мере сверки). Пусто = ещё не опознан.
inline const char* label(int id){
    switch (id){                                    // annotated/zt (weapons/objects_items/enemy_ai) + слух юзера
        case 0x19: return "handgun fire";
        case 0x1d: return "shotgun fire";
        case 0x1e: return "laser/pulse fire";
        case 0x27: return "revenant death";
        case 0x29: return "boss1 death";
        case 0x2d: return "enemy death (FH)";
        case 0x2f: return "boss3 death";
        case 0x31: return "boss2 death";
        case 0x67: return "door open/close";
        case 0x68: return "wound (+0x97 noise)";
        case 0x69: return "item COLLECTED";
        case 0x6a: return "medpak pickup";
        case 0x6b: return "confirm select";
        case 0x6c: return "mine / grenade";
        case 0x6d: return "fire extinguisher";
        case 0x6f: return "elevator";
        case 0x70: return "ALARM (long)";
        case 0x71: return "detect beep (x8t)";
        case 0x8a: return "explosion";
        case 0x96: return "rocket / enemy shot";
        case 0x3b: return "gameover explosion";
    }
    return "";
}

inline void play(){
    int id = cursor(); if (id < 0 || id >= count()) return;
    const snd::SfxDesc& d = snd::sfxTable()[id];
    snd::playSfxNote(id, (d.type == 1) ? curNote(id) : 0);
}
inline void moveCursor(int dd){
    int mx = count(); if (mx < 1) mx = 1;
    int n = cursor() + dd; if (n < 0) n = 0; if (n >= mx) n = mx - 1; cursor() = n;
}
inline void adjNote(int dd){ init(); int id = cursor(); int v = curNote(id) + dd; if (v < 0) v = 0; if (v > 127) v = 127; noteOv()[id] = v; }
inline void resetNote(){ init(); noteOv()[cursor()] = -1; }

// ── Отрисовка оверлея (замороженный кадр под ним рисуется вызывающим) ──
inline void draw(FB& fb){
    init();
    const int k = uiScale();
    // затемняющая подложка панели
    Rect panel{ 24 * k, 20 * k, FBW - 48 * k, FBH - 40 * k };
    if (panel.w < 100) panel = Rect{ 8, 8, FBW - 16, FBH - 16 };
    fbBox(fb, panel, 0xE8101018u, 0xFF7E92AAu);

    const int cx = panel.x + panel.w / 2;
    drawTextC(fb, cx, panel.y + 6 * k, "SOUND  TEST", 0xFFFFD050u, 2 * k);

    int id = cursor();
    const snd::SfxDesc& d = snd::sfxTable()[id];

    // строка деталей текущего звука
    char det[96];
    if (d.type == 1)
        std::snprintf(det, sizeof det, "id 0x%02X  FM  patch %d   note %d %s", id, d.p1, curNote(id),
                      (noteOv()[id] >= 0 ? "(tuned)" : "(rom)"));
    else if (d.type == 0)
        std::snprintf(det, sizeof det, "id 0x%02X  MUSIC song %d  (skipped in test)", id, d.p1);
    else
        std::snprintf(det, sizeof det, "id 0x%02X  DAC  p1=%d -> sample %d%s", id, d.p1, snd::dacSampleIdx(d),
                      snd::dacRemap48() ? " (swap 0x30)" : " (raw)");
    drawTextC(fb, cx, panel.y + 26 * k, det, 0xFF80E0FFu, k);
    { const char* lb = label(id); if (*lb) drawTextC(fb, cx, panel.y + 36 * k, lb, 0xFF60FF80u, k); }

    // список: окно строк вокруг курсора
    const int rowH   = 9 * k;
    const int listTop= panel.y + 50 * k;
    const int listBot= panel.y + panel.h - 22 * k;
    const int rows   = (listBot - listTop) / rowH;
    int first = id - rows / 2; if (first < 0) first = 0;
    if (first + rows > count()) first = count() - rows; if (first < 0) first = 0;

    for (int r = 0; r < rows; ++r){
        int i = first + r; if (i < 0 || i >= count()) continue;
        const snd::SfxDesc& e = snd::sfxTable()[i];
        int y = listTop + r * rowH;
        bool cur = (i == id);
        if (cur) fbBox(fb, Rect{ panel.x + 6 * k, y - 1, panel.w - 12 * k, rowH }, 0xFF243450u, 0xFF4A6A9Au);
        char line[128];
        if (e.type == 1)
            std::snprintf(line, sizeof line, "0x%02X  %s  p%-2d  n%-3d %s", i, typeName(e.type), e.p1, curNote(i), label(i));
        else if (e.type == 0)
            std::snprintf(line, sizeof line, "0x%02X  %s  song%-2d       %s", i, typeName(e.type), e.p1, label(i));
        else
            std::snprintf(line, sizeof line, "0x%02X  %s  s%-3d        %s", i, typeName(e.type), snd::dacSampleIdx(e), label(i));
        uint32_t col = cur ? 0xFFFFFFFFu : (e.type == 0 ? 0xFF708090u : (e.type == 1 ? 0xFFB8D0E8u : 0xFFC8B090u));
        drawText(fb, panel.x + 12 * k, y, line, col, k);
    }

    // футер: подсказки + hold
    char foot[128];
    std::snprintf(foot, sizeof foot, "UP/DN id  PgUp/Dn 16  ENTER play  L/R note  BKSP stop  R reset  [ ] hold=%.2fs  F2/ESC exit",
                  snd::sfxHold());
    drawTextC(fb, cx, panel.y + panel.h - 12 * k, foot, 0xFFB8C0C8u, k);
}

}  // namespace sndtest
