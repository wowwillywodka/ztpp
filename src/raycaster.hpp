// ztpp — рейкастер (Фаза 2): вид от первого лица по реальной геометрии ZT.
//
// ВАЖНО: прагматичный DDA-рейкастер поверх данных ZT (карта, celltype-классификация,
// метатекстуры стен). НЕ пиксель-в-пиксель с Mega Drive (оригинал — ячеечный проектор
// со «скомпилированным» колоночным скейлером, render3d.asm). Классификация клеток и декод
// текстур — по канону ztextractor (см. cells.hpp, gfx.hpp::decodeTile, _build_metatexture).
#pragma once
#include "rom.hpp"
#include "gfx.hpp"
#include "level.hpp"
#include "cells.hpp"
#include "background_fx.hpp"   // рантайм-хелперы фона (bgSample/bgGradient/activeBg); Panorama — транзитивно из rom/background.hpp
#include "gamedata.hpp"          // GameData/WallBank (ЭТАП 0: данные, не Rom+адреса)
#include <cmath>
#include <cstdint>
#include <vector>
#include <unordered_map>
#include <algorithm>

static const uint16_t DOOR_METATEX = 18; // канонич. метатекстура двери (пустой texorder)

// ── ДВЕРИ: открытие при ВСТАВАНИИ НА ЯЧЕЙКУ двери (как ZT step-on b202: наступил на клетку ct6/7 →
// дверь в список открытия). НЕ «перед дверью» — именно НА её клетке. Створки разъезжаются (раздвижные),
// проходимы. Закрываются (анимация) когда игрок сошёл с клетки. doorMap/doorOpen/doorKey — в cells.hpp. ──
// Возвращает true, если какая-то дверь ТОЛЬКО ЧТО начала открываться (для звука двери — caller играет SFX_DOOR).
inline bool   rcUpdateDoors(int floor, double px, double py, const Level& lvl) {
    auto& m = doorMap();
    int cx = (int)px, cy = (int)py;
    bool justOpened = false;
    for (int y = cy - 3; y <= cy + 3; ++y)
        for (int x = cx - 3; x <= cx + 3; ++x) {
            if (x < 0 || y < 0 || x >= Level::W || y >= Level::H) continue;
            uint8_t ct = lvl.cellType(floor, x, y);
            if (ct != 6 && ct != 7) continue;
            bool onCell = (cx == x && cy == y);          // игрок СТОИТ на клетке двери (ZT step-on)
            int k = doorKey(floor, x, y);
            double o = m.count(k) ? m[k] : 0.0, prev = o;
            o += (onCell ? 0.20 : -0.10) * simDt();      // ⭐фаза в ROM-тиках (fps-инвариантно)
            if (o < 0) o = 0; if (o > 1) o = 1;
            if (onCell && prev < 0.01 && o > 0.0) justOpened = true;   // фронт открытия двери
            m[k] = o;
        }
    return justOpened;
}

// Общий коэффициент гор. растяжки дисплея (faithful И DDA). 1.0 = без растяжки.
// MD показывает узкий по ширине рендер растянутым → стены/двери ШИРОКИЕ. Ключи −/=. Дефолт 2.0.
inline double& faHStretch() { static double v = 2.2; return v; }

// ── ДЕКОР/ОБЪЕКТЫ-БИЛБОРДЫ (лампы/растения/мебель/столбы/деревья) ──────────────
// Объекты окружения ZT (класс иконки 16) — спрайты-билборды из банка объектов 0x10E9BE,
// стоящие в клетках карты. Рендер ТОЧНО как в игре (блиттер eda0 + хендлеры 0x11868..0x119be,
// дизассемблированы из ROM): билборд в ЦЕНТРЕ клетки X, z-тест против стен по колонке, прозрачный
// индекс 0, палитра-линия 0 (= wallPal). Тумблер faDecor.
//
// ВЕРТИКАЛЬ (из хендлеров; S = высота клетки на дистанции = высота полной стены, горизонт = центр вида):
//   eda0 рисует ВЕРХ на screenY, вниз на D0(высоту). База Ybase = пол = горизонт + S/2.
//   Каждый объект: top = горизонт + topOff·S, высота = hFrac·S, ширина(нат) = wFrac·S.
//   Напольные объекты заканчиваются НИЗОМ на полу; ЛАМПЫ (0x62/63/64/75) висят ВЕРХОМ на ПОТОЛКЕ
//   (top = горизонт − S/2). Доли — точные сдвиги/asr из дизасма хендлеров (в восьмых).
inline bool& faDecor()    { static bool v = true; return v; }   // показывать декор
inline int&  decorFrame() { static int  f = 0;    return f; }   // тик анимации декора (инкремент в main/кадр)

// Один спрайт билборда объекта: тайл банка + вертикаль (в 1/16 S; S = высота клетки на дист., горизонт =
// центр вида): topOff16 = (top−горизонт)·16/S, hFrac16 = высота·16/S, wFrac16 = ширина·16/ширина_клетки.
// anim: 0 статич.; 1 вентилятор (4 кадра: tile/tile+1 + hflip, дизасм 0x119be); 2 мигание (alt=tile−4, 0x1195e).
struct DecorSprite { uint8_t tile; int8_t topOff16; uint8_t hFrac16, wFrac16; uint8_t anim; };
struct DecorDef { DecorSprite s[2]; uint8_t count; };          // 1–2 спрайта (напр. лампа + отражение)

// celltype → объект (дизассемблировано из хендлеров 0x11868..0x119be). Доли — точные asr/sub в 1/16 S.
inline bool decorForCt(uint8_t ct, DecorDef& d) {
    switch (ct) {                          //  { {tile, topOff16, hFrac16, wFrac16, anim}, ... }, count
        case 0x37: d = {{{46,-8,16, 8,0}}, 1}; return true;  // Дерево        — пол, во всю высоту
        case 0x5C: d = {{{47,-8,16, 8,0}}, 1}; return true;  // Дерево
        case 0x5D: d = {{{48,-4,12, 6,0}}, 1}; return true;  // Напольная лампа — пол, 3/4 высоты
        case 0x5E: d = {{{55, 2, 6, 8,0}}, 1}; return true;  // Стол          — пол, низкий и ШИРОКИЙ
        case 0x5F: d = {{{55, 0, 8, 2,0}}, 1}; return true;  // Барный стул    — пол, полвысоты, узкий
        case 0x60: d = {{{51,-8,16, 4,0}}, 1}; return true;  // Металлич. столб — пол, во всю высоту, тонкий
        case 0x61: d = {{{54,-8,16, 4,0}}, 1}; return true;  // Бетонный столб
        case 0x76: d = {{{52,-8,16, 4,0}}, 1}; return true;  // Металлич. столб 2
        case 0x62: d = {{{53,-8, 4, 2,2}}, 1}; return true;  // Мигающая лампа — ПОТОЛОК (мигает 53↔49)
        case 0x63: d = {{{53,-8, 4, 2,0}}, 1}; return true;  // Лампа         — ПОТОЛОК
        // Полусфера-лампа: купол на ПОТОЛКЕ (56) + световое ОТРАЖЕНИЕ-пятно на полу (50, дизасм 0x1198a)
        case 0x64: d = {{{56,-8, 2, 4,0}, {50, 7, 2, 8,0}}, 2}; return true;
        case 0x75: d = {{{57,-8, 4, 8,0}}, 1}; return true;  // Лампа 2       — ПОТОЛОК, широкая
        case 0x78: d = {{{58, 7, 2, 8,1}}, 1}; return true;  // Напольный вентилятор (аним. tile 58/59)
        default:   return false;
    }
}

// ── ПИКАПЫ ОРУЖИЯ/ПРЕДМЕТОВ как floor-билборды (celltype 0x18..0x26) ──────────────
// Из дизасма диспетча предметов (хендлеры @0x115a0..0x11676, таблица @0x113F4): каждый item грузит
// a1 = указатель в объект-банк 0x10E9BE → тайл = (a1−0x10E9BE)/0x200; общий позиционер 0x11a52 ставит
// его на пол, размер ≈ S/4 (квадрат). idx = celltype−0x18 (как в weapons.hpp). Тайлы (idx→тайл):
inline bool itemBillboardForCt(uint8_t ct, DecorDef& d) {
    static const uint8_t TILE[15] = {0,3,8,10,11,15,4,13,14,7,5,6,12,18,19};  // Map..Pulse → тайл объект-банка
    int ic = cellIcon(ct);
    if (ic != 10 && ic != 11) return false;             // не оружие(10)/предмет(11)
    int idx = (int)ct - 0x18;
    uint8_t tile;
    if (idx >= 0 && idx < 15)      tile = TILE[idx];
    else if (ct == 0x36)           tile = 17;           // ПУЛЬС-ЛАЗЕР — пикап ct0x36 (хендлер 0x115ea → 0x110bbe → tile17)
    else if (ct == 0x82)           tile = 60;           // ОГНЕМЁТ — пикап ct0x82 (хендлер 0x115cc → 0x1161be → tile60)
    else return false;
    // ⭐РАЗМЕР пикапа (2026-07-15, VERIFIED дампом+юзером): ширина по cwd (мир-аспект = ROM X-растяжка дисплея ×2). ROM eda0-хелперы:
    //   обычные $11a52 s/4×s/4 → экран ШИРОКИЙ 2:1 (width=Sd/2 cwd, height=Sd/4); БИОСКАНЕР(0x19)/ОГНЕМЁТ(0x82) $11a3a s/4×s/2 → экран
    //   КВАДРАТ (height вдвое = Sd/2 компенсирует растяжку); огнезащ.костюм(0x1d) 0x11608 s/2×s/4 → экран ШИРОКИЙ 4:1 (width=Sd).
    uint8_t h = 4, w = 4;                                // обычные (огнетушитель/аптечка/оружие/...): ШИРОКИЙ 2:1 на экране ($11a52)
    if (ct == 0x19 || ct == 0x82) h = 8;                // ⭐биосканер/огнемёт: h вдвое → КВАДРАТ на экране (ZT $11a3a)
    else if (ct == 0x1d)          w = 8;                // огнезащ.костюм: w вдвое → ШИРОКИЙ 4:1 (ZT 0x11608)
    int8_t topOff = (int8_t)(8 - h);                    // НИЗ на полу (горизонт + S/2 = +8/16)
    d = {{{tile, topOff, h, w, 0}}, 1};
    return true;
}
// Хук «пикап уже подобран» (ставит weapons.hpp через pickedSet); null → показывать все.
inline bool (*&pickupHiddenFn())(int, int, int) { static bool (*p)(int, int, int) = nullptr; return p; }

// Динамические эффекты стрельбы в МИРЕ (взрывы-импакты по стене, снаряды) — билборды объект-банка.
// Заполняет updateShots (weapons.hpp) каждый кадр; рисуются в drawDecorBillboards. Доли в 1/16 S.
struct WorldFx { double wx, wy; int floor; uint8_t tile; int8_t topOff16; uint8_t hFrac16, wFrac16;
                 int sprId = -1; uint8_t efr = 0; bool corpse = false; float zlift = 0;
                 uint8_t dir = 0, animSt = 0, variant = 0; bool foam = false; bool burned = false; uint8_t atkPose = 0; bool overlay = false;
                 bool wide = false; };  // wide=огонь: ширина по cwd (X-растяжка дисплея ×2, ROM eda0), а не квадрат Sd. overlay=поверх стен (сейчас не исп.)
inline std::vector<WorldFx>& worldFx() { static std::vector<WorldFx> v; return v; }

#include "tuning.hpp"   // gameDist/playerPhysics/gameDistOctagonal — общие тумблеры reference-точности

#include "camera.hpp"   // struct Camera (вынесена в отдельный заголовок)

// ── СОЛИДНЫЙ ДЕКОР (ROM a1d2→a33a/a38c, VERIFIED): цилиндр-выталкивание игрока из ЦЕНТРА клетки декора.
// Мелкие (0x5D лампа/0x5F стул/0x60,0x76 столбы): dist<0x28 → pos = центр + d·0x30/(dist+1).
// Крупные (0x37,0x5C деревья/0x5E стол/0x61 столб): dist<0x58 → pos = центр + d·0x60/(dist+1).
// dist = (|dx|+|dy|+max)/2 (d7c0); потолочные лампы/вентилятор (0x62-64,0x75,0x78) хендлера НЕ имеют.
inline void rcDecorPush(Camera& c, const Level& lvl) {
    int cx = (int)c.px, cy = (int)c.py;
    if (cx < 0 || cy < 0 || cx >= Level::W || cy >= Level::H) return;
    uint8_t ct = lvl.cellType(c.floor, cx, cy);
    int thr, r;
    switch (ct) {
        case 0x5D: case 0x5F: case 0x60: case 0x76: thr = 0x28; r = 0x30; break;
        case 0x37: case 0x5C: case 0x5E: case 0x61: thr = 0x58; r = 0x60; break;
        default: return;
    }
    int px = (int)std::lround(c.px * 256.0), py = (int)std::lround(c.py * 256.0);
    int ctrX = (px & ~0xFF) | 0x80, ctrY = (py & ~0xFF) | 0x80;
    int dx = px - ctrX, dy = py - ctrY;
    int ax = std::abs(dx), ay = std::abs(dy);
    int dist = (ax + ay + (ax > ay ? ax : ay)) >> 1;            // approx-дистанция d7c0
    if (dist >= thr) return;
    int nd = dist + 1;
    px = ctrX + dx * r / nd; py = ctrY + dy * r / nd;           // divs = trunc к нулю (как ROM)
    c.px = px / 256.0; c.py = py / 256.0; c.pxI = px; c.pyI = py;
}

// Кэш метатекстур стены 128x64 (4x2 тайла 32x32). Ключ — индекс метатекстуры эпизода.
// Раскладка: тайл i -> col=i/2, row=i%2 (верх=0,2,4,6 / низ=1,3,5,7) — как _build_metatexture.
struct MetaCache {
    const WallBank* wall = nullptr;          // банк текстур стен (данные, не Rom)
    const WallBank* obj  = nullptr;          // банк графики объектов/декора (билборды)
    const Level*    lvl  = nullptr;
    const uint8_t*  shadeRampData = nullptr; // регион рамп затенения (для ShadeRamps)
    const uint8_t*  fcTemplateData = nullptr; // шаблоны пол/потолок (5 env × 0xA0, для FloorCeil)
    const uint16_t* texdefSrc = nullptr;     // живой texdef аниматора (256*8); null → база уровня
    std::unordered_map<uint16_t, std::vector<uint8_t>> cache;

    void reset(const Level* l) { lvl = l; cache.clear(); } // вызывать при смене эпизода
    void invalidate(uint16_t meta) { cache.erase(meta); }  // аниматор: метатекстура изменилась

