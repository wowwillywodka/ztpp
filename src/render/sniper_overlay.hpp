// ztpp — src/render/sniper_overlay.hpp: ⭐ФОНОВЫЙ СНАЙПЕР эп2-крыши (ROM 2404/2682/26da/27f8/28b4/28f6).
// VERIFIED побайтно 2026-07-22 (annotated/zt/enemy_ai.asm + screens_hud.asm + weapons.asm 14076).
//
// Система целиком (дизасм):
//  - ЗДАНИЕ на панораме: рисуется КАЖДЫЙ кадр при городском фоне (гейт -$58e6==0, ed3e), независимо
//    от снайпера. Стопка VDP-спрайтов: X d2=(−угол·2+0x210−Y·256/64)&0x3FF (едет с панорамой+параллакс),
//    невидимо при d2==0 или d2≥0x1A0; слот-таблица @0x2510 (d3=столбцы по краям), ряды Y с шагом 0x10
//    от 0xCF−floor·8−cabin/16 до <0xF8, тайлы ряда @0x2600. Тайлы 0x515.. = ROM 0x16DA58 (48, загрузка
//    уровня 2bd8), пал.линия 2 = городская палитра 0x2132.
//  - СНАЙПЕР = ОДИН спрайт 8×8 (Y=0xC9, X=окно(-$6f8b)+0x10+d2), тайл-анимация @0x265E по phase&~1:
//    0x545..0x549 вылезает, 0x54A стреляет, обратно, 0x54B убит. Банк 0x16E058 (46 тайлов, спавн 2682).
//  - СПАВН 2682: диспетчер celltype @0xE420 — пока игрок СТОИТ на клетке 0x27/0x28; гейт: жизни
//    (-$6f89, =3 на уровень @1058) ≠0 && не активен → активация, окно=rnd&0x78.
//  - ФАЗЫ 27f8 (++ каждый тик): 1..9 вылезает; 0xA = флаг рикошетов + звук 0x68+0x97 + ХИТ-ЧЕК;
//    0xB..0xF звук каждый тик (очередь); 0x10 хит-чек; 0x16 снять флаг рикошетов; 0x20 спрятался
//    (active=0, жизнь НЕ тратится → респавн в новом окне); 0x24 убит (active=0, жизнь−1).
//    Хит: игрок в ct 0x28 — всегда, 0x27 — только БЕЗ приседа (pitch>−10) → d800 X=0x64 = 15 HP.
//  - РИКОШЕТЫ 26da (пока флаг -$6f87): каждый тик ОДИН случайный спрайт «пуля бьёт в крышу» вокруг
//    игрока (Y=0xB0+rnd&0x3F, X=0xE0+rnd&0x7F), тайлы/флипы по сектору угла камеры; pri=1 (поверх 3D).
//    Это и есть видимый «патрон»/имитация рикошета.
//  - ТРАССЕР 28b4/28f6: ракета, улетевшая за ВОСТОЧНЫЙ край карты (X≥0x1F00) над 0x27/0x28 (weapons
//    14076: 0x27 требует Z>0) → спрайт-«снаряд» на фоне (@0x297A: 4 размера 8/16/24/32px, чередование
//    hflip) от X выстрела (0x120=центр); если снайпер в |X−0x120|<0x40 → захват: X трассера каждый тик
//    (3·prev+snipX)/4, на 8-м тике «долетел»: звук 0x18, жизни=1, фаза=0x20 → 0x24 → снайпер убит
//    НАВСЕГДА. Без захвата (таймер 8) трассер просто улетает. Спрайт трассера pri=0 (за стенами).
//  - УБИЙСТВО ХИТСКАНОМ (хвост 167b0 @16862): этаж 0, игрок в 0x28 (любой pitch) или 0x27 c pitch≥−5
//    (из глубокого приседа НЕ пострелять), |X_прицела(0x120) − X_снайпера| ≤ 3 → фаза=0x20 → 0x24 →
//    жизнь−1. Звука убийства НЕТ (только link-пакет 29ba, в порте no-op).
//
// Слои (VDP-приоритеты): здание/снайпер/трассер pri=0 → НАД панорамой (Plane B), ПОД 3D-видом →
// порт рисует их в bgAt (только там, где виден фон). Рикошеты pri=1 → поверх кадра (drawHi).
#pragma once
#include "rom.hpp"
#include "gfx.hpp"     // readPalette (городская палитра 0x2132)
#include <cstdint>
#include <cstring>
#include <cmath>
#include <vector>

