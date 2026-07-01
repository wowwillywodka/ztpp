// ztpp — ЭТАП 0: модель игровых данных (мод-готовность + мульти-билд).
//
// GameData — единый контейнер ассетов, которым ВЛАДЕЕТ порт: уровни, банк текстур стен, палитра,
// фоны-панорамы, рампы. Рендеры и логика работают ТОЛЬКО с GameData (через Level/WallBank/Palette/
// Panorama), не зная об ИСТОЧНИКЕ и БИЛДЕ. Источник наполняет GameData:
//   loadGameDataFromRom(gd, rom)   — детект билда → per-build адреса/форматы.  ← адреса ROM ТОЛЬКО тут.
//   (будущее) loadGameDataFromOztd(gd, path) — моды.
//
// МУЛЬТИ-БИЛД (цель — «верный каждому билду движок»): различия по билду = (1) данные/адреса (этот слой),
// (2) КОНСТАНТЫ движка → EngineProfile, (3) редкие форки логики → флаг gd.build. Модель/потребители —
// build-agnostic. Сейчас полностью подключён ZT; ZTU/нем./прото — заготовки (расширяются по одному).
#pragma once
#include "rom.hpp"
#include "gfx.hpp"
#include "level.hpp"
#include "cells.hpp"
#include "background.hpp"
#include "font8x8.hpp"   // ZtFont — настоящий шрифт ZT (декодируется из ROM ниже)
#include <vector>
#include <cstdint>
#include <cstdio>

enum class Build { ZT, ZTU, BZT_June, BZT_July, ZT_German, Unknown };

// HUD/кокпит (референс-режим): экран 320×224, вьюпорт 3D-вида = 256×80 @ (32,40) — нативный рендер
// 128×80 показан ×2 по горизонтали. Раскладка вскрыта по дырке (idx==0) в тайлкарте HUD (ztextractor).
static const int HUD_W = 320, HUD_H = 224;
static const int HUD_VX = 32, HUD_VY = 40, HUD_VW = 256, HUD_VH = 80;
// Радар/миникарта на HUD-кокпите (центр-низ). Из дизасма построителя миникарты FUN_0000e816:
// цикл 10×10 (D7=D6=9), VRAM-адрес D2 = 0xC81A + 0x80/строку → первая строка 0xC89A; при базе
// плана 0xC000, ширина 64 тайла → tile (col 13, row 17). Совпадает с дыркой idx==0 тайлкарты HUD
// (cols 13..22, rows 17..26). Итог: 10×10 тайлов = 80×80 px @ пиксель (104, 136).
static const int HUD_RX = 104, HUD_RY = 136, HUD_RW = 80, HUD_RH = 80;

inline const char* buildName(Build b) {
    switch (b) {
        case Build::ZT:        return "Zero Tolerance (релиз)";
        case Build::ZTU:       return "Zero Tolerance Underground";
        case Build::BZT_June:  return "Beyond ZT (прото 1995-06-23)";
        case Build::BZT_July:  return "Beyond ZT (прото 1995-07-14)";
        case Build::ZT_German: return "Zero Tolerance (нем.)";
        default:               return "неизвестный";
    }
}

// Константы/поведение движка по билду (НЕ адреса). Дом для проторазличий: число эпизодов,
// сжатие фонов, наличие PCM, позже — проекционные/скейлерные константы прото и т.п.
struct EngineProfile {
    int  episodes     = 3;       // число эпизодов (банков уровней)
    bool bgCompressed = false;   // фоны сжаты byte-pair (July-прото, распак. 0x98B98)
    bool hasPcm       = true;    // есть PCM-сэмплы (June-прото — нет)
};

// Банк тайлов стен (владеет байтами; декод 32×32 4bpp column-major, как gfx.hpp::decodeTile).
struct WallBank {
    std::vector<uint8_t> data;
    int count = 0;
    void decode(int tnum, uint8_t out[32 * 32]) const {
        size_t off = static_cast<size_t>(tnum) * 512;
        if (tnum < 0 || off + 512 > data.size()) { for (int i = 0; i < 1024; ++i) out[i] = 0; return; }
        const uint8_t* s = data.data() + off;
        for (int col = 0; col < 16; ++col) {
            int x = col * 2;
            for (int y = 0; y < 32; ++y) {
                uint8_t b = s[col * 32 + y];
                out[y * 32 + x]     = b >> 4;
                out[y * 32 + x + 1] = b & 0x0F;
            }
        }
    }
};

