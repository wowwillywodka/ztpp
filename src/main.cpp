// ztpp — Zero Tolerance C++ port, прототип (Фаза 0–2).
//
// Режимы (TAB): 0 celltype-карта · 1 текстурная карта · 2 атлас банка стен ·
//               3 FPS-вид (рейкастер, вид от 1-го лица по реальной геометрии ZT).
//
// Конвейер (валидирован): ROM -> CRAM-палитра -> 32x32 тайлы (column-major) ->
// ZMAP с ДВОЙНОЙ КОСВЕННОСТЬЮ (cell-ID -> celltype / texorder -> texdef -> тайл).
//
// Управление: 1/2/3 эпизод · TAB режим · G сетка · , / . (или [ ]) — этаж ·
//   FPS:    W/S или ↑/↓ — вперёд/назад · A/D — стрейф · ←/→ — поворот · N — noclip
//   ESC/Q — выход.
//
// Без окна: `ztpp <rom> --dump out.ppm [--ep N] [--floor N] [--mode N]`.

#include "rom.hpp"
#include "gfx.hpp"
#include "level.hpp"

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>
#include <vector>
#include <cstdint>
#include <fstream>
#include <algorithm>

// ROM-адреса ZT-релиза вынесены в gamedata.hpp::loadGameDataFromRom (ЭТАП 0: модель данных).

// ----- Софт-framebuffer ----- вынесен в render/framebuffer.hpp (FB + FBW/FBH/g_view*).
#include "framebuffer.hpp"

#include "raycaster.hpp"
#include "faithful.hpp"
#include "ui.hpp"          // экранное меню настроек (ESC) + шрифт + сохранение в файл
#include "map_icons.hpp"   // значки клеток карты (16×16, из mdgfx/mapres) — рисуются по типу клетки
#include "map_render.hpp"  // ⭐ассеты пауза-карты ZT (кирпичики/палитра CRAM3/маркер-крестик из VRAM+ROM)
#include "radar_render.hpp"// ⭐ассеты РАДАРА кокпита ZT (тайлы-грид 0x347+/палитра линии3/маркер-«+» 0x351 из геймплей-VRAM)
#include "walls.hpp"       // разрушаемые/секрет-стены (применение очереди разрушения к Level)
#include "weapons.hpp"     // инвентарь / подбор / выбор оружия / отрисовка в руках (после FB + ui)
#include "messages.hpp"    // HUD-сообщения ZT (очередь, нижний-левый угол) — после ui + actors
#include "console.hpp"     // внутриигровая консоль (`): команды give/spawn/god/…, история, автодополнение, логи
#include "sound.hpp"       // PCM-звук эффектов (SDL audio): загрузка сэмплов + микшер + триггеры событий
#include "soundtest.hpp"   // Sound Test меню (F2): прокрутка/проигрывание/стоп/подстройка ноты всех звуков

#include "mapview.hpp"   // top-down виды карт (вынесены из main.cpp)

// (см. README: враги — AI/LOS/типы атак в actors.hpp; карта — drawGameMap выше; настройки — ui.hpp)
// HUD оружия: имя текущего ствола + боезапас (низ-лево) + HP. Кулаки = "FISTS" без счётчика.
static void drawWeaponHud(FB& fb, const Inventory& inv) {
    char line[64];
    if (inv.current < 0) std::snprintf(line, sizeof(line), "FISTS");
    else                 std::snprintf(line, sizeof(line), "%s  %d", ITEMS[inv.current].name, inv.ammo[inv.current]);
    drawText(fb, 12, FBH - 28, line, 0xFFFFD050u, 2);   // жёлтый, как HUD оружия ZT
    drawText(fb, 12, FBH - 50, "[Z/X] WEAPON", 0xFF80C0FFu, 1);
    const PlayerState& p = player();                     // HP (низ-право): число + цвет по HP (жёлт/оранж/красн, как ZT)
    char hp[16]; std::snprintf(hp, sizeof(hp), "HP %d", p.hp);
    drawText(fb, FBW - 90, FBH - 28, hp, hpColor(p.hp), 2);
}

static void renderFPStoFB(FB& fb, const GameData& gd, const Level& lvl, const Palette& wallPal,
                          Camera& cam, MetaCache& meta, std::vector<double>& zbuf, bool faithful,
                          const Inventory& inv) {
    int envMode = lvl.env(cam.floor);
    auto putfn = [&](int x, int y, uint32_t c) { fb.put(x, y, c); };
    if (faithful) renderFaithful(putfn, FBW, FBH, lvl, wallPal, meta, cam, zbuf, envMode);
    else          renderFPS(putfn, FBW, FBH, lvl, wallPal, meta, cam, zbuf, envMode);
    drawMinimap(fb, lvl, cam);
    drawHeldWeapon(fb, FBW, FBH, gd, inv);   // оружие в руках (низ-центр)
    drawLaserSight([&](int x, int y, uint32_t c) { fb.put(x, y, c); }, 0, 0, FBW, FBH, inv,
                   zbuf.empty() ? 10.0 : zbuf[FBW / 2]);   // лазерный прицел (id 10): точка по дистанции центр.колонны
    drawWeaponHud(fb, inv);                   // имя + боезапас + HP
    applyDamageFlash(fb.px.data(), FBW * FBH); // вспышка урона (тинт всего кадра)
}

// РЕФЕРЕНС-РЕЖИМ: рендер игры точно как в оригинале — 3D-вид в нативном окошке 128×80 (×2 по гориз.
// = 256×80) внутри кокпит-HUD 320×224, всё масштабируется равномерно в окно. Для точного сравнения
// с игрой и заполнения пространства HUD-ом. Раскладка/HUD — из дизасма (gamedata.hpp).
static void renderReference(FB& fb, const GameData& gd, const Level& lvl, const Palette& wallPal,
                            Camera& cam, MetaCache& meta, std::vector<double>& zbuf, const Inventory& inv) {
    // 1) 3D faithful в ИСТИННОМ нативном ZT 256×80 (focalH=128/focalV=64, неквадратный пиксель 2:1):
    //    128 геом-колонн × 2px, дизер пол/потолок 256 (2 нибла = 2 разных px), стены 256, высота 1:1.
    //    hstretch=1 — растяжки нет (×2 по гориз. уже встроена в нативные 256). Это и есть кадр игры 1:1.
    static std::vector<uint32_t> view; view.assign((size_t)HUD_VW * HUD_VH, 0xFF000000u);
    double savedHs = faHStretch(); faHStretch() = 1.0;
    int envMode = lvl.env(cam.floor);
    // ⭐НАТИВ 128 КОЛОНОК (как ZT: 128 геом-колонн, каждая ×2px = 256 на экране). renderFaithful рендерит
    // стены/дизер/аффинный-texU в 128-широкий nat, затем УДВАИВАЕТ по X до 256 (scX=W/NW=2). Рендер в 256
    // колонок давал ВДВОЕ высокое разрешение → мелкую «шахматку» дизера и «волны» аффин-texU при повороте.
    renderFaithful([&](int x, int y, uint32_t c) { if (x >= 0 && x < HUD_VW && y >= 0 && y < HUD_VH) view[(size_t)y * HUD_VW + x] = c; },
                   HUD_VW, HUD_VH, lvl, wallPal, meta, cam, zbuf, envMode, HUD_VW / 2, HUD_VH);
    faHStretch() = savedHs;

    // 2) кадр 320×224 = HUD-кокпит + 3D-вид (256×80) в окно (HUD_VX,HUD_VY) — НАПРЯМУЮ 1:1
    static std::vector<uint32_t> frame; frame.assign((size_t)HUD_W * HUD_H, 0xFF101014u);
    if ((int)gd.hud.size() == HUD_W * HUD_H) std::copy(gd.hud.begin(), gd.hud.end(), frame.begin());
    for (int y = 0; y < HUD_VH; ++y)
        for (int x = 0; x < HUD_VW; ++x)
            frame[(size_t)(HUD_VY + y) * HUD_W + (HUD_VX + x)] = view[(size_t)y * HUD_VW + x];

    // (радар рисуем НИЖЕ — прямо на fb в полном разрешении, чтобы значки/cell ID были мельче и чётче,
    //  а не удваивались вместе с кокпитом ×scale)

    // 2c) ОРУЖИЕ В РУКАХ — ПИКСЕЛЬ-В-ПИКСЕЛЬ как ZT: VDP-спрайт в нативных экранных координатах 320×224
    //     (точные SAT-позиции из дизасма), клип к 3D-окну (кокпит режет низ оружия, как в оригинале).
    if (!faWallsOnly()) {   // отладка wallsonly: без оружия/прицела в кадре
    drawHeldNative([&](int x, int y, uint32_t c) { if (x >= 0 && x < HUD_W && y >= 0 && y < HUD_H) frame[(size_t)y * HUD_W + x] = c; },
                   HUD_VX, HUD_VY, HUD_VW, HUD_VH, gd, inv);
    drawLaserSight([&](int x, int y, uint32_t c) { if (x >= 0 && x < HUD_W && y >= 0 && y < HUD_H) frame[(size_t)y * HUD_W + x] = c; },
                   HUD_VX, HUD_VY, HUD_VW, HUD_VH, inv, zbuf.empty() ? 10.0 : zbuf[HUD_VW / 2]);  // лазерный прицел id 10
    }

    // 2d) HUD ИНВЕНТАРЯ — карусель иконок оружия (5 верхних панелей) + счётчик боезапаса, текущий по центру.
    drawInventoryHud(frame.data(), HUD_W, HUD_H, gd, inv);
    // 2e) HP — ЧИСЛО в ЖЁЛТОМ слоте справа-сверху кокпита (296,72) + СЧЁТЧИК ВРАГОВ в БЕЛОМ слоте слева (8,72).
    drawHpHud(frame.data(), HUD_W, HUD_H, gd);
    drawEnemyCountHud(frame.data(), HUD_W, HUD_H, gd, cam.floor);
    // 2f) ВСПЫШКА УРОНА — тинт всего кадра (палитра краснеет/белеет, как ZT d98e), сила ~ урон, затухает.
    applyDamageFlash(frame.data(), HUD_W * HUD_H);

    // 3) масштаб 320×224 → fb (равномерно, целочисленно, по центру; рамка-леттербокс чёрная)
    fb.clear(0xFF000000u);
    int scale = std::min(FBW / HUD_W, FBH / HUD_H); if (scale < 1) scale = 1;
    int ox = (FBW - HUD_W * scale) / 2, oy = (FBH - HUD_H * scale) / 2;
    for (int y = 0; y < HUD_H; ++y)
        for (int x = 0; x < HUD_W; ++x) {
            uint32_t c = frame[(size_t)y * HUD_W + x];
            for (int sy = 0; sy < scale; ++sy)
                for (int sx = 0; sx < scale; ++sx)
                    fb.put(ox + x * scale + sx, oy + y * scale + sy, c);
        }
    // 4) РАДАР — НА fb в ПОЛНОМ разрешении (значки + cell ID мельче/чётче: шрифт не удваивается с кокпитом).
    //    Позиция = радар-область кокпита (HUD_RX/RY) × scale.
    int rrx = ox + HUD_RX * scale, rry = oy + HUD_RY * scale, rrw = HUD_RW * scale, rrh = HUD_RH * scale;
    if (gameMapMode()) drawGameMap(fb.px.data(), FBW, FBH, rrx, rry, rrw, rrh, lvl, cam, gd, inv.owned[1]);  // блипы: только BIO SCANNER (idx1)
    else               drawMinimapRect(fb.px.data(), FBW, FBH, rrx, rry, rrw, rrh, lvl, cam);
}

static void render(FB& fb, const GameData& gd,
                   int ep, int floor, int mode, bool grid,
                   Camera& cam, MetaCache& meta, std::vector<double>& zbuf, bool faithful, bool reference,
                   const Inventory& inv) {
    // регион игры в fb для презентации (reference кладёт 320×224 ×2 = 640×448 по центру; иначе весь fb).
    // Презентуем именно его (без запечённых чёрных полос) → 4:3 заполняет окно по высоте.
    if (mode == 3 && reference) {
        g_viewW = HUD_W * 2; g_viewH = HUD_H * 2;
        g_viewX = (FBW - g_viewW) / 2; g_viewY = (FBH - g_viewH) / 2;     // = (0, 96) при scale 2
    } else { g_viewX = 0; g_viewY = 0; g_viewW = FBW; g_viewH = FBH; }
    if      (mode == 3 && reference) renderReference(fb, gd, gd.levels[ep], gd.wallPal, cam, meta, zbuf, inv);
    else if (mode == 3) renderFPStoFB(fb, gd, gd.levels[ep], gd.wallPal, cam, meta, zbuf, faithful, inv);
    else if (mode == 2) renderAtlas(fb, gd.wall, gd.wallPal);
    else                renderMap(fb, gd.levels[ep], gd.wallPal, gd.wall, floor, mode, grid);
}

static bool writePPM(const FB& fb, const std::string& path) {
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    std::fprintf(f, "P6\n%d %d\n255\n", FBW, FBH);
    std::vector<uint8_t> row(static_cast<size_t>(FBW) * 3);
    for (int y = 0; y < FBH; ++y) {
        for (int x = 0; x < FBW; ++x) {
            uint32_t c = fb.px[static_cast<size_t>(y) * FBW + x];
            row[x * 3 + 0] = (c >> 16) & 0xFF;
            row[x * 3 + 1] = (c >> 8) & 0xFF;
            row[x * 3 + 2] = (c >> 0) & 0xFF;
        }
        std::fwrite(row.data(), 1, row.size(), f);
    }
    std::fclose(f);
    return true;
}

static std::string findRom(int argc, char** argv) {
    if (argc > 1 && argv[1][0] != '-') return argv[1];
    const char* cand[] = {
        "Zero Tolerance (USA, Europe) (Rev A).gen",
        "../Zero Tolerance (USA, Europe) (Rev A).gen",
        "../../Zero Tolerance (USA, Europe) (Rev A).gen",
    };
    for (auto c : cand) { std::ifstream f(c, std::ios::binary); if (f) return c; }
    return "";
}
static int argInt(int argc, char** argv, const char* key, int def) {
    for (int i = 1; i < argc - 1; ++i)
        if (std::strcmp(argv[i], key) == 0) return std::atoi(argv[i + 1]);
    return def;
}
static const char* argStr(int argc, char** argv, const char* key) {
    for (int i = 1; i < argc - 1; ++i)
        if (std::strcmp(argv[i], key) == 0) return argv[i + 1];
    return nullptr;
}