namespace snip {

// ── ROM-адреса (релиз ZT Rev A) ─────────────────────────────────────────────
inline constexpr size_t BLD_GFX  = 0x16DA58;  // 48 тайлов здания (VRAM 0xA2A0 = тайл 0x515)
inline constexpr size_t SNP_GFX  = 0x16E058;  // 46 тайлов снайпер/трассер/рикошеты (VRAM 0xA8A0 = 0x545)
inline constexpr size_t PAL_CITY = 0x2132;    // палитра городской панорамы = пал.линия 2 спрайтов
inline constexpr size_t TAB_SLOT = 0x2510;    // слоты колонок здания (int16, чтение до отрицательного)
inline constexpr size_t TAB_ROOF = 0x2600;    // тайл-слово на РЯД здания (47 слов)
inline constexpr size_t TAB_SNIP = 0x265E;    // тайл снайпера по phase&~1 (18 слов)
inline constexpr size_t TAB_TRAC = 0x297A;    // трассер: 8 записей (dY, size, attr, dX)

// ── графика (декод раз на загрузку) ─────────────────────────────────────────
struct Gfx {
    bool ok = false;
    uint32_t bld[48][64];    // ARGB; индекс 0 → 0 (прозрачно)
    uint32_t bank[46][64];
    int16_t  slot[120];      // @0x2510 (0xF0 байт до 0x2600 хватает: слоты 0..0x13 + перетекание)
    uint16_t roof[47];
    uint16_t snipTile[18];
    int16_t  trac[8][4];
};
inline Gfx& gfx() { static Gfx g; return g; }

inline void decode8x8(const Rom& rom, size_t addr, const Palette& pal, uint32_t out[64]) {
    for (int r = 0; r < 8; ++r)
        for (int c = 0; c < 8; ++c) {
            uint8_t b = rom.u8(addr + r * 4 + (c >> 1));
            uint8_t pix = (c & 1) ? (b & 0xF) : (b >> 4);
            out[r * 8 + c] = pix ? pal.c[pix] : 0u;          // 0 = прозрачный тексель
        }
}

inline bool init(const Rom& rom) {
    Gfx& g = gfx();
    Palette pal = readPalette(rom, PAL_CITY);
    for (int i = 0; i < 48; ++i) decode8x8(rom, BLD_GFX + (size_t)i * 32, pal, g.bld[i]);
    for (int i = 0; i < 46; ++i) decode8x8(rom, SNP_GFX + (size_t)i * 32, pal, g.bank[i]);
    for (int i = 0; i < 120; ++i) g.slot[i] = (int16_t)rom.u16(TAB_SLOT + (size_t)i * 2);
    for (int i = 0; i < 47;  ++i) g.roof[i] = rom.u16(TAB_ROOF + (size_t)i * 2);
    for (int i = 0; i < 18;  ++i) g.snipTile[i] = rom.u16(TAB_SNIP + (size_t)i * 2);
    for (int i = 0; i < 8;   ++i) for (int k = 0; k < 4; ++k)
        g.trac[i][k] = (int16_t)rom.u16(TAB_TRAC + ((size_t)i * 4 + k) * 2);
    g.ok = true;
    return true;
}

// ── состояние (RAM -$6f8c..-$6f80) ──────────────────────────────────────────
struct State {
    int     lives  = 3;      // -$6f89 (3 на уровень, 1058)
    bool    active = false;  // -$6f8c
    uint8_t phase  = 0;      // -$6f88
    int     window = 0;      // -$6f8b = rnd&0x78 (где на фасаде вылезает)
    bool    repos  = false;  // -$6f87 (флаг рикошетов, фазы 0xA..0x15)
    // трассер (28b4/28f6)
    uint8_t tracerT = 0;     // -$6f83
    bool    lock    = false; // -$6f84
    int     aimX = 0x120;    // -$6f82 (SAT X трассера)
    int     aimY = 0xD0;     // -$6f80
    int     scrX = 0;        // -$6f86 (SAT X снайпера, обновляет buildFrame)
    // рикошет текущего тика (26da; перекат в тике = кадр ROM)
    bool ricOk = false;
    int  ricX = 0, ricY = 0, ricW = 1, ricH = 1, ricTile = 0;
    bool ricHf = false, ricVf = false;
    int  lastPyRam = 0;      // -$7212 последнего кадра (для d5 рикошета 2748/276c)
    uint32_t rng = 0x2f6c1a35u;
};
inline State& st() { static State s; return s; }
inline uint32_t rnd() { State& s = st(); s.rng = s.rng * 1664525u + 1013904223u; return s.rng; }

inline void reset() {                        // загрузка уровня (1054/1058)
    State& s = st();
    s.lives = 3; s.active = false; s.phase = 0; s.repos = false;
    s.tracerT = 0; s.lock = false; s.ricOk = false; s.scrX = 0;
}

// ── тик (события наружу: main играет звуки/наносит урон) ────────────────────
struct TickEv { bool burst = false; bool hitCheck = false; bool shot = false; };

inline void rollRicochet(int angRam) {       // 26da: один случайный спрайт «удар пули» на тик
    State& s = st(); Gfx& g = gfx();
    uint32_t r = rnd();
    int d1 = (int)((r >> 16) & 0x3F), d2 = (int)((r >> 8) & 0x7F);
    int sector = angRam & 0x1FF;
    auto fixedSpark = [&](bool hflip) {      // 271c: фикс. спрайт 16×8 (тайлы 0x551/0x553)
        int attr = 0xC551;
        if (d1 < 0x10)      attr += 0x1002;  // тайл 0x553 + vflip
        else if (d1 >= 0x30) attr += 2;      // тайл 0x553
        s.ricY = 0xB0 + d1; s.ricX = 0xE0 + d2;
        s.ricW = 2; s.ricH = 1;
        s.ricTile = (attr & 0x7FF) - 0x545;
        s.ricVf = (attr & 0x1000) != 0; s.ricHf = hflip;
    };
    if (sector < 0x40 || sector >= 0x1C0) { fixedSpark(true);  s.ricOk = true; return; }   // 2712 (+hflip)
    if (sector >= 0xC0 && sector < 0x140) { fixedSpark(false); s.ricOk = true; return; }   // 2742
    // боковые сектора (2748: bias 0x220 / 276c: bias 0x20 + eor обоих флипов)
    bool eor = (sector >= 0x140);
    int bias = eor ? 0x20 : 0x220;
    int d5 = (int)((uint16_t)(-(int)(int16_t)angRam * 2 + bias - (s.lastPyRam >> 6) + s.window) & 0x3FF);
    s.ricY = 0xB0 + d1; s.ricX = 0xE0 + d2;
    int attr = 0xC54C + (s.ricY & 3);
    if (s.ricY >= 0xC6 && s.ricY <= 0xDA) attr = 0xC54F;
    if (s.ricX <= d5 + 0x18 && s.ricX >= d5 - 0x18) attr = 0xC550;  // 27b6: в створе снайпера
    bool vf = (s.ricY <= 0xD0);                                     // 27ce
    bool hf = (s.ricX <= d5);                                       // 27d8
    if (eor) { vf = !vf; hf = !hf; }                                // 278e: eori #$1800
    s.ricW = 1; s.ricH = 1;
    s.ricTile = (attr & 0x7FF) - 0x545;
    s.ricVf = vf; s.ricHf = hf;
    s.ricOk = true;
}

// onRoofCell = СОБЫТИЕ ВХОДА в клетку 0x27/0x28 (не «пока стоит»!): ROM step-on диспетчер e3f8
// вызывается из радара e870 ТОЛЬКО при смене кэша клетки игрока (-$71ea) → 2682 одноразово на вход.
// После «спрятался» (фаза 0x20) респавн — только при следующем входе на roof-клетку.
inline TickEv tick(bool onRoofCell, int angRam) {
    State& s = st();
    TickEv ev;
    // 2682: спавн по входу в клетку 0x27/0x28 (диспетчер e420), жизни ≠0, не активен
    if (onRoofCell && s.lives != 0 && !s.active) {
        s.active = true; s.phase = 0; s.repos = false;
        s.window = (int)(rnd() & 0x78);
    }
    if (s.active) {                                  // 27f8: ++фаза каждый тик
        ++s.phase;
        if (s.phase == 0x20)      { s.active = false; s.phase = 0; }                 // спрятался (жизнь цела)
        else if (s.phase == 0x24) { s.active = false; s.repos = false; --s.lives; }  // убит (28ae: subq)
        else if (s.phase == 0x0A) { s.repos = true; ev.burst = true; ev.hitCheck = true; }
        else if (s.phase == 0x10) { ev.hitCheck = true; }
        else if (s.phase == 0x16) { s.repos = false; }
        else if (s.phase > 0x0A && s.phase < 0x10) ev.burst = true;                  // 282e: очередь
    }
    if (s.active && s.repos) rollRicochet(angRam);   // 240e: рикошет на тик
    else s.ricOk = false;
    if (s.tracerT) {                                 // 28f6 (гейт 2418: таймер ≠0)
        if (s.lock) { s.aimX = (3 * s.aimX + s.scrX) >> 2; s.phase = 0x14; }
        --s.tracerT;
        if (s.tracerT == 8) {                        // «долетел» (достижимо только при захвате)
            ev.shot = true;                          // звук 0x18
            s.lives = 1; s.phase = 0x20; s.lock = false;   // 292c/2932: → 0x21..0x24 → жизни 1−1=0 НАВСЕГДА
        }
    }
    return ev;
}

// 28b4: взвод трассера (ракета улетела за восточный край; satX = 0x120 = центр)
inline void aimTrigger(int satX) {
    State& s = st();
    s.aimX = satX; s.aimY = 0xD0;
    int d = s.scrX - 0x120; if (d < 0) d = -d;
    if (s.active && d < 0x40) { s.lock = true; s.tracerT = 0x10; s.phase = 0x14; }
    else                      { s.lock = false; s.tracerT = 8; }
}

// 167b0 хвост (16862): убийство хитсканом. Условия клетки/этажа/питча проверяет вызывающий;
// тут — активность + совпадение X прицела (центр 0x120) с X снайпера ±3.
inline bool aimKill() {
    State& s = st();
    if (!s.active) return false;
    int d = 0x120 - s.scrX; if (d < 0) d = -d;
    if (d > 3) return false;
    s.phase = 0x20;                                  // 168a2 → фазы 0x21..0x24 → жизнь−1
    return true;
}

// ── рендер ──────────────────────────────────────────────────────────────────
// Low-pri оверлей: буфер MD-вьюпорта 256×80 (0 = прозрачно), сэмплится в bgAt (за стенами).
inline uint32_t* ovBuf() { static std::vector<uint32_t> b(256 * 80); return b.data(); }
inline bool& ovOn() { static bool v = false; return v; }

// Блит спрайта w×h тайлов (column-major, как MD SAT) в буфер вьюпорта; SAT-коорд (офсет 128),
// вьюпорт = экран MD (32,40)-(288,120): vx = satX−160, vy = satY−168.
template <class TileArr>
inline void blitSprite(TileArr tiles, int nTiles, int base, int satX, int satY,
                       int w, int h, bool hf, bool vf) {
    uint32_t* ov = ovBuf();
    for (int tc = 0; tc < w; ++tc)
        for (int tr = 0; tr < h; ++tr) {
            int ti = base + tc * h + tr;
            if (ti < 0 || ti >= nTiles) continue;
            int ox = (hf ? (w - 1 - tc) : tc) * 8, oy = (vf ? (h - 1 - tr) : tr) * 8;
            for (int r = 0; r < 8; ++r)
                for (int c = 0; c < 8; ++c) {
                    uint32_t px = tiles[ti][(vf ? 7 - r : r) * 8 + (hf ? 7 - c : c)];
                    if (!px) continue;
                    int vx = satX - 160 + ox + c, vy = satY - 168 + oy + r;
                    if (vx < 0 || vx >= 256 || vy < 0 || vy >= 80) continue;
                    ov[vy * 256 + vx] = px;
                }
        }
}

// 2404: построение оверлея на кадр. cityBg = городская панорама (гейт ed3e: -$58e6==0).
// angRam = МД-угол (-$71fc, 0..511), pyRam = -$7212 (Y·256), cabin = $FF116E (−64..64).
inline void buildFrame(bool cityBg, int angRam, int pyRam, int floorNo, int cabin) {
    State& s = st(); Gfx& g = gfx();
    ovOn() = false;
    if (!cityBg || !g.ok) { s.scrX = 0; return; }
    std::memset(ovBuf(), 0, 256 * 80 * sizeof(uint32_t));
    ovOn() = true;
    s.lastPyRam = pyRam;
    int d2 = (int)((uint16_t)(-(int)(int16_t)angRam * 2 + 0x210 - (pyRam >> 6)) & 0x3FF);
    if (d2 == 0 || d2 >= 0x1A0) return;               // 2442: здание за кадром (scrX НЕ трогается)
    s.scrX = 0;                                       // 2444: clr -$6f86 (перезапишется при active)
    // ЗДАНИЕ (2484..2508) — рисуем ПЕРВЫМ (низ стека SAT: спрайт снайпера в ROM эмитится раньше = выше)
    int d3 = 0x13, bx = d2;
    if (d2 < 0xA0)        { d3 = d2 >> 3; bx = (d2 & 7) + 0x98; }   // левый край: слот режет колонки
    else if (d2 > 0x100)  { d3 = (0x1A0 - d2) >> 3; }               // правый край
    int d5 = 0xCF - floorNo * 8 - (cabin >> 4);       // 24ba: Y первого ряда (этаж+кабина лифта)
    int rowIdx = 0;
    while (d5 <= 0x98 && rowIdx < 46) { d5 += 0x10; ++rowIdx; }     // 24d2: верхний клип → скип рядов
    while (d5 < 0xF8 && rowIdx < 47) {                              // 2500..2508
        uint16_t rowAttr = g.roof[rowIdx++];
        int col = d3 * 6, x = bx;                     // 24aa: слот = d3·12 байт (lsl#2 + lsl#1 каскадом) = 6 слов
        while (col < 120 && g.slot[col] >= 0) {
            int size = g.slot[col++];
            int w = ((size >> 10) & 3) + 1, h = ((size >> 8) & 3) + 1;
            blitSprite(g.bld, 48, (rowAttr & 0x7FF) - 0x515, x, d5, w, h,
                       (rowAttr & 0x800) != 0, (rowAttr & 0x1000) != 0);
            x += 0x20;
        }
        d5 += 0x10;
    }
    // СНАЙПЕР (244e): один 8×8, Y=0xC9, X=окно+0x10+d2 — поверх здания
    if (s.active) {
        int satX = s.window + 0x10 + d2;
        s.scrX = satX;
        int fi = (s.phase & ~1) >> 1; if (fi > 17) fi = 17;    // @265E: 18 слов (фазы 0..0x23)
        uint16_t a = g.snipTile[fi];
        blitSprite(g.bank, 46, (a & 0x7FF) - 0x545, satX, 0xC9, 1, 1,
                   (a & 0x800) != 0, (a & 0x1000) != 0);
    }
    // ТРАССЕР (2948..2976): один спрайт из @297A по timer&7 — поверх всего low-слоя
    if (s.tracerT) {
        const int16_t* t = g.trac[s.tracerT & 7];
        int size = (uint16_t)t[1];
        int w = ((size >> 10) & 3) + 1, h = ((size >> 8) & 3) + 1;
        uint16_t a = (uint16_t)t[2];
        blitSprite(g.bank, 46, (a & 0x7FF) - 0x545, s.aimX + t[3], s.aimY + t[0], w, h,
                   (a & 0x800) != 0, (a & 0x1000) != 0);
    }
}

// Сэмпл low-оверлея для пикселя рендер-вью (x∈[0,W), y∈[0,H)); 0 = прозрачно.
inline uint32_t sampleLow(int x, int W, int y, int H) {
    if (!ovOn()) return 0;
    int vx = (int)((int64_t)x * 256 / (W > 0 ? W : 1));
    int vy = (int)((int64_t)y * 80 / (H > 0 ? H : 1));
    if (vx < 0 || vx >= 256 || vy < 0 || vy >= 80) return 0;
    return ovBuf()[vy * 256 + vx];
}

// Hi-pri (рикошеты 26da, pri=1): поверх готового кадра, В 3D-окно (vx,vy,vw,vh) — MD-вью
// (32,40)-(288,120) маппится в прямоугольник (референс: vx=32,vy=40,vw=256,vh=80 → 1:1).
template <class Put>
inline void drawHi(Put put, int vx, int vy, int vw, int vh) {
    State& s = st(); Gfx& g = gfx();
    if (!s.ricOk || !g.ok || !ovOn()) return;
    for (int tc = 0; tc < s.ricW; ++tc)
        for (int tr = 0; tr < s.ricH; ++tr) {
            int ti = s.ricTile + tc * s.ricH + tr;
            if (ti < 0 || ti >= 46) continue;
            int ox = (s.ricHf ? (s.ricW - 1 - tc) : tc) * 8, oy = (s.ricVf ? (s.ricH - 1 - tr) : tr) * 8;
            for (int r = 0; r < 8; ++r)
                for (int c = 0; c < 8; ++c) {
                    uint32_t px = g.bank[ti][(s.ricVf ? 7 - r : r) * 8 + (s.ricHf ? 7 - c : c)];
                    if (!px) continue;
                    int mx = s.ricX - 160 + ox + c, my = s.ricY - 168 + oy + r;   // MD-вью коорд [0,256)×[0,80)
                    if (mx < 0 || mx >= 256 || my < 0 || my >= 80) continue;
                    if (vw == 256 && vh == 80) { put(vx + mx, vy + my, px); continue; }   // референс 1:1
                    int x0 = vx + mx * vw / 256, x1 = vx + (mx + 1) * vw / 256;   // скейл-блок (nearest)
                    int y0 = vy + my * vh / 80,  y1 = vy + (my + 1) * vh / 80;
                    for (int yy = y0; yy < y1; ++yy) for (int xx = x0; xx < x1; ++xx) put(xx, yy, px);
                }
        }
}

} // namespace snip