    // Текущий тайл слота метатекстуры: из живого texdef аниматора, иначе из базы уровня.
    uint16_t texdefAt(uint16_t meta, int sub) const {
        return texdefSrc ? texdefSrc[(static_cast<size_t>(meta & 0xFF) * 8) + sub]
                         : lvl->texdef(meta, sub);
    }

    const uint8_t* get(uint16_t meta) {
        auto it = cache.find(meta);
        if (it != cache.end()) return it->second.data();
        std::vector<uint8_t> buf(128 * 64, 0);
        uint8_t t[32 * 32];
        for (int i = 0; i < 8; ++i) {
            wall->decode(texdefAt(meta, i), t);
            int ox = (i / 2) * 32, oy = (i % 2) * 32;
            for (int y = 0; y < 32; ++y)
                for (int x = 0; x < 32; ++x)
                    buf[(oy + y) * 128 + ox + x] = t[y * 32 + x];
        }
        auto r = cache.emplace(meta, std::move(buf));
        return r.first->second.data();
    }
};

// ── АНИМАТОР СТЕН (по дизасму ZT FUN_000023c2 / BZT FUN_000022a6) ──
// Держит ЖИВУЮ копию texdef эпизода и циклит кадры анимаций по таймеру period (как RAM-копия игры
// $FF63C6). Кадр перезаписывает слоты texdef → метатекстура помечается грязной в MetaCache (пересборка
// при следующем get). Применение КУМУЛЯТИВНО: live не сбрасывается между кадрами (пустой кадр = без
// изменений), как в оригинале. Один на эпизод; init при смене эпизода, update() раз в игр.кадр.
struct WallAnimator {
    const Level* lvl = nullptr;
    MetaCache*   mc  = nullptr;
    std::vector<uint16_t> live;              // живой texdef 256*8 (мутирует аниматор)
    std::vector<int> frameIdx, counter;      // на каждую анимацию: текущий кадр + счётчик до смены
    bool active = false;

    // Применить кадр ai: записать его подстановки в live + инвалидировать затронутые метатекстуры.
    void applyFrame(size_t ai, int fi) {
        const auto& fr = lvl->wallAnims[ai].frames[(size_t)fi];
        for (const auto& s : fr) {
            uint16_t off = s.first;                          // байт-смещение в texdef
            if (off + 1 >= live.size() * 2) continue;
            size_t w = off / 2;                              // индекс тайл-слова = meta*8 + sub
            if (w >= live.size()) continue;
            live[w] = s.second;
            if (mc) mc->invalidate((uint16_t)(off / 16));    // метатекстура = off/16
        }
    }

    // (Пере)инициализация для эпизода: копируем базу texdef, подключаем как источник MetaCache,
    // применяем кадр 0 каждой анимации (базовое состояние).
    void init(const Level& l, MetaCache& cache) {
        lvl = &l; mc = &cache;
        live.assign(l.texdefT, l.texdefT + 256 * 8);
        size_t na = l.wallAnims.size();
        frameIdx.assign(na, 0);
        counter.assign(na, 0);
        for (size_t i = 0; i < na; ++i) counter[i] = l.wallAnims[i].period < 1 ? 1 : l.wallAnims[i].period;
        active = na > 0;
        mc->lvl = &l;                                        // согласуем эпизод
        mc->texdefSrc = live.data();                         // рендер берёт живой texdef
        mc->cache.clear();                                   // полная пересборка под новый источник
        for (size_t i = 0; i < na; ++i)
            if (!l.wallAnims[i].frames.empty()) applyFrame(i, 0);
    }

    // Один игровой кадр: для каждой анимации тикаем счётчик; на нуле — следующий кадр (циклично).
    void update() {
        if (!active || !lvl) return;
        for (size_t ai = 0; ai < lvl->wallAnims.size(); ++ai) {
            const auto& an = lvl->wallAnims[ai];
            int nf = (int)an.frames.size();
            if (nf < 1) continue;
            if (--counter[ai] > 0) continue;
            // ×wallAnimSlow(): ZT-аниматор (0x23c2) вызывался раз в игр.кадр, но рейкастер реально шёл ~30fps; порт на
            // 60fps крутил бы анимацию вдвое быстрее (period=1 → строб 60Гц). Множитель настраивается в меню (дефолт 2.0).
            int base = an.period < 1 ? 1 : an.period;
            int eff = (int)(base * wallAnimSlow() + 0.5); if (eff < 1) eff = 1;
            counter[ai] = eff;
            frameIdx[ai] = (frameIdx[ai] + 1) % nf;
            applyFrame(ai, frameIdx[ai]);
        }
    }
};

// Стена для ЛУЧА (стены+двери) — луч останавливается и рисует текстуру.
inline bool rcSolidRay(const Level& lvl, int floor, int cx, int cy) {
    if (cx < 0 || cy < 0 || cx >= Level::W || cy >= Level::H) return true;
    return cellRenderWall(lvl.cellType(floor, cx, cy));
}
// Блокирует ДВИЖЕНИЕ (стены, но не двери).
inline bool rcSolidMove(const Level& lvl, int floor, int cx, int cy) {
    if (cx < 0 || cy < 0 || cx >= Level::W || cy >= Level::H) return true;
    return cellBlocks(lvl.cellType(floor, cx, cy));
}

inline uint32_t shade(uint32_t c, double f) {
    if (f < 0.0) f = 0.0; if (f > 1.0) f = 1.0;
    uint32_t r = (uint32_t)(((c >> 16) & 0xFF) * f);
    uint32_t g = (uint32_t)(((c >> 8) & 0xFF) * f);
    uint32_t b = (uint32_t)((c & 0xFF) * f);
    return 0xFF000000u | (r << 16) | (g << 8) | b;
}

// Стартовая клетка: самая «открытая» пустая клетка (icon 0); взгляд вдоль длинного коридора.
void rcSpawn(Camera& cam, const Level& lvl);   // тело в raycaster.cpp

// ── Межэтажные переходы: ЛИФТЫ и ЛЕСТНИЦЫ (по дизасму ZT) ────────────────────
// Этаж = индекс (cam.floor 0..15); карта = lvl.cellId(floor,...). В ZT: индекс этажа $FF1028,
// карта $FF7CD6+floor*0x400; FUN_0xb862 (этаж+1 = вниз по зданию)/FUN_0xb800 (этаж-1 = вверх)
// перезагружают этаж и спавнят его врагов; FUN_0xb8fc грузит этаж D0. Шаг-он диспетчер 0xa1d2.
// Лифт = одна шахта (x,y) на этажах: celltype «down» 0x33/52/56/5a → этаж+1, «up» 0x32/51/55/59
// → этаж-1, «up/down» 0x34/53/57/5b → авто по соседнему этажу. Едет, пока след. клетка того же
// направления (многоэтажный), прибытие на лифт противоположного напр. (down→up). Подтверждено:
// e1 D5(0x56)→этаж+1 D6(0x55); 99(0x5a)→9A(0x59). Лестница: interst-down 0x16→этаж+1, up 0x15→-1.
// ТОЧНЫЙ набор клеток ШАХТЫ лифта (НЕ весь диапазон 0x30-0x5b!): направленные кабины (step-on idx3
// @0xa20a) + стены/площадки шахты. Иначе ловит НЕ-лифтовые клетки в этом диапазоне (оружие 0x36
// «лазер-винтовка», декор 0x37/0x5c-0x5f, клетки лестниц 0x3c/0x3e/0x3f/0x44/0x46/0x47/0x4c/0x4e/0x4f)
// → им рисовался синий void (баг «весь фон синий на ячейке оружия»). Лифты idx3: 0x32-34/51-53/55-57/59-5b.
inline bool rcElevCell(uint8_t ct) {
    return (ct >= 0x32 && ct <= 0x34) || (ct >= 0x51 && ct <= 0x53) ||
           (ct >= 0x55 && ct <= 0x57) || (ct >= 0x59 && ct <= 0x5b) ||   // направленные кабины
           ct == 0x30 || ct == 0x31 || ct == 0x50 || ct == 0x54 || ct == 0x58;  // стены/площадки шахты
}
// Целтайп ПЕРВОЙ стена-рендер клетки на пути луча колонны x (упрощённый осевой DDA, без дверей/диагоналей).
// Нужно рендеру лифта: отличить колонну «стена кабины» (rcElevCell) от «вид на выход» (коридор) для
// фильмстрип-скролла в faithful-блите (cellRenderWall — из cells.hpp).
inline uint8_t rcColumnHitCt(const Level& lvl, const Camera& cam, int x, int W, double hs) {
    double cameraX = (2.0 * x / W - 1.0) / hs;
    double rayX = cam.dirX + cam.planeX * cameraX;
    double rayY = cam.dirY + cam.planeY * cameraX;
    int mapX = (int)cam.px, mapY = (int)cam.py;
    double dDistX = (rayX == 0) ? 1e30 : std::fabs(1.0 / rayX);
    double dDistY = (rayY == 0) ? 1e30 : std::fabs(1.0 / rayY);
    int stepX, stepY; double sideX, sideY;
    if (rayX < 0) { stepX = -1; sideX = (cam.px - mapX) * dDistX; } else { stepX = 1; sideX = (mapX + 1.0 - cam.px) * dDistX; }
    if (rayY < 0) { stepY = -1; sideY = (cam.py - mapY) * dDistY; } else { stepY = 1; sideY = (mapY + 1.0 - cam.py) * dDistY; }
    for (int g = 0; g < 256; ++g) {
        if (sideX < sideY) { sideX += dDistX; mapX += stepX; } else { sideY += dDistY; mapY += stepY; }
        if (mapX < 0 || mapY < 0 || mapX >= Level::W || mapY >= Level::H) return 1;
        uint8_t ct = lvl.cellType(cam.floor, mapX, mapY);
        if (cellRenderWall(ct)) return ct;
    }
    return 1;
}
// «Кабина» — направленные клетки шахты (step-on idx3 @0xa3ec, где центрируется игрок). Площадки
// (area 0x31/50/54/58) и стены (0x30) сюда НЕ входят: сойдя с кабины на площадку, игрок выходит.
inline bool rcElevCabin(uint8_t ct) {
    return (ct >= 0x32 && ct <= 0x34) || (ct >= 0x51 && ct <= 0x53) ||
           (ct >= 0x55 && ct <= 0x57) || (ct >= 0x59 && ct <= 0x5b);
}
inline int  rcElevDepart(uint8_t ct) {                 // направление старта поездки (этаж)
    if (ct == 0x33 || ct == 0x52 || ct == 0x56 || ct == 0x5a) return +1;  // вниз по зданию (floor++)
    if (ct == 0x32 || ct == 0x51 || ct == 0x55 || ct == 0x59) return -1;  // вверх по зданию (floor--)
    return 0;
}
inline bool rcElevArrival(uint8_t ct, int dir) {       // клетка-прибытие для текущего напр. поездки
    if (dir > 0) return (ct == 0x32 || ct == 0x51 || ct == 0x55 || ct == 0x59); // ехали вниз → стоп на up
    else         return (ct == 0x33 || ct == 0x52 || ct == 0x56 || ct == 0x5a);
}
// Клетка-переход лестницы (0x17/0x3f/0x47/0x4f): свап по суб-позиции вдоль оси + гистерезис cabin.
inline void rcStairTrans(Camera& c, double sub, bool needHigh) {
    bool high = (sub >= 128.0);
    if (c.cabin != 64.0) {                              // ещё не «вверху» → можно подняться
        if (needHigh ? high : !high) { c.cabin = 64.0;  if (c.floor > 0)  c.floor--; }   // этаж-1 (вверх)
    } else {                                            // cabin==64 → можно спуститься
        if (needHigh ? !high : high) { c.cabin = -64.0; if (c.floor < 15) c.floor++; }   // этаж+1 (вниз)
    }
}
// ── КЛАССЫ КЛЕТОК ЛИФТА (ZT step-on таблица @e420, автомат b768/b7aa) ──────────
// up-кабина: старт UP (elevState=+1, floor--). ОНА ЖЕ клетка-прибытия для DOWN-поездки (b7da).
inline bool ctElevUp  (uint8_t ct){ return ct==0x32||ct==0x51||ct==0x55||ct==0x59; }
// down-кабина: старт DOWN (elevState=-1, floor++). ОНА ЖЕ клетка-прибытия для UP-поездки (b798).
inline bool ctElevDown(uint8_t ct){ return ct==0x33||ct==0x52||ct==0x56||ct==0x5a; }
inline bool ctElevUpdn(uint8_t ct){ return ct==0x34||ct==0x53||ct==0x57||ct==0x5b; }  // updown → toggle (e75e)
inline bool ctElevArea(uint8_t ct){ return ct==0x31||ct==0x50||ct==0x54||ct==0x58; }  // площадка → стоп (e7d6)

// Лестничный наклон cabin по суб-позиции игрока (ZT @b51a: b612/b664/... задают $FF116E из subpos).
// Возвращает false, если ct — не лестничная клетка.
bool rcStairCabin(Camera& c, uint8_t ct, double subX, double subY);   // тело в raycaster.cpp

// ── ROM-ТОЧНЫЙ АВТОМАТ ПЕРЕХОДОВ (faElevZT) ───────────────────────────────────
// Дизасм ZT (тройная сверка): step-on e3f8→@e420 (старт/стоп), b4e4→b768/b7aa (рампа кабины +
// своп этажа), a3ec (центрирование). elevState: 0 idle | ±1 едет | ±2 приехал-стоп. cabin −64..+64.
// ⚠ Знак elevState = ROM-семантика (+1=UP/floor--, -1=DOWN/floor++); вне этой функции нигде не читается.
bool rcUpdateTransitZT(Camera& c, const Level& lvl, bool enteredNewCell);   // тело в raycaster.cpp

// Вызывать КАЖДЫЙ кадр. enteredNewCell=true в кадре, когда игрок вошёл в новую клетку (x,y).
// Возвращает true, если движение игрока заблокировано (идёт поездка лифта; в ROM-режиме — всегда false).
bool rcUpdateTransit(Camera& c, const Level& lvl, bool enteredNewCell);   // тело в raycaster.cpp

inline void rcRotate(Camera& c, double a) {
    double s = std::sin(a), co = std::cos(a);
    double ox = c.dirX;   c.dirX   = ox * co - c.dirY * s;     c.dirY   = ox * s + c.dirY * co;
    double op = c.planeX; c.planeX = op * co - c.planeY * s;   c.planeY = op * s + c.planeY * co;
}

// Точечная проверка движения с УЧЁТОМ диагоналей (game-true, FUN_e1b2 / таблица e23e).
inline bool rcBlockedPt(const Level& lvl, int floor, double px, double py) {
    int cx = (int)px, cy = (int)py;
    if (cx < 0 || cy < 0 || cx >= Level::W || cy >= Level::H) return true;
    return cellBlockedAt(lvl.cellType(floor, cx, cy), px - cx, py - cy);
}