#ifndef ZTPP_NO_SDL
#include <SDL.h>
// ── ПЕРЕНАЗНАЧАЕМЫЕ КЛАВИШИ + 4 ПРЕСЕТА: экранное имя (SDL) + дефолты (SDL_SCANCODE_*) ──
static void refreshKeyName(int a) {                              // имя клавиши АКТИВНОГО пресета
    const char* n = SDL_GetScancodeName((SDL_Scancode)keyBind(a));
    keyBindNames()[a] = (n && n[0]) ? std::string(n) : std::string("?");
}
static void refreshAllKeyNames() { for (int i = 0; i < GA_COUNT; ++i) refreshKeyName(i); }  // при смене пресета/загрузке
static void initKeyBinds() {                                     // дефолты для НЕзаданных (ini мог задать) в КАЖДОМ пресете + имена
    static const int DEF[GA_COUNT] = {
        SDL_SCANCODE_W, SDL_SCANCODE_S, SDL_SCANCODE_A, SDL_SCANCODE_D, SDL_SCANCODE_LEFT, SDL_SCANCODE_RIGHT,
        SDL_SCANCODE_SPACE, SDL_SCANCODE_J, SDL_SCANCODE_C, SDL_SCANCODE_Z, SDL_SCANCODE_X, SDL_SCANCODE_TAB
    };
    for (int p = 0; p < NUM_PRESETS; ++p)
        for (int i = 0; i < GA_COUNT; ++i)
            if (keyBindP(p, i) == 0) keyBindP(p, i) = DEF[i];    // каждый пресет: незаданные → дефолт
    refreshAllKeyNames();                                        // экранные имена активного пресета
}
// Прямоугольник аспекта contentAR, вписанный в окно по центру (леттербокс/пилларбокс).
static SDL_Rect fitRect(int winW, int winH, double contentAR) {
    double winAR = (winH > 0) ? (double)winW / winH : 1.0;
    int dw, dh;
    if (winAR > contentAR) { dh = winH; dw = (int)(winH * contentAR + 0.5); }  // окно шире → поля по бокам
    else                   { dw = winW; dh = (int)(winW / contentAR + 0.5); }  // окно выше → поля сверху/снизу
    return SDL_Rect{(winW - dw) / 2, (winH - dh) / 2, dw, dh};
}
// SRC (что копируем из fb) + DST (куда в окне). Вариант A: в игре презентуем ТОЛЬКО регион игры (без
// запечённых полос) → 4:3 растягивается на всю высоту окна. В меню — весь fb (чтобы меню не обрезалось).
static void presentRects(int winW, int winH, bool menuOpen, SDL_Rect& src, SDL_Rect& dst) {
    if (menuOpen) { src = SDL_Rect{0, 0, FBW, FBH}; dst = fitRect(winW, winH, (double)FBW / FBH); return; }
    src = SDL_Rect{g_viewX, g_viewY, g_viewW, g_viewH};                  // только регион игры
    double ar = (presentAspect() == 2) ? ((winH > 0) ? (double)winW / winH : 1.0)  // Stretch
              : (presentAspect() == 0) ? (4.0 / 3.0)                              // 4:3 (ЭЛТ)
                                       : (double)g_viewW / g_viewH;               // 1:1 (квадратный пиксель)
    dst = fitRect(winW, winH, ar);
}
inline void applyFullscreen(SDL_Window* w) {
    SDL_SetWindowFullscreen(w, presentFullscreen() ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
}
#endif

int main(int argc, char** argv) {
    std::string romPath = findRom(argc, argv);
    if (romPath.empty()) {
        std::fprintf(stderr, "ROM не найден. Запуск: ztpp <path-to-rom.gen> [--dump out.ppm]\n");
        return 1;
    }
    Rom rom;
    if (!rom.load(romPath)) { std::fprintf(stderr, "Не удалось прочитать ROM: %s\n", romPath.c_str()); return 1; }
    std::printf("ROM: %s (%zu байт)\n", romPath.c_str(), rom.size());

    // РЕНДЕР-МАСШТАБ (до создания FB/окна): внутренний fb = 640×RS. Из ini (раннее чтение) + override --rscale.
    presentRenderScale() = loadRenderScaleEarly("ztpp_settings.ini");
    { int rs = argInt(argc, argv, "--rscale", presentRenderScale()); presentRenderScale() = rs < 1 ? 1 : (rs > 3 ? 3 : rs); }
    FBW = FBH = 640 * presentRenderScale();

    // ЭТАП 0: вся игровая модель — в GameData; адреса ROM только в loadGameDataFromRom.
    // (Будущее: loadGameDataFromOztd для модов — порт об источнике не знает.)
    GameData gd;
    if (!loadGameDataFromRom(gd, rom))
        std::fprintf(stderr, "ВНИМАНИЕ: ZMAP-сигнатура E1 не совпала — данные могут быть неверны\n");
    pickupHiddenFn() = pickupIsConsumed;   // рендер пикапов: скрывать подобранные клетки (хук в raycaster)
    g_uiFont = gd.font.have ? &gd.font : nullptr;   // настоящий ZT-шрифт для drawText (иначе public-domain)
    g_uiFontBig = gd.fontBig.have ? &gd.fontBig : nullptr;  // Font_grph 8×16 для меню/настроек/заголовков

    // Фоны: E1=КОСМОС, E2=ГОРОД, E3=КОСМОС (дизасм 0x105e, выбор по эпизоду). Скролл по углу в рендере.
    if (argStr(argc, argv, "--findct")) {            // диаг: скан всех эпизодов/этажей на celltype HEX → печать локаций
        int want = (int)std::strtol(argStr(argc, argv, "--findct"), nullptr, 0);
        for (int e = 0; e < gd.episodes(); ++e) for (int f = 0; f < Level::FLOORS; ++f)
            for (int y = 0; y < Level::H; ++y) for (int x = 0; x < Level::W; ++x)
                if (gd.levels[e].cellType(f, x, y) == (uint8_t)want)
                    std::printf("ct=0x%02x ep=%d floor=%d x=%d y=%d\n", want, e + 1, f, x, y);
        return 0;
    }
    int ep    = argInt(argc, argv, "--ep", 1) - 1;   if (ep < 0 || ep >= gd.episodes()) ep = 0;
    int floor = argInt(argc, argv, "--floor", 0);    if (floor < 0 || floor >= Level::FLOORS) floor = 0;
    int mode  = argInt(argc, argv, "--mode", 3);     if (mode < 0 || mode > 3) mode = 3; // ТОЛЬКО шутер (mode 3); прежние карта-режимы убраны, карта — по TAB (пауза)
    bool mapOpen = false;                            // ПАУЗА-КАРТА (по TAB): полная карта уровня + позиция игрока
    bool paused  = false;                            // ФОТОРЕЖИМ: пауза симуляции (кадр застывает для скрина; PAUSE/F1/\\ или консоль `pause`)
    bool prevFireHeld = false;                        // прошлый кадр: SPACE/ЛКМ зажаты (для edge-триггера semi-auto оружия)
    int      pulseSndBurst = 0;                        // ПУЛЬС: очередь звука 0x1e (1 выстрел = звук 3×, но 1 патрон — как в оригинале)
    uint32_t pulseSndNext  = 0;                        // время следующего звука очереди пульса
    uint32_t flameSndNext  = 0;                        // ОГНЕМЁТ: кулдаун ретриггера SFX_FLAME (авто-фаер ~20/сек = слишком часто)
    double   footAccum = 0.0, footPrevX = 0.0, footPrevY = 0.0;  // ШАГИ игрока (sfx 0x65/0x66): аккумулятор дистанции
    int      footToggle = 0; bool footInit = false;    // чередование левой/правой ноги
    double mouseTurn = 0;                             // аккумулятор поворота мышью (применяется в движении, БЕЗ инерции)
    activeBg() = gd.bgForEpisode(ep);
    bool grid = true;

    FB fb;
    Camera cam;  cam.floor = floor;
    MetaCache meta; meta.wall = &gd.wall; meta.obj = &gd.obj; meta.shadeRampData = gd.shadeRamps.data();
    meta.fcTemplateData = gd.fcTemplates.data();   // слой пол/потолок (ROM-шаблоны по env)
    WallAnimator wallAnim;                          // анимация текстур стен (мигание экранов/ламп/глаз)
    std::vector<double> zbuf;
    Inventory inv;                  // инвентарь игрока (старт: пусто = кулаки, current=−1)
    int aEp = -1, aFloor = -1;      // эпизод/этаж, для которого заспавнены враги-актёры (респавн при смене)
    bool fullInv = false;           // V: тест-режим «всё оружие + неограниченный инвентарь» (Z/X листают карусель)
    Inventory savedInv;             // сохранённый инвентарь до входа в fullInv (восстанавливается на выходе)
    HudMessages msgs;               // HUD-сообщения как в ZT (очередь, нижний-левый угол, 4×9, Letters)
    int msgFloor = cam.floor;       // для детекта смены этажа (вверх/вниз)
    int prevAlive = -1;             // живых врагов в прошлом кадре (для «FLOOR SECURED»)
    int deathTimer = 0;             // показ «WASTED» после смерти (кадры)
    int spdMsg = 0;                 // показ «ENEMY SPEED» при регулировке (кадры)
    int szMsg  = 0;                 // показ «SPRITE SIZE» при регулировке (N/M)
    int lastHp = 100;               // HP прошлого кадра (детект пересечения порогов 50/15 для предупреждений)
    bool noclip = false;
    int lastCX = -1, lastCY = -1;   // клетка игрока (для триггера лифтов/лестниц по входу)
    // ── ВРЕМЕННО (пока дорабатываем reference-рендер): игра работает ТОЛЬКО в reference-режиме.
    //    DDA-рейкастер и фуллскрин-faithful отключены — клавиши F/R и кнопки меню Render/Reference
    //    заблокированы, режим форсится при старте и после загрузки настроек (перебивает ini).
    //    ВЕРНУТЬ ВСЕ РЕЖИМЫ — поставить REFERENCE_ONLY=false (одна строка). Код DDA/FPS не тронут.
    constexpr bool REFERENCE_ONLY = true;
    bool faithful = REFERENCE_ONLY ? true : (argInt(argc, argv, "--faithful", 1) != 0); // F — тумблер faithful/DDA
    bool reference = REFERENCE_ONLY ? true : (argInt(argc, argv, "--reference", 1) != 0); // R — референс-режим (дефолт ON, = ztpp_settings.ini)
    bool enemiesOn = (argInt(argc, argv, "--enemies", 1) != 0);  // настройка: спавн врагов на уровне (для отладки можно выкл.)
    gameMapMode() = (argInt(argc, argv, "--gamemap", 1) != 0);   // настройка: карта как в игре (vs классическая)

    auto respawn = [&]() { cam.floor = floor; meta.reset(&gd.levels[ep]); rcSpawn(cam, gd.levels[ep]); };
    respawn();
    wallAnim.init(gd.levels[ep], meta);   // живой texdef эпизода → MetaCache (анимация стен)

    // Тест-override позиции/угла камеры (для отладки граней): --px --py --ang(градусы)
    if (argStr(argc, argv, "--px")) cam.px = std::atof(argStr(argc, argv, "--px"));
    if (argStr(argc, argv, "--py")) cam.py = std::atof(argStr(argc, argv, "--py"));
    if (argStr(argc, argv, "--dir")) { double a = std::atof(argStr(argc, argv, "--dir")) * 3.14159265 / 180.0;
        cam.dirX = std::cos(a); cam.dirY = std::sin(a); cam.planeX = -cam.dirY * 0.66; cam.planeY = cam.dirX * 0.66; }
    if (argStr(argc, argv, "--hstretch")) faHStretch() = std::atof(argStr(argc, argv, "--hstretch"));
    if (argStr(argc, argv, "--weapscale")) weaponScale() = std::atof(argStr(argc, argv, "--weapscale")); // тюнинг размера ствола
    if (argStr(argc, argv, "--decor"))  faDecor()  = (std::atoi(argStr(argc, argv, "--decor")) != 0); // вкл/выкл декор
    if (argStr(argc, argv, "--edgeclamp")) faEdgeClamp() = (std::atoi(argStr(argc, argv, "--edgeclamp")) != 0); // тест клампа края грани
    if (argStr(argc, argv, "--wallsonly")) faWallsOnly() = (std::atoi(argStr(argc, argv, "--wallsonly")) != 0); // отладка: только стены на сплошном фоне
    if (argStr(argc, argv, "--nowalls")) faNoWalls() = (std::atoi(argStr(argc, argv, "--nowalls")) != 0); // отладка: без стен → чистый пол/потолок-градиент
    if (argStr(argc, argv, "--decorfullres")) faDecorFullRes() = (std::atoi(argStr(argc, argv, "--decorfullres")) != 0); // декор/враги 256 (пост-блит) vs 128 (нат)
    if (argStr(argc, argv, "--spriteclampf")) faSpriteClampF() = std::atof(argStr(argc, argv, "--spriteclampf")); // кламп масштаба спрайта (0=выкл, дефолт 0.853=eda0 0x12C)
    if (argStr(argc, argv, "--walldither")) faWallDither() = (std::atoi(argStr(argc, argv, "--walldither")) != 0); // полный CLUT[band][byte] (дизер стен) vs диагональ
    if (argStr(argc, argv, "--spriteclut")) faSpriteClut() = (std::atoi(argStr(argc, argv, "--spriteclut")) != 0); // CLUT-шейд спрайтов (как стены) vs готовый ARGB
    if (argStr(argc, argv, "--spritebandadj")) faSpriteBandAdj() = std::atoi(argStr(argc, argv, "--spritebandadj")); // сдвиг band спрайта (калибровка)
    if (argStr(argc, argv, "--spritescalec")) faSpriteScaleC() = std::atof(argStr(argc, argv, "--spritescalec")); // константа scale=C/дист (калибровка порога band)
    if (argStr(argc, argv, "--spritebandrom")) faSpriteBandRom() = (std::atoi(argStr(argc, argv, "--spritebandrom")) != 0); // ROM-band (ed50) vs как стена
    if (argStr(argc, argv, "--spritediscscale")) faSpriteDiscScale() = (std::atoi(argStr(argc, argv, "--spritediscscale")) != 0); // ДИСКРЕТНЫЙ размер спрайта (MD scale-ступени) vs плавный float
    if (argStr(argc, argv, "--spritesizek")) faSpriteSizeK() = std::atof(argStr(argc, argv, "--spritesizek")); // калибровка размера спрайта (1.0=ZT-доля)
    if (argStr(argc, argv, "--decorclut")) faDecorSpriteClut() = (std::atoi(argStr(argc, argv, "--decorclut")) != 0); // шейд декора спрайтовым CLUT vs стеновым
    if (argStr(argc, argv, "--wallditherd0")) faWallDitherD0() = (int)std::strtol(argStr(argc, argv, "--wallditherd0"), nullptr, 0); // порог D0 дизера стен (дефолт 0x28=40)
    if (argStr(argc, argv, "--affinetex")) faAffineTexU() = (std::atoi(argStr(argc, argv, "--affinetex")) != 0); // тест аффин/перспектива texU
    if (argStr(argc, argv, "--frustumclip")) faFrustumClip() = (std::atoi(argStr(argc, argv, "--frustumclip")) != 0); // тест фрустум-клип vs near-plane
    if (argStr(argc, argv, "--fixedpoint")) faFixedPoint() = (std::atoi(argStr(argc, argv, "--fixedpoint")) != 0); // тест fixed/float рендер
    if (argStr(argc, argv, "--wallband0")) faWallBand0() = (std::atoi(argStr(argc, argv, "--wallband0")) != 0); // тест: стены band 0 (как ROM)
    if (argStr(argc, argv, "--dbgbg")) faDbgBgIdx() = std::atoi(argStr(argc, argv, "--dbgbg")); // индекс сплошного фона для wallsonly
    if (argStr(argc, argv, "--striptexel")) faStripTexel() = (std::atoi(argStr(argc, argv, "--striptexel")) != 0); // тест полосы текселя
    if (argStr(argc, argv, "--drawdist")) faDrawDist() = std::atoi(argStr(argc, argv, "--drawdist")); // отладка дальности (cull)
    if (argStr(argc, argv, "--stairk")) faStairK() = std::atof(argStr(argc, argv, "--stairk")); // калибровка крутизны скоса лестницы
    if (argStr(argc, argv, "--stairpitch")) faStairPitchOverride() = std::atof(argStr(argc, argv, "--stairpitch")); // статик-тест наклона
    if (argStr(argc, argv, "--stairuni")) faStairUni() = std::atof(argStr(argc, argv, "--stairuni")); // тест равномерного псевдопитча
    if (argStr(argc, argv, "--stairoff")) faStairOff() = (std::atoi(argStr(argc, argv, "--stairoff")) != 0); // диаг: откл. спец-лестницу
    if (argStr(argc, argv, "--stairfixtex")) faStairFixedTex() = (std::atoi(argStr(argc, argv, "--stairfixtex")) != 0); // A/B: fixed-point тексель профиль-грани (ROM ccb6)
    if (argStr(argc, argv, "--stairfacelr")) faStairFaceLR() = (std::atoi(argStr(argc, argv, "--stairfacelr")) != 0); // A/B: фикс направления шира (L к texU=0, своп при flipU)
    if (argStr(argc, argv, "--stairfacemap")) faStairFaceMap() = (std::atoi(argStr(argc, argv, "--stairfacemap")) != 0); // A/B: ремап слов профиля по граням [0,2,3,1]
    if (argStr(argc, argv, "--stairsteps")) faStairSteps() = (std::atoi(argStr(argc, argv, "--stairsteps")) != 0); // A/B: ступени-сегменты (слой B) vs floor-cast legacy
    if (argStr(argc, argv, "--transitnogate")) faTransitNoCamGate() = (std::atoi(argStr(argc, argv, "--transitnogate")) != 0); // A/B: профиль/шахта по геометрии клетки vs гейт по клетке камеры
    if (argStr(argc, argv, "--raydda")) faRayDDA() = (std::atoi(argStr(argc, argv, "--raydda")) != 0); // A/B: пер-колоночный DDA-рендер стен (в разработке)
    if (argStr(argc, argv, "--ang")) {
        double a = std::atof(argStr(argc, argv, "--ang")) * 3.14159265 / 180.0;
        cam.dirX = std::cos(a); cam.dirY = std::sin(a);
        cam.planeX = -cam.dirY * 0.66; cam.planeY = cam.dirX * 0.66;
        cam.ang512 = camDirToAng512(cam);
        cam.angI = ((int)std::lround(camDirToAng512(cam)) + 128) & 0x1FF;  // целочисл. camback как МД (для точной сверки)
    }

    if (argStr(argc, argv, "--pitch")) cam.pitch = std::atof(argStr(argc, argv, "--pitch")); // отладка питча
    if (argStr(argc, argv, "--cabin")) { cam.cabin = std::atof(argStr(argc, argv, "--cabin")); cam.pitch = -cam.cabin; } // отладка перехода (лестница)
    if (argStr(argc, argv, "--elev"))  { cam.cabin = std::atof(argStr(argc, argv, "--elev")); cam.pitch = -cam.cabin; cam.elevState = 1; } // отладка лифта

    // --- Отладка: найти клетки заданного celltype во всех этажах эпизода (--scanct 0x56) ---
    if (const char* sc = argStr(argc, argv, "--scanct")) {
        int want = (int)std::strtol(sc, nullptr, 0);
        const Level& L = gd.levels[ep];
        std::printf("scanct эп%d ct=0x%02X:\n", ep + 1, want);
        for (int f = 0; f < Level::FLOORS; ++f)
            for (int y = 0; y < Level::H; ++y)
                for (int x = 0; x < Level::W; ++x)
                    if (L.cellType(f, x, y) == want)
                        std::printf("  этаж %2d  (%2d,%2d)\n", f, x, y);
        return 0;
    }
    // --- Отладка: сетка celltype вокруг клетки (--celldump "x y floor" [радиус --cdr]) ---
    if (const char* cd = argStr(argc, argv, "--celldump")) {
        int cx = 0, cy = 0, cf = floor; std::sscanf(cd, "%d %d %d", &cx, &cy, &cf);
        int r = argInt(argc, argv, "--cdr", 8);
        const Level& L = gd.levels[ep];
        std::printf("celldump эп%d этаж%d центр(%d,%d) r=%d  (#=стена, *=центр)\n     ", ep + 1, cf, cx, cy, r);
        for (int x = cx - r; x <= cx + r; ++x) std::printf("%3d", x);
        std::printf("\n");
        for (int y = cy - r; y <= cy + r; ++y) {
            std::printf("y%3d ", y);
            for (int x = cx - r; x <= cx + r; ++x) {
                if (x < 0 || y < 0 || x >= Level::W || y >= Level::H) { std::printf("  ."); continue; }
                uint8_t ct = L.cellType(cf, x, y);
                char mark = (x == cx && y == cy) ? '*' : (cellRenderWall(ct) ? '#' : ' ');
                std::printf("%c%02X", mark, ct);
            }
            std::printf("\n");
        }
        return 0;
    }
    // --- Отладка: найти все клетки лифта в эпизоде (--findelev), печатает floor/x/y/ct/класс ---
    if (argStr(argc, argv, "--findelev")) {
        const Level& L = gd.levels[ep];
        int total = 0;
        for (int f = 0; f < Level::FLOORS; ++f)
            for (int y = 0; y < Level::H; ++y)
                for (int x = 0; x < Level::W; ++x) {
                    uint8_t ct = L.cellType(f, x, y);
                    const char* cls = ctElevUp(ct) ? "UP-cabin" : ctElevDown(ct) ? "DOWN-cabin"
                                    : ctElevUpdn(ct) ? "UPDOWN" : ctElevArea(ct) ? "area"
                                    : (ct == 0x30) ? "shaft-wall"
                                    : isStairProfileCT(ct) ? "STAIR-wall" : isStairFloorCT(ct) ? "STAIR-floor" : nullptr;
                    if (cls) { std::printf("  этаж=%2d (%2d,%2d) ct=0x%02X %-11s renderWall=%d blocks=%d\n", f, x, y, ct, cls, cellRenderWall(ct)?1:0, cellBlocks(ct)?1:0); ++total; }
                }
        std::printf("ep%d: найдено клеток лифта: %d\n", ep, total);
        return 0;
    }
    // --- Отладка: трасса поездки лифта (--simelev "x y floor"), печатает cabin/floor/pitch по кадрам ---
    if (const char* se = argStr(argc, argv, "--simelev")) {
        int sx = 0, sy = 0, sf = 0; std::sscanf(se, "%d %d %d", &sx, &sy, &sf);
        Camera sc{}; sc.px = sx + 0.4; sc.py = sy + 0.5; sc.floor = sf; sc.dirX = 1; sc.dirY = 0;
        sc.pxI = (int)std::lround(sc.px * 256.0); sc.pyI = (int)std::lround(sc.py * 256.0);  // 8.8 (центрирование клампит их)
        const Level& L = gd.levels[ep];
        std::printf("simelev старт (%d,%d) этаж %d, ct=0x%02X\n", sx, sy, sf, L.cellType(sf, sx, sy));
        // первый кадр со «входом в клетку» — триггер старта
        rcUpdateTransit(sc, L, true);
        for (int i = 0; i < 200; ++i) {
            bool busy = rcUpdateTransit(sc, L, false);
            if (i % 4 == 0 || sc.elevState == 0 || sc.elevState == 2 || sc.elevState == -2)
                std::printf("  k%3d: cabin=%+5.0f pitch=%+5.0f этаж=%2d elev=%+d ct=0x%02X %s\n",
                            i, sc.cabin, sc.pitch, sc.floor, sc.elevState,
                            L.cellType(sc.floor, (int)sc.px, (int)sc.py), busy ? "ride" : "");
            // СТОП: приехал (elevState ±2) или простаивает без наклона
            if ((sc.elevState == 2 || sc.elevState == -2) || (sc.elevState == 0 && i > 2 && sc.cabin == 0.0)) {
                std::printf("  СТОП на k%d, этаж=%d, elev=%+d\n", i, sc.floor, sc.elevState); break;
            }
        }
        return 0;
    }

    // --- Headless: дамп в PPM, без окна ---
    if (const char* dump = argStr(argc, argv, "--dump")) {
        { int di = argInt(argc, argv, "--dooriter", 15);   // степень открытия дверей в дампе (отладка)
          if (mode == 3) for (int i = 0; i < di; ++i) rcUpdateDoors(cam.floor, cam.px, cam.py, gd.levels[ep]); }
        for (int i = 0, af = argInt(argc, argv, "--animframe", 0); i < af; ++i)
            wallAnim.update();                              // прокрутить N игр.кадров анимации стен (отладка)
        decorFrame() = argInt(argc, argv, "--animframe", 0);  // фаза анимации декора (вентилятор/мигание)
        if (const char* w = argStr(argc, argv, "--weap")) {   // отладка: показать ствол N в руках
            int wi = (int)std::strtol(w, nullptr, 0);
            if (wi >= 1 && wi < 15) { inv.addItem(wi); inv.ammo[wi] = ITEMS[wi].ammoPickup;
                                      inv.sel = (int)inv.carried.size() - 1; inv.syncCurrent(); }
            else inv.current = -1;
        }
        if (argStr(argc, argv, "--fullinv")) {                // отладка: весь инвентарь (неогранич.) для HUD-карусели
            inv.unlimited = true;
            for (int i = 1; i < 15; ++i) { inv.addItem(i); inv.ammo[i] = 99; }
            inv.sel = argInt(argc, argv, "--sel", 0); inv.syncCurrent();
        }
        if (const char* fg = argStr(argc, argv, "--fighter")) { int v = std::atoi(fg); if (v >= 0 && v <= 4) playerFighter() = v; }  // боец U-RON 0..4 (перки оружия)
        if (const char* fr = argStr(argc, argv, "--fire")) inv.fire = std::atoi(fr);  // отладка: кадр выстрела 1..4
        if (const char* pv = argStr(argc, argv, "--punchvar"))  inv.punchVariant = std::atoi(pv);  // отладка: тип удара 0/1/2
        if (const char* ps = argStr(argc, argv, "--punchside")) inv.punchSide = std::atoi(ps);     // отладка: рука 0/1
        if (const char* hp = argStr(argc, argv, "--hp")) player().hp = std::atoi(hp);  // отладка: HP игрока (полоска)
        if (const char* fl = argStr(argc, argv, "--testflash")) player().flash = std::atoi(fl);  // отладка: сила вспышки урона
        if (argStr(argc, argv, "--mapids")) mapShowIds() = true;        // карта: показать cell ID (для дампа)
        if (argStr(argc, argv, "--god")) player().godmode = true;       // отладка: бессмертие в дампе
        if (argStr(argc, argv, "--freeze")) enemiesFrozen() = true;     // отладка: враги замерли в дампе
        if (mode == 3 && enemiesOn) spawnEnemiesFromLevel(gd.levels[ep], floor);   // враги этажа (для дампа/отладки)
        if (const char* sa = argStr(argc, argv, "--spawnahead")) {                 // отладка: враг ПЕРЕД камерой (имя или hex celltype)
            static const std::pair<const char*, uint8_t> EN[] = {{"sgt",0x29},{"fh",0x2A},{"imp",0x2B},{"hydaca",0x65},
                {"revenant",0x66},{"boss1",0x67},{"dog",0x68},{"fhsf",0x69},{"boss3",0x6A},{"boss2",0x6B}};
            uint8_t ct = 0; std::string n = sa; for (auto& c : n) c = (char)tolower((unsigned char)c);
            for (auto& p : EN) if (n == p.first) { ct = p.second; break; }
            if (!ct) ct = (uint8_t)std::strtol(sa, nullptr, 0);
            double d = argStr(argc, argv, "--spawndist") ? std::atof(argStr(argc, argv, "--spawndist")) : 2.5;
            if (ct) spawnEnemyByType(cam.floor, cam.px + cam.dirX * d, cam.py + cam.dirY * d, ct);
        }
        if (argStr(argc, argv, "--desttest")) {                        // отладка: разрушить все разруш./секрет-клетки этажа
            Level& L = gd.levels[ep]; int n = 0, sx = -1, sy = -1; uint8_t sct = 0, sid = 0;
            for (int yy = 0; yy < Level::H; ++yy) for (int xx = 0; xx < Level::W; ++xx) {
                uint8_t ct = L.cellType(floor, xx, yy);
                if (wallIsDestructible(ct)) { if (sx < 0) { sx = xx; sy = yy; sct = ct; sid = L.cellId(floor, xx, yy); } requestDestruct(floor, xx, yy); ++n; }
            }
            applyDestruct(L);
            if (sx >= 0) std::printf("desttest: %d разруш.клеток; пример (%d,%d): cellId %d→%d, celltype 0x%02X→0x%02X\n",
                                     n, sx, sy, sid, L.cellId(floor, sx, sy), sct, L.cellType(floor, sx, sy));
        }
        if (argStr(argc, argv, "--shootray")) { double hx, hy; damageRay(gd.levels[ep], floor, cam.px, cam.py, cam.dirX, cam.dirY, 10, hx, hy);
            std::printf("shootray: hit (%.2f,%.2f), очередь разрушения=%zu\n", hx, hy, destructQueue().size()); }
        if (argStr(argc, argv, "--shoot")) fireSpawn(inv, gd.levels[ep], cam);        // отладка: выстрел
        for (int i = 0, n = argInt(argc, argv, "--shoot", 0); i < n; ++i) updateActors(gd.levels[ep], cam);
        if (mode == 3) { collectAlarmCams(gd.levels[ep], floor); updateActors(gd.levels[ep], cam); applyDestruct(gd.levels[ep]); pushCameraBillboards(floor); }  // 1 тик: спрайты + разрушение + камеры (для дампа)
        if (argStr(argc, argv, "--probe")) { int cx = (int)(cam.px + cam.dirX * 0.7), cy = (int)(cam.py + cam.dirY * 0.7);
            std::printf("probe (%d,%d): cellId %d celltype 0x%02X\n", cx, cy, gd.levels[ep].cellId(floor, cx, cy), gd.levels[ep].cellType(floor, cx, cy)); }
        if (const char* di = argStr(argc, argv, "--dumpidx")) faDumpIdxPath() = di;  // почисленный дамп нат-индексов стен
        render(fb, gd, ep, floor, mode, grid, cam, meta, zbuf, faithful, reference, inv);
        if (argStr(argc, argv, "--pause")) drawPauseMap(fb, gd, ep, floor, cam);  // отладка: дамп меню паузы (Tab)
        if (const char* tm = argStr(argc, argv, "--testmsg")) {   // отладка: HUD-сообщение в дампе
            HudMessages dmsg;
            int id = std::atoi(tm); if (id > 0) dmsg.pushItem(id);
            dmsg.push(ztmsg::FLOOR_SECURED); dmsg.push(ztmsg::AMMO_LOW);
            dmsg.showFrames = 999; dmsg.update();
            dmsg.draw(fb, 14, FBH - 188, 2);
        }
        if (argStr(argc, argv, "--menu")) { // отладка: наложить меню настроек (значение = номер страницы 0..NPAGE-1)
            initKeyBinds();                 // биндинги/имена (в --dump путь до обычной инициализации не доходит)
            int mp = std::atoi(argStr(argc, argv, "--menu")); if (mp < 0 || mp >= menu::NPAGE) mp = 0;
            drawMenu(fb, 0.07, 0.04, faHStretch(), 60, 1.0, mp, faithful, noclip, reference, true, true, "Saved to ztpp_settings.ini");
        }
        if (!writePPM(fb, dump)) { std::fprintf(stderr, "Не удалось записать %s\n", dump); return 1; }
        std::printf("Записано %s (эп %d, этаж %d, режим %d)\n", dump, ep + 1, floor, mode);
        return 0;
    }

#ifdef ZTPP_NO_SDL
    std::fprintf(stderr, "Собрано без SDL. Используйте --dump out.ppm\n");
    return 1;
#else
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) != 0) { std::fprintf(stderr, "SDL_Init: %s\n", SDL_GetError()); return 1; }
    snd::load(rom);                                  // загрузка PCM-сэмплов + открытие аудио-устройства
    // РАЗМЕР ОКНА НЕЗАВИСИМ от внутреннего fb (640×RS): окно — нормального размера по дисплею, fb рендерится
    // внутри и ужимается SDL в окно (при RS>1 = суперсэмплинг → cell ID/оверлеи мельче и чётче). Не привязывать к FBW!
    int winSide = 720;
    { SDL_DisplayMode dm; if (SDL_GetDesktopDisplayMode(0, &dm) == 0) {
        winSide = std::min(dm.w, dm.h) - 80; if (winSide < 480) winSide = 480; if (winSide > 1000) winSide = 1000; } }
    SDL_Window* win = SDL_CreateWindow("ztpp — Zero Tolerance (prototype)",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, winSide, winSide, SDL_WINDOW_RESIZABLE);
    // без vsync — лимит кадров регулируем вручную (настройка 5..60)
    SDL_Renderer* ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED);
    SDL_Texture* tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888,
        SDL_TEXTUREACCESS_STREAMING, FBW, FBH);

    const char* modeName[] = {"celltype", "textured", "wall-atlas", "fps-3d"};
    bool running = true, dirty = true;
    double moveSpd = 0.102875, turnSpd = 0.074; // настраиваемые: O/P движение, K/L вращение (дефолты = ztpp_settings.ini)
    int frameLimit = 15;                        // лимит кадров (5..60), регулируется в меню

    // --- Настройки: автозагрузка из файла (меню ESC сохраняет туда же) ---
    initKeyBinds();                               // дефолты биндингов ДО загрузки (авто-ini запишет реальные клавиши, не нули)
    const char* CFG_PATH = "ztpp_settings.ini";
    { double st = faHStretch(); bool gm = gameMapMode();
      if (loadSettings(CFG_PATH, moveSpd, turnSpd, st, frameLimit, enemySpeedScale(), faithful, noclip, reference, enemiesOn, gm)) {
          faHStretch() = st; gameMapMode() = gm;
          if (frameLimit < 5) frameLimit = 5; if (frameLimit > 60) frameLimit = 60;
          std::printf("Настройки загружены из %s\n", CFG_PATH);
      } else {
          // Файла нет — первый запуск: звук ВКЛючаем по умолчанию (геттер по дефолту OFF),
          // затем сразу создаём файл с настройками по умолчанию (значения из инициализации
          // переменных выше + дефолты геттеров), чтобы он существовал с первого запуска.
          soundOn() = true;
          saveSettings(CFG_PATH, moveSpd, turnSpd, faHStretch(), frameLimit, enemySpeedScale(),
                       faithful, noclip, reference, enemiesOn, gameMapMode());
          std::printf("Создан %s с настройками по умолчанию\n", CFG_PATH);
      } }
    initKeyBinds();                               // биндинги клавиш: дефолты для незаданных ini + экранные имена
    if (REFERENCE_ONLY) { faithful = true; reference = true; }   // ВРЕМЕННО: держим reference-режим (перебиваем ini)
    applyFullscreen(win);                         // применить сохранённый режим фуллскрина
    msgs.showFrames = frameLimit * 2;           // HUD-сообщение держится ~2 сек (масштаб по fps)
    bool menu = false;                          // меню настроек открыто (ESC)
    int menuPage = 0;                            // страница меню настроек (0..NPAGE-1)
    char menuStatus[64] = "";
    auto clampd = [](double v, double lo, double hi) { return v < lo ? lo : (v > hi ? hi : v); };

    auto setFloor = [&](int nf) {
        if (nf < 0 || nf >= Level::FLOORS) return;
        floor = nf; dirty = true;
        if (mode == 3) respawn();
    };
    auto setEp = [&](int ne) { if (ne < 0 || ne >= gd.episodes()) return; ep = ne; activeBg() = gd.bgForEpisode(ep); dirty = true; if (mode == 3) respawn(); wallAnim.init(gd.levels[ep], meta); inv.reset(); fullInv = false; resetPlayerHP(); rcResetPickups(); clearActors(); clearWallState(); aEp = aFloor = -1; };  // новый эпизод → инвентарь/HP/актёры/стены с нуля
    auto setMode = [&](int nm) { mode = nm; dirty = true; if (mode == 3) respawn(); };

    // ── РЕГИСТРАЦИЯ КОНСОЛЬНЫХ КОМАНД (захват состояния игры) ──
    {
        // имя→id предмета (оружие + предметы). Алиасы для удобства.
        static const std::vector<std::pair<const char*, int>> ITEMID = {
            {"scanner",1},{"bioscanner",1},{"mine",2},{"vest",3},{"fireext",4},{"extinguisher",4},
            {"suit",5},{"flashlight",6},{"grenade",7},{"handgun",8},{"pistol",8},{"nightvision",9},
            {"laser",10},{"rocket",11},{"rocketlauncher",11},{"shotgun",12},{"flamethrower",13},{"flame",13},{"pulse",14},{"pulselaser",14}};
        static const std::vector<std::pair<const char*, uint8_t>> ENEMYID = {
            {"sgt",0x29},{"sergeant",0x29},{"fh",0x2A},{"formerhuman",0x2A},{"imp",0x2B},{"hydaca",0x65},
            {"revenant",0x66},{"boss1",0x67},{"dog",0x68},{"fhsf",0x69},{"boss3",0x6A},{"boss2",0x6B}};
        auto findItem = [](const std::string& s)->int { std::string n=s; for(auto&c:n)c=(char)tolower((unsigned char)c);
            for (auto& p : ITEMID) if (n == p.first) return p.second; return -1; };
        auto giveItem = [&](int id){ if (id<1||id>=15) return; if (!inv.has(id)) inv.addItem(id);
            inv.owned[id]=true; inv.ammo[id]=ITEMS[id].ammoCap; inv.syncCurrent(); };

        con::registerCmd("give", "give <handgun|shotgun|all|weapons|items|ammo|...> - give weapon/item", [&,findItem,giveItem](const std::vector<std::string>& a){
            std::string what = a.empty() ? "all" : a[0]; for(auto&c:what)c=(char)tolower((unsigned char)c);
            if (what=="all")     { for(int i=1;i<15;++i) giveItem(i); con::log("gave all weapons and items"); }
            else if (what=="weapons"){ for(int i=1;i<15;++i) if(ITEMS[i].weapon) giveItem(i); con::log("gave all weapons"); }
            else if (what=="items")  { for(int i=1;i<15;++i) if(!ITEMS[i].weapon) giveItem(i); con::log("gave all items"); }
            else if (what=="ammo")   { for(int i=1;i<15;++i) if(inv.has(i)) inv.ammo[i]=ITEMS[i].ammoCap; con::log("ammo refilled"); }
            else { int id=findItem(what); if(id>0){ giveItem(id); con::log(std::string("gave: ")+ITEMS[id].name); } else con::log("unknown item: "+what); }
        });
        con::registerCmd("spawn", "spawn <imp|fh|sgt|revenant|dog|hydaca|boss1..3|fhsf> [n] - spawn enemy ahead", [&](const std::vector<std::string>& a){
            if (a.empty()) { con::log("usage: spawn <enemy> [count]"); return; }
            std::string n=a[0]; for(auto&c:n)c=(char)tolower((unsigned char)c);
            uint8_t ct=0; for(auto&p:ENEMYID) if(n==p.first){ ct=p.second; break; }
            if(!ct){ con::log("unknown enemy: "+n); return; }
            int cnt = a.size()>1 ? std::atoi(a[1].c_str()) : 1; if(cnt<1)cnt=1; if(cnt>20)cnt=20;
            for(int i=0;i<cnt;++i){ double off=(i-cnt/2)*0.6;
                spawnEnemyByType(cam.floor, cam.px+cam.dirX*1.8 - cam.dirY*off, cam.py+cam.dirY*1.8 + cam.dirX*off, ct); }
            con::log("spawned "+std::to_string(cnt)+"x "+n);
        });
        con::registerCmd("clearinv","clearinv - clear inventory (back to fists)", [&](const std::vector<std::string>&){ inv.reset(); con::log("inventory cleared"); });
        con::registerCmd("playsnd","playsnd <n> - play PCM/DAC sample #n (find SFX index)", [&](const std::vector<std::string>& a){ if(a.empty()){con::log("PCM samples: "+std::to_string(snd::count()));return;} int n=std::atoi(a[0].c_str()); snd::play(n); con::log("play PCM "+std::to_string(n)+"/"+std::to_string(snd::count())); });
        con::registerCmd("playfm","playfm <patch> [note] - play FM patch as a note (find synth SFX)", [&](const std::vector<std::string>& a){ if(a.empty()){con::log("FM patches: "+std::to_string(snd::patchCount()));return;} int p=std::atoi(a[0].c_str()); int nt=a.size()>1?std::atoi(a[1].c_str()):60; snd::playFm(p,nt); con::log("play FM patch "+std::to_string(p)+" note "+std::to_string(nt)+"/"+std::to_string(snd::patchCount())); });
        con::registerCmd("playsfx","playsfx <id> - play game SFX by ID (ZT table @0xc5eb4; e.g. 0x1b=fire,0x6b=switch)", [&](const std::vector<std::string>& a){ if(a.empty()){con::log("SFX ids: "+std::to_string(snd::sfxCount()));return;} int id=(int)std::strtol(a[0].c_str(),nullptr,0); bool ok=snd::playSfx(id); con::log("SFX 0x"+std::to_string(id)+(ok?" played":" (music/empty)")); });
        con::registerCmd("music", "music <n|off> - play GEMS music sequence (ZT: 0..8) / stop", [&](const std::vector<std::string>& a){
            if(a.empty()){ con::log("songs: "+std::to_string(snd::musicSongCount())+", playing: "+std::to_string(snd::musicCurrent())); return; }
            if(a[0]=="off"||a[0]=="stop"){ snd::musicStop(); con::log("music stopped"); return; }
            int n=std::atoi(a[0].c_str());
            con::log(snd::musicPlay(n) ? ("music "+std::to_string(n)+" playing ("+std::to_string(snd::musicSongCount())+" songs)") : "music: bad index"); });
        con::registerCmd("autmus", "autmus <on|off> - auto-music from game sound ids (type0)", [&](const std::vector<std::string>& a){ if(!a.empty()) snd::musicEnabled()=(a[0]!="off"&&a[0]!="0"); con::log(std::string("auto-music ")+(snd::musicEnabled()?"ON":"OFF")); });
        con::registerCmd("sfxtr","sfxtr <n> - transpose FM-SFX notes by n semitones (tune pitch; e.g. 12/-12)", [&](const std::vector<std::string>& a){ if(!a.empty()) snd::sfxTranspose()=std::atoi(a[0].c_str()); con::log("FM-SFX transpose = "+std::to_string(snd::sfxTranspose())); });
        con::registerCmd("dacremap","dacremap <on|off> - type2 DAC sample = p1-48 (GEMS remap, see ZT_SOUND_SUBSYSTEM.md)", [&](const std::vector<std::string>& a){ if(!a.empty()) snd::dacRemap48()=(a[0]!="off"&&a[0]!="0"); con::log(std::string("DAC -48 remap ")+(snd::dacRemap48()?"ON":"OFF")); });
        con::registerCmd("noiseenv", "noiseenv <hz> - PSG NOISE GEMS envelope tick rate (default 150)", [&](const std::vector<std::string>& a){ if(!a.empty()) snd::noiseEnvHz()=std::atof(a[0].c_str()); con::log("PSG noise env = "+std::to_string(snd::noiseEnvHz())+" Hz"); });
        con::registerCmd("noisepeak","noisepeak <amp> - PSG NOISE peak amplitude (default 1300)", [&](const std::vector<std::string>& a){ if(!a.empty()) snd::noisePeak()=std::atof(a[0].c_str()); con::log("PSG noise peak = "+std::to_string(snd::noisePeak())); });
        con::registerCmd("noiserate","noiserate <n|0> - PSG NOISE LFSR period override in native samples (0=auto from ctrl)", [&](const std::vector<std::string>& a){ if(!a.empty()) snd::noiseRateOv()=std::atoi(a[0].c_str()); con::log("PSG noise rate override = "+std::to_string(snd::noiseRateOv())); });
        con::registerCmd("god",    "god - toggle invulnerability", [&](const std::vector<std::string>&){ player().godmode=!player().godmode; con::log(std::string("god ")+(player().godmode?"ON":"OFF")); });
        con::registerCmd("noclip", "noclip - toggle walk through walls", [&](const std::vector<std::string>&){ noclip=!noclip; con::log(std::string("noclip ")+(noclip?"ON":"OFF")); });
        con::registerCmd("kill",   "kill - kill all enemies on floor", [&](const std::vector<std::string>&){ int n=0; for(auto&e:actors()) if(e.active&&e.think==AT_ENEMY){ e.hp=-9999; e.hitT=1; e.vx=e.vy=0; ++n; } con::log("killed "+std::to_string(n)+" enemies"); });
        con::registerCmd("heal",   "heal [n] - restore health (full by default)", [&](const std::vector<std::string>& a){ int amt=a.empty()?player().maxHp:std::atoi(a[0].c_str()); player().hp=std::min(player().maxHp, player().hp+amt); con::log("HP="+std::to_string(player().hp)); });
        con::registerCmd("freeze", "freeze - toggle enemy freeze", [&](const std::vector<std::string>&){ enemiesFrozen()=!enemiesFrozen(); con::log(std::string("freeze ")+(enemiesFrozen()?"ON":"OFF")); });
        con::registerCmd("pause",  "pause - toggle photo/pause mode (freeze simulation for a clean screenshot)", [&](const std::vector<std::string>&){ paused=!paused; con::log(std::string("pause ")+(paused?"ON":"OFF")); });
        con::registerCmd("blood",  "blood <on|off> - toggle blood spatter (ZT 0x157ca)", [&](const std::vector<std::string>& a){ if(!a.empty()) faBlood()=(a[0]!="off"&&a[0]!="0"); con::log(std::string("blood ")+(faBlood()?"ON":"OFF")); });
        con::registerCmd("transitzt", "transitzt <on|off> - ROM-exact transit render: D4A6 gradient resample + full-strength column pitch (d214)", [&](const std::vector<std::string>& a){ if(!a.empty()) faTransitZT()=(a[0]!="off"&&a[0]!="0"); con::log(std::string("transitzt ")+(faTransitZT()?"ON (ROM-exact D4A6)":"OFF (tuned legacy)")); });
        con::registerCmd("camback", "camback <on|off> - render eye 1/8 cell behind player (ROM b9a6, always-on in game)", [&](const std::vector<std::string>& a){ if(!a.empty()) faCamBack()=(a[0]!="off"&&a[0]!="0"); con::log(std::string("camback ")+(faCamBack()?"ON (ROM-true)":"OFF")); });
        con::registerCmd("elevzt", "elevzt <on|off> - ROM-exact elevator MOVEMENT automaton (b4e4/b768/e420/a3ec: elevState +/-1/2, cabin +/-4/frame, floor swap on +/-0x40, centering)", [&](const std::vector<std::string>& a){ if(!a.empty()) faElevZT()=(a[0]!="off"&&a[0]!="0"); con::log(std::string("elevzt ")+(faElevZT()?"ON (ROM-exact automaton)":"OFF (reconstructed legacy)")); });
        con::registerCmd("stairflip", "stairflip <on|off> - A/B invert stair pitch sign (bug #1: descent looked like ascent)", [&](const std::vector<std::string>& a){ if(!a.empty()) faStairFlip()=(a[0]!="off"&&a[0]!="0"); con::log(std::string("stairflip ")+(faStairFlip()?"ON (pitch inverted)":"OFF (default)")); });
        con::registerCmd("stairfacelr", "stairfacelr <on|off> - fix stair shear direction (align profile L to texU=0 end; swap on flipU faces N/E)", [&](const std::vector<std::string>& a){ if(!a.empty()) faStairFaceLR()=(a[0]!="off"&&a[0]!="0"); con::log(std::string("stairfacelr ")+(faStairFaceLR()?"ON (L->texU=0, fixed)":"OFF (L->geom x0, legacy)")); });
        con::registerCmd("stairfacemap", "stairfacemap <on|off> - remap profile words to correct faces (N/S/W/E per disasm; fixes 'only one texture right')", [&](const std::vector<std::string>& a){ if(!a.empty()) faStairFaceMap()=(a[0]!="off"&&a[0]!="0"); con::log(std::string("stairfacemap ")+(faStairFaceMap()?"ON (remap [0,2,3,1], fixed)":"OFF (identity, legacy scrambled)")); });
        con::registerCmd("stairsteps", "stairsteps <on|off> - stair step segments layer B (riser gradient / blue floor via d5ce) vs floor-cast legacy", [&](const std::vector<std::string>& a){ if(!a.empty()) faStairSteps()=(a[0]!="off"&&a[0]!="0"); con::log(std::string("stairsteps ")+(faStairSteps()?"ON (segments, layer B)":"OFF (floor-cast legacy)")); });
        con::registerCmd("raydda", "raydda <on|off> - faithful per-column supercover-DDA wall render (default ON); off=legacy box-iteration", [&](const std::vector<std::string>& a){ if(!a.empty()) faRayDDA()=(a[0]!="off"&&a[0]!="0"); con::log(std::string("raydda ")+(faRayDDA()?"ON (per-column DDA)":"OFF (box iteration)")); });
        con::registerCmd("transitnogate", "transitnogate <on|off> - stair profile / elevator shaft blue by CELL geometry+distance (no camera-cell gate; fixes 'blocky' pop-in); off=legacy cam-cell gate", [&](const std::vector<std::string>& a){ if(!a.empty()) faTransitNoCamGate()=(a[0]!="off"&&a[0]!="0"); con::log(std::string("transitnogate ")+(faTransitNoCamGate()?"ON (by cell geometry)":"OFF (cam-cell gate)")); });
        con::registerCmd("affinetex", "affinetex <on|off> - affine texU like ROM ccb6 (texture swims on near angled walls); off=perspective-correct", [&](const std::vector<std::string>& a){ if(!a.empty()) faAffineTexU()=(a[0]!="off"&&a[0]!="0"); con::log(std::string("affinetex ")+(faAffineTexU()?"ON (ROM affine)":"OFF (perspective)")); });
        con::registerCmd("angle512", "angle512 <on|off> - quantize camera angle to 512 steps like ROM (LUT 0x8124); off=continuous", [&](const std::vector<std::string>& a){ if(!a.empty()) faAngle512()=(a[0]!="off"&&a[0]!="0"); con::log(std::string("angle512 ")+(faAngle512()?"ON (512-step)":"OFF (continuous)")); });
        con::registerCmd("fixedpoint", "fixedpoint <on|off> - integer/fixed-point wall math like Mega Drive (no FPU); off=double (smooth but not authentic)", [&](const std::vector<std::string>& a){ if(!a.empty()) faFixedPoint()=(a[0]!="off"&&a[0]!="0"); con::log(std::string("fixedpoint ")+(faFixedPoint()?"ON (MD integer)":"OFF (double)")); });
        con::registerCmd("fixedmove", "fixedmove <on|off> - integer movement like Mega Drive (DAB8): angle 0..511, pos 8.8, integer inertia; bit-exact trajectory", [&](const std::vector<std::string>& a){ if(!a.empty()) faFixedMove()=(a[0]!="off"&&a[0]!="0"); cam.angI=-1; con::log(std::string("fixedmove ")+(faFixedMove()?"ON (MD integer)":"OFF (double)")); });
        con::registerCmd("striptexel", "striptexel <on|off> - sample 2 adjacent texels per 2px column via full CLUT like ROM scaler (full horiz resolution); off=1 texel duplicated (blurrier)", [&](const std::vector<std::string>& a){ if(!a.empty()) faStripTexel()=(a[0]!="off"&&a[0]!="0"); con::log(std::string("striptexel ")+(faStripTexel()?"ON (2-texel strip)":"OFF (1 texel)")); });
        con::registerCmd("edgeclamp", "edgeclamp <on|off> - clamp wall face screenX to viewport & compress texture at edges like ROM ccb6 (less texture swim on near grazing walls); off=clip texU (swims)", [&](const std::vector<std::string>& a){ if(!a.empty()) faEdgeClamp()=(a[0]!="off"&&a[0]!="0"); con::log(std::string("edgeclamp ")+(faEdgeClamp()?"ON (ROM compress)":"OFF (clip/swim)")); });
        con::registerCmd("wallband0", "wallband0 <on|off> - walls always use CLUT band 0 like ROM (no per-distance dither; ROM has only 2 CLUT scalers, both band 0); off=distance bands (more dither far)", [&](const std::vector<std::string>& a){ if(!a.empty()) faWallBand0()=(a[0]!="off"&&a[0]!="0"); con::log(std::string("wallband0 ")+(faWallBand0()?"ON (band0, less dither)":"OFF (distance bands)")); });
        con::registerCmd("floor",  "floor <n> - teleport to floor", [&](const std::vector<std::string>& a){ if(a.empty()){con::log("floor="+std::to_string(floor));return;} setFloor(std::atoi(a[0].c_str())); con::log("floor="+std::to_string(floor)); });
        con::registerCmd("ep",     "ep <n> - change episode (1..)", [&](const std::vector<std::string>& a){ if(a.empty())return; setEp(std::atoi(a[0].c_str())-1); con::log("episode="+std::to_string(ep+1)); });
        con::registerCmd("tp",     "tp <x> <y> - teleport to cell", [&](const std::vector<std::string>& a){ if(a.size()<2){con::log("usage: tp x y");return;} cam.px=std::atof(a[0].c_str())+0.5; cam.py=std::atof(a[1].c_str())+0.5; con::log("tp"); });
        con::registerCmd("pos",    "pos - print player position", [&](const std::vector<std::string>&){ char b[96]; std::snprintf(b,sizeof(b),"ep%d floor%d  px=%.2f py=%.2f", ep+1, floor, cam.px, cam.py); con::log(b); });
        con::registerCmd("fighter","fighter <0-4> - select U-RON member (weapon perks)", [&](const std::vector<std::string>& a){ if(a.empty()){con::log("fighter="+std::to_string(playerFighter()));return;} int v=std::atoi(a[0].c_str()); if(v>=0&&v<=4){playerFighter()=v; con::log("fighter="+std::to_string(v));} else con::log("range 0..4"); });
        con::registerCmd("enemies","enemies <on|off> - enemy spawning", [&](const std::vector<std::string>& a){ if(!a.empty()) enemiesOn=(a[0]!="off"&&a[0]!="0"); con::log(std::string("enemies ")+(enemiesOn?"ON":"OFF")); if(!enemiesOn){for(auto&e:actors())if(e.think==AT_ENEMY)e.active=false;} else {clearActors(); aEp=aFloor=-1;} });
        con::log("Zero Tolerance port console - type 'help'. Open/close: backquote key");
    }

    while (running) {
        Uint32 frameStart = SDL_GetTicks();
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) running = false;
            else if (e.type == SDL_TEXTINPUT && con::isOpen()) {  // ВВОД ТЕКСТА в консоль (раскладко-зависимый — правильно для печати)
                con::onText(e.text.text);
            }
            else if (e.type == SDL_KEYDOWN) {
                // ⚠ РАСКЛАДКО-НЕЗАВИСИМО: используем ФИЗИЧЕСКИЙ scancode, НЕ keysym.sym (рус.раскладка ломала клавиши).
                SDL_Scancode key = e.key.keysym.scancode;
                if (con::isOpen()) {                          // КОНСОЛЬ открыта — все клавиши ей (кроме ` / Esc закрывают)
                    if (key == SDL_SCANCODE_GRAVE || key == SDL_SCANCODE_ESCAPE) con::toggle();
                    else con::onKey(key, (SDL_GetModState() & KMOD_SHIFT) != 0);
                    continue;
                }
                if (menu && rebindAction() >= 0) {           // РЕЖИМ ПЕРЕНАЗНАЧЕНИЯ: следующая клавиша → биндинг (ESC/` отменяют)
                    if (key != SDL_SCANCODE_ESCAPE && key != SDL_SCANCODE_GRAVE) { keyBind(rebindAction()) = key; refreshKeyName(rebindAction()); }
                    std::snprintf(menuStatus, sizeof(menuStatus), "%s = %s", gaLabel(rebindAction()), keyBindNames()[rebindAction()].c_str());
                    rebindAction() = -1; dirty = true; continue;   // клавиша поглощена (не идёт в геймплей)
                }
                if (key == SDL_SCANCODE_GRAVE) { con::toggle(); SDL_StartTextInput(); continue; }  // ` — открыть консоль (GZDoom-style)
                if (key == SDL_SCANCODE_ESCAPE) {            // ESC — Sound Test закрыть / иначе меню настроек
                    if (sndtest::open()) { sndtest::open() = false; snd::stopAllSfx(); dirty = true; }
                    else { menu = !menu; menuStatus[0] = 0; rebindAction() = -1; dirty = true; }
                } else if (key == SDL_SCANCODE_F2) {         // F2 — Sound Test меню (сверка звуков на слух)
                    sndtest::open() = !sndtest::open(); if (sndtest::open()) { menu = false; sndtest::init(); } snd::stopAllSfx(); dirty = true;
                } else if (key == SDL_SCANCODE_F11) {        // F11 — фуллскрин (работает и в меню, и в игре)
                    presentFullscreen() = !presentFullscreen(); applyFullscreen(win); dirty = true;
                } else if (sndtest::open()) {                // SOUND TEST — UP/DN выбор id, ENTER играть, LEFT/RIGHT нота
                    switch (key) {
                        case SDL_SCANCODE_UP:        sndtest::moveCursor(-1); dirty = true; break;    // выбор id (БЕЗ авто-play)
                        case SDL_SCANCODE_DOWN:      sndtest::moveCursor(+1); dirty = true; break;
                        case SDL_SCANCODE_PAGEUP:    sndtest::moveCursor(-16); dirty = true; break;
                        case SDL_SCANCODE_PAGEDOWN:  sndtest::moveCursor(+16); dirty = true; break;
                        case SDL_SCANCODE_RETURN: case SDL_SCANCODE_SPACE: sndtest::play(); dirty = true; break;  // играть
                        case SDL_SCANCODE_LEFT:      sndtest::adjNote(-1); sndtest::play(); dirty = true; break;  // нота ниже (+проигрыш)
                        case SDL_SCANCODE_RIGHT:     sndtest::adjNote(+1); sndtest::play(); dirty = true; break;  // нота выше
                        case SDL_SCANCODE_BACKSPACE: case SDL_SCANCODE_S:  snd::stopAllSfx(); dirty = true; break;
                        case SDL_SCANCODE_R:         sndtest::resetNote(); sndtest::play(); dirty = true; break;
                        case SDL_SCANCODE_LEFTBRACKET:  { double h = snd::sfxHold() - 0.02; snd::sfxHold() = h < 0.02 ? 0.02 : h; dirty = true; } break;
                        case SDL_SCANCODE_RIGHTBRACKET: { double h = snd::sfxHold() + 0.02; snd::sfxHold() = h > 2.0  ? 2.0  : h; dirty = true; } break;
                        default: break;
                    }
                } else if (menu) {
                    // в меню реагируем только на ESC (выше) — остальное мышью; ребинд ловится в начале KEYDOWN
                } else if (key == keyBind(GA_JUMP)) {
                    if (mode == 3 && player().jumpY <= 0.0 && player().crouchY >= -1.0) player().jumpVel = 9.0;   // ПРЫЖОК (переназначаемо)
                } else if (key == keyBind(GA_WEAP_PREV)) {
                    if (mode == 3) { cycleWeapon(inv, -1); snd::ev(snd::SFX_SWITCH); }                            // оружие ← (переназначаемо)
                } else if (key == keyBind(GA_WEAP_NEXT)) {
                    if (mode == 3) { cycleWeapon(inv, +1); snd::ev(snd::SFX_SWITCH); }                            // оружие → (переназначаемо)
                } else if (key == keyBind(GA_MAP)) {
                    mapOpen = !mapOpen; dirty = true;                                                             // карта/пауза (переназначаемо)
                } else switch (key) {
                    case SDL_SCANCODE_Q: running = false; break;
                    case SDL_SCANCODE_1: setEp(0); break;
                    case SDL_SCANCODE_2: setEp(1); break;
                    case SDL_SCANCODE_3: setEp(2); break;
                    case SDL_SCANCODE_PAUSE: case SDL_SCANCODE_F1: case SDL_SCANCODE_BACKSLASH:
                        if (mode == 3) paused = !paused; break;   // ФОТОРЕЖИМ: пауза симуляции (кадр застывает, курсор свободен — скринь)
                    case SDL_SCANCODE_G: grid = !grid; dirty = true; break;
                    case SDL_SCANCODE_N: noclip = !noclip; dirty = true; break;        // debug noclip
                    case SDL_SCANCODE_I: player().godmode = !player().godmode; break;   // ЧИТ: бессмертие
                    case SDL_SCANCODE_H: enemiesFrozen() = !enemiesFrozen(); break;     // ЧИТ: враги замерли
                    case SDL_SCANCODE_Y: { double v = enemySpeedScale() - 0.1; enemySpeedScale() = v < 0.2 ? 0.2 : v; spdMsg = 90; } break;  // враги МЕДЛЕННЕЕ
                    case SDL_SCANCODE_U: { double v = enemySpeedScale() + 0.1; enemySpeedScale() = v > 2.5 ? 2.5 : v; spdMsg = 90; } break;  // враги БЫСТРЕЕ
                    case SDL_SCANCODE_T: { double v = faSpriteSizeK() - 0.15; faSpriteSizeK() = v < 0.5 ? 0.5 : v; szMsg = 90; } break;  // спрайты МЕЛЬЧЕ (tiny)
                    case SDL_SCANCODE_B: { double v = faSpriteSizeK() + 0.15; faSpriteSizeK() = v > 3.5 ? 3.5 : v; szMsg = 90; } break;  // спрайты КРУПНЕЕ (bigger)
                    case SDL_SCANCODE_V: if (mode == 3) {     // тест: дать ВСЁ ОРУЖИЕ (не предметы/пикапы), карусель Z/X
                        for (int i = 1; i < 15; ++i) if (ITEMS[i].weapon) { if (!inv.has(i)) inv.addItem(i); inv.ammo[i] = ITEMS[i].ammoCap; }
                        inv.syncCurrent();
                    } break;
                    case SDL_SCANCODE_F: if (!REFERENCE_ONLY) { faithful = !faithful; dirty = true; } break;     // faithful/DDA (ВРЕМЕННО заблок.)
                    case SDL_SCANCODE_R: if (!REFERENCE_ONLY) { reference = !reference; dirty = true; } break;    // референс-режим (ВРЕМЕННО заблок.)
                    case SDL_SCANCODE_MINUS:  case SDL_SCANCODE_KP_MINUS: { double v = faHStretch() - 0.1; faHStretch() = v < 0.5 ? 0.5 : v; dirty = true; } break;
                    case SDL_SCANCODE_EQUALS: case SDL_SCANCODE_KP_PLUS:  { double v = faHStretch() + 0.1; faHStretch() = v > 4.0 ? 4.0 : v; dirty = true; } break;
                    case SDL_SCANCODE_PERIOD: case SDL_SCANCODE_RIGHTBRACKET: setFloor(floor + 1); break;
                    case SDL_SCANCODE_COMMA:  case SDL_SCANCODE_LEFTBRACKET:  setFloor(floor - 1); break;
                    case SDL_SCANCODE_UP:   if (mode != 3) setFloor(floor + 1); break;
                    case SDL_SCANCODE_DOWN: if (mode != 3) setFloor(floor - 1); break;
                    case SDL_SCANCODE_O: moveSpd = moveSpd > 0.015 ? moveSpd - 0.01 : moveSpd; break;
                    case SDL_SCANCODE_P: moveSpd = moveSpd < 0.30  ? moveSpd + 0.01 : moveSpd; break;
                    case SDL_SCANCODE_K: turnSpd = turnSpd > 0.008 ? turnSpd - 0.005 : turnSpd; break;
                    case SDL_SCANCODE_L: turnSpd = turnSpd < 0.15  ? turnSpd + 0.005 : turnSpd; break;
                    case SDL_SCANCODE_SEMICOLON: {  // ';' — печать позиции камеры
                        double ang = std::atan2(cam.dirY, cam.dirX) * 180.0 / 3.14159265;
                        std::printf("POS ep=%d floor=%d px=%.3f py=%.3f ang=%.1f\n", ep + 1, floor, cam.px, cam.py, ang);
                        std::fflush(stdout);
                    } break;
                    default: break;
                }
            }
            else if (e.type == SDL_MOUSEMOTION) {            // ПОВОРОТ МЫШЬЮ (только в шутере; БЕЗ инерции — применяется прямо)
                if (mode == 3 && !menu && !con::isOpen() && !mapOpen && !paused) mouseTurn += e.motion.xrel;
            }
            else if (e.type == SDL_MOUSEWHEEL) {             // КОЛЁСО → выбор оружия (вверх/вниз); кнопки Z/X не трогаем
                if (mode == 3 && !menu && !con::isOpen() && !mapOpen) { cycleWeapon(inv, e.wheel.y > 0 ? -1 : 1); snd::ev(snd::SFX_SWITCH); }
            }
            else if (e.type == SDL_MOUSEBUTTONDOWN && menu && e.button.button == SDL_BUTTON_LEFT) {
                int ww = FBW, wh = FBH; SDL_GetWindowSize(win, &ww, &wh);
                SDL_Rect src, dst; presentRects(ww, wh, true, src, dst);  // меню → весь fb (тот же rect, что и блит)
                int mx = (dst.w > 0) ? (e.button.x - dst.x) * src.w / dst.w + src.x : 0;   // окно → координаты FB
                int my = (dst.h > 0) ? (e.button.y - dst.y) * src.h / dst.h + src.y : 0;
                double frac = 0;
                MenuAction mact = menuHit(mx, my, menuPage, frac);
                if (mact >= MA_KEYBIND) {                     // CONTROLS: клик по кнопке-клавише → ждём нажатие для переназначения
                    rebindAction() = mact - MA_KEYBIND;
                    std::snprintf(menuStatus, sizeof(menuStatus), "Press a key for %s  (Esc=cancel)", gaLabel(rebindAction()));
                    dirty = true;
                } else if (mact >= MA_PRESET && mact < MA_KEYBIND) {   // CONTROLS: выбор пресета управления
                    curPreset() = mact - MA_PRESET; rebindAction() = -1; refreshAllKeyNames();
                    std::snprintf(menuStatus, sizeof(menuStatus), "Preset %d active", curPreset() + 1);
                    dirty = true;
                } else switch (mact) {
                    case MA_MV_DEC: moveSpd = clampd(moveSpd - 0.005, 0.015, 0.30); break;
                    case MA_MV_INC: moveSpd = clampd(moveSpd + 0.005, 0.015, 0.30); break;
                    case MA_MV_BAR: moveSpd = clampd(0.015 + frac * (0.30 - 0.015), 0.015, 0.30); break;
                    case MA_TN_DEC: turnSpd = clampd(turnSpd - 0.002, 0.008, 0.15); break;
                    case MA_TN_INC: turnSpd = clampd(turnSpd + 0.002, 0.008, 0.15); break;
                    case MA_TN_BAR: turnSpd = clampd(0.008 + frac * (0.15 - 0.008), 0.008, 0.15); break;
                    case MA_ST_DEC: faHStretch() = clampd(faHStretch() - 0.1, 0.5, 4.0); break;
                    case MA_ST_INC: faHStretch() = clampd(faHStretch() + 0.1, 0.5, 4.0); break;
                    case MA_ST_BAR: faHStretch() = clampd(0.5 + frac * (4.0 - 0.5), 0.5, 4.0); break;
                    case MA_FL_DEC: if (frameLimit > 5)  --frameLimit; break;
                    case MA_FL_INC: if (frameLimit < 60) ++frameLimit; break;
                    case MA_FL_BAR: frameLimit = (int)(5.5 + frac * (60 - 5)); if (frameLimit<5) frameLimit=5; if (frameLimit>60) frameLimit=60; break;
                    case MA_ES_DEC: enemySpeedScale() = clampd(enemySpeedScale() - 0.1, 0.2, 2.5); break;  // СКОРОСТЬ ВРАГОВ
                    case MA_ES_INC: enemySpeedScale() = clampd(enemySpeedScale() + 0.1, 0.2, 2.5); break;
                    case MA_ES_BAR: enemySpeedScale() = clampd(0.2 + frac * (2.5 - 0.2), 0.2, 2.5); break;
                    case MA_WA_DEC: wallAnimSlow() = clampd(wallAnimSlow() - 0.25, 1.0, 4.0); break;  // АНИМАЦИЯ СТЕН (множитель замедления)
                    case MA_WA_INC: wallAnimSlow() = clampd(wallAnimSlow() + 0.25, 1.0, 4.0); break;
                    case MA_WA_BAR: wallAnimSlow() = clampd(1.0 + frac * (4.0 - 1.0), 1.0, 4.0); break;
                    case MA_SK_DEC: faStairK() = clampd(faStairK() - 0.1, 0.0, 3.0); dirty = true; break;  // ЛЕСТНИЦА: скос
                    case MA_SK_INC: faStairK() = clampd(faStairK() + 0.1, 0.0, 3.0); dirty = true; break;
                    case MA_SK_BAR: faStairK() = clampd(frac * 3.0, 0.0, 3.0); dirty = true; break;
                    case MA_SD_DEC: faStairUni() = clampd(faStairUni() - 0.05, 0.0, 1.0); dirty = true; break;  // ЛЕСТНИЦА: спуск
                    case MA_SD_INC: faStairUni() = clampd(faStairUni() + 0.05, 0.0, 1.0); dirty = true; break;
                    case MA_SD_BAR: faStairUni() = clampd(frac * 1.0, 0.0, 1.0); dirty = true; break;
                    case MA_MS_DEC: mouseSensitivity() = clampd(mouseSensitivity() - 0.1, 0.0, 2.0); break;  // ЧУВСТВИТЕЛЬНОСТЬ МЫШИ
                    case MA_MS_INC: mouseSensitivity() = clampd(mouseSensitivity() + 0.1, 0.0, 2.0); break;
                    case MA_MS_BAR: mouseSensitivity() = clampd(frac * 2.0, 0.0, 2.0); break;
                    case MA_SND:    soundOn() = !soundOn(); break;                              // ЗВУК вкл/выкл
                    case MA_SV_DEC: soundVolume() = clampd(soundVolume() - 0.1, 0.0, 1.0); break;  // ГРОМКОСТЬ
                    case MA_SV_INC: soundVolume() = clampd(soundVolume() + 0.1, 0.0, 1.0); break;
                    case MA_SV_BAR: soundVolume() = clampd(frac, 0.0, 1.0); break;
                    case MA_PAGE:   menuPage = (menuPage + 1) % menu::NPAGE; break;   // следующая страница настроек
                    case MA_REND:   if (!REFERENCE_ONLY) faithful = !faithful; break;   // ВРЕМЕННО заблок. (только reference)
                    case MA_NOCLIP: noclip = !noclip; break;
                    case MA_REFERENCE: if (!REFERENCE_ONLY) reference = !reference; break;   // ВРЕМЕННО заблок. (только reference)
                    case MA_ENEMIES: enemiesOn = !enemiesOn;            // вкл/выкл врагов на уровне (для отладки)
                                     if (!enemiesOn) { for (auto& a : actors()) if (a.think == AT_ENEMY) a.active = false; }  // убрать существующих
                                     else { clearActors(); aEp = aFloor = -1; }   // принудит. респавн врагов этажа
                                     break;
                    case MA_MAP: gameMapMode() = !gameMapMode(); break; // переключить карту (игровая/классическая)
                    case MA_MAPIDS: mapShowIds() = !mapShowIds(); dirty = true; break;  // cell ID на карте
                    case MA_PAUSEFULLMAP: pauseFullMap() = !pauseFullMap(); dirty = true; break;  // TAB: меню паузы ↔ full map
                    case MA_ASPECT: presentAspect() = (presentAspect() + 1) % 3; break;       // 4:3 → 1:1 → stretch
                    case MA_FILTER: presentLinear() = !presentLinear(); break;                // nearest/linear
                    case MA_FULLSCREEN: presentFullscreen() = !presentFullscreen(); applyFullscreen(win); break;
                    case MA_PHYSICS:  playerPhysics() = !playerPhysics();          // ZT-инерция ⇄ свободное движение
                                      cam.turnVel = cam.fwdVel = cam.strafeVel = 0.0; break;
                    case MA_GAMEDIST: gameDistOctagonal() = !gameDistOctagonal(); break;  // октаг. ⇄ Евклид
                    case MA_INVUNLIM: inventoryUnlimited() = !inventoryUnlimited(); inv.unlimited = inventoryUnlimited(); break;  // безлимит инвентаря
                    case MA_RSCALE: presentRenderScale() = presentRenderScale() % 3 + 1;   // 1→2→3→1 (применится с перезапуска)
                                    saveSettings(CFG_PATH, moveSpd, turnSpd, faHStretch(), frameLimit, enemySpeedScale(), faithful, noclip, reference, enemiesOn, gameMapMode());
                                    std::snprintf(menuStatus, sizeof(menuStatus), "Render scale %dx — restart to apply", presentRenderScale()); break;
                    case MA_SAVE:   saveSettings(CFG_PATH, moveSpd, turnSpd, faHStretch(), frameLimit, enemySpeedScale(), faithful, noclip, reference, enemiesOn, gameMapMode());
                                    std::snprintf(menuStatus, sizeof(menuStatus), "Saved to %s", CFG_PATH); break;
                    case MA_QUIT:   running = false; break;
                    case MA_RESUME: menu = false; dirty = true; break;
                    default: break;
                }
            }
        }

        if (con::isOpen()) dirty = true;   // консоль открыта → перерисовывать (видеть ввод/лог) и в статичных режимах
        // ЗАХВАТ МЫШИ для поворота: только в активном шутере (в меню/консоли/карте — обычный курсор для кликов)
        SDL_SetRelativeMouseMode((mode == 3 && !menu && !con::isOpen() && !mapOpen && !sndtest::open() && !paused && mouseSensitivity() > 0.0) ? SDL_TRUE : SDL_FALSE);

        if (sndtest::open()) {
            // SOUND TEST: замороженный кадр сцены + оверлей меню звуков (мир на паузе — блок mode==3 пропущен)
            render(fb, gd, ep, floor, mode, grid, cam, meta, zbuf, faithful, reference, inv);
            sndtest::draw(fb);
            con::draw(fb, FBW, FBH, presentRenderScale());
            SDL_UpdateTexture(tex, nullptr, fb.px.data(), FBW * sizeof(uint32_t));
            SDL_SetWindowTitle(win, "ztpp — SOUND TEST  (UP/DN id · L/R note · ENTER play · BKSP stop · F2/ESC выход)");
        } else if (menu) {
            // меню настроек: рисуем замороженный кадр сцены + оверлей меню (мир на паузе)
            render(fb, gd, ep, floor, mode, grid, cam, meta, zbuf, faithful, reference, inv);
            drawMenu(fb, moveSpd, turnSpd, faHStretch(), frameLimit, enemySpeedScale(), menuPage, faithful, noclip, reference, enemiesOn, gameMapMode(), menuStatus);
            con::draw(fb, FBW, FBH, presentRenderScale());   // оверлей консоли (поверх всего)
            SDL_UpdateTexture(tex, nullptr, fb.px.data(), FBW * sizeof(uint32_t));
            SDL_SetWindowTitle(win, "ztpp — SETTINGS  (ESC закрыть, клик мышью)");
        } else if (mode == 3) {
          if (!paused) {   // ⏸ ФОТОРЕЖИМ: при паузе ПРОПУСКАЕМ всё обновление симуляции (движение/физику/актёров/лифты) —
                           // камера и мир застывают ровно; кадр рисуется ниже как есть (для чистого скрина). Ввод/мышь тоже заморожены.
            // непрерывное движение по зажатым клавишам (скорости настраиваемые: O/P, K/L)
            static const Uint8 ZEROKS[SDL_NUM_SCANCODES] = {0};
            const Uint8* ks = (con::isOpen() || mapOpen) ? ZEROKS : SDL_GetKeyboardState(nullptr);  // консоль/пауза-карта → ввод заморожен
            const bool halveMove = (player().knockPitch < -1) || (player().crouchY < -1.0);  // нокдаун/присед ÷2
            // Движение разрешено всегда: в поездке лифта rcUpdateTransit центрирует кабину в середине
            // перехода, а у этажа отпускает — можно «выскочить» вперёд на промежуточный этаж.
            if (playerPhysics()) {
                // ZT-ФИЗИКА (DAB8): инерция поворота (разгон/выбег), раздельные вперёд/назад, стрейф.
                // Раскладка: W/S(↑/↓) = вперёд/назад · A/D = стрейф влево/вправо · ←/→ = поворот (всё независимо).
                bool fwd = ks[keyBind(GA_FORWARD)], back = ks[keyBind(GA_BACK)];   // переназначаемые клавиши (CONTROLS)
                rcMovePhysics(cam, gd.levels[ep], fwd, back,
                              ks[keyBind(GA_STRAFE_R)], ks[keyBind(GA_STRAFE_L)], // стрейф вправо/влево
                              ks[keyBind(GA_TURN_L)],   ks[keyBind(GA_TURN_R)],   // поворот ←/→
                              /*runMod*/ false, halveMove, noclip);
            } else {
                // «КАРТА БЕЗ ФИЗИКИ»: мгновенная скорость (свободный облёт для навигации/отладки).
                const double mv = moveSpd * (halveMove ? 0.5 : 1.0), rot = turnSpd;
                const double sx = cam.dirY, sy = -cam.dirX;   // «влево» (исправлено направление стрейфа)
                if (ks[keyBind(GA_FORWARD)])  rcMove(cam, gd.levels[ep],  cam.dirX * mv,  cam.dirY * mv, noclip);
                if (ks[keyBind(GA_BACK)])     rcMove(cam, gd.levels[ep], -cam.dirX * mv, -cam.dirY * mv, noclip);
                if (ks[keyBind(GA_STRAFE_R)]) rcMove(cam, gd.levels[ep],  sx * mv,  sy * mv, noclip);
                if (ks[keyBind(GA_STRAFE_L)]) rcMove(cam, gd.levels[ep], -sx * mv, -sy * mv, noclip);
                if (ks[keyBind(GA_TURN_L)])   rcRotate(cam, -rot);
                if (ks[keyBind(GA_TURN_R)])   rcRotate(cam,  rot);
                cam.turnVel = cam.fwdVel = cam.strafeVel = 0.0;   // сброс инерции (чтобы при возврате не «дёрнуло»)
            }
            // ПОВОРОТ МЫШЬЮ — ПРЯМОЙ, БЕЗ ИНЕРЦИИ: сразу на целый МД-угол angI (без camSetAngleU/ре-синка,
            // которые давали лаг = «инерцию»). dir пересчитывается тут же, чтобы кадр отрисовался с новым углом.
            if (mouseTurn != 0.0 && !con::isOpen() && !mapOpen) {
                double da = mouseTurn * mouseSensitivity() * 0.004;              // рад
                if (faFixedMove() && cam.angI >= 0) {
                    cam.angI = (cam.angI + (int)std::lround(da * (512.0 / 6.28318530717958648))) & 0x1FF;
                    int cs = zAngLUT()[cam.angI * 2], sn = zAngLUT()[cam.angI * 2 + 1];   // cos/sin ×256 (LUT 0x8124)
                    cam.dirX = cs / 256.0; cam.dirY = sn / 256.0;
                    cam.planeX = -cam.dirY * 0.66; cam.planeY = cam.dirX * 0.66;
                    cam.ang512 = (double)((cam.angI - 128) & 0x1FF);
                } else { rcRotate(cam, da); cam.ang512 = camDirToAng512(cam); cam.angI = -1; }
            }
            mouseTurn = 0.0;
            // ШАГИ игрока: ZT sfx 0x65/0x66 (FM patch 0x13, ноты 0x28/0x29 — чередование лев/прав) по пройденной дистанции
            if (mode == 3 && !mapOpen && !con::isOpen()) {
                if (!footInit) { footPrevX = cam.px; footPrevY = cam.py; footInit = true; }
                double ddx = cam.px - footPrevX, ddy = cam.py - footPrevY;
                footAccum += std::sqrt(ddx * ddx + ddy * ddy);
                footPrevX = cam.px; footPrevY = cam.py;
                if (footAccum >= 1.2) { footAccum = 0.0; snd::playSfx(footToggle ? 0x66 : 0x65); footToggle ^= 1; }  // шаг каждые ~1.2 кл (замедлено — было слишком часто)
            } else { footPrevX = cam.px; footPrevY = cam.py; footAccum = 0.0; }
            // ОТСКОК от урона (d800: вектор от источника, затухание) — двигаем игрока с коллизией.
            { PlayerState& pl = player();
              if (pl.knockVx != 0.0 || pl.knockVy != 0.0) {
                  rcMove(cam, gd.levels[ep], pl.knockVx, pl.knockVy, noclip);
                  pl.knockVx *= 0.75; pl.knockVy *= 0.75;               // ZT dd46: затухание ×¾/кадр (asr#2: v−=v/4)
                  if (std::fabs(pl.knockVx) < 0.004 && std::fabs(pl.knockVy) < 0.004) pl.knockVx = pl.knockVy = 0.0;
              } }
            // ПРЫЖОК / ПРИСЕД (питч = верт. положение камеры): прыжок — парабола импульса (ZT -0x71e0→-0x71e6,
            // импульс 9 / гравитация 2); присед — питч вниз пока держишь C (ZT вниз+A, -0x71e4=-0x10).
            { PlayerState& pl = player();
              if (pl.jumpVel != 0.0 || pl.jumpY > 0.0) {                  // в полёте: гравитация
                  pl.jumpY += pl.jumpVel; pl.jumpVel -= 2.0;
                  if (pl.jumpY <= 0.0) { pl.jumpY = 0.0; pl.jumpVel = 0.0; }   // приземление
              }
              double cTgt = (ks[keyBind(GA_CROUCH)] && pl.jumpY <= 0.0) ? -16.0 : 0.0;  // присед (держать); не в прыжке
              if (pl.crouchY > cTgt) { pl.crouchY -= 4; if (pl.crouchY < cTgt) pl.crouchY = cTgt; }
              else if (pl.crouchY < cTgt) { pl.crouchY += 4; if (pl.crouchY > cTgt) pl.crouchY = cTgt; } }
            // ОГОНЬ: SPACE или ЛКМ. Стреляет ТЕКУЩИМ оружием (inv.current = carried[sel]); тратит патрон.
            bool mouseFire = !con::isOpen() && !mapOpen && (SDL_GetMouseState(nullptr, nullptr) & SDL_BUTTON(SDL_BUTTON_LEFT));
            bool fireHeld  = (ks[keyBind(GA_FIRE)] != 0) || mouseFire;
            if (fireHeld) {
                int wid = inv.current;
                bool isW    = (wid >= 0 && wid < 15 && ITEMS[wid].weapon);
                bool ammoOk = !isW || inv.ammo[wid] > 0;
                // не-авто оружие (пистолет/дробовик/ПУЛЬС/ракета/граната/кулаки) — semi-auto: выстрел ТОЛЬКО на свежее нажатие.
                // Иначе при удержании inv.fire циклится 1..4→0 и перестреливает каждые 5 кадров → pulse звучит 3× за «тап».
                bool start  = (inv.fire == 0) && (fireAuto(wid) || !prevFireHeld);
                bool autoRe = fireAuto(wid) && inv.fire >= 3;                    // авто-перестрел (лазер/огнемёт/пена — держать)
                if ((start || autoRe) && inv.slide <= 0 && ammoOk) {
                    if (isW) { --inv.ammo[wid];                                  // расход боезапаса
                        if (inv.ammo[wid] == 5 || inv.ammo[wid] == 0) msgs.push(ztmsg::AMMO_LOW); }  // «мало патронов»
                    if (wid < 0) {                                              // КУЛАКИ (ZT -$6fa4): тип удара по D-pad в момент удара
                        inv.punchSide ^= 1;                                     // рука чередуется (лев/прав)
                        bool up = ks[keyBind(GA_FORWARD)];    // «вперёд» зажат → ВЕРХНИЙ удар (+2)
                        bool dn = ks[keyBind(GA_BACK)];       // «назад» зажат → НОГА-кик (−2)
                        inv.punchVariant = up ? 1 : (dn ? 2 : 0);
                    }
                    inv.fire = 1;
                    fireSpawn(inv, gd.levels[ep], cam);
                    switch (wid) {                                              // ЗВУК выстрела по типу оружия
                        case 8:  snd::ev(snd::SFX_HANDGUN); break;
                        case 12: snd::ev(snd::SFX_SHOTGUN); break;
                        case 10: snd::ev(snd::SFX_LASER); break;
                        case 14: snd::ev(snd::SFX_PULSE); pulseSndBurst = 2; pulseSndNext = SDL_GetTicks() + 70; break;  // 1 выстрел = звук 3× (тут 1-й, ещё 2 по таймеру), 1 патрон
                        case 11: snd::ev(snd::SFX_ROCKET); break;
                        case 7:  snd::ev(snd::SFX_GRENADE); break;
                        case 13: if (SDL_GetTicks() >= flameSndNext) { snd::ev(snd::SFX_FLAME); flameSndNext = SDL_GetTicks() + 1000; } break;  // ретриггер ~1/сек (оригинал; было ~20/сек)
                        case 4:  if (inv.fire == 1) snd::ev(snd::SFX_FOAM); break;
                        default: break;
                    }
                    if (isW && inv.ammo[wid] <= 0) inv.dropItem(wid);           // боезапас кончился → оружие выбывает (ZT FUN_10b00)
                }
            }
            // ПУЛЬС: доиграть очередь звука 0x1e (2 повтора после выстрела, ~70мс) — 1 выстрел звучит 3×, патрон 1
            if (pulseSndBurst > 0 && SDL_GetTicks() >= pulseSndNext) { snd::playSfx(0x1e); --pulseSndBurst; pulseSndNext = SDL_GetTicks() + 70; }
            prevFireHeld = fireHeld;                          // edge-детект для semi-auto (следующий кадр)
            // Враги-актёры: ПРОКСИ-СПАВН (ZT 11×11): при смене этажа собираем маркеры, каждый кадр спавним близкие.
            if (ep != aEp || cam.floor != aFloor) {
                clearActors(); if (enemiesOn) collectEnemyMarkers(gd.levels[ep], cam.floor);
                spawnMapFires(gd.levels[ep], cam.floor);     // карта-огонь (celltype 0x18) → вечные AT_FIRE
                collectAlarmCams(gd.levels[ep], cam.floor);  // камеры-тревоги 0x26 этажа (разрушают стены/открывают двери)
                aEp = ep; aFloor = cam.floor;
                prevAlive = 0;
            }
            player().fireImmune = inv.has(5);                 // огнезащ.костюм → иммун. к огню (нужно и для рендера)
            if (!mapOpen) {                                   // ПАУЗА-КАРТА: мир заморожен (не обновляем актёров/спавн/разрушение)
            if (enemiesOn) updateEnemySpawns(gd.levels[ep], cam.floor, cam.px, cam.py);  // спавн врагов рядом с игроком
            updateAlarmCams(gd.levels[ep], cam.floor, cam.px, cam.py);  // камера-тревога: детект→взвод→разрушение стен/дверей 11×11 + будильник
            // БРОНЯ из инвентаря (перед уроном): жилет (3) → пул брони = заряд·10 (поглощает урон, −10%/попадание).
            player().armor      = inv.has(3) ? inv.ammo[3] * 10 : 0;
            updateActors(gd.levels[ep], cam);                 // think всех актёров + очередь спрайтов (worldFx)
            pushCameraBillboards(cam.floor);                  // ЖИВЫЕ камеры-тревоги → билборды (после updateActors: worldFx уже очищен/заполнен)
            if (applyDestruct(gd.levels[ep])) snd::ev(snd::SFX_WALL);   // разрушаемые/секрет-стены + ЗВУК разрушения (0x3b)
            if (inv.has(3)) {                                 // броня израсходовалась в уроне → вернуть в заряд жилета; 0 → жилет выбывает
                inv.ammo[3] = player().armor / 10;
                if (player().armor <= 0) inv.dropItem(3);
            }
            { int alive = aliveEnemies(cam.floor);            // этаж зачищен → ZERO ENEMIES + FLOOR SECURED (и спавн-маркеры кончились)
              if (prevAlive > 0 && alive == 0 && pendingSpawns().empty()) { msgs.push(ztmsg::ZERO_ENEMIES); msgs.push(ztmsg::FLOOR_SECURED); }
              prevAlive = alive; }
            // СМЕРТЬ: HP дошло до 0 → респаун на точке спавна этажа + сброс HP + перезаспавн врагов.
            if (player().dead) {
                resetPlayerHP();
                respawn();                                    // на точку спавна (клетка 0x77) текущего этажа
                clearActors(); if (enemiesOn) collectEnemyMarkers(gd.levels[ep], cam.floor);
                aEp = ep; aFloor = cam.floor; prevAlive = 0;
                deathTimer = frameLimit;                       // показ «WASTED» ~1 сек
            }

            // лифты (авто-поездка с иллюзией питча) и лестницы (наклон по ходьбе + своп на переходе)
            int ccx = (int)cam.px, ccy = (int)cam.py;
            bool entered = (ccx != lastCX || ccy != lastCY);
            int prevElev = cam.elevState;
            rcUpdateTransit(cam, gd.levels[ep], entered);
            if (cam.elevState != 0 && prevElev == 0) snd::playElevatorHum();   // СТАРТ поездки: ОДНА длинная нота 0x6f (гудение по огибающей, не пульс)
            cam.pitch += player().knockPitch + player().jumpY + player().crouchY;  // нокдаун + ПРЫЖОК (вверх) + ПРИСЕД (вниз) — верт. положение камеры (поверх лифт/лестница-питча)
            lastCX = (int)cam.px; lastCY = (int)cam.py;   // после возможной центровки в кабине
            if (cam.floor != floor) floor = cam.floor;    // синхрон заголовка (без respawn)
            if (cam.floor != msgFloor) {                  // смена этажа → STEPPING ONE FLOOR UP/DOWN (ZT: ↑ индекс=вниз)
                msgs.push(cam.floor > msgFloor ? ztmsg::FLOOR_DOWN : ztmsg::FLOOR_UP);
                msgFloor = cam.floor;
            }
            if (rcUpdateDoors(cam.floor, cam.px, cam.py, gd.levels[ep])) snd::ev(snd::SFX_DOOR);  // анимация дверей + ЗВУК открытия (0x6d)
            wallAnim.update();                              // анимация текстур стен (1 игр.кадр)
            ++decorFrame();                                 // анимация декора (вентилятор/мигание ламп)
            { bool wasUnlim = inv.unlimited; inv.unlimited = inventoryUnlimited();   // тумблер «безлимитный инвентарь»
              if (wasUnlim != inv.unlimited) inv.syncCurrent(); }                    // смена режима → кольцо меняет размер, пере-клампить sel
            { static double s_hpx = cam.px, s_hpy = cam.py;  // ФАКТИЧЕСКАЯ скорость игрока (для покачивания ствола ∝ скорости)
              double sp = std::hypot(cam.px - s_hpx, cam.py - s_hpy);
              s_hpx = cam.px; s_hpy = cam.py;
              updateHeld(inv, sp); }                         // анимация выезда/боба(∝скорость)/выстрела ствола

            int got = rcTryPickup(inv, gd.levels[ep], cam.floor, cam.px, cam.py);  // подбор пикапа под игроком
            if (got == PICK_MEDKIT)   { msgs.push(ztmsg::MEDIPACK); con::log("picked up: medipack"); snd::playSfx(0x6a); }   // медпак (+HP) — звук 0x6a (дизасм objdef 011b7c; общий предмет COLLECTED=0x69)
            else if (got >= 0)        { msgs.pushItem(got); con::log(std::string("picked up: ") + ITEMS[got].name); snd::ev(snd::SFX_PICKUP); }  // «<предмет> COLLECTED»
            int dropped = rcTryCorpsePickup(inv, cam);             // подбор оружия с трупа солдата (шаг на труп)
            if (dropped >= 0) { msgs.pushItem(dropped); con::log(std::string("looted: ") + ITEMS[dropped].name); snd::ev(snd::SFX_PICKUP); }  // «<оружие> COLLECTED»
            // предупреждения о здоровье (ZT текст.asm d952<0x32 / d970<0xf): по пересечению порогов 50/15
            { int hp = player().hp; if (hp > 0) {
                if (hp <= 15 && lastHp > 15)      msgs.push(ztmsg::HEALTH_CRITICAL);
                else if (hp <= 50 && lastHp > 50) msgs.push(ztmsg::HEALTH_LOW);
              } lastHp = hp; }   // NB: у попадания в игрока НЕТ своего звука (ориг.) — слышен только звук выстрела врага
            msgs.update();                                // тик очереди HUD-сообщений
            }   // !mapOpen (конец заморозки мира на пауза-карте)
          }   // !paused (⏸ ФОТОРЕЖИМ: конец блока обновления — дальше только отрисовка застывшего кадра)

            // выставить регион игры в fb (для аспект-блита И позиционирования оверлеев в видимой части)
            if (mapOpen) {   // ПАУЗА-КАРТА (TAB): меню паузы (хедер+статус) ИЛИ плоская «full map» (тумблер настроек)
                             g_viewX = 0; g_viewY = 0; g_viewW = FBW; g_viewH = FBH;
                             if (pauseFullMap()) drawFullMap(fb, gd, ep, floor, cam);
                             else                drawPauseMap(fb, gd, ep, floor, cam); }
            else if (reference) { g_viewW = HUD_W * 2; g_viewH = HUD_H * 2; g_viewX = (FBW - g_viewW) / 2; g_viewY = (FBH - g_viewH) / 2;
                             renderReference(fb, gd, gd.levels[ep], gd.wallPal, cam, meta, zbuf, inv); }
            else           { g_viewX = 0; g_viewY = 0; g_viewW = FBW; g_viewH = FBH;
                             renderFPStoFB(fb, gd, gd.levels[ep], gd.wallPal, cam, meta, zbuf, faithful, inv); }
            // ОВЕРЛЕИ — внутри РЕГИОНА ИГРЫ (g_viewX/Y..+W/H); текст ×US (render-scale), иначе мельчает
            const int VX = g_viewX, VY = g_viewY, VW = g_viewW, VH = g_viewH, US = uiScale();
            if (!mapOpen) {                                     // на пауза-карте HUD-оверлеи не нужны
            msgs.draw(fb, VX + 14*US, VY + VH - 188*US, 2*US);   // HUD-сообщения: низ-лево региона (ROM x16/y160), над оружием
            if (deathTimer > 0) { drawTextC(fb, VX + VW / 2, VY + 80*US, "WASTED", 0xFFFF4040u, 4*US); --deathTimer; }  // смерть
            { int cy = VY + 8*US;                            // индикаторы читов (отладка) — верх региона
              if (player().godmode)   { drawText(fb, VX + VW - 130*US, cy, "GOD MODE",  0xFF50FF50u, US); cy += 12*US; }
              if (enemiesFrozen())    { drawText(fb, VX + VW - 130*US, cy, "ENEMIES FROZEN", 0xFF50FF50u, US); cy += 12*US; }
              if (spdMsg > 0) { char sb[40]; std::snprintf(sb, sizeof sb, "ENEMY SPEED %.1f [Y/U]", enemySpeedScale());
                  drawText(fb, VX + VW - 190*US, cy, sb, 0xFFFFD050u, US); cy += 12*US; --spdMsg; }
              if (szMsg > 0) { char sb[48]; std::snprintf(sb, sizeof sb, "SPRITE SIZE %.2f [T/B]", faSpriteSizeK());
                  drawText(fb, VX + VW - 190*US, cy, sb, 0xFFFFD050u, US); cy += 12*US; --szMsg; } }
            if (fullInv) {                                  // индикатор тест-режима «неогранич. инвентарь»
                char vb[80]; int wv = inv.current;
                std::snprintf(vb, sizeof(vb), "FULL INV (%d/%d)  %s", (int)inv.carried.size(),
                              inv.unlimited ? 99 : inv.capacity, wv < 0 ? "FISTS" : ITEMS[wv].name);
                drawTextC(fb, VX + VW / 2, VY + 70*US, vb, 0xFF80E0FFu, 2*US);
                drawTextC(fb, VX + VW / 2, VY + 90*US, "[Z/X] select   [SPACE] fire   [V] exit", 0xFF80C0FFu, US);
            }
            }   // !mapOpen
            con::draw(fb, FBW, FBH, presentRenderScale());   // оверлей консоли (поверх всего)
            SDL_UpdateTexture(tex, nullptr, fb.px.data(), FBW * sizeof(uint32_t));
            char fmode[32];
            if (reference) std::snprintf(fmode, sizeof(fmode), "REFERENCE");
            else if (faithful) std::snprintf(fmode, sizeof(fmode), "faithful x%.1f", faHStretch());
            else          std::snprintf(fmode, sizeof(fmode), "DDA");
            char title[300];
            std::snprintf(title, sizeof(title),
                "%sztpp FPS [%s] эп%d эт%d (%.1f,%.1f)%s  спид:%.2f пов:%.3f  [PAUSE/F1/\\\\=пауза · ESC · Z/X оружие]",
                paused ? "\xE2\x8F\xB8 PAUSED  —  " : "", fmode, ep + 1, floor, cam.px, cam.py, noclip ? " NOCLIP" : "", moveSpd, turnSpd);
            SDL_SetWindowTitle(win, title);
        } else if (dirty) {
            render(fb, gd, ep, floor, mode, grid, cam, meta, zbuf, faithful, reference, inv);
            con::draw(fb, FBW, FBH, presentRenderScale());   // оверлей консоли (поверх всего)
            SDL_UpdateTexture(tex, nullptr, fb.px.data(), FBW * sizeof(uint32_t));
            char title[200];
            std::snprintf(title, sizeof(title),
                "ztpp — эп %d  этаж %d/%d  режим: %s   [ESC меню · 1/2/3 эп · , . этаж · TAB режим · G]",
                ep + 1, floor, Level::FLOORS - 1, modeName[mode]);
            SDL_SetWindowTitle(win, title);
            dirty = false;
        }

        SDL_RenderClear(ren);
        SDL_SetTextureScaleMode(tex, presentLinear() ? SDL_ScaleModeLinear : SDL_ScaleModeNearest);  // фильтр
        { int ww = FBW, wh = FBH; SDL_GetWindowSize(win, &ww, &wh);
          SDL_Rect src, dst; presentRects(ww, wh, menu || sndtest::open(), src, dst);   // аспект-корректный блит (4:3 / 1:1 / stretch); Sound Test — весь FB (не обрезать)
          SDL_RenderCopy(ren, tex, &src, &dst); }
        SDL_RenderPresent(ren);
        // лимит кадров: спим до конца кадрового интервала (1000/frameLimit мс)
        Uint32 target = (Uint32)(1000 / (frameLimit < 5 ? 5 : frameLimit));
        Uint32 spent = SDL_GetTicks() - frameStart;
        if (spent < target) SDL_Delay(target - spent);
    }
    saveSettings(CFG_PATH, moveSpd, turnSpd, faHStretch(), frameLimit, enemySpeedScale(), faithful, noclip, reference, enemiesOn, gameMapMode()); // автосохранение при выходе
    SDL_DestroyTexture(tex);
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
#endif
}
