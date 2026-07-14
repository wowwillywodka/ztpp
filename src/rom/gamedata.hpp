// ztpp — ЭТАП 0: модель игровых данных (мод-готовность + мульти-билд).
//
// GameData — единый контейнер ассетов, которым ВЛАДЕЕТ порт: уровни, банк текстур стен, палитра,
// фоны-панорамы, рампы. Рендеры и логика работают ТОЛЬКО с GameData (через Level/WallBank/Palette/
// Panorama), не зная об ИСТОЧНИКЕ и БИЛДЕ. Источник наполняет GameData:
//   loadGameDataFromRom(gd, rom)   — детект билда → per-build адреса/форматы.  ← адреса ROM ТОЛЬКО в gamedata.cpp.
//   (будущее) loadGameDataFromOztd(gd, path) — моды.
//
// МОДУЛЬ src/rom: заголовок = контейнер + ГОРЯЧИЕ inline-методы (WallBank::decode/decodeIcon — на тайл/иконку
// рендера). Загрузчики/декодеры (addrsForBuild, decodeScreen320, шрифты, loadGameDataFromRom) — в gamedata.cpp.
#pragma once
#include "rom.hpp"
#include "gfx.hpp"
#include "level.hpp"
#include "cells.hpp"
#include "background.hpp"     // Panorama + decode (декодер-слой)
#include "font8x8.hpp"        // ZtFont/ZtFontBig
#include "enemy_sprites.hpp"  // спрайты врагов (вынесены из gamedata)
#include <vector>
#include <string>
#include <cstdint>

enum class Build { ZT, ZTU, BZT_June, BZT_July, ZT_German, Unknown };

const char* buildName(Build b);   // gamedata.cpp

// HUD/кокпит (референс-режим): экран 320×224, вьюпорт 3D-вида = 256×80 @ (32,40) — нативный рендер
// 128×80 показан ×2 по горизонтали. Раскладка вскрыта по дырке (idx==0) в тайлкарте HUD (ztextractor).
inline constexpr int HUD_W = 320, HUD_H = 224;
inline constexpr int HUD_VX = 32, HUD_VY = 40, HUD_VW = 256, HUD_VH = 80;
// Радар/миникарта на HUD-кокпите (центр-низ). Из дизасма построителя миникарты FUN_0000e816:
// 10×10 тайлов = 80×80 px @ пиксель (104, 136).
inline constexpr int HUD_RX = 104, HUD_RY = 136, HUD_RW = 80, HUD_RH = 80;

// Константы/поведение движка по билду (НЕ адреса). Дом для проторазличий: число эпизодов,
// сжатие фонов, наличие PCM, позже — проекционные/скейлерные константы прото и т.п.
struct EngineProfile {
    int  episodes     = 3;       // число эпизодов (банков уровней)
    bool bgCompressed = false;   // фоны сжаты byte-pair (July-прото, распак. 0x98B98)
    bool hasPcm       = true;    // есть PCM-сэмплы (June-прото — нет)
};

// Банк тайлов стен (владеет байтами; декод 32×32 4bpp column-major, как gfx.hpp::decodeTile). decode — горячий.
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
    std::vector<uint8_t> fistAction;    // блок ДЕЙСТВИЯ кулаков (замах/удар/кик, @0x16c038=heldBank−0x280); idle=0x16c2b8
    uint8_t  heldBlockForId[15] = {0};  // weapon-id (0..14) → индекс блока графики (таблица @0x11C98)
    // HUD-ИКОНКИ ИНВЕНТАРЯ: банк 0x15846E (14 иконок 0..13 = weapon-id 1..14), картинка+«0%» запечены
    // в 32×32 (16 тайлов 8×8, 4×4 COLUMN-MAJOR), шаг 0x200. Палитра — та же heldPal (0x20D2).
    std::vector<uint8_t> hudIcons;      // банк иконок: hudIconCount × 0x200 б
    int      hudIconCount = 0;
    ZtFont   font;                      // Letters (A-Z 0-9 . - : ?) — нижний-левый HUD-инфо
    ZtFont   fontNum;                   // Numbers (0-9) — HP / счётчик врагов на HUD
    ZtFont   fontAlt;                   // Font2 (0-9 A-Z) — название уровня (пауза)
    ZtFontBig fontBig;                  // Font_grph 8×16 (цветной+палитра) — меню/настройки/сюжет/пауза
    // ШРИФТ ЦИФР БОЕЗАПАСА (резолвер FUN_1DF54, база 0x15A06E): 5 пар по 0x18 б, цифра 4px×6.
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
    // Декод иконки n (32×32, 4×4 column-major) в out[1024] (индексы палитры 0..15). Горячий (рендер HUD).
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

// ── Наполнение из ROM (детект билда → per-build адреса/форматы; адреса ТОЛЬКО в gamedata.cpp) ──
bool loadGameDataFromRom(GameData& gd, const Rom& rom);