inline void rcMove(Camera& c, const Level& lvl, double dx, double dy, bool noclip = false) {
    if (noclip) { c.px += dx; c.py += dy; return; }
    const double r = 0.18;
    double nx = c.px + dx;
    if (!rcBlockedPt(lvl, c.floor, nx + (dx > 0 ? r : -r), c.py)) c.px = nx;
    double ny = c.py + dy;
    if (!rcBlockedPt(lvl, c.floor, c.px, ny + (dy > 0 ? r : -r))) c.py = ny;
}

// ── ZT-ФИЗИКА ДВИЖЕНИЯ ИГРОКА (DAB8, collision_move.asm dc3c..dd0c) ──
// Три значения скорости плавно идут к ЦЕЛИ (ввод): при цель≠0 разгон, при цель=0 — затухание ÷2/кадр.
//   • Поворот  -$7182 → цель ±16 (ед/512=окружность), разгон ÷8.
//   • Вперёд   -$717e → цель +40/+45(бег) / −20/−35(назад), разгон ÷16.
//   • Стрейф   -$717a → цель ±40 (A+Left/Right), разгон ÷16.
// Присед/нокдаун (питч<0) ХАЛВИТ вперёд+стрейф (asr -$717c/-$7178). Применение через sin/cos.
namespace ztmove {
    constexpr double PI2 = 6.28318530717958647692;
    constexpr double U   = 1.0 / 256.0;          // ZT 8.8 fixed → клетки
    constexpr double A   = PI2 / 512.0;          // ZT угл.ед. → радианы (512 = окружность)
    constexpr double FWD = 40 * U, FWD_RUN = 45 * U, BACK = -20 * U, BACK_RUN = -35 * U, STRAFE = 40 * U;
    constexpr double TURN = 16 * A;              // пиковая цель угл.скорости
}
// КВАНТ. УГЛА 512 ШАГОВ (ROM: угол 0..511 andi #$1ff, dir/plane из LUT sin/cos @0x8124). Аккумулятор ang512
// копит ПЛАВНУЮ угл.скорость (turnVel), а dir берётся от ОКРУГЛЁННОГО до целого шага угла → 512 дискретных
// направлений (дискретный поворот оригинала). angDelta512 — приращение угла в 512-ед. (turnVel рад → 512-ед).
inline void camSetAngleU(Camera& c, double ang512) {
    while (ang512 < 0.0)     ang512 += 512.0;
    while (ang512 >= 512.0)  ang512 -= 512.0;
    c.ang512 = ang512;
    int q = ((int)std::lround(ang512)) & 511;                  // ОКРУГЛЕНИЕ до шага (512 дискретных)
    double th = q * (ztmove::PI2 / 512.0);
    c.dirX = std::cos(th); c.dirY = std::sin(th);
    c.planeX = -c.dirY * 0.66; c.planeY = c.dirX * 0.66;
}
inline double camDirToAng512(const Camera& c) {
    double a = std::atan2(c.dirY, c.dirX); if (a < 0) a += ztmove::PI2;
    return a / ztmove::PI2 * 512.0;
}
// ZT-ease: цель≠0 → cur += (цель−cur)·accel (разгон); цель=0 → cur ·= 0.5 (затухание/2, инерция выбега).
inline double ztEase(double cur, double target, double accel) {
    if (target != 0.0) { cur += (target - cur) * accel; }
    else { cur *= 0.5; if (cur < 1e-4 && cur > -1e-4) cur = 0.0; }
    return cur;
}
// Полный шаг движения с ИНЕРЦИЕЙ. Явные намерения ввода (уже разрешённые из раскладки клавиш).
void rcMovePhysics(Camera& c, const Level& lvl, bool fwd, bool back, bool strafeL, bool strafeR, bool turnL, bool turnR, bool runMod, bool halveMove, bool noclip);   // тело в raycaster.cpp

// Дальность видимости по режиму темноты env (0 Bright..4 Black) — клаустрофобный полумрак ZT.
inline double rcMaxVis(int envMode) {
    switch (envMode) {
        case 0: return 13.0; case 1: return 9.0; case 2: return 6.5;
        case 3: return 12.0; case 4: return 4.0; default: return 10.0;
    }
}

// ТОЧНАЯ таблица ZT height→band, извлечена статически из скейлер-таблицы 0x4392 → per-height
// рутины (0x4766..0x700c), у каждой зашит слот рампы (movea.l (d16,A6),A3). h = высота колонны
// (= D0 = scale = 0x10000/(forward>>6) = 64/forward). h≥42: рутины CLUT не грузят → банд 0.
inline int bandForHeight(int h) {
    if (h >= 38) return 0;
    if (h >= 34) return 1;
    if (h >= 30) return 2;
    if (h >= 26) return 3;
    if (h >= 22) return 4;
    if (h >= 16) return 5;
    if (h >= 10) return 6;
    return 7;                       // h<10 (далеко) → самый тёмный
}

// ⭐band СПРАЙТА (eda0 @0xedd0, отдельно от стен; ВЕРИФИЦИРОВАНО побайтно capstone+ROM): band=ed50_table[base_idx+t].
//   base_idx — по ОСВЕЩЕНИЮ этажа (указатель $FF1096, ладдер 0x1d66→блоки, все 5 записей сверены):
//     env0 Bright→ed50(idx0, band0 ВСЕГДА), env1 Dim→ed64(idx5), env2 Haze→ed78(idx10),
//     env3 NoCeil→ed64(idx5), env4 Black→ed64(idx5). ⚠ ed8c(band11) — НЕ per-floor, а блэкаут-режим ($FF105E).
//   t = max(0, 5-(scale>>3)), scale=64/дист (C=64 верифиц.); scale<8 (>8 клеток) в ROM = CULL (тут клампим t=4).
// ed50-таблица (idx→band, из ROM 0xed50): 0-4→0, 5→1, 6→2, 7→3, 8→4, 9→5, 10→6, 11→7, 12→8, 13→9, 14→10, 15+→11.
inline int ed50Band(int idx) {
    static const int B[20] = {0,0,0,0,0,1,2,3,4,5,6,7,8,9,10,11,11,11,11,11};
    if (idx < 0) idx = 0; if (idx > 19) idx = 19;
    return B[idx];
}
inline int bandForSprite(double distCells, int envMode) {
    static const int BASE[5] = {0, 5, 10, 5, 5};    // Bright,Dim,Haze,NoCeil,Black → idx базы (1/3/4 общий ed64)
    int base = BASE[envMode < 0 ? 0 : (envMode > 4 ? 4 : envMode)];
    if (base == 0) return 0;                          // Bright: band0 всегда (ROM beq $edf4 пропускает t)
    double scale = faSpriteScaleC() / (distCells < 1e-3 ? 1e-3 : distCells);
    if (scale > 300.0) scale = 300.0;                // кламп 0x12C (софт-скейлер не растит бесконечно)
    int s3 = (int)scale >> 3;
    int t = 5 - s3; if (t < 0) t = 0; if (t > 4) t = 4;   // ztpp не отбраковывает (drawDist свой) → клампим t
    return ed50Band(base + t);
}

// ДАЛЬНОСТЬ ПРОРИСОВКИ (cull): сеттер 0x1d66 ставит -0x714c по env; рендер @0xba60 клампит X/Y-смещение
// клетки от камеры к [−dist,+dist] (box/Чебышёв) → клетки-стены дальше dist НЕ рисуются (за ними фон/тёмный
// градиент). env: Bright 16 (≈весь уровень), Dim/Haze/Black 5, No-ceiling 12.
inline int drawDistForEnv(int env) {
    switch (env) { case 0: return 16; case 3: return 12; default: return 5; }  // 1/2/4 → 5
}
inline int& faDrawDist() { static int v = -1; return v; }   // override дальности (cull); <0 = по env (--drawdist)
// faStairK() / faStairUni() — в tuning.hpp (общие тюнинги, слайдеры меню)

