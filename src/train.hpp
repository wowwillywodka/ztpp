// ztpp — src/train.hpp: ⭐ПОЕЗД МЕТРО ZTU (в ZT подсистемы нет). VERIFIED по дизасму 2026-07-24:
// FSM 0xDE8DA (тик каждый кадр игровой петли 0x150C..0x1AAA, хук @0x1522), инит 0xDE7F0 (хук @0x1506
// на ВХОДЕ в петлю = вход на уровень), state-запись $FFCCDE {+0 укз. d0-байта, +4 state, +6 таймер,
// +8 укз. скрипта}, указатель записи $FFCD26.
//
// ПОЕЗД = РЯД ИЗ 8 КЛЕТОК в RAM-карте ($FF7CDA); врайтеры (этаж = 0xA5−d0, смещение ряда @0xDE572):
//   DE83E: 22 02 6F 22 22 6F 02 22 (стены + ДВЕ ДВЕРИ 0x02/ct6) = поезд СТОИТ, двери работают;
//   DE88C: клетки 0x67..0x6E (ct1, сплошные)                    = поезд УШЁЛ (барьер путей).
// Текстуры: аниматоры-близнецы DEA8E/DEAF8 (байт-в-байт одинаковы) пишут 8 байт скрипта за тик в
// ПЕРВОЕ слово texorder-записей клеток 0x67..0x6E ($FF73CA+0x338..0x370 → texorderT[cell*4+0]).
// Скрипты (ROM 0xDE578..0xDE7E0, обе фазы в одном буфере 0x268):
//   off 0x000 (DE578, 40 строк, state 1): поезд ПРИБЫВАЕТ (e8/f0/f4 заполняют слева + кадры дверей
//     fc/f9/fd/fa/fe/fb); 40-я строка заезжает в DE6B0[0] → финал «полный состав, двери закрыты»;
//   off 0x138 (DE6B0, 38 строк, state 3): двери закрываются → состав уезжает → пустой туннель (f8×8).
// ФАЗОВЫЙ ЦИКЛ (тик = кадр петли ~15fps):
//   2 СТОИТ (ряд-двери; гейт: игрок «внутри» [сторона этажа @DE7E0] → таймер=10 — НЕ УЕДЕТ с игроком;
//     иначе таймер--, на 0 → 6) → 6 (ряд-барьер + скрипт 0x138) → 3 отъезд-анимация (0x25) →
//   4 путей пусто (0x37; ряд-барьер остаётся — «не выходить на пути») → 1 прибытие (скрипт 0, 0x27;
//     в конце DE83E ставит ряд-двери) → 5 → 2 (таймер 0x37).
// Стороны по этажам @0xDE7E0: {4,3,4,2,3,2,2, 0×9} — 1: y≤2 · 2: x≤2 · 3: x≥29 · 4: y≥29 (оси
//   заякорены рядами этажей 0/2: юг y=29 ⇒ сторона 4 = y≥29 ⇒ -0x7212=Y). Сторона 0 (этажи 7+,
//   реактор) = FSM ЗАМОРОЖЕНА (таймер не тикает — ROM bra done без subq).
// Этаж 0: ряд x10..17 y29 (динамика; DE572[0]=0x3AA). Этаж 2: x11..18 y29 (DE572[2]=0x3AB) — состав
//   0x67..0x6E ставится ИНИТОМ НАВСЕГДА (декорация; его текстуры листаются ТЕМИ ЖЕ скриптами — как в
//   ROM: texorder глобален на уровень, стоящий состав «мигает» фазами цикла — воспроизводим как есть).
// Звука в FSM НЕТ (хуки 0xDEB62/0xDEB64 = голые rts).
#pragma once
#include "level.hpp"      // Level: setCell / texorderT
#include <vector>
#include <cstdint>

struct SubwayTrain {
    // ── данные ROM (байт-в-байт) ──
    static constexpr uint8_t ROW_STAND[8] = {0x22,0x02,0x6F,0x22,0x22,0x6F,0x02,0x22}; // DE86A..DE886
    static constexpr uint8_t ROW_GONE[8]  = {0x67,0x68,0x69,0x6A,0x6B,0x6C,0x6D,0x6E}; // DE8B8..DE8D4
    static constexpr uint8_t SIDES[16]    = {4,3,4,2,3,2,2,0, 0,0,0,0,0,0,0,0};        // DE7E0
    static constexpr int F0_X = 10, F0_Y = 29;   // DE572[0] = 0x3AA = y29·32+x10 (этаж 0)
    static constexpr int F2_X = 11, F2_Y = 29;   // DE572[2] = 0x3AB (этаж 2, декорация)
    static constexpr int SCR_ARRIVE = 0x000;     // DE578 (фаза 1: прибытие)
    static constexpr int SCR_DEPART = 0x138;     // DE6B0 (фаза 3: отъезд)