struct GameData {
    Build    build = Build::Unknown;
    EngineProfile profile;
    std::vector<Level> levels;          // эпизоды (число — по билду)
    Palette  wallPal;
    WallBank wall;
    WallBank obj;                       // банк графики объектов/декора (0x10E9BE, 66 тайлов 32×32 — тот же формат)
    Panorama bgCity, bgSpace;
    std::vector<uint8_t> shadeRamps;    // копия ROM-региона рамп (база 0x3392, 0x1000 б)
    std::vector<uint8_t> fcTemplates;   // шаблоны пол/потолок: 5 env × 0xA0 б (по env-индексу 0..4)
    std::vector<uint32_t> hud;          // HUD/кокпит 320×224 ARGB (референс-режим); пусто если нет
    std::vector<uint32_t> pauseBg;      // ФОН меню паузы 320×224 ARGB (руки держат карту-PDA, ZT @0x12DD06); пусто если нет
    std::vector<std::string> levelNames; // имена уровней (ZT @0x30b3, 16 симв./шт, 48 = 3 эп × 16 этажей)
    // ОРУЖИЕ В РУКАХ (held-графика FPS-вида): VDP-спрайты 8×8 column-major. Банк = N блоков
    // по 0x2A0=672 б = 21 тайл (тело 12 + кадр выстрела 9). Палитра — спрайт-линия эпизода (ZT 0x20D2).
    Palette  heldPal;                   // палитра оружия в руках (синие перчатки/серебро/вспышка)
    std::vector<uint8_t> heldGfx;       // банк held-графики: heldBlocks × 672 б (от 0x16C2B8 в ZT)
    int      heldBlocks = 0;            // число уникальных блоков графики (ZT 9: кулаки + 8 стволов)
    uint8_t  heldBlockForId[15] = {0};  // weapon-id (0..14) → индекс блока графики (таблица @0x11C98)
    // HUD-ИКОНКИ ИНВЕНТАРЯ: банк 0x15846E (14 иконок 0..13 = weapon-id 1..14), у каждой имя+картинка+
    // «0%» запечены в 32×32 (16 тайлов 8×8, 4×4 COLUMN-MAJOR), шаг 0x200. Палитра — та же heldPal (0x20D2).
    // Дисплей слотов FUN_0108d6 рисует тайл = id·0x10 из VRAM-иконок на 5 X-позициях верхних панелей.
    std::vector<uint8_t> hudIcons;      // банк иконок: hudIconCount × 0x200 б
    int      hudIconCount = 0;
    ZtFont   font;                      // Letters (A-Z 0-9 . - : ?) — нижний-левый HUD-инфо
    ZtFont   fontNum;                   // Numbers (0-9) — HP / счётчик врагов на HUD
    ZtFont   fontAlt;                   // Font2 (0-9 A-Z) — название уровня (пауза)
    ZtFontBig fontBig;                  // Font_grph 8×16 (цветной+палитра) — меню/настройки/сюжет/пауза
    // ШРИФТ ЦИФР БОЕЗАПАСА (резолвер FUN_1DF54, база 0x15A06E): 5 пар по 0x18 б, цифра 4px×6 (упакованы
    // по 2 на 8px-строку: чёт=левые 4px, нечёт=правые). Маленький угловой номер на иконке оружия.
    uint8_t  digitFont[0x78] = {0};
    bool valid = false;

    // пиксель (px 0..3, row 0..5) цифры d (0..9) → индекс палитры (heldPal)
    uint8_t digitPx(int d, int px, int row) const {
        if (d < 0 || d > 9 || px < 0 || px > 3 || row < 0 || row > 5) return 0;
        uint8_t b = digitFont[(d / 2) * 0x18 + row * 4 + (d & 1) * 2 + (px >> 1)];
        return (px & 1) ? (b & 0x0F) : (b >> 4);
    }

    // weapon-id (1..14) → индекс иконки в банке (id−1; id 0 «карта» иконки не имеет → 0).
    int iconForId(int id) const { return (id >= 1 && id <= hudIconCount) ? id - 1 : -1; }
    // Декод иконки n (32×32, 4×4 column-major) в out[1024] (индексы палитры 0..15).
    void decodeIcon(int n, uint8_t out[32 * 32]) const {
        size_t off = (size_t)n * 0x200;
        if (n < 0 || off + 0x200 > hudIcons.size()) { for (int i = 0; i < 1024; ++i) out[i] = 0; return; }
        const uint8_t* s = hudIcons.data() + off;
        for (int col = 0; col < 4; ++col)
            for (int row = 0; row < 4; ++row) {
                const uint8_t* t = s + (size_t)(col * 4 + row) * 32;
                for (int r = 0; r < 8; ++r)
                    for (int c = 0; c < 4; ++c) {
                        uint8_t b = t[r * 4 + c];
                        out[(row * 8 + r) * 32 + col * 8 + c * 2]     = b >> 4;
                        out[(row * 8 + r) * 32 + col * 8 + c * 2 + 1] = b & 0x0F;
                    }
            }
    }

    int episodes() const { return (int)levels.size(); }
    const Panorama* bgForEpisode(int ep) const { return (ep == 1) ? &bgCity : &bgSpace; }
};