// ДИАГОНАЛЬ ЛЕСТНИЦЫ (профиль, дровер D0C4): хендлеры стен-лестниц 0x0c-0x11 кладут на каждую грань СЛОВО
// (L,R)=байт-смещения левого/правого края; дровер сдвигает выборку ТЕКСТУРЫ по вертикали поколоночно
// (интерполяция L→R по ширине грани × 1/z) → «искажённая по диагонали» текстура на наклонных гранях,
// обычная на плоских (L==R). Селектор asc/desc = знак -0x6e92 (под-позиция камеры в клетке).
// Клетки-СТЕНЫ лестницы с профиль-скосом: 0x0c-0x11 (низ этажа) + повороты 0x38-3b/40-43/48-4b (верх/12-19).
inline bool isStairProfileCT(uint8_t ct) {
    return (ct >= 0x0c && ct <= 0x11) || (ct >= 0x38 && ct <= 0x3b) ||
           (ct >= 0x40 && ct <= 0x43) || (ct >= 0x48 && ct <= 0x4b);
}
// НАКЛОН ПИТЧА НА ЛЕСТНИЦЕ (главная механика спуска, дизасм BAEE: -0x71e6 -= -0x6e92). Стоя на клетке-лестницы
// игра наклоняет камеру по под-позиции в клетке (B4E4 → таблица B51A). Возвращает добавку к питчу (= −V).
// Формулы V (-0x6e92) per celltype (camXf/camYf = дробь*256, 0..255): спуск-клетки дают плавный переход.
inline double& stairPitchState() { static double g = 0.0; return g; }      // персистентный -0x6e92 (эволюция)
inline double& stairEasedPitch()  { static double v = 0.0; return v; }      // СГЛАЖЕННЫЙ питч (лерп к цели ±4/кадр)
inline double& faStairPitchOverride() { static double v = 1e9; return v; }  // CLI-оверрайд для статик-теста (1e9=выкл)
// faStairUni() — в tuning.hpp
inline bool&   faStairOff() { static bool b = false; return b; }            // ДИАГ: отключить спец-обработку лестницы
inline double stairPitchAdjust(const Level& lvl, int floor, double px, double py) {
    int cx = (int)px, cy = (int)py;
    if (cx < 0 || cy < 0 || cx >= Level::W || cy >= Level::H) return 0.0;
    uint8_t ct = lvl.cellType(floor, cx, cy);
    double yf = (py - cy) * 256.0, xf = (px - cx) * 256.0;
    double& g = stairPitchState();
    // обновляем персистентный g по celltype (дизасм B612/B664/B65E/B6B0/B6B8/B6C0). 0x17 — ГИСТЕРЕЗИС: держит
    // предыдущее g, ставит 64 только если xf>=128 (camX_frac<0 как signed). Спуск 0x12/0x14 — плавно по yf.
    switch (ct) {
        case 0x12: g = (yf - 255.0) / 4.0;  break;   // спуск (ось Y): -63..0
        case 0x14: g = (255.0 - yf) / 4.0;  break;   // спуск (ось Y): 0..63
        case 0x13: g = 0.0;   break;                 // ровно
        case 0x15: g = -64.0; break;                 // вверх
        case 0x16: g = 64.0;  break;                 // вниз
        case 0x17: if (g != 64.0 && xf >= 128.0) g = 64.0;  break;  // держит предыдущее (гистерезис)
        default:   return 0.0;                       // не лестница → без наклона (g сохраняется)
    }
    return g;    // ЦЕЛЬ питча (мгновенная -0x6e92); сглаживание/оверрайд — в renderFaithful
}
// Клетки-ШАХТЫ/СПУСКА: их ПОЛ (и потолок-пустота) = синяя пустота пролёта/шахты. Спуск-сегменты 0x12-0x14
// (наклонный пол лестницы) — синий пол; floor-casting в блите красит их пол/потолок в VOID_BLUE (не барьер-полосы).
inline bool isShaftFloorCT(uint8_t ct) { return ct >= 0x12 && ct <= 0x14; }
// Все ПРОХОДИМЫЕ клетки лестницы (спуск 0x12/0x14 + синий 0x13 + ровные смещённые 0x15-0x17): пол ОПУЩЕН.
inline bool isStairFloorCT(uint8_t ct)   { return ct >= 0x12 && ct <= 0x17; }
// НИЗКОЕ направление клетки-спуска (открытая грань градиент-сегмента → туда пол УХОДИТ ВНИЗ). {0,0}=не спуск.
inline void descentLowDir(uint8_t ct, int& ddx, int& ddy) {
    ddx = 0; ddy = 0;
    switch (ct) {
        case 0x12: case 0x14: case 0x55: case 0x56: case 0x57: ddy = 1;  break;  // South
        case 0x3c: case 0x3e: case 0x59: case 0x5a: case 0x5b: ddy = -1; break;  // North
        case 0x44: case 0x46: case 0x32: case 0x33: case 0x34: ddx = -1; break;  // West
        case 0x4c: case 0x4e: case 0x51: case 0x52: case 0x53: ddx = 1;  break;  // East
    }
}
// НАПРАВЛЕНИЕ УГЛУБЛЕНИЯ спуска (куда РАСТЁТ cabin = пол ниже) по celltype слоупа. {0,0}=не слоуп.
// (по формулам cabin в rcUpdateTransit: 0x14=(255-subY)/4 растёт при y↓ → North, и т.д.)
inline void descentDeepDir(uint8_t ct, int& ddx, int& ddy) {
    ddx = 0; ddy = 0;
    switch (ct) {
        case 0x14: case 0x3c: ddy = -1; break;   // North (cabin↑ при y↓)
        case 0x12: case 0x3e: ddy =  1; break;   // South
        case 0x4e: case 0x44: ddx = -1; break;   // West
        case 0x4c: case 0x46: ddx =  1; break;   // East
    }
}
// Направление углубления СЕКЦИИ возле (cx,cy): берём слоуп-клетку рядом (сама/сосед). false=не нашли.
inline bool stairSectionDeepDir(const Level& lvl, int floor, int cx, int cy, int& ddx, int& ddy) {
    auto chk = [&](int x, int y) -> bool {
        if (x < 0 || y < 0 || x >= Level::W || y >= Level::H) return false;
        descentDeepDir(lvl.cellType(floor, x, y), ddx, ddy);
        return (ddx || ddy);
    };
    if (chk(cx, cy)) return true;
    for (int r = 1; r <= 2; ++r)
        for (int dy = -r; dy <= r; ++dy) for (int dx = -r; dx <= r; ++dx)
            if ((dx || dy) && chk(cx + dx, cy + dy)) return true;
    return false;
}
// КАРТА ВЫСОТ ПОЛА ЛЕСТНИЦЫ (целое: больше = выше пол): уровни клеток через пропагацию по спускам. Клетка-спуск
// роняет пол на 1 в свою низкую сторону (по descentLowDir). Лестница часто ЗАМКНУТА стенами (вход = спуск с
// уровня выше) — поэтому НЕ ищем «обычный пол», а распространяем относительные уровни от любой клетки.
inline void buildStairHeightMap(const Level& lvl, int floor, std::vector<int>& ht) {
    const int W = Level::W, H = Level::H;
    const int UNSET = -100000;
    ht.assign((size_t)W * H, 0);
    std::vector<int> level((size_t)W * H, UNSET), q;
    const int dx[4] = {1, -1, 0, 0}, dy[4] = {0, 0, 1, -1};
    auto isS = [&](int x, int y) { return x >= 0 && y >= 0 && x < W && y < H && isStairFloorCT(lvl.cellType(floor, x, y)); };
    for (int sy = 0; sy < H; ++sy) for (int sx = 0; sx < W; ++sx) {
        if (!isS(sx, sy) || level[(size_t)sy * W + sx] != UNSET) continue;
        level[(size_t)sy * W + sx] = 0; q.clear(); q.push_back(sy * W + sx);   // новая компонента
        for (size_t i = 0; i < q.size(); ++i) {
            int c = q[i], cx = c % W, cy = c / W;
            int adx, ady; descentLowDir(lvl.cellType(floor, cx, cy), adx, ady);
            for (int d = 0; d < 4; ++d) { int nx = cx + dx[d], ny = cy + dy[d];
                if (!isS(nx, ny) || level[(size_t)ny * W + nx] != UNSET) continue;
                int bdx, bdy; descentLowDir(lvl.cellType(floor, nx, ny), bdx, bdy);
                int dl = 0;                                            // разница уровня A(c)→B(n)
                if ((adx || ady) && dx[d] == adx && dy[d] == ady) dl = -1;          // B — низкая сторона A
                if ((bdx || bdy) && dx[d] == -bdx && dy[d] == -bdy) dl += 1;        // A — низкая сторона B
                level[(size_t)ny * W + nx] = level[c] + dl;
                q.push_back(ny * W + nx);
            }
        }
    }
    for (size_t i = 0; i < (size_t)W * H; ++i) ht[i] = (level[i] == UNSET) ? 0 : level[i];
}
// ПРОФИЛЬ-СКОС граней лестницы ROM (хендлеры 945e-9818, §9.1 + агент 2026-07-10). По CELLTYPE и знаку
// cabin: 4 слова граней [N,W,E,S] (порядок ГРАНЕЙ ПОРТА), слово=(L<<8|R) знаковые сдвиги вершин колонн.
// STAIR_PROF[ctIdx][branch][face]; branch 0=cabin>0, 1=cabin<=0; face 0=N,1=W,2=E,3=S.
static const uint16_t STAIR_PROF[18][2][4] = {
    {{0x4040,0x7f7f,0x407f,0x7f40},{0xc0c0,0x0000,0xc000,0x00c0}},  // 0x0C
    {{0x4040,0x4040,0x4040,0x4040},{0xc0c0,0xc0c0,0xc0c0,0xc0c0}},  // 0x0D
    {{0x4040,0x4040,0x4040,0x4040},{0xc0c0,0xc0c0,0xc0c0,0xc0c0}},  // 0x0E
    {{0x4040,0x4040,0x4040,0x4040},{0xc0c0,0xc0c0,0xc0c0,0xc0c0}},  // 0x0F
    {{0x4040,0x0000,0x4000,0x0040},{0xc0c0,0x8080,0xc080,0x80c0}},  // 0x10
    {{0x4040,0x7f00,0x407f,0x0040},{0xc0c0,0x0080,0xc000,0x80c0}},  // 0x11
    {{0x7f7f,0x4040,0x7f40,0x407f},{0x0000,0xc0c0,0x00c0,0xc000}},  // 0x38
    {{0x4040,0x4040,0x4040,0x4040},{0xc0c0,0xc0c0,0xc0c0,0xc0c0}},  // 0x39
    {{0x0000,0x4040,0x0040,0x4000},{0x8080,0xc0c0,0x80c0,0xc080}},  // 0x3A
    {{0x7f00,0x4040,0x0040,0x407f},{0x0080,0xc0c0,0x80c0,0xc000}},  // 0x3B
    {{0x407f,0x7f40,0x7f7f,0x4040},{0xc000,0x00c0,0x0000,0xc0c0}},  // 0x40
    {{0x4040,0x4040,0x4040,0x4040},{0xc0c0,0xc0c0,0xc0c0,0xc0c0}},  // 0x41
    {{0x4000,0x0040,0x0000,0x4040},{0xc080,0x80c0,0x8080,0xc0c0}},  // 0x42
    {{0x407f,0x0040,0x7f00,0x4040},{0xc000,0x80c0,0x0080,0xc0c0}},  // 0x43
    {{0x7f40,0x407f,0x4040,0x7f7f},{0x00c0,0xc000,0xc0c0,0x0000}},  // 0x48
    {{0x4040,0x4040,0x4040,0x4040},{0xc0c0,0xc0c0,0xc0c0,0xc0c0}},  // 0x49
    {{0x0040,0x4000,0x4040,0x0000},{0x80c0,0xc080,0xc0c0,0x8080}},  // 0x4A
    {{0x0040,0x407f,0x4040,0x7f00},{0x80c0,0xc000,0xc0c0,0x0080}},  // 0x4B
};
// ==0-СЛОВА cat4 (beq→ФИКСИРОВАННОЕ слово, когда -0x6e92==0 = кабина ровно на этаже, ДО спуска):
// ct 0x11@9594 / 0x3b@967c / 0x43@9764 / 0x4b@984c. Порядок ХРАНЕНИЯ [N,S,W,E] (как STAIR_PROF:
// N@-6e8c=hi(L1), S@-6e8a=lo(L1), W@-6e86=lo(L2), E@-6e88=hi(L2)); доступ через ремап F2A.
// Адверсарно сверено с сырыми словами 9594 (2026-07-16): 0x11 N=40c0 S=0000 E=0040 W=c000 ✓.
static const uint16_t STAIR_PROF_ZERO4[4][4] = {
    {0x40c0,0x0000,0xc000,0x0040},  // 0x11 @9594: L1=$40c00000 L2=$0040c000
    {0x0000,0x40c0,0x0040,0xc000},  // 0x3B @967c: L1=$000040c0 L2=$c0000040
    {0xc000,0x0040,0x0000,0x40c0},  // 0x43 @9764: L1=$c0000040 L2=$40c00000
    {0x0040,0xc000,0x40c0,0x0000},  // 0x4B @984c: L1=$0040c000 L2=$000040c0
};
// Профиль-байты (L,R) грани face (0=N,1=W,2=E,3=S) клетки ct. false = не профиль-клетка.
// L,R — знаковые сдвиги вершин концов грани (0x40=+64, 0x7F=+127, 0xC0=-64, 0x80=-128).
// cabinSign: знак -0x6e92 (кабина/питч): >0 → «подъём» (POS, branch0), <0 → «спуск» (NEG, branch1),
//   ==0 → ==0-ВЕТКА ROM (кабина ровно на этаже, ДО спуска — «до спуска отображается неверно»).
// dx,dy = смещение клетки от камеры (cx−camCX, cy−camCY); нужно ==0-веткам cat3 (0x0e/39/41/49 по знаку d0/d1).
// ==0 расходится по типу условной инструкции хендлера (дизасм 945e-9818):
//   ble (0x0c/0d/38/40/48): 0 попадает в ветку ≤0 = спуск/NEG (branch1) — как раньше;
//   bmi (0x0f/10/3a/42/4a): 0 попадает в ветку ≥0 = подъём/POS (branch0) — БЫЛО НЕВЕРНО (порт брал NEG);
//   beq→tst d0 (0x0e @94f6 bmi / 0x39 @9610 bpl): 0 выбирает POS/NEG по знаку смещения по X;
//   beq→tst d1 (0x41 @96f8 bmi / 0x49 @97e0 bpl): по знаку смещения по Y;
//   beq→фикс   (0x11/3b/43/4b): своё ==0-слово (STAIR_PROF_ZERO4).
inline bool stairProfile(uint8_t ct, int face, int cabinSign, int dx, int dy, int& L, int& R) {
    int idx;
    if      (ct >= 0x0c && ct <= 0x11) idx = ct - 0x0c;
    else if (ct >= 0x38 && ct <= 0x3b) idx = 6 + (ct - 0x38);
    else if (ct >= 0x40 && ct <= 0x43) idx = 10 + (ct - 0x40);
    else if (ct >= 0x48 && ct <= 0x4b) idx = 14 + (ct - 0x48);
    else return false;
    // РЕМАП ГРАНЬ→ИНДЕКС-СЛОВА: массив хранит слова в порядке записи handler'ом [N,S,W,E] (по адресам
    // -6e8c/-6e8a/-6e86/-6e88, дизасм c71e/c758/c798/c7d4). Порт-грань [0=N,1=W,2=E,3=S]. face→array:
    // N→0, W→2, E→3, S→1. Без ремапа W/E/S брали чужие слова (верна только N). См. faStairFaceMap.
    static const int F2A[4] = {0, 2, 3, 1};
    const int fa = faStairFaceMap() ? F2A[face & 3] : (face & 3);
    uint16_t w;
    if      (cabinSign > 0) w = STAIR_PROF[idx][0][fa];   // -0x6e92>0 → подъём (POS)
    else if (cabinSign < 0) w = STAIR_PROF[idx][1][fa];   // -0x6e92<0 → спуск  (NEG)
    else switch (ct) {                                    // -0x6e92==0 → ==0-ветка
        case 0x11: w = STAIR_PROF_ZERO4[0][fa]; break;
        case 0x3b: w = STAIR_PROF_ZERO4[1][fa]; break;
        case 0x43: w = STAIR_PROF_ZERO4[2][fa]; break;
        case 0x4b: w = STAIR_PROF_ZERO4[3][fa]; break;
        case 0x0e: w = STAIR_PROF[idx][dx <  0 ? 0 : 1][fa]; break;  // @94f6 bmi d0: d0<0→POS
        case 0x39: w = STAIR_PROF[idx][dx >= 0 ? 0 : 1][fa]; break;  // @9610 bpl d0: d0>=0→POS
        case 0x41: w = STAIR_PROF[idx][dy <  0 ? 0 : 1][fa]; break;  // @96f8 bmi d1: d1<0→POS
        case 0x49: w = STAIR_PROF[idx][dy >= 0 ? 0 : 1][fa]; break;  // @97e0 bpl d1: d1>=0→POS
        case 0x0f: case 0x10: case 0x3a: case 0x42: case 0x4a:
            w = STAIR_PROF[idx][0][fa]; break;            // bmi: 0→POS (branch0)
        default:
            w = STAIR_PROF[idx][1][fa]; break;            // ble: 0→NEG (branch1)
    }
    L = (int8_t)(w >> 8); R = (int8_t)(w & 0xFF);
    return true;
}

// ОТЛОЖЕННЫЙ СЕГМЕНТ ПОЛ/ПОТОЛКА ЛИФТА/ЛЕСТНИЦЫ (по дизасму FUN_9226 хендлеры → FUN_9d42/9d4a → FUN_9d72).
// Спец-клетка добавляет сегмент на РЕБРО, обращённое к камере: 9d42 = ГРАДИЕНТ (направленные клетки шахты),
// 9d4a = СИНИЙ idx1 (площадки/вход/area; источник 0xd074 = сплошной idx1). Ребро рисуется поверх стен →
// иллюзия пол/потолок шахты. Геометрия извлечена из всех ~30 хендлеров (axis+знак → ближнее ребро).
// Возвращает 1=синий, 2=градиент, 0=нет. Ребро (целочисл. углы клетки) в ex0,ey0..ex1,ey1.
// КАНОНИЧЕСКОЕ РЕБРО+ТИП сегмента транзит-клетки (сверено со ВСЕМИ 16 хендлерами 988a-9a4a, 2026-07-16):
// каждый celltype пакует ровно ОДНО фиксированное ребро (не «ближнее к камере» вообще — оно задано
// хендлером и совпадает с направлением его строгого гейта tst d0/d1). edge: 0=N(y=cy) 1=S(y=cy+1)
// 2=W(x=cx) 3=E(x=cx+1). Возврат: 0=нет сегмента, 1=СИНИЙ idx1 (9d4a), 2=ГРАДИЕНТ (9d42).
inline int segEdgeType(uint8_t ct, int& edge) {
    edge = -1;
    switch (ct) {
        // ГРАДИЕНТ (9d42) — ризы лестницы / кабина-стойки лифта
        case 0x12: case 0x14: case 0x55: case 0x56: case 0x57: edge = 1; return 2; // S (988a: dy<0 / 99f2)
        case 0x3c: case 0x3e: case 0x59: case 0x5a: case 0x5b: edge = 0; return 2; // N (98aa: dy>0 / 9a2e)
        case 0x44: case 0x46: case 0x32: case 0x33: case 0x34: edge = 2; return 2; // W (98c6: dx>0 / 997a)
        case 0x4c: case 0x4e: case 0x51: case 0x52: case 0x53: edge = 3; return 2; // E (98e2: dx<0 / 99b6)
        // СИНИЙ (9d4a) — площадки/вход лестницы + area лифта
        case 0x3d: case 0x58: edge = 1; return 1; // S (991e: dy<0 / 9a4a)
        case 0x13: case 0x54: edge = 0; return 1; // N (9902: dy>0 / 9a12)
        case 0x45: case 0x31: edge = 3; return 1; // E (993e: dx<0 / 9996)
        case 0x4d: case 0x50: edge = 2; return 1; // W (995e: dx>0 / 99d6)
        default: return 0;
    }
}

inline int shaftSeg(uint8_t ct, int cx, int cy, int camCX, int camCY,
                    int& ex0, int& ey0, int& ex1, int& ey1) {
    int edge = -1;
    int type = segEdgeType(ct, edge);              // канон. ребро+тип по celltype (988a-9a4a)
    if (type == 0) return 0;
    int dx = cx - camCX, dy = cy - camCY;          // направленный гейт хендлера (строгий tst d0/d1)
    if (edge == 0 && !(dy > 0)) return 0;          // North-ребро видно, если клетка южнее камеры
    if (edge == 1 && !(dy < 0)) return 0;          // South — клетка севернее
    if (edge == 2 && !(dx > 0)) return 0;          // West — клетка восточнее
    if (edge == 3 && !(dx < 0)) return 0;          // East — клетка западнее
    if      (edge == 0) { ex0 = cx;     ey0 = cy;     ex1 = cx + 1; ey1 = cy; }
    else if (edge == 1) { ex0 = cx;     ey0 = cy + 1; ex1 = cx + 1; ey1 = cy + 1; }
    else if (edge == 2) { ex0 = cx;     ey0 = cy;     ex1 = cx;     ey1 = cy + 1; }
    else                { ex0 = cx + 1; ey0 = cy;     ex1 = cx + 1; ey1 = cy + 1; }
    return type;
}

