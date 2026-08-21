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
#include <array>
#include <vector>
#include <string>
#include <cstdint>

enum class Build { ZT, ZTU, BZT_June, BZT_July, ZT_German, Unknown };

const char* buildName(Build b);   // gamedata.cpp
// Ключ билда для контура данных (профиль настроек/сейвов): "zt"/"ztu"/"zt_german"/"bzt_june"/"bzt_july"/"unknown".
const char* buildKey(Build b);    // gamedata.cpp
// Детекция билда по заголовку ROM (= mdgfx/rom.py::detect_version): серийник @0x180 (-01=релиз;
// -00: SUBWAY@0x3170→ZTU, DOCKING BAY@0x30B0→нем., иначе June-прото), ≥3МБ→July-прото.
// hdr — первые байты файла (нужно ≥0x3200), fileSize — полный размер. Не-ZT MD-ROM → Unknown.
Build detectBuildFromHeader(const uint8_t* hdr, size_t hdrLen, size_t fileSize);   // gamedata.cpp

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

// ⭐ФИРМЕННЫЙ ШРИФТ ЭКРАНА ВЫБОРА (ZT sub_0c6934/0xC6AA4): 8×16, ЗАСЕЧКИ+строчные, сине-белый градиент.
// Таблица глифов @0xC7196 (4 б/символ: верх+низ тайл), тайлы 0x125116 (VRAM-база 0x4B0), палитра 3 @0xC7302.
struct SelFont {
    uint32_t glyph[96][16 * 8] = {};   // ASCII 0x20..0x7F, 8×16 ARGB (индекс 0 = прозрачный, alpha 0) — экран выбора
    uint8_t  idx[96][16 * 8] = {};     // те же глифы как СЫРЫЕ индексы палитры 0..15 (для перекраски: текст брифинга иной палитрой)
    bool have = false;
};

// ⭐КАРТОЧКА БОЙЦА (экран выбора персонажа, PLANET DEFENSE CORPS). Текст = ASCII @0xC6B36 (шаг 0x13E, 5 бойцов):
// 6 заголовков ×0x12 (имя/код/класс/рост/вес/дата) + 6 строк био ×0x23. Портрет — отдельная графика (декод в gamedata.cpp).
struct FighterCard {
    std::string name, code, cls, height, weight, dob;   // 6 заголовочных полей
    std::string bio[6];                                 // 6 строк биографии
    std::vector<uint32_t> portrait;                     // ARGB-портрет (0 если не декодирован)
    int pw = 0, ph = 0;                                 // размер портрета
};