// ── Per-build таблица ROM-адресов (ЕДИНСТВЕННОЕ место с адресами) ──
struct BuildAddrs {
    std::vector<size_t> sig;            // ZMAP-сигнатуры по эпизодам
    size_t wallBank = 0; int wallTiles = 0; size_t wallPal = 0, shadeRampBase = 0;
    size_t objBank = 0; int objTiles = 0;   // банк графики объектов/декора (billboard-тайлы 32×32)
    size_t fcTemplateBase = 0;          // база таблицы шаблонов пол/потолок (5×0xA0), -0x7152 в сеттере 0x1d66
    size_t hudGfx = 0, hudTm = 0, hudPal = 0; int hudLineAdd = 0;  // HUD/кокпит (экран 0x1F72 40×28)
    size_t pauseGfx = 0, pauseTm = 0, pausePal = 0; int pauseLineAdd = 0;  // фон меню паузы (экран 0x1F72)
    size_t levelNameTable = 0;          // таблица имён уровней (16 симв./шт)
    size_t heldBank = 0; int heldBlocks = 0;   // банк held-графики оружия (блоки по 672 б) + число блоков
    size_t heldPalAddr = 0, heldTable = 0;     // палитра оружия (0x20D2) + таблица id→графика (15 лонгов @0x11C98)
    size_t hudIconBank = 0; int hudIconCount = 0;  // банк HUD-иконок инвентаря (14 иконок 32×32, шаг 0x200)
    size_t hudDigitFont = 0;                       // шрифт цифр боезапаса (0x15A06E, 0x78 б; резолвер 0x1DF54)
    size_t fontLettersBank = 0; int fontLettersCount = 0;  // шрифт Letters (8×8 4bpp; ZT 0x16E758, 40 глифов)
    size_t fontNumBank = 0;     int fontNumCount = 0;      // шрифт Numbers (8×8; ZT 0x16E618, 10 цифр) — HP/враги
    size_t fontAltBank = 0;     int fontAltCount = 0;      // шрифт Font2 (8×8; ZT 0x12EAA6, 36: 0-9 A-Z) — назв.уровня
    size_t fontBigBank = 0, fontBigTable = 0, fontBigPal = 0; int fontBigCount = 0;  // Font_grph 8×16 (ZT 0x125116, табл 0x1DD3A, пал жёлтая 0xCD47A)
    size_t animTableOff = 0x5914;       // смещение таблицы анимаций стен от ZMAP (ZT 0x5914, BZT 0x1914)
    bool wired = false;                 // адреса этого билда заведены
};
inline BuildAddrs addrsForBuild(Build b) {
    BuildAddrs a;
    switch (b) {
        case Build::ZT:
        case Build::ZT_German:          // нем. = пересборка US: базовые адреса те же (objdef отличается — позже)
            a.sig = {0x15A106, 0x160420, 0x166028};
            a.wallBank = 0x12EF26; a.wallTiles = 255; a.wallPal = 0x20F2; a.shadeRampBase = 0x3392;
            a.objBank = 0x10E9BE; a.objTiles = 66;   // банк объектов/декора (палитра — линия 0 = wallPal)
            a.fcTemplateBase = 0x14ED26;   // env1 Dim; env-таблица не по порядку (см. ниже)
            a.hudGfx = 0x156CAE; a.hudTm = 0x1563EE; a.hudPal = 0x2072; a.hudLineAdd = 3;  // HUD/кокпит
            a.pauseGfx = 0x12DD06; a.pauseTm = 0x12D446; a.pausePal = 0x2072; a.pauseLineAdd = 3;  // ФОН меню паузы (руки+карта)
            a.levelNameTable = 0x30B3;   // имена уровней: 48 × 16 симв (DOCKING BAY 1 / BRIDGE 1 / ...)
            a.heldBank = 0x16C2B8; a.heldBlocks = 9;   // 9 блоков held-графики (кулаки + 8 стволов)
            a.heldPalAddr = 0x20D2; a.heldTable = 0x11C98;  // палитра оружия + таблица id→графика
            a.hudIconBank = 0x15846E; a.hudIconCount = 14;  // 14 HUD-иконок инвентаря (палитра = held 0x20D2)
            a.hudDigitFont = 0x15A06E;                       // шрифт цифр боезапаса (сразу после 14 иконок)
            a.fontLettersBank = 0x16E758; a.fontLettersCount = 40;  // Letters: A-Z(0-25) 0-9(26-35) .-:?(36-39)
            a.fontNumBank = 0x16E618; a.fontNumCount = 10;          // Numbers: 0-9
            a.fontAltBank = 0x12EAA6; a.fontAltCount = 36;          // Font2: 0-9(0-9) A-Z(10-35)
            a.fontBigBank = 0x125116; a.fontBigTable = 0x1DD3A; a.fontBigCount = 150; a.fontBigPal = 0xCD47A;  // Font_grph 8×16 (char→верх|низ; пал жёлтая)
            a.animTableOff = 0x5914;       // таблица анимаций стен сразу после env (sig+0x5904+16)
            a.wired = true; break;
        case Build::ZTU:                // TODO: банки/сигнатуры ZTU переразмечены
        case Build::BZT_June:           // TODO: прото-адреса + форматы
        case Build::BZT_July:
        default: break;                 // не заведено → fallback на ZT-раскладку (warn)
    }
    return a;
}

inline EngineProfile profileForBuild(Build b) {
    EngineProfile p;
    switch (b) {
        case Build::BZT_June: p.hasPcm = false; break;         // June: нет PCM-сэмплов
        case Build::BZT_July: p.bgCompressed = true; break;    // July: фоны byte-pair
        default: break;
    }
    return p;
}

// Детект билда (эвристика; уточняется при подключении конкретного билда). Для текущего ROM (ZT, 2МБ) → ZT.
inline Build detectBuild(const Rom& rom) {
    size_t n = rom.size();
    if (n >= 0x2C0000) return Build::BZT_July;   // ~3МБ → прото (July; June уточнить по дате)
    return Build::ZT;                            // 2МБ: ZT/ZTU/нем. — различение позже (заголовок/маркеры)
}