// ДОСТИЖИМОСТЬ КЛЕТКИ ЛУЧОМ (ROM: транзит-сегмент клетки пакуется ⟺ DDA-луч дошёл до неё ДО первой
// непрозрачной стены; bc90/9d72, разбор 2026-07-11). Заменяет жёсткий гейт «камера НА клетке»: сегменты
// появляются плавно по мере приближения (клетка становится видимой) и НЕ торчат через карту (за стеной —
// не достижима). Грид-DDA (Amanatides-Woo) от позиции камеры к центру клетки; блокирует любая рендер-стена
// (0x01-05 оси/диаг, 0x0c-11/38-4b профиль, 0x30 шахта). Транзит-пол (0x12-17, area/кабина) — прозрачен.
inline bool cellReachable(const Level& lvl, int floor, double px, double py, int tcx, int tcy) {
    int cx = (int)px, cy = (int)py;
    if (cx == tcx && cy == tcy) return true;                       // камера в самой клетке
    double dx = (tcx + 0.5) - px, dy = (tcy + 0.5) - py;
    int stepX = dx > 0 ? 1 : -1, stepY = dy > 0 ? 1 : -1;
    double adx = dx < 0 ? -dx : dx, ady = dy < 0 ? -dy : dy;
    double tMaxX = (adx > 1e-9) ? ((stepX > 0 ? (cx + 1 - px) : (px - cx)) / adx) : 1e30;
    double tMaxY = (ady > 1e-9) ? ((stepY > 0 ? (cy + 1 - py) : (py - cy)) / ady) : 1e30;
    double tDeltaX = (adx > 1e-9) ? (1.0 / adx) : 1e30;
    double tDeltaY = (ady > 1e-9) ? (1.0 / ady) : 1e30;
    for (int guard = 0; guard < 256; ++guard) {
        if (tMaxX < tMaxY) { cx += stepX; tMaxX += tDeltaX; } else { cy += stepY; tMaxY += tDeltaY; }
        if (cx == tcx && cy == tcy) return true;                   // дошли до цели раньше стены
        if (cx < 0 || cy < 0 || cx >= Level::W || cy >= Level::H) return false;
        if (cellRenderWall(lvl.cellType(floor, cx, cy))) return false;  // непрозрачная стена на пути
    }
    return false;
}

// Рампы затенения ZT (CLUT): сеттер 0x1d66 ставит 8 таблиц-рамп по 256 б (ремап упакованного
// байта-текселя). Дальние банды ремапят индексы → 15 (чёрный). hi/lo нибблы байта затеняются
// по-разному → горизонтальный ДИЗЕРИНГ. Режим Haze (env 2) — свой набор 0x3b92.., прочие — 0x3392..
struct ShadeRamps {
    uint8_t lo[8][16], hi[8][16];   // [банд][индекс] -> затенённый индекс (lo/hi для дизеринга); диагональ CLUT (сплошной тексель)
    uint8_t full[8][256];           // ПОЛНАЯ CLUT банда: [банд][байт=2 текселя] -> затенённый байт (hi=лев.px, lo=прав.px). ROM ccb6/скейлер move.b (a3,d0.w)
    int env = -1;
    // base = регион рамп (копия ROM с 0x3392): MAIN[b] = base + b*0x100; HAZE[b] = base + 0x800 + b*0x100.
    void build(const uint8_t* base, int envMode) {
        if (envMode == env || !base) return;
        env = envMode;
        size_t setBase = (envMode == 2) ? 0x800 : 0x0;     // Haze (env2) → HAZE-набор
        for (int b = 0; b < 8; ++b) {
            for (int by = 0; by < 256; ++by)               // полная CLUT (как a3=-$7176 в скейлере)
                full[b][by] = base[setBase + (size_t)b * 0x100 + by];
            for (int p = 0; p < 16; ++p) {
                uint8_t byte = full[b][(p << 4) | p];       // диагональ = сплошной тексель p
                hi[b][p] = byte >> 4; lo[b][p] = byte & 0x0F;
            }
        }
    }
    // финальный индекс палитры для текселя idx на банде band, дизер по чётности тексель-X.
    // doShade=false (Bright) — без рампы (сырой индекс, полная яркость).
    inline uint8_t map(uint8_t idx, int band, bool hiNib, bool doShade) const {
        if (!doShade) return idx;
        return hiNib ? hi[band][idx] : lo[band][idx];
    }
};

// ── ПОЛ/ПОТОЛОК = ОТДЕЛЬНЫЙ СЛОЙ ZT (разобрано по дизасму, НЕ скейлер) ─────────
// Дизасм: пол/потолок рисует НЕ колоночный скейлер стен, а предрассчитанный буфер ($FF6226/$FF6276),
// который рутина d202 копирует в кадр ПО ЭКРАННОЙ СТРОКЕ (потолок сверху d2d6, пол снизу d342/d40a),
// а стена накладывается поверх (occlusion по z-буферу $FF6126). Буфер строится из ШАБЛОНА @-0x7152
// (сеттер 0x1d66, свой на каждый env) рутиной d4a6 (вертикальный скролл по питчу).
// Шаблон = 0xA0 байт = ДВА вертикальных паттерна по 80 строк (нечёт/чёт нативная колонка → гориз.
// дизер); байт = 2 пикселя 4bpp (hi-ниббл = левый px, lo = правый). Затенение вдаль ЗАПЕЧЕНО в данные
// (индекс 15 = чёрный у горизонта на тёмных env), НЕ применяется рантайм-рампа. Индекс 0 = ПРОЗРАЧНЫЙ
// (виден фон-панорама; так env3 NoCeil = открытое небо). Палитра — общая со стенами (0x20F2):
// потолок idx3→2→1 (сине-серый), пол idx8→9→b (красно-коричневый) — сверено с палитрой и сэмплами кадров.
// Из шаблона env извлекаем ПЛАВНЫЙ градиент: на каждую из 80 строк — 2 индекса (по ЯРКОСТИ палитры:
// lo тёмный / hi светлый) и доля светлого hiFrac (0..1) из 4 семплов строки [evenHi,evenLo,oddHi,oddLo].
// Это разделяет «градиент» (плавный) и «дизер» (наш, тонкий, на экранном res) — иначе двойной дизер
// (наш поверх запечённого) даёт муар, а нативный 1px-дизер при апскейле — толстые полосы.
struct FloorCeil {
    int env = -1;
    bool ok = false;
    const uint8_t* tpl = nullptr;              // СЫРОЙ шаблон env (0xA0): [0..0x4f] нечёт-кол, [0x50..0x9f] чёт
    uint8_t loIdx[80] = {}, hiIdx[80] = {};   // индексы палитры по строке (lo=тёмный, hi=светлый)
    uint8_t hiCnt[80] = {};                    // 0..4 — сколько из 4 семплов = hiIdx (доля яркого)
    void build(const uint8_t* fcBase, const Palette& pal, int envMode) {
        if (envMode == env) return;
        env = envMode;
        tpl = fcBase ? fcBase + (size_t)(envMode & 7) * 0xA0 : nullptr;
        ok = (tpl != nullptr);
        if (!ok) return;
        auto luma = [&](int idx) { uint32_t c = pal.c[idx & 0xF];
            return (int)(((c >> 16) & 0xFF) * 30 + ((c >> 8) & 0xFF) * 59 + (c & 0xFF) * 11); };
        for (int r = 0; r < 80; ++r) {
            uint8_t s[4] = { (uint8_t)(tpl[0x50 + r] >> 4), (uint8_t)(tpl[0x50 + r] & 0xF),
                             (uint8_t)(tpl[r] >> 4),        (uint8_t)(tpl[r] & 0xF) };
            int lo = s[0], hi = s[0];
            for (int k = 1; k < 4; ++k) { if (luma(s[k]) < luma(lo)) lo = s[k]; if (luma(s[k]) > luma(hi)) hi = s[k]; }
            int cnt = 0; for (int k = 0; k < 4; ++k) if (s[k] == hi) cnt++;
            loIdx[r] = (uint8_t)lo; hiIdx[r] = (uint8_t)hi; hiCnt[r] = (uint8_t)((lo == hi) ? 4 : cnt);
        }
    }
};

// Индекс палитры пол/потолок для ЭКРАННОГО пикселя (x,y), высота вида H, горизонт на строке horizon.
// Возвращает 0..15; вызывающий: idx==0 → прозрачный (sentinel/панорама).
//
// ВАЖНО (по дизасму d2d6/d40a): нативный вид ZT = 80 СКАНЛАЙНОВ, дизер шаблона = 1px (per-scanline по
// вертикали + 2 нибблы/2 паттерна по горизонтали). При фуллскрин-апскейле (80→H≈640, ×8) прямой сэмпл
// шаблона дал бы ТОЛСТЫЕ 8px-полосы. Поэтому: ГРАДИЕНТ берём из строки шаблона (по экранному y), а САМ
// ДИЗЕР накладываем на ЭКРАННОМ разрешении (тонкий, ~1px) — верт. смесь соседних строк шаблона по
// чётности экранной строки + горизонт. 4px-цикл по 4 семплам [evenHi,evenLo,oddHi,oddLo] со сдвигом-
// шахматкой на нечёт. строке. Так вид совпадает с оригиналом (мелкий дизер), а не «лесенка из полос».
inline int fcIndexZT(const FloorCeil& fc, int x, int y, int horizon, int H, int /*coordW*/) {
    if (!fc.ok || H < 1) return 0;
    // float позиция в градиенте шаблона (0..79, горизонт=40)
    double fr;
    if (y < horizon) fr = (horizon > 0) ? (double)y * 40.0 / horizon : 0.0;
    else             fr = 40.0 + (double)(y - horizon) * 40.0 / ((H - horizon) > 0 ? (H - horizon) : 1);
    int r0 = (int)fr; if (r0 < 0) r0 = 0; if (r0 > 79) r0 = 79;
    int r1 = r0 + 1;  if (r1 > 79) r1 = 79;
    double t = fr - (int)fr;
    int lo = fc.loIdx[r0], hi = fc.hiIdx[r0];
    double frac;                                   // доля яркого (hi), сглаженная между строками
    if (fc.loIdx[r1] == lo && fc.hiIdx[r1] == hi)  // та же пара → плавная интерполяция доли
        frac = (fc.hiCnt[r0] + (fc.hiCnt[r1] - fc.hiCnt[r0]) * t) / 4.0;
    else { int r = (t < 0.5) ? r0 : r1; lo = fc.loIdx[r]; hi = fc.hiIdx[r]; frac = fc.hiCnt[r] / 4.0; }
    // ЧИСТЫЙ упорядоченный дизер Bayer 4×4 на ЭКРАННОМ разрешении (тонкий ~1px) между lo и hi по доле
    static const int B4[4][4] = {{0,8,2,10},{12,4,14,6},{3,11,1,9},{15,7,13,5}};
    double thr = (B4[y & 3][x & 3] + 0.5) / 16.0;
    return (thr < frac) ? hi : lo;
}

// Цвет пол/потолок. idx 0 → прозрачный (0x00000000 sentinel: блит подставит панораму/синий).
inline uint32_t floorCeilColor(const Palette& pal, const FloorCeil& fc,
                               int x, int y, int horizon, int H, int coordW) {
    int idx = fcIndexZT(fc, x, y, horizon, H, coordW);
    if (idx == 0) return 0x00000000u;
    return pal.c[idx & 0x0F];
}

// ── D4A6-РЕСЕМПЛ ГРАДИЕНТ-ПОЛОС ПО ПИТЧУ (дизасм d494/d49a/d4a6, вскрыто 2026-07-02) ──
// Игра при питче НЕ сдвигает градиент, а ПЕРЕСЭМПЛИРУЕТ его в буфер $FF6226: n=1.25·|pitch|;
//   pitch>0 (камера ВЫШЕ: прыжок): строки 0..n = flat src[0]; потолок (src 0..39) сжат Брезенхэмом
//     в строки n..40; пол (src 40..79) растянут ×(40+n)/40 вниз от горизонта (строка 40);
//   pitch<0 (камера НИЖЕ: присед/спуск/поездка): зеркально — строки 80−n..80 = flat src[79];
//     пол сжат в строки 40..80−n; потолок растянут вверх от горизонта ↔ его при-горизонтная
//     часть (в env2 — сплошной idx2, «синий») расползается вниз по экрану.
// Функция: экранная строка r (0..79) + питч → строка ИСХОДНОГО шаблона (0..79). Дробную
// Брезенхэм-механику заменяем эквивалентной линейной перепроекцией (та же геометрия).
inline int fcRowD4A6(int row, int pitch) {
    if (pitch == 0) return row;
    int n = std::abs(pitch) + (std::abs(pitch) >> 2);          // 1.25·|pitch| (asr #2 + add)
    if (n > 80) n = 80;   // |pitch|≥64 → вся полоса = flat (ROM: цикл flat покрывает все 80 строк)
    if (pitch > 0) {
        if (row < n) return 0;                                  // flat: верх = src[0]
        if (row < 40) {                                         // потолок сжат: n..40 ← src 0..40
            int span = 40 - n; if (span < 1) return 39;
            int s = (row - n) * 40 / span; return s > 39 ? 39 : s;
        }
        // пол растянут: 40..80 ← src 40..40+40·40/(40+n)
        int s = 40 + (row - 40) * 40 / (40 + n); return s > 79 ? 79 : s;
    } else {
        if (row >= 80 - n) return 79;                           // flat: низ = src[79]
        if (row >= 40) {                                        // пол сжат: 40..80−n ← src 40..80
            int span = 40 - n; if (span < 1) return 40;
            int s = 40 + (row - 40) * 40 / span; return s > 79 ? 79 : s;
        }
        // потолок растянут вверх от горизонта: строка 39-к ← src 39 − k·40/(40+n)
        int s = 39 - (39 - row) * 40 / (40 + n); return s < 0 ? 0 : s;
    }
}

