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
#include "password.hpp"
#include "select_screen.hpp" // ⭐экран выбора бойца (PLANET DEFENSE CORPS) + DECEASED (после ui.hpp/gamedata)
#include "briefing.hpp"      // ⭐заставки/брифинги (скроллящийся текст поверх фона)
#include "password_entry.hpp" // ⭐экран ввода пароля (сетка ROM 58054 + клавиатура)
#include "title_fx.hpp"
#include "version.hpp"      // ⭐версия порта (ZTPP_VERSION из CMake)
#include "app_icon.hpp"      // ⭐иконка окна/Dock из встроенного логотипа (logo_data.hpp)
#include "sega_intro.hpp"    // ⭐SEGA-заставка: вращающийся логотип (5711e, колоночный скейлер + палитро-цикл)
#include "map_icons.hpp"   // значки клеток карты (16×16, из mdgfx/mapres) — рисуются по типу клетки
#include "map_render.hpp"  // ⭐ассеты пауза-карты ZT (кирпичики/палитра CRAM3/маркер-крестик из VRAM+ROM)
#include "radar_render.hpp"// ⭐ассеты РАДАРА кокпита ZT (тайлы-грид 0x347+/палитра линии3/маркер-«+» 0x351 из геймплей-VRAM)
#include "walls.hpp"       // разрушаемые/секрет-стены (применение очереди разрушения к Level)
#include "weapons.hpp"     // инвентарь / подбор / выбор оружия / отрисовка в руках (после FB + ui)
#include "messages.hpp"    // HUD-сообщения ZT (очередь, нижний-левый угол) — после ui + actors
#include "console.hpp"     // внутриигровая консоль (`): команды give/spawn/god/…, история, автодополнение, логи
#include "launcher.hpp"    // стартовое окно выбора ROM (а-ля rednukem): скан+детекция билдов, drag&drop
#include "profile.hpp"     // контур данных на ROM: настройки/сейвы в profiles/<buildKey>/
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
    // ⭐СНАЙПЕР-ОВЕРЛЕЙ (ROM 2404): построение слоя на кадр (живой цикл минует render() — строим здесь)
    { bool city = faSnipers() && activeBg() == &gd.bgCity && gd.bgCity.valid();
      int angRam = cam.angI >= 0 ? cam.angI : (((int)std::lround(camDirToAng512(cam)) + 128) & 0x1FF);
      snip::buildFrame(city, angRam, (int)std::lround(cam.px * 256.0), cam.floor, (int)cam.cabin); }
    if (faithful) renderFaithful(putfn, FBW, FBH, lvl, wallPal, meta, cam, zbuf, envMode);
    else          renderFPS(putfn, FBW, FBH, lvl, wallPal, meta, cam, zbuf, envMode);
    snip::drawHi(putfn, 0, 0, FBW, FBH);        // рикошеты снайпера (pri=1) поверх вида
    drawMinimap(fb, lvl, cam);
    drawHeldWeapon(fb, FBW, FBH, gd, inv);   // оружие в руках (низ-центр)
    drawLaserSight([&](int x, int y, uint32_t c) { fb.put(x, y, c); }, 0, 0, FBW, FBH, inv,
                   laserAimDepth(lvl, cam.floor, cam.px, cam.py, cam.dirX, cam.dirY));   // ⭐лазер: глубина сквозь парапеты/окна (ROM -$1eda)
    drawWeaponHud(fb, inv);                   // имя + боезапас + HP
    // (вспышка — палитрой сцены в main)
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
    // ⭐СНАЙПЕР-ОВЕРЛЕЙ (ROM 2404, гейт ed3e: городской фон): построение спрайт-слоя НА КАДР —
    // здесь (а не в render()), т.к. живой цикл зовёт renderReference напрямую, минуя render().
    { bool city = faSnipers() && activeBg() == &gd.bgCity && gd.bgCity.valid();
      int angRam = cam.angI >= 0 ? cam.angI : (((int)std::lround(camDirToAng512(cam)) + 128) & 0x1FF);
      snip::buildFrame(city, angRam, (int)std::lround(cam.px * 256.0), cam.floor, (int)cam.cabin); }
    // ⭐НАТИВ 128 КОЛОНОК (как ZT: 128 геом-колонн, каждая ×2px = 256 на экране). renderFaithful рендерит
    // стены/дизер/аффинный-texU в 128-широкий nat, затем УДВАИВАЕТ по X до 256 (scX=W/NW=2). Рендер в 256
    // колонок давал ВДВОЕ высокое разрешение → мелкую «шахматку» дизера и «волны» аффин-texU при повороте.
    int M = faIntRes(); if (M < 1) M = 1; if (M > 4) M = 4;
    if (M == 1) {                                                // ⭐ДЕФОЛТ: бит-точный путь (не трогаем)
        renderFaithful([&](int x, int y, uint32_t c) { if (x >= 0 && x < HUD_VW && y >= 0 && y < HUD_VH) view[(size_t)y * HUD_VW + x] = c; },
                       HUD_VW, HUD_VH, lvl, wallPal, meta, cam, zbuf, envMode, HUD_VW / 2, HUD_VH);
    } else {                                                     // ⭐ЭКСПЕРИМЕНТ: суперсэмплинг M× → downsample
        const int HW = HUD_VW * M, HH = HUD_VH * M;
        static std::vector<uint32_t> hires; hires.assign((size_t)HW * HH, 0xFF000000u);
        static std::vector<double> hzbuf;
        renderFaithful([&](int x, int y, uint32_t c) { if (x >= 0 && x < HW && y >= 0 && y < HH) hires[(size_t)y * HW + x] = c; },
                       HW, HH, lvl, wallPal, meta, cam, hzbuf, envMode, (HUD_VW / 2) * M, HH);
        const int nn = M * M;                                    // box-downsample цвета → нативный вид 256×80
        for (int y = 0; y < HUD_VH; ++y)
            for (int x = 0; x < HUD_VW; ++x) {
                long r = 0, g = 0, b = 0;
                for (int sy = 0; sy < M; ++sy) { const uint32_t* row = &hires[(size_t)(y * M + sy) * HW + x * M];
                    for (int sx = 0; sx < M; ++sx) { uint32_t c = row[sx]; r += (c >> 16) & 0xFF; g += (c >> 8) & 0xFF; b += c & 0xFF; } }
                view[(size_t)y * HUD_VW + x] = 0xFF000000u | ((uint32_t)(r / nn) << 16) | ((uint32_t)(g / nn) << 8) | (uint32_t)(b / nn);
            }
        zbuf.assign(HUD_VW, 1e9);                                // downsample глубины (min) для held/лазера
        for (int x = 0; x < HUD_VW; ++x) { double m = 1e9;
            for (int sx = 0; sx < M && x * M + sx < (int)hzbuf.size(); ++sx) { double z = hzbuf[x * M + sx]; if (z < m) m = z; }
            zbuf[x] = m; }
    }
    faHStretch() = savedHs;
    // 2) кадр 320×224 = HUD-кокпит + 3D-вид (256×80) в окно (HUD_VX,HUD_VY) — НАПРЯМУЮ 1:1
    static std::vector<uint32_t> frame; frame.assign((size_t)HUD_W * HUD_H, 0xFF101014u);
    if ((int)gd.hud.size() == HUD_W * HUD_H) std::copy(gd.hud.begin(), gd.hud.end(), frame.begin());
    // ⭐ID-КАРТА БОЙЦА на кокпите (ROM 2acc при загрузке уровня: таблица 0x2CC8[$FF1030] → блиттер 1f72,
    //   nametable 0xC830 = ячейка (24,16) → (192,128), пал.линия 3 = 0x20D2; статический HUD там пуст)
    { int fi = playerFighter();
      if (fi >= 0 && fi < (int)gd.idCards.size() && !gd.idCards[fi].empty() && gd.idcW > 0)
          for (int y = 0; y < gd.idcH && 128 + y < HUD_H; ++y)
              for (int x = 0; x < gd.idcW && 192 + x < HUD_W; ++x) {
                  uint32_t p = gd.idCards[fi][(size_t)y * gd.idcW + x];
                  if (p >> 24) frame[(size_t)(128 + y) * HUD_W + 192 + x] = p;
              } }
    for (int y = 0; y < HUD_VH; ++y)
        for (int x = 0; x < HUD_VW; ++x)
            frame[(size_t)(HUD_VY + y) * HUD_W + (HUD_VX + x)] = view[(size_t)y * HUD_VW + x];

    // ⭐РИКОШЕТЫ СНАЙПЕРА (26da, VDP pri=1) — поверх 3D-вида НА frame (натив 320×224, 1:1), под оружием.
    snip::drawHi([&](int x, int y, uint32_t c) { if (x >= 0 && x < HUD_W && y >= 0 && y < HUD_H) frame[(size_t)y * HUD_W + x] = c; },
                 HUD_VX, HUD_VY, HUD_VW, HUD_VH);

    // (радар рисуем НИЖЕ — прямо на fb в полном разрешении, чтобы значки/cell ID были мельче и чётче,
    //  а не удваивались вместе с кокпитом ×scale)

    // 2c) ОРУЖИЕ В РУКАХ — ПИКСЕЛЬ-В-ПИКСЕЛЬ как ZT: VDP-спрайт в нативных экранных координатах 320×224
    //     (точные SAT-позиции из дизасма), клип к 3D-окну (кокпит режет низ оружия, как в оригинале).
    if (!faWallsOnly() && !faHideHand()) {   // отладка wallsonly/nohand: без оружия/прицела в кадре
    drawHeldNative([&](int x, int y, uint32_t c) { if (x >= 0 && x < HUD_W && y >= 0 && y < HUD_H) frame[(size_t)y * HUD_W + x] = c; },
                   HUD_VX, HUD_VY, HUD_VW, HUD_VH, gd, inv);
    drawLaserSight([&](int x, int y, uint32_t c) { if (x >= 0 && x < HUD_W && y >= 0 && y < HUD_H) frame[(size_t)y * HUD_W + x] = c; },
                   HUD_VX, HUD_VY, HUD_VW, HUD_VH, inv,
                   laserAimDepth(lvl, cam.floor, cam.px, cam.py, cam.dirX, cam.dirY));  // ⭐глубина СКВОЗЬ парапеты/окна (ROM прицел-буфер -$1eda)
    }

    // ⭐НОЧНИК: «вся CRAM зелёная» (ROM 29ee: все 64 цвета из 0x2072) — порт добивает пост-маппингом ОКНА
    // ВИДА в frame ПОСЛЕ оружия/скоб кокпита (drawHeldNative = VDP-спрайты поверх вида, тоже зеленеют).
    // Пиксели уже зелёные (стены палитрой) не трогаем; серые/синие/красные → зелёный грейд по яркости
    // со снапом к DAC-уровням MD. Иконки HUD/кокпит вне окна вида не трогаем (расхождение — BACKLOG).
    if (nvActive()) {
        static const int DAC[8] = { 0, 52, 87, 116, 144, 172, 206, 255 };
        auto snapG = [&](int v) { int best = 0, bd = 1 << 30;
            for (int k = 0; k < 8; ++k) { int d = v - DAC[k]; if (d < 0) d = -d; if (d < bd) { bd = d; best = DAC[k]; } }
            return best; };
        // ⭐ВЕСЬ КАДР (ROM 29ee: ночник красит ВСЕ 64 CRAM-цвета — панели кокпита/иконки тоже зелёные;
        // прежнее окно-вида было упрощением, BACKLOG-остаток закрыт 2026-07-22)
        for (int y = 0; y < HUD_H; ++y)
            for (int x = 0; x < HUD_W; ++x) {
                uint32_t& px = frame[(size_t)y * HUD_W + x];
                int r = (px >> 16) & 0xFF, g = (px >> 8) & 0xFF, b = px & 0xFF;
                if (g > r && g > b) continue;                        // уже зелёный (палитра 0x2072)
                int lum = (r * 3 + g * 6 + b) / 10;
                px = 0xFF000000u | ((uint32_t)snapG(lum) << 8);
            }
    }

    // 2d) HUD ИНВЕНТАРЯ — карусель иконок оружия (5 верхних панелей) + счётчик боезапаса, текущий по центру.
    drawInventoryHud(frame.data(), HUD_W, HUD_H, gd, inv);
    // 2e) HP — ЧИСЛО в ЖЁЛТОМ слоте справа-сверху кокпита (296,72) + СЧЁТЧИК ВРАГОВ в БЕЛОМ слоте слева (8,72).
    drawHpHud(frame.data(), HUD_W, HUD_H, gd);
    // ⭐счётчик врагов = ТЕКУЩИЙ этаж камеры (где игрок): согласовано с тикером «FLOOR SECURED».
    drawEnemyCountHud(frame.data(), HUD_W, HUD_H, gd, cam.floor);
    // 2f) ⭐ВСПЫШКА УРОНА/СМЕРТИ (ROM VBlank 0xB12): $FF1072 → CRAM-слот 63 (линия 3 цвет 15) = ЧЁРНЫЙ ФОН
    // КОКПИТА. В порту вся line3 рисует чёрный как 0xFF000000 (слот 63) → перекрашиваем ЧЁРНЫЕ пиксели ВНЕ
    // 3D-окна (фон панелей, плашки цифр, ЖК-панель, ID-карта — в ROM всё это line3/15). Сцена не трогается.
    if (player().flashCram > 0) {
        uint32_t fc = cramToArgb((uint16_t)(player().flashCram & 0x0EEE));
        for (int y = 0; y < HUD_H; ++y) {
            bool inVyRow = (y >= HUD_VY && y < HUD_VY + HUD_VH);
            for (int x = 0; x < HUD_W; ++x) {
                if (inVyRow && x >= HUD_VX && x < HUD_VX + HUD_VW) continue;   // 3D-окно не красим
                uint32_t& px = frame[(size_t)y * HUD_W + x];
                if ((px & 0x00FFFFFFu) == 0) px = fc;
            }
        }
    }

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
    // ⭐НОЧНИК (ROM 29ee): вся 3D-сцена рисуется ЗЕЛЁНОЙ палитрой 0x2072 вместо 0x20F2. Флаги активных
    // пассивок ставим здесь (ед. источник — и игровой цикл, и headless-дамп): наличие = активность (13182).
    nvActive() = inv.has(9);
    flActive() = inv.has(6) && !inv.has(9);
    // ⭐ВСПЫШКА УРОНА НЕ красит сцену (ROM 0xB12: $FF1072 → CRAM-слот 63 = чёрный фон КОКПИТА; линии сцены
    // не трогаются) — перекраска фона кокпита в renderReference ниже. Прежний палитро-своп сцены был неверен.
    Palette scenePal = nvActive() ? gd.nvPal : gd.wallPal;
    // (снайпер-оверлей строится/рисуется ВНУТРИ renderReference/renderFPStoFB — живой цикл зовёт их напрямую)
    if      (mode == 3 && reference) renderReference(fb, gd, gd.levels[ep], scenePal, cam, meta, zbuf, inv);
    else if (mode == 3) renderFPStoFB(fb, gd, gd.levels[ep], scenePal, cam, meta, zbuf, faithful, inv);
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
// Булев флаг (наличие без значения, может быть последним аргументом).
static bool hasFlag(int argc, char** argv, const char* key) {
    for (int i = 1; i < argc; ++i)
        if (std::strcmp(argv[i], key) == 0) return true;
    return false;
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
    if (hasFlag(argc, argv, "--help") || hasFlag(argc, argv, "-h")) {
        std::printf(
            "ztpp — Zero Tolerance port. Запуск: ztpp <rom.gen> [флаги]\n"
            "\nДЕБАГ (быстрый старт):\n"
            "  --fighter N       начать сразу за бойца N (0..4), пропуск заставок и выбора\n"
            "                    0=Ishii 1=Haile 2=Wolf 3=Ramos 4=Gjoerup (со своим стартовым инвентарём)\n"
            "  --skip-intro      пропустить заставки старта (сразу экран выбора бойца)\n"
            "  --ep N            эпизод (1..)\n"
            "  --floor N         этаж\n"
            "\nHEADLESS-ДАМП:\n"
            "  --dump out.ppm    рендер кадра в файл без окна (+ --brief N / --selscreen N для экранов)\n"
            "\nЛАУНЧЕР (окно выбора ROM, а-ля rednukem):\n"
            "  без явного пути ROM в оконном режиме открывается окно выбора\n"
            "  --nolauncher      пропустить окно выбора (старое поведение: автопоиск ROM)\n"
            "  --launcherdump    (вместе с --dump out.ppm) кадр лаунчера в PPM без окна\n");
        return 0;
    }
    // Headless-кадр лаунчера (ДО загрузки ROM — лаунчеру ROM не нужен).
    if (hasFlag(argc, argv, "--launcherdump")) {
        const char* dp = argStr(argc, argv, "--dump");
        if (!dp) { std::fprintf(stderr, "--launcherdump требует --dump out.ppm\n"); return 2; }
        return launcher::dumpFrame(dp) ? 0 : 1;
    }
    std::string romPath = findRom(argc, argv);
    // Окно выбора ROM: интерактивный запуск без явного пути (headless-режимы не трогаем).
    bool explicitRom = argc > 1 && argv[1][0] != '-';
    bool headless = argStr(argc, argv, "--dump") || argStr(argc, argv, "--dumpmusic") || argStr(argc, argv, "--findct");
    if (!explicitRom && !headless && !hasFlag(argc, argv, "--nolauncher")) {
        romPath = launcher::run(romPath);
        if (romPath.empty()) return 0;   // закрыли лаунчер без выбора = штатный выход
    }
    if (romPath.empty()) {
        std::fprintf(stderr, "ROM не найден. Запуск: ztpp <path-to-rom.gen> [--fighter N] [--skip-intro] [--dump out.ppm]  (--help для справки)\n");
        return 1;
    }
    Rom rom;
    if (!rom.load(romPath)) { std::fprintf(stderr, "Не удалось прочитать ROM: %s\n", romPath.c_str()); return 1; }
    std::printf("ROM: %s (%zu байт)\n", romPath.c_str(), rom.size());

    // ЭТАП 0: вся игровая модель — в GameData; адреса ROM только в loadGameDataFromRom.
    // (Будущее: loadGameDataFromOztd для модов — порт об источнике не знает.)
    GameData gd;
    if (!loadGameDataFromRom(gd, rom))
        std::fprintf(stderr, "ВНИМАНИЕ: ZMAP-сигнатура E1 не совпала — данные могут быть неверны\n");
    std::printf("Билд: %s\n", buildName(gd.build));
    initProfile(gd.build);   // ⭐КОНТУР ДАННЫХ: настройки/сейвы этого билда — в profiles/<key>/
    // ⭐ФОНОВЫЙ СНАЙПЕР эп2 (sniper_overlay.hpp): тайлы 0x16DA58/0x16E058 + таблицы 2510/2600/265E/297A.
    // Адреса релиза ZT — на других билдах не инициализируем (клеток 0x27/0x28 у них нет).
    if (gd.build == Build::ZT) snip::init(rom);

    // РЕНДЕР-МАСШТАБ (до создания FB/окна): внутренний fb = 640×RS. Из ini (раннее чтение) + override --rscale.
    presentRenderScale() = loadRenderScaleEarly(profilePath("ztpp_settings.ini").c_str());
    { int rs = argInt(argc, argv, "--rscale", presentRenderScale()); presentRenderScale() = rs < 1 ? 1 : (rs > 3 ? 3 : rs); }
    FBW = FBH = 640 * presentRenderScale();
    pickupHiddenFn() = pickupIsConsumed;   // рендер пикапов: скрывать подобранные клетки (хук в raycaster)
    g_uiFont = gd.font.have ? &gd.font : nullptr;   // настоящий ZT-шрифт для drawText (иначе public-domain)
    g_uiFontBig = gd.fontBig.have ? &gd.fontBig : nullptr;  // Font_grph 8×16 для меню/настроек/заголовков
    g_uiOptPal   = gd.pauseTextPal.c.data();               // ⭐палитра текста экрана ОПЦИЙ (CRAM line3 0x1CDAE) для ESC-меню
    g_uiOptArrow = gd.optArrowHave ? gd.optArrow : nullptr; // ⭐стрелка-курсор ОПЦИЙ (иначе фолбэк-треугольник)
    g_pwCursor   = gd.pwCursorHave ? gd.pwCursor : nullptr; // ⭐курсор-скобка экрана пароля (реальная графика ROM)

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
    int  pauseViewFloor = 0;                         // ⭐листаемый этаж пауза-карты (ROM 2cec: бит0/бит1, не выше текущего)
    uint32_t visitedFloors = 0;                      // битмаска посещённых этажей уровня (вынесено из тика для сейвов)
    bool paused  = false;                            // ФОТОРЕЖИМ: пауза симуляции (кадр застывает для скрина; PAUSE/F1/\\ или консоль `pause`)
    bool prevFireHeld = false;                        // прошлый кадр: SPACE/ЛКМ зажаты (для edge-триггера semi-auto оружия)
    bool fireBlockUntilRelease = false;               // ⭐после меню/паузы/карты: НЕ стрелять, пока кнопку огня не отпустят (клик по CONTINUE не должен палить)
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
    int floorSecT = 0;              // тикер зачищенного этажа (ROM -$58d4: 0=не зачищен; 0xE1-цикл напоминаний)
    // ⭐СТЕК ПАРОЛЕЙ (ROM -$58C4, 9 симв × слот; индекс -$58D6 = pwProgressFloor()): пароль этажа N
    // ФИКСИРУЕТСЯ ОДИН РАЗ при смене этажа с зачищенным прогресс-этажом (b8fc→57ec4, снапшот состояния
    // НА ТОТ МОМЕНТ) — карта листает сохранённые строки, НЕ перегенеривает. Старт с пароля floor N →
    // слоты 0..N-1 = «*SECURED*» (ROM 0x1094). Эпизод-пароль (ROM -$58CE, статус ep<<4|0xF) — отдельный
    // буфер, строка НАД этажом 0 с именем ПРЕДЫДУЩЕГО эпизода (57b5a: «SPACE STATION/HIGH RISE»).
    std::vector<std::string> pwStack;
    std::string pwEpisodePass;
    int deathTimer = 0;             // показ «WASTED» после смерти (кадры)
    // ⭐ЭКРАН ВЫБОРА БОЙЦА (ZT @0xC6362): старт → выбор → игра; смерть → длинная вспышка → возврат сюда с DECEASED.
    SelectState selState;           // курсор + флаги живых/мёртвых (ZT $FF1030 + $FF28EC)
    bool selectActive = false;      // экран выбора активен (вместо игры)
    // ⭐СТАРТОВЫЙ ФЛОУ (ROM 0x976): копирайт Technopop (fade) → 22-кадровая TECHNOPOP-заставка → интро-брифинг.
    int      bootPhase = -1;         // -1=нет; 0=SEGA (вращающийся логотип 5711e); 1=копирайт; 2=ACCOLADE; 3=technopop-анимация; 4=титул
    int      bootFrame = 0;          // текущий кадр technopop (0..21)
    Uint32   bootPhaseMs = 0;        // старт фазы (реальное время, независимо от fps)
    bool     bootThenBrief = false;  // после boot → интро-брифинг (как обычный старт)
    bool     titleMenuOn = false;    // ⭐титул: меню START/OPTIONS показано (после первой кнопки)
    int      titleSel = 1;           // выбранный пункт: 1=START 2=OPTIONS (ROM -$778e)
    bool gameOver = false;          // все бойцы мертвы → GAME OVER
    int  deathAnim = 0;             // таймер анимации смерти (ZT 30 кадров -$721e; длинная вспышка $FF1072=0xFFF)
    bool selPending = false;        // после смерти: ждём выбора нового бойца, потом продолжить уровень (не сброс на спавн)
    int  selFade = 0;               // fade-in экрана выбора (0=чёрное → 16=видно; ZT 1f85e плавное появление)
    int  selFadeOut = 0;            // fade-out при подтверждении выбора (16=видно → 0=чёрное, потом игра)
    // ⭐ЗАСТАВКИ/БРИФИНГИ (ZT @0xCB1E4): очередь показа между эпизодами. briefQueue = список idx (BriefId) для показа подряд.
    BriefingState briefState;       // активная заставка (idx<0 = неактивна)
    std::vector<int> briefQueue;    // очередь СЛЕДУЮЩИХ заставок к показу
    int pendingEpisode = -1;        // ⭐после очереди заставок перейти на эпизод N (конец эпизода, ROM 1a40)
    // ⭐МУЗЫКА ЗАСТАВОК — СЕССИОННАЯ (юзер/ROM -$58f8): ОДИН общий трек на ВСЮ сессию заставок (сколько бы
    // экранов ни было в очереди), БЕЗ рестартов между экранами. Играется только при СТАРТЕ сессии.
    // Треки: интро-сессия=id0x00 (caade), межэпизодные=0x03, victory=0x89, bad-end=0x01/0x02 (предп.).
    auto briefMusic = [&](int idx) {
        int sid = 0x03;                                    // общий трек текст-заставок (вкл. интро; id0 = тема эп0, НЕ интро)
        if (idx == BR_VICTORY)      sid = 0x89;
        else if (idx == BR_BADEND1 || idx == BR_BADEND2 || idx == BR_EXPLODE || idx == BR_EXIT)
            sid = 0x87;                                    // game-over/эпилог (caf1a; юзер: 0x01/0x02 неверны)
        snd::playSfx(sid);
    };
    int  briefFade = 0;             // fade заставки (0=чёрное → 16=видно)
    bool briefThenSelect = false;   // после очереди заставок → экран выбора бойца (старт игры: intro→выбор)
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
    // ⭐СТАРТОВЫЙ ИНВЕНТАРЬ бойца (ZT @0xF98): сброс + 2 пары {id,count}. VEST(id3)→броня. RAMOS(3)=кулаки.
    auto applyFighterStart = [&](int f) {
        inv.reset(); fullInv = false; player().armor = 0;
        if (f < 0 || f >= 5) { inv.syncCurrent(); return; }
        for (int i = 0; i < 2; ++i) {
            int id = gd.startInv[f][i * 2], cnt = gd.startInv[f][i * 2 + 1] >> 8;   // count 8.8 → целое
            if (id >= 1 && id <= 14) { inv.addItem(id); inv.ammo[id] = cnt;
                if (id == 3) { player().armor = cnt * 10; if (player().armor > 100) player().armor = 100; } }
        }
        inv.syncCurrent();
        if (argStr(argc, argv, "--actordbg")) { std::fprintf(stderr, "FIGHTER %d start-inv:", f);
            for (int id = 1; id <= 14; ++id) if (inv.has(id)) std::fprintf(stderr, " id%d(x%d)", id, inv.ammo[id]);
            std::fprintf(stderr, " armor=%d current=%d\n", player().armor, inv.current); }
    };
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
    if (argStr(argc, argv, "--intres")) faIntRes() = std::atoi(argStr(argc, argv, "--intres")); // эксперимент: внутр.разрешение вида
    if (argStr(argc, argv, "--edgeclamp")) faEdgeClamp() = (std::atoi(argStr(argc, argv, "--edgeclamp")) != 0); // тест клампа края грани
    if (argStr(argc, argv, "--wallsonly")) faWallsOnly() = (std::atoi(argStr(argc, argv, "--wallsonly")) != 0); // отладка: только стены на сплошном фоне
    if (argStr(argc, argv, "--nohand")) faHideHand() = (std::atoi(argStr(argc, argv, "--nohand")) != 0); // отладка: скрыть оружие в руках
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
    if (argStr(argc, argv, "--snipers")) faSnipers() = (std::atoi(argStr(argc, argv, "--snipers")) != 0); // фоновый снайпер (вся система)
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
    if (argStr(argc, argv, "--seggate"))   faSegGate()   = (std::atoi(argStr(argc, argv, "--seggate")) != 0);   // A/B: канонич. ребро+направленный гейт транзит-сегментов (ROM 988a-9a4a)
    if (argStr(argc, argv, "--shaftprof")) faShaftProf() = (std::atoi(argStr(argc, argv, "--shaftprof")) != 0); // A/B: профиль шахты 0x30 = cabin|1 (ROM 9862) vs жёсткий pshift=0
    if (argStr(argc, argv, "--blued28e")) faBlueD28E() = (std::atoi(argStr(argc, argv, "--blued28e")) != 0); // A/B: синий-задник по ROM d28e (односторонний при |pshift|≥0x28) vs всегда оба
    if (argStr(argc, argv, "--fxproj")) fxProj() = (std::atoi(argStr(argc, argv, "--fxproj")) != 0); // A/B: fixed-point проекция стен DDA (этап 1, МД ca4c: D0=divs.w, тексель 8.8) vs double
    if (argStr(argc, argv, "--fxzquant")) fxZQuant() = (std::atoi(argStr(argc, argv, "--fxzquant")) != 0); // A/B: этап 2 — квант. глубина (D0-сетка) + спрайт-z-тест целыми скейлами (ef20) vs BIAS 0.40
    if (argStr(argc, argv, "--fxfan")) fxFan() = (std::atoi(argStr(argc, argv, "--fxfan")) != 0); // A/B: этап 3 — веер лучей ROM $9124 (512-квант углы) vs непрерывный dirX/planeX
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
    // --- Отладка: скан НЕОБРАБОТАННЫХ celltype по всем эпизодам/этажам (--ctscan) ---
    // Помечает celltype, которые НЕ рисуются ничем в порту (не стена/декор/пикап/враг/дверь/лифт/пусто) —
    // кандидаты в «декор-трупы/остатки», которые игра ставит на карту как окружение. Юзер: трупы/остатки = декор.
    if (argStr(argc, argv, "--ctscan")) {
        const bool full = argStr(argc, argv, "--ctfull") != nullptr;
        for (int e = 0; e < gd.episodes(); ++e) {
            const Level& L = gd.levels[e];
            int hist[256] = {0};
            for (int f = 0; f < Level::FLOORS; ++f)
                for (int y = 0; y < Level::H; ++y)
                    for (int x = 0; x < Level::W; ++x) {
                        uint8_t ct = L.cellType(f, x, y); DecorDef dd;
                        bool handled = (ct == 0) || cellRenderWall(ct) || cellRendersDoor(ct) ||
                                       decorForCt(ct, dd) || itemBillboardForCt(ct, dd) || (enemyGfxSlot(ct) >= 0) ||
                                       ctElevUp(ct) || ctElevDown(ct) || ctElevUpdn(ct) || (ct == 0x18);
                        if (full ? (ct != 0) : !handled) hist[ct]++;
                    }
            bool any = false; for (int c = 0; c < 256; ++c) if (hist[c]) any = true;
            if (!any) continue;
            std::printf("=== эп%d: celltype ===\n", e + 1);
            for (int c = 0; c < 256; ++c) if (hist[c]) {
                uint8_t ct = (uint8_t)c; DecorDef dd;
                const char* cls = cellRenderWall(ct) ? "WALL/DOOR" : decorForCt(ct, dd) ? "decor" :
                                  itemBillboardForCt(ct, dd) ? "pickup" : (enemyGfxSlot(ct) >= 0) ? "enemy" :
                                  (ct == 0x18) ? "map-fire" : (ctElevUp(ct)||ctElevDown(ct)||ctElevUpdn(ct)) ? "elev" : "??? НИЧЕГО";
                std::printf("  0x%02X : %4d клеток | maptype=%d icon=%d | %s\n", c, hist[c],
                            cellTable().maptype[ct], cellIcon(ct), cls);
            }
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
    if (const char* mw = argStr(argc, argv, "--dumpmusic")) {   // отладка: "song:secs:out.wav" (headless, без аудио)
        int song = 0; double secs = 10; char path[512] = "music.wav";
        std::sscanf(mw, "%d:%lf:%511s", &song, &secs, path);
        snd::load(rom, gd.sndSamp, gd.sndPatch, gd.sndSeq, gd.sndSfx);
        return snd::renderMusicWav(song, secs, path) ? 0 : 1;
    }
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
        if (const char* fg = argStr(argc, argv, "--fighter")) { int v = std::atoi(fg); if (v >= 0 && v <= 4) { playerFighter() = v; applyFighterStart(v); } }  // боец 0..4 (перки + стартовый инвентарь; в дампе экрана выбора нет)
        if (const char* fr = argStr(argc, argv, "--fire")) inv.fire = std::atoi(fr);  // отладка: кадр выстрела 1..4
        if (const char* pv = argStr(argc, argv, "--punchvar"))  inv.punchVariant = std::atoi(pv);  // отладка: тип удара 0/1/2
        if (const char* ps = argStr(argc, argv, "--punchside")) inv.punchSide = std::atoi(ps);     // отладка: рука 0/1
        if (const char* hp = argStr(argc, argv, "--hp")) player().hp = std::atoi(hp);  // отладка: HP игрока (полоска)
        if (const char* fl = argStr(argc, argv, "--testflash")) player().flashCram = (int)std::strtol(fl, nullptr, 0);  // отладка: CRAM-слово вспышки (8..15 урон, 0xFFF смерть)
        if (argStr(argc, argv, "--mapids")) mapShowIds() = true;        // карта: показать cell ID (для дампа)
        if (argStr(argc, argv, "--god")) player().godmode = true;       // отладка: бессмертие в дампе
        if (argStr(argc, argv, "--freeze")) enemiesFrozen() = true;     // отладка: враги замерли в дампе
        if (mode == 3 && enemiesOn) spawnEnemiesFromLevel(gd.levels[ep], floor);   // враги этажа (для дампа/отладки)
        if (mode == 3) spawnMapFires(gd.levels[ep], floor);                        // карта-огонь 0x18 (для дампа)
        if (const char* sa = argStr(argc, argv, "--spawnahead")) {                 // отладка: враг ПЕРЕД камерой (имя или hex celltype)
            static const std::pair<const char*, uint8_t> EN[] = {{"sgt",0x29},{"fh",0x2A},{"imp",0x2B},{"hydaca",0x65},
                {"revenant",0x66},{"boss1",0x67},{"dog",0x68},{"fhsf",0x69},{"boss3",0x6A},{"boss2",0x6B}};
            uint8_t ct = 0; std::string n = sa; for (auto& c : n) c = (char)tolower((unsigned char)c);
            for (auto& p : EN) if (n == p.first) { ct = p.second; break; }
            if (!ct) ct = (uint8_t)std::strtol(sa, nullptr, 0);
            double d = argStr(argc, argv, "--spawndist") ? std::atof(argStr(argc, argv, "--spawndist")) : 2.5;
            if (ct) spawnEnemyByType(cam.floor, cam.px + cam.dirX * d, cam.py + cam.dirY * d, ct);
        }
        for (int t = 0, n = argInt(argc, argv, "--simticks", 0); t < n; ++t) updateActors(gd.levels[ep], cam);  // отладка: прокрутить N игровых тиков
        if (int kt = argInt(argc, argv, "--killspawned", -1); kt >= 0) {   // отладка: убить ПОСЛЕДНЕГО заспавненного врага + прокрутить N тиков
            for (int i = (int)actors().size() - 1; i >= 0; --i) if (actors()[i].active && actors()[i].think == AT_ENEMY) {
                Actor& a = actors()[i]; hitEnemy(a, a.hp + 1, cam.px, cam.py, 0x400); break; }
            for (int t = 0; t < kt; ++t) updateActors(gd.levels[ep], cam);
            for (auto& a : actors()) if (a.active && (a.think == AT_ENEMY || a.think == AT_CORPSE) && a.srcType >= 0x60)
                std::printf("killspawned: think=%d hitT=%d hp=%d state=%d @(%.1f,%.1f)\n", a.think, a.hitT, a.hp, a.state, a.x, a.y);
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
        if (argStr(argc, argv, "--hydtest")) {   // отладка: 3 Hydaca в ряд, выстрел в ближнюю, 120 тиков — лог смертей
            inv.addItem(8); inv.ammo[8] = 99; inv.syncCurrent();
            double bx = cam.px + cam.dirX * 2.0, by = cam.py + cam.dirY * 2.0;
            spawnEnemyByType(cam.floor, bx, by, 0x65);
            spawnEnemyByType(cam.floor, bx + cam.dirX * 1.2, by + cam.dirY * 1.2, 0x65);
            spawnEnemyByType(cam.floor, bx + cam.dirX * 2.4, by + cam.dirY * 2.4, 0x65);
            { double hz = argInt(argc, argv, "--hydceil", 0) ? 1.0 : 0.0;   // --hydceil 1 → все на потолке
              for (auto& a : actors()) if (a.active && a.think == AT_ENEMY) { a.z = hz; a.variant = hz > 0.5 ? 1 : 0; a.state = 1; a.aimX = a.x; a.aimY = a.y; } }
            auto dumpHyd = [&](const char* tag) {
                std::printf("[%s]", tag);
                int i = 0;
                for (auto& a : actors()) { if (!a.active) { ++i; continue; }
                    if (a.think == AT_ENEMY && a.srcType == 0x65)
                        std::printf(" E#%d(hp=%d st=%d z=%.2f vz=%.3f pos=%.1f,%.1f)", i, a.hp, a.state, a.z, a.vz, a.x, a.y);
                    if (a.think == AT_CORPSE && a.srcType == 0x65)
                        std::printf(" C#%d(pos=%.1f,%.1f)", i, a.x, a.y);
                    ++i; }
                std::printf("\n"); };
            dumpHyd("spawn");
            fireSpawn(inv, gd.levels[ep], cam);
            dumpHyd("shot");
            for (int t = 0; t < 120; ++t) { updateActors(gd.levels[ep], cam);
                if (t == 0 || t == 5 || t == 20 || t == 60 || t == 119) { char b[8]; std::snprintf(b, 8, "t%d", t); dumpHyd(b); } }
            return 0;
        }
        if (argStr(argc, argv, "--shoot")) fireSpawn(inv, gd.levels[ep], cam);        // отладка: выстрел
        for (int i = 0, n = argInt(argc, argv, "--shoot", 0); i < n; ++i) updateActors(gd.levels[ep], cam);
        if (mode == 3) { collectAlarmCams(gd.levels[ep], floor); updateActors(gd.levels[ep], cam); applyDestruct(gd.levels[ep]); pushCameraBillboards(floor); }  // 1 тик: спрайты + разрушение + камеры (для дампа)
        if (int nt = argInt(argc, argv, "--snipticks", 0)) {   // отладка: прокрутить N тиков снайпера (спавн+фазы), как если стоим на 0x27/0x28
            int angRam = ((int)std::lround(camDirToAng512(cam)) + 128) & 0x1FF;
            for (int i = 0; i < nt; ++i) snip::tick(true, angRam);
            std::printf("snip: active=%d phase=0x%02X lives=%d win=0x%02X ric=%d\n",
                        (int)snip::st().active, snip::st().phase, snip::st().lives, snip::st().window, (int)snip::st().ricOk);
        }
        if (argStr(argc, argv, "--probe")) { int cx = (int)(cam.px + cam.dirX * 0.7), cy = (int)(cam.py + cam.dirY * 0.7);
            std::printf("probe (%d,%d): cellId %d celltype 0x%02X\n", cx, cy, gd.levels[ep].cellId(floor, cx, cy), gd.levels[ep].cellType(floor, cx, cy)); }
        if (argStr(argc, argv, "--actordbg")) std::fprintf(stderr, "CAM @(%.2f,%.2f) dir(%.2f,%.2f) floor=%d HP=%d\n", cam.px, cam.py, cam.dirX, cam.dirY, cam.floor, player().hp);
        if (argStr(argc, argv, "--actordbg")) for (auto& a : actors()) if (a.active)
            std::fprintf(stderr, "actor think=%d state=%d ct=0x%02X hp=%d burned=%d timer=%d @(%.2f,%.2f)\n",
                a.think, a.state, a.srcType, a.hp, a.burned, a.timer, a.x, a.y);
        if (const char* pa = argStr(argc, argv, "--postang")) {   // отладка: развернуть камеру ПОСЛЕ симуляции (навестись на труп сзади)
            double a = std::atof(pa) * 3.14159265 / 180.0; cam.dirX = std::cos(a); cam.dirY = std::sin(a); }
        if (const char* px = argStr(argc, argv, "--postpx")) cam.px = std::atof(px);
        if (const char* py = argStr(argc, argv, "--postpy")) cam.py = std::atof(py);
        if (const char* di = argStr(argc, argv, "--dumpidx")) faDumpIdxPath() = di;  // почисленный дамп нат-индексов стен
        render(fb, gd, ep, floor, mode, grid, cam, meta, zbuf, faithful, reference, inv);
        if (const char* ss = argStr(argc, argv, "--selscreen")) {  // отладка: дамп экрана выбора «N[:dead[:slideT[:dir]]]»
            SelectState dss; dss.cursor = std::atoi(ss); const char* d = std::strchr(ss, ':');
            if (d && std::atoi(d + 1)) dss.dead[dss.cursor] = true;   // ":1" → DECEASED
            if (d) { const char* d2 = std::strchr(d + 1, ':');       // ":T:dir" → форс слайд-перехода (для проверки анимации)
                if (d2) { dss.slideT = std::atoi(d2 + 1); const char* d3 = std::strchr(d2 + 1, ':');
                          dss.slideDir = d3 ? std::atoi(d3 + 1) : 1; dss.slidePrev = (dss.cursor - dss.slideDir + 5) % 5; } }
            drawSelectScreen(fb, gd, dss);
        }
        if (const char* br = argStr(argc, argv, "--brief")) {  // отладка: дамп заставки «N[:scroll]»
            BriefingState bs; bs.idx = std::atoi(br); const char* d = std::strchr(br, ':');
            if (d) bs.scroll = std::atof(d + 1);
            drawBriefing(fb, gd, bs, 2);
        }
        if (const char* sd = argStr(argc, argv, "--segadump")) {    // отладка: дамп SEGA-заставки --segadump <кадр>
            fb.clear(0xFF000000u);
            double sc = (double)FBW / 320.0; { double sy = (double)FBH / 224.0; if (sy < sc) sc = sy; }
            int vpW = (int)(320 * sc), vpH = (int)(224 * sc), vpX = (FBW - vpW) / 2, vpY = (FBH - vpH) / 2;
            std::vector<uint32_t> sfr(320 * 224, 0xFF000000u);
            segai::renderSegaFrame(gd, std::atoi(sd), sfr.data());
            for (int y = 0; y < vpH; ++y) { int ny = (int)(y / sc); if (ny > 223) ny = 223;
                for (int x = 0; x < vpW; ++x) { int nx = (int)(x / sc); if (nx > 319) nx = 319;
                    fb.put(vpX + x, vpY + y, sfr[(size_t)ny * 320 + nx]); } }
        }
        if (const char* td = argStr(argc, argv, "--titledump")) {   // отладка: дамп титула --titledump <кадр 0..687> (полный рендер)
            fb.clear(0xFF000000u);
            double sc = (double)FBW / 320.0; { double sy = (double)FBH / 224.0; if (sy < sc) sc = sy; }
            int vpW = (int)(320 * sc), vpH = (int)(224 * sc), vpX = (FBW - vpW) / 2, vpY = (FBH - vpH) / 2;
            std::vector<uint32_t> tf(320 * 224);
            ttl::renderTitleFrame(gd, std::atoi(td), 1, tf.data());
            for (int y = 0; y < vpH; ++y) { int ny = (int)(y / sc); if (ny > 223) ny = 223;
                for (int x = 0; x < vpW; ++x) { int nx = (int)(x / sc); if (nx > 319) nx = 319;
                    fb.put(vpX + x, vpY + y, tf[(size_t)ny * 320 + nx]); } }
        }
        if (const char* bd = argStr(argc, argv, "--bootdump")) {   // отладка: дамп заставки (0=копирайт, 1..22=technopop, 100+N=accolade, 200+N=лого мода)
            fb.clear(0xFF000000u);
            int fi = std::atoi(bd);
            const std::vector<uint32_t>* img = (fi >= 200) ? (fi - 200 < (int)gd.logoFrames.size() ? &gd.logoFrames[fi - 200] : nullptr)
                : (fi >= 100) ? (fi - 100 < (int)gd.accoladeFrames.size() ? &gd.accoladeFrames[fi - 100] : nullptr)
                : (fi == 0) ? &gd.copyrightScreen
                : (fi - 1 < (int)gd.introFrames.size() ? &gd.introFrames[fi - 1] : nullptr);
            if (img && !img->empty()) {
                double sc = (double)FBW / 320.0; { double sy = (double)FBH / 224.0; if (sy < sc) sc = sy; }
                int vpW = (int)(320 * sc), vpH = (int)(224 * sc), vpX = (FBW - vpW) / 2, vpY = (FBH - vpH) / 2;
                for (int y = 0; y < vpH; ++y) { int ny = (int)(y / sc); if (ny > 223) ny = 223;
                    const uint32_t* src = &(*img)[(size_t)ny * 320];
                    for (int x = 0; x < vpW; ++x) { int nx = (int)(x / sc); if (nx > 319) nx = 319; fb.put(vpX + x, vpY + y, src[nx]); } }
            }
        }
        if (argStr(argc, argv, "--pause")) {                                    // отладка: дамп меню паузы (Tab)
            std::vector<std::string> pls;
            for (int f = (floor > 1 ? floor - 2 : 0); f <= floor && (int)pls.size() < 3; ++f) {
                int gi = ep * 16 + f;
                std::string lbl = (gi < (int)gd.pauseNames.size() && !gd.pauseNames[gi].empty())
                                    ? gd.pauseNames[gi] : "LEVEL ? : ";
                if (f == floor) pls.push_back(lbl + "*NOT SECURED*");
                else { ztpass::PwState pst; pst.status = ep * 16 + f; pst.hp = 99;
                       pls.push_back(lbl + ztpass::encode(pst)); }
            }
            drawPauseMap(fb, gd, ep, floor, cam, &pls);
        }
        if (const char* tm = argStr(argc, argv, "--testmsg")) {   // отладка: HUD-сообщение в дампе
            HudMessages dmsg;
            int id = std::atoi(tm); if (id > 0) dmsg.pushItem(id);
            if (const char* ef = argStr(argc, argv, "--testfloor")) {   // «ENTERING X»: --testfloor "ep:floor" (0-based)
                int te = 0, tf = 0; std::sscanf(ef, "%d:%d", &te, &tf);
                const char* nm = ztmsg::enteringFloor(te, tf);
                dmsg.push(nm ? nm : ztmsg::FLOOR_DOWN);
            }
            dmsg.push(ztmsg::FLOOR_SECURED); dmsg.push(ztmsg::AMMO_LOW);
            dmsg.showFrames = 999; dmsg.update();
            if (reference) { const int msc = (HUD_W * 2) / HUD_W;             // как игровой цикл: ЖК-панель кокпита
                             const int vx0 = (FBW - HUD_W * 2) / 2, vy0 = (FBH - HUD_H * 2) / 2;
                             dmsg.draw(fb, vx0 + 16*msc, vy0 + 160*msc, msc, gd.heldPal.c[7]); }
            else dmsg.draw(fb, 14, FBH - 188, 2, gd.heldPal.c[7]);
        }
        if (argStr(argc, argv, "--menu")) { // отладка: наложить меню настроек (значение = номер страницы 0..NPAGE-1)
            initKeyBinds();                 // биндинги/имена (в --dump путь до обычной инициализации не доходит)
            int mp = std::atoi(argStr(argc, argv, "--menu"));   // -1=корень; 0..3=OPTIONS стр.; 10+N=DEBUG стр.N
            if (mp < 0)       { menuMode() = 0; mp = 0; }
            else if (mp >= 10){ menuMode() = 2; mp -= 10; if (mp >= menu::NPAGE_DBG) mp = 0; }
            else              { menuMode() = 1; if (mp >= menu::NPAGE_OPT) mp = 0; }
            menuPreGame() = (menuMode() == 1);   // дамп: показать ENTER PASSWORD на OPTIONS-страницах
            drawMenu(fb, 0.07, 0.04, faHStretch(), 60, 1.0, mp, faithful, noclip, reference, true, true, "Saved to ztpp_settings.ini");
        }
        if (argStr(argc, argv, "--pwdump")) {   // отладка: экран ввода пароля (значение = префилл буфера, опц.)
            pwscr::st().open();
            const char* pre = argStr(argc, argv, "--pwdump");
            if (pre && pre[0] && pre[0] != '-') pwscr::st().buf = pre;
            pwscr::st().cx = 3; pwscr::st().cy = 2;   // курсор на 'U' для наглядности
            if ((int)pwscr::st().buf.size() == pwscr::PW_LEN) {   // полный пароль → сообщение принят/ошибка
                ztpass::PwState pst;
                if (ztpass::decode(pwscr::st().buf.c_str(), pst)) pwscr::st().ok(); else pwscr::st().fail();
            }
            pwscr::render(fb, gd.selFont);
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
    snd::load(rom, gd.sndSamp, gd.sndPatch, gd.sndSeq, gd.sndSfx);   // PCM/GEMS-банки билда + открытие аудио
    // РАЗМЕР ОКНА НЕЗАВИСИМ от внутреннего fb (640×RS): окно — нормального размера по дисплею, fb рендерится
    // внутри и ужимается SDL в окно (при RS>1 = суперсэмплинг → cell ID/оверлеи мельче и чётче). Не привязывать к FBW!
    int winSide = 720;
    { SDL_DisplayMode dm; if (SDL_GetDesktopDisplayMode(0, &dm) == 0) {
        winSide = std::min(dm.w, dm.h) - 80; if (winSide < 480) winSide = 480; if (winSide > 1000) winSide = 1000; } }
    char wtitle[64]; std::snprintf(wtitle, sizeof wtitle, "ztpp v%s — Zero Tolerance", ztppVersion());
    SDL_Window* win = SDL_CreateWindow(wtitle,
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, winSide, winSide, SDL_WINDOW_RESIZABLE);
    // ⭐macOS press-and-hold фикс: SDL2 стартует с АКТИВНЫМ text input → удержание буквы вызывало
    // системный попап альтернативных символов (юзер). Гасим; Start — только консоль/ввод пароля.
    SDL_StopTextInput();
    ztppApplyWindowIcon(win);                      // ⭐иконка ZTPP (logo_data.hpp): окно (Win/Linux) + Dock (macOS)
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
    static std::string cfgPathStr = profilePath("ztpp_settings.ini");   // ⭐контур данных: per-ROM профиль
    const char* CFG_PATH = cfgPathStr.c_str();
    { double st = faHStretch(); bool gm = gameMapMode();
      if (loadSettings(CFG_PATH, moveSpd, turnSpd, st, frameLimit, enemySpeedScale(), faithful, noclip, reference, enemiesOn, gm)) {
          faHStretch() = st; gameMapMode() = gm;
          if (frameLimit < 5) frameLimit = 5; if (frameLimit > 60) frameLimit = 60;
          // ⭐МИГРАЦИЯ 2026-07-21: 0.8 в ini был ПРИБЛИЖЕНИЕМ ROM-хода 3/4; теперь 0.75 применяется точно
          // (ROM_WALK34 в enemyMove) → сохранённые 0.8 читаем как 1.0, иначе враги замедлятся вдвойне.
          if (std::fabs(enemySpeedScale() - 0.8) < 1e-6) enemySpeedScale() = 1.0;
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
    msgs.showFrames = 20;                        // ⭐ROM -0x76f0=0x14=20 сим-тиков (фикс-timestep: тик=ROM-тик)
    msgs.fps = (int)(simBaseFps() + 0.5);        // масштаб озвучки (dur в 1/60с → сим-тики) = частота симуляции
    snd::musicEnabled() = musicOn();            // тумблер Music из настроек (ini) → звуковая подсистема
    bool menu = false;                          // меню настроек открыто (ESC)
    // (ввод пароля вынесен в отдельный экран pwscr — сетка ROM 58054 + клавиатура)
    int menuPage = 0;                            // страница меню настроек (0..NPAGE-1)
    char menuStatus[64] = "";
    auto clampd = [](double v, double lo, double hi) { return v < lo ? lo : (v > hi ? hi : v); };

    auto setFloor = [&](int nf) {
        if (nf < 0 || nf >= Level::FLOORS) return;
        floor = nf; dirty = true;
        if (mode == 3) respawn();
    };
    auto setEp = [&](int ne) { if (ne < 0 || ne >= gd.episodes()) return; ep = ne; snd::musicStop(); activeBg() = gd.bgForEpisode(ep); dirty = true; if (mode == 3) respawn(); wallAnim.init(gd.levels[ep], meta); inv.reset(); fullInv = false; resetPlayerHP(); rcResetPickups(); clearActors(); clearWallState(); aEp = aFloor = -1; };  // новый эпизод → инвентарь/HP/актёры/стены с нуля
    auto setMode = [&](int nm) { mode = nm; dirty = true; if (mode == 3) respawn(); };

    // ── ⭐ПАРОЛИ (ROM 58720/58a74, password.hpp — бит-в-бит): построение из текущего состояния + применение ──
    // status: −1 = ep·16+текущий этаж; иначе явный (ep<<4|floor; floor 0xF = «эпизод пройден», ROM 5801a)
    auto buildPasswordFor = [&](int status) -> std::string {
        ztpass::PwState st;
        for (int i = 0; i < 5; ++i) st.alive[i] = !selState.dead[i];
        for (int i = 0; i < 5; ++i) { st.weapons[i] = 0; st.ammoN[i] = 0; }
        for (int i = 0; i < 5 && i < (int)inv.carried.size(); ++i) {
            st.weapons[i] = inv.carried[i];
            st.ammoN[i]   = inv.ammo[inv.carried[i]];
        }
        st.hp = player().hp * 99 / (player().maxHp > 0 ? player().maxHp : 100);
        st.status = (status >= 0) ? status : ep * 16 + cam.floor;
        return ztpass::encode(st);
    };
    auto buildPassword = [&]() -> std::string { return buildPasswordFor(-1); };
    auto applyPassword = [&](const ztpass::PwState& st) {
        for (int i = 0; i < 5; ++i) selState.dead[i] = !st.alive[i];
        // ⭐ROM 0x1062/0x107c: эпизод=(status>>4)&3, этаж=status&0xf — НАПРЯМУЮ (без ++nep; Boxing!!!=0x1f → эп1/эт15).
        int nep = (st.status >> 4) & 3, nfl = st.status & 15;
        if (nep >= gd.episodes()) nep = gd.episodes() - 1;
        if (nfl >= Level::FLOORS) nfl = Level::FLOORS - 1;
        // ⭐стек паролей (ROM 0x1094): слоты 0..N−1 = заглушки «*SECURED*», прогресс = N, эпизод-буфер пуст
        pwStack.assign((size_t)nfl, "*SECURED*");
        pwProgressFloor() = nfl; pwEpisodePass.clear();
        floor = nfl; cam.floor = nfl; msgFloor = nfl;
        setEp(nep);                                             // сброс инвентаря/актёров/стен
        floor = nfl; cam.floor = nfl; setFloor(nfl);            // respawn на этаж пароля
        inv.reset();
        if (!st.cheatFull)                                      // ⭐чит-слово = только варп, снаряжение бойца (не 5-ствольный лут)
            for (int i = 0; i < 5; ++i) if (st.weapons[i] > 0 && st.weapons[i] < 15) {
                inv.addItem(st.weapons[i]);
                inv.ammo[st.weapons[i]] = st.ammoN[i];
            }
        inv.syncCurrent();
        player().hp = st.hp * player().maxHp / 99; if (player().hp < 1) player().hp = 1;
        aEp = aFloor = -1; dirty = true;
    };
    // ⭐ПАРОЛЬ, ОЖИДАЮЩИЙ СТАРТА (ROM 0xfc0: буфер пароля декодируется при СТАРТЕ уровня, не сразу): применяется
    //   на переходе выбор→игра (ep/floor + снаряжение). Иначе START = обычная новая игра (этаж 0, инвентарь бойца).
    bool startPwPending = false; ztpass::PwState startPwState;
    // Обработчик действия экрана пароля (общий для клавиатуры и мыши).
    auto handlePwAct = [&](pwscr::Act a) {
        auto& ps = pwscr::st();
        if (a == pwscr::A_TYPED || a == pwscr::A_DELETED) snd::playSfx(0x69);
        else if (a == pwscr::A_CANCEL) { ps.close(); SDL_StopTextInput(); }
        else if (a == pwscr::A_SUBMIT) {
            ztpass::PwState pst;
            if (ztpass::decode(ps.buf.c_str(), pst)) {   // ⭐валиден: НЕ стартуем игру (ROM: возврат в опции),
                startPwState = pst; startPwPending = true;  //  прогресс применится при СТАРТЕ уровня (ROM 0xfc0)
                int nep = (pst.status >> 4) & 3, nfl = pst.status & 15;
                std::snprintf(menuStatus, sizeof menuStatus, "PASSWORD OK - EP %d FLOOR %d (press START)", nep + 1, nfl);
                ps.ok(); snd::playSfx(0x69);              // зелёное «PASSWORD ACCEPTED», экран закроется сам
            } else { ps.fail(); snd::playSfx(0x6b); }    // «INCORRECT PASSWORD», буфер СОХРАНЯЕТСЯ
        }
    };

    // ── ⭐СЕЙВЫ DOOM-STYLE: ПОЛНОЕ состояние мира (грид уровня с разрушениями, актёры: враги/трупы/огни,
    // дорманты, двери, пикапы, камеры, потушенные огни, visited/floorSec) + слоты 0..5 (0 = quick, F5/F9). ──
    auto slotPath = [](int slot) { static char p[300];
        char nm[32];
        if (slot == 0) std::snprintf(nm, sizeof nm, "ztpp_quick.sav"); else std::snprintf(nm, sizeof nm, "ztpp_save%d.sav", slot);
        std::snprintf(p, sizeof p, "%s", profilePath(nm).c_str());   // ⭐контур данных: сейвы в профиле билда
        return (const char*)p; };
    auto saveGameSlot = [&](int slot) -> bool {
        std::ofstream f(slotPath(slot)); if (!f) return false;
        const char* fn = (playerFighter() >= 0 && playerFighter() < (int)gd.fighters.size())
                            ? gd.fighters[playerFighter()].name.c_str() : "?";
        f << "ztppsave=2\n";
        f << "desc=EP " << (ep + 1) << " FLOOR " << (cam.floor + 1) << "  " << fn << "  HP " << player().hp << "\n";
        f << "ep=" << ep << "\nfloor=" << cam.floor << "\nfighter=" << playerFighter()
          << "\nhp=" << player().hp << "\narmor=" << player().armor
          << "\npx=" << cam.px << "\npy=" << cam.py << "\ndirx=" << cam.dirX << "\ndiry=" << cam.dirY
          << "\nsel=" << inv.sel << "\nvisited=" << visitedFloors << "\nfloorsec=" << floorSecT << "\n";
        for (int i = 0; i < 5; ++i) f << "dead" << i << "=" << (selState.dead[i] ? 1 : 0) << "\n";
        for (int i = 0; i < (int)inv.carried.size() && i < 5; ++i)
            f << "slot" << i << "=" << inv.carried[i] << "\nammo" << i << "=" << inv.ammo[inv.carried[i]] << "\n";
        // грид уровня (cellId; cellType — производный) — фиксирует разрушенные стены/открытые навсегда двери
        static const char* HEX = "0123456789abcdef";
        for (int fl = 0; fl < Level::FLOORS; ++fl) {
            f << "grid" << fl << "=";
            for (int y = 0; y < Level::H; ++y) for (int x = 0; x < Level::W; ++x) {
                uint8_t c = gd.levels[ep].cellId(fl, x, y); f << HEX[c >> 4] << HEX[c & 15]; }
            f << "\n";
        }
        f << "pwprog=" << pwProgressFloor() << "\n";                    // ⭐стек паролей (ROM -$58C4/-$58CE/-$58D6)
        if (!pwEpisodePass.empty()) f << "pwep=" << pwEpisodePass << "\n";
        for (auto& s : pwStack)         f << "pwstk=" << s << "\n";
        for (auto& kv : doorMap())      f << "door=" << kv.first << ":" << kv.second << "\n";
        for (int k : pickedSet())       f << "picked=" << k << "\n";
        for (int k : fireExtinguished()) f << "fireout=" << k << "\n";
        for (auto& m : pendingSpawns()) f << "pend=" << m.x << ":" << m.y << ":" << m.floor << "\n";
        for (auto& c : alarmCams())     f << "cam=" << c.x << ":" << c.y << ":" << c.floor << ":" << c.state << ":" << c.timer << ":" << (c.dead?1:0) << "\n";
        for (auto& a : actors()) {      // враги/трупы/огни (transient снаряды/fx не сохраняем)
            if (!a.active || (a.think != AT_ENEMY && a.think != AT_CORPSE && a.think != AT_FIRE)) continue;
            f << "actor=" << a.think << ":" << a.x << ":" << a.y << ":" << a.floor << ":" << a.state << ":" << a.timer
              << ":" << a.hp << ":" << (int)a.variant << ":" << (int)a.srcType << ":" << a.drop << ":" << (a.burned?1:0)
              << ":" << a.aimX << ":" << a.aimY << ":" << a.homeX << ":" << a.homeY << ":" << (a.revived?1:0)
              << ":" << (int)a.tile << ":" << a.frameT << "\n";
        }
        return true;
    };
    auto loadGameSlot = [&](int slot) -> bool {
        std::ifstream f(slotPath(slot)); if (!f) return false;
        int lep=0, lfl=0, lfg=0, lhp=100, larm=0, lsel=0, lsec=0; uint32_t lvis=0;
        double lpx=-1, lpy=-1, ldx=1, ldy=0;
        int slots[5] = {0,0,0,0,0}, ammo[5] = {0,0,0,0,0}; bool ldead[5] = {false,false,false,false,false};
        std::vector<std::string> grids(Level::FLOORS);
        std::vector<std::pair<int,double>> doors; std::vector<int> picked, fireout;
        std::vector<PendingSpawn> pend; std::vector<AlarmCam> cams; std::vector<Actor> acts;
        std::vector<std::string> lpwstk; std::string lpwep; int lpwprog = 0;
        std::string line; bool ok = false;
        while (std::getline(f, line)) {
            size_t eq = line.find('='); if (eq == std::string::npos) continue;
            std::string k = line.substr(0, eq); const char* v = line.c_str() + eq + 1;
            if      (k == "ztppsave") ok = true;
            else if (k == "ep") lep = std::atoi(v);      else if (k == "floor") lfl = std::atoi(v);
            else if (k == "fighter") lfg = std::atoi(v); else if (k == "hp") lhp = std::atoi(v);
            else if (k == "armor") larm = std::atoi(v);  else if (k == "sel") lsel = std::atoi(v);
            else if (k == "visited") lvis = (uint32_t)std::strtoul(v, nullptr, 10);
            else if (k == "floorsec") lsec = std::atoi(v);
            else if (k == "px") lpx = std::atof(v);      else if (k == "py") lpy = std::atof(v);
            else if (k == "dirx") ldx = std::atof(v);    else if (k == "diry") ldy = std::atof(v);
            else if (k.size() == 5 && !k.compare(0, 4, "dead")) { int i = k[4]-'0'; if (i>=0&&i<5) ldead[i] = std::atoi(v) != 0; }
            else if (k.size() == 5 && !k.compare(0, 4, "slot")) { int i = k[4]-'0'; if (i>=0&&i<5) slots[i] = std::atoi(v); }
            else if (k.size() == 5 && !k.compare(0, 4, "ammo")) { int i = k[4]-'0'; if (i>=0&&i<5) ammo[i] = std::atoi(v); }
            else if (!k.compare(0, 4, "grid")) { int fl = std::atoi(k.c_str() + 4); if (fl >= 0 && fl < Level::FLOORS) grids[fl] = v; }
            else if (k == "door")   { int dk; double dv; if (std::sscanf(v, "%d:%lf", &dk, &dv) == 2) doors.push_back({dk, dv}); }
            else if (k == "picked") picked.push_back(std::atoi(v));
            else if (k == "fireout") fireout.push_back(std::atoi(v));
            else if (k == "pwprog") lpwprog = std::atoi(v);
            else if (k == "pwep")   lpwep = v;
            else if (k == "pwstk")  lpwstk.push_back(v);
            else if (k == "pend")   { PendingSpawn m{}; if (std::sscanf(v, "%d:%d:%d", &m.x, &m.y, &m.floor) == 3) pend.push_back(m); }
            else if (k == "cam")    { AlarmCam c{}; int d=0; if (std::sscanf(v, "%d:%d:%d:%d:%d:%d", &c.x,&c.y,&c.floor,&c.state,&c.timer,&d) == 6) { c.dead = d != 0; cams.push_back(c); } }
            else if (k == "actor")  { Actor a{}; int var=0, st=0, bu=0, rv=0, tl=0;
                if (std::sscanf(v, "%d:%lf:%lf:%d:%d:%d:%d:%d:%d:%d:%d:%lf:%lf:%lf:%lf:%d:%d:%d",
                                &a.think,&a.x,&a.y,&a.floor,&a.state,&a.timer,&a.hp,&var,&st,&a.drop,&bu,
                                &a.aimX,&a.aimY,&a.homeX,&a.homeY,&rv,&tl,&a.frameT) == 18) {
                    a.variant=(uint8_t)var; a.srcType=(uint8_t)st; a.burned=bu!=0; a.revived=rv!=0; a.tile=(uint8_t)tl;
                    a.active = true; acts.push_back(a); } }
        }
        if (!ok || lep < 0 || lep >= gd.episodes() || lfl < 0 || lfl >= Level::FLOORS) return false;
        for (int i = 0; i < 5; ++i) selState.dead[i] = ldead[i];
        playerFighter() = lfg; selState.cursor = lfg;
        floor = lfl; cam.floor = lfl; msgFloor = lfl; pauseViewFloor = lfl;
        setEp(lep); floor = lfl; cam.floor = lfl;                     // сброс контейнеров + базовая инициализация
        for (int fl = 0; fl < Level::FLOORS; ++fl)                    // грид (разрушения/двери-навсегда)
            if ((int)grids[fl].size() >= Level::W * Level::H * 2)
                for (int y = 0; y < Level::H; ++y) for (int x = 0; x < Level::W; ++x) {
                    const char* h = grids[fl].c_str() + (y * Level::W + x) * 2;
                    auto hv = [](char c){ return c <= '9' ? c - '0' : (c | 32) - 'a' + 10; };
                    gd.levels[lep].setCell(fl, x, y, (uint8_t)((hv(h[0]) << 4) | hv(h[1])));
                }
        clearActors(); clearFloorState(); alarmCams().clear();
        doorMap().clear();  for (auto& kv : doors) doorMap()[kv.first] = kv.second;
        pickedSet().clear(); for (int k2 : picked) pickedSet().insert(k2);
        fireExtinguished().clear(); for (int k2 : fireout) fireExtinguished().insert(k2);
        pendingSpawns() = pend; alarmCams() = cams;
        for (const Actor& a : acts) { Actor& na = allocActor(); na = a; }
        visitedFloors = lvis; floorSecT = lsec; prevAlive = -1;
        pwStack = lpwstk; pwEpisodePass = lpwep;                      // ⭐стек паролей из сейва
        pwProgressFloor() = (lpwprog >= 0 && lpwprog < 16) ? lpwprog : (int)pwStack.size();
        aEp = lep; aFloor = lfl;                                      // мир восстановлен — НЕ пересобирать маркеры
        inv.reset();
        for (int i = 0; i < 5; ++i) if (slots[i] > 0 && slots[i] < 15) { inv.addItem(slots[i]); inv.ammo[slots[i]] = ammo[i]; }
        inv.sel = lsel < (int)inv.carried.size() ? lsel : 0; inv.syncCurrent();
        player().hp = lhp > 0 ? lhp : 1; player().armor = larm;
        if (lpx >= 0 && lpy >= 0) { cam.px = lpx; cam.py = lpy;
            double L = std::hypot(ldx, ldy); if (L > 1e-6) { cam.dirX = ldx/L; cam.dirY = ldy/L;
                cam.planeX = -cam.dirY * 0.66; cam.planeY = cam.dirX * 0.66; } }
        dirty = true;
        return true;
    };

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
        con::registerCmd("save", "save [slot 0-5] - save game (0=quick)", [&,saveGameSlot](const std::vector<std::string>& a){ int sl = a.empty()?0:std::atoi(a[0].c_str()); con::log(saveGameSlot(sl) ? "saved slot "+std::to_string(sl) : "save failed"); });
        con::registerCmd("load", "load [slot 0-5] - load game (0=quick)", [&,loadGameSlot](const std::vector<std::string>& a){ int sl = a.empty()?0:std::atoi(a[0].c_str()); con::log(loadGameSlot(sl) ? "loaded slot "+std::to_string(sl) : "load failed"); });
        con::registerCmd("password", "password [code] - show current password / apply a 9-char password (ROM 58720/58a74)", [&,applyPassword,buildPassword](const std::vector<std::string>& a){
            if (a.empty()) { con::log("PASSWORD: " + buildPassword()); return; }
            ztpass::PwState st;
            if (!ztpass::decode(a[0].c_str(), st)) { con::log("invalid password"); return; }
            applyPassword(st); con::log("password accepted: ep " + std::to_string(((st.status>>4)&3)+1) + " floor " + std::to_string(st.status&15)); });
        con::registerCmd("die", "die - kill the player (triggers death flash -> DECEASED -> select/bad end)", [&](const std::vector<std::string>&){
            player().godmode = false; player().hp = 0; player().dead = true; con::log("player killed"); });
        con::registerCmd("badend", "badend - trigger bad-end cutscene flow for current episode (as if all fighters died)", [&](const std::vector<std::string>&){
            if (gd.briefings.empty()) { con::log("no briefings"); return; }
            if (ep == 0)      { briefState.idx = BR_BADEND1; briefQueue = {}; }
            else if (ep == 1) { briefState.idx = BR_BADEND2; briefQueue = { BR_EXPLODE }; }
            else              { briefState.idx = BR_EXIT;    briefQueue = { BR_EXPLODE }; }
            briefState.scroll = 0; briefFade = 0; briefThenSelect = true; pendingEpisode = 0;   // ⭐рестарт с начала
            briefMusic(briefState.idx); con::log("bad end (ep " + std::to_string(ep + 1) + ")"); });
        con::registerCmd("killall", "killall - kill all enemies on current floor (corpses+drops, clears dormant markers; triggers ZERO ENEMIES ticker)", [&](const std::vector<std::string>&){
            int n = 0;
            for (auto& a : actors()) if (a.active && a.think == AT_ENEMY && a.floor == cam.floor) {
                int dr = enemyWeaponDrop(a.srcType);                                    // как смерть в бою (actors.cpp)
                if (dr == -2) dr = (enemyRng() & 0x10) ? 10 : 7;                        // Sgt/FH-SF: laser/grenade
                spawnCorpse(a.x, a.y, a.floor, a.srcType, 0, 0, a.variant, dr, a.burned);
                if (a.srcType == 0x67 || a.srcType == 0x6A || a.srcType == 0x6B) episodeEndT() = 15;   // босс → конец эпизода (как ROM)
                a.active = false; ++n;
            }
            int m = 0;                                                                   // дормантные маркеры ТЕКУЩЕГО этажа
            { auto& ps = pendingSpawns();
              for (size_t i = 0; i < ps.size(); ) { if (ps[i].floor == cam.floor) { ps[i] = ps.back(); ps.pop_back(); ++m; } else ++i; } }
            // ⭐явная регистрация зачистки (ROM 57e0c): тикер мог пропустить переход prevAlive→0
            if (aliveEnemies(cam.floor) == 0 && pendingOnFloor(cam.floor) == 0 && floorSecT == 0) {
                msgs.push(ztmsg::ZERO_ENEMIES);
                floorSecT = 0xE1;
            }
            prevAlive = 0;
            con::log("killed " + std::to_string(n) + " enemies, cleared " + std::to_string(m) + " markers");
        });
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
        con::registerCmd("seggate", "seggate <on|off> - transit segments only via canonical edge+side per celltype (ROM 988a-9a4a; kills wrong-side stripes / blue-before-door); off=any-entry legacy", [&](const std::vector<std::string>& a){ if(!a.empty()) faSegGate()=(a[0]!="off"&&a[0]!="0"); con::log(std::string("seggate ")+(faSegGate()?"ON (canonical edge, ROM)":"OFF (any entry, legacy)")); });
        con::registerCmd("shaftprof", "shaftprof <on|off> - shaft wall 0x30 profile = cabin|1 (ROM 9862: moves with world on jump/crouch, frozen only during ride); off=hard pshift=0", [&](const std::vector<std::string>& a){ if(!a.empty()) faShaftProf()=(a[0]!="off"&&a[0]!="0"); con::log(std::string("shaftprof ")+(faShaftProf()?"ON (cabin|1 profile, ROM)":"OFF (frozen, legacy)")); });
        con::registerCmd("snipers", "snipers <on|off> - background sniper system (building+sprite on panorama, ricochets, tracer; ROM 2404/2682/27f8; crouch dodges 0x27)", [&](const std::vector<std::string>& a){ if(!a.empty()) faSnipers()=(a[0]!="off"&&a[0]!="0"); con::log(std::string("snipers ")+(faSnipers()?"ON":"OFF")); });
        con::registerCmd("voice", "voice <on|off|pace> - digitized speech for HUD messages (ROM Options 'Voice:'); pace = ROM-pause multiplier (0=off, 1=ROM)", [&](const std::vector<std::string>& a){ if(!a.empty()){ if(a[0]=="off"||a[0]=="0") voicePace()=0.0; else if(a[0]=="on") voicePace()=1.0; else voicePace()=std::max(0.0,std::min(2.0,std::atof(a[0].c_str()))); } con::log("voice pace x"+std::to_string(voicePace())); });
        con::registerCmd("blued28e", "blued28e <on|off> - blue backdrop per ROM d28e: one-sided when |pshift|>=0x28, full-blue column when strip off-screen (fixes up+down blue stripe on stair center / level bleed in elevator ride); off=always both sides", [&](const std::vector<std::string>& a){ if(!a.empty()) faBlueD28E()=(a[0]!="off"&&a[0]!="0"); con::log(std::string("blued28e ")+(faBlueD28E()?"ON (ROM d28e one-sided)":"OFF (both sides, legacy)")); });
        con::registerCmd("fxproj", "fxproj <on|off> - fixed-point wall projection in DDA path (stage 1: MD ca4c integer D0=divs.w(0x10000,fwd>>6), texel from 8.8 sub-pos; z-buffer contract unchanged); default ON", [&](const std::vector<std::string>& a){ if(!a.empty()) fxProj()=(a[0]!="off"&&a[0]!="0"); con::log(std::string("fxproj ")+(fxProj()?"ON (MD integer projection)":"OFF (double, legacy)")); });
        con::registerCmd("fxzquant", "fxzquant <on|off> - fixed-point stage 2: quantized column depth (integer D0 grid) + sprite z-test by integer scales (ROM ef20: sprite wins equality, replaces float BIAS 0.40); off=legacy BIAS (default)", [&](const std::vector<std::string>& a){ if(!a.empty()) fxZQuant()=(a[0]!="off"&&a[0]!="0"); con::log(std::string("fxzquant ")+(fxZQuant()?"ON (integer scales, ef20 sprite-wins-tie)":"OFF (float BIAS 0.40, legacy)")); });
        con::registerCmd("fxfan", "fxfan <on|off> - fixed-point stage 3: ray fan from ROM table $9124 (per-column angle offset, 512-quant, atan FOV 90) instead of continuous dirX/planeX; off=float fan (default)", [&](const std::vector<std::string>& a){ if(!a.empty()) fxFan()=(a[0]!="off"&&a[0]!="0"); con::log(std::string("fxfan ")+(fxFan()?"ON (ROM $9124 quantized fan)":"OFF (continuous fan, legacy)")); });
        con::registerCmd("fixedrender", "fixedrender <on|off> - master switch for all fixed-point render stages (fxproj + fxzquant + fxfan)", [&](const std::vector<std::string>& a){ if(!a.empty()){ bool v=(a[0]!="off"&&a[0]!="0"); fxProj()=v; fxZQuant()=v; fxFan()=v; } con::log(std::string("fixedrender: fxproj ")+(fxProj()?"ON":"OFF")+", fxzquant "+(fxZQuant()?"ON":"OFF")+", fxfan "+(fxFan()?"ON":"OFF")); });
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
        con::registerCmd("brief","brief <0-7> - play cutscene (0=intro 1=mission 2=decoy 3=central 4=subbase 5=victory 6=badend1 7=badend2)", [&](const std::vector<std::string>& a){ if(a.empty()){con::log("cutscenes 0-7: intro/mission/decoy/central/subbase/victory/badend1/badend2");return;} int n=std::atoi(a[0].c_str()); if(n>=0&&n<(int)gd.briefings.size()){ briefState.idx=n; briefState.scroll=0; briefFade=0; briefQueue.clear(); briefThenSelect=false; con::log("cutscene "+std::to_string(n)); } else con::log("range 0..7"); });
        // ⭐ОТЛАДОЧНЫЕ команды (перенесены с горячих клавиш, чтобы не мешали игре):
        con::registerCmd("grid",   "grid - toggle debug grid overlay", [&](const std::vector<std::string>&){ grid=!grid; dirty=true; con::log(std::string("grid ")+(grid?"ON":"OFF")); });
        con::registerCmd("enemyspeed","enemyspeed <x> - enemy speed scale (0.2..2.5)", [&](const std::vector<std::string>& a){ if(!a.empty()){ double v=std::atof(a[0].c_str()); enemySpeedScale()=v<0.2?0.2:(v>2.5?2.5:v); spdMsg=90; } con::log("enemyspeed="+std::to_string(enemySpeedScale())); });
        con::registerCmd("spritesize","spritesize <x> - enemy sprite size K (0.5..3.5)", [&](const std::vector<std::string>& a){ if(!a.empty()){ double v=std::atof(a[0].c_str()); faSpriteSizeK()=v<0.5?0.5:(v>3.5?3.5:v); szMsg=90; } con::log("spritesize="+std::to_string(faSpriteSizeK())); });
        con::registerCmd("hstretch","hstretch <x> - horizontal stretch (0.5..4.0)", [&](const std::vector<std::string>& a){ if(!a.empty()){ double v=std::atof(a[0].c_str()); faHStretch()=v<0.5?0.5:(v>4.0?4.0:v); dirty=true; } con::log("hstretch="+std::to_string(faHStretch())); });
        con::registerCmd("quit",   "quit - exit the game", [&](const std::vector<std::string>&){ running=false; });
        con::log("Zero Tolerance port console - type 'help'. Open/close: backquote key");
    }

    // ⭐СТАРТ: ЗАСТАВКИ (intro+mission) → ЭКРАН ВЫБОРА БОЙЦА → игра (ZT flow: титул→брифинг→выбор→игра).
    //   ДЕБАГ-ФЛАГИ: --fighter N (0..4) = сразу в игру за бойца N (пропуск заставок+выбора).
    //               --skip-intro (=--nobrief) = пропуск заставок, сразу экран выбора.
    {
        const char* fg = argStr(argc, argv, "--fighter");
        if (fg) { int v = std::atoi(fg); if (v >= 0 && v <= 4) { playerFighter() = v; selState.cursor = v; } }
        bool skipIntro  = hasFlag(argc, argv, "--skip-intro") || hasFlag(argc, argv, "--nobrief");
        bool wantSelect = (mode == 3 && !fg && !gd.fighters.empty());
        bool wantBrief  = (wantSelect && !skipIntro && !gd.briefings.empty());
        bool wantBoot   = (wantSelect && !skipIntro &&
                           (!gd.introFrames.empty() || !gd.logoFrames.empty() || !gd.titleGfxBlk.empty()));   // ⭐анимированный старт (ZT: technopop+титул; ZTU: лого мода+копирайт+титул)
        if (wantBoot) {                                   // ⭐ROM 0x976: копирайт → TECHNOPOP-заставка → интро
            bootPhase = 0; bootFrame = 0; bootPhaseMs = SDL_GetTicks(); bootThenBrief = wantBrief;
            snd::playSfx(0x03);                           // ⭐ROM 976: jsr c6156(#3) — музыка id3 играет с самого старта
        } else if (wantBrief) {                           // старт → INTRO + MISSION → потом выбор бойца
            briefState.idx = BR_INTRO; briefState.scroll = 0; briefFade = 0;
            briefMusic(BR_INTRO);                          // ⭐ROM caade: интро-заставка = музыка id0
            briefQueue = { BR_MISSION };                  // следующая после intro
            briefThenSelect = true;
        } else {
            selectActive = wantSelect;
            selFade = selectActive ? 0 : 16;
        }
        if (mode == 3 && fg) { applyFighterStart(playerFighter()); respawn(); }   // боец задан → стартовый инвентарь + игра сразу
    }
    // ⭐завершение стартовой заставки → интро-брифинг ИЛИ сразу экран выбора
    auto endBoot = [&]() {
        bootPhase = -1;
        if (bootThenBrief && !startPwPending && !gd.briefings.empty()) {   // ⭐continue по паролю → БЕЗ интро-заставки, сразу выбор
            briefState.idx = BR_INTRO; briefState.scroll = 0; briefFade = 0;
            briefQueue = { BR_MISSION }; briefThenSelect = true;   // музыка id3 УЖЕ играет с boot (ROM: непрерывно)
        } else { selectActive = true; selFade = 0; }
    };

    // ⭐КОНЕЦ СЕССИИ ЗАСТАВОК (Start-скип И авто-доскролл — одна логика). ROM-флоу:
    //  межэпизодный переход (1a40): эпизод+1 (снаряжение переносится) → ЭКРАН ПАРОЛЯ (58054-показ) → игра;
    //  финал/game over (976): возврат в BOOT-цикл (SEGA→…→титул), НЕ сразу выбор бойца (юзер 2026-07-22).
    auto finishBriefSession = [&]() {
        briefState.idx = -1;
        snd::musicStop();                                       // конец сессии заставок → тишина
        bool restart   = (pendingEpisode == 0);                 // victory/badend → рестарт с начала
        bool interlude = (pendingEpisode > 0);                  // межэпизодный переход
        if (pendingEpisode >= 0) { Inventory keep = inv; int keepHp = player().hp;
            floor = 0; cam.floor = 0; msgFloor = 0;             // старт с этажа 0
            setEp(pendingEpisode);                              // пароль ROM (58720) переносит жизни/оружие/патроны
            if (pendingEpisode != 0) { inv = keep; inv.syncCurrent(); player().hp = keepHp; }
            pwStack.clear(); pwProgressFloor() = 0;             // новый эпизод: стек с нуля (pwEpisodePass остаётся)
            if (pendingEpisode == 0) pwEpisodePass.clear();
            pendingEpisode = -1; }
        if (interlude && !pwEpisodePass.empty()) {              // ⭐MISSION CODE между эпизодами (показ, START=игра)
            pwscr::st().open(); pwscr::st().showOnly = true; pwscr::st().buf = pwEpisodePass;
        }
        if (briefThenSelect) { briefThenSelect = false;
            if (restart && (!gd.introFrames.empty() || !gd.logoFrames.empty() || !gd.titleGfxBlk.empty())) {
                bootPhase = 0; bootFrame = 0; bootPhaseMs = SDL_GetTicks(); bootThenBrief = true;   // заново с заставки
                snd::playSfx(0x03);                             // ROM 976: музыка id3 с самого старта
            } else { selectActive = true; selFade = 0; }
        }
        dirty = true;
    };

    // ⭐ДЕЙСТВИЯ КОРНЕВОГО ESC-МЕНЮ — общие для клика мышью и клавиатуры (Enter по стрелке-курсору).
    auto doRoot = [&](MenuAction a) {
        switch (a) {
            case MA_RESUME:   menu = false; dirty = true; break;
            case MA_OPTIONS:  menuMode() = 1; menuPage = 0; menuStatus[0] = 0; dirty = true; break;
            case MA_DEBUG:    menuMode() = 2; menuPage = 0; menuStatus[0] = 0; dirty = true; break;
            case MA_SAVEGAME: menuMode() = 3; menuStatus[0] = 0; dirty = true; break;
            case MA_LOADGAME: menuMode() = 4; menuStatus[0] = 0; dirty = true; break;
            case MA_QUIT:     running = false; break;
            case MA_PASSWORD: pwscr::st().open(); SDL_StartTextInput(); dirty = true; break;   // ⭐экран ввода пароля
            case MA_ABOUT:    menuMode() = 5; menuStatus[0] = 0; dirty = true; break;          // ⭐ABOUT (версия/инфо)
            case MA_NEWGAME: {                                                // с начала игры: свежий отряд, эп 0, этаж 0
                for (bool& dd : selState.dead) dd = false; selState.cursor = 0; gameOver = false;
                startPwPending = false;                                       // ⭐новая игра отменяет ожидающий пароль
                pwStack.clear(); pwEpisodePass.clear(); pwProgressFloor() = 0;
                floor = 0; cam.floor = 0; msgFloor = 0; setEp(0);
                menu = false; menuMode() = 0; selPending = false;
                selectActive = true; selFade = 0; dirty = true; break; }
            default: break;
        }
    };

    while (running) {
        Uint32 frameStart = SDL_GetTicks();
        menuPreGame() = (bootPhase >= 0);   // ⭐OPTIONS с титула (до геймплея) → пункт ENTER PASSWORD виден/кликабелен
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) running = false;
            else if (e.type == SDL_TEXTINPUT && pwscr::st().active) {   // ⭐ВВОД ПАРОЛЯ С КЛАВИАТУРЫ (регистрозависимо, только 64-симв. алфавит)
                for (const char* c = e.text.text; *c; ++c)
                    if (pwscr::st().typeChar(*c) == pwscr::A_TYPED) snd::playSfx(0x69);
                dirty = true;
            }
            else if (e.type == SDL_TEXTINPUT && con::isOpen()) {  // ВВОД ТЕКСТА в консоль (раскладко-зависимый — правильно для печати)
                con::onText(e.text.text);
            }
            else if (e.type == SDL_KEYDOWN) {
                // ⚠ РАСКЛАДКО-НЕЗАВИСИМО: используем ФИЗИЧЕСКИЙ scancode, НЕ keysym.sym (рус.раскладка ломала клавиши).
                SDL_Scancode key = e.key.keysym.scancode;
                // ⭐ЭКРАН ВВОДА ПАРОЛЯ (модально, поверх меню/выбора): навигация сетки + Enter/Esc/Backspace.
                if (pwscr::st().active) {
                    auto& ps = pwscr::st();
                    if (ps.showOnly) {                          // ⭐MISSION CODE (показ): START/ESC → в игру
                        if (key == SDL_SCANCODE_RETURN || key == SDL_SCANCODE_SPACE ||
                            key == SDL_SCANCODE_RCTRL || key == SDL_SCANCODE_ESCAPE) { ps.close(); snd::playSfx(0x6b); }
                        dirty = true; continue;
                    }
                    switch (key) {
                        case SDL_SCANCODE_ESCAPE: ps.close(); SDL_StopTextInput(); break;
                        case SDL_SCANCODE_RETURN: case SDL_SCANCODE_KP_ENTER: handlePwAct(pwscr::A_SUBMIT); break;   // Enter = DONE (QoL)
                        case SDL_SCANCODE_BACKSPACE: handlePwAct(ps.keyBackspace()); break;
                        case SDL_SCANCODE_LEFT:  ps.move(-1, 0); snd::playSfx(0x6b); break;
                        case SDL_SCANCODE_RIGHT: ps.move(+1, 0); snd::playSfx(0x6b); break;
                        case SDL_SCANCODE_UP:    ps.move(0, -1); snd::playSfx(0x6b); break;
                        case SDL_SCANCODE_DOWN:  ps.move(0, +1); snd::playSfx(0x6b); break;
                        case SDL_SCANCODE_SPACE: case SDL_SCANCODE_RCTRL: handlePwAct(ps.activate()); break;   // выбор ячейки сетки
                        default: break;
                    }
                    dirty = true; continue;
                }
                // ⭐СТАРТОВАЯ ЗАСТАВКА: START/ENTER/SPACE пропускает текущую фазу (копирайт→technopop→игра)
                if (bootPhase >= 0 && !menu) {
                    if (bootPhase == 4) {                             // ⭐ТИТУЛ: кнопка → меню; UP/DOWN; START = выбор
                        bool confirm = (key == SDL_SCANCODE_RETURN || key == SDL_SCANCODE_SPACE || key == SDL_SCANCODE_RCTRL);
                        if (!titleMenuOn) {
                            if (confirm || key == SDL_SCANCODE_UP || key == SDL_SCANCODE_DOWN) { titleMenuOn = true; snd::playSfx(0x6b); }
                        } else if (key == SDL_SCANCODE_UP)   { titleSel = 1; snd::playSfx(0x6b); }
                        else if (key == SDL_SCANCODE_DOWN)   { titleSel = 2; snd::playSfx(0x6b); }
                        else if (confirm) {
                            snd::playSfx(0x69);
                            if (titleSel == 1) endBoot();             // START → интро-брифинг → выбор бойца
                            else { menu = true; menuMode() = 1; menuPage = 0; }   // OPTIONS → наши опции (полноэкр.)
                        }
                        dirty = true; continue;
                    }
                    if (key == SDL_SCANCODE_RETURN || key == SDL_SCANCODE_SPACE || key == SDL_SCANCODE_RCTRL) {
                        bootPhase = bootPhase + 1; bootFrame = 0; bootPhaseMs = SDL_GetTicks();
                        if (bootPhase == 4) { titleMenuOn = false; titleSel = 1; }
                        dirty = true;
                    }
                    continue;   // заставка поглощает ввод
                }
                // ⭐ЗАСТАВКА (ZT @0xCB1E4): Start/Enter пропускает к следующей, остальной ввод поглощается.
                if (briefState.idx >= 0) {
                    if (key == SDL_SCANCODE_RETURN || key == SDL_SCANCODE_SPACE) {   // Start → следующая заставка / конец
                        if (!briefQueue.empty()) { briefState.idx = briefQueue.front(); briefQueue.erase(briefQueue.begin()); briefState.scroll = 0; briefFade = 0;
                            if (briefState.idx == BR_EXPLODE) snd::playSfx(0x98); }   // ⭐ВЗРЫВ-кадр game-over = БОЛЬШОЙ взрыв id0x98 (ROM cb120, ×3 с фейдами; 0x8a был неверен)
                        else finishBriefSession();          // ⭐переход эпизода + MISSION CODE / рестарт с заставки
                        dirty = true;
                    }
                    continue;   // заставка поглощает ввод
                }
                // ⭐ЭКРАН ВЫБОРА БОЙЦА (ZT @0xC6362): перехват ввода. ВВЕРХ/ВНИЗ навигация (bit0/bit1 $ff002e), START — выбор.
                if (selectActive) {
                    int n = (int)gd.fighters.size();
                    if (key == SDL_SCANCODE_P) { pwscr::st().open(); SDL_StartTextInput(); dirty = true; continue; }  // ⭐экран пароля (сетка ROM 58054)
                    if (gameOver) { if (key == SDL_SCANCODE_RETURN || key == SDL_SCANCODE_SPACE) { for (bool& d : selState.dead) d = false; gameOver = false; selState.cursor = 0;
                        startPwPending = false; pwStack.clear(); pwEpisodePass.clear(); pwProgressFloor() = 0;
                        floor = 0; cam.floor = 0; msgFloor = 0; setEp(0); dirty = true; } continue; }   // ⭐рестарт С НАЧАЛА ИГРЫ
                    if (selState.slideT > 0) continue;                   // слайд-переход блокирует ввод (ZT: c683c блокирующий)
                    if (key == SDL_SCANCODE_UP)        { selState.move(+1, n); snd::playSfx(0x6b); dirty = true; }   // ↑ следующий (ZT addq)
                    else if (key == SDL_SCANCODE_DOWN) { selState.move(-1, n); snd::playSfx(0x6b); dirty = true; }   // ↓ предыдущий (ZT subq)
                    else if (key == SDL_SCANCODE_RETURN || key == SDL_SCANCODE_SPACE || key == SDL_SCANCODE_RCTRL) {  // START/A/B/C — выбор
                        if (!selState.dead[selState.cursor] && selFadeOut == 0) {   // ЖИВ → выбрать (ZT c6504: SFX 0x6b, fade-out в игру)
                            playerFighter() = selState.cursor; snd::playSfx(0x6b);
                            selFadeOut = 16;                             // плавный переход экран→игра (ZT 1f866 fade-out)
                            dirty = true;
                        } else if (selState.dead[selState.cursor]) snd::playSfx(0x69);  // МЁРТВ → отказ (ZT c64f8: SFX 0x69, остаться)
                    }
                    continue;   // экран выбора поглощает весь ввод
                }
                if (con::isOpen()) {                          // КОНСОЛЬ открыта — все клавиши ей (кроме ` / Esc закрывают)
                    if (key == SDL_SCANCODE_GRAVE || key == SDL_SCANCODE_ESCAPE) { con::toggle(); if (!con::isOpen()) SDL_StopTextInput(); }
                    else con::onKey(key, (SDL_GetModState() & KMOD_SHIFT) != 0);
                    continue;
                }
                if (menu && rebindAction() >= 0) {           // РЕЖИМ ПЕРЕНАЗНАЧЕНИЯ: следующая клавиша → биндинг (ESC/` отменяют)
                    if (key != SDL_SCANCODE_ESCAPE && key != SDL_SCANCODE_GRAVE) { keyBind(rebindAction()) = key; refreshKeyName(rebindAction()); }
                    std::snprintf(menuStatus, sizeof(menuStatus), "%s = %s", gaLabel(rebindAction()), keyBindNames()[rebindAction()].c_str());
                    rebindAction() = -1; dirty = true; continue;   // клавиша поглощена (не идёт в геймплей)
                }
                if (key == SDL_SCANCODE_GRAVE) { con::toggle(); SDL_StartTextInput(); continue; }  // ` — открыть консоль (GZDoom-style)
                if (key == SDL_SCANCODE_ESCAPE) {            // ESC — Sound Test закрыть / подменю → корень / меню вкл-выкл
                    if (sndtest::open()) { sndtest::open() = false; snd::stopAllSfx(); dirty = true; }
                    else if (menu && menuMode() != 0) {
                        if (bootPhase >= 0) { menu = false; menuMode() = 0; }   // с титула — закрыть меню целиком
                        else menuMode() = 0;
                        menuStatus[0] = 0; rebindAction() = -1; dirty = true; }
                    else { menu = !menu; menuMode() = 0; menuPage = 0; menuStatus[0] = 0; rebindAction() = -1; dirty = true; }
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
                    // ⭐КОРНЕВОЕ МЕНЮ: клавиатурная навигация (стрелка-курсор) + Enter. Подменю (OPTIONS/DEBUG/слоты) — мышью.
                    if (menuMode() == 0) {
                        using menu::NROOT;
                        if (key == SDL_SCANCODE_UP)        { menuSel() = (menuSel() - 1 + NROOT) % NROOT; snd::playSfx(0x6b); dirty = true; }
                        else if (key == SDL_SCANCODE_DOWN) { menuSel() = (menuSel() + 1) % NROOT; snd::playSfx(0x6b); dirty = true; }
                        else if (key == SDL_SCANCODE_RETURN || key == SDL_SCANCODE_KP_ENTER || key == SDL_SCANCODE_SPACE)
                            doRoot(rootAction(menuSel()));
                    }
                    // ребинд ловится в начале KEYDOWN
                } else if (key == keyBind(GA_JUMP)) {
                    if (mode == 3 && player().jumpY <= 0.0 && player().crouchY >= -1.0) player().jumpVel = 9.0;   // ПРЫЖОК (переназначаемо)
                } else if (key == keyBind(GA_WEAP_PREV)) {
                    if (mode == 3) { cycleWeapon(inv, -1); snd::ev(snd::SFX_SWITCH); }                            // оружие ← (переназначаемо)
                } else if (key == keyBind(GA_WEAP_NEXT)) {
                    if (mode == 3) { cycleWeapon(inv, +1); snd::ev(snd::SFX_SWITCH); }                            // оружие → (переназначаемо)
                } else if (key == keyBind(GA_MAP)) {
                    mapOpen = !mapOpen;
                    { int total = pwProgressFloor() + 1 + (pwEpisodePass.empty() ? 0 : 1);   // строк в списке паролей
                      int ms = total - 3; pauseViewFloor = ms < 0 ? 0 : ms; } dirty = true;  // скролл → текущий виден внизу
                } else if (mapOpen && (key == SDL_SCANCODE_UP || key == SDL_SCANCODE_DOWN)) {
                    // ⭐ЛИСТАНИЕ на паузе (ROM 2cec бит0/бит1, звук 0x6b) СКРОЛЛИТ СПИСОК ПАРОЛЕЙ (карта НЕ меняется!)
                    int nf = pauseViewFloor + (key == SDL_SCANCODE_DOWN ? 1 : -1);
                    int total = pwProgressFloor() + 1 + (pwEpisodePass.empty() ? 0 : 1);
                    int maxScroll = total - 3; if (maxScroll < 0) maxScroll = 0;
                    if (nf >= 0 && nf <= maxScroll) { pauseViewFloor = nf; snd::playSfx(0x6b); dirty = true; }
                } else switch (key) {
                    // ⭐Только QoL-клавиши. Отладка (ep/floor/god/noclip/give/grid/enemyspeed/spritesize/hstretch/quit/…)
                    //   перенесена в КОНСОЛЬ (клавиша `) — чтобы случайные нажатия не мешали игре.
                    case SDL_SCANCODE_F5:                        // ⭐QUICKSAVE (Doom-style)
                        if (mode == 3) { bool okq = saveGameSlot(0); msgs.pushRaw(okq ? "QUICKSAVE OK" : "QUICKSAVE FAILED"); dirty = true; }
                        break;
                    case SDL_SCANCODE_F9:                        // ⭐QUICKLOAD
                        if (mode == 3) { if (loadGameSlot(0)) msgs.pushRaw("QUICKLOAD OK"); else msgs.pushRaw("NO QUICKSAVE"); dirty = true; }
                        break;
                    case SDL_SCANCODE_PAUSE: case SDL_SCANCODE_F1: case SDL_SCANCODE_BACKSLASH:
                        if (mode == 3) paused = !paused; break;   // ФОТОРЕЖИМ: пауза симуляции (кадр застывает, курсор свободен — скринь)
                    default: break;
                }
            }
            else if (e.type == SDL_MOUSEMOTION) {            // ПОВОРОТ МЫШЬЮ (только в шутере; БЕЗ инерции — применяется прямо)
                if (bootPhase == 4 && titleMenuOn && !menu) { // ховер мыши по START/OPTIONS на титуле
                    int ww = FBW, wh = FBH; SDL_GetWindowSize(win, &ww, &wh);
                    SDL_Rect src, dst; presentRects(ww, wh, false, src, dst);
                    int mx = (dst.w > 0) ? (e.motion.x - dst.x) * src.w / dst.w + src.x : 0;
                    int my = (dst.h > 0) ? (e.motion.y - dst.y) * src.h / dst.h + src.y : 0;
                    double sc = (double)FBW / 320.0; { double sy = (double)FBH / 224.0; if (sy < sc) sc = sy; }
                    int vpX = (FBW - (int)(320 * sc)) / 2, vpY = (FBH - (int)(224 * sc)) / 2;
                    int tx = (int)((mx - vpX) / sc), ty = (int)((my - vpY) / sc);
                    if (tx >= 110 && tx < 220 && ty >= 146 && ty < 194) {
                        int ns = (ty < 170) ? 1 : 2;
                        if (ns != titleSel) { titleSel = ns; snd::playSfx(0x6b); dirty = true; }
                    }
                }
                else if (menu && menuMode() == 0) {           // ⭐КОРНЕВОЕ МЕНЮ: стрелка-курсор следует за мышью
                    int ww = FBW, wh = FBH; SDL_GetWindowSize(win, &ww, &wh);
                    SDL_Rect src, dst; presentRects(ww, wh, false, src, dst);
                    int mx = (dst.w > 0) ? (e.motion.x - dst.x) * src.w / dst.w + src.x : 0;
                    int my = (dst.h > 0) ? (e.motion.y - dst.y) * src.h / dst.h + src.y : 0;
                    for (int i = 0; i < menu::NROOT; ++i)
                        if (menu::rootBtn(i).has(mx, my) && menuSel() != i) { menuSel() = i; dirty = true; break; }
                }
                else if (bootPhase < 0 && mode == 3 && !menu && !con::isOpen() && !mapOpen && !sndtest::open()
                         && !paused && !selectActive && !gameOver && briefState.idx < 0)
                    mouseTurn += e.motion.xrel;   // копим ТОЛЬКО в геймплее — иначе на выборе/заставке накопится и дёрнет при возврате
            }
            else if (e.type == SDL_MOUSEWHEEL) {             // КОЛЁСО → выбор оружия (вверх/вниз); кнопки Z/X не трогаем
                if (mode == 3 && !menu && !con::isOpen() && !mapOpen) { cycleWeapon(inv, e.wheel.y > 0 ? -1 : 1); snd::ev(snd::SFX_SWITCH); }
            }
            else if (e.type == SDL_MOUSEBUTTONDOWN && pwscr::st().active && e.button.button == SDL_BUTTON_LEFT) {
                // ⭐МЫШЬ на экране пароля: клик по ячейке/действию = поставить курсор + активировать (набор/DEL/CANCEL/DONE).
                int ww = FBW, wh = FBH; SDL_GetWindowSize(win, &ww, &wh);
                SDL_Rect src, dst; presentRects(ww, wh, false, src, dst);
                int mx = (dst.w > 0) ? (e.button.x - dst.x) * src.w / dst.w + src.x : 0;
                int my = (dst.h > 0) ? (e.button.y - dst.y) * src.h / dst.h + src.y : 0;
                if (pwscr::hitTest(mx, my)) handlePwAct(pwscr::st().activate());
                dirty = true;
            }
            else if (e.type == SDL_MOUSEBUTTONDOWN && bootPhase >= 0 && !menu && e.button.button == SDL_BUTTON_LEFT) {
                // ⭐МЫШЬ на стартовых заставках/титуле: клик = пропуск фазы; на титуле — выбор START/OPTIONS
                if (bootPhase < 4) { bootPhase++; bootFrame = 0; bootPhaseMs = SDL_GetTicks();
                    if (bootPhase == 4) { titleMenuOn = false; titleSel = 1; } dirty = true; }
                else {
                    int ww = FBW, wh = FBH; SDL_GetWindowSize(win, &ww, &wh);
                    SDL_Rect src, dst; presentRects(ww, wh, false, src, dst);
                    int mx = (dst.w > 0) ? (e.button.x - dst.x) * src.w / dst.w + src.x : 0;
                    int my = (dst.h > 0) ? (e.button.y - dst.y) * src.h / dst.h + src.y : 0;
                    double sc = (double)FBW / 320.0; { double sy = (double)FBH / 224.0; if (sy < sc) sc = sy; }
                    int vpX = (FBW - (int)(320 * sc)) / 2, vpY = (FBH - (int)(224 * sc)) / 2;
                    int tx = (int)((mx - vpX) / sc), ty = (int)((my - vpY) / sc);   // → координаты 320×224 титула
                    if (!titleMenuOn) { titleMenuOn = true; titleSel = 1; snd::playSfx(0x6b); }
                    else if (tx >= 110 && tx < 220 && ty >= 146 && ty < 194) {      // строки меню (Y=146/170, по 24px)
                        titleSel = (ty < 170) ? 1 : 2; snd::playSfx(0x69);
                        if (titleSel == 1) endBoot();
                        else { menu = true; menuMode() = 1; menuPage = 0; }
                    }
                    dirty = true;
                }
            }
            else if (e.type == SDL_MOUSEBUTTONDOWN && menu && e.button.button == SDL_BUTTON_LEFT) {
                int ww = FBW, wh = FBH; SDL_GetWindowSize(win, &ww, &wh);
                SDL_Rect src, dst; presentRects(ww, wh, false, src, dst);  // ⭐мышь мапится тем же единым регионом
                int mx = (dst.w > 0) ? (e.button.x - dst.x) * src.w / dst.w + src.x : 0;   // окно → координаты FB
                int my = (dst.h > 0) ? (e.button.y - dst.y) * src.h / dst.h + src.y : 0;
                double frac = 0;
                MenuAction mact = menuHit(mx, my, menuPage, frac);
                if (mact >= MA_KEYBIND && mact < MA_KEYBIND + GA_COUNT) {   // CONTROLS: клик по клавише → ребинд
                                                              // ⚠ ДИАПАЗОН: MA_SLOT=240 > MA_KEYBIND — слоты сейвов ловились как ребинд!
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
                    case MA_MS_DEC: mouseSensitivity() = clampd(mouseSensitivity() - 0.1, 0.0, 2.0); break;  // ЧУВСТВИТЕЛЬНОСТЬ МЫШИ
                    case MA_MS_INC: mouseSensitivity() = clampd(mouseSensitivity() + 0.1, 0.0, 2.0); break;
                    case MA_MS_BAR: mouseSensitivity() = clampd(frac * 2.0, 0.0, 2.0); break;
                    case MA_SND:    soundOn() = !soundOn(); break;                              // ЗВУК вкл/выкл
                    case MA_SV_DEC: soundVolume() = clampd(soundVolume() - 0.1, 0.0, 1.0); break;  // ГРОМКОСТЬ
                    case MA_SV_INC: soundVolume() = clampd(soundVolume() + 0.1, 0.0, 1.0); break;
                    case MA_SV_BAR: soundVolume() = clampd(frac, 0.0, 1.0); break;
                    case MA_ET_DEC: enemyTimerScale() = clampd(enemyTimerScale() - 0.1, 0.3, 4.0); break;  // ТИК-ТАЙМЕРЫ ВРАГОВ (морф/атаки/cd)
                    case MA_ET_INC: enemyTimerScale() = clampd(enemyTimerScale() + 0.1, 0.3, 4.0); break;
                    case MA_ET_BAR: enemyTimerScale() = clampd(0.3 + frac * (4.0 - 0.3), 0.3, 4.0); break;
                    case MA_VP_DEC: voicePace() = clampd(voicePace() - 0.1, 0.0, 2.0); break;   // ТЕМП ОЗВУЧКИ (0=выкл)
                    case MA_VP_INC: voicePace() = clampd(voicePace() + 0.1, 0.0, 2.0); break;
                    case MA_VP_BAR: voicePace() = clampd(frac * 2.0, 0.0, 2.0); break;
                    case MA_MUSIC:  musicOn() = !musicOn(); snd::musicEnabled() = musicOn();
                                    if (!musicOn()) snd::musicStop(); break;   // МУЗЫКА вкл/выкл (OFF = немедленный стоп)
                    case MA_FPSINV: fpsInvariant() = !fpsInvariant(); break;
                    case MA_INTRES: faIntRes() = faIntRes() % 4 + 1; std::snprintf(menuStatus, sizeof menuStatus, "Internal res %dx (experimental SSAA)", faIntRes()); break;
                    case MA_SB_DEC: simBaseFps() = clampd(simBaseFps() - 1.0, 10.0, 60.0); break;
                    case MA_SB_INC: simBaseFps() = clampd(simBaseFps() + 1.0, 10.0, 60.0); break;
                    case MA_SB_BAR: simBaseFps() = clampd(10.0 + frac * 50.0, 10.0, 60.0); break;
                    case MA_PAGE:   menuPage = (menuPage + 1) % (menuMode() == 2 ? menu::NPAGE_DBG : menu::NPAGE_OPT); break;   // следующая страница подменю
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
                    case MA_QUIT: case MA_RESUME: case MA_OPTIONS: case MA_DEBUG:    // ⭐корневые пункты → общий doRoot
                    case MA_SAVEGAME: case MA_LOADGAME: case MA_NEWGAME: case MA_PASSWORD: case MA_ABOUT:
                        doRoot(mact); break;
                    case MA_BACK:    if (bootPhase >= 0) menu = false; menuMode() = 0; menuStatus[0] = 0; rebindAction() = -1; dirty = true; break;   // с титула — закрыть целиком
                    default:
                        if (mact >= MA_SLOT && mact < MA_SLOT + 6) {
                            int sl = mact - MA_SLOT;
                            if (menuMode() == 3) std::snprintf(menuStatus, sizeof menuStatus, saveGameSlot(sl) ? "Saved to slot %d" : "SAVE FAILED (slot %d)", sl);
                            else if (menuMode() == 4) { if (loadGameSlot(sl)) { menu = false; menuMode() = 0; }
                                                        else std::snprintf(menuStatus, sizeof menuStatus, "Slot %d: no save", sl); }
                            dirty = true;
                        }
                        break;
                }
            }
        }

        // ⭐FIRE-ЛАТЧ: пока открыт любой оверлей (меню/консоль/карта/пауза/выбор/брифинг), взводим блок огня —
        // при возврате в игру выстрел не сработает, пока кнопку огня не отпустят (клик по CONTINUE ≠ выстрел).
        if (menu || con::isOpen() || sndtest::open() || paused || mapOpen || selectActive || briefState.idx >= 0 || pwscr::st().active)
            fireBlockUntilRelease = true;
        if (con::isOpen()) dirty = true;   // консоль открыта → перерисовывать (видеть ввод/лог) и в статичных режимах
        // ЗАХВАТ МЫШИ для поворота: ТОЛЬКО в активном геймплее И только если управление мышью включено (sens>0).
        // Не захватываем на бут-заставке/титуле (bootPhase>=0), экране выбора бойца (selectActive), брифингах
        // (briefState.idx>=0), GAME OVER (gameOver), в меню/консоли/карте/саунд-тесте/паузе — там нужен обычный курсор.
        bool gameplayActive = (mode == 3 && bootPhase < 0 && !menu && !con::isOpen() && !mapOpen
                               && !sndtest::open() && !paused && !selectActive && !gameOver && briefState.idx < 0
                               && !pwscr::st().active);
        SDL_SetRelativeMouseMode((gameplayActive && mouseSensitivity() > 0.0) ? SDL_TRUE : SDL_FALSE);
        // ⭐ФИКС-TIMESTEP: сколько ТИКОВ симуляции запустить в этом кадре (аккумулятор по РЕАЛЬНОМУ времени).
        // fpsInvariant ON → сим крутится на simBaseFps() Гц независимо от frameLimit (скорость игры стабильна).
        // OFF → 1 тик/кадр (легаси, fps-зависимо). Клампы: пропуск больших пауз, антиспираль ≤6 тиков/кадр.
        int nSimTics; double simAlpha = 1.0;   // simAlpha = дробный остаток тика (для интерполяции камеры)
        { static Uint32 s_simLast = 0; static double s_simAccum = 0.0;
          Uint32 nowMs = SDL_GetTicks(); if (s_simLast == 0) s_simLast = nowMs;
          double dtSec = (nowMs - s_simLast) / 1000.0; s_simLast = nowMs;
          if (dtSec > 0.25) dtSec = 0.25;
          if (fpsInvariant()) {
              double rate = simBaseFps(); if (rate < 1.0) rate = 1.0;
              s_simAccum += dtSec * rate;
              nSimTics = (int)s_simAccum; s_simAccum -= nSimTics;
              if (nSimTics > 6) nSimTics = 6;
              simAlpha = s_simAccum;                     // 0..1: доля пути к следующему тику
          } else { nSimTics = 1; s_simAccum = 0.0; } }
        // ⭐ПАУЗА МУЗЫКИ (ROM GEMS cmd 0x0D/0x0C при паузе игры): ESC-меню и Tab-карта замораживают секвенсер
        { static bool musPaused = false; bool want = menu || mapOpen || paused;
          if (want != musPaused) { musPaused = want; snd::musicSetPaused(want); } }
        // ⭐ЗАТУХАНИЕ ВСПЫШКИ $FF1072 (ROM VBlank 0xB12, 60 Гц реального времени, идёт и на паузе как в ROM):
        // >15 → −0x110/кадр (смерть: белый→красный за 15 кадров), ≤15 → −1/кадр (красный→чёрный), кламп 0.
        { static Uint32 s_flashBase = 0; static long s_flashSeen = 0; Uint32 now = SDL_GetTicks();
          if (s_flashBase == 0) s_flashBase = now;
          long vbl = (long)(now - s_flashBase) * 60 / 1000;        // счётчик 60Гц-кадров (без потери остатка)
          int steps = (int)(vbl - s_flashSeen); s_flashSeen = vbl;
          { int& fc = player().flashCram;
            while (steps-- > 0 && fc > 0) fc = (fc > 15) ? fc - 0x110 : fc - 1;
            if (fc < 0) fc = 0; if (fc > 0) dirty = true; } }

        if (pwscr::st().active) {
            // ⭐ЭКРАН ВВОДА ПАРОЛЯ (ROM 58054): сетка символов + курсор + буфер (поверх всего, мир на паузе).
            if (pwscr::st().msgT > 0 && --pwscr::st().msgT == 0) {           // сообщение погасло
                pwscr::st().msg.clear();
                if (pwscr::st().doneClose) { pwscr::st().close(); SDL_StopTextInput(); }   // «ACCEPTED» → закрыть (вернуться в опции)
            }
            pwscr::render(fb, gd.selFont);
            g_viewX = 0; g_viewY = 0; g_viewW = HUD_W * 2; g_viewH = HUD_H * 2;
            con::draw(fb, FBW, FBH, presentRenderScale());
            SDL_UpdateTexture(tex, nullptr, fb.px.data(), FBW * sizeof(uint32_t));
            SDL_SetWindowTitle(win, "ztpp — Enter Password");
            dirty = true;
        } else if (bootPhase >= 0 && !menu) {
            // ⭐СТАРТОВАЯ ЗАСТАВКА (ROM-тайминги): копирайт (1cdee: fade-in 180 к 0x1CD8E,
            // fade-out 60 кадров) → TECHNOPOP 22 кадра (1efd2: задержки в VBLANK-кадрах из числа jsr 1f5fe×6).
            fb.clear(0xFF000000u);
            double sc = (double)FBW / 320.0; { double sy = (double)FBH / 224.0; if (sy < sc) sc = sy; }
            int vpW = (int)(320 * sc), vpH = (int)(224 * sc), vpX = (FBW - vpW) / 2, vpY = (FBH - vpH) / 2;
            g_viewX = vpX; g_viewY = vpY; g_viewW = vpW; g_viewH = vpH;   // ⭐единый 448-регион (было «битое» растяжение)
            const std::vector<uint32_t>* img = nullptr;
            Uint32 el = SDL_GetTicks() - bootPhaseMs;
            int fadeT = -1; bool fadeIn = false;
            if (bootPhase == 0) {                                    // ⭐SEGA (sub_05711e): вращающийся логотип —
                // «стена» движка с текстурой из банка стен (тайлы 0xF7..0xFE), зум+вращение по sin-LUT 0x8124,
                // палитро-анимация кольца @0x570CA. Рендер sega_intro.hpp; кончился → копирайт.
                static std::vector<uint32_t> segaFrame(320 * 224);
                int sf = (int)(el * 60 / 1000);
                if (gd.hasSega && segai::renderSegaFrame(gd, sf, segaFrame.data())) {
                    for (int y = 0; y < vpH; ++y) { int ny = (int)(y / sc); if (ny > 223) ny = 223;
                        const uint32_t* srcp = &segaFrame[(size_t)ny * 320];
                        for (int x = 0; x < vpW; ++x) { int nx = (int)(x / sc); if (nx > 319) nx = 319;
                            fb.put(vpX + x, vpY + y, srcp[nx]); } }
                } else if (!gd.hasSega && !gd.logoFrames.empty()) {
                    // ⭐ЛОГО МОДА (ZTU 0x2638E): экраны по 0xB4=180 кадров (счётчик @0x26484), палитро-фейд
                    // 0x1d5d0 (рампа к целевой палитре; порт: in/out по 30 кадров, длительность из дизасма).
                    const Uint32 DUR = 180 * 1000 / 60, RAMP = 30 * 1000 / 60;
                    int li = (int)(el / DUR);
                    if (li >= (int)gd.logoFrames.size()) { bootPhase = 1; bootFrame = 0; bootPhaseMs = SDL_GetTicks(); el = 0; }
                    else {
                        img = &gd.logoFrames[li];
                        Uint32 pe = el - (Uint32)li * DUR;
                        if (pe < RAMP)            { fadeT = (int)(pe * 8 / RAMP); if (fadeT > 7) fadeT = 7; fadeIn = true; }
                        else if (pe > DUR - RAMP) { fadeT = (int)((DUR - pe) * 8 / RAMP); if (fadeT > 7) fadeT = 7; fadeIn = false; }
                    }
                } else { bootPhase = 1; bootFrame = 0; bootPhaseMs = SDL_GetTicks(); el = 0; }
            }
            if (bootPhase == 1) {                                    // КОПИРАЙТ (ROM 1cdee): цикл in 180 кадров, out 60;
                // ⭐фейд-функция 1cc04 идёт ЧЕРЕЗ КАДР по одному каналу (R→G→B) шагом 1 DAC-уровень → полный
                // фейд ≈ 42 кадра, остаток 180-кадрового цикла = ХОЛД (было: фейд растянут на все 180 — медленно).
                img = &gd.copyrightScreen;
                const Uint32 IN = 180 * 1000 / 60, OUT = 60 * 1000 / 60, RAMP = 42 * 1000 / 60;
                if (el < IN)            { fadeT = (int)(el * 8 / RAMP); if (fadeT > 7) fadeT = 7; fadeIn = true; }
                else if (el < IN + OUT) { fadeT = 7 - (int)((el - IN) * 8 / RAMP); if (fadeT < 0) fadeT = 0; fadeIn = false; }
                else { bootPhase = 2; bootFrame = 0; bootPhaseMs = SDL_GetTicks(); el = 0; }
            }
            if (bootPhase == 2) {                                    // ⭐ACCOLADE (sub_052C7E): 15 шагов «печатной
                // машинки» по 5 VBLANK (~83мс), финальная выдержка 61 VBLANK (~1.02с). Звука нет (в ROM нет).
                const Uint32 STEP = 5 * 1000 / 60, HOLD = 61 * 1000 / 60;
                int ns = (int)gd.accoladeFrames.size();
                if (ns == 0 || el >= (Uint32)ns * STEP + HOLD) { bootPhase = 3; bootFrame = 0; bootPhaseMs = SDL_GetTicks(); el = 0; }
                else { int fi = (int)(el / STEP); if (fi >= ns) fi = ns - 1; img = &gd.accoladeFrames[fi]; }
            }
            if (bootPhase == 3) {                                    // TECHNOPOP: ROM-задержки per-кадр (VBLANK-кадры)
                static const int DUR[22] = {27,6,6,6,6,6,6,6,6,6,6,6,6,6,36,6,6,6,6,6,6,12};
                Uint32 acc = 0; int fi = 0;
                for (; fi < 22; ++fi) { acc += (Uint32)(DUR[fi] * 1000 / 60); if (el < acc) break; }
                bootFrame = fi;
                if (bootFrame >= (int)gd.introFrames.size()) { bootPhase = 4; titleMenuOn = false; titleSel = 1; bootPhaseMs = SDL_GetTicks(); }
                else img = &gd.introFrames[bootFrame];
            }
            if (bootPhase == 4) {                                    // ⭐ТИТУЛ (ROM 1cf36): ПОЛНАЯ анимация —
                // лента ZERO TOLERANCE + санбёрст-лучи с пульсом + глоу силуэта + меню (рендерер title_fx.hpp).
                static std::vector<uint32_t> ttlFrame(320 * 224);
                int tf = (int)(el * 60 / 1000);                      // кадр 60Гц от старта фазы
                ttl::renderTitleFrame(gd, tf, titleMenuOn ? titleSel : 0, ttlFrame.data());
                for (int y = 0; y < vpH; ++y) { int ny = (int)(y / sc); if (ny > 223) ny = 223;
                    const uint32_t* srcp = &ttlFrame[(size_t)ny * 320];
                    for (int x = 0; x < vpW; ++x) { int nx = (int)(x / sc); if (nx > 319) nx = 319;
                        fb.put(vpX + x, vpY + y, srcp[nx]); } }
                // демо-таймаут ROM (-$7928=0x564≈23с): демо-плейбека нет — аттракт-петля на Technopop
                if (SDL_GetTicks() - bootPhaseMs > 23000 && !titleMenuOn) {
                    bootPhase = 3; bootFrame = 0; bootPhaseMs = SDL_GetTicks();
                }
            }
            if (bootPhase >= 0 && img && !img->empty()) {
                for (int y = 0; y < vpH; ++y) { int ny = (int)(y / sc); if (ny > 223) ny = 223;
                    const uint32_t* src = &(*img)[(size_t)ny * 320];
                    for (int x = 0; x < vpW; ++x) { int nx = (int)(x / sc); if (nx > 319) nx = 319; fb.put(vpX + x, vpY + y, src[nx]); } }
                if (fadeT >= 0) { if (fadeIn) fbFadeInMD(fb, fadeT); else fbFadeOutMD(fb, 7 - fadeT); }
            }
            con::draw(fb, FBW, FBH, presentRenderScale());
            SDL_UpdateTexture(tex, nullptr, fb.px.data(), FBW * sizeof(uint32_t));
            SDL_SetWindowTitle(win, "ztpp — Zero Tolerance");
            dirty = true;
        } else if (briefState.idx >= 0) {
            // ⭐ЗАСТАВКА/БРИФИНГ (ZT @0xCB1E4): скроллящийся текст поверх фона. Up ускоряет, Start пропускает.
            const Uint8* ks = con::isOpen() ? nullptr : SDL_GetKeyboardState(nullptr);
            briefState.fast   = ks && ks[SDL_SCANCODE_UP];
            briefState.paused = ks && (ks[SDL_SCANCODE_DOWN] || ks[SDL_SCANCODE_A]);   // ⭐ROM: DOWN/A удерживать = стоп текста
            if (briefFade < 16) ++briefFade;
            if (briefFade >= 16) briefState.tick();       // скролл после появления
            if (briefState.done(gd)) {                    // текст доскроллил → следующая заставка / конец
                if (!briefQueue.empty()) { briefState.idx = briefQueue.front(); briefQueue.erase(briefQueue.begin()); briefState.scroll = 0; briefFade = 0;
                            if (briefState.idx == BR_EXPLODE) snd::playSfx(0x98); }   // ⭐ВЗРЫВ-кадр game-over = БОЛЬШОЙ взрыв id0x98 (ROM cb120, ×3 с фейдами; 0x8a был неверен)
                else finishBriefSession();                    // ⭐переход эпизода + MISSION CODE / рестарт с заставки
            }
            drawBriefing(fb, gd, briefState, 2);
            if (briefFade < 16) fbFadeInMD(fb, briefFade * 7 / 16);   // ⭐ROM-фейд 1f870 (ступени 8 уровней)
            g_viewX = 0; g_viewY = 0; g_viewW = HUD_W * 2; g_viewH = HUD_H * 2;
            con::draw(fb, FBW, FBH, presentRenderScale());
            SDL_UpdateTexture(tex, nullptr, fb.px.data(), FBW * sizeof(uint32_t));
            SDL_SetWindowTitle(win, "ztpp — Zero Tolerance");
            dirty = true;
        } else if (selectActive) {
            // ⭐ЭКРАН ВЫБОРА БОЙЦА (ZT @0xC6362): портрет + карточка + DECEASED. Симуляция мира пропущена.
            const bool fadingOut = (selFadeOut > 0);   // ПОДТВЕРЖДЕНИЕ: fade-out → игра (ZT 1f866)
            if (!fadingOut) {
                if (selFade < 16) ++selFade;          // ПОЯВЛЕНИЕ: fade-in (ZT 1f85e плавно из чёрного)
                if (!paused) selState.tickSlide();    // слайд-переход между бойцами (ZT c683c/c6896)
            }
            drawSelectScreen(fb, gd, selState);
            int fadeLvl = fadingOut ? (selFadeOut - 1) : selFade;   // fade-out 15→0 (последний кадр = ЧЁРНОЕ, без вспышки/скачка)
            if (fadeLvl < 16) { if (fadingOut) fbFadeOutMD(fb, 7 - fadeLvl * 7 / 16);   // ⭐ROM-фейды: out=1f93a (вычитание
                                else           fbFadeInMD(fb, fadeLvl * 7 / 16); }         //  ступеней), in=1f870 (подъём к цели)
            if (gameOver) drawTextC(fb, FBW / 2, FBH / 2 + 100, "ALL SOLDIERS DECEASED - GAME OVER  (ENTER: RESTART)", 0xFFFF4040u, 1);
            drawTextC(fb, FBW / 2, FBH - 14*uiScale(), "P: PASSWORD", 0xFF8090A0u, uiScale());   // ⭐P → экран ввода пароля (сетка)
            dirty = true;                             // экран выбора перерисовывается (слайд/fade-анимация)
            // РЕГИОН презентации = верхняя reference-область (0,0,640,448); экран рисуется в (0,0), НЕ смещён как игра
            // (иначе после смерти g_viewY=96 от игрового рендера обрезал экран выбора). Фикс: свой g_view.
            g_viewX = 0; g_viewY = 0; g_viewW = HUD_W * 2; g_viewH = HUD_H * 2;
            con::draw(fb, FBW, FBH, presentRenderScale());
            SDL_UpdateTexture(tex, nullptr, fb.px.data(), FBW * sizeof(uint32_t));
            SDL_SetWindowTitle(win, "ztpp — Select Soldier");
            // ПЕРЕХОД после презентации кадра (fade-out дошёл до чёрного) → инициализация игры, следующий кадр = игра.
            if (fadingOut && --selFadeOut == 0) {     // ZT: после выбора, НЕ раньше
                selectActive = false; resetPlayerHP();
                if (startPwPending) {                            // ⭐СТАРТ ПО ПАРОЛЮ: ep/floor (+снаряжение, ROM 0xfc0)
                    applyPassword(startPwState);
                    if (startPwState.cheatFull) applyFighterStart(playerFighter());   // чит-слово: снаряжение бойца по умолчанию
                    startPwPending = false;
                } else { applyFighterStart(playerFighter()); respawn(); }   // ⭐стартовый инвентарь выбранного бойца
                floor = cam.floor;                    // синхронизировать внешний floor с загруженным этажом
                // СМЕРТЬ → новый боец: мир ПЕРСИСТЕНТЕН (ROM: респавн ct0x77 без reload — убитые/разбуженные/
                // позиции сохраняются; пул чистится только на загрузке уровня 133a4). Прежний clearActors+re-collect
                // дублировал маркеры при персистентности этажей.
                if (selPending) { selPending = false; aEp = ep; aFloor = cam.floor; prevAlive = -1; floorSecT = 0; }
            }
        } else if (sndtest::open()) {
            // SOUND TEST: замороженный кадр сцены + оверлей меню звуков (мир на паузе — блок mode==3 пропущен)
            render(fb, gd, ep, floor, mode, grid, cam, meta, zbuf, faithful, reference, inv);
            sndtest::draw(fb);
            con::draw(fb, FBW, FBH, presentRenderScale());
            SDL_UpdateTexture(tex, nullptr, fb.px.data(), FBW * sizeof(uint32_t));
            SDL_SetWindowTitle(win, "ztpp — Sound Test");
        } else if (menu) {
            // меню настроек: рисуем замороженный кадр сцены + оверлей меню (мир на паузе)
            render(fb, gd, ep, floor, mode, grid, cam, meta, zbuf, faithful, reference, inv);
            drawMenu(fb, moveSpd, turnSpd, faHStretch(), frameLimit, enemySpeedScale(), menuPage, faithful, noclip, reference, enemiesOn, gameMapMode(), menuStatus);
            con::draw(fb, FBW, FBH, presentRenderScale());   // оверлей консоли (поверх всего)
            SDL_UpdateTexture(tex, nullptr, fb.px.data(), FBW * sizeof(uint32_t));
            SDL_SetWindowTitle(win, "ztpp — Settings");
        } else if (mode == 3) {
          // ⭐ИНТЕРПОЛЯЦИЯ КАМЕРЫ: помним позу камеры ПЕРЕД тиками этого кадра; при рендере, если тиков
          // не было или кадр «между тиками», рисуем камеру в lerp(prev,cur,simAlpha) — 30/60fps плавные,
          // мир при этом бит-точно шагает на simBaseFps Гц. static: поза живёт между кадрами.
          static double ipx = cam.px, ipy = cam.py, ipDirX = cam.dirX, ipDirY = cam.dirY, ipPitch = cam.pitch;
          static int ipFloor = cam.floor;
          if (nSimTics > 0) { ipx = cam.px; ipy = cam.py; ipDirX = cam.dirX; ipDirY = cam.dirY; ipPitch = cam.pitch; ipFloor = cam.floor; }
          // ⭐ЦИКЛ ФИКС-TIMESTEP: тело симуляции крутится nSimTics раз (частота = simBaseFps, не frameLimit).
          // Останавливаемся, если тик переключил экран (смерть→селект, конец эпизода→заставка) — не симить дальше.
          for (int _simStep = 0; _simStep < nSimTics && !paused && !selectActive && briefState.idx < 0; ++_simStep) {
                           // ⏸ ФОТОРЕЖИМ: при паузе весь этот блок не выполняется (nSimTics в расчёте участвует, но
                           // цикл сразу отсекается по !paused); камера/мир застывают, кадр рисуется ниже как есть.
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
                  // ⭐В fixed-move (дефолт) авторитетна ЦЕЛАЯ позиция pxI/pyI, а cam.px выводится из неё КАЖДЫЙ кадр
                  //   (rcMovePhysics). rcMove двигает только float px → без синка нокбэк ОТКАТЫВАЛСЯ следующим кадром
                  //   («отталкивает и обратно возвращает»). Синхронизируем целую позицию (ROM dd9e: нокбэк в дельту движения).
                  if (faFixedMove()) { cam.pxI = (int)std::lround(cam.px * 256.0); cam.pyI = (int)std::lround(cam.py * 256.0); }
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
            // ⭐латч возврата из меню/паузы: гасим огонь, пока кнопку не отпустят (иначе клик по CONTINUE = выстрел)
            if (fireBlockUntilRelease) { if (fireHeld) fireHeld = false; else fireBlockUntilRelease = false; }
            if (fireHeld) {
                int wid = inv.current;
                bool isW    = (wid >= 0 && wid < 15 && ITEMS[wid].weapon);
                bool ammoOk = !isW || inv.ammo[wid] > 0;
                // не-авто оружие (пистолет/дробовик/ПУЛЬС/ракета/граната/кулаки) — semi-auto: выстрел ТОЛЬКО на свежее нажатие.
                // Иначе при удержании inv.fire циклится 1..4→0 и перестреливает каждые 5 кадров → pulse звучит 3× за «тап».
                bool start  = (inv.fire == 0) && (fireAuto(wid) || !prevFireHeld);
                bool autoRe = fireAuto(wid) && inv.fire >= 3;                    // авто-перестрел (лазер/огнемёт/пена — держать)
                // ⭐МЁРТВЫЙ НЕ СТРЕЛЯЕТ (ROM 11c2c: tst -$721e = счётчик смерти → ввод оружия отрублен;
                // юзер: «после смерти всё равно можно бить/стрелять» — гейт отсутствовал в порте)
                if (!player().dead &&
                    (start || autoRe) && inv.slide <= 0 && inv.switching == 0 && ammoOk) {   // смена оружия запрещает выстрел (ZT 12a78)
                    if (isW) { --inv.ammo[wid];                                  // расход боезапаса
                        if (inv.ammo[wid] == 5 || inv.ammo[wid] == 0) msgs.push(ztmsg::AMMO_LOW); }  // «мало патронов»
                    if (wid < 0 || heldDisplayKind(wid) == 0) {                 // КУЛАКИ (ZT -$6fa4): и при ПАССИВНОМ ПРЕДМЕТЕ
                                                                                // (ID-карта/медпак и т.п. display=0x11f3c → кулачный бой)
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
            // ⭐ПЕРСИСТЕНТНОСТЬ ЭТАЖЕЙ (ROM b8fc): смена этажа НЕ переинициализирует мир. Пул актёров
            // глобальный (чистится только на загрузке уровня, ROM 133a4), убитые/разбуженные/позиции
            // сохраняются; маркеры/огни/камеры собираются ОДИН РАЗ на первый визит этажа. Возврат на
            // зачищенный этаж → перезачёт (b8fc: clr -$58d4 + jsr 57e0c) → «ZERO ENEMIES» заново.
            if (ep != aEp) {                                 // смена ЭПИЗОДА/уровня = полный сброс (ROM 133a4)
                clearActors(); clearFloorState(); alarmCams().clear(); visitedFloors = 0;
            }
            if (ep != aEp || cam.floor != aFloor) {
                if (!(visitedFloors & (1u << (cam.floor & 31)))) {   // ПЕРВЫЙ визит этажа
                    visitedFloors |= 1u << (cam.floor & 31);
                    if (enemiesOn) collectEnemyMarkers(gd.levels[ep], cam.floor);
                    spawnMapFires(gd.levels[ep], cam.floor);         // карта-огонь (celltype 0x18) → вечные AT_FIRE
                    collectAlarmCams(gd.levels[ep], cam.floor);      // камеры-тревоги 0x26 этажа
                }
                // ⭐ФИКСАЦИЯ ПАРОЛЯ (ROM b8fc→57ec4): при смене этажа, если прогресс-этаж ЗАЧИЩЕН
                // (-$58d4≠0), пароль этажа pwProgress (снапшот состояния СЕЙЧАС) кладётся в стек и
                // прогресс++ — пароль больше НЕ меняется. Без зачистки прогресс стоит (пароль не выдан).
                if (aFloor >= 0 && floorSecT > 0 && (int)pwStack.size() == pwProgressFloor()) {
                    pwStack.push_back(buildPasswordFor(ep * 16 + pwProgressFloor()));
                    ++pwProgressFloor();
                }
                aEp = ep; aFloor = cam.floor;
                prevAlive = -1;                              // «только пришли» → перезачёт объявит ZERO ENEMIES при 0
                floorSecT = 0;                               // как ROM: clr -$58d4 перед пересчётом нового этажа
                snip::reset();                               // ⭐снайпер: ROM 1054/1058 на level load (жизни=3, неактивен)
            }
            // ⭐РАСХОД ПАССИВОК (ROM sub_013182): 1.0 заряда каждые 128 тиков ПОКА ПРЕДМЕТ В ИНВЕНТАРЕ.
            // Ночник(9) приоритетнее фонаря(6) — при ночнике фонарь НЕ тратится (и конус не ставится);
            // сканер(1) независим (фаза +0x30). При 0 — предмет исчезает (10bd8: clr слота). Кап 10 ≈ 85с.
            { static int passiveT = 0; ++passiveT;
              int period = 128;                              // ROM 128 сим-тиков (фикс-timestep)
              auto drain = [&](int id) { if (inv.has(id) && --inv.ammo[id] <= 0) inv.dropItem(id); };
              if (passiveT % period == 0)                 { if (inv.has(9)) drain(9); else drain(6); }
              if ((passiveT + period * 3 / 8) % period == 0) drain(1); }
            // ⭐КОСТЮМ (ROM d7dc→10f98): каждый ПОГЛОЩЁННЫЙ ожог списывает 1.0 заряда костюма; 0 → исчез.
            while (suitBurnAbsorbs() > 0) { --suitBurnAbsorbs();
                if (inv.has(5) && --inv.ammo[5] <= 0) inv.dropItem(5); }
            player().fireImmune = inv.has(5);                 // огнезащ.костюм → иммун. к огню (нужно и для рендера)
            // ⭐флаги пассивок КАЖДЫЙ тик (баг: ставились только в render(), а игровой цикл зовёт
            // renderReference НАПРЯМУЮ → ночник/фонарь «включались» лишь после захода в меню ESC)
            nvActive() = inv.has(9);
            flActive() = inv.has(6) && !inv.has(9);
            if (!mapOpen) {                                   // ПАУЗА-КАРТА: мир заморожен (не обновляем актёров/спавн/разрушение)
            if (enemiesOn) updateEnemySpawns(gd.levels[ep], cam.floor, cam.px, cam.py);  // спавн врагов рядом с игроком
            updateAlarmCams(gd.levels[ep], cam.floor, cam.px, cam.py);  // камера-тревога: детект→взвод→разрушение стен/дверей 11×11 + будильник
            // БРОНЯ из инвентаря (перед уроном): жилет (3) → пул брони = заряд·10 (поглощает урон, −10%/попадание).
            player().armor      = inv.has(3) ? inv.ammo[3] * 10 : 0;
            updateActors(gd.levels[ep], cam);                 // think всех актёров + очередь спрайтов (worldFx)
            inv.tickBlink();                                  // ⭐мигание иконок после подбора (ZT 0108d6, 1/тик)
            pushCameraBillboards(cam.floor);                  // ЖИВЫЕ камеры-тревоги → билборды (после updateActors: worldFx уже очищен/заполнен)
            if (applyDestruct(gd.levels[ep])) snd::ev(snd::SFX_WALL);   // разрушаемые/секрет-стены + ЗВУК разрушения (0x3b)
            if (inv.has(3)) {                                 // броня израсходовалась в уроне → вернуть в заряд жилета; 0 → жилет выбывает
                inv.ammo[3] = player().armor / 10;
                if (player().armor <= 0) inv.dropItem(3);
            }
            // ⭐ТИКЕР ЗАЧИЩЕННОГО ЭТАЖА (ROM 57e0c + 1e0fa): при нуле врагов -$58d4=0xE1 + «ZERO ENEMIES
            // REMAINING»; дальше счётчик тикает вниз КАЖДЫЙ игровой кадр: на 0x69=105 → «PROCEED TO NEXT
            // LEVEL» (cmpi #$69 @1e0fa), на 0 → «FLOOR SECURED» + перезаряд 0xE1=225 — напоминания чередуются
            // по кругу (~120 тиков между). floorSecT масштабируем по frameLimit (ROM-цикл ~15 fps).
            // ⭐считается ЭТАЖ ПРОГРЕССА -$58D6 (ROM 57e0c: d5=-$58d6), НЕ этаж камеры: спуск без зачистки
            // оставляет счёт на прежнем этаже — SECURED/пароль не выдаются, пока прогресс-этаж не зачищен.
            // ⭐СЧИТАЕМ ТЕКУЩИЙ ЭТАЖ (cam.floor), где игрок — НЕ прогресс-этаж. Баг-фикс (юзер): раньше считался
            //   pwProgressFloor; зачистив нижний этаж (прогресс=0 врагов) и зайдя на верхний НЕзачищенный, порт
            //   ложно объявлял его зачищенным. «FLOOR SECURED» теперь про этаж, на котором игрок стоит.
            { int pf = cam.floor;
              int alive = aliveEnemies(pf);
              if (prevAlive != 0 && alive == 0 && pendingOnFloor(pf) == 0) {   // prevAlive=-1 (приход) тоже объявляет (ROM b8fc→57e0c)
                  msgs.push(ztmsg::ZERO_ENEMIES);
                  floorSecT = 0xE1;                             // старт цикла напоминаний (ROM 0xE1, сим-тики)
              }
              prevAlive = alive; }
            // ⭐КОНЕЦ ЭПИЗОДА (ROM 1728→1a40): смерть босса дотикивает episodeEndT → слайд-заставка →
            // эпизод+1 → загрузка нового с этажа 0 (setEp по концу очереди заставок). Маппинг заставок
            // (ROM-слайды 1/2/4 после эп 0/1/2): эп1→DECOY, эп2→CENTRAL+SUBBASE, эп3→VICTORY→выбор бойца.
            if (episodeEndT() > 0 && --episodeEndT() == 0) {
                // ⭐ЭПИЗОД-ПАРОЛЬ (ROM 0x1A62: a0=-$58CE → 57f98→5801a, status = ep<<4|0xF): генерится ЗДЕСЬ,
                // со снапшотом состояния конца эпизода; отобразится на карте след. эпизода НАД этажом 0.
                pwEpisodePass = buildPasswordFor(ep * 16 + 15);
                // ⭐ПОРЯДОК = ROM-секвенсер (блоки: intro,mission,decoy,central,subbase,intro,victory;
                // вход caa4e: d0=1→[DECOY,CENTRAL], d0=2→[SUBBASE], d0=4→[VICTORY+титры в конце текста])
                if (ep == 0)      { briefState.idx = BR_DECOY;   briefQueue = { BR_CENTRAL }; pendingEpisode = 1; }
                else if (ep == 1) { briefState.idx = BR_SUBBASE; briefQueue = {}; pendingEpisode = 2; }
                else              { briefState.idx = BR_VICTORY; briefQueue = {}; pendingEpisode = 0; briefThenSelect = true; }  // финал: победа+титры → заново
                briefState.scroll = 0; briefFade = 0;
                snd::stopAllSfx(); briefMusic(briefState.idx);
            }
            if (floorSecT > 0) {
                --floorSecT;
                if (floorSecT == 0x69) msgs.push(ztmsg::PROCEED_NEXT);   // отметка 0x69 (сим-тики)
                else if (floorSecT == 0) { msgs.push(ztmsg::FLOOR_SECURED); floorSecT = 0xE1; }
            }
            // ⭐СМЕРТЬ (ZT @0x1742): длинная вспышка ($FF1072=0xFFF, ~30 кадров, pitch пикирует −28) → пометить бойца DECEASED
            //   ($FF28EC[боец]=0) → все мертвы? GAME OVER : экран выбора нового бойца → продолжение уровня (HP=100).
            if (player().dead) {
                if (deathAnim == 0) {                          // 1-й кадр смерти: старт длинной вспышки + пикирование вида
                    deathAnim = 30;                          // ROM ~30 сим-тиков (-$721e)
                    player().flashCram = 0xFFF;              // ⭐ROM 1756: $FF1072=0xFFF (белый→красный→чёрный за 30 VBlank, затухает в 0xB12-логике ниже)
                    player().knockTimer = deathAnim; player().knockPitch = -16;   // вид падает (ZT pitch-цель −28)
                } else if (--deathAnim <= 0) {                 // анимация завершена → DECEASED + переход
                    if (!gd.fighters.empty()) selState.dead[playerFighter()] = true;
                    player().dead = false;
                    if (con::isOpen()) { con::toggle(); SDL_StopTextInput(); }   // ⭐консоль закрывается при переходе на селект (висела)
                    // ⭐ROM: смерть НЕ сбрасывает уровень (respawn ct0x77 без reload, пул чистится только на
                    // загрузке уровня 133a4) — убитые/двери/трупы/позиции остаются (был clearActors+re-collect = сброс)
                    respawn();
                    aEp = ep; aFloor = cam.floor; prevAlive = -1; floorSecT = 0;
                    if (gd.fighters.empty()) { resetPlayerHP(); }          // нет карточек → просто респаун (старое поведение)
                    else if (selState.allDead()) {                          // ВСЕ бойцы мертвы → BADEND (ZT screen 5, по эпизоду) → рестарт отряда
                        for (bool& d : selState.dead) d = false; selState.cursor = 0;   // рестарт: отряд снова жив
                        if (!gd.briefings.empty()) {
                            // ZT hendler5: эп0→badend1(космос); эп1→badend2(central)+взрыв; эп2→EXIT(badend2 на EXIT-фоне)+взрыв
                            if (ep == 0)      { briefState.idx = BR_BADEND1; briefQueue = {}; }
                            else if (ep == 1) { briefState.idx = BR_BADEND2; briefQueue = { BR_EXPLODE }; }
                            else              { briefState.idx = BR_EXIT;    briefQueue = { BR_EXPLODE }; }
                            briefState.scroll = 0; briefFade = 0; briefThenSelect = true;   // после → выбор бойца
                            pendingEpisode = 0;                             // ⭐GAME OVER = рестарт С НАЧАЛА ИГРЫ (эп 0, этаж 0),
                            briefMusic(briefState.idx);                     //  не с этажа смерти (юзер 2026-07-17)
                        } else { gameOver = true; selectActive = true; selFade = 0; }
                    }
                    else { selPending = true; selectActive = true; selState.cursor = playerFighter(); selFade = 0;
                           snd::musicStop(); }  // ⭐на экране выбора музыки НЕТ (юзер: тема эпизода продолжала играть)
                }   // затухание вспышки — VBlank-правило 0xB12 в главном цикле (не сим-тик)
            }

            // лифты (авто-поездка с иллюзией питча) и лестницы (наклон по ходьбе + своп на переходе)
            int ccx = (int)cam.px, ccy = (int)cam.py;
            bool entered = (ccx != lastCX || ccy != lastCY);
            int prevElev = cam.elevState;
            rcUpdateTransit(cam, gd.levels[ep], entered);
            if (!noclip) rcDecorPush(cam, gd.levels[ep]);   // ⭐солидный декор (ROM a33a/a38c): выталкивание из деревьев/столов/столбов
            // СТАРТ поездки: ОДНА длинная нота 0x6f (гудение по огибающей, не пульс). ROM e69e/e6fe играет её на
            // КАЖДОМ старте (гейт только cabin==0). Порт гейтил prevElev==0, но после прибытия elevState=±2
            // держится, пока стоишь на area → повторная поездка (±2→±1) шла БЕЗ звука (BACKLOG «звук только раз»).
            { const bool rideNow  = (cam.elevState == 1 || cam.elevState == -1);
              const bool ridePrev = (prevElev      == 1 || prevElev      == -1);
              if (rideNow && !ridePrev) {
                  snd::playElevatorHum();
                  // ⭐«FLOOR NOT SECURED» при старте лифта на НЕзачищенном этаже (ROM render3d e69e/e6fe/e75e:
                  // tst -0x58d4; bne skip). floorSecT — прямой аналог -$58d4 (0 = не зачищен).
                  if (floorSecT == 0) msgs.push(ztmsg::FLOOR_NOT_SEC);
              }
              }
            cam.pitch += player().knockPitch + player().jumpY + player().crouchY;  // нокдаун + ПРЫЖОК (вверх) + ПРИСЕД (вниз) — верт. положение камеры (поверх лифт/лестница-питча)
            lastCX = (int)cam.px; lastCY = (int)cam.py;   // после возможной центровки в кабине
            if (cam.floor != floor) floor = cam.floor;    // синхрон заголовка (без respawn)
            if (cam.floor != msgFloor) {                  // смена этажа (ZT: ↑ индекс=вниз по зданию)
                // ⭐Эпизоды 1/2 (ep 0/1) → ИМЕНОВАННОЕ «ENTERING ‹место› LEVEL n» из ROM-таблицы 0x1ee94
                // (level_load b854/b8ca). Эпизод 3 (ep 2) → generic «STEPPING ONE FLOOR UP/DOWN» по направлению.
                const char* nm = ztmsg::enteringFloor(ep, cam.floor);
                if (nm) msgs.push(nm);
                else    msgs.push(cam.floor > msgFloor ? ztmsg::FLOOR_DOWN : ztmsg::FLOOR_UP);
                msgFloor = cam.floor;
            }
            if (rcUpdateDoors(cam.floor, cam.px, cam.py, gd.levels[ep])) snd::ev(snd::SFX_DOOR);  // анимация дверей + ЗВУК открытия (0x6d)
            // ⭐МУЗЫКА ГЕЙМПЛЕЯ (ROM level_load 119c/1826: id = НОМЕР ЭПИЗОДА → seq0/7/2). В ЛИФТЕ (клетки
            // кабины/area) — музыка кабины 0x88 ВСЁ ВРЕМЯ пребывания, тема эпизода вернётся ТОЛЬКО по выходе
            // (юзер). want-модель: смена желаемого трека → стоп+старт; тишина (трек кончился) → перезапуск.
            { static int musWant = -1; static bool liftMus = false;
              const int pcx = (int)cam.px, pcy = (int)cam.py;
              const uint8_t pct2 = (pcx >= 0 && pcy >= 0 && pcx < Level::W && pcy < Level::H)
                                   ? gd.levels[ep].cellType(cam.floor, pcx, pcy) : 0;
              // ⭐ЛИФТ (ROM e69e/e6fe): музыка кабины стартует С ПОЕЗДКОЙ (elevState ±1) и держится, пока
              // игрок остаётся на клетках лифта; ушёл с клеток → тема эпизода (юзер).
              if (cam.elevState == 1 || cam.elevState == -1) liftMus = true;
              if (!rcElevCell(pct2)) liftMus = false;
              int want = liftMus ? 0x88 : (ep <= 2 ? ep : -1);
              if (briefState.idx >= 0 || selectActive || !musicOn()) { musWant = -1; }
              else if (want >= 0) {
                  if (want != musWant) { snd::musicStop(); snd::playSfx(want); musWant = want; }
                  else if (snd::musicCurrent() < 0) snd::playSfx(want);      // трек дозвучал → заново (луп)
              } }
            // ⭐ФОНОВЫЙ СНАЙПЕР (sniper_overlay.hpp; ROM 2682/27f8/26da/28b4/28f6 VERIFIED): спавн пока
            // игрок СТОИТ на ct 0x27/0x28 (диспетчер celltype @E420), жизни 3 на уровень (-$6f89@1058);
            // фазы: 1..9 вылезает, 0xA..0xF очередь (звук 0x68+0x97 КАЖДЫЙ тик — ROM ретриггерит канал),
            // хит-чеки 0xA/0x10 (0x28 всегда, 0x27 без приседа → 15 HP), 0x20 спрятался → респавн в новом
            // окне; рикошеты по крыше + трассер ракеты (звук долёта 0x18). Тумблер `snipers`.
            if (faSnipers() && ep == 1) {
                const int scx = (int)cam.px, scy = (int)cam.py;
                uint8_t pct = (scx >= 0 && scy >= 0 && scx < Level::W && scy < Level::H)
                              ? gd.levels[ep].cellType(cam.floor, scx, scy) : 0;
                bool onCell = (pct == 0x27 || pct == 0x28);
                // ⭐спавн = СОБЫТИЕ ВХОДА в клетку (ROM e870/e88a: step-on e3f8 только при смене кэша
                // клетки игрока -$71ea), не «пока стоит» — иначе снайпер зациклен без пауз.
                static int prevCx = -1, prevCy = -1;
                bool entered = onCell && (scx != prevCx || scy != prevCy);
                prevCx = scx; prevCy = scy;
                int angRam = cam.angI >= 0 ? cam.angI
                             : (((int)std::lround(camDirToAng512(cam)) + 128) & 0x1FF);
                snip::TickEv sev = snip::tick(entered, angRam);
                if (sev.burst) { snd::playSfx(0x68); snd::playSfx(0x97); }   // очередь (2822/284a)
                if (sev.shot)  snd::playSfx(0x18);                           // трассер долетел (2922)
                if (sev.hitCheck && onCell) {
                    // 0x27: укрытие если камера НИЗКО (2882: -71e6 ≤ −10) — присед ИЛИ нокдаун (ROM
                    // берёт полный -71e6). Порт: crouchY+knockPitch (jumpY>0 не спасает — только выше).
                    double camH = player().crouchY + player().knockPitch;
                    if (pct == 0x28 || camH > -10.0)
                        damagePlayer(15, cam.px, cam.py, cam.px + 2.0, cam.py);   // d800 X=0x64 → 15 HP
                }
            }
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
          }   // конец ЦИКЛА ФИКС-TIMESTEP (nSimTics тиков симуляции)

            // выставить регион игры в fb (для аспект-блита И позиционирования оверлеев в видимой части)
            if (mapOpen) {   // ПАУЗА-КАРТА (TAB): меню паузы ИЛИ full map. ⭐g_view = 448-регион (единый размер)
                             { double sc = (double)FBW/320.0; double sy = (double)FBH/224.0; if (sy < sc) sc = sy;
                               g_viewW = (int)(320*sc); g_viewH = (int)(224*sc);
                               g_viewX = (FBW - g_viewW)/2; g_viewY = (FBH - g_viewH)/2; }
                             if (pauseFullMap()) drawFullMap(fb, gd, ep, floor, cam);
                             else {   // ⭐СПИСОК ПАРОЛЕЙ (ROM 5755e/5761e): строки = [эпизод-пароль (57b5a, метка
                                      //  ПРЕДЫДУЩЕГО эпизода) при наличии] + этажи 0..pwProgress; пройденный этаж =
                                      //  «label + пароль ИЗ СТЕКА» (зафиксирован при переходе, НЕ перегенеривается),
                                      //  текущий (pwProgress) = «*SECURED*»/«*NOT SECURED*» по -$58d4 (floorSecT).
                                      //  Окно 3 строк по скроллу; КАРТА ВСЕГДА ТЕКУЩИЙ этаж.
                                 std::vector<std::string> all;
                                 if (!pwEpisodePass.empty())
                                     all.push_back((ep == 1 ? std::string("SPACE STATION : ")
                                                            : std::string("HIGH RISE : ")) + pwEpisodePass);   // ROM 57b90/57ba9
                                 for (int f = 0; f <= pwProgressFloor() && f < 16; ++f) {
                                     int gi = ep * 16 + f;
                                     std::string lbl = (gi < (int)gd.pauseNames.size() && !gd.pauseNames[gi].empty())
                                                         ? gd.pauseNames[gi] : "LEVEL ? : ";
                                     if (f < (int)pwStack.size()) all.push_back(lbl + pwStack[f]);
                                     else all.push_back(lbl + (floorSecT > 0 ? "*SECURED*" : "*NOT SECURED*"));   // ROM 5769d/57690
                                 }
                                 std::vector<std::string> pls;
                                 int base = pauseViewFloor; if (base > (int)all.size() - 1) base = (int)all.size() - 1; if (base < 0) base = 0;
                                 for (int i = base; i < (int)all.size() && (int)pls.size() < 3; ++i) pls.push_back(all[i]);
                                 drawPauseMap(fb, gd, ep, cam.floor, cam, &pls); } }   // карта = текущий этаж
            else {
                // ⭐ИНТЕРПОЛЯЦИЯ КАМЕРЫ (fps-invariant): рендерим позу lerp(prev,cur,simAlpha) — плавные
                // 30/60fps при бит-точной симуляции 15 Гц. Подмена только на время рендера (angI=-1 → float-путь).
                double bpx = cam.px, bpy = cam.py, bdx = cam.dirX, bdy = cam.dirY, bpitch = cam.pitch;
                double bplx = cam.planeX, bply = cam.planeY; int bangI = cam.angI;
                // ⛔ НЕ лерпить через разрыв позы: смена этажа (лестница/лифт) или телепорт (respawn) —
                // иначе камеру «подбрасывает» промежуточными кадрами между старой и новой позой.
                bool contin = (cam.floor == ipFloor) &&
                              (std::fabs(bpx - ipx) + std::fabs(bpy - ipy) < 1.0) &&
                              (std::fabs(bpitch - ipPitch) < 60.0);
                bool interp = fpsInvariant() && simAlpha > 0.001 && simAlpha < 0.999 && !paused && contin;
                if (interp) {
                    double a = simAlpha;
                    cam.px = ipx + (bpx - ipx) * a; cam.py = ipy + (bpy - ipy) * a;
                    double dx = ipDirX + (bdx - ipDirX) * a, dy = ipDirY + (bdy - ipDirY) * a;
                    double L = std::hypot(dx, dy);
                    if (L > 1e-6) { cam.dirX = dx / L; cam.dirY = dy / L;
                                    cam.planeX = -cam.dirY * 0.66; cam.planeY = cam.dirX * 0.66; }
                    cam.pitch = ipPitch + (bpitch - ipPitch) * a;
                    cam.angI = -1;                                   // рендер по float-направлению (без квант-LUT)
                }
                // ⭐ВСПЫШКА урона/смерти НЕ красит сцену (ROM 0xB12: $FF1072 → CRAM-слот 63 = чёрный фон
                // КОКПИТА) — перекраска в renderReference. Сцена всегда обычной палитрой.
                Palette scenePal = nvActive() ? gd.nvPal : gd.wallPal;
                if (reference) { g_viewW = HUD_W * 2; g_viewH = HUD_H * 2; g_viewX = (FBW - g_viewW) / 2; g_viewY = (FBH - g_viewH) / 2;
                             renderReference(fb, gd, gd.levels[ep], scenePal, cam, meta, zbuf, inv); }
                else           { g_viewX = 0; g_viewY = 0; g_viewW = FBW; g_viewH = FBH;
                             renderFPStoFB(fb, gd, gd.levels[ep], scenePal, cam, meta, zbuf, faithful, inv); }
                if (interp) { cam.px = bpx; cam.py = bpy; cam.dirX = bdx; cam.dirY = bdy;
                              cam.planeX = bplx; cam.planeY = bply; cam.pitch = bpitch; cam.angI = bangI; }
            }
            // ОВЕРЛЕИ — внутри РЕГИОНА ИГРЫ (g_viewX/Y..+W/H); текст ×US (render-scale), иначе мельчает
            const int VX = g_viewX, VY = g_viewY, VW = g_viewW, VH = g_viewH, US = uiScale();
            if (!mapOpen) {                                     // на пауза-карте HUD-оверлеи не нужны
            // HUD-СООБЩЕНИЯ: ROM печатает в ЧЁРНУЮ ЖК-ПАНЕЛЬ кокпита (тайлы col2/row20-23 = пиксель 16,160
            // экрана 320×224; messages.hpp шапка). В reference-режиме кладём ТУДА (координата кокпита ×
            // масштаб региона); без кокпита (reference off) — старое место (низ-лево вида, над оружием).
            const uint32_t msgYellow = gd.heldPal.c[7];                       // ⭐ЖЁЛТЫЙ штрих как HP/счётчик врагов (idx7 #FCFC24)
            if (reference) { const int msc = VW / HUD_W;                      // 320→VW масштаб кокпита
                             msgs.draw(fb, VX + 16*msc, VY + 160*msc, msc, msgYellow, &gd.font); }
            else           msgs.draw(fb, VX + 14*US, VY + VH - 188*US, 2*US, msgYellow, &gd.font);
            (void)deathTimer;   // ZT: смерти НЕТ надписи (был нефейтфул «WASTED», не срабатывал) — только white-out + DECEASED
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
            // ⭐чистый заголовок (дебаг-инфа убрана по просьбе юзера; поза/режим — в консоли)
            char title[96];
            std::snprintf(title, sizeof(title), "%sztpp v%s — Zero Tolerance",
                paused ? "\xE2\x8F\xB8 PAUSED — " : "", ztppVersion());
            SDL_SetWindowTitle(win, title);
        } else if (dirty) {
            render(fb, gd, ep, floor, mode, grid, cam, meta, zbuf, faithful, reference, inv);
            con::draw(fb, FBW, FBH, presentRenderScale());   // оверлей консоли (поверх всего)
            SDL_UpdateTexture(tex, nullptr, fb.px.data(), FBW * sizeof(uint32_t));
            char title[96];
            std::snprintf(title, sizeof(title), "ztpp v%s — Zero Tolerance", ztppVersion());
            SDL_SetWindowTitle(win, title);
            dirty = false;
        }

        SDL_RenderClear(ren);
        SDL_SetTextureScaleMode(tex, presentLinear() ? SDL_ScaleModeLinear : SDL_ScaleModeNearest);  // фильтр
        { int ww = FBW, wh = FBH; SDL_GetWindowSize(win, &ww, &wh);
          SDL_Rect src, dst; presentRects(ww, wh, false, src, dst);   // ⭐ЕДИНЫЙ регион g_view для ВСЕХ экранов (меню в 448-регионе)
          SDL_RenderCopy(ren, tex, &src, &dst); }
        SDL_RenderPresent(ren);
        // лимит кадров: спим до конца кадрового интервала (1000/frameLimit мс). Экран выбора — 60fps (ZT VBLANK: плавный слайд).
        int fps = (selectActive || briefState.idx >= 0 || bootPhase >= 0) ? 60 : (frameLimit < 5 ? 5 : frameLimit);   // выбор/заставка/boot = 60fps (плавно)
        Uint32 target = (Uint32)(1000 / fps);
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