// Декод HUD/кокпита (экран формата 0x1F72): 40×28 стандартных MD-тайлов 8×8 4bpp + тайлкарта
// (MD nametable: idx&0x7FF, hflip b11, vflip b12, линия палитры b13-14) + палитра 64 цв (4 линии).
// line = ((word>>13)+lineAdd)&3. Воспроизводит mdgfx/gui.py OtherScreenViewer._compose. → gd.hud 320×224.
// Общий декод экрана 0x1F72 (40×28 MD-тайлов 8×8 + nametable + 64-цв палитра) → 320×224 ARGB.
inline std::vector<uint32_t> decodeScreen320(const Rom& rom, size_t gfx, size_t tm, size_t palAddr, int lineAdd) {
    std::vector<uint32_t> out;
    if (!gfx || !tm || !palAddr) return out;
    uint32_t pal[64];
    for (int i = 0; i < 64; ++i) pal[i] = cramToArgb(rom.u16(palAddr + i * 2));
    out.assign((size_t)HUD_W * HUD_H, 0xFF000000u);
    const int TW = 40, TH = 28;
    for (int ty = 0; ty < TH; ++ty)
        for (int tx = 0; tx < TW; ++tx) {
            uint16_t e = rom.u16(tm + (size_t)(ty * TW + tx) * 2);
            int idx  = e & 0x7FF;
            int line = ((e >> 13) + lineAdd) & 3;
            bool hflip = (e & 0x800) != 0, vflip = (e & 0x1000) != 0;
            size_t toff = gfx + (size_t)idx * 32;            // MD-тайл 8×8 = 32 байта (4 байта/строка)
            int pbase = line * 16;
            for (int r = 0; r < 8; ++r) {
                int sr = vflip ? (7 - r) : r;
                uint8_t row8[8];
                for (int c = 0; c < 4; ++c) {
                    uint8_t b = rom.u8(toff + (size_t)sr * 4 + c);
                    row8[c * 2] = b >> 4; row8[c * 2 + 1] = b & 0x0F;   // hi=левый px
                }
                if (hflip) for (int k = 0; k < 4; ++k) { uint8_t t = row8[k]; row8[k] = row8[7 - k]; row8[7 - k] = t; }
                uint32_t* dst = &out[(size_t)(ty * 8 + r) * HUD_W + tx * 8];
                for (int c = 0; c < 8; ++c) dst[c] = pal[pbase + row8[c]];
            }
        }
    return out;
}
inline void decodeHud(GameData& gd, const Rom& rom, const BuildAddrs& a) {
    gd.hud = decodeScreen320(rom, a.hudGfx, a.hudTm, a.hudPal, a.hudLineAdd);
    gd.pauseBg = decodeScreen320(rom, a.pauseGfx, a.pauseTm, a.pausePal, a.pauseLineAdd);  // фон меню паузы (руки+карта-PDA)
}

// ── Декод шрифтов ZT (1-бит маска по «не-фон»; фон = самый частый ниббл глифа) ──
// 8×8: тайл 32 б, row-major, ниббл=пиксель. order[i] = ASCII символ для глифа i (по порядку в банке).
inline void ztFontClear(ZtFont& f, int h) {
    f.have = false; f.h = h;
    for (int i = 0; i < 128; ++i) { f.supported[i] = false; for (int r = 0; r < 16; ++r) f.glyph[i][r] = 0; }
}
inline uint8_t ztTileNib(const Rom& rom, size_t base, int r, int c) {
    uint8_t b = rom.u8(base + (size_t)r * 4 + c / 2);
    return (c & 1) ? (b & 0x0F) : (uint8_t)(b >> 4);
}
inline int ztTileBg(const Rom& rom, size_t base) {            // самый частый ниббл = фон
    int cnt[16] = {0};
    for (int r = 0; r < 8; ++r) for (int c = 0; c < 8; ++c) cnt[ztTileNib(rom, base, r, c)]++;
    int bg = 0; for (int n = 1; n < 16; ++n) if (cnt[n] > cnt[bg]) bg = n;
    return bg;
}
inline ZtFont decodeZtFont8(const Rom& rom, size_t bank, const char* order) {
    ZtFont f; ztFontClear(f, 8);
    for (int g = 0; order[g]; ++g) {
        size_t base = bank + (size_t)g * 32;
        int bg = ztTileBg(rom, base);
        int ch = (unsigned char)order[g];
        for (int r = 0; r < 8; ++r) {
            uint8_t bits = 0;
            for (int c = 0; c < 8; ++c) if (ztTileNib(rom, base, r, c) != bg) bits |= (uint8_t)(1 << c);
            f.glyph[ch][r] = bits;
        }
        f.supported[ch] = true;
    }
    f.have = true; return f;
}
// Font_grph 8×16 (ЦВЕТНОЙ): таблица char(0x20+)→long (верх<<16|низ), тайлы VRAM-абсолютные, gfx = tile−1
// (tile0=пусто). Храним 4bpp-индексы пикселей (idx 0 = прозрачный) + палитру шрифта (жёлтая/белая).
inline ZtFontBig decodeZtFontBig(const Rom& rom, size_t bank, size_t table, int count, size_t palAddr) {
    ZtFontBig f;
    f.have = false;
    for (int i = 0; i < 128; ++i) {
        f.supported[i] = false;
        for (int r = 0; r < 16; ++r) for (int c = 0; c < 8; ++c) f.pix[i][r][c] = 0;
    }
    Palette pal = readPalette(rom, palAddr);
    for (int i = 0; i < 16; ++i) f.pal[i] = pal.c[i];
    for (int ch = 0x20; ch <= 0x7E; ++ch) {
        uint32_t e = rom.u32(table + (size_t)(ch - 0x20) * 4);
        int tile[2] = { (int)((e >> 16) & 0x7FF) - 1, (int)(e & 0x7FF) - 1 };  // [верх, низ] gfx-индексы
        bool any = false;
        for (int half = 0; half < 2; ++half) {
            int gi = tile[half];
            if (gi < 0 || gi >= count) continue;
            size_t base = bank + (size_t)gi * 32;
            for (int r = 0; r < 8; ++r)
                for (int c = 0; c < 8; ++c)
                    f.pix[ch][half * 8 + r][c] = ztTileNib(rom, base, r, c);
            any = true;
        }
        if (any) f.supported[ch] = true;
    }
    f.have = true; return f;
}