// ТОЧНАЯ выборка шаблона (БИТ-В-БИТ ZT, БЕЗ Bayer-реконструкции) на НАТИВНОЙ сетке 256×80 — для FAITHFUL.
// Дизасм d4a6/d202: буфер колонки индексируется ПРЯМО по сканлайну вида (ny = строка буфера, pitch=0),
// горизонт = строка 40; гориз. — нативная 2px-колонка (байт) с выбором паттерна нечёт/чёт + 2 нибблы.
// nx∈0..255 (нативный пиксель вида 256 шир.), ny∈0..79 (сканлайн). Паттерн период 4px (повторяется),
// magnitude c не важна — только c&1 (чёт/нечёт колонка) и nx&1 (ниббл). Кламп nx до 255 (НЕ 127!).
inline int fcIndexExact(const FloorCeil& fc, int nx, int ny) {
    if (!fc.ok) return 0;
    if (nx < 0) nx = 0; if (nx > 255) nx = 255;
    if (ny < 0) ny = 0; if (ny > 79)  ny = 79;
    int c = nx >> 1;                                          // нативная 2px-колонка (байт)
    const uint8_t* pat = (c & 1) ? fc.tpl : (fc.tpl + 0x50);  // нечёт-кол → база, чёт → +0x50 (как ccb6)
    uint8_t byte = pat[ny];
    return (nx & 1) ? (byte & 0x0F) : (byte >> 4);            // hi-ниббл = левый px (чёт nx), lo = правый
}
inline uint32_t floorCeilColorExact(const Palette& pal, const FloorCeil& fc, int nx, int ny) {
    int idx = fcIndexExact(fc, nx, ny);
    if (idx == 0) return 0x00000000u;
    return pal.c[idx & 0x0F];
}

// ── РЕНДЕР ДЕКОРА-БИЛБОРДОВ (точно как блиттер ZT eda0 + хендлеры) ──────────────
// Рисуется ПОСЛЕ стен в «экранном пространстве» рендера (нат-буфер faithful или фреймбуфер DDA).
// Билборд — экранный прямоугольник в центре клетки по X (как eda0: верх на screenY, вниз на высоту),
// z-тест против стен ПО КОЛОНКЕ, прозрачный индекс 0, затенение рампами по банду дистанции.
// Размеры — ТОЧНЫЕ доли S (высота клетки на дистанции) из дизасм-хендлеров (DecorDef в восьмых).
// Ширину берём из ПРОЕКЦИИ ширины клетки `cellW=sx(0.5)−sx(−0.5)` (×wFrac) — это автоматически
// учитывает FOV/растяжку каждого режима (faithful-фуллскрин, референс 256, DDA с hs). Обобщён лямбдами:
//   put(x,y,argb) · wallCloser(x,fObj)→скрыт ли за стеной · sx(f,l)→screenX · colH(f)→высота клетки.
template <typename Put, typename WallCloser, typename SX, typename ColH>
inline void drawDecorBillboards(int SW, int SH, const Level& lvl, const Camera& cam,
                                const WallBank& obj, const Palette& wallPal,
                                const ShadeRamps& ramps, bool doShade, int drawDist,
                                Put put, WallCloser wallCloser, SX sx, ColH colH) {
    const int ccx = (int)cam.px, ccy = (int)cam.py;
    struct Item { double f, l; DecorDef d; bool square = false; int sprId = -1; uint8_t efr = 0; bool corpse = false; float zlift = 0; uint8_t dir = 0, animSt = 0, variant = 0; bool foam = false; bool burned = false; uint8_t atkPose = 0; bool overlay = false; };  // overlay=огонь поверх стен
    std::vector<Item> items;
    for (int y = ccy - drawDist; y <= ccy + drawDist; ++y) {
        if (y < 0 || y >= Level::H) continue;
        for (int x = ccx - drawDist; x <= ccx + drawDist; ++x) {
            if (x < 0 || x >= Level::W) continue;
            uint8_t ct = lvl.cellType(cam.floor, x, y);
            DecorDef dd;
            // ⭐ДЕКОР-ТРУП / BURNT-REMAINS как окружение (ZT celltype 0x2C/0x6C-0x74 = corpse-anim врага, 0x35 = burnt-remains;
            // хендлеры 0x11772-0x11854 → jmp 1bd72). Рисуется тем же enemy-спрайтом (death-anim / g_burntRemains), что и динамич.труп.
            int corpseSlot = decorCorpseSlot(ct); bool burntDecor = decorBurntCt(ct);
            if (corpseSlot >= 0 || burntDecor) {
                int slot = burntDecor ? 0 : corpseSlot;
                if (slot < 0 || !g_enemyAnim[slot].ok) continue;
                double rx0 = (x + 0.5) - cam.px, ry0 = (y + 0.5) - cam.py;
                double f0 = rx0 * cam.dirX + ry0 * cam.dirY; if (f0 < 0.05) continue;
                double l0 = ry0 * cam.dirX - rx0 * cam.dirY;
                DecorDef dummy = {{{0, 0, 1, 1, 0}}, 1};
                // sprId=slot, animSt=3(death/corpse-anim); burnt → burned=true (спрайт заменится на g_burntRemains). efr=0 статик.
                items.push_back({f0, l0, dummy, false, slot, 0, false, 0.f, 0, (uint8_t)(burntDecor ? 0 : 3), 0, false, burntDecor, 0, false});
                continue;
            }
            if (!decorForCt(ct, dd)) {                        // не декор → может быть пикап оружия/предмета
                if (!itemBillboardForCt(ct, dd)) continue;
                if (pickupHiddenFn() && pickupHiddenFn()(cam.floor, x, y)) continue;  // уже подобран
            }
            double rx = (x + 0.5) - cam.px, ry = (y + 0.5) - cam.py;
            double f = rx * cam.dirX + ry * cam.dirY;          // forward (как faProject)
            if (f < 0.05) continue;                            // позади камеры / слишком близко
            double l = ry * cam.dirX - rx * cam.dirY;          // lateral
            items.push_back({f, l, dd});                       // ширина по cwd (мир-аспект = ROM X-растяжка дисплея ×2)
        }
    }
    // Динамические эффекты стрельбы (взрывы/снаряды): мировые позиции → билборды того же прохода.
    for (const WorldFx& fx : worldFx()) {
        if (fx.floor != cam.floor) continue;
        double rx = fx.wx - cam.px, ry = fx.wy - cam.py;
        double f = rx * cam.dirX + ry * cam.dirY;
        if (f < 0.05) continue;
        double l = ry * cam.dirX - rx * cam.dirY;
        DecorDef d = {{{fx.tile, fx.topOff16, fx.hFrac16, fx.wFrac16, 0}}, 1};
        items.push_back({f, l, d, !fx.wide, fx.sprId, fx.efr, fx.corpse, fx.zlift, fx.dir, fx.animSt, fx.variant, fx.foam, fx.burned, fx.atkPose, fx.overlay});   // square=!wide: огонь по cwd (×2), эффекты/враги квадрат Sd
    }
    // painter's: дальние раньше, ближние поверх (z-тест только против стен)
    std::sort(items.begin(), items.end(), [](const Item& a, const Item& b) { return a.f > b.f; });

    const double horizon = SH * 0.5;
    const int frame = decorFrame();
    uint8_t tile[32 * 32];
    // КЛАМП МАСШТАБА (ограничение железа eda0 0x12C): при f<clampF держим РАЗМЕР (S/cw) как на f=clampF —
    // MD не умеет hw-скейл спрайтов, софт-скейлер не растит их бесконечно у самого лица. Позиция — по реальному f.
    const double clampF = faSpriteClampF();
    const double fSizeMin = (clampF > 1e-6) ? clampF : 1e-6;
    for (const auto& it : items) {
        const double f = it.f, l = it.l;
        const double fS = (f < fSizeMin) ? fSizeMin : f;        // f для РАЗМЕРА (кламп), не для позиции
        const double S  = colH(fS);                             // высота клетки (полной стены) на дистанции (клампнута)
        double cw = sx(fS, 0.5) - sx(fS, -0.5);  if (cw < 0) cw = -cw;   // ширина клетки (клампнута)
        // ⚠ Спрайты (враг/квадрат/пена) — от S (верт.масштаб), НЕ растягиваем ×cw/S: враги ZT = 1:2 (высокие,
        // eda0 H=2·W), эффекты — КВАДРАТ. Клетка вида 2:1 (cw шир) — для СТЕН/декора-по-cw, не для спрайтов.
        const double cx = sx(f, l);                             // центр билборда по X — по РЕАЛЬНОМУ f (позиция)
        // ПИТЧ спрайта = ROM 1bcaa `screenY = scale·(pitch − eyeH − actorZ) >> 6 + horizon` = PER-SCALE `S·pitch/64`.
        // Зеркалит ПОЛНЫЙ сдвиг стены (pshift + viewShiftPx), чтобы спрайт совпал с геометрией в ОБОИХ режимах:
        //   ROM-точный (faTransitZT/лифт): `S·pitch/64` (полная сила, как d214/eda0). ← дефолт ON = ROM-питч.
        //   legacy (faStairUni-ослабленный): `S·pitch·faStairUni/64` (per-column) + `pitch·faStairUni` (constant vShift).
        const double pitchShift = (faTransitZT() || cam.elevState != 0)
            ? S * cam.pitch / 64.0
            : (S * cam.pitch * faStairUni() / 64.0 + cam.pitch * faStairUni());
        const double hz = horizon + pitchShift;
        const int band = doShade ? bandForHeight((int)(64.0 / f)) : 0;  // тот же банд, что у стены на f
        // ПЕНА ОГНЕТУШИТЕЛЯ: ФОРМА декор-тайла 0 (как ZT draw 0x14272), но огненные индексы РЕМАПЛЮ в сине-белую пену
        // (ZT рисует тот же тайл палитра-линией 0x20d2 = голубой, а не 0x20f2 = огонь). Размер квадрат wFrac·S/16.
        if (it.foam) {
            double width = it.d.s[0].wFrac16 * 0.0625 * S, height = it.d.s[0].hFrac16 * 0.0625 * S;
            if (width < 0.5 || height < 0.5) continue;
            double topY = hz + it.d.s[0].topOff16 * 0.0625 * S, xa = cx - width * 0.5;
            int ix0 = (int)std::ceil(xa), ix1 = (int)std::floor(cx + width * 0.5);
            int iy0 = (int)std::ceil(topY), iy1 = (int)std::floor(topY + height) - 1;
            if (ix1 < 0 || ix0 > SW - 1 || iy1 < 0 || iy0 > SH - 1) continue;
            if (ix0 < 0) ix0 = 0; if (ix1 > SW - 1) ix1 = SW - 1; if (iy0 < 0) iy0 = 0; if (iy1 > SH - 1) iy1 = SH - 1;
            obj.decode(it.d.s[0].tile, tile);                            // форма тайла 0
            for (int ix = ix0; ix <= ix1; ++ix) {
                if (wallCloser(ix, f)) continue;
                int su = (int)((ix + 0.5 - xa) / width * 32.0); if (su < 0) su = 0; if (su > 31) su = 31;
                for (int iy = iy0; iy <= iy1; ++iy) {
                    int sv = (int)((iy + 0.5 - topY) / height * 32.0); if (sv < 0) sv = 0; if (sv > 31) sv = 31;
                    uint8_t idx = tile[sv * 32 + su]; if (!idx) continue;
                    uint32_t fc = (idx == 12 || idx == 13 || idx == 10) ? 0xFFF0F4FFu      // ярко → почти белая пена
                                : (idx == 11 || idx == 9)               ? 0xFFB4CCF0u      // средне → светло-голубая
                                                                        : 0xFF6890C8u;     // тёмно → голубая
                    put(ix, iy, fc);
                }
            }
            continue;
        }
        // РЕАЛЬНЫЙ СПРАЙТ ВРАГА (ARGB-дерево). Размер по ZT-формуле (eda0/0x1BCAA): ширина=drawW·S/32,
        // высота=drawH·S/16 (S = высота стены = мастер-скейл). Кадр ходьбы efr; corpse = лежит на полу.
        if (it.sprId >= 0 && it.sprId < 16 && g_enemyAnim[it.sprId].ok) {
            // FH вар2: при variant==1 и наличии АЛЬТ-БАНКА (другая модель, напр. коммандо) берём ЕГО целиком
            EnemyAnimSet& A = (it.variant && g_enemyAnimVar2[it.sprId].ok) ? g_enemyAnimVar2[it.sprId] : g_enemyAnim[it.sprId];
            // ВЫБОР АНИМАЦИИ: 0=ходьба (по направлению dir), 1=стрельба/удар, 2=стаггер, 3=смерть, 4=лазанье по стене. Фолбэк → ходьба.
            std::vector<EnemySprite>* lst = &A.walk[it.dir < (uint8_t)A.walkDirs ? it.dir : 0];
            // Hydaca потолок: при variant==1 и наличии walkB (тот же банк) используем второй набор ходьбы
            if (it.variant && A.hasVariant && it.animSt == 0) {
                uint8_t bd = it.dir < (uint8_t)A.walkBDirs ? it.dir : 0;
                if (!A.walkB[bd].empty()) lst = &A.walkB[bd];
            }
            if (it.animSt == 1) {                                          // огонь: fire2 по atkPose, иначе DIRECTIONAL (ROM 1ba04)
                lst = (it.atkPose && !A.fire2.empty()) ? &A.fire2
                    : (A.fireDirs > 0 && it.dir < (uint8_t)A.fireDirs && !A.fireD[it.dir].empty()) ? &A.fireD[it.dir]
                    : (!A.fire.empty() ? &A.fire : lst);
            }
            else if (it.animSt == 2) {                                     // стаггер: DIRECTIONAL (facing=velocity, как ROM $2a/$2c)
                if (A.hitDirs > 0 && it.dir < (uint8_t)A.hitDirs && !A.hitD[it.dir].empty()) lst = &A.hitD[it.dir];
                else if (!A.hit.empty()) lst = &A.hit;
            }
            else if (it.animSt == 3 && !A.death.empty()) lst = &A.death;
            else if (it.animSt == 4) {                                     // Hydaca лазанье: DIRECTIONAL вертик. поза (6) по ракурсу, иначе одиночная
                if (A.climbDirs > 0 && it.dir < (uint8_t)A.climbDirs && !A.climbDir[it.dir].empty()) lst = &A.climbDir[it.dir];
                else if (!A.climb.empty()) lst = &A.climb;
            }
            else if (it.animSt == 5) {                                     // Hydaca ПАДЕНИЕ (ZT draw state2): dir 0=вверх a8 / 2=мёртв a10 / 1=вниз a1 (climb)
                if (it.dir == 0 && !A.fallUp.empty())        lst = &A.fallUp;
                else if (it.dir == 2 && !A.fallDead.empty()) lst = &A.fallDead;
                else if (!A.climb.empty())                   lst = &A.climb;
            }
            else if (it.animSt == 6 && !A.morph.empty())       lst = &A.morph;        // Sgt морф (ZT 1b628, ступени по efr)
            else if (it.animSt == 7 && !A.pretendLie.empty())  lst = &A.pretendLie;   // Boss3 притворство: лежит (a10)
            else if (it.animSt == 8 && !A.pretendJerkA.empty())lst = &A.pretendJerkA; // Boss3 дёрг (a7, 15/16)
            else if (it.animSt == 9 && !A.pretendJerkB.empty())lst = &A.pretendJerkB; // Boss3 дёрг-альт (a8, 1/16)
            else if (it.animSt == 10) {                                              // Revenant падение (ZT 1adfe): efr = стадия 0..3
                int fi = it.efr > 3 ? 3 : it.efr;
                if (!A.revFall[fi].empty()) lst = &A.revFall[fi];
            }
            // ⭐СПАЛЁННЫЙ труп → BURNT REMAINS спрайт (ZT 1b8d8, банк 0x1cb96a), НЕ обычный труп/перекраска.
            // burned=true в worldFx ставится ТОЛЬКО у AT_CORPSE (живой AT_ENEMY его не передаёт), поэтому it.corpse
            // (=!hasDeath, костыль сплющивания) тут не нужен — иначе горючие враги С death-анимацией (Imp/FH/Hydaca) не заменялись бы.
            if (it.burned && !g_burntRemains.empty()) lst = &g_burntRemains;
            auto& frames = *lst;
            if (frames.empty()) continue;
            size_t efi = it.efr; if (it.animSt == 10) efi = 0;   // revFall: стадия УЖЕ выбрала аним → внутри суб0
            const EnemySprite& es = frames[efi < frames.size() ? efi : 0];
            if (!es.ok) continue;
            // РАЗМЕР ПО ТАЙЛ-СЕТКЕ спрайта (ИСПРАВЛЕН аспект): real px = Wтайл·32 × Hтайл·32 = es.w × es.h →
            // экранные W/H пропорц. РЕАЛЬНЫМ пикселям, единый скейл K (0.375·S держит гуманоида 1×2 на ~0.75·S).
            // Пёс (1×1=32×32)=КВАДРАТ низкий, Hydaca (2×1=64×32)=ШИРОКАЯ низкая, боссы (2×2) крупнее. (Был баг:
            // drawW/drawH≈равны + формула 16/8 делала ВСЕХ вытянутыми 1:2 → пёс/Hydaca выглядели длинными.)
            // РАЗМЕР: MD НЕ hw-скейлит — eda0 масштабирует ЦЕЛОЧИСЛЕННЫМ scale=64/F (клам 0x12c), H=drawH·scale>>4
            // (>>4 = /16 огрубляет → КРУПНЫЕ дискретные ступени по дистанции, не плавно). Порт считал от float S/f.
            double height, width;
            if (faSpriteDiscScale()) {
                int scaleI = (int)(faSpriteScaleC() / fS);                  // ZT invDepth = 64/F, ЦЕЛЫЙ
                if (scaleI < 1) scaleI = 1; if (scaleI > 0x12c) scaleI = 0x12c;
                // ⭐ROM-ТОЧНАЯ ФОРМУЛА (1bcaa): на ТАЙЛ width=drawW·scale>>5, height=drawH·scale>>4; спрайт = Wт×Hт тайлов.
                //   ⚠drawW/drawH — PER-SPRITE поля кадра (ROM +2/+3): ГУМАНОИДЫ 6, Dog/БОССЫ 8 → Dog/боссы КРУПНЕЕ человека.
                //   Старый порт брал жёстко 6 + нормировал по tile-размеру (es.h/64) → Dog вдвое мельче, хотя в ROM drawH=8>6.
                int dW = es.drawW > 0 ? es.drawW : 6, dH = es.drawH > 0 ? es.drawH : 6;
                int wtile = (dW * scaleI) >> 5; if (wtile < 1) wtile = 1;  // ширина 1 тайла на экране (>>5)
                int htile = (dH * scaleI) >> 4; if (htile < 1) htile = 1;  // высота 1 тайла на экране (>>4 → выше)
                int Wt = es.w / 32, Ht = es.h / 32; if (Wt < 1) Wt = 1; if (Ht < 1) Ht = 1;   // тайлов по ширине/высоте
                // ⚠НОРМИРОВКА на 2-тайловый эталон-гуманоид (·0.5 по H) → K=2.1 (существующий ini) даёт ТОТ ЖЕ гуманоид, что
                // старая формула (hstep·es.h/64). Dog/боссы (drawH=8, Wт/Hт≠гуманоида) получают верный ОТНОСИТЕЛЬНЫЙ размер.
                double unit = (SH / 80.0) * faSpriteSizeK();               // ZT-доля вида (K=2.1 совместим со старым ini)
                height = htile * Ht * unit * 0.5;                          // Hтайлов·(drawH·scale>>4)·0.5 (норм. на эталон 2 тайла)
                width  = wtile * Wt * unit;                                // Wтайлов·(drawW·scale>>5) (×2 дисплей ÷2 норм. = ×1)
            } else {
                double K = 0.375 * S;                                       // старый плавный float-скейл
                height = (es.h / 32.0) * K; width = (es.w / 32.0) * K;
            }
            if (it.corpse) { height *= 0.42; width *= 1.25; }            // ОБЫЧНЫЙ труп (без death-anim) сплющен в кучу; burnt-remains НЕ трогаем —
            // у него свои drawW=7/drawH=6 (ROM 1cb96a) → размер уже задан формулой 1bcaa (было ×0.42 → неверно, юзер: «не соответствует оригиналу»)
            double bottomY = hz + S * 0.5 - it.zlift * S * 0.9;     // ноги на полу; zlift>0 = поднят к потолку (Hydaca «на стене/потолке»)
            double topY = bottomY - height;
            double xa = cx - width * 0.5;
            int ix0 = (int)std::ceil(xa), ix1 = (int)std::floor(cx + width * 0.5);
            int iy0 = (int)std::ceil(topY), iy1 = (int)std::floor(bottomY) - 1;
            if (ix1 < 0 || ix0 > SW - 1 || iy1 < 0 || iy0 > SH - 1 || width < 0.5 || height < 0.5) continue;
            if (ix0 < 0) ix0 = 0; if (ix1 > SW - 1) ix1 = SW - 1; if (iy0 < 0) iy0 = 0; if (iy1 > SH - 1) iy1 = SH - 1;
            double fdim = (band > 0) ? (1.0 - band * 0.07) : 1.0; if (fdim < 0.4) fdim = 0.4;
            // CLUT-ШЕЙД спрайта (eda0/0x10d1be, как стены): сырой тексель → байт(2 текселя) → CLUT[band][byte] → пал.
            const bool useClut = faSpriteClut() && g_spriteClut().ok && !es.idx.empty() && !it.burned;
            // band спрайта: ROM-формула (ed50: освещение этажа + scale) ЛИБО как стена (bandForHeight).
            int sband = faSpriteBandRom() ? bandForSprite(f, lvl.envT[cam.floor]) : band;
            sband += faSpriteBandAdj(); if (sband < 0) sband = 0; if (sband > 11) sband = 11;
            for (int ix = ix0; ix <= ix1; ++ix) {
                if (wallCloser(ix, f)) continue;
                int su = (int)((ix + 0.5 - xa) / width * es.w); if (su < 0) su = 0; if (su >= es.w) su = es.w - 1;
                for (int iy = iy0; iy <= iy1; ++iy) {
                    int sv = (int)((iy + 0.5 - topY) / height * es.h); if (sv < 0) sv = 0; if (sv >= es.h) sv = es.h - 1;
                    uint32_t c;
                    if (useClut) {                                       // ⭐CLUT-путь: тексель → байт → CLUT → пал
                        uint8_t t = es.idx[(size_t)sv * es.w + su];
                        if (t == 0) continue;                            // прозрачно (idx0)
                        int suE = su & ~1, suO = suE + 1; if (suO >= es.w) suO = suE;   // байт-колонка (2 текселя)
                        uint8_t byte = (uint8_t)((es.idx[(size_t)sv * es.w + suE] << 4) | es.idx[(size_t)sv * es.w + suO]);
                        uint8_t sh = g_spriteClut().band[sband][byte];
                        uint8_t nib = (su & 1) ? (sh & 0x0F) : (sh >> 4);
                        c = wallPal.c[nib];
                    } else {
                        c = es.argb[(size_t)sv * es.w + su];
                        if (c == 0) continue;
                        // ⭐БЕЗ перекраски: спалённый труп = ОТДЕЛЬНЫЙ спрайт burnt-remains (0x1cb96a, свои цвета), НЕ тонирование enemy.
                        if (fdim < 1.0) { int r=(c>>16)&0xFF,g=(c>>8)&0xFF,b=c&0xFF;
                            c = 0xFF000000u | ((int)(r*fdim)<<16) | ((int)(g*fdim)<<8) | (int)(b*fdim); }
                    }
                    put(ix, iy, c);
                }
            }
            continue;                                                    // спрайт врага нарисован
        }
        // MD-ДИСКРЕТНЫЙ масштаб объекта на полу (пикапы/декор идут через тот же eda0/1bcaa, что враги — размер
        // ступенями по целому scale, не плавно). Квантуем S/cw тем же множителем, что у врагов.
        double Sd = S, cwd = cw;
        if (faSpriteDiscScale()) {
            int scaleI = (int)(faSpriteScaleC() / fS); if (scaleI < 1) scaleI = 1; if (scaleI > 0x12c) scaleI = 0x12c;
            int hstep = (6 * scaleI) >> 4; if (hstep < 1) hstep = 1;
            double cont = 6.0 * faSpriteScaleC() / fS / 16.0;             // непрерывный эквивалент hstep (без огрубления)
            double m = (cont > 0.01) ? (double)hstep / cont : 1.0;        // отношение дискр/непрер → квантует размер ступенями
            Sd = S * m; cwd = cw * m;
        }
        // band для спрайтового CLUT-шейда декора (по освещению этажа + scale, как враги; ZT eda0 CLUT 0x10d1be)
        const bool decClut = faDecorSpriteClut() && g_spriteClut().ok;
        int sbandD = band;
        if (decClut) { sbandD = faSpriteBandRom() ? bandForSprite(f, lvl.envT[cam.floor]) : band;
                       sbandD += faSpriteBandAdj(); if (sbandD < 0) sbandD = 0; if (sbandD > 11) sbandD = 11; }
        for (int si = 0; si < it.d.count; ++si) {
            const DecorSprite& sp = it.d.s[si];
            uint8_t tnum = sp.tile; bool hflip = (it.variant & 1) != 0;  // ⭐worldFx-эффект (кровь): гориз.флип по $3e&1 (ZT eda0 -$6f78); level-декор variant=0
            if (sp.anim == 1) {                                       // вентилятор: 4 кадра (tile/tile+1 ± hflip)
                int ph = (frame >> 3) & 3;
                if (ph == 1 || ph == 2) tnum = sp.tile + 1;
                hflip = (ph == 2 || ph == 3);
            } else if (sp.anim == 2) {                                // мигание: alt = tile−4
                if ((frame >> 4) & 1) tnum = (uint8_t)(sp.tile - 4);
            }
            const double height = sp.hFrac16 * 0.0625 * Sd;          // высота (доля S, 1/16) — дискр. масштаб
            // ширина: декор — доля ширины КЛЕТКИ (cw, мир-аспект); эффекты/враги — КВАДРАТ от S (не растянуто).
            const double width  = sp.wFrac16 * 0.0625 * (it.square ? Sd : cwd);
            const double topY   = hz + sp.topOff16 * 0.0625 * Sd;    // верх (отн. горизонта)
            if (width < 0.5 || height < 0.5) continue;
            const double xa = cx - width * 0.5;
            int ix0 = (int)std::ceil(xa), ix1 = (int)std::floor(cx + width * 0.5);
            int iy0 = (int)std::ceil(topY), iy1 = (int)std::floor(topY + height) - 1;
            if (ix1 < 0 || ix0 > SW - 1 || iy1 < 0 || iy0 > SH - 1) continue;
            if (ix0 < 0) ix0 = 0; if (ix1 > SW - 1) ix1 = SW - 1;
            if (iy0 < 0) iy0 = 0; if (iy1 > SH - 1) iy1 = SH - 1;
            obj.decode(tnum, tile);
            for (int ix = ix0; ix <= ix1; ++ix) {
                if (!it.overlay && wallCloser(ix, f)) continue;       // за стеной (огонь-overlay НЕ клипится — ROM eda0 рисует поверх, юзер: в тесноте не обрезалось)
                int su = (int)((ix + 0.5 - xa) / width * 32.0);  if (su < 0) su = 0; if (su > 31) su = 31;
                if (hflip) su = 31 - su;
                const bool hiNib = !(ix & 1);
                for (int iy = iy0; iy <= iy1; ++iy) {
                    int sv = (int)((iy + 0.5 - topY) / height * 32.0);  if (sv < 0) sv = 0; if (sv > 31) sv = 31;
                    uint8_t idx = tile[sv * 32 + su];
                    if (idx == 0) continue;                           // прозрачный (MD цвет 0)
                    uint32_t c;
                    if (decClut) {                                    // ⭐СПРАЙТОВЫЙ CLUT (eda0/0x10d1be): байт-пара → CLUT[band][byte] → нибл
                        int suE = su & ~1, suO = suE + 1; if (suO > 31) suO = suE;
                        uint8_t byte = (uint8_t)((tile[sv * 32 + suE] << 4) | tile[sv * 32 + suO]);
                        uint8_t sh = g_spriteClut().band[sbandD][byte];
                        c = wallPal.c[(su & 1) ? (sh & 0x0F) : (sh >> 4)];
                    } else c = wallPal.c[ramps.map(idx, band, hiNib, doShade)];  // старый стеновой CLUT (ramps)
                    put(ix, iy, c);
                }
            }
        }
    }
}