struct GameData {
    Build    build = Build::Unknown;
    EngineProfile profile;
    std::vector<Level> levels;          // эпизоды (число — по билду)
    Palette  wallPal;
    Palette  nvPal;      // ⭐НОЧНИК: зелёная CRAM-палитра 0x2072 (ROM 29ee: -$6fa2≥0 → вся сцена ею; иначе 0x20F2)
    Palette  pauseTextPal; // ⭐заголовок паузы: CRAM line 2 @0x2132 (глифы 57bc2 = pal2, CRAM на паузе геймплейная)
    // ⭐SEGA-заставка (ROM sub_05711e, вызывается из boot 0xA2C ПЕРЕД копирайтом): вращающийся+приближающийся
    // логотип SEGA = «стена» движка (рендер ccb6) с текстурой из БАНКА СТЕН (тайлы 0xF7..0xFE, лист 128×64,
    // профиль @0x5710E чередует верх/низ), геометрия по sin-LUT 0x8124, палитро-анимация окна 12 цветов
    // (CRAM 4..15) по кольцу @0x570CA (22 шага). segaPal = база @0x570C2 (16 слов CRAM).
    uint16_t segaPal[16] = {};
    uint16_t segaAnim[34] = {};         // кольцо палитро-анимации @0x570CA (22 сдвига × окно 12 слов)
    bool     hasSega = false;
    WallBank wall;
    WallBank obj;                       // банк графики объектов/декора (0x10E9BE, 66 тайлов 32×32 — тот же формат)
    // ⭐PER-EPISODE ресурсы (BZT June: у каждого эпизода СВОЙ банк стен/палитра/fc-шаблоны — дескрипторы
    // @0xB9B96; ZT/ZTU: пусто — общий банк). applyEpisodeAssets(ep) переливает в wall/wallPal/fcTemplates.
    std::vector<WallBank>             wallEp;
    std::vector<WallBank>             objEp;      // June: items_imgs эпизода (66 тайлов, формат = ZT obj-банк)
    std::vector<Palette>              wallPalEp;
    std::vector<std::vector<uint8_t>> fcTemplatesEp;
    std::vector<std::vector<uint8_t>> shadeRampsEp;   // June: CLUT-рампы эпизода (дескриптор +0x10)
    std::vector<std::vector<uint32_t>> hudEp;         // June: кокпит-HUD эпизода (палитра из дескриптора +0x04)
    std::vector<Palette> heldPalEp;                   // June: палитра held-оружия (линия 1 CRAM эпизода)
    std::vector<Palette> hudIconPalEp;                // June: палитра иконок инвентаря (линия 3)
    void applyEpisodeAssets(int ep) {
        if (ep < 0) return;
        if (ep < (int)wallEp.size())        wall        = wallEp[ep];
        if (ep < (int)objEp.size())         obj         = objEp[ep];
        if (ep < (int)wallPalEp.size())     wallPal     = wallPalEp[ep];
        if (ep < (int)fcTemplatesEp.size()) fcTemplates = fcTemplatesEp[ep];
        if (ep < (int)shadeRampsEp.size())  shadeRamps  = shadeRampsEp[ep];
        if (ep < (int)hudEp.size() && !hudEp[ep].empty()) hud = hudEp[ep];
        if (ep < (int)heldPalEp.size())    heldPal    = heldPalEp[ep];
        if (ep < (int)hudIconPalEp.size()) hudIconPal = hudIconPalEp[ep];
    }
    Panorama bgCity, bgSpace;
    std::vector<uint8_t> shadeRamps;    // копия ROM-региона рамп (база 0x3392, 0x1000 б)
    std::vector<uint8_t> fcTemplates;   // шаблоны пол/потолок: 5 env × 0xA0 б (по env-индексу 0..4)
    std::vector<uint32_t> hud;          // HUD/кокпит 320×224 ARGB (референс-режим); пусто если нет
    std::vector<uint32_t> pauseBg;      // ФОН меню паузы 320×224 ARGB (руки держат карту-PDA, ZT @0x12DD06); пусто если нет
    std::vector<uint32_t> bztMapBg;     // BZT June: отдельный экран карты 320×224 (панель, ROM 2B9E)
    std::array<std::array<uint8_t, 16>, 8> bztMapBrick{};   // 8 микротайлов карты: 4×4 px, из ROM 0x164B62
    std::array<uint8_t, 256> bztMapLut{};                   // June sub_00DC2A: celltype/nibble -> 0..7
    std::array<uint8_t, 64> bztMapPlayerMark{};             // sprite tile 0x276, 8×8, idx0 transparent
    std::array<uint8_t, 64> bztMapObjectMark{};             // sprite tile 0x286, 8×8, idx0 transparent
    // ⭐ID-КАРТЫ ОТРЯДА U-RON (пауза-экран Tab, ROM 2acc: таблица 0x2CC8[$FF1030] → блиттер 0x1F72 @(192,128)):
    std::vector<std::vector<uint32_t>> idCards;   // 5 карт 128×96 ARGB (idx0 прозрачен); пусто = не декодированы
    int idcW = 0, idcH = 0;             // размер карты в px (ZT 128×96)
    std::vector<std::string> levelNames; // имена уровней (ZT @0x30b3, 16 симв./шт, 48 = 3 эп × 16 этажей)
    std::vector<std::string> pauseNames; // ⭐длинные имена пауза-списка (ZT @0x576aa, 25 б/шт «NAME LEVEL N : », 48)
    // ⭐СТАРТОВЫЕ ЗАСТАВКИ (ROM boot 0x976): копирайт-экран (0x116DBE, fade) + TECHNOPOP-анимация (22 кадра
    // 0x2057E.., палитра 0x1FC3E) — анимированное интро при запуске (1efd2). Каждый кадр = 320×224 ARGB.
    std::vector<uint32_t>              copyrightScreen;   // экран копирайта Technopop (0x116DBE); пусто если нет
    std::vector<std::vector<uint32_t>> introFrames;       // 22 кадра TECHNOPOP-заставки; пусто если нет
    std::vector<std::vector<uint32_t>> accoladeFrames;    // ⭐15 шагов логотипа ACCOLADE (sub_052C7E); пусто если нет
    // ⭐ЛОГО-ЭКРАНЫ МОДА (ZTU 0x2638e: PIKO + эмблема разработчика; тайлы 0x2444A, NT {x,y,w,h}+w×h слов
    // @0x2616A/0x261FE, палитры @0x2442A/0x2440A, по 0xB4=180 кадров с палитро-фейдом). 320×224 ARGB.
    std::vector<std::vector<uint32_t>> logoFrames;
    // ⭐ТИТУЛ (1cf36): спрайт-оверлеи (тайлы 0x123716/0x124DD6 @VRAM 0x355, пал линии 0x1DCF4+n·32):
    std::vector<uint32_t> titleFighter;   // силуэт бойца (16 спрайтов @0x1D5A2), 320×224 ARGB (alpha)
    std::vector<uint32_t> titleMenu;      // текст START/OPTIONS (4 спрайта @0x1D4D6), 320×224 ARGB
    std::vector<uint32_t> titleCursor;    // курсор-стрелка (tile 0x40B, 2×1 тайла = 16×8), ARGB
    std::vector<uint32_t> titleLogo;      // фон-логотип титула (когда вскроем раскладку); 320×224
    // ⭐ТИТУЛ-АНИМАЦИЯ (лента+лучи+глоу, рендерер title_fx.hpp): сырые ROM-блоки
    std::vector<uint8_t> titleGfxBlk;     // [0x119B8E..0x125100): тайл-банки + NT ленты/лучей + спрайт-тайлы
    std::vector<uint8_t> titleTabBlk;     // [0x1D400..0x1DE00): SAT/стримы пульса/палитры/глоу-таблицы
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
    Palette  hudIconPal;                // палитра HUD-иконок (ZT = heldPal 0x20D2; June = линия 3 палитры эпизода)
    ZtFont   font;                      // Letters (A-Z 0-9 . - : ?) — нижний-левый HUD-инфо
    ZtFont   fontNum;                   // Numbers (0-9) — HP / счётчик врагов на HUD
    ZtFont   fontAlt;                   // Font2 (0-9 A-Z) — название уровня (пауза)
    ZtFontBig fontBig;                  // Font_grph 8×16 (цветной+палитра) — меню/настройки/сюжет/пауза
    std::vector<FighterCard> fighters;  // 5 бойцов PLANET DEFENSE CORPS (экран выбора; текст @0xC6B36)
    SelFont  selFont;                   // фирменный шрифт экрана выбора (8×16, засечки, градиент)
    uint16_t startInv[5][4] = {};       // СТАРТОВЫЙ ИНВЕНТАРЬ по бойцу (ZT @0xF98): 2 пары (id, count 8.8). RAMOS=пусто (кулаки)
    // ⭐ПОЕЗД МЕТРО ZTU (FSM 0xDE8DA, см. train.hpp): скрипты текстур ROM 0xDE578..0xDE7E0 (0x268 Б).
    std::vector<uint8_t> trainScript;   // пусто = подсистемы нет (все билды кроме ZTU)
    // ⭐ЗАСТАВКИ/БРИФИНГИ (ZT @0xCB1E4): скроллящийся текст поверх фона. Фон = MD nametable 40×28, 64-цвет палитра
    // (линии 0-1 из screen-pal, линии 2-3 из общей 0xCD47A). Текст = палитра-линия 3 (шрифт-LUT 0xCD30E).
    struct Briefing { std::vector<std::string> lines; std::vector<uint32_t> bg; int bgW = 0, bgH = 0;
                      uint32_t textCol[16] = {}; };   // палитра текста (линия 3) для перекраски selFont глифов
    std::vector<Briefing> briefings;    // индекс = Brief enum (intro/mission/decoy/central/subbase/victory/badend1/badend2)
    uint32_t selBg = 0xFF000000u;       // ФОН экрана выбора (backdrop CRAM[0], светло-серый ~218)
    std::vector<uint32_t> selArrow;     // спрайт СТРЕЛКА (24×32, ARGB, палитра 1 жёлтая); ↓ = vflip
    std::vector<uint32_t> selPush;      // спрайт "PUSH START TO SELECT" (32×32, ARGB)
    // ⭐СТРЕЛКА-КУРСОР МЕНЮ ОПЦИЙ: 16×8 ARGB «────►», два горизонтальных тайла. idx0 = прозрачный.
    //   Оригинальные данные подключены для ZT и ZTU; у остальных билдов optArrowHave=false.
    uint32_t optArrow[16 * 8] = {};   // 16 шириной × 8 высотой
    bool     optArrowHave = false;
    // ⭐КУРСОР-СКОБКА ЭКРАНА ПАРОЛЯ (ROM 0x581BA: спрайт-тайл 0x243 @ROM 0x12ea86, 8×8, палитра line2 0x1CDAE):
    //   угол ┌; зеркалится в 4 угла рамки вокруг ячейки. idx0 = прозрачный. Только ZT.
    uint32_t pwCursor[8 * 8] = {};
    bool     pwCursorHave = false;
    std::vector<uint32_t> deceasedGfx;  // графика надписи DECEASED (ARGB); пусто если нет
    int      deceasedW = 0, deceasedH = 0;
    int      deceasedX = 0, deceasedY = 0;  // native-позиция bbox надписи в панели 320×224 (для точной посадки как в ZT)
    // ШРИФТ ЦИФР БОЕЗАПАСА (резолвер FUN_1DF54, база 0x15A06E): 5 пар по 0x18 б, цифра 4px×6.
    uint8_t  digitFont[0x78] = {0};
    // GEMS-банки звука per-build (передаются в snd::load; ZT-дефолты)
    size_t sndSamp = 0x5E51C, sndPatch = 0x5A804, sndSeq = 0x5B0CC, sndSfx = 0xC5EB4;
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