    std::vector<uint8_t> script;   // ROM 0xDE578..0xDE7E0 (0x268 Б); ПУСТО = подсистемы нет (не-ZTU)
    int  state = 2, timer = 0x37, off = SCR_DEPART;
    uint16_t def0[8] = {0};        // дефолт face0-texorder клеток 0x67..0x6E (восстановление на init:
    bool haveDef = false;          //  ROM переливает texorder из ROM на КАЖДОЙ загрузке уровня)

    bool active() const { return !script.empty(); }

    void rowTo(Level& lv, int fl, int x, int y, const uint8_t* ids) {
        for (int k = 0; k < 8; ++k) lv.setCell(fl, x + k, y, ids[k]);
    }
    void scriptRow(Level& lv) {    // аниматор DEA8E/DEAF8: 8 байт скрипта → texorder face0 0x67..0x6E
        if (off + 8 > (int)script.size()) return;   // ROM в буфер укладывается ровно; страховка
        for (int k = 0; k < 8; ++k) lv.texorderT[(size_t)(0x67 + k) * 4 + 0] = script[(size_t)off + k];
        off += 8;
    }
    void init(Level& lv) {         // DE7F0 (вход на уровень); texorder-дефолт = как level-load ROM
        if (!active()) return;
        if (!haveDef) { for (int k = 0; k < 8; ++k) def0[k] = lv.texorderT[(size_t)(0x67 + k) * 4]; haveDef = true; }
        else          { for (int k = 0; k < 8; ++k) lv.texorderT[(size_t)(0x67 + k) * 4] = def0[k]; }
        rowTo(lv, 0, F0_X, F0_Y, ROW_STAND);        // DE7F8: d0=*DE546(0xA5) → DE83E, этаж 0
        rowTo(lv, 2, F2_X, F2_Y, ROW_GONE);         // DE826-цикл: группа @DE547 (0xA3) → DE88C, этаж 2
        state = 2; timer = 0x37; off = SCR_DEPART;  // DE80C..DE81E: state=2, таймер=0x37, +8=DE6B0
    }
    // ОДИН ROM-тик FSM DE8DA. px/py/floor — КЛЕТКА игрока и его текущий этаж (ROM -0x7214/-0x7212>>8,
    // -0x6FD4). Звать 1×/сим-тик (сим-тик порта = ROM-тик, simDt()=1).
    void tick(Level& lv, int px, int py, int floor) {
        if (!active()) return;
        switch (state) {
        case 1:                                     // ПРИБЫТИЕ (скрипт SCR_ARRIVE)
            scriptRow(lv);                          // de928: jsr DEA8E
            if (--timer >= 0) break;                // de934: subq/bpl
            state = 5; rowTo(lv, 0, F0_X, F0_Y, ROW_STAND);   // de940/de946: state=5 + DE83E
            break;
        case 2: {                                   // СТОИТ: гейт по позиции игрока (de94e)
            int side = (floor >= 0 && floor < 16) ? SIDES[floor] : 0;
            if (!side) break;                       // de96e: сторона 0 → FSM заморожена
            bool inside = false;
            switch (side) { case 1: inside = (py <= 2);    break;   // de996: d1≤2
                            case 2: inside = (px <= 2);    break;   // de9a2: d0≤2
                            case 3: inside = (px >= 0x1D); break;   // de9ae: d0≥0x1D
                            case 4: inside = (py >= 0x1D); break; } // de9ba: d1≥0x1D
            if (inside) { timer = 0x0A; break; }    // de9dc: игрок внутри → поезд не уедет
            if (--timer == 0) state = 6;            // de9c6: subq/beq → 6 (иначе ждём дальше)
            break; }
        case 3:                                     // ОТЪЕЗД-анимация (скрипт SCR_DEPART)
            scriptRow(lv);                          // de9ec: jsr DEAF8
            if (--timer >= 0) break;
            state = 4; timer = 0x37;                // dea00: стоянка «пусто»
            break;
        case 4:                                     // ПУТЕЙ ПУСТО (ряд-барьер стоит)
            if (--timer != 0) break;                // dea10: subq/bne
            state = 1; off = SCR_ARRIVE; timer = 0x27;   // dea18: скрипт=DE578
            break;
        case 5: state = 2; timer = 0x37; break;     // dea30 (deb62=rts, звука нет)
        case 6:                                     // СТАРТ ОТЪЕЗДА
            state = 3; timer = 0x25; off = SCR_DEPART;   // dea4c: скрипт=DE6B0
            rowTo(lv, 0, F0_X, F0_Y, ROW_GONE);     // dea70: DE88C(0xA5) — ряд-барьер
            break;
        }
    }
};