// Рендер кадра вида от первого лица. put(x,y,argb); zbuf[x] = глубина колонки.
template <typename PutFn>
void renderFPS(PutFn&& put, int W, int H, const Level& lvl, const Palette& wallPal,
               MetaCache& meta, const Camera& camIn, std::vector<double>& zbuf, int envMode) {
    // ROM b9a6: глаз рендера = позиция игрока − dir/8 (камера «за плечом», см. tuning faCamBack)
    Camera cam = camIn;
    if (faCamBack()) { cam.px -= cam.dirX * 0.125; cam.py -= cam.dirY * 0.125; }
    zbuf.assign(W, 1e9);
    const bool   openTop = (envMode == 3);
    // PITCH = ВЫСОТА ГЛАЗА (не наклон! dead_ends #5/#8: наклона камеры в ZT НЕТ). Равномерный вертикальный
    // сдвиг ВСЕГО вида (стены+пол+потолок вместе через общий horizon), без перспективного наклона колонн.
    // Лестница/нокбэк/прыжок/присед → горизонт плывёт вверх/вниз. Лифт (elevState≠0) — своя ветка, не трогаем.
    const int    pitchPx = (cam.elevState != 0) ? 0 : (int)cam.pitch;
    const int    horizon = H / 2 + pitchPx;
    const bool   doShade = (envMode != 0);      // env0 Bright — рампы не ставит (полная яркость)
    static ShadeRamps ramps; ramps.build(meta.shadeRampData, envMode);
    static FloorCeil fc; fc.build(meta.fcTemplateData, wallPal, envMode);  // слой пол/потолок (ROM-шаблон по env)

    const double hs = faHStretch();             // гор. растяжка: ýже FOV → шире стены
    // ДАЛЬНОСТЬ ПРОРИСОВКИ (cull @0xba60): box ±dist от камеры; стены дальше → фон/градиент.
    const int camCXd = (int)cam.px, camCYd = (int)cam.py;
    const int ddist = (faDrawDist() > 0) ? faDrawDist() : drawDistForEnv(envMode);
    const Panorama* bgF = activeBg();
    const bool skyF = (openTop && bgF && bgF->valid());
    auto colBackdrop = [&](int x_) {            // весь столбец = пол/потолок/панорама (нет стены)
        for (int y = 0; y < H; ++y) {
            bool ceil = (y < horizon);
            uint32_t c = (skyF && ceil)
                ? bgSample(*bgF, cam.dirX, cam.dirY, cam.floor, cam.cabin, x_, W, y, horizon)
                : floorCeilColor(wallPal, fc, x_, y, horizon, H, W);
            if ((c & 0xFF000000u) == 0)
                c = (bgF && bgF->valid()) ? bgSample(*bgF, cam.dirX, cam.dirY, cam.floor, cam.cabin, x_, W, y, horizon) : 0xFF000000u;
            put(x_, y, c);
        }
    };
    for (int x = 0; x < W; ++x) {
        double cameraX = (2.0 * x / W - 1.0) / hs;
        double rayX = cam.dirX + cam.planeX * cameraX;
        double rayY = cam.dirY + cam.planeY * cameraX;

        int mapX = (int)cam.px, mapY = (int)cam.py;
        double dDistX = (rayX == 0) ? 1e30 : std::fabs(1.0 / rayX);
        double dDistY = (rayY == 0) ? 1e30 : std::fabs(1.0 / rayY);
        int stepX, stepY; double sideX, sideY;
        if (rayX < 0) { stepX = -1; sideX = (cam.px - mapX) * dDistX; }
        else          { stepX =  1; sideX = (mapX + 1.0 - cam.px) * dDistX; }
        if (rayY < 0) { stepY = -1; sideY = (cam.py - mapY) * dDistY; }
        else          { stepY =  1; sideY = (mapY + 1.0 - cam.py) * dDistY; }

        int side = 0, guard = 0; bool hit = false, diag = false, culled = false;
        double diagPerp = 0, diagTexU = 0; uint16_t diagMeta = 0;
        while (!hit && guard++ < 256) {
            if (sideX < sideY) { sideX += dDistX; mapX += stepX; side = 0; }
            else               { sideY += dDistY; mapY += stepY; side = 1; }
            if (mapX < 0 || mapY < 0 || mapX >= Level::W || mapY >= Level::H) { hit = true; break; }
            if (mapX < camCXd - ddist || mapX > camCXd + ddist ||
                mapY < camCYd - ddist || mapY > camCYd + ddist) { culled = true; break; }  // за дальностью
            uint8_t ctc = lvl.cellType(cam.floor, mapX, mapY);
            uint8_t cid = lvl.cellId(cam.floor, mapX, mapY);
            if (cellRendersDoor(ctc)) {
                // ДВЕРЬ: панель в ЦЕНТРЕ ячейки. гориз (0x06/фейк 0x83) y=mapY+0.5, верт (0x07/фейк 0x84) x=mapX+0.5.
                bool horiz = doorIsHoriz(ctc);
                double denom = horiz ? rayY : rayX;
                if (std::fabs(denom) > 1e-9) {
                    double td = horiz ? ((mapY + 0.5 - cam.py) / denom)
                                      : ((mapX + 0.5 - cam.px) / denom);
                    if (td > 1e-4) {
                        double lp = horiz ? (cam.px + rayX * td - mapX)   // вдоль X
                                          : (cam.py + rayY * td - mapY);  // вдоль Y
                        // РАЗДВИЖНЫЕ СТВОРКИ: открыта середина шириной open; створки уезжают к краям
                        double half = doorOpen(cam.floor, mapX, mapY) * 0.5;
                        bool inGap = (lp > 0.5 - half && lp < 0.5 + half);
                        if (lp >= 0.0 && lp <= 1.0 && !inGap) {
                            // texU скользит со створкой (левая +half, правая −half)
                            double tu = (lp <= 0.5 - half) ? lp + half : lp - half;
                            hit = diag = true; diagPerp = td; diagTexU = tu;
                            uint16_t mm = lvl.texorder(cid, 0);
                            diagMeta = mm ? mm : DOOR_METATEX;
                        }
                    }
                }
                // не пересёк створку (открытая середина) — луч идёт сквозь
            } else if (ctc >= 2 && ctc <= 5) {
                // ДИАГОНАЛЬНАЯ (угловая) стена: пересечение луча с диагональю клетки.
                // ct2(UR)/ct4(LL) → "\" (x-y=cx-cy); ct3(LR)/ct5(UL) → "/" (x+y=cx+cy+1).
                bool bs = (ctc == 2 || ctc == 4);                 // "\"
                double denom = bs ? (rayX - rayY) : (rayX + rayY);
                if (std::fabs(denom) > 1e-9) {
                    double td = bs ? (((mapX - mapY) - (cam.px - cam.py)) / denom)
                                   : (((mapX + mapY + 1) - (cam.px + cam.py)) / denom);
                    if (td > 1e-4) {
                        double lx = cam.px + rayX * td - mapX;
                        double ly = cam.py + rayY * td - mapY;
                        if (lx >= -1e-6 && lx <= 1.0 + 1e-6 && ly >= -1e-6 && ly <= 1.0 + 1e-6) {
                            hit = diag = true;
                            diagPerp = td; diagTexU = lx;
                            int diagFace = (ctc == 2 || ctc == 5) ? 3 : 0;
                            diagMeta = lvl.texorder(cid, diagFace);
                        }
                    }
                }
                // не пересёк диагональ — луч идёт сквозь открытую половину (продолжаем марш)
            } else if (cellRenderWall(ctc)) {
                hit = true;                                       // осевая стена
            }
        }

        if (culled) { colBackdrop(x); zbuf[x] = 1e9; continue; }  // за дальностью — нет стены, весь столбец фон

        // ---- параметры столбца (осевой хит или диагональ/дверь) ----
        double perp; uint16_t m; int texX;
        if (diag) {
            perp = diagPerp < 1e-4 ? 1e-4 : diagPerp;
            m = diagMeta;
            texX = (int)(diagTexU * 128.0); if (texX < 0) texX = 0; if (texX > 127) texX = 127;
        } else {
            perp = (side == 0) ? (sideX - dDistX) : (sideY - dDistY);
            if (perp < 1e-4) perp = 1e-4;
            bool inb = (mapX >= 0 && mapY >= 0 && mapX < Level::W && mapY < Level::H);
            uint8_t cell = inb ? lvl.cellId(cam.floor, mapX, mapY) : 0;
            uint8_t ct   = inb ? lvl.cellType(cam.floor, mapX, mapY) : 1;
            int face = (side == 1) ? (rayY > 0 ? 0 : 3) : (rayX > 0 ? 1 : 2);
            m = lvl.texorder(cell, face);
            if (m == 0 && cellIsDoor(ct)) m = DOOR_METATEX;
            double wallX = (side == 0) ? (cam.py + perp * rayY) : (cam.px + perp * rayX);
            wallX -= std::floor(wallX);
            texX = (int)(wallX * 128.0); if (texX < 0) texX = 0; if (texX > 127) texX = 127;
            if ((side == 0 && rayX < 0) || (side == 1 && rayY > 0)) texX = 127 - texX;
        }
        zbuf[x] = perp;
        if (x == W/2 && std::getenv("ZTDBG"))
            std::fprintf(stderr, "DBG center: cam(%.3f,%.3f) ray(%.3f,%.3f) HIT (%d,%d) ct=0x%02X perp=%.3f diag=%d\n",
                         cam.px, cam.py, rayX, rayY, mapX, mapY, lvl.cellType(cam.floor, mapX, mapY), perp, (int)diag);
        const uint8_t* mt = meta.get(m);                // 128x64
        // ТОЧНЫЙ банд ZT по высоте колонны (D0 = 64/forward); дизер hi/lo по чётности тексель-X
        int band = bandForHeight((int)(64.0 / perp));
        bool hiNib = !(texX & 1);

        int lineH = (int)(H / perp);
        const bool elevRide = (cam.elevState != 0);
        const bool hitInb = (mapX >= 0 && mapY >= 0 && mapX < Level::W && mapY < Level::H);
        const uint8_t hitCt = hitInb ? lvl.cellType(cam.floor, mapX, mapY) : 1;
        const bool cabinWall = elevRide && rcElevCell(hitCt);  // стена САМОЙ кабины → статична + синий пол/пот
        const uint32_t VOID_BLUE = 0xFF002448u;
        const Panorama* bg = activeBg();
        const double step = 64.0 / lineH;
        // СЛОИСТАЯ МОДЕЛЬ (Plane A фон / Plane B градиент+стены). bgAt = ФОН (панорама, универсальный
        // backdrop; чёрный если фона нет). backdrop = слой пол/потолок-градиента (Plane B): idx0 градиента
        // (env3-небо) → фон. Стена тексель-0 (ОКНО) → bgAt НАПРЯМУЮ (а не градиент пола, как было).
        auto bgAt = [&](int x_, int sy) -> uint32_t {
            return (bg && bg->valid()) ? bgSample(*bg, cam.dirX, cam.dirY, cam.floor, cam.cabin, x_, W, sy, horizon)
                                       : 0xFF000000u;
        };
        auto backdrop = [&](int x_, int sy) -> uint32_t {     // пол/потолок-градиент (Plane B); idx0 → фон
            uint32_t c = floorCeilColor(wallPal, fc, x_, sy, horizon, H, W);
            return ((c & 0xFF000000u) == 0) ? bgAt(x_, sy) : c;   // прозрачный (idx0 = env3-небо) → фон (Plane A)
        };

        if (elevRide && !cabinWall) {
            // КОРИДОР (вид на выход): СТЕНА едет вверх ПЕРСПЕКТИВНЫМ питчем (lineH·pitch/64, питч=−cabin,
            // дизасм d214). ГРАДИЕНТ пол/потолок — СТАТИЧНЫЙ фоновый слой (screen-anchored буфер A5 в
            // дизасме d2d6/d40a: A5[i]→строка i всегда, НЕ скроллится с перспективой!). Снизу (спуск) /
            // сверху (после свопа) наезжает тёмно-синий МЕЖЭТАЖНЫЙ пол (band), ЗАКРЫВАЯ статичный градиент;
            // на пике |cabin|=0x40 — весь синий → своп этажа незаметен; затем след. этаж аналогично.
            int shift = (int)(lineH * cam.pitch / 64.0);
            int top = H / 2 + shift - lineH / 2;
            int y0 = top < 0 ? 0 : top;
            int y1 = top + lineH; if (y1 >= H) y1 = H - 1;
            int blueLo, blueHi;                       // межэтажный синий: спуск — снизу [..H], после свопа — сверху [0..]
            if (cam.cabin >= 0) { blueLo = (int)(H * (1.0 - cam.cabin / 64.0)); blueHi = H; }
            else                { blueLo = 0; blueHi = (int)(H * (-cam.cabin / 64.0)); }
            for (int y = 0; y < H; ++y) {
                if (y >= blueLo && y < blueHi) { put(x, y, VOID_BLUE); continue; }   // межэтажный синий пол
                if (y < y0 || y > y1) { put(x, y, backdrop(x, y)); continue; }       // СТАТИЧНЫЙ градиент (screen y!)
                int ty = (int)((double)(y - top) * step) & 63;
                uint8_t idx = mt[ty * 128 + texX];
                put(x, y, idx == 0 ? bgAt(x, y) : wallPal.c[ramps.map(idx, band, hiNib, doShade)]);  // окно → фон
            }
            continue;
        }

        // СТЕНА КАБИНЫ ЛИФТА (статична, пол/потолок = синий) ИЛИ обычный столбец (лестница/не-лифт: питч).
        const bool blueFC = cabinWall;                // пол/потолок кабины = тёмно-синий
        // питч теперь = равномерный сдвиг ГОРИЗОНТА (выше, в horizon), БЕЗ наклона колонн (lineH·pitch — убрано)
        int top = horizon - lineH / 2;
        int y0 = top < 0 ? 0 : top;
        int y1 = top + lineH; if (y1 >= H) y1 = H - 1;
        for (int y = 0; y < y0; ++y) put(x, y, blueFC ? VOID_BLUE : backdrop(x, y));
        for (int y = y1 + 1; y < H; ++y) put(x, y, blueFC ? VOID_BLUE : backdrop(x, y));
        double texPos = (y0 - top) * step;            // пропуск текселей из-за клампа верха
        for (int y = y0; y <= y1; ++y) {
            int ty = (int)texPos & 63; texPos += step;
            uint8_t idx = mt[ty * 128 + texX];
            if (idx == 0) put(x, y, blueFC ? VOID_BLUE : bgAt(x, y));   // ОКНО (тексель-0) → ФОН (Plane A)
            else put(x, y, wallPal.c[ramps.map(idx, band, hiNib, doShade)]);
        }
    }

    // ДЕКОР-БИЛБОРДЫ (DDA): прямо в фреймбуфер, z-тест против zbuf[x]=forward (меньше=ближе).
    // screenX по FOV DDA (плоскость 0.66) с растяжкой hs; высота клетки = lineH = H/forward.
    if (faDecor() && meta.obj && meta.obj->count > 0)
        drawDecorBillboards(W, H, lvl, cam, *meta.obj, wallPal, ramps, doShade, ddist,
            [&](int x, int y, uint32_t c) { put(x, y, c); },
            // z-тест с ДЕПТ-БИАСОМ: спрайт занимает ~0.4кл клетки, поэтому колонку перекрываем ТОЛЬКО если стена
            // ЗАМЕТНО ближе (>BIAS). Иначе соседняя боковая стена «вровень» (чуть ближе по краю спрайта) срезала
            // пол-текстуры врага у стены. Стена явно впереди (враг за углом) при разнице >BIAS — всё ещё перекрывает.
            [&](int x, double f) {
                const double BIAS = 0.40;                          // ≈ полу-ширина спрайта (кл)
                return zbuf[x] < f - BIAS;                         // стена СУЩЕСТВЕННО ближе → перекрывает; вровень → спрайт целиком
            },
            [&](double f, double l) { return W * 0.5 * (1.0 + hs * l / (0.66 * f)); },
            [&](double f) { return (double)H / f; });
}
