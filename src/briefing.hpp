// briefing.hpp — ЗАСТАВКИ/БРИФИНГИ ZT (@0xCB1E4): скроллящийся текст поверх графического фона.
// Тексты/фоны извлечены в gd.briefings. Механика: строки появляются снизу, едут вверх (верт.скролл $FF2706).
// Up ускоряет, Start пропускает. Подключается ПОСЛЕ FB, select_screen.hpp (drawSelText), rom/gamedata.hpp.
#pragma once
#include "rom/gamedata.hpp"

// Индексы заставок (порядок в gd.briefings) — по game-flow ZT. Screens: 0=[intro,mission] 1=[decoy,central] 2=[subbase] 4=victory 5=badend.
enum BriefId { BR_INTRO = 0, BR_MISSION = 1, BR_DECOY = 2, BR_CENTRAL = 3, BR_SUBBASE = 4, BR_VICTORY = 5,
               BR_BADEND1 = 6, BR_BADEND2 = 7, BR_EXPLODE = 8, BR_EXIT = 9 };

struct BriefingState {
    int    idx = -1;         // индекс активной заставки (-1 = неактивна)
    double scroll = 0;       // пиксельный вертикальный скролл (растёт со временем)
    bool   fast = false;     // Up зажат → ускорение (ZT btst #0 → задержка 0 вместо 3)
    // общая высота текста (для детекта конца — весь текст уехал вверх)
    bool done(const GameData& gd) const {
        if (idx < 0 || idx >= (int)gd.briefings.size()) return true;
        int lines = (int)gd.briefings[idx].lines.size();
        return scroll > (double)(224 + lines * 16 + 16);   // всё проскроллило за верх
    }
    bool paused = false;                          // ⭐ROM cb24c/cb256: удержание DOWN (бит1) или A (бит6) = СТОП текста
    void tick() { if (paused) return; scroll += fast ? 3.0 : 1.0; }   // ZT: ~1px/кадр (Up → быстрее). VBLANK 60fps.
};

// ── РЕНДЕР заставки: фон (статичен) + скроллящийся текст поверх. K = масштаб native→fb. ──
inline void drawBriefing(FB& fb, const GameData& gd, const BriefingState& st, int K) {
    if (st.idx < 0 || st.idx >= (int)gd.briefings.size()) { for (auto& p : fb.px) p = 0xFF000000u; return; }
    const GameData::Briefing& b = gd.briefings[st.idx];
    // ФОН (native 320×224 → ×K); если нет — чёрный
    for (int y = 0; y < 224 * K; ++y) for (int x = 0; x < FBW; ++x)
        fb.px[(size_t)y * FBW + x] = (!b.bg.empty() && x < 320 * K) ? b.bg[(size_t)(y / K) * 320 + (x / K)] : 0xFF000000u;
    for (int y = 224 * K; y < FBH; ++y) for (int x = 0; x < FBW; ++x) fb.px[(size_t)y * FBW + x] = 0xFF000000u;
    // ТЕКСТ скроллит снизу вверх: строка i на native-Y = 224 - scroll + i*16. Шрифт 8×16 selFont ИНДЕКСАМИ,
    // перекрашен палитрой брифинга (b.textCol = линия 3, серо-красный — своя у каждой заставки, НЕ сине-белая экрана выбора).
    const int lineH = 16;
    int baseY = 224 - (int)st.scroll;
    for (int i = 0; i < (int)b.lines.size(); ++i) {
        int ny = baseY + i * lineH;
        if (ny <= -lineH || ny >= 224) continue;
        int px = 8 * K;
        for (char chc : b.lines[i]) {
            int e = (unsigned char)chc - 0x20;
            if (e >= 0 && e < 96) {
                const uint8_t* g = gd.selFont.idx[e];
                for (int r = 0; r < 16; ++r) for (int c = 0; c < 8; ++c) {
                    uint8_t n = g[r * 8 + c]; if (n == 0) continue;           // прозрачный
                    uint32_t col = b.textCol[n];                             // цвет из палитры брифинга (линия 3)
                    for (int sy = 0; sy < K; ++sy) for (int sx = 0; sx < K; ++sx)
                        fb.put(px + c * K + sx, ny * K + r * K + sy, col);
                }
            }
            px += 8 * K;
        }
    }
}