// ── СПРАЙТЫ ВРАГОВ (дерево state→dir→frame, по отчёту дизасма; кадр 0x26 б, тайл 32×32 col-major) ──
// Декод: tileBase=(a1+2)+u16(a1+2); animCount=u16(a1); animBlock=(a1+2)+u16(a1+4+anim*2);
// dirCount=u16(animBlock); dirBlock=animBlock+u16(animBlock+2+dir*2); frameCount=u16(dirBlock)(0→1);
// frame=dirBlock+2+f*0x26: +4 W(тайлов) +5 H, +0x06 attr[W*H stride4 bit0=hflip], +0x16 tileIdx[stride4].
// Палитра — спрайт/стен 0x20F2 (=wallPal). idx0 прозрачный. Враги лицом к игроку → достаточно dir0 frame0.
struct EnemySprite { int w = 0, h = 0, drawW = 0, drawH = 0; std::vector<uint32_t> argb; bool ok = false; };
// КАДРЫ ХОДЬБЫ (front, dir0) по слоту enemyGfxSlot (0..9) — совместимость; = g_enemyAnim[slot].walk[0].
inline std::vector<EnemySprite> g_enemyWalk[16];
// ── НАБОР АНИМАЦИЙ ВРАГА (по разбору draw-селекторов objdef+0x1c → таблица 0x1690E): ходьба по НАПРАВЛЕНИЯМ +
//    стрельба/удар + смерть. Направление по углу facing(скорость)↔игрок (ZT 0x1BA08). hflip запечён в тайлах. ──
struct EnemyAnimSet {
    std::vector<EnemySprite> walk[6];            // anim ходьбы: до 6 направлений
    int walkDirs = 1;
    std::vector<EnemySprite> walkB[6];           // ВТОРАЯ вариация ходьбы (FH: anim9 — 2-й полный набор спрайтов в дереве)
    int walkBDirs = 0;
    bool hasVariant = false;                      // есть ли 2-я вариация (per-actor выбор по Actor::variant)
    std::vector<EnemySprite> fire, hit, death;   // стрельба/удар, стаггер, смерть (dir0)
    std::vector<EnemySprite> climb;               // Hydaca: ВЕРТИКАЛЬНЫЙ спрайт лазанья по стене (a1, 32×64); пусто у прочих
    bool ok = false;
};
inline EnemyAnimSet g_enemyAnim[16];
// {walk, fire, hit, death} anim-индексы по слоту (−1 = нет → fallback ходьба). slot: 0Sgt 1FH 2Imp 3Hydaca
// 4Reven 5Boss1 6Dog 7FH-SF 8Boss3 9Boss2 (из per-enemy draw-fn: FH walk0/fire7/stagger1/death4 и т.д.).
// ⚠ ВСЕ выверено: draw-fn каждого врага (state→класс→anim) + РЕНДЕР кадров (декодер как ztextractor: nfr==0→1 кадр,
// поэтому «пустых» анимов НЕТ — это были 1-кадровые позы hit/death). hit=stagger(HP≥0), death=stagger(HP<0).
static const int ENEMY_ANIM_IDX[10][4] = {   // {walk, fire, hit, death}
    {0, 3,  7, 11}, // Sgt:   fire=a3(ВСПЫШКА, draw st1; a4 был НЕВЕРНО — это state5) hit=a7 death=a11(комок)
    {0, 7,  1,  4}, // FH:    fire=a7(музл st1) hit=a1(draw 0x32) death=a4(5к распад)
    {0, 2,  3,  6}, // Imp:   strike=a2 hit=a3(draw 0x7b) death=a6(5к)
    {0,-1, -1,  8}, // Hydaca:пол=a0/потолок=a5(walkB)/climb=a1; death=a8(целый лёжа; a9=распад,a10 были неверны); hit нет
    {0, 1,  3,  7}, // Reven: fire=a1 hit=a3(draw 0x62) death=a7(комок)
    {0, 1,  3,  4}, // Boss1: fire=a1 hit=a3(draw 0x75) death=a4 (анимы ЕСТЬ — были ошибочно «пусты»)
    {0, 1,  3,  6}, // Dog:   lunge=a1 hit=a3(draw 0x70) death=a6(ЛЁЖА 64×32; a2 был НЕВЕРНО — это поза прыжка)
    {0, 5,  1,  4}, // FH-SF: fire=a5 hit=a1(draw 0x53) death=a4(5к)
    {0, 1,  5,  6}, // Boss3: fire=a1 hit=a5(draw 0x84) death=a6
    {0, 5,  1,  2}  // Boss2: fire=a5 hit=a1(draw 0x4d) death=a2 (a6 был НЕВЕРНО)
};
// 2-я ВАРИАЦИЯ ходьбы по слоту (−1 = нет). FH (slot1) имеет в gfx-дереве ВТОРОЙ полный 6-напр walk-набор (anim9):
// «у former human две вариации при идентичном cell id» — per-actor выбор по Actor::variant (RNG при спавне).
// walkB = АЛЬТ-АНИМ В ТОМ ЖЕ банке. Hydaca(3): anim5 = ПОТОЛОК-краул (variant драйвится z: на потолке→1; пол=anim0).
// FH(1) вариацию НЕ через walkB (это отдельный БАНК — см. ENEMY_ALT_GFX ниже), потому здесь −1.
static const int ENEMY_VARIANT_WALK[10] = { -1, -1, -1, 5, -1, -1, -1, -1, -1, -1 };
// Спрайт лазанья по СТЕНЕ (вертикальный) по слоту: только Hydaca(3) = anim1 (32×64).
static const int ENEMY_CLIMB_ANIM[10] = { -1, -1, -1, 1, -1, -1, -1, -1, -1, -1 };
// АЛЬТ-БАНК ГРАФИКИ (ПОЛНОСТЬЮ другой набор спрайтов для вариации). FH(1): 0x1C258A — мускулистый коммандо (выверено
// ztextractor ZT_CELLTYPE_ENEMY[0x2A]=(...,0x16EC58,0x1C258A)). «Две вариации Former Human при одном cell id» = разные банки.
static const unsigned ENEMY_ALT_GFX[10] = { 0, 0x1C258A, 0, 0, 0, 0, 0, 0, 0, 0 };
inline EnemyAnimSet g_enemyAnimVar2[16];          // полный альт-набор по слоту (FH вар2); .ok=false если нет
// ВЫБОР НАПРАВЛЕНИЯ — ТОЧНО ПО ZT 0x1ba2a: U=(self−player), V=скорость(facing); d3=256·cos∠(U,V); cross=U×V.
//   ПРИБЛИЖЕНИЕ (V≈к игроку = −U) → d3≈−256 → **dir0 = ФРОНТ** (враг смотрит на тебя). Бегство → d3≈+256 → dir1=спина.
//   rx/ry = (player−self). U=−R. (Прежний код брал dir1 для приближения → «все спиной» — исправлено.)
inline int enemyDirIndex(double vx, double vy, double rx, double ry, int dirCount) {
    if (dirCount <= 1) return 0;
    double vl = std::hypot(vx, vy);
    if (vl < 1e-4) return 0;                              // стоит → dir0 (фронт)
    double ux = -rx, uy = -ry, ul = std::hypot(ux, uy);  // U = self−player
    double dot = ux * vx + uy * vy;                       // (self−player)·V
    if (dirCount == 2) return (dot >= 0) ? 1 : 0;         // ZT d7==2: dot≥0 (бегство)→dir1, иначе dir0(фронт)
    double d3 = 256.0 * dot / (ul * vl + 1e-6);           // 256·cos∠(U,V)
    double cross = ux * vy - uy * vx;
    if (dirCount == 4) {                                  // ZT d7==4
        if (d3 >= 0x9b) return 1;                         // бегство → спина
        if (d3 <= -0x9b) return 0;                        // приближение → ФРОНТ
        return (cross < 0) ? 2 : 3;                       // бок
    }
    if (d3 >= 0x9b) return 1;                             // ZT d7==6: спина
    if (d3 <= -0xC8) return 0;                            // ФРОНТ (приближение, d3≤−200)
    if (d3 >= -0x5A) return (cross < 0) ? 2 : 3;          // бок (−90..155)
    return (cross < 0) ? 4 : 5;                           // боко-фронт (−200..−90)
}
// celltype → слот (порядок: Sgt/FH/Imp/Hydaca/Revenant/Boss1/dog/FH-SF/Boss3/Boss2).
inline int enemyGfxSlot(uint8_t ct) {
    switch (ct) { case 0x29: return 0; case 0x2A: return 1; case 0x2B: return 2; case 0x65: return 3;
        case 0x66: return 4; case 0x67: return 5; case 0x68: return 6; case 0x69: return 7;
        case 0x6A: return 8; case 0x6B: return 9; default: return -1; }
}
inline EnemySprite decodeEnemySprite(const Rom& rom, size_t a1, const Palette& pal, int anim, int dir, int frame) {
    EnemySprite s;
    if (!a1 || a1 + 6 >= rom.size()) return s;
    size_t base = a1 + 2;
    size_t tileBase = base + rom.u16(a1 + 2);
    int animCount = rom.u16(a1); if (animCount < 1 || animCount > 32) return s;
    if (anim >= animCount) anim = 0;
    size_t animBlock = base + rom.u16(a1 + 4 + (size_t)anim * 2);
    int dirCount = rom.u16(animBlock); if (dirCount < 1 || dirCount > 32) return s;
    if (dir >= dirCount) dir = 0;
    size_t dirBlock = animBlock + rom.u16(animBlock + 2 + (size_t)dir * 2);
    int frameCount = rom.u16(dirBlock); if (frameCount == 0) frameCount = 1; if (frameCount > 64) return s;
    if (frame >= frameCount) frame = 0;
    size_t fr = dirBlock + 2 + (size_t)frame * 0x26;
    int W = rom.u8(fr + 4), H = rom.u8(fr + 5);
    if (W < 1 || W > 8 || H < 1 || H > 8) return s;
    s.drawW = rom.u8(fr + 2); s.drawH = rom.u8(fr + 3);    // on-screen scale: W=drawW·scale>>5, H=drawH·scale>>4
    if (s.drawW < 1) s.drawW = W * 6; if (s.drawH < 1) s.drawH = H * 6;
    s.w = W * 32; s.h = H * 32; s.argb.assign((size_t)s.w * s.h, 0u);
    for (int r = 0; r < H; ++r)
        for (int c = 0; c < W; ++c) {
            uint8_t idx  = rom.u8(fr + 0x16 + (size_t)r * 4 + c);
            bool   flip  = rom.u8(fr + 0x06 + (size_t)r * 4 + c) & 1;
            size_t ta = tileBase + (size_t)idx * 512;
            if (ta + 512 > rom.size()) continue;
            for (int col = 0; col < 16; ++col)
                for (int row = 0; row < 32; ++row) {
                    uint8_t b = rom.u8(ta + (size_t)col * 32 + row);
                    int x0 = col * 2, x1 = col * 2 + 1; uint8_t p0 = b >> 4, p1 = b & 0x0F;
                    if (flip) { x0 = 31 - x0; x1 = 31 - x1; }
                    int py = r * 32 + row;
                    if (p0) s.argb[(size_t)py * s.w + c * 32 + x0] = pal.c[p0];
                    if (p1) s.argb[(size_t)py * s.w + c * 32 + x1] = pal.c[p1];
                }
        }
    s.ok = true; return s;
}
inline void decodeEnemySprites(const Rom& rom, const Palette& pal) {
    static const size_t GFX[10] = { 0x1B7B38, 0x16EC58, 0x1A7828, 0x1ACC72, 0x18D078,
                                    0x183B94, 0x1784F2, 0x17BA80, 0x193F00, 0x19D50C };
    auto decAnim = [&](size_t a1, int anim, int dir, std::vector<EnemySprite>& out) {
        if (anim < 0) return;
        int animCount = rom.u16(a1); if (anim >= animCount) return;
        size_t animBlock = (a1 + 2) + rom.u16(a1 + 4 + (size_t)anim * 2);
        int dirc = rom.u16(animBlock); if (dirc < 1) dirc = 1;
        if (dir >= dirc) return;
        size_t dirBlock = animBlock + rom.u16(animBlock + 2 + (size_t)dir * 2);
        int fc = rom.u16(dirBlock); if (fc == 0) fc = 1; if (fc > 12) fc = 12;
        for (int f = 0; f < fc; ++f) { EnemySprite s = decodeEnemySprite(rom, a1, pal, anim, dir, f);
            if (s.ok) out.push_back(std::move(s)); }
    };
    for (int i = 0; i < 10; ++i) {
        EnemyAnimSet& A = g_enemyAnim[i]; A = EnemyAnimSet{};
        size_t a1 = GFX[i];
        int wA = ENEMY_ANIM_IDX[i][0], fA = ENEMY_ANIM_IDX[i][1], hA = ENEMY_ANIM_IDX[i][2], dA = ENEMY_ANIM_IDX[i][3];
        // ХОДЬБА = anim wA по ВСЕМ направлениям (повороты). dir0=фронт (как было).
        size_t animBlock = (a1 + 2) + rom.u16(a1 + 4 + (size_t)wA * 2);
        int dirs = rom.u16(animBlock); if (dirs < 1) dirs = 1; if (dirs > 6) dirs = 6;
        A.walkDirs = dirs;
        for (int d = 0; d < dirs; ++d) decAnim(a1, wA, d, A.walk[d]);
        if (A.walk[0].empty()) decAnim(a1, 0, 0, A.walk[0]);           // фолбэк
        decAnim(a1, fA, 0, A.fire);                                    // стрельба/удар
        decAnim(a1, hA, 0, A.hit);                                     // стаггер
        decAnim(a1, dA, 0, A.death);                                   // смерть
        // 2-я вариация ходьбы (FH anim9 — второй полный набор спрайтов в дереве): per-actor выбор по variant.
        int vA = ENEMY_VARIANT_WALK[i];
        if (vA >= 0 && vA < rom.u16(a1)) {
            size_t vBlock = (a1 + 2) + rom.u16(a1 + 4 + (size_t)vA * 2);
            int vdirs = rom.u16(vBlock); if (vdirs < 1) vdirs = 1; if (vdirs > 6) vdirs = 6;
            for (int d = 0; d < vdirs; ++d) decAnim(a1, vA, d, A.walkB[d]);
            if (!A.walkB[0].empty()) { A.walkBDirs = vdirs; A.hasVariant = true; }
        }
        decAnim(a1, ENEMY_CLIMB_ANIM[i], 0, A.climb);                  // Hydaca: вертикальный спрайт лазанья по стене
        A.ok = !A.walk[0].empty();
        g_enemyWalk[i] = A.walk[0];                                    // совместимость
    }
    // АЛЬТ-БАНКИ: полный второй набор спрайтов (другая модель) для вариации врага. Сейчас FH(1)=0x1C258A (коммандо).
    for (int i = 0; i < 10; ++i) {
        EnemyAnimSet& A = g_enemyAnimVar2[i]; A = EnemyAnimSet{};
        size_t a1 = ENEMY_ALT_GFX[i]; if (!a1) continue;
        int wA = ENEMY_ANIM_IDX[i][0], fA = ENEMY_ANIM_IDX[i][1], hA = ENEMY_ANIM_IDX[i][2], dA = ENEMY_ANIM_IDX[i][3];
        size_t animBlock = (a1 + 2) + rom.u16(a1 + 4 + (size_t)wA * 2);
        int dirs = rom.u16(animBlock); if (dirs < 1) dirs = 1; if (dirs > 6) dirs = 6;
        A.walkDirs = dirs;
        for (int d = 0; d < dirs; ++d) decAnim(a1, wA, d, A.walk[d]);
        if (A.walk[0].empty()) decAnim(a1, 0, 0, A.walk[0]);
        decAnim(a1, fA, 0, A.fire); decAnim(a1, hA, 0, A.hit); decAnim(a1, dA, 0, A.death);
        A.ok = !A.walk[0].empty();
    }
}

// ── Наполнение из ROM ──
inline bool loadGameDataFromRom(GameData& gd, const Rom& rom) {
    gd.build   = detectBuild(rom);
    gd.profile = profileForBuild(gd.build);
    BuildAddrs a = addrsForBuild(gd.build);
    if (!a.wired) {                               // билд ещё не заведён → ZT-раскладка как fallback
        std::fprintf(stderr, "ВНИМАНИЕ: билд «%s» ещё не подключён — пробую ZT-раскладку\n", buildName(gd.build));
        a = addrsForBuild(Build::ZT);
    }

    gd.levels.clear(); gd.levels.resize(a.sig.size());
    for (size_t i = 0; i < a.sig.size(); ++i) loadLevelFromRom(gd.levels[i], rom, a.sig[i], a.animTableOff);
    gd.profile.episodes = (int)gd.levels.size();

    gd.wallPal = readPalette(rom, a.wallPal);
    gd.wall.count = a.wallTiles;
    gd.wall.data.assign(static_cast<size_t>(a.wallTiles) * 512, 0);
    for (size_t i = 0; i < gd.wall.data.size(); ++i) gd.wall.data[i] = rom.u8(a.wallBank + i);

    // Банк объектов/декора (billboard-тайлы 32×32 column-major, тот же формат что стены).
    gd.obj.count = a.objTiles;
    gd.obj.data.assign(static_cast<size_t>(a.objTiles) * 512, 0);
    for (size_t i = 0; i < gd.obj.data.size(); ++i) gd.obj.data[i] = rom.u8(a.objBank + i);

    gd.bgCity  = loadZtBackground(rom, 0);
    gd.bgSpace = loadZtBackground(rom, 1);

    decodeHud(gd, rom, a);              // HUD/кокпит (320×224) + фон меню паузы
    gd.levelNames.clear();             // имена уровней (для меню паузы): 48 × 16 симв, обрезаем пробелы
    if (a.levelNameTable) for (int i = 0; i < 48; ++i) {
        std::string s; for (int c = 0; c < 16; ++c) { char ch = (char)rom.u8(a.levelNameTable + (size_t)i * 16 + c); if (ch >= 32 && ch < 127) s += ch; }
        size_t b = s.find_first_not_of(' '), e = s.find_last_not_of(' ');
        gd.levelNames.push_back(b == std::string::npos ? "" : s.substr(b, e - b + 1));
    }

    // ОРУЖИЕ В РУКАХ: банк held-графики (heldBlocks × 672 б) + палитра (0x20D2) + таблица id→блок.
    // Таблица @0x11C98 (15 лонгов, weapon-id→указатель графики) → индекс блока = (ptr−heldBank)/672.
    if (a.heldBank && a.heldBlocks > 0) {
        const size_t HELD_BLOCK = 672;             // 0x2A0 = 21 тайл × 32 б
        gd.heldBlocks = a.heldBlocks;
        gd.heldGfx.assign((size_t)a.heldBlocks * HELD_BLOCK, 0);
        for (size_t i = 0; i < gd.heldGfx.size(); ++i) gd.heldGfx[i] = rom.u8(a.heldBank + i);
        gd.heldPal = readPalette(rom, a.heldPalAddr ? a.heldPalAddr : a.wallPal);
        for (int id = 0; id < 15; ++id) {
            size_t ptr = a.heldTable ? rom.u32(a.heldTable + (size_t)id * 4) : a.heldBank;
            long blk = a.heldBank ? (long)((ptr - a.heldBank) / HELD_BLOCK) : 0;
            gd.heldBlockForId[id] = (blk >= 0 && blk < a.heldBlocks) ? (uint8_t)blk : 0;
        }
    }

    // HUD-иконки инвентаря (банк 0x15846E, 14 × 0x200). Палитра — та же held (0x20D2).
    if (a.hudIconBank && a.hudIconCount > 0) {
        gd.hudIconCount = a.hudIconCount;
        gd.hudIcons.assign((size_t)a.hudIconCount * 0x200, 0);
        for (size_t i = 0; i < gd.hudIcons.size(); ++i) gd.hudIcons[i] = rom.u8(a.hudIconBank + i);
    }
    if (a.hudDigitFont) for (int i = 0; i < 0x78; ++i) gd.digitFont[i] = rom.u8(a.hudDigitFont + i);  // шрифт цифр боезапаса

    // ШРИФТЫ ZT (1-бит маски в формате FONT8X8 → drawChar/drawCharBig единообразны).
    if (a.fontLettersBank) gd.font    = decodeZtFont8(rom, a.fontLettersBank, "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789.-:?");
    if (a.fontNumBank)     gd.fontNum = decodeZtFont8(rom, a.fontNumBank,     "0123456789");
    if (a.fontAltBank)     gd.fontAlt = decodeZtFont8(rom, a.fontAltBank,     "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ");
    if (a.fontBigBank && a.fontBigTable)
        gd.fontBig = decodeZtFontBig(rom, a.fontBigBank, a.fontBigTable, a.fontBigCount, a.fontBigPal);

    gd.shadeRamps.assign(0x1000, 0);
    for (size_t i = 0; i < 0x1000; ++i) gd.shadeRamps[i] = rom.u8(a.shadeRampBase + i);

    // Шаблоны пол/потолок (ОТДЕЛЬНЫЙ СЛОЙ, не скейлер). Сеттер 0x1d66 ставит указатель -0x7152 по env:
    //   env0 Bright=base+0x140, env1 Dim=base+0x00, env2 Haze=base+0xA0, env3 NoCeil=base+0x1E0,
    //   env4 Black=base+0x280 (в ROM лежат не по порядку env). Каждый = 0xA0 б = 2 верт. паттерна по
    //   80 строк (нечёт/чёт колонка = гориз. дизер); байт = 2px 4bpp (hi=левый). Раскладываем по env 0..4.
    gd.fcTemplates.assign(5 * 0xA0, 0);
    if (a.fcTemplateBase) {
        static const size_t envOff[5] = { 0x140, 0x000, 0x0A0, 0x1E0, 0x280 };  // env0..4 → смещение в ROM
        for (int e = 0; e < 5; ++e)
            for (size_t i = 0; i < 0xA0; ++i)
                gd.fcTemplates[(size_t)e * 0xA0 + i] = rom.u8(a.fcTemplateBase + envOff[e] + i);
    }

    setActiveCellTable(cellTableForBuild((int)gd.build));   // классификация клеток по билду (сейчас ZT)
    if (gd.build == Build::ZT || gd.build == Build::ZT_German)
        decodeEnemySprites(rom, gd.wallPal);               // реальные спрайты врагов (дерево 0x1B7B38… пал 0x20F2)

    gd.valid = !gd.levels.empty() && gd.levels[0].valid();
    return gd.valid;
}
